# Web renderer design

This document describes the browser build's renderer in
[web_renderer.cpp](../../app/src/web_renderer.cpp). It describes the tree at
2026-11-01. The code is the source of truth. The measurements behind individual
rules are in `docs/HANDOFF-*.md` and `docs/DECISIONS.md`.

## Scope

The renderer implements `ultramodern::renderer::RendererContext` with WebGL2. It
parses the game's display lists, performs the F3DEX2/S2DEX2 RSP work on the CPU,
and draws the RDP output with one GLSL program.

The renderer is independent of RT64, which is the native renderer. The browser
renderer shares no code with it. It ports RT64's rules where a reference is
needed, and the comments cite the RT64 file and line each rule came from. That
is what lets a browser frame be compared pixel by pixel with a native RT64
frame.

The renderer is a high-level emulator. It does not run the game's graphics
microcode. It decodes the F3DEX2 and S2DEX2 command streams and evaluates the RDP
state directly.

The supported GBIs are F3DEX2, F3DEX2.NoN and S2DEX2
([web_renderer.cpp:551](../../app/src/web_renderer.cpp#L551)). No other GBI is
implemented.

The renderer is validated screen by screen against RT64's render targets. The
records cover the publisher stills, title, attract story, New Game opening
(cathedral backdrop on both visits), world map, tutorial field, tutorial battle
and mission briefing. There is no recorded full-game browser playthrough.

## 1. Build and runtime integration

[app/CMakeLists.txt](../../app/CMakeLists.txt#L183) selects the renderer:

| Build | Renderer source |
|---|---|
| `OGRE_USE_RT64` | `src/renderer.cpp` (RT64) |
| `EMSCRIPTEN` | `src/web_renderer.cpp` |
| anything else | `src/null_renderer.cpp` |

The runtime creates the renderer through the `create_render_context` callback
([main_web.cpp](../../app/src/main_web.cpp#L127)). `create_renderer` allocates a
`WebGLRenderer`, stores the RDRAM base, and applies a canvas handle that may have
arrived before the gfx thread started
([web_renderer.cpp:10433](../../app/src/web_renderer.cpp#L10433)).

The WebGL2 context is created on the page's main thread. `web.js` calls
`ogre_gfx_create_context` on `#game-canvas`, then passes the handle to
`ogre_gfx_set_canvas`
([web_renderer.cpp:10454](../../app/src/web_renderer.cpp#L10454),
[web_renderer.cpp:10477](../../app/src/web_renderer.cpp#L10477)). The context is
WebGL2 with no default depth, stencil, alpha or antialias buffer, and with
`preserveDrawingBuffer`. `set_canvas` does not create any GL object; GL setup is
deferred to the first `ogre_gfx_flush` on the browser main thread
([web_renderer.cpp:2711](../../app/src/web_renderer.cpp#L2711)).

The C entry points the page and the game use:

| Export | Purpose |
|---|---|
| `ogre_gfx_create_context` | create the WebGL2 context on `#game-canvas` |
| `ogre_gfx_set_canvas` | hand the context handle and size to the renderer |
| `ogre_gfx_flush` | run one batch of GL on the main thread; returns 1 when the renderer exists |
| `ogre_gfx_stats` | workload, flush, texture and framebuffer counters for the status line |
| `ogre_gfx_test_draw` | synthetic display list for the renderer controls |
| `ogre_gfx_debug_flags`, `ogre_gfx_debug_tex`, `ogre_gfx_debug_last_tex`, `ogre_gfx_debug_fb_source`, `ogre_gfx_debug_tex_count` | probe hooks |
| `ogre_sync_framebuffers` | write rendered colour images back to RDRAM; called by the game's CPU readback path, not by the page |

`ogre_gfx_flush` also runs the per-frame work that the native build hangs off
`update_gfx`: the scene poll and the timed RDRAM dump
([web_renderer.cpp:10532](../../app/src/web_renderer.cpp#L10532)). The browser
build has no `sdl_platform.cpp`, so this is where those hooks live.

The page calls `ogre_gfx_flush` from a 16 ms `setInterval` once the context is
handed over ([web.js](../../app/web/web.js#L2264)); the `requestAnimationFrame`
loop handles input and the page's own UI. The canvas CSS box is a whole multiple
of the drawing buffer; see §7.

## 2. Frame pipeline and threading

```text
game thread
  |  OSTask
  v
gfx pthread: send_dl(task)
  |  analyze_dl (workload stats, no lock)
  |  identify_gbi (XXH3-64 of the ucode text)
  |  fog repair (optional, OB64-specific)
  |  gbi::walk_dl -> exec_command
  |     RSP state + geometry + RDP state
  |     -> DrawCmd and TexUpload records
  |  publish_exec (short lock)
  v
browser main thread: ogre_gfx_flush()
  |  swap the queues under a try_lock
  |  upload GL textures
  |  for each DrawCmd: bind FBO, set state, glDrawArrays
  |  service framebuffer-source requests
  |  present the VI's scanout buffer
  |  write colour images back to RDRAM if a sync is pending
  v
canvas
```

The renderer runs on two threads and the split is deliberate. The gfx pthread
does the CPU work: it walks display lists, transforms vertices, decodes textures
and records commands. The browser main thread owns the WebGL2 context and
executes every GL call during a frame. The gfx thread records commands only, so
Emscripten never has to proxy GL calls from a pthread to the main thread. The
shutdown path deletes the GL objects.

`send_dl` is called on the gfx thread
([web_renderer.cpp:5558](../../app/src/web_renderer.cpp#L5558)). It:

1. runs the workload analyzer for `ogre_gfx_stats`, outside the renderer mutex;
2. seeds a `RenderState` with the RDP state that persists across display lists
   (the colour and depth image);
3. identifies the GBI and executes the list;
4. publishes the records under `mutex_`.

The display list is executed without `mutex_`. A slow or stalled walk therefore
cannot block the browser main thread. The mutex covers the vector exchange only.

`update_screen` on the gfx thread requests a present. The request carries the VI
registers that produced the frame: the visible flag and `fbSize` as well as the
scanout address ([web_renderer.cpp:5679](../../app/src/web_renderer.cpp#L5679)).
The size and the visible flag travel with the address because OB64 writes the
next scene's scanout address while the VI is blanked or still in the previous
mode.

`flush_commands` on the main thread does all GL
([web_renderer.cpp:4451](../../app/src/web_renderer.cpp#L4451)). Its queue
exchange uses `try_lock`. A contended pthread mutex cannot legally block the
browser main thread, and a blocked main thread would freeze the page, so a busy
renderer skips the flush and the next frame drains the queue. Skips are counted
in `ogre_gfx_stats`.

`ogre_sync_framebuffers` is the handshake for a CPU readback
([web_renderer.cpp:5744](../../app/src/web_renderer.cpp#L5744)). The game calls
it before it copies a rendered colour image out of RDRAM. The caller bumps
`sync_request_` and waits for `sync_done_`, which the main thread sets after the
writeback. The wait has a 1000 ms timeout and a fast path for "the queue is empty
and nothing is waiting to be written back". The timeout exists so a missing
flush cannot deadlock the game thread; it is logged when it fires.

The runtime posts the RSP task's DP completion when `send_dl` returns. The game
therefore runs ahead while a batch is still queued. The writeback happens at the
game's own readback request; §7 gives the reason.

## 3. Display-list front end

### GBI identification

`identify_gbi` hashes the task's ucode text with XXH3-64
([web_renderer.cpp:962](../../app/src/web_renderer.cpp#L962)). The hash length is
part of the entry because the `OSTask`'s `ucode_size` is one IMEM load (`0x1000`)
and not the text length.

| Length | XXH3-64 | GBI |
|---|---|---|
| `0x1390` | `0xCF55FAE288BFE48D` | F3DEX2 |
| `0x1390` | `0x4C12DAE0534D7135` | F3DEX2.NoN |
| `0x18C0` | `0x9300F34F3B438634` | S2DEX2 |

An unrecognised ucode falls back to the ucode address (`0x800A5110` is S2DEX2,
anything else is F3DEX2) and logs the digests at every database length. The
fallback makes an unknown ucode visible instead of silently drawing a wrong
frame.

The GBI matters because the opcode spaces collide. In an S2DEX2 list `0xDA` is
`G_OBJ_RECTANGLE_R` and `0xDC` is `G_OBJ_MOVEMEM`; in an F3DEX2 list the same
bytes are `G_MTX` and `G_MOVEMEM`. Walking an S2DEX2 list as F3DEX2 makes the
njpeg macroblock draws disappear.

### Walker

`gbi::walk_dl` ([gbi.cpp:63](../../app/src/gbi.cpp#L63)) walks the list in
execution order. It handles `G_DL` push and branch, segment registers, a branch
depth of 64 and a 4M-command budget. The walker and the executor share
`gbi::resolve_address`, so a `G_DL` target cannot mean two different addresses.
`gbi.cpp` is also compiled into the native build for the workload analyzer, so a
change to the walker affects both renderers' diagnostics.

`execute_dl` adds the per-list context and the visitor
([web_renderer.cpp:6980](../../app/src/web_renderer.cpp#L6980)). The visitor
updates the progress telemetry (the command the walk is about to run), records
the head and tail of a runaway walk, and calls `exec_command`.

### Commands

`exec_command` decodes one command
([web_renderer.cpp:7093](../../app/src/web_renderer.cpp#L7093)). It handles:

| Group | Commands |
|---|---|
| Matrices | `G_MTX`, `G_POPMTX` |
| Geometry state | `G_GEOMETRYMODE`, `G_TEXTURE`, `G_MOVEMEM`, `G_MOVEWORD` |
| Vertices | `G_VTX` |
| Primitives | `G_TRI1`, `G_TRI2`, `G_QUAD`, `G_FILLRECT`, `G_TEXRECT`, `G_TEXRECTFLIP` |
| Images | `G_SETCIMG`, `G_SETZIMG`, `G_SETTIMG` |
| Tiles | `G_SETTILE`, `G_SETTILESIZE`, `G_LOADTILE`, `G_LOADBLOCK`, `G_LOADTLUT` |
| Combiner | `G_SETCOMBINE` |
| Render state | `G_RDPSETOTHERMODE`, `G_SETOTHERMODE_H`, `G_SETOTHERMODE_L`, `G_SETSCISSOR` |
| Colours | `G_SETPRIMCOLOR`, `G_SETENVCOLOR`, `G_SETFILLCOLOR`, `G_SETFOGCOLOR`, `G_SETBLENDCOLOR` |
| Conversion and key | `G_SETCONVERT`, `G_SETKEYR`, `G_SETKEYGB` |
| Depth | `G_SETPRIMDEPTH` |

`G_MODIFYVTX`, `G_CULLDL`, `G_BRANCH_Z`, `G_LINE3D`, `G_SPECIAL_1`, `G_DMA_IO`,
`G_LOAD_UCODE`, `G_SPNOOP` and the pipe-sync commands fall through with no
action. `G_DL` and `G_ENDDL` are handled by the walker, not by this switch.

In an S2DEX2 list the object commands are decoded before the F3DEX2 switch:
`0xDA`, `0xDC`, `0x01` and `0x02` draw. `0xE4` (`G_RDPHALF_0`, S2DEX2's texrect
encoding) is reported once and not implemented. The S2DEX2 commands this
renderer does not implement (`0x03`..`0x0B`) return instead of falling into the
F3DEX2 triangle arms, so a future list cannot draw from a stale vertex buffer.

The `G_MOVEMEM` and `G_MOVEWORD` cases decode the viewport, the light slots, the
texture-generation lookat vectors, the fog vector and the segment registers. The
light layout is the game's own bytes: colour at logical 0/1/2, direction at
8/9/10, and the ambient term in slot `num_lights`.

### Fog repair (OB64-specific)

OB64 draws its scrolling fog and cloud layers as `G_TEXRECT`s with the white
combiner (`FCFFFFFF FFFF73B9`). The rectangles inherit a zero-area tile window
from the loop that precedes them, so the sample collapses onto one stale row and
the layer paints as an opaque band.

Before the walker runs, `execute_dl` calls `fog_repair_display_list`
([web_renderer.cpp:7046](../../app/src/web_renderer.cpp#L7046)) when `OGRE_FOG`
is on. The repair reads the layer table at `D_801B80E0`, takes layer `i`'s image
record, and rewrites the inherited `G_SETTIMG`, `G_SETTILE` (tile 7 and tile 0),
`G_LOADTILE` and `G_SETTILESIZE` (tile 0) so the rectangle samples that whole
image. It rewrites the submitted display list in RDRAM, which is the same bytes
the native `OGRE_FOG` pass rewrites in `app/src/renderer.cpp`; the walker then
sees an ordinary textured rectangle. `OGRE_FOG=0` disables the pass.

This is the one display-list rewrite in the browser renderer. It is
game-specific and should stay out of any generic renderer core.

## 4. RSP-side emulation

### Matrices and viewport

F3DEX2 stores the `G_MTX` push bit inverted. A stored `0x00` means
`MODELVIEW|MUL|PUSH`. The renderer XORs the bit, then applies load versus
multiply:

- The projection is one composite matrix. `LOAD` replaces it; `MUL` multiplies
  the new matrix in front of the old one.
- The modelview is a stack. `PUSH` copies the current matrix, `LOAD` replaces it,
  and `MUL` multiplies the new matrix in front. `G_POPMTX` pops `w1 >> 6`
  matrices and never pops the bottom entry.

The viewport comes from `G_MOVEMEM` index 8. `vscale`/`vtrans` are 14.2 fixed
point; the renderer divides them once and uses the result to map clip space to
screen space. Before the game sends a viewport, projection output is treated as
NDC. The NDC denominator is the screen size the viewport defines, not the
canvas size; OB64 uses `vscale = [640 480 511 0]` for its 320x240 screen and a
640x476 VI mode for other screens.

### Vertex path

The F3DEX2 vertex is 16 bytes: `(x,y) (z,flag) (s,t) (r,g,b,a)`. The runtime
stores each guest word byte-reversed, so the renderer reads fields with `n64h`
and `n64b` rather than a struct cast.

For each `G_TRI1`/`G_TRI2`/`G_QUAD` index the renderer:

1. transforms the position by modelview and projection;
2. computes the lit colour when `G_LIGHTING` is set: the vertex colour bytes are
   the signed normal over 127, the ambient term is light slot `num_lights`, and
   each directional light adds `max(dot(normal, light_dir * modelview), 0)` times
   its colour, clamped to 1;
3. computes texture coordinates, including `G_TEXTURE_GEN` and
   `G_TEXTURE_GEN_LINEAR`, or applies the `G_TEXTURE` scale to the vertex `s,t`;
4. replaces the alpha with the fog alpha when `G_FOG` is set:
   `max(ndc_z, 0) * fog_mul + fog_offset`, over 255;
5. clips at the eye plane. A triangle that crosses `w = 0` is cut against
   `w > 1e-3` and re-triangulated, instead of being dropped whole. Dropping it
   removed the tutorial field's ground;
6. applies RT64's half-pixel screen offset to triangle vertices (half a pixel
   right and half a pixel down) and maps the result to NDC;
7. applies the F3DEX2 cull bits in screen space. Both bits set draws nothing;
   front-face culling swaps the first and last index; `cross(V, U).z >= 0`
   decides.

`G_CULLDL`, `G_MODIFYVTX` and `G_BRANCH_Z` are parsed by the walker but have no
effect. No OB64 display list needs them.

When the page's WIDESCREEN setting is on and the current scene is an expanding
one, `vertex_clip` scales the clip-space x by 0.75 after the projection. That is
RT64's `ProjectionProcessor::adjustProjectionMatrix` for a 16:9 window against
the game's 4:3 target. The page also sets a 16:9 canvas box; the renderer does
not change its render targets for widescreen.

### S2DEX2 objects

OB64's njpeg backgrounds use `G_OBJ_MOVEMEM` (the sub matrix) and
`G_OBJ_RECTANGLE_R` (a 24-byte `uObjSprite`). `draw_obj_rect` decodes the sprite
fields, applies the object matrix and emits a textured quad
([web_renderer.cpp:9749](../../app/src/web_renderer.cpp#L9749)). This is a 2D
path: the RSP vertex pipeline does not run for these draws.

## 5. RDP state and shading

### State

`RenderState` holds everything the RDP carries across commands
([web_renderer.cpp:593](../../app/src/web_renderer.cpp#L593)): matrices, geometry
mode, othermode, lights, lookat vectors, fog vector, the 32 vertex slots, colours,
scissor, viewport, combiner, texture state, tiles, colour/depth images, the
S2DEX2 object matrix, the pending tile load, and the key/convert registers.

A draw is copied into a `DrawCmd`
([web_renderer.cpp:2259](../../app/src/web_renderer.cpp#L2259)). The record holds
the decoded combiner mux, the blender inputs, the texture keys and sampler state,
the scissor, the target image and its layout, the depth state, the per-vertex NDC
position, UV, shade, `w` and `z`, and the raw `OTHERMODE`/`G_SETCOMBINE` words for
the diagnostics. The gfx thread snapshots the state; the main thread never reads
`RenderState`.

### Combiner

`G_SETCOMBINE` is decoded with RT64's position-dependent selector rules
([web_renderer.cpp:297](../../app/src/web_renderer.cpp#L297)). A colour selector
value means a different input in A, B, C and D: 6 is ONE for A/D, KEY_CENTER for
B and KEY_SCALE for C; 7 is NOISE for A, K4 for B and COMBINED_ALPHA for C.
Alpha D reads the high word in both cycles. The decoded selectors feed both the
shader and the log names.

The fragment shader evaluates RT64's formula
([web_renderer.cpp:1338](../../app/src/web_renderer.cpp#L1338)):

```text
rgb   = wrap_clamp((A - B) * C + D)
alpha = wrap_clamp((Aa - Ba) * Ca + Da)
```

A one-cycle draw evaluates the second mux. A two-cycle draw evaluates mux 0 then
mux 1, with TEXEL0/TEXEL1 swapped and COMBINED wrapped in the second cycle. Copy
mode passes TEXEL0 through. The shader also has the debug modes that output a raw
texel or the alpha channel for the probes.

`combiner_uses_texture` answers whether a draw samples TEXEL0 or TEXEL1 at all.
A rectangle with a PRIM-only combiner draws without a texture; dropping it
removed the QUEST still's white plate.

### Blender

`Blender` decodes `OTHERMODE_L` bits 16-31 into P/M/A/B for each cycle
([web_renderer.cpp:422](../../app/src/web_renderer.cpp#L422)). It computes the
number of cycles, whether the blender reads the framebuffer, and whether a
shader approximation is needed. The shader evaluates the blenders that do not
need the framebuffer colour directly. Two of RT64's approximations are ported:
`CombinerFramebuffer1MA_SquareMix` and `AnyFramebuffer1MA_MultiplyMix`.

The GL blend is plain `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`. RT64 uses dual-source
blending with the factor in the secondary output; the primary output's alpha
carries the same value here, so the two are equivalent.

### Texture sampler

The sampler is a port of RT64's `TextureSampler.hlsli`:

- tiles sample in texel space, not normalized UV space;
- the coordinate is quantized to the hardware's 1/128 texel;
- `G_TF_POINT`, `G_TF_BILERP` and `G_TF_AVERAGE` are implemented; the default
  RT64 path uses its three-point filter;
- `G_TX_WRAP`, `G_TX_MIRROR` and `G_TX_CLAMP` are applied per axis, with the
  tile's `masks`/`maskt` as the wrap size;
- the `ShiftScaleMap` from `G_TX_SHIFT` scales the coordinate;
- `G_TP_PERSP` bit 19 selects perspective-correct interpolation, with RT64's
  `perspCorrectionMod` halving for a non-perspective triangle.

`OGRE_TEXF` selects the RT64 sampler (`rt64`, default), true bilinear (`1` or
`bilinear`) or the GL fixed-function sampler (`gl`) for an A/B.

### Rectangles and fills

`G_TEXRECT` takes its tile from the command, not from the current `G_TEXTURE`
tile. The lower-right corner is exclusive, and an equal corner draws nothing.
A tile with `lrs == uls` or `lrt == ult` covers no texels and is skipped, matching
RT64. A rectangle's corner UVs get half a texel subtracted
(`OGRE_RECT_HALFPIX`, on by default) so pixel `i` samples texel `i`, which is
what RT64's shifted quad does.

`G_FILLRECT` is a full-screen or partial clear. Its colour is decoded in the
colour image's format: a 16-bit image takes the low 16 bits of
`G_SETFILLCOLOR` as RGBA5551, a 32-bit image takes the whole word as RGBA8888.
The shader turns the fill colour into a PRIM-only combiner. The fill also respects
the scissor.

### Alpha compare, coverage and dither

The shader implements `G_AC_THRESHOLD` and `G_AC_DITHER`, the `CVG_X_ALPHA`
coverage discard, and the `CVG_DST` target alpha. The `u_cvg_range` uniform is
255 for an RGBA8 target and 65535 for the RGBA16F target. The dither used for
alpha compare is RT64's `initRand`, seeded from the frame count and the draw
index.

The writeback dither is separate: see §7.

### Depth

The depth test and write flags come from `OTHERMODE_L` exactly as RT64 decodes
its pipeline flags: `depth_test = zCmp && !copy && zMode != ZMODE_DEC`, and
`depth_write = zUpd && !copy`. `G_ZS_PRIM` substitutes `G_SETPRIMDEPTH`'s screen
depth for every vertex z. The shader also applies the F3D depth clip for the
F3DEX2 path; F3DEX2.NoN disables it.

`ZMODE_DEC`'s decal tolerance is not implemented. Such a draw keeps its depth
write but loses the tolerance test.

## 6. Textures and TMEM

### TMEM

The renderer keeps a persistent 4 KiB TMEM and a persistent 256-entry palette
([web_renderer.cpp:6240](../../app/src/web_renderer.cpp#L6240)). RT64 holds one
`RDP::TMEM` for the run and does not clear it in `RDP::reset`, so a palette loaded
in one display list remains available in a later one. OB64 relies on this: the
title's CI8 sprites use a palette loaded several scenes earlier.

`G_LOADTILE`, `G_LOADBLOCK` and `G_LOADTLUT` copy their source into TMEM with
RT64's `loadWord` byte layout (`texture byte = RDRAM[address ^ 3]`)
([web_renderer.cpp:8063](../../app/src/web_renderer.cpp#L8063)). The load's
source stride is the `G_SETTIMG` width converted with the image's format
(`width << siz >> 1`), not the tile's `line`, which is the TMEM stride.
`G_LOADBLOCK` is contiguous; `G_LOADTILE` is a rectangle; `G_LOADTLUT` fills the
palette half.

A load is claimed by the next `G_SETTILE` that names a different tile. That is
the F3DEX2 idiom: the load always names tile 7, then the game configures the
render tile that samples the loaded image.

### Decode

`decode_tmem_tile` turns a tile into RGBA8
([web_renderer.cpp:1139](../../app/src/web_renderer.cpp#L1139)). It implements:

- RGBA16, RGBA32, IA16, IA8, IA4, I8, I4, CI8 and CI4;
- CI tiles through the palette, with the 2-bit LUT mode deciding whether an
  entry is RGBA16 or IA16; a CI tile with LUT mode 0 is decoded as I;
- YUV16, the RDP's two-plane format: one luma byte per texel at
  `offset + stride*t + s` in the upper TMEM half, one U/V pair per two texels in
  the lower half. The conversion uses the game's `G_SETCONVERT` matrix,
  sign-extended and scaled `2*K+1`, with the NTSC JPEG matrix as the fallback.

The palettes are `G_TT_RGBA16` and `G_TT_IA16`; the entry's own bit 15 does not
choose the decode.

### Caches

Two caches sit in front of the decode:

- `decoded_tiles_` on the gfx thread, capped at 16 MiB. The key hashes the TMEM
  bytes a tile can read, the decoder's mask and shift, the palette half for an
  indexed tile, and `convert_k` for YUV16. A hit skips `decode_tmem_tile`.
- `gl_textures_` on the main thread, capped at 32 MiB, 16384 entries and eight
  flushes. The key is XXH3-64 of the decoded RGBA8 bytes plus the size and wrap
  fields, so an unchanged reload reuses the resident texture.

The upload queue de-duplicates keys within one display list.

### Framebuffer-as-texture

A texture load whose source lies inside a colour image the RDP has drawn should
read the GPU's pixels instead of the RDRAM bytes, which may be stale. The
renderer supports this through a private shadow of the colour image. It is
opt-in with `OGRE_FBTEX=1`
([web_renderer.cpp:3955](../../app/src/web_renderer.cpp#L3955)). The gfx thread
publishes the draws so far, the main thread reads the image's drawn rectangles
into a buffer in the guest byte layout, and the load copies TMEM out of that
buffer. RDRAM is never written.

Before serving the copy, the renderer compares the image's RDRAM hash with the
hash at its last upload or writeback (RT64's `checkRAM`). A different hash means
the CPU wrote the image after the GPU copy was made, so the load reads RDRAM and
logs `[GFX-FBTEX-STALE]`. The GPU copy is not re-uploaded and the renderer has no
per-workload writeback, so the default stays RDRAM.

## 7. Framebuffers, writeback and presentation

### Colour and depth targets

Each guest colour image gets a `GlFramebuffer`
([web_renderer.cpp:2466](../../app/src/web_renderer.cpp#L2466)). The guest
address is the key. The attachment is RGBA16F when `EXT_color_buffer_float`
exists, and RGBA8 otherwise, which is the closest WebGL2 equivalent of RT64's
high-precision colour buffer. `OGRE_HPFB=0` forces RGBA8.

The framebuffer is created on the first draw into the image. Its width is the
colour image's width; its height is the tallest height any draw into it has asked
for, the way RT64's `FramebufferManager` keeps a framebuffer's height while its
width and size do not change. RDRAM's existing bytes are uploaded on creation, so
a partial frame keeps what the CPU wrote.

Depth is a separate GL renderbuffer, keyed by the `G_SETZIMG` address and the
size ([web_renderer.cpp:2559](../../app/src/web_renderer.cpp#L2559)). One depth
buffer serves every colour image that names it, as RT64's depth target does. The
buffer is cleared to 1.0 once when it is created or invalidated. It is
invalidated when a colour draw writes an address that a display list named with
`G_SETZIMG`, because that colour write has overwritten the depth contents in
RDRAM.

### Draw execution

`flush_commands` binds the colour image's framebuffer and the depth renderbuffer,
then sets the viewport, scissor, blend, depth test and uniforms, and issues one
`glDrawArrays` per `DrawCmd` ([web_renderer.cpp:4451](../../app/src/web_renderer.cpp#L4451)).
There is no draw merging; a measured merge saved about 1.5% on the mission and
was reverted.

The viewport is the draw's screen size at the top of the colour image. A 320x240
scene drawn into a 320x476 target would otherwise be stretched over 476 rows.
The scissor is half-open, matching the RDP and GL; a `(0,0)-(319,239)` scissor
leaves the last column and row unwritten, which is what RT64's target has.

The uniform block is uploaded only when this draw's values differ from the
previous draw's. `draw_uniform_key` is an FNV-1a over every value the block sets,
including the live debug and interpolation state
([web_renderer.cpp:4404](../../app/src/web_renderer.cpp#L4404)). A uniform added
to the block must be added to the key.

### Writeback

The renderer writes colour images back to RDRAM only when the game asks, through
`ogre_sync_framebuffers` (§2). It does not write on every present. Writing every
framebuffer on every present put 16-bit pixels over a live heap block in scene
`0x12` and crashed the game's allocator.

`readback_framebuffer` converts the GL attachment to the image's guest format
([web_renderer.cpp:3595](../../app/src/web_renderer.cpp#L3595)):

- RGBA5551 for a 16-bit image, with RT64's `Float4ToRGBA16` and the `G_CD` writeback
  dither. The dither pattern is the one the image's draws used most
  (`G_CD_MAGICSQ`, `G_CD_BAYER` or `G_CD_NOISE`); `G_CD_DISABLE` gives zero.
  A 32-bit image keeps the undithered RGBA8888 path.
- 16-bit values go into RDRAM at `off ^ 2` and `off ^ 2 + 1`, the runtime's
  halfword order.

Only the rows the RDP drew are copied. The target tracks `min_row`/`max_row`,
its exact rects, and a per-row RDRAM hash taken immediately before each draw
into that row (`era_row_hash`). The hash test skips a row the CPU changed after
its last draw and logs `[GFX-FBWRITE-STALE]`. A whole-image hash is not enough:
a 320x476 target at `0x025C00` contains the game's next framebuffer at
`0x04B400`, and that framebuffer's writeback used to drop the njpeg pass that
drew rows 0..239.

### Presentation

`update_screen` carries the VI registers that produced the frame
([web_renderer.cpp:5726](../../app/src/web_renderer.cpp#L5726)). The presenter:

1. computes the VI's visible area (the framebuffer minus the overscan crop);
2. resizes the canvas to that area, so the drawing buffer and the visible area
   are the same size and the blit covers every pixel;
3. finds the framebuffer containing `VI_ORIGIN_REG` and prefers the most
   recently written one, which is RT64's `findMostRecentContaining` rule;
4. blits the visible region with the COPY combiner and point sampling;
5. falls back to uploading RDRAM when no framebuffer contains the address;
6. clears the canvas black when the VI is blanked or has no visible size.

The canvas CSS box is a whole multiple of the drawing buffer (`css_scale_for`).
A 320x240 buffer keeps the 2x box; a 640-wide mode is shown 1:1. The page uses
`image-rendering: pixelated`, so a non-integer stretch would be a
nearest-neighbour resample and would move pixels by a row or column.

The scanout `fldRegs[].origin` selects the containing framebuffer. Sampling
starts at the framebuffer's row 0.

## 8. Performance model

The renderer records one command per draw and executes one GL draw per command.
The measured costs are:

- the gfx thread can contend on the queue exchange; the GL batch runs without the
  mutex;
- per-draw uniform uploads are skipped when the uniform key is unchanged, which
  took the mission's uniform calls from about 1.88M/s to 0.50M/s at GAME SPEED 4x;
- uniform locations are resolved once after the program links;
- the texture caches avoid decoding and uploading unchanged images;
- the per-flush `glGetError` guard is off by default (`OGRE_GL_CHECK=1` restores
  it) because it is a GPU sync.

Measured on this machine: the mission submits about 29-30 display lists/s, against
the game's 60 in native/retail. A profile of the mission put 87% of the main
thread idle, so the remaining cost is outside the flush loop. GAME SPEED 1x/2x/4x
hold 60/120/240 guest frames/s after the session-208 change. The remaining
overheads are the per-draw GL state calls (`glViewport`, `glScissor`, enable) and
the per-row writeback hashes.

The renderer targets native resolution: `get_resolution_scale()` returns 1.0 and
`update_config` returns false. There is no HD path, no MSAA and no frame
interpolation.

## 9. Diagnostics and test hooks

The renderer reads 25 `OGRE_*` environment switches, which the page also accepts
as query parameters. They fall into four groups:

| Group | Switches |
|---|---|
| A/B switches | `OGRE_DEPTH`, `OGRE_FBTEX`, `OGRE_FBWRITE`, `OGRE_FOG`, `OGRE_HPFB`, `OGRE_PERSP`, `OGRE_RECT_HALFPIX`, `OGRE_TEXF`, `OGRE_TEXWINCLAMP`, `OGRE_OVERSCAN`, `OGRE_ONLY_COMB` |
| Traces | `OGRE_TRACE_DL`, `OGRE_TEX_TRACE` (+ `_AFTER_MS`, `_MS`), `OGRE_TLUT_TRACE`, `OGRE_YUV_TRACE`, `OGRE_FOG_TRACE`/`_SUMMARY`, `OGRE_FRAME_DUMP` |
| Probes | `OGRE_DRAW_PROBE` (+ `_SCENE`, `_AFTER_TASK`), `OGRE_GL_CHECK` |
| Files and hooks | `OGRE_PREF_DIR` |

`ogre_gfx_debug_flags` is a live bitmask for the probes:

| Bit | Effect |
|---|---|
| 0 | ignore alpha blending |
| 1 | draw only the first sprite of a list |
| 2 | skip full-screen texrects |
| 3-4 | output TEXEL1 or TEXEL0 raw |
| 5 | framebuffer-as-texture positive control |
| 6 | framebuffer-as-texture negative controls |
| 7 | depth control |
| 8 | clear `Z_CMP` for the depth control |
| 9 | alpha-compare control |
| 10 | writeback-dither control |
| 11 | texture-cache control |
| 12 | source-change control |
| 13 | disable the framebuffer-source copy |
| 15 | live `OGRE_RECT_HALFPIX=0` |

`test_draw` builds a synthetic F3DEX2 display list in RDRAM and runs it through
the normal executor ([web_renderer.cpp:2727](../../app/src/web_renderer.cpp#L2727)).
It covers a fill, a textured rect, a textured quad, and the controls above.
`debug/probes/testdraw.cjs` drives it and asserts the readbacks. This is the
renderer's conformance test: it separates "the GL path is broken" from "the
game's display list is wrong".

The main log tags are `[GFX-WORKLOAD]`, `[GFX-DRAW]`, `[GFX-CMD]`,
`[GFX-RECT]`, `[GFX-TXR]`, `[GFX-TEXTRACE]`, `[GFX-SAMPLER]`, `[GFX-LIGHT]`,
`[GFX-DEPTH]`, `[GFX-VI]`, `[GFX-PRESENT]`, `[GFX-FBSYNC]`, `[GFX-FBTEX]`,
`[GFX-FBWRITE-STALE]`, `[GFX-TEXCACHE]`, `[GFX-S2D]` and `[GFX-FOG]`. The file
defines other one-shot `[GFX-*]` tags for specific diagnostics. All of them are
written at a bounded cadence, not per draw, so a run stays readable.

The probes and the comparison tools are documented in
[web-probes.md](web-probes.md). The browser frame/canvas comparison is
`tools/web_frame_cmp.py`; the native reference captures come from
`OGRE_CAPTURE_TARGET`.

## 10. Limitations

The current limitations are:

- **GBI coverage.** F3DEX2, F3DEX2.NoN and S2DEX2 only. No F3D, F3DEX,
  F3DZEX, S2DEX1 or extended GBI. Many commands are no-ops: `G_BRANCH_Z`,
  `G_CULLDL`, `G_MODIFYVTX`, `G_LINE3D`.
- **Effects.** `G_LIGHTING` is directional only; point lighting and `ZMODE_DEC`'s
  decal tolerance are not implemented. Copy-mode rectangles and fractional
  rectangles differ from RT64. `G_TEXRECTFLIP`'s half-step is wrong for
  non-square flips.
- **Display features.** Native resolution only, no MSAA, no HD, no frame
  interpolation, no texture packs, no path tracing. Widescreen is the page's
  counter-scaled projection, not a general RT64 aspect-ratio mode.
- **Framebuffer-as-texture.** Opt-in and unverified as a default. The copy is
  refused when RDRAM changed, and the GPU copy is not re-uploaded.
- **Performance.** About 29-30 display lists/s on the mission against 60 native.
- **Validation.** Screens are compared against RT64 captures. There is no
  automated full-game browser regression and no recorded full playthrough.

## 11. Where to change what

| Change | Location |
|---|---|
| Add a GBI or fix a hash | `kGbiTextSegments`, `identify_gbi` ([web_renderer.cpp:946](../../app/src/web_renderer.cpp#L946)) |
| Add or fix a display-list command | `exec_command` ([web_renderer.cpp:7093](../../app/src/web_renderer.cpp#L7093)) and the opcode constants in [gbi.hpp](../../app/src/gbi.hpp) |
| Add a vertex or lighting rule | `vertex_clip`, `compute_vertex_light`, `project_clip` ([web_renderer.cpp:8950](../../app/src/web_renderer.cpp#L8950)) |
| Add a combiner or blender input | `Combiner`/`Blender` ([web_renderer.cpp:297](../../app/src/web_renderer.cpp#L297), [web_renderer.cpp:422](../../app/src/web_renderer.cpp#L422)) and the fragment shader ([web_renderer.cpp:1338](../../app/src/web_renderer.cpp#L1338)) |
| Fix texture sampling | `kFragmentShaderSrc` and `record_state`'s tile setup |
| Add a texture format or TMEM rule | `decode_tmem_tile`, `tmem_load_command` ([web_renderer.cpp:8063](../../app/src/web_renderer.cpp#L8063)) |
| Fix a target's size, depth or writeback | `ensure_framebuffer`, `depth_target_for`, `readback_framebuffer` ([web_renderer.cpp:3350](../../app/src/web_renderer.cpp#L3350), [web_renderer.cpp:3595](../../app/src/web_renderer.cpp#L3595)) |
| Fix presentation or the canvas | `resize_canvas`, `present_framebuffer`, `blit_to_canvas` ([web_renderer.cpp:4135](../../app/src/web_renderer.cpp#L4135), [web_renderer.cpp:4276](../../app/src/web_renderer.cpp#L4276)) |
| Fix a frame-rate cost | `flush_commands`, `draw_uniform_key`, the texture caches ([web_renderer.cpp:4451](../../app/src/web_renderer.cpp#L4451)) |
| Add a diagnostic | the constructor's `getenv` block, `ogre_gfx_debug_flags`, the exports ([web_renderer.cpp:2569](../../app/src/web_renderer.cpp#L2569)) |
| Add a synthetic control | `test_draw` and `debug/probes/testdraw.cjs` |

The file is one translation unit of about 10,600 lines. The `// ====`
section markers separate the workload analysis, the math and state types, the
shaders, the fog repair and the renderer class. Split along those markers if the
file is ever extracted.

## 12. Related documents

- [app-architecture.md](app-architecture.md): where the app's renderer callback
  sits in the runtime.
- [app-build.md](app-build.md): `make web`, the `OGRE_*` knobs and the browser
  build's environment.
- [web-probes.md](web-probes.md): the headless browser probes and the scene
  capture routes.
- [njpeg-backgrounds.md](njpeg-backgrounds.md): the njpeg/S2DEX2 background path
  the renderer draws.
- [emulator-first.md](emulator-first.md): the five checks before treating a
  renderer difference as a game or port fact.
- [../WEB-PORT.md](../WEB-PORT.md): the 2026-08-29 feasibility plan. It is
  historical; this document describes the current design.
- [../DECISIONS.md](../DECISIONS.md): the measurements behind each rule, newest
  first.
- [../../PLAN.md](../../PLAN.md): current status and open work.
