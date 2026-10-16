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
`docs/HANDOFF-2026-10-16-session153.md`.

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

## Status

The game runs end to end. A build boots the publisher stills, the title, the New
Game opening, the map, missions and battles, the tutorial, the shop, the
Organize Screen, the credits, the ending and the attract loop.

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
   dispatch targets as 130 missing-function candidates, 152 bank-selection
   candidates, 369 inert, 332 resolvable and 3 IMEM. The main unit also makes 951
   direct calls into swappable RAM across 236 targets (`make cross-bank-check`),
   a known backlog that `cross_bank.py dispatch --only` fixes one target at a
   time. The 288 indirect `jalr` sites are outside any static measurement; a
   run's stub log is the only check for them.
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
   Still open in the browser renderer: framebuffer-as-texture (a `G_LOADTILE`
   whose source is a rendered colour image reads RDRAM) — instrumented in session
   152 (`[GFX-FBTEX]`, `fbtex: hits=` in `#gfxstats`, the positive control
   `testdraw.cjs --flags 32 --expect-fbtex`), and no screen reached so far needs
   it, so it stays unimplemented until one reports itself — and the residual
   per-channel difference between the browser's canvas and RT64's render target,
   which session 146 reduced from 4.66/5.05/5.54 to 2.29/2.48/3.10 mean absolute
   per channel by fixing the present geometry (exact channel samples 39.4% ->
   81.4%; `tools/web_frame_cmp.py` is the comparator). What is left is two moving
   cloud bands (rows ~64..112 and ~195..238, browser 12-16 darker), and the
   developer states the port's clouds are not exact against retail yet, so the
   two runs' cloud phases are not separable on that screen. Presentation now
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
   outstanding items are framebuffer-as-texture (instrumented in session 152, no
   screen found that needs it), the `fldRegs` x/y scale and offset (RT64 uses
   them only in `VI::fbSize`, which the browser ports, and not in
   `VideoInterfacePS` or `cropRectangle`, so no browser change is outstanding
   there), the unwritten-edge and rounded-corner item (j) below, the RDP fill
   scissor question (k) below, and the two moving-cloud
   bands.
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
   **(j) The canvas element's rounded corners clip four content pixels, and the
   presenter's unwritten last column and row read as differences against a whole
   render target.** `#game-canvas` carries `border-radius:6px`, so the four
   corner pixels of every frame are blended with the border (72..108 levels off
   RT64's target); the last column and row (639 of 640 and 475 of 476) are never
   written, because `blit_to_canvas` writes only the VI's visible area, which is
   the `fbSize` minus the overscan crop, while RT64's own window is shrunk to the
   visible size by `removeBlackBorders`. Both are cosmetic and neither is a
   rendering difference; over the visible area the publisher still is exact
   (99.997% of channel samples, 9 differing pixels). `tools/web_frame_cmp.py
   --inset N` keeps them out of a measurement.
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
    that would have measured it did not have the mod off. The A/B that was meant to show this did
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
> are the same except `brew install` → `apt install` equivalents (listed there).

```sh
# 1. Tools
brew install mips-linux-gnu-binutils cmake
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
