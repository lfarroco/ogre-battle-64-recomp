#pragma once

#include <cstdint>
#include <cstddef>

#include "ultramodern/ultramodern.hpp"

namespace ogre {

// Web (Emscripten) platform callbacks (Milestone 5: real input + audio; see
// docs/WEB-PORT.md §20). Everything is callback-driven, so the game/runtime
// never touches the browser directly:
//
//   Input: the browser main thread captures keyboard/gamepad events and pushes
//   the state into wasm atomics via ogre_input_set(); the runtime's input
//   callbacks read the same atomics on the game thread.
//
//   Audio: the game thread pushes interleaved stereo int16 samples into a wasm
//   ring buffer (queue_samples); an AudioWorklet consumes them straight from
//   the SharedArrayBuffer using ogre_audio_state_ptr()/ogre_audio_ring_ptr().
//
//   Save: the config dir (/ogre) is mounted on IDBFS by the web shell so the
//   validated ROM copy and any save data persist across page loads.

ultramodern::input::callbacks_t make_input_callbacks();
ultramodern::audio_callbacks_t make_audio_callbacks();
ultramodern::error_handling::callbacks_t make_error_handling_callbacks();
ultramodern::events::callbacks_t make_events_callbacks();
ultramodern::threads::callbacks_t make_threads_callbacks();

}  // namespace ogre

// Exports used by the web shell (app/web/web.js). Declared here so the symbol
// names are visible to other TUs; the Emscripten linker exports them by name
// (see the EXPORTED_FUNCTIONS list in app/CMakeLists.txt).
extern "C" {

// Push the current state of one controller slot from JS.
void ogre_input_set(int controller, uint16_t buttons, float x, float y, int connected);

// Current slot-0 button mask (debug / test helper).
uint16_t ogre_input_debug();

// Byte offsets (within wasm linear memory) of the audio ring-buffer state and
// sample data, for the AudioWorklet (app/web/audio-worklet.js).
uint32_t ogre_audio_state_ptr();
uint32_t ogre_audio_ring_ptr();

// Number of stereo frames currently buffered (debug / test helper).
uint32_t ogre_audio_frames_available();

}
