# Handoff — 2026-09-20, session 94: no console window, and `error.log` on a crash

**Goal (developer):** *"when we launch this game dist, a terminal window show up
(with the logs) and the actual game window shows up later. instead, let's remove
that first terminal window, only the actual game window should show up. also, if
the game crashes (due to finding some uncompiled code stub or a seg fault), it
should create an "error.log" file in the executable dir. this will allow players
to submit issues on github with those logs"*. Asked which dist, the developer
chose **macOS and Windows**; asked what the file should hold, they chose **crash
details plus the last ~200 log lines**.

**Result:** the macOS package ships `Ogre Battle 64.app`, so Finder launches it
without Terminal, and Windows links the `.exe` as a GUI-subsystem binary, so no
console is allocated. `app/src/crash_log.cpp` replaces the lost console: it
captures stdout/stderr in two rings, keeps forwarding them, and writes
`error.log` beside the save file on a fatal signal or an unhandled exception,
with the fault details and the tail of the app's own log.

---

## 1. Where the console came from

* **macOS.** The package carried a bare `ogrebattle64`. Finder runs a bare Unix
  executable through Terminal, so double-clicking it opened a Terminal window
  and the process's stderr (`[boot] ...`) appeared there; the SDL window came
  later. Nothing in the app creates it.
* **Windows.** `add_executable` with no `WIN32_EXECUTABLE` links a
  console-subsystem image. Launching the `.exe` from Explorer allocates a
  console for the process, which shows the same `[boot]` lines from
  `fprintf(stderr, ...)`.

## 2. macOS: the bundle (`Makefile`, `packaging/macos-Info.plist`)

`make dist` on macOS now writes:

```
dist/ogre-battle-64-recomp/
  Ogre Battle 64.app/Contents/Info.plist
  Ogre Battle 64.app/Contents/MacOS/ogrebattle64
  Ogre Battle 64.app/Contents/_CodeSignature/CodeResources
  README.txt, SDL2-LICENSE.txt, mods/, mod_config/, saves/ ...
```

The Makefile computes `DIST_APP`/`DIST_BIN_DIR` once from `DIST_OS` and uses
`DIST_BIN_DIR` for the executable copy and for `otool`/`install_name_tool`/
`codesign`, so the shared-SDL2 fallback (`DIST_STATIC_SDL=0`) puts its dylib
inside `Contents/MacOS` where `@executable_path` resolves it. The bundle is
ad-hoc signed last (`codesign --force --sign - "$(DIST_APP)"`), after any nested
dylib.

**The load-bearing plist key is `SDL_FILESYSTEM_BASE_DIR_TYPE=parent`.** SDL2's
`Cocoa_GetBasePath` (`src/filesystem/cocoa/SDL_sysfilesystem.m`) reads it:

```objc
} else if (SDL_strcasecmp(baseType, "parent") == 0) {
    base = [[[bundle bundlePath] stringByDeletingLastPathComponent] fileSystemRepresentation];
```

Without it the default is `resource`, i.e. `Contents/Resources`, and the config
directory would move inside the bundle. With `parent`, `SDL_GetBasePath` — and
therefore `ogre::executable_directory()`, `resolve_pref_dir()`, `find_exe_rom()`
and `error.log` — resolves to the folder that holds the `.app`, which is the
folder `packaging/README-dist.txt` describes. Verified by running the bundle's
own binary and reading its boot line:

```
[boot] config path ok: /Users/momo/dev/ogre/dist/ogre-battle-64-recomp/
```

## 3. Windows: the subsystem (`app/CMakeLists.txt`, `app/src/main.cpp`)

```cmake
if (WIN32)
    set_target_properties(ogrebattle64 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
```

`WIN32_EXECUTABLE` links `/SUBSYSTEM:WINDOWS`. That subsystem's CRT entry point
is `WinMainCRTStartup`, which calls `WinMain`; the app defines `main`, so
`main.cpp` adds the forwarder:

```cpp
#if defined(_WIN32) && !defined(__MINGW32__)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
```

MinGW's CRT already forwards `WinMain` to `main` for `-mwindows` (which
`WIN32_EXECUTABLE` adds), and a second definition would be a duplicate symbol,
hence the `!defined(__MINGW32__)` guard.

A GUI-subsystem process does not inherit a parent terminal, so `main()` first
calls `AttachConsole(ATTACH_PARENT_PROCESS)` (also Windows/MSVC-only) and points
`stdout`/`stderr` at `CONOUT$` when it succeeds. Explorer has no console, so a
double-click attaches to nothing and no window appears; a developer running the
`.exe` from a terminal sees the `[boot]` log exactly as before. This runs before
`crash_log::install()`, so the capture forwards to the console like any other
terminal.

**`/ENTRY:mainCRTStartup` was tried first and does not work with this project's
Windows toolchain.** The Windows job builds with clang-cl (session 89e).
clang-cl has no `/ENTRY` driver option: LLVM's cl-compatible option table has
`_SLASH_link` (`/link`, which consumes the remaining arguments) but no
`_SLASH_ENTRY`, and an unrecognized `/x` is passed to the compiler as an input
file. Reproduced with the local clang in cl mode:

```
$ clang --driver-mode=cl --target=x86_64-pc-windows-msvc -### /ENTRY:mainCRTStartup e.c
clang: error: no such file or directory: '/ENTRY:mainCRTStartup'
```

The `WinMain` forwarder needs no linker option and works for MSVC and clang-cl
alike. **Windows is code-verified only in this session** (no MSVC or mingw-w64 on
this machine); the release workflow is the first real compile.

## 4. `error.log` (`app/src/crash_log.{hpp,cpp}`, new)

`crash_log::install(ogre::executable_directory())` is the first statement of
`main()`, before `init_sdl()`. `set_directory(pref_dir)` re-points the file once
`resolve_pref_dir()` is known; the two differ only when the executable's own
directory is read-only.

**Capture.** `redirect_start()` replaces fd 1 and fd 2 with the write ends of two
pipes (separate rings, separate threads: a single pipe cannot forward each byte
to the descriptor it came from, and `> run.log 2>&1` versus `2> run.log` must
both keep working). Each `capture_loop` appends to a 128 KiB ring, publishing the
length with release ordering, then forwards the bytes to the saved descriptor.
`stdout` is set back to `_IOLBF` because it became a pipe and would otherwise be
block-buffered, which would have delayed the interactive `printf` stream.

**Handlers.** POSIX: `sigaction` with `SA_SIGINFO|SA_RESETHAND` for `SIGSEGV`,
`SIGBUS`, `SIGABRT`, `SIGFPE`, `SIGILL`. Windows: `signal` for the same names
minus `SIGBUS`, plus `SetUnhandledExceptionFilter` for access violations. Both
install `std::set_terminate`. The old POSIX-only `on_fatal_signal` in
`bank_overlays.cpp` was deleted, so two handlers cannot fight over one signal;
its diagnostics moved into `write_report`.

**The report.** Header: reason, UTC time (integer civil-from-days, safe in a
handler), OS/arch, pid, path. Then the signal, `rdram base`, the host fault
address and its offset from RDRAM when it is inside it, and the host pc (with a
`dladdr` module + offset on macOS). Then the last 200 stderr lines and the last
100 stdout lines, which carry the boot log and any `[overlays] streamed function
stub called @ 0x...` or `[bank] UNCOMPILED streamed record ...` line — the
"uncompiled code stub" half of the developer's request. Then the guest
diagnostics: `n64 thread`, `debug_dump_call_chain`, the per-thread last function,
and `debug_dump_chain_history`, with fd 1 and fd 2 redirected into the file
because those helpers are `printf`-based (`ultramodern/src/function_trace.cpp`).
`OGRE_DUMP_RDRAM=<path>` still adds the 8 MiB image on this path.

The handler's own output is `open`/`write`/`close` and a fixed char buffer with
local hex/decimal formatters; `g_reported` (`compare_exchange`) makes a
`std::terminate` that then aborts write one report, not two. The runtime's
`message_box` channel (`show_message_box` in `sdl_platform.cpp`) calls
`write_error_log(msg)`, which does **not** consume that guard, so a later real
crash still writes.

**Drain.** `crash_log::flush()` restores the descriptors, joins both capture
threads and closes the forwarded fds. `std::atexit(flush)` covers a normal
`return EXIT_SUCCESS`; the bounded-run path in `sdl_platform.cpp` calls it
explicitly before `_Exit(EXIT_SUCCESS)`, which skips atexit. Without that call
the last `[SDL]` lines of a scripted run would be lost to the pipe.

## 5. What was run for verification

All on macOS (x86-64), with `build-app` and with the package built by `make dist`.

* `make dist` → `Ogre Battle 64.app` with `Contents/Info.plist` and an ad-hoc
  signature (`codesign -dv` prints `Identifier=io.github.lfarroco.ogrebattle64`,
  `Signature=adhoc`), and `otool -L` on the bundle binary prints no non-system
  dependency.
* **No Terminal window.** `osascript -e 'tell application "Terminal" to count
  windows'` was 1 before `open "dist/.../Ogre Battle 64.app"` and 1 after, with
  the process running (`pgrep` shows `Ogre Battle 64.app/Contents/MacOS/ogrebattle64`).
* **The bundle's config directory is the package folder**, from the bundle
  binary's own `[boot] config path ok: /Users/momo/dev/ogre/dist/ogre-battle-64-recomp/`.
* **Synthetic crash:** `OGRE_CRASH_TEST=segv ./Ogre Battle 64.app/Contents/MacOS/ogrebattle64`
  exits 139 and writes `dist/ogre-battle-64-recomp/error.log` with
  `reason: SIGSEGV (11)`, `rdram base: 0x0` (before boot), the
  `[boot] OGRE_CRASH_TEST=segv` line and the host pc. Repeated from
  `build-app/ogrebattle64` with the same result.
* **Live crash:** the bundle binary booted with a ROM, then `kill -SEGV <pid>`
  after 5 s. `error.log` holds the whole boot log — `[boot] init_sdl` through
  `[renderer] RT64 renderer initialized (api=3)` on stderr, `[bank] 44
  streamed-overlay record(s), 3512 function(s) armed` and `[pi] PI manager
  initialized` on stdout — plus `rdram base: 0x11E52D000`. This is the file a
  player would attach to an issue.
* **A normal close writes nothing:** `kill -TERM` of the running app left no
  `error.log` (`SIGTERM` is not in the handler set).
* **Stream separation and the drain:** `OGRE_EXIT_AFTER_MS=4000 ... > out.log 2>
  err.log` exits 0 with `[bank]` (printf) in `out.log` and `[boot]`/`[SDL]` in
  `err.log`, and the last `[SDL] per-thread live call chain:` line is present, so
  `flush()` before `_Exit` drained the pipe.
* `tools/runlog.py`-style consumers are unaffected by the tee: the app's output
  bytes and order are unchanged for a terminal, a merged redirect and a split
  redirect.

## 6. Files changed

* `app/src/crash_log.hpp`, `app/src/crash_log.cpp` — new: the capture, the fatal
  handlers, the report writer, `write_error_log`, `flush`.
* `app/src/main.cpp` — `crash_log::install()` first, `set_directory(pref_dir)`
  after `resolve_pref_dir()`, the `OGRE_CRASH_TEST` knob, and the `WinMain`
  forwarder plus its `windows.h` include on Windows.
* `app/src/sdl_platform.cpp` — `show_message_box` writes `error.log`;
  `crash_log::flush()` before the bounded-run `_Exit`.
* `app/src/bank_overlays.cpp` — the POSIX `on_fatal_signal` handler and its
  includes removed (the diagnostics are in `crash_log.cpp` now);
  `register_bank_overlays()` keeps only the DMA hook.
* `app/CMakeLists.txt` — `src/crash_log.cpp` in the native sources;
  `WIN32_EXECUTABLE TRUE` on Windows.
* `packaging/macos-Info.plist` — new: the bundle's `Info.plist`, including
  `SDL_FILESYSTEM_BASE_DIR_TYPE=parent`.
* `Makefile` — `DIST_APP`/`DIST_BIN_DIR`, the bundle copy, the plist, the ad-hoc
  signature, and the executable paths in the dylib/`otool`/`ldd` blocks.
* `packaging/README-dist.txt` — the per-platform launch step, and a section on
  `error.log` and how to submit it.
* `docs/guides/app-build.md` — the distribution section, "Crash reports —
  `error.log`", the config-directory note, and `OGRE_DUMP_RDRAM`/`OGRE_CRASH_TEST`
  in the knob table.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

The generated game code, `config*.toml`/`config-bank*.yaml`, the runtime and
RT64 were not touched, so `make recomp`/`make bank-recomp`/`rsp-recomp` were not
needed and `n64modernruntime-ob64.patch` is unchanged.

## 7. Open leads

1. **The Windows build is compiled for the first time in CI.** The
   `WIN32_EXECUTABLE` + `WinMain` change is the part to watch: a release run must
   link `ogrebattle64.exe` and launch it with no console window. A `WinMain`
   conflict would appear as `LNK2005`/`duplicate symbol`; a subsystem mismatch as
   an immediate exit with no window.
2. **`error.log` is overwritten by the next crash.** A player who crashes twice
   and submits the second file loses the first. Appending with a size cap would
   preserve both; it was not asked for.
3. **A crash before `set_directory(pref_dir)`** writes the file into the
   executable's directory even when that directory is read-only, where the write
   fails and no file appears. The window is the first few milliseconds of boot.
4. **The runtime's hard lookup failure (`Failed to find function at 0x...` in
   `librecomp/src/overlays.cpp`) still `std::exit`s in a release build**, where
   `assert(false)` is compiled out, so it writes no `error.log`. The common
   "uncompiled stub" path is the streamed range, which returns the no-op stub and
   logs; if the process then faults, the report contains those lines. Routing
   that terminal call through `message_box` would need a runtime patch.
