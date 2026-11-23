# Session 141 handoff — the browser renderer follows the VI's scanout size, and its TEXRECT UV step was 32x too large

> **Summary.** Session 141 handoff — **the browser renderer's render targets and canvas follow the VI's scanout size, its per-pixel `G_TEXRECT` S/T step was 32x too large, and the hi-res title screen turned out to be a pre-existing wall.** Continue the browser port from session 140's open item 3. (1) `VI::fbSize` (`tools/RT64/src/hle/rt64_vi.cpp`) is ported to `vi_framebuffer_size()` over the six register words `ultramodern::renderer::get_vi_regs()` returns; a throwaway Python reimplementation of that arithmetic reproduces RT64's own `fbSizeX`/`fbSizeY` on all 93 `[vi]` lines of a native run, `320x240` and the world map's `640x476` included. The render target's height is now `max(viewport height, VI height, scissor bottom + 1)` (RT64's `framebufferHeightForDisplay`) and a target keeps the tallest height while its width and `siz` hold (`FramebufferManager::get`), so the world map's three heap colour images are `640x481` instead of `640x240`; the NDC denominator is the render target's size (colour-image width, else viewport, else VI size), so the 640-wide targets no longer clamp at x=319; and the canvas element is resized to the VI's scanout size on a mode switch, so that mode presents 640x476 instead of through a 320x240 drawing buffer. Only a **visible** VI mode updates the size (RT64 `VI::visible`: `status.type != BLANK && hRegion.hStart > 0`), because OB64 blanks the VI across the switch and `fbSize` alone reads that blanked state, whose serrate bit already belongs to the next mode, as `320x952` — which is what this change's first version produced (`[GFX-FB] framebuffer 0x2E6840: 640x952`, canvas 320x952). (2) The per-pixel `G_TEXRECT` S/T step was 32x too large: RT64's `RDP::drawRect` gives `u2 - u1 = dsdx * rectWidth / 1024`, and the renderer used `dsdx / 32` times the width. The title screen exposes it because its backdrop is forty 640x12 texrects over one 320x240 CI8 image (`addr=0x38F6C8 stride=320 line=40 dsdx=dtdy=512`, `t0` stepping 192 per strip): correct, a strip samples its own six source rows (`512 * 12 / 1024 = 6`) and the 2x upscale (`512 * 640 / 1024 = 320` texels over 640 pixels); with the old constant each strip repeated those rows about 35 times, which is `docs/proofs/web-title-640-before-uv-fix.png`. Nothing else the browser had rendered used the path (the intro and cutscenes draw triangles, and the S2DEX2 object rectangle takes its UVs from the sprite's image size). (3) `K4`/`K5` are combiner colour inputs (`convertK[4] / 255`, RT64 `RasterPS.hlsl`) instead of 0. **The wall found while measuring:** both this build and the untouched session-140 `dist-web` package fault with `divide by zero` at exactly task 277, with the same wasm stack (`func_801989AC` <- `func_80198ECC` <- `func_801A29B8` <- `func_801A40EC` <- `func_80177EC0` <- `func_80072398` <- `func_8008AFE0`), the first time the game renders its hi-res title screen; the pre-change package is the control that proves it is not this session's regression. `func_801989AC` is unit E's `bne` delay slot at `0x801989A8` and `tools/guestmap.py` gives that RAM to unit E with the main ELF sharing it, so the wasm name says the main unit's body ran there. New bounded diagnostics `[GFX-VI]` and `[GFX-WIDE]`. See `PLAN.md` open work 8 and `docs/DECISIONS.md`.

## Goal

Continue the browser renderer (developer, session start). Session 140's open item
3 lists the three renderer gaps session 139 named: the render target's height and
the canvas do not follow the VI mode, framebuffer-as-texture is missing, and
`K4`/`K5` return 0. The first and the third are closed here; measuring the first
turned up a fourth renderer defect (the `G_TEXRECT` UV step) and a pre-existing
wall in the browser build (a `divide by zero` fault at the hi-res title screen).

## Result

1. **The render target's size and the canvas come from the VI.** `VI::fbSize`
   (`tools/RT64/src/hle/rt64_vi.cpp`) is ported to `vi_framebuffer_size()` in
   `app/src/web_renderer.cpp` over the register words
   `ultramodern::renderer::get_vi_regs()` returns. The target's height is
   `max(viewport height, VI height, scissor bottom + 1)`, which is RT64's
   `framebufferHeightForDisplay(drawnBottom, viHeight)`
   (`tools/RT64/src/hle/rt64_framebuffer.h`), and `ensure_framebuffer` keeps the
   tallest height it has been asked for while the width and `siz` are unchanged,
   the way `FramebufferManager::get` does. Only a **visible** mode updates it
   (RT64 `VI::visible`: `status.type != BLANK && hRegion.hStart > 0`), because
   OB64 blanks the VI across the mode switch and `fbSize` alone reads that state
   as `320x952`.
2. **The NDC denominator is the render target's size**, not the viewport: the
   colour image's own `G_SETCIMG` width first, then the viewport, then the VI
   size. RDP screen coordinates are framebuffer pixels, so this is what makes
   the world map's 640-wide targets stop clamping at x=319.
3. **The canvas element is resized to the VI's scanout size** on a mode switch,
   from the main thread's present path. A 640x476 mode presents 1:1 instead of
   through a 320x240 drawing buffer.
4. **The per-pixel `G_TEXRECT` S/T step was 32x too large.** RT64's
   `RDP::drawRect` computes `lrs = ((uls << 7) + dsdx * (rectWidth << 2)) >> 7`
   and then `u2 = lrs / 32`, i.e. `u2 - u1 = dsdx * rectWidth / 1024`. The
   renderer used `dsdx / 32` multiplied by the rectangle's width. Fixed to
   `dsdx / 1024` and `dtdy / 1024`.
5. **`K4`/`K5` are combiner colour inputs.** `convert_k[4]`/`[5]` (raw 9-bit,
   already parsed from `G_SETCONVERT`) go to two new uniforms and the
   `C_K4`/`C_K5` cases of `color_input()`, matching RT64 `RasterPS.hlsl`
   (`ccInputs.K4 = convertK[4] / 255.0f`; no sign extension, unlike `k0..k3` of
   the YUV matrix).
6. **Two bounded diagnostics stay**: `[GFX-VI]` (each distinct visible scanout
   size with the register words it comes from) and `[GFX-WIDE]` (the first 64
   draws of the first task that targets a 640-wide colour image, with the
   geometry, the fill colour and the tile source).

## Evidence

### 1. The ported `fbSize` reproduces RT64 on every line of a native run

A native run with the developer's battery (`OGRE_SAVE=save-mission-1`,
`OGRE_VI_TRACE=1`, 45 s at `OGRE_SPEED=4`) logs 93 `[vi]` lines with RT64's own
`fbSizeX`/`fbSizeY` and all six register words. A throwaway Python
reimplementation of the port's arithmetic
(`width`, `status`, `hRegionW`, `vRegionW`, `xformW`, `yformW`) checks every one:

```sh
python3 /tmp/vicheck.py /tmp/vi-map.log
# checked 93 [vi] lines, 0 mismatches
```

The run's distinct sizes are `320x240` (84 lines), `640x476` (8) and `0x0` (1,
before any mode is set). The world map's mode is
`status=0x00003252 origin=0x1BAC80 width=640 hRegionW=0x006C02EC
vRegionW=0x002301FD xformW=0x00000400 yformW=0x02000800`:

```
size.x = 640                                  (serrate set, but 640/1.0 = 640 is not < 640/1.875)
size.y = (0x1FD - 0x23) / (2 * 0.5 * 1.0) = 474; +2 = 476; /4 -> 476
```

### 2. The blanked mode must not update the size

`events.cpp`'s `update_vi` sets `regs.VI_H_START_REG = 0` while the game holds
`VI_STATE_BLACK`. The game blanks the VI across the 320x240 → 640x480 switch, and
that state still carries the next mode's serrate bit, so `fbSize` alone reads it
as `320x952`:

```
[GFX-VI] VI scanout 320x952 origin=0x026100 status=0x0000324E width=640 hStart=0x00000000 ...
[GFX-CIMG] SETCIMG addr=0x2E6840 fmt=0 siz=2 width=640 rowBytes=1280
[GFX-FB] framebuffer 0x2E6840: 640x952 siz=2 fmt=0 pitch=1280
[GFX-PRESENT] canvas resized to 320x952 for the VI mode
```

That was this change's first version (the capture is
`debug/out/web141-map-canvas.png`'s run). With `vi_visible()` the same run gives

```
[GFX-VI] VI scanout 320x240 ...  [GFX-VI] VI scanout 640x476 origin=0x1BAC80 status=0x00003252 ...
[GFX-FB] framebuffer 0x2E6840: 640x481 siz=2 fmt=0 pitch=1280
[GFX-FB] framebuffer 0x1BA780: 640x481 siz=2 fmt=0 pitch=1280
[GFX-FB] framebuffer 0x250800: 640x481 siz=2 fmt=0 pitch=1280
[GFX-PRESENT] canvas resized to 640x476 for the VI mode
```

`481` is RT64's number too: `framebufferHeightForDisplay(bottom(true) = 481,
viFbSize.y = 476)` = 481, because the game's scissor is `(0,0)-(640,480)`.

### 3. The world map renders in the browser from the developer's battery

The battery was written into the wasm filesystem before the boot
(`Module.FS.writeFile('/ogre/saves/ogrebattle64-us-rev1.bin', ...)`), which is how
`<config>/saves/<game id>.bin` is resolved (`librecomp/src/pi.cpp`
`set_save_file_path`). Screenshot of the canvas at the 640-wide mode after this
change: `debug/out/web141-map2-canvas.png`, 307200/307200 non-black, 247857
colourful, where the run before it produced the poisoned 320x952 canvas and
`docs/proofs` has no earlier browser world-map frame.

### 4. The TEXRECT UV step

The title screen's backdrop is forty strips of 640x12 pixels over one 320x240 CI8
image. The renderer's own `[GFX-WIDE]` lines give every field:

```
[GFX-WIDE] texrect (0,0)-(640,12)   fb=0x1BA780 cimg=640 target=640x481 siz=2 scissor=1(0,0,640,480) tile=0 addr=0x38F6C8 stride=320 fmt=2 siz=1 line=40 uls=0 ult=0   lrs=1276 lrt=20  s0=0 t0=0    dsdx=512 dtdy=512
[GFX-WIDE] texrect (0,12)-(640,24)  ... ult=24  lrt=44  t0=192 ...
[GFX-WIDE] texrect (0,372)-(640,384) ... ult=744 lrt=764 t0=5952 ...
```

- `stride=320` with `siz=1` (`G_IM_SIZ_8b`) is the 320-texel-wide source's row
  pitch; `line=40` (words) is the same 320 bytes; `lrs-uls = 1276/4 = 319`, so
  the tile is 320 texels wide.
- `dsdx = 512` over a 640-pixel rect: `512 * 640 / 1024 = 320` texels, i.e. the
  320-texel image spread over the full width (the RDP's 2x upscale). The old
  constant gave `(512 / 32) * 640 = 10240` texels.
- `dtdy = 512` over the 12-row strip: `512 * 12 / 1024 = 6` texels, which is
  exactly the six source rows the strip's tile declares (`ult=0..lrt=20` is
  0..5, and the next strip starts at `ult=24` = row 6). The old constant gave
  `(512 / 32) * 13 = 208` texels, so every strip repeated its own six rows about
  35 times.

`docs/proofs/web-title-640-before-uv-fix.png` is that state: the 640x476 title
mode with its backdrop shredded into horizontal bands
(`debug/out/web141-wide-640-30.png`).

One convention is deliberately left alone: a rectangle covers
`ndc_x(xl)..ndc_x(xh + 1)`, i.e. `xh - xl + 1` pixels, where RT64's `drawRect`
covers `[xl, xh)`. Every rectangle in the game is therefore one pixel wider than
the native renderer's, and the title screen's strips paint one row into the next
strip. That is the pre-existing convention of this renderer's fill-rect path
(which needs the `+1` to cover a full-screen clear's last column), it is what
`h` is computed from, and the texel-per-pixel rate it produces is the game's own
(6.5 texels over 13 rows is the same 0.5 as 6 over 12).

### 5. The hi-res title screen is a pre-existing wall, not this session's change

Both the session-140 `dist-web` package (served by `python3 -m http.server` on
8940, service worker isolating it) and the session-141 build freeze at **exactly
task 277** with a black canvas, and both report the same wasm stack:

```
PAGEERROR: divide by zero
  at func_801989AC  <- func_80198ECC <- func_801A29B8 <- func_801A40EC
     <- func_80177EC0 <- func_80072398 <- func_8008AFE0   (the frame pump)
```

The control run (`web141-control2`, 300 s, the pre-change package) reaches it at
t≈200 s; the session-141 builds reach it at t≈72–105 s. `tools/guestmap.py` on
the faulting addresses:

```
0x801989AC  unit E bankRec0 (rom 0x066E30 +0xE1C, size 0x2AF0)
            symbol: _ovlE_801988C8 +0xE4  (NOT a function entry, bankE.elf)
            also:   func_801989AC (ogrebattle64.elf) - a different module's layout shares this RAM
```

`0x801989AC` is the delay slot of unit E's `bne` at `0x801989A8`
(`func_ovlE_801988C8`): `801989a8: 14620007 bne v1,v0,801989c8` /
`801989ac: 24050001 li a1,1`. The wasm function name is the main ELF's, so the
main unit's body ran at that address. That is the session-45 class of question,
and the discriminating experiment is the runtime's resident-module report at that
workload, which this session did not run.

## What the renderer does now

- `target_width_px(st)` / `target_height_px(st)` replace the viewport-only
  `screen_width`/`screen_height`; both are used by `ndc_xf`/`ndc_yf` and by
  `record_state`'s `fb_height_px`.
- `vi_size()` returns the last visible scanout size, packed into one
  `std::atomic<uint64_t>` as `width << 32 | height` so the main thread's present
  path and the gfx thread's draws read one value.
- `resize_canvas()` calls `emscripten_set_canvas_element_size` and drops the
  RDRAM-fallback staging texture; the element's CSS size stays index.html's, so a
  320x240 mode keeps its 2x pixelated upscale.
- `DrawCmd::k4`/`k5` carry `convert_k[4]`/`[5]` over 255 to the `u_k4`/`u_k5`
  uniforms.

## Diagnostics added (kept)

- `[GFX-VI]` — each distinct visible VI scanout size once (8 max), with
  `origin`, `status`, `width`, `hStart`, `vStart`, `xScale`, `yScale`.
- `[GFX-WIDE]` — the first 64 draws of the first task that targets a 640-wide
  colour image, with kind, rectangle, framebuffer, colour image and target size,
  scissor, and (per kind) the fill colour, the tile source or the texture flag.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `vi_framebuffer_size()`/`vi_visible()`/`ViSize`; `vi_size()`, `target_width_px()`, `target_height_px()`; `resize_canvas()`; the `G_TEXRECT` UV step fix (and `test_draw`'s step, so the synthetic self-check still spreads its tile); `K4`/`K5` uniforms and combiner cases; the two new diagnostics; the header's limitation list |
| `docs/proofs/web-title-640-before-uv-fix.png` | new; the banded 640x476 title frame |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md` | status, decisions, log |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Verification run

- `python3 /tmp/vicheck.py /tmp/vi-map.log` — 93/93 `[vi]` lines match RT64's
  `fbSizeX`/`fbSizeY`.
- `make web` — builds `build-wasm/ogrebattle64.wasm` (run after every source
  change; the final run is clean).
- `node debug/probes/boot.cjs --attempts 2 --secs 75 --out web141-final-title`
  against `python3 debug/server.py`, on the finished build (after the UV fix) —
  `RENDERED {"w":640,"h":480,"nonBlack":25496,"colorful":18868}` with
  `sawRealDl=true`. The same probe and route measured `nonBlack 24840 /
  colorful 15108` in session 138 and `24840 / 15076` in session 139, so the
  320x240 path is unchanged (the count is the intro scene, which is what
  `docs/proofs/web-title.png` has always shown; the number moves with the
  intro's fade).
- `node debug/probes/testdraw.cjs --warmup 14000 --out web141-testdraw` — the
  synthetic display list still renders (`test_draw exported= true returned= 1`).
  Its `G_TEXRECT` step was `dsdx=dtdy=1` under the old constant, which sampled a
  1/16-texel sliver; it is now `32, 43`, which spreads the 2x2 tile over the
  64x48 rectangle (`2 * 1024 / 64` and `2 * 1024 / 48`), so the self-check keeps
  testing the path it names. It cannot serve as the fix's picture: the game
  presents its own framebuffer over the canvas every frame.
- A throwaway probe (`/tmp/map-probe.cjs`, not committed) writes the developer's
  battery into `/ogre/saves/` and samples the canvas and the milestone text. The
  640-wide run after this change: `web141-map2` (VI `320x240` and `640x476`,
  framebuffers `640x481`, canvas 307200/307200 non-black).
- The same probe against the untouched `dist-web` package on port 8940 is the
  control for §5.
- No native build was run; `app/src/web_renderer.cpp` is Emscripten-only.

## Probes

`/tmp/map-probe.cjs` and `/tmp/vicheck.py` are throwaway (both are named in the
evidence). No probe is left in the tree; `[GFX-VI]` and `[GFX-WIDE]` are
diagnostics that stay, both bounded.

## Open

1. **The browser build faults at the hi-res title screen** (§5). Both the
   session-140 package and this build, `divide by zero` at task 277, canvas
   black. It is the next thing to fix if the browser build is to be playable.
2. **Framebuffer-as-texture is still missing.** A `G_LOADTILE` whose source lies
   inside a colour image the RDP has already rendered reads RDRAM. The writeback
   happens on every present and on `ogre_sync_framebuffers()`, so the browser's
   version of this gap is a staleness race rather than the native renderer's
   systematic one, but a display list that draws into a target and loads a tile
   from it in the same task reads the previous frame.
3. **Presentation ignores the VI's own view rectangle** (the `fldRegs[].origin`
   row offset and the x/y scale). The port resolves the origin by containment, so
   the one-row offset of the standard NTSC mode is not applied.
4. **The `G_TEXRECT` UV fix has no post-fix frame.** The title screen is the only
   scene the browser has reached that exercises the path, and the game faults
   there before a frame settles, so the fix is evidenced by RT64's arithmetic and
   the game's own `dsdx`/`dtdy`/tile numbers (§4) rather than by a picture.
5. Session 140's items stand: the deployed site has only been exercised headless,
   the first load fetches the whole 21 MB wasm before a ROM is picked, and the
   browser build assigns gamepads to N64 controllers 2-4.
