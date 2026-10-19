# Session 164 handoff — the browser samples tiles with RT64's sampler, and the fog is point-sampled

## Goal

Continue the browser (WebAssembly) renderer from session 163, taking RT64's
renderer as the reference. Session 163's open item 1 is the title/story fog
band (0.27 in rows 64..119) and its proposed next measurement is the sampled
intensity and blend alpha of one repaired fog group. The browser sampled every
tile with the GL fixed-function sampler: filtering was a property of the
uploaded texture, there was no tile-window clamp or `masks`/`maskt` wrap/mirror
beyond repeating the whole texture, no `G_TF_AVERAGE`, and no hardware
quantization of the interpolated coordinate. RT64 implements all of that in
`src/shaders/TextureSampler.hlsli`. So the session ported that sampler and used
it to measure what the screens actually sample.

Timebox: 100 minutes.

## Result

1. **The sampler is RT64's, in the fragment shader.** `web_renderer.cpp` now
   samples with `texelFetch` and filters in fp32:

   - `clampWrapMirrorSample`: clamp to the tile window per axis under
     `G_TX_CLAMP`, then modulo the mask size, then mirror on `G_TX_MIRROR`.
     `masks`/`maskt` are the wrap sizes `1 << masks` and a tile with no mask is
     forced to clamp, which is what `State::setupDrawCall` writes into
     `RDPTile` (`(tile.masks > 0) ? (1 << tile.masks) : 0`, and `cms` gains
     `G_TX_CLAMP` when `masks == 0`).
   - `sampleTextureLevel`'s filter selection: point when `G_TF_POINT` or
     `G_CYC_COPY`, otherwise bilinear, and the average of four texels when the
     coordinate is within 1/128 of a texel centre under `G_TF_AVERAGE`.
   - RT64's default weighting: `threePointFiltering = true` in
     `rt64_user_configuration.cpp`, so the three-point formula
     (`tri0/tri1` with `step(1, frac.x + frac.y)`), not plain bilinear.
   - `SIMULATE_LOW_PRECISION`: the interpolated coordinate is rounded to 1/128
     texel before the tile origin is subtracted.

   `DrawCmd` gained the per-unit tile state (`RDPTile`): the decoded image's
   texel size, the tile origin, `cms`/`cmt`, the wrap sizes, and `G_TF_*` from
   OTHERMODE_H. The filter is therefore draw state now; the old code decided it
   per texture at decode time, so a texture shared by a bilerp and a point draw
   kept whichever decode ran last.

   `OGRE_TEXF` selects the path: `rt64` (default), `1`/`bilinear` (the same
   shader sampler with true bilinear, `u_texf == 1`), and `gl` (the
   fixed-function sampler, restored for the A/B). `[RENDERER] texture sampler:`
   states which one a run used.

2. **The screens are measured, and the title samples point.** The new
   `[GFX-SAMPLER]` census counts textured draws by filter and how many sample a
   masked tile:

   ```
   [GFX-SAMPLER] textured draws=591 point=591 bilerp=0 average=0 masked=456   (forced title)
   [GFX-SAMPLER] textured draws=99 point=99 bilerp=0 average=0 masked=1      (forced world map 0x05)
   [GFX-SAMPLER] textured draws=56 point=56 bilerp=0 average=0 masked=0      (publisher still)
   [GFX-SAMPLER] textured draws=8 point=2 bilerp=6 average=0 masked=6        (forced tutorial 0x17)
   ```

3. **The fog group's draw state answers session 163's open item.** The new
   `[GFX-FOG-DRAW]` line prints a repaired group's sampler state and rectangle:

   ```
   [GFX-FOG-DRAW] rect=(0,129)-(320,130) size=64x64 filt=0 cm=(0,0) mask=(64,64)
                  s=0.00..320.00 t=105.00..106.00 orig=(0,0) key=0xFC634D46FABF0FBD
   [GFX-FOG-DRAW] rect=(0,64)-(64,128) size=64x64 filt=0 cm=(0,0) mask=(64,64)
                  s=63.00..-1.00 t=0.00..64.00 orig=(0,0) key=0xFC634D46FABF0FBD
   ```

   The fog is `filt=0`: point-sampled, one texel per pixel for the 64x64 blocks
   and a wrap every 64 texels for the 320x1 strips, so the sampled texel is the
   same in both renderers. A filter or quantization difference is excluded from
   the 1-3-level band, and the survivor is the composition the fog blends onto
   (session 158's translucent `0x217508` rows).

4. **The title's point-sampled frames are unchanged in magnitude.** Forced-title
   series against the native 397-frame fog-on series (`/tmp/w162/nat-fog`, the
   same reference sessions 162/163 used), best pair over the whole series:

   | run | best native target | meanAbs | bands 0-63 / 64-119 / 120-239 |
   |---|---|---|---|
   | pre-change (`/tmp/w164/base-*`) | `t.125` | 0.137 | 0.014 / 0.069 / 0.235 |
   | pre-change | `t.182` | 0.162 | 0.014 / 0.069 / 0.285 |
   | pre-change | `t.275` | 0.104 | 0.044 / 0.267 / 0.059 |
   | RT64 sampler (`/tmp/w164/rt64-00`) | `t.167` | 0.134 | 0.014 / 0.070 / 0.227 |
   | RT64 sampler (`/tmp/w164/rtctl-00`) | `t.455` | 0.166 | 0.045 / 0.328 / 0.154 |
   | `OGRE_TEXF=gl` (`/tmp/w164/glctl-00`) | `t.476` | 0.201 | 0.045 / 0.396 / 0.194 |

   The band moves with the fog's scroll phase (0.069 at `t.125`/`t.182`, 0.27-0.40
   at `t.275`-`t.476`) in both builds, and the two samplers land on different
   phases because each run presents its own instant. That is why the sampler
   cannot be read as a fix or a regression here: these draws are all point.

5. **The tutorial is the reached bilinear screen, and its draw is now covered.**
   Its background is a 64x64 tiling I8 image drawn as two triangles sampling
   `s` 0..447 and `t` 0..335 texels over 320x240, with `masks=6 maskt=6`,
   `cm=(0,0)` wrap and `filt=2`. That is a draw the new rules apply to, and it
   is the first reached draw where they can matter.

6. **The tutorial's dialogue box is broken, and it is not the sampler.** The
   forced tutorial renders the background and window frame but its portrait is
   torn into stripes and the glyphs are black blocks with fragments around them;
   RT64's target of the same forced scene is a clean Deneb portrait and readable
   text. The defect is identical under `OGRE_TEXF=gl`, so it predates the port.
   Recorded as open work 8(r) with `docs/proofs/web-tutorial-browser-vs-native.png`
   and `web-tutorial-sampler-ab.png`. The forced scene has no pre-state (AGENTS
   rule 13), so it is not yet established as a renderer bug.

7. **No regression on the checked screens.** `boot.cjs` is `RENDERED`
   (`nonBlack=25376`), `testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`
   exits 0 with `fbtex hits=1 lines=1` and after-batch `nonBlack=3024`, and the
   title series is in the table above. The publisher comparison was not
   reproduced: the runs that reached the scene were the fill-stage capture
   (`/tmp/w164/pub3-00.png`, 639x475 canvas, mid-draw) and every other attempt
   either stalled on the idle trajectory or landed on another scene. The
   publisher's census above is from the fill-stage run, and it is all point with
   no masks, so the sampler has nothing to change there; the 100% measurement
   from session 163 stands unrefuted and un-re-measured.

## What changed, in one paragraph

`GlFramebuffer`/`TexUpload`/`DrawCmd` keep the same shape except `DrawCmd`'s new
sampler fields (`tex_size[2][2]`, `tex_cms`/`tex_cmt`/`tex_masks`/`tex_maskt`
per unit, `tex_filter`). `record_state` fills them from `TileState` with RT64's
normalization (`masks == 0` forces clamp, `masks` becomes the wrap size).
`flush_commands` uploads textures as RGBA8 with point filtering, sets the new
per-draw uniforms (`u_tsize*`, `u_torig*`, `u_tcm*`, `u_tmask*`, `u_tex_filter`,
`u_texf`) and, for `OGRE_TEXF=gl`, the per-draw GL filter on both units. The
vertex shader passes the interpolated texel coordinate; the fragment shader owns
the origin, the scale and the filtering. `blit_to_canvas` (the present path)
sets `u_texf=0` with a unit-sized, clamp-only tile because it samples normalized
UVs. `ensure_gl` reads `OGRE_TEXF` and logs the choice. `[GFX-SAMPLER]` prints
the census on the first textured batch and every 120 flushes, `[GFX-FOG-DRAW]`
prints a repaired fog group's sampler state (first 8), and `[GFX-UV]` gained
`filt=`, `size0=`, `cm0=`/`cm1=` and `mask0=`/`mask1=`.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | RT64 sampler in the fragment shader (`tex_modulo`, `sampler_texel`, `sample_unit`, `fixed_texel`, `tex_val`); `DrawCmd` sampler fields and `record_state` fill; per-draw sampler uniforms; `OGRE_TEXF` with the `texf_` mode; uploads point-filtered; present path on the GL sampler; `[GFX-SAMPLER]`, `[GFX-FOG-DRAW]`, extended `[GFX-UV]`; `fog_repaired` on `ExecCtx` and `fog_draw_logged_` |
| `debug/probes/testdraw.cjs` | the render witness is the screenshot *or* the `[GFX-AFTER]` canvas readback right after the batch, because a later present can overwrite the canvas with the game's blank frame |
| `debug/probes/fbtex.cjs` | each entry reports the run's last `[GFX-SAMPLER]` census beside its framebuffer-as-texture hits |
| `docs/HANDOFF-2026-10-17-session164.md` | this file |
| `PLAN.md` | Status bullet **Browser texture sampler**; the session-164 paragraph; correction to session 163's fog next step; open work 8(q) and 8(r) |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | log entry, index row, decision row |
| `docs/guides/app-build.md` | `OGRE_TEXF` paragraph and the `[GFX-SAMPLER]`/`[GFX-FOG-DRAW]` diagnostics |
| `docs/guides/web-probes.md` | `testdraw` witness, `fbtex` sampler census |
| `docs/proofs/web-tutorial-browser-vs-native.png` | browser tutorial (RT64 sampler) left, RT64 target right |
| `docs/proofs/web-tutorial-sampler-ab.png` | the forced tutorial with `OGRE_TEXF=rt64` left and `gl` right (the background scrolls between runs; the portrait and text box are the comparison) |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made. No probe was added:
`grep -rn probe164 app/ debug/` is empty.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `[100%] Built target
  ogrebattle64`, `web: build-wasm/ogrebattle64.wasm is ready`. The first builds
  failed at shader compile and the page rendered black; the three GLSL ES 3.00
  rules the ported code had to satisfy are a non-constant `const ivec2`, a
  non-constant `const bool`, and the reserved `gl_` prefix (the shader log was
  read out of `window.ogreLog` with a small script, because the harness only
  keeps `[GFX-]`/`[RENDERER]` milestone lines).
- Title series, RT64 sampler: `OGRE_URL='http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_FOG_SUMMARY=1'
  node debug/probes/shots.cjs --shots 24 --gap 400 --out /tmp/w164/rt64`, and the
  same with `&OGRE_TEXF=gl` (`/tmp/w164/gl`). Best-pair numbers use
  `python3 /tmp/w162/torgb.py <canvas> <rgb>` and `/tmp/w161/cmp3 <rgb>
  /tmp/w162/nat-fog` (the comparator's own math, a 397-target search per frame).
- Fog draw state: `OGRE_FOG_SUMMARY=1` title run, `[GFX-FOG-DRAW]` and
  `[GFX-FOG-SUMMARY]` lines (`/tmp/w164/fogsum-milestones.txt`).
- Census: `[GFX-SAMPLER]` in `/tmp/w164/rtctl-milestones.txt` (title),
  `/tmp/w164/map-milestones.txt` (0x05), `/tmp/w164/pub3-a1-pagelog.txt`
  (publisher), `/tmp/w164/tut2-milestones.txt` (0x17).
- Tutorial A/B: `shots.cjs --shots 6` on
  `?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1` (`/tmp/w164/brt17`) and the same with
  `&OGRE_TEXF=gl` (`/tmp/w164/bgl17`); the RT64 reference is a native capture,
  `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=0x17
  OGRE_CAPTURE_TARGET=/tmp/w164/nat17/t OGRE_CAPTURE_AFTER=100
  OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=14000 ./build-dist/ogrebattle64
  assets/ogre64.z64`, converted with `tools/rt64_target.py`.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — `RENDERED`
  (`nonBlack=25376`).
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`, `after-batch nonBlack=3024`.
- `node debug/probes/fbtex.cjs --scenes 0x04,...,0x18 --secs 16` — all entries
  idle (`tasks=0`) on a machine whose load average was 6-15 during the session;
  the workload-analyzer counter is the browser boot's own pacing, and this
  environment stalled most boots. The per-scene census is therefore taken from
  the individual runs above, not from a full sweep.
- `grep -rn probe164 app/ debug/` — no output, exit 1.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The tile's `shifts`/`shiftt` are still not part of the browser's UV scale.**
   The browser pre-scales a vertex's `s`/`t` with the RSP's `G_TEXTURE` scale on
   the CPU and the sampler works in that space; RT64's sampler multiplies the
   interpolated coordinate by `ShiftScaleMap[rdpTile.shifts]`. Every measured
   window has agreed with RT64's (`OGRE_TEX_TRACE` compares), and the tutorial's
   `[GFX-V]` shows `sc=32768 tc=32768` with the tile's own shift fields unread,
   so this is recorded as a difference to check on a screen where the two
   disagree rather than changed blind.
2. **The fog band is now a composition question, not a sampling one.** The fog
   groups are point-sampled 1:1 (or wrapped every 64 texels), so their texels
   are exact in both renderers. The band varies with the scroll phase (0.069 to
   0.40 at different native targets) and is the translucent fog edge blending
   over the `0x217508` cloud rows. The next instrument is one draw's output
   readback inside each renderer on a row where the fog's alpha is partial, not
   another format or sampler change.
3. **`OGRE_FOG_SCALE` is still not ported** (session 162, open item 2).
4. **The browser presents one instant per capture window** on this build, so
   phase-matched A/B needs several runs; `fps.cjs`/`vi.cjs` on the current build
   are where a faster presenter starts (session 163, open item 2).
5. **The tutorial `0x17` dialogue box** (open work 8(r)): identical under both
   samplers, clean in RT64's target. Check the natural route or a save first; a
   forced scene has no pre-state.
6. The screens not compared against the reference are unchanged from session
   163: the publisher still is exact by session 163's measurement and the
   boot/story/fill checks pass.
