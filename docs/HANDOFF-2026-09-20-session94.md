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

The generated game code, `config*.toml`/`config-bank*.yaml` and RT64 were not
touched, so `make recomp`/`make bank-recomp`/`rsp-recomp` were not needed. The
runtime is touched later in this session (part 2), and
`n64modernruntime-ob64.patch` was regenerated then.

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

---

# Part 2: the window-close freeze

**Goal (developer):** *"one issue, that predates your changes is: when I load the
rom, if I close the window by clicking on its 'x', the window freezes. I need to
force quit it"*.

**Result:** the close button now exits the process in about half a second. The
teardown no longer unmaps RDRAM under the game's still-running threads
(`n64modernruntime-ob64.patch`), the app leaves through `_Exit` after the SDL
teardown instead of running the C++ static destructors with those threads live,
and the crash handler can no longer deadlock on a stdio stream lock.

## 8. Reproduction

The close button makes SDL2 post `SDL_QUIT`: `-[Cocoa_WindowListener
windowShouldClose:]` sends `SDL_WINDOWEVENT_CLOSE`, and
`SDL_SendWindowEvent` (`src/events/SDL_windowevents.c`) posts `SDL_QUIT` when
that window is the last one. The app already handles `SDL_QUIT`
(`pump_sdl_events`), so a probe that pushes the same event reproduces the freeze
without a mouse. `probe94` did that from `pump_sdl_events`
(`OGRE_PROBE94_QUIT_MS=<n>`); the freeze reproduces every time, and it is the only
probe this session used (reverted; `grep -r probe94 app/ RecompiledFuncs/
Bank*Funcs/ RspFuncs/` is empty).

`sample <pid>` on the frozen process, plus `lsof -p <pid>`, gave the whole
picture:

* the main thread was in `exit` → `__cxa_finalize_ranges` →
  `ogre::crash_log::flush` → `std::thread::join` → `__ulock_wait`;
* an N64 thread (`N64 Thread 1`) was in `run_thread_function` → `__sflush` →
  `_sigtramp` → `fatal_signal_handler` → `write_report`, i.e. the signal handler
  was itself stuck inside a stdio flush;
* `lsof` showed the two capture pipes' **write ends still open** (fds 12 and 13)
  while fd 1/2 had been restored, which is why `flush`'s join could never finish.

And `error.log` (written by that handler before it hung) recorded
`reason: SIGSEGV (11)`, `rdram base: 0x12025B000`, `fault offset from rdram:
0xE7A18` — a fault **inside the 8 MiB image**.

## 9. Root cause, in two layers

**The teardown unmaps RDRAM while the game's threads run.**
`recomp::start` ends with `join_event_threads`, `join_thread_cleaner_thread`,
`join_saving_thread`, then `munmap(rdram)`. Those joins cover the *runtime's*
threads; the game's N64 threads are host threads that nothing joins, and
`ultramodern::quit()` only sets `exited` (which stops the runtime's service
loops). So the guest threads were still executing recompiled code when the image
was unmapped, and the next guest access faulted. The fault address being inside
the image is the proof.

**The crash handler deadlocked instead of dying.** The fault arrived on a thread
that was inside `__sflush`, i.e. it held a stdio stream lock. The handler's
`std::fflush(nullptr)` re-acquires every stream lock, so it hung on the lock the
faulting thread already owned; the handler never returned, never re-raised, and
the process never died — the window froze and needed a force quit. The handler
had also duplicated the capture pipes' write ends (its `io_dup(1)`/`io_dup(2)`),
so the main thread's `crash_log::flush()` join could not complete either. This
deadlock is the reason the pre-existing bug showed as a freeze and not as a
crash; the old `bank_overlays.cpp` handler had the same `fflush` pattern.

## 10. The fix

* **`librecomp/src/recomp.cpp`** — free RDRAM only when the run did not exit
  through `ultramodern::quit()`. The process is about to end, so the OS reclaims
  the mapping. Landed through `n64modernruntime-ob64.patch`, regenerated with
  `git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`,
  reverse-checked against the working tree and forward-checked against a pristine
  `589bbf0` worktree.
* **`app/src/main.cpp`** — after `recomp::start` returns, run
  `overlay_shutdown()`/`shutdown_sdl()`, then `ogre::crash_log::flush()` and
  `_Exit(EXIT_SUCCESS)`. `return` would run the C++ static destructors with the
  game's threads still live, which is the same race from the other side; `_Exit`
  also skips the atexit flush, hence the explicit call. The battery is not at
  risk: `join_saving_thread()` is inside `recomp::start` and still runs.
* **`app/src/crash_log.cpp`** — the signal path never calls `fflush` (that was
  the deadlock), and the `printf`-based runtime dumps run only after a
  non-blocking `ftrylockfile` probe shows both stream locks free; the report says
  when they were skipped. The header and the captured ring tail are written with
  raw `write()` and are unaffected.

## 11. What was run for verification

* **Synthetic close (`OGRE_PROBE94_QUIT_MS=5000`, `SDL_QUIT`):** before the fix,
  still alive after 30 s and `sample` showed the stack above; after the fix,
  `exit=0` in 5.49 s wall (probe at 5.00 s, so teardown ≈ 0.49 s) and **no
  `error.log`**.
* **Real close button:** `osascript -e 'tell application "System Events" to tell
  process "ogrebattle64" to click button 1 of window 1'` on the packaged bundle,
  after the game had loaded — the click was accepted and the process exited 0.
  (`System Events` intermittently reports zero windows for this process, so the
  later repeats of the click test could not run; the probe and the first click
  cover the same `SDL_QUIT` path.)
* **Real Quit event:** `open "dist/.../Ogre Battle 64.app"` then
  `osascript -e 'tell application "Ogre Battle 64" to quit'` — exited cleanly.
* **Crash reports still work:** `OGRE_CRASH_TEST=segv` before boot writes
  `error.log` with the header and the captured log; a live `kill -SEGV` of a
  booted process writes the `--- guest diagnostics ---` section with the
  per-thread data (not the "skipped" line), so the lock probe does not disable
  it in the normal case.
* **Regression:** the maintained 45 s title-route run (`OGRE_SPEED=4
  OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1
  OGRE_EXIT_AFTER_MS=45000`) exits 0 and `tools/runlog.py --check` prints PASS.
  `build-app`, `build-dist` (the package) and `build-null` all link.
* `n64modernruntime-ob64.patch` is the only change under `tools/`: reverse-apply
  against the working runtime tree succeeds, `git apply --check` against a
  pristine `589bbf0` worktree succeeds, and it carries no `N64Recomp` hunk.

## 12. Files changed in part 2

* `tools/N64ModernRuntime/librecomp/src/recomp.cpp` and
  `n64modernruntime-ob64.patch` — the RDRAM free is skipped on a user quit.
* `app/src/main.cpp` — `_Exit(EXIT_SUCCESS)` after the teardown.
* `app/src/crash_log.cpp` — no `fflush` in the signal path, and the
  `ftrylockfile` probe around the stdio-based dumps.
* `app/src/sdl_platform.cpp` — `probe94` added and reverted, no net change.
* `docs/guides/app-build.md` — "Closing the window" and the handler note.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

## 13. Open leads

1. **The game's threads are still never joined.** The fix stops the teardown from
   tearing memory out from under them, but they remain alive until `_Exit`. A
   clean solution would ask the runtime to terminate them (or wait for them), and
   would make the `_Exit` unnecessary.
2. **A fault inside stdio on Windows** has no `ftrylockfile` guard (the MSVC CRT
   has no non-blocking stream-lock probe), so the handler there keeps the old
   risk. The `SetUnhandledExceptionFilter` path is what would hit it.
3. **`graphics_shutdown_ready` and `join_event_threads` are still run** before the
   `_Exit`, so the renderer does shut down; only the process teardown is
   immediate. If a future change makes that slow, the window would linger after
   the click.


---

# Part 3: the Windows package does nothing

**Goal (developer):** *"nothing happens in the windows version. nothing opens, no
logs, nothing. can you fix, commit, push and create a rc release?"*, then *"can we
have a small smoke test in the os image that builds it?"*.

**Result:** the Windows package no longer depends on the Visual C++
redistributable, every failure before the first window writes `error.log` and
shows a dialog instead of exiting silently, and the release workflow now
smoke-tests each package on the runner image that built it. The workflow change
is what would have caught this class before the release.

## 14. What the shipped v0.2.0 Windows build actually was

The report was "no window, no log, nothing", so the first question was whether
the process ran at all. The `.exe` in the release zip is sound:

* `Subsystem 2 (Windows GUI)`, `AddressOfEntryPoint 0x128969c`, and its imported
  CRT entry markers (`_get_narrow_winmain_command_line`, `_configure_narrow_argv`,
  `_set_app_type`) are the `WinMainCRTStartup` set, so the entry is
  `WinMainCRTStartup` → the `WinMain` forwarder → `main`. The 0.1.0 build was
  `Subsystem 3 (Windows CUI)` with the same CRT imports.
* The zip's CRCs are intact and the PE headers are sane.

So the process start path was correct, and the failure was inside the app, before
the first window (section 15).

## 15. The cause: `setvbuf(stdout, nullptr, _IOLBF, 0)` is an invalid parameter on the MSVC CRT

`crash_log::install()` (part 1) called `std::setvbuf(stdout, nullptr, _IOLBF, 0)`
to keep `stdout` line-buffered after fd 1 became a pipe. glibc and macOS accept a
null buffer with size 0; **the MSVC CRT does not** — it reports an invalid
parameter, and the invalid-parameter handler calls `_invoke_watson`, which
fast-fails the process. The call is the top of `main`, before the fatal handlers
are installed, so every Windows launch died immediately with `0xC0000409` and no
`error.log`. That is the whole "nothing happens in the windows version" report.

The stack, from `cdb.exe` on the runner (the SDK debugger is installed there),
against the **released v0.2.0 `.exe`**, which reproduces it exactly:

```
ucrtbase!invoke_watson+0x18
ucrtbase!_invalid_parameter_internal+0x3829c
ucrtbase!_setvbuf_internal+0xaf
ucrtbase!setvbuf+0x2d
ogrebattle64+0x10e40          <- crash_log::install's setvbuf call
ogrebattle64+0x10a4           <- WinMain
ogrebattle64+0x128962e        <- WinMainCRTStartup
```

Windows Error Reporting agrees: `Faulting module name: ucrtbase.dll`, `Exception
code: 0xc0000409`.

The isolation, before cdb, ruled out everything else: a trivial GUI-subsystem
exe builds and runs from Git Bash on the runner (`gui exit=9`); the
`AttachConsole` + `freopen_s("CONOUT$")` block runs (`exit=0`); `notepad.exe`
launches; every system DLL the exe imports is present; and the exe's own imports
are all either shipped or system DLLs.

**Fix (`app/src/crash_log.cpp`).** A static buffer with a real size, and the
handlers armed first:

```cpp
static char stdout_buffer[4096];
std::setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
```

`install_handlers()` now runs before the capture setup, so a later fault in the
capture itself still reaches `write_report`.

## 16. A second, real defect: the package did not carry its runtime DLLs

Found by the new static smoke check, and independent of the cause above.
`ogrebattle64.exe` statically imports `dxcompiler.dll` and `dxil.dll` (RT64's
shader compiler; the package bundles both), and **both of those Microsoft
binaries import `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`**,
which the package did not ship. A machine without the Visual C++ 2015-2022
redistributable cannot start the process at all; the CI runner has the
redistributable, so a launch there could not have shown it. The same import sets
are in the 0.1.0 package, so the Windows build never was self-contained.

Landed:

* `app/CMakeLists.txt` sets `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` for every
  target on Windows (the app, N64ModernRuntime, RT64), so the app and the
  recompiled code have no CRT DLL dependency of their own.
* The same file adds a POST_BUILD copy of `msvcp140.dll`,
  `msvcp140_atomic_wait.dll`, `vcruntime140.dll` and `vcruntime140_1.dll` from
  `VCToolsRedistDir`/`VCINSTALLDIR` next to the `.exe`.
* The `Makefile` builds SDL2 with the same runtime-library setting (a `-MD`
  SDL2 cannot link into a `-MT` exe: `LNK2038 RuntimeLibrary mismatch`), with a
  stamp file so a cached Windows SDL2 is rebuilt, and the `dist` recipe copies
  the four DLLs into the package and **fails** when one is missing.

## 17. Silent boot failures

Even with the DLLs, the build had no way to report why it did not start: the
GUI-subsystem image has no console, and four paths ended the process with no file
and no dialog — `init_sdl()` failure, the game `create_window` failure, and the
start screen's window and renderer failures. `report_boot_failure()`
(`app/src/sdl_platform.cpp`) now writes `error.log` (with the captured boot log)
and shows the reason in a message box; all four call it.

`init_sdl()` also stopped requiring audio. `SDL_Init` fails the whole call when
any requested subsystem fails, and `SDL_INIT_AUDIO` fails with "No available
audio device" on a host with no usable output device, which stopped the app
before its first window. Video and events are required; audio and game
controllers are initialised separately and may be absent. The audio callbacks
already handle a null device, so the game runs silently.

## 18. The smoke test the workflow was missing

* `app/src/main.cpp` handles `OGRE_SMOKE=1`: print `[smoke] main reached`, try
  SDL once (reported but not required, because a hosted runner has no display or
  GPU), flush and exit 0. It runs after `crash_log::install`.
* `tools/pe_imports.py` parses the import directory of every shipped
  `.exe`/`.dll` and requires each imported DLL to be shipped beside them or to be
  a Windows system DLL (`api-ms-win-*` counts as system). On the v0.2.0 package it
  reports exactly `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`
  and `vcruntime140_1.dll`, with no OS-DLL false positives; on a package with
  those files present it passes.
* `tools/smoke-dist.sh [dist-dir]` checks the companion files, runs the PE check
  for a Windows package (or `otool -L`/`ldd` for macOS/Linux), then runs the
  binary with `OGRE_SMOKE=1` under a deadline so a loader dialog or a hang cannot
  stall a release. The launch check is skipped when the package is for another
  platform. `make smoke` runs it against the current `dist/`.
* `.github/workflows/release.yml` runs `tools/smoke-dist.sh` on every matrix
  runner between "Build and package" and "Collect the archive".

The runtime half of the smoke test cannot catch the missing-redistributable case
on a CI image, because the image has the redistributable installed; the static
PE check is what covers the package itself, and it is platform-independent.

## 19. What was run for verification

The Windows diagnosis ran on the `windows-2022` runner through a temporary
`diag.yml` workflow (removed; `git ls-files .github/workflows` lists only
`release.yml`), and the fix was verified locally on macOS plus by the rc2
release run.

**On the runner, against the released v0.2.0 `.exe`:**

* `powershell Start-Process` → `exited code=-1073740791 hex=0xC0000409`;
  `Get-WinEvent` → `Faulting module name: ucrtbase.dll`, `Exception code:
  0xc0000409`.
* `cdb.exe -o -g -G -c "g; .ecxr; kb; q"` → the stack in section 15, which lands
  in `setvbuf` and then `crash_log::install`.
* The plain run and `OGRE_CRASH_TEST=segv` both exit 127 with **no `error.log`**,
  so the process dies before the handlers are armed.
* Ruled out first: a trivial x64 GUI exe builds and runs from Git Bash
  (`gui exit=9`); `AttachConsole` + `freopen_s` alone exits 0; `notepad.exe`
  launches; every system DLL the exe imports is present in `System32`
  (`d3dcompiler_47.dll`, `d3d12.dll`, `dxgi.dll`, `winmm.dll`, `imm32.dll`,
  `setupapi.dll`, `version.dll`, `ucrtbase.dll`); and the exe's own imports are
  all shipped or system DLLs.

**Locally on macOS (the fix and the tooling):**

* `tools/smoke-dist.sh` on the packaged macOS app: PASS (`otool` clean, the
  binary loaded, reached main, exited 0). `make smoke` is the same path.
* `tools/smoke-dist.sh /tmp/winrc/ogre-battle-64-recomp` (the v0.2.0 Windows zip
  from the release): FAIL at `msvcp140.dll is missing from the package`, before
  any launch attempt; with the four DLLs added to a copy of that package the
  static checks pass. `tools/pe_imports.py` reports exactly those four names and
  no OS-DLL false positives.
* `OGRE_SMOKE=1` on `build-app/ogrebattle64`: exit 0, `[smoke] main reached`,
  `[smoke] sdl ok`.
* `SDL_AUDIODRIVER=doesnotexist` on the macOS build: boots, logs
  `[SDL] audio unavailable, running silently`, reaches the window and exits 0.
* `SDL_VIDEODRIVER=doesnotexist`: writes `error.log` with
  `reason: SDL could not start: doesnotexist not available` and the captured
  `[boot] init_sdl...` line.
* Regression: the maintained 45 s route run is `runlog.py --check` PASS, and the
  window-close path still exits cleanly (part 2).
* `make -n dist DIST_OS=windows` shows the DLL copy loop and the SDL2 stamp
  guard. **The `setvbuf` fix is only observable on Windows**, so `v0.2.1-rc2`'s
  Windows job — which now runs the smoke test after packaging — is its
  verification.

## 20. Files changed in part 3

* `app/src/crash_log.cpp` — the `setvbuf` fix (static line-buffer, real size) and
  `install_handlers()` before the capture setup.
* `app/src/main.cpp` — `OGRE_SMOKE=1`; `report_boot_failure` on the SDL-init and
  game-window failures.
* `app/src/sdl_platform.cpp`, `app/src/sdl_platform.hpp` — video/events required,
  audio and controllers optional; `report_boot_failure`.
* `app/src/launcher.cpp` — `report_boot_failure` on the start screen's window and
  renderer failures.
* `app/CMakeLists.txt` — static CRT for all Windows targets; POST_BUILD copy of
  the app-local MSVC runtime DLLs.
* `Makefile` — SDL2 built with the static CRT (plus a stamp guard); the `dist`
  recipe copies the runtime DLLs and fails when they are missing; `make smoke`.
* `.github/workflows/release.yml` — the smoke step; `-rc` tags become GitHub
  pre-releases.
* `tools/pe_imports.py`, `tools/smoke-dist.sh` — new.
* `packaging/README-dist.txt`, `docs/guides/app-build.md` — the Windows runtime
  note and the smoke-test section and knob.
* `PLAN.md`, `docs/DECISIONS.md`, this file.

## 21. Open leads

1. **The smoke test caught this bug, and that is the point.** It runs the built
   binary on the runner image; the `setvbuf` fast-fail fails it. A packaging
   defect that a runner cannot reproduce (a missing redistributable DLL) is
   covered by the static `tools/pe_imports.py` check instead.
2. **`report_boot_failure` shows a modal box**, so a scripted run that hits an
   early failure now waits for a click. Only the boot-failure paths do this, and
   they are fatal; a harness with a bad ROM argument takes the older
   `fprintf`+exit path instead.
3. **The app-local runtime is copied from the build machine's Visual Studio.**
   That is Microsoft's supported local deployment, but it means the Windows
   release runner must have the redist directory (it does, via `msvc-dev-cmd`);
   `make dist` now fails rather than shipping without it.
4. **`tools/pe_imports.py`'s system-DLL allowlist is a list.** A new dependency
   on a DLL that is neither shipped nor listed fails the smoke test with the
   name, which is the intended behaviour, but the list has to be extended for a
   legitimate new system dependency.
5. **A `setvbuf` on a stream that has already been used is undefined** and the
   MSVC CRT may still reject it; `install()` runs before the first write, and
   after `AttachConsole`/`freopen_s` only re-open the stream, so this call is
   valid. If that order ever changes, the smoke test fails on Windows.


---

# Part 4: a Windows access violation on game start

**Goal (developer):** the rc2 Windows package now runs, but loading the ROM and
starting the game dies with `unhandled exception 0xC0000005`, and restarting with
the stored ROM dies the same way (deleting the ROM lets the launcher run). The
`error.log` for both has **empty captured sections** and an empty guest
diagnostics section. The developer also asked for a Windows-only build ("the
macos build takes 30min") and suggested checking how Zelda64Recomp handles
Windows.

**Result:** the AV itself is **not fixed yet** — this part is the diagnostics and
the iteration speed needed to find it. Windows reports now carry the faulting
instruction and the address it touched with their modules and offsets, a
`0xC0000005` says whether the access was a read, a write or an execute, the
handler no longer uses stdio on Windows (so it cannot die on a stream lock and
truncate the report), `OGRE_CONSOLE=1` gives a double-clicked build a console,
the smoke test now fails a package whose crash report is empty, and a dispatched
release can build Windows alone in ~8 minutes.

## 22. What the report showed, and what it did not

Both reports carried a header (`reason: unhandled exception 0xC0000005`, a
non-null `rdram base`) and then stopped: no fault address, no host pc, no
captured lines, and an empty `--- guest diagnostics ---`.

Two conclusions. (1) The process got past RDRAM allocation, so the crash is in
the game boot, not in startup. (2) The report cannot be trusted to be complete:
the Windows handler called the runtime's `printf`-based dumpers with no
`ftrylockfile` guard, so a fault arriving while the faulting thread holds a stdio
stream lock hangs the handler and truncates the file — the same class as the
macOS window-close freeze of part 2. (The missing fault lines are from an earlier
filter shape; the fields are unconditionally ordered before the dumps now.)

## 23. Diagnostics landed

* `app/src/crash_log.cpp`: the report prints `fault instruction` and `fault
  address` separately, each with a host module and offset (`write_host_symbol`
  uses `GetModuleHandleExW`/`GetModuleFileNameW` on Windows, `dladdr` on macOS),
  the `access` kind from `ExceptionInformation[0]`, and the RDRAM-relative offset
  when the address is inside the image. `write_guest_diagnostics` writes the N64
  thread and the last recompiled function per thread with raw writes on every
  platform, and only the POSIX path adds the `ftrylockfile`-guarded call-chain
  dump. **Windows never touches stdio in the handler.**
* `app/src/main.cpp`: `OGRE_CONSOLE=1` calls `AllocConsole` and points
  stdio at it, so a double-clicked GUI build shows the `[boot]` log live
  (Zelda64Recomp's Windows build has the same switch as `--show-console`).
  `OGRE_CRASH_TEST` flushes and waits 50 ms before raising, so the smoke test's
  report check is not a race.
* `tools/smoke-dist.sh` gained check 4: `OGRE_CRASH_TEST=segv` must leave an
  `error.log` that contains the captured `[boot]` line. A report with empty
  sections now fails the package.
* `.github/workflows/release.yml`: a `plan` job builds the matrix from data, and
  `workflow_dispatch` has a `platforms` input (`all`/`windows`/`linux`/`macos`).
  A tag push still builds all three; `-f platforms=windows` is ~8 minutes.
* `app/src/sdl_platform.cpp`: on Windows, `SDL_AUDIODRIVER` is pinned to
  `wasapi` unless the player set it, the workaround Zelda64Recomp carries for
  the same runtime ("some issue with sample queueing with directsound").
* `app/CMakeLists.txt`: `/OPT:NOICF` for a link driven by `cl.exe`, because
  identical code folding can merge two recompiled functions and the runtime
  patches a function's own code for a mod hook (Zelda64Recomp disables it for the
  same reason). The CI's clang-cl + lld-link pair does not fold by default, so
  nothing changes there.

## 24. What Zelda64Recomp does on Windows, and what was not taken

Checked `Zelda64Recomp/Zelda64Recomp` (`dev`) `CMakeLists.txt` and
`src/main/main.cpp`:

* It keeps `main` and sets `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` for a
  cl.exe link; this port cannot use `/ENTRY` because its Windows build is
  clang-cl (clang-cl has no such driver option, session 94 part 1), so it keeps
  the `WinMain` forwarder.
* It leaves the CRT dynamic and copies `SDL2.dll` and the two DXC DLLs beside the
  exe, plus **`/OPT:NOICF`**. This port links SDL2 and the CRT statically and
  copies the DXC DLLs plus the app-local MSVC runtime DLLs (part 3), and now
  sets NOICF for a cl.exe link.
* `preload_executable` maps the exe and `VirtualLock`s it, so a runtime code
  patch cannot touch a page the loader has dropped. **Not taken**: this port
  patches function code only for a mod hook, and the crash here happens with no
  mods loaded. It is the next thing to try if a fault lands in the exe at a
  patched entry.
* `timeBeginPeriod(1)`, `SDL_HINT_WINDOWS_DPI_AWARENESS`, `SetConsoleOutputCP`,
  an `.rc` icon and an `NFD_Init()` call: not taken (timing/cosmetic).
* Its audio workaround (WASAPI) **was** taken.

## 25. Verification so far

macOS only, plus the Windows job of the rc3 dispatch.

* `OGRE_CRASH_TEST=segv` on `build-app/ogrebattle64`: the report now has
  `fault instruction: 0x... (/usr/lib/system/libsystem_kernel.dylib+0x7846)`, the
  captured `[boot] OGRE_CRASH_TEST=segv` line, and a guest-diagnostics section.
* `tools/smoke-dist.sh`: PASS, including the new crash-report check (check 4) on
  the packaged macOS app.
* The maintained 45 s route run is `runlog.py --check` PASS.
* The Windows job of `v0.2.1-rc3` is the check for the Windows handler path: its
  smoke run executes check 4 on Windows and fails the package if the capture or
  the handler is wrong there.

## 26. Files changed in part 4

* `app/src/crash_log.cpp`, `app/src/main.cpp`, `app/src/sdl_platform.cpp`,
  `app/CMakeLists.txt`, `tools/smoke-dist.sh`,
  `.github/workflows/release.yml`, `docs/guides/app-build.md`, `PLAN.md`,
  `docs/DECISIONS.md`, this file.

## 27. Open leads

1. **The AV is unfixed.** The next report (or `OGRE_CONSOLE=1` output) should
   name the faulting module: `ogrebattle64.exe` means recompiled code or the
   runtime, a `*wgfx*`/`amdxx*`/`nvwgf2umx` DLL means the GPU driver, `ucrtbase`
   means a CRT call with a bad argument. `rdram base` was non-null in both
   reports, so the boot had begun.
2. **The empty captured sections were never explained.** If rc3's Windows smoke
   check 4 passes, the capture works and the user's empty report was a truncated
   handler; if it fails, the Windows capture is broken and that is the first
   thing to fix.
3. **The stored-ROM restart crash** is the same boot path; it should be re-tested
   with the new report.
