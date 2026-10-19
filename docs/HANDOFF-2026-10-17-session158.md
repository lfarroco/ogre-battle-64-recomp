# Session 158 handoff — the fog layer samples in-window with no mask, so the tile-window candidate is excluded; the title's residual stays on the translucent cloud

## Goal

Continue the browser (WebAssembly) renderer from session 157. That session left
one measured lead: at title rows 190..238 the fog image `0x249538` is 200 texels
wide (`stride=800`, `lrs=796`) while its draw's `s` was said to run to 320
texels, and the browser implements only `GL_REPEAT`/`GL_CLAMP_TO_EDGE` from
`cms`/`masks == 0`, with no RDP `masks`/`cms` masking of the tile window.

The session had to confirm or refute the out-of-window sample with a tagged
probe, read RT64's masking rule from `tools/RT64`, implement the rule if the
probe justified it, and measure. If the probe showed the in-window texel, the
session had to record that and name the next candidate with the measurement that
separates it.

Timebox: about two hours. The probe refuted the lead, so this session records
the measurement and reverts every probe. No renderer behaviour changed.

## Result

1. **Session 157's tree claims hold.** `grep -rn probe157 app/ debug/` prints
   nothing and exits 1. `make web EM_CACHE=/Users/momo/dev/ogre/.emcache`
   builds clean. `/tmp/w156` still holds both series, and the baseline
   comparison recomputes exactly:

   ```
   python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1
     meanAbs=[2.08, 2.23, 2.74] meanSigned=[-2.03, -2.17, -2.67] exact=79.2% over 318x238
   ```

2. **The texture-coordinate path, read before any theory.** A `G_SETTILE` fills
   a `TileState`'s `cmt`/`cms`/`maskt`/`masks`/`shiftt`/`shifts` and `line`
   (`app/src/web_renderer.cpp:3795-3811`), `G_SETTILESIZE` fills
   `uls`/`ult`/`lrs`/`lrt` (`3814-3821`), and `ensure_tile_image` derives the
   decoded rect from them, `x0 = uls>>2`, `y0 = ult>>2`,
   `w = (lrs>>2) - x0 + 1`, `h = (lrt>>2) - y0 + 1` (`4154-4202`). The decoded
   rect is uploaded once per key as the whole GL texture
   (`glTexImage2D(..., GL_RGBA8, w, h, ...)`, `2401`). The wrap mode is the only
   place `cms`/`masks` reach GL: `up.clamp_s = (t.cms != 0) || (t.masks == 0)`
   and the same for T (`4279-4280`), applied as
   `glTexParameteri(GL_TEXTURE_WRAP_S, up.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT)`
   (`2403-2406`). A texrect's `s`/`t` become the shader's `a_uv` in texels
   (`(v.s * tex_scale_s) / 32`, `4523-4524`), and the vertex shader normalises
   with the tile window, `v_uv0 = (a_uv - u_uv_origin) * u_uv_scale` (`1065`),
   where `u_uv_origin` is `(uls>>2, ult>>2)` and `u_uv_scale` is
   `(1/w, 1/h)` (`4403-4410`). So the sampler's coordinate is
   `(s_texel - uls_texel) / w`, and the texture is the tile window itself.

3. **RT64's rule, quoted** (`tools/RT64/src/shaders/TextureSampler.hlsli`,
   `clampWrapMirrorSample`; the sampler's coordinate is made tile-relative in
   `sampleTexture`: `uvCoord -= (float2(rdpTile.uls, rdpTile.ult) * gpuTile.ulScale) / 4.0f`):

   ```hlsl
   if (rdpTile.cms & G_TX_CLAMP) {
       texelInt.x = clamp(texelInt.x, 0, (round(tcScale.x * rdpTile.lrs) / 4) - (round(tcScale.x * rdpTile.uls) / 4) + round(tcScale.x - 1.0f));
   }
   if (rdpTile.cmt & G_TX_CLAMP) {
       texelInt.y = clamp(texelInt.y, 0, (round(tcScale.y * rdpTile.lrt) / 4) - (round(tcScale.y * rdpTile.ult) / 4) + round(tcScale.y - 1.0f));
   }

   const int masks = round(rdpTile.masks * tcScale.x);
   if ((rdpTile.cms & G_TX_MIRROR) && (masks > 0) && (modulo(texelInt.x, masks * 2) >= masks)) {
       texelInt.x = (masks - 1) - modulo(texelInt.x, masks);
   }
   else {
       texelInt.x = modulo(texelInt.x, masks);
   }
   ...
   // Allows selection of only particular columns or rows as defined by the tile.
   texelInt = (texelInt & gpuTile.texelMask) + gpuTile.texelShift;
   ```

   `modulo` is `x - y * floor(x / y)` and returns `x` when `y == 0`
   (`src/shaders/Math.hlsli:11-19`), and `texelMask`/`texelShift` are
   `UINT_MAX`/`0` for a tile that was not created as a framebuffer
   reinterpretation (`src/hle/rt64_state.cpp:805-806`). The rule is therefore:
   clamp by `lrs`/`lrt` only when `cms`/`cmt` has `G_TX_CLAMP` (2) set, wrap by
   `masks`/`maskt` only when those fields are non-zero, and index the
   loader-relative texel otherwise.

4. **Probe 1: there is no out-of-window sample in the title's fog draws.** A
   tagged probe (`probe158`) printed, for every five-row texrect strip of the
   first title frame, the rect, the `s`/`t` texel range, the window,
   `masks`/`maskt`/`cms`/`cmt`, the uploaded size and the wrap mode, plus the
   texel three columns sample under the browser's rule and under RT64's rule at
   the strip's top row and the row below it. 22 strips were logged, all of
   `img=0x249538`:

   ```
   [GFX-probe158] rec img=0x249538 rect=0,156..200,161 win=0,120..796,136 masks=0 maskt=0
     cms=0 cmt=0 up=200x5 org=0,30 clampS=1 clampT=1
     x50 gl=(50,0,a216) rdp=(50,0,a216) a+1=240
     x100 gl=(100,0,a0) rdp=(100,0,a0) a+1=0
     x150 gl=(150,0,a0) rdp=(150,0,a0) a+1=0
   ```

   On all 22 strips `masks=0`, `maskt=0`, `cms=0`, `cmt=0`, the upload is
   `200x5`, and both wrap modes are `GL_CLAMP_TO_EDGE` (`clampS=1 clampT=1`).
   Of the 66 texel comparisons, the browser's texel and RT64's rule's texel are
   identical in 66: the same `(x, y)` and the same decoded alpha in every case.
   No case is out of window, so there is nothing to mask.

5. **Session 157's `s`-range number is corrected.** The fog rect is 200 pixels
   wide and its `s` runs from 0 to 200 texels (`uv0=(0.000,0.000)`,
   `uv3=(200.000,5.000)`), not to 320. Its window is `uls=0, lrs=796`, i.e.
   texels 0..199. The last pixel column samples `s = 199.5`, texel 199, the
   window's last texel; the rectangle's right edge lands exactly on the window
   edge. The `319x239` draw of the same image that both renderers skip has
   `dsdx=dtdy=0`, so it samples one texel and is degenerate. There was never a
   320-texel span over a 200-texel window on this screen, which is why the
   probe finds no out-of-window sample.

6. **Probe 2: the fog draw's own output is the blend of its decoded texel, and
   the browser matches the native where the fog is opaque.** The same probe
   read the destination row before and after each fog draw:

   ```
   [GFX-probe158] dst row=8 w=320 x50 bef=(0,0,0) aft=(18,39,36) a=255 a1=255
     x100 bef=(0,0,0) aft=(14,34,23) a=255 a1=255 x150 bef=(6,7,2) aft=(5,17,8) a=255 a1=255
   ```

   Where the sampled texel's alpha is 255 the draw replaces the destination with
   the texel; where it is 0 the destination is unchanged (`bef == aft`). That is
   the RDP's blend for a black texel at full alpha in one case and a zero alpha
   in the other, and it is what session 157 measured for `0x217508`. The fog
   image is a dark teal band, not a bright one: the texels it writes at full
   alpha are `(14,34,23)`, `(18,39,36)`, `(23,47,46)`.

   Against the native, the same pixels agree. On rows 200, 210, 220, 222, 226
   and 231 at x = 30, 50, 80, 100, 120 and 150, the fresh browser frame and
   `/tmp/w156/nat2/t.574.bin` are identical in 36 of 36 comparisons, including
   the pixels the fog writes opaquely. So the fog's decode, its sampling and its
   blend agree with RT64.

7. **The residual is confined to the rows where the visible layer is
   translucent.** A fresh browser series of the forced title (`shots.cjs
   --attempts 2 --secs 200 --shots 3 --gap 400`) was compared with the 525-frame
   native series. Its best pair is `probe-00` against `t.137` at
   `meanAbs=[2.32,2.52,3.14] meanSigned=[-2.30,-2.50,-3.12] exact=81.3%`, the
   same value as session 156/157's minimum. Row bands of that pair
   (x 1..318, channels 0..2):

   ```
   rows   0..239  meanAbs=2.64  meanSigned=-2.62  exact=81.4%
   rows 120..189  meanAbs=0.03  meanSigned= 0.03  exact=97.2%
   rows  80..119  meanAbs=7.75  meanSigned=-7.75  exact=39.3%
     x < 140      meanAbs=7.95  meanSigned=-7.95  exact=36.1%
     x >= 140     meanAbs=7.60  meanSigned=-7.60  exact=41.8%
   rows 190..238  meanAbs=6.52  meanSigned=-6.51  exact=64.9%
     x < 140      meanAbs=0.05  meanSigned=-0.04  exact=98.0%
     x >= 140     meanAbs=11.55 meanSigned=-11.53 exact=39.3%
   ```

   Rows 120..189 are the rows where `0x217508` is opaque, and they are exact.
   In rows 190..238 the left 140 columns are exact because the fog writes them
   opaquely, and the difference is on the right where the fog is transparent and
   the cloud shows. In rows 80..119 the cloud is translucent across the whole
   width and the whole width differs. So every difference is on a row where the
   visible layer has partial alpha, and every row with an opaque visible layer
   is exact.

   Session 157's alpha-bucketed residuals were not recomputed. The bucket needs
   each pixel's `0x217508` texel alpha from the same frame, and this session took
   no same-frame RDRAM dump; the x < 140 / x >= 140 split above carries the same
   information here, because the fog's opaque left half and the cloud-only right
   half separate the two layers by position rather than by alpha.

8. **The negative result.** The tile-window/mask candidate is excluded: the
   fog samples in-window, `masks`/`maskt` are 0 and `cms`/`cmt` are 0, the
   browser's clamp choice and RT64's clamp rule select the same texel, and the
   fog's draw output equals `a*texel + (1-a)*dst` with the decoded texel alpha.
   What remains is the partial-alpha composition of the cloud layer `0x217508`.
   Session 157 excluded the blend's enable state, its factors and its arithmetic
   for that draw, and this session excludes the fog as the source of the extra
   brightness. The two candidates left are the alpha the browser's sampler uses
   for `0x217508` and the destination the cloud blends onto.

   The measurement that separates them: the browser already prints each
   rectangle's decoded `mean` and `opaque` count per strip (`[GFX-TEXTRACE]`),
   and the native renderer can decode the same window from the same RDRAM with
   `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1` (`app/src/renderer.cpp:817-832`,
   the `colour: mean ... over WxH texels, opaque N` line). Strip by strip on the
   same frame, equal numbers mean both sides hold the same image content and the
   difference is in the composition; different numbers mean the two runs hold
   different content for that image and the pixel residual is that difference.
   The native side has not been run with that knob yet.

   The reference here is RT64's own render target; a match to it is a browser
   result and not a statement about retail hardware.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | the tagged probe (`probe158`: the `DrawCmd::p158*` fields, a record-time line per five-row texrect strip with the window/mask/wrap facts and both texel rules, and a `glReadPixels` before/after line in the flush) was added for the measurements and then removed. `grep -rn probe158 app/ debug/` is empty and the build after the revert is the verified one. No renderer behaviour was changed: no sampler rule, no wrap mode, no upload size. |
| `docs/HANDOFF-2026-10-17-session158.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, decision row; item 8 sub-item (n) records the exclusion and the next measurement |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or `tools/RT64` was
changed. No commit was made. No `docs/proofs/` image was added or replaced,
because no rendered frame changed: the probe only read state, and the frames it
produced are the reverted tree's output.

## What was run for verification

- `grep -rn probe157 app/ debug/` — no output, exit 1.
- `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1`
  — `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2% over 318x238`.
- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `[100%] Built target
  ogrebattle64`, `web: build-wasm/ogrebattle64.wasm is ready`, three times: the
  baseline, the probe build, and the build after the probe revert.
- `node debug/probes/shots.cjs --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1' --attempts 2 --secs 200 --shots 3 --gap 400 --out /tmp/w158/probe`
  — `CAPTURED 3 frames (attempt 1)`; the `[GFX-probe158]` lines in §4 and §6 are
  from `/tmp/w158/probe-milestones.txt` (22 `rec` lines, 22 `dst` lines).
- `python3 tools/web_frame_cmp.py /tmp/w158/probe-00.png '/tmp/w156/nat2/t.*.bin' --inset 1`
  — best `/tmp/w156/nat2/t.137.bin: meanAbs=[2.32,2.52,3.14] meanSigned=[-2.30,-2.50,-3.12] exact=81.3% over 318x238`.
- The row bands of §7 are the same two frames read through
  `tools/web_frame_cmp.py`'s `canvas_pixels`/`target_pixels`.
- `python3 tools/web_frame_cmp.py /tmp/w147/final2-00.png /tmp/w146/nat/tt.140.bin --buffer 640x476 --target-size 640x480`
  — the publisher still is unchanged: `meanAbs=[0.87,0.84,0.75]
  meanSigned=[-0.87,-0.84,-0.75] exact=99.6% over 640x476`.
- `node debug/probes/boot.cjs --attempts 2 --secs 45 --out /tmp/w158/boot-final`
  on the reverted build — `RENDERED {"w":640,"h":480,"total":307200,"nonBlack":25136,"colorful":18868,...}`.
- `grep -rn probe158 app/ debug/` after the revert — no output, exit 1.

There is no repo test harness; each check is a ROM-dependent run. The native
series and the publisher target are session 156's and session 146's, still under
`/tmp/w156` and `/tmp/w146`, and were reused because no renderer behaviour
changed: the probe only read state, and the frame it produced
(`/tmp/w158/probe-00.png`) is the reverted tree's output.

## Open

1. **The cloud's translucency.** Every differing pixel is on a row where the
   visible layer has partial alpha, and every opaque row is exact. The next
   measurement is one native run with `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1`
   (about 30 s, no code change) to print the native renderer's decoded `mean`
   and `opaque` for each `0x217508` strip, and compare them with the browser's
   `[GFX-TEXTRACE]` `mean`/`opaque` for the same strips. Equal numbers put the
   difference in the composition (the destination the cloud blends onto);
   different numbers put it in the image's content or alpha.
2. **The unwritten last column and row, and the canvas's rounded corners.**
   Unchanged from session 157: cosmetic, and the canvas's CSS box needs the
   developer's intent.
3. **The LOADTLUT destination key** is still the load tile's `palette` field
   where RT64 uses its `tmem`; they agree for every traced load.
4. **The screens not compared against the reference** are unchanged.

## Correction (session 161)

The "renderer draws the cloud layers differently" conclusion here is corrected.
The two runs do share a scroll state: at the third display list of each run the
135 draws whose tile window is non-degenerate are identical in order, image,
rectangle, `s`, `t`, `dsdx`, `dtdy` and window (the `0x1C3EA8` partial tile is
`rect=0,16..63,80 s=32` in both). Every native list also has 456 draws with a
degenerate tile window (`ult == lrt`): the mirrored `0x1E2918` pieces and the
72-pixel `0x217508` columns. Those are the native app's `OGRE_FOG` repair pieces
(`app/src/renderer.cpp`, `nop_rect::Walker::fog_fix`), which the browser skips at
its own empty-tile guard, so the browser draws no fog layer. Two native series
differing only in `OGRE_FOG=0` give a fog contribution of 3.4 mean abs (rows
64..119 `5.68`), and the browser frame matches the fog-off series at
`meanAbs=0.052` (rows 0.046/0.072/0.044) against `2.349` against the fog-on
series. The residual is the missing fog layer, not the sampler, the blend or the
phase. See `docs/HANDOFF-2026-10-17-session161.md`.
