# Session 167 handoff — RT64's half-pixel screen offset is the tutorial box's one-texel difference, and the triangle path now applies it

## Goal

Close session 166's open item 1: the tutorial scene `0x17` dialogue box's texel
row was one above the native target's on 24 of 24 rows of 172..195, with no
mechanism named. Session 166's hypothesis for this round was RT64's
`halfPixelOffset` in `tools/RT64/src/render/rt64_framebuffer_renderer.cpp`, which
the browser's `transform_vertex` did not apply.

Method: reproduce the half-pixel arithmetic from session 166's own probe dump
before any code change; add the offset to the triangle path only; rebuild and
measure the same box, the same regions and the same native target; then the
regression battery and the title and world-map screens.

Timebox: 100 minutes.

## Result

1. **The arithmetic reproduces the measured pattern, and it fixes the sign of
   the offset.** Session 166's probe dump (`/tmp/w166/p166-h-p166.txt`) gives
   the browser's interpolated `v` at column 285 and the decoded 64x64 CI4 image:
   `v(170) = 0.5025`, `v(190) = 20.1227`, step `0.98077` texel per pixel. A
   half-pixel offset moves the quad down 0.5 pixel, so the coordinate at a fixed
   pixel centre becomes `v - 0.5 * 0.98077 = v - 0.49038` texels. At that shift
   `floor(v)` changes on **24/24** rows of 172..195 and on **1/23** rows of
   196..218 (row 196, where the fractional part is 0.008); the other 22 rows of
   196..218 keep their texel. This round's brief expected 0/23
   for 196..218; the corrected count is 1/23, and the native target at row 196
   samples the shifted row, so that row agrees with the mechanism rather than
   excepting it. The opposite sign (`v + 0.49038`) changes **0/24** rows of
   172..195, so the data fixes the direction: the quad has to move **down**.
   The measured texel-row match over the same range is 24/24 at `k = -1`, 3/24
   at `k = 0`, 4/24 at `k = -2` and 5/24 at `k = +1`.
   Script: `/tmp/w167/arith.py`.

2. **RT64 applies half a pixel in `x` and `y` to every draw's vertices, and for
   a triangle that is a half-pixel shift of the geometry; the browser did not
   apply it at all.** `rt64_framebuffer_renderer.cpp:1528` builds
   `halfPixelOffset = { 1.0f / framebuffer.viewport.width, -1.0f /
   framebuffer.viewport.height }`, lines 1571-1572 assign it as
   `triangles.screenScale = {1,1}` / `triangles.screenOffset = halfPixelOffset`
   for the draw call, and `RasterVS.hlsl:25` applies it as
   `ndcPos.xy = (ndcPos.xy * gConstants.screenScale) + gConstants.screenOffset *
   ndcPos.w;`. The default scale is 1 and the offset is modified only for the
   stretched/origin viewport cases (lines 1592 and 1769-1770). In window space
   that is +0.5 pixel in x and +0.5 pixel in y for a 320x240 target, i.e. +1/W in
   NDC x and -1/H in NDC y under the browser's `ndc_xf`/`ndc_yf`
   (`2x/W - 1`, `1 - 2y/H`).

3. **The change is two lines in `transform_vertex`, on the triangle path only.**
   `sx += 1.0f / screen_width(st); sy -= 1.0f / screen_height(st);` before
   `out[0] = sx; out[1] = sy;`, with the RT64 file and line references in the
   comment above them. `transform_vertex` has one caller, `draw_tri`, so the
   texrect, S2DEX rectangle and fill paths are untouched.

4. **The tutorial box now matches the native target, not only on the parchment
   but on the glyphs and the portrait.** Canvas aligned at `(0,0)` on the box
   frame against `/tmp/w166/nat17/t.335.bin` (the session-166 target), mean abs
   per channel, before -> after:
   - frame rows 166..170: `0.33 -> 0.29`; the 31 pixels that changed in that
     band are backdrop columns outside the border (0..20 and 297..312), so the
     border strips, which are texrects, did not move,
   - portrait 175..215 x30..75: `1.10 -> 0.00`,
   - parchment rows 172..195: `10.49 -> 0.25`,
   - lower rows 196..218: `2.01 -> 0.17` (`2.12 -> 0.18` with the blinking
     cursor's columns 276..291 excluded),
   - box whole rows 166..219 x16..304: `5.88 -> 0.22`, and rows 166..222:
     `5.68 -> 0.21`,
   - the text as its own regions: the name line (174..191 x88..200)
     `13.32 -> 0.00` and the body (191..218 x88..276) `4.01 -> 0.00`.
   The after numbers are frame `tutb-01` of `/tmp/w167/after/tutb`; `tutb-02` is
   identical and `tutb-00` reads `0.64` over the whole box because its cursor
   was in the other blink phase. Session 165's "5.43 over the box" is the same
   measurement over a slightly different rectangle; on this frame the region
   rows 166..219 x16..304 is 5.88 before and 5.68 for rows 166..222.

5. **The texel-row test passes at every column tested, and the sampled row is now
   the native's.** Against `/tmp/w166/nat17/t.335.bin` the native pixel at rows
   172..195 equals the browser canvas at **24/24** rows for columns 100, 150, 200
   and 285 after the change; before it, the same test on the session-166 column
   285 was 3/24, and columns 100 and 150 were 12/24 and 3/24. Read the other way,
   with session 166's probe ladder `v(y)` and its decoded image: before the
   change the canvas at column 285 equalled `img[floor(v) + 0][7]` on **24/24**
   rows and `img[floor(v) - 1][7]` on 3/24; after it the canvas equals
   `img[floor(v) - 1][7]` on **24/24** rows and `img[floor(v) + 0][7]` on 3/24,
   and the native equals the canvas on 24/24. So the browser's sample moved by
   exactly one row, which is the row RT64 was already reading. Rows 196..203 are
   8/8 at every column after the change (before: 8/8 at 200 and 285, 5/8 at 100,
   1/8 at 150). The text and the portrait are drawn by the same triangle path,
   which is why their residuals went to zero in the same change.

6. **The backdrop moves too, as RT64's rule requires, and it moves toward the
   native.** The tutorial's tiling 64x64 backdrop is twelve triangle draws
   (session 166), so it also shifts by half a pixel. Against the same native
   target, backdrop rows 0..160 read `9.52` before and `6.21` after. That
   comparison is confounded: session 166 recorded that the native's backdrop is
   at a different scroll phase, so this number is not a clean attribution. The
   direction is the RT64 one; the size is not measured cleanly.

7. **The title is unchanged.** The title animates (the cloud scroll), so a
   before/after pixel diff of two runs is dominated by the phase; instead each
   browser frame was matched against the stored native title series
   (`/tmp/w162/nat-fog`, 397 frames). The best match is `t.228` at `meanAbs 0.20`
   before and `t.313`/`t.314` at `0.18` after: the same residual at a different
   animation instant, so no draw in the title moved measurably. The title's
   logged draws are 6-vertex rectangles (`[GFX-V]` draws 1..6, `verts=6`), and
   its `[GFX-SAMPLER]` census reports `persp=0`, i.e. no textured triangle with a
   `w` difference.

8. **The world map still renders; its frame moved only where a triangle is
   drawn.** The pre-change map series was discarded first: that run never drew
   the map (its shots are black, `nonBlack` 25044/25388/42700). The numbers below
   come from a rebuilt A/B: the two added lines were commented out, `make web`
   was run, the map captured, the lines restored, `make web` run again and the
   map captured with the same `--secs 80 --shots 4 --gap 2500`. Both builds
   render the screen (`nonBlack=252820` in all four shots on each side), but the
   two runs sit at different fade levels (mean pixel 109.6 before, 56.8 after at
   the same wall time). After normalising the fade with one scale (`1.931`),
   **451 pixels** differ by more than 6 levels, and all of them lie in rows
   97..121 x columns 82..97: the unit sprite, which is a triangle quad (session 64's 14-part
   sprite). A whole-pixel shift search on the normalised frames with that sprite
   excluded minimises at `(0,0)` = `0.87`, every other shift `>= 9.56`, so the
   map did not move as a whole. Expected direction: +0.5 pixel right and down,
   which changes the sprite's sampled texels the way the tutorial's parchment
   changed; the single fade scale cannot establish the direction from this pair.

9. **No regression.** `make web` is clean. `boot.cjs --attempts 3 --secs 40` is
   `RENDERED` on attempt 1 (`nonBlack=24808`). `testdraw --flags 96
   --expect-fbtex --expect-hits 1` exits 0 with `fbtex hits=1 lines=1` and
   `nonBlack=3024` after the batch. The title and map numbers are above.

## Evidence

RT64's code, read for the reference:

- `tools/RT64/src/render/rt64_framebuffer_renderer.cpp:1528`:
  `const interop::float2 halfPixelOffset = { 1.0f / framebuffer.viewport.width,
  -1.0f / framebuffer.viewport.height };`.
- Same file, `1571-1572`: `triangles.screenScale = { 1.0f, 1.0f };
  triangles.screenOffset = halfPixelOffset;`. Line 1592 changes `screenScale.x`
  for a wide viewport; lines 1769-1770 change both for a rectangle projection
  (`screenOffset.x = halfPixelOffset.x + ((rect.x + rect.width/2) -
  halfViewportSize.x) / halfViewportSize.x`, and the `y` term with the opposite
  sign), which is the rect path.
- `tools/RT64/src/render/rt64_framebuffer_renderer.cpp:647`:
  `rasterParams.screenOffset = triangles.screenOffset;`, and
  `src/shared/rt64_raster_params.h:16`: the push constant.
- `tools/RT64/src/shaders/RasterVS.hlsl:16-25`: the `!renderFlagRect(rp.flags)`
  branch does `ndcPos.xy -= resolution/2; ndcPos.xy /= (res.x/2, res.y/-2);
  ndcPos.xyz *= ndcPos.w;`, then every draw does
  `ndcPos.xy = (ndcPos.xy * gConstants.screenScale) + gConstants.screenOffset *
  ndcPos.w;`.
- `tools/RT64/src/hle/rt64_rdp.cpp:1419-1560` `drawRect`: the rect's vertices are
  the fixed NDC quad `(-1,1)..(1,-1)` (lines 1555-1562), and the rect itself
  reaches the shader through `screenScale = viewportRect.size /
  framebuffer.viewport.size` plus the centre term above. Because the rect's
  edges and the offset move together, a rect on whole pixels covers the same
  pixels and samples the same texels; the browser's rect path already matches the
  target (`0.29` on the border rows) and was not changed.

Measured:

- The native target is `/tmp/w166/nat17/t.335.bin`, the last frame of session
  166's series (`t.101..t.335`). Session 166's prose names `t.338`, which is not
  in that directory; its own `final.py` read `t.335`, and this session's numbers
  use the same frame. Session 165's `/tmp/w165/nat17/t.338.bin` is the same
  screen: it differs from `t.335` in 14 pixels of the whole frame, all of them
  the blinking cursor at rows 211..216, so every box number below holds for it
  too.
- The half-pixel arithmetic and the sign test: `/tmp/w167/arith.py`, run on
  `/tmp/w166/p166-h-p166.txt` and `/tmp/w166/nat17/t.335.bin`.
- Browser before: `/tmp/w167/before/tut-{00,01,02}.png`
  (`?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1`, `shots.cjs --secs 90 --shots 3 --gap
  2000`), frame `tut-00` used; `tut-00` and `tut-01` are byte-identical and
  `tut-02` differs only in the blinking cursor (99 channel pixels, rows 204..218,
  columns 276..291).
- Browser after: `/tmp/w167/after/tutb-{00,01,02}.png`, same options, frame
  `tutb-01` used.
- The region numbers: `/tmp/w167/regions2.py` (the session-166 alignment: a
  319x239 canvas buffer, the shift that minimises the box frame band searched
  over 0..2, which is `(0,0)` at 3.43-5.97 before and 3.43 after).
- The texel-row test: `/tmp/w167/` inline script over the same two canvases and
  `/tmp/w166/nat17/t.335.bin`.
- Title: `/tmp/w167/before/title-*.png` and `/tmp/w167/after/title-*.png`
  (`?OGRE_SCENE=title`, `--secs 45 --shots 3 --gap 2000`; the after side needed
  four boot attempts), matched with `/tmp/w167/title_phase.py` against
  `/tmp/w162/nat-fog/t.*.bin`. `/tmp/w164/nat2` does not exist, so the reference
  is the session-162 native title series (the one session 156/161/162 compared
  against), not a session-164 target.
- Map: `/tmp/w167/ab/map-before-*.png` (the A/B build with the two lines
  commented out) and `/tmp/w167/ab/map-after-*.png`, `?OGRE_SCENE=0x05`,
  `--secs 80 --shots 4 --gap 2500`; the diff and shift search are inline in
  `/tmp/w167/`.
- The proof: `docs/proofs/web-tutorial-half-pixel-before-after.png` is the box
  region rows 160..232 x 10..310 at 2x, three panels top to bottom: browser
  before, browser after, native target.

## What changed

`app/src/web_renderer.cpp`'s `transform_vertex` adds RT64's half-pixel screen
offset to the NDC position of every triangle vertex: `sx += 1.0f /
screen_width(st)` and `sy -= 1.0f / screen_height(st)`, with a comment quoting
`rt64_framebuffer_renderer.cpp`'s `halfPixelOffset` assignment and
`RasterVS.hlsl`'s application, and stating why the rect path is left alone. The
two lines are the whole renderer change. The vertex's clip `w` (`out[8]`) is
untouched, so the shader still multiplies the NDC position by `w`; the offset is
an NDC quantity, which is the same thing as RT64's `screenOffset * ndcPos.w` in
clip space.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `transform_vertex`: +1/W NDC x, -1/H NDC y on the triangle path, with the RT64 references |
| `docs/proofs/web-tutorial-half-pixel-before-after.png` | browser before / browser after / native, box region rows 160..232, 2x |
| `docs/HANDOFF-2026-10-18-session167.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session entry, open item 8(r) closed, the half-pixel decision, session 166's "mechanism unnamed" corrected |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean three times: the
  fixed build, the A/B build with the two lines commented out, and the fixed
  build again. The last build reproduces the wasm the checks below ran on
  (`md5 a7119e28bd09f19f31ad706b86c65c37`), so the A/B build did not move the
  tested binary.
- `node debug/probes/boot.cjs --attempts 3 --secs 40` — `RENDERED` on attempt 1,
  `nonBlack=24808`.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`, after-batch `nonBlack=3024`,
  `[GFX-FBTEX] load tex=0xC02000 rect=32x32 ... rows=[0,31] drawn rows=[0,239]`.
- Tutorial: `OGRE_URL='…?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1' node
  debug/probes/shots.cjs --secs 90 --shots 3 --gap 2000 --out /tmp/w167/…`, once
  before the change and twice after it. The first after-run landed on another
  tutorial moment (party sprites on black, `nonBlack=25048`), so it was
  discarded and re-run. Both runs had to be retried for the boot trajectory,
  which is the known idle-boot lottery, not a renderer effect.
- Title: `shots.cjs --secs 45 --shots 3 --gap 2000 --out /tmp/w167/…/title` on
  `?OGRE_SCENE=title` before and after.
- World map: `shots.cjs --secs 80 --shots 4 --gap 2500 --out /tmp/w167/ab/map-…`
  on `?OGRE_SCENE=0x05` on the A/B build and on the fixed build.
- `grep -rn "S167AB" app/ debug/` and `grep -rn "probe167\|OGRE_P167" app/ debug/`
  — both empty (the A/B was a temporary comment, restored and rebuilt).

There is no repo test harness; each check is a ROM-dependent browser run. The
machine's load average was 10-31 during these runs (several headless Chromium
instances at a time is enough to do it), so the boot sometimes took the idle
trajectory and the runs were serialised.

## Open

1. **The backdrop's half-pixel effect is not measured cleanly.** Its rows 0..160
   read `9.52 -> 6.21` against the native, but the native's backdrop is at a
   different scroll phase (session 166), so that number is evidence that the
   change did not make it worse, not a measurement of the shift. A screen whose
   triangle backdrop is static and whose native capture is at a known phase would
   settle it.
2. **The tutorial box's residual after the change is 0.22 over the whole box.**
   The parchment rows are `0.25`, the frame `0.29`; the remaining differences are
   single-level and scattered, and no mechanism is claimed for them. The text and
   the portrait are exact, so the old 5.4-mean item is closed.
3. **The world map's fade level differs between the two A/B runs** (mean 109.6
   against 56.8 at the same wall time), which is why the map comparison needed a
   single scale. The map's fade timing is not this session's subject and is not
   measured; a future map comparison should either match the fade or capture
   until it saturates.
4. **The title's two moving-cloud bands** (session 164/165 items) are untouched.
   This session measured only that the title frame did not move.
5. **`OGRE_TEXF=gl`** still cannot express the tile window (session 165 item 3),
   and the session-164 items (the one-instant-per-capture presenter,
   `OGRE_FOG_SCALE`, the screens not compared against the reference) remain.
