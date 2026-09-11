# Handoff — 2026-09-11 — session 23: the title sprites are decoded the way the RDP samples them

## Outcome

The title scene's twelve character sprites now render as themselves. Sessions
19–21 had the geometry right and the *colours* wrong in a way that looked like
noise: the sprites came out as thin vertical slivers of scrambled hue. Five
independent decode bugs were in the way, and the last one (the vertex byte
order) had been silently transposing every vertex since the renderer existed.

The decisive evidence is a new probe, `debug/probes/spritecheck.cjs`: it asks the
renderer to draw **one** sprite (`ogre_gfx_debug_flags(2)`) and puts the canvas
crop next to the `colour × mask` composite of that same frame's first two tile
loads. They match — same silhouette, same orange/amber body, same magenta and
green accents, same dark outline. That is an end-to-end check of the whole path
(vertex transform → texture decode → combiner → blender → GL), not a pixel-count
proxy.

| | session-21 baseline | now |
|---|---|---|
| `boot.cjs` default boot | 7 988 non-black / 5 852 colorful, slivers | **21 680 / 16 492**, twelve readable characters |
| sprite quad | 39x23 px (x/y swapped) | 23x39 px, matches the 20x34 sprite sheet |
| `TEXEL0` (alpha mask) | 16x34 I8 | **32x34 I4** (the sampling tile's format) |
| `TEXEL1` (colour) | 20x34 RGBA16 | 20x34 RGBA16 (unchanged) |
| texture coordinates | `s/32` (G_TEXTURE scale ignored) | `s * sc / (64 * 32)` = `s/64` |
| blend | `blend=0` (heuristic read the wrong bits) | `ablend=1 b1=[P=CC M=FB A=CC_A B=1MA]` (RT64 `G_RM_XLU_SURF`) |
| vertex decode | `(y,x) (flag,z) (t,s) (a,b,g,r)` | `(x,y) (z,flag) (s,t) (r,g,b,a)` |

No `[GFX-ESCAPE]`/`[GFX-RUNAWAY]` regressions; the intro still reaches the same
display-list counts.

## The five bugs

### 1. Vertex fields were read at their logical offsets in a byte-reversed rdram

`recomp.h` defines the word-reversed accessors explicitly:

```
#define MEM_W(offset, reg) (*(int32_t*)(rdram + addr))
#define MEM_H(offset, reg) (*(int16_t*)(rdram + (addr ^ 2)))
#define MEM_B(offset, reg) (*(int8_t*)(rdram + (addr ^ 3)))
```

so the 16-bit field at logical offset `o` inside a 32-bit word lives at `o ^ 2`.
`read_matrix()` has always compensated (its `c ^ 1` column index is exactly that
`^ 2`), but `OP_VTX` read `rd16(p + 0/2/4/…)` directly. Every halfword pair came
out swapped, i.e. the renderer decoded an F3DEX2 `Vtx` as

```
(y, x) (flag, z) (t, s) (a, b, g, r)
```

instead of `(x, y) (z, flag) (s, t) (r, g, b, a)`. Two consequences were visible
at once and neither was obvious in isolation: the sprite quads were transposed
(39x23 instead of 23x39) and their texture coordinates were transposed with
them, so a 20x34 portrait sprite was sampled 34x20 and looked like static. The
ring survived the transposition because a ring is nearly symmetric — which is
why this hid for five sessions.

`gbi.hpp` now has `n64h()`/`n64b()` helpers next to `rd16()`, and `OP_VTX` uses
them with the *named* N64 offsets, so the compensation is one line instead of a
layout nobody can check.

### 2. `G_TEXTURE`'s scale was ignored

RT64 `RSP::setVertexCommon` computes the texel coordinate as

```
tc = (s * sc) / (65536 * 32)
```

with `sc` zero-extended from the 16-bit field. OB64's title sprites use
`sc = tc = 0x8000` (0.5), so the correct coordinate is `s/64`; the renderer used
`s/32` and every sprite sampled twice the texture it should have. The fix is
`tex_scale_s`/`tex_scale_t` on `RenderState`, applied in `transform_vertex` —
and deliberately **not** applied to `G_TEXRECT`, matching RT64's
`RDP::drawRect`, which uses `uls/32` straight.

Note the field must be read unsigned: the session-21 code cast it to `int16_t`,
so `0x8000` became `-32768` in the logs.

### 3. I8/I4 texels did not carry intensity into alpha

`decode_texture_rect` set an I8 texel to `(v, v, v, 255)` and an I4 texel to
`(i, i, i, 255)`. RT64's `I8ToFloat4`/`I4ToFloat4` return the intensity for
*all four* channels, and the title sprites take their alpha from
`TEXEL0_ALPHA` — so the mask was a constant `alpha = 1` and did nothing at all.
`RGBA16ToFloat4`'s bit replication (`(c << 3) | (c >> 2)`) was adopted at the
same time so the two renderers agree bit for bit.

### 4. The othermode decode read the wrong word, and the blend "heuristic" knew nothing

`G_SETOTHERMODE_H`/`_L` carry the **already shifted** data word, so only the
target field is cleared (RT64 `RSP::setOtherMode*`). The old code masked and
shifted it again, which zeroed every field — `OTHERMODE_H` was always 0, so
every draw was `G_CYC_1CYCLE` with no filtering and no perspective correction
(session 21 fixed that part). The *blend* was still guessed from
`othermode_l & 0xFFF` with a two-entry heuristic, which reads the wrong bits
entirely: OB64's sprites are `oml = 0x00184240`, `blend = 0x240`, and the
heuristic looked at bits 6–11 of that, so alpha blending was **off** for the
whole title scene and the mask's alpha was thrown away.

`Blender` now mirrors `rt64_blender.h`: `OtherMode::blenderInputs` (L bits
16–31, packed `P1 P0 A1 A0 M1 M0 B1 B0`), `checkEmulationRequirements`,
`usesAlphaBlend`, and a field-for-field port of `run`/`runCycle` into the
fragment shader (including both approximations). The GL blend is
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA` when — and only when — `usesAlphaBlend` says
so; RT64 uses dual-source blending with the factor in the secondary output,
which is equivalent here because the primary output's alpha carries the same
value and the canvas has no alpha channel.

The title sprite render mode decodes to exactly the classic two-cycle XLU:
`b0=[P=CC M=CC A=CC_A B=ONE]`, `b1=[P=CC M=FB A=CC_A B=1MA]`, `FORCE_BL` set.

### 5. The texture was decoded with the *load*'s format and rect, not the *tile*'s

The RDP does not sample the source image — it samples TMEM, addressed by the
**render tile**, whose `fmt`/`siz`/`line` need not match the load's. OB64 uses
the standard `gDPLoadTextureBlock` idiom and it matters here:

```
SETTIMG fmt=4 siz=1 w=16 addr=0x801C41E0      <- I8, 16 bytes/row
SETTILE t7 fmt=4 siz=1 line=2 tmem=365        <- the LOAD tile (G_TX_LOADTILE = 7)
LOADTILE t7 uls=0 ult=0 lrs=62 lrt=132        <- 16 x 34 units
SETTILE t0 fmt=4 siz=0 line=2 tmem=365        <- the RENDER tile: 4-bit!
SETTILESIZE t0 uls=0 ult=0 lrs=124 lrt=132    <- 32 x 34 texels
SETTIMG fmt=0 siz=2 w=20 addr=0x801C3C88      <- the colour image
SETTILE t7 fmt=0 siz=2 line=5 tmem=0
LOADTILE t7 uls=0 ult=0 lrs=76 lrt=132        <- 20 x 34
SETTILE t1 fmt=0 siz=2 line=5 tmem=0
SETTILESIZE t1 uls=0 ult=0 lrs=76 lrt=132     <- 20 x 34
```

So the mask is a **32x34 I4** image whose 16 bytes per row were loaded with
8-bit addressing (each byte is two 4-bit texels), and the sampled `u` range
`0..19` covers exactly its opaque region. Decoding the load's 16x34 I8 rect
instead produced a thin silhouette that did not match the character at all.

`RenderState` now tracks a *pending load* (set by `LOADTILE`/`LOADBLOCK`,
claimed by the next `SETTILE` that names a different tile) and each tile carries
the source address plus a lazily decoded image. `ensure_tile_image()` decodes
the tile's rect at the tile's format with `line * 8` as the row stride — the
same three inputs RT64 uses (`RDPTile` + `GPUTile`) — and one upload is queued
per key per display list (a sprite's two triangles share it). The old
`recent_loads[2]` "last two loads are TEXEL0/TEXEL1" model is gone.

`decode_texture_rect` now takes an explicit `row_bytes`; the previous
`width * bytes_per_texel` was only accidentally right when the load's format
matched the tile's.

## How the sprite path was actually verified

Guessing from screenshots was actively misleading (a 20x37 character at 1:1
through a 2x canvas, re-encoded to JPEG for inspection, looks like noise).
Two new probes exist for this and both are in the repo:

- `debug/probes/textures.cjs` — dumps the last 24 decoded images (a whole
  frame's `mask, colour` pairs) as `<n>-WxH-t<tile>-f<fmt>s<siz>-rgb.png` and
  `-alpha.png`, **plus** `-pair-NN-WxH.png`, the `colour × mask` composite of
  each pair. `ogre_gfx_debug_tex(i, &w, &h, &tile, &fmt, &siz)` exposes the
  pixels; the probe composites over a checkerboard for alpha and opaquely for
  RGB (a colour image usually has `alpha = 0` everywhere and a mask is white,
  so either view alone hides half the story).
- `debug/probes/spritecheck.cjs` — sets `ogre_gfx_debug_flags(2)` so the
  renderer draws only the first sprite, reads the canvas back with
  `readPixels`, and writes `sprite-compare.png` with the rendered crop beside
  the composite. **This is the probe to run before believing any sprite fix.**
  (The readback must not clear the canvas immediately before `readPixels`: the
  main thread only issues GL from `ogre_gfx_flush()` every 16 ms, so a clear in
  the same frame reads black.)

`ogre_gfx_debug_flags` bits: `1` = ignore alpha blending (writes the combiner
colour opaquely, which separates "colour pipeline" from "mask"), `2` = draw only
the first sprite.

## Diagnosing the tile recipe

`[GFX-TDSTATE]` (first three textured draws) prints the `G_TEXTURE` state, the
image, every programmed tile's `fmt/siz/line/tmem/rect`, and a 20-entry ring of
the `SETTIMG/SETTILE/SETTILESIZE/LOAD*/TEXTURE` commands that led to the draw.
That ring is what produced the recipe above; without it the load-vs-tile
distinction is guesswork.

Careful: a milestone line is capped at 511 bytes (`milestones.hpp`'s
`char line[512]`), so `[GFX-CMD]`'s trailing fields (scissor, `p0`, `uv0`) were
being silently truncated. Put new diagnostics in a short line of their own.

## Verified

- `spritecheck.cjs`: rendered sprite matches its `colour × mask` composite.
- `textures.cjs`: mask `32x34 t0 f4s0`, colour `20x34 t1 f0s2`, and the six
  title combiners/logs agree with the ROM words (`cmb=0xFCFFFFFFFFFD7238`).
- `boot.cjs`: `RENDERED` 21 680 non-black / 16 492 colorful, twelve characters.
- `logmode.cjs`, `stats.cjs`, `progress.cjs`: see the result files under
  `debug/out/` (`s23-logmode`, `s23-stat`, `s23-progress`, `s23-final`).

## Still open

1. **The idle trajectory is still the gate.** Nothing here changes the pacing:
   ~half of all boots submit only the boot blanking display list. Session 21's
   question stands — who sends to t5 (`0x800E9BA8`) and t16 (`0x800B9C40`)?
2. **A reference frame.** The sprite path is now internally consistent and
   faithful to RT64, but no session has compared the finished frame against the
   real title screen. `nonBlack` rose from 7 988 to 21 680 because the sprites
   are now ~20x37 of *drawn* character rather than a scrambled subset; a
   side-by-side against an emulator (or against the game's own asset sheet) is
   the next fidelity check, not another log line.
3. **LOADBLOCK's 2D shape.** The tile-rect model removes the old `dxt`
   approximation for the loads this scene uses, but a `G_LOADBLOCK` that fills a
   tile wider than one TMEM row still needs `dxt`/`line` reasoning (RT64
   `loadToTMEMCommon` / `GPUTile`).
4. **TMEM is still not modelled.** Two tiles that alias the same `tmem` (or a
   load that wraps) will decode differently from hardware. The current model
   decodes each tile's rect from the source image, which is right whenever the
   tile's data came straight from a load of that image.
5. Untouched from session 21: combiner inputs `NOISE`/`K4`/`K5`/`LOD_FRACTION`
   /`PRIM_LOD_FRAC`/`KEY_CENTER`/`KEY_SCALE` are still 0, coverage/alpha-compare
   is ignored, framebuffer/VI indirection is still direct-to-canvas, and the
   other ~40 combiners OB64 uses are unchecked.

## Repro

```sh
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8
python3 debug/server.py &                          # :8931, COOP/COEP for pthreads
node debug/probes/spritecheck.cjs --attempts 3 --secs 60 --out sprite   # does one sprite match?
node debug/probes/textures.cjs --attempts 3 --secs 60 --out tex         # what was decoded
node debug/probes/boot.cjs --attempts 4 --secs 55 --out final           # the whole frame
node debug/probes/progress.cjs --attempts 4 --secs 70 --out prog        # pacing, bad walks
```

JS-only changes (`app/web/`) need no rebuild; a renderer change needs
`cmake --build build-wasm` and a page reload (`server.py` sends `no-store`).

## Files changed (tracked)

- `app/src/web_renderer.cpp` — `Blender` (RT64 decode + `usesAlphaBlend` +
  `run`/`runCycle` in GLSL), I8/I4 intensity-into-alpha, `RGBA16` bit
  replication, `n64h()`/`n64b()` vertex decode, `G_TEXTURE` scale,
  tile-based image model (`pending_load`, `TileState::image`,
  `ensure_tile_image`, `decode_texture_rect(row_bytes)`), `[GFX-TDSTATE]`,
  `ogre_gfx_debug_tex`/`_count`/`ogre_gfx_debug_flags`.
- `app/CMakeLists.txt` — export the three new debug entry points.
- `debug/probes/textures.cjs` (new), `debug/probes/spritecheck.cjs` (new).
- `debug/package.json`, `debug/README.md`, `docs/guides/web-probes.md` — the
  two new probes.
- `docs/DECISIONS.md`, `PLAN.md`, this file.
