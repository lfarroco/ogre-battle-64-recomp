// Player-editable app settings for the port's own UI.
//
// The launcher's SETTINGS tab and the in-game Esc overlay show the same Panel
// (ui.hpp), so the live values and their file live here, as input_map.{hpp,cpp}
// does for the controller bindings: the panel reads them to draw a row and
// writes them through these functions, which apply the change and persist it.
//
// One setting so far: GAME SPEED, the runtime's emulated-clock multiplier
// (`ultramodern::set_speed_multiplier`). n means the emulated CPU counter and
// the VI retrace schedule run n times faster than wall clock, so timed
// sequences complete in 1/n of the wall time. `OGRE_SPEED=<n>` sets the
// starting value as a debug override; the SETTINGS tab then changes and saves
// it. The file is `<config>/settings.cfg`, plain text.
#pragma once

#include <filesystem>

namespace ogre {

// The offered multipliers, in display order. This is the GAME SPEED row's radio
// group; index 0 is the default. 6 and 8 were offered first and were dropped:
// at those speeds the game's own cursor aims are too fast to control.
constexpr int kGameSpeedCount = 3;

// The multiplier at `index`, or 1 for an out-of-range index.
int game_speed_value(int index);
// The index of `value` in the table, or -1 when the live value did not come
// from it (only reachable through `OGRE_SPEED`, whose range is 1..64).
int game_speed_index(int value);
// The offered value closest to `value`, for a settings file that names one the
// panel no longer offers.
int nearest_game_speed(int value);

// The live multiplier.
int game_speed();

// Applies `multiplier` to the runtime and writes `<config>/settings.cfg`.
// Clamped to the runtime's range (1..64). Called from the UI.
void set_game_speed(int multiplier);

// The configuration file's path for a given config directory.
std::filesystem::path settings_path(const std::filesystem::path& pref_dir);

// Loads `<config>/settings.cfg` if it exists, applies the result to the
// runtime, and remembers the directory for later writes. `OGRE_SPEED` wins over
// the file. Call once at startup, after `resolve_pref_dir()`.
void load_settings(const std::filesystem::path& pref_dir);

}  // namespace ogre
