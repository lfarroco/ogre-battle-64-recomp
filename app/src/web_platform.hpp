#pragma once

#include <cstdint>
#include <cstddef>

#include "ultramodern/ultramodern.hpp"

namespace ogre {

// Web (Emscripten) platform callbacks. The initial browser build runs the
// runtime with no-op audio/input (Milestone 5 adds real input/audio/save
// persistence; see docs/WEB-PORT.md §20). Everything is callback-driven, so
// the game/runtime never touches the browser directly.

ultramodern::input::callbacks_t make_input_callbacks();
ultramodern::audio_callbacks_t make_audio_callbacks();
ultramodern::error_handling::callbacks_t make_error_handling_callbacks();
ultramodern::events::callbacks_t make_events_callbacks();
ultramodern::threads::callbacks_t make_threads_callbacks();

}  // namespace ogre
