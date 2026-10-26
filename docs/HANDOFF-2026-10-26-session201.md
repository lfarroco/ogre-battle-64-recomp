# Session 201 handoff — the practice field's ground: near-plane clipping, then perspective-correct UVs

## Goal

Fix the tutorial practice field's flat grass (sessions 198-199: the ground sampled
no texture and the visible surface was a fill).

## Result

Two renderer defects, in this order. Both are in the browser renderer
(`app/src/web_renderer.cpp`); the native RT64 path is untouched.

1. **A triangle that crosses the eye plane was dropped whole.** `transform_vertex`
   returned false for any vertex with clip `w <= 0`, and `draw_tri` dropped the
   whole triangle. `OGRE_DRAW_PROBE=12,160 OGRE_DRAW_PROBE_SCENE=0x3
   OGRE_DRAW_PROBE_AFTER_TASK=1300` printed the two triangles that paint the
   practice field:

   ```
   [GFX-REJPROBE] rejected: v0=(-38,0,-38)w=32.64 v1=(38,0,38)w=-1.67 v2=(38,0,-38)w=32.64 tex=0 comb=FC127E24
   [GFX-REJPROBE] rejected: v0=(-38,0,-38)w=32.64 v1=(-38,0,38)w=-1.67 v2=(38,0,38)w=-1.67 tex=0 comb=FC127E24
   ```

   That is the two halves of one 76x76-unit ground quad, whose near corner is at
   `w = -1.67` (behind the eye). Both were dropped every frame, so the field's
   ground was never drawn and the frame kept the ground fill. The renderer now
   cuts the triangle against `w > 1e-3` (`emit_clipped_tri`) and emits the
   resulting triangle or quad; attributes at the cut vertices are interpolated
   perspective-correctly (`attr/w` and `1/w`) and the fog alpha is recomputed from
   the cut vertex's own clip position. Depth is unchanged: `project_clip` derives
   the window z from the cut vertex's clip position exactly as before.

2. **The texture coordinate was interpolated in screen space.** With clipping in,
   the ground draws but its tile is stretched into straight bands of green, while
   the cliffs and roads (small triangles) look right. RT64 interpolates the
   coordinate perspective-correctly: `hle/rt64_rsp.cpp:748-755` stores
   `s * sc / (65536 * 32)` undivided and `shaders/RasterVS.hlsl` multiplies the
   position by `w` before the rasterizer. The browser's vertex shader wrote
   `gl_Position = vec4(a_pos, a_z, 1.0)` (session 172) and interpolated in screen
   space. The shader now writes `vec4(a_pos * a_w, a_z * a_w, a_w)` by default,
   which is RT64's rule; `OGRE_PERSP=0` restores the old screen-space path. The
   depth is unchanged for the same reason as above (`a_z * w` maps back to the
   same window depth).

   The developer's report after (1) was "the grass has straight textures lines
   (different lines of green), but the original one is noisy. the mountains have
   perfect textures, but the grass not, the roads are ok" — which is what (2)
   explains: only the ground quad has a large spread of `w` (32.64 to -1.67).

Result frame: `docs/proofs/web-tutorial-field-session201.png`. The three-frame
series is `-flat-session201.png` (before, a fill), `-bands-session201.png` (after
clipping, screen-space UVs) and `-session201.png` (final, perspective UVs).

## Evidence

- **The dropped triangles are the ground quad.** The probe line above, 12 lines
  over two frames, `comb=FC127E24` and `tex=0` at the frame's first draws.
- **Grass detail, same pixel region, one build per state**
  (guest x 20..120, y 150..170, measured on the 640x481 captures; neighbour-pixel
  mean absolute difference in parentheses):
  - before (fill): 341 distinct colours, `(33,33,0)` on 904 of 4000 pixels, 1.77,
  - after clipping, screen-space UVs: 428 distinct, 2.48,
  - after clipping, perspective UVs: 441 distinct, 2.38,
  - native reference `docs/proofs/native-tutorial-practice-field.png` (3x scale,
  x 200..300, y 460..500): 726 distinct, 2.32.
- **The field triangle now covers the pixel.** `OGRE_DRAW_PROBE` at `12,160`
  lists one covering triangle per frame in the settled screen (the whole-field
  overlay `comb=FCFF99FF`, a 64x64 CI8 tile at `0x37E5A8`, `PRIM` colour with
  `alpha = TEXEL0_A * SHADE_A`, `shade a=0.73`), and the *terrain* family's bbox
  hits with `tri_cover=0` before the clip fix. The overlay is a separate layer of
  the same screen: its combiner alpha is ~0 at the field, so it adds nothing.
- **Perspective does not regress the mission briefing.** One build, `OGRE_PERSP=1`
  against the default screen-space run, same save and route
  (`assets/escort_suspend.bin`, `title:start:6,0x12:a:6`), mean absolute difference
  over the frame's 640x481 capture: field upper band 1.43, field lower band 3.81,
  the framed illustration 0.55; the `MISSION` band's 69.11 is its scroll phase.
  Session 172's "with `w` in `gl_Position` the mission's top cliff goes 0.93 ->
  10.27" was measured before near-plane clipping existed, so it does not speak to
  this shader state.
- **Title/attract unchanged.** `OGRE_PERSP` on against off at the forced title:
  mean absolute difference 1.42 over the frame, same clouds and coastline.
- **Build.** `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final
  source; every capture above is from that build.

## Verification

- `node debug/probes/scene.cjs --route tutorial-field --points '12,160'` on the
  final build reaches scene `0x0003` and writes
  `docs/proofs/web-tutorial-field-session201.png`.
- `--route title` and `--route mission-briefing` on the same build, as above.
- The probe family census, the alpha view and the isolation switch each produced
  the evidence quoted here (see the new knobs below).

## Diagnostics added (kept; they are toolkit, not probes)

`app/src/web_renderer.cpp`:

| knob | what it does |
|---|---|
| `OGRE_DRAW_PROBE=x,y` | logs every draw whose screen span covers that target pixel: kind, screen span, tile, rect, masks/shifts, combiner, PRIM, and for a triangle the UV/shade/z range and the decoded mux |
| `OGRE_DRAW_PROBE_SCENE=<id>` | limits the probe to one scene |
| `OGRE_DRAW_PROBE_AFTER_TASK=<n>` | counts from the start but logs only after that task, so the log holds the settled screen |
| `OGRE_ONLY_COMB=0x........` | draws only the triangles whose combiner low word matches, over the normal fills and rectangles (shows where one draw family lands) |
| `ogre_gfx_debug_flags(1<<17)` | fragment output becomes `(combiner alpha, blender alpha, 0)`, so an alpha-map texture is visible directly |
| `OGRE_PERSP=0` | the A/B for the perspective-correct interpolation |

The family census (`[GFX-FAMILY]`) prints, for the probe's scene, one line per
(kind, textured, tile image, combiner) family with count, on-screen count, bbox
and depth range for the first eight non-empty flushes.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `ClipVertex`/`vertex_clip`/`project_clip`/`fog_vertex_alpha`, `emit_triangle` split out of `draw_tri`, `emit_clipped_tri` (near-plane cut), perspective-correct clip position in the vertex shader with `OGRE_PERSP`, and the draw probe / family census / isolation / alpha view |
| `docs/proofs/web-tutorial-field-session201.png`, `-flat-session201.png`, `-bands-session201.png` | the three states |
| `docs/HANDOFF-2026-10-26-session201.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md` | record and knobs |

No probe was added to generated code; `RecompiledFuncs/`, `Bank*Funcs/` and
`tools/RT64` are unchanged.

## Open

- The whole-field overlay (`comb=FCFF99FF`, tile `0x37E5A8`) contributes alpha ~0
  across the practice field, so its mottling never appears. With the ground now
  textured this is no longer visible as a flat field, but whether that overlay is
  meant to shade the field is unmeasured.
- Session 172's screen-space choice is now reversed for triangles. Any screen
  whose texture alignment was tuned against it (the mission briefing's picture
  strips, the title) measured within a few units of difference in this session's
  A/B; a wider sweep is still worth a run.
- The scripted `tutorial-field` route is not deterministic: two runs this session
  stopped before the field's ground was on screen (`tri_cover=0` where the settled
  screen has it), so a "no change" result from that route is not trustworthy
  without the probe's covering-draw count.
