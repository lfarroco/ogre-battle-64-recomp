#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include <SDL.h>

#include "ultramodern/ultramodern.hpp"

namespace ogre {

// The live debug console (see the block comment in sdl_platform.cpp).
//   tick()      polls the watched command file and the 1..9 hotkeys
//   exec(line)  runs one command line — tick uses it, and so does the
//               scene/step trigger in bank_overlays.cpp
// Both run on the main thread.
namespace console {
bool tick();
void exec(const std::string& line);
}

// Platform I/O for the app: SDL2 window, N64-style input, and SDL audio queue.
// The game itself never touches SDL; ultramodern calls these callbacks.
struct Platform {
    SDL_Window* window = nullptr;
    SDL_AudioDeviceID audio_device = 0;
    uint32_t audio_frequency = 0;

    // macOS/Metal only: the SDL_MetalView backing the window's CAMetalLayer.
    // It is handed to RT64 as WindowHandle::view and must outlive the window,
    // so it is owned here and destroyed in shutdown_sdl().
    void* metal_view = nullptr;

    // Tracked game controllers (slot i maps to N64 controller i).
    SDL_GameController* controllers[4] = {};

    // --- self-driving runs (env-gated, see init_sdl) -------------------------
    //
    // The native app has no equivalent of the web probes' `tap()`: a run that
    // is never given input sits on the boot blanking display list forever
    // (session 24), so every native measurement needed a human at the keyboard.
    // OGRE_TAP_MS makes controller 0 press one scripted button (Start by
    // default; OGRE_TAP_BUTTON selects others/a schedule) for one poll every N
    // ms, and OGRE_EXIT_AFTER_MS makes the app request exit after N ms so a run
    // is bounded and scriptable. Both default to 0 (off): interactive runs
    // behave exactly as before.
    uint32_t tap_ms = 0;
    uint32_t exit_after_ms = 0;
    uint64_t start_ticks = 0;
};

// Initializes SDL (video, audio, gamecontroller, events).
bool init_sdl();

// Reads OGRE_TAP_MS / OGRE_TAP_BUTTON / OGRE_EXIT_AFTER_MS for scripted runs.
// Call after init_sdl.
void configure_automation(Platform& platform);

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
