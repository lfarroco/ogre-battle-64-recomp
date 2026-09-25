// Player-editable app settings for the port's own UI.
//
// The launcher's SETTINGS tab and the in-game Esc overlay show the same Panel
// (ui.hpp), so the live values and their file live here, as input_map.{hpp,cpp}
// does for the controller bindings: the panel reads them to draw a row and
// writes them through these functions, which apply the change and persist it.
//
// Two settings:
//
// * GAME SPEED, the runtime's emulated-clock multiplier
//   (`ultramodern::set_speed_multiplier`). n means the emulated CPU counter and
//   the VI retrace schedule run n times faster than wall clock, so timed
//   sequences complete in 1/n of the wall time. `OGRE_SPEED=<n>` sets the
//   starting value as a debug override; the SETTINGS tab then changes and saves
//   it.
// * WIDESCREEN, which puts RT64 into its Expand aspect ratio (a wider field of
//   view) for the mission. See `WidescreenMode` below and `widescreen.cpp` for
//   why it is not simply on.
//
// The file is `<config>/settings.cfg`, plain text.
#pragma once

#include <filesystem>
#include <string>

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

// The WIDESCREEN row. RT64's Expand aspect ratio widens the view: its
// `ProjectionProcessor` counter-scales the projection matrices, so a 3D scene
// keeps its proportions and gains width instead of being stretched
// (`tools/RT64/src/render/rt64_projection_processor.cpp:101-115`). The game's
// 2D screens were authored for 4:3, so `Off` is the default and `Missions`
// limits Expand to the mission scene (`0x03`). `Always` is offered because the
// choice belongs to the player, and the modes are what make the setting
// expressible at all.
enum class WidescreenMode {
    Off = 0,
    Missions = 1,
    Always = 2,
};

constexpr int kWidescreenModeCount = 3;

// "OFF", "MISSIONS" or "ALWAYS" for the radio row's `index`-th option.
const char* widescreen_mode_label(int index);

// The live mode.
WidescreenMode widescreen_mode();

// The index of the live mode in the radio row, or -1 when it is out of range.
int widescreen_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`. `widescreen.cpp` applies it
// on the next frame, so a change while the game runs takes effect immediately.
void set_widescreen_mode(WidescreenMode mode);

// The WIDESCREEN row's options as one string (radio markers and labels), for
// `right_text`; the drawn row uses the panel's own radio layout.
std::string widescreen_mode_text();

// The configuration file's path for a given config directory.
std::filesystem::path settings_path(const std::filesystem::path& pref_dir);

// Loads `<config>/settings.cfg` if it exists, applies the result to the
// runtime, and remembers the directory for later writes. `OGRE_SPEED` and
// `OGRE_WIDESCREEN` win over the file. Call once at startup, after
// `resolve_pref_dir()`.
void load_settings(const std::filesystem::path& pref_dir);

}  // namespace ogre
