#include "sdl_platform.hpp"
#include "synth_frame.hpp"

#include <cstdio>
#include <cstdlib>

#include <SDL.h>
#if defined(__APPLE__)
#include <SDL_metal.h>
#include <SDL_syswm.h>
#endif

// Defined in librecomp/src/recomp.cpp: the RDRAM base handed to the game.
extern "C" uint8_t* ultramodern_get_rdram_base();

namespace ogre {

namespace {

// How long a synthetic Start press is held, in ms. Long enough that a 60 Hz
// input poll sees it, short enough that the release dominates the interval.
constexpr uint32_t kTapHoldMs = 150;

// Milliseconds since the platform was initialised. SDL_GetTicks64 is a
// monotonic host clock, which is what the tap/exit timers below want.
uint64_t platform_millis(const Platform& platform) {
    return SDL_GetTicks64() - platform.start_ticks;
}

// `OGRE_TAP_MS=<n>` / `OGRE_EXIT_AFTER_MS=<n>`; 0 or unset disables. Read once
// so a mid-run change cannot move the timers.
uint32_t env_millis(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    const long parsed = strtol(value, nullptr, 0);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

}  // namespace

// N64 controller button bits (as read by libultra osContGetReadData).
enum N64Button : uint16_t {
    N64_BTN_A = 0x8000,
    N64_BTN_B = 0x4000,
    N64_BTN_Z = 0x2000,
    N64_BTN_START = 0x1000,
    N64_BTN_UP = 0x0800,
    N64_BTN_DOWN = 0x0400,
    N64_BTN_LEFT = 0x0200,
    N64_BTN_RIGHT = 0x0100,
    N64_BTN_L = 0x0020,
    N64_BTN_R = 0x0010,
    N64_BTN_C_LEFT = 0x0008,
    N64_BTN_C_RIGHT = 0x0004,
    N64_BTN_C_DOWN = 0x0002,
    N64_BTN_C_UP = 0x0001,
};

bool init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[SDL] Failed to init: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

// Reads the self-driving-run knobs. Must run after SDL_Init so that
// SDL_GetTicks64 has a valid base.
void configure_automation(Platform& platform) {
    platform.start_ticks = SDL_GetTicks64();
    platform.tap_ms = env_millis("OGRE_TAP_MS");
    platform.exit_after_ms = env_millis("OGRE_EXIT_AFTER_MS");
    if (platform.tap_ms != 0 || platform.exit_after_ms != 0) {
        fprintf(stderr, "[SDL] automation: tap_ms=%u exit_after_ms=%u\n",
                platform.tap_ms, platform.exit_after_ms);
    }
}

ultramodern::renderer::WindowHandle create_window(Platform& platform, const char* title) {
    uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(__linux__) || defined(__ANDROID__)
    flags |= SDL_WINDOW_VULKAN;
#endif
    SDL_Window* window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
    if (window == nullptr) {
        fprintf(stderr, "[SDL] Failed to create window: %s\n", SDL_GetError());
        return {};
    }
    platform.window = window;

#if defined(__APPLE__)
    // RT64's Metal path needs two native handles, and neither of them is the
    // SDL_Window*:
    //
    //   * WindowHandle::window is cast to an NSWindow* by plume
    //     (`CocoaWindow::updateWindowAttributesInternal` does
    //     `[[nsWindow contentView] frame]`, `[nsWindow screen]`), so it must be
    //     the Cocoa window from SDL_GetWindowWMInfo, not the SDL window.
    //     Passing the SDL_Window* crashes instantly in objc_msgSend with
    //     EXC_BAD_ACCESS on the main-thread event pump.
    //   * WindowHandle::view must be the window's CAMetalLayer*.
    //
    // This used to pass `{window, nullptr}` because SDL_Metal_GetLayer was
    // believed to segfault under Homebrew's sdl2-compat. That no longer
    // reproduces (verified with a standalone probe: SDL_GetWindowWMInfo,
    // SDL_Metal_CreateView and SDL_Metal_GetLayer all return valid pointers).
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fprintf(stderr, "[SDL] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return {};
    }
    platform.metal_view = SDL_Metal_CreateView(window);
    if (platform.metal_view == nullptr) {
        fprintf(stderr, "[SDL] SDL_Metal_CreateView failed: %s\n", SDL_GetError());
        return {};
    }
    ultramodern::renderer::WindowHandle handle;
    handle.window = reinterpret_cast<void*>(wm_info.info.cocoa.window);
    handle.view = SDL_Metal_GetLayer(static_cast<SDL_MetalView>(platform.metal_view));
    if (handle.view == nullptr) {
        fprintf(stderr, "[SDL] SDL_Metal_GetLayer returned null\n");
        return {};
    }
    fprintf(stderr, "[SDL] window=%p ns_window=%p metal_layer=%p\n",
            static_cast<void*>(window), handle.window, handle.view);
    return handle;
#else
    return window;
#endif
}

void shutdown_sdl(Platform& platform) {
    for (auto& controller : platform.controllers) {
        if (controller != nullptr) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
    if (platform.audio_device != 0) {
        SDL_CloseAudioDevice(platform.audio_device);
        platform.audio_device = 0;
    }
#if defined(__APPLE__)
    // The Metal view must be released before the window it belongs to.
    if (platform.metal_view != nullptr) {
        SDL_Metal_DestroyView(static_cast<SDL_MetalView>(platform.metal_view));
        platform.metal_view = nullptr;
    }
#endif
    if (platform.window != nullptr) {
        SDL_DestroyWindow(platform.window);
        platform.window = nullptr;
    }
    SDL_Quit();
}

void open_audio(Platform& platform, uint32_t frequency) {
    if (platform.audio_device != 0) {
        SDL_CloseAudioDevice(platform.audio_device);
        platform.audio_device = 0;
    }
    // OGRE_NO_AUDIO=1: skip opening an output device entirely. SDL's
    // SDL_OpenAudioDevice() blocks indefinitely on some hosts (observed on
    // macOS when no output device is usable), and because the runtime calls it
    // from the game start thread's preinit(), that stalls the whole boot before
    // a single recompiled function runs. Audio is not needed for rendering
    // diagnostics, so allow a run to proceed silently without it.
    if (getenv("OGRE_NO_AUDIO") != nullptr) {
        platform.audio_frequency = frequency;
        return;
    }
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(frequency);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    platform.audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    platform.audio_frequency = frequency;
    if (platform.audio_device == 0) {
        fprintf(stderr, "[SDL] Failed to open audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(platform.audio_device, 0);
}

void pump_sdl_events(Platform& platform, bool* quit) {
    // Bounded run: OGRE_EXIT_AFTER_MS ends the process from the main thread.
    if (platform.exit_after_ms != 0 && platform_millis(platform) >= platform.exit_after_ms) {
        const uint32_t budget = platform.exit_after_ms;
        platform.exit_after_ms = 0;
        // The queue snapshot's per-thread view is a block *kind* and a resume
        // count, which cannot say where a thread that is simply not being
        // scheduled is sitting. The recompiled code reports every function
        // entry and return, so the runtime also knows the live call chain.
        fprintf(stderr, "[SDL] exit_after_ms=%u elapsed, stopping\n", budget);
        fprintf(stderr, "[SDL] per-thread last recompiled function:\n");
        for (int tid = 1; tid < 32; tid++) {
            const uint32_t func = ultramodern::debug_last_func_vram(tid);
            if (func != 0) {
                fprintf(stderr, "[SDL]   t%-3d last func 0x%08X\n", tid, func);
            }
        }
        if (getenv("OGRE_CHAIN_HISTORY") != nullptr) {
            ultramodern::debug_dump_chain_history();
        }
        fprintf(stderr, "[SDL] per-thread live call chain:\n");
        for (int tid = 1; tid < 32; tid++) {
            if (ultramodern::debug_last_func_vram(tid) != 0) {
                ultramodern::debug_dump_call_chain(tid, "at exit");
            }
        }
        // OGRE_SCHED_TRACE=1: the scheduler event ring, in true order. This is
        // how a thread that yielded into the running queue and was never
        // resumed is read off the trace (the [Sched] printfs interleave).
        if (getenv("OGRE_SCHED_TRACE") != nullptr) {
            int only = -1;
            if (const char* f = getenv("OGRE_SCHED_TRACE_TID")) {
                only = atoi(f);
            }
            ultramodern::debug_dump_sched_ring(only);
        }
        // OGRE_PROFILE=1: the sampling profiler's (thread, function) histogram.
        ultramodern::debug_profile_dump();
        // OGRE_DUMP_RDRAM=<path>: write the whole RDRAM image so the state at
        // the stall can be analysed offline (and diffed against a run on the
        // other platform) instead of guessed from a handful of snapshot words.
        if (const char* dump_path = getenv("OGRE_DUMP_RDRAM")) {
            uint8_t* rdram = ultramodern_get_rdram_base();
            if (rdram != nullptr) {
                if (FILE* f = fopen(dump_path, "wb")) {
                    const size_t size = 0x800000;
                    size_t written = fwrite(rdram, 1, size, f);
                    fclose(f);
                    fprintf(stderr, "[SDL] dumped %zu bytes of rdram to %s\n", written, dump_path);
                } else {
                    fprintf(stderr, "[SDL] could not open %s for rdram dump\n", dump_path);
                }
            }
        }
        fflush(stderr);
        // A bounded run is a measurement: exit immediately with a success code
        // instead of unwinding. The graceful path tears down the renderer and
        // the runtime's worker threads while the game's are still mid-call (the
        // stalled boot leaves t1 inside a PI busy-wait), which segfaults
        // intermittently - observed both with and without debug traces. Nothing
        // in a scripted run needs the unwind, and the 100 ms lets the VI thread
        // finish the snapshot it may be printing concurrently.
        ultramodern::sleep_milliseconds(100);
        fflush(nullptr);
        _Exit(EXIT_SUCCESS);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                *quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                SDL_JoystickID joy_id = event.cdevice.which;
                SDL_GameController* controller = SDL_GameControllerOpen(joy_id);
                if (controller != nullptr) {
                    // Assign to the first free N64 slot (slot 0 is keyboard-first).
                    for (int slot = 1; slot < 4; slot++) {
                        if (platform.controllers[slot] == nullptr) {
                            platform.controllers[slot] = controller;
                            break;
                        }
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_JoystickID joy_id = event.cdevice.which;
                for (auto& controller : platform.controllers) {
                    if (controller != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == joy_id) {
                        SDL_GameControllerClose(controller);
                        controller = nullptr;
                        break;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

// The synthetic Start press of a scripted run (`OGRE_TAP_MS`), or 0.
//
// This is the native equivalent of the web harness's `tap()`: one short press
// per interval, so the game's title branch sees a button *up* after it. It
// contributes button state only; it does not synthesise SDL events. That is
// deliberate - ultramodern calls poll_input from the game thread, and pushing
// events into SDL from there is exactly the Cocoa main-thread violation fixed
// in the session-24 native bring-up.
static uint16_t automation_buttons() {
    extern Platform g_platform;
    if (g_platform.tap_ms == 0) {
        return 0;
    }
    const uint64_t elapsed = platform_millis(g_platform);
    const uint64_t phase = elapsed % g_platform.tap_ms;
    const uint64_t tap = elapsed / g_platform.tap_ms;
    if (phase >= kTapHoldMs) {
        return 0;
    }
    // `OGRE_TAP_MAX=<n>`: stop tapping after tap number n (tap 0 is the boot
    // window). Lets a run press Start through the title and then go silent so
    // later taps can't abort a loading scene mid-init (session 40: a Start
    // during 0x0D init aborts to 0x02 and the instant re-entry crashes on
    // torn-down state). 0 or unset = tap forever.
    {
        static long tap_max = -1;
        if (tap_max < 0) {
            tap_max = 0;
            if (const char* v = getenv("OGRE_TAP_MAX")) tap_max = atol(v);
        }
        if (tap_max > 0 && tap > (uint64_t)tap_max) {
            return 0;
        }
    }
    // Log once per press so a run's stderr shows the taps landed.
    static uint64_t logged_taps = 0;
    if (tap != logged_taps && tap > 0) {
        logged_taps = tap;
        fprintf(stderr, "[SDL] automation tap %llu at %llums\n",
                (unsigned long long)tap, (unsigned long long)elapsed);
    }
    return N64_BTN_START;
}

static uint16_t keyboard_buttons() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t buttons = 0;

    auto is_down = [&](SDL_Scancode scancode) { return keys[scancode] != 0; };

    if (is_down(SDL_SCANCODE_X)) buttons |= N64_BTN_A;
    if (is_down(SDL_SCANCODE_Z)) buttons |= N64_BTN_B;
    if (is_down(SDL_SCANCODE_C)) buttons |= N64_BTN_Z;
    if (is_down(SDL_SCANCODE_RETURN) || is_down(SDL_SCANCODE_RETURN2)) buttons |= N64_BTN_START;
    if (is_down(SDL_SCANCODE_Q)) buttons |= N64_BTN_L;
    if (is_down(SDL_SCANCODE_E)) buttons |= N64_BTN_R;
    if (is_down(SDL_SCANCODE_UP)) buttons |= N64_BTN_UP;
    if (is_down(SDL_SCANCODE_DOWN)) buttons |= N64_BTN_DOWN;
    if (is_down(SDL_SCANCODE_LEFT)) buttons |= N64_BTN_LEFT;
    if (is_down(SDL_SCANCODE_RIGHT)) buttons |= N64_BTN_RIGHT;
    if (is_down(SDL_SCANCODE_I)) buttons |= N64_BTN_C_UP;
    if (is_down(SDL_SCANCODE_K)) buttons |= N64_BTN_C_DOWN;
    if (is_down(SDL_SCANCODE_J)) buttons |= N64_BTN_C_LEFT;
    if (is_down(SDL_SCANCODE_L)) buttons |= N64_BTN_C_RIGHT;

    buttons |= automation_buttons();

    return buttons;
}

static uint16_t gamecontroller_buttons(SDL_GameController* controller) {
    if (controller == nullptr) {
        return 0;
    }
    uint16_t buttons = 0;

    auto is_down = [&](SDL_GameControllerButton button) {
        return SDL_GameControllerGetButton(controller, button) != 0;
    };

    if (is_down(SDL_CONTROLLER_BUTTON_A)) buttons |= N64_BTN_A;
    if (is_down(SDL_CONTROLLER_BUTTON_B)) buttons |= N64_BTN_B;
    if (is_down(SDL_CONTROLLER_BUTTON_LEFTSTICK)) buttons |= N64_BTN_Z;
    if (is_down(SDL_CONTROLLER_BUTTON_START)) buttons |= N64_BTN_START;
    if (is_down(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) buttons |= N64_BTN_L;
    if (is_down(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= N64_BTN_R;
    if (is_down(SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons |= N64_BTN_UP;
    if (is_down(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons |= N64_BTN_DOWN;
    if (is_down(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons |= N64_BTN_LEFT;
    if (is_down(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= N64_BTN_RIGHT;
    if (is_down(SDL_CONTROLLER_BUTTON_X)) buttons |= N64_BTN_C_UP;
    if (is_down(SDL_CONTROLLER_BUTTON_Y)) buttons |= N64_BTN_C_DOWN;

    // Right stick maps to the C buttons.
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) < -8000) buttons |= N64_BTN_C_UP;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) > 8000) buttons |= N64_BTN_C_DOWN;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) < -8000) buttons |= N64_BTN_C_LEFT;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) > 8000) buttons |= N64_BTN_C_RIGHT;

    return buttons;
}

static void poll_input() {
    // Deliberately does NOT pump SDL events.
    //
    // ultramodern calls this from the game thread (inside osContStartReadData),
    // and on macOS SDL_PumpEvents -> Cocoa_PumpEvents ->
    // [NSApp nextEventMatchingMask:...] raises
    //   NSInternalInconsistencyException: 'nextEventMatchingMask should only be
    //   called from the Main Thread!'
    // which terminates the process - 2 of 3 native runs died here, right after
    // the boot display list. The main thread already pumps every frame in
    // pump_sdl_events() (called from update_gfx), and reading SDL's keyboard and
    // controller *state* from another thread is fine; only pumping is not.
}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    uint16_t out_buttons = 0;
    bool connected = false;

    if (controller_num == 0) {
        out_buttons |= keyboard_buttons();
        connected = true;
    }

    SDL_GameController* controller = nullptr;
    if (controller_num >= 0 && controller_num < 4) {
        extern Platform g_platform;
        controller = g_platform.controllers[controller_num];
    }

    if (controller != nullptr) {
        out_buttons |= gamecontroller_buttons(controller);
        // Left stick maps to the N64 analog stick.
        constexpr float axis_scale = 1.0f / 32768.0f;
        *x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) * axis_scale;
        *y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) * axis_scale;
        connected = true;
    }

    if (!connected) {
        return false;
    }

    *buttons = out_buttons;
    return true;
}

static void set_rumble(int controller_num, bool rumble) {
    if (controller_num < 0 || controller_num >= 4) {
        return;
    }
    extern Platform g_platform;
    SDL_GameController* controller = g_platform.controllers[controller_num];
    if (controller != nullptr) {
        SDL_GameControllerRumble(controller, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0, 100);
    }
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    info.connected_device = ultramodern::input::Device::None;
    info.connected_pak = ultramodern::input::Pak::None;

    if (controller_num == 0) {
        info.connected_device = ultramodern::input::Device::Controller;
        return info;
    }

    if (controller_num >= 1 && controller_num < 4) {
        extern Platform g_platform;
        if (g_platform.controllers[controller_num] != nullptr) {
            info.connected_device = ultramodern::input::Device::Controller;
        }
    }
    return info;
}

ultramodern::input::callbacks_t make_input_callbacks() {
    return {
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
}


static void queue_audio_samples(int16_t* samples, size_t count) {
    extern Platform g_platform;
    if (g_platform.audio_device != 0) {
        SDL_QueueAudio(g_platform.audio_device, samples, static_cast<Uint32>(count * sizeof(int16_t)));
    }
}

static size_t get_audio_frames_remaining() {
    extern Platform g_platform;
    if (g_platform.audio_device == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(g_platform.audio_device) / sizeof(int16_t);
}

static void set_audio_frequency(uint32_t frequency) {
    extern Platform g_platform;
    if (frequency != g_platform.audio_frequency) {
        open_audio(g_platform, frequency);
    }
}

ultramodern::audio_callbacks_t make_audio_callbacks() {
    return {
        .queue_samples = queue_audio_samples,
        .get_frames_remaining = get_audio_frames_remaining,
        .set_frequency = set_audio_frequency,
    };
}

static void show_message_box(const char* msg) {
    extern Platform g_platform;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ogre Battle 64", msg, g_platform.window);
}

ultramodern::error_handling::callbacks_t make_error_handling_callbacks() {
    return {.message_box = show_message_box};
}

// Runs on the VI thread once per vblank; the synthetic-frame probe submits from
// here (see synth_frame.cpp).
static void vi_tick() {
    synth_frame_vi_tick(ultramodern::get_rdram_base());
}

ultramodern::events::callbacks_t make_events_callbacks() {
    return {
        .vi_callback = synth_frame_enabled() ? vi_tick : nullptr,
        .gfx_init_callback = nullptr,
    };
}

static std::string get_game_thread_name(const OSThread* thread) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "N64 Thread %d", thread->id);
    return std::string(buffer);
}

ultramodern::threads::callbacks_t make_threads_callbacks() {
    return {.get_game_thread_name = get_game_thread_name};
}

}  // namespace ogre

