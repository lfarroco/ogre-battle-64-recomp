# Session 163 handoff — the browser colour image is a high-precision attachment, and the fog band is not blend rounding

## Goal

Continue the browser (WebAssembly) renderer from session 162. Verify its
diagnosis that the title/story fog band residual (0.273 in rows 64..119) is the
browser's 8-bit GL blend quantizing every draw against RT64's float-to-RGBA16
conversion, then port RT64's high-precision internal colour format if that is
the cause.

Timebox: 100 minutes.

## Result

1. **Session 162 reproduces.** A fresh 40-shot browser series on
   `?OGRE_SCENE=title` against session 162's native series (`/tmp/w162/nat-fog`,
   397 frames) settles on one presented story frame, best native target
   `t.230`, `meanAbs=0.106` with bands 0.041 / 0.272 / 0.061. A second run's
   frame matches `t.177` at `0.098` with bands 0.014 / 0.271 / 0.060, which is
   session 162's `0.09` with `0.273 / 0.060`. The frame is the attract story
   text, not the title logo. The run can keep one presented instant for the
   whole capture window, as session 162 recorded.

2. **RT64's high-precision colour buffer is ported.** RT64's
   `internalColorFormat` defaults to Automatic, which resolves to High when the
   device reports `preferHDR`; the native captures on this machine are
   `R16G16B16A16_UNORM` (`tools/rt64_target.py`), so the native renderer keeps
   every blend at 16 bits and quantizes once, on the RDRAM writeback. A browser
   colour image is now an RGBA16F attachment when the context exposes
   `EXT_color_buffer_float`, and RGBA8 when it does not or when `OGRE_HPFB=0`.
   WebGL2 has no 16-bit UNORM colour attachment without `EXT_texture_norm16`,
   which the probe browser does not expose. The float attachment has three
   requirements the first build hit: `glTexImage2D` needs `type=GL_HALF_FLOAT`
   (WebGL2 rejects `UNSIGNED_BYTE` for RGBA16F with INVALID_OPERATION and leaves
   the attachment incomplete, status 0x8CD6), `upload_framebuffer` must stage
   float client data, and the readback must read floats. The writeback converts
   through RT64's `Float4ToRGBA16` rule with `usesHDR` (RGB rounds through 8
   bits, the coverage bit is bit 2 of the 16-bit alpha); the RGBA8 path keeps the
   8-bit rule. The fragment shader now declares `highp` (RT64's raster works in
   fp32; `mediump` is fp16 on parts of the mobile GLSL spec).

3. **The 0.27 fog-band residual is not blend rounding.** Two runs at the same
   native instant compared:
   - RGBA16F, `hp-a-03` vs `t.177`: `meanAbs=0.098`, bands 0.014 / 0.271 / 0.060
   - RGBA8 (`OGRE_HPFB=0`), `h0-a-01` vs `t.177`: `meanAbs=0.099`, bands 0.014 /
     0.273 / 0.060

   The two canvases differ from each other by `0.0091` mean over 225387 channel
   samples: 99.09% identical, 2043 samples (0.906%) all exactly one level, 1055
   below and 988 above. So the attachment does change the pixels the precision
   affects (session 163 proof: `docs/proofs/web-hpfb-vs-rgba8-diff.png`, the
   translucent cloud/fog boundaries), and it does not move the native residual
   by more than 0.001. Session 162's attribution of the band to per-blend
   quantization is corrected: the destination precision is not the source of
   the remaining difference.

4. **The residual is a value difference in the fog's own rows.** Against the
   native target converted with the target's own 8-bit rounding (instead of the
   high byte), the same frame's band 64..119 is still `0.241`; the two native
   quantizations differ from each other by `0.068` in that band, so at most
   `0.068` of the `0.27` is the comparison format. The amplified difference
   (`docs/proofs/web-title-fog-residual-location.png`) is one horizontal wave in
   rows 81..119, the top edge of the repaired fog layer. The distribution over
   band 64..119 is symmetric: 81.97% exact, 7.99% -1, 5.78% +1, 1.61% -2, 1.44%
   +2, and 0.5% at 3..8 levels.

5. **The repaired layer image is I8, not RGBA.** The new
   `[GFX-FOG-SUMMARY]` line prints the last repaired layer's record:

   ```
   task 2: repaired=2 last_layer=2 last_image=0x801D51E8 white=3 zero=2 image siz=1 fmt=4 64x64
   ```

   `fmt=4` is `G_IM_FMT_I`, `siz=1` is 8-bit, so the fog is a 64x64 intensity
   image. The browser decodes I8 as `(v,v,v,v)` (RT64 `I8ToFloat4`), so the
   fog's alpha is the texel intensity. The next measurement is one fog pixel's
   sampled intensity on both sides, not another attachment format.

6. **No regression.** The publisher still matches RT64's target exactly
   (`meanAbs=[0.0,0.0,0.0]`, 100.0% over 639x475) on the new attachment; the
   synthetic draw's framebuffer-as-texture control still reports
   `fbtex hits=1` with flags 96; the boot still renders
   (`nonBlack=25424`); the attract story still draws with no page error.

## What changed, in one paragraph

`GlFramebuffer` has an `fp` flag and is created as `GL_RGBA16F` when
`EXT_color_buffer_float` is available. `ensure_gl` enables the extension, reads
`OGRE_HPFB` (default on) and logs `[RENDERER] colour attachment: RGBA16F|RGBA8`.
`upload_framebuffer` stages `GL_FLOAT` data into a float attachment;
`readback_framebuffer` reads `GL_FLOAT` and calls `float4_to_rgba16` (RT64's
`Float4ToRGBA16` with dither 0 and the 16-bit `usesHDR` coverage test). An
incomplete RGBA16F attachment logs and falls back to RGBA8 for the run. The
fog walker's summary line gained the last repaired layer's image record, so a
run states the fog format. `OGRE_HPFB=0` is the A/B switch; `?OGRE_HPFB=0` on
the page reaches it through `web.js`'s `ENV`.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `#include <emscripten/html5_webgl.h>`; `float4_to_rgba16`; `GlFramebuffer::fp`; RGBA16F creation with float upload and float readback and the RGBA8 fallback; `ensure_gl` extension/knob/milestone; `precision highp float`; `[GFX-FOG-SUMMARY]` image format |
| `docs/HANDOFF-2026-10-17-session163.md` | this file |
| `PLAN.md` | Status paragraph; correction to the session-162 blend-rounding claim; open-work subitem |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | log entry, index row, decision row |
| `docs/guides/app-build.md` | browser knob paragraph for `OGRE_HPFB` |
| `docs/proofs/web-hpfb-vs-rgba8-diff.png` | the same instant under RGBA16F and RGBA8, difference amplified x64 |
| `docs/proofs/web-title-fog-residual-location.png` | the remaining difference against native `t.177` with the target's 8-bit rounding, amplified x16 |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made. No probe was added, so
there is nothing to revert beyond the kept diagnostic: `grep -rn probe163 app/
debug/` is empty.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `[100%] Built target
  ogrebattle64`, `web: build-wasm/ogrebattle64.wasm is ready` (twice: after the
  attachment port, after the `highp` and fog-summary edits).
- Browser A/B, same URL except the knob, 30 shots each, six runs:
  `OGRE_URL='http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1'
  node debug/probes/shots.cjs --shots 30 --gap 400 --out /tmp/w163/hp-a` and the
  same with `&OGRE_HPFB=0` (`h0-a`), then `hp-b`/`h0-b`, `hp-c`/`h0-c`.
- The same-instant pair is `hp-a-03` and `h0-a-01`; both match native `t.177`.
  The comparisons are `python3 tools/web_frame_cmp.py <canvas> /tmp/w162/nat-fog/t.177.bin
  --inset 1 --canvas-size 319x239`, and the series minima use `/tmp/w162/torgb.py`
  + `/tmp/w161/cmp3` (the tool's own reader and math; a 397-target Python search
  is minutes per frame).
- The band histogram and the two quantizer conversions are a Python script over
  `web_frame_cmp.canvas_pixels` and `t.177.bin`; the 8-bit conversion of the
  target is `round(v / 65535 * 255)`, the high byte is `v >> 8`.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  `returned=1`, `fbtex hits=1`, canvas `nonBlack=46452`.
- `node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_SCENE_LOG=1'
  --out /tmp/w163/pub --secs 40 --attempts 4 --shots 2` then
  `tools/web_frame_cmp.py /tmp/w163/pub-00.png /tmp/w146/nat/tt.140.bin
  --target-size 640x480 --canvas-size 639x475` — `[0.0,0.0,0.0]`, 100.0%.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — `RENDERED`,
  `nonBlack=25424`.
- `OGRE_FOG_SUMMARY=1` title run (`/tmp/w163/fogsum2`): the layer record line
  quoted above, on a run whose frames show the story screen
  (`nonBlack=292496`).
- `grep -rn probe163 app/ debug/` — no output, exit 1.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The fog band's 1-3-level difference is now the top browser item, and it is
   not the colour attachment.** The layer is a 64x64 I8 image sampled as
   TEXEL0_ALPHA with a white RGB combiner; the difference is a symmetric ±1
   over its translucent top edge. Compare one repaired group's sampled texel
   intensity and blend alpha on the browser and RT64 sides (browser
   `OGRE_FOG_TRACE`/`OGRE_FOG_SUMMARY`, native `fog` verbose), rather than
   changing another target format. The comparison floor for this metric is the
   `0.068` between the native target's high byte and its 8-bit rounding in that
   band.
2. **The browser series often presents one instant for a whole capture window.**
   Session 162's "first flush takes the window" also happens here, so a
   phase-matched A/B costs several runs. A faster presenter (or a debug pause)
   is what makes the next comparison cheap; start from
   `node debug/probes/fps.cjs` and `vi.cjs` on the current build.
3. **`OGRE_FOG_SCALE` is still not ported** (session 162, open item 2).
4. The screens not compared against the reference are unchanged from session
   162: the publisher still is exact and the boot/story/fill checks pass.
