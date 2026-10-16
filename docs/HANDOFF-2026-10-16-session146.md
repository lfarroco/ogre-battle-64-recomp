# Session 146 handoff — the browser presenter stretched the VI's visible area over the whole canvas, which moved the right and bottom halves of every frame by one pixel

## Goal

Continue the web renderer (developer, session start). Session 145's open item 1
was the residual difference between the browser's canvas and RT64's
`OGRE_CAPTURE_TARGET` for the same title frame, which it measured as a 5.1/255
mean absolute difference per channel and described as "a small overall
darkening, not a channel order".

## Result

1. **The browser's present path drew the visible area over the whole canvas.**
   `blit_to_canvas` built a full-canvas quad and interpolated the texture's
   `[0, u_max] x [v_min, 1]` across it, so the visible 319x239 region (the VI's
   `fbSize` minus the overscan crop) was stretched onto a 320x240 canvas. That is
   a 320/319 horizontal and 240/239 vertical resample: canvas pixel `X` sampled
   framebuffer texel `int(0.996875 * (X + 0.5))`, which is `X` for `X <= 160` and
   `X - 1` above it, and the same vertically at `Y > 120`.
2. **The old build's own measurements identify it.** Against the same RT64
   target frame, the session-145 build's canvas was *closer* to the target passed
   through that stretch (2.30/2.50/3.11) than to the target itself
   (4.66/5.05/5.54); on the fixed build the relation is reversed
   (2.29/2.48/3.10 against 4.56/4.94/5.47).
3. **RT64 draws the visible area into a viewport anchored at the window's
   top-left.** `VIRenderer::getViewportAndScissor`
   (`tools/RT64/src/render/rt64_vi_renderer.cpp`) builds `viViewRect` from
   `visibleFramebufferSize(vi)` at `(0,0)` and maps it through `fromSDtoHD` /
   `fromHDtoWindow`; with the window equal to the VI's size the viewport is
   `[0, visible)` and the last column and row of the window are never written
   (`removeBlackBorders` shrinks the window to the visible size instead). The
   canvas is the VI's own size, so the same fraction of it is written now.
4. **The RDP scissor is half-open, and the browser's was not.** `glScissor` used
   `w = sx1 - sx0 + 1` / `h = sy1 - sy0 + 1`. RT64 converts the scissor with
   `convertFixedRect` into `RenderRect(left, top, right, bottom)`
   (`FixedRect::right/bottom` ceil the 1/4-pixel bounds into pixels,
   `tools/RT64/src/common/rt64_common.cpp`), and plume's `RenderRect` is a
   half-open rectangle (`VkRect2D{ offset, extent{right-left, bottom-top} }`), so
   the game's scissor of `(0,0)-(319,239)` covers pixels 0..318 x 0..238. RT64's
   own target shows it: column 319 and row 239 of the captured render target are
   black on every title frame while the browser had drawn them.
5. **Measured after both changes, on the same title screen and the same RT64
   target frame (`t.160`):** mean absolute difference per channel
   `4.66/5.05/5.54 -> 2.29/2.48/3.10`, and the fraction of channel samples that
   match exactly `39.4% -> 81.4%`. The last column and row now match RT64's black
   (browser `0.2/0.2/0.2` and `0.1/0.1/0.2` mean against `0.0`).
6. **What is left is two cloud bands, and it is not established as a renderer
   fault.** The remaining difference is two horizontal bands of moving cloud
   (rows ~64..112 and ~195..238) where the browser is 12-16 darker; every other
   block of the frame is `0.0`. The developer stated in this session that the
   port's **moving clouds were hard to implement and are still not 100% correct
   against retail**, so a cross-run difference in those bands cannot be
   attributed to the browser renderer from this comparison; the two runs are not
   phase-locked on a moving layer.

## Evidence

### 1. The present path, before and after

`app/src/web_renderer.cpp`, `blit_to_canvas` before:

```cpp
const float quad[6 * 8] = {
    -1, -1, 0,     v_min, 1, 1, 1, 1,
     1, -1, u_max, v_min, 1, 1, 1, 1,
     1,  1, u_max, 1,     1, 1, 1, 1,
    ...
```

after (`x0 = -1`, `x1 = -1 + 2*frac_w`, `yt = 1`, `yb = 1 - 2*frac_h`):

```cpp
const float quad[6 * 8] = {
    x0, yb, 0,     v_min, 1, 1, 1, 1,
    x1, yb, u_max, v_min, 1, 1, 1, 1,
    x1, yt, u_max, 1,     1, 1, 1, 1,
    ...
```

`u_max` and `v_min` still bound the texture region (the framebuffer can be taller
than the VI, so the sample is cropped and not squashed); `frac_w`/`frac_h` are
`visible.width / canvas_width` and `visible.height / canvas_height`. Canvas pixel
`X` now has `u = (X + 0.5) / canvas_width`, so it samples framebuffer texel `X`,
and the same for `Y`.

### 2. The measurement, with `tools/web_frame_cmp.py` (new)

The canvas is a `#game-canvas` screenshot: the element's inline CSS size is
640x480 and `emscripten_set_canvas_element_size` sets only the drawing buffer, so
a 320x240 VI mode is captured as each guest pixel duplicated 2x2 inside a 1 px
border. The tool crops the border, undoes the duplication (76787/76800 pixels
duplicate on the fixed frame; the 13 that do not are the canvas's antialiased
left edge), and compares against `OGRE_CAPTURE_TARGET` `.bin` files.

| frame | vs RT64 target `t.160` (mean abs R,G,B) | exact channel samples | vs the same target through the old present |
|---|---|---|---|
| session-145 build (`web-title-present-stretch-before.png`) | `4.66 5.05 5.54` | `39.4%` | `2.30 2.50 3.11` |
| this session (`web-title-present-1to1-fix.png`) | `2.29 2.48 3.10` | `81.4%` | `4.56 4.94 5.47` |

The two frames are static screens: within the session-145 run
`web-title-a1-00` against `-01` is `0.03` mean abs, and within the native run
`t.100` against `t.160` is `0.87`. The fixed build's frame is
`docs/proofs/web-title-present-1to1-fix.png`; RT64's target for the same screen
is `docs/proofs/native-title-present-reference.png`. The amplified difference
images are `docs/proofs/web-title-present-diff-stretch-before.png` (clouds plus
the edge column and row) and `-diff-1to1-after.png` (only the two cloud bands).

The remaining difference by 32x32 block on the fixed frame (mean abs over the
three channels):

```
 0.0  0.0  0.0  0.0  0.0  0.0  0.0  0.0  0.0  0.0
 0.0  0.0  0.0  0.0  0.0  0.0  0.0  0.1  0.0  0.0
 2.5  2.8  4.6  4.2  4.5  4.1  4.0  2.8  3.4  0.3
 0.5  3.8  5.0 15.1  9.6 13.9  8.6  4.5  2.2  0.1
 0.1  0.2  0.1  0.0  0.0  0.0  0.0  0.0  0.0  0.0
 0.0  0.0  0.1  0.1  0.0  0.0  0.0  0.0  0.0  0.0
 0.0  0.0  0.0  0.0  1.7  8.7 18.9 16.0 18.5 17.9
 0.1  0.0  0.0  0.0  0.0  1.0  8.3  8.8  6.2 11.2
```

### 3. The scissor

The game's display lists set one scissor, `[GFX-RECT] ... scissor=1(0,0,319,239)`.
`st.scissor_x1 = p0(w1,12,12) >> 2` is the lower-right corner in pixels, and
`glScissor` was called with `w = sx1 - sx0 + 1` (and `h`, `y` likewise), one
pixel wider and taller than the RDP's rectangle. RT64's target for the title
frame has column 319 and row 239 black on every captured present (`t.100` through
`t.1000`); after the change the browser's canvas matches them.

### 4. What the fix does not change

The scissor change does not alter the visible frame on its own: the presenter no
longer draws the last column and row at all. It changes the GL framebuffer's
content at that edge, which is what `readback_framebuffer` writes back to RDRAM
and what the game reads back.

## Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web` | clean, twice (once per change) |
| title frames on the fixed build | `node /tmp/w146bright.cjs --query 'OGRE_SCENE=title&OGRE_SCENE_LOG=1' --out /tmp/w146/fixb --attempts 4 --secs 50 --shots 6` | 6 frames, `[scene] forced scene 0x04 is active`, `[GFX-PRESENT] VI scanout 0x025E80 -> fb 0x025C00 ... visible=319x239 canvas=320x240` |
| the comparison itself | `python3 tools/web_frame_cmp.py docs/proofs/web-title-present-1to1-fix.png /tmp/w146/nt/t.160.bin` | `meanAbs=[2.29, 2.48, 3.1] exact=81.4%` against `[4.66, 5.05, 5.54] / 39.4%` for the session-145 frame |
| RT64's reference frames | `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SCENE=title OGRE_CAPTURE_TARGET=/tmp/w146/nt/t OGRE_CAPTURE_AFTER=60 OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64 assets/ogre64.z64` | 68 targets, `t.160` used |
| boot regression | `node debug/probes/boot.cjs --attempts 4 --secs 70 --out /tmp/w146/fix-boot` | `RENDERED nonBlack 238157 / colorful 20332` (a fade-in frame; sessions 143-145 report frames from the same probe between `24840` and `239425` depending on which frame of the fade it samples) |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000 --out /tmp/w146/fix-testdraw` | `exported=true returned=1`, `colorful 49384` |
| the 640-wide mode | `node /tmp/w146bright.cjs --query 'OGRE_SCENE=publishers' --out /tmp/w146/fix-pub --shots 3 --min-colorful 20000` | ATLUS still renders, `[GFX-PRESENT] ... size=640x481 visible=639x475 canvas=640x476`, `docs/proofs/web-publishers-atlus-present-fix.png` |
| the publishers, native, 1x | `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=publishers OGRE_CAPTURE_TARGET=/tmp/w146/nat/tt OGRE_CAPTURE_AFTER=60 OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=9000 ./build-app/ogrebattle64 assets/ogre64.z64` | `tt.140..240`: the still's letters are `(48,80,160)` in the render target |
| the publishers, native, speed | the same run without `OGRE_SPEED=1` | `[settings] game speed x4` (the saved setting), so every capture above passes `OGRE_SPEED=1` |
| the publishers, native trace | `... OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_DUMP=/tmp/w146/natdump ...` | `[tex] dl=69 n=0 TEXRECT rect=0,0..640,12 ... timg=0x8038F6C8 fmt=2 siz=1`; before the `colour_mean` change `OGRE_TEX_TRACE_COLOUR=1` printed no `colour:` line at all |
| the ATLUS still, native trace | `... OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1 OGRE_TEX_TRACE_AFTER_MS=5000 OGRE_TEX_TRACE_MS=250 ...` (arming sweep 1500..6000 ms; only 5000 reached the still) | `[tex] dl=144 n=0 ... timg=0x8037C898 fmt=2 siz=1` `colour: mean 225.4/255 over 321x7 texels, opaque 2247`, 40 strips at `224.4..225.4/255` — cream, matching the browser's `mean=224.9` for the same image |
| native rebuild | `cmake --build build-app -j` | clean (warnings only from the Homebrew SDL2 compatibility dylib) |

The 640-wide run also shows the present geometry on a second mode: the canvas is
the VI's 640x476, so the visible 639x475 is written 1:1 and the last column and
row stay as the resize left them.

## The ATLUS logo's letters render blue in the native build, and black in the browser

The developer reported, while this session's native runs were on screen, that the
publisher still's ATLUS logo renders with its "A" and "LUS" characters blue. The
two renderers were measured on the same still (forced scene `0x0A`, `OGRE_SPEED=1`
on the native side):

| | the still's letters | where it was measured |
|---|---|---|
| native, RT64 | `(48,80,160)`, 2459 of every 2x2-sampled pixels | `OGRE_CAPTURE_TARGET` of presents 140..240 (`/tmp/w146/nat/tt.140.bin`), so the colour is in the **RDP draw** and not in the present path |
| browser | `(56,56,56)` | `#game-canvas` at the same screen (`docs/proofs/web-publishers-atlus-present-fix.png`) |

The two runs are on the same screen, and the browser's frame is otherwise the
same picture (cream background, grey drop shadow, red T). The draw is a 640-wide
CI8 image: `img=0x37C898 fmt=2 siz=1 stride=320 line40 tile=0 pal0`, drawn as
12-row `G_TEXRECT` strips at `dsdx=dtdy=512` (2x), with `lut=2` (`G_TT_RGBA16`)
and a 256-entry palette that the browser's LOADTLUT trace reports as
`loadtlut t7 addr=0x3A22C8 count=256 bank=0`.

**RT64's own decode of that draw is cream, so the fault is in its palette
lookup and not in the data.** `colour_mean` (`app/src/renderer.cpp`) refused CI8
because its guard was `timg_siz != 2`, and CI8 is `siz = 1`; that guard now admits
`siz = 1` for the indexed formats (CI4, `siz = 0`, is still refused). With it,
one native run at `OGRE_SPEED=1 OGRE_SCENE=publishers OGRE_TEX_TRACE=1
OGRE_TEX_TRACE_COLOUR=1 OGRE_TEX_TRACE_AFTER_MS=5000 OGRE_TEX_TRACE_MS=250` traces
the still itself and reports, for its 40 strips:

```
[tex] dl=144 n=0 TEXRECT rect=0,0..640,12 px=640x12 s=0 t=0 dsdx=512 dtdy=512
      window=uls0 ult0 lrs1276 lrt20 tile=line40 pal0 timg=0x8037C898 fmt=2 siz=1 imgw=320 texels=321x7
[tex]   colour: mean 225.4/255 over 321x7 texels, opaque 2247
```

`mean 224.4..225.4/255` is the cream, and it matches the browser's own decode of
the same image (`[GFX-TEXTRACE] ... img=0x37C898 ... mean=224.9 opaque=1920`).
So the source bytes and the palette RT64 reads from RDRAM (`tlut_addr`) decode to
the correct picture in RT64's own decoder, while RT64's rendered frame has blue
letters. The remaining difference is therefore between the palette that decoder
reads from RDRAM and the palette the *shader* samples out of TMEM:

- RT64 writes the TLUT with `tmemStart = loadTile.tmem << 3`
  (`RDP::loadTLUTOperation`, `tools/RT64/src/hle/rt64_rdp.cpp:755`) and the CI
  sampler always reads `RDP_TMEM_PALETTE + (palette << 7) + index * 8`
  (`TextureDecoder.hlsli:255`), i.e. TMEM `0x800` plus the draw tile's palette
  field. A `G_LOADTLUT` whose tile descriptor has `tmem != 0x100` therefore
  leaves the palette outside the region the sampler reads, and the sampler takes
  whatever an earlier still left there.
- The browser has no TMEM: it keeps one flat 256-entry table filled from RDRAM
  (`bank = tile.palette * 16`) and indexes it by the texel byte, so it always
  reads the palette the game last loaded.

The three `G_SETTILE` patterns for tile 7 in a browser RDRAM dump are
`(fmt=0, siz=0, line=0, tmem=256, pal=0)`, which is the palette-tile convention,
and `(fmt=2, siz=1, line=40, tmem=0, pal=0)` ×80, which is tile 7 used as a CI8
image tile; the *order* of those two against the `G_LOADTLUT` is what decides
whether the palette lands at `0x800`, and that order was not extracted. The next
datum is therefore one line of RT64 diagnostic — the load tile's `tmem` and the
16 TMEM bytes at `RDP_TMEM_PALETTE` at the draw — or the same order read out of a
display-list dump.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `blit_to_canvas` takes `frac_w`/`frac_h` and builds the destination rectangle from them; `present_framebuffer` computes them from `visible_framebuffer_size(vi)` over the canvas and passes them to both present paths; `[GFX-PRESENT]` prints `canvas=WxH`; the GL scissor uses the half-open pixel bounds |
| `tools/web_frame_cmp.py` | new; crops a `#game-canvas` screenshot's border, undoes the 2x duplication, and reports mean abs / mean signed / exact per channel against one or more `OGRE_CAPTURE_TARGET` `.bin` files, with `--diff-out` writing an amplified difference PNG |
| `app/src/renderer.cpp` | `colour_mean`'s guard is now `indexed ? (timg_siz != 1) : (timg_siz != 2)`, so `OGRE_TEX_TRACE_COLOUR` decodes CI8 through the loaded TLUT (the format of the publisher stills, the title's clouds and the njpeg output) instead of reporting nothing; CI4 stays refused |
| `docs/proofs/web-title-present-1to1-fix.png` | new; the title on the fixed build |
| `docs/proofs/web-title-present-stretch-before.png` | new; the same screen on the session-145 build |
| `docs/proofs/native-title-present-reference.png` | new; RT64's target for the same frame (`t.160`) |
| `docs/proofs/web-title-present-diff-stretch-before.png`, `-diff-1to1-after.png` | new; the amplified differences |
| `docs/proofs/web-publishers-atlus-present-fix.png` | new; the 640x476 publishers mode on the fixed build |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session146.md` | this file |

`tools/RT64` is dirty with the port's own renderer changes from earlier sessions
(27 files, session 50 onward); nothing in it was touched this session.

## Probes

- `/tmp/w146title.cjs` (throwaway): boots the wasm build at a forced scene with
  any query string and writes canvas frames plus the page log.
- `/tmp/w146bright.cjs` (throwaway): the same, but keeps only frames whose canvas
  is bright enough (`--min-colorful`, default 100000), so a run that lands on the
  title's fade-in is not compared against a settled native frame; it prints each
  attempt's `[scene]` lines.
- `/tmp/w146both.cjs`, `/tmp/w146both2.cjs` (throwaway): capture the canvas at
  the same moment as an `OGRE_RAM_KEY_FULL` dump, so a frame and the guest bytes
  it was drawn from can be compared without cross-run timing. The second retries
  until a real display list is reached. Both were used to establish that the
  title screen's cloud images are re-drawn between runs, which is why the
  cross-run comparison cannot settle the cloud bands.
- `/tmp/w146rapid.cjs` (throwaway): samples the canvas every `--interval` ms
  (400 ms was tried) to catch a short-lived state. Each sample costs a screenshot
  plus a decode round-trip in headless Chromium, so the effective rate was about
  2 s per frame and the probe added nothing over `w146bright.cjs`.
- `/tmp/w146cmp.py`, `/tmp/w146struct.py`, `/tmp/w146diffimg.py`,
  `/tmp/w146srccmp.py`, `/tmp/w146check.py` (throwaway): the successive forms of
  the comparison, replaced by `tools/web_frame_cmp.py`, which stays.
- `tools/web_frame_cmp.py` is not a probe: it reads a documented diagnostic
  format on both sides (`#game-canvas` screenshots and `OGRE_CAPTURE_TARGET`) and
  is the comparator `tools/rt64_target.py` needs.
- No probe is left in the tree; no diagnostic was removed.

## Open

1. **The ATLUS still's letters are blue in the native build** (section above).
   That is a native RT64 fault and not a browser one: RT64's render target has
   `(48,80,160)` for the strokes while the browser draws `(56,56,56)`, and
   RT64's own colour decode of the same draw's source and palette is cream. The
   next datum is the load tile's `tmem` at that `G_LOADTLUT` and the TMEM bytes at
   `RDP_TMEM_PALETTE` when the draw samples them, because the sampler reads a
   fixed TMEM `0x800` while the load writes at `loadTile.tmem << 3`.
   **Corrected by session 147:** the native stroke colour is `(57,90,165)`, not
   `(48,80,160)`; RT64's blue render is correct; and the fault was the browser's,
   which chose a palette entry's decoder from the entry's own bit 15 instead of
   the draw's 2-bit LUT mode. See
   `docs/HANDOFF-2026-10-16-session147.md`.
2. **The two cloud bands** (rows ~64..112 and ~195..238, browser 12-16 darker)
   are the whole of the remaining difference on the title screen. The developer
   states the port's moving clouds are not exact against retail yet, so the next
   step is not another renderer change on this screen: it is a screen without a
   moving layer (the publishers' stills, or the cathedral) compared at a matched
   phase, which separates "the browser blends a static layer differently" from
   "the two runs are at different phases of a moving one". The publisher stills
   are the right screen for it and are now known to differ for a second reason
   (item 1).
3. **The native run's saved game speed is x4 and the browser is x1.**
   `build-app` prints `[settings] game speed x4 ...` and only `OGRE_SPEED=1`
   overrides it; every native capture this session used `OGRE_SPEED=1` after the
   developer pointed it out. A comparison that does not set it compares a 4x run
   with a 1x one. `play-session.sh`-style launch scripts should carry the knob.
4. **The canvas's CSS size does not follow the drawing buffer.**
   `app/web/index.html` pins `width:640px; height:480px` on `#game-canvas` and
   `emscripten_set_canvas_element_size` sets only the buffer, so the publishers'
   and world map's 640x476 VI mode is presented through a 640x480 CSS box (a
   476 -> 480 vertical stretch) and a 320x240 mode is a 2x upscale. Session 143
   relies on the 2x for the title; the 640x476 case has a 0.8% vertical stretch
   that no measurement has looked at.
5. **Framebuffer-as-texture** stands: a `G_LOADTILE` whose source is a rendered
   colour image inside the same task reads RDRAM before the writeback. The
   browser's version is a staleness race rather than the native renderer's
   systematic gap.
6. Session 141/142's other items stand: the VI's own view rectangle is now the
   part that this session's geometry covers (top-left, visible size); the x/y
   scale and offset of `fldRegs` are still not applied. The deployed site has
   only been exercised headless, the first load fetches the whole 21 MB wasm
   before a ROM is picked, the browser build assigns gamepads to N64 controllers
   2-4, and `debug/probes/rdram.cjs` reads one dump per run.
7. Session 141's `divide by zero` at task 277 in the publishers is still
   reachable on a route that reaches scene `0x000A`.
8. The native's forced `OGRE_SCENE=publishers` lands in scene `0x0009`
   (`descriptor 0x8018FB70`) in some runs and in `0x000A`
   (`0x8018FB84`) in others, so two "publishers" runs can show different screens;
   the browser's forced entry shows `id=0x000A` at 3.5 s. A comparison that
   assumes both are on the same scene has to read the `[scene]` line.
