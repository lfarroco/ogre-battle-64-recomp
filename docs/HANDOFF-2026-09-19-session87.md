# Handoff — 2026-09-19, session 87: code distribution (the start screen + app-local saves)

**Goal (developer):** *"code distribution! the project is quite mature, and it
should be good enough for us to have a build process that generates an
executable. we need to make it user friendly, so these are our mvp requirements:
upon opening, a black window appears. in it, it is written "Ogre Battle 64:
Recomp" / "Click to load your ROM (or drop it in this window)". When running the
project like this, the program should create a save file in the same directory
that is is being executed on."*

**Result: landed.** `make dist` produces `dist/ogre-battle-64-recomp/` — a
self-contained **single executable** (SDL2 linked statically, §5) plus its
README and SDL2's license — that a player can run with their own ROM. On first
launch it shows the black start screen; the ROM can be supplied by **click**
(native file picker), **drag-and-drop**, or by leaving it beside the executable.
The battery save is written to `saves/ogrebattle64-us-rev1.bin` **in the
executable's own directory**.

Two product decisions were confirmed with the developer before building:

- the save goes **directly beside the executable**, under `saves/` (the
  runtime's own layout: `<config>/saves/<game id>.bin`);
- once a ROM has been loaded, the **next launch skips the prompt** and boots the
  stored ROM (the runtime already stores a hash-checked copy).

## 1. What the start screen is, and why it is not the runtime's UI

`app/src/launcher.cpp` + `app/src/font.cpp`, ~600 lines total, no new
dependency. It runs **before** the runtime exists (no RDRAM, no RT64), so it is
a plain SDL2 window (`SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI`) drawn by
`SDL_Renderer`, and destroyed before the game window is created (the game needs
its own `SDL_WINDOW_METAL`/`SDL_WINDOW_VULKAN` surface).

The text is drawn from a **built-in 5x7 ASCII bitmap font** (`font.cpp`): one
glyph table, rasterised into a one-channel texture atlas at startup, then drawn
per text block into a small supersampled (`SDL_TEXTUREACCESS_TARGET`) texture
and scaled down by the GPU with linear filtering. That is what gives clean
anti-aliased text at any size without a TTF/fontconfig dependency, which matters
because this screen must work on a fresh machine with nothing installed.

The screen content is exactly the MVP spec:

```
OGRE BATTLE 64: RECOMP
───────────────────────
CLICK TO LOAD YOUR ROM (OR DROP IT IN THIS WINDOW)

OR PLACE THE ROM IN THIS FOLDER AND LAUNCH AGAIN

SAVED TO: <config dir>
```

plus, after a rejected file, `THAT IS NOT A USABLE ROM` / the runtime's own
reason / `CLICK OR DROP A ROM TO TRY AGAIN`. The window title is
`Ogre Battle 64: Recomp`.

## 2. ROM resolution order (and why the second launch is quiet)

`app/src/main.cpp` resolves the ROM before it decides whether to show the
screen:

1. `argv[1]` (unchanged: every scripted harness and `tools/run-save.sh` passes
   the ROM this way, and an invalid argument still exits non-zero),
2. `OGRE_ROM=<path>`,
3. a ROM sitting **beside the executable** (`find_exe_rom`: `ogre64.z64`,
   `ogre64.n64`, `ogrebattle64.z64`, the full retail filename, then any
   `.z64`/`.n64`/`.v64` in the folder or in `roms/`),
4. the copy the runtime **stored** on a previous run
   (`<config>/<game id>.z64`, `entry.stored_filename()`),
5. `assets/ogre64.z64` (development convenience, unchanged),
6. otherwise: **the start screen**.

Step 4 is what makes the second launch boot straight into the game. It is safe
to boot a stored ROM without checking it, because `recomp::select_rom` re-reads
and re-hashes it: a stored ROM that no longer validates is deleted by the
runtime and the app falls through to the start screen showing the reason.

`OGRE_LAUNCHER=1` forces the screen even when a ROM is available (mainly for
testing the first-launch experience).

## 3. The save location (the load-bearing change)

`main.cpp` used to call `SDL_GetPrefPath` and hand that to
`recomp::register_config_path`. It now calls **`ogre::resolve_pref_dir()`**
(`launcher.cpp`):

- `OGRE_PREF_DIR` still wins (every scripted run and `run-save.sh` sets it, and
  the directory is created by the runtime — verified);
- otherwise `SDL_GetBasePath()`, i.e. the **executable's own directory**, after
  a write probe (`create_directories` + write/remove `.ogre-write-probe`);
- if the probe fails (a read-only install such as a `.app` in `/Applications`),
  fall back to `SDL_GetPrefPath`.

Consequence: the runtime writes the battery to
`<exe dir>/saves/ogrebattle64-us-rev1.bin` and mods to `<exe dir>/mods/`, so the
package carries its saves. The start screen prints the resolved directory at the
bottom so it is never a mystery.

## 4. The file picker, and what the null build does

Clicking calls RT64's vendored **nativefiledialog-extended** (`NFD_OpenDialogN`,
filters `*.z64;*.n64;*.v64`), guarded by `OGRE_HAVE_NFD`, which
`app/CMakeLists.txt` defines **only when `OGRE_USE_RT64` is on**. The
null-renderer variant therefore keeps drag-and-drop and `OGRE_ROM` and reports
the missing browser on screen instead of growing a GTK dependency — the null
build stays GPU-free and dependency-free.

One C++20 trap worth recording: `nfdu8char_t` is `char`, so the filter
descriptions must be plain literals (`"N64 ROM …"`), not `u8"…"` (`char8_t`),
which does not convert.

## 5. `make dist` / `make dist-zip`: one self-contained executable

`Makefile` gains `sdl2-static`, `app`, `dist` and `dist-zip`. **SDL2 is linked
statically, so the package is a single file** and `otool -L` shows only system
frameworks:

```sh
otool -L dist/ogre-battle-64-recomp/ogrebattle64 | grep -v "System/Library\|/usr/lib"   # nothing
```

A static link cannot use Homebrew's SDL2 here, and the reason is worth
recording: **on macOS the `sdl2` formula is `sdl2-compat`** — a shim that
`dlopen`s `libSDL3.0.dylib` at runtime (the crash report from §7 shows
`libSDL3.0.dylib` on the stack under `libSDL2-2.0.0.dylib`) — and it ships no
static library at all. So `make dist` first runs `make sdl2-static`, which
fetches a **pinned real SDL2 (2.32.10)** into the gitignored
`tools/SDL2-static/` and builds it with `-DSDL_SHARED=OFF -DSDL_STATIC=ON`
(≈1 min, once), then builds the app in its own `build-dist/` against that prefix
(`-DSDL2_DIR=…`); the static target supplies the extra frameworks SDL2 needs
(Cocoa, IOKit, CoreAudio, AudioToolbox, AVFoundation, Carbon, CoreVideo,
Foundation, plus weakly GameController/Metal/QuartzCore/CoreHaptics). SDL2's
zlib license is copied into the package as `SDL2-LICENSE.txt`, which the license
requires. `DIST_STATIC_SDL=0 make dist` skips all of it and bundles the shared
dylib as before (the `install_name_tool` path is retained for that mode).

The app's own CMake needs no change for this: `find_package(SDL2)` honours
`SDL2_DIR`, and its imported static target brings the framework list.

## 6. Verification

| check | how | result |
|---|---|---|
| start screen renders as specified | ran the null and RT64 builds with no ROM in an isolated directory; captured **their own window** (`CGWindowListCreateImage` via a throwaway dlsym helper — `screencapture` grabbed a different app's window) | black window, `OGRE BATTLE 64: RECOMP`, the two prompt lines, `SAVED TO: <dir>` |
| drop handler → ROM → game | `OGRE_TEST_DROP=<rom>` on a fresh folder, null build | `[launcher] test drop` → `[boot] rom ok` → `create_window` → game runs → `[save] wrote …/saves/ogrebattle64-us-rev1.bin` |
| same on RT64 | `OGRE_TEST_DROP` + `OGRE_EXIT_AFTER_MS=12000`, isolated dir | `[renderer] RT64 renderer initialized (api=3)`, display lists 1..N, save written beside the executable |
| second launch skips the screen | re-ran the RT64 isolated dir with no ROM argument | `[boot] selecting rom /private/tmp/ogre-dist-rt64/ogrebattle64-us-rev1.z64` → `rom ok` → boot (no `[launcher]`) |
| `OGRE_PREF_DIR` regression | `OGRE_PREF_DIR=/tmp/ogre-fresh-pref` | directory created, save written into it (harnesses unaffected) |
| the package is self-contained | `make dist`, then ran `dist/ogre-battle-64-recomp/ogrebattle64` from that directory | launcher appears; config dir is the package folder |
| **static SDL2 needs no runtime library** | built a pinned SDL2 2.32.10 (`SDL_SHARED=OFF`), built a trivial SDL program against it, then relinked the app's own objects + RT64 against `libSDL2.a` | `otool -L`: **no non-system entry at all** (system frameworks only); the app-sized binary is 28.7 MB vs 26.9 MB for the dylib link |
| the static binary runs the whole flow | copied the static-link executable into an empty directory, `OGRE_TEST_DROP=<rom> OGRE_EXIT_AFTER_MS=9000` | start screen → ROM accepted → `RT64 renderer initialized (api=3)` → `[save] wrote …/saves/ogrebattle64-us-rev1.bin` |
| both builds compile | `cmake --build build-app` and `build-null` | clean (only the pre-existing `p[2 ^ 3]` xor warning in `renderer.cpp`) |

**Not verified by a human:** the native file picker itself. Synthesizing a click
needs macOS accessibility permission (`System Events` returns `privilege
violation (-10004)`), so the `NFD_OpenDialogN` path is code-verified only. The
drag-and-drop, auto-scan, stored-ROM and error paths are all exercised.

**Not verifiable on this machine:** a *from-scratch* `make dist` run. RT64's
Metal shader step needs the Xcode Metal toolchain, and this environment now
answers `xcrun -sdk macosx metal --version` with *"cannot execute tool 'metal'
due to missing Metal Toolchain; use: xcodebuild -downloadComponent
MetalToolchain"* — the existing `build-app` still works only because its
generated `.ir`/`.metal` artifacts are cached (and CMake insists on regenerating
them in any *new* build directory). The static-link half was therefore verified
by relinking the already-compiled app/RT64 objects against `libSDL2.a` (the two
rows above), and the `make dist`/`sdl2-static` targets were parsed and dry-run
(`make -n dist`). On a machine with the Metal toolchain — or with
`build-dist/` seeded from an existing `build-app/` — the target runs as written.
Install it with `xcodebuild -downloadComponent MetalToolchain`.

## 7. A development hook, and a bug it exposed

`OGRE_TEST_DROP=<path>` feeds one synthetic drop to the start screen. It must
call the **same handler** the `SDL_DROPFILE` branch calls
(`handle_dropped`), not `SDL_PushEvent`: pushing a fabricated `SDL_DROPFILE`
through sdl2-compat crashes in `Event3to2` → `SDL_strdup_REAL` on a null string
(confirmed from the crash report's `_platform_strlen` frame). The hook exists
because SDL cannot synthesize a real Finder drag.

## 8. Addendum — cross-platform builds (same session, second half)

The developer asked for Windows and Linux builds on the GitHub Releases page.
The first thing that mattered was a **blocker, not a build script**: the
recompiled code `make dist` links (`RecompiledFuncs/`, 34 `Bank*Funcs/`,
`RspFuncs/`, `app/src/bank_funcs.inc` — 62 MB) is **generated and gitignored,
and has never been tracked** (`git log -- RecompiledFuncs` is empty). A fresh
clone cannot build the app at all, and a GitHub-hosted runner has no ROM, so it
cannot generate the code either. Asked whether to commit it, the developer's
answer was the right question — *"are we adding copyrighted code into the
recomp client then?"* — and the honest answer is that the generated C is a
mechanical translation of the ROM's instructions, so **if a recompiled binary
is a derivative work, so is the generated C**; this repo's stated policy is no
distributed extracted assets, so it was **not** committed. (Zelda64Recomp takes
the same "user generates it" path: `BUILDING.md` has the user run N64Recomp
themselves. Some other recomp projects do commit the output; that is practice,
not a clearance.) Consequence: **hosted CI cannot build these releases**, so the
workflow runs on a self-hosted runner or a machine with the ROM, and there is a
one-line change to switch it if that decision ever changes.

### Real portability bugs found by actually building for Linux

The Linux build was attempted end to end in a container
(`ubuntu:24.04` + `libsdl2-dev`/`libgtk-3-dev`); **it did not compile at all**
before these fixes, each macOS-only in effect:

1. **SSE4.1 was never enabled.** `librecomp/rsp_vu.hpp` selects its SIMD path on
   `#if defined(__x86_64__)` and then uses `_mm_shuffle_epi8` / `_mm_blendv_epi8`
   with no runtime guard, but no target passed `-march`/`-m` flags; the x86-64
   baseline is SSE2. GCC fails hard (`inlining failed in call to
   'always_inline' '_mm_shuffle_epi8': target specific option mismatch`). Apple
   clang defaults to a newer `-march` that happens to include them, which is why
   the port built here for 60 sessions and nowhere else. Fixed in
   `app/CMakeLists.txt`: `-msse4.1` on `ogrebattle64_rsp` and `librecomp` on
   x86-64 (the header gates on `ARCHITECTURE_SUPPORTS_SSE4_1`, and SSE4.1
   implies SSSE3; aarch64 needs nothing because it goes through sse2neon).
   **First attempt applied to nothing** — `ogrebattle64_rsp` is not defined yet
   at the top-level point where `librecomp` is, so the flag now goes where each
   target is created.
2. **`sdl_platform.cpp` would not compile off Apple**: `const ssize_t n` for
   `readlink` at function scope collided with `size_t n` in the copy loop below
   (the Apple branch declares no `n`, so only one branch is ever compiled).
   Fixed by scoping the loop variable in the `while` condition.
3. **`gbi.cpp` used a designated-initializer array** (`[OP_X] = true, …`) whose
   designators are not in declaration order; GCC rejects that ("non-trivial
   designated initializers not supported"). Replaced with a switch — same
   information, both compilers.
4. **Linux always asked SDL for `SDL_WINDOW_VULKAN`**, so the *null-renderer*
   build — the variant whose entire purpose is a machine with no GPU driver —
   failed to create its window (`Vulkan support is either not configured in SDL
   or not available in current SDL video driver`). The flag is now behind
   `OGRE_USE_RT64` (new compile definition) together with the macOS
   `SDL_WINDOW_METAL` flag.

Verified in the container: the **null build runs the whole flow** — launcher,
`OGRE_TEST_DROP` ROM, `null renderer initialized`, game boots, reads and writes
the battery, `saves/ogrebattle64-us-rev1.bin` created **beside the executable**.
The RT64/Vulkan path also *initializes* on Linux (`RT64 renderer initialized
(api=2)` under Xvfb + Mesa `lavapipe`), then segfaults early in the first frame;
whether that is a real Linux bug or a software-Vulkan artifact is **unresolved**
(no Linux GPU here, and `lavapipe` is not a shipping configuration).

### Release infrastructure

- `.github/workflows/release.yml` — on `v*` tags (or manual dispatch) builds a
  matrix of macOS / Linux / Windows, collects the archive per platform, and
  creates or updates a **draft** release with `gh release create --draft`.
- `tools/release-build.sh` — the single build entry point CI and developers
  share; checks the generated code is present and explains what to run if not,
  then `make dist` + archive.
- `packaging/README-dist.txt` — the player-facing README that ships in every
  package (how to load the ROM, where saves go, requirements, legal note),
  replacing the inline `printf` the Makefile used to hold.
- `Makefile` — `dist` is now platform-generic (`DIST_OS=macos|linux|windows`
  overrides the host; `EXE_NAME` handles `.exe`), plus `dist-tar` alongside
  `dist-zip`.

**Windows is written but unbuilt here**: no MSVC, and `brew install mingw-w64`
failed on this machine (Homebrew directory ownership). The workflow's Windows
job is therefore the only untested path, and it needs a real Windows runner to
validate.

### Addendum 2 — can a contributor regenerate the code? Yes, and the recipe was wrong

The developer's follow-up question (*"any new contributor should be able to
regenerate that code, right? otherwise that will be an issue for future mod
contributors"*) was tested rather than asserted, by checking out the tracked
tree into a clean worktree (`git worktree add --detach`) with only the
third-party trees linked — i.e. exactly what a contributor clones — and running
the pipeline from nothing.

**It works, but the documented recipe did not.** `docs/guides/app-build.md` went
straight to `make recomp`, which (a) needs `build/ogrebattle64.elf`, produced by
`make resplit` + `make`, neither of which the recipe ran, and (b) runs the
cross-bank dispatch *before* `make bank-recomp` has written the bank records the
dispatch reads. On a fresh tree the dispatch therefore printed

    cross_bank: rewrite: no bank entries registered yet (`make bank-recomp` not run);
    leaving --only target(s) undispatched

and left **15 call sites bound to the wrong bank** — silently, with a successful
build. That is precisely the failure class of sessions 41/45/55, and the fresh
tree's `RecompiledFuncs/` hashed differently from the working tree because of
it. Running `make recomp` a second time (after the banks exist) produced a tree
that is **byte-identical** to the one this project has been building and
testing.

Fixes landed:

- **`make regenerate`** — the whole pipeline in the one correct order
  (`resplit` → link the ELF → `bank-recomp` → `recomp` → `rsp-recomp`),
  verified end to end in the clean worktree: exit 0, and the resulting
  `RecompiledFuncs/` matches the working tree byte for byte.
- **`make recomp-prep`** (a prerequisite of `recomp`) refuses to recompile when
  `app/src/bank_funcs.inc` is absent, with a message naming the dispatch
  consequence and `make regenerate`. Verified to fail on a pristine checkout
  and pass on a regenerated one.
- The guide's setup section now leads with regeneration and explains the
  ordering constraint and the two cosmetic traps below.

Two findings a contributor will hit, both now documented:

- **`make resplit` leaves tracked files dirty.** The committed `asm/1CE040.s`
  and `asm/40E80.s` are the *post-`fix-labels`* form; a re-split writes raw
  splat output and the phony `fix-labels` prerequisite re-applies the patch on
  every build, so `git status` always shows them modified. `git status --short`
  is clean only if you accept that two files are permanently dirty (AGENTS §10
  says it must show only intended changes — this is the exception, and it is
  the patch round-trip, not a content difference).
- **Regenerated bank units differ cosmetically.** 26 files under
  `Bank{B..L}Funcs/` differ from this tree only in `recomp_trace_return(rdram,
  N)` depth constants (e.g. 33 vs 17) — the recompiler's trace numbering moved
  at some point and this tree's bank output predates it. Filtering those lines
  out leaves a **zero-line** diff across all 26 files, and the values only feed
  the shadow call-chain diagnostic, so the regenerated code is functionally
  identical. The main unit (`RecompiledFuncs/`) matches exactly.

## 9. Files changed

- **new** `app/src/launcher.hpp`, `app/src/launcher.cpp` — the start screen,
  `resolve_pref_dir`, `executable_directory`, `find_exe_rom`
- **new** `app/src/font.hpp`, `app/src/font.cpp` — the built-in 5x7 font
- `app/src/main.cpp` — config dir via `resolve_pref_dir`; ROM resolution order;
  the start-screen path and the shared `accept_rom` validator; window creation
  moved after ROM selection; `rom_error_text`
- `app/src/sdl_platform.cpp` — Linux/Windows portability: the window's
  Metal/Vulkan flag follows `OGRE_USE_RT64`, and the `readlink` fingerprint loop
  no longer redeclares `n`
- `app/src/gbi.cpp` — the known-opcode table is a switch (GCC rejects the
  out-of-order designated-initializer array)
- `app/CMakeLists.txt` — add the launcher/font sources; `OGRE_HAVE_NFD=1` +
  link `nfd`, and `OGRE_USE_RT64=1`, when RT64 is on; `-msse4.1` for the RSP
  recompilation and librecomp on x86-64
- `Makefile` — `app`, `dist`, `dist-zip`, `dist-tar`, **`sdl2-static`** targets;
  `dist` is platform-generic (`DIST_OS`, `EXE_NAME`), builds `build-dist/`
  against the static SDL2 prefix and ships `SDL2-LICENSE.txt`; **`regenerate`**
  (the full pipeline in dependency order) and **`recomp-prep`** (the
  bank-data guard on `recomp`)
- **new** `tools/release-build.sh` — the build entry point CI and developers
  share, with the generated-code precondition check
- **new** `.github/workflows/release.yml` — tag-driven release matrix
- **new** `packaging/README-dist.txt` — the player-facing README that ships in
  every package (previously inline `printf` in the Makefile)
- `.gitignore` — `dist/`, `build-dist/`, `tools/SDL2-static/`
- `docs/guides/app-build.md` — Running/distribution/releases sections
  (including the static-SDL2 flow and why CI is self-hosted), config-directory
  section, save path, `OGRE_ROM`/`OGRE_LAUNCHER`/`OGRE_TEST_DROP`/`OGRE_PREF_DIR`
- `README.md` — build-and-run + playable build
- `PLAN.md`, `docs/DECISIONS.md` — status + decision

No probes were added to generated code, and no submodule or generated file was
touched (`git status --short` shows only the files above plus the pre-existing
`tools/RT64` submodule modification from earlier sessions).

## 10. Open follow-ups

- Human-check the file picker (click → picker → game) on a machine where the
  click can be made.
- Run a full from-scratch `make dist` on a machine with the Xcode Metal
  toolchain (see §6) — the only step not executed here.
- **Decide the generated-code policy.** Hosted CI is only possible if
  `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` and `app/src/bank_funcs.inc`
  are committed (~62 MB of generated C, no ROM data in it, but a derivative of
  the game's code). Until then releases build on a machine with the ROM. The
  workflow's `runs-on:` lines carry the hosted labels in comments.
- **Regenerate once and commit the result** if you want the tree to be
  self-consistent again: the 26 `Bank{B..L}Funcs/` files here carry stale
  `recomp_trace_return` constants relative to the current recompiler (cosmetic
  only — see §8 Addendum 2 — but it means a regenerated tree is not
  byte-identical to this one, which will confuse the next person who diffs a
  rebuild).
- **Windows is written but entirely unvalidated**: no MSVC and no mingw-w64 on
  this machine (`brew install mingw-w64` failed on Homebrew directory
  ownership). The Windows job needs a real Windows runner; expect to shake out
  a `SDL2.dll` bundling step and possibly the `EXE_NAME` handling.
- **Linux + RT64 is unresolved.** The renderer initializes (`api=2`, Vulkan)
  and then segfaults in the first frame under Xvfb + Mesa `lavapipe`. A real
  Linux GPU may well be fine (the crash was not diagnosed), but nobody has
  played it on real Linux hardware.
- A macOS `.app` bundle (with `Info.plist`) would be the next step for
  double-click distribution; `SDL_GetBasePath` inside a bundle points at
  `Contents/MacOS/`, so the current write probe would put saves inside the
  bundle — revisit the fallback before shipping a bundle.
- The start-screen font is ASCII-only; the screen's fixed strings are ASCII, so
  this only matters if a path with non-ASCII characters must be shown.
- Linux packages are not single-file the way macOS is: `make sdl2-static`
  already builds the real SDL2 from source, so `DIST_STATIC_SDL=1` should work
  there too, but it has only been exercised on macOS (the container test used
  the distro `libsdl2-dev`).
