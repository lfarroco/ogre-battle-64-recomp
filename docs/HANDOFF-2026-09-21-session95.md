# Session 95 — the DEBUG tab and the Chaos Frame

## Goal

Add a `DEBUG` tab to the app's shared menu panel (the launcher and the in-game
Esc overlay) that shows the player's current **Chaos Frame**, the hidden value
that decides the good and the bad ending and that the game prints only on the
scene-`0x13` screen after the credits.

## Result

The tab is in and it reads the live game. One read-only row under a
`GAME STATE` section shows `Chaos Frame    <n>  <band>`, where the band is the
ending band the value falls in (developer): **0-35 `LOW`, 36-64 `NEUTRAL`,
65-100 `HIGH`**.

The byte is **`0x801936C9`**, not the community figure `0x801936A9`.

## 1. The address: `0x801936C9`

The developer supplied `0x801936A9` (u8) from an online source
([ogrebattle64.net](https://ogrebattle64.net/research/how-to-track-and-edit-chaos-frame-with-the-project64-debugger/),
a Project64 symbol) and said it might be inaccurate. It is, for this ROM. Three
independent instruction-level checks in `build/ogrebattle64.elf` point at
`0x801936C9`:

1. **The game-state initialiser writes the documented default there.**
   `func_8016C900` bzeroes 58 (`0x3A`) bytes at `0x80193698` (`move a0,s2` at
   `0x8016C928`, `jal func_80093380` at `0x8016C974` with `a1=0x3A`) and then
   stores `50` at `0x801936C9` (`li v0,50` / `sb v0,14025(at)` at `0x8016C998`).
   The community source says the Chaos Frame is 50 at the title screen and on a
   new game, and that it ranges 0..100.
2. **The ending screen reads it.** The scene-`0x13` update `func_801B5128` loads
   `0x801936C9`, masks it with `0x7F` and stores it into the sprite it lays out
   (`lbu v0,14025(v0)` / `andi v0,v0,0x7F` / `sw v0,8(v1)` at `0x801B5360`).
   It is the only byte of the flags block that screen reads.
3. **The scene script can set it.** The scene-script VM (`func_80170974`) has a
   jump table at `0x80190BC8`; its opcode `0xFF` entry (`0x8017146C`) stores the
   script operand at `0x801936C9` (`sb v0,14025(at)` at `0x80171480`).

`0x801936A9` has **no** static read or write: an ad-hoc scan of
`build/ogrebattle64.elf` and all 34 bank ELFs for the `lui` + `lbu`/`lb`/`sb`
pair that names the address found nothing, so it can only change through a
pointer, the whole 121-byte block's save/load, or a DMA. It read `0` at the
attract loop after a completed playthrough, where the Chaos Frame is 50, and it
read `173` / `237` from the two endgame saves, values outside the documented
0..100 range.

Live readings of `0x801936C9` (all from RDRAM dumps taken with the unchanged,
pre-change `build-app` binary, so no probe is involved):

| run | scene | `0x801936C9` | `0x801936A9` |
|---|---|---|---|
| `last_perfect` (battery slot 0) | `0x05` map | **99** (`0x63`) | 173 |
| `last_low_chaos` (battery slot 0) | `0x05` map | **100** (`0x64`) | 237 |
| `suspend_final_boss`, at the attract loop after the credits | `0x09`/`0x0A` | **50** (`0x32`, the default) | 0 |

`/tmp/last_perfect-map.bin`, `/tmp/last_low_chaos-map.bin`,
`/tmp/credits-natural.bin`; each was written by `OGRE_CONSOLE_ON_SCENE` +
`OGRE_CONSOLE_ON_CMD='dump …'`.

## 2. The tab

* `app/src/ui.hpp` — `Tab::Debug` (after `Settings`, before `Count`),
  `RowKind::ChaosFrame`, and `Panel::set_chaos_frame()` /
  `clear_chaos_frame()`. The panel does not read guest memory; the caller pushes
  the value in before `draw_panel`, so the launcher (no running game) and the
  overlay share one implementation.
* `app/src/ui.cpp` — `tab_label` returns `DEBUG`; `tab_enabled` enables it in
  Overlay mode next to `CONTROLS`/`SETTINGS`; `rebuild_rows` adds the
  `GAME STATE` section and the row; `left_text`/`right_text` render it
  (`GAME NOT RUNNING` when no value was pushed; otherwise `<n>` plus
  `chaos_frame_band()`, which returns `LOW`/`NEUTRAL`/`HIGH` for
  `<=35`/`<=64`/else); the `activate` switch returns `RowAction::None`
  (read-only).
* `app/src/overlay.cpp` — `guest_byte()` reads one logical byte of the running
  game's RDRAM (`rdram[(addr & 0x1FFFFFFF) ^ 3]`, the rule in
  `docs/guides/app-build.md` → "Reading a dump"); `draw()` samples
  `CHAOS_FRAME_ADDRESS` every frame; `OGRE_OVERLAY_TAB=debug` opens the tab.
* `app/src/game.hpp` — `CHAOS_FRAME_ADDRESS = 0x801936C9` with the evidence.

The launcher's DEBUG tab (the panel is shared) shows `GAME NOT RUNNING`, because
the launcher has no running game and never calls the setter.

## 3. Verification

* `docs/proofs/native-overlay-debug-tab.png` — `OGRE_OVERLAY_TAB=debug` +
  `OGRE_CAPTURE_OVERLAY`, at the map with `last_perfect` on the battery. The tab
  bar shows `[ DEBUG ]` and the row shows `Chaos Frame  99  HIGH`; the same
  byte reads `63` hex = 99 through the live console in the same run, and 99
  through `tools/rdram.py` in the offline dump. A second capture at the title
  showed `50  NEUTRAL`, the documented starting value.
* The RDRAM readings in the table above are independent of the panel: they come
  from the game's own memory through the live console's `dump`, and the port was
  rebuilt from scratch after the address was changed.
* `make app` (both `build-dist`, the default static-SDL build, and
  `build-app` with `DIST_STATIC_SDL=0`, which `tools/run-save.sh` uses) builds
  clean. The only warning is the pre-existing `renderer.cpp:484` `-Wxor-used-as-pow`.

**Not verified.** No automated run reached the scene-`0x13` screen, so the
displayed end-screen number was never compared with the byte in one shot. The
suspend save in front of the final boss needs input through the last mission:
with no taps it stalls at `t≈44 s` (scene `0x02`/`0x0D`), and a continuous `A`
tap drives the title's `Load Game` to the map instead. The developer reported the
same: their previous run did not reach the end of the credits. The proof capture
gets as close as the port can without that run: it checkpoints the map (scene
`0x05`) with `last_perfect`, reloads the checkpoint, and the panel and the live
console then read the same byte. A future session with a save in front of the
*ending* (not the final boss) can close the last step in one run: read
`0x801936C9` at scene `0x13` and compare it with the printed number.

## 4. Files changed

| file | change |
|---|---|
| `app/src/game.hpp` | `CHAOS_FRAME_ADDRESS = 0x801936C9` + evidence comment |
| `app/src/ui.hpp` | `Tab::Debug`, `RowKind::ChaosFrame`, `set_chaos_frame`/`clear_chaos_frame`, two members |
| `app/src/ui.cpp` | tab label/enable, the DEBUG rows, the row text, the no-op activate |
| `app/src/overlay.cpp` | `guest_byte()`, the per-frame sample, `OGRE_OVERLAY_TAB=debug` |
| `docs/guides/app-build.md` | `OGRE_OVERLAY_TAB` gains `debug` |
| `docs/symbols.md` | `0x801936C9` → `g_chaos_frame`, with the evidence |
| `PLAN.md`, `docs/DECISIONS.md`, this handoff | the record |

**Probes: none.** No generated C, runtime or app file was instrumented, so
there is nothing to revert.

## 5. Open

* The scene-`0x13` screen has not been read live (see "Not verified").
* `0x801936A9` remains unidentified. It is inside the same saved flags block
  (`0x80193698`, 121 bytes, listed in the save region table at `0x80187464`), so
  it is game state; naming it is a separate question.
