# Handoff — 2026-10-13, session 137: 8x speed, the speed controls (keys and pad), and the speed badge

> **Summary.** Session 137 handoff — **GAME SPEED offers 1, 2, 4 and 8, `=`/`-` and the pad's R3 change it, and a two-second badge draws `2x`/`4x`/`8x` in the game window's lower-right corner.** Developer request: add the 8x speed (the row's max was 4x), a speed-up key and a speed-down key ("seed down" confirmed as *speed down*), show the multiplier in the window's lower-right corner when it changes, and then allow toggling the speed from a controller. Session 93 had dropped 6 and 8 because the game's cursor aiming is fast at those speeds; 8 is back and 6 stays out, and the row, the keys and the pad button share one table (`kGameSpeeds = {1,2,4,8}`). `step_game_speed(direction)` picks the next offered value numerically, so `OGRE_SPEED=6` steps to 8 up and 4 down, and it returns false at the ends instead of wrapping; `cycle_game_speed()` is the pad's one-button walk of the same table and wraps 8 → 1. The keys are `=` and keypad `+` up, `-` and keypad `-` down, handled from the SDL event in `pump_sdl_events`; the pad's **R3** (the right stick's click) cycles it from `SDL_CONTROLLERBUTTONDOWN`. R3 is what the default pad map leaves free (`Z` is L3, `BACK` opens the panel), and a player who binds R3 to a button still gets that button because the handler only observes the event. Both are ignored while the `ESC` panel is open, because the panel owns the pad and the keyboard and carries its own GAME SPEED row. The badge (`app/src/speed_badge.cpp`, new) samples `game_speed()` once per frame, creates its window on the first change, and draws `2x` through `8x` (and `1x`) for 2000 ms at the game window's lower-right corner, 20 px in, at 0.85 uniform window opacity with the panel's bitmap font, re-positioning every drawn frame — a second always-on-top SDL window because RT64 owns the game's swap chain and has no UI pass, the technique the Esc overlay uses. `SDL_ShowWindow` makes it the key window on macOS (`Cocoa_ShowWindow` = `makeKeyAndOrderFront`), so the tick re-raises the game window after each show and the key handler accepts the event from any window. Two limits the developer accepted: no badge in fullscreen, and the badge hides itself while the pointer is over it (SDL2 has no click-through hit-test result). Verified on `build-dist/ogrebattle64`: the scripted `up,up,up,up` logs `x2 x4 x8` with no fourth step, `r3,r3,r3,r3,r3` logs `x2 x4 x8 x1 x2` (the wrap), `OGRE_SPEED=6` with two R3 presses logs `x8` then `x1`, two R3 presses with the panel open change nothing, the badge sits at `(1087,681) 56x47` in a game window at `(203,28) 960x720` with the last PPM capture reading `8x`, `down` presses from a file-persisted 8 reach `4/2/1` and stop, the keys' run draws 1270 display lists in 14 s against 260 at 1x and 1630 at 8x, `=` pressed with the panel open changes nothing, `Right,Right,Right` on the row writes `game_speed = 8`, and a whole-screen capture shows the badge over the game's picture (`docs/proofs/native-speed-badge-8x.png`; the overlay and launcher SETTINGS proofs were refreshed — both still showed `[ ] 1 [ ] 2 [x] 4`).

Developer request, verbatim: *"let's add the following things to this game
recomp: add the 8x speed (current max is 4x); add a 'speed up' key; add a 'seed
down' key; when the speed changes, display '2x', '4x', etc in the lower right
corner of the window"*. Asked to confirm, the developer chose: "seed down" means
**speed down**; the keys are **`=`** (up) and **`-`** (down); 8x joins the
SETTINGS row so the row and the keys share one list; the keys **stop at the
ends** rather than wrapping; the badge is shown for **about two seconds after a
change** and then disappears; and a windowed-only badge is acceptable, with the
fullscreen limitation recorded. A follow-up request, *"allow toggling the speed
while using a controller"*, became the pad's **R3**, which cycles the list and
wraps; asked about it, the developer chose the cycle over a two-direction pad
input and chose to keep the shortcut inert while the Esc panel is open.

**Result.** GAME SPEED now offers **1, 2, 4 and 8** (8 is back; 6 stays out).
`=`/keypad `+` steps the multiplier up and `-`/keypad `-` steps it down through
that list while the game runs, stopping at 1 and 8; the pad's **R3** walks the
same list with one button and wraps, `1 → 2 → 4 → 8 → 1`. Any change to the
multiplier — the keys, R3, the SETTINGS row, or the launcher — puts `2x`, `4x` or
`8x` (and `1x`) in the game window's lower-right corner for two seconds. The badge
is a small borderless always-on-top SDL window, because RT64 owns the game's swap
chain and the renderer has no UI pass; the Esc overlay already uses that
technique.

No generated code, no recompiled function and no `tools/` submodule was touched.

## 1. The table, the step function and the keys

`app/src/settings.cpp`'s offered table is the row's radio group and the keys'
list, so one change covers both:

```cpp
const int kGameSpeeds[kGameSpeedCount] = {1, 2, 4, 8};
```

`step_game_speed(direction)` picks the next offered value **numerically**, not by
index. `OGRE_SPEED` can start a run on a value the row does not offer (3, 6, 64),
and stepping from one of those has to reach the neighbouring offered value, so
`OGRE_SPEED=6` goes to 8 pressing up and to 4 pressing down. It returns whether
the value changed; at 1x a down press and at 8x an up press return false and do
nothing. `set_game_speed` now also logs `[settings] game speed x<n>` on every
change, which is what makes a run's log say what the keys and the panel did.

The keys are handled in `app/src/sdl_platform.cpp`'s `pump_sdl_events`, from the
SDL event rather than from the keyboard state, because a step is an edge:

```cpp
if (event.type == SDL_KEYDOWN && !event.key.repeat && platform.window != nullptr &&
    !overlay_visible()) {
    handle_speed_key(event.key.keysym.sym);
}
```

Three deliberate properties:

* **The window id is not checked.** `SDL_ShowWindow` makes the badge's own window
  the key window on macOS (`Cocoa_ShowWindow` calls `makeKeyAndOrderFront`), so a
  handler that required the game window's id would drop the press after the first
  step. The tick re-raises the game window immediately after showing the badge,
  and the handler accepts the event either way.
* **The panel owns the keyboard while it is open**, so the keys are ignored then;
  the panel has its own GAME SPEED row, which steps the same table.
* **`platform.window != nullptr`** keeps the launcher phase out.

## 2. The pad's R3

The developer then asked to *"allow toggling the speed while using a controller"*.
Of the pad inputs the default map leaves free, **R3** (the right stick's click) is
the only usable one: `Z` is L3, `BACK` opens the Esc panel, and the OS consumes
`GUIDE` on most platforms. One button has no direction, so R3 **cycles** the
table and wraps — `cycle_game_speed()`, `1 → 2 → 4 → 8 → 1` — and a run started on
a value the table does not hold joins at the first offered value above it
(`OGRE_SPEED=6` → 8 → 1, `64` → 1).

It is handled from `SDL_CONTROLLERBUTTONDOWN` in the same event loop as the keys
(`handle_speed_pad`), not by polling the button state. Two reasons: a cycle is a
press and not a hold, and an event handler is what `OGRE_SPEED_PAD` can drive
from a script, so a run without a pad exercises the real path. The handler only
observes the event, so a player who binds R3 to an N64 button still gets that
button; it is ignored while the `ESC` panel is open, on the keys' rule.

## 3. The badge

`app/src/speed_badge.{hpp,cpp}` (new). One tick per frame from `update_gfx`,
beside `overlay_render`:

* `game_speed()` is sampled every frame. The first frame only records the value
  the run started on, so a boot draws no badge.
* On a change, a 56x47 window (text scale 3, 10 px padding) is created on first
  use, sized, positioned 20 px from the game window's lower-right corner, shown
  and drawn: a dark background at 0.85 uniform window opacity, a warm 1 px border
  and the white text. `SDL_RenderSetLogicalSize` keeps the text the same size on
  a high-DPI display, and the position is recomputed on every drawn frame, so the
  badge follows the game window while it is dragged or resized during its two
  seconds (the write happens only when the coordinates change).
* It hides itself 2000 ms later, and immediately if the pointer is over it (see
  §4).
* `[badge] <n>x shown for 2000ms at (x,y) WxH; game window (gx,gy) WxH` and
  `[badge] hidden` are the log lines a scripted run reads.

The window is a second SDL window over the game window because the game's picture
is drawn by RT64 straight to a Metal/Vulkan/D3D12 swap chain and the renderer has
no UI pass to draw into. SDL2 has no per-pixel window alpha, so the whole window
carries one uniform opacity.

## 4. What was run for verification

All runs are `build-dist/ogrebattle64 assets/ogre64.z64` with `OGRE_NO_AUDIO=1`
and `OGRE_SCENE=title`, and a scratch `OGRE_PREF_DIR` so the developer's
`settings.cfg` is not written.

| run | command | evidence |
|---|---|---|
| A, the keys up | `OGRE_SPEED=1 OGRE_SPEED_KEYS="up,up,up,up" OGRE_CAPTURE_BADGE=/tmp/s137-badge-8x.ppm OGRE_EXIT_AFTER_MS=14000` | `game speed x2/x4/x8` with `[SDL] speed key -> xN` after each, **no fourth change** (8 is the end); `[badge] 2x/4x/8x shown for 2000ms at (1087,681) 56x47; game window (203,28) 960x720` — `203+960-56-20 = 1087`, `28+720-47-20 = 681`; one PPM per show, the last reading `8x`; `[badge] hidden` 2 s after the last show |
| B, the keys down and the floor | same pref dir (its `settings.cfg` reads `game_speed = 8` from run A), no `OGRE_SPEED`, `OGRE_SPEED_KEYS="down,down,down,down,down"` | the boot log reads `game speed x8` (the file is loaded), then `x4`, `x2`, `x1`, and the last two presses change nothing; the last capture reads `1x` |
| C, the row reaches 8 | `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="Right,Right,Right"` | `game speed x2`, `x4`, `x8`, a badge show per step, and `/tmp/ogre-s137j/settings.cfg` holds `game_speed = 8` |
| D, the panel owns the keyboard | `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="=,=,="` | no `game speed` line and no `[badge]` line: three `=` presses changed nothing while the panel was open |
| E, the clock really moves | A vs `OGRE_SPEED=1` (no keys) vs `OGRE_SPEED=8`, all `OGRE_EXIT_AFTER_MS=14000` | display lists submitted in 14 s wall: **1270** with the keys (1x for the first second, then 8x), **260** at a pinned 1x, **1630** at a pinned 8x |
| F, the badge over the game's own picture | `screencapture -x -R203,28,960,720` at t=6.2 s of a keys run at `OGRE_SPEED_KEYS_AT_MS=5000` | `docs/proofs/native-speed-badge-8x.png`: the badge in the lower-right corner over the title's backdrop, with the `[badge] 8x shown` line at t≈6.3 s in the run's log |
| G, the row and the badge in the panel | `OGRE_SPEED=8 OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_CAPTURE_OVERLAY=…`, and `OGRE_LAUNCHER=1 OGRE_LAUNCHER_TAB=settings OGRE_SPEED=8 OGRE_LAUNCHER_SHOT=…` | `docs/proofs/native-overlay-settings.png` and `docs/proofs/native-launcher-settings.png`, both refreshed: `GAME SPEED [ ] 1 [ ] 2 [ ] 4 [x] 8` with the row inside the panel's width. Both old files still showed the three-option row |
| H, the pad cycles and wraps | `OGRE_SPEED=1 OGRE_SPEED_PAD="r3,r3,r3,r3,r3" OGRE_CAPTURE_BADGE=…` | `[SDL] speed pad (R3) -> x2`, `x4`, `x8`, **`x1`**, `x2` — the wrap, with a badge show per press; `/tmp/ogre-s137q/settings.cfg` holds `game_speed = 2` |
| I, the pad from a value outside the table | `OGRE_SPEED=6 OGRE_SPEED_PAD="r3,r3"` | the boot reads `x6`, then `x8` (the first offered value above 6) and `x1` (the wrap) |
| J, the pad and the panel | `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_SPEED_PAD="r3,r3"` | no `speed pad` line and no `game speed` line: both presses were ignored while the panel was open |
| K, a key and a pad press in one run | `OGRE_SPEED=1 OGRE_SPEED_KEYS="up" OGRE_SPEED_PAD="r3"` | `speed key -> x2` then `speed pad (R3) -> x4`: the key script is delivered first in the frame both are due |
| `make app` | — | links clean; the only warning is the pre-existing `ld: ignoring duplicate libraries … libSDL2.a` |
| `git status --short` | — | `app/CMakeLists.txt`, `app/src/{main,sdl_platform,settings}.cpp`, `app/src/settings.hpp`, `app/src/speed_badge.{hpp,cpp}`, the three proof images, this handoff and the four record files. `tools/RT64`'s modifications pre-date this session (newest mtime 2026-10-12 15:22) |

The badge text itself was read back from the PPMs, not inferred: the capture
decodes to `8x` and, in run B, to `1x`. Runs A, B, C and D were repeated on the
final binary after the position code was factored into `place()` for the
per-frame follow, with the same log lines and the same captures; the pad runs H
to K were run on the final binary.

Note on the build directory: `make app` links **`build-dist/ogrebattle64`**
(`DIST_STATIC_SDL` defaults to 1). The first test run used
`build-app/ogrebattle64`, which is stale (Oct 11) and has no badge strings, and
produced no `[badge]`/`[speed key]` output at all.

## 5. What is left open

* **No badge in fullscreen.** The badge is a separate window over the game
  window, and fullscreen is a display space of its own on macOS, so there is
  nowhere to draw it. The speed still changes; only the readout is missing. The
  tick checks `GraphicsConfig::wm_option` and logs the reason once
  (`[badge] fullscreen: the renderer owns the display, so the speed readout is
  not drawn`) instead of placing a window from a stale window rect. Recorded as
  `PLAN.md` open work 18.
* **The badge hides itself when the pointer is over it.** SDL2's
  `SDL_HitTestResult` has no click-through value (the bundled 2.32 headers have
  `NORMAL`, `DRAGGABLE` and the resize edges, and cocoa implements only
  `DRAGGABLE`), so the window takes the mouse focus while the pointer is inside
  its 56x47 rect. The events that would not reach the game are the cursor
  motion's (`ogre_map_cursor_note_mouse` requires the game window's id) and a
  click; the badge hides on mouse focus instead. `speed_badge_window()` is also
  checked in `set_game_cursor_hidden` so the operating system pointer does not
  reappear over the badge. **Not verified by hand**: the developer did not move
  the pointer into the badge's corner during a show, so the hide-on-focus path is
  code-verified only.
* Fullscreen was not exercised either; the run for it would be
  `OGRE_WINDOW=fullscreen` with the keys, and the expectation is the log line
  above and no window.
* `OGRE_SPEED=3` (outside the row) was reasoned about, not run: up should reach 4
  and down 2, because the step compares numerically.
* **The pad's R3 was verified through synthetic `SDL_CONTROLLERBUTTONDOWN`
  events, not a real pad**, which this machine does not have (session 92 recorded
  the same gap for the CONTROLS rebinding test). The script enters at the same
  handler a real press does, so the cycle, the wrap, the out-of-table join, the
  badge and the panel rule are all exercised; what is not exercised is SDL's own
  controller layer deciding that R3 was pressed. `OGRE_SPEED_PAD` is the
  instrument. The developer should confirm the button feels right in play, and
  say so if another input would be better.

## 6. Files changed

| file | what |
|---|---|
| `app/src/settings.hpp` | the offered table's comment, `step_game_speed`, `cycle_game_speed` |
| `app/src/settings.cpp` | `kGameSpeeds = {1,2,4,8}`, `step_game_speed`, `cycle_game_speed`, the change log line, the file's own comment |
| `app/src/speed_badge.{hpp,cpp}` | new: the badge window, its draw and hold, `OGRE_SPEED_KEYS`, `OGRE_SPEED_PAD`, `OGRE_CAPTURE_BADGE` |
| `app/src/sdl_platform.cpp` | `handle_speed_key`, `handle_speed_pad`, the two event-loop calls, the badge window in the pointer-window test, the `speed_badge.hpp` include |
| `app/src/main.cpp` | `speed_badge_init` beside `overlay_init`, `speed_badge_tick` beside `overlay_render`, `speed_badge_shutdown` |
| `app/CMakeLists.txt` | `src/speed_badge.cpp` in the native source list |
| `docs/proofs/native-speed-badge-8x.png` | new: the badge over the game's picture, from a screen-region capture |
| `docs/proofs/native-overlay-settings.png`, `native-launcher-settings.png` | refreshed; both were stale (three speed options, and the overlay's was missing MOUSE ENABLED) |
| `docs/guides/app-build.md` | the Game speed section (four values, the hotkeys, the pad's R3, the badge), `OGRE_SPEED_KEYS`/`OGRE_SPEED_PAD`/`OGRE_CAPTURE_BADGE` in the env table, the CONTROLS notes |
| `PLAN.md` | Status (Player features) and open work 18 |
| `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | the record |

No probe was used, so nothing was reverted. `grep -rl probe RecompiledFuncs/
Bank*Funcs/ app/` is unaffected: no generated file was touched.
