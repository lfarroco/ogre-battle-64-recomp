// The in-game overlay (Esc): the launcher's panel over the running game.
//
// The game window is owned by RT64 (a Metal/Vulkan/D3D12 swap chain), and the
// renderer has no UI pass, so the overlay is a separate borderless,
// always-on-top SDL window placed over the game window. It draws with the same
// bitmap font and the same `ui::Panel` as the launcher, which is what keeps the
// two screens in one style. SDL window opacity dims it so the game shows
// through.
//
// While the overlay is visible `ogre::overlay_visible()` is true, and the
// input callbacks report an idle pad instead of the keyboard and controller, so
// the game cannot act on the keys used to navigate the panel. The game itself
// keeps running (the developer chose "suppress input only").
//
// The overlay is a development-verifiable path: `OGRE_OVERLAY=1` opens it at
// startup and `OGRE_CAPTURE_OVERLAY=<path>` writes the window's pixels as a PPM.
#pragma once

#include <filesystem>
#include <string>

#include <SDL.h>

#include "sdl_platform.hpp"

namespace ogre {

// Creates the panel. Call after the game window exists (create_window), because
// the overlay is positioned over it. The mod list is not shown in Overlay mode;
// the game id is still passed because Panel construction reads the mod folder.
void overlay_init(Platform& platform, const std::filesystem::path& pref_dir,
                  const std::string& mod_game_id);

// Destroys the overlay window and renderer.
void overlay_shutdown();

// One SDL event from the main-thread pump. Ignored while the overlay is hidden.
void overlay_handle_event(const SDL_Event& event);

// Opens the overlay if hidden, closes it if visible.
void overlay_toggle();

// True while the overlay is up. Read from the game thread's input poll, so it is
// a plain atomic flag.
bool overlay_visible();

// True once the MAIN tab's EXIT GAME row has been activated. `update_gfx` polls
// it next to the window-close flag, so the two exit paths share one teardown.
bool overlay_quit_requested();

// How many frames the game has produced this run, from its own frame counter
// `D_800AEFA4` (`func_80072398`), or -1 while the runtime has no image (which
// includes the whole time before the renderer exists). 0 during the first
// seconds of a boot. `update_gfx` uses it to tell an exit that may run the
// runtime's teardown from one that must leave the process instead: an exit
// before the game produces frames leaves `recomp::start` blocked in
// `game_thread.join` or throws out of the runtime's shutdown (both seen on
// macOS), because the game thread is still inside `ultramodern::preinit` or RT64
// is still building its renderer.
int overlay_frames_produced();

// Draws one frame of the overlay (main thread, from update_gfx).
void overlay_render();

}  // namespace ogre
