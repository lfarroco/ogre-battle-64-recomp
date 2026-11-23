# Session 144 handoff — a CI tile with texture LUT mode 0 is I8, so the browser's black rectangle was an empty palette

> **Summary.** Session 144 handoff — **a CI tile whose RDP texture LUT mode is 0 is I8, not palette-indexed, and the browser was painting it opaque black.** Continue the web renderer from session 143's open item 2 ("CI8 tiles decode pure black in display lists that load no palette"). `app/src/web_renderer.cpp`'s `decode_texture_rect` read every `G_IM_FMT_CI` tile through its TLUT and wrote `{0,0,0,255}` when no `G_LOADTLUT` had filled one. `OTHERMODE_H` bits 14-15 are the texture LUT mode (RT64's `OtherMode::textLUT()`), and RT64 treats LUT mode 0 as "not a palette at all": `rt64_framebuffer_manager.cpp`'s `(dstFmt == G_IM_FMT_CI) && (dstSiz == G_IM_SIZ_8b) && (tlutFormat == 0)` case runs `ANY8toI8` in `FbReinterpretCS.hlsl`, so each byte is an intensity replicated into RGB and alpha. The title screen's prologue draws its two unit sprites (`rect=22,127..94,155` and `22,155..94,175`, `img=0x1C13F8 fmt=2 siz=1 line=9 pal=0`, `oml=0x504240` so `lut=0`) through that path, and they were the black rectangle over the scene; the browser trace's mean for the first moved from `0.0` to `72.2` (`opaque 2016 -> 597`) and the frame matches RT64's silhouette (`docs/proofs/web-title-lut-mode0-i8-fix.png`). The decode takes the LUT mode as `palette_enabled`, and `G_LOADTLUT` writes to renderer state rather than to the per-display-list `RenderState`, because RT64 clears `RDP::TMEM` in `RDP::RDP` only and `RDP::reset()` leaves it, so the RDP's TMEM palette outlives a display list. The sprites do not need that part: they trace `tlut=no` and are read as I8. A CI image's cache key now carries the palette generation and the LUT mode. New browser diagnostic `OGRE_TEX_TRACE` / `[GFX-TEXTRACE]` (bounded at 1500 texture rectangles) prints what each rectangle decodes, which is what made the line-by-line comparison against RT64's `OGRE_TEX_TRACE` possible. Verified: `make web` and `make app` clean, `boot.cjs` `RENDERED nonBlack 25644 / colorful 18568`, `testdraw.cjs` `returned=1`, and a 250 s natural run that reaches the publishers and then reproduces session 141's pre-existing `divide by zero` at task 277 (`func_801989AC`), which session 143's run passed only because it took a different route. Open: the title/story hue shift against RT64 is not the decode (the same rect, window, format and UV arithmetic decode to a plausible cloud in the browser and to a different canvas colour), so it is in the GL sampling or the blend; and session 141/142's items stand.

## Goal

Continue the web renderer (developer, session start). Session 143's open item 1
was the browser's title and story frames being hue-shifted against RT64's target
for the same text; item 2 was "CI8 tiles decode pure black in display lists that
load no palette". Items 3 onwards were session 141/142's list.

## Result

1. **The black rectangle over the title screen is two unit sprites, and the
   browser painted them opaque black because it decoded a CI tile through an
   empty palette.** A CI tile does not always read a palette. RT64 keys that on
   the RDP's texture LUT mode, and it reinterprets the one case the game uses as
   I8. The browser read every `G_IM_FMT_CI` tile through its TLUT, so a tile
   whose list loaded no palette decoded to `{0,0,0,255}`.
2. **`decode_texture_rect` takes the LUT mode now** (`palette_enabled`, from
   `OTHERMODE_H` bits 14-15, RT64's `OtherMode::textLUT()`), and a CI tile with
   LUT mode 0 decodes as I8: one byte per texel, replicated into RGB and into
   alpha. The browser trace's mean for the sprite moved from `0.0` to `72.2`
   (`opaque 2016 -> 597`), and the frame's silhouette matches RT64's
   (`docs/proofs/web-title-lut-mode0-i8-fix.png`).
3. **The TMEM palette moved out of the per-display-list `RenderState`.** RT64
   holds one `RDP::TMEM` for the whole run; `RDP::reset()` does not touch it.
   The browser built a fresh `RenderState` per display list, so a palette loaded
   in one list was gone in the next. The two sprites of item 1 do not need this
   (they trace `tlut=no` and are read as I8), so the change is the state model
   matching RT64's and not a fix for a measured screen.
4. **A CI image's cached decode now depends on the palette generation and on the
   LUT mode**, so both are part of the image key.
5. **The browser has a texture trace comparable with the native one.**
   `OGRE_TEX_TRACE=1` prints one bounded `[GFX-TEXTRACE]` line per `G_TEXRECT`
   with the sampled rect, the window, the tile's format/size/line/palette, the
   source address, whether a TLUT was in force, the decoded size, the UV corners
   and the mean RGB of the decoded image. That line is what showed the black
   rectangle's mechanism and what shows the remaining colour difference.
6. **The hue shift of item 1 is not the decode.** The browser and RT64 decode the
   same rectangle from the same address, and the browser's decode of it is a
   plausible RGBA32 cloud; the canvas colour at that row is a different one.
   Section 4 has the comparison.

## Evidence

### 1. The black rectangle, before and after

The browser trace for the title scene (`OGRE_SCENE=title OGRE_TEX_TRACE=1`, the
build before the fix):

```
[GFX-TEXTRACE] n=110 dl=2 rect=22,127..94,155 px=72x28 s=0 t=0 dsdx=1024 dtdy=1024
  window=uls0 ult0 lrs284 lrt108 tile=0 line9 pal0 img=0x1C13F8 fmt=2 siz=1
  stride=72 tlut=no ntlut=0 texels=73x29 dec=72x28 mean=0.0 opaque=2016
[GFX-TEXTRACE] n=111 dl=2 rect=22,155..94,175 px=72x20 s=0 t=896 dsdx=1024 dtdy=1024
  window=uls0 ult112 lrs284 lrt188 tile=0 line9 pal0 img=0x1C13F8 fmt=2 siz=1
  stride=72 tlut=no ntlut=0 texels=73x21 dec=72x20 mean=0.0 opaque=1440
```

`mean=0.0` with `opaque` equal to the whole window is the empty-palette decode:
`decode_texture_rect` left every texel at its initial `{0,0,0,255}`. In the frame,
the two rectangles tile one black box: canvas `x 40..190`, `y 250..355`, i.e.
game `x 22..94`, `y 127..175`, which is exactly their union. The last `[GFX-TEXTRACE]`
of the whole run is `tlut=no` for all 24 CI draws, and the run's summary line says
`0 G_LOADTLUT(s) seen` for the 1500 traced draws, so no palette was available to
them.

The same rectangle on the native renderer (`OGRE_SCENE=title OGRE_TEX_TRACE=1`):

```
[tex] dl=2 n=565 TEXRECT rect=22,127..94,155 px=72x28 s=0 t=0 dsdx=1024 dtdy=1024
  window=uls0 ult0 lrs284 lrt108 tile=line9 pal0 timg=0x801C13F8 fmt=2 siz=1 imgw=72 texels=73x29
[tex] dl=2 n=566 TEXRECT rect=22,155..94,175 px=72x20 s=0 t=896 dsdx=1024 dtdy=1024
  window=uls0 ult112 lrs284 lrt188 tile=line9 pal0 timg=0x801C13F8 fmt=2 siz=1 imgw=72 texels=73x21
```

The two lists name the same rectangles, the same image, the same window and the
same format. RT64 draws them as the dark silhouette in
`/tmp/nt144d/t.400-noflip.png` (the native render target, 320x240), and the
browser's pre-fix frame draws a black box in the same place. RT64's trace prints
no `colour:` line for either rectangle, because `colour_mean` requires `tlut_seen`
when the format is CI.

After the fix, from the final build:

```
[GFX-TEXTRACE] n=110 dl=2 rect=22,127..94,155 px=72x28 s=0 t=0 dsdx=1024 dtdy=1024
  window=uls0 ult0 lrs284 lrt108 tile=0 line9 pal0 img=0x1C13F8 fmt=2 siz=1
  stride=72 tlut=no ntlut=0 texels=73x29 dec=72x28 uv0=(0.000,0.000)
  uv3=(72.000,28.000) mean=72.2 opaque=597 oml=0x504240 lut=0
```

### 2. Why LUT mode 0 means I8

`OTHERMODE_H` bits 14-15 are `G_MDSFT_TEXTLUT` (RT64
`src/shared/rt64_other_mode.h` `OtherMode::textLUT()`, `H & (3U <<
G_MDSFT_TEXTLUT)`). RT64 uses it two ways:

- `State::loadDrawState` (`src/hle/rt64_state.cpp`) sets
  `dstCallTile.tlut = rdp->otherMode.textLUT()` and requires
  `usesTLUT = textLUT() > 0` before looking up a tile copy for a palette.
- `FramebufferManager::makeTileReinterpretation` asserts the supported cases,
  and one of them is `(srcSiz == G_IM_SIZ_8b) && ((dstFmt == G_IM_FMT_CI) ||
  (dstFmt == G_IM_FMT_I)) && (dstSiz == G_IM_SIZ_8b) && (tlutFormat == 0)`
  (`src/hle/rt64_framebuffer_manager.cpp`). `FbReinterpretCS.hlsl` runs
  `ANY8toI8` for it: the byte is the intensity, and `I8ToFloat4` puts it in RGB
  and in alpha.

The browser's textures are loaded from RDRAM rather than reinterpreted out of
TMEM, so the equivalent is to read the same bytes the same way, which is what
the new `palette_enabled` argument does.

Neither the rectangle nor the tile carries the LUT mode; the mode is in
`RenderState::othermode_h`, decoded at `gbi::OP_RDPSETOTHERMODE` /
`OP_SETOTHERMODE_H`.

### 3. The palette outlives a display list

RT64 constructs one `RDP` for the run (`State::rdp`) and its constructor is the
only place that clears `TMEM` (`RDP::RDP`, `rt64_rdp.cpp`); `RDP::reset()` does
not touch it. A `G_LOADTLUT` therefore stays in force for later display lists.
The browser's `RenderState` is per display list (session 139 carried only the
colour image over), so its `tlut[256]` was discarded at the end of every list.

The traced window of the title scene contains one `G_LOADTLUT`, and it is not in
the display list of these two rectangles: the trace's own line for them reads
`tlut=no ntlut=0`, and it printed that for all 24 CI draws of the run. They are
read as I8 now, so this change is not what fixes them; it makes the palette live
where RT64's does. `tlut_`, `tlut_valid_`, `tlut_count_` and `tlut_generation_`
are renderer members now, the `G_LOADTLUT` handler writes them,
`decode_texture_rect` reads them, and a CI image's key is
`tlut_generation_ * 2 + palette_enabled`. A screen that loads a palette in one
display list and draws with it in the next would be the test for it, and none was
found this session, so it is unverified.

### 4. The remaining colour difference is not the decode

Item 1's subject is the sky, not the sprites. The title's backdrop is drawn as
3-pixel-tall strips over one 320x240 RGBA32 image, and both traces name the same
first strip:

| | browser | RT64 |
|---|---|---|
| trace line | `n=64 dl=2 rect=0,101..320,104` | `dl=2 n=70 TEXRECT rect=0,101..320,104` |
| window | `uls0 ult84 lrs1276 lrt92` | `uls0 ult84 lrs1276 lrt92` |
| image | `img=0x217508` (`0x80217508` native) | `timg=0x80217508` |
| format | `fmt=0 siz=3 stride=1280` | `fmt=0 siz=3 line=80` |
| uv | `uv0=(0,0) uv3=(320,3)` | `s=0 t=672 dsdx=1024 dtdy=1024`, so `uls/32=0` and `t/32=21` |

Both are the same texel window of the same image, and RT64's own
`OGRE_RECT_STATE=101-104` prints for it `rect 0,404..1280,416 cycle=0
ccL=FCFFFFFF ccH=FFFCF279 ... texOn=1 tile=0 fmt=0 siz=3 line=80 tmem=0 uls=0
ult=84 lrs=1276 lrt=92` with `cc0 rgb (ZERO - ZERO) * ZERO + TEXEL0` and the same
for alpha, so the draw's colour is `TEXEL0` unmodified.

The browser's decode of that window is a smooth magenta-to-white gradient
(`[GFX-TEXTRACE]`'s per-texel dump of an earlier build printed
`754BFF 9171FF 9F82FF A38AFF ...`, and `src 83 75 4B FF ...` shows the source
bytes decode to exactly that). The canvas at that row is not that colour: the
browser's own canvas reads `(91,46,105)` at game `(100,101)` and
`(141,109,61)` at the same row a little to the right, while RT64's target for the
same row reads `(115,102,74)` and `(151,136,92)`. The two frames' content is the
same (the same castle, the same text, the same cloud shapes), so this is a
per-pixel colour difference and not a different screen.

What that rules out: the u/v arithmetic for this rectangle is identical on both
sides (`u2 - u1 = dsdx * rectWidth / 1024 = 512 * 640 / 1024`), the source bytes
are the same bytes, and the browser's decode of them is a plausible image. What
it does not rule out: the GL texture the fragment shader actually samples, the
sampler's wrap mode for a UV just past 1.0, and the blender. The next step is to
print `[GFX-UV]`'s keys and scales for these particular strips (the existing
diagnostic is bounded at four draws and stops on the first six-pixel rect) and to
check that `gl_textures_` holds the image the decode produced.

### 5. Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web` | clean, after every source change; the final run builds the trimmed source |
| native build | `make app` | clean (the only change there, a trace added and reverted, is not in the tree) |
| forced title, traced | `node /tmp/web144story.cjs --query 'OGRE_SCENE=title&OGRE_TEX_TRACE=1'` | `[scene] t=13486ms id=0x0004`; sprite `mean=72.2`, `lut=0`, `tlut=no` |
| boot regression | `node debug/probes/boot.cjs --attempts 4 --secs 70 --out web144-boot-final2` | `RENDERED nonBlack 25644 / colorful 18568` (sessions 138-143: 23656-25644 / 15076-19688) |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000` | `test_draw exported= true returned= 1` |
| publishers after the fix | `node /tmp/web144px.cjs --query 'OGRE_SCENE=publishers'` | the ATLUS still renders (`/tmp/web144pub-002.png`); no scene-wide change from the I8 rule |
| decoded tile images | `node debug/probes/textures.cjs --attempts 2 --secs 60` | dumps the last 24 decoded images and their colour x mask composites |
| natural route, 250 s | `node /tmp/web143scene.cjs --secs 250 --out /tmp/web144nat` | page error at wall 215 s: `divide by zero` in `func_801989AC` at task 277, in scene `0x000A` |
| native reference frames | `OGRE_SCENE=title OGRE_CAPTURE_TARGET=/tmp/nt144d/t OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=100` | `t.300`..`t.600`, 320x240, used for the row and crop comparisons |
| native rect state | `OGRE_RECT_STATE=101-104` | `[rect-state]` for the sky strip, quoted in §4 |

The natural-route fault is session 141's pre-existing wall, not this session's
change: the same `func_801989AC <- func_80198ECC <- func_801A29B8 <-
func_801A40EC <- func_80177EC0 <- func_80072398 <- func_8008AFE0` stack, the
same task counter 277, and it is reached in scene `0x000A` before any title
sprite is drawn. Session 143's 250 s run reached the attract story instead and
did not hit it, so the route and not the code decides whether it appears. It is
open work item 8's existing entry.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `decode_texture_rect`'s `palette_enabled` argument and the I8 rule for a CI tile with LUT mode 0; the palette moved from `RenderState` to renderer members (`tlut_`, `tlut_valid_`, `tlut_count_`, `tlut_generation_`); `LoadedImage::palette` and the cache-key change; `OGRE_TEX_TRACE` / `[GFX-TEXTRACE]` |
| `docs/proofs/web-title-lut-mode0-i8-fix.png` | new; the title prologue on the final build, with the silhouette where the black rectangle was |
| `PLAN.md` | browser section and open work item 8 |
| `docs/DECISIONS.md` | the session-144 row |
| `docs/STATUS-LOG.md`, `docs/README.md` | log entry, index |
| `docs/HANDOFF-2026-10-14-session144.md` | this file |

## Probes

- `/tmp/web144story.cjs` (throwaway): boots the wasm build to a forced scene with
  any query string, captures the canvas and the page log. `/tmp/web144px.cjs`
  also samples canvas rows into text. `/tmp/pngread.py` and `/tmp/target2png16.py`
  read the canvas PNGs and RT64's `R16G16B16A16_UNORM` captures offline.
- **`OGRE_TEX_TRACE` on the browser stays.** The native renderer has had the same
  knob since session 131, and the I8 rule was found by comparing the two lines.
  It is bounded at 1500 texture rectangles and its per-texel byte dump was
  removed after it had answered the question, so what remains is one line per
  rectangle plus up to eight `loadtlut` lines.
- **A trace was added to `app/src/renderer.cpp` and reverted.** It would have
  printed the decoded RGB of the texel a rectangle samples, to compare the two
  renderers' decodes directly. It never printed (the native `colour_mean` guard
  refuses `siz=3`, and the block that would have run instead did not), so it was
  reverted rather than shipped unexercised: `git checkout app/src/renderer.cpp`,
  and `git status --short` shows no change to that file.
- No probe is left in the tree; no diagnostic was removed.

## Open

1. **The browser's title and story frames are hue-shifted against RT64's target
   for the same text.** §4 has the comparison for one sky strip: the same image,
   window, format and UV arithmetic on both sides, a plausible decode in the
   browser, and a different canvas colour. The mechanism is in the GL sampling or
   the blend, not in the decode. The existing `[GFX-UV]` diagnostic is bounded at
   four draws and lands on the first six-pixel rectangles; extending it (or the
   trace) to the strips at rows 80..140 is the next step.
2. Session 141/142's items stand: framebuffer-as-texture (a `G_LOADTILE` whose
   source is a rendered colour image reads RDRAM), presentation still resolves
   the VI's origin by containment rather than applying its own view rectangle,
   the deployed site has only been exercised headless, the first load fetches the
   whole 21 MB wasm, the browser build assigns gamepads to N64 controllers 2-4,
   and `debug/probes/rdram.cjs` reads one dump per run.
3. The attract story (scene `0x0B`) was not measured this session: the natural
   run faulted in the publishers before reaching it, and the forced entries used
   for the trace work are the title and the publishers. Session 143's
   `docs/proofs/web-attract-story-full-frame.png` is the last frame of it on
   record.
4. Session 141's `divide by zero` at task 277 in the publishers is still reachable
   on a route that reaches scene `0x000A` (§5). It is the `func_801989AC` divide
   session 141 read as a corrupted descriptor width and session 142 as the
   writeback's one-row overrun; both were fixed and it still reproduces, so the
   diagnosis is not complete.
