# Session 157 handoff — the title's cloud blend is correct at the draw; the residual is a partial-alpha layer of the composition, and the three candidate mechanisms are excluded by measurement

## Goal

Continue the browser (WebAssembly) renderer from session 156. Session 156 left
one open item: at the title screen's cloud bands the browser's pixel equals the
raw cloud texel while the native target's is a blend, although the browser's
decoded state says `ablend=1 force=1 ccyc=1` with a TEXEL0 combiner and
alpha = TEXEL0.

The session had to decide which of three explanations is true, in order of cost:

1. GL blending is not actually enabled for that draw, whatever the decoded
   command says.
2. The sampler's alpha is not the decoded texel alpha (upload or key fault).
3. The blend mapping is wrong in the renderer.

Then either fix the smallest correct mechanism or record the negative result and
what it excludes.

Timebox: about two hours. No measured improvement was found in that window, so
this session records the probe results and reverts every probe.

## Result

1. **Session 156's three tree claims are true.** `grep -rn probe156 app/ debug/`
   prints nothing and exits 1. The `[GFX-TEXTRACE]` strip line in
   `app/src/web_renderer.cpp` (the texrect block that ends
   `ablend=%d force=%d ccyc=%d`) is passed `cmd.alpha_blend`,
   `cmd.force_blend`, `cmd.combiner_cycles` in that order, after `cmd.cycle`,
   `st.othermode_h`, `st.othermode_l` and `(st.othermode_h >> 14) & 3u`; the
   format string and the argument list match one for one. The three proof PNGs
   exist (`docs/proofs/web-title-clouds-browser-aligned-title03.png`,
   `-native-title-clouds-aligned-t574.png`, `-web-title-clouds-diff-amplified.png`),
   and `/tmp/w156` still holds the two series, so the aligned minimum
   recomputes to the handoff's numbers:

   ```
   python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1
     meanAbs=[2.08, 2.23, 2.74] meanSigned=[-2.03, -2.17, -2.67] exact=79.2% over 318x238
   ```

2. **The one RGBA32 cloud draw's whole path, read before any theory.** A texel is
   decoded in `decode_texture_rect` (`app/src/web_renderer.cpp:887`); RGBA32
   reads guest bytes 0/1 as R,G and 2/3 as B,A
   (`app/src/web_renderer.cpp:1028-1031`). The decoded vector becomes a
   `TexUpload` and is uploaded once per image key with
   `glTexImage2D(..., GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ...)`
   (`app/src/web_renderer.cpp:2401`), so the alpha byte reaches an 8-bit alpha
   channel. For a 1-cycle draw `record_state` evaluates mux 1
   (`app/src/web_renderer.cpp:4369-4380`); the words `0xFCFFFFFF`/`0xFFFCF279`
   decode to `rgb=TEXEL0`, `a=TEXEL0` under RT64's bit layout
   (`tools/RT64/src/shared/rt64_color_combiner.h`). The blender
   `P=CC M=FB A=CC_A B=1-A` (`oml=0x00504240`) takes the shader's
   framebuffer-colour branch (`M == FB`), which outputs
   `vec4(combiner.rgb, combiner.alpha)` (`app/src/web_renderer.cpp:1246-1252`).
   `cmd.alpha_blend` is true for that state (`compute_uses_alpha_blend`,
   `app/src/web_renderer.cpp:414-426`) and becomes
   `glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`
   (`app/src/web_renderer.cpp:2654-2659`), which is the RDP's
   `P=CC A=CC_A M=FB B=1-A` equation.

3. **Probe 1: GL blending is on with the RDP's factors, and there is no GL
   error.** A tagged probe read the state for the first `0x217508` draw of every
   flush, after the renderer had applied its blend state:

   ```
   [GFX-probe157] state img=0x217508 blendEnabled=1 srcAlpha=0x0302 dstAlpha=0x0303
     srcRGB=0x0302 dstRGB=0x0303 eqRGB=0x8006 eqA=0x8006 err=0x0000
     cmdAblend=1 force=1 cyc=1 fbo=0x000400 row=146 w=320
   ```

   `0x0302` is `GL_SRC_ALPHA`, `0x0303` is `GL_ONE_MINUS_SRC_ALPHA`, `0x8006`
   is `GL_FUNC_ADD`, `err=0` is `GL_NO_ERROR`. Hypothesis 1 is false.

4. **Probe 2: the sampler's alpha is the decoded texel alpha, and the GL blend
   output is the RDP equation to the level.** The same probe read a whole row of
   the target before the draw, after the draw, with the blend off and the
   fragment alpha written as grey (`u_debug_mode == 3`, a probe-only shader
   branch returning `vec4(vec3(outc.a), 1.0)`), and with the blend off and the
   normal output. The probed row was the one the draw covers (guest row 93,
   image row 13):

   ```
   [GFX-probe157] row=146 mid=47 match=47 worst=0 x=8 dst=(190,176,157)
     after=(190,175,157) frag=(190,175,157) fragAlpha=159 predicted=(190.0,175.4,157.0)
   ```

   `fragAlpha=159` is the decoded alpha of texel (8,13), whose decode from the
   session's own `OGRE_DUMP_RDRAM` image is `(190,175,157,159)`; the fragment
   colour is the same texel's RGB. Of the 47 pixels of that row with
   `0 < alpha < 255`, all 47 match `alpha*frag + (1-alpha)*dst` within 0 levels.
   A fully transparent texel was also seen to give `fragAlpha=0` and leave the
   destination unchanged. Hypothesis 2 is false (upload and key included) and
   hypothesis 3 is false for this mode.

5. **What the bands are, from the draw list.** The browser's trace and the
   native `[tex]` trace of the same display list have the same 136 draws in the
   same order: 1 `G_FILLRECT`, 15 six-row strips and 6 64x64 blocks of the sky
   `0x1C3EA8` (rows 0..89 and 16..80), 35 three-row strips of `0x1E2918`
   (rows 24..129), 53 three-row strips of `0x217508` (rows 80..239), 2
   `0x1C13F8`, 24 five-row strips of `0x249538` (rows 126..238) and one
   full-screen `0x249538` draw that both renderers skip as a degenerate tile
   window. The first draw of every list is the fill, and the probe shows it is
   an opaque write with blending off:

   ```
   [GFX-probe157] fill fbo=0x000400 prim=(0.000,0.000,0.000,1.000)
     alphaBlend=0 force=0 cyc=1 approx=0 blendEnabled=0
   ```

   So the framebuffer starts each list from an opaque black fill, and the bands
   are the rows where no layer of the stack is opaque: rows 80..119 (`0x1E2918`
   and `0x217508` both partial) and rows 190..238 (`0x217508` partial and then
   the `0x249538` fog over it). Rows 0..79 are covered by opaque sky and rows
   120..189 by the `0x217508` layer at alpha 255, which is why those rows are
   exact.

6. **The difference follows partial alpha, and only partial alpha.** Bucketing
   every pixel of rows 80..239 by the `0x217508` texel's alpha, browser minus
   native (`/tmp/w156/web/title-03.png` against `t.574.bin`, inset 1):

   ```
   alpha   0- 31  n=13275  meanSigned = -4.87
   alpha  32- 63  n=  322  meanSigned = -12.86
   alpha  64- 95  n=  308  meanSigned = -10.58
   alpha  96-127  n=  291  meanSigned =  -7.49
   alpha 128-159  n=  264  meanSigned =  -5.52
   alpha 160-191  n=  278  meanSigned =  -3.64
   alpha 192-223  n=  420  meanSigned =  -1.88
   alpha 224-255  n=13462  meanSigned =  -7.94
   ```

   Row by row the pattern is sharper. At rows 200, 210 and 220 the pixels whose
   `0x217508` texel has alpha 0 are identical in the two renderers (meanSigned
   0.0, 0.0, 0.0) and the pixels the layer writes opaquely are the raw texel in
   the browser while the native is brighter by 12 to 44 levels:

   ```
   row 200 x=200  texel (21,23,8,255)   browser (21,23,8)   native (42,44,30)
   row 210 x=200  texel (79,74,44,255)  browser (79,74,44)  native (103,99,73)
   row 220 x=300  texel (34,33,0,255)   browser (34,33,0)   native (75,75,48)
   ```

   At rows 100 and 110, where the layer beneath is `0x1E2918` over the sky and
   the `0x217508` texel is transparent, the two also differ but by less
   (`row 100 x=80: browser (76,70,54), native (80,74,60)`).

7. **The negative result.** None of the three candidate mechanisms is the
   cause. The blend is enabled with the RDP's factors, the sampled alpha is the
   decoded texel alpha, and the GL blend output is `alpha*src + (1-alpha)*dst`
   exactly. The residual is in the composition of a *partial-alpha layer other
   than the probed draw*: the `0x249538` fog that is drawn over rows 190..238
   contributes brightness in the native that the browser does not produce at
   those pixels, and in rows 80..119 the layer beneath (`0x1E2918`) is darker in
   the browser. Both are RGBA32 with 8-bit alpha in both renderers, so the next
   measurement is the same readback probe pointed at those two draws, not at
   `0x217508`.

   The reference here is RT64's own render target; a match to it is a browser
   result and not a statement about retail hardware.

8. **One untested browser-side candidate, named.** `0x249538` is 200 texels
   wide (`stride=800`, `lrs=796`) and its draw's `s` runs to 320 texels, so the
   rect samples past the end of the image. The browser implements only
   `GL_REPEAT`/`GL_CLAMP_TO_EDGE` chosen from `cms`/`masks == 0`
   (`app/src/web_renderer.cpp:4279-4282`) and does not apply the RDP's
   `masks`/`cms` masking to the tile window. The `0x217508` image is 320 wide
   and never crosses its window, which is why the probed draw cannot show that
   fault. This is the cheapest thing to check next.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | the tagged probe (`DrawCmd::tex_img_addr`, the per-flush GL-state line, the row readback with the alpha/frag passes, the fill line, and the `u_debug_mode == 3` shader branch) was added for the measurements and then removed. The tree is back to session 156's content: `grep -rn probe157 app/ debug/` is empty and `make web` is clean. No renderer behaviour was changed: no blend mapping, no alpha path, no shader output. |
| `docs/HANDOFF-2026-10-17-session157.md` | this file |
| `docs/proofs/` | no new image. No pixel moved: the only source change was the probe, and it is reverted (`app/src/web_renderer.cpp` is back to 5541 lines, session 156's count). The band evidence is the alpha-bucket table in §6 and session 156's three `web-title-clouds-*` PNGs, which still show the before state. |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, decision row; item 8 sub-item (n) records the probe results and the next experiment |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or `tools/RT64` was
changed. `tools/RT64` still carries session 150's uncommitted fill clip. No
commit was made.

## What was run for verification

- `grep -rn probe156 app/ debug/` — no output, exit 1.
- `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1`
  — `meanAbs=[2.08, 2.23, 2.74] meanSigned=[-2.03, -2.17, -2.67] exact=79.2% over 318x238`.
- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` after the probe revert —
  `[100%] Built target ogrebattle64`, `web: build-wasm/ogrebattle64.wasm is ready; ...`.
- `grep -rn probe157 app/ debug/` after the revert — no output, exit 1.
- `node debug/probes/shots.cjs --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1' --attempts 2 --secs 200 --shots 2 --gap 400 --out /tmp/w157/probe4`
  — `CAPTURED 2 frames (attempt 1)`; the probe lines quoted in §3-§5 are from
  `/tmp/w157/probe4-milestones.txt`. The first probe run had `OGRE_TEX_TRACE=1`
  in the URL (`/tmp/w157/probe-milestones.txt`), which is where the browser's
  ordered `img=`/rect list in §5 comes from.
- `node debug/probes/boot.cjs --attempts 2 --secs 45 --out /tmp/w157/boot-final`
  on the reverted build — `attempt 1: sawRealDl=true rendered=true`,
  `RENDERED {..."total":307200,"nonBlack":25136,"colorful":18868,...}`
  (`nonBlack` is the same number session 156 recorded).
- The `0x249538` / `0x1E2918` / `0x217508` formats and windows were read from
  the same two traces: `grep "GFX-TEXTRACE" ... | grep 249538` on the browser
  side and `grep "\[tex\] dl=2 .*249538" /tmp/w156/nat-trace2.log` on the native
  side; both give `y126..131 s0 t0 win0,0..796,16` and `line50`/`stride800`.

There is no repo test harness; each check is a ROM-dependent run. The native
series and the browser series are session 156's, still under `/tmp/w156`, and
were reused rather than recaptured: the tree's rendered output is unchanged (no
renderer edit survived), so the comparison is the same measurement.

## Open

1. **Point the row-readback probe at the two other partial-alpha layers.**
   `0x249538` (rows 126..238, drawn last over the cloud) and `0x1E2918`
   (rows 24..129, drawn under it) are the layers whose contributions the
   measurements of §6 implicate. The probe to reuse is the one this session
   wrote: state line, row before/after, `u_debug_mode == 3` alpha pass, normal
   pass, then `alpha*frag + (1-alpha)*dst` against the result. One build, one
   browser run, and it names the layer that differs.
2. **Tile-window masking.** The browser's sampler maps a tile's window to
   `GL_REPEAT`/`GL_CLAMP_TO_EDGE` and ignores `masks`/`cms` masking. `0x249538`
   is 200 texels wide but its draw's `s` runs to 320 texels, so its rows in
   190..238 are the only place in this list where the window and the sample
   range disagree. If the probe in item 1 shows the fog's sampled texel or alpha
   is wrong at those rows, this is the mechanism to implement against
   `RDP::sampleTexel` in `tools/RT64` (`mask`/`cm` are applied to `s`/`t`
   before the TMEM fetch).
3. Session 156's item 2 stays open and unexplained: the full-screen `0x249538`
   `G_TEXRECT` with `s=t=dsdx=dtdy=0` and the 319x239 draw of the same image are
   skipped by both renderers, so they are a question about which colour image
   those draws target, not a known difference.
4. The remaining browser-renderer items are unchanged: the LOADTLUT `tmem` key
   (needs a reference fact), the canvas's rounded corners and unwritten last
   column/row (cosmetic, needs developer intent), and the screens not compared
   against the reference.

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
