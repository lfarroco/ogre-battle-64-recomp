# Handoff — 2026-10-10, session 127: the world map's cursor follows the mouse

> **Summary.** Session 127 handoff — **the merged PR `cf5b9285` adds no working mouse support, and the world map's cursor is now driven by the mouse.** The PR's `ogre_cursor_click_apply` is defined and called from nowhere: `git grep` finds it only at its own definition, and the generated bank code that would have to call it has no hook table, so the only observable effect was one `[cursor] click -> x,y` line. The map's cursor is a screen-space sprite at `state+0x52`/`+0x54` (`state = *(u32*)0x80197B18`), clamped by `func_ovlM_8019AF0C` to x `0x28..0x110`, y `0x28..0xC0`, and read by the scene hook's cursor draw `func_ovlM_801A103C` at `0x801A1C48`/`0x801A1C4C` just before it emits sprite `a2=6`. `app/src/map_cursor.cpp` writes the pointer's pixel into that pair at the draw site; `tools/map_cursor.py` inserts the call and `make bank-recomp` re-applies it, because bank code is outside the mod hook map. A left click ORs the N64 `A` bit into controller 0 in `get_input` for 8 polls, and a pointer within 24 pixels of a play-area bound holds the matching D-pad bit, which scrolls the view. Both are gated on the map's cursor draw, so they are inert on other screens. The operating system pointer is hidden over the game window and shown over the Esc overlay window. The click log prints the game's own cursor pixel beside the pointer's, and they agree at every click on the loaded map. All developer-confirmed.

Developer request: *"I just merged a PR in my project (commit cf5b9285). It claims
to add some basic mouse support, but I tested the game and could not reproduce
it. can you help me check if it really adds the mouse support feature?"* and then,
after the review: *"we should refactor/discard the contribution, and use only the
added knowledge. that 'world map' screen has a cursor. if we could make the cursor
position match the mouse, that would be very cool!"*

**Result: the merged PR contains no working mouse support, and the world map's
cursor is now driven by the mouse.** The cursor sprite follows the pointer and is
clamped by the game's own play area; a left click acts as the game's `A` and
selects the location under the cursor; a pointer pushed past the map's edge
scrolls the view; the operating system pointer is hidden while it is over the
game window. All four are developer-confirmed. The PR's own cursor design is
deleted; its two `_WIN32`-only fixes and its `tools/elfcheck.py` sign-extension
fix are left in the tree as they were.

## 1. The merged PR's cursor support is dead code

`cf5b9285` is a merge of `338c15a` ("Keep the window responsive, and aim the
world-map cursor with a click"), touching five files. Four parts are real:

| Part | File | Scope |
|---|---|---|
| An owned `IFileDialog` for the start screen's picker | `app/src/launcher.cpp` | `_WIN32` only |
| Quick Edit disable on the boot console | `app/src/main.cpp` | `_WIN32` only |
| `nm` sign-extension mask in the symbol check | `tools/elfcheck.py` | unrelated to input |
| `ole32`/`uuid` link for the COM dialog | `app/CMakeLists.txt` | `_WIN32` only |

The fifth part, `ogre_cursor_click_apply` (`app/src/sdl_platform.cpp`), is the
cursor feature, and **nothing ever calls it**:

- `git grep cursor_click` returns only the definition and its own helper
  functions. A caller in another translation unit, or an address-of, does not
  exist.
- The intended caller is generated bank code, which has no hook table. It is a
  flat library of `RECOMP_FUNC` bodies (`BankMFuncs/funcs.h`) and mentions the
  hook nowhere.
- The function has no compile error: compiling `app/src/sdl_platform.cpp`
  standalone with the project's own command from
  `build-app/compile_commands.json` produces `T _ogre_cursor_click_apply`. The
  symbol exists in the binary as dead code.

What the PR did do is print one `[cursor] click -> x,y` line on stderr per click.
That is the whole observable effect, and it is why the feature could not be
reproduced. Note also that `build-app/ogrebattle64` was dated 2026-10-03 while the
merge is 2026-10-09, so a test of the pre-existing binary contained none of it
either.

## 2. What the cursor actually is, at instruction level

The map module is bank unit M (scene `0x05`, descriptor `0x8018FD70`, enter
`func_8017B60C`, update `func_8017B858`, hook `func_8017B9C8`). The cursor is a
**screen-space sprite**, and its position is a pair of halves in the map's state
object, `state = *(u32*)0x80197B18`:

```
state[+0x50] (u8)   1 = draw the cursor   (set by the state machine at
                                          0x8019B094, 0x8019BD64, 0x8019D738;
                                          cleared at 0x8019B32C)
state[+0x52] (s16)  cursor x
state[+0x54] (s16)  cursor y
```

The **scene hook's cursor draw** reads that pair and emits sprite `a2=6`
(`func_ovlM_801A103C`, generated in `BankMFuncs/funcs_0.c`):

```
801a1c34: lw   v1,0x7B18(v1)     ; v1 = *(u32*)0x80197B18 = the map state
801a1c3c: lbu  v0,0x50(v1)       ; 1 = draw
801a1c40: beq  v0,zero,0x801a1c5c
801a1c48: lh   a0,0x52(v1)       ; x
801a1c4c: lh   a1,0x54(v1)       ; y
801a1c50: jal  0x8019F83C        ; emit sprite, a2 = 6 = the cursor
```

The **state machine that keeps the pair current** is `func_ovlM_8019AF0C`: a
jump table on `state[+0x1C4]` (bound `0x6F` at `0x8019AF48`, table at
`0x801A7F80`), with the cursor moved by clamped adds and subtracts at
`0x8019B388`, `0x8019B3A8`, `0x8019B3E8`, `0x8019B410`, `0x8019B4F0`,
`0x8019B7B4`, `0x8019B834`, `0x8019B894`, `0x8019C8E8` and `0x8019D514`, and
clamped to x `0x28..0x110`, y `0x28..0xC0` at `0x8019B8E0`-`0x8019B924`. The
clamps are the play area the pointer may place the cursor in.

The cursor is **not** a node grid. The developer asked for snap-on-rest first;
635 logged cursor positions (run 1) span x 49..240 and y 40..176 with 1-2 pixel
deltas and no repeated node set, so there is no node table to snap to, and the
developer cancelled that milestone: *"on second thoughts, we don't need the 'snap
on rest' feature. is works very well now!"*

Two facts about the state that a later session should not have to rediscover:
`state[+0x1CA]` is an index and `state[+0x110] + 6*index` is a 6-byte record
array the state machine writes the current index value into (a dozen sites, e.g.
`0x8019B110`-`0x8019B128`); and the `sw ..., 0x50($t2)` / `0x54($t3)` stores
inside the draw are the **display-list cursor** `D_800E9BA0`, not the state.

## 3. The change

- **`app/src/map_cursor.cpp`** (new) — the hook and the mouse state.
  `ogre_map_cursor(rdram, state)` writes the pointer's framebuffer pixel into
  `state+0x52`/`+0x54`. `ogre_map_cursor_note_mouse` aspect-fits the 320x240
  framebuffer inside the drawable and keeps the pixel, clamped, plus the edge
  direction. `ogre_map_cursor_click` arms the confirm.
  `ogre_map_cursor_take_confirm` and `ogre_map_cursor_edge_direction` are read
  by `get_input`.
- **`app/src/map_cursor.hpp`** (new) — the state layout, the call site, and what
  each entry point is for.
- **`tools/map_cursor.py`** (new) — inserts the call into `BankMFuncs/funcs_0.c`
  immediately before the `0x801A1C48` x load, and the declaration after
  `#include "funcs.h"`. Idempotent, `--revert` removes it, and an anchor that
  stops matching is an error rather than a silent no-op.
- **`Makefile`** — `bank-recomp` runs `tools/map_cursor.py` after
  `tools/hd_backgrounds.py`, because `Bank*Funcs/` is regenerated and bank code
  is outside the mod hook map (the `tools/hd_backgrounds.py` pattern).
- **`app/CMakeLists.txt`** — `src/map_cursor.cpp` in both the native and web
  source lists.
- **`app/src/sdl_platform.cpp`** — samples `SDL_MOUSEMOTION` and reports the
  click to `ogre_map_cursor_note_mouse`/`_click`; ORs the confirm's `A` and the
  edge D-pad bits into controller 0 in `get_input`; owns the operating system
  pointer's visibility. The PR's `note_cursor_click`, `ogre_cursor_click_apply`
  and their latched-click globals are deleted.
- **Docs** — `docs/guides/app-build.md` §"The world map's mouse cursor",
  `docs/DECISIONS.md` (2026-10-10), `PLAN.md` Key technical findings.

### How the three behaviours reach the game

**Cursor position.** The write happens on the game thread, at the draw site,
after the state machine has updated `+0x52`/`+0x54` for the frame and before the
draw reads them, so it wins for the frame in progress. The game's own clamp
still applies (`game=(40,40)` when the pointer is at the framebuffer corner).

**Click.** The click does not move the cursor. `ogre_map_cursor_click` arms a
counter, and `ogre_map_cursor_take_confirm` decrements it once per pad poll, so
`get_input` ORs `0x8000` (N64 `A`) into controller 0's buttons for 8 polls
(`OGRE_MAP_CURSOR_CONFIRM_POLLS`). This is the same path the keyboard and the
live console's `press` command use. The poll is `func_8007297C`
(`asm/1060.s:2163`), the report is `D_800E79B0`, and the OSContPad is
`D_800C4BF0` (`func_8007284C` reads `lhu 0(D_800C4BF0)`).

**Edge scroll.** The cursor's clamp means a pointer beyond a bound cannot place
the sprite there, so the intent is sent as **D-pad** input instead: a pointer
within 24 pixels of a bound (`OGRE_MAP_CURSOR_EDGE`) holds the matching bit
(`0x0100` right, `0x0200` left, `0x0400` down, `0x0800` up). The direction is
computed from the pointer's pixel clamped into the framebuffer, so a pointer in
the pillarbox or in the black border of a fullscreen window still scrolls.

**Scoping.** The map's cursor draw is the only report that the map is the active
scene, so it sets a frame counter and `map_is_active()` gates both the confirm
and the edge direction to it. The `A` a click injects is therefore inert on the
title, the menus and the mission screen. The hook first runs immediately after
the map module is DMA'd in (`[bank] loading overlay record rom=0x079750
ram=0x8019A7C0 size=0xDAD0`), so nothing precedes the map.

**The operating system pointer.** SDL has one cursor for the whole application,
so whoever hides it must show it again: `set_game_cursor_hidden` in
`pump_sdl_events` is the single owner, derives the state from
`SDL_GetMouseFocus() == platform.window && !overlay_visible()` on every pump
(rather than from ENTER/LEAVE, so a window that opens under a stationary pointer
is handled), calls `SDL_ShowCursor` only on a change, and restores the pointer on
the close path. The Esc overlay is its own window (`overlay.cpp:206`,
`SDL_CreateWindow("Ogre Battle 64: Options", ...)`), so the pointer is visible
over it. `OGRE_CURSOR=0` disables this part.

## 4. What was run for verification

The map was reached from the developer's own save without replaying the game:
`OGRE_SAVE=<path>/scene_14.bin OGRE_SAVE_RESET=1`, in an isolated run directory
(`OGRE_PREF_DIR`) so nothing wrote back to the developer's save file. `scene_14`
was chosen by the developer because later chapters have a larger world map.

| Run | Evidence |
|---|---|
| `/tmp/mapcursor-run1.log` | 1169 cursor samples. The first version, before the clamping fixes; 635 distinct positions, x 49..240, y 40..176, 1-2 pixel deltas, `y=40` is the top clamp. This is what ruled out a node grid. |
| `/tmp/mapcursor-run4.log` | 6 clicks. The click count reaching `get_input` is what showed the `A` path fires. |
| `/tmp/mapcursor-run5.log` | 44 edge pushes, 14 clicks. The `[mapcursor] edge push 0x100 at (278,113)` lines are the first proof the edge direction is computed and held. |
| `/tmp/mapcursor-run8.log` | 10 clicks with the game's own cursor beside the pointer, 38 edge pushes. On the loaded map every click has `game=` equal to `at=` (nine at 0-2 pixels; one logged before the map's first real draw as `game=(-1,-1)`). This is the evidence that a click cannot select a location other than the one under the pointer. |
| `/tmp/mapcursor-run9.log` | 14 `[cursor] hidden/shown` transitions, 30 edge pushes, 4 clicks. The pointer hides when it enters the game window and shows again when the overlay opens. |

Developer-confirmed, in their words: the cursor *"moves smoothly"*; a click
*"worked as expected (the character moved to the target location)"*; the edge
scroll *"worked nicely"*; and after the pointer hiding, *"very good!"*.

Builds: `cmake --build build-app --target ogrebattle64` and the same for
`build-null` both link. `tools/map_cursor.py` was run twice (second run reports
`already applied`), then `--revert` and apply again, leaving exactly three
`ogre_map_cursor` references in `BankMFuncs/funcs_0.c`.

**Not verified:** the exit-path pointer restore was not observed in a log,
because `set_game_cursor_hidden` only logs on a change and the pointer was
already visible when run 9's window closed (`[SDL] game window close requested`
is in the log, no `[cursor]` line after it). The `[dialog]`-style visual check of
the pointer hiding was the developer's report, not a capture.

## 5. Files changed

| File | What |
|---|---|
| `app/src/map_cursor.cpp` | new; cursor placement, click confirm, edge scroll, mouse state |
| `app/src/map_cursor.hpp` | new; the state layout and the call site |
| `tools/map_cursor.py` | new; the bank-code patcher |
| `Makefile` | `bank-recomp` re-applies the patcher |
| `app/CMakeLists.txt` | `src/map_cursor.cpp` in both source lists |
| `app/src/sdl_platform.cpp` | mouse sampling, click/edge reporting, pointer visibility; the PR's latched-click block removed |
| `docs/guides/app-build.md` | "The world map's mouse cursor" section and the six knobs |
| `docs/DECISIONS.md` | the 2026-10-10 entry |
| `PLAN.md` | Key technical findings |
| `docs/HANDOFF-2026-10-10-session127.md` | this file |

`BankMFuncs/funcs_0.c` carries the inserted call but is gitignored and
regenerated; the patch is the tracked artifact. Nothing is committed, and
`git status --short` shows only these files plus the modified `tools/RT64`
submodule pointer that predates this session. The merge's `launcher.cpp`,
`main.cpp`, `app/CMakeLists.txt` (COM link) and `elfcheck.py` changes are
untouched.

## 6. Probes used and reverted

- `ogre_cursor_click_apply` and the PR's latched-click block: **deleted**, not
  reverted, because the design was discarded. `git grep cursor_click` is empty.
- The location-record logging (`slot=`, `value=`, `other=`) and the one-shot
  state dump (`OGRE_MAP_CURSOR_DUMP`): **removed** after the developer cancelled
  snap-on-rest. Nothing in the tree reads `state+0x110`/`+0x1CA` any more.
- `OGRE_MAP_CURSOR_EDGE_FORCE` is kept deliberately: it is documented as a
  measurement aid, so a scripted run can hold a direction with nobody at the
  mouse.
- No probe was added to `RecompiledFuncs/`; the only generated-code insertion is
  the production call that `tools/map_cursor.py` owns.

## 7. Open

- The cursor is only wired for scene `0x05`. Scene `0x07` shares the same
  dispatcher pair (`0x8017B858`/`0x8017B9C8`) and the same hook, but it streams a
  different module, and the hook bails out because the state pointer it is given
  is not the map's (`pointer != (uint32_t)state`). No other screen was checked
  for a cursor that would benefit.
- The click's `A` is not edge-triggered against the game's own input: a click
  while a real `A` is held is harmless (OR), but there is no debounce, so a
  double click is two confirms.
- The exit-path pointer restore is implemented and unobserved (see §4).
- The map's scroll speed is whatever the game's held-direction input does; there
  is no acceleration and no measured pixels-per-second.
