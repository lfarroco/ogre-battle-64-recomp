# Session 213 handoff — issue #24 items 1-4 (fork fixes taken upstream)

## Goal

Developer request: implement items 1-4 of the analysis of
`https://github.com/lfarroco/ogre-battle-64-recomp/issues/24`, the Windows fork's
four commits, without changing branches.

1. `upscale2D = Original` in `set_application_user_config` (`2e2b65d`).
2. `OGRE_SCENE` stores the game's own scene request (`33c5df6`).
3. Dispatch `0x801AB740`/`0x801AB76C` (`33c5df6`).
4. Rewrite `tools/stubmap.py`'s `looks_like_entry` (`33c5df6`).

The fork is Windows-only and its base is `6429179`; this tree is `ad6f6a3`, so
each item was ported against the current files, not applied as a patch.

## Result

All four landed and build. Items 1 and 2 were verified with runs; item 3 is
verified in the generated code and its runtime check needs the credits scene;
item 4 is a tool change with its counts below.

## Item 1 — RT64 2D rectangles sample at native pixels

`app/src/renderer.cpp` sets `app->userConfig.upscale2D` in
`set_application_user_config`: `Original` by default, `ScaledOnly` with
`OGRE_UPSCALE_2D=scaled`, `All` with `=all`. The boot log prints
`[renderer] 2D rectangles: upscale2D <mode>` once.

Mechanism, from RT64's own code: `ScaledOnly` upscales every `G_TEXRECT` whose
scale is not exactly 1024 (`DrawCall::identityRectScale`, consumer
`tools/RT64/src/hle/rt64_state.cpp:1089-1161`). OB64's dialogue box is three
rectangles stretched by 1026..1169/1024 over a 16x61 texel window, so at a
scaled resolution the last row and column sample past the window. `Original`
snaps each rectangle's UV to the native pixel (`RasterPS.hlsl`, `lowResUV`).

A/B at 3x (960x720 present), cathedral dialogue step 2, one setting changed per
run (`OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_RESOLUTION=auto
OGRE_DISPLAY_EXPERIMENTS=1 OGRE_CAPTURE_PRESENT=...`):

- `OGRE_UPSCALE_2D=scaled` (RT64's default behaviour): a dashed light row along
  the box's bottom edge, over the black shadow, and a dark column between the
  box's left cap and the portrait. Rows 238-239 of the frame differ from the
  `original` frame across ~790 of the 830 sampled columns; the rest of the box
  differs by single-digit counts (text timing).
- `OGRE_UPSCALE_2D=original` (new default): neither artifact.

Crops are `docs/proofs/native-dialogue-upscale2d-scaled.png` and
`-original.png`. The 3x present is the window's `auto` integer scale on this
machine; the fork measured the same artifact at 9-12x.

## Item 2 — `OGRE_SCENE` writes `D_800C4C26`

Instruction-level evidence from `build/ogrebattle64.elf`, `func_80075BC0`:

- `0x80075DA8: lhu v0,-32236(v0)` (v0 = `0x800F0000`) reads the root id
  `D_800E8214` at the loop top.
- `0x80075F54: sh s1,19494(at)` stores `0xFFFF` (s1) into `D_800C4C26` in the
  `beqz v0` delay slot, before each enter.
- `0x80075FB8: lhu v0,19494(v0)` / `0x80075FC0: beq v0,s1,0x80075FB8` spins
  while the request word is `0xFFFF`.
- `0x800760A0` tests bit 15; `0x800760C4: andi v0,v0,0x7fff` clears it;
  `0x800760CC` writes it back to `D_800C4C26`; `0x800760D8: sh v0,4(v1)` stores
  the id into the block and `0x800760DC` jumps to the loop top.

`app/src/bank_overlays.cpp` replaces `poke_scene` with `request_scene`, which
writes `0x8000 | id` only when a scene past the boot's `0x00` runs and the word
reads `0xFFFF`, and `try_force_scene`, called from `poll_scene` and
`force_scene_on_load`. `OGRE_SCENE_TRACE` now prints `request(D_800C4C26)` and
`root(D_800E8214)`. The `bank_overlays.hpp` comment is rewritten.

Verified:

- `OGRE_SCENE=title OGRE_SCENE_LOG=1 OGRE_SCENE_TRACE=1 OGRE_EXIT_AFTER_MS=12000`
  (1x): the word stays `0x0000` through scene `0x00`, then
  `[scene] forcing scene 0x04: requested 0x8004 while 0x09 runs`,
  `[scene] forced scene 0x04 is active` at `t=6353ms`, and the trace reads
  `request=0x0004 block->id=0x0004 done=1`.
- `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2`: forced `0x02` at `t=2724ms`
  while `0x09` runs, step seeded, `0x000D` at `t=2930ms`, dialogue box renders.

Timing changed: a forced screen now arrives at the first transition after the
boot's `0x00`, ~6.4 s at 1x (was ~2.7 s with the old early poke), ~2.7 s at 8x.
`AGENTS.md` rules 9 and 13 and `docs/guides/app-build.md` are updated.

One stale note removed: `docs/guides/app-build.md` said forcing `new-game` (0x02)
still SIGSEGVs. It reaches `0x0D` in this session's run, so the note is gone.

## Item 3 — `0x801AB740`/`0x801AB76C`

`func_80079BD8` calls `0x801AB740` at `0x80079C4C` and `0x801AB76C` at
`0x80079C78`. `config/config.toml` extends `func_801AB568` to `0x208`, which
covers `0x801AB740`, so N64Recomp bound both calls to `func_801AB568` and the
call re-entered that body's start (it builds a frame at `0x801AB568`; the code at
`0x801AB740` is the loop epilogue `L_801AB738`'s exit). Unit A registers both
addresses (`app/src/bank_funcs.inc`: `func_ovlA_801AB740`, `func_ovlA_801AB76C`),
and both are now in the `dispatch --only` line in the `Makefile`.

`make recomp` prints `dispatched 0x801AB740 (1 site(s))`, `0x801AB76C (1
site(s))` and 16 tail repairs. `RecompiledFuncs/funcs_3.c` now has
`LOOKUP_FUNC(0x801AB740)(rdram, ctx); goto after_63;` (and `after_64` for the
second), with the duplicated delay slot as dead code after the `goto`. Both
lookups miss the map and become the no-op stub when record 3 is not resident.

Open: the runtime check is the credits path (scene `0x11`, the path the
`config.toml` comment names). It was not played in this session; `OGRE_COVER=1`
or a save at the credits is the check.

## Item 4 — `stubmap.py` entry test

`looks_like_entry` now requires, in order: the previous non-nop pair is `jr ra`
plus its delay slot; no branch or `j` in the containing body reaches the
address; no word of the module's image holds the address; and the address opens
a frame (`addiu sp,sp,-N`) or returns within 16 instructions. `branch_target`
is the new helper. The four checks separate a function start from a body
interior, a switch case and data, which the old `lui`/`sw`/`j`/`jal`/`jr`
opener test admitted.

Before/after on the same tree:

- before: 986 static dispatch targets, 0 definite, **130 missing-function
  candidates**, 152 bank-selection candidates, 369 inert, 332 resolvable, 3 IMEM.
- after: 986 targets, 0 definite, **0 missing-function candidates**, 282
  bank-selection candidates (152 + 130), 369 inert, 332 resolvable, 3 IMEM.

`tools/stubmap.py report --strict` exits 0. The 130 reclassified targets are body
interiors in the module that a run would have to select, which is the
bank-selection class the report already documents.

## Verification

| command | result |
|---|---|
| `make recomp` | 19 sites in 11 files -> `LOOKUP_FUNC`, 16 tail repairs, both new targets dispatched |
| `make app` | clean (one pre-existing `-Wxor-used-as-pow` warning at `renderer.cpp:1083`) |
| `cmake --build build-app --target ogrebattle64` | clean |
| `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` | clean |
| `make patch-check` | PASS (no third-party tree changed) |
| `python3 tools/cross_bank.py check` | main unit 949 direct calls across 235 targets (was 951 across 236) |
| `tools/stubmap.py report --strict` | exit 0 |
| title run, 1x | `upscale2D original`, forced scene 0x04 active |
| cathedral run, 8x, `scaled` and `original` | both reach `0x0D` step 2; the A/B crops above |

The first title run used `build-app/ogrebattle64` before it was rebuilt and
showed the old trace format; `make app` writes `build-dist` when
`DIST_STATIC_SDL=1` (its default), so `build-app` was rebuilt with
`cmake --build build-app`. That is a build-tree detail, not a code finding.

Runs used `OGRE_NO_AUDIO=1` and a fresh `OGRE_PREF_DIR` per run; the scene and
capture runs were bounded with `OGRE_EXIT_AFTER_MS`. No probes were added or
reverted. `/tmp` artefacts: `/tmp/s203-*.log`, `/tmp/s203/*.ppm`.

## Crash report: loading a save with the Item Randomizer

The developer's save load SIGBUSed. Reproduced deterministically on their
battery (`build-app/saves/ogrebattle64-us-rev1.bin`, two valid slots) and route
(`OGRE_MOD_TEST=1 OGRE_SAVE=<battery> OGRE_SCENE=title OGRE_TAP_MS=1000
OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6"`):

- With the randomizer enabled (`build-app/mods.json` lists only
  `ogre_item_randomizer`): `Bus error: 10`, and the crash report is the same
  shape as the developer's: `signal: SIGBUS`, `n64 thread: 4`, fault address
  `rdram + 0x800375F0`, `[snap] callchain t4 crash (0 deep)`.
- With `enabled_mods` empty: the same battery loads to scene `0x05` and the run
  continues to the exit timer.
- With session 213's two dispatch targets removed from the `Makefile` and
  `make recomp` + a full app rebuild: the crash still happens, so it is not the
  cross-bank change. The dispatch was restored and verified afterwards.

Under lldb the fault is `movslq (%rbx,%rax)` with `rax = 0x375F0`, inside an
`rwx` region outside every loaded image (the mod's loaded code). The caller is
the Load Game update `func_80178430`; the randomizer replaces the save-field
reader `func_800749C0` to hook the save read. Opening the Load Game screen alone
does not crash; pressing A to apply a slot does. A rebuild of the mod with the
current toolchain (`build/mods/item-randomizer.nrm`, 5977 bytes, against the
developer's 7290-byte Oct 3 copy) crashes the same way.

The fault address has the same `rdram + 0x800375F0` shape as the four tutorial
SIGBUS reports in `PLAN.md` item 17, whose mod-off A/B was never run. Recorded
there and in item 16. Workaround for the developer: untick Item Randomizer in
MODS before loading. This session changed no mod code and no runtime mod
loader; the cause is still open.

## Files changed

- `app/src/renderer.cpp` — `upscale2D` in `set_application_user_config`.
- `app/src/bank_overlays.cpp`, `app/src/bank_overlays.hpp` — `request_scene`,
  `try_force_scene`, the trace line, corrected dispatcher comments.
- `Makefile` — `0x801AB740`/`0x801AB76C` in the dispatch list and its notes.
- `tools/stubmap.py` — `branch_target`, rewritten `looks_like_entry`.
- `docs/guides/app-build.md` — `OGRE_UPSCALE_2D` row, `OGRE_SCENE`/`FORCE_SCENE`
  rows, `OGRE_SCENE_AFTER_MS`, `OGRE_SCENE_TRACE`, the timing paragraph.
- `docs/scenes.md` — the Witch's Den note no longer says the poke races the boot.
- `AGENTS.md` — rules 9 and 13 (forcing timing).
- `PLAN.md` — Status bullet, Open work item 6 counts.
- `docs/DECISIONS.md` — three session-213 rows.
- `docs/proofs/native-dialogue-upscale2d-original.png`, `-scaled.png` — new.
- Generated and gitignored: `RecompiledFuncs/` regenerated by `make recomp`.

## Not done

The other issue #24 items are untouched: the audio fake reply
(`mesgqueue.cpp`), the idle-thread `[[patches.hook]]`, the hang report, the
lookup check, the RT64 queue condition variables, and the launcher text cache.
The first four are in the session-213 analysis; none needs a fork-specific
change.
