#include "sdl_platform.hpp"
#include "synth_frame.hpp"
#include "bank_overlays.hpp"
#include "crash_log.hpp"
#include "input_map.hpp"
#include "overlay.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <librecomp/overlays.hpp>

#include <SDL.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath (the checkpoint build id)
#include <SDL_metal.h>
#include <SDL_syswm.h>
#elif defined(_WIN32)
#include <SDL_syswm.h>  // SDL_GetWindowWMInfo (RT64 wants the HWND)
#include <cstdlib>      // _pgmptr (the checkpoint build id)
#else
#include <unistd.h>  // readlink (the checkpoint build id)
#endif

// Defined in librecomp/src/recomp.cpp: the RDRAM base handed to the game.
extern "C" uint8_t* ultramodern_get_rdram_base();

namespace ogre {

// Defined below (the live debug console block); declared here because
// pump_sdl_events, above it, is the main-thread caller.
namespace console {
bool tick();
}

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

// N64 controller button bits and the player-editable bindings live in
// input_map.hpp; this file only reads the map.

namespace {

// Names accepted inside `OGRE_TAP_BUTTON` slots. Matched case-insensitively.
struct ButtonName {
    const char* name;
    uint16_t bit;
};

constexpr ButtonName kButtonNames[] = {
    {"a", N64_BTN_A},         {"b", N64_BTN_B},
    {"z", N64_BTN_Z},         {"start", N64_BTN_START},
    {"l", N64_BTN_L},         {"r", N64_BTN_R},
    {"up", N64_BTN_UP},       {"down", N64_BTN_DOWN},
    {"left", N64_BTN_LEFT},   {"right", N64_BTN_RIGHT},
    {"cu", N64_BTN_C_UP},     {"cd", N64_BTN_C_DOWN},
    {"cl", N64_BTN_C_LEFT},   {"cr", N64_BTN_C_RIGHT},
};

// `OGRE_TAP_BUTTON` is a comma-separated schedule, one entry per synthetic
// press, advanced once per press; the last entry sticks for the rest of the run
// (so the default single `start` is Start forever, and
// `start,start,start,start,start,a` is "press Start through the title, then
// A"). Slot 0 is the boot window, the same index OGRE_TAP_MAX counts. An entry
// is a button name or several names joined by '+' (e.g. `a`, `start`, `a+z`);
// `none` is a silent slot. This is what lets a scripted run press Start through
// the title (the only input that path wants) and then give a scene another
// button without a human, and it is how session 41 proved scene 0x0D is a fixed
// ~28.7 s cutscene that no button changes (a Start during it just drives the
// same scripted leave to 0x02).
constexpr size_t kMaxTapSlots = 64;

struct TapSchedule {
    uint16_t slots[kMaxTapSlots];
    size_t count;
};

std::string lowercase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// One slot: '+' separates buttons that are held together.
uint16_t parse_button_combo(const std::string& combo, bool& ok) {
    ok = true;
    uint16_t mask = 0;
    size_t start = 0;
    for (;;) {
        const size_t plus = combo.find('+', start);
        const std::string name = lowercase(trim(combo.substr(
            start, plus == std::string::npos ? std::string::npos : plus - start)));
        if (name != "none") {
            bool found = false;
            for (const ButtonName& button : kButtonNames) {
                if (name == button.name) {
                    mask |= button.bit;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ok = false;
            }
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
    return mask;
}

TapSchedule parse_tap_schedule(const char* spec) {
    TapSchedule schedule{{N64_BTN_START}, 1};
    schedule.count = 0;
    const std::string text(spec);
    size_t start = 0;
    for (;;) {
        const size_t comma = text.find(',', start);
        const std::string slot = text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        bool ok = true;
        uint16_t mask = parse_button_combo(slot, ok);
        if (schedule.count >= kMaxTapSlots) {
            fprintf(stderr, "[SDL] OGRE_TAP_BUTTON: more than %zu slots; rest ignored\n",
                    kMaxTapSlots);
            break;
        }
        if (!ok) {
            fprintf(stderr, "[SDL] OGRE_TAP_BUTTON: unrecognised button in slot '%s'; using Start\n",
                    trim(slot).c_str());
            mask = N64_BTN_START;
        }
        schedule.slots[schedule.count++] = mask;
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (schedule.count == 0) {
        schedule.slots[schedule.count++] = N64_BTN_START;
    }
    return schedule;
}

// Read once: a mid-run change should not move the schedule under the game.
const TapSchedule& tap_schedule() {
    static const TapSchedule schedule = [] {
        const char* spec = getenv("OGRE_TAP_BUTTON");
        if (spec == nullptr || spec[0] == '\0') {
            return TapSchedule{{N64_BTN_START}, 1};
        }
        return parse_tap_schedule(spec);
    }();
    return schedule;
}

// `OGRE_TAP_SCENE_BUTTON=<scene>:<buttons>[:<count>],...` - the scene-keyed
// sibling of `OGRE_TAP_BUTTON`.
//
// `OGRE_TAP_BUTTON`'s slot index advances with **wall time**, so a route that
// crosses several scenes has to be hand-tuned to how long each took: session 67's
// "title -> Load Game -> map" schedule landed on the map in one run and in the
// attract loop in the next, because the boot reaches the title anywhere between
// 1.6 s and 15 s (the forced-scene poke races the publisher stills). This knob
// keys the button on the **dispatcher's active scene** instead, so a route says
// what each screen wants rather than when:
//
//   OGRE_TAP_MS=1000 OGRE_TAP_SCENE_BUTTON="title:start:2,0x12:a:1,0x05:right:3,0x05:a:1"
//
// presses Start twice while the title runs, A once on the Load Game screen, then
// three Rights and one A on the map - however long each screen took to appear.
// An entry's `count` is how many presses it may spend (default: unlimited); an
// entry with none left is skipped, and the first entry that still matches the
// active scene wins. Entries in the same scene are consumed in order, so
// `0x05:right:3,0x05:a:1` is "walk right three times, then press A once".
struct SceneButton {
    std::string scene;   // a `kScenes` name or a hex id
    uint16_t buttons = 0;
    long count = -1;     // -1 = unlimited
    long used = 0;
};

std::string describe_buttons(uint16_t mask);  // defined below

std::vector<SceneButton>& scene_button_schedule() {
    static std::vector<SceneButton> schedule = [] {
        std::vector<SceneButton> out;
        const char* spec = getenv("OGRE_TAP_SCENE_BUTTON");
        if (spec == nullptr || spec[0] == '\0') {
            return out;
        }
        const std::string text(spec);
        size_t start = 0;
        while (start <= text.size()) {
            const size_t comma = text.find(',', start);
            const std::string entry = text.substr(
                start, (comma == std::string::npos) ? std::string::npos : comma - start);
            // `scene:buttons` with an optional `:count`.
            const size_t colon = entry.find(':');
            const size_t colon2 = (colon == std::string::npos)
                                      ? std::string::npos
                                      : entry.find(':', colon + 1);
            if (colon != std::string::npos) {
                SceneButton item;
                item.scene = trim(entry.substr(0, colon));
                const std::string buttons = trim(entry.substr(
                    colon + 1, (colon2 == std::string::npos) ? std::string::npos : colon2 - colon - 1));
                bool ok = true;
                item.buttons = parse_button_combo(buttons, ok);
                if (colon2 != std::string::npos) {
                    item.count = strtol(entry.c_str() + colon2 + 1, nullptr, 0);
                }
                if (!ok) {
                    fprintf(stderr, "[SDL] OGRE_TAP_SCENE_BUTTON: unrecognised button in '%s'; skipping\n",
                            entry.c_str());
                } else if (item.scene.empty()) {
                    fprintf(stderr, "[SDL] OGRE_TAP_SCENE_BUTTON: no scene in '%s'; skipping\n",
                            entry.c_str());
                } else {
                    out.push_back(item);
                }
            } else {
                fprintf(stderr, "[SDL] OGRE_TAP_SCENE_BUTTON: '%s' is not <scene>:<buttons>[:<count>]\n",
                        entry.c_str());
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        for (const SceneButton& item : out) {
            fprintf(stderr, "[SDL] OGRE_TAP_SCENE_BUTTON: scene %s -> %s%s\n", item.scene.c_str(),
                    describe_buttons(item.buttons).c_str(),
                    (item.count < 0) ? "" : " (counted)");
        }
        fflush(stderr);
        return out;
    }();
    return schedule;
}

// The scene-keyed tap, or 0 when no entry applies this interval. Reports how
// many presses the winning entry has spent through `press_number` so the caller
// can log once per press. The result is cached per tap interval, because the
// game polls input more than once per frame and a count must not burn twice.
uint16_t scene_button_tap(uint64_t tap, long& press_number) {
    static uint64_t consumed_tap = ~0ull;
    static uint16_t cached = 0;
    static long cached_press = 0;
    if (tap == consumed_tap) {
        press_number = cached_press;
        return cached;
    }
    consumed_tap = tap;
    cached = 0;
    cached_press = 0;
    std::vector<SceneButton>& schedule = scene_button_schedule();
    if (!schedule.empty()) {
        const uint16_t scene = ogre::active_scene_id();
        for (SceneButton& item : schedule) {
            if ((item.count >= 0) && (item.used >= item.count)) {
                continue;
            }
            if (!ogre::scene_list_matches(item.scene.c_str(), scene)) {
                continue;
            }
            item.used++;
            cached = item.buttons;
            cached_press = item.used;
            break;
        }
    }
    press_number = cached_press;
    return cached;
}

std::string describe_buttons(uint16_t mask) {
    if (mask == 0) {
        return "none";
    }
    std::string out;
    for (const ButtonName& button : kButtonNames) {
        if ((mask & button.bit) != 0) {
            if (!out.empty()) {
                out += '+';
            }
            out += button.name;
        }
    }
    return out;
}

}  // namespace

bool init_sdl() {
    // The app keeps its own main (the MSVC build defines SDL_MAIN_HANDLED), so
    // tell SDL not to wait for SDL_main's setup before SDL_Init. No-op on the
    // platforms where SDL does not wrap main.
    SDL_SetMainReady();
    // Only video and events are required. SDL_Init fails the whole call when any
    // requested subsystem fails, and audio genuinely fails on a machine with no
    // usable output device (a VM, an RDP session, a stopped Windows Audio
    // service): SDL_Init(SDL_INIT_AUDIO) then returns "No available audio
    // device". Asking for it in the same call made the app exit before it
    // created a window, which on a console-less build looked like "nothing
    // happens" (session 94). Audio and controllers are brought up separately and
    // may be absent; the game runs silently without them.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[SDL] Failed to init: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL] audio unavailable, running silently: %s\n", SDL_GetError());
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[SDL] game controllers unavailable: %s\n", SDL_GetError());
    }
    return true;
}

// A boot failure used to end the process with no window, no text and no file.
// The macOS package and the Windows build have no console, so that is
// indistinguishable from "the program does nothing" (session 94). Write the
// captured log to error.log and put the reason on screen.
void report_boot_failure(const std::string& message) {
    crash_log::write_error_log(message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ogre Battle 64", message.c_str(), nullptr);
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
    if (platform.tap_ms != 0) {
        // Report the scene gate up front so a run's stderr says why taps stop.
        if (const char* s = getenv("OGRE_TAP_SCENE")) {
            fprintf(stderr, "[SDL] taps only while scene in '%s'\n", s);
        }
        if (const char* s = getenv("OGRE_TAP_NOT_SCENE")) {
            fprintf(stderr, "[SDL] taps only while scene NOT in '%s'\n", s);
        }
        const TapSchedule& schedule = tap_schedule();
        fprintf(stderr, "[SDL] tap schedule (%zu slots, last sticks):", schedule.count);
        for (size_t i = 0; i < schedule.count; ++i) {
            fprintf(stderr, " %s", describe_buttons(schedule.slots[i]).c_str());
        }
        fprintf(stderr, "\n");
    }
}

ultramodern::renderer::WindowHandle create_window(Platform& platform, const char* title) {
    uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
    // RT64 creates a CAMetalLayer on the window; the null-renderer build needs
    // no such flag (SDL ignores it there, but keeping the two in step avoids
    // surprising anyone who ports the flag set to another platform).
#if defined(OGRE_USE_RT64)
    flags |= SDL_WINDOW_METAL;
#endif
#elif defined(__linux__) || defined(__ANDROID__)
    // Only the RT64 build renders through Vulkan. Asking for SDL_WINDOW_VULKAN
    // unconditionally makes window creation *fail* on a Linux/Windows machine
    // with no Vulkan-capable driver or no Vulkan loader, which is exactly the
    // machine the null-renderer variant exists for (found by running the
    // headless Linux build: "Failed to create window: Vulkan support is either
    // not configured in SDL or not available in current SDL video driver").
#if defined(OGRE_USE_RT64)
    flags |= SDL_WINDOW_VULKAN;
#endif
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
#elif defined(_WIN32)
    // Windows WindowHandle is `{ HWND window; DWORD thread_id; }`: RT64 creates
    // its D3D12/Vulkan surface from the HWND, and installs a
    // SetWindowsHookEx(WH_GETMESSAGE) filter on thread_id when it does not have
    // an SDL window to attach to (rt64_application_window.cpp).
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fprintf(stderr, "[SDL] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return {};
    }
    ultramodern::renderer::WindowHandle handle;
    handle.window = wm_info.info.win.window;
    handle.thread_id = GetCurrentThreadId();
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

bool PadList::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which);
            if (controller != nullptr) {
                for (SDL_GameController*& slot : pads) {
                    if (slot == nullptr) {
                        slot = controller;
                        return true;
                    }
                }
                // No free host slot; do not leave the handle open.
                SDL_GameControllerClose(controller);
            }
            return true;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            for (SDL_GameController*& slot : pads) {
                if (slot != nullptr &&
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot)) ==
                        event.cdevice.which) {
                    SDL_GameControllerClose(slot);
                    slot = nullptr;
                    return true;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

void PadList::close_all() {
    for (SDL_GameController*& slot : pads) {
        if (slot != nullptr) {
            SDL_GameControllerClose(slot);
            slot = nullptr;
        }
    }
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
    // Enumerate once: SDL reports already-connected controllers with
    // SDL_CONTROLLERDEVICEADDED events pushed at SDL_InitSubSystem time, and the
    // launcher's own event loop drains those before the game window exists
    // (the start screen is shown whenever a mod is installed). Without this a
    // pad that was connected before launch is invisible to the game.
    {
        static bool enumerated = false;
        if (!enumerated) {
            enumerated = true;
            for (int device = 0; device < SDL_NumJoysticks(); device++) {
                if (!SDL_IsGameController(device)) {
                    continue;
                }
                SDL_GameController* controller = SDL_GameControllerOpen(device);
                if (controller == nullptr) {
                    continue;
                }
                // Slot 0 is keyboard-first, so a pad fills slots 1..3.
                bool placed = false;
                for (int slot = 1; slot < 4; slot++) {
                    if (platform.controllers[slot] == nullptr) {
                        platform.controllers[slot] = controller;
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    SDL_GameControllerClose(controller);
                }
            }
            if (platform.controllers[1] != nullptr) {
                fprintf(stderr, "[input] controller 2: %s\n",
                        SDL_GameControllerName(platform.controllers[1]));
            }
        }
    }

    // Live debug console: the watched command file and the OGRE_KEY_<n>
    // hotkeys. Runs before the exit check so a command sent on the final frame
    // still executes, and on the main thread because a command may write a
    // multi-megabyte dump (see the console block in this file).
    console::tick();

    // Bounded run: OGRE_EXIT_AFTER_MS ends the process from the main thread.
    if (platform.exit_after_ms != 0 && platform_millis(platform) >= platform.exit_after_ms) {
        const uint32_t budget = platform.exit_after_ms;
        platform.exit_after_ms = 0;
        // The queue snapshot's per-thread view is a block *kind* and a resume
        // count, which cannot say where a thread that is simply not being
        // scheduled is sitting. The recompiled code reports every function
        // entry and return, so the runtime also knows the live call chain.
        fprintf(stderr, "[SDL] exit_after_ms=%u elapsed, stopping\n", budget);
        // OGRE_DMA_TRACE=1: the process leaves through _exit, so the streamed-DMA
        // summary has to be printed here rather than from an atexit handler.
        dump_dma_trace();
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
        // OGRE_COVER=1: every recompiled function this run entered, one per
        // line, for `tools/recompcov.py --log` (see that tool).
        ultramodern::debug_cover_dump();
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
        // _Exit skips atexit, so the captured stdout/stderr has to be drained
        // here or the run log loses its last lines.
        crash_log::flush();
        _Exit(EXIT_SUCCESS);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // The in-game overlay sees every event first: it owns Escape (open and
        // close) and, while it is up, the keyboard and mouse.
        ogre::overlay_handle_event(event);
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

// The synthetic press of a scripted run (`OGRE_TAP_MS` + `OGRE_TAP_BUTTON`),
// or 0.
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
    // `OGRE_TAP_SCENE=<list>` / `OGRE_TAP_NOT_SCENE=<list>`: scope the synthetic
    // press to the *scene* the dispatcher reports (`D_800E810E`) instead of to
    // wall time. Wall-clock schedules drift with `OGRE_SPEED`, capture readback
    // and load times, which is how a run meant for New Game ended up driving the
    // attract loop; `OGRE_TAP_NOT_SCENE=new-game` presses Start through the
    // title and goes silent the moment New Game is confirmed.
    {
        static const char* tap_scene = getenv("OGRE_TAP_SCENE");
        static const char* tap_not_scene = getenv("OGRE_TAP_NOT_SCENE");
        if (tap_scene != nullptr || tap_not_scene != nullptr) {
            const uint16_t scene = ogre::active_scene_id();
            if (tap_scene != nullptr && !ogre::scene_list_matches(tap_scene, scene)) {
                return 0;
            }
            if (tap_not_scene != nullptr && ogre::scene_list_matches(tap_not_scene, scene)) {
                return 0;
            }
        }
    }
    const uint64_t elapsed = platform_millis(g_platform);
    const uint64_t phase = elapsed % g_platform.tap_ms;
    const uint64_t tap = elapsed / g_platform.tap_ms;
    if (phase >= kTapHoldMs) {
        return 0;
    }
    // `OGRE_TAP_SCENE_BUTTON`: the scene-keyed schedule replaces the wall-clock
    // slot schedule entirely when it is set (`tap_schedule()` above).
    if (getenv("OGRE_TAP_SCENE_BUTTON") != nullptr) {
        long press = 0;
        const uint16_t buttons = scene_button_tap(tap, press);
        static uint64_t logged_scene_taps = ~0ull;
        if (buttons != 0 && tap != logged_scene_taps) {
            logged_scene_taps = tap;
            fprintf(stderr,
                    "[SDL] automation scene tap %ld at %llums scene=0x%04X buttons=%s\n",
                    press, (unsigned long long)elapsed, (unsigned)ogre::active_scene_id(),
                    describe_buttons(buttons).c_str());
            fflush(stderr);
        }
        return buttons;
    }
    // `OGRE_TAP_MAX=<n>`: stop tapping after tap number n (tap 0 is the boot
    // window). Lets a run press Start through the title and then go silent, so
    // later taps cannot drive the scene's own scripted leave before the run has
    // measured the visit. 0 or unset = tap forever.
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
    const TapSchedule& schedule = tap_schedule();
    const size_t slot = tap < schedule.count ? static_cast<size_t>(tap) : schedule.count - 1;
    const uint16_t buttons = schedule.slots[slot];
    static uint64_t logged_taps = 0;
    if (tap != logged_taps && tap > 0) {
        logged_taps = tap;
        fprintf(stderr, "[SDL] automation tap %llu at %llums buttons=%s\n",
                (unsigned long long)tap, (unsigned long long)elapsed,
                describe_buttons(buttons).c_str());
    }
    return buttons;
}

// Defined below (with the console's `press` command); declared here because the
// game thread's input poll consumes the hold.
uint16_t console_input_take(float* x, float* y);

static uint16_t keyboard_buttons() {
    // The map holds one scancode per N64 button (see input_map.cpp); a key the
    // player clears contributes nothing. The synthetic press of a scripted run
    // is OR'd on top.
    uint16_t buttons = keyboard_buttons_from_map(read_input_map());
    buttons |= automation_buttons();
    return buttons;
}

// The live console's `press <buttons> [polls] [x] [y]` hold: a synthetic press
// that lasts a fixed number of input polls and then releases, so an external
// tool can walk a menu interactively ("press right", look at the capture,
// "press a") instead of guessing a wall-clock tap schedule. `x`/`y` set the
// analog stick (screens that move a cursor with the stick rather than the
// D-pad, e.g. the world map). Written from the console on the main thread, read
// from the game thread's poll, hence the atomics.
std::atomic<uint16_t> g_console_buttons{0};
std::atomic<int> g_console_polls{0};
std::atomic<float> g_console_x{0.0f};
std::atomic<float> g_console_y{0.0f};

void console_press(uint16_t buttons, int polls, float x, float y) {
    g_console_buttons.store(buttons, std::memory_order_relaxed);
    g_console_x.store(x, std::memory_order_relaxed);
    g_console_y.store(y, std::memory_order_relaxed);
    g_console_polls.store(polls, std::memory_order_relaxed);
}

uint16_t console_input_take(float* x, float* y) {
    if (g_console_polls.load(std::memory_order_relaxed) <= 0) {
        return 0;
    }
    g_console_polls.fetch_sub(1, std::memory_order_relaxed);
    *x = g_console_x.load(std::memory_order_relaxed);
    *y = g_console_y.load(std::memory_order_relaxed);
    return g_console_buttons.load(std::memory_order_relaxed);
}

static uint16_t gamecontroller_buttons(SDL_GameController* controller) {
    if (controller == nullptr) {
        return 0;
    }
    uint16_t buttons = gamecontroller_buttons_from_map(read_input_map(), controller);

    // The right stick is a fixed extra layer for the C buttons, in addition to
    // whatever the map binds to them. C-LEFT and C-RIGHT have no default pad
    // button, so without this they would be unreachable on a pad.
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) < -8000) buttons |= N64_BTN_C_UP;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) > 8000) buttons |= N64_BTN_C_DOWN;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) < -8000) buttons |= N64_BTN_C_LEFT;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) > 8000) buttons |= N64_BTN_C_RIGHT;

    return buttons;
}

// --- live debug console ---------------------------------------------------
//
// The problem this solves: almost every wall in this project is "what is in
// guest RAM at the moment X happens", and a bounded run can only dump at its
// exit. By then the interesting buffer has usually been overwritten (session
// 56 lost an afternoon to exactly that). The console lets a running game be
// queried on demand:
//
//   * a watched command file (default `/tmp/ogre-console.txt`, override with
//     `OGRE_CONSOLE_FILE`): when it exists, each line is executed and the file
//     is removed. An agent can therefore drive a live run with its own tools.
//   * number keys `1`..`9` run `OGRE_KEY_1`..`OGRE_KEY_9` (a key press is an
//     edge, so one command per press).
//
// Both paths land in the same command set:
//
//   r  <addr> [count]        words (int-dump order)
//   rh <addr> [count]        halfwords
//   rb <addr> [count]        logical bytes (XOR-3)
//   rk <addr> [count]        raw host-order 32-bit words
//   d  <addr> [len]          hex/ascii dump of a byte range
//   f  <value> [limit]       find a 32-bit word in RDRAM (int-dump order)
//   fb <hexbytes> [limit]    find a byte pattern
//   s  <addr> [len] [max]    strings in a range
//   k  <addr> [len]          checksum (fnv1a) of a range, for before/after
//   w  <addr> <value>        store a 32-bit word (A/B experiments)
//   c  [bytes]               report the scene/descriptor/state words
//   dump [path]              write the whole 8 MiB RDRAM image right now
//   save [path]              write a checkpoint (RDRAM + overlay state)
//   load [path]              restore a checkpoint written by this build
//   help                     this list
//
// Addresses are guest KSEG0 (`0x80xxxxxx`) or bare hex; a leading `0x` is
// optional. Output goes to stdout (prefixed `[console]`) and is flushed, so
// `./build-app/ogrebattle64 ... | tee run.log` is enough to keep it.
//
// The commands run on the **main thread**, from `pump_sdl_events` (called by
// update_gfx), not from the game thread: writing a multi-megabyte dump while
// the game thread is inside a recompiled function would race its own reads,
// and the main thread already owns the SDL event pump.
namespace console {

constexpr uint32_t kConsoleMaxTokens = 12;

std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Accepts `0x1F0A00`, `1F0A00` and `8018FC3C`. Returns false on junk.
bool parse_num(const std::string& text, uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* p = text.c_str();
    char* end = nullptr;
    const unsigned long long value = strtoull(p, &end, 16);
    if (end == p || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

std::string fmt_bytes(const uint8_t* p, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        if (i != 0) out.push_back(' ');
        out.push_back(hex[p[i] >> 4]);
        out.push_back(hex[p[i] & 0xF]);
    }
    return out;
}

// The runtime byte-reverses RDRAM, so the logical word at guest `a` is the
// little-endian word at `a - 0x80000000` and a logical byte is `addr ^ 3`
// (docs/guides/app-build.md -> "Reading a dump"). These accessors are the same
// rules the offline `tools/rdram.py` implements.
inline uint32_t console_word(const uint8_t* base, uint32_t addr) {
    const uint32_t off = (addr & 0x1FFFFFFFu);
    uint32_t v;
    memcpy(&v, base + off, 4);
    return v;
}

// A *halfword* needs the XOR-2 as well: the runtime stores guest bytes
// byte-reversed inside each word (logical byte at `a` == `rdram[(a & 0x1FFFFFFF) ^ 3]`),
// so the logical halfword at an even `a` is the little-endian halfword at
// `(a & 0x1FFFFFFE) ^ 2` — not at `a`. Reading it without the XOR silently
// returns the *neighbouring* halfword, which is how `c` printed `step` and
// `next` swapped (the game stores the step with an `sh` at `0x8018F1C0`, so its
// value appeared under `next`) until session 58 fixed this accessor.
inline uint16_t console_half(const uint8_t* base, uint32_t addr) {
    const uint32_t off = (addr & 0x1FFFFFFEu) ^ 2u;
    uint16_t v;
    memcpy(&v, base + off, 2);
    return v;
}

inline uint8_t console_byte(const uint8_t* base, uint32_t addr) {
    return base[(addr & 0x1FFFFFFFu) ^ 3u];
}

// --- checkpoints -----------------------------------------------------------
//
// `save`/`load` are the fast-testing instrument: instead of replaying the whole
// New Game opening (movie -> cathedral -> name form -> birthday -> personality
// questions, ~45 s at 4x) to reach a scene, snapshot the machine once and jump
// back to it. The file is the whole RDRAM image plus the runtime's overlay state
// (which recompiled body runs at each RAM address, i.e. which streamed bank is
// resident) — see recomp::overlays::get_overlay_state_blob. Without that second
// half a restore would rewind the game's data but leave the function map on a
// later bank, and the restored scene would run the wrong module's code.
//
// Everything else (the game's own RDRAM pointers, its framebuffers, the heap)
// travels inside the image, so a checkpoint is a full machine rewind: the game
// re-runs from that point on load.
//
// Limits, deliberately: the file is only valid for the binary that wrote it
// (the overlay blob is versioned and checksummed), the console does not pause
// the game threads, and the app's own knobs (`OGRE_TAP_*`, `OGRE_SCENE`, the
// input schedule) keep their current position rather than being part of the
// snapshot.
constexpr char kCheckpointMagic[8] = {'O', 'G', 'R', 'E', 'C', 'K', 'P', 'T'};
constexpr uint32_t kCheckpointVersion = 2;  // 2: added build_id (see below)

struct CheckpointHeader {
    char magic[8];
    uint32_t version;
    uint32_t rdram_size;
    uint32_t host_size;
    uint32_t flags;
    uint64_t checksum;  // FNV-1a over host blob + RDRAM image
    uint64_t build_id;  // FNV-1a of this executable; a rebuild invalidates
};

uint64_t fnv1a_bytes(uint64_t hash, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr uint32_t kRdramSize = 0x800000u;

// A checkpoint is only meaningful for the binary that wrote it: the overlay
// blob carries no function pointers (they are process-local), so a restore in a
// *different* build would re-register this process's bodies for the saved bank
// extents — which after a recompile can be different code. The blob's own
// version/size checks catch a format change but not "same format, new code", so
// the header also carries a hash of the running executable. Reading 20 MB costs
// a few ms, once per save/load.
bool executable_fingerprint(uint64_t& out, std::string& error) {
    char path[4096];
    uint32_t size = (uint32_t)sizeof(path);
#if defined(__APPLE__)
    if (_NSGetExecutablePath(path, &size) != 0) {
        error = "cannot locate the running executable";
        return false;
    }
#elif defined(_WIN32)
    // _pgmptr is the full path of the running executable, set by the CRT.
    if (strlen(_pgmptr) >= sizeof(path)) {
        error = "cannot locate the running executable";
        return false;
    }
    strcpy(path, _pgmptr);
#else
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        error = "cannot locate the running executable";
        return false;
    }
    path[n] = '\0';
#endif
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        error = "cannot open the running executable";
        return false;
    }
    uint64_t hash = kFnv1aOffsetBasis;
    uint8_t buf[65536];
    // Declared inside the loop so it cannot collide with the `n` the non-Apple
    // branch above uses for readlink's return value (which is const there).
    while (size_t n = fread(buf, 1, sizeof(buf), f)) {
        hash = fnv1a_bytes(hash, buf, n);
    }
    fclose(f);
    out = hash;
    return true;
}

bool write_checkpoint(const char* path, uint8_t* rdram, std::string& error) {
    const std::vector<uint8_t> host = recomp::overlays::get_overlay_state_blob();

    CheckpointHeader header{};
    memcpy(header.magic, kCheckpointMagic, sizeof(header.magic));
    header.version = kCheckpointVersion;
    header.rdram_size = kRdramSize;
    header.host_size = (uint32_t)host.size();
    header.flags = 0;
    if (!executable_fingerprint(header.build_id, error)) {
        return false;
    }
    uint64_t hash = fnv1a_bytes(kFnv1aOffsetBasis, host.data(), host.size());
    hash = fnv1a_bytes(hash, rdram, kRdramSize);
    header.checksum = hash;

    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        error = "could not open for writing";
        return false;
    }
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
              fwrite(host.data(), 1, host.size(), f) == host.size() &&
              fwrite(rdram, 1, kRdramSize, f) == kRdramSize;
    fclose(f);
    if (!ok) {
        error = "short write";
    }
    return ok;
}

bool read_checkpoint(const char* path, uint8_t* rdram, std::string& error) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        error = "could not open for reading";
        return false;
    }
    CheckpointHeader header{};
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        error = "file is shorter than the header";
        return false;
    }
    if (memcmp(header.magic, kCheckpointMagic, sizeof(header.magic)) != 0) {
        fclose(f);
        error = "bad magic (not an OGRE checkpoint)";
        return false;
    }
    if (header.version != kCheckpointVersion) {
        fclose(f);
        error = "checkpoint version mismatch";
        return false;
    }
    if (header.rdram_size != kRdramSize) {
        fclose(f);
        error = "checkpoint RDRAM size mismatch";
        return false;
    }
    if (header.host_size > 64u * 1024u * 1024u) {
        fclose(f);
        error = "implausible overlay-state size";
        return false;
    }
    std::vector<uint8_t> host(header.host_size);
    std::vector<uint8_t> image(kRdramSize);
    const bool read_ok = (host.empty() || fread(host.data(), 1, host.size(), f) == host.size()) &&
                         fread(image.data(), 1, image.size(), f) == image.size();
    fclose(f);
    if (!read_ok) {
        error = "short read";
        return false;
    }
    uint64_t hash = fnv1a_bytes(kFnv1aOffsetBasis, host.data(), host.size());
    hash = fnv1a_bytes(hash, image.data(), image.size());
    if (hash != header.checksum) {
        error = "checksum mismatch (truncated or edited file)";
        return false;
    }
    // Same format but new code: the blob has no function pointers, so a restore
    // in a rebuilt binary would bind this process's bodies to the saved extents.
    // Refuse rather than run the wrong module (see executable_fingerprint).
    uint64_t build_id = 0;
    if (!executable_fingerprint(build_id, error)) {
        return false;
    }
    if (build_id != header.build_id) {
        error = "checkpoint was written by a different build of this executable";
        return false;
    }
    // Overlay state first: if the runtime rejects the blob, the RDRAM image has
    // not been touched yet.
    if (!recomp::overlays::restore_overlay_state_blob(host)) {
        error = "overlay state rejected (checkpoint from another build?)";
        return false;
    }
    memcpy(rdram, image.data(), image.size());
    return true;
}

uint32_t range_check(uint32_t addr, uint32_t len) {
    return addr >= 0x80000000u && addr + len <= 0x80800000u;
}

void console_exec(const std::string& line_in) {
    const std::string line = trim_copy(line_in);
    if (line.empty() || line[0] == '#') {
        return;
    }
    uint8_t* rdram = ultramodern_get_rdram_base();
    if (rdram == nullptr) {
        printf("[console] RDRAM not mapped yet; ignoring \"%s\"\n", line.c_str());
        fflush(stdout);
        return;
    }
    std::string tok[kConsoleMaxTokens];
    size_t ntok = 0;
    {
        size_t i = 0;
        while (i < line.size() && ntok < kConsoleMaxTokens) {
            while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) i++;
            size_t start = i;
            while (i < line.size() && !isspace(static_cast<unsigned char>(line[i]))) i++;
            if (i > start) tok[ntok++] = line.substr(start, i - start);
        }
    }
    if (ntok == 0) {
        return;
    }
    const std::string cmd = lower_copy(tok[0]);
    printf("[console] > %s\n", line.c_str());

    auto num = [&](size_t idx, uint32_t def) -> uint32_t {
        uint32_t v = def;
        if (idx < ntok && parse_num(tok[idx], v)) {
            return v;
        }
        return def;
    };

    if (cmd == "help") {
        printf("[console] r/rh/rb/rk <addr> [n] | d <addr> [len] | f <value> [max] | fb <hex> [max]\n"
               "[console] s <addr> [len] [max] | k <addr> [len] | w <addr> <value> | c\n"
               "[console] dmatrace        (print the streamed-DMA bases seen so far; needs OGRE_DMA_TRACE=1)\n"
               "[console] cover           (print the recompiled functions entered so far; needs OGRE_COVER=1)\n"
               "[console] press <buttons> [polls] [x] [y]  (hold a synthetic pad press and analog\n"
               "[console]                stick, e.g. `press a`, `press start+down`, `press none 60 -1 0`)\n"
               "[console] dump [path]   (bare `dump` writes /tmp/ogre-rdram-NNNN.bin, one per press)\n"
               "[console] save [path]   (checkpoint: RDRAM + overlay state; bare `save` writes\n"
               "[console]                /tmp/ogre-checkpoint-NNNN.ckpt)\n"
               "[console] load [path]   (restore a checkpoint this build wrote; bare `load` uses\n"
               "[console]                the most recent bare `save`)\n");
    } else if (cmd == "press") {
        bool ok = true;
        const uint16_t mask = (ntok > 1) ? parse_button_combo(tok[1], ok) : 0;
        const int polls = (ntok > 2) ? (int)num(2, 8) : 8;
        const float sx = (ntok > 3) ? (float)atof(tok[3].c_str()) : 0.0f;
        const float sy = (ntok > 4) ? (float)atof(tok[4].c_str()) : 0.0f;
        if (!ok || ntok < 2) {
            printf("[console] press <buttons> [polls] [stickx] [sticky]: buttons are `+`-joined names"
                   " (start,a,b,up,down,left,right,cu,cd,cl,cr,zl,zr,l,r,none);"
                   " `press none 60 -1 0` is a full-left analog stick for 60 polls\n");
        } else {
            console_press(mask, polls, sx, sy);
            printf("[console] pressing %s for %d poll(s), stick=(%.2f,%.2f)\n",
                   describe_buttons(mask).c_str(), polls, sx, sy);
        }
    } else if (cmd == "r" || cmd == "rw" || cmd == "rh" || cmd == "rb" || cmd == "rk") {
        const uint32_t addr = num(1, 0);
        uint32_t count = num(2, 1);
        if (count == 0) count = 1;
        if (count > 256) count = 256;
        if (!range_check(addr, count * 4)) {
            printf("[console] out of RDRAM: 0x%08X\n", addr);
        } else if (cmd == "rb") {
            std::string s = fmt_bytes(rdram + ((addr & 0x1FFFFFFFu) ^ 3u), count);
            printf("[console] bytes 0x%08X: %s\n", addr, s.c_str());
        } else {
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t a = addr + i * (cmd == "rh" ? 2u : 4u);
                if (cmd == "rh") {
                    printf("[console] %08X = %04X\n", a, (unsigned)console_half(rdram, a));
                } else if (cmd == "rk") {
                    uint32_t raw;
                    memcpy(&raw, rdram + (a & 0x1FFFFFFFu), 4);
                    printf("[console] %08X = %08X (raw)\n", a, raw);
                } else {
                    printf("[console] %08X = %08X\n", a, console_word(rdram, a));
                }
            }
        }
    } else if (cmd == "d") {
        const uint32_t addr = num(1, 0);
        uint32_t len = num(2, 64);
        if (len > 1024) len = 1024;
        if (!range_check(addr, len)) {
            printf("[console] out of RDRAM: 0x%08X+0x%X\n", addr, len);
        } else {
            for (uint32_t i = 0; i < len; i += 16) {
                char ascii[17];
                for (uint32_t j = 0; j < 16 && i + j < len; j++) {
                    const uint8_t b = console_byte(rdram, addr + i + j);
                    ascii[j] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                }
                ascii[std::min<uint32_t>(16, len - i)] = '\0';
                uint8_t raw[16];
                for (uint32_t j = 0; j < 16 && i + j < len; j++) {
                    raw[j] = console_byte(rdram, addr + i + j);
                }
                printf("[console] %08X  %-47s  %s\n", addr + i,
                       fmt_bytes(raw, std::min<uint32_t>(16, len - i)).c_str(), ascii);
            }
        }
    } else if (cmd == "f") {
        const uint32_t value = num(1, 0);
        uint32_t limit = num(2, 32);
        if (limit > 256) limit = 256;
        uint32_t found = 0;
        for (uint32_t off = 0; off + 4 <= 0x800000u && found < limit; off += 4) {
            uint32_t v;
            memcpy(&v, rdram + off, 4);
            if (v == value) {
                printf("[console] found %08X at guest 0x%08X (file 0x%06X)\n", value,
                       0x80000000u + off, off);
                found++;
            }
        }
        printf("[console] find %08X: %u hit(s)%s\n", value, found,
               found >= limit ? " (limit reached)" : "");
    } else if (cmd == "fb") {
        uint8_t pat[32];
        size_t plen = 0;
        {
            const std::string hex = tok[1];
            for (size_t i = 0; i + 1 < hex.size() && plen < sizeof(pat); i += 2) {
                const std::string b = hex.substr(i, 2);
                pat[plen++] = static_cast<uint8_t>(strtoul(b.c_str(), nullptr, 16));
            }
        }
        uint32_t limit = num(2, 16);
        if (limit > 64) limit = 64;
        uint32_t found = 0;
        if (plen == 0) {
            printf("[console] fb needs hex bytes\n");
        } else {
            for (uint32_t off = 0; off + plen <= 0x800000u && found < limit; off++) {
                if (memcmp(rdram + off, pat, plen) == 0) {
                    printf("[console] pattern at guest 0x%08X\n", 0x80000000u + off);
                    found++;
                }
            }
            printf("[console] fb: %u hit(s)\n", found);
        }
    } else if (cmd == "s") {
        const uint32_t addr = num(1, 0);
        uint32_t len = num(2, 256);
        uint32_t max = num(3, 16);
        if (len > 0x10000) len = 0x10000;
        if (max > 64) max = 64;
        if (!range_check(addr, len)) {
            printf("[console] out of RDRAM: 0x%08X+0x%X\n", addr, len);
        } else {
            uint32_t found = 0;
            char buf[64];
            size_t n = 0;
            for (uint32_t i = 0; i < len && found < max; i++) {
                const uint8_t b = console_byte(rdram, addr + i);
                if (b >= 32 && b < 127 && n + 1 < sizeof(buf)) {
                    buf[n++] = static_cast<char>(b);
                } else {
                    if (n >= 4) {
                        buf[n] = '\0';
                        printf("[console] str at 0x%08X: \"%s\"\n", addr + i - (uint32_t)n, buf);
                        found++;
                    }
                    n = 0;
                }
            }
            if (n >= 4 && found < max) {
                buf[n] = '\0';
                printf("[console] str at 0x%08X: \"%s\"\n", addr + (uint32_t)(len - n), buf);
                found++;
            }
            printf("[console] s: %u string(s) in 0x%X bytes\n", found, len);
        }
    } else if (cmd == "k") {
        const uint32_t addr = num(1, 0);
        uint32_t len = num(2, 0x1000);
        if (!range_check(addr, len)) {
            printf("[console] out of RDRAM: 0x%08X+0x%X\n", addr, len);
        } else {
            uint32_t h = 2166136261u;
            const uint32_t off = addr & 0x1FFFFFFFu;
            for (uint32_t i = 0; i < len; i++) {
                h ^= rdram[off + i];
                h *= 16777619u;
            }
            printf("[console] fnv1a(0x%08X, 0x%X) = %08X\n", addr, len, h);
        }
    } else if (cmd == "w") {
        const uint32_t addr = num(1, 0);
        const uint32_t value = num(2, 0);
        if (!range_check(addr, 4)) {
            printf("[console] out of RDRAM: 0x%08X\n", addr);
        } else {
            memcpy(rdram + (addr & 0x1FFFFFFFu), &value, 4);
            printf("[console] wrote %08X to 0x%08X\n", value, addr);
        }
    } else if (cmd == "c") {
        const uint32_t scene = console_half(rdram, 0x800E810Eu);
        const uint32_t pending = console_half(rdram, 0x800E8214u);
        const uint32_t desc = console_word(rdram, 0x800E8294u);
        const uint32_t mask = desc ? console_word(rdram, desc + 0x10u) : 0;
        const uint32_t step = console_half(rdram, 0x8018F1C0u);
        const uint32_t next = console_half(rdram, 0x8018F1C2u);
        const uint32_t spin = console_half(rdram, 0x800C4C26u);
        printf("[console] scene=0x%04X pending=0x%04X desc=0x%08X mask=0x%08X step=%u next=0x%04X spin=0x%04X\n",
               scene, pending, desc, mask, step, next, spin);
    } else if (cmd == "dmatrace" || cmd == "dm") {
        // Print the accumulated streamed-DMA bases *now* instead of only on the
        // bounded-run exit path (`OGRE_DMA_TRACE=1` + `OGRE_DMA_TRACE_FULL=1`).
        // This is how a module the game loads into RAM the port has no record
        // for is found while the interesting screen is still on: run to the
        // screen, drop `dmatrace` into the watched file, and compare the bases
        // with `app/src/bank_funcs.inc`.
        dump_dma_trace();
    } else if (cmd == "cover" || cmd == "cv") {
        // OGRE_COVER=1: the recompiled functions entered so far, now rather than
        // at exit — so a long interactive session can snapshot its coverage
        // without ending the run (`tools/recompcov.py --log` reads the same
        // lines from stdout).
        ultramodern::debug_cover_dump();
    } else if (cmd == "dump") {
        // A bare `dump` never overwrites an earlier one: the key can be pressed
        // as often as the developer likes and every snapshot is kept, numbered.
        // `dump <path>` still writes exactly where it is told.
        static uint32_t dump_seq = 0;
        std::string path;
        if (ntok > 1) {
            path = tok[1];
        } else {
            char auto_path[128];
            snprintf(auto_path, sizeof(auto_path), "/tmp/ogre-rdram-%04u.bin", ++dump_seq);
            path = auto_path;
        }
        if (FILE* f = fopen(path.c_str(), "wb")) {
            const size_t written = fwrite(rdram, 1, 0x800000u, f);
            fclose(f);
            printf("[console] dumped %zu bytes to %s\n", written, path.c_str());
        } else {
            printf("[console] could not open %s\n", path.c_str());
        }
    } else if (cmd == "save" || cmd == "load") {
        // Checkpoints: see the block comment above `write_checkpoint`. The file
        // carries the RDRAM image *and* the runtime's overlay state, so a load
        // rewinds the machine to the instant of the save.
        static uint32_t save_seq = 0;
        static std::string last_save;
        std::string path;
        if (ntok > 1) {
            path = tok[1];
        } else if (cmd == "save") {
            char auto_path[128];
            snprintf(auto_path, sizeof(auto_path), "/tmp/ogre-checkpoint-%04u.ckpt", ++save_seq);
            path = auto_path;
            last_save = path;
        } else if (!last_save.empty()) {
            path = last_save;
        } else {
            path = "/tmp/ogre-checkpoint.ckpt";
        }

        std::string error;
        // The game runs on its own host threads, so a multi-megabyte read or
        // write here must wait for every thread executing recompiled code to
        // park at a function boundary — otherwise the image tears (session 58:
        // the game kept submitting RSP tasks and building frames *during* the
        // write, so the file was a mix of frames). See
        // ultramodern::checkpoint_pause_begin.
        const int parked = ultramodern::checkpoint_pause_begin();
        if (cmd == "save") {
            if (write_checkpoint(path.c_str(), rdram, error)) {
                printf("[console] checkpoint saved to %s (scene=0x%04X step=%u, %d thread(s) parked)\n",
                       path.c_str(), (unsigned)console_half(rdram, 0x800E810Eu),
                       (unsigned)console_half(rdram, 0x8018F1C0u), parked);
            } else {
                printf("[console] save failed: %s\n", error.c_str());
            }
        } else {
            // Report the *pre-restore* state too: a load that silently does
            // nothing is the failure mode that matters (checksum/version).
            const uint32_t before_scene = console_half(rdram, 0x800E810Eu);
            const uint32_t before_step = console_half(rdram, 0x8018F1C0u);
            if (read_checkpoint(path.c_str(), rdram, error)) {
                printf("[console] checkpoint loaded from %s (%d thread(s) parked)\n", path.c_str(),
                       parked);
                printf("[console]   scene 0x%04X step=%u -> scene 0x%04X step=%u\n",
                       before_scene, before_step,
                       (unsigned)console_half(rdram, 0x800E810Eu),
                       (unsigned)console_half(rdram, 0x8018F1C0u));
            } else {
                printf("[console] load failed: %s\n", error.c_str());
            }
        }
        ultramodern::checkpoint_pause_end();
    } else {
        printf("[console] unknown command \"%s\" (try help)\n", cmd.c_str());
    }
    fflush(stdout);
}

// Run one command line from outside the main-thread poll (the scene trigger in
// bank_overlays.cpp). Same semantics as a line in the watched file.
void exec(const std::string& line) {
    console_exec(line);
}

// Poll the watched command file and the number-key triggers. Returns true when
// a command ran, so the caller can skip other edge handling for that frame.
bool tick() {
    bool ran = false;

    // 1. The watched file. Written by an external tool (an agent can create it
    //    mid-run), read wholesale, then removed so it fires exactly once.
    //    `OGRE_CONSOLE_AT_MS=<n>` delays every read until n ms of wall clock have
    //    elapsed: a scripted run can leave the file in place at launch (no
    //    background writer, no race with the harness) and the command still lands
    //    on a chosen frame.
    {
        static const char* path = getenv("OGRE_CONSOLE_FILE") ? getenv("OGRE_CONSOLE_FILE")
                                                              : "/tmp/ogre-console.txt";
        static const uint64_t boot_ticks = SDL_GetTicks64();
        static const uint32_t at_ms = [] {
            const char* v = getenv("OGRE_CONSOLE_AT_MS");
            return (v != nullptr) ? (uint32_t)strtoul(v, nullptr, 10) : 0u;
        }();
        if (at_ms == 0 || (uint32_t)(SDL_GetTicks64() - boot_ticks) >= at_ms) {
        if (FILE* f = fopen(path, "rb")) {
            std::string text;
            char buf[512];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                text.append(buf, n);
            }
            fclose(f);
            remove(path);
            size_t pos = 0;
            while (pos <= text.size()) {
                const size_t nl = text.find('\n', pos);
                const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (!trim_copy(line).empty()) {
                    console_exec(line);
                    ran = true;
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
        }
    }

    // 2. `1`..`9` -> OGRE_KEY_<n>. Edge-triggered so a held key runs once.
    {
        static bool was_down[10] = {};
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        for (int d = 1; d <= 9; d++) {
            const SDL_Scancode sc = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (d - 1));
            const bool down = keys[sc] != 0;
            if (down && !was_down[d]) {
                char env[32];
                snprintf(env, sizeof(env), "OGRE_KEY_%d", d);
                if (const char* cmd = getenv(env)) {
                    if (cmd[0] != '\0') {
                        printf("[console] key %d -> %s\n", d, cmd);
                        console_exec(cmd);
                        ran = true;
                    }
                } else {
                    printf("[console] key %d pressed; set %s=\"<command>\" to bind it\n", d, env);
                    fflush(stdout);
                }
            }
            was_down[d] = down;
        }
    }

    return ran;
}

}  // namespace console

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
    // The in-game overlay (Esc) owns the keyboard while it is open. The game
    // still runs behind it, so report an idle pad rather than a disconnected
    // one: a `false` here would make the game's own menus say "no controller".
    if (ogre::overlay_visible()) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr, "[input] overlay open: game input suppressed\n");
            fflush(stderr);
        }
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    uint16_t out_buttons = 0;
    bool connected = false;
    *x = 0.0f;
    *y = 0.0f;
    // The live console's synthetic press/stick, if one is armed.
    float console_x = 0.0f, console_y = 0.0f;
    uint16_t console = 0;

    if (controller_num == 0) {
        out_buttons |= keyboard_buttons();
        console = console_input_take(&console_x, &console_y);
        out_buttons |= console;
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

    // The console's stick wins over an idle pad, so a scripted run can move a
    // cursor that is stick-driven (the world map) with no hardware attached.
    if (console != 0 || console_x != 0.0f || console_y != 0.0f) {
        *x = console_x;
        *y = console_y;
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
        // OB64 saves to a Controller Pak; the device itself (a flat 32 KiB image
        // kept in `<config>/saves/<game id>.mpk`) is in
        // librecomp/src/pak.cpp, which bridges __osContRamRead/__osContRamWrite/
        // __osPfsGetStatus. This bit is what osContInit reports and what the
        // game's save menu checks for "Insert Controller Pak." (session 65).
        info.connected_pak = ultramodern::input::Pak::ControllerPak;
        // One line per process so a run's log says whether the game was told a
        // pak is inserted (the device itself logs `[pak]` on its first access).
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr, "[input] controller 1: Controller Pak inserted\n");
            fflush(stderr);
        }
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
    // The runtime's fatal-error channel. There is no console in a distributed
    // build, so put the same message in error.log for the player to submit.
    // This does not consume the one-report guard: a real crash afterwards still
    // writes its own report.
    crash_log::write_error_log(msg);
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

