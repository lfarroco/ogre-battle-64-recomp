# Session 166 handoff — the tutorial box's parchment draw is point-sampled, and RT64 selects a texel one row above the browser's

## Goal

Close session 165's open item 1: the tutorial scene `0x17` dialogue box's upper
parchment band differed from RT64's target by 11-17 mean on rows 171..195 while
the flat band below it, the border strips and the portrait were 1-4 or exact.
Session 165's proposed measurement was that draw's interpolated `v` and its four
texels/weights at one pixel in both renderers.

Method: a tagged temporary probe (`OGRE_P166`) printing the draw's UVs, tile
state, the interpolated coordinate at chosen pixels, the floor/round results and
the sampled texel indices and colours; RT64's code read for the reference
(`TextureSampler.hlsli`, `TextureDecoder.hlsli`, `rt64_rsp.cpp`,
`rt64_rdp.cpp`); the native `OGRE_CAPTURE_TARGET` series as the reference image.

Timebox: 100 minutes.

## Result

1. **The box interior is one quad of two triangles, point-sampled, at about one
   texel per pixel — not two bands at two texels per pixel.** The `OGRE_P166`
   probe (removed) reported the two triangle draws that cover the interior:
   `img=64x64` CI4 (`fmt=2 siz=0 line=4 tmem=0 pal=0`, tile rect
   `uls=0 ult=0 lrs=252 lrt=252`, i.e. texels 0..63 square), `masks=6 maskt=6`
   `cms=0 cmt=0`, `G_TEXTURE sc=tc=32768`, one-cycle combiner with
   `cd=CSEL_TEXEL0` (so the output pixel *is* the sampled texel), and
   `OTHERMODE_H=0x088C30`, whose `G_TF_*` field is **0 = point sampling**.
   Vertices (NDC, `w = 1.000000` on all three): `u=265` at screen `x=27.2`,
   `u=0` at `x=292.8`, `v=0` at `y=170.0`, `v=51` at `y=222.0`. The interpolated
   coordinate at pixel centres is therefore `v(y) = 0.9804 * (y - 169.5)` and
   `u(x) = 265 * (292.8 - (x + 0.5)) / 265.6`, i.e. **0.98 texel per pixel** in
   `v` and one texel per pixel in `u`, with the `u` axis mirrored (265 at the
   left edge, 0 at the right). Session 165's "upper band `v` 51 to 0 at about two
   texels per pixel, flat `v=51` band below" is corrected: there is one draw
   pair for the whole interior (rows 170..222), and the row-196 boundary its
   per-row profile showed is a *contrast* boundary in the paper texture, not a
   draw boundary.

2. **The browser's sampling arithmetic equals RT64's for this draw.** With
   `w = 1` the perspective interpolation is affine, so the CPU barycentrics are
   the shader's; the probe's predicted texel
   `img[floor(v) mod 64][floor(u) mod 64]` equals the browser's decoded image at
   all 25 probed rows (0 mismatches) and `maskt=6` wraps (it does not clamp). On
   the draw side the tile's `uls`/`ult` are 0, so RT64's sub-texel origin
   `(uls, ult) * ulScale / 4` is 0 as well, `tcScale` is 1 (native resolution
   mode `native`, multiplier 1), `shifts`/`shiftt` are 0 and `ulScale` is 1;
   RT64's `round(uv * 128) / 128` (`SIMULATE_LOW_PRECISION`) cannot move a point
   sample by a whole texel. So neither the 1/128 quantization nor a fractional
   tile origin explains the difference.

3. **The difference is one texel row, and only in `v`.** The browser canvas was
   first aligned on the box frame (best shift `(0,0)`, `meanAbs 0.33` over rows
   166..171, and the portrait `1.10`, so the canvas pixel maps to the target
   pixel with no offset). At target column 285 with that alignment, the native
   target's colour at each row equals `img[floor(v) - 1][7]` — the column the
   browser samples (`s=7`) and **one texel row above** the browser's
   `img[floor(v)][7]`: **24 of 24 rows** of rows 172..195 match at `k = -1`,
   against **3 of 24** at `k = 0` (and 4-5 of 24 at `k = ±2`, 11 of 24 for the
   best other column). The text region's 7.9 mean is the same offset over the
   glyph rows.

4. **The lower part of the same draw differs by only 3 levels for that same
   one-texel shift.** With the frame alignment the per-region difference is
   `0.33` (frame rows 166..171), `1.10` (portrait), `10.49` (rows 172..195) and
   `2.99` (rows 196..218). The lower rows are not a second draw: the paper
   texture is nearly uniform in its rows 26..48, so the same one-texel error
   costs 2-3 levels there and 10-17 where the texture has contrast. Session
   165's "the flat band below matches at 1-4" is therefore a contrast effect,
   not evidence about two draws.

5. **What this does not establish: whether the browser or RT64 owns the pixel.**
   A one-texel `v` offset at 0.98 texel per pixel is a one-pixel vertical
   offset of the triangle's screen position. The border strips and the portrait
   are texrects and are exact, so the difference is confined to the triangle
   path (or to RT64's rasterization of it). The RSP viewport agrees in both
   renderers (`vscale=[640 480 511 0]`, `vtrans=[640 480 511 0]` →
   scale `(160,120)`, translate `(160,120)`; RT64's `setViewport` reads the
   guest words through its swapped view, so its `scale.x/translate.x` are 160
   and its `scale.y/translate.y` 120), and RT64's screen-to-NDC mapping in
   `RasterVS.hlsl` (`ndcPos.xy -= resolution/2; /= (res.x/2, res.y/-2)`) is the
   same as the browser's `ndc_xf`/`ndc_yf`, so no mechanism is named yet. Both
   survivors are namable experiments, not guesses: a synthetic triangle with a
   sharp horizontal edge drawn and captured in both renderers, or a probe of the
   vertex screen position in RT64 (which would mean instrumenting
   `tools/RT64`, not allowed).

6. **No code change.** No browser defect was located to fix: the sampler, the
   tile state, the coordinate arithmetic and the wrap are all identical to
   RT64's, and the one-texel difference has no mechanism in either renderer's
   code. Adding a pixel bias to the triangle path to make this box match would
   be a rule 7 violation, so the measurement is recorded and the item stays
   open.

## Evidence

RT64's code, read for the reference:

- `tools/RT64/src/hle/rt64_rsp.cpp` `setVertexCommon`:
  `tcFloats = (float)((double)(vertices[i].s * TextureSc) / (65536.0 * 32.0))`,
  `posScreen = (tfPos.xyz / float3(w, -w, w)) * viewport.scale + translate`;
  `setViewport` at `0x…`: `scale.x = vscale[1]/4`, `scale.y = vscale[0]/4`,
  `translate.x = movedFromOrigin(vtrans[1], ori) + offx)/4`,
  `translate.y = (vtrans[0] + offy)/4`.
- `tools/RT64/src/shaders/RasterVS.hlsl`: `ndcPos.xy -= resolution/2;
  ndcPos.xy /= (resolution.x/2, resolution.y/-2); ndcPos.xyz *= ndcPos.w`.
- `tools/RT64/src/shaders/TextureSampler.hlsli`: `sampleTexture`'s
  `uvCoord = inputUV * perspCorrectionMod * float2(shifts, shiftt)`, the
  `SIMULATE_LOW_PRECISION` rounding `round(uvCoord * 128)/128`,
  `uvCoord *= gpuTile.tcScale`, `uvCoord -= (uls, ult) * ulScale / 4`;
  `sampleTextureLevel`'s `floor(uvCoord)` base and point sample;
  `clampWrapMirrorSample`'s clamp-then-modulo order.
- `tools/RT64/src/shaders/TextureDecoder.hlsli` `sampleTMEMWithConvert` /
  `pixelAddress`: `t * stride + ((s << shift) >> 1)`, `tmemShift`, the
  `0x7FF`/`0xFFF` address masks.
- `tools/RT64/src/shaders/RasterPS.hlsl`: the fragment receives the rasterizer's
  interpolated `vertexUV` and `lowResUV` (the `fmod(screenPos, resolutionScale)`
  correction is zero at `resolutionScale = 1`) and samples through
  `sampleTexture`.
- `tools/RT64/src/hle/rt64_workload_queue.cpp`: `Resolution::Original` (the
  native run's saved `resolution native`) takes `resolutionMultiplier = 1.0f`,
  so the native capture is a 1x render of the 320x240 target.

Measured:

- Browser, forced `0x17` with `OGRE_P166=1`: 12 textured triangle draws of the
  64x64 tiling backdrop (`u=[0..447] v=[0..335]`, `filt=2` bilerp, shade 0.753)
  then the two box-interior triangles (`u=[0..265] v=[0..51]`, `filt=0` point,
  shade 0.710). The probe's per-pixel rows at `x=100` and `x=285`, rows 165..200,
  and the decoded 64x64 image (64 rows of texel RGB) are in
  `/tmp/w166/p166-h-p166.txt`; `p166-h-canvas.png` is the same run's frame with
  the box on screen.
- Native: `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=0x17
  OGRE_CAPTURE_TARGET=/tmp/w166/nat17/t OGRE_CAPTURE_AFTER=100
  OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=14000 ./build-dist/ogrebattle64
  assets/ogre64.z64`, 235 frames `t.101..t.335`, 320x240
  (`[renderer] config: resolution native (mult 2.00)`, which the workload queue
  resolves to 1x). The box region is byte-identical across the series, so no
  frame phase enters the comparison.
- The alignment and per-region numbers: frame rows 166..171 `0.33`, portrait
  `1.10`, rows 172..195 `10.49`, rows 196..218 `2.99`, and the same regions at
  the `(0,+1)` and `(+1,0)` shifts (76.52 and 2.40 for the frame; 9.63 and 22.01
  for rows 172..195) in `/tmp/w166/align2.py`.
- The texel-row match: 24/24 at `(s=7, k=-1)`, 3/24 at `(s=7, k=0)`, 24/24 only
  for that pair over all 55 decoded columns and shifts -8..8 (`/tmp/w166/final.py`).

## What changed, in one paragraph

Nothing in the renderer. The measurement instrument was an `OGRE_P166`-gated
block in `app/src/web_renderer.cpp` (`probe166_dump`, called from `draw_tri`
after `record_state`) that printed each textured 64x64 triangle draw's UV range
and tile fields, the three vertices' NDC/UV/w and raw `s`/`t`, the decoded 64x64
image as hexadecimal rows, and for two columns of pixels the barycentric
interpolation, `round(v*128)/128`, the local coordinate, `floor`, the fraction,
the four neighbouring texels and the three-point filter result. It is removed;
the tree carries no renderer change from this session.

## Files changed

| file | change |
|---|---|
| `docs/HANDOFF-2026-10-18-session166.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session entry, corrected `0x17` box claim, decision 166 |

`app/src/web_renderer.cpp` is byte-identical to its state at the start of the
session (`git diff HEAD -- app/src/web_renderer.cpp` is the pre-existing 155-165
work, with no probe text). No file under `RecompiledFuncs/`, `Bank*Funcs/`,
`RspFuncs/`, `tools/RT64` or `app/src/renderer.cpp` was changed. No commit was
made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean after the probe was
  removed (the same command was clean with the probe in place).
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`, `nonBlack=3024` after the batch.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — **NO RENDER**. Three
  invocations (`--attempts 3`, `--attempts 6`, `--attempts 8`, 17 boots in
  total) all reported `sawRealDl=false`, i.e. every boot landed on the known
  idle trajectory (normally about one boot in two). The machine's load average
  was 24 rising to 52 during those runs, and the boot stall is a boot-timing
  artifact, so this is not evidence about the renderer: the same build rendered
  the tutorial in this session's own probe run (`tasks=13`, the box on the
  canvas, `p166-h-canvas.png`) at 15:57, and `testdraw` renders and composites
  (`nonBlack=3024`). The boot `RENDERED` line still has to be reproduced on an
  idle machine.
- The probe runs: `node /tmp/w166/p166.cjs --secs 40 --attempts 8 --out
  /tmp/w166/p166-h` (throwaway copy of the harness loop; three of eight boots
  reached the box, `tasks=13`, 578 `[P166]` lines).
- `grep -rn "probe166\|OGRE_P166\|p166" app/ debug/` — no output (proved after
  the removal and again after the rebuild).

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The tutorial box's parchment band: a one-texel `v` difference, mechanism
   unnamed.** RT64's texel row is one less than the browser's at the same pixel
   (24/24 rows), i.e. its quad is about one screen pixel lower; the tile state,
   the UVs, the wrap, the sampler arithmetic and the RSP viewport all agree. The
   next measurements, in order of cost: (a) `test_draw` gains a triangle whose
   texture has a sharp horizontal edge (1 texel per pixel, point-sampled) so one
   synthetic frame can be captured on both sides without the game; (b) if that
   shows the same one-pixel difference, a tagged probe of the browser's
   `posScreen` (the NDC `out[0..1]` and the `w`) against the guest's own
   vertex data, which is not allowed inside `tools/RT64` and would need the
   native renderer's `[rdp]`/vertex trace instead.
2. **Session 165's `0x17` description is corrected** (one draw pair, point
   sampled, ~0.98 texel per pixel) and its "flat `v=51` band" is withdrawn; the
   handoff's 5.43-mean "box at (0,0)" reading holds with this session's
   alignment (frame 0.33, portrait 1.10).
3. **The title fog band** (0.09-0.24) is untouched; nothing here was measured
   against it.
4. **`OGRE_TEXF=gl`** still cannot express the tile window (session 165 item 3),
   and the session-164 items (the one-instant-per-capture presenter,
   `OGRE_FOG_SCALE`, the screens not compared against the reference) remain.
5. **(`OGRE_P166` was a temporary probe):** no diagnostic was kept, so
   `docs/guides/web-probes.md` and `docs/guides/app-build.md` are unchanged.
