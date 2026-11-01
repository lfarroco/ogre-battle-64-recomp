# Session 202 handoff — the tutorial combat's green background and freeze: a stale framebuffer writeback over the njpeg buffers

## Goal

Fix the browser's combat screen. The developer's report: entering combat (tutorial
→ Field Map → Combat) renders only the background, in inverted/green-magenta
colours, then the screen goes black while the HTML page still responds.

## Result

The green frame and the freeze are one fault in the browser renderer's
framebuffer writeback:

1. **A framebuffer is written back after the game has reused its byte range.**
   The browser defers every writeback to `ogre_sync_framebuffers()`. A
   `GlFramebuffer` keeps its `min_row`/`max_row` until that writeback, so a
   640x481 colour image the game used earlier (address `0x1BA780`, span
   `0x1BA780..0x1E05C0` at 481 rows of 0x140 bytes) still records its draws when
   the game enters the tutorial combat. By then the allocator has reused that
   range: the scene-0x02 njpeg buffers are at `0x801CDA00` and `0x801CE3A0`,
   inside the old image's span. The sync writes the old image's pixels over the
   njpeg input and destination, so the S2DEX2 draw samples non-YUV bytes (the
   green/magenta bands), and the game's next `free()` of the njpeg buffer traps
   in the allocator node unlink. The page error is
   `memory access out of bounds` at `func_80071950` ← `func_800712C4` ←
   `func_ovlE_80199D30` ← `func_ovlE_8019A1FC`; the game threads stop, the main
   thread keeps presenting the black canvas.

2. **The writeback now applies RT64's stale-RDRAM rule.** `readback_framebuffer`
   hashes the image's RDRAM (`framebuffer_rdram_hash`, the same XXH3-64 the
   framebuffer-as-texture check uses) and compares it with the hash recorded at
   the last upload or writeback. A difference means the CPU wrote those bytes
   after the GPU copy; the writeback is skipped, its recorded rows are dropped,
   and `[GFX-FBWRITE-STALE]` names the image. RT64's
   `FramebufferManager::checkRAM` is the reference rule. This is the same class
   as session 170, from the other side: that fix stopped writing on every
   present, and this one stops a writeback that has become stale before it runs.

The tutorial's Unit Commands combat now renders over the practice field
(`docs/proofs/web-tutorial-combat-session202.png`), and the Field Map route that
used to trap passes scene `0x0002` into `0x000D` and back to `0x0003` with no
page error and the task count advancing (`1435` at the field, `3409` at `0x02`,
`4972` at the end of the run).

## Evidence

### Before the fix

Route: natural title → Tutorial → Field Map, then A. At game time `t=91870ms` the
log prints `[scene] id=0x0002` and loads record 0 (`rom=0x066E30`) and record 14
(`rom=0x281830`), then submits the njpeg task (`data_ptr=0x801CE3A0`,
`data_size=0x12C`) and draws 300 YUV macroblocks (`[GFX-S2D-CENSUS] task 3517:
DA=300 DC=300 ...`). `[GFX-FBSYNC]` then writes back seven images:

```
fb 0x000400 rows=[0,475]   fb 0x025C00 rows=[0,475]   fb 0x04B400 rows=[0,475]
fb 0x1BA780 rows=[0,480]   fb 0x250800 rows=[0,480]   fb 0x2E6840 rows=[0,479]
fb 0x3336C0 rows=[0,239]
```

and the next line is `worker sent an error! ... Uncaught RuntimeError: memory
access out of bounds`, with the stack above. Tasks freeze at 3517; present
continues at 13200 and the canvas is black.

A guest RDRAM dump at the freeze (`Module._ogre_guest_rdram`) reads:

```
state (*(0x8019A680)) = 0x801A8050
state[0x64] source fb  = 0x80025C00
state[0x70] destination = 0x801CDA00   (320x240x2 from here)
state[0x68] njpeg input = 0x801CE3A0   (the RSP task's data_ptr)
state[0x7E]             = 6
```

The image at `0x801CDA00` and the njpeg input at `0x801CE3A0` are inside
`0x1BA780`'s 481-row span (`0x1BA780 + 0x140*481 = 0x1E05C0`), and the bytes at
both are the image's `0x00010001` pixels.

The unfixed side is the developer's screenshot
(`docs/proofs/web-tutorial-combat-green-session202.png`): a full-screen
green/magenta striped background with no UI, then black.

### After the fix

The same route on the final build (`make recomp` + `make web`, probe removed):

```
n=75 scene=none tasks=3383
n=76 scene=0002 tasks=3409
n=77 scene=000D tasks=3429
n=90 scene=0003 tasks=3855
...
n=130 scene=0003 tasks=4972
errors (none)
```

`docs/proofs/web-tutorial-story-session202.png` is a frame from the `0x000D`
visit: the dialogue box over a black/leafy scene, not a green field.

The tutorial's Unit Commands lesson (`TUTORIAL menu` → C-Up to the lesson list →
`5. Unit Commands` → A) reaches the battle screen and renders
(`docs/proofs/web-tutorial-combat-session202.png`: Thompson vs Whitman,
`HP 199/199`, `205/205`, the unit grid, `AUTO`, the field behind it).

### The cathedral's green backdrop is older than this change

The New Game cathedral (scene `0x0D`, `OGRE_SCENE=new-game OGRE_STEP=2`, 25-30 s
after the scene line) renders green/magenta in the browser on this build. It
does the same on the unmodified `HEAD` renderer with the same route and delay:
`git show HEAD:app/src/web_renderer.cpp` was rebuilt into `build-wasm` and the
frame is the same green field (`/tmp/combat/base-cathedral-normal.png`). The
session-138 proof `docs/proofs/web-newgame-cathedral-njpeg.png` is from an older
build. The cathedral's raw backdrop and the combat's overwritten njpeg buffers
look alike but are separate: the writeback change below does not alter the
cathedral. The developer's combat frame may still contain this pre-existing
backdrop defect after the freeze is fixed; the tutorial's Unit Commands battle
(3D field) renders normally.

### The regression screens

On the final build, `scene.cjs` reaches and captures:

- `--route title`: scene `0x0004`, `25.17 tasks/s`, normal clouds/logo,
- `--route tutorial-field`: scene `0x0003`, `19.86 tasks/s`, textured grass,
- `--route mission-briefing`: scene `0x0003`, `24.80 tasks/s`, the NOTE box and
  the framed Castle Burgund illustration.

## Verification

- Final build: `make recomp` (probe202 reverted; `grep -rn probe202
  RecompiledFuncs Bank*Funcs app/` empty) then
  `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean.
- `node debug/probes/boot.cjs --attempts 3 --secs 40`: `RENDERED`
  (`nonBlack=307840`); the run reported `sawRealDl=false`, the known idle
  trajectory, so the scene routes above are the frame evidence.
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex` **fails on this
  build** (`fbtex hits=0`). The guard is not the cause: the same command with
  the guard compiled out (`if (false && fb.rdram_hash_valid)`) still reports
  `fbtex hits=0`. The testdraw scratch display list and colour images sit at
  12 MiB (`kDlOff=0xC00000`, `kFbOff=0xC02000`), above the 8 MiB guest RDRAM and
  inside the wasm heap, so the positive control depends on heap bytes beyond
  `rdram_`. This is a pre-existing test defect and is recorded as open.
- The final-build route captured 130 A presses through the `0x02` loader with
  no page errors and the task count at 4972.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `readback_framebuffer` skips a writeback whose RDRAM no longer matches `fb.rdram_hash`, drops its recorded rows, and logs `[GFX-FBWRITE-STALE]`; `stale_writebacks_` counter |
| `app/src/main_web.cpp` | debug exports `ogre_guest_rdram(len)` and `ogre_debug_threads()` (call chains, running queue, scheduler ring) |
| `app/CMakeLists.txt` | export the two new functions |
| `docs/proofs/web-tutorial-combat-session202.png` | the fixed tutorial battle |
| `docs/proofs/web-tutorial-combat-green-session202.png` | the developer's report, before the fix |
| `docs/proofs/web-tutorial-story-session202.png` | the `0x000D` story frame after the fix |
| `docs/HANDOFF-2026-10-26-session202.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/guides/web-probes.md`, `docs/scenes.md` | status, log, decision, index, knobs and the browser freeze recipe |

No `tools/RT64` file was edited; `m tools/RT64` is the pre-existing dirt.

## Probes used and reverted

`probe202` in `RecompiledFuncs/funcs_0.c` printed the `free()` argument and
return address for the njpeg driver's call sites; it is removed by
`make recomp`, and the grep above is empty. The kept diagnostics are the
`[GFX-FBWRITE-STALE]` line and the two `main_web.cpp` exports.

## Open

1. **The exact `3. Combat` lesson.** The right-hand lesson list is scrolled with
   C-Up/C-Down; `5. Unit Commands` also contains a battle, which the fixed build
   renders. The `3. Combat` item is two steps above it in the list; drive it and
   capture the same pair.
2. **The writeback guard hashes the whole target.** A framebuffer whose extra
   VI-height rows changed is skipped whole, and its drawn rows are not written
   either. The reached screens are correct; a per-row or drawn-range hash would
   narrow it if a screen is found where the target's first rows are still the
   GPU's and a later row is the heap.
3. **`testdraw`'s positive control is above RDRAM.** Move its scratch to a
   guest address below 8 MiB so `fbtex hits=1` is measurable on a fresh
   generated build.
4. **The njpeg backdrop's raw green/magenta display is pre-existing.** It is the
   same on unmodified `HEAD` at the cathedral; the session-138 proof is an older
   build. The next measurement is the S2DEX2 YUV draw itself: whether the shader
   samples the tile as YUV16 or as raw RGBA16, and which change after session 138
   moved it. The combat freeze is fixed; this backdrop colour is not.
