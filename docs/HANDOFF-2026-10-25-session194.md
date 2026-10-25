# Session 194 handoff — the briefing's green hairlines are a band sampled one row past its art

## Goal

Continue session 193's open item 1: the green lines around the mission
briefing's framed illustration. Session 193's lead was the CI4 decode of the
1-row border strips.

## Result

1. **Session 193's lead is wrong, and the strips are RGBA16 bands.** The framed
   illustration is two RGBA16 images the game composes in RDRAM, drawn as
   `G_TEXRECT`s in scene `0x03`'s 23rd display list of the run (`dl=23`):
   - the L-shaped gold frame, 19 rects of 91x4 px, each sampling one 92x4 band
     (`img=0x2C4B70 + n*0x2E0`, `line=23`, `uls=0 ult=0 lrs=364 lrt=12`,
     `mask=7/5`, `cm=0/0`, `dtdy=1024`);
   - the castle picture, three rects of 82x24, 82x24 and 82x19
     (`img=0x2D0690`, `line=21`), drawn after the frame;
   - the pin, one 20x20 rect (`img=0x2E6790`, `line=5`).

   The images' transparent texel is not `0x0000`: it is `0x07C0`, pure green with
   alpha 0. Proofs `docs/proofs/web-mission-briefing-framesrc-session194.png`
   (the 92x76 frame: checkerboard = alpha 0) and
   `-picturesrc-session194.png` (the 84x67 picture).

2. **The load and the decode are exact.** An offline simulation of RT64's
   `RDP::loadTileOperation` / `loadToTMEMCommon` on the strip's source bytes
   reproduces the TMEM dumped at the draw over all 736 bytes the load writes (0
   bytes differ; the bytes past 0x2E0 differ only because a fresh TMEM was
   simulated). Decoding the dumped TMEM for the tile's 92x4 window reproduces the
   source texels exactly (0 of 368 differ, with the renderer's 5-to-8 bit
   conversion). The green is the game's own data, not a decode fault.

3. **The hairline is the last row of each band.** A band is 4 rows drawn over 4
   pixels, so the last pixel's centre is `t=3.5` and its bilinear taps are rows 3
   and 4. Row 4 is outside the band's art. It holds what the same TMEM base held
   before the load (on later frames, the picture blocks' rows). The band's row 3
   is green with alpha 0 and the row past it is opaque, so the filtered texel has
   alpha near 0.5 and its mixed RGB is drawn. With alpha blending forced off
   (`ogre_gfx_debug_flags(1)`) the whole transparent region turns green
   (`docs/proofs/web-mission-briefing-noablend-session194.png`); that is the
   control which shows the source texel really is green-with-alpha-0 and that the
   default path only shows it at the seams.

4. **`OGRE_TEXWINCLAMP` bounds a wrapped sample to the tile's own rect**, after
   the mask wrap and before the image clamp. It is off by default and `=1`
   enables it. Same-screen A/B:
   - off (`docs/proofs/web-mission-briefing-wrapmask-session194.png`): the
     horizontal hairlines at every band seam, the green-striped patch in the
     frame's notch, and the green right edge;
   - on (`docs/proofs/web-mission-briefing-texwin-session194.png`): the band-seam
     hairlines and the notch stripes are gone; the picture is unchanged
     elsewhere. The frame's own green outline, and the pin's, remain: those
     texels are filtered across the boundary between opaque art and the image's
     own transparent green texels, which a window bound does not reach.

5. **The developer confirms the native build shows no green hairlines** on this
   screen, and after seeing the clamp-on frame reports that some green lines
   remain around the frame. So the clamp removes one of the two mechanisms and
   not the other.

6. **New diagnostics.** `OGRE_FRAME_DUMP=1` writes, at the draw that samples a
   91x4 band, the band image's source bytes (`texsrc-<n>.bin`, guest byte order),
   the 4 KiB TMEM the load left (`tmem-<n>.bin`) and the decoded RGBA8 image the
   draw samples (`texdec-<n>.bin`). A timer dump cannot bracket this draw: the
   heap buffer is reused by later screens, and a dump taken 100 s later at the
   same address is byte-identical only by luck. `debug/probes/framedump.cjs`
   reads the three files out of the wasm filesystem, and
   `debug/probes/stripab.cjs` takes the default / no-alpha-blend / TEXEL0-raw
   frames on one screen of one run.

## Evidence

- `[GFX-FRAME-DUMP] n=0 src=0x2C4B70 len=11776 fmt=0 siz=2 line=23 tmem=0
  key=0x2E7EDFD5C728E443 decoded=1` — the strip draw, its source and its TMEM.
- The band's TMEM rows 0..3 equal source rows 0..3; row 4 at x=26 is
  `(115,247,24)` in a different image's data, where the band's row 3 is
  `(0,255,0,0)`.
- `[GFX-FRAME-RECT] rect=114,72..205,76 src=0x2C4B70 fmt=0 siz=2 line=23
  uls=0 ult=0 lrs=364 lrt=12 mask=7/5 cm=0/0 sh=0/0 s0=0 t0=0 dsdx=1024
  dtdy=1024 omh=0x002C30 oml=0x00504240 comb=FCFF9BFF/FF34FE7F
  mux=[rgb=(ZERO-ZERO)*ZERO+TEXEL0 a=(TEXEL0-ZERO)*ENV+ZERO]` — the state the
  seam is drawn with: 1 cycle, `FORCE_BL`, bilerp, alpha = `TEXEL0.a * ENV.a`.
- Green pixel counts in the illustration rect (game `x 112..207`, `y 70..151`,
  canvas at 2x): 430 with the wrap mask against 314 with the window bound. The
  remaining colour is partly the picture's own green foliage, so the number is a
  locator, not a score.
- The diff between the two frames is confined to the band seam rows (every
  fourth game row: 76, 79, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120,
  124 …) and the frame's notch.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean with the switch in
  both default positions.
- Browser route for every run in this session:
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
  with `OGRE_FRAME_DUMP=1` and, for the A/B, `OGRE_TEXWINCLAMP=1`.
- `node debug/probes/framedump.cjs` copied 6 files from 2 dumps and exited 0.
- `node debug/probes/stripab.cjs` wrote 3 screenshots, with `OGRE_TEXWINCLAMP`
  off and on.
- Native route notes for the next session: `OGRE_MOD_TEST=1 OGRE_ROM=…` is
  required or the app waits in the launcher; with the `build-app` mods installed
  the save load crashed with SIGBUS at scene `0x12`, and a clean
  `OGRE_PREF_DIR` avoided that crash but the same taps landed on scene `0x05`
  (the map) instead of `0x03` in three of four runs. `OGRE_SCENE=title` races the
  boot and took effect between 3 s and 28 s.
- `git status --short` shows only the renderer, the two new probes, the docs and
  the proofs.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `OGRE_FRAME_DUMP` (`dump_frame_source`, `[GFX-FRAME-DUMP]`); `OGRE_TEXWINCLAMP` (`LoadedImage::win_w/win_h`, `DrawCmd::tex_win`, `u_twin0/u_twin1`, the sampler bound); session 193's work is in the same file and was uncommitted |
| `debug/probes/framedump.cjs` | reads the `OGRE_FRAME_DUMP` files out of the wasm filesystem with the page log and a canvas shot |
| `debug/probes/stripab.cjs` | default / no-alpha-blend / TEXEL0-raw frames on one screen of one run |
| `docs/proofs/web-mission-briefing-wrapmask-session194.png` | the default frame: band-seam hairlines, notch stripes, green right edge |
| `docs/proofs/web-mission-briefing-texwin-session194.png` | the same screen with `OGRE_TEXWINCLAMP=1`: seams and stripes gone, the frame's own outline still green |
| `docs/proofs/web-mission-briefing-noablend-session194.png` | the control with alpha blending forced off: the whole transparent region is green |
| `docs/proofs/web-mission-briefing-framesrc-session194.png`, `-picturesrc-session194.png` | the two source images the rects sample |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/guides/web-probes.md` | status, log, decision 176, index, knob and probe rows |
| `docs/HANDOFF-2026-10-25-session194.md` | this file |

## Probes used and reverted

None. No generated code (`RecompiledFuncs/`, `Bank*Funcs/`) was touched and
`tools/RT64` is unchanged. `OGRE_FRAME_DUMP`, `framedump.cjs`, `stripab.cjs` and
`OGRE_TEXWINCLAMP` are bounded diagnostics and stay.

## Open

1. **The frame's own green outline and the pin's.** These are filtered texels at
   the boundary between opaque art and the image's transparent green texels
   (`0x07C0`, alpha 0), which a window bound does not touch. Either the reference
   does not filter there, or its transparent texels are not green. The
   discriminating datum is a native whole-RDRAM dump taken while the briefing is
   on screen: compare the same 92x60 buffer's transparent texels. The route above
   is not reliable natively (see Verification), so the developer's own save and
   pacing is the better instrument (AGENTS §11).
2. **`OGRE_TEXWINCLAMP` needs the wrapped-tile screens before it can default on.**
   It changes every tile whose rect is smaller than its wrap mask: the `OGRE_FOG`
   repaired cloud/fog layers and the world map's scrolling water are the two to
   compare with `=1` and `=0`. If they match, the default should follow the
   reference and flip to on.
3. The mission field is still flatter and darker than native; its light is blue
   (`amb=77,77,127`) and it is 3D. Session 193's item 3 stands.
