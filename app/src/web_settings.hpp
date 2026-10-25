#pragma once

#include <cstdint>

// The browser build's bridge to the settings the native Esc overlay owns.
//
// The native port loads its panel values from `<config>/settings.cfg` and reads
// them from `settings.cpp` (`load_settings`). The browser has the same file on
// its IDBFS `/ogre` mount, so the settings module is compiled into the wasm
// target as well and these exports are the page's way to read and change the
// values. Input bindings are the exception: the page's input path is
// JavaScript, so the CONTROLS tab keeps its bindings in localStorage; see
// `app/web/web.js`.
//
// `web_settings_load` runs from the web boot, after the JS shell has mounted
// IDBFS and before `recomp::start`, which is the point the native app calls
// `load_settings`.
namespace ogre {

void web_settings_load();

// True when WIDESCREEN is On and the active scene is one the wide view is
// meant for (the mission `0x03`, the dialogue scene `0x0D`, an HD-background
// covered scene, or an `OGRE_WS_SCENES` entry). The web renderer uses it to
// counter-scale the projection matrix, the way RT64's Expand aspect does.
bool web_widescreen_expand();

}  // namespace ogre

extern "C" {

// Reads `/ogre/settings.cfg` and applies it to the runtime. Called from
// `main_web.cpp`; not needed by the page.
void ogre_web_settings_load();

// GAME SPEED, the values the SETTINGS tab offers. `set` clamps to 1..64 and
// writes settings.cfg.
int ogre_web_settings_game_speed();
void ogre_web_settings_set_game_speed(int multiplier);

// WIDESCREEN: 0 off, 1 on.
int ogre_web_settings_widescreen();
void ogre_web_settings_set_widescreen(int on);

// MOUSE ENABLED: 0 off, 1 on. This also publishes the value to the two cursor
// modules, so the pointer works on the map and the mission field.
int ogre_web_settings_mouse();
void ogre_web_settings_set_mouse(int on);

// Whether the two experimental display rows (RESOLUTION, MSAA) are enabled for
// this run. They are not implemented by the browser renderer; the page shows
// them disabled when the native gate is on.
int ogre_web_settings_display_experiments();

// The game's live Chaos Frame byte from guest RDRAM, or -1 while the runtime has
// no image. The DEBUG tab reads it once per update.
int ogre_web_chaos_frame();

// The dispatcher's active scene id (`D_800E810E`), or 0xFFFF when unknown.
int ogre_web_active_scene();

// 1 while the wide projection should be applied. Same value as
// `ogre::web_widescreen_expand`, exported for the page's canvas aspect class.
int ogre_web_widescreen_expand();

// Pointer input for the two cursor modules. Coordinates are CSS pixels inside
// the canvas; `draw_w`/`draw_h` are its drawing-buffer size. The web shell calls
// these from pointermove/pointerdown on #game-canvas.
void ogre_web_mouse_move(int x, int y, int win_w, int win_h, int draw_w, int draw_h);
void ogre_web_mouse_click(int x, int y, int win_w, int win_h, int draw_w, int draw_h);
void ogre_web_mouse_leave();

}  // extern "C"
