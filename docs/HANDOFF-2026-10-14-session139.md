# Session 139 handoff — RDRAM framebuffer indirection in the browser renderer

> **Summary.** Session 139 handoff — **RDRAM framebuffer indirection is in the WebGL2 prototype, and the assembled New Game backdrop appears in the browser.** Continue the browser port; session 138's open item 1 was the last of the three named renderer gaps. `G_SETCIMG` was a `break` and every draw went to the canvas, so the game's own CPU readback of its framebuffer read stale RDRAM. `app/src/web_renderer.cpp` now decodes `G_SETCIMG` the way RT64's `GBI_RDP::setColorImage` does (`fmt` bits 21-23, `siz` bits 19-20, `width` bits 0-11 plus one, address masked `& 0xFFFFFF`), keeps the colour image across display lists the way RT64's RDP state is persistent, and gives each guest address a GL framebuffer object: created lazily, uploaded from what RDRAM already holds, bound per draw, its viewport and scissor set from the target, and the drawn window-row band recorded so the writeback copies only what was touched. The writeback is the RDP's own 16-bit layout — a value `V` at logical byte offset `off` is read by the runtime as `rd16(p + (off ^ 2))` (`recomp.h` `MEM_H`), so `V & 0xFF` goes to `off ^ 2` and `V >> 8` to `off ^ 2 + 1`, which is the byte order RT64's `copyLastNativeToRAM` produces with a word swap, and the alpha bit is RT64 `Float4ToRGBA16`'s coverage test `(round(a*255) % 8) & 4`. **The readback is a handshake**, because the game's CPU thread and the browser main thread are different threads and a browser main thread may not block on a pthread mutex (session 138's flush already skips instead of blocking): `ogre_sync_framebuffers()` bumps `sync_request_` and waits on `sync_done_`, bounded at 1000 ms so a renderer that never flushes cannot hang the game, and `ogre_gfx_flush()` renders the queued draws and writes every dirty target back before storing `sync_done_`; a fast path returns without waiting when the queue is empty and nothing is dirty, because the game reaches this several times per background assembly and each wait otherwise costs one 16 ms flush interval. **The VI origin is not the colour-image address.** The standard NTSC `OSViMode`'s `fldRegs[0].origin` is `0x280` (one 640-byte row of a 320x240 16-bit image), so `events.cpp` `update_vi()` sets `VI_ORIGIN_REG = osVirtualToPhysical(framebuffer) + origin` and the address never equals the target the RDP rendered into; before this, every present missed its framebuffer and fell back to a raw RDRAM upload shifted by one row. `update_screen()` now publishes `VI_ORIGIN_REG`/`VI_STATUS_REG` and the main thread takes the framebuffer whose `[address, address + rowBytes*height)` contains it, preferring the most recently written — RT64's `FramebufferManager::findMostRecentContaining` rule (`[GFX-PRESENT] VI scanout 0x04B680 -> fb 0x04B400`). **The NDC denominator is the screen size the viewport defines**, not the canvas: OB64 submits `vscale = [640 480 511 0]` for its 320x240 screen, so `2 * (vscale/4)` is the width, and the game also runs a 640x476 VI mode (`OGRE_VI_TRACE=1`: `fbSizeX=640 fbSizeY=476 fbSiz=2`) whose colour images are `SETCIMG width=640` with a 640x480 scissor, and dividing by the canvas put every coordinate past x=319 outside NDC. A target's width now comes from the colour image. Measured: at the moment the game's njpeg readback copies, RDRAM holds the whole rendered image (`sync 1: waited=343ms ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75263`), and the cathedral backdrop renders in the browser (`docs/proofs/web-newgame-cathedral-assembled.png`, 304073/307200 non-black and 218964 colourful; the movie pass is `-movie.png`) where session 138's probe on the same URL produced a 32-pixel black frame (`web-newgame-cathedral-covered.png`). Title regression unchanged (`boot.cjs`: 24840 non-black, 15076 colourful against session 138's 24840/15108). New bounded diagnostics: `[GFX-CIMG]`, `[GFX-FB]`, `[GFX-FBREAD]`, `[GFX-FBSYNC]`, `[GFX-PRESENT]`. Open: a target's height and the canvas size still follow what the viewport gives a display list rather than the VI mode, so the world map's 640-wide targets came out `640x240`; framebuffer-as-texture; `K4`/`K5`.

## Goal

Continue the browser port. Session 138's open item 1 was the last of the three
named renderer gaps: the RDP renders into a guest colour image, the game reads
that image back out of RDRAM with the CPU (the njpeg backdrop assembly), and the
WebGL2 prototype rendered straight to the canvas, so it never wrote those pixels
anywhere. Session 138 §5 recorded the design.

## Result

1. **`app/src/web_renderer.cpp` keeps a GL framebuffer object per RDP colour
   image.** `G_SETCIMG` was a `break`; it now decodes `fmt`/`siz`/`width`/address
   the way RT64's `GBI_RDP::setColorImage` does, the RDP colour image is kept
   across display lists (RT64's RDP state is persistent), and every `DrawCmd`
   carries the target. The main thread creates the target lazily, uploads what
   RDRAM already holds, draws into it, and writes the drawn rows back.
2. **`ogre_sync_framebuffers()` is a handshake.** The recompiled njpeg readback
   calls it from the game's CPU thread; it asks the browser main thread to render
   the queued draws and write every dirty target back to RDRAM and waits on an
   atomic, bounded at 1000 ms. A fast path returns without waiting when RDRAM
   already holds every draw.
3. **`update_screen()` presents the VI's scanout buffer.** The VI origin is not
   the colour-image address: the standard NTSC `OSViMode`'s `fldRegs[].origin` is
   `0x280`, so the VI scans out from `framebuffer + 0x280` while the RDP renders
   at `framebuffer`. Presentation resolves it by containment, the way RT64's
   `FramebufferManager::findMostRecentContaining` does.
4. **The assembled backdrop appears in the browser**
   (`docs/proofs/web-newgame-cathedral-assembled.png`). Session 138's
   `web-newgame-cathedral-covered.png` is the state this replaces.
5. **The 640-pixel-wide colour image no longer clips at x=319.** The game sets
   six distinct `G_SETCIMG` configurations, three of them 640 pixels wide (the
   world map, VI `fbSizeX=640 fbSizeY=476`). The render target's width now comes
   from the colour image, and the NDC denominator is the *screen* size the
   viewport defines rather than the canvas, so geometry past x=319 renders. The
   height is incomplete: it comes from the viewport, and a display list that does
   not set one falls back to the canvas height, so the world map's 640-wide
   targets were created `640x240` in the runs here and the lower half of their
   480-row area is not rendered (open item 1).

## Evidence

### 1. The game's colour images, from the ROM's own display lists

`OGRE_SAVE=last_low_chaos OGRE_MOD_TEST=1 OGRE_DL_DECODE=all` on the native build
decodes every display list, and `SETCIMG` appears 3076 times in 6 distinct
configurations:

```
 717 SETCIMG fmt=0 siz=2 width=320 addr=0x00025C00
 717 SETCIMG fmt=0 siz=2 width=320 addr=0x00000400
 716 SETCIMG fmt=0 siz=2 width=320 addr=0x0004B400
 463 SETCIMG fmt=0 siz=2 width=640 addr=0x002E6840
 232 SETCIMG fmt=0 siz=2 width=640 addr=0x001BA780
 231 SETCIMG fmt=0 siz=2 width=640 addr=0x00250800
```

Every one is `G_IM_FMT_RGBA`, `G_IM_SIZ_16b`. The row pitch is RT64's
`imageRowBytes` (`width << siz >> 1`), so 640 and 1280 bytes; the code uses the
same formula. `OGRE_VI_TRACE=1` on the same save shows the VI mode that goes with
the 640-wide targets:

```
  32 fbSizeX=320 fbSizeY=240 fbSiz=2
   8 fbSizeX=640 fbSizeY=476 fbSiz=2
```

The 640x480 target is rendered with a matching scissor, from the same decode:

```
3A46B0 SETSCISSOR ulx=0 uly=0 lrx=2560 lry=1920   (10.2 fixed: 640x480)
3A46C8 SETCIMG    fmt=0 siz=2 width=640 addr=0x002E6840
1863B0 FILLRECT   ulx=0 uly=0 lrx=2556 lry=116
```

### 2. The VI origin carries a 0x280 offset

Before this session the present path looked the VI origin up as a colour-image
address and never found one. The run's own log shows why (`[GFX-PRESENT]`, after
the containment fix):

```
VI scanout 0x04B680 -> fb 0x04B400
VI scanout 0x025E80 -> fb 0x025C00
VI scanout 0x000680 -> fb 0x000400
```

`0x4B680 - 0x4B400 = 0x280`, which is one 640-byte row of a 320x240 16-bit
image and is the standard NTSC mode's `fldRegs[0].origin`
(`events.cpp` `update_vi()` computes `osVirtualToPhysical(framebuffer) +
field_regs->origin`). Presentation now takes the framebuffer whose
`[address, address + rowBytes*height)` contains the origin.

### 3. The pixels reach RDRAM before the game reads them

The handshake was measured with a temporary diagnostic that counts non-black
texels in the game's three framebuffers inside `sync_framebuffers()`, i.e. what
the CPU is about to copy:

```
[GFX-FBSYNC] sync 1: waited=343ms ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75263
[GFX-FBSYNC] sync 2: waited=0ms   ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75257
[GFX-FBSYNC] sync 3: waited=309ms ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75257
[GFX-FBSYNC] sync 4: waited=178ms ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75250
```

76800 of 76800 texels is the whole 320x240 image. The writeback itself, from the
same run:

```
[GFX-FBSYNC] wrote back 0x000400 320x240 siz=2 pitch=640 guestRows=[0,239]
[GFX-FBSYNC] wrote back 0x025C00 320x240 siz=2 pitch=640 guestRows=[0,239]
[GFX-FBSYNC] wrote back 0x04B400 320x240 siz=2 pitch=640 guestRows=[0,239]
```

The byte layout is the RDP's. A 16-bit RGBA5551 value `V` at logical byte offset
`off` is read by the runtime as `rd16(p + (off ^ 2))` (`recomp.h` `MEM_H` adds
`^ 2`), so `V & 0xFF` goes to `off ^ 2` and `V >> 8` to `off ^ 2 + 1`; RT64
produces the same bytes by copying its native buffer and swapping every word
(`Framebuffer::copyLastNativeToRAM`). The value is
`((r8 >> 3) << 11) | ((g8 >> 3) << 6) | ((b8 >> 3) << 1) | coverage`, where the
alpha bit is RT64's `Float4ToRGBA16` coverage test `(round(a*255) % 8) & 4`.

### 4. The assembled backdrop, in the browser

Route and capture:

```sh
make web
python3 debug/server.py &                      # port 8931, COOP/COEP
NODE_PATH=~/dev/fatos/node_modules node debug/probes/atmark.cjs \
  --wait 'GFX-S2D' --shots 20 --gap 500 --timeout 200 --out web-final \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_TAP_MS=1500&OGRE_TAP_BUTTON=start,a,start,a,start,a,a,a,a,a,a,a,a&OGRE_S2D_TRACE=1'
```

| shot | non-black | colourful |
|---|---|---|
| 0 (`web-newgame-cathedral-movie.png`) | 304085/307200 | 178564 |
| 17-19 (`-assembled.png` = 19) | 245732 → 304073 | 181152 → 218964 |

The same probe was re-run on the final build after the last two edits (a
comment and the removal of an unused counter), and it reproduces the scene:
`--out web-ship` gives shot 19 at `nonBlack 245652, colorful 19496`, a frame of
the same cathedral during the game's fade.

Shot 0 is the S2DEX2 macroblock movie. Shots 1-16 are the game's own near-black
transition frames (a few glyph-sized white runs; the native present capture on
the same tap route shows the same black transitions between stills). Shot 19 is
the cathedral: the same statues, carpet, candelabra and characters as the native
reference `docs/proofs/native-newgame-cathedral.png`, at a different scroll
offset. Session 138's probe on the same URL produced
`web-newgame-cathedral-covered.png` (2240 bytes, 32 non-black pixels) for the
same scene.

### 5. Title regression

`node debug/probes/boot.cjs --attempts 2 --secs 60 --out web-title-final`:

```
RENDERED {"w":640,"h":480,"total":307200,"nonBlack":24840,"colorful":15076,...}
```

Session 138 recorded `nonBlack 24840 / colorful 15108` for the same probe, so the
title path is unchanged by the framebuffer indirection.

## What the renderer does now

- `RenderState::cimg_*` (`address`, `row_bytes`, `width`, `siz`, `fmt`) is
  decoded from `G_SETCIMG` and copied into every task's state from the renderer's
  persistent copy, because the RDP keeps it across display lists.
- `DrawCmd::fb_*` carries the target to the main thread.
- `GlFramebuffer` (one per guest address) holds the FBO, its size, the guest row
  pitch, the window-row band drawn since the last writeback, and the last write
  stamp presentation uses to break a containment tie.
- `flush_commands()` binds the target per draw, sets the viewport and the scissor
  from it, records the drawn row band, then presents and writes back.
- The NDC denominator is the viewport's screen size (`2 * viewScale`) when the
  task sets a viewport, and the canvas size otherwise. The comment on
  `set_viewport` records why the factor is 2: OB64 submits
  `vscale = [640 480 511 0]` for its 320x240 screen, so `2 * vscale.x / 4` is the
  screen width.
- Presentation blits the target's texture to the canvas with the COPY combiner.
  A scanout buffer with no GL target falls back to uploading RDRAM, and a frame
  whose draws went straight to the canvas (no `G_SETCIMG` ever seen) is left
  alone, which is the prototype's original path.

## Diagnostics added (kept)

Bounded, on by default, one line each:

- `[GFX-CIMG]` — each distinct `G_SETCIMG` configuration (16 max).
- `[GFX-FB]` — each framebuffer created (16 max), with its size, `siz` and pitch.
- `[GFX-FBREAD]` — the first 8 writebacks and every 500th, with the non-black
  count of the band written.
- `[GFX-FBSYNC]` — the first 6 writebacks with their guest row range, and the
  first 8 sync handshakes with the wait and the RDRAM content of the game's three
  framebuffers.
- `[GFX-PRESENT]` — the first 12 presents and every 300th, with the framebuffer
  the VI origin resolved to and its last measured non-black count.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | framebuffer indirection: `G_SETCIMG`, per-address GL framebuffer objects, the `ogre_sync_framebuffers` handshake, VI presentation by containment, the viewport-derived screen size, and the diagnostics |
| `docs/proofs/web-newgame-cathedral-assembled.png`, `-movie.png` | the two browser captures |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status, decision, log, index |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Probes

None were left in the tree. The measurements used the diagnostics listed above
and the port's existing knobs (`OGRE_DL_DECODE`, `OGRE_VI_TRACE`,
`OGRE_RDP_TRACE`, `OGRE_SAVE`, `OGRE_CAPTURE_PRESENT` for the native reference
frames) plus `/tmp/ppm2png.py`, a throwaway PPM-to-PNG converter for the native
present captures.

## Open

1. **The render target's height and the canvas size follow the VI mode.** The VI
   scans out 640x476 in the world map; the renderer takes a target's height from
   the viewport, which a display list need not set, so the world map's 640-wide
   targets came out `640x240` and their lower half is not rendered. Deriving the
   screen size from the VI registers (`RT64`'s `VI::fbSize`, which also handles
   the interlaced half-width case) and resizing the 320x240 canvas on a mode
   switch are the two pieces.
2. **Framebuffer-as-texture.** A `G_LOADTILE` whose source lies inside a rendered
   colour image still reads RDRAM rather than the target. The njpeg backdrop does
   not need it (its assembly is a CPU copy, which is why the sync exists), but
   RT64's `checkRegionsTMEM` tile copy has no equivalent here.
3. **`K4`/`K5` in the fragment shader** still return 0. `G_SETCONVERT`'s last two
   fields are parsed, so wiring them as uniforms is small.
4. **CI** (`.github/workflows/wasm.yml`) still has no observed run.
5. **Presentation ignores the VI's scanout geometry.** It blits the whole target,
   so a `fldRegs[].origin` offset inside the frame and the VI's x/y scaling are
   not applied. Session 138's shot 0 has the game's own black letterbox, so the
   offset is not visible in the captures used here, but a mode that starts
   mid-frame would show it.
