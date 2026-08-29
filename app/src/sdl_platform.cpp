#include "sdl_platform.hpp"

#include <cstdio>

#include <SDL.h>

namespace ogre {

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
    // For the Metal backend: hand RT64/ultramodern the SDL window and the
    // CAMetalLayer SDL creates for it.
    //
    // NOTE: SDL_Metal_GetLayer segfaults with Homebrew's sdl2-compat on this
    // setup, so the view is left null for now. The null renderer doesn't use it;
    // the RT64 Metal integration will need a working Metal layer (possibly via
    // SDL3 or a direct CAMetalLayer creation).
    ultramodern::renderer::WindowHandle handle;
    handle.window = window;
    handle.view = nullptr;  // SDL_Metal_GetLayer(window) segfaults w/ sdl2-compat
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
    if (platform.window != nullptr) {
        SDL_DestroyWindow(platform.window);
        platform.window = nullptr;
    }
    SDL_Quit();
}

void open_audio(Platform& platform, uint32_t frequency) {
    if (platform.audio_device != 0) {
        SDL_CloseAudioDevice(platform.audio_device);
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
    // Keep the keyboard/gamecontroller state current. Safe to call from any thread.
    SDL_PumpEvents();
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

ultramodern::events::callbacks_t make_events_callbacks() {
    return {
        .vi_callback = nullptr,
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

