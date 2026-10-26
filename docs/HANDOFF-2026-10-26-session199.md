# Session 199 handoff — the mission's frame cost is outside the flush loop, and the practice field's ground is a fill

## Goal

Fix the two defects the developer sees on the mission/tutorial screens: the flat
grass on the tutorial practice stage (Jangurri), and the low frame rate on the
mission screen.

## Result

1. **The mission is at 29.4-29.8 fps with the session-198 uniform cache, and the
   remaining cost is not the flush loop.** A draw merge (consecutive draws with
   identical uniform state emitted as one `glDrawArrays`) reduced the mission's
   draw calls from 1 410 777 to 164 198 (8.6 draws per call) and changed the
   frame rate by nothing: **29.39 fps with the merge off, 29.83 fps with it on**
   over the same 10 s window, same save and route. A CDP CPU profile of the
   mission with the merge on spent **87.1% of the main thread idle**; the largest
   non-idle entries were `(program)` 2.8%, `wasm-to-js` 1.5%,
   `ogre_gfx_flush` 1.4%, the uniform-cache hash lookup 1.1% and `uniform1i`
   0.8%. The merge was reverted: it is a large change with no measured gain.

   The uniform cache is session 198's change and is in the tree uncommitted
   (`uniform_cache_`, `uloc()`, `OGRE_GL_CHECK`). The final binary (that change
   only, no merge) measures **28.66 fps** at scene `0x0003` reached at 14.7 s, so
   the tree already carries the fast build. The mission's remaining 2x against
   native RT64 is therefore outside the main thread (the recompiled game/RSP
   threads or the GPU). The caveat on "2x against native": the counter used here
   is the game's submitted display lists per second, while RT64 presents per VI
   field, so session 176's 60 Hz native present series does not establish that
   retail renders this mission at 60. The next measurements are a per-thread time
   split and the native display-list rate, not more flush-loop work.

2. **The practice field's ground is a flat `G_FILLRECT`, and the textured ground
   tiles do not cover the grass.** At scene `0x0003`, 12 s after entry, a pixel
   probe at N64 `(12,160)` (grass, screen `(24,320)` in the 640x480 capture)
   logged **five covering draws, all `kind=1` full-screen fills**, the last two
   `prim=(0.13,0.13,0.00)` then `(0.13,0.16,0.00)`. The 640x480 frame samples
   `(34,42,2)` there, which is `(0.13,0.16,0.00) * 255 = (33,41,0)` within
   rounding. No triangle or rectangle in the sampled frames covers that pixel
   with a texture.

   The field's textured ground family is present in the same frames:
   `cmb=FC127E24/FFFFF9FC` = `TEXEL0 * SHADE`, 32x64 CI8 tiles
   (`0x2FB540`, `0x2FE5E0`, `0x2FBDF8`, `0x2FE840`, `0x2FE6B0`, `0x2FF1E8`,
   `0x2FFA80`, `0x2CBB30`, `0x28BEE0`), and it covers the cliffs and roads. The
   tutorial box, the panel and the `MISSION` band are drawn as rectangles. The
   ground under the visible grass is never covered.

   The other field layer is a whole-field overlay
   (`cmb=FCFF99FF/FF0DFE3F` = `PRIM`, alpha `TEXEL0_A * SHADE_A`), a 64x64 CI8
   tile at `0x37E5A8` decoded alpha mean 0.30, `PRIM=(0.42,0.39,0.36)`,
   `PRIM_A=0.03` in the captured frame. Its combiner output colour is `PRIM`, so
   it cannot supply a green textured ground either. Its texels are what
   `TEXEL0` raw (debug flag 16) shows at the grass in the party-menu lesson
   (grey mottle); a capture of the "Movement & Liberation" lesson shows `TEXEL0`
   raw **pure white** at N64 `(12,160)` with the same flat `(33,33,0)` normal
   pixel, i.e. that lesson has no textured draw over the grass at all. The
   tutorial's lesson advances with A without a scene change, which is why the two
   captures of scene `0x0003` differ in which overlay covers the grass; the flat
   fill is constant.

   The ground *mesh* family that carries the green looks like
   `cmb=FC151604/2F1125FF` = `(TEXEL0 - TEXEL1) * TEXEL1_A + TEXEL1` then
   `COMBINED * SHADE`, with `TEXEL0` a 32x32 RGBA16 tile at `0x358F00`, `TEXEL1`
   a 32x32 tile with alpha 1.0, `masks=32`, `shifts=13`. Its draws exist but land
   at the top-left of the screen and off it (bounding boxes `x -201..37`,
   `y -45..10` in a sampled frame), not on the visible grass.

   Ruled out this session, each changing no pixel at the grass:
   - `OGRE_DEPTH=0` (the GL depth test off): `(34,42,1)` before and `(36,43,4)`
     after.
   - Backface culling disabled (debug bit 19, added for the test and reverted):
     `(36,43,4)`.
   - The matrices are valid and shared: grass family modelview scale `0.01`,
     translation `(14.87, 0.01, 11.32)`; cliff family scale `0.01`, translation
     `(-14.40, 0.00, -15.63)`; both use projection
     `[2.14 0 0 0 | 0 0.64 -1.97 -1.95 | 0 -2.78 -0.46 -0.45 | -19.85 -16.89 11.65 15.48]`
     with viewport scale `160/120`, translate `159.5/119.5`, `vpset=1`.
   - The ground tiles' vertex positions are the game's: the bboxes above are
     computed from the transformed NDC positions, not from a guess.

   The defect is therefore upstream of the combiner and the sampler: the draw
   list the browser walks does not cover the visible ground with the ground
   mesh. The next step is to compare the field's display-list walk with RT64's
   on the same list, and to ask the developer which of the field's layers is
   meant to be the grass (the tutorial box says "These are called the Field").

## Evidence

- **Mission fps, one build and route** (final binary), `OGRE_SAVE=assets/escort_suspend.bin`,
  `OGRE_SCENE=title`, `title:start:6,0x12:a:6`: scene `0x0003` at 14.7 s,
  `frames 824 -> 1112 in 10.0 s = 28.66 fps`.
- **Mission fps, merge A/B**, `OGRE_SAVE=assets/escort_suspend.bin`,
  `OGRE_SCENE=title`, `title:start:6,0x12:a:6`; scene `0x0003` at 40.8 s on both
  runs. Merge on `/tmp/w199/mission-log-on.txt`: `frames 1558 -> 1857 in 10.0 s =
  29.83 fps`; merge off (`OGRE_BATCH=0`) `/tmp/w199/mission-log-off.txt`:
  `frames 1574 -> 1869 in 10.0 s = 29.39 fps`. The on-run's census reads
  `total_draws=1410777 total_batches=164198`.
- **Mission profile**, `/tmp/w199/prof/profile.cpuprofile` (4 s, sampling 200 us):
  87.1% idle; see the percentages above.
- **Mission frames**: `docs/proofs/web-mission-fps-session199.png`. The merge-on
  and merge-off frames are identical outside the scrolling `MISSION` band
  (mean absolute difference 0.0-0.8 per 80x60 block above and below it; 10.6-39.0
  inside it).
- **Grass pixel probe**, `OGRE_DRAW_PROBE=12,160`,
  `OGRE_DRAW_PROBE_SCENE=0x3`, `OGRE_DRAW_PROBE_SKIP_FLUSHES=700`,
  `/tmp/w199/log-allk.txt`. Five lines, all `k=1` full-screen fills:
  - `prim=1.00,1.00,0.97,0.00` (white, alpha 0)
  - `prim=0.00,0.00,0.00,1.00` (black)
  - `prim=1.00,1.00,0.97,0.00`
  - `prim=0.13,0.13,0.00,1.00`
  - `prim=0.13,0.16,0.00,1.00`
- **Full-field draw families**, one 361-draw flush (`/tmp/w199/log-full1.txt`) and
  an eight-flush capture (`/tmp/w199/log-frame8.txt`): the nine 32x64 CI8 ground
  textures above, 100+41+35+24+24+16+15 draws for the first seven; the
  `FC151604` ground mesh family has 9 draws in that flush at `x -6..37`,
  `y -29..10`.
- **TEXEL0 raw at the field**: `docs/proofs/web-tutorial-grass-texel0-session199.png`;
  the normal frame `docs/proofs/web-tutorial-grass-flat-session199.png`.
- **No-depth A/B**: `/tmp/w199/log-nodepth.txt`, samples
  `normal (34,42,1) ... nocull (36,43,4)`.

## Verification

- The two field captures and the mission frame now run through one command:
  `node debug/probes/scene.cjs --route tutorial-field --raw --points '12,160' --out /tmp/field`
  and `--route mission-briefing` (see `docs/guides/web-probes.md` → "Capturing a
  screen").
- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final source
  (the tree carries only session 198's uniform-cache change).
- The final binary (session 198's cache only) is the one measured at 28.66 fps
  above; `debug/probes/boot.cjs --attempts 3 --secs 60` reports `RENDERED`
  (`nonBlack=307840`).
- The field proofs were re-captured on the final binary with
  `debug/probes/scene.cjs` (session 200); the earlier captures came from a build
  that also carried the probe logging, whose merge A/B changes no pixel outside
  the scrolling `MISSION` band.
- The mission on/off A/B is the fps evidence above; both runs reached the scene
  and submitted frames.
- Every probe added this session (`OGRE_DRAW_PROBE*`, the flush-loop merge,
  `same_draw_uniforms`, `OGRE_BATCH`, debug bit 19, debug mode 3, `GFX-MTX2`,
  `GFX-BATCH`, the `LoadedImage`/`DrawCmd` probe fields) is removed;
  `grep -n "probe199\|GFX-DRAWPROBE\|OGRE_BATCH\|same_draw_uniforms\|GFX-MTX2\|GFX-BATCH" app/src/web_renderer.cpp`
  is empty. `git diff app/src/web_renderer.cpp` shows only the session-198
  uniform cache.

## Files changed

| file | change |
|---|---|
| `docs/HANDOFF-2026-10-26-session199.md` | this file |
| `docs/proofs/web-tutorial-grass-flat-session199.png`, `-texel0-session199.png` | the normal and TEXEL0-raw field frames |
| `docs/proofs/web-mission-fps-session199.png` | the mission frame from the final binary (28.66 fps) |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md` | status, log, the batching decision and the index |

No application source changed in this session; `app/src/web_renderer.cpp` keeps
session 198's uncommitted uniform cache.

## Probes used and reverted

As listed under Verification. The probe scripts live in `/tmp/w199` and are not
part of the tree.

## Open

1. **The practice field's ground.** The visible grass is a `G_FILLRECT`
   (`0.13,0.16,0.00`); the ground mesh (`FC151604`, `0x358F00`) draws at the
   top-left and off-screen, and the textured cliff family covers only the
   cliffs and roads. Compare the field's display-list walk against RT64 on the
   same list; the developer can say which layer is the grass.
2. **The mission's remaining frame cost.** The main thread is 87% idle and the
   draw merge changes nothing. Split the time between the recompiled game/RSP
   threads and the GPU (Metal via ANGLE), and measure the native *display-list*
   rate (RT64's present rate is per VI field) before treating 29-30/s as a
   regression.
