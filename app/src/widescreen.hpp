#pragma once

// Forward declaration: SDL typedefs `SDL_Window` as `struct SDL_Window`, so this
// header does not need SDL.
struct SDL_Window;

namespace ogre {

// Applies the WIDESCREEN setting to RT64's aspect ratio and reports whether the
// *mode* changed since the previous call.
//
// The setting (`settings.hpp`) is a toggle, not an aspect ratio: RT64 has one
// Expand mode, and this decides when it is on. `Off` is always 4:3, and `On` is
// Expand while the dispatcher runs scene `0x03` (the mission) and 4:3 elsewhere,
// in a window that stays 16:9 either way.
//
// Called once per frame from the app's `update_gfx` callback, because the
// scene can change on any frame. The function reads the live
// `ultramodern::renderer::GraphicsConfig` and writes it only when the wanted
// `ar_option` differs, so a steady scene costs one comparison per frame and the
// renderer sees one `update_config` at each transition.
//
// The return value is about the window and not about the scene: the window's
// shape follows the mode, so it changes only when the player changes the mode.
// A scene transition resizes nothing. In a widescreen mode the window stays
// wide and the 4:3 scenes are pillarboxed inside it, which is the presentation
// the developer asked for.
bool widescreen_update();

// Sets the window's width from its height and the shape the toggle asks for:
// 4:3 for `Off` and 16:9 for `On`. Called once right after the
// window is created, and again only when `widescreen_update` reports a mode
// change. A manual resize by the player is left alone.
void widescreen_fit_window(SDL_Window* window);

// The size a run should open its window at: 4:3 for `Off`, 16:9 otherwise.
void widescreen_initial_window_size(int& width, int& height);

}  // namespace ogre
