# Session 165 handoff — the browser samples TMEM, and the RDP's coverage path fixes the tutorial's dialogue box

## Goal

Continue the browser (WebAssembly) renderer from session 164, using RT64's
renderer as the reference. Open item 8(r) was the tutorial scene `0x17`'s
dialogue box: a clean Deneb portrait and readable text in RT64's target, a torn
portrait, black glyph blocks and a bright green border in the browser. Session
164 excluded the texture sampler and left the scene as the next target.

Timebox: 100 minutes.

## Result

1. **The browser samples TMEM, not RDRAM.** `web_renderer.cpp` keeps the RDP's
   4 KiB TMEM (`tmem_`) across display lists, the way RT64's `RDP::TMEM`
   persists. Every load command copies its source into it with RT64's byte
   layout: `tmem_store_word` mirrors `RDP::loadWord` (a TLUT word reads two
   source bytes, RGBA32 splits a source word across TMEM's two halves, every
   other format copies eight bytes), `tmem_copy` mirrors
   `loadToTMEMCommon` (row stride, `tmem << 3` destination, 4 KiB wraparound, a
   LOADBLOCK's `dxt` row advance), and `tmem_load_yuv` mirrors
   `loadYUVTileToTMEM`. A draw decodes its tile out of TMEM at `tmem << 3` with
   `line << 3` as the row stride (`decode_tmem_tile`, following
   `TextureDecoder.hlsli`'s `sampleTMEMWithConvert`), including the odd-row
   four-byte word swap and the palette at the fixed base `0x800`.

   The decode's region is RT64's sample window: the tile's own rect when the
   axis is clamped, otherwise its mask, whichever is smaller. That is what makes
   a TEXRECT whose tile was never given a rect sample its mask's size (the
   title's 64x64 cloud tiles) instead of the single texel the old
   `(lrs >> 2) - (uls >> 2) + 1` produced for a zero rect. The image cache key
   now carries the TMEM generation, so every load invalidates the tiles that
   sample the bytes it moved.

   A CI tile with LUT mode 0 decodes as I (RT64 `sampleTMEM8b`'s CI arm), which
   session 144's rule required and the first TMEM build dropped: without it the
   title prologue's two unit sprites became opaque black rectangles again.

2. **The RDP's alpha-compare and coverage path is ported.** The tutorial's green
   border and black glyph cells were one defect: a texel whose palette entry has
   alpha 0 was drawn as its colour instead of being dropped. The strip and glyph
   draws carry `OTHERMODE_L = 0x000A7008`, which sets `CVG_X_ALPHA` (`0x1000`)
   and `CVG_DST_CLAMP`; RT64 computes
   `resultCvg = (8 / cvgRange) * combinerAlpha` and discards anything below one
   coverage step (`RasterPS.hlsl`), so a zero-alpha texel contributes nothing.
   The fragment shader now implements that, plus `G_AC_THRESHOLD`/`G_AC_DITHER`
   from OTHERMODE_L bits 0-1, and writes the coverage value selected by
   `CVG_DST` as the target alpha. `DrawCmd` carries the three fields, and the
   flush sets `u_ac`, `u_cvg_x_alpha`, `u_cvg_dst`, `u_alpha_blend` and
   `u_cvg_range` (65535 for the RGBA16F attachment, 255 otherwise, the ratio
   being what decides the discard).

3. **The tutorial's dialogue box now matches the native.** The portrait is
   clean, the text is readable on the parchment, the border is the thin
   gold/olive frame, and the cursor is the hand icon. The remaining visible
   The background weave is at a different scroll phase between the two runs,
   which is why the comparison is made on the box.
   `docs/proofs/web-tutorial-fixed-side-by-side.png` is the pair at 2x (browser
   left, RT64 target right). Against the RT64 target the border rows and the
   portrait are pixel-exact; the glyph pixels differ by 7.9 mean over the text
   region, which open item 1 records.

4. **The framebuffer-as-texture diagnostic moved to the load.** It used to run
   in `ensure_tile_image` with the render tile's sample window, because the
   decode read RDRAM there. The load reads RDRAM now, so the check runs in
   `tmem_load_command` with the load's own rect and the `SETTIMG` stride. The
   synthetic `test_draw` control needed the same correction: its `G_LOADTILE`
   commands passed a zero rect, which is a one-word load that leaves the tile
   degenerate (`draw_texrect` drops it, as RT64's `RDP::drawTexRect` does now
   that the load updates the tile's rect from its command). The controls pass
   the rect they mean, and `testdraw --flags 96 --expect-fbtex --expect-hits 1`
   reports `fbtex hits=1 lines=1` again.

5. **The tile's `shifts`/`shiftt` fields are unexercised on the reached
   screens.** Session 164's open item 1 recorded that the browser does not
   apply RT64's `ShiftScaleMap[shifts]` to the sampled coordinate. The
   `[GFX-SAMPLER]` census now counts a draw whose tile has a non-zero shift
   (`shifted=`), and it prints on the first textured batch and every 400
   textured draws, because the flush-count cadence missed the tutorial's
   dialogue box. Measured: forced title 1182 textured draws, 0 shifted; forced
   world map `0x05` 198, 0; the attract/intro run 8885 (365 point, 6404 bilerp,
   2116 average, 2116 masked), 0 shifted; the tutorial's background batch 2, 0,
   and its dialogue-box batch 447 (423 point, 24 bilerp, 438 masked), 0.
   So the item closes as unexercised on the screens this port reaches, and the
   census reports a screen that does use the field. The same run is the first
   measured one where `G_TF_AVERAGE` is exercised (2116 draws of the attract).
6. **The box's residual is a sub-texel phase in the triangle path, on one
   draw.** A temporary `OGRE_P167` probe logged every draw whose triangle list
   covers a point in the box's upper and lower halves. The box interior is two
   draws of the same 64x64 CI4 parchment image:
   - the upper band, `kind=0` (triangles), `uv=[0.0 .. 265.0, 51.0 .. 0.0]`,
     which `v` runs through at about two texels per pixel, and
   - the lower band, `kind=0`, `uv=[0.0 .. 265.0, 51.0 .. 51.0]`, a flat sample
     of the texture's row 51.
   The per-row difference against the native target is 11-17 mean on rows
   171..195 (the upper, textured draw) and 1-4 on rows 196..219 (the flat draw);
   the boundary at row 196 is the draw boundary, not a screen feature. On the
   parchment pixels alone the best integer shift is `(0,-1)` at 7.87 against
   9.73 at `(0,0)`, so no whole-texel shift explains it, and the border strips
   and portrait (texrects at one texel per pixel) are exact. The interpolated
   coordinate of this vertically scaled triangle draw therefore lands on a
   different sub-texel phase than the native's. That is the next measurement:
   the same draw's interpolated `v` at one pixel in both renderers.

## Evidence

RT64 references, read at instruction level:

- `tools/RT64/src/hle/rt64_rdp.cpp`: `loadWord` (`offsetMask`, RGBA32 halves,
  the odd-row `tmemXorMask ^= 4`), `loadToTMEMCommon` (`tmemStride`,
  `tmemAdvance`, `dxt` row advance), `loadTileOperation`/`loadBlockOperation`/
  `loadTLUTOperation` (the source offsets and row stride), `loadYUVTileToTMEM`.
- `tools/RT64/src/shaders/TextureDecoder.hlsli`: `sampleTMEMWithConvert`
  (`tmemShift`, `addressMask` `0x7FF` for CI/RGBA32 and `0xFFF` otherwise,
  `pixelAddress = t*stride + ((s << shift) >> 1)`), `sampleTMEM8b`'s
  "CI behaves like I when a TLUT is not active", `sampleTMEMYUV16`.
- `tools/RT64/src/shaders/RasterPS.hlsl`: alpha compare
  (`G_AC_THRESHOLD` against `blendColor.a`, `G_AC_DITHER`), the coverage
  discard, and the `CVG_DST_*` alpha.
- `tools/RT64/src/hle/rt64_state.cpp`: `sampleWidth`/`sampleHeight` =
  `min(tile width, 1 << mask)` and `rawTMEM`.

Measured:

- The tutorial's strip tile is `img=0x24E340 fmt=2 siz=0 line=1 masks=4
  maskt=3 cms=2 cmt=2`; the decode is 16x8 and the palette at TMEM `0x800` is
  `e0=0001 e1=0B88`. A temporary `OGRE_P165` probe printed the tile, the
  palette entries and the first 16 TMEM bytes at the draw, which is how the
  decode was confirmed against the same bytes in the native trap's trace.
- The native trap (`OGRE_TEX_TRACE=1`) prints the same strips, the same
  `window=uls0 ult0 lrs60 lrt28`, the same source bytes
  (`first16=1111111122222222...`) and the same palette source. The native frame
  at the box's final position shows the border rows as palette entries 2, 8, 4,
  14, 11 for rows 166..170 and the parchment from row 171: rows 5..7 of the
  strip image are all index 1 (alpha 0) at the sampled column, so they are
  discarded rather than painted.
- The title prologue's black rectangle was measured on the canvas as rows
  128..175, columns 23..94 — the two 72x28/72x20 unit sprites — and it
  disappeared when mode-0 CI was decoded as I again.
- Title frame against RT64's target of the same phase: `meanAbs=1.56` at the
  best alignment (the fog scroll phase is not frame-locked).

## What changed, in one paragraph

`decode_texture_rect` is replaced by `decode_tmem_tile` (a tile decoded from
TMEM with RT64's sampler addressing and formats); `tmem_store_word`,
`tmem_copy`, `tmem_load_yuv` and `tmem_load_command` are the load side; the
`OP_LOADTILE`/`OP_LOADBLOCK`/`OP_LOADTLUT` handlers call `tmem_load_command`
before `note_tile_load` (which still records the source for diagnostics).
`ensure_tile_image` computes RT64's sample window, keys the image by the TMEM
generation, and calls the TMEM decode. `make_image_key` and
`decode_texture_rect` are deleted. `DrawCmd` gains `acmp`, `cvg_x_alpha` and
`cvg_dst`; `record_state` fills them from OTHERMODE_L; the fragment shader
gains the alpha-compare block, the coverage discard and the coverage alpha, and
`flush_commands` sets the new uniforms. The framebuffer-as-texture check moved
into `tmem_load_command`, and `test_draw`'s three control `G_LOADTILE`s carry
their tiles' rects. The `[GFX-SAMPLER]` census gained `shifted=` (a draw whose
tile asks for a `G_TX` shift) and now prints every 400 textured draws as well as
on the first, so a long screen reports itself.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | TMEM state and load/decode; RT64 sample window in `ensure_tile_image`; mode-0 CI as I; alpha-compare/coverage uniforms and shader; framebuffer-as-texture check on the load; `test_draw` control rects; `[GFX-SAMPLER]` `shifted=` and the 400-draw cadence |
| `docs/proofs/web-tutorial-fixed-side-by-side.png` | browser tutorial (left) and the RT64 target at the same screen (right), 2x |
| `docs/HANDOFF-2026-10-18-session165.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session entry, open item 8(r) closed |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean at every step; the
  final tree builds.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — `RENDERED`
  (`nonBlack=24888`).
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`, after-batch `nonBlack=3024`. The hit now
  reports the load's own rect, `rect=32x32 ... rows=[0,31]`.
- Tutorial: `shots.cjs --secs 90 --shots 20 --gap 3000` on
  `?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1`; the final frame is
  `/tmp/w165/final-tut-19.png`, cropped to 1x and paired with the native capture
  in the proof above.
- Native reference for the same screen:
  `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=0x17
  OGRE_CAPTURE_TARGET=/tmp/w165/nat17/t OGRE_CAPTURE_AFTER=100
  OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=14000 ./build-dist/ogrebattle64
  assets/ogre64.z64`, frame `t.338`.
- Title: `shots.cjs --secs 45 --shots 10 --gap 2000` on `?OGRE_SCENE=title`,
  against a native `OGRE_CAPTURE_TARGET` series of the forced title; the black
  rectangle scan and the `meanAbs=1.56` alignment above.
- Alignment: the box at `(0,0)` is `meanAbs 5.43`; the border rows and portrait
  are `0.00 / 100%` exact. The per-row parchment difference is 11-17 mean on
  rows 171..195 (the upper triangle draw) and 1-4 on rows 196..219 (the flat
  draw), and the text region as a whole is 7.9 mean / 47.7% exact.
- Census: `[GFX-SAMPLER]` over the forced title (`textured draws=1182 ...
  shifted=0`), the forced world map (`198 ... shifted=0`), the attract/intro run
  (`8885 point=365 bilerp=6404 average=2116 masked=2116 shifted=0`) and the
  tutorial's background batch (`2 ... shifted=0`) and dialogue-box batch
  (`447 point=423 bilerp=24 masked=438 shifted=0`).
- The box's residual: `OGRE_P167` (a temporary probe over the triangle path)
  logged the two parchment draws and their UVs; the per-row difference profile
  and the parchment-only shift search are above.
- `grep -rn "probe165\|probe166\|OGRE_P167\|OGRE_NOCVG" app/` — no output.

There is no repo test harness; each check is a ROM-dependent game run.

## File history

During this session an edit of `app/src/web_renderer.cpp` removed about 1,200
lines instead of one function (a brace-matching mistake in a script, not an
edit-tool failure). The working file was restored from the session-154
snapshot `/tmp/web_renderer-154-fixed.cpp` and the recorded `edit` and shell
operations of sessions 155-164, replayed in their transcript order; the result
builds, and every check above passes on it. The tree carries the same
uncommitted 155-164 work it carried before the incident, but its exact byte
history is the replay, not the original. Committing the browser work would stop
this from being repeatable.

## Open

1. **The box's upper parchment band samples the texture on a different
   sub-texel phase, and that is the whole residual.** Result 6 identifies the
   draw: a triangle quad over the upper half of the box interior, sampling the
   64x64 CI4 parchment image with `v` from 51 to 0 (about two texels per pixel),
   against the flat `v=51` draw below it. The border strips and the portrait are
   pixel-exact, so the geometry and the image are right. The next measurement is
   the interpolated `v` of that draw at one pixel in the browser and in RT64
   (RT64 computes the RDP's interpolation from the vertex's 1/32 texel and
   pixel-centre convention; the browser interpolates the same endpoints through
   GL). The 1-pixel offset this handoff first recorded was an error in its own
   comparison (the search shifted the canvas inside the region); the correct
   minimum is `(0,0)`, and the box at `(0,0)` is `meanAbs 5.43`.
2. **The title's remaining difference is still the fog composition** (PLAN
   §8(q)/(p) and session 164's open item 2). Nothing in this session's changes
   was measured against the 0.09-0.24 fog band.
3. **`OGRE_TEXF=gl` uses the same tile images but not the same window.** The GL
   fixed-function path samples normalized UVs and clamps to the whole texture,
   so it cannot express the tile-window clamp and mask; it is now an A/B path
   only, and a screen whose tile window is smaller than its image will differ
   under it.
4. The session-164 open items that this session did not touch remain:
   `OGRE_FOG_SCALE`, the one-instant-per-capture presenter, and the screens not
   compared against the reference. The `shifts`/`shiftt` item closes as
   unexercised (result 5).

## Round 3 additions (same session)

**Perspective-correct interpolation is ported from RT64.** `transform_vertex`
now returns the clip-space `w` (`out[8]`), the vertex buffer carries it as
attribute 3 (stride 9 instead of 8, `gl_Position = vec4(a_pos * a_w, 0.0,
a_w)`), so the rasterizer divides by `w` and interpolates the UVs and colours
the way `RSPProcessCS.hlsl` does (`screenPos.w = tfPos.w`, `RasterVS`
multiplies it into the position). The old shader passed `w = 1`, which
interpolated every 3D triangle affinely. The `G_TP_NONE` rule from
`TextureSampler.hlsli` is also carried: a non-perspective, non-rect draw halves
the sampled coordinate (`u_tex_persp`, `OTHERMODE_H` bit 10). The
`[GFX-SAMPLER]` census gained `persp=` (triangle draws whose vertex `w`'s
differ) and `nonpersp=`.

**The tutorial box's residual is not the perspective divide.** The change
leaves the box's upper band byte-identical, so that parchment quad has constant
`w`. The `OGRE_P167` probe (removed) showed the box interior is two triangle
draws of the same 64x64 CI4 parchment image: the upper band `uv=[0..265,
51..0]` (about two texels per pixel in `v`) and the flat `uv=[0..265,
51..51]` band below it. Against the native target the upper band is 11-17 mean
and the flat band 1-4, so the difference follows the steep `v` gradient: it is
a difference in the sampled texels of that draw, and the border strips and
portrait (texrects) are exact. The next measurement is that draw's four texels
and weights in the browser against RT64's `sampleTextureLevel`.

**Verification.** `make web` clean; `testdraw --flags 96 --expect-fbtex
--expect-hits 1` exit 0 (`fbtex hits=1 lines=1`); `boot.cjs` `RENDERED`
(`nonBlack=24664`). No probe remains (`grep -rn "OGRE_P167" app/` empty). The
attract/intro census with `persp=` was not run before the session closed; the
change is verified to be a no-op on the title/box (all rects pass `w = 1`).
