# Session 195 handoff — a rectangle samples the texel under its pixel

## Goal

Continue session 194's open items 1 and 2 on the mission briefing's green
hairlines and the frame's own green outline.

## Result

1. **The cause is the browser renderer's rectangle sample coordinate, and
   session 194's mechanism reading is corrected.** RT64 draws every rectangle
   shifted half a pixel right and down: `rt64_framebuffer_renderer.cpp` sets
   `halfPixelOffset = { 1.0f / framebuffer.viewport.width, -1.0f /
   framebuffer.viewport.height }` (one pixel in NDC is `2 / width`, so this is
   one half pixel), `triangles.screenOffset = halfPixelOffset` is the default
   (`:1571-1572`), and a `Projection::Type::Rectangle` call replaces the scale
   with the rectangle's size and adds the half pixel (`:1768-1770`).
   `RasterVS.hlsl` skips the coordinate transform for a rectangle and then
   applies `ndcPos.xy = ndcPos.xy * screenScale + screenOffset * ndcPos.w`
   (`w` is 1 for a rectangle), so the rectangle's quad
   edge sits half a pixel right and down of the RDP's rect while the corner UVs
   stay on `uls / 32` and `lrs / 32` (`RDP::drawRect`, `rt64_rdp.cpp:1561-1568`).
   Pixel `i` of a 1:1 `G_TEXRECT` therefore samples texel `i`.

   The browser renderer put the corner UVs on the rectangle's edges and its
   fragments sample at pixel centres, so pixel `i` sampled `us + (i + 0.5) *
   step`. Every filtered rectangle was half a texel low and right. On the
   briefing's 4-row bands the last row read the row past the band's art (session
   194's seam hairlines), and the image's right and bottom edges mixed its
   transparent green surround (`0x07C0`) into the opaque art (the frame's own
   green outline and the pin's, which a wrap bound cannot reach).

2. **`OGRE_RECT_HALFPIX` (on by default) subtracts half a step from the four
   corner UVs** of `draw_texrect` and of the S2DEX object rectangle
   (`draw_obj_rect`). Subtracting half a step is the same sample coordinate as
   RT64's shifted quad for any scale, and it leaves the rectangle's coverage
   unchanged; shifting the quad instead would put pixel `i`'s centre exactly on
   the quad's edge, where the fill rule decides. `=0` restores the old
   behaviour. RT64 turns an S2DEX object rectangle into a `drawTexRect`
   (`rt64_gbi_s2dex.cpp` `drawObjRectangle`, `:634-636`), so that path gets the
   same correction; a point-sampled rectangle is unchanged because
   `floor(i + 0.5)` and `floor(i)` are the same texel.

3. **The triangle path already had this offset, which is why only rectangles
   were wrong.** `transform_vertex` (`app/src/web_renderer.cpp`) adds
   `1.0f / screen_width` to NDC x and subtracts `1.0f / screen_height` from NDC
   y before writing the vertex, with a comment that names RT64's
   `halfPixelOffset`; its note says "the browser's rect vertices already carry
   the same convention". They did not: the rectangle paths build their own
   corner UVs and never went through `transform_vertex`. The comment is
   corrected to point at `OGRE_RECT_HALFPIX`.

4. **Session 194's two open items are closed.** The band seams, the notch
   stripes, the frame's own outline and the pin's are all gone. The
   `OGRE_TEXWINCLAMP` question is answered: it bounded the symptom (a wrapped
   read past the band), it is not the fix, and it stays off by default. It is
   still a useful diagnostic and was not removed.

5. **Native runs reach the briefing, and session 194's map landings are
   explained.** With a *fresh* `OGRE_PREF_DIR` and the route
   `OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6"` the native build enters scene
   `0x03` at `t=39419ms` and stays on the briefing. Session 194's native runs
   that landed on the map (scene `0x05`) reused `/tmp/s194pref/saves/…bin`, which
   the game had already rewritten and which differs from
   `assets/escort_suspend.bin` (md5 `f9d83841…` against `1fc64435…`), so they
   loaded a different save. A clean pref dir imports the repo save and the route
   works in both renderers.

## Evidence

- **The reference's own framebuffer samples the texel under the pixel.** A
  native run with `OGRE_DUMP_RDRAM` at exit gives RT64's 320x240 framebuffer of
  the briefing (`[vi] fbAddr=0x000400`; the dump's frame is at `0x400` in the
  `off ^ 2` halfword order). Of the 213 fully opaque samples of the top band
  (`x 114..204`, `y 72..75`), 144 equal the tile's decoded texel exactly and the
  other 69 differ by exactly one 5-bit step in one channel — the RDRAM
  writeback's `G_CD` dither. None is a half-texel mixture.
- **The port's frame before the fix was the half-texel mixture.** With
  `OGRE_RECT_HALFPIX=0` the same 213 samples are exact in 0 of 213, and 136 equal
  the 3-point bilinear prediction at fraction `(0.5, 0.5)`, `(s01 + s10) / 2`
  (`sample_unit` in `app/src/web_renderer.cpp`); the rest are mixed with
  neighbours whose alpha is not 1.
- **With the fix the port's frame is the decoded texel**: 213 of 213 exact.
- **Green-pixel counts** in the illustration rectangle (`x 112..207`,
  `y 70..151` of the 320x240 frame; a pixel is green-dominant when `g > 90`,
  `g > r + 40`, `g > b + 40`): `0` with the switch on, `398` with it off, of
  7872. The seam rows alone (every fourth row from `y=75`) are `0` against
  `109`. Session 194's frames for comparison: 230 (wrap mask) and 1942 (alpha
  blending forced off).
- **Against RT64's framebuffer, region by region** (canvas pixel
  `(2x + 1, 2y + 2)`, every game pixel of the region, two on runs and two off
  runs):

  | region (320x240) | mean abs, switch on | mean abs, switch off |
  |---|---|---|
  | illustration, `112,70..208,152` | `2.44` / `2.44` | `22.21` / `22.20` |
  | NOTE band, `110,20..300,60` | `3.96` / `3.95` | `3.95` / `3.95` |
  | map upper, `0,90..320,160` | `3.02` / `3.06` | `8.06` / `7.94` |
  | map lower, `0,160..320,190` | `3.09` / `3.09` | `3.09` / `3.09` |
  | `MISSION` band, `0,190..320,220` | `20.13` / `21.89` | `21.68` / `21.85` |

  The two on-run values and the two off-run values are the same build run
  twice, so the spread in the last row is the run-to-run dither phase: two runs
  of one build differ by one 8-bit step in one channel on about 40% of the
  pixels of a static map band, which is the `G_CD` noise dither's seed. The
  illustration and the map's upper band both improve, by about 20 and 5
  mean-abs units; the NOTE band and the map's lower band are unchanged to two
  decimals.
- **The A/B is one build, one route, one screen.** The off proof is the same
  commit with `OGRE_RECT_HALFPIX=0`; the two runs' page logs both end at scene
  `0x03` with two `[GFX-FRAME-DUMP]` lines.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean after the change.
- `node debug/probes/rectab.cjs --out /tmp/w195/rect-on2` and the same with
  `--query '…&OGRE_RECT_HALFPIX=0'` (`--out /tmp/w195/rect-off2`), both
  `FRAME DUMPS 2`; the probes take 100 s to 3 min of wall clock depending on
  host load, because the browser reaches the briefing after the game's own boot.
- The pixel comparisons above (native framebuffer, both browser frames) were
  computed offline from `nat-briefing.bin` and the two PNGs; `debug/out` holds
  nothing new. The proof PNGs are the pair from the build that has the code
  change; the last rebuild changed comments only, and the same probe on that
  binary (`/tmp/w195/rect-final2-briefing.png`) reads 0 green pixels, 213 of 213
  exact samples and 2.45 mean absolute difference in the illustration.
- A second screen with the switch on: the Load Game book (scene `0x12`, the same
  slot data as RT64's capture of that screen) renders with crisp text and clean
  ornamental borders (`/tmp/w195/field-on-01.png`). No measured A/B on it: the
  off run's timed shots caught a fade to black. The change is derived from
  RT64's own sample coordinate, so a new difference on a filtered rectangle is a
  `OGRE_RECT_HALFPIX=0` A/B first.
- The synthetic `testdraw` controls were not measurable on this host: `node
  debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` reports
  `[web:gfx] Only the boot blanking display list has been submitted` and a black
  canvas on every attempt, and the same failure is in `debug/out/testdraw-*`
  from before this session and with `OGRE_RECT_HALFPIX=0`. The frame dump's
  own texel comparison above is the control that was used instead.
- `git status --short` shows `app/src/web_renderer.cpp`, `debug/probes/rectab.cjs`,
  the docs and the proofs. `tools/RT64` carries the local modifications it had
  before this session (the `OGRE_*` diagnostics in the submodule); this session
  added nothing there.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `rect_halfpix_` member and its `OGRE_RECT_HALFPIX` read (default on); the corner-UV shift in `draw_texrect`; the step shift in `draw_obj_rect`; the `OGRE_TEXWINCLAMP` comment now names the real cause |
| `debug/probes/rectab.cjs` | one screenshot of the briefing per run, signalled by the strip draw's `[GFX-FRAME-DUMP]` line; the `OGRE_RECT_HALFPIX` A/B probe |
| `docs/proofs/web-mission-briefing-recthalfpix-on-session195.png`, `-off-session195.png`, `-on-zoom-session195.png`, `-off-zoom-session195.png` | the same screen and build with the switch on and off, plus the illustration zoomed 6x |
| `docs/proofs/native-mission-briefing-session195.png` | RT64's 320x240 framebuffer of the briefing from an exit `OGRE_DUMP_RDRAM` (the sampling reference) |
| `docs/proofs/native-mission-briefing-present-session195.png` | the same run's 960x720 presented frame (3x), as the eye sees it |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/guides/web-probes.md` | status, log, the session-195 durable row plus the session-194 banner, index, the `OGRE_RECT_HALFPIX` row, the `OGRE_TEXWINCLAMP` correction and the `rectab.cjs` row |
| `docs/HANDOFF-2026-10-25-session195.md` | this file |

## Probes used and reverted

None in generated code or in `tools/RT64`: every measurement is a knob, a probe
or a native capture. `OGRE_RECT_HALFPIX` and `rectab.cjs` stay as diagnostics.

## Open

1. **The other filtered 2D screens with the switch on.** The change is RT64's own
   convention, but the screens whose texels it moves have not each been measured:
   the `OGRE_FOG` repaired layers, the world map's water and scrolling overlays,
   the title's logo band, the map's other scrolling layers and the font's
   texrects. A `OGRE_RECT_HALFPIX=0` A/B on each is the check, and the title has
   native captures to compare against. The njpeg/S2DEX path is excluded by
   measurement: its macroblock rectangles are `G_CYC_COPY`
   (`[rect-state] rect 0,0..64,64 cycle=0 ... fmt=1 siz=2 line=2 tmem=0 uls=0
   lrt=60`, from a native run of the seeded opening at `OGRE_SPEED=8
   OGRE_SCENE=new-game OGRE_STEP=2 OGRE_RECT_STATE=0-16`), and a COPY draw is
   point-sampled in both renderers, where `floor(i + 0.5)` and `floor(i)` select
   the same texel.
2. **The mission field is still flatter and darker than native.** Its light is
   blue (`amb=77,77,127`) and the mission is 3D; it is not explained by any
   rectangle path. Session 193's item 3 stands.
3. **The browser's framebuffer-as-texture copy (`OGRE_FBTEX`) still needs RT64's
   re-upload and per-workload writeback** before it can be enabled (session 193's
   item 2). Unchanged by this session.
