// Player-editable app settings for the port's own UI.
//
// The launcher's SETTINGS tab and the in-game Esc overlay show the same Panel
// (ui.hpp), so the live values and their file live here, as input_map.{hpp,cpp}
// does for the controller bindings: the panel reads them to draw a row and
// writes them through these functions, which apply the change and persist it.
//
// Four settings:
//
// * GAME SPEED, the runtime's emulated-clock multiplier
//   (`ultramodern::set_speed_multiplier`). n means the emulated CPU counter and
//   the VI retrace schedule run n times faster than wall clock, so timed
//   sequences complete in 1/n of the wall time. `OGRE_SPEED=<n>` sets the
//   starting value as a debug override; the SETTINGS tab then changes and saves
//   it.
// * WIDESCREEN, a toggle that puts RT64 into its Expand aspect ratio (a wider
//   field of view) for the mission. See `WidescreenMode` below and
//   `widescreen.cpp`.
// * SOUNDS and VOLUME, the audio output's mute and gain. The app's audio
//   callback scales every buffer it queues by `audio_gain_percent()`. See
//   `SoundMode` and the block below it.
//
// Eight settings in four groups, in the order the SETTINGS tab draws them:
//
// * GAME SPEED.
// * DISPLAY: WIDESCREEN, then RESOLUTION, MSAA and WINDOW, which are the rest of
//   `ultramodern::renderer::GraphicsConfig`. The renderer applies all three on a
//   change (`RT64Renderer::update_config`, app/src/renderer.cpp), so a row
//   changed under the Esc overlay takes effect on the next frame. `widescreen.cpp`
//   owns `ar_option` and this file owns `res_option`, `msaa_option` and
//   `wm_option`; each writer starts from the live config, so neither loses the
//   other's value.
// * AUDIO: SOUNDS and VOLUME.
// * INPUT: MOUSE ENABLED, off by default, which is what lets the pointer drive
//   the world map's and the mission field's cursors. See `MouseMode` below.
//
// The file is `<config>/settings.cfg`, plain text.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ogre {

// The offered multipliers, in display order. This is the GAME SPEED row's radio
// group; index 0 is the default. 6 and 8 were offered first and were dropped in
// session 93: at those speeds the game's own cursor aims are too fast to
// control. 8 is offered again at the developer's request (session 137), which
// also added the speed hotkeys below; 6 stays out.
constexpr int kGameSpeedCount = 4;

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
// Clamped to the runtime's range (1..64). Called from the UI and the speed
// hotkeys. Logs `[settings] game speed x<n>` on every change.
void set_game_speed(int multiplier);

// The GAME SPEED hotkeys' step: `direction` negative moves to the next lower
// offered value, positive to the next higher one. The value it lands on is the
// nearest offered one in that direction, so a run started on `OGRE_SPEED=6`
// steps 6 -> 8 up and 6 -> 4 down. Returns whether the multiplier changed: at
// 1x a down press and at 8x an up press change nothing. Called from
// `sdl_platform.cpp`'s event pump.
bool step_game_speed(int direction);

// The pad's speed button (R3, see `sdl_platform.cpp`): the next offered value,
// **wrapping** 8 -> 1, because one button has no direction. A run started on a
// value the row does not offer joins at the first offered value above it (6 ->
// 8), or at 1 when there is none (64 -> 1). Returns whether the multiplier
// changed.
bool cycle_game_speed();

// The WIDESCREEN row: a toggle. RT64's Expand aspect ratio widens the view: its
// `ProjectionProcessor` counter-scales the projection matrices, so a 3D scene
// keeps its proportions and gains width instead of being stretched
// (`tools/RT64/src/render/rt64_projection_processor.cpp:101-115`). The game's 2D
// screens were authored for 4:3, so `Off` is the default and Expand is on only
// while the dispatcher runs the mission scene (`0x03`). The window itself is
// 16:9 for the whole run whenever the toggle is on, and the 4:3 scenes are
// pillarboxed inside it.
//
// Earlier builds offered `missions` and `always` as separate modes. The two
// opened the same 16:9 window and pillarboxed the same 4:3 scenes, so the split
// is gone (developer, session 105). The loader still reads the old spellings as
// `On`.
enum class WidescreenMode {
    Off = 0,
    On = 1,
};

constexpr int kWidescreenModeCount = 2;

// "OFF" or "ON" for the radio row's `index`-th option.
const char* widescreen_mode_label(int index);

// The live mode.
WidescreenMode widescreen_mode();

// The index of the live mode in the radio row, or -1 when it is out of range.
int widescreen_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`. `widescreen.cpp` applies it
// on the next frame, so a change while the game runs takes effect immediately.
void set_widescreen_mode(WidescreenMode mode);

// The scenes RT64's Expand aspect ratio is used for. Only scene `0x03`, the
// mission, is in the default set: it is the scene the setting exists for, and
// its field is 3D across the whole frame, so `ProjectionProcessor` can widen it
// without stretching the game's 2D layout. Every other scene keeps its 4:3
// authoring and is presented pillarboxed.
// `OGRE_WS_SCENES=<hex>[,<hex>...]` replaces the set for a developer run.
//
// This list lives here, and not in `widescreen.cpp`, because the browser build
// compiles `settings.cpp` and not `widescreen.cpp`: both targets ask the same
// question and must answer it the same way.
//
// Scene `0x0D` (the dialogue/cutscene engine) is deliberately absent. Its
// cutscene frames are 2D draws across the whole 4:3 frame, and RT64's Expand
// maps those draws into its wide viewport, so the columns outside the game's
// own 320-pixel image show content that is not part of the 4:3 frame
// (developer report, issue #27). RT64 has no mid-scene signal
// for a 4:3-authored draw, and the game does not emit the extended GBI commands
// that would provide one, so the only correct presentation for it is 4:3.
const std::vector<uint16_t>& widescreen_expand_scenes();

// Whether the toggle should Expand the given scene.
bool widescreen_scene_expands(uint16_t scene);

// The WIDESCREEN row's options as one string (radio markers and labels), for
// `right_text`; the drawn row uses the panel's own radio layout.
std::string widescreen_mode_text();

// RESOLUTION and MSAA are experimental and shipped behind a flag. Both are
// implemented and reach RT64, but a player run with them produced visual
// artifacts (developer, session 121), so they are off and their rows are absent
// from the SETTINGS tab unless `OGRE_DISPLAY_EXPERIMENTS=1` asks for them. The
// gate also accepts a set `OGRE_RESOLUTION` or `OGRE_MSAA`, so the debug
// spellings keep working. While it is off the two values are not applied at all
// (native resolution, no antialiasing) and the values already in
// `settings.cfg` are kept for a later run with the flag on.
bool display_experiments_enabled();

// The RESOLUTION row: how many pixels RT64 renders before it scales the picture
// to the window. `Native` is the game's own target (320x240), which is what the
// port drew before the row existed; `Double` renders at twice that in each axis;
// `Auto` renders at the largest integer multiple of the native resolution that
// fits the window (RT64's `WindowIntegerScale`). The names are the row's own;
// `settings.cpp` maps them to `ultramodern::renderer::Resolution`.
//
// Experimental (`display_experiments_enabled`). `resolution_mode()` returns the
// mode in use, which is `Native` while the gate is off.
//
// The enumerators follow the row's display order, so `Native` is the marker of
// the first option and the default.
enum class ResolutionMode {
    Native = 0,
    Double = 1,
    Auto = 2,
};

constexpr int kResolutionModeCount = 3;

// "NATIVE", "2X" or "AUTO" for the radio row's `index`-th option.
const char* resolution_mode_label(int index);

// The live mode.
ResolutionMode resolution_mode();

// The index of the live mode in the radio row.
int resolution_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`. Applied on the next frame.
void set_resolution_mode(ResolutionMode mode);

// The RESOLUTION row's options as one string (radio markers and labels).
std::string resolution_mode_text();

// The MSAA row: RT64's multisample antialiasing on the render target. `Off` is
// the default and what the port did before the row existed. MSAA affects 3D
// edges only; the 2D screens are drawn as rectangles and are unchanged.
//
// Experimental, on the same gate as RESOLUTION. `antialias_mode()` returns `Off`
// while the gate is off.
enum class AntialiasMode {
    Off = 0,
    Msaa2X = 1,
    Msaa4X = 2,
    Msaa8X = 3,
};

constexpr int kAntialiasModeCount = 4;

// "OFF", "2X", "4X" or "8X" for the radio row's `index`-th option.
const char* antialias_mode_label(int index);

// The live mode.
AntialiasMode antialias_mode();

// The index of the live mode in the radio row.
int antialias_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`. Applied on the next frame,
// which also rebuilds the render targets the sample count belongs to.
void set_antialias_mode(AntialiasMode mode);

// The MSAA row's options as one string (radio markers and labels).
std::string antialias_mode_text();

// The WINDOW row: fullscreen or not. Fullscreen is RT64's, not SDL's: the
// renderer is handed the toggle and calls its backend's `setFullScreen`, so the
// window keeps its size and the swap chain follows the display mode.
enum class DisplayMode {
    Windowed = 0,
    Fullscreen = 1,
};

constexpr int kDisplayModeCount = 2;

// "WINDOWED" or "FULLSCREEN" for the radio row's `index`-th option.
const char* display_mode_label(int index);

// The live mode.
DisplayMode display_mode();

// The index of the live mode in the radio row.
int display_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`. Applied on the next frame.
void set_display_mode(DisplayMode mode);

// The WINDOW row's options as one string (radio markers and labels).
std::string display_mode_text();

// The SOUNDS row: the same two-option radio group as WIDESCREEN. `Off` queues
// silence instead of skipping the buffer, because the runtime paces the game's
// audio generation from the queued depth (`ultramodern::audio_dma_busy()` reads
// `get_frames_remaining`): a mute that queued nothing would leave the AI idle
// and make the game overproduce.
//
// The enumerators follow the row's display order, so `On` is the marker of the
// first option and the default, as index 0 is on the other radio rows.
enum class SoundMode {
    On = 0,
    Off = 1,
};

constexpr int kSoundModeCount = 2;

// "ON" or "OFF" for the radio row's `index`-th option.
const char* sound_mode_label(int index);

// The live mode. `On` is the default, and `On` with volume 100 is what the app
// did before the row existed.
SoundMode sound_mode();

// The index of the live mode in the radio row.
int sound_mode_index();

// Stores `mode` and writes `<config>/settings.cfg`.
void set_sound_mode(SoundMode mode);

// The SOUNDS row's options as one string (radio markers and labels).
std::string sound_mode_text();

// The MOUSE ENABLED row: the same two-option radio group as WIDESCREEN, and
// `Off` is the default. `On` makes the pointer drive the two screen cursors the
// port implements: the world map's (scene `0x05`, screen pixels at
// `state+0x52`/`+0x54`) and the mission field's (scene `0x03`, f32 framebuffer
// pixels at `state+0x08`/`+0x0C`), including the click that acts as the game's
// `A` and the edge push that scrolls either view. `Off` is the state every build
// before the row had, made the default here at the developer's request; the two
// cursor modules read the same value through `ogre_map_cursor_set_enabled` and
// `ogre_mission_cursor_set_enabled`, and `OGRE_MAP_CURSOR` /
// `OGRE_MISSION_CURSOR` still override it for one run.
//
// The enumerators follow the row's display order, so `Off` is the marker of the
// first option and the default, as index 0 is on the other radio rows.
enum class MouseMode {
    Off = 0,
    On = 1,
};

constexpr int kMouseModeCount = 2;

// "OFF" or "ON" for the radio row's `index`-th option.
const char* mouse_mode_label(int index);

// The live mode.
MouseMode mouse_mode();

// The index of the live mode in the radio row.
int mouse_mode_index();

// Stores `mode`, publishes it to the cursor modules and writes
// `<config>/settings.cfg`. Both cursors read the value on their next frame, so a
// change under the Esc overlay takes effect immediately.
void set_mouse_mode(MouseMode mode);

// The MOUSE ENABLED row's options as one string (radio markers and labels).
std::string mouse_mode_text();

// The VOLUME row: a slider over 0..100 percent, drawn as `kVolumeCells`
// characters with the handle on one of them.
constexpr int kVolumeStepPercent = 5;
constexpr int kVolumeCells = 11;
constexpr int kVolumeDefaultPercent = 100;

// The live volume, 0..100.
int volume_percent();

// The cell the handle is drawn on for `percent`, 0..kVolumeCells-1.
int volume_handle_cell(int percent);

// The percent a click on slider cell `cell` sets.
int volume_percent_for_cell(int cell);

// The slider's text: the bar with its handle, a space and the percent, for
// example `-----|----- 50%`.
std::string volume_text(int percent);

// Stores `percent` (clamped to 0..100) and writes `<config>/settings.cfg`.
void set_volume_percent(int percent);

// The gain the audio callbacks scale every queued sample by: 0 while SOUNDS is
// `Off`, else the volume. Atomic, because the game's audio thread reads it while
// the main thread (the panel) writes it.
int audio_gain_percent();

// The configuration file's path for a given config directory.
std::filesystem::path settings_path(const std::filesystem::path& pref_dir);

// Loads `<config>/settings.cfg` if it exists, applies the result to the
// runtime, and remembers the directory for later writes. `OGRE_SPEED`,
// `OGRE_WIDESCREEN`, `OGRE_RESOLUTION`, `OGRE_MSAA`, `OGRE_WINDOW`,
// `OGRE_SOUNDS` and `OGRE_VOLUME` win over the file, and
// `OGRE_DISPLAY_EXPERIMENTS` gates the two experimental rows. Call once at
// startup, after `resolve_pref_dir()`.
void load_settings(const std::filesystem::path& pref_dir);

}  // namespace ogre
