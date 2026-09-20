// The app's start screen: a black window that asks for the ROM.
//
// This is the very first thing a distributed build shows. The game runtime has
// not started yet (no RDRAM, no RT64), so the screen is drawn with SDL's 2D
// renderer and the app's built-in bitmap font (font.hpp). It returns the path of
// a ROM the runtime has accepted, or an empty path if the user closed the
// window.
//
// A ROM can be supplied by clicking (a native file dialog) or by dropping the
// file onto the window. `run_launcher` also shows the runtime's validation
// error and keeps asking, so a wrong file is a message, not a process exit.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace ogre {

// --- config directory ---------------------------------------------------------

// The directory of the running executable (SDL_GetBasePath), or the current
// directory if SDL cannot report one.
std::filesystem::path executable_directory();

// The runtime config directory. `OGRE_PREF_DIR` still wins (test harnesses use
// it); otherwise a distributable build keeps everything in its own folder — the
// executable's directory — so the save file lands right beside the app. If that
// directory is not writable (a read-only install location), the platform
// preference directory is used instead.
std::filesystem::path resolve_pref_dir();

// A ROM sitting next to the executable (or in `<exe dir>/roms/`), or an empty
// path. `ogre64.z64` is preferred; otherwise the first plausible N64 dump is
// taken. This is what makes "put the ROM in the folder and launch" work without
// a click.
std::filesystem::path find_exe_rom(const std::filesystem::path& exe_dir);

// --- the launcher screen ------------------------------------------------------

struct LauncherContext {
    // Where the save and config files will go; shown at the bottom of the
    // screen so it is never a mystery.
    std::filesystem::path pref_dir;
    // Message to show when the launcher opens (the previous failure, if any).
    std::string initial_error;

    // A ROM that was already found and validated before the screen opened (the
    // stored copy, or one sitting next to the executable). When set, the screen
    // opens ready to play and returns this path on Enter or a click; when empty,
    // the player must click, drop or pick a ROM first.
    std::filesystem::path ready_rom;

    // The id mods target. The mod panel lists the mods opened for this id, and
    // writes each toggle to `mods.json` through the runtime's mod system.
    std::string mod_game_id;

    // Validates `path` and stores it as the ROM to boot. Returns an empty
    // string on success, or a human-readable explanation to show on screen
    // (the launcher then keeps running).
    std::function<std::string(const std::filesystem::path& path)> accept_rom;
};

// Runs the launcher until a ROM is accepted or the window is closed. Returns
// the accepted ROM path, or an empty path if the user quit.
std::filesystem::path run_launcher(const LauncherContext& context);

}  // namespace ogre
