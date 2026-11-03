# Ogre Battle 64: Person of Lordly Caliber — PC Port Project

Goal: a native Windows/Linux/macOS **port** of the N64 game **Ogre Battle 64:
Person of Lordly Caliber** (USA, Rev A) that runs the full game with modern
enhancements and modding support.

**Scope note:** this is a *PC port* project — NOT a byte-exact ("matching")
decompilation. We do not reconstruct the original C source or aim for a
byte-identical ROM. We use **static recompilation** (`N64Recomp`): the ROM's MIPS
code is automatically translated to C, then compiled natively against the
`N64ModernRuntime` runtime library. This is the same approach as Zelda 64:
Recompiled and Majora's Mask / Ocarina of Time PC ports.

**Legal note:** this is a "bring your own ROM" project. No copyrighted ROM data is
committed to this repository. You must provide a dump of your own cartridge.

**Working rules for AI agents: [`AGENTS.md`](AGENTS.md).** Read it before you
start. Every rule in it is a session that lost time — especially "ask the
developer what the current scene should display instead of inferring intent from
the code" and "a previous handoff is a hypothesis, verify it at instruction
level".

**Per-session record:** `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, newest number
wins. The session-by-session status log that this file used to carry (sessions 3
to 97) is `docs/STATUS-LOG.md`.

---

## Goal

A native Windows/Linux/macOS port that runs the full game (menus, world map,
battles, cutscenes, audio, Controller Pak saves) with a modding framework.

## Browser port (WebAssembly)

The project also has a **WebAssembly / browser build**: the recompiled game plus
`N64ModernRuntime` under Emscripten. It compiles and runs the game again
(session 109). Its renderer is the WebGL2 prototype. Session 138 added S2DEX2 and
YUV16 to it, so the New Game njpeg backdrop decodes. Session 139 added RDRAM
framebuffer indirection: every `G_SETCIMG` target gets a GL framebuffer object
keyed by its guest address, the pixels are written back into RDRAM in the RDP's
RGBA5551 layout before the game reads them, and the VI presents that target, so
the assembled cathedral backdrop now appears in the browser
(`docs/proofs/web-newgame-cathedral-assembled.png`). Session 140 made the build
deployable as a static site: `make web-dist` writes `dist-web/`, one directory a
plain static host serves as-is, and `.github/workflows/pages.yml` publishes it to
GitHub Pages. The pthread build needs `SharedArrayBuffer`, which exists only on a
cross-origin-isolated page, and a static host cannot send the two response
headers that provide it, so the package carries
`app/web/coi-serviceworker.js` (coi-serviceworker v0.1.7, MIT, vendored verbatim)
as a service worker that adds them and reloads the page once. The route is
recorded in
`docs/DECISIONS.md`; the native port is the primary target and RT64 remains its
renderer. `docs/WEB-PORT-DEPLOYMENT.md` describes the current package;
`docs/WEB-PORT.md` and `docs/WEB-PORT-REPORT.md` describe the 2026-08-29
feasibility work and not the current state. The current state is this file and
`docs/HANDOFF-2026-11-01-session209.md`. The browser renderer's design is
documented in `docs/guides/web-renderer.md`.

Session 189 ported four RT64 behaviours into the browser renderer. The RDRAM
writeback now applies `G_CD` colour dither (`Float4ToRGBA16` plus
`DitherPatternValue`), choosing the most-used pattern per colour image the way
RT64's `Framebuffer::bestDitherPattern` does; a synthetic control reads
`67200`/`9600` undithered/dithered pixels on a `G_CD_MAGICSQ` target and
`76800`/`0` on `G_CD_DISABLE`. It fixed the session-181 WebGL feedback loop:
`upload_framebuffer` no longer leaves the target's texture bound, and every draw
binds the 1x1 white texture before its own, so fills are no longer rejected with
`INVALID_OPERATION` (the dither fill read `(0,0,0)` with error 502 before and
its fill colour with error 0 after). Tile images are keyed by an XXH3-64 of the
decoded RGBA bytes plus the size and wrap fields, so an unchanged reload reuses
the resident GL texture; `--flags 2048 --repeat 3` measures one upload and two
reuses, and `ogre_gfx_stats()` reports `tex: cache=N uploads=N reused=N
released=N`. An S2DEX2 audit found that OB64 uses only `0xDA`/`0xDC`; the
remaining `0x03`..`0x0B` now return instead of falling into the F3DEX2 triangle
arms. See `docs/HANDOFF-2026-10-24-session189.md`.

Session 190 split the browser tile cache into two tiers and measured both.
`tile_source_key` hashes a superset of the TMEM bytes a tile can read (the
decoder's own `(rgba32 || indexed) ? 0x7FF : 0xFFF` mask, the decoder's byte
shift, the 2 KiB palette half for an indexed tile, and `convert_k` only for
YUV16) before any decode; `decoded_tiles_` keeps the decoded pixels in 16 MiB
and a hit skips `decode_tmem_tile`. The GL texture cache is now bounded by eight
flushes, 32 MiB and 16 384 entries instead of sweeping every texture the batch
did not name, so a texture used on alternating frames survives. `#gfxstats`
reports `decodes=`/`decode_hits=` and a `[GFX-TEXCACHE]` census every 60
flushes. Controls: `--flags 2048 --repeat 3` gives one upload, two reuses, one
decode miss and two hits; `--gap 64 --repeat 4` gives one upload, three reuses
and three hits across four flushes; `--flags 6144 --repeat 3 --gap 64` changes
the source each call and gives three distinct keys and three misses. Under
`OGRE_TEX_TRACE=1` a hit is re-decoded and compared (`[GFX-TEXCACHE-MISMATCH]`,
0 in the synthetic run). `debug/probes/atmarkfast.cjs` replaces the
screenshot-cadence series with a per-flush `readPixels` series. One Load Game
run reached scene `0x12` and recorded the page (`mean3=120.86`) and a later
black frame (`mean3=0`), with no white frame, then the game stopped submitting
lists at frame 11. A native RT64 run of the same route (`OGRE_MOD_TEST=1`,
`OGRE_CAPTURE_EVERY_MS=50`) captured 184 targets through the `0x12` entry; the
highest `meanPC` is 97.73 and the highest `fracW240` is 0.00445, so the game does
not present a white plate there either. The other browser attempts landed on
the idle trajectory under host load 16-27 (`[scene] id=0x0000`,
`block=0x00000000`). The exact entry instant and the `0x12 -> 0x03` transition
are unrun in the browser. See `docs/HANDOFF-2026-10-25-session190.md`.

Session 191 gave the browser the native Esc overlay's options. The page now has
MAIN, CONTROLS, SETTINGS and DEBUG tabs (MODS is disabled), opened by Esc or the
topbar Options button and driven by keyboard or pad; MAIN returns to the
launcher with `?menu`, CONTROLS rebinds the 14 N64 rows in localStorage,
SETTINGS carries GAME SPEED, WIDESCREEN, WINDOW (browser fullscreen), SOUNDS,
VOLUME and MOUSE ENABLED, and DEBUG reads the live Chaos Frame and scene id.
`settings.cpp` is compiled into the wasm target and `main_web.cpp` calls
`load_settings("/ogre")` before `recomp::start`, so GAME SPEED, WIDESCREEN and
MOUSE ENABLED are the shared `settings.cfg` values; `web_settings.cpp` exports
them plus the Chaos Frame, scene id, widescreen-expansion test and pointer
forwarding to the two cursor modules. SOUNDS/VOLUME are the page's AudioWorklet
gain. WIDESCREEN counter-scales the projection x row by 3/4 on the mission and
dialogue scenes and puts a 16:9 CSS box on the canvas. The final build compiles
clean and its exports and overlay were measured, but every game run (and a
rebuilt unmodified HEAD control) landed on the idle trajectory under host load
11-13, so the final build's game frame and the 3D wide frame are not captured.
The modal `<dialog>` was replaced by a fixed overlay inside `#stage` because a
modal top-layer element is not part of the fullscreen element. See
`docs/HANDOFF-2026-10-25-session191.md`.

Session 188 fixed a browser freeze after 2–3 minutes with no input. The WebGL
renderer's tile-image cache keyed each decoded image by `tmem_generation_`,
which every load command increments, so each display list created a new GL
texture per tile image and nothing deleted them. The cache is now swept at the
end of every flush and keeps only the images the batch's draws named. A 150 s
no-input run went from 5,063 live textures / 20.3 MB to 4 live / 0.9 MB (5,871
created, 5,867 deleted), and a 232 s run held 7 live / 4.4 MB. A 117 s run with
hardware ANGLE created and deleted 332k textures and held 8 live / 5.6 MB.
`ogre_gfx_stats()`
reports `tex: cache=N uploads=N reused=N released=N`. `boot.cjs` measures the
launcher overlay on the session-187 shell (its canvas PNG includes the overlay),
so `shots.cjs` is the browser render check. See
`docs/HANDOFF-2026-10-23-session188.md`.

Session 187 replaced the page's debug dump with a game shell: a slim top bar, a
bezel around the canvas, a launcher overlay, one curated status line, a controls
dialog, fullscreen, and a `?log` drawer. `#rom-input`, `#save-input`,
`#game-canvas`, `#status`, `#gfxstats` and `#gfx-status` keep their ids and
contracts; `#gfxstats`/`#gfx-status` are hidden but still updated, and
`#game-canvas` is `box-sizing: content-box` so the renderer's CSS size stays 1:1
with the drawing buffer (`docs/proofs/web-ui-launcher-session187.png`,
`-web-ui-game-session187.png`).

Session 168 gave the browser a battery-save import and the native scene-keyed
tap route, and reached the 3D mission scene `0x03` from
`assets/escort_suspend.bin`. `?OGRE_SAVE=<url>` fetches a 32 KiB port image,
writes it to `/ogre/saves/ogrebattle64-us-rev1.bin` after the IDBFS load,
`syncfs(false)`s it, and holds `ogre_start_boot()` until the import settles, so
`pi.cpp`'s `read_save_file` reads it; the run logs `[web:save] imported
assets/escort_suspend.bin (32768 bytes) -> /ogre/saves/...` and then `[save]
loaded /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)`. `OGRE_TAP_SCENE_BUTTON`
is now a page knob too (`<scene>:<buttons>[:<count>]`, the active scene read from
the page's own `[scene]` lines), so the census's route
`title:start:6,0x12:a:6` drives the browser from the forced title through Load
Game to `[scene] t=85121ms id=0x0003`. The mission then **traps**: the worker
logs `Uncaught RuntimeError: memory access out of bounds` after five of the six
overlay bank loads the native run makes (`0x0E4910`, `0x0EBBD0`, `0x101D00`,
`0x145230`, `0x14EC00`; the native continues with `0x171EC0 ram=0x80214FA0`) and
submits no mission display list, so the canvas keeps the last presented frame,
the Load Game screen with its warning prompt. There is therefore no browser
mission frame to compare: `docs/proofs/web-mission-baseline.png` is that frozen
canvas beside the native mission frame (`t.800`, the intro's `Castle Burgund`
target). The census's 916 448 lit, 926 539 backface-culled and 516 463
depth-tested draws of scene `0x0003` remain the expected size of the
depth/lighting/culling gap, not a measurement of it. See
`docs/HANDOFF-2026-10-18-session168.md`.

Session 158 excluded the last named candidate for the title screen's band
residual, the RDP tile-window/mask sampling rule. The browser applies `cms` and
`masks` only by choosing `GL_CLAMP_TO_EDGE` or `GL_REPEAT` for the uploaded tile
window, and session 157's lead was that the fog layer `0x249538` is 200 texels
wide while its draw's `s` runs to 320. A tagged probe (`probe158`) printed all 22
five-row fog strips of a title frame: `masks=0 maskt=0 cms=0 cmt=0`, the upload
is `200x5`, both wrap modes are `GL_CLAMP_TO_EDGE`, the `s` span is 0..200 texels
over the window `uls=0, lrs=796` = texels 0..199, and the browser's sampler
selects the same texel as RT64's `clampWrapMirrorSample` in 66 of 66
comparisons. The same probe read the destination row before and after each fog
draw and shows the draw is the blend of its decoded texel (the texel at alpha
255, no change at alpha 0), and a fresh browser frame equals the native capture
in 36 of 36 sampled pixels of rows 200..231, where the fog writes opaquely. So no
masking rule was implemented. The residual's shape is unchanged and now fully
localised: the fresh pair `probe-00` against `t.137` is
`meanAbs=[2.32,2.52,3.14] / 81.3%`, rows 120..189 are `0.03`, rows 80..119 are
`-7.75` across the whole width, and rows 190..238 are `0.05` at x < 140 (where
the fog is opaque) and `-11.5` at x >= 140 (where the cloud shows). Every
differing row is a row whose visible layer is translucent; every opaque row is
exact. Next: one native run with `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1` to
compare the native's decoded `mean`/`opaque` per `0x217508` strip with the
browser's, which separates image content from the composition beneath the
partial-alpha cloud. See `docs/HANDOFF-2026-10-17-session158.md`.

Session 159 ran that comparison and closed the content/alpha candidate. The
native's `colour_mean` decodes CI8 and RGBA16 only, so a tagged probe
(`probe159`) added the window decode of an RGBA32 tile to
`app/src/renderer.cpp` and the same window's alpha statistics to the browser's
`[GFX-TEXTRACE]` line. Window by window over the same draws (the tile window
`uls..lrs`/`ult..lrt` in texels, the same `(imgw+1)*4` row stride on both sides),
444 of 444 draw windows agree and none differ (`|dmean| < 0.15`, equal `opaque`),
including all 54 windows of `0x217508` over the 53 native frames the trace armed
for. The cloud image's content is constant across that span and equals the
browser's decode, so both renderers hold the same RGBA32 bytes and the same
alpha, and the survivor is the composition. The residual's shape is now measured
per row: zero in rows 186..193 and 234..238, rising from row 194 to a mean of
+18.9 over the 5116 differing pixels of rows 195..232 (native brighter), from a
slanted left edge (x≈168 at row 200, x≈152 at row 210) to x=319. The difference
is neither a translation (the identity shift is the best of +-1 in x and y) nor
an alpha scale (correlation with the browser's own value 0.011). The two runs'
display lists also differ in structure, so a layer cannot be attributed by
comparing them: the browser draws 135 textured draws per list with the cloud as
53 full-width 320x3 strips, and the native 590 with the cloud as seven 72-pixel
columns of 1-pixel strips. Session 156's 0.46-3.01-level bound on adjacent
native phases keeps that out of the residual. The probes are reverted and no
renderer behaviour changed. See `docs/HANDOFF-2026-10-17-session159.md`.

Session 160 closed the two mechanical browser items. The canvas drawing buffer
is now the VI's visible area, which is what RT64's presenter writes: session 148
measured the publisher still's unwritten last column and last row as a whole-frame
difference of -0.87/-0.84/-0.75 per channel (222 and 223 levels on those two
edges) and the `border-radius` arc as 26 differing pixels in the visible area, and
the fix is `resize_canvas(visible.width, visible.height)` in
`present_framebuffer` with the 1:1 blit over the whole canvas plus the removal of
`border-radius` from `#game-canvas`. The publisher still now matches RT64's target
exactly (`meanAbs=[0.0,0.0,0.0]`, 100.0% over 639x475, every corner and both edges
equal to the target), the presented pixels of the same still are otherwise
identical (303499 of 303525; the 26 that changed are the corner arcs), and the
title is unchanged (`[2.31,2.52,3.15] / 81.4%` over 319x239 for the pre-change and
post-change captures, which are pixel-identical). The LOADTLUT destination item was
measured before any change: a tagged `probe160` printed every load's `tmem` and
`palette` over 8 forced scenes plus the publisher still, and all four loads are
`tmem=0x100 palette=0` with 16 and 256 entries, which is the destination RT64's
fixed palette base samples (`RDP_TMEM_PALETTE` 0x800 in bytes), so the browser's
`palette*16` key is the same addressing and the item closes as unexercised with a
kept `OGRE_TLUT_TRACE=1` report. See
`docs/HANDOFF-2026-10-17-session160.md`.

Session 162 ported the native `OGRE_FOG` repair to the browser's display-list
path, and the title screen now matches the native fog-on frame. A browser
`FogWalker` in `app/src/web_renderer.cpp` runs on each submitted list in RDRAM
before the walker reads it: it tracks the last `SETTIMG`, `SETTILE t7`,
`LOADTILE`, `SETTILE t0` and `SETTILESIZE t0` plus the white combiner
(`w1 == 0xFFFF73B9`), reads the layer table at `D_801B80E0`
(`+0xFC + i*0x24`), and rewrites that setup to sample the layer's own image with
`masks`/`maskt`, the whole-image `LOADTILE` and the whole-image `SETTILESIZE`
(the rule is `renderer.cpp:1003-1219`). `OGRE_FOG=0` disables it (default on,
like the native, and `?OGRE_FOG=0` already reaches `getenv` through the page's
`ENV`). Measured over fresh series (397 native fog-on frames, 587 fog-off; 40
browser shots each way), the fog-on minimum is `t.177`
`meanAbs=[0.09,0.09,0.12] exact=91.5%` (bands 0.014/0.273/0.060) against
session 161's `[2.08,2.23,2.74]/79.2%`; the fog-off control is
`[0.03,0.03,0.03] exact=97.3%` (0.042/0.011/0.026), and the cross-controls are
2.740 and 2.733. The 0.273 in rows 64..119 is 0-3-level blend rounding: the
browser's fog overlay per band is `-0.683/5.691/-0.007/6.719` against the
native's `-0.674/5.705/-0.007/6.637`, and the band-minimising frame gives 0.074
with every difference exactly one level. The publisher still is
`[0.0,0.0,0.0]/100%`, `boot.cjs` is `RENDERED`, and neither the publisher nor
the story fires the repair. See `docs/HANDOFF-2026-10-17-session162.md`.

Session 163 ported RT64's high-precision colour buffer to the browser and
corrected session 162's explanation of the remaining fog band. A browser colour
image is now an RGBA16F attachment when `EXT_color_buffer_float` is available
(RGBA8 otherwise, or with `OGRE_HPFB=0`), because RT64's default
`internalColorFormat` resolves to High on this machine and its targets are
`R16G16B16A16_UNORM`: the native renderer quantizes a stack of translucent draws
once, on the RDRAM writeback, instead of at every blend. The float attachment
needs `type=GL_HALF_FLOAT` (WebGL2 rejects `UNSIGNED_BYTE` for RGBA16F, status
0x8CD6), float staging in `upload_framebuffer`, and a `GL_FLOAT` readback that
converts through `float4_to_rgba16` (RT64 `Float4ToRGBA16` with dither 0 and the
16-bit `usesHDR` coverage test); the RGBA8 path keeps its 8-bit rule, an
incomplete float attachment falls back to RGBA8, and the fragment shader asks
for `highp`. Measured at the same native instant (`t.177`), the RGBA16F canvas
is `meanAbs=0.098` (bands 0.014/0.271/0.060) and the RGBA8 canvas is `0.099`
(0.014/0.273/0.060); the two canvases differ from each other by 0.0091 mean,
0.906% of channel samples at exactly one level. So the 0.27 fog band is not
per-blend quantization: against the native target converted with the target's
own 8-bit rounding, the band is still 0.241 (the target's own high-byte-vs-round
difference is 0.068 there), and the amplified difference is one wave in rows
81..119, the translucent top edge of the repaired fog layer. The new
`[GFX-FOG-SUMMARY]` line shows that layer's image record is I8 (`siz=1 fmt=4`)
64x64 at `0x801D51E8` (layer 2), so the next comparison is the sampled intensity
and blend alpha of one repaired group, not another target format. No regression:
the publisher still is `[0.0,0.0,0.0]/100%`, `testdraw --flags 96` reports
`fbtex hits=1`, and `boot.cjs` is `RENDERED` (`nonBlack=25424`). See
`docs/HANDOFF-2026-10-17-session163.md`.

Session 164 ported RT64's texture sampler into the browser fragment shader and
measured what the screens actually sample. `OGRE_TEXF` selects it: `rt64`
(default) is RT64's three-point filter, `1`/`bilinear` is true bilinear in the
same shader, and `gl` is the fixed-function sampler this renderer used before.
The port covers `clampWrapMirrorSample` (tile-window clamp, `masks`/`maskt` wrap,
mirror), `sampleTextureLevel`'s point/bilerp/average selection, RT64's default
three-point weights, and the `SIMULATE_LOW_PRECISION` quantization of the
interpolated coordinate to 1/128 texel; the filter mode is now draw state
(OTHERMODE_H `G_TF_*`) instead of a property of the uploaded texture, `AVERAGE`
exists, and a masked tile wraps/mirrors instead of the whole texture repeating.
Measured with the new `[GFX-SAMPLER]` census: the forced title is 591 textured
draws, all `point`, 456 masked; the forced world map is 99 draws, all `point`, 1
masked; the publisher still is 56 draws, all `point`, 0 masked. So filtering is
not what the title fog band is: the repaired fog draws are point-sampled 320x1
strips and 64x64 blocks (`[GFX-FOG-DRAW]`: `filt=0`, `mask=(64,64)`,
`cm=(0,0)`), and the band is unchanged by the port at the same native instant.
The tutorial `0x17` is the one reached screen with bilinear draws (8 textured
draws, 6 `bilerp`, 6 masked): its background is a 64x64 tiling I8 image drawn
447x335 texels over 320x240 with `masks=6`, which is a draw the new rules apply
to. The browser's tutorial dialogue box is broken in both samplers (open work
8(r)), so the sampler is verified by the point-sampled screens being unchanged
and by the flag/census data, not by that screen's content. See
`docs/HANDOFF-2026-10-17-session164.md`.

Session 161 verified session 160 and closed the title screen's band residual as a
missing display-list repair, not a renderer difference. The publisher still's
canvas is the visible area (639x475) and all 303525 of its pixels equal RT64's
target, so the presenter leaves no canvas pixel unwritten and the
`border-radius` removal was the last non-game pixel. The per-draw `s`/`t` of the
two title runs' scrolling layers were compared for the first time: at the third
display list of each run the 135 non-degenerate draws are identical in order,
image, rectangle, `s`, `t`, `dsdx`, `dtdy` and tile window (the sky image
`0x1C3EA8`'s partial tile is `0,16..63,80 s=32` in both), so the two runs share a
scroll state. The native list also holds 456 draws with a degenerate tile window
(`ult == lrt`): the mirrored `0x1E2918` pieces and the 72-pixel `0x217508`
columns, which are the `OGRE_FOG` repair's pieces (the native app rewrites their
tile setup to sample the layer image; the browser's `draw_texrect` rejects them
with the guard that mirrors RT64's `RDP::drawTexRect`). Turning that repair off
in the native series and comparing the same animation instant gives
`meanAbs=0.052` (rows 0.046/0.072/0.044) against `2.349` with it on, so the
residual is exactly the fog layer the browser does not draw. No renderer
behaviour changed; the port of the fog pass is the open item. See
`docs/HANDOFF-2026-10-17-session161.md`.

Session 156 measured the title screen's two moving cloud bands, the last known
browser rendering difference, and answered the phase question. A native series
of 525 `OGRE_CAPTURE_TARGET` frames (forced title, `OGRE_SPEED=1`) was compared
with a 40-frame browser series of the same forced scene, and the minimum over
the series is `title-03` against `t.574`: `meanAbs=[2.08,2.23,2.74]`,
`meanSigned=[-2.03,-2.17,-2.67]`, 79.2% exact with the presenter's unwritten edge
inset out (`[2.31,2.32,2.71] / 79.2%` without). Native frames of the same phase
are identical in the bands and adjacent phases differ by 0.46-3.01, so the
remaining 8-13 levels of one-signed band difference cannot be phase: **the
renderer draws the cloud layers differently.** (Session 161 corrects the
mechanism: the browser draws the same cloud and sky draws as the native at the
same scroll state, and the difference is the 456 zero-texel-window rectangles the
native's `OGRE_FOG` pass rewrites into the fog layer, which the browser skips.) The draw state is the same in both
renderers for the strip at rows 101..104 (rect, S/T step, texel window, image
`0x80217508`, `fmt=0 siz=3`, `line=80`, palette 0, LUT mode 0, 1-cycle, combiner
`0xFCFFFFFF`/`0xFFFCF279`, force blend `P=CC A=CC_A M=FB B=1-A`), and the
browser's decoded texel mean equals an independent decode of the same rows
(92.11/92.1, 104.55/104.6, 66.73/66.7). The band rows are exactly where the cloud
strips sample partial-alpha texels, and there the browser's pixel equals the raw
texel (94/95, 108/140, 155/156, 245/246 at rows 98/101/104/110) while the native's
is a blend. That is the open lead: the sampled alpha is not the value the blend
uses, and the next experiment is to print the fragment's combiner alpha for one
cloud strip. `[GFX-TEXTRACE]` gained the draw's `cyc/omh/ablend/force/ccyc` to
make that comparison from one line. An alpha probe that forced every RGBA32
texel's alpha to 128 changed the frame hugely but could not separate the
hypotheses, so it was reverted. See `docs/HANDOFF-2026-10-17-session156.md`.

Session 155 closed the two items session 154 left. (a) The `[GFX-FBTEX]` row
test's inversion was one row up: `note_fb_write` computed
`height - 1 - trunc((pos + 1) / 2 * height)` for a vertex, which is `y - 1` where
the float product lands on or above the integer (194 of the 241 rows of a
240-tall image) and `y` where it lands below. It now inverts `ndc_yf` with
`(1 - pos) / 2 * height`, treats a vertex as an edge (the lower-right corner is
exclusive, so the last row covered is the bottom edge minus one) and rounds with
`std::lround`, so a fill of rows 100..163 is recorded as 100..163. `test_draw`'s
bit-6 branch gained a row control - a second colour image at `0x00C28000`, a fill
of rows 100..163, and a load of rows 98..99 - and `--expect-hits 1` is the
assertion: the old arithmetic prints `fbtex hits=2 lines=2` with
`drawn rows=[99,163]` and `FAIL: expected fbtex hits=1, read 2`, the fixed build
prints `hits=1 lines=1` and exits 0. (b) Framebuffer-as-texture is **closed as a
decision: no reachable screen needs it.** The full 25-scene sweep
(`fbtex.cjs --scenes 0x00..0x18 --secs 18`) reports `0 over 25`, with 21 of the
25 scenes producing display lists (3..136 tasks; `0x02`/`0x03`/`0x0D`/`0x0E`
produce one task and a page error, so they say nothing, and `0x0B` reached only
the 3-task idle trajectory), and a natural tap route through the opening
(`start,a,start,a,a,a,a,a,a,a` at 1200 ms) reports `tasks=196 hits=0`. The
detector and its counter stay. Diagnostic only: no rendered frame moves, and the
boot is unchanged (`RENDERED nonBlack 25136`). See
`docs/HANDOFF-2026-10-17-session155.md`.

Session 154 closed session 152's `[GFX-FBTEX]` over-report. The detector's row
test fired for a load whose bytes lie in a row a draw wrote but outside every
column it wrote, and the load side claimed the whole row stride for a rect that
reads only `w * bytes_per_texel` bytes of each row. `app/src/web_renderer.cpp`
now records the byte columns each draw covered (`FbWrittenRange::col_min`/
`col_max`, from the same vertex mapping the row test uses and
`bytes_per_texel(fb_siz)`), bounds the load's rect to `x0 .. x0+w` texels, ends
its row range at the rect's last row, and requires both axes to overlap. The
column test is applied only when the image's pitch divides the load's stride;
otherwise the row-only answer stands. `test_draw`'s positive fill is narrowed to
columns 0..63 of its 320x240 image and a new debug flag bit 6 (64) adds the
negative control, a second load from texels 160..191 in the drawn rows and
outside the drawn columns. Measured with only the column comparison disabled:
`testdraw.cjs --flags 96` reports `fbtex hits=2 lines=2`, the second line
`cols=[320,384) drawn cols=[0,128)`; with it, `hits=1 lines=1` and only
`cols=[0,64) drawn cols=[0,128)`. The probe gained `--expect-hits N` so that is a
hard assertion, and the bit-5 and bit-0 controls, the boot
(`RENDERED nonBlack 25136 / colorful 19296`) and the scene sweep
(`0x04`/`0x0A` `hits=0`, `0x10` `tasks=52 hits=0`) are unchanged. Diagnostic
only: no rendered frame moves. The session also corrected session 153's claim
that `app/src/web_platform.cpp`'s header comment records the native slot rule;
the committed tree still had the old "slot 0 is the keyboard, slots 1-3 are
gamepads" text, which is now rewritten.

Session 153 closed open item 13 (the browser build assigned gamepads to N64
controllers 2-4). `app/web/web.js` gave each pad slot `(pad.index % 3) + 1`, so
controller 1 was keyboard-only and a pad was invisible in game; the native rule
(`app/src/sdl_platform.cpp`) is that a pad takes the lowest free slot and the
keyboard is OR'd into slot 0. `updateGamepads` now assigns slots 0..3 in that
order and ORs slot 0's keyboard and tap mask into the pad's buttons. The pad pass
also had to run *after* the keyboard pass in the animation frame: it writes slot
0, so the previous order (keyboard, then pads) made a connected pad overwrite the
keyboard every frame. `ogre_input_debug` now takes a slot and replies `0xFFFF`
for a slot with no connected device, and the new `debug/probes/gamepad.cjs`
installs a synthetic pad with `page.addInitScript` and requires slot 0 to carry
the pad's `A` (`0x8000`) *and* to carry `0xC000` while the probe holds the
keyboard's `B`. Before the fix the probe read `[web:input] gamepad connected
(slots 1-3)` and slot 0 idle; after it, `pad 0 -> slot 0` and `0xC000`. The
change is JS-side plus one debug export, so no renderer path moved: `boot.cjs`
reports `RENDERED`, `testdraw.cjs --flags 32 --expect-fbtex` still prints
`fbtex hits=1 lines=1`, and a `start,a,...` tap route still reaches scene `0x09`.

Session 152 built the detector for session 151's framebuffer-as-texture item and
found no screen that needs the feature. A texture decode
(`app/src/web_renderer.cpp`, `note_fb_texture`) now compares the guest byte range
it reads against `fb_written_`, one entry per colour image a recorded draw has
written since the main thread last wrote the framebuffers back to RDRAM (the new
`fb_writeback_epoch_` counter), and reports `[GFX-FBTEX]` plus an `fbtex: hits=N`
counter in `#gfxstats`. The test needs the drawn *rows*, not only the byte range:
a colour image is sized to `max(viewport, VI height, scissor bottom + 1)`, so a
640x480 mode's target is 640x481 while the game writes rows 0..479, and the heap
past the image holds the game's own textures — the range-only version of the
check reported 142 such loads (scene `0x0A` 9, `0x10` 133) that all read RDRAM
correctly. `test_draw` gained a positive control under `ogre_gfx_debug_flags` bit
5 that renders into a colour image and loads a tile back out of the same address;
`testdraw.cjs --flags 32 --expect-fbtex` prints `fbtex hits=1 lines=1` and the
same probe with `--flags 0` prints `hits=0 lines=0`. With the control firing, all
25 forced scene ids (`fbtex.cjs --secs 18`) and three natural routes (the
unforced boot to title, `OGRE_SCENE=new-game OGRE_STEP=2` to the cathedral, and a
`start,a,...` tap route; 120 s each, 351/443/471 display lists) report 0 hits.
The instrument stays, so a screen that needs the feature reports itself. The
session also found that session 151's native cathedral route does not work in
this build: with `OGRE_MOD_TEST=1` the native run logs `forcing scene 0x02` and
`seeded step 2` and then plays `0x09 -> 0x0A -> 0x04`, while the same query in
the browser reaches `0x0D`, so no browser-vs-native cathedral frame comparison
was made.

Session 151 went back to the browser's own items. The browser's scripted tap
schedule now matches the native one: `app/web/web.js` had cycled
`OGRE_TAP_BUTTON` forever (`names[slot % names.length]`) while the native advance
is `slot = tap < count ? tap : count - 1` (`app/src/sdl_platform.cpp`, "the last
slot sticks") plus `OGRE_TAP_MAX`, so a route could not stop tapping mid-run and
the browser has none of the native's scene-keyed knobs; a `none` tail now stops
it, verified by a run logging `[web:input] tap schedule done after 3 presses`.
Two route facts came out of the attempt to reach the cathedral: the session-54
title route is racy in the browser (this run's forced title landed at t=8189 ms,
after the first presses), and `OGRE_SCENE=new-game OGRE_STEP=2` reaches the
cathedral on both sides and waits for input there, so a cathedral comparison
needs no taps. The next renderer item is framebuffer-as-texture: the gfx thread
decodes a `G_LOADTILE`/`G_LOADBLOCK` out of RDRAM at record time
(`decode_texture_rect`, `app/src/web_renderer.cpp:3827`), while a colour image
the same display list rendered into is still only in a GL framebuffer and is
written back on the main thread at present or at a `sync_framebuffers` request
(`readback_all_framebuffers`), so such a load decodes stale bytes.

Session 150 settled session 149's fill-scissor question and fixed the native
renderer. The RDP's scissor does clip a FILL-mode write: RetroArch with the
`parallel_n64` core (ParaLLEl-RDP) run to the attract story shows the letterbox
rows 0..16 and the bottom band as `(0,0,0)`, the second fill's black, with the
game's gradient rows from 18 and the last column holding stale content from the
previous scene. n64brew says the same. `RDP::fillRect`
(`tools/RT64/src/hle/rt64_rdp.cpp`) now intersects the rectangle with the current
scissor before `drawRect` and returns on an empty intersection, so RT64 no longer
clears the whole rectangle the game asked for; the story target's bands are
`(0,0,0)` where they were `(74,90,0)`, the trace prints `fillRect clipped
(0,0)-(1279,959) -> (0,72)-(1276,776)`, and the developer confirmed the screen
("the attract screen has the correct bg color now! it is black instead of
olive!!"). `patches/rt64-ob64.patch` was regenerated with `make patch-fix` so
the hosted release path carries the clip, and `make patch-check` PASSes. No
regression: the publisher stills' `tt.120`..`tt.220` are
byte-identical to session 146's reference and the title stays at
`[2.29,2.49,3.10] / 81.4%`. Session 149's "the browser's fill scissor is one
column narrow" is withdrawn as a browser-vs-reference difference: the reference
emulator leaves that column unwritten too, so the browser's half-open scissor
agrees with it. Session 84's olive-band branch reading is corrected at
instruction level in the handoff: the `0x4F00C308` it named is the data word of a
`G_SETSCISSOR` pair, and `li a0,0x4AC1` is a `beqz` delay slot, so it is not a
two-armed colour choice - and with the scissor clipping, the band colour is
fill 2's black in either case.

Session 149 fixed the browser's fill colour and found that the attract story's
remaining difference is mostly RT64's. `G_FILLRECT` now decodes the fill colour
in the colour image's format: a 16-bit image takes the low 16 bits of the
`G_SETFILLCOLOR` word as RGBA5551 and a 32-bit image the whole word as RGBA8888,
which is what RT64 does (`ColorConverter::RGBA16::toRGBAF(fillColor & 0xFFFF)` /
`RGBA32::toRGBAF`). The story's letterbox fill is `0x4AC14AC1`: the browser
painted the 32-bit decode `(74,193,74)`, a green, where the 16-bit value is
`(74,90,0)`, and `0x00010001` painted `(0,1,0)` where it is `(0,0,0)`. The four
fills isolated with `ogre_gfx_debug_flags` bit 1 read before/after as
`(74,193,74) -> (74,90,0)` and `(0,1,0) -> (0,0,0)`, and the trace prints the
word, the image size and the scissor. The same screen then splits into two other
differences: RT64 implements a fill as a clear of the *unscissored* rect
(`InstanceDrawCall::Type::FillRect` -> `clearColor` with
`clearRect.rect = convertFixedRect(call.callDesc.rect)`), so the story's fourth
fill paints the whole 320x240 and RT64's target bands are `(74,90,0)` where the
browser, which honours the scissor, has `(0,0,0)` - 27% of the frame (64 of 240
rows) and the largest term in `meanAbs=[25.21,30.50,5.73]`; and the scene itself animates, so its
rows cannot be compared across runs (`s.160` sky `(10,1,27)` against `s.180`
`(24,16,47)`). Whether hardware clips a fill by the scissor is now the open
question that decides which renderer is right there (open work 8(k) below), and
it also decides whether session 84's olive-band branch reading was chasing a
symptom.

Session 148 closed the publisher still's one-row offset, and it was not in the
renderer. `app/web/index.html` pins `#game-canvas` at `640x480` and
`present_framebuffer` sizes only the drawing buffer, which for that VI mode is
`640x476`, so the browser scaled the 476-row buffer into the 480-row box and
`image-rendering: pixelated` made that a nearest-neighbour resample. On session
147's own capture, canvas row `Y` equals RT64's target row
`floor((Y + 0.5) * 476 / 480)` for every row: mean absolute difference
`[0.87, 0.84, 0.75]` per channel, 99.6% of channel samples identical, against
`10.38` for the identity and `7.26` for the best constant shift. The ATLUS
strokes are `x 156..471, y 200..283` in the canvas against `156..471, 198..281`
in RT64: the same box, two rows lower, which is what the stretch does at that
row. `resize_canvas` now sets the element's CSS box with
`emscripten_set_element_css_size` to an integer multiple of the drawing buffer
(`max(1, min(2, 480 / height))`), so a 320x240 buffer keeps the 2x box and the
640x476 mode is shown 1:1. On the fixed build the still matches RT64's target at
`[0.87, 0.84, 0.75]` with 99.6% exact and the whole-pixel shift search minimising
at exactly `(0, 0)` (`0.82`; every other shift `>= 6.80`), the title is unchanged
at `[2.30, 2.50, 3.12] / 81.3%`, `boot.cjs` renders and `testdraw.cjs` returns 1.
Over the visible area (`tools/web_frame_cmp.py --inset 1`) the still is exact:
`meanAbs=[0.0,0.0,0.0]` and 99.997% of channel samples identical, with 9 differing
pixels - the four that `#game-canvas`'s `border-radius` clips and five at
+-1/+-2. The whole-frame signed difference `-0.87/-0.84/-0.75` is the buffer's
last column and last row, which the presenter never writes because the VI's
visible area is 639x475. Two session-147 numbers are corrected: the canvas
bounding box it recorded was one pixel right and one row low (a crop offset), and
its "9836 = 4 x 2459 native" stroke count is 9836 in both frames at 640 wide.

Session 147 fixed the browser's palette decode. `decode_texture_rect` chose a
palette entry's decoder from the entry's own bit 15 (RGBA16 when set, IA16 when
clear); the RDP chooses it from the draw's 2-bit texture LUT mode
(`OTHERMODE_H` bits 14-15), which is `G_TT_RGBA16` (5/5/5/1) or `G_TT_IA16`
(8-bit intensity plus alpha) for every entry of the tile, and RT64's
`TextureDecoder.hlsli` switches on that mode. The publisher still's ATLUS strokes
are entry `0x3AE8`, whose bit 15 is 0, so the browser painted
`ia16(0x3AE8)` = `(58,58,58)` where the entry is `rgba16(0x3AE8)` =
`(57,90,165)`; the red T's entry has bit 15 set and matched by accident. The
canvas's colour histogram now equals RT64's capture entry for entry (cream
`(239,231,206)`, shadow `(222,214,181)`, strokes `(57,90,165)`, red T
`(222,0,41)`). Session 146's reading is corrected in two places: the native
stroke colour is `(57,90,165)` and not `(48,80,160)` (the earlier value occurs 0
times in the capture), and the blue native render is correct, so the fault was
never in RT64's TMEM palette lookup. Session 147's one-row item is closed by
session 148 above; the title
is unchanged
(`meanAbs=[2.32,2.51,3.12] exact=81.2%` against the same target).

Session 146 fixed the browser's present geometry, which was the bulk of session
145's residual frame difference. `blit_to_canvas` drew the VI's visible area (the
`fbSize` minus the overscan crop) over the *whole* canvas, so 319x239 was
stretched onto 320x240 and every canvas pixel past column 160 and row 120 sampled
its framebuffer texel one column or row along. The measurement that identifies
it is that the session-145 build's canvas was *closer* to the target passed
through that stretch (`2.30/2.50/3.11`) than to the target itself
(`4.66/5.05/5.54`); on the fixed build the relation is reversed
(`2.29/2.48/3.10` against `4.56/4.94/5.47`). RT64's
`VIRenderer::getViewportAndScissor` anchors the viewport of
`visibleFramebufferSize` at the window's top-left, and the canvas is the VI's own
size, so the destination rectangle is now the same fraction of the canvas and the
leftover last column and row are left alone. The same session fixed a second
off-by-one: the GL scissor used `sx1 - sx0 + 1`, while the RDP's scissor is
half-open (`convertFixedRect` builds a plume `RenderRect`, exclusive at the right
and bottom), so the game's `(0,0)-(319,239)` covers pixels 0..318 by 0..238,
which is why RT64's own render target has a black last column and row. Exact
channel samples against RT64's target for the same title frame moved from 39.4%
to 81.4%, and `tools/web_frame_cmp.py` is new as the comparator between a
`#game-canvas` screenshot and an `OGRE_CAPTURE_TARGET` `.bin`. What remains is two
moving-cloud bands, and the developer states the port's clouds are not exact
against retail yet, so the two runs' cloud phases are not separable there. The
session also read the publisher still's ATLUS letters as RT64 drawing them blue
(`(48,80,160)`) where the browser drew them black (`(56,56,56)`), and left the
cause in RT64's TMEM palette lookup; session 147 measured the native colour as
`(57,90,165)` and found the browser decode at fault. That is open work 8(h) and
8(i) below.

Session 145 closed session 144's open item 1, "the browser's title and story
frames are hue-shifted against RT64's target for the same text". The mechanism
was the decode, and not the GL sampling or the blend as session 144 concluded:
`decode_texture_rect`'s `IM_SIZ_32b` case read an RGBA32 texel's two big-endian
halfwords as `(alpha,red)` and `(green,blue)` and so assigned the source bytes
G, B, A, R to the destination R, G, B, A. RT64 reads the same four guest bytes as
R, G, B, A (`rt64_rdp.cpp:527` `loadWord<RGBA32>` splits them across TMEM's two
halves and `TextureDecoder.hlsli`'s `sampleTMEMRGBA32` rebuilds
`(R<<24)|(G<<16)|(B<<8)|A`), and `tools/ogre_frame.py` decodes the same order
from a dump. The title draws its 320x240 RGBA32 backdrop as 3-row `G_TEXRECT`
strips (`img=0x217508`, `stride=1280`), so the shift turned a brown-to-white sky
magenta-to-white; the attract story's frames were hue-shifted in session 143's
comparison and use the same case. The browser trace's own numbers moved from
the `(G,B,A,R)` decode to the `(R,G,B,A)` decode of the same run's RDRAM dump
(rows 0..3 `mean 25.5 -> 56.8`, rows 21..24 `mean 87.9 -> 104.6`), and the canvas
against RT64's `OGRE_CAPTURE_TARGET` of the same frame went from 40.7 to 5.1 mean
absolute difference per channel
(`docs/proofs/web-title-rgba32-rgba-order-fix.png` against
`-before.png` and `native-title-rgba32-reference.png`). Session 144's comparison
was the browser's decode against the browser's canvas, and both agreed because
the trace's `mean` is computed by the decode under test. New
`tools/rt64_target.py` reads an `OGRE_CAPTURE_TARGET` `.bin`
(`R16G16B16A16_UNORM`, 8 bytes/texel) into a PNG, and `docs/guides/app-build.md`
records that a scripted native run needs `OGRE_MOD_TEST=1` while
`build-app/mods/` holds mods, or the start screen waits for a ROM and no capture
is written.

Session 144 closed session 143's second open gap, "CI8 tiles decode black in
display lists that load no palette". A CI tile whose RDP texture LUT mode
(`OTHERMODE_H` bits 14-15, RT64's `OtherMode::textLUT()`) is 0 does not read a
palette at all: RT64 reinterprets the loaded bytes as I8
(`rt64_framebuffer_manager.cpp`'s `(dstFmt == G_IM_FMT_CI) && (dstSiz ==
G_IM_SIZ_8b) && (tlutFormat == 0)` case, which runs `ANY8toI8` in
`FbReinterpretCS.hlsl`), so the byte is an intensity replicated into RGB and into
alpha. The browser decoded every such tile as CI against an empty palette, which
is opaque black. The title screen's prologue draws its two unit sprites
(`72x28` and `72x20`, `img=0x1C13F8 fmt=2 siz=1 line=9`, `oml=0x504240`, `lut=0`)
through that path, and they were the black rectangle over the scene; they now
render as the dark silhouette RT64 draws
(`docs/proofs/web-title-lut-mode0-i8-fix.png`; the browser trace's mean for the
first sprite moved from `0.0` to `72.2`). The palette also moved out of the
per-display-list `RenderState` into renderer state, because the RDP's TMEM
palette outlives a display list (RT64 clears `RDP::TMEM` in the constructor only
and `RDP::reset()` does not touch it) and a CI image's decoded colour therefore
depends on the palette generation and on the LUT mode as well as on its source
bytes. Both are in the image cache key now, and `OGRE_TEX_TRACE` prints what the
browser decodes. The two sprites do not need the persistence part - they trace
`tlut=no` and are read as I8 - so no screen has yet exercised it.

Session 143 fixed the browser's presentation of a frame. The presenter drew the
whole GL framebuffer texture over the canvas, and that texture is the RDP colour
image at the tallest height it has ever been asked for (RT64's
`Framebuffer::maxHeight`), so the title's 320x240 frame was drawn into the top
240/476 = 50.4% of the canvas with the next guest image's rows below it as noise
or black - session 142's "the attract story is only partly drawn".
`visible_framebuffer_size()` ports RT64's `visibleFramebufferSize` (the VI's
`fbSize` minus `OGRE_OVERSCAN`, default 1) and `blit_to_canvas` samples only that
area. The title logo and the attract story's pages now fill the frame
(`docs/proofs/web-title-640-after-texrect-fix.png`,
`docs/proofs/web-attract-story-full-frame.png`). Session 145 then fixed the hue
shift between the browser's title and story frames and RT64's target: it was the
RGBA32 decode's channel order (see above).

Session 141 continued the renderer. The render targets and the canvas are now
sized from the VI's scanout size (a port of RT64's `VI::fbSize`, updated only by
a visible mode), the world map's 640-wide images are `640x481` and present at
640x476 instead of being drawn into `640x240` and shown through a 320x240 canvas,
the per-pixel `G_TEXRECT` S/T step was corrected from 32x too large (the title
screen's banding), and `K4`/`K5` are combiner inputs. A pre-existing wall was
found while measuring: the browser build faults with `divide by zero` the first
time the game renders the hi-res title screen, in the session-140 package and in
this one alike.

**Renderer completeness (session 176).** The WebGL2 renderer implements the RDP
state the screens it reaches use: TMEM with RT64's load layout and YUV16, RT64's
texture sampler (`clampWrapMirrorSample`, point/bilerp/average, the three-point
filter, the `1/128` quantisation, `ShiftScaleMap`, `G_TP` from OTHERMODE_H bit
19), alpha compare and `CVG_X_ALPHA`/`CVG_DST` coverage, the combiner (two
cycles, key centre/scale, `K4`/`K5`), the blender, fog colour, `G_FOG` per-vertex
alpha, `G_LIGHTING` with seven lights plus ambient, backface culling, the depth
pipeline (`G_SETZIMG`/`G_SETPRIMDEPTH`, RT64 screen-z, a target keyed by the
depth image, on by default), the half-pixel triangle offset, a high-precision
`RGBA16F` colour attachment, RDRAM framebuffer indirection with the writeback
only on the game's own sync request, S2DEX2 object rectangles and the njpeg YUV16
backdrop, and the browser port of the `OGRE_FOG` repair.

Verified with numbers, against a native `OGRE_CAPTURE_TARGET` of the same screen:
the boot smoke probe renders on attempt 1 of 3 on a quiet machine
(`nonBlack=24452 colorful=10140`); the synthetic controls pass (`--flags 96` ->
`fbtex hits=1 lines=1`, `--flags 128` reads the near triangle, `--flags 384` the
far one); the title's attract **story** beat matches native `t.640` at
`meanAbs=[0.24,0.25,0.28]` (85.1% exact) and its **logo** beat matches `t.1080`
at `[1.88,1.96,2.23]` (72.1%), with the clouds exact (`0.07`/`0.08`) and the logo
and copyright bands the residual (`5.32`/`8.49`, no placement offset; the four
frames side by side are `docs/proofs/web-title-forced-pair-175.png`); the
mission `0x03` renders (`nonBlack=303208 colorful=294108`); the tutorial `0x17` box
is `0.25/0.00/0.18` by band and the world map `nonBlack=252820` (session 174).

**The mission pair is now frame-locked, and the low mean hides a missing sprite
layer (session 176).** A native mission series was captured from the same save and
route as the browser's (637 targets at 60 Hz, `OGRE_CAPTURE_EVERY=1`, mission
entered at `[scene] t=17821ms id=0x0003`, window `t.264`..`t.640`) and compared with
every browser mission frame over every native frame (2262 pairs, the
`web_frame_cmp.compare()` arithmetic in `/tmp/w176/crosscmp.c`). The global minimum
is browser `m4-a1-014` against native `t.289`: `meanAbs 4.74/3.59/1.90`,
`meanSigned -1.12/-0.81/-1.40`, `exact=45.08%`, and ranks 2-5 are
`3.421/3.424/3.429/3.434`, flat inside a 0.5 s window because the map is static at
that phase. Terrain regions of the best pair: top cliff rows 0..38 `0.93`, left
grass `1.66`, right terrain `2.95`, bottom middle `8.56`. The residual is not the
terrain: **the browser does not draw the party's character sprites.** In the party
cluster (x18..78 y60..98) the pair measures `26.54`, 735 of 2280 pixels differ by
more than 30, the browser's brightest channel value is 97 against native's 255, and
`docs/proofs/web-mission-framelocked.png` shows native's five characters where the
browser has the terrain and the cursor brackets only. The three element-screenshot
frames are 6.7-7.3 against their best natives; at those phases the note box is
`4.65` (79.8% exact) and two other elements lose their texels one at a time: the
portrait box `13.58` (31.3%) and the MISSION band `8.94` (18.6%, 207 bright glyph
pixels against native's 1083, no lattice period and no parity bias). That stipple
and the absent sprites are one family, an alpha/coverage rejection on alpha-tested
sprites (`G_AC_THRESHOLD` at `web_renderer.cpp:1596-1599`, `G_AC_DITHER` at
`:1600-1606`, `CVG_X_ALPHA` coverage at `:1614-1617`).

**The stipple was the `G_AC` enum mapping (session 177).** The mission's portrait
frame and `MISSION` band lost their texels one at a time because the shader
tested `u_ac == 2` for `G_AC_THRESHOLD` and `u_ac == 1` for `G_AC_DITHER`, while
RT64's values are 0/1/3. The 33 770 `acmp=1` mission draws therefore ran the
dither arm against a random hash; a combiner alpha around 0.2 kept about a fifth
of the texels. The shader now runs the threshold compare for `acmp=1` and RT64's
`initRand`/`nextRand` dither for `acmp=3`, with the frame, instance and pixel
seeds and the NOISE pre-consumption. The synthetic control
(`testdraw.cjs --flags 512 --expect-alpha`) reads full/none/partial for
threshold/blend-0, threshold/blend-1 and dither, and the depth, fbtex and tutorial
controls are unchanged. The browser boot stalled at scene `0x0000` with
`tasks=0` on every attempt after about 01:40, so the frame-locked mission
re-capture is pending; that is the measurement that will close or re-open the
missing party sprites. See `docs/HANDOFF-2026-10-19-session177.md`.

Residuals, each with the reason it is not closed:

- **The mission's stipple was the `G_AC` enum mapping: fixed in session 177, and
  the mission re-capture is pending.** RT64's values are `G_AC_NONE 0`,
  `G_AC_THRESHOLD 1`, `G_AC_DITHER 3`
  (`tools/RT64/src/shared/rt64_f3d_defines.h:8-10`); the browser's shader treated
  `u_ac == 2` as the threshold arm and `u_ac == 1` as the dither arm. A real
  `G_AC_THRESHOLD` draw ran the dither arm against a random hash, so a combiner
  alpha of about 0.2 kept a fifth of the texels at random: the portrait frame's
  border and the `MISSION` band glyphs. The mission census has 33 770 `acmp=1`
  draws and no `acmp=2`. `u_ac == 1` is now the threshold compare against the
  blend alpha and `u_ac == 3` is RT64's `initRand`/`nextRand` dither (frame,
  instance and pixel seeds, NOISE pre-consumption). The synthetic control proves
  the mapping: `testdraw.cjs --flags 512 --expect-alpha` reads
  `threshold/full=full threshold/none=none dither/partial=partial`
  (`[GFX-AC-TEST]` 1406/1406, 0/1406, 277/1369). The frame-locked mission
  re-capture is the open item: the browser boot stalled at scene `0x0000` with
  `tasks=0` on every attempt in the second half of session 177, so whether the
  party sprites and the two overlay elements are now solid is **not measured**.
  If the sprites are still absent after this fix, that is a second mechanism and
  must be named separately.
- **The lit terrain has a colour offset.** Signed G +6.2 / B -14.3 on the top cliff
  and R +6.4 / B -7.6 on the bottom band; the 2D elements do not carry it. Not named.
- **`G_CD` colour dither is not implemented.** The game does enable it (`[GFX-TP]`
  families decode as `G_CD_MAGICSQ` on the title, map, mission and attract story,
  `G_CD_BAYER` on the tutorial), but RT64 applies it only inside its native-format
  writeback shader (`FbWriteColorCS.hlsl:19-21`, `FbReinterpretCS.hlsl:20-23`) and
  the presented frame samples the GPU render target, as the browser's samples its
  GL colour image. The browser's writeback already uses the same
  `Float4ToRGBA16` with dither 0, and no reached screen reads a framebuffer back
  (`[GFX-FBREAD]` is absent from every session-174/175 log), so dither can only
  change RDRAM bytes a screen never displays. It is a readback-path difference.
- **A dense browser mission series is still not captured.** The round's three
  browser frames come from Playwright screenshots that cost 12.9 s each under load
  average 15-50, so they sample the intro at three phases instead of every
  250-400 ms. `canvas.toDataURL` reads the same drawing buffer (the renderer creates
  its context with `preserveDrawingBuffer = true`) at 0.2-1.0 s per frame and is the
  replacement to finish.
- **The Load Game screen draws white before its content arrives** (session 174's
  item, unchanged).
- **The publisher stills `0x0A` do not present, and the natural boot never reaches a
  scene in the browser.** Six natural boots of 20 s each (10-66 shots per boot)
  printed no `[scene]` line at all and every captured frame was black; the forced
  entry advances 30 display lists (`texrect=136 fillrect=96 triangles=0`) but
  presents nothing. So the earlier exact match (`meanAbs 0.0`/100% over 639x475) is
  not re-confirmed.
- **The S2DEX2 opcode census is implemented and unexercised.** `[GFX-S2D-CENSUS]`
  counts every opcode byte of an S2DEX2 list and prints the undecoded ones, but no
  reached screen submits an S2DEX2 list (the njpeg backdrop in scene `0x02` does),
  so the claim that those lists carry only `0xDA`/`0xDC` still rests on session
  51's trace. `0x05`/`0x06`/`0x07` are the opcodes to watch: they fall through to
  the F3DEX2 triangle arms and would draw from an empty vertex buffer.
- **Unexercised by measurement**: `ZMODE_DEC`, the `G_AC_DITHER` hash, `G_TEXGEN`,
  `G_SHADE`, the depth image's RDRAM transfer, `a_w` in `gl_Position`, S2DEX2
  `0x01`/`0x02`'s full-matrix path, `OGRE_RAM_KEY_AT_MS` in a browser, and the
  deployed GitHub Pages site by hand.

- **The boot intro's soldier trails were the fill quad's second triangle
  (session 178).** The intro (`0x09`) draws 12 soldiers once per frame into a
  triple-buffered 320x240 colour image, each frame starting with one full-screen
  black `G_FILLRECT`. The renderer built that fill as six vertices in the order
  top-left, bottom-left, bottom-right, top-right, then paired them as
  `(0,1,2)` and `(3,0,1)`. The second pair repeats the first triangle's left
  edge, so both triangles covered the left half of the rectangle and the right
  half was never written. Every buffer therefore kept the previous frames'
  right half: as the ring of soldiers ran outward, each earlier position
  stayed on screen, and the frame filled with a wedge of repeated sprites
  clipped by the quad's diagonal. The developer saw it as "a triangle on the
  right where the sprites are not cleaned up". The two triangles are now
  `(0,1,2)` and `(0,2,3)`, which is the same index pattern the S2DEX object
  rectangle and the texrect already use. Verified against a native RT64
  capture of the same phase (`p.155`: three soldiers per corner and the cube,
  no trail): the browser frame at the same task now shows the same picture, and
  the synthetic controls still pass (`--flags 96` -> `fbtex hits=1 lines=1`,
  `--flags 128` near, `--flags 384` far, `--flags 512` alpha). The other
  screens whose fills are not full-screen (the attract story's letterbox) have
  not been re-measured since the quad got its second triangle.

- **The QUEST publisher still's q meshes do rasterize, and the reference frame
  is produced; a normal-speed run presents a white plate with no q colour
  (session 181).** Session 180 concluded that the q draws rasterize and that the
  presenting frames have a black plate. The first holds; the second is
  incomplete, because white-plate frames with q colour exist. The q draws do
  rasterize, and the renderer *does* produce the reference frame: a colour
  image read back after a q list is `white=279632, dark=2824` (the wordmark),
  `coloured=20588`, and the canvas read back right after the blit is
  `white=275957, coloured=20580`. But in a normal-speed run the QUEST hold
  presents `white=296537` of 303525 visible pixels, `dark=2192` (the wordmark)
  and `coloured=0`, i.e. the plate and the wordmark and no q colour. The native
  sequence was measured with `OGRE_CAPTURE_PRESENT` (`OGRE_CAPTURE_EVERY=2`,
  window 1366x768): the q's animate in on a black plate (`f.166..182`,
  `col 0..49766`), the white plate + q's + wordmark is held for about 12 presents
  (`f.184..194`, `white=716779, col=54320, green=21213`), then black and the
  title. The browser runs the same list sequence. What differs is which frame
  the batch presents: with one display list per flush (the probe build's slow
  main thread) the white+q frame is presented; with two lists batched in one
  flush (the normal build) the presented target holds no q colour even though the
  same batch records 756 triangles into it. The native display-list decode
  (`OGRE_DL_DECODE=all`, this session) gives the q list's exact recipe: `SETTIMG`
  + `SETTILE t7` + `G_LOADBLOCK 0..0x3FF dxt=0x80` + `SETTILE t0` +
  `SETTILESIZE 31x31`, `MOVEWORD segment[14]`, `G_DL 0x0E000000`; each q
  sub-list is `G_VTX` + `G_TRI2`/`G_TRI1` only. Ruled out for the q draws by
  measurement: NDC bounds (all 756 inside), finite positions and z, viewport
  640x481, scissor `(0,1,640,480)`, no alpha blend, no alpha compare, zero GL
  errors on triangle draws, correct FBO binding, and texture keys found (mean
  114/255). **Session 183 corrects two items in this bullet:** the q draws are
  depth-tested and depth-written (`OTHERMODE_L 0x00552078` sets bit 4 and bit 5,
  so `depth_test`/`depth_write` are both true), and the presented frame differed
  because the depth buffer held stale contents, not because of how many lists
  were batched. The reference frame in the one-list build and the missing-q frame
  in the normal build differ by one depth clear (session 183's A/B). See
  `docs/HANDOFF-2026-10-19-session181.md` and `-session183.md`.

- **The web renderer leaves a framebuffer's own texture bound, and WebGL then
  drops the next draws into it (session 181).** `upload_framebuffer` binds the
  colour image's texture to the active sampler unit and does not restore the
  binding, and `blit_to_canvas` leaves it bound too. The program's samplers are
  active for every draw, so a following draw into that framebuffer is a WebGL
  texture-feedback loop: `glDrawArrays` raises `INVALID_OPERATION` (0x0502) and
  the draw is skipped. A probe counted 86 `kind=1` (fill) and 16 `kind=2`
  (texrect) dropped draws in one run, e.g. `draw error 0x0502 kind=1
  fb=2E6840 fbtex=10590 tex0=10590`, which is why the stills' plate fills are
  dropped. A fix that restores the previous binding is not landed: it did not
  change the presented QUEST frame, and binding the 1x1 white fallback instead
  made the stills present black (the fallback changes what untextured draws
  sample). Land it with an A/B on the plate's RDRAM contents before trusting it.

- **The QUEST publisher still's white plate was a dropped PRIM-only rectangle
  (session 179).** Scene `0x0A`'s final list clears a 640x480 plate white, the
  target black, then draws 16 full-screen `G_TEXRECT` bands whose combiner
  evaluates to PRIM only. The renderer decoded the tile and dropped every
  rectangle whose tile had no image, and those bands use the preamble's
  unconfigured tile (`line=0`, zero rect), so all 16 were dropped and the frame
  kept the black clear; only the CI8 wordmark, which has its own tile, appeared.
  RT64 has no such test: `RDP::drawTexRect` drops only a non-zero tile with a
  zero window, and the sampler is never read when the mux does not name
  TEXEL0/TEXEL1. A rectangle now needs a decoded image only when the evaluated
  mux reads a texel (`combiner_uses_texture`, mirroring RT64
  `ColorCombiner::usesTexture`). The plate and the wordmark draw
  (`docs/proofs/web-publisher-quest-white-background.png`). The three q meshes
  were a separate fault, fixed in session 183: the still shares `0x2E6840`
  between its plate colour image and its depth image, and the depth target was
  not invalidated when the plate wrote that address, so every q fragment failed
  `GL_LESS` against stale depth. See
  `docs/HANDOFF-2026-10-19-session179.md`, `-session180.md` and `-session181.md`.

- **The opening can be captured as a 1 fps series in both renderers, and the
  browser's QUEST hold presents the reference frame at that sampling (session
  182).** `tools/intro_capture.py` writes one PNG per second from the game's
  first scene (not the runtime's blank first present), a contact sheet and a
  per-frame pixel report, natively and in the browser; the browser half is
  `debug/probes/intro-capture.cjs` and needs the GPU backend, because the
  software rasterizer's boot is much slower (one 40 s run: first non-black
  frame at 22.8 s, scene `0x09` at game time 27.6 s), so a 30 s window never
  reaches the stills. The native cadence needed a new RT64 knob,
  `OGRE_CAPTURE_EVERY_MS`: a present count is not a clock in this game, because
  the 320x476 mode presents per VI field and the 320x240 mode per frame, so one
  `OGRE_CAPTURE_EVERY` value ran at 1 Hz and then at 2 Hz after the title's mode
  switch. Measured at `OGRE_SPEED=1`: the native series holds the QUEST logo at
  frames 23-24 (`white=17519, colourful=1281` of the 160x120 sample) and the GPU
  browser at frames 24-25 (`white=17489, colourful=1309`). Two browser runs
  produced the reference frame, and the same frames appear with the probe's
  per-capture `_ogre_gfx_flush` disabled, so the series is not what makes the
  q colour present. See `docs/HANDOFF-2026-10-19-session182.md`.

- **The QUEST still's three q meshes render, and the cause was the depth image's
  contents, not the batch boundary (session 183).** Scene `0x0A` uses guest
  address `0x2E6840` as both the stills' plate colour image and their depth
  image. The q draws are depth-tested and depth-written: their `OTHERMODE_L` is
  `0x00552078`, and `cmd.depth_test`/`depth_write` are both true for that word.
  The renderer keeps one depth renderbuffer per depth image, so the plate's
  colour fills left the depth contents stale, and every q fragment compared
  against them and failed `GL_LESS`. The renderer now mirrors RT64
  `depthFbTypeChanged` (`hle/rt64_state.cpp:1581`, `:1630`): a colour draw whose
  target address a display list has named with `G_SETZIMG` queues that depth
  target for a clear, and the clear runs with the scissor disabled so a depth
  image shared by targets of different sizes is cleared whole. One build and one
  environment variable (`OGRE_PROBE183_NOINVAL`, removed afterwards) separate the
  two states: with the invalidation off, the held frame is
  `white=18797, colourful=0` (the session-181 missing-q frame,
  `docs/proofs/web-publisher-quest-no-depth-invalidation.png`); with it on,
  `white=17489, colourful=1309` (`docs/proofs/web-publisher-quest-q-meshes.png`).
  A 10 Hz capture of both renderers holds the reference frame for the whole
  still: native `white=17519, colourful=1281` for 1.9 s, browser
  `white=17489, colourful=1309` for 1.6 s, every 100 ms sample carrying both the
  plate and the q colour. The browser's final frame is `0.317` mean absolute
  difference per channel from native's over the 639x475 overlap. Session 181's
  batch/present explanation is withdrawn. See
  `docs/HANDOFF-2026-10-19-session183.md`.

- **The QUEST spin's q texture bytes are identical in both renderers; the
  difference was the geometry-mode mask, not the sampling (session 184, closed
  in session 185).** Same-moment
  whole-RDRAM dumps of the forced `publishers` still (`OGRE_RAM_KEY_AT_MS` +
  `OGRE_RAM_KEY_FULL=1` natively, the same query string read back from the wasm
  filesystem) give byte-identical `0x400`/`0x1400`/`0x2400` at 12.5, 14.5 and
  15.5 s, and at the spin `0x400..0x1400` matches 4096 of 4096 bytes: the same
  4 KB cloud-like environment map, 199 distinct RGB, max `(248,240,224)`. A
  temporary probe logged the browser's decode of that tile as
  `32x32 line=8 mean=91,75,35 max=248,240,224`, so its TMEM load and decoder
  reproduce the guest image; session 183's "brown gradient" is the same decoder
  at `t=7.5 s`. The spin's sub-list installs lookat vectors and sets
  `G_TEXTURE_GEN` but clears `G_LIGHTING`, and RT64's `usesTextureGen` requires
  both, so an implementation of `G_TEXTURE_GEN` plus the `MV_LIGHT` slots 0/1
  changed no pixel and was reverted. RT64's per-workload framebuffer writeback
  (`hle/rt64_state.cpp:1655-1670`), which the browser lacks, was also ported;
  it made the q frames darker and flatter and was reverted. Native has a
  near-white texel in 24 of 24 sampled spin frames, the browser in 3 of 14, at
  silhouette IoU up to 0.985. Session 185 closes the item: `w0`'s low 24 bits
  are a keep mask, so the sub-list keeps `G_LIGHTING` and RT64 does generate
  the coordinates; the browser now mirrors that and has a channel above 200 in
  26 of 27 spin frames. See `docs/HANDOFF-2026-10-23-session184.md` and
  `-session185.md`.

- **The natural title drew noise and then only its upper half: a shared-depth
  framebuffer and a viewport sized from the colour image (session 186).** The
  title after scene `0x0A` renders into 320x476 colour images and names the
  depth image `0x2E6840` that the publisher still created at 640x481. Depth
  targets were keyed by the `G_SETZIMG` address alone, so the title attached
  that 640x481 renderbuffer to a 320x476 colour texture; GL reports
  `GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS` (`status=0x8CD9`), rejects every draw,
  and the canvas keeps the colour texture's undefined contents (colour stripes
  over black, `sha256 06bff23c…`). Depth targets are now keyed by
  `(address, width, height)`, which is also RT64's `RenderTargetKey` shape, and
  a colour write to a depth-image address invalidates every size-keyed buffer.
  With that fixed the title showed only its upper half, stretched: its 320x240
  rects record with `screen=320x240`, but the colour image is 320x476 (an
  earlier draw grew it) and `glViewport` used the image's full height. The
  viewport is now per draw (`fb.height - cmd.fb_height_px` at the top), the same
  mapping as the scissor. The natural title then draws the full prologue scene:
  the text bands `34..74`, `78`, `87..130` match the native capture's exactly,
  the cloud and rock fill the frame, the logo follows
  (`docs/proofs/web-title-prologue-session186.png`,
  `-web-title-vs-native-session186.png`, `-web-title-logo-session186.png`), and
  the run advances to `0x0B`; the fixed title animates (220 title frames and
  198 distinct hashes in a 48 s 100 ms capture). The browser's `OGRE_TEX_TRACE`
  gained the native renderer's `OGRE_TEX_TRACE_AFTER_MS`/`_MS` window, because
  the trace budget was previously spent on the publisher stills before the
  title. See `docs/HANDOFF-2026-10-23-session186.md`.

See `docs/HANDOFF-2026-10-19-session179.md` and `-session178.md` for these two
fixes, and `docs/HANDOFF-2026-10-18-session176.md` / `-session175.md` for the
numbers and the screens those rounds did not re-measure.

Session 194 found the mission briefing's green hairlines are a texture band
sampled one row past its own art. The framed illustration is two RGBA16 images
the game composes in RDRAM, drawn as `G_TEXRECT`s at `dl=23`: an L-shaped
frame of 19 rects, each sampling one 92x4 band (`line=23`, `lrs=364 lrt=12`,
`mask=7/5`, `cm=0/0`), and the 84x67 picture in three blocks (`line=21`).
Their transparent texel is `0x07C0`, pure green with alpha 0. RT64's
`loadTileOperation` on the dumped source reproduces the dumped TMEM over all
736 bytes the load writes, and the decode reproduces the source texels
exactly, so the green is the game's data and not a decode fault; session 193's
CI4/1-row lead is withdrawn. A 4-row band drawn over 4 pixels puts the last
pixel's centre at `t=3.5`, whose bilinear taps are rows 3 and 4; row 4 is
outside the band and holds the previous occupant of that TMEM base, so the
transparent green is mixed with an opaque texel and drawn at alpha near 0.5.
`OGRE_TEXWINCLAMP=1` bounds a wrapped sample to the tile's rect and removes
the band-seam hairlines and the notch stripes, but not the frame's own green
outline or the pin's, which are filtered across the boundary between opaque
art and the same image's transparent green texels. The switch is off until the
`OGRE_FOG` layers and the map's water are compared with it on. New
diagnostics: `OGRE_FRAME_DUMP`, `debug/probes/framedump.cjs`,
`debug/probes/stripab.cjs`. See `docs/HANDOFF-2026-10-25-session194.md`.

Session 195 withdrew session 194's fix and its reading of the cause. The browser
renderer sampled every rectangle half a texel low and right: its corner UVs sit
on the rectangle's edges and its fragments sample at pixel centres, while RT64
draws a rectangle shifted half a pixel right and down (`halfPixelOffset`,
`src/render/rt64_framebuffer_renderer.cpp`) so that pixel `i` of a 1:1
`G_TEXRECT` samples texel `i`. `OGRE_RECT_HALFPIX` (on by default) subtracts half
a step from the corner UVs of `draw_texrect` and of the S2DEX object rectangle,
which is the same sample coordinate as RT64's shifted quad and leaves the
rectangle's coverage unchanged. It removes all green from the illustration:
0 green-dominant pixels in its rectangle against 398 with the switch off, and
its mean absolute difference from RT64's own 320x240 framebuffer is 2.44 in two
on runs against 22.21 and 22.20 in two off runs, and the map's upper band
improves too (3.02/3.06 against 8.06/7.94) while the NOTE band and the map's
lower band do not move. The frame's own green
outline and the pin's are gone too, so session 194's open item 1 is closed by the
same change, and `OGRE_TEXWINCLAMP` is confirmed as a symptom bound that stays off
by default. RT64's framebuffer confirms the convention: 144 of 213 fully opaque
band samples equal the decoded texel exactly and the other 69 differ by one
5-bit step in one channel (the writeback dither); none is a half-texel mixture.
A native run reaches the briefing with a fresh `OGRE_PREF_DIR` and the route
`title:start:6,0x12:a:6`; session 194's runs that landed on the map reused a
battery the game had already rewritten. New probe: `debug/probes/rectab.cjs`.
See `docs/HANDOFF-2026-10-25-session195.md`.

Session 196 withdrew session 195's "the mission field is flatter and darker than
native". At the same Castle Dunkweld NOTE screen, the browser's static field
regions are within 1-3 levels of RT64's mean and their standard deviation and
gradient are higher: left field mean `[26.8,28.4,13.3]` against
`[26.3,27.8,12.4]`, standard deviation `[16.9,16.0,17.3]` against
`[13.9,12.6,13.8]`, mean absolute difference 4.82 (right 5.25, bottom 6.19,
top-left 1.20, top-right 2.09). The Castle Burgund briefing reads the same way
(left field `[30,32,1]` against `[30,33,3]`). `OGRE_RECT_HALFPIX` changes no
pixel on the boot intro or a publisher still: the static holds are byte-identical
between the switch-on and switch-off runs. Session 195's item 1 is therefore
closed for those screens and the briefing. A live switch, debug flag bit 15
(`ogre_gfx_debug_flags(32768)`), and `debug/probes/recttoggle.cjs` were added for
a same-run A/B; the probe did not reach the briefing under host load 11-28 (two
attempts waited 33 and 36 minutes), so its same-run pair is still to run. The
mission's framed illustration is clean on this build
(`docs/proofs/web-mission-briefing-illustration-session196.png`). See
`docs/HANDOFF-2026-10-26-session196.md`.

Session 197 ran the same-run rectangle A/B on one build and one screen. The
blocker was the probe harness's SwiftShader launch: with
`--enable-gpu --use-angle=metal` the briefing is reached in about seven minutes
instead of 33-36. Triggered by the illustration's `[GFX-FRAME-DUMP]` line and
the route that holds the briefing (`title:start:6,0x12:a:6`), the switch-on
frame has 0 green-dominant pixels in the illustration rectangle, the switch-off
frame 399 (175 on the seam rows) and the switch-on-again frame 0. A scene-line
trigger crosses the briefing's NOTE-page change and is not usable. The session
also applied RT64's `checkRAM` test to `OGRE_FBTEX`: each `GlFramebuffer` keeps
an XXH3-64 of its RDRAM at the last upload/writeback, the gfx thread refuses the
GPU copy when the current bytes differ (one hash, no flush round trip), and the
main thread repeats the check before substituting `exact_rects`. With
`OGRE_FBTEX=1` the briefing's NOTE and label now match the copy-off frame
(`meanAbs 0.057`/`0.024`, max 1) instead of the white bars of session 193;
`[GFX-FBTEX-STALE]` reports four refusals and there is no sync timeout or crash.
The copy stays opt-in because a copy that does fire still reads the whole image
back. The top-of-file limitation list is corrected: TEXEL1, `G_TEXTURE_GEN`,
lighting and LOADBLOCK `dxt` are implemented. Open: `G_TEXRECTFLIP`'s half-step
is `0.5*dv` where its quad interpolates `dv*h/w`, copy-mode texrects and
fractional rect coordinates do not reproduce RT64's `dsdx >>= 2`/edge pad and
`vFractionOffset`, and the title residual is rows ~81..119, not the
session-159 rows 195..232. The world map/title A/B still needs a reachable
state: `map` is not a scene name and the `OGRE_SCENE=0x05` retry did not reach
the map, `OGRE_SCENE=title` landed on `0x09`/`0x0A` in session 196, and a natural
title capture landed on the idle trajectory (`id=0x0000`). See
`docs/HANDOFF-2026-10-26-session197.md`.

Session 198 fixed the browser's mission and tutorial-field frame rate and
bounded the reported flat grass. The flush loop resolved every uniform with
`glGetUniformLocation` on every draw; a CDP profile of the mission screen spent
27% of the main thread in that lookup and its UTF-8 conversion, and 12% in the
once-per-flush `glGetError` GPU sync. `WebGLRenderer` now keeps a
`uniform_cache_` (`std::string_view` -> `GLint`) filled once after the program
links, and the error check is `OGRE_GL_CHECK=1` and off by default. The Castle
Burgund mission, same save and route, measures **16.78 -> 26.10 fps** in one
8 s window and **28.94 fps** on a re-run; the tutorial practice field measures
about 20 fps. The grass defect reproduces on the practice stage (scene `0x0003`
after the tutorial's `0x17 -> 0x02 -> 0x0D` sequence): TEXEL0 raw turns the grass
pure white while the cliffs and roads keep their textures, and `OGRE_FOG=0`,
culling off and the depth clip off each change no pixel. The large `64x64` CI4
tile at the field is the top-left UI panel (its decoded parchment image matches
its source), and a second layer of `32x32` RGBA16 tiles blends
`(TEXEL0 - TEXEL1) * PRIM_A + TEXEL1` with `PRIM_A=0.5`; the grass layer itself
is not among the logged large draws, so small textured rectangles are the next
check. See
`docs/HANDOFF-2026-10-26-session198.md`.

Session 199 measured the mission's remaining frame cost and advanced the grass
question. A draw merge that emitted consecutive same-state draws as one
`glDrawArrays` cut the mission's calls from 1 410 777 to 164 198 (8.6 draws per
call) and changed the rate by nothing: **29.39 fps off, 29.83 fps on** over the
same 10 s window, with session 198's uniform cache in both (the final binary, that cache only, reads 28.66 fps). A CDP profile of the
mission spent **87.1% of the main thread idle**, so the remaining 2x against
RT64 is outside the flush loop (the recompiled game/RSP threads or the GPU); the
merge was reverted. The practice field's ground at N64 `(12,160)`, 12 s after
scene `0x0003`, is covered by **five `G_FILLRECT`s and no textured draw**: the
last fills are `prim=(0.13,0.13,0.00)` and `(0.13,0.16,0.00)`, and the frame
samples `(34,42,2)`, which is that fill colour. The field's textured 32x64 CI8
ground tiles (`cmb=FC127E24/FFFFF9FC`, `0x2FB540` and eight others) cover the
cliffs and roads only; the green ground family (`FC151604`, `0x358F00`,
`masks=32`, `shifts=13`) draws at the top-left and off-screen
(`x -201..37`, `y -45..10`). `OGRE_DEPTH=0`, culling off and valid shared
matrices each change no pixel, so the defect is upstream of the combiner and the
sampler. Proofs `docs/proofs/web-tutorial-grass-flat-session199.png` and
`-texel0-session199.png`; see `docs/HANDOFF-2026-10-26-session199.md`.

Session 200 replaced the throwaway route scripts with `debug/probes/scene.cjs`:
one command boots the build, drives a named route, waits for the `[scene]` line
and writes a PNG per `ogre_gfx_debug_flags` value plus sampled pixels, the page
log, the scene timeline and the `tasks=` rate. The routes are data (`title`,
`mission-briefing`, `tutorial-field`, `tutorial`); a new screen is a `ROUTES`
entry with `waitScene`/`sleep`/`press`/`pressUntilScene` steps. The field capture
also gives a cleaner grass repro: at the "Movement & Liberation" lesson N64
`(12,160)` is `(33,33,0)` normal and pure white in `TEXEL0` raw, an untextured
fill. See `docs/HANDOFF-2026-10-26-session200.md`.

Session 201 fixed the tutorial practice field's ground, in two steps. A triangle
that crosses the eye plane was dropped whole: `transform_vertex` returned false
for a vertex with clip `w <= 0` and `draw_tri` dropped the triangle, so the field's
76x76-unit ground quad (near corner `w = -1.67`) never drew and the ground was the
frame's fill. The renderer now cuts such a triangle against `w > 1e-3`
(`emit_clipped_tri`). The ground then drew but its tile was stretched into straight
bands, because the vertex shader interpolated the texture coordinate in screen
space (session 172) while RT64 interpolates it perspective-correctly
(`hle/rt64_rsp.cpp` stores it undivided, `shaders/RasterVS.hlsl` multiplies the
position by `w`). The shader now writes `vec4(a_pos * a_w, a_z * a_w, a_w)` by
default, `OGRE_PERSP=0` restores the old path, and the grass is the noisy texture
native shows. The mission briefing and the title measure within 1.4 mean absolute
difference of the screen-space frames. New diagnostics: `OGRE_DRAW_PROBE` (every
draw covering a pixel), the `[GFX-FAMILY]` census, `OGRE_ONLY_COMB` and a
`1<<17` alpha view. Proofs
`docs/proofs/web-tutorial-field-{flat,bands,}-session201.png`; see
`docs/HANDOFF-2026-10-26-session201.md`.

## Status

The game runs end to end. A build boots the publisher stills, the title, the New
Game opening, the map, missions and battles, the tutorial, the shop, the
Organize Screen, the credits, the ending and the attract loop.

- **Issue #24, items 1-4 (session 213)** — four items from the Windows fork in
  `issues/24` landed. RT64 now runs with `upscale2D = Original`
  (`app/src/renderer.cpp`), so 2D rectangles sample at the console's pixel
  positions; at 3x the dialogue box drew a dashed row under its shadow and a dark
  column beside the portrait with RT64's `ScaledOnly` default, and neither is
  present with `Original` (`docs/proofs/native-dialogue-upscale2d-*.png`).
  `OGRE_SCENE` stores the game's own request `0x8000 | id` in `D_800C4C26`
  instead of poking the state block and the root word; verified with
  `OGRE_SCENE=title` and the cathedral route. Two calls `func_80079BD8` makes into
  record 3 (`0x801AB740`, `0x801AB76C`) are dispatched through `LOOKUP_FUNC`, and
  `tools/stubmap.py`'s entry test no longer accepts body interiors as function
  starts (130 missing-function candidates -> 0). See
  `docs/HANDOFF-2026-11-02-session213.md`.
- **Issue #22, items 1 and 2 (session 212)** — the Esc overlay's MAIN tab has a
  RESTART GAME row between ROM and EXIT GAME. It ends the run through the same
  teardown as EXIT GAME, then spawns a new copy of the executable with the same
  arguments (`posix_spawn`, or `_wspawnv` on Windows) and leaves with `_Exit`.
  `execv` hung on a libc lock held by the runtime's unjoined game threads.
  `OGRE_RESTARTED=1` keeps the new process off the start screen when a mod is
  installed. `mods/permanent-suspend/` patches `func_80074834` (`.main`) to skip
  erasing SRAM record 15 when a suspend save is loaded, so the same suspend
  point can be loaded again after a relaunch. Verified on the real renderer: the
  vanilla load turns `0x30B0` from `dc41 9ec6` into `00000000` and the patched
  one leaves the 32 KiB image byte-identical, and a second process then resumes
  the same save to scene `0x03`. The restart was measured with the null renderer
  and `SDL_VIDEODRIVER=dummy` (two boots in one log); it has not been pressed in
  the RT64 window. See `docs/HANDOFF-2026-11-02-session212.md`.
- **Input** — the left stick's Y sign matches the N64 pad on both builds
  (session 211). SDL's left-stick Y is negative up and the Gamepad API's axis 1
  is also negative up, while the N64 pad's `stick_y` is positive up. The native
  path passed the SDL value through, so pushing up moved the world-map cursor
  down (issue #21); the browser had the same fault. Both now negate the axis.
  The game's own consumer is `func_ovlM_8019AF0C`, which computes
  `screen_y - stick_y / 8` at `0x8019B490`. The right-stick C layer already read
  SDL's convention. See `docs/HANDOFF-2026-11-02-session211.md`.
- **Recompilation** — 99.05% of the ROM code span `0x001000..0x2B8B70` is
  compiled; the one gap (`0x0DDF80..0x0E4910`) is data. 44 records in 34 bank
  units (`BankA`..`BankZ`, `BankAA`..`BankAH`), 5037 registered function entries
  at 4963 addresses, 986 static dispatch targets with 0 unresolvable.
  `tools/recompcov.py` measures all of it.
- **Streamed code** — no un-recompiled module remains on any reachable path
  (session 85). A hand-played session executes 44.4% of the entries; the rest
  are paths nobody has played.
- **Windows build** — the Release workflow's Windows job compiles the app with
  clang-cl against the MSVC and UCRT headers, and those headers declare no
  two-argument `mkdir`. The trace dump directories now use
  `std::filesystem::create_directories` (`ensure_dir` in `renderer.cpp`), the
  call the rest of the app uses, and the job's four errors are gone (session
  135). A hosted run needs the private data bundle regenerated for the commit it
  builds, because the unpack step refuses a bundle whose recorded commit differs
  from `GITHUB_SHA`; `platforms=windows` is the ~8-minute iteration path.
- **Browser renderer** — the New Game cathedral backdrop now renders
  (`docs/proofs/web-newgame-cathedral-session203.png`). Session 203 found and
  fixed two faults in the browser renderer. `tmem_load_yuv` indexed the njpeg
  macroblock's source word with RT64's native byte order through an accessor that
  already applies the guest `^ 3` permutation, so luma and chroma were exchanged
  and the backdrop drew as green/magenta stripes; it now reads the logical
  `[U, Y(even), V, Y(odd)]` order, and the TMEM planes match the values session
  51 verified (`luma 28 27 25 24`, `chroma 7A 9A 7B 96`). The framebuffer
  writeback recorded its row band from NDC scaled by the colour image's own
  height, so a 320x240 image drawn into the 320x476 target the title grew marked
  476 rows as drawn and copied 236 rows over the next framebuffer; the band now
  uses the same viewport mapping the draws render through, and the reference hash
  is taken when a draw era starts (RT64's `hashTracking`) and re-tested once per
  batch (RT64's `checkRAM(updateHashes = true)`). Before the row fix the first
  njpeg pass copied a black framebuffer; before the hash fix every pass did. The
  session-202 writeback guard keeps its `[GFX-FBWRITE-STALE]` line. Title,
  tutorial field, mission briefing and the tutorial battle still render. See
  `docs/HANDOFF-2026-10-30-session203.md`.
- **Browser cathedral backdrop, second visit** — session 207 fixed the black
  top-left quadrant that appeared after the first New Game question
  (`docs/proofs/web-newgame-cathedral-birth-session207.png`). The writeback guard
  compared the colour image's whole `row_bytes * height` range, and a 320x476
  target at `0x025C00` spans `0x025C00..0x070200`; the game's next framebuffer at
  `0x04B400` is row 240 of that target, so its writeback changed the hash and
  the whole pending band was dropped. The band also mixed the previous screen's
  rows with the pass's rows, and the epoch reference kept the previous screen's
  reference for a row the pass redrew. `GlFramebuffer::era_row_hash` now holds one
  reference per guest row, refreshed immediately before each draw into the row;
  `readback_framebuffer` compares and skips row by row. The first pass of the
  `0x025C00` assembly copies `69915/76800` pixels instead of `14875`. See
  `docs/HANDOFF-2026-10-31-session207.md`.
- **Browser GAME SPEED** — 2x and 4x hold the requested speed and the display
  holds 60 Hz (session 208). Before, the guest reached 188.8 frames/s of the 240
  asked for at 4x, and the page's requestAnimationFrame cadence was 50 ms at its
  95th percentile; at 1x both were fine. `flush_commands()` held the renderer
  mutex across its whole GL batch, and the gfx pthread publishes every display
  list and present through that mutex, so the GL batch paced the game: with the
  flush disabled, the same 4x run reached 239.9 frames/s. The lock now covers
  only the queue exchange. The flush also uploaded ~66 uniforms per draw, 1.88 M
  WebGL uniform calls/s at 4x; `draw_uniform_key()` skips the block when a draw's
  values equal the previous draw's, which took the calls to 495 353/s and the
  mean flush from 11.4 ms to 5.4 ms. Measured after, same page and screen: 1x 60.0,
  2x 119.7-120.2, 4x 234.5-240.7 frames/s, rAF p95 16.67 ms at all three; 8x
  reaches 414.8 of 480. The title capture is identical to its pre-change one,
  and the tutorial field and mission briefing keep their composition. See `docs/HANDOFF-2026-11-01-session208.md`.
- **Browser audio** — the page plays the game's own audio (session 204). The
  audio microcode runs in the wasm build (`[rsp] task type 2 submitted (audio
  microcode)`) and the ring in `app/src/web_platform.cpp` holds real PCM at the
  game's 32 kHz rate: the title's ring peak is 2073-4237 across runs, and its
  waveform's zero-crossing rate matches the native capture's. Six page-side faults
  kept it silent, stale or distorted. `initAudio()` was only called from the ROM
  file-picker path, so a session restored from the stored ROM never created an
  AudioContext; that call is a promise continuation, which is not a user
  activation, and the early `if (audioContext) return` meant a gesture could not
  resume it. SOUNDS defaulted to off, from a session-22 comment that predates the
  recompiled audio microcode (session 75). The worklet's ring view used
  `ringPtr / 2` as its byte offset, so it read unrelated wasm memory, and it wrote
  int16 magnitudes into a float output without dividing by 32768, which is the
  clipping the session-22 note called "screeching". It also started at ring index
  0, so a context that attached late played stale audio. `initAudio()` is now
  idempotent, runs from `startBoot()`, the file picker and the first pointer/key
  event, and resumes a suspended context. The worklet reads the ring at its byte
  address, scales each sample by `1/32768`, and adopts the producer's newest frame
  on `init` and `resync` (posted when a suspended context resumes).
  `get_frames_remaining` now reports int16 samples like SDL (`(head - tail) * 2`)
  instead of stereo frames, which restores the 52 ms AI pacing cushion. Two
  cursor faults then stalled the game's audio thread after the first drain:
  `tail` dropped the fractional frame every quantum, and a 1e-12 disagreement
  between the float `readPos` and the integer `tail` was treated as a desync and
  moved `readPos` to `head`, skipping every queued frame, so `head - tail` looked
  like a backlog and `audio_dma_busy()` stopped the producer. `pendingFrames`
  carries the fraction, and the guard clamps to `tail`; only a real underrun
  moves to `head`. Headless Chromium renders through SwiftShader, where the
  frame-paced audio task rate is 2-3 buffers/s; a headed ANGLE Metal browser
  measures 57.4 tasks/s, 31 758 ring frames/s and continuous output at the title
  (0.33 peak), the tutorial (0.52) and the mission (0.77). Sound is
  on by default, as in native; SETTINGS > SOUNDS and `?audio=0` mute, and VOLUME
  scales the worklet's output. The page also retries `resume()` on window focus,
  `pageshow` and visibility, and raises a `SOUND PAUSED · CLICK TO RESUME` button
  over the screen when browser autoplay policy still keeps the context suspended;
  the stored-ROM auto-start has no gesture, so the first click anywhere starts
  it. `debug/probes/audio.cjs` measures the ring and the worklet's own output
  levels: output peak 0.099-0.117, muted peak 0 while the ring still drains,
  unmuted again 0.245-0.294. Open: listen on a real output device; the probe
  measures samples, not speakers. See
  `docs/HANDOFF-2026-10-30-session204.md`.
  Session 205 measured the mission's "laggy" sound and found the worklet is
  starved, not at fault. The game's audio thread generates one 552-frame buffer
  per RSP completion, and a completion is delivered only while a game thread is
  blocked or yields, so the mission's ~30 fps frame rate lowered the rate below
  the 58 buffers/s the 32 kHz DAC consumes: the title read 56.3-58.0 buffers/s
  with 0-2.6% worklet underrun, the mission 40.8-57.4/s with 1.5-29.8%, and the
  tutorial field 57.0/s with 1.9%. Native audio has the same pacing: the SDL
  path queues with `SDL_QueueAudio`, reports `SDL_GetQueuedAudioSize()/2`, and
  uses the same 52 ms `audio_dma_busy` cushion; retail renders the mission at 60 fps (session 199),
  so its frame-paced audio thread reaches 58 buffers/s. The browser runtime now
  runs `M_AUDTASK` on the submitting thread (under a mutex around the shared RSP
  DMEM, with `OGRE_AUDIO_INLINE=0` as the A/B), so the completion is queued
  before the game's `osRecvMesg` and the audio thread refills the cushion in one
  visit. The mission measures 57.8/s with 0.1-0.4% underrun and the tutorial
  field 58.0/s with 0%. The 30 s cathedral capture was the backdrop still
  assembling; at a 60 s settle it is complete. Posting the completion straight
  to `0x800C49E8` hung the title and is not used. The worklet's `levels` reply
  carries `underruns` and `underrunFrames`, and `audio.cjs --wait-scene 0x0003`
  measures the mission. See `docs/HANDOFF-2026-10-31-session205.md`.
- **Browser public page** — session 209 added the two things the GitHub Pages
  release needed beyond the static package: an experimental notice and a battery
  save export. The launcher card states that rendering, audio and saving are
  incomplete, that scenes can crash, and that the build is unofficial; the
  Options MAIN tab repeats it. The topbar "Export save" button and the Options
  MAIN "Export battery save" row read the runtime's
  `/ogre/saves/ogrebattle64-us-rev1.bin` out of the wasm FS and download it as
  `ogrebattle64-us-rev1.bin`, the raw 32 KiB logical image the native build reads
  from `<config>/saves/`. A read that is not 32 KiB or lacks the `QuestOG3`
  device signature is retried once, because the runtime writes through a `.temp`
  file and copies it over the image. Verified on the static package with the
  COOP/COEP service worker: the imported `assets/escort_suspend.bin` round-trips
  byte-for-byte (same SHA-256), and `tools/sramsave.py check` validates both slots
  and the map record. A blank boot writes the file from the runtime's saving
  thread (`[save] wrote /ogre/saves/... (32768 bytes)`), so the export reads the
  bytes the game last committed. `debug/probes/save.cjs` is the regression
  check. See `docs/HANDOFF-2026-11-01-session209.md`.
- **Renderer** — RT64 (Metal, Vulkan, D3D12) with a null renderer for bring-up.
  RT64's **"Fix LR with Scissor"** rect enhancement is off by default here
  (`OGRE_RECT_LR_FIX=1` restores it): it rewrites a rectangle's right edge to the
  scissor's edge whenever the two are within one pixel, which widened the unit
  profile's rightmost DEF/MDEF digit rectangle and made it sample the next font
  cell (session 101). RT64's framebuffer tile copy is refused when the tile's
  texels lie outside the framebuffer's `lastWriteRect`, the rect the copy is
  actually filled from, and when the source framebuffer was not written in the
  workload being processed (`Framebuffer::lastWriteWorkload`, printed as
  `[fbcopy-nowrite]`); a texture load then reads RDRAM. The first condition fixed
  the flat ENV-coloured block on a map unit's head (session 134), the second the
  same block in a second cutscene, where the record was a leftover from an earlier
  scene whose stale rect covered the tiles the game had put its sprite scratch on
  (session 136, open work 0).
- **Audio** — the game's own audio microcode is recompiled and runs by default.
  The Linux package needs a static SDL2 with an ALSA, PulseAudio or PipeWire
  backend. The 0.3.0 build host had no audio development headers, SDL2 dropped
  all three without failing, and the shipped binary ran silently on every
  PipeWire and PulseAudio machine (`dsp: No such audio device`). `make
  sdl2-static` and `tools/smoke-dist.sh` now fail instead of shipping that
  (session 100, issue #8).
- **Saves** — the cartridge battery is SRAM and works; the Controller Pak menus
  render.
- **Player features** — tabbed launcher, in-game `ESC` overlay, rebindable
  controls, GAME SPEED, WIDESCREEN, mods, `make dist` packages, GitHub releases.
  The modding framework has a third example mod, `mods/item-randomizer/`: it
  randomizes the item the game grants for a map pickup and for a battle reward,
  and recomputes the item screen's used count (`PLAN.md` open work 16). Session 122 also
  bounded the mod-facing diagnostics with a new `recomp_log` export, because a
  code mod is compiled without libc and cannot print otherwise. The mod is
  marked EXPERIMENTAL in its manifest and has no config options (session 125):
  every eligible item is rerolled over the whole item table, the seed is derived
  from the frame counter each boot, a shop purchase is left alone, and the
  diagnostics are the compile-time `LOG_LEVEL` in the source. A frame that fills
  more than one item record is the game's own bulk grant — a save load, or a map
  load's starting items and the created units' class equipment — so the mod
  re-primes its shadows and leaves those items as the game granted them
  (session 126).
  A gamepad drives both panels (D-pad moves and steps, `A` selects, `LB`/`RB`
  switch tab, `SELECT` opens and closes the overlay when no binding uses it), and
  the SETTINGS tab carries WINDOW (`windowed` / `fullscreen`) beside WIDESCREEN,
  applied live by `RT64Renderer::update_config` and persisted in `settings.cfg`.
  A fullscreen window's geometry belongs to the renderer, so the port's
  WIDESCREEN fit is skipped while fullscreen and re-run once the renderer has
  restored the window. GAME SPEED offers **1, 2, 4 and 8** (8 is back at the
  developer's request, session 137), `=` and `-` step it while the game runs, the
  pad's **R3** cycles it, and a two-second badge draws `2x`/`4x`/`8x` in the game
  window's lower-right corner on any change. RESOLUTION (`native` / `2x` / `auto`) and MSAA
  (`off` … `8x`) are implemented on the same path but ship **behind
  `OGRE_DISPLAY_EXPERIMENTS=1`**: a player run with them produced visual
  artifacts (developer, session 121), so their rows are absent and their values
  are not applied by default (session 121).
  `make release VERSION=vX.Y.Z` is the supported way to publish: it regenerates
  the data bundle the hosted runners unpack, proves the packed code is the
  tree's, commits and pushes it to the private data repository, and dispatches
  the Release workflow with `draft=true` (session 120; `make release-check` is
  the same path with `NO_PUSH=1`).
  The launcher and the overlay draw one `ui::Panel`: the tabs are MAIN (which
  carries the ROM row and EXIT GAME), MODS, CONTROLS, SETTINGS and DEBUG, and the
  panel reserves the CONTROLS tab's height so the header, the tab bar and the
  footer keep one position on every tab (session 104; MAIN replaced START GAME in
  session 110). `mods/skip-boot-logos/` is the
  mod reference; `mods/exp-overflow/` carries a level-up's leftover EXP over
  instead of letting the game zero it (session 103). The package carries them:
  `make dist` and `tools/smoke-dist.sh` require at least one `.nrm`, and the
  hosted release runners get them from the private data bundle (session 108).
  WIDESCREEN is a toggle (`off` / `on`): `on` turns RT64's Expand aspect ratio
  on only while scene `0x03` runs, so the mission field is hor+ 16:9 and the 4:3
  2D screens are untouched (sessions 102, 105). The window shape follows the
  toggle and never the scene: `off` opens 4:3 with no pillarbox, and `on` opens
  16:9 with the 4:3 scenes pillarboxed inside. SOUNDS is a radio group
  (`off` / `on`) and VOLUME a 0..100 slider; the audio callback scales every
  queued buffer by the gain and queues silence rather than nothing at `off`, so
  the runtime's queue-depth pacing is unchanged (session 106). A connected pad
  fills `Platform::controllers[0]`, the N64 controller the game reads, with the
  keyboard OR'd into the same slot; pads used to fill slots 1..3, so a pad was
  visible in the launcher and the overlay and invisible in game (session 118,
  issue #13).
- **Mouse (world map)** — the map screen's cursor follows the pointer, a left
  click acts as the game's `A` on the location under it, and pushing the pointer
  past the map's edge scrolls the view. The operating system pointer is hidden
  while it is over the game window and shown over the ESC overlay window. The
  cursor is the map module's screen-space sprite at `state+0x52`/`+0x54`
  (`state = *(u32*)0x80197B18`, clamped by `func_ovlM_8019AF0C` to x
  `0x28..0x110`, y `0x28..0xC0`), written at the scene hook's cursor draw
  (`func_ovlM_801A103C`, `0x801A1C48`). The draw site is bank code, so
  `tools/map_cursor.py` inserts the call and `make bank-recomp` re-applies it
  (session 127). The click and the edge direction ride `get_input`, the path the
  keyboard uses, and are gated on the map's cursor draw so they are inert on
  other screens. **Both this screen and the mission field are behind
  SETTINGS > INPUT > MOUSE ENABLED, off by default** (session 129): the row
  publishes to `ogre_map_cursor_set_enabled` / `ogre_mission_cursor_set_enabled`,
  and `OGRE_MAP_CURSOR=1`/`=0` still forces this screen for a scripted run.
  `OGRE_CURSOR=0` keeps the operating system pointer visible
  (`docs/guides/app-build.md` → "The world map's mouse cursor"). The merged
  contribution that claimed this feature had no caller and is deleted.
- **Mouse (mission field)** — the mission screen's cursor follows the pointer and
  a left click acts as the game's `A`. The cursor is two f32 framebuffer pixels
  at `state+0x08`/`+0x0C` (`state = *(u32*)0x801AB870 = 0x800E91D0`, a fixed
  address), kept by the mover `func_ovlN_801BE4E0` and clamped to x 40..280,
  y 30..210. The hook is inserted at the generated label `L_801BEC04`, after both
  clamps, by `tools/mission_cursor.py` and re-applied by `make bank-recomp`
  (session 128). **The mission's cursor is digital (D-pad) where the map's is
  analog (the stick), and it is a servo the game moves on nearly every frame**
  (1-9 px with nothing held, measured), so the pointer hands it to the game only
  on an explicit signal — a direction held, the C-LEFT centring branch, the
  mover's own delta, or the window after a click — and adopts the game's position
  then. The click window is what leaves the cursor on the place a command acted
  on. **The view scrolls when the pointer is close to a play-area edge**
  (session 129): the field's camera pans with the cursor, not with a held
  direction. The mover turns the held D-pad into the delta bytes
  `state+0x06`/`+0x07`, and `func_ovlN_801BECF0` (called right after the mover at
  `0x801AF130`) adds `delta/128 * 8.5 * table[e]` to the eye and the look-at x of
  the camera at `*(u32*)0x8018F5AC` (`0x801BEE04`-`0x801BEE1C`; the same for z at
  `0x801BEE9C`-`0x801BEF5C`), clamped to the world rect at `0x801F0DD8`, and only
  while `*(0x801F3630)` is set, which the mover sets only when the cursor pair
  changed. The pointer's pixel is clamped by the mover, so the edge intent
  arrives as input: within `OGRE_MISSION_CURSOR_EDGE` (24) px of a bound the hook
  holds the matching N64 D-pad bit, `get_input` ORs it into controller 0, and the
  `pad=` signal already makes the pointer adopt the game's cursor, so the two do
  not fight. Developer-confirmed. Both this screen and the world map are behind
  SETTINGS > INPUT > MOUSE ENABLED, off by default (session 129), and
  `OGRE_MISSION_CURSOR=1`/`=0` forces this screen for one run
  (`docs/guides/app-build.md` → "The mission field's mouse cursor").
- **HD backgrounds (experiment)** — `mods/backgrounds/` is a pack of PNGs that
  replaces the dialogue/cutscene backdrops. The app writes the pack's image over
  the pixels the njpeg readback copies, split to match the game's four-chunk
  496x384 backdrop canvas, so one PNG fills the whole backdrop. The first
  mapping is scene `0x0D` step 2 (the New Game cathedral), with a plain
  `01.png` and a widescreen `01-wide.png`. With WIDESCREEN on, scene `0x0D`
  expands whether or not the pack is installed (its own 496x384 backdrop has
  content the 4:3 view crops), and a scene the pack covers is added to the RT64
  Expand set, so the backdrop fills the 16:9 window and reveals more of the
  canvas at the sides while the dialogue box and text keep their size.
  `OGRE_BG=0` runs without the pack. `tools/backgrounds.py` (and `make
  bg-extract`) extracts the game's own backdrop as the reference an artist
  draws over; the output lives in the gitignored `mods/backgrounds/reference/`.
  It is a reskin at the game's resolution, and not an HD texture replacement.
  See `docs/guides/hd-backgrounds.md` and
  `docs/HANDOFF-2026-09-29-session114.md`.
- **Performance** — five misclassified work loops in bank unit N carried a
  recompiler-injected blocking `yield_self`; their backward branches are listed
  in `config/banks/config-bankN.toml`'s `yield_work_loop_branches`
  (`0x801B3008`, `0x801DA2C0`, `0x801D0C08`, `0x8020A910`, `0x80204C48`;
  sessions 80, 90, 98, 112, 115). A slow screen is measured with `OGRE_PROFILE=1
  OGRE_DL_TRACE=1 OGRE_SCENE_LOG=1` for its display-list series and its `t4`
  `[prof]` line, but the profiler reports the most recently entered function and
  can name an entry-heavy function rather than the one holding the thread. Take a
  host stack sample (`sample <pid> 4 -file …`) during the slow state as well; that
  named `func_ovlN_80208E84` in session 112 and `func_ovlN_80204C08` in session
  115 after the profiler had named other addresses. `debug/menu-probe.sh` runs
  the app, the sampler, the console `snap` output and the coverage census
  together, so no capture time has to be agreed in advance.
- **Periodic hitch** — the runtime's message-queue snapshot
  (`ultramodern::debug_dump_queue_snapshot`) ran unconditionally every 90 VI
  frames on the Critical-priority VI thread, which measured as a 33 -> 59 ms
  frame every 1.5 s on macOS and is larger on Windows, where its
  `sleep_for(200us)` sampler resolves to SDL's 1 ms timer. It is now behind
  `OGRE_SNAP` and off by default (session 116). `OGRE_SNAP=1` restores the old
  ~1.5 s period for hang diagnosis; a number sets the period in VI frames.
  `tools/dlgaps.py <run.log>` measures the next such report: it prints the gap
  distribution and the phase histogram of the spikes, and `--check` fails when
  they are periodic. See `docs/HANDOFF-2026-10-01-session116.md`.
- **Debug console** — the live console's `snap [prefix] [ms]` writes the whole
  8 MiB RDRAM image with the game threads parked and, for `ms`, RT64's
  presented-frame capture for the same screen, so a player-reported defect can be
  read offline without a bounded run (`docs/guides/app-build.md`, session 101).
- **Windows** — v0.4.0's access violation on the first present is fixed, released
  and confirmed by a Windows run. The presented-frame capture was left enabled by
  `init_capture_env`'s `putenv` name-mangling (the UCRT copies the string), and
  RT64's readback handed the D3D12 backend a null texture. The capture path is
  now app-owned and the D3D12 copy skips sample positions for a buffer
  destination (session 107). The released Windows package of 2026-09-28 carries
  the fix: session 113 measured it opening its window on the developer's GeForce
  940MX / Intel HD 620 laptop with `d3d12.dll` and `D3D12Core.dll` loaded and no
  `vulkan-1.dll`, exit 0 and no new `error.log`. Players on v0.4.0 need the newer
  package. **Those two changes were not in any release**: the hosted runner
  builds the third-party trees from `patches/`, and both RT64 patches had drifted
  from them (session 117). The patches are regenerated and `make patch-check` now
  fails when a patch stops describing its tree. The captured `stdout`/`stderr`
  sections are empty in an Explorer launch because the process starts with NULL
  standard handles, and in that state the capture records nothing at all
  (session 119). A player's report therefore carries no log, and
  `ogrebattle64.exe > run.log 2>&1` from a shell discards the redirect as well.
- **Release parity** — a GitHub-hosted release builds three different sources: the
  public repository, the private generated-code bundle (`ogre-data/files.tar.gz`,
  which has a commit-drift check), and the `patches/` applied to pristine
  third-party checkouts, which had none. `tools/patchcheck.py` and
  `make patch-check` add it, and `tools/release-build.sh` runs it before
  packaging. Measured: v0.5.1 was built from `c7bd999` (session 112), so session
  115's slowdown fix and session 116's stutter fix are in no release, and the
  bundle holds 34 `yield_self` sites against the tree's 33 (session 117).
- **Debug behaviour in a release** — an audit with no environment variables set
  found the live console channel (a fixed path whose commands write guest RAM and
  load checkpoints), 116 `[rsp]` lines/s, per-display-list `[renderer]` lines, and
  trace hooks on every recompiled function entry and return. The console is now
  off unless `OGRE_LIVE_CONSOLE=1` (or the trigger's own variable) is set, so a
  stock run opens no path, reads no keyboard and prints nothing (session 117). The
  rest is open work below.
- **Browser VI presentation** — the frames of noise between the publisher logos
  and the title are fixed (session 206, developer report). At title entry the VI
  is blanked (`hStart = 0`) with its origin already on the title's buffer
  `0x000400`, and the old presenter paired that address with the last *visible*
  mode's 640x476 area. It resolved the address to the title's new 320x476 colour
  image and sampled rows 240..320, which the title never draws and which held
  RDRAM bytes converted as RGBA5551. The present now carries the visible flag and
  `fbSize` of the VI that produced it; a blanked or sizeless VI clears the canvas
  to black, which is RT64's `colorTarget == nullptr` present
  (`tools/RT64/src/hle/rt64_present_queue.cpp`), and the canvas is resized from
  the request's size. The 639x475 frames before the visible mode now read
  `nonBlack=0`, then the 319x239 title fades in; the proof pair is
  `docs/proofs/web-title-transition-noise-before-session206.png` and
  `-black-after-session206.png`. Open in the same pipeline: a depth-only present
  is not converted to colour, a presented image's CPU-written bytes are not
  re-uploaded, and a new colour image is still seeded from its whole RDRAM range.
  See `docs/HANDOFF-2026-10-31-session206.md`.
- **Browser renderer state (session 174)** — `G_TP` is decoded from OTHERMODE_H
  bit 19, the tile `ShiftScaleMap` is applied in the sampler, the chroma-key
  centre/scale reach the combiner's `KEY_CENTER`/`KEY_SCALE` inputs, and `G_FOG`
  replaces the vertex alpha. The permanent `[GFX-TP]` family census reports
  `changes=0` on every reached screen, the sampler's `shifted=` count is 0 on the
  tutorial, title and map, the mission's nonzero count changes no pixel (runtime
  A/B), and no reached screen selects a key input or sets `G_FOG`. The tutorial
  box, title, map and mission numbers are unchanged at their own capture phases.
  Publisher stills `0x0A` were not reached and the title/attract 0.11-0.20 match
  was not reproduced. See `docs/HANDOFF-2026-10-18-session174.md`.
- **Browser title parity** — the browser renderer now performs the native
  `OGRE_FOG` display-list repair, so the forced title frame matches the native
  fog-on render target at `[0.09,0.09,0.12]` (91.5% exact, `--inset 1`) and the
  `OGRE_FOG=0` control at `[0.03,0.03,0.03]` (97.3%), down from
  `[2.08,2.23,2.74]` (79.2%). The remaining fog-band difference is 0-3 levels of
  blend rounding. The publisher still is `[0.0,0.0,0.0]/100%` over 639x475, and
  neither the publisher nor the attract story fires the repair (session 162).
- **Browser texture sampler** — the browser samples the RDP's tiles with a port
  of RT64's `TextureSampler.hlsli` instead of the GL fixed-function sampler:
  tile-window clamp, `masks`/`maskt` wrap and mirror, point/bilerp/average from
  OTHERMODE_H, RT64's default three-point filter, and the 1/128 texel
  quantization of the interpolated coordinate. `OGRE_TEXF` selects `rt64`
  (default), `bilinear`, or `gl`. The `[GFX-SAMPLER]` census shows the forced
  title and world map are point-sampled (591 and 99 textured draws, 0 bilinear)
  and the tutorial `0x17` is the reached bilinear screen (6 of 8 draws, 6
  masked). The census also counts draws whose tile asks for a `G_TX` shift,
  which the sampler now applies (RT64's `ShiftScaleMap`, session 174): the
  forced title (1182 draws), the forced
  world map (198), the attract/intro run (8885, of which 2116 are
  `G_TF_AVERAGE`) and the tutorial's dialogue-box batch (447, 423 point and 24
  bilerp) all report `shifted=0`; the mission `0x03` is the screen that
  exercises the field (18 794-24 563 of 27 692-33 450 textured draws) and its
  frame is unchanged by the multiply (a runtime A/B, four of five interleaved
  pairs bit-identical). See `docs/HANDOFF-2026-10-17-session164.md`,
  `docs/HANDOFF-2026-10-18-session165.md` and `-session174.md`.
- **Browser TMEM and coverage** — the browser samples the RDP's TMEM instead of
  decoding each tile from RDRAM. A persistent 4 KiB `tmem_` receives every
  LOADTILE/LOADBLOCK/LOADTLUT (and YUV) with RT64's byte layout, and a draw
  decodes its tile at `tmem << 3` with `line << 3` over RT64's sample window
  (`min(tile rect, 1 << mask)`). The fragment shader also runs the RDP's alpha
  compare and `CVG_X_ALPHA` coverage, so a zero-alpha texel is discarded and the
  target alpha is the `CVG_DST` coverage value. The tutorial `0x17` dialogue
  box was the reached defect for both: its portrait, text, border and parchment
  now match RT64's target (`docs/proofs/web-tutorial-fixed-side-by-side.png`),
  where before the portrait was torn, the glyph cells were opaque black and the
  border was a bright green band. `boot.cjs` is `RENDERED`, `testdraw --flags
  96` reports `fbtex hits=1`, and the title frame matches the native at 1.56
  mean abs at its phase. The tutorial box's border rows and portrait are
  pixel-exact against the native target. Its remaining difference was the
  box-interior parchment, which session 166 measured: it is **one** draw pair
  (two triangles) over the whole interior (rows 170..222) of the 64x64 CI4
  image at `v` 0..51, **point-sampled** (`OTHERMODE_H=0x088C30`, `G_TF_POINT`),
  at about 0.98 texel per pixel, and it wraps by `maskt=6`. The sampler
  arithmetic, the tile state and the RSP viewport all agree with RT64's, and
  RT64 selected the texel **one row above** the browser's at the same pixel (24
  of 24 rows), i.e. its quad sits about one screen pixel lower. Session 167
  names the mechanism as RT64's half-pixel screen offset
  (`rt64_framebuffer_renderer.cpp:1528` `halfPixelOffset`, applied in
  `RasterVS.hlsl:25` as `screenOffset * ndcPos.w`) and applies it in
  `transform_vertex` for the triangle path only: the box goes from `5.88` to
  `0.22` mean over rows 166..219 against the native target, its parchment from
  `10.49` to `0.25`, the glyphs from `13.32`/`4.01` to `0.00` and the portrait
  from `1.10` to `0.00`, with the texrect border unchanged. The title frame does
  not move and the world map changes only on its triangle-drawn unit sprite.
  Session 165's "two texels per pixel and a flat `v=51` band below"
  is withdrawn: the lower rows look flat only because the paper texture has no
  contrast there. See
  `docs/HANDOFF-2026-10-18-session166.md` and
  `docs/HANDOFF-2026-10-18-session167.md`.

What each screen should show is `docs/scenes.md`. Build instructions and every
`OGRE_*` knob are `docs/guides/app-build.md`. A picture or a display list that a
run produces is evidence about the port until the five checks in
`docs/guides/emulator-first.md` pass.

## Open work

Ordered by how much each item blocks a player. Each item names the handoff that
holds the evidence.

0. **A map unit's head sprite drew as a flat ENV-coloured block. Fixed in two
   steps (sessions 134 and 136).** The head is the mission field's 14-part
   map-unit sprite and its 16x15 colour tile was served by RT64's framebuffer
   tile-copy path instead of being read from RDRAM. A framebuffer at masked
   `0x2E6840` whose `lastWriteFmt` is `G_IM_FMT_DEPTH` is still on record over the
   heap the game builds the head-down bitmap in (`func_8019ABC4`, `0x802FB0E8`),
   and `FramebufferManager::createTileCopyRecord` fills a tile copy from that
   framebuffer's `lastWriteRect` only. The head's tile (`468,65..484,67`) lies
   outside it, so the copy it sampled was never written: `TEXEL1` read zero and the
   combiner `(1 - TEXEL1) * ENV + TEXEL1` (`FC62FE03 2F0D73FD`, `G_CYC_2CYCLE`)
   painted the flat `ENV >> 3` = `0x2947` over the mask's correct silhouette
   (session 134). The same fault in a second cutscene, reached from `pre_ogre.bin`,
   showed the write-rect test is one instance of a larger condition: there the
   source record (`0x32AF40`, 320x240 RGBA16) is not a colour image of the frame at
   all (`SETCIMG` is `0x400`), its stale `lastWriteRect` `0,0..319,239` covers the
   tiles, and the addresses hold the game's CPU-built sprite scratch, so the
   guard served 23 copies and the head was the frame's own `G_SETENVCOLOR 0x000001`
   (session 136). A tile copy is now served only when the source framebuffer was
   written **in the workload being processed** (`Framebuffer::lastWriteWorkload`
   against the manager's per-workload `usedTimestamp`, `[fbcopy-nowrite]`), which
   keeps the copy for a framebuffer this frame rendered and refuses a leftover one.
   `FramebufferManager::checkRegionsTMEM` holds both conditions; the load then
   reads RDRAM the way hardware does. Verified in the first scene (the head box
   holds 1 pixel of `0x2947` against 76 before) and in the second (14
   `[fbcopy-nowrite]` refusals, 0 `[fbcopy]`, developer-confirmed), with the
   scene-14 regression frame matching the session-134 verified dump and its
   `y=120` scenery row unchanged. A blanket refusal of depth framebuffers as colour
   sources also fixed the first scene but changed one scenery scanline, because a
   one-row strip legitimately samples the same framebuffer's rendered rows; the
   write-rect test keeps that copy. Instruments that found it and stay:
   `OGRE_SPRITE_TRACE=1` (one line per small tile with the draw-call state,
   descriptor and resolved hash, printed after the upload decision; stops at 40000
   lines), `OGRE_TILE_DECODE=1` (the RGBA the renderer decodes out of the TMEM
   snapshot it hashes, plus the raw-TMEM path and the `[fbcopy]` lines), and
   `OGRE_TILE_TRACE_ALL=1` with `OGRE_TILE_TRACE_MAXH=64` (cap 5000). Residual: a
   freshness test against `fb->RAMHash` is unreliable on its own because
   `hashTracking` re-hashes every framebuffer at each frame end without touching
   the GPU copies, and the writers of the two leftover framebuffer records
   (`0x802E6840` and `0x32AF40`) are not identified; neither address is in its
   fault frame's image. See `docs/HANDOFF-2026-10-11-session134.md`,
   `docs/HANDOFF-2026-10-12-session136.md`, `docs/notes-black-sprite.md` §10-§11,
   and the session-133 handoff for the evidence that led here.

1. **Windows access violation when the game starts (`0xC0000005`). Closed, and
   verified on Windows (session 113).** Session 107 root-caused v0.4.0's fault at
   `ogrebattle64.exe+0x10001AE`: `plume::d3d12::D3D12CommandList::setSamplePositions`
   with a null `texture`, called from `copyTextureRegion` with a `PlacedFootprint`
   (buffer) destination, which is the port's own presented-frame readback. The
   readback ran because `init_capture_env` hid `OGRE_CAPTURE_PRESENT` by rewriting
   the first byte of a `putenv`'d name, which the UCRT's copying `_putenv` ignores.
   `snap` now toggles an app-owned path through `ogre_present_capture_path()`,
   the D3D12 path skips sample positions for a non-texture destination, and
   `tools/smoke-dist.sh` fails a package whose capture is on. The released Windows
   package of 2026-09-28 contains the fix and not the defect (the pre-fix
   `OGRE_CAPTURE_PRESENT=` string is absent, session 107's `capture path:` strings
   are present), and a bounded run on the laptop that produced the report
   initializes D3D12 (`d3d12.dll`, `D3D12Core.dll`, no `vulkan-1.dll`), opens its
   window, exits 0 and writes no `error.log`. Part (b), the failure on releases
   older than the capture code, no longer reproduces: RT64's
   `Falling back to Vulkan due to device workaround.` did not run, because the
   adapters report Intel `31.0.101.2140` and NVIDIA `32.0.15.8266` (582.66),
   both above session 94e's thresholds. The captured `stdout`/`stderr` sections
   are empty in an Explorer launch, and that is now root-caused: the process
   starts with NULL standard handles, and in that state the capture records
   nothing — a six-second boot puts 101 `stdout` and 53 `stderr` lines into the
   report with valid handles and 0 lines with NULL handles, so the device, the
   driver version and the fallback are still not visible in a player's report.
   The proposed fix is to give fds 1/2 a real destination before
   `crash_log::install`, and to stop the `AttachConsole` branch from freopening
   `CONOUT$` over an inherited redirection. See
   `docs/HANDOFF-2026-09-30-session119.md`,
   `docs/HANDOFF-2026-09-30-session113.md`,
   `docs/HANDOFF-2026-09-27-session107.md` and
   `docs/HANDOFF-2026-09-20-session94.md` part 5.
2. **SIGBUS after a lost battle, `func_800862C0+0x59A` at guest `0x80086328`.**
   The faulting instruction is identified and the writer is not. The instruction
   is `lw $s6, 4($a1)`, where `$a1` is `+0x7C` of an effect object; the value
   looks like a misread record field, not a pointer, and the crash lands on a
   scene/mode transition. The effect subsystem itself has no cross-bank call
   site, so the write that corrupts the record list comes from another resident
   function. See `docs/HANDOFF-2026-09-21-session96.md` §1.
3. **Title sprite green border, a regression.** A transparent sprite composites
   opaquely for 1-3 presents on a type's first appearance, and the sprite's colour
   texture border shows as exactly `(0,132,0)`. The mask and the combiner are
   byte-exact; the failing draw reaches the `M == PM_FRAMEBUFFER_COLOR` arm with
   `forceBlend` set and returns `finalAlpha = 1` instead of the combiner alpha
   (`rt64_blender.h`). The defect is present in every source state buildable on
   this machine, including RT64 at the 2026-09-17 commit `52f7160`, so no RT64
   change since then causes it and none fixes it. The next instrument is in the
   shader at the tagged draws: log `otherMode.forceBlend()`, `cycleType()`,
   `combinerCycles` and `blenderInputs()` and compare them with the CPU's values
   for the same `omL`. See `docs/HANDOFF-2026-09-21-session96.md` §2-§3.
4. **Credits screen (scene `0x11`) artifacts, and three latent leak-shaped
   `jal`s.** The screen plays; its backgrounds carry artifacts, and three `jal`s
   are emitted in the call-plus-early-`return` shape that leaks the frame-pump
   stack if their scene is reached. See
   `docs/HANDOFF-2026-09-19-session86.md` §5 and `-session85.md` §8.
5. **Teardown crash `func_8007F8E4+0x2F8` after the DMA census dump.** The
   window-close path dumps the DMA census and then faults. See
   `docs/HANDOFF-2026-09-19-session85.md` §4.
6. **Latent static backlog.** `tools/stubmap.py report` classifies the 986 static
   dispatch targets as 0 missing-function candidates, 282 bank-selection
   candidates, 369 inert, 332 resolvable and 3 IMEM (session 213; the old entry
   test reported 130 missing-function candidates, and every one is a body
   interior, switch case or data). The main unit also makes 949 direct calls into
   swappable RAM across 235 targets (`make cross-bank-check`), a known backlog
   that `cross_bank.py dispatch --only` fixes one target at a time; session 213
   dispatched `0x801AB740`/`0x801AB76C` (record 3, unit A), which removed the two
   call sites and one target. The 288 indirect
   `jalr` sites are outside any static measurement; a run's stub log is the only
   check for them.
7. **Controller Pak copy/backup (cartridge battery ↔ Controller Pak
   `Save`/`Load`/`Erase`).** The screen renders; the copy flow is deferred. The
   developer's decision is that it is low priority for a PC port. The device half
   (`osPfs*` and a flat 32 KiB `.mpk`) is implemented. See `docs/DECISIONS.md`
   (88b).
8. **Web build.** The wasm target compiles and runs (session 109, repaired again
   in session 138). Session 109's two defects were `app/src/bank_overlays.cpp`
   including the native-only `sdl_platform.hpp` for the live console, and
   `app/src/main_web.cpp` registering `recomp::SaveType::None` so the game's
   first SRAM DMA exited the runtime at about 350 ms (`librecomp/src/pi.cpp`).
   Session 138 repaired the same class a second time — `app/src/ram_key.cpp`
   included the same native-only header — and added `make web` so the target has
   a supported local build, plus `.github/workflows/wasm.yml` so CI compiles it.
   The title renders (`docs/proofs/web-title.png`), and session 138 added S2DEX2
   and YUV16 so the New Game njpeg backdrop decodes. Session 139 landed the last
   of the three named renderer gaps, **RDRAM framebuffer indirection**:
   `app/src/web_renderer.cpp` keeps a GL framebuffer object per `G_SETCIMG`
   target, `ogre_sync_framebuffers()` is a handshake that makes the browser main
   thread render the queue and write the target back to RDRAM in the RDP's
   RGBA5551 layout, and `update_screen()` presents the target the VI scans out.
   The assembled cathedral backdrop now appears
   (`docs/proofs/web-newgame-cathedral-assembled.png`); session 138's
   `web-newgame-cathedral-covered.png` is the state this replaces.
   Not verified in a browser: CI, and the `OGRE_RAM_KEY_AT_MS` path.
   Session 141 closed three of those items from the renderer side.
   (a) The render target's size and the canvas now come from the VI's scanout
   size, a port of RT64's `VI::fbSize` over the register words
   `ultramodern::renderer::get_vi_regs()` returns: the target's height is
   `max(viewport, VI height, scissor bottom + 1)` (RT64's
   `framebufferHeightForDisplay`) and keeps the tallest value while width and
   `siz` hold, so the world map's 640-wide colour images are `640x481` instead of
   `640x240`, and the canvas element is resized on a mode switch, so that mode
   presents 640x476 instead of being halved into a 320x240 drawing buffer.
   Only a *visible* mode updates the size (`status.type != BLANK &&
   hRegion.hStart > 0`, RT64's `VI::visible`): the blanked state OB64 puts the VI
   in across the switch carries the next mode's serrate bit with `hStart = 0`,
   which `fbSize` alone reads as `320x952`. The NDC denominator is the render
   target's size (colour-image width, else viewport, else VI), which is what the
   RDP's screen coordinates are, so the 640-wide targets no longer clamp at
   x=319. (b) The per-pixel `G_TEXRECT` S/T step was 32x too large
   (`dsdx / 32` where RT64's `drawRect` gives `dsdx * rectWidth / 1024`); this is
   the horizontal banding the title screen showed
   (`docs/proofs/web-title-640-before-uv-fix.png`), because its 320x240 backdrop
   is forty 640x12 texrects. (c) `K4`/`K5` are combiner colour inputs now
   (`convertK[4] / 255`, RT64 `RasterPS.hlsl`) instead of returning 0.
   Closed in session 162: the residual
   per-channel difference between the browser's canvas and RT64's render target,
   which session 146 reduced from 4.66/5.05/5.54 to 2.29/2.48/3.10 mean absolute
   per channel by fixing the present geometry (exact channel samples 39.4% ->
   81.4%; `tools/web_frame_cmp.py` is the comparator). Session 161 showed the
   remainder was not the two cloud bands but the native app's `OGRE_FOG`
   display-list repair, which the browser had not ported, and session 162 ported
   it: the title against RT64's target at the same instant is
   `meanAbs=[0.09,0.09,0.12]` (91.5% exact) with the repair on, against
   `[2.08,2.23,2.74]` (79.2%) before, and `[0.03,0.03,0.03]` (97.3%) in the
   repair-off control. Session 163 ported RT64's high-precision colour buffer
   (RGBA16F with `EXT_color_buffer_float`) and showed the remaining 1-3 levels
   are not blend precision: at the same instant the float and 8-bit attachments
   differ by 0.009 mean and the residual against the native stays 0.098 vs
   0.099. Against the native target's own 8-bit rounding the fog band is still
   0.241, localized to the translucent top edge of the repaired I8 fog layer.
   See `docs/HANDOFF-2026-10-17-session161.md`, `-session162.md` and
   `-session163.md`. Presentation now
   writes the visible area into the same fraction of the canvas from its
   top-left, the way RT64's `VIRenderer::getViewportAndScissor` does, and the GL
   scissor uses the RDP's half-open bounds. Session 145 closed the hue
   shift itself: the RGBA32 decode read the texel one byte along
   (`(G,B,A,R)` for `(R,G,B,A)`), so the browser and RT64 did not decode the same
   rectangle after all. Session 144's "the browser's decode of it is a plausible
   RGBA32 cloud, while the canvas colour at that row is a different one" compared
   the decode against the trace that the same decode computes. The comparison is
   `[GFX-TEXTRACE]` against `OGRE_TEX_TRACE`/`OGRE_RECT_STATE`, both of which
   stay, and the frame comparison uses `tools/rt64_target.py` on an
   `OGRE_CAPTURE_TARGET` `.bin`.
   Session 157 measured the cloud draw's blend path and found it correct: GL
   blending is enabled with the RDP's factors and `GL_FUNC_ADD`, the fragment
   alpha equals the decoded texel alpha, and the GL result equals
   `alpha*src + (1-alpha)*dst` on all 47 mid-alpha pixels of a probed row. The
   band residual is in the translucent layers of the composition (`0x1E2918`
   below the cloud, `0x249538` above it), not in the cloud draw. See
   `docs/HANDOFF-2026-10-17-session157.md`.
   Session 158 excluded the fog layer `0x249538` as well, and with it the RDP
   tile-window/mask candidate: its 22 five-row strips carry `masks=0 maskt=0
   cms=0 cmt=0`, sample `s` 0..200 texels over a 200-texel window, and the
   browser's sampler selects the same texel as RT64's
   `clampWrapMirrorSample` in 66 of 66 comparisons. The fog's draw output is the
   blend of its decoded texel, and the browser equals the native in 36 of 36
   sampled pixels of rows 200..231 where the fog is opaque. Every remaining
   differing row is a row whose visible layer is translucent (`0x217508`), and
   every opaque row is exact. See
   `docs/HANDOFF-2026-10-17-session158.md`.
   Session 142 closed the title-screen fault and the geometry behind it.
   `draw_texrect` treated the RDP's lower-right corner as inclusive (`xh + 1`,
   `yh + 1`, and a `xh - xl + 1` texel span). The RDP covers `[ul, lr)` for a
   texrect (`RDP::drawRect` takes the 1/4-pixel corners without snapping; the
   `lrx |= 3; lry |= 3` snap belongs to `RDP::fillRect`), so every texrect was
   one pixel wider and one row taller than hardware. The title screen's backdrop
   is forty 640x12 strips over a 640x480 colour image, and the strip that ends
   at row 480 (the last of the forty) therefore drew row 480, one row past the
   end of the image.
   `readback_framebuffer` copies every row a draw touched, so it wrote 1280 bytes
   at `image_end` - the next heap block's block header. The game's allocator
   `func_800712C4` then read image pixels as a block size and called
   `func_80071950`, whose body trapped (`memory access out of bounds`); session
   141's `divide by zero` in `func_801989AC` is the same corruption on a
   descriptor's width field. The texrect corner is exclusive now and the browser
   runs the publishers -> title (`0x04`) -> attract story (`0x0B`) with no page
   error (`docs/proofs/web-title-640-after-texrect-fix.png` replaces session
   141's banded before-frame). The browser build also got the per-frame
   diagnostics it never had: `ogre_gfx_flush()` calls `ogre::poll_scene()`
   (`OGRE_SCENE` forcing, `OGRE_SCENE_LOG`, `OGRE_SCENE_TRACE`) and
   `ogre::ram_key_tick()` (`OGRE_RAM_KEY_AT_MS`), and `debug/probes/rdram.cjs`
   reads the dump out of the wasm filesystem so browser RDRAM is analysed with
   `tools/rdram.py` like a native image. New bounded diagnostic `[GFX-EDGE]`.
   Session 143 closed session 142's "the attract story is only partly drawn": the
   presenter drew the whole framebuffer texture over the canvas, and that texture
   is the colour image at the tallest height `ensure_framebuffer` has been asked
   for (RT64's `Framebuffer::maxHeight`), so the title's 320x240 frame was drawn
   into the top 240/476 = 50.4% of the canvas with the next guest image's rows
   below it as noise or black. `visible_framebuffer_size()` ports RT64's
   `visibleFramebufferSize` (the VI's `fbSize` minus `OGRE_OVERSCAN`, default 1)
   and `blit_to_canvas(tex, u_max, v_min)` samples only that area, taking the
   high end of `v` because `upload_framebuffer` flips the image; `[GFX-PRESENT]`
   prints `visible=WxH`. The title's logo and the story's parchment page now fill
   the frame (`docs/proofs/web-title-640-after-texrect-fix.png`,
   `docs/proofs/web-attract-story-full-frame.png`), the 640x476 world-map mode is
   unchanged, and the frames were checked against RT64's own render target for
   the same screen.
   Session 145 fixed the RGBA32 channel order: `decode_texture_rect`'s
   `IM_SIZ_32b` case assigned the source bytes G, B, A, R to the destination
   R, G, B, A, so the title and the story drew their 320x240 RGBA32 backdrop
   (`img=0x80217508`, `fmt=0 siz=3`) with every channel one byte along; the
   trace's `mean`/`opaque` for its 3-row rectangles now equal the `(R,G,B,A)`
   decode of the same run's dump, and the canvas against RT64's
   `OGRE_CAPTURE_TARGET` of the same frame went from 40.7 to 5.1 mean absolute
   difference per channel. `tools/rt64_target.py` reads the target `.bin`.
   See `docs/HANDOFF-2026-10-14-session145.md`,
   `docs/HANDOFF-2026-10-14-session144.md`,
   `docs/HANDOFF-2026-10-14-session143.md`,
   `docs/HANDOFF-2026-10-14-session142.md`,
   `docs/HANDOFF-2026-10-14-session141.md`, `docs/HANDOFF-2026-10-14-session139.md`
   and `docs/DECISIONS.md`. Session 144 fixed the black CI tiles: a CI tile with
   texture LUT mode 0 is I8, not palette-indexed, so it samples intensity rather
   than an empty palette (`decode_texture_rect`'s `palette_enabled`). The title
   prologue's two unit sprites (`img=0x1C13F8 fmt=2 siz=1 line=9`,
   `oml=0x504240`, `lut=0`) were the black rectangle over the scene and now match
   RT64's silhouette (`docs/proofs/web-title-lut-mode0-i8-fix.png`). The TMEM
   palette also moved out of the per-display-list state, because it outlives a
   display list the way the colour image does.
   Session 146 fixed the window the visible area is drawn into (below), session
   147 fixed the palette decode (item (h) below), and session 148 fixed the
   canvas element's CSS box (item (i) below). The browser's own
   outstanding items were the `fldRegs` x/y scale and offset (RT64 uses
   them only in `VI::fbSize`, which the browser ports, and not in
   `VideoInterfacePS` or `cropRectangle`, so no browser change was outstanding
   there), the unwritten-edge and rounded-corner item (j, closed in session 160),
   the RDP fill scissor question (k, settled in session 150), and the title's
   cloud bands (closed in session 162 as the missing `OGRE_FOG` repair port).
   Framebuffer-as-texture is closed (item (m) below) and the `LOADTLUT`
   destination is closed as unexercised (item (o) below).
   **(h) The publisher still's ATLUS letters: closed in session 147.** The
   browser decoded every palette entry by the entry's own bit 15 (RGBA16 when
   set, IA16 when clear). The RDP selects the decode from the draw's 2-bit
   texture LUT mode (`OTHERMODE_H` bits 14-15), so a `G_TT_RGBA16` draw decodes
   every entry as 5/5/5/1; RT64's `TextureDecoder.hlsli` switches on that mode.
   The strokes are entry `0x3AE8`, bit 15 clear, so the browser painted
   `ia16(0x3AE8)` = `(58,58,58)` where the entry is `rgba16(0x3AE8)` =
   `(57,90,165)`. `decode_texture_rect` now takes the mode, mode 0 still reads a
   CI tile as I8 (session 144), the image cache key carries the whole mode, and
   the browser canvas's colour histogram equals RT64's capture entry for entry.
   Session 146's two readings are corrected here: the native stroke colour is
   `(57,90,165)` and not `(48,80,160)` (0 occurrences in `tt.140.bin`), and
   RT64's blue render is correct, so the fault was never in its TMEM palette
   lookup. `colour_mean`'s CI8 guard change from session 146 stays.
   **(i) The publisher still sits about one row low in the browser: closed in
   session 148.** It was the element's CSS box, and not the renderer. Session
   147 measured the canvas against RT64's `tt.140` target as
   `meanAbs=[8.26,9.53,15.92] meanSigned=[-0.09,-0.08,-0.07] exact=58.0%` with a
   shift minimum at `dy=-1`, and named the 476 -> 480 CSS stretch and the
   `fldRegs` x/y scale and offset as the candidates. `app/web/index.html` pins
   `#game-canvas` at `640x480`, `present_framebuffer` sizes only the drawing
   buffer (640x476 for that VI mode), and `image-rendering: pixelated` makes the
   browser's fit a nearest-neighbour resample: canvas row `Y` is target row
   `floor((Y + 0.5) * 476 / 480)`, which fits session 147's own capture at
   `[0.87, 0.84, 0.75]` with 99.6% of channel samples identical (`10.38` for the
   identity, `7.26` for the best constant shift). `resize_canvas` now calls
   `emscripten_set_element_css_size` so the CSS box is `max(1, min(2, 480 /
   height))` times the buffer. On the fixed build the still matches RT64's target
   at `[0.87, 0.84, 0.75]`, 99.6% exact, minimum at `(0, 0)`; the title is
   unchanged at `[2.30, 2.50, 3.12] / 81.3%`.
   **(j) The canvas element's rounded corners and the presenter's unwritten last
   column and row: closed in session 160.** `#game-canvas` carried
   `border-radius:6px` and `present_framebuffer` sized the drawing buffer to the
   VI's `fbSize` while `blit_to_canvas` wrote only the visible area (`fbSize`
   minus the overscan crop) into its top-left corner, so 639 of 640 columns and
   475 of 476 rows were written. Measured against RT64's `tt.140` target: the
   publisher still's last column (476/476 pixels) and last row (640/640) differed
   by a mean of 222.4 and 223.1 levels, the four corners showed the page
   background `(20,22,26)` against the target's `(239,231,206)`, and the
   `border-radius` arcs made 26 pixels of the visible area differ by up to 219
   levels. The drawing buffer is now the visible area (`resize_canvas(visible.
   width, visible.height)`, present fraction 1.0, so every pixel is written by
   the 1:1 blit), `#game-canvas` has no `border-radius`, and the RDRAM fallback
   stages its texture at the VI's pitch (`vi.width << siz >> 1`) rather than the
   canvas's. The publisher still measures `meanAbs=[0.0,0.0,0.0]`, 100.0% over
   639x475, with every corner and both edges equal to the target; the presented
   pixels of the same still are otherwise identical (`303499` of `303525`, the 26
   that changed are the corner arcs) so no frame moved; the title is unchanged
   (`[2.31,2.52,3.15] / 81.4%` over 319x239, pre- and post-change captures
   pixel-identical). See `docs/HANDOFF-2026-10-17-session160.md`.
   **(o) The LOADTLUT destination key: measured, unexercised, and reported.**
   The browser keys a palette upload by the load tile's `palette` field
   (`palette * 16`), where RT64 writes the table at `loadTile.tmem << 3` and
   samples it from the fixed upper-half base `RDP_TMEM_PALETTE` = `0x800` in
   bytes, CI4 at `0x800 + (palette << 7)` and CI8 at `0x800 + (index << 3)`
   (`tools/RT64/src/shaders/TextureDecoder.hlsli`). A tagged `probe160` printed
   every `LOADTLUT` destination and every CI draw's tile fields over 8 forced
   scenes (`0x04`, `0x05`, `0x07`, `0x0A`, `0x0B`, `0x0C`, `0x17`, `0x18`) plus
   the forced publisher still: the four distinct loads are all
   `tmem=0x100 palette=0` (16 entries/32 bytes at `0x1F16C0` and `0x1F1A90`, 256
   entries/512 bytes at `0x1F1A00` and `0x207CA8`), and all six CI draws are
   `palette=0` with `tmem=0x0` (their own texture address), `siz=0` against the
   16-entry tables and `siz=1` against the 256-entry ones, one of them
   `lut_mode=0` and reading no palette. `tmem=0x100` is the address the fixed base
   samples, and the browser's entry `palette*16 + i` is TMEM byte
   `0x800 + palette*128 + i*8`, so the two agree on every measured screen. The
   item closes as unexercised, and `OGRE_TLUT_TRACE=1` prints one `[GFX-TLUT]`
   line per distinct load destination, saying whether it is the assumed
   `tmem=0x100 palette=0`, so a screen that loads elsewhere reports itself.
   See `docs/HANDOFF-2026-10-17-session160.md`.
   **(k) The RDP's scissor clips a FILL-mode write: settled in session 150, and
   the native renderer fixed.** Session 149 fixed the
   browser's fill-colour decode (16-bit colours take the low 16 bits as RGBA5551,
   32-bit colours the whole word as RGBA8888, matching RT64's
   `ColorConverter::RGBA16/RGBA32::toRGBAF`) and found that the attract story's
   remaining difference is mostly RT64's: RT64 implements `G_FILLRECT` as a clear
   of the *unscissored* rect (`InstanceDrawCall::Type::FillRect` ->
   `clearColor`, with `clearRect.rect = convertFixedRect(call.callDesc.rect)`;
   the scissor only reaches `fbPair.drawColorRect`), so the story's fourth fill
   (`0x4AC14AC1`, scissored to rows 18..193) paints the whole 320x240 and RT64's
   target bands are `(74,90,0)` where the browser has `(0,0,0)`. n64brew's Set
   Scissor says the scissor clips every pixel-pipeline write, FILL included, which
   would make RT64 wrong and would also mean session 84's olive-band branch
   reading chased a symptom (the band rows are outside fill 4's scissor, so they
   hold fill 2's `0x0001` in either arm). Session 150 answered it with the
   reference emulator: ParaLLEl-RDP shows those bands as `(0,0,0)` (uniformly
   black, gradient rows from 18, the last column left stale), so hardware clips
   the fill; `RDP::fillRect` now intersects with the scissor and RT64's bands are
   `(0,0,0)` too, which the developer confirmed on screen. The publisher stills
   are byte-identical to their reference and the title is unchanged. The
   browser's half-open fill scissor is not a difference from the reference (the
   reference leaves that column unwritten as well); n64brew's FILL/COPY right-edge
   inclusivity note stays a possible 1-pixel nuance that no screen has shown.
   New kept diagnostic `debug/probes/fills.cjs` captures a frame with only the
   fills drawn. See `docs/HANDOFF-2026-10-16-session149.md`.
   **(l) The `[GFX-FBTEX]` row test's column over-report: closed in session
   154.** Session 152 recorded that the detector compared a load against the rows
   a draw covered and not the columns, so it could report a load that reads a
   drawn row but no drawn pixel. `app/src/web_renderer.cpp` now records the byte
   columns each draw covered and bounds the load's rect to its own columns; the
   `testdraw.cjs` negative control (debug flag bit 6) proved it, `fbtex hits=2
   lines=2` with the column comparison disabled and `hits=1 lines=1` with it. Its
   open item 4, that the row test over-reports the top row by one, is closed in
   session 155 (item (m) below).
   **(m) The `[GFX-FBTEX]` row inversion, and framebuffer-as-texture: closed in
   session 155.** `note_fb_write` computed
   `height - 1 - trunc((pos + 1) / 2 * height)`, which is `y - 1` where the float
   product lands on or above the integer and `y` where it lands below, so a fill
   of rows 100..163 was recorded as 99..163. The inverse is now
   `(1 - pos) / 2 * height` with the vertex treated as an edge (the lower-right
   corner is exclusive) and `std::lround`, so the recorded rows are the rows the
   draw covers. `test_draw`'s bit 6 gained a row control - a second colour image
   at `0x00C28000`, a fill of rows 100..163 and a load of rows 98..99 - and
   `testdraw.cjs --flags 96 --expect-hits 1` is the assertion: the old
   arithmetic prints `fbtex hits=2 lines=2` with `drawn rows=[99,163]` and
   `FAIL: expected fbtex hits=1, read 2`, the fixed build prints `hits=1
   lines=1` and exits 0. Framebuffer-as-texture itself is closed as **no screen
   needs it, with the sweep recorded**: the full 25-scene sweep
   (`fbtex.cjs --scenes 0x00..0x18 --secs 18`) reports
   `TOTAL framebuffer-as-texture loads: 0 over 25`, 21 of the scenes producing
   display lists (3..136 tasks; `0x02`/`0x03`/`0x0D`/`0x0E` produce one task and
   a page error; `0x0B` reached only the 3-task idle trajectory), the fixed build
   re-checks `0x04`/`0x0A`/`0x10` at `hits=0` (`tasks` 39/25/29), and a natural
   tap route through the opening reports `tasks=196 hits=0`. The detector stays
   so a screen that needs the feature reports itself. See
   `docs/HANDOFF-2026-10-17-session155.md`.
   **(n) The title's cloud bands: closed in session 162 as the browser's missing
   `OGRE_FOG` repair, not a blend defect.** 525 native `OGRE_CAPTURE_TARGET` frames
   of the forced title
   (`OGRE_SPEED=1`, `OGRE_CAPTURE_EVERY=1`) against 40 browser frames of the same
   forced scene: the minimum over the series is `meanAbs=[2.08,2.23,2.74]`,
   `meanSigned=[-2.03,-2.17,-2.67]`, 79.2% exact with the presenter's unwritten
   edge inset out (`[2.31,2.32,2.71] / 79.2%` without), and the per-row profile
   keeps a one-signed -8..-13 in rows 80..119 and 190..233. Session 156 left
   three candidates: GL blending not enabled for that draw, the sampled alpha
   not the decoded texel alpha, or the blend mapping wrong. Session 157
   measured all three and all are false. `GL_BLEND` is enabled for the
   `0x217508` draw with `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA` and
   `GL_FUNC_ADD`, `glGetError()` is 0; the fragment alpha of the shader equals
   the decoded texel alpha (159 at image (8,13), whose decode is
   `(190,175,157,159)`, and 0 at a fully transparent texel); and the GL blend
   result equals `alpha*frag + (1-alpha)*dst` on all 47 mid-alpha pixels of a
   probed row, worst deviation 0. The bands are the rows where no layer of the
   list is opaque: the list is one opaque black `G_FILLRECT`, then the sky
   `0x1C3EA8` (rows 0..89, 16..80), `0x1E2918` (24..129), `0x217508` (80..239)
   and `0x249538` (126..238), with rows 120..189 exact because `0x217508` is
   opaque there. Where the `0x217508` texel is transparent the two renderers
   agree exactly at rows 200/210/220 (meanSigned 0.0) and where it is opaque the
   browser writes the raw texel while the native is 12..44 levels brighter
   (row 200 x=200 browser (21,23,8) vs native (42,44,30)), so the difference is
   in the layers that are translucent in those rows, not in the probed draw.
   Next: point the same row readback at `0x249538` and `0x1E2918`, and check the
   tile-window masking the browser does not implement. Session 158 did both and
   excluded the tile-window candidate: on all 22 five-row texrect strips of the
   first title frame the `probe158` line reads `masks=0 maskt=0 cms=0 cmt=0`,
   the upload is `200x5`, both wrap modes are `GL_CLAMP_TO_EDGE`, and the
   browser's texel equals RT64's `clampWrapMirrorSample` texel in 66 of 66
   comparisons. The `s` range is 0..200 texels (`uv0=(0.000,0.000)`,
   `uv3=(200.000,5.000)`) over a window of `uls=0, lrs=796` = texels 0..199, so
   no sample is out of window; session 157's "s runs to 320" is corrected. The
   fog draw's own output is the blend of its decoded texel (`bef=(0,0,0)` ->
   `aft=(18,39,36)` at alpha 255, `bef == aft` at alpha 0), and the fresh browser
   frame equals the native capture in 36 of 36 sampled pixels of rows 200..231,
   including the pixels the fog writes opaquely. The fresh pair's profile puts
   every difference on a row whose visible layer is translucent: rows 120..189
   `0.03`, rows 80..119 `-7.75` across the whole width, rows 190..238 `0.05` at
   x < 140 (where the fog is opaque) and `-11.5` at x >= 140 (where the cloud
   shows). Next: one native run with `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1`
   to compare the native's decoded `mean`/`opaque` per `0x217508` strip with the
   browser's; equal numbers put the difference in the composition (the
   destination the partial-alpha cloud blends onto), different numbers put it in
   the image's content or alpha. Session 159 ran it and closed that candidate.
   `colour_mean` does not decode RGBA32, so a tagged probe printed the tile
   window of an RGBA32 draw on the native side and the same window's alpha
   statistics on the browser side: 444 of 444 draw windows agree and none differ
   (`|dmean| < 0.15`, equal `opaque`), including all 54 windows of `0x217508`
   over 53 native frames, whose content is constant. So the RGBA32 bytes and the
   alpha are the same in both runs and the survivor is the composition. The
   residual is zero in rows 186..193 and 234..238, and +18.9 mean over the 5116
   differing pixels of rows 195..232 (native brighter) from a slanted left edge;
   it is neither a translation nor an alpha scale on the composite. The two
   runs' lists also differ in structure (browser 135 draws with full-width
   `0x217508` strips, native 590 with seven 72-pixel columns of 1-pixel strips),
   which session 156's 0.46-3.01-level phase bound keeps out of the residual, so
   the next instrument is a per-draw readback of one target row inside one
   renderer. See
   `docs/HANDOFF-2026-10-17-session156.md`,
   `docs/HANDOFF-2026-10-17-session157.md`,
   `docs/HANDOFF-2026-10-17-session158.md` and
   `docs/HANDOFF-2026-10-17-session159.md`.
   **The native's saved game speed is x4 and the browser's is x1**; every native
   capture needs `OGRE_SPEED=1` to be comparable with the browser.
   See `docs/HANDOFF-2026-10-16-session148.md` and
   `docs/HANDOFF-2026-10-16-session146.md`.
   Session 140 added the static package and the deployment path: `make web-dist`
   writes `dist-web/` (page, shell, Emscripten output, `coi-serviceworker.js`,
   `.nojekyll`), `.github/workflows/pages.yml` uploads it as the Pages artifact
   and deploys it, and `app/web/ogre-web-config.js` lets one page serve both the
   repository layout (`../../build-wasm/`) and the flat package (`"./"`). The
   service worker is what makes a header-less host work: it adds COOP/COEP and
   reloads the page once. Measured on a plain `python3 -m http.server` serving
   `dist-web` (no COOP/COEP): the page is cross-origin isolated at 811 ms and the
   glue reaches `WASM runtime initialized`; `boot.cjs` reports `RENDERED`
   (`nonBlack 239425`, `sawRealDl=true`) against the same numbers for the same
   build served by `debug/server.py` (`nonBlack 239425 / colorful 19688`, the
   server's own COOP/COEP headers and no worker); a GitHub-Pages-like
   subpath and a gzip-encoded server both isolate and instantiate the wasm; and
   the deployed site (`https://lfarroco.github.io/ogre-battle-64-recomp/`, runs
   `38576916888`/`38576918066`) isolates at 1131 ms and `boot.cjs` renders the
   same frame there as on the two local servers (`nonBlack 239425` on all
   three). Session 140 also found that the
   glue publishes `Module._ogre_*` after `onRuntimeInitialized` fires (measured
   509 ms on the local package and 2015 ms on the CI-built one), which made a ROM
   handed to the page inside that window fail with `TypeError: func is not a
   function`; `app/web/web.js` now waits with `whenWasmExportsReady()`.
   Open: the deployed site has only been exercised headless (no hand-played
   pass), CI and this machine use different Emscripten versions, and the first
   load downloads the whole 21 MB wasm before the visitor has picked a ROM. See
   `docs/HANDOFF-2026-10-14-session140.md` and `docs/WEB-PORT-DEPLOYMENT.md`.
   **(p) The title fog repair in the browser: closed in session 162.** The
   browser now performs the native `OGRE_FOG` repair as a pre-pass over each
   submitted display list (`web_renderer.cpp` `FogWalker`/`fog_repair_group`,
   called from `execute_dl` before `gbi::walk_dl`), with the same white-combiner
   test (`0xFFFF73B9`), zero-area-window test, layer-table reads at `D_801B80E0`
   (`+0xFC + i*0x24`), image-record field order and `SETTIMG`/`SETTILE t7`/
   `SETTILE t0`/`LOADTILE`/`SETTILESIZE t0` rewrite as
   `app/src/renderer.cpp:1003-1219`, plus the same layer advance (0, 2, 3, ...)
   and the negative-coordinate fallback. It is a browser-side port only;
   `renderer.cpp` is unchanged. `OGRE_FOG=0` disables it (default on), read from
   the page query string by the existing `ENV` publish in `app/web/web.js`.
   Measured: browser title vs native fog-on `t.177` is
   `[0.09,0.09,0.12]` 91.5% exact (rows 0.014/0.273/0.060) against
   `[2.08,2.23,2.74]`/79.2% before; vs native fog-off `t.279` it is
   `[0.03,0.03,0.03]` 97.3%; the cross-controls are 2.740 and 2.733. Session 163
   retested the remaining band with RT64's high-precision colour attachment and
   found it is not blend precision: the RGBA16F and RGBA8 canvases differ by
   0.0091 mean (0.906% of samples, one level) while both stay ~0.098 against
   `t.177`, and against the native target's own 8-bit rounding the band is still
   0.241. The difference is one wave in rows 81..119, the translucent top edge
   of the repaired fog layer, whose image is a 64x64 I8 image
   (`[GFX-FOG-SUMMARY]`: `siz=1 fmt=4`, layer 2, `0x801D51E8`). Session 164
   corrected the "compare the sampled intensity" next step: `[GFX-FOG-DRAW]`
   shows the repaired groups are point-sampled (`filt=0`, `mask=(64,64)`,
   `cm=(0,0)`) 320x1 strips and 64x64 1:1 blocks, so the sampled texel is exact
   in both renderers and a filter difference is excluded; the remaining
   candidate is the composition under the fog's partial alpha. See (q).
   Publisher `[0.0,0.0,0.0]/100%`, `boot.cjs` `RENDERED`, and the story's fills
   are unchanged; none of those three runs fires the repair. See
   `docs/HANDOFF-2026-10-17-session162.md` and `-session163.md`.
   **(q) The RDP texture sampler: ported from RT64 in session 164.** The browser
   sampled with the GL fixed-function sampler, whose filtering was a property of
   the uploaded texture (the `G_TF_*` of whichever draw caused the decode), which
   had no `masks`/`maskt` wrap or mirror beyond the whole texture repeating, no
   `G_TF_AVERAGE`, and no hardware coordinate quantization. `web_renderer.cpp`
   now runs RT64's `TextureSampler.hlsli` in the fragment shader:
   `clampWrapMirrorSample` (clamp to the tile window, modulo the mask, mirror on
   `G_TX_MIRROR`), `sampleTextureLevel`'s point/bilerp/average selection, the
   three-point filter RT64's default `threePointFiltering` uses, and
   `round(uv * 128) / 128`. The filter is now per draw from OTHERMODE_H.
   `OGRE_TEXF=rt64` (default), `1`/`bilinear`, `gl` (the old path) select it, and
   `[GFX-SAMPLER]` counts textured draws by filter and mask. Measured: title 591
   textured draws (point 591, masked 456), world map 99 (point 99, masked 1),
   publisher 56 (point 56, masked 0); `[GFX-FOG-DRAW]` shows the repaired fog
   draws are `filt=0` point-sampled, so the fog band is not a filtering
   difference; the tutorial `0x17` is the reached bilinear screen (6 of 8 draws
   bilerp, 6 masked) and its 64x64 tiling background is drawn 447x335 texels over
   320x240 with `masks=6`. No regression on the point-sampled screens
   (`boot.cjs` `RENDERED`, `testdraw --flags 96` `fbtex hits=1`, title series
   matching the native at `0.134` mean / band `0.070`). See
   `docs/HANDOFF-2026-10-17-session164.md`.
   **(r) The browser's tutorial `0x17` dialogue box: closed in session 167.**
   The forced tutorial drew a torn portrait, opaque black glyph cells and a
   bright green border against RT64's clean Deneb portrait and readable text.
   Both remaining faults were one mechanism: the renderer decoded each tile
   directly from RDRAM with the `SETTIMG` width as the row stride, while the RDP
   samples TMEM (a load's source stride and the tile's `line` are independent),
   and its fragment shader had no alpha-compare/coverage path, so the
   `CVG_X_ALPHA` draws painted their zero-alpha palette entries. `web_renderer.cpp`
   now keeps a persistent 4 KiB TMEM, copies every load into it with RT64's
   layout, decodes a tile at `tmem << 3` over RT64's sample window, and runs the
   RDP's alpha compare and coverage discard. The strip tiles decode 16x8 from
   `img=0x24E340` (`line=1 masks=4 maskt=3`) and the glyphs 16x16 CI4, with the
   palette at TMEM `0x800`. The remaining difference was the box-interior
   parchment, measured in session 166: one draw pair over the whole interior,
   **point-sampled**, at about 0.98 texel per pixel, whose sampled texel row was
   one less in RT64 than in the browser at the same pixel (24 of 24 rows).
   Session 166 recorded that as "mechanism unnamed"; session 167 names it:
   **RT64's half-pixel screen offset**. `rt64_framebuffer_renderer.cpp:1528`
   builds `halfPixelOffset = {1/width, -1/height}`, assigns it as
   `triangles.screenOffset` (lines 1571-1572), and `RasterVS.hlsl:25` applies it
   as `ndcPos.xy = ndcPos.xy * screenScale + screenOffset * ndcPos.w`, i.e. half
   a pixel right and half a pixel down for every non-rect vertex; the browser's
   `transform_vertex` did not apply it. The two-line change (`sx += 1/W`,
   `sy -= 1/H`) brings the box to `0.22` mean over rows 166..219 against the
   session-166 native target, from `5.88`: parchment rows 172..195 `10.49 ->
   0.25`, lower rows 196..218 `2.01 -> 0.17`, portrait `1.10 -> 0.00`, the name
   line `13.32 -> 0.00` and the text body `4.01 -> 0.00`, with the border rows
   (texrects) unchanged at `0.33 -> 0.29`. The texel-row test is 24/24 at columns
   100, 150, 200 and 285 after the change (3/24 at column 285 before). The title
   is unchanged (`0.20 -> 0.18` best native match at its scroll phase) and the
   world map's frame moves only on its triangle-drawn unit sprite. Session 165's
   "two texels per pixel, flat `v=51` band below" is withdrawn (the lower rows
   lack contrast, which is why they matched). The browser also interpolates
   attributes perspective-correctly (vertex clip `w` through the rasterizer, as
   RT64's `RSPProcessCS`/`RasterVS` do) and carries RT64's `G_TP_NONE` halving;
   the box's quad has constant `w`, so those changes leave it unchanged. The old proofs `web-tutorial-browser-vs-native.png` and
   `web-tutorial-sampler-ab.png` show the defect this replaces; the pair after,
   during and against the native is
   `docs/proofs/web-tutorial-half-pixel-before-after.png`. See
   `docs/HANDOFF-2026-10-18-session165.md`,
   `docs/HANDOFF-2026-10-18-session166.md` and
   `docs/HANDOFF-2026-10-18-session167.md`.
   **(s) The browser reaches mission scene `0x03` from a battery save and then
   traps before its first frame (session 168).** `?OGRE_SAVE=<url>` imports a
   32 KiB battery to `/ogre/saves/ogrebattle64-us-rev1.bin` after the IDBFS load
   and before `ogre_start_boot()`, and `?OGRE_TAP_SCENE_BUTTON=...` is the
   native scene-keyed tap schedule keyed on the page's own `[scene]` lines. With
   `assets/escort_suspend.bin` the browser logs `[save] loaded
   /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)`, drives the forced title
   -> Load Game -> `[scene] t=85121ms id=0x0003`, streams five of the six
   overlay banks the native streams, and then the worker reports `Uncaught
   RuntimeError: memory access out of bounds` without submitting a mission
   display list. The canvas keeps the Load Game frame. **Session 169 names the
   trap and gets a mission frame.** The trap is a guest `MEM_W` store in
   `func_80071950` (the message-queue node unlink), reached from the scene-`0x03`
   enter `func_80173724` -> `func_801792DC` (flush pending queue message) ->
   `func_800712C4` (queue send). The flush reads the queue-pool global
   `0x8018FC88` (`G = 0x80243E10`, set by the queue init `func_8017909C`), and in
   the browser the words at `G-0x20`/`G-8` hold `0xFFFDFFFD` - two 16-bit
   `0xFFFD` = RGBA5551 (31,31,31,1) opaque white, i.e. pixel data written into the
   game's heap block - where the native run has valid list pointers and `0`. The
   flush then unlinks a node whose pointer is the white pattern and stores
   through it, out of bounds. The bank link is not involved: all 22
   `func_ovlP_*` symbols are in `build-wasm/ogrebattle64.wasm` and
   `libBankPFuncs.a` is in the wasm link line, and the sixth record is requested
   only after an A press in scene `0x03` (native log), not on the enter path.
   The writer of the white data is still unnamed; the native watchpoint on
   `0x80243E10` shows only `func_8017909C`. When the clobber happens to land
   eight bytes higher the enter survives and the browser **renders the mission**:
   `docs/proofs/web-mission-first-frame.png` is that frame beside the native
   `t.800` (meanAbs R=23.54 G=14.96 B=7.86, exact=0.3094, against session 168's
   two-different-screens 81.14/73.78/54.39 and 0.0406). Lighting, depth and
   culling are still unimplemented, so the browser's terrain is unshaded.
   **Session 170 names the writer and fixes it.** `D_8018FC88` is the sprite
   part cache (session 132's layout), not a message queue: `func_8017909C`
   allocates it, `func_801792DC` releases it (`D_8018FC88 = free(...)`),
   `func_800712C4` is the game allocator's free/coalesce path and
   `func_80071950` is a tree-node unlink. The words that trap are the **block
   header** at `G-0x20`: `+0x18` is the size the free tests against `0x21` and
   `+0x08`/`+0x0C`/`+0x10` are the links it stores through. The writer is the
   browser renderer's framebuffer writeback, `readback_framebuffer` /
   `readback_all_framebuffers` in `app/src/web_renderer.cpp`: scene `0x12`
   renders a 320x240 colour image at guest `0x80228080`
   (`0x228080..0x24D880`), the cache block at `0x80243E10` lies inside it, and
   `ogre_gfx_flush` wrote **every** framebuffer back on **every present**, so the
   image's 16-bit pixels landed on the live cache header (`probe170` logged
   `writeback fb=0x228080 covers 0x243DF0 ... rows(wy)=[0,239]`, and the guest
   dump showed 22 891 `0xFFFD` halfwords in `G ±0x8000`; native has none). The
   fix writes framebuffers back only for the game's own
   `ogre_sync_framebuffers` request, which is where RT64 does it
   (`State::syncFramebuffers`) and the only point at which the guest is blocked
   on the renderer; the route then reaches `0x03` and renders
   (`nonBlack=303148`, `colorful=288300`) with `G-4 = 0x353212FF`, the native
   value. See `docs/HANDOFF-2026-10-18-session170.md`.
   **Session 171 lands the two biggest renderer features this scene uses:
   `G_LIGHTING` (916 448 of 989 100 triangle draws) and backface culling
   (926 539).** `app/src/web_renderer.cpp` keeps RT64's lights
   (`std::array<Light, RSP_MAX_LIGHTS + 1>`, `hle/rt64_rsp.h:163`): `G_MOVEMEM`
   index 10 (`MV_LIGHT`, slot `offset/24`, slots >= 2) writes one light,
   `G_MOVEWORD` `G_MW_NUMLIGHT`/`G_MW_LIGHTCOL`/`G_MW_FOG` set the count, a
   colour and the fog vector, and the 24 raw bytes are read with the runtime's
   `n64b` (`off ^ 3`). `transform_vertex` then replaces the vertex colour with
   RT64's result (`RSPProcessCS.hlsl:88-108`): the Vtx colour bytes are the
   normal over 127, `ambient = light[num_lights]`, and each of the
   `num_lights` lights adds `max(dot(norm, normalize(dir * worldMat)), 0) * col`
   with `worldMat = st.modelview_stack.back()` (`hle/rt64_rsp.cpp:538`, the
   G_MTX MODELVIEW matrix, `extended.viewProjMatrix` identity), then
   `rgb = min(result, 1)`. The light layout is measured from the game's own
   bytes (`raw 4D 4D 7F 00 4D 4D 7F 00 FD 99 C5 00`, pads at 3/7/11), so the
   logical reads are colour 0/1/2 and direction 8/9/10. Point lights are
   unreachable (RT64's `pointLighting` flag is false for every GBI,
   `gbi/rt64_gbi.h:39`) and are not implemented. Culling mirrors
   `RSP::drawIndexedTri` (`hle/rt64_rsp.cpp:1059/1094/1132`): both bits draw
   nothing, `G_CULL_FRONT` alone swaps the first and last index, then
   `N = cross(V, U)` from screen-space positions (NDC `(x, -y)`) must have
   `N.z >= 0`. F3DEX2's cull bits are `0x200`/`0x400`. A permanent
   `[GFX-LIGHT]` census prints every 400 lit draws. **Measured** against native
   `/tmp/w168/nat2/t.800`: `meanAbs 23.72/15.30/8.11` (`exact 0.3090`) ->
   `5.28/5.05/3.73` (`exact 0.4372`), `meanSigned +0.91/+1.16/+0.84`; terrain
   regions top cliff rows 0..38 `18.92 -> 0.93`, right terrain `17.94 -> 2.26`,
   bottom middle `17.18 -> 5.24`, left grass `18.10 -> 7.87`, whole frame
   `15.71 -> 4.69`. The two frames are different intro phases (portrait box
   position, camera distance, `MISSION` band). No regression: the tutorial
   `0x17` frame is byte-identical to session 167's (`md5
   10b3536365b1bcfb6d2e2116aaf42a24`, box 0.22), the title's best native match
   is 0.11 (no triangle draws), and the world map differs from session 167's
   frame only by its fade (gain 1.932, residual 0.99). Depth (516 463 draws)
   and per-vertex fog alpha are the remaining features of this scene. See
   `docs/HANDOFF-2026-10-18-session171.md`.
   **Session 172 lands the RDP depth pipeline's state, target, transform and a
   synthetic assertion, and ships the depth test off.** `G_SETZIMG` (2 360
   commands of scene `0x0003`) and `G_SETPRIMDEPTH` (11 740) now decode into
   `RenderState` (`zimg_*` with address `w1 & 0xFFFFFF`, fmt 21-23, siz 19-20,
   width bits 0-11 + 1; `prim_depth = (z & 0x7FFF)/32767`), `DrawCmd` carries
   `depth_test = zCmp && !copy && zMode != ZMODE_DEC` and
   `depth_write = zUpd && !copy` plus `z_source_prim`, `prim_depth` and per-vertex
   NDC z, each `GlFramebuffer` owns a `GL_DEPTH_COMPONENT24` renderbuffer cleared
   to 1.0, and the fragment shader runs RT64's F3D depth clip
   (`1022.0/1024.0`, RasterPS.hlsl:63-104). The vertex depth is RT64's screen z:
   `pos.z/w * vscale[2]/1024 + vtrans[2]/1024` (`hle/rt64_rsp.cpp:23/714/908`,
   so the axis is `/1024`, and the mission's `Vp_t [640 480 511 0]` puts the 511
   at index 2, not RT64's struct index 3), and the shader gets `2*z - 1` so
   `gl_FragCoord.z` is that 0..1 value. A permanent `[GFX-DEPTH]` census prints
   the zCmp/zUpd/prim/DEC counts, the `G_SETZIMG` configurations, the draw's
   window-z range and the viewport z transform. **The synthetic control is the
   correctness proof**: `test_draw` debug flag bit 7 draws the near triangle first
   (red) and the far one second (blue) with `Z_CMP|Z_UPD`, and the renderer reads
   the target's pixel back - `testdraw.cjs --flags 128 --expect-depth near` reads
   red, `--flags 384 --expect-depth far` (Z_CMP cleared) reads blue, both exit 0.
   On the mission, `[GFX-DEPTH]` reads `zcmp=zupd=20307` of 44 800 draws (45%,
   the `0x00552078` group), `prim=0`, `dec=0`, `zimg_seen=2` (`0x228080`,
   `0x3350C0`, both `fmt=0 siz=0 width=1` - RT64's `setDepthImage` takes only the
   address). Session 172 measured the test as a frame regression (7.25/6.63/6.98,
   `exact 0.3614`, top cliff 0.93 -> 9.52) and shipped it off; **session 173 shows
   that regression was a capture-phase difference, not a depth effect, and the
   test now ships on** (open item 8(u)). Depth off measured 4.77/4.54/3.19,
   `exact 0.4387`, cliff 0.93 at that round's phase. `ZMODE_DEC` is reported, not
   approximated. See `docs/HANDOFF-2026-10-18-session172.md` and
   `-session173.md`.
   **(u) The browser's depth target was keyed by the colour image, not by the
   depth image; fixed, and the mission's depth test is not what the top band
   showed.** Session 173 found the ordering problem in the target, not in the
   comparison: every `GlFramebuffer` (one per `G_SETCIMG` address) owned its own
   depth renderbuffer, while RT64 keys its depth `RenderTarget` by `G_SETZIMG`'s
   address/width/siz independently of the colour target
   (`hle/rt64_state.cpp:1354-1365`, `:1389-1401`,
   `hle/rt64_framebuffer_manager.cpp:171`). The mission renders one depth image
   (`0x3350C0`) from four colour images (`0x3350C0`, `0x025C00`, `0x04B400`,
   `0x000400`; the new `[GFX-DEPTH] pair` census reads `pairs=11`), so the colour
   images after the first drew against a cleared buffer. The depth renderbuffers
   now live in a map keyed by the depth image, the flush attaches the matching one
   to the colour FBO and clears it once, and the contents persist across colour
   switches as RT64's do. **The measured result is that the depth test changes no
   pixel of the mission's gameplay frames**: with flag bit 9 toggling it inside one
   run at the same intro phase, four of five on/off pairs are bit-identical
   (whole frame and rows 0..38, 200..238, 40..180 all `0.000`). So session 172's
   `0.93 -> 9.52` was a phase difference (the captured band contains the note
   box, the portrait and the scrolling MISSION text), not a depth ordering
   failure, and that item is withdrawn as a depth finding. The depth test now
   ships **on** by default (`OGRE_DEPTH=0` disables it); the synthetic control
   still passes (`--flags 128 --expect-depth near`, `--flags 384 --expect-depth
   far`), and a one-shot `[GFX-DEPTH] first depth-tested draw` line reports the
   attachment (`status=0x8CD5` complete). Not re-measured this round: tutorial
   `0x17`, title `0x04`, map `0x05`. See
   `docs/HANDOFF-2026-10-18-session173.md`.
   **(t) The browser mission frame is between two intro phases.** The scene
   `0x03` frames compared this round are the same screen at two moments (the
   browser's camera and the "Castle Burgund" portrait box sit differently from
   native `t.800`'s), so a whole-frame number is not a like-for-like measure.
   A frame-locked pair needs a deterministic tick: a save state entered at the
   same step in both renderers, or an `OGRE_CAPTURE_TARGET` series on the native
   side matched to the browser frame's phase, as the title comparison does
   against `/tmp/w162/nat-fog/t.*.bin`.
   **(v) The browser renderer takes `G_TP` from OTHERMODE_H bit 19, applies the
   tile `ShiftScaleMap`, feeds the chroma-key combiner inputs and applies
   per-vertex fog alpha (session 174).** `record_state` read bit 10, which is
   not an RDP field; RT64's `G_MDSFT_TEXTPERSP` is bit 19
   (`tools/RT64/src/shared/rt64_f3d_defines.h:51`, consumed at
   `src/shaders/TextureSampler.hlsli:221-226`). The new `[GFX-TP]` census prints
   one line per (kind, OTHERMODE_H) family with the old and new reading and
   `changes=0` on every family of the tutorial, title, map, mission and attract
   story: every observed triangle family has both bits set and the rect families
   have bit 19 clear while `kind != 0` keeps their correction suppressed.
   `ShiftScaleMap` (`hle/rt64_state.cpp:33-51`, `:306-307`,
   `TextureSampler.hlsli:226`) is now a uniform pair multiplied into the sampled
   coordinate before the 1/128 rounding; it is 0 on the tutorial, title and map,
   and the mission's ~19-25 k shifted draws do not change its frame (runtime
   A/B). `OP_SETKEYR`/`OP_SETKEYGB` decode RT64's fields
   (`hle/rt64_rdp.cpp:1223-1236`, `gbi/rt64_gbi_rdp.cpp:154-169`) and the
   combiner's `C_KEY_CENTER`/`C_KEY_SCALE` inputs return them
   (`RasterPS.hlsl:185-186`, `shared/rt64_color_combiner.h:482-485`); a one-shot
   `[GFX-KEY]` reports the first draw that reads one and no reached screen did.
   `G_FOG` now replaces the vertex alpha with
   `clamp((max(tfPos.z,0)/tfPos.w * mul + offset)/255, 0, 1)`
   (`RSPProcessCS.hlsl:70-80`), with a one-shot `[GFX-FOG] per-vertex alpha`
   note; no reached screen sets `G_FOG`. Measured this round: tutorial box
   0.25/0.00/0.18 by band against native `t.335` (frame rows 0.29), map
   `nonBlack=252820` with fade gain 1.927 and residual 0.84, mission
   `meanAbs 7.07-7.58` with `exact 0.3489-0.3633` against `t.800` at its own
   intro phase. Not verified: the publisher stills `0x0A` (the forced entry
   never advanced) and the title/attract 0.11-0.20 match (the browser's forced
   title lands on the menu, which the native series does not contain). See
   `docs/HANDOFF-2026-10-18-session174.md`.
   **(w) The close-out round: smoke on a quiet machine, a fresh title pair, the
   S2DEX2 opcode census, and the `G_CD` decision (session 175).** The smoke
   battery passes on a quiet machine — `boot.cjs --attempts 3 --secs 40`
   `RENDERED` on attempt 1 (`nonBlack=24452 colorful=10140`), `testdraw --flags
   96` `fbtex hits=1 lines=1`, `--flags 128` reads the near (red) triangle and
   `--flags 384` the far (blue) one — so session 174's `NO RENDER` was
   environmental. The title comparison is re-made with a **fresh native series of
   the same forced scene** (`/tmp/w175/nat-title60`, 80 targets): the browser's
   attract **story** frame matches `t.640` at `[0.24,0.25,0.28]` (85.1% exact)
   and its **logo** frame matches `t.1080` at `[1.88,1.96,2.23]` (72.1%), with
   the clouds exact (`0.07`/`0.08`) and the difference on the logo (`5.32`) and
   copyright (`8.49`) bands, at a `(0,0)` shift minimum. So `/tmp/w162/nat-fog`
   was the wrong series (attract backdrop and story overlay, no logo) and the
   forced title is not deterministic in the browser (three runs, two beats). The
   new permanent `[GFX-S2D-CENSUS]` counts every S2DEX2 opcode byte and prints
   `undecoded=`; **no reached screen submits an S2DEX2 list**, and the forced
   `new-game` route never advanced (`tasks=1`), so the census is compiled and
   unexercised and the `0xDA`/`0xDC`-only claim still rests on session 51's
   trace. `G_CD` is decided not to be implemented: the game enables it
   (`G_CD_MAGICSQ` on title/map/mission/story, `G_CD_BAYER` on the tutorial), but
   RT64 dithers only in its native-format writeback and the presented canvas
   samples the GL colour image, and no reached screen reads a framebuffer back
   (`[GFX-FBREAD]` absent), so it is a readback-path difference. The publisher
   `0x0A` forced entry now advances 30 display lists (`texrect=136 fillrect=96`)
   but presents nothing (black canvas or the unpresented canvas), so the still is
   still unverified. Mission `0x03` on the final build: `nonBlack=303208`,
   `colorful=294108`, whole frame `7.88/7.18/6.91` (`exact=0.3481`), terrain
   `10.30/7.57/4.62/9.48`, with the same intro-phase caveat. See
   `docs/HANDOFF-2026-10-18-session175.md`.
   **(x) Session 175's `G_CD` decision is revised: the writeback dither is
   implemented (session 189).** RT64 applies `G_CD` only in its native-format
   writeback, and the browser writes back for the game's CPU readback (the njpeg
   assembly) and for `ogre_sync_framebuffers`. The browser now counts each
   colour image's `OTHERMODE_H` bits 6-7, picks the most-used pattern the way
   `Framebuffer::bestDitherPattern` does, and adds `DitherPatternValue` to
   `Float4ToRGBA16` before the 5-bit shift. The synthetic control reads
   `67200`/`9600` undithered/dithered on a `G_CD_MAGICSQ` target and
   `76800`/`0` on `G_CD_DISABLE` (`testdraw.cjs --flags 1024 --expect-dither`).
   The same session fixed the session-181 texture feedback loop: the target's
   own texture is unbound after `upload_framebuffer`, and a white texture is
   bound before every draw, so the synthetic fill reads its fill colour with no
   GL error instead of `(0,0,0)` with `INVALID_OPERATION`. Tile images are now
   keyed by content (`XXH3-64` of the decoded RGBA bytes plus size and wrap
   fields); `testdraw.cjs --flags 2048 --repeat 3` measures one upload and two
   reuses. The S2DEX2 audit found only `0xDA`/`0xDC` used; `0x03`..`0x0B` now
   return instead of falling into the F3DEX2 triangle arms. The title
   logo/copyright residual is closed as fog phase: two native logo frames 0.25 s
   apart differ by `7.01/7.03/8.77` in the logo band and `9.78/9.78/10.19` in
   the copyright band, more than the browser-vs-native pair
   (`4.33/4.37/5.39`, `6.09/6.08/6.35`) and with the same signed direction. The
   mission `0x03` party sprites also render now: the save route's map field
   shows the top-left five-character cluster and the bottom pair
   (`docs/proofs/web-mission-party-session189.png`), against session 176's
   missing party. The unit sprites' `OTHERMODE_L` has alpha compare none, so
   the feedback fix is the change that can restore a rejected draw; the pre-189
   build was not re-run for a one-variable proof. The decode still runs once per
   display list. The Load Game white frame did not reproduce in a 4 Hz capture
   of the transition: shots 0-2 are the Load Game page, shot 3 the switch, and
   shots 4-19 the mission map, none white; if it exists it is shorter than
   250 ms, and session 169's `meanRGB=255` is 85 per channel, not white. See
   `docs/HANDOFF-2026-10-24-session189.md`.
   **(x) The session-189 "4 Hz" premise is withdrawn and the tile caches are
   two-tier (session 190).** `atmark.cjs` waits `--gap` after a Playwright
   element screenshot, which costs 0.95-9.5 s with the game running, so the 20
   nominal 250 ms samples span tens of seconds and the transition got one
   sample. `debug/probes/atmarkfast.cjs` wraps `Module._ogre_gfx_flush`, reads
   the default framebuffer with `gl.readPixels` after each flush, and arms and
   disarms from `[scene]` lines. The tile decode is now skipped when a
   pre-decode TMEM source key matches (`tile_source_key`, 16 MiB decoded
   cache), and the GL texture cache keeps a texture for eight flushes plus a
   32 MiB/16 384-entry cap instead of the per-flush sweep. `testdraw.cjs
   --flags 2048 --repeat 3` measures one upload, two reuses, one decode miss and
   two decode hits; `--gap 64 --repeat 4` measures the keep window; `--flags
   6144 --repeat 3 --gap 64` measures three distinct keys and three misses for a
   changed source; `OGRE_TEX_TRACE=1` re-decodes every hit and reports
   `[GFX-TEXCACHE-MISMATCH]` (0 in the synthetic run). One Load Game run reached
   `0x12`: its two distinct frames are the page (`mean3=120.86`) and a later
   black frame (`mean3=0`), no white, and the game stopped at display list 11.
   The native reference on the same route captured 184 targets through the
   `0x12` entry and also has no white frame (highest `meanPC=97.73`, highest
   `fracW240=0.00445`). The other browser attempts landed on the idle trajectory
   at host load 16-27 (`scene 0x0000`, `block=0x00000000`), so the exact entry
   instant and the `0x12 -> 0x03` transition are still unrun there. See
   `docs/HANDOFF-2026-10-25-session190.md`.
   **(y) The tutorial combat's green background and freeze: fixed session 202,
   with three measurements open.** The browser's deferred writeback wrote a
   stale 640x481 image (`0x1BA780`) over the scene-0x02 njpeg buffers at
   `0x801CDA00`/`0x801CE3A0`, which the allocator had reused inside that image's
   481-row span; the S2DEX2 draw sampled the overwritten bytes (green/magenta)
   and the next `free()` trapped in `func_80071950`. `readback_framebuffer` now
   skips a writeback whose RDRAM differs from the hash at its last
   upload/writeback (RT64 `checkRAM`) and logs `[GFX-FBWRITE-STALE]`; the final
   build passes `0x0002 -> 0x000D -> 0x0003` with no page error and the tutorial
   battle renders (`docs/proofs/web-tutorial-combat-session202.png`). Session 203
   closed two of the three open measurements: the writeback records its row band
   through the draw's own viewport (so it no longer copies rows past the image
   into the next framebuffer), and the njpeg backdrop's green/magenta display was
   the YUV load's byte order, fixed in `tmem_load_yuv`
   (`docs/proofs/web-newgame-cathedral-session203.png`). Session 207 replaced the
   whole-image reference with one hash per guest row, refreshed before each draw
   into that row, so a writeback skips only a row the CPU changed after its last
   draw; this closed the session-202/203 per-row-hash item and fixed the second
   cathedral visit's black top-left quadrant
   (`docs/proofs/web-newgame-cathedral-birth-session207.png`). Open: drive the
   exact `3. Combat` lesson (the list scrolls with C-Up; `5. Unit Commands` also
   battles), and move `testdraw`'s scratch below the 8 MiB RDRAM (its positive
   control fails on a fresh generated build; the guard is not the cause). See
   `docs/HANDOFF-2026-10-26-session202.md`,
   `docs/HANDOFF-2026-10-30-session203.md` and
   `docs/HANDOFF-2026-10-31-session207.md`.
9. **The published v0.4.0 archives ship an empty `mods/`, and the bundle that
   feeds the release did not carry one.** Fixed on main (session 108):
   `make dist`, `tools/release-build.sh`, the workflow's unpack step and
   `tools/smoke-dist.sh` all require an `.nrm`, and `tools/data-bundle.sh` packs
   `build/mods/*.nrm`. Session 111 regenerated the private data repository's
   `files.tar.gz` — it still predated the rule, so every hosted job failed at the
   unpack step with `build/mods/*.nrm is missing from files.tar.gz` — and made
   the unpack step fail when the bundle's recorded commit differs from the commit
   being built, where it used to warn. A clean `make recomp` and `make bank-recomp`
   reproduced the tree's generated code byte for byte, so the new bundle changes
   only the mods and the recorded commit. The re-run (`36349726660`) completed
   success on all three platforms and its Release job produced **`v0.5.0` as a
   draft**, whose Linux and macOS archives both contain
   `mods/exp-overflow.nrm` and `mods/skip-boot-logos.nrm`. That release is
   published: session 113's download carries both mods and the session-107 fix.
   The published `v0.4.0` assets still have the empty `mods/`. Session 120 added
   `make release`, which is the supported way to prepare this bundle and start
   the release.
   See `docs/HANDOFF-2026-09-27-session111.md` and
   `docs/HANDOFF-2026-10-01-session120.md`.
10. **HD backgrounds: true HD needs the renderer.** `mods/backgrounds/` reskins
    the backdrop at the game's own 320x240 (box-filtered to the 496x384 canvas),
    so its detail is capped by the original. Sampling a larger texture needs
    RT64 texture replacement, keyed by the drawn texture's hash. RT64 already has
    the machinery (`TextureCache::loadReplacementDirectory`, keyed by a TMEM
    hash) and `upscale2D = ScaledOnly`, but the port does not load a replacement
    directory and the hash for one backdrop has to be recorded from a run. The
    four-chunk layout the current feature uses is hardcoded to the cathedral
    asset's chunk sizes; a pack image for a backdrop with a different layout is
    left alone (`no canvas rect`). See `docs/guides/hd-backgrounds.md`.
11. **Confirm the periodic-hitch fix on Windows, and keep the hang dump
    available.** Session 116 measured the snapshot's 1.5 s hitch on macOS and
    gated it; the Windows size is inferred from SDL's `timeBeginPeriod(1)` and
    the 200 us sampler, so one run there decides it (`OGRE_DL_TRACE=1` with
    `OGRE_SNAP` unset against `OGRE_SNAP=1`). With the gate off by default a hang
    produces no `[snap]` output unless the run asked for it; a
    frame-counter-triggered dump would keep the diagnosis at zero periodic cost.
    See `docs/HANDOFF-2026-10-01-session116.md`.
12. **Gate the remaining debug behaviour that ships (session 117).** The live
    console is done: it is off unless `OGRE_LIVE_CONSOLE=1` or one of its own
    trigger variables is set. What is left, ranked by the difficulty of removing
    it rather than by the cost:
    (a) `[rsp]` (116 lines/s) and the per-display-list `[renderer]` lines — gate
    them and set the variable in `make smoke`, `tools/smoke-dist.sh` and the
    CI `runlog.py --check`, or those checks silently stop checking anything;
    (b) the trace hooks on every recompiled function entry and return (28 + 4
    instructions at 5316 sites with the gates off) — making the disabled path a
    load-and-branch needs a change to `patches/n64recomp-ob64.patch`, so it
    regenerates the game code and re-runs the stutter measurement;
    (c) the audio auto-response on `0x800C49E8` is a behaviour shim, not a log:
    firing the real `OS_EVENT_AI` when a buffer drains lets it be deleted.
    See `docs/HANDOFF-2026-10-01-session117.md` for the full list.
13. **The browser build assigns gamepads to N64 controllers 2-4. Closed (session
    153).** The native fix for issue #13 landed in `app/src/sdl_platform.cpp` (the
    first pad takes slot 0), while `app/web/web.js` placed gamepads in slots 1..3
    with the keyboard alone in slot 0, so a browser gamepad could not reach N64
    controller 1 (`docs/HANDOFF-2026-10-01-session118.md`). `web.js` now assigns
    the lowest free slot, so the first pad is controller 1, and slot 0's mask is
    the pad's buttons OR'd with the keyboard and the tap schedule. The pad pass
    runs after the keyboard pass, because it writes slot 0. The header comment in
    `app/src/web_platform.cpp` and its `ogre_input_debug` helper documented the
    old model and returned slot 0 only; both are corrected, and
    `debug/probes/gamepad.cjs` verifies the pad in slot 0 and the keyboard beside
    it. No hand-played check was made with a real pad, so a physical pad on a
    real page is the one thing still unverified.
14. **RESOLUTION and MSAA artifacts.** Both rows work and reach RT64, and a
    player run with them showed visual artifacts that were not characterised
    (developer, session 121). They ship behind `OGRE_DISPLAY_EXPERIMENTS=1`
    (hidden rows, values not applied) until someone reproduces the artifacts
    with a capture and fixes them in RT64 or in the port's configuration. The
    first step is a captured A/B of a named screen at native vs `2x`, and at
    MSAA off vs `4x`, since both settings are one `GraphicsConfig` field.
    See `docs/HANDOFF-2026-10-01-session121.md`.
15. **A mod's own config file is rejected unless it carries `mod_id`, so every
    mod option read its default. Closed (session 123).**
    `parse_mod_config_storage`
    (`tools/N64ModernRuntime/librecomp/src/mod_manifest.cpp:815`) returned false
    when the JSON had no `mod_id` key, and `Config::load_config` then leaves every
    option on its default with no error and no log. The loader now accepts a
    missing key, because the file's own name is the mod id, and still rejects a
    key that names a different mod. A/B on one file: unpatched
    `[mod] start verb mode shops 0 0 0 0`, patched `2 1 1 0` /
    `[mod] start chance seed 55 12345`. Session 122's `recomp_log` export was also
    in no patch, so no release carried it and every mod that imports `recomp_log`
    was refused by a shipped build; both changes are now in
    `patches/n64modernruntime-ob64.patch` and `tools/patchcheck.py` passes. See
    `docs/HANDOFF-2026-10-03-session123.md` §1.
16. **The Item Randomizer: a map pickup is finished and verified; the battle
    reward is not (session 123), and the mod has no options (session 125).** The
    mod randomizes a map pickup end to end on
    the developer's own save: the popup and the inventory both name the
    replacement (`pickup-table 205 10 0 0` Old Clothing -> Glamdring,
    `acquire 10 21 0 1`, `table-grant 10 21 0 1`,
    `frame acq countup reroll 8975 1 0 0`). Session 125 removed the five MODS
    panel options: the replacement may be anything in the item table, the roll is
    unconditional, a shop purchase is always skipped, the seed is derived from
    the frame counter every boot, and the log is the compile-time `LOG_LEVEL`.
    Session 123 fixed the option loading,
    the consumable table (base `0x8018E6EC`, name at `+0x00`), the save-load
    misread, the duplicate-id case, the fight between the two mechanisms, and the
    unrewritten per-map ground-item list. The save-load signal is a hook on the
    save-field reader `func_800749C0` (`.main`, regenerates cleanly). A scene
    change is **not** the signal: the game grants items on scene changes, so a
    scene-keyed re-prime swallowed the mission-entry grants and the battle
    reward, which the developer saw as a reward that was "the expected item from
    the loot table, not a random one". A hook on the used-count rebuild
    `func_8016B774` crashes the save load. What remains:
    * **The battle reward is randomized** by rewriting the pending-reward queue
      at `0x801936D8` (20 `u16`; `func_ovlN_801E49D0` reads
      `lhu(0x801936D8)` at `0x801E5230`, stores it to `0x8021C760`, grants it at
      `0x801E53DC` and clears it at `0x801E53F4`). The queue, not the drop table,
      is where a battle reward id lives at grant time: with the table's 155
      rewritten to 17 the queue still held `0x809B` and the reward merged 7 -> 8
      into a stack of Leather Armor the party already owned. Verified with the
      developer's seed (`reward-queue 155 17 2 0`, `reward-queue 94 235 4 0`) and
      the developer's report of "oracion" from a battle, and then two rewards read
      back name for name: vanilla Iron Claw (94) -> 3 Falchion and vanilla Leather
      Armor (155) -> 193 Magician's Robe, with the message naming the same item
      for both. The second merged into a stack the party already owned (count
      1 -> 2), which the list watch alone could never reach.
    * **The `+0x02` used count is recomputed by the mod (session 124).** The
      item screen draws `+0x02` then `+0x03` and the equip test is
      `sltu(+0x02, +0x03)`, while the grant writes the id and `+0x03` only, so a
      record that was empty with a non-zero `+0x02` reads `1/1` under the new
      item's name and refuses every equip. A mod cannot call the game's rebuild
      `func_8016B774` (`jal` from `0x81000000` to `0x80xxxxxx` fails
      `R_MIPS_26`, and a hook on it crashes the save load), so the mod
      reimplements it: 30 unit records at `0x80197210` stride `0x19`, 100
      character records at `0x80193BE0` stride `0x38`, and four class-derived
      ids per character from `0x80186FF4` into `0x80187C62`. `fix_record_use`
      writes the count for every record the game newly filled. **The
      reconstruction is verified against the game's own byte** by `make refscan
      DUMP=<image>` (`tools/refscan.py`): three developer dumps give 0
      mismatches over 21, 22 and 25 occupied equipment records and all the
      consumable records, and `--item <id>` names the records that credit an
      item. **The write itself has never run in game**: no acquisition in nine
      runs inherited a non-zero `+0x02`, so the correction has had nothing to
      fix. A self-test that forced the case was removed because its gate
      (`drop_table_present()`) tests words the mod itself rewrites. See
      `docs/HANDOFF-2026-10-03-session124.md` and `docs/notes-item-equipped.md`.
    * **The party's starting items are left as the game granted them (session
      126).** The map loader `func_ovlR_80215C38` (bankR record 9d, called once
      per map from bankN `func_ovlN_801AE880+0x764`, `jal 80215c38` at
      `0x801AEFE4`) equips each unit it creates from that unit's class row via
      `func_ovlR_801DD430` (`0x80216F10`-`0x80217028`) and then grants the
      starting set with literal ids: `1,1,1,1,1,8,8,22,22` at `0x80217068` when
      `lbu(0x8018F4A1)` is 63/64, else `2..5/8/21..23` at `0x802170CC`. Several
      records fill in one frame, which the old `RELOAD_SLOTS 12` threshold did
      not catch, so every one was rerolled. `BULK_SLOTS` (1) makes a frame that
      fills more than one record a re-prime instead of a reroll; a single filled
      record is still an acquisition, so a find and a reward are unaffected. A/B
      on a New Game from the title: the old build rerolled the four bulk frames
      (9, 9, 8 and 14 records, 39 `replace` lines) and the developer saw the
      randomized starting items; the new build re-primes the same frames and the
      developer saw the expected items. See
      `docs/HANDOFF-2026-10-03-session126.md`.
    * **Bank-function hooks** would let the grant routine be hooked directly. The
      scope is in `docs/notes-bank-hooks.md`; it needs a merged `mod-syms` dump,
      a hook-section registry kept out of `sections_info`, and `rom_size` on
      `BankFunctionEntry`. A hook on `func_8016B774` is known to crash the save
      load, so the re-prime signal must avoid the save path.
    * **Loading a save crashes with the mod enabled (session 213, developer
      report).** The developer's battery `build-app/saves/ogrebattle64-us-rev1.bin`
      (2 valid slots) SIGBUSes when the Load Game screen applies a slot:
      `signal: SIGBUS`, `n64 thread: 4`, fault address `rdram + 0x800375F0`,
      empty call chain. The same battery and settings load to scene `0x05`
      without the crash when the randomizer is unticked, and the crash still
      reproduces with session 213's cross-bank dispatch removed and the whole
      game recompiled, so it is the mod's path. Under lldb the fault is
      `movslq (%rbx,%rax)` with index `0x375F0`, in an `rwx` region outside the
      app image (the mod's loaded code), called from the Load Game update
      `func_80178430`; the randomizer replaces the save-field reader
      `func_800749C0` to hook it. Opening the Load Game screen alone does not
      crash; pressing A to apply the slot does. A rebuild of the mod with the
      current toolchain (`build/mods/item-randomizer.nrm`, 5977 bytes) crashes
      the same way. Workaround: untick the mod in MODS before loading.
      Reproduction: `OGRE_MOD_TEST=1 OGRE_SAVE=<battery> OGRE_SCENE=title
      OGRE_TAP_MS=1000 OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6"`. The fault
      address is the same `rdram + 0x800375F0` shape as item 17's tutorial
      reports, which were never A/B'd with the mod off.
    See `docs/HANDOFF-2026-10-03-session123.md`.
17. **The Tutorial (scene `0x17`) crashes on entry with a SIGBUS; the Item
    Randomizer is in no frame of it.** Four macOS crash reports on 2026-10-03 (16:59:21,
    17:38:38, 17:46:34, 17:46:57) are one fault: `EXC_BAD_ACCESS` /
    `KERN_PROTECTION_FAILURE` on N64 Thread 3, frame 1 at `func_80075BC0`
    (the scene dispatcher) and frame 2 at `func_80071EB0` (the streamed-overlay
    loader). Frame 0 has no symbol and the fault address differs per run
    (`0x199ab25f0`, `0x1a4e015f0`, `0x1a4c195f0`, `0x19dd635f0`), so the crash is
    a data address the guest computed, not a fixed one. Every address is above
    `0xE0000000`, which no guest mapping in this port reaches, so the guest is
    dereferencing a value that is neither a guest pointer nor a guest code
    address. The run log ends right after the tutorial's two bank records load
    (`rom=0x069920` -> `0x80197B90`, `rom=0x1BA020` -> `0x80220F60`) and a save
    read. The mod is in no frame, it only writes guest RAM and never installs a
    pointer the game follows, and an unknown guest code address resolves to a stub
    rather than a jump. The earliest report (16:59) also predates session 124's
    change to the mod. That is a mechanism argument and not a measurement: the run
    that would have measured it did not have the mod off. Session 213 reproduced
    the same `rdram + 0x800375F0` fault on the Load Game path, deterministically
    with the randomizer enabled and absent with it disabled (item 16), so the
    mod-off A/B is the first check for this fault too. The A/B that was meant to show this did
    not: `run-with-save.sh` copies `build-app/mods.json` over any `mods.json`
    written into the preference directory, so the mod was loaded in both runs;
    the veto has to move `<pref>/mods/item-randomizer.nrm` aside. Open: which
    instruction in `func_80075BC0` computes the bad address, and whether the
    tutorial is reachable at all from a suspend save rather than through its own
    flow (`AGENTS.md` §9). **It is not reproducible now**: the developer reports
    the tutorial runs fine, and neither binary changed, so the rebuild cannot be
    the reason (`build-app/ogrebattle64` is still `Oct 3 12:06:40`, 27277408
    bytes, the binary the reports came from; the mod's `.nrm` is unchanged at
    8591 bytes). The leading candidate is the save sandbox: a crash mid-write
    leaves the sandbox copy inconsistent with the imported file, and every report
    follows another run. If it returns, capture the save at the crash and read
    `D_800AF028`, the resolved descriptor and its hook words from the frame
    before. See `docs/notes-tutorial-crash.md` §5.
18. **The speed badge is not drawn in fullscreen, and it hides itself when the
    pointer is over it (session 137).** The badge is a small borderless
    always-on-top SDL window over the corner of the game window, because RT64
    owns the game's swap chain and has no UI pass. Fullscreen is a display space
    of its own, so the window has nowhere to draw; the speed still changes and
    only the readout is missing. The window also takes the mouse focus while the
    pointer is inside its 56x47 rect, because SDL2's `SDL_HitTestResult` has no
    click-through value (the bundled headers have `NORMAL`, `DRAGGABLE` and the
    resize edges, and cocoa implements only `DRAGGABLE`), so the badge hides on
    mouse focus rather than swallowing a click or holding the map and mission
    cursors' motion events. A badge that survives both needs a draw inside RT64's
    frame, or a texture and display list the port submits itself.

19. **Closed (session 183): the browser's q meshes were failing the depth test
    against stale depth; the depth-image invalidation fixes them.** Scene `0x0A`
    shares `0x2E6840` between the plate colour image and the depth image, the q
    draws are depth-tested and depth-written (`OTHERMODE_L 0x00552078`), and the
    colour fills did not invalidate the depth target, so the q fragments compared
    against stale depth and failed `GL_LESS`. The renderer now mirrors RT64
    `depthFbTypeChanged`; the A/B that separates the two states is in
    `docs/HANDOFF-2026-10-19-session183.md`, and the 10 Hz capture holds the
    reference frame for the whole still in both renderers. The batch-boundary
    experiment session 181 named is not needed. This also corrects the
    "no depth test/write" reading in the session-180/181 bullets.

20. **Closed (session 185): the browser's QUEST q's were flat because
    `G_GEOMETRYMODE`'s keep mask was inverted, which kept `G_LIGHTING` out of the
    state and made `G_TEXTURE_GEN` inert.** `OP_GEOMETRYMODE` read the low 24 bits
    of `w0` as the bits to clear; RT64 reads them as the bits to keep
    (`GBI_F3DEX2::geometryMode`, `gbi/rt64_gbi_f3dex2.cpp:104-108`, and
    `RSP::modifyGeometryMode`, `hle/rt64_rsp.cpp:886-890`). The ROM's own reset
    idiom `D9000000 00000000` (41 sites, and `D9FFFFFF 00000000` never appears)
    is a reset under the keep rule and a no-op under the inverted one. The q
    state block at guest `0x801B85B8` (native full-RDRAM dump at 14.5 s) sets
    `G_LIGHTING` with `D9FFFFFF 00200405` and `D9FFFFFF 00020000`, and the
    sub-list at `0x3A4790` adds `G_TEXTURE_GEN`: keep rule `0x260405` (both
    bits), inverted rule `0x040000` (no lighting). RT64's `usesTextureGen` needs
    both, so native sweeps the 32x32 image with the surface normal while the
    browser sampled the vertex `s,t` (`G_TEXTURE` `0x07C0` puts that at about
    1.3 texels). The decoder now uses the keep mask, and `G_TEXTURE_GEN`,
    `G_TEXTURE_GEN_LINEAR` and the `MV_LIGHT` lookat slots 0/1 are implemented
    as RT64's `computeTextureGen` (`TextureGen.hlsli:19-33`); the lookat vectors
    are the per-frame `(1,0,0)`/`(0,1,0)` at `0x803A2B60`/`0x803A2B70`.
    Measured on 34 s 100 ms captures from a natural boot: frames with a channel
    above 200 are native 24 of 24 (169-192), the browser 26 of 27 (209-235)
    after the fix against 6 of 28 before. The fixed pose pair (native 190,
    browser 234) has mask IoU `0.791`, whole-frame mean absolute difference
    `2.31`, and mask means native `(80.4,56.5,17.2)` against browser
    `(80.7,63.5,22.1)`. Session 184's "the sub-list clears `G_LIGHTING`, so
    neither renderer generates coordinates" is withdrawn, and that is why its
    `G_TEXTURE_GEN` implementation measured no pixel change. Scene `0x09`
    against native is unchanged (`10.032` against `10.109`) and the first
    scene-`0x04` frame is byte-identical to the pre-change build, so the
    change is neutral outside the stills. See
    `docs/HANDOFF-2026-10-23-session185.md`.

21. **The browser options overlay is in the page; its final-build game frame and
    the 3D widescreen frame are unrun.** Esc and the topbar Options button open
    MAIN, CONTROLS, SETTINGS and DEBUG (MODS disabled). GAME SPEED, WIDESCREEN
    and MOUSE ENABLED are the shared `settings.cfg` values through
    `app/src/web_settings.cpp`; CONTROLS is localStorage; SOUNDS/VOLUME are the
    AudioWorklet. The overlay, exports, bindings, volume, fullscreen and `?menu`
    routes are measured, but every final-build boot (and a rebuilt unmodified
    HEAD control) landed on the idle trajectory under host load 11-13, and the
    wide projection has no frame capture. Re-run on an idle host with the
    mission/dialogue route. See `docs/HANDOFF-2026-10-25-session191.md`.

22. **The tutorial practice stage's ground (sessions 198-201). Closed.** The
    stage is scene `0x0003`, reached by `tutorial (0x17) -> 0x02 -> 0x0D -> 0x03`.
    Two browser renderer defects were behind it. A triangle that crosses the eye
    plane was dropped whole (`transform_vertex` failed on clip `w <= 0`), and the
    field's ground is one 76x76-unit quad whose near corner is at `w = -1.67`, so
    both of its triangles were never drawn; the renderer now cuts such triangles
    against `w > 1e-3` (`emit_clipped_tri`). The ground then drew, but its tile was
    stretched into bands because the texture coordinate was interpolated in screen
    space while RT64 interpolates it perspective-correctly; the vertex shader now
    writes `vec4(a_pos * a_w, a_z * a_w, a_w)` by default, with `OGRE_PERSP=0` as
    the A/B. The grass is the noisy texture native shows
    (`docs/proofs/web-tutorial-field-session201.png` against `-flat-session201.png`
    and `-bands-session201.png`). The FPS side of the same report is closed
    separately: the uniform-location cache and the `glGetError` gate took the
    mission from 16.78 to 29.4-29.8 fps, and a draw merge that cut the calls 8.6x
    changed the rate by nothing. Open from this work: the whole-field overlay
    (`comb=FCFF99FF`, 64x64 CI8 tile `0x37E5A8`) contributes alpha ~0 across the
    field, so its mottling never appears, and the scripted `tutorial-field` route
    is not deterministic. See `docs/HANDOFF-2026-10-26-session201.md`, `-199.md`
    and `-198.md`.

23. **Browser GAME SPEED 2x/4x holds (session 208); 8x and the remaining flush
    cost are open.** The main-thread flush no longer holds the renderer mutex
    across its GL batch, and it skips the per-draw uniform block when a draw's
    values equal the previous draw's. 2x and 4x now reach 120 and 234.5-240.7 guest
    frames/s with the page's requestAnimationFrame cadence at 16.67 ms (95th
    percentile); 8x reaches 414.8 of 480. The next candidates are the per-draw GL
    state calls (`glViewport`/`glScissor`/`glEnable`/`glDisable` are issued once
    per draw: about 90 000 viewport and scissor calls and 271 000
    enable/disable calls per second at 4x) and `reference_era_rows`, which
    XXH3-hashes every guest row each draw covers. `ogre_gfx_flush` also still runs
    twice per 16 ms timer tick (the interval callback and `updateGfxStatus`); the
    second call is redundant now that the flush is cheap. See
    `docs/HANDOFF-2026-11-01-session208.md`.

24. **Browser public release (session 209).** The page now carries the
    experimental notice and exports the battery. The Pages workflow and the
    static package were already in place; the deployed site was not re-checked
    after this change (the local static server served the same layout). The
    export is the raw logical image; a player whose emulator wants parallel-n64's
    byteswapped `.sra` still needs `tools/sramsave.py export`. The mid-copy retry
    window was not measured at the file-system level. See
    `docs/HANDOFF-2026-11-01-session209.md`.

25. **RESTART GAME has not been pressed in the RT64 window (session 212).** The
    row, the request path and the relaunch were measured with `build-null` and
    `SDL_VIDEODRIVER=dummy`: `[overlay] RESTART GAME` is followed by a second
    `[boot] init_sdl...` in the same log, through the full `recomp::start`
    teardown and `shutdown_sdl`. What is unmeasured is the RT64/Metal window on
    macOS (the child starts after the parent has released the renderer) and the
    Windows `_wspawnv` path, which is compiled only by the Release workflow. The
    recipe is in `docs/guides/app-build.md` -> "Verifying it without a human".
    See `docs/HANDOFF-2026-11-02-session212.md`.

Parked, not defects:

- Audio above 1× speed is untested; `OGRE_SPEED` runs always had audio off
  (`docs/HANDOFF-2026-09-20-session93.md` §8).
- The `ESC` overlay does not pause the game (the developer's choice), and gamepad
  rebinding is code-verified only because this machine has no gamepad
  (`docs/HANDOFF-2026-09-20-session92.md` §9).
- `OGRE_NO_AUDIO=1` is the workaround on hosts where `SDL_OpenAudioDevice`
  blocks.
- `0x801936A9`, the community's Chaos Frame address, is in the same saved flags
  block but has no static read or write in this ROM
  (`docs/HANDOFF-2026-09-21-session95.md` §5).

## Roadmap

1. **Phase 0 — toolchain & ROM** ✅
2. **Phase 1 — disassembly & ELF** ✅
3. **Phase 2 — recompile the main segment** ✅
4. **Phase 3 — first boot** ✅ (sessions 3-6)
5. **Phase 4 — overlays & streamed code** ✅ — 44 records in 34 units (A..AH);
   no un-recompiled module on a reachable path (session 85)
6. **Phase 5 — assets** ✅ — LZ, njpeg backgrounds, textures; 99.05% of the code
   span recompiled; `tools/recompcov.py` measures coverage
7. **Phase 6 — saves, audio, QoL** — the SRAM battery and the Controller Pak
   device work, the audio microcode runs, and settings, controls and the overlay
   are done. Open: the Controller Pak copy flow, audio above 1×
8. **Phase 7 — modding framework & packaging** ✅ — mod system with a shipped
   example, `make dist` packages, GitHub releases

## Toolchain

| Tool | Purpose | Location |
|---|---|---|
| splat 0.50 (`splat64[mips]`) | ROM splitting / disassembly | `tools/venv` |
| spimdisasm | MIPS disassembler (used by splat) | via pip |
| mips-linux-gnu-binutils | assemble `.s` → `.o`, link ELF | Homebrew |
| N64Recomp (forked) | MIPS → C recompilation | `tools/N64Recomp` (vendored clone) |
| N64ModernRuntime | recompiled game runtime (libultra shim, renderer) | `tools/N64ModernRuntime` (vendored clone) |
| RT64 | renderer | `tools/RT64` (git submodule) |

`tools/N64Recomp`, `tools/N64ModernRuntime` and `tools/RecompFrontend` are
gitignored clones, not submodules. RT64 is the only submodule.

Our modifications to the upstream tools are the five patches under `patches/`:
`n64recomp-ob64.patch`, `n64modernruntime-ob64.patch`,
`n64modernruntime-n64recomp.patch`, `rt64-ob64.patch` and
`rt64-plume-ob64.patch`. `tools/rt64-plume-sdl.patch` is separate: it is the
SDL ≥ 2.0.22 guard for RT64's plume, for systems with an older SDL2. Apply order
and commands are in `docs/guides/app-build.md` → "Configure & build".

## Reproduce (macOS)

> Linux (Ubuntu) setup: see `docs/guides/linux-migration.md`. The commands below
> are the same except `brew install` → `apt install` equivalents (listed there)
> and the `xcodebuild ... MetalToolchain` line, which is macOS-only.

```sh
# 1. Tools
brew install mips-linux-gnu-binutils cmake
# Xcode 26+: `metal` is a separate download; skip if `xcrun --find metal` works
xcodebuild -downloadComponent MetalToolchain
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../patches/n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4

# 2. ROM: place your big-endian dump at assets/ogre64.z64
#    (tools/convert_rom.py converts a 16-bit byte-swapped .n64)

# 3. Regenerate the recompiled code (splat -> ELF -> 34 bank units ->
#    main recompilation -> RSP microcode), then build the app
make regenerate
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
./build-app/ogrebattle64
```

`make regenerate` is the one supported order. `make recomp` refuses to run when
`app/src/bank_funcs.inc` is missing, because its cross-bank dispatch would then
be skipped and calls into swappable RAM would bind to the wrong bank. See
`docs/guides/app-build.md` → "Regenerating the recompiled code".

## Key technical findings

- **Byte order.** The conventional dump is a 16-bit byte-swapped `.n64`.
  `tools/convert_rom.py` converts it to big-endian `.z64`. The app accepts
  `.z64`, `.n64` and `.v64`.
- **Header layout.** Official Nintendo layout: entry point at offset `0x08`
  (`0x80070C00`), internal name at `0x20` (`OgreBattle64`), country at `0x3E`.
- **Memory map.** ROM `0x1000` ↔ vram `0x80070C00` (entry segment, `0x60` bytes);
  main segment ROM `0x1060` ↔ `0x80070C60`, ending at BSS start `0x800AEDB0`;
  BSS `0x800AEDB0..0x800E9C20`; stack `0x800C6D60`; `main` at `0x8007F880`.
- **libultra** is bridged by name: `config/symbols/symbol_addrs.txt` names the
  libultra functions so the runtime's native `osXxx_recomp` services replace
  OB64's verbatim copy. `docs/LIBULTRA-BRIDGING.md` records the identification
  method and the address→name table.
- **Streamed code is mapped and compiled.** Each streamed overlay is plain
  linked MIPS code and data DMA'd to a fixed RAM address. Records that share a
  RAM range are separate bank units, so a call into the range compiles as
  `LOOKUP_FUNC` and the runtime's DMA-driven bank map picks the resident module.
  Compiling a swappable range into the unit that calls into it binds the call to
  one bank's bodies at build time and runs them with the wrong frame contract
  (sessions 41/45/55). AGENTS.md rule 4 holds the details.
- **Scene graph.** `func_80075BC0` looks up `D_800AF028[scene_id]()`; there are
  exactly 25 scene types, 39 transitions and a 1693-entry scripted step table.
  `tools/scenemap.py` extracts all of it from the ROM in about 3 seconds.
  AGENTS.md's "Verified facts" holds the addresses.
- **Display lists are not run through recompiled microcode.** The runtime routes
  gfx tasks to RT64, which parses them with its own GBI interpreters. RSPRecomp
  compiles the game's njpeg and audio microcodes only.
- **The world map's cursor is a screen-space sprite, and the mouse drives it.**
  The map module keeps `state+0x52`/`+0x54` as screen pixels and clamps them to
  x `0x28..0x110`, y `0x28..0xC0` (`func_ovlM_8019AF0C`), and the scene hook's
  cursor draw (`func_ovlM_801A103C`) reads the pair at
  `0x801A1C48`/`0x801A1C4C`. `app/src/map_cursor.cpp` writes the pointer's pixel
  into that pair at the draw site (`tools/map_cursor.py` inserts the call and
  `make bank-recomp` re-applies it, because bank code is outside the mod hook
  map). A left click ORs the N64 `A` bit into controller 0 in `get_input`, and a
  pointer within 24 pixels of a play-area bound holds the matching D-pad bit,
  which scrolls the view. All three are developer-confirmed. See
  `docs/guides/app-build.md` → "The world map's mouse cursor" and
  `docs/DECISIONS.md` (2026-10-10).
- **The mission field's cursor is also a screen-space pair, and it is digital.**
  `state = *(u32*)0x801AB870 = 0x800E91D0`, a fixed BSS address that the mission's
  modules each hold as a pointer constant (session 115 read it through
  `*(0x80213690)` and called it a heap pointer). `state+0x08`/`+0x0C` are f32
  framebuffer pixels; the mover `func_ovlN_801BE4E0` reads the **held** button
  word `*(0x800E8108)` and the profile's D-pad masks at `*(0x801F0CC0)`,
  integrates a per-frame delta and clamps to x 40..280, y 30..210. There is no
  analog path in any mission bank: the map reads the stick at
  `*(0x800C47F0)+2/+3`, the mission does not. `app/src/mission_cursor.cpp` writes
  the pointer's pixel at the generated label `L_801BEC04`, after both clamps and
  before the tail that reverts the cursor when no unit is focused, and stands
  down while a direction is held, adopting the game's position so the release
  does not jump. **The mission field's camera pans with the cursor, and not with
  a held direction, so the pointer scrolls the view by holding the direction the
  game's own mover reads.** The mover `func_ovlN_801BE4E0` converts the held
  D-pad into the delta bytes `state+0x06`/`+0x07`, and `func_ovlN_801BECF0`
  (called right after it at `0x801AF130`) adds `delta/128 * 8.5 * table[e]` to
  the eye x and the look-at x of the camera at `*(u32*)0x8018F5AC`
  (`0x801BEE04`-`0x801BEE1C`), and the same for z (`0x801BEE9C`-`0x801BEF5C`),
  clamped to the map's world rect at `0x801F0DD8`; it runs only while
  `*(0x801F3630)` is set, which the mover sets only when the cursor pair changed
  on that call (`0x801BEC3C`-`0x801BEC70`). The camera is `+0x14` the eye (its
  height at `+0x18`), `+0x20` the look-at and `+0x2C` the up vector, which
  `func_ovlA_801AAE30` builds the screen-to-world basis from; `lw $v0,-2644($v0)`
  with `lui $v0,0x8019` is `0x8018F5AC`, and not `0x8019F5AC`.
  `app/src/mission_cursor.cpp` holds the matching N64 D-pad bit while the pointer
  is within `OGRE_MISSION_CURSOR_EDGE` (24) px of a play-area bound, and
  `get_input` ORs it into controller 0; the four masks the mover compares are this mode's
  profile's own (`*(0x801F0CC0)`), which are the N64 bits. See
  `docs/guides/app-build.md` → "The mission field's mouse cursor" and
  `docs/DECISIONS.md` (2026-10-10, sessions 128 and 129).

## Working documents

| Document | Holds |
|---|---|
| `AGENTS.md` | working rules and the durable verified facts |
| `docs/README.md` | documentation index |
| `docs/HANDOFF-YYYY-MM-DD-sessionN.md` | per-session record, newest number wins |
| `docs/STATUS-LOG.md` | the session status log this file used to carry |
| `docs/DECISIONS.md` | decision log; the "Durable decisions" table at the top is the current set |
| `docs/scenes.md` | what each screen should show, from the developer |
| `docs/symbols.md` | proposed symbol names with evidence and confidence |
| `docs/guides/app-build.md` | build, run, every `OGRE_*` knob, the diagnostics toolkit |
| `docs/guides/emulator-first.md` | read before interpreting a display list |
| `docs/guides/njpeg-backgrounds.md` | the njpeg background pipeline |
| `docs/guides/rsp-microcode.md` | RSP microcode research and recompilation |
| `docs/guides/app-architecture.md` | app structure, runtime flow, callbacks |
| `docs/guides/linux-migration.md` | Ubuntu setup |
| `docs/WEB-PORT*.md` | the browser port (dated 2026-08-29) |

Session 192 identified the mission's 3D ucode as `F3DEX2.NoN.fifo 2.08`
(`ucode=0x800A08D0`, text hash `0x4C12DAE0534D7135`). RT64's database maps it to
`GBIUCode::F3DEX2` with `flags.NoN`, which disables only the F3D near-depth clip;
the browser reported it as unknown and kept the clip. The hash is now
recognized as `GbiKind::F3DEX2_NON_FIFO` and draws from it carry
`no_near_clip`, so `u_zclip` stays 0. The green lines around the mission's framed
target-fort illustration are the missing framebuffer-as-texture tile copy: the
frame art is built from loads whose sources sit inside colour image `0x228080`,
and the browser reads stale RDRAM (24 `[GFX-FBTEX]` loads at scene `0x03`). RT64
makes a per-tile copy with a stale-RDRAM guard. A whole-image writeback was tried
and crashed the game in `func_ovlN_801F7C60`, because the image's byte range also
holds game textures; it was reverted. The browser RDRAM dump renders the same
garbage frame with `tools/ogre_frame.py`, against native's gold frame
(`docs/proofs/web-mission-intro-rdram-session192.png`,
`native-mission-intro-rdram-session192.png`). The field's darker, flatter look
is still open and is not explained by the frame path. See
`docs/HANDOFF-2026-10-25-session192.md`.

Session 193 implemented that tile copy and corrected the paragraph above. The
copy exists behind `OGRE_FBTEX=1` and never writes RDRAM: the gfx thread
publishes the draws so far, the browser main thread reads the colour image's
framebuffer object into a private buffer in the guest byte layout, and the load
copies TMEM out of it. A same-screen A/B on the mission briefing shows it must
not be enabled. With the copy off the NOTE and label read correctly; with it on
they become white bars. A dump of the copy for `0x228080` decodes as a white
320x240 frame and `[GFX-FBCONTENT]` shows seven full-image rect draws replacing
the whole image, while RDRAM at the load time holds the CPU-composed briefing.
The loads were therefore reading the right bytes. RT64's tile copy is
authoritative only because RT64 re-uploads RDRAM into the framebuffer when the
CPU changed it (`checkRAM`/`uploadRAM`) and writes the framebuffer back at the
end of every workload; the browser has neither, so RDRAM is the source a load
must read. The green lines around the framed illustration are not stale RDRAM:
they are present with the copy on and off, so the next lead is the decode and
sampling of the 1-row border strips. Default behaviour is unchanged from
session 192 and the mission run does not crash. See `docs/HANDOFF-2026-10-25-session193.md`.

Session 194 withdrew that last lead. The border strips are not CI4 and not
1-row loads: the illustration is two RGBA16 images (a 19-band frame of 91x4
rects with `line=23`, an 84x67 picture in three `line=21` blocks) whose
transparent texel is `0x07C0`, pure green with alpha 0. The load and the decode
are exact against the source and RT64's `loadTileOperation`, so the green is the
game's data, and the hairline is the last row of each band: 4 rows over 4 pixels
puts the last pixel's centre at `t=3.5`, whose bilinear taps are rows 3 and 4,
and row 4 holds the previous occupant of that TMEM base. `OGRE_TEXWINCLAMP=1`
bounds a wrapped sample to the tile's rect and removes the band-seam hairlines
and the notch stripes, but not the frame's own outline, which is filtered across
the boundary between opaque art and the same image's transparent green texels.
The switch stays off until the `OGRE_FOG` layers and the map's water are
compared with it on, and a native RDRAM dump of the same screen is the datum
that decides the remaining outline. See `docs/HANDOFF-2026-10-25-session194.md`.

Session 195 corrected both: the bands are not sampled past their art by the game
or by the hardware, they were by this renderer. A rectangle's fragments sample
at pixel centres, so a 1:1 `G_TEXRECT` was half a texel low and right of the
texel RT64 samples; RT64 shifts every rectangle half a pixel right and down
(`halfPixelOffset`). `OGRE_RECT_HALFPIX`, on by default, subtracts half a step
from the corner UVs of `draw_texrect` and of the S2DEX object rectangle. It
removes the band-seam hairlines, the frame's own green outline and the pin's:
0 green-dominant pixels in the illustration rectangle against 398 with the
switch off, and the region's mean absolute difference from RT64's own 320x240
framebuffer is 2.44 in two on runs against 22.21 and 22.20 in two off runs, and
the map's upper band improves too (3.02/3.06 against 8.06/7.94).
`OGRE_TEXWINCLAMP` is a symptom bound, not
the fix, and stays off. See `docs/HANDOFF-2026-10-25-session195.md`.
