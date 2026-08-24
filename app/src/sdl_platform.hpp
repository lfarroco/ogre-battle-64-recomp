#pragma once

#include <cstdint>
#include <cstddef>

#include <SDL.h>

#include "ultramodern/ultramodern.hpp"

namespace ogre {

// Platform I/O for the app: SDL2 window, N64-style input, and SDL audio queue.
// The game itself never touches SDL; ultramodern calls these callbacks.
struct Platform {
    SDL_Window* window = nullptr;
    SDL_AudioDeviceID audio_device = 0;
    uint32_t audio_frequency = 0;

    // Tracked game controllers (slot i maps to N64 controller i).
    SDL_GameController* controllers[4] = {};
};

// Initializes SDL (video, audio, gamecontroller, events).
bool init_sdl();

// Creates the app window and returns the ultramodern window handle.
ultramodern::renderer::WindowHandle create_window(Platform& platform, const char* title);

// Stops audio and destroys SDL subsystems.
void shutdown_sdl(Platform& platform);

// Pump window/event loop; called from the main thread.
void pump_sdl_events(Platform& platform, bool* quit);

// Opens an SDL audio output device that the audio callbacks feed.
void open_audio(Platform& platform, uint32_t frequency);

// --- ultramodern callbacks ---------------------------------------------------
ultramodern::input::callbacks_t make_input_callbacks();
ultramodern::audio_callbacks_t make_audio_callbacks();
ultramodern::error_handling::callbacks_t make_error_handling_callbacks();
ultramodern::events::callbacks_t make_events_callbacks();
ultramodern::threads::callbacks_t make_threads_callbacks();

}  // namespace ogre
