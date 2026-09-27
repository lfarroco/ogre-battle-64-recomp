# Session 110 handoff — the overlay's window no longer breaks the close button, and MAIN carries EXIT GAME

## Goal

Two developer requests:

1. **The game window's close button stops working after the Esc overlay has been
   opened and closed once.** On macOS (the other platforms were untested), a
   fresh run closes on the button, but `Esc` open + `Esc` close then makes the
   button do nothing.
2. **Rename the overlay's START GAME tab to MAIN and add an EXIT GAME option to
   it.**

## Result

Both are in. The close button was never the overlay's drawing or its input
grab: it was SDL's own `SDL_QUIT` rule.

1. **The close button.** SDL synthesises `SDL_QUIT` from a window's
   `SDL_WINDOWEVENT_CLOSE` only while that window is the **last one in SDL's
   global window list** (`tools/SDL2-static/SDL2-2.32.10/src/events/SDL_windowevents.c:221-227`);
   otherwise the close event is delivered and nothing quits. The overlay is a
   second SDL window that is hidden and shown again rather than destroyed
   (`app/src/overlay.cpp`), so the first time it is created the game window stops
   being last and the quit stops. Before the overlay exists the game window is
   the only one, which is why a fresh run closes normally.
   `pump_sdl_events` (`app/src/sdl_platform.cpp`) now quits on a
   `SDL_WINDOWEVENT_CLOSE` whose `windowID` is the game window's, and keeps
   `SDL_QUIT` as well. A close request for the overlay window is ignored: `Esc`
   is that window's dismiss.
2. **MAIN + EXIT GAME.** `ui::Tab::Start` is `Tab::Main`, `tab_label` returns
   `MAIN`, and `Panel::tab_enabled` enables every tab in overlay mode except
   MODS (the runtime loads mods before it boots). `RowKind::Start` is
   `RowKind::StartGame` and a new `RowKind::ExitGame` is the MAIN tab's first
   selectable row in both modes, so the overlay's panel opens with Exit Game
   selected. `RowAction::QuitGame` replaces the unused `RowAction::CloseOverlay`;
   the launcher stops its loop on it, and the overlay records a request that
   `update_gfx` polls next to the window-close flag, so both exits share main.cpp's
   dumps and `ultramodern::quit`.

## What the exit work cost, and the runtime defect under it

Routing the overlay's exit through `ultramodern::quit` hung the process, and the
cause is a runtime defect that the overlay was the first thing to reach.

`recomp::start` runs its frame loop until `exited` is set, then joins the game
thread (`librecomp/src/recomp.cpp`, `while (!exited)` … `game_thread.join()`).
The game thread spends the first seconds of a run inside
`ultramodern::preinit` → `init_events`, which starts the RSP gfx/task threads and
waits on their `thread_ready` semaphores, and the RT64 renderer context is built
on the gfx thread at the same time. Setting `exited` in that window produced:

* **a hang**, with `recomp::start` blocked in `game_thread.join()` forever, when
  `EXIT GAME` was the first frame's input. Measured with temporary `[probe151]`
  and `[probeRT]` prints: the main loop left, `graphics_shutdown_ready` was
  signalled, and the join never returned. The game thread was still in `preinit`;
  the gfx thread had not printed RT64's `Device Name` line.
* **an uncaught C++ exception, `reason: std::terminate`,** once RT64 finished
  initialising and the game thread had reached its SRAM/PI setup. Every early
  window-close and `EXIT GAME` run in that window crashed with `error.log` until
  the gate below went in.

Landing both, `app/src/main.cpp` now reads the game's own frame counter
(`D_800AEFA4`, written by `func_80072398` on the frame-pump thread; `0` at 6 s of
a boot and `0x327` at 12 s in a measured run) through
`ogre::overlay_frames_produced()`. An interactive exit taken before
`kFirstFrameCount` (60) frames leaves the process with `_Exit(EXIT_SUCCESS)`
instead of running the runtime's teardown, after the same dumps every other exit
path prints. That is the rule `pump_sdl_events` already uses for
`OGRE_EXIT_AFTER_MS`, which leaves through `_Exit` for the same reason. Both the
window close and EXIT GAME take it.

## Evidence

* SDL's quit rule, in the pinned source this build links:
  `tools/SDL2-static/SDL2-2.32.10/src/events/SDL_windowevents.c:221`.
* The game window's close event, in the same tree:
  `src/video/cocoa/SDL_cocoawindow.m:735` (`windowShouldClose:` posts
  `SDL_WINDOWEVENT_CLOSE` and returns `NO`).
* The frame counter: `[console] 800AEFA4 = 00000000` at ~6 s and `00000327` at
  ~12 s of a plain boot (`r 0x800AEFA4 1` through the watched console file).
* The hang: `[probe151] quit: returned from ultramodern::quit`, then
  `[probeRT] signaling done, joining game_thread`, then nothing, with
  `[boot] start_game...` printed between them by the game-starter thread.
* `ultramodern::is_game_started()` returns true as soon as `start_game` sets
  `GameStatus::Running` (`librecomp/src/recomp.cpp:591-598`), i.e. before preinit
  finishes, so it is not a usable readiness test. That is why the gate is the
  frame counter.

## Verification

macOS, `build-dist/ogrebattle64` (the `make app` output). Every row below exits 0
with no `error.log`:

| run | result |
|---|---|
| `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=main OGRE_OVERLAY_KEYS=Space` | `[overlay] EXIT GAME`, then `exiting before the game produced frames` (the early path) |
| `… OGRE_OVERLAY_KEYS="Escape,Escape,Space"` | `[overlay] EXIT GAME` with no early line, i.e. the runtime's own teardown |
| `OGRE_OVERLAY=1 OGRE_OVERLAY_KEYS=Escape OGRE_WINDOW_CLOSE_AT_MS=9000` | `[SDL] game window close requested`, then exit |
| `OGRE_OVERLAY=1 OGRE_OVERLAY_KEYS=Escape OGRE_WINDOW_CLOSE_AT_MS=20000` | `[SDL] game window close requested` with no early line |
| `OGRE_WINDOW_CLOSE_AT_MS=4000` (no overlay) | `[SDL] game window close requested`, then the early path |
| `OGRE_EXIT_AFTER_MS=6000` | the bounded-run path, unchanged |
| `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=main OGRE_OVERLAY_KEYS="Down,Space"` | runs to `OGRE_EXIT_AFTER_MS`; Down moves to No ROM and never quits |
| `OGRE_DUMP_RDRAM` on the early path | writes the full 8388608 bytes |
| all five launcher tabs (`OGRE_LAUNCHER_TAB`) and all four overlay tabs (`OGRE_OVERLAY_TAB`) | capture and exit 0 |
| launcher `MAIN` with **Down, Space** | quits without booting (`closed without a ROM`); **Space** on Start Game still boots |
| `OGRE_SMOKE=1` | `[smoke] main reached`, `sdl ok` |

`OGRE_WINDOW_CLOSE_AT_MS=<n>` is added for this test: it pushes one
`SDL_WINDOWEVENT_CLOSE` for the game window, because a real close button cannot
be synthesised and the bug needs the overlay window to have existed first
(`OGRE_OVERLAY_AT_MS` + `OGRE_OVERLAY_KEYS=Escape`).

Proofs refreshed (macOS, from the same build):
`docs/proofs/native-launcher-main.png` (new; replaces `native-launcher-start.png`
— the tab is MAIN now), `-launcher-controls.png`, `-launcher-mods.png`,
`-launcher-settings.png`, `-overlay-main.png` (new), `-overlay-controls.png`,
`-overlay-settings.png`, `-widescreen-launcher.png`, `-widescreen-overlay.png`.
The overlay MAIN capture shows `[ MAIN ] [ MODS ] …`, `[ ] Start Game  ALREADY
RUNNING` (inert and skipped by the selection), `[ ] Exit Game` selected with
`Quit the game and close the window`, and `[ ] No ROM`.

## Files changed

| file | change |
|---|---|
| `app/src/sdl_platform.cpp` | quit on the game window's own `SDL_WINDOWEVENT_CLOSE`; `OGRE_WINDOW_CLOSE_AT_MS` test hook |
| `app/src/sdl_platform.hpp` | no change (the hook is in the .cpp) |
| `app/src/main.cpp` | poll `overlay_quit_requested`; `_Exit` before the runtime teardown when the game has produced fewer than 60 frames |
| `app/src/overlay.{hpp,cpp}` | `overlay_quit_requested`, `overlay_frames_produced`; `QuitGame` handling on the key and mouse paths; `OGRE_OVERLAY_TAB=main` (and `start` as its alias) |
| `app/src/ui.{hpp,cpp}` | `Tab::Main`, `MAIN` label, MODS the only disabled overlay tab, `RowKind::StartGame`/`RowKind::ExitGame`, `RowAction::QuitGame` (replaces the unused `CloseOverlay`) |
| `app/src/launcher.cpp` | `QuitGame` stops the launcher loop; tab comments |
| `docs/guides/app-build.md` | the tab list and key map (which still described the retired ROM tab), the overlay section's close-button rule, `OGRE_OVERLAY_TAB=main`, `OGRE_WINDOW_CLOSE_AT_MS`, two new verification recipes |
| `docs/proofs/native-launcher-*.png`, `native-overlay-*.png`, `native-widescreen-*.png` | refreshed; `native-launcher-start.png` deleted |
| `PLAN.md`, `docs/STATUS-LOG.md` | status entries |

No probe survives. The only instrumented files were
`tools/N64ModernRuntime/librecomp/src/recomp.cpp` (temporary `[probeRT]` prints
around the exit path) and `app/src/main.cpp` (`[probe151]`); both are reverted
and `grep -rn 'probe151\|probeRT' app/ tools/N64ModernRuntime/librecomp/src/` is
empty. `tools/N64ModernRuntime` is a submodule and carries the port's own
pre-existing modifications, which are untouched.

`make dist` was run, so `dist/ogre-battle-64-recomp/Ogre Battle 64.app` carries
the new binary.

## Open

* The unsaved-profile interaction is unaddressed: neither exit path asks about a
  save in progress, and the game's own "save before quitting" prompt is not
  reachable from the panel.
* The early-exit `_Exit` path skips the runtime's teardown on purpose, so it also
  skips anything a game-side shutdown would do. For a run ended in its first
  second that is the whole point, but a game that wanted an orderly stop from
  MAIN would not get one.
* The MAIN tab's ROM row in the overlay reads `[ ] No ROM`: the overlay passes an
  empty ROM path to the panel. It is inert there, and the wrapped
  `PRESS SPACE TO CHOOSE A ROM…` description overlaps the row below it in the
  overlay's narrower window. That overlap is the existing fixed-position layout
  (the CONTROLS tab sets the block height), not a change this session made.
* A mouse click on Exit Game is wired the same way as the keyboard path but was
  not exercised, because no synthetic-mouse hook exists.
* Windows and Linux were not run. The close-button fix is in SDL's portable
  window-event path and should apply, but the early-exit crash and the hang were
  only observed on macOS (Metal).
* The pre-existing runtime defect is not fixed, only avoided: `ultramodern::quit`
  before the game thread has left `preinit` should not hang
  `game_thread.join()`, and an exit while RT64 is still building its renderer
  should not throw. Fixing that belongs in `N64ModernRuntime`, which is vendored.
