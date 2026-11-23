# Handoff — 2026-10-10, session 128: the mission field's cursor follows the mouse

> **Summary.** Session 128 handoff — **the mission field's cursor follows the mouse.** Scene `0x03` keeps its cursor as two f32 framebuffer pixels at `state+0x08`/`+0x0C`, where `state = *(u32*)0x801AB870 = 0x800E91D0` (a fixed address the mission's modules each hold as a pointer constant; session 115 read it through `*(0x80213690)` and took it for a heap pointer). The mover is `func_ovlN_801BE4E0` (bank unit N, record 7): it reads the **held** button word `*(0x800E8108)` = `D_800E79B0` and the profile's D-pad masks at `*(0x801F0CC0)`, integrates a per-frame delta (`0x801BE974`/`0x801BE988`) and clamps to x 40..280 / y 30..210 (`0x801BEB98`/`0x801BEBB8`, `0x801BEBE0`/`0x801BEC00`). `app/src/mission_cursor.cpp` writes the pointer's pixel at the generated label `L_801BEC04`, after both clamps and before the "revert when no unit is focused" tail, inserted by `tools/mission_cursor.py` and re-applied by `make bank-recomp`. **The mission's cursor is digital (D-pad) and the world map's is analog (the stick at `*(0x800C47F0)+2/+3`)**, and it is a **servo the game drives on nearly every frame** (measured 1-9 px with nothing held), so the pointer hands it to the game only on an explicit signal — a direction held, the C-LEFT centring branch at `0x801BE890`, the mover's own delta sampled at `L_801BEC04` before the mover zeroes it, or the ~0.25 s window after a click — and **adopts** the game's position then, which is what leaves the cursor on the place a command acted on. The placement is at the mover's epilogue `L_801BECC8`, which both of its paths reach. A left click is the game's `A`. The armed click now expires on every poll, so a click on one screen cannot press `A` on another; the world map was re-checked afterwards and is unchanged. Developer-confirmed: the cursor follows the mouse, a click confirms, the arrow keys still move the cursor, the D-pad no longer jumps the pointer on release, and after a command the cursor stays on the clicked place.

Developer request: *"our mission now: adding mouse support to the mission
screen. I can help you test by driving the game (I can enter the tutorial
mission, it should be good for testing this)"* — the open lead session 127 left
(*"No other screen was checked for a cursor that would benefit"*).

**Result: the mission field's cursor (scene `0x03`) is driven by the mouse.** The
pointer places the cursor, a left click is the game's `A`, and the D-pad keeps
the cursor while it is held and hands it back without a jump. All four are
developer-confirmed. The world map's cursor (session 127) is untouched except
that its armed click now expires (see §5).

## 1. What the mission's cursor is, at instruction level

Scene `0x03` is **not** driven by the scene dispatcher: its descriptor's update
and hook are no-ops (`func_801737CC` is `jr ra`; the hook `func_801737D4` calls
`func_8016C814`, which is also a bare `jr ra`). The mission runs from the task
the enter queues with `func_80076F5C` at `0x8017375C`, i.e. record 7 (bank unit
N, RAM `0x801AD5C0`..`0x801F4050`).

**The cursor is a pair of f32 framebuffer pixels, and its state object is a fixed
address, not a heap pointer.** `state = *(u32*)0x801AB870 = 0x800E91D0`:

| offset | type | meaning |
|---|---|---|
| `+0x04` | u8 | camera zoom level 0..2 |
| `+0x06` | s8 | per-frame cursor delta x |
| `+0x07` | s8 | per-frame cursor delta y |
| `+0x08` | f32 | **cursor x, framebuffer pixels** (320x240, centre 160) |
| `+0x0C` | f32 | **cursor y, framebuffer pixels** (320x240, centre 120) |
| `+0x10` | f32 | camera scroll position |
| `+0x14` | f32 | camera scroll velocity |
| `+0x18` | u8 | flags; bit 1 is set by the mover at the end of every call |

`0x800E91D0` is in the main ELF's BSS, and several records hold the same word as
a pointer constant in their data — record 7 `0x801AB870`, record 9
`0x80213690` and `0x80213798`, record 2 `0x8019E1B4`, record 3 `0x801AB870`,
record 9b `0x8021AA90`, record 9c `0x8021EF14/20`, record 6
`0x801B33B0`/`0x801B3E40`. **One object, many pointer constants.** Session 115
read it through `*(0x80213690)` and called it a heap pointer; it is a fixed
address. The object is **re-laid-out per phase** (record 2's `func_ovlA_80198C08`
bzeroes `0x478` bytes and writes pointers at `+4`/`+8`), which is why the hook is
gated by its call site rather than by the object's contents: only the field's own
mover reaches it.

**The mover is `func_ovlN_801BE4E0`** (bank unit N, record 7):

| address | instruction | what |
|---|---|---|
| `0x801BE510` | `lw $v1, -18320($v1)` | `$v1 = *(0x801AB870)` = the state |
| `0x801BE574`-`0x801BE584` | `lui $v0,0x800F` / `lw $v0,0x8108($v0)` / `lhu $a0,0($v0)` | the **held** button word `*(0x800E8108)` = `D_800E79B0` |
| `0x801BE580` | `lw $v1, 0xCC0($v1)` | the input profile `*(0x801F0CC0)` |
| `0x801BE588`/`0x801BE5AC` | `lhu $v0, 0xE($v1)` / `0xC($v1)` | D-right `0x100` / D-left `0x200` |
| `0x801BE624`/`0x801BE648` | `lhu $v0, 0x8($v1)` / `0xA($v1)` | D-up `0x800` / D-down `0x400` |
| `0x801BE5A8`/`0x801BE5B8` | `li $v1, 26` / `li $v1, -26` | the first-press delta byte |
| `0x801BE924`-`0x801BE974` | `lb $v0,6($v1)` / `div.s` by `0x4300` / `mul.s` by `f20` / `add.s` / `swc1 $f4,8($v1)` | integrate x and store |
| `0x801BE988` | `swc1 $f2, 12($v1)` | store y |
| `0x801BEB98`/`0x801BEBB8` | `0x4220` / `0x438C` | x clamp 40.0 .. 280.0 |
| `0x801BEBE0`/`0x801BEC00` | `0x41F0` / `0x4352` | y clamp 30.0 .. 210.0 |
| `0x801BECA4`/`0x801BECA8` | `swc1 $f24,8($v0)` / `swc1 $f26,12($v0)` | revert to the pre-move pair |

**The movement is continuous, and it is digital.** The delta is integrated every
frame with an acceleration ramp (delta `±26` on the first press, then
`±8 * counter` with `counter` ramping 0..10 at `0x801BE688`, divided by 128 and
multiplied by 32.0), which is 6.5 px/frame at step 1 and 20 px/frame at step 10.
The button source is `*(0x800E8108)`, which the base ELF's `func_80077494` fills
with `&D_800E79B0` (the raw **held** buttons) at `0x800774BC`/`0x800774C0`, or
with a zeroed block when that pad is inactive. Its sibling `0x800C4BDC` is
filled with `&D_800E8100` (the **edge** mask) at `0x800774CC`/`0x800774D0`;
`func_8007297C` writes the held mask at `0x80072A9C` and the edge mask at
`0x80072A58`/`0x80072A5C`.

**The mission's cursor is digital and the world map's is analog.** The mission
banks never load `0x800C4BE0`/`0x800C47F0`, so no mission module reads the stick;
the world map reads it directly at `*(0x800C47F0)+2`/`+3` (`func_ovlM_8019AF0C`,
`0x8019B438`). The developer said the same from play: *"in the mission screen you
SURELY can move the cursor with the arrow keys (digital cursor, not analog)"*.
That difference is why the mission needed a handover rule the map did not (§3).

**The cursor is also a servo the game runs continuously.** Measured over a played
run, the pair moves 1-9 px (median 3.6) on nearly every frame with no button held
(`none mode0`: 123 of 162 sampled frame-pairs), from three sources: the mover's own
integration, its screen-centre pull, and the snap-to-unit writers
`func_ovlN_801B8070` (0x801B81C4/C8) and `func_ovlN_801B9694` (0x801B9C84/88).
That measurement is what decides the whole design (§3): a rule that hands the
cursor to the game whenever the game moves it locks the mouse out, and a rule that
always gives it to the mouse discards the recentring.

The cursor is a **screen** position: the mover converts it to a world point
(`0x801BEA40`-`0x801BEB28` clamps that point to the map's world bounds
`0x801F0DD8`/`0x801F0DE0`/`0x801F0DDC`/`0x801F0DE4`), and, when the world clamp
bit, projects the clamped point back to the screen and stores that
(`0x801BEB4C`-`0x801BEB6C`). The marker itself is drawn from the resolved world
position (`func_ovlN_801B0714` reads `state+8`/`+12` at `0x801B085C`/`0x801B0864`
and hands them to record 3's screen-to-world `func_ovlA_801AA8C4`). The visible
result is therefore the pointer's pixel, bounded by the map.

## 2. The change

Two calls are inserted into the mover, because the evidence the placement needs is
destroyed before the placement's own site:

| site | call | why there |
|---|---|---|
| `L_801BEC04` | `ogre_mission_cursor_note_move` | the one point on the mover's default path where its per-frame delta bytes still hold this call's values. The mover zeroes them at 0x801BEC48-0x801BEC58, before the epilogue |
| `L_801BECC8` (the epilogue) | `ogre_mission_cursor` | runs on **both** of the mover's paths, so it also sees the screen-centring branch at 0x801BE890 (`x = (2x + 160) / 3`, `y = (2y + 120) / 3`), which jumps straight to that label |

The placement reads the state pointer out of the module's own data word
(`*(u32*)0x801AB870`) and requires `0x800E91D0`, so it does not depend on which
register holds the state on either path.

- **`app/src/mission_cursor.cpp`** (new) — the two hooks and the mouse state.
- **`app/src/mission_cursor.hpp`** (new) — the state layout, both call sites, the
  arbitration, and the knobs.
- **`tools/mission_cursor.py`** (new) — inserts both calls into the generated
  `BankNFuncs/funcs_3.c` and both declarations after `#include "funcs.h"`. It
  finds the unit that **defines** `func_ovlN_801BE4E0` rather than hardcoding the
  file. Idempotent, `--revert` removes both, and an anchor that stops matching is
  an error, not a silent no-op.
- **`Makefile`** — `bank-recomp` runs `tools/mission_cursor.py` after
  `tools/map_cursor.py`, because `Bank*Funcs/` is regenerated.
- **`app/CMakeLists.txt`** — `src/mission_cursor.cpp` in both source lists.
- **`app/src/sdl_platform.cpp`** — samples `SDL_MOUSEMOTION` and reports the
  click to the mission cursor beside the map's, and ORs the mission's confirm
  into controller 0 in `get_input`.
- **`app/src/map_cursor.cpp`** — `ogre_map_cursor_take_confirm` now consumes the
  armed count on every poll and returns `map_is_active()` (§4).
- **Docs** — `docs/guides/app-build.md` §"The mission field's mouse cursor",
  `docs/DECISIONS.md`, `PLAN.md`, `docs/scenes.md`.

## 3. The arbitration, and three designs that failed

The pointer **places** the cursor unless one of four explicit signals says the
game is moving it itself, in which case the pointer **adopts** the game's position
instead of overwriting it:

| signal | log | source |
|---|---|---|
| a D-pad direction is held | `pad=1` | `*(0x800E8108)` & 0x0F00 |
| C-LEFT held in a mode that centres | `centre=1` | profile `+0x1C` = 0x0002 (C-LEFT; the profile table is `0x801E7E04` indexed by `*(0x801F0E04)`, dumped from ROM `0x13C4C4`), field mode `*(0x801F0E00)` < 2 or == 42 |
| the mover integrated a delta this call | `nudge=1` | `state+0x06`/`+0x07`, sampled at `L_801BEC04` |
| the post-click yield window is running | `yield=1` | armed by a left click |

Three simpler designs were tried on the developer and rejected. Each reason is a
property of the game rather than of the code, so they are recorded:

1. **Pin the cursor to the pointer every frame.** Overwrites the game's own D-pad
   movement, so the arrow keys appeared to move only the camera. Fixed by the
   `pad=`/`centre=` signals.
2. **Adopt whenever the state differs from what the hook last wrote** (a 1 px
   tolerance). Correct in principle and unusable in practice: the cursor is a servo
   (§1), so this fires on nearly every frame and the mouse is locked out.
   Developer: *"the pointer was being pulled to the right, I could move it only
   with the d-pad."*
3. **Adopt when the mover's delta bytes are non-zero, read at the epilogue.** The
   mover zeroes those bytes at 0x801BEC48-0x801BEC58, *before* the epilogue, so
   the test read zero except when the cursor was clamped; the game's own movement
   was still overwritten. Fixed by sampling at `L_801BEC04`.

The **post-click yield** is the fourth design, and it resolves the developer's
original report. A click is the one unambiguous signal that the game is about to
move the cursor itself: a command recentres the camera and carries the cursor with
it, so the world place under the pointer changes. For
`OGRE_MISSION_CURSOR_YIELD` frames after a click (default 15, ~0.25 s) the pointer
adopts the game's position, which leaves the cursor on the place that was just
acted on, so a second click confirms it. Outside that window the mouse owns the
cursor. Developer-tuned twice: 90 frames (~1.5 s) first, then 30, then 15 (~0.25 s). The
cost of the window is that the pointer does not respond during it, so the player
has to move the mouse afterwards to re-sync it.

Also fixed on the way: the release-jump. While the D-pad holds the cursor the
pointer must adopt the game's position, or releasing the direction snaps the
cursor back to a stale pointer pixel. Developer: *"the d-pad no longer has the
jumping pointer issue."*

## 4. The change to the armed click

An armed click used to hold the `A` until the screen that armed it was the active
one, which meant a click on the mission could fire later on the map (both
cursors are armed by the same click). `ogre_map_cursor_take_confirm` and
`ogre_mission_cursor_take_confirm` now consume one poll from the armed count
**whatever screen is up**, and return whether their own screen is active, so an
`A` armed on one screen expires instead of waiting for another. `get_input` asks
both (no short-circuit) so each count is consumed. On the screen that armed it
the behaviour is unchanged.

## 5. What was run for verification

The mission was reached with the developer's own battery save and session 112's
tap recipe, `OGRE_SAVE=assets/scene_5_suspend.bin OGRE_SAVE_RESET=1
OGRE_TAP_MS=2000 OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:1,0x03:a:4"`,
which reaches scene `0x03` at `t≈34.1 s` of game time (title `0x04` →
tutorial `0x17` → `0x02` → `0x0D` → the mission). Every run used its own
`OGRE_PREF_DIR`, so nothing wrote back to the developer's save file.

| run | evidence |
|---|---|
| `/tmp/mc-verify.log` | The write path, with the pointer parked by `OGRE_MISSION_CURSOR_AT=60,200`: the log's `game=` reads back `(60.00,200.00)` across many frames, so the value the hook stores is the value the game reads. |
| `/tmp/mc-live.log`, `-live2.log`, `-live3.log` | Developer-driven. `[missioncursor] hook active on the mission field` once; 1105 + 1157 + 698 hook lines, of which 1085 + 1154 + 696 have a live pointer pixel (real `SDL_MOUSEMOTION`); 14 + 22 + 10 clicks; 112 + 176 + 56 `confirm: A held for one poll` lines, exactly 8 polls per click; 223 `pad=1` frames in `-live3.log`. |
| `/tmp/mc-pad.log`, `-pad2.log` | The arbitration, scripted: with a direction held the log shows `pad=1` for 181 / many frames while the **game's own** cursor moves (`145.00,114.25` → `145.00,116.75` → `145.00,120.75`; and `64.25,166.00` → `51.00,166.00` → `40.00,166.00`, the x clamp) and the hook's target is not written. |
| `/tmp/mc-pad2.log` | The handover: at the `pad=1` → `pad=0` transition the mouse position becomes the cursor's own position (`game=(235.18,147.19) mouse=(235.00,147.00)`), not the parked pixel `(200,40)`, so the release has nothing to snap back to. |
| `/tmp/mc-live5.log` | The world map after the `take_confirm` change, same developer, one run with both logs on: 131 `[mapcursor] draw=` lines, 16 edge pushes (`0x800`, `0xA00`, `0x200`, …), and one click logged as `click at (186,151) game=(186,151) draw=1` — the game's own cursor exactly on the pointer's pixel. |
| `/tmp/mc-play2.log` | The measurement that decided the design: over 548 logged hook frames, the cursor pair moved on nearly every frame with nothing held — classified by the previous frame's state, `none mode0` moved on 123 of 162 pairs. Per-frame step with no button held and mode 0: p10 1.0, p50 3.6, p90 9.0, p99 152 px. That is the servo of §1, and it is why design 2 in §3 locked the mouse out. |

Developer-confirmed, in their words: the mission cursor *"follows the mouse"*, a
left click *"confirms"*, and the arrow keys *"still move the cursor"*; *"the d-pad
no longer has the jumping pointer issue"*; and after the yield window was added,
normal movement, the post-command cursor position and the arrow keys all hold at
once. The three negative reports in §3 (arrow keys moving only the camera; *"the
pointer was being pulled to the right, I could move it only with the d-pad"*; the
cursor not staying on the clicked place) are what produced the three rejected
designs. On the world map the cursor still follows the mouse, a click still
selects, and the edge scroll still works.

Builds: `cmake --build build-app --target ogrebattle64` and
`cmake --build build-null --target ogrebattle64` both link. `make
bank-recomp` regenerates `BankNFuncs/` and re-applies both patchers
(`map_cursor: already applied`, `mission_cursor: already applied`), with four
`ogre_mission_cursor` references left in `BankNFuncs/funcs_3.c` (two declarations,
two calls) and three `ogre_map_cursor` in `BankMFuncs/funcs_0.c`. A 45 s `build-null` boot exits 0 and
passes `tools/runlog.py --check` (`PASS: no crash, no stub call, no unknown
module, no bad RSP exit, no stubbed non-gfx task`).

**Not verified:** the cursor's exact art on screen at the placed pixel. The recon
found no 2D emitter that reads `state+8`/`+12`, so the marker is reached through
record 3's screen-to-world and record 9's streamed draw, and that chain was not
traced to the emit.

## 6. Two process notes

**Do not run a window with a placement aid while the developer is testing a
window.** Two of the developer's test rounds were wasted by my own scripted runs.
The first had `OGRE_MISSION_CURSOR_AT=60,200`, which parks the pointer, so the
real mouse could not move the cursor and the feature looked broken; the second
opened a second window with the same aid while the developer was testing the
first, which produced the same wrong report. `OGRE_MISSION_CURSOR_AT` is
documented as a test aid for exactly this reason.

**Do not `pkill` the game between rounds.** A scripted run was started with
`pkill -f build-app/ogrebattle64`, which kills the developer's own window as well
as the script's; it happened twice and cost a round each time. A scripted run
must kill only its own PID (the scripts here do), and a replaced test window must
be killed by the PID it was launched with.

## 7. Files changed

| File | What |
|---|---|
| `app/src/mission_cursor.cpp` | new; the delta sampler, the placement/adoption rule, the post-click yield, the click confirm |
| `app/src/mission_cursor.hpp` | new; the state layout, both call sites, the arbitration, the knobs |
| `tools/mission_cursor.py` | new; the bank-code patcher, both insertions |
| `Makefile` | `bank-recomp` re-applies the patcher |
| `app/CMakeLists.txt` | `src/mission_cursor.cpp` in both source lists |
| `app/src/sdl_platform.cpp` | pointer/click reporting for the mission cursor; both confirms asked |
| `app/src/map_cursor.cpp` | the armed click expires on every poll |
| `app/src/map_cursor.hpp` | the `take_confirm` contract |
| `docs/guides/app-build.md` | "The mission field's mouse cursor" section and the knobs |
| `docs/DECISIONS.md`, `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md` | the record |
| `docs/scenes.md` | the mission cursor row |
| `docs/HANDOFF-2026-10-10-session128.md` | this file |

`BankNFuncs/funcs_3.c` carries the inserted call but is gitignored and
regenerated; the patch is the tracked artifact.

## 8. Knobs

| variable | what |
|---|---|
| `OGRE_MISSION_CURSOR=0` | disable the placement and the click's `A` (`off`, `false`, `no` too) |
| `OGRE_MISSION_CURSOR_LOG=1` | one `[missioncursor]` line per change of the game's cursor, the pointer, the D-pad state; one line per confirm poll; one line per click |
| `OGRE_MISSION_CURSOR_CONFIRM_POLLS=<n>` | how many pad polls a click holds `A` (default 8) |
| `OGRE_MISSION_CURSOR_YIELD=<n>` | frames after a click during which the pointer adopts the game's cursor instead of placing it (default 15, ~0.25 s). Raise it if a command's camera move stops sticking; lower it if the mouse feels unresponsive after a click |
| `OGRE_MISSION_CURSOR_AT=<x>,<y>` | park the pointer at that framebuffer pixel once, with no mouse event. A measurement aid, so a scripted run can place the cursor with nobody at the mouse |

## 9. Open

- **Edge scroll is not implemented for the mission.** The map scrolls its view by
  holding a D-pad bit when the pointer is near a play-area bound; the mission
  needs no equivalent, because the cursor is clamped to the map's world bounds
  and the camera follows the cursor. The developer asked for movement and the
  click first.
- **The mission's menus are not mouse-driven**: the unit-command menu, the `R`
  hold-menu and the Organize Screen (scene `0x06`, bank unit S) each have their
  own cursor. The Organize Screen is the next candidate; it is a separate scene
  and a separate module.
- **The cursor is a servo, and the app overrides it.** Outside the yield window
  the mouse's placement wins over the game's continuous cursor motion (§1), which
  is what makes the cursor track the pointer at all; the cost is that the game's
  own cursor behaviour is suppressed except in the four signalled cases of §3. If
  a screen state appears where the game's cursor logic matters and none of the
  four signals fires, that state needs its own signal — do not widen the yield.
- The sub-pixel difference between the pointer's pixel and the game's cursor
  (e.g. `game=(226.46,58.25)` against `mouse=(226.00,58.00)`) comes from the
  mover's own integration running after the placement on some frames. It is well
  under one pixel and was not chased.
