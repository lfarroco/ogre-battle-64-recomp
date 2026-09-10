# Handoff — 2026-09-10 — session 20: the initial scene renders (title-screen sprite ring)

## Outcome

**The browser build now renders the game's title screen**, not a black canvas:
a ring of twelve character sprites placed around the screen centre, stable and
reproducible. The scene is byte-identical across captures 1.5 s apart (same
screenshot SHA-256 over 5 shots / 7.5 s), so it is a deterministic frame, not a
timing fluke.

The geometry pipeline is now correct end to end:

| metric (task 2, the first real title DL) | session 19 | now |
|---|---|---|
| triangles rejected (`w <= 0`) | 18 of 24 | **0** |
| draw cmds queued | 8 | **24** |
| NDC extent x | `[-1.19, 3.25]` | `[-0.55, 0.50]` |
| NDC extent y | `[-2.21, 1.70]` | `[-0.33, 0.27]` |
| canvas | ~21 000 non-black px of a stretched brown wedge | full sprite ring |

Every root cause found this session is in `app/src/web_renderer.cpp`; no
runtime (`tools/N64ModernRuntime`) changes were needed and the vendored patches
are unchanged.

## The renderer bugs (all fixed in `app/src/web_renderer.cpp`)

1. **`G_MOVEMEM MV_VIEWPORT` was a no-op.** The RSP viewport was never applied,
   so 3D geometry stayed in raw clip space. OB64 submits
   `vscale=[480 640 0 511] vtrans=[480 640 0 511]`, i.e. the fields are laid out
   **(y, x, pad, z)** as the microcode consumes them: index 1 (640) is the x
   scale, index 0 (480) the y scale, index 3 (511) the z scale. RT64's
   `RSP::setViewport` reads exactly those indices. The SDK manual's "index 0 is
   x" describes the *logical* Vp fields, not the byte layout OB64 writes - the
   ROM's own `{640,480,511,0}` constant at file offset `0x5C230` is a different,
   apparently unused, structure.
2. **`read_matrix()` was missing RT64's `j ^ 1` column-pair swap.** The runtime
   stores N64 words byte-reversed, so a plain 16-bit read returns each row's
   columns in the order 1,0,3,2 (`FixedMatrix::toFloat` in
   `tools/RT64/src/common/rt64_common.cpp` compensates the same way). Session
   19 fixed the int/frac split but not this, so **every** matrix was still
   transposed in column pairs. The tell was that no modelview decoded as an
   affine matrix (`m[15]` was never 1).
3. **The projection matrix `MUL` was ignored.** OB64 loads a perspective matrix
   (`PROJ|LOAD`) and then multiplies the *view* matrix onto it (`PROJ|MUL`).
   Overwriting on every projection matrix left `st.projection` holding only the
   second matrix, which is why the "projection" looked like a rotation. RT64
   keeps a single `viewProjMatrix` and composes `MUL` as `m * old`; the
   composite check (`world origin -> screen centre`, `m[11] = -1`) confirms it.
4. **Modelview `MUL` composed in the wrong order.** For the row-vector
   convention the new matrix applies to the vertex first: `M = m * M_old`, not
   `M_old * m`.
5. **F3DEX2 stores the PUSH bit inverted.** The SDK macro XORs `G_MTX_PUSH`
   into the flags (`0xDA380000` = `MODELVIEW|MUL|PUSH`, `0xDA380001` =
   `MODELVIEW|MUL|NOPUSH`), and RT64 does `p0(0,8) ^ pushMask`
   (`GBI_F3DEX2::matrix`). Reading the bit literally made every per-object
   matrix a no-push MUL, so the title DL's twelve object matrices **accumulated
   into one another** (its two `G_POPMTX` per object had nothing to pop) and the
   sprites drifted progressively off-screen.
6. **`push_back(vector.back())` UB.** `st.modelview_stack.push_back(
   st.modelview_stack.back())` dangles when the push reallocates. The path was
   dead until fix 5 made it live; it produced a canvas that was entirely black.
7. **The RDP ignores empty/inverted rectangles.** OB64's title DL ends with a
   `TEXRECT` that decodes (under this renderer's field order) as
   `xl=319 yl=239 xh=0 yh=0` with `s0=t0=0, dsdx=dtdy=0`. Drawing that span as a
   quad produced a **full-screen cover** that hid every sprite. RT64 drops empty
   rects (`RDP::fillRect` and `FixedRect::isEmpty`); both `draw_fill_rect()` and
   `draw_texrect()` now do the same. Removing that cover is what revealed the
   sprite ring.

## Evidence (browser probes, `/tmp/ogre-probe`)

```
[GFX-VP]  viewport addr=0x00186330 vscale=[480 640 0 511] vtrans=[480 640 0 511]
          -> scale=[160.00 120.00 127.75] trans=[160.00 120.00 127.75]
[GFX-MTX] mtx#3 flags=0x06 PROJ addr=0x001BF500 r2=[0.0000 0.0000 -1.0005 -1.0000] r3=[0.0000 0.0000 -2.0005 0.0000]
[GFX-MTX] mtx#4 flags=0x04 PROJ addr=0x001BF540 r3=[0.0000 0.0000 -81.8963 1.0000]
[GFX-MT2] cmd 288 mtx#7 flags=0x01 MODEL addr=0x001BF600 mv=[2] r3=[0.000 0.000 0.000 1.000]
[GFX-POP] cmd 305 pop n=1 mv=2      <- push/pop now balanced (was stuck at 1)
[GFX-POP] cmd 306 pop n=1 mv=1
[GFX-DRAW] task 2: cmds=1004 queued=24 rejected=0 ndc_x=[-0.55,0.50] ndc_y=[-0.33,0.27]
[GFX-TXR]  texrect xl=319 yl=239 xh=0 yh=0 ... oml=0x820E40 cyc=0 comb[15,15,31,3] texel0=(0,0,0,0)
[GFX-V]    draw 1 verts=3 tex=1 scale=(0.0500,0.0294) org=(0.0,0.0) sc=-32768 tc=-32768:
           (-0.070,0.086 u0.00,0.00) (-0.324,0.273 u66.00,38.00) (-0.070,0.273 u0.00,38.00)
```

Bisection used to find bug 7: the fragment shader was temporarily replaced with
(a) solid magenta -> screen fully magenta, (b) magenta only when
`cmd.textured`, (c) a per-draw-kind uniform (fill=black / texrect=green /
triangles=UV gradient) -> **screen fully green**, i.e. the texrect covered
everything. `probe-shots.cjs` takes a series of canvas screenshots so an
animating scene can be judged without relying on one capture moment.

## Open questions (ordered, with the data needed to settle each)

1. **`G_TEXTURE` scale (`sc`/`tc`) is not applied.** OB64 submits
   `0xD7000002` / `w1=0x80008000`, i.e. `sc = tc = 0x8000`, and RT64 converts
   vertex texcoords as `(s * sc) / (65536 * 32)` (= `s/64`, half of our `s/32`).
   Applying it made the sprites visibly *worse* - the v span drops from 38 to
   19 texels against a 34-row texture, truncating each character - so the
   renderer keeps the unscaled `s/32`. Either the vertex `s/t` unit, the loaded
   texture height, or the `timg_width` unit is off by ~2x; resolve before
   applying the scale. `[GFX-V]` logs `sc`/`tc` per draw.
2. **`TEXRECT`/`TEXRECTFLIP` field order.** RT64 reads `ulx/uly` from `w1` and
   `lrx/lry` from `w0` (`GBI_RDP::texrect`); this renderer reads the opposite.
   OB64's rect is inverted under our order and valid under RT64's. The
   empty-rect filter makes the two agree *for this DL*, but one of them is wrong
   for other games - check against the SDK `gsSPTextureRectangle` macro and a
   second game before changing it. Note `G_FILLRECT`'s macro
   (`funcs_3.c` around `0x80078E40`) really does build `w0=(ulx,uly)`,
   `w1=(lrx,lry)`, i.e. the two rect commands may genuinely differ.
3. **Combiner mux tables + 1-cycle mode.** `src_rgb`/`src_a` are still the
   approximation from session 19. Regenerate A/B/C/D from
   `tools/RT64/src/shared/rt64_color_combiner.h` (`colorInputA/B/C/D`,
   `alphaInputC`), and note that **in `G_CYC_1CYCLE` the RDP evaluates the
   *second* mux** (`ColorCombiner::run` -> `runCycle(..., 1, false, ...)`),
   whereas the shader currently uses cycle 0's. The title DL's cycle-0 mux is
   `A=ZERO B=ZERO C=ZERO D=TEXEL1` and cycle 1 is `... D=COMBINED`, so this
   matters as soon as the tables are correct.
4. **Texture binding is a single unit.** `cmd.tex_scale`/`tex_origin` come from
   the last `LOADTILE`, and the draw binds the last loaded texture rather than
   the tile `G_TEXTURE` selected; `TEXEL1` falls back to `TEXEL0`. Each title
   object loads *two* textures (16x34 and 20x34) and the DL has 24 texture loads
   for 12 quads.
5. **`timg_width` units.** `G_SETTIMG`'s width field is documented as 64-bit
   words; `decode_texture_rect()` uses it as a texel count for the row stride.
   The observed value is 20, which is suspiciously equal to the 20-texel tile
   width.
6. Untouched from earlier sessions: native-vs-browser divergence, latent
   unyielded poll loops, the milestone buffer's 32 KB truncation, and the stale
   hard-coded "boot-stall" message in `app/web/web.js` (it still prints while
   the game is visibly rendering).

## Instrumentation (kept, all capped)

- `[GFX-VP]` first 4 viewports with raw `vscale`/`vtrans` (task 2).
- `[GFX-V]` first 6 presented draws with all vertices, UVs, tex scale/origin and
  the raw `G_TEXTURE` `sc`/`tc`.
- `[GFX-TXR]` first 3 texrects with geometry, othermode, cycle type, the
  cycle-0 mux and the last-loaded texture's texel (0,0).
- `[GFX-DRAW]` / `[GFX-FLUSH]` / `[GFX-TEX]` / `[GFX-CMD]` / `[GFX-REJ]` from
  session 19, and the opt-in `OGRE_TRACE_DL=1` per-command trace.
- Removed this session: the `[GFX-MTX]`/`[GFX-MT2]`/`[GFX-POP]`/`[GFX-SEQ]`
  investigation dumps and the ASCII texture preview.

## Repro

```sh
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8
# serve the repo root on :8931 (COOP/COEP for pthreads)
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8931/app/web/index.html
node /tmp/ogre-probe/probe-final.cjs 10 45 /tmp/ogre-probe/run   # one capture
node /tmp/ogre-probe/probe-shots.cjs /tmp/ogre-probe/ts 8 45 5 1500  # time series
```

**Trajectory bifurcation is still real:** roughly one in two boots stays on the
idle trajectory; probes must retry (`probe-final.cjs` and `probe-shots.cjs` both
do).

## Files changed (tracked)

- `app/src/web_renderer.cpp` - all of the above.
- `docs/HANDOFF-2026-09-10-session20.md` - this file.
- `tools/RT64` - pre-existing submodule pointer change, not touched this session.
