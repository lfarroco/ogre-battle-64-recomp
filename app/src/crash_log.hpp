#pragma once

#include <filesystem>

// Player-facing crash report (see crash_log.cpp). Installed at the top of
// main(); writes `error.log` beside the executable when the process dies on a
// fatal signal or an unhandled exception.
namespace ogre {
namespace crash_log {

// Start capturing stdout/stderr into an in-memory ring, install the fatal
// handlers, and use `dir` for error.log until set_directory replaces it.
void install(const std::filesystem::path& dir);

// Point error.log at `dir`. main() calls this with the resolved config
// directory, which is the executable's own directory unless that one is
// read-only.
void set_directory(const std::filesystem::path& dir);

// Write error.log for a fatal condition that does not raise a signal: the
// runtime's error message box. `reason` is the message shown to the player.
void write_error_log(const char* reason);

// Restore stdout/stderr and join the capture threads. Registered with atexit
// by install(); a bounded run's _Exit path must call it explicitly.
void flush();

}  // namespace crash_log
}  // namespace ogre
