# Handoff — 2026-09-15, session 51: the cathedral background's missing geometry is found and implemented (the ucode is **S2DEX2**, and its object commands were unmapped in RT64)

## Goal and result

**Goal:** the New Game cathedral (scene `0x0D` step 2) has a black background
(`docs/proofs/native-newgame-cathedral.png`).

**Result: the root cause of the missing geometry is identified and fixed, and the
whole njpeg display chain now runs end to end.** Session 50 §9 concluded that the
`0x800A5110` display list "carries texture loads and no geometry". **That is
wrong, and this session disproves it at instruction and hash level.**

* The ucode at `0x800A5110` is **`S2DEX 2.08`** (`GBIUCode::S2DEX2` in RT64), not
  F3DEX2. Verified by hashing the game's own ucode text and data against RT64's
  database: text 0x18C0 bytes → `0x9300F34F3B438634` = `S2DEX2_FIFO_2_08`;
  data 0x390 bytes → `0x50EF0DFBD3A8CD0F` = the same instance (§2).
* The two commands session 50 decoded as F3DEX2 `G_MOVEMEM`/`G_MTX` are S2DEX2's
  **`G_OBJ_MOVEMEM` (0xDC) and `G_OBJ_RECTANGLE_R` (0xDA)** — the *draw* command
  and its transform (§3).
* RT64's `GBI_S2DEX2` mapped **neither** of them, so every macroblock was loaded
  into TMEM and never drawn; the framebuffer the game reads back stayed empty.
  Both are now implemented, along with `G_OBJ_SPRITE`/`G_OBJ_RECTANGLE` and the
  four `objLoadTx*` stubs that previously `assert(false)` (§4).
* Live verification: **300 object rectangles tiling a 320×240 framebuffer
  exactly** (16×16 each, at `cimg=0x00000400`), one per njpeg macroblock (§5).
* The YUV16 half of the pipeline is also re-done: RT64's `LOADTILE` now
  **de-interleaves** a YUV tile into TMEM's two halves, and the YUV sampler was
  rewritten to the RDP's model (parallel-rdp's `sample_texel_yuv16`) (§6).

**Not yet fixed:** the sampled colours are still wrong — the background now draws
as blue banding rather than black (§7). Every intermediate stage is verified
correct, so the remaining defect is localised to the YUV **sampling/upload** step.

## 1. What scene 50 read wrong

Session 50 §9 and the `docs/README.md`/`AGENTS.md` summaries of it say the
`0x800A5110` list "loads 300 macroblocks into TMEM and emits no primitives …
what turns these loads into a picture — if anything does — has not been
identified".

That reading came from decoding the list with the **app's F3DEX2-named decoder**
(`app/src/gbi.cpp`, `OGRE_DL_ANALYZE` / `OGRE_DL_DECODE`). Under F3DEX2 the two
per-macroblock commands decode as `G_MOVEMEM`/`G_MTX`, which is why
`tri1=0 tri2=0 … texrect=0` looked like "no geometry". Under the GBI the ucode
actually is, `0xDA` **is** the geometry. Instruction-level confirmation in §3.

The build corroborates it: `func_ovlE_80199130` emits, per macroblock, exactly

```
FD30000F <texaddr>   SETTIMG  fmt=1(YUV) siz=2(16b) width=16
F5300400 07080200    SETTILE  t7 fmt=1 siz=2 line=2 tmem=0
E6000000 00000000    RDPLOADSYNC
F4000000 0703C03C    LOADTILE t7 uls=0 ult=0 lrs=60 lrt=60   (16 rows x 32 bytes)
DC070002 <ptr>       G_OBJ_MOVEMEM  -> the per-macroblock uObjSubMtx
DA000000 8019A390    G_OBJ_RECTANGLE_R -> the constant uObjSprite
```

(`lrs=60`/`lrt=60` are 1/4-texel units, i.e. 16 rows, and the SETTIMG address
advances by 0x300 = 768 per macroblock — the njpeg block size.)

## 2. The ucode is S2DEX2, proven by hash

The game keeps a `{text, data}` ucode table at ROM `0x3A2B0` (VRAM
`0x800A9EB0`): six static pairs whose data blocks carry the SDK's own ASCII
strings — `F3DEX`, `F3DEX.NoN`, `F3DEX.Rej`, `F3DLX.Rej`, `L3DEX`, `S2DEX`, plus
S2DEXD. The fifth text, `0x800A5110`, pairs with data `0x800AD590` = ROM
`0x3D990`, which contains `"RSP Gfx ucode S2DEX       fifo 2.08  Yoshitaka
Yasumoto"`.

`func_ovlE_80199588` (bankE, ROM `0x69020` region) sets the OSTask fields
directly, so the runtime sizes are known exactly:

```
+0x10 ucode=0x800A5110  +0x14 ucode_size=0x18C0
+0x18 ucode_data=0x800BD590 +0x1C ucode_data_size=0x390
```

RT64's database (`tools/RT64/src/gbi/rt64_gbi.cpp`) has, in `textSegments`:
`GBISegment{ 0x18C0, 0x9300F34F3B438634, { &S2DEX2_FIFO_2_08 } }` and in
`dataSegments`: `GBISegment{ 0x390, 0x50EF0DFBD3A8CD0F, { &S2DEX2_FIFO_2_08 } }`.

Hashing the ROM text at `0x35510` (0x18C0) and the data at `0x3D990` (0x390)
with **XXH3-64 over the word byte-reversed bytes** — which is what RT64 sees,
because the runtime stores host-order 32-bit words in RDRAM — reproduces both
constants exactly:

```
A5110 len 0x18C0 hosthash 0x9300F34F3B438634   (expected S2DEX2_FIFO_2_08)
3D990 len 0x390  hosthash 0x50EF0DFBD3A8CD0F   (expected S2DEX2_FIFO_2_08)
F540  len 0x1390 hosthash 0xCF55FAE288BFE48D   (expected F3DEX2_FIFO_2_08)
```

So RT64 **does** select `GBIUCode::S2DEX2` for this task — the GBI *selection*
was never the bug. The bug is that the S2DEX2 command map is incomplete.

## 3. What `0xDC` and `0xDA` really are

The SDK's own header (`gs2dex.h`, the `F3DEX_GBI_2` branch) gives S2DEX2's
opcode set:

```c
#define G_OBJ_RECTANGLE_R 0xda
#define G_OBJ_MOVEMEM     0xdc
#define G_OBJ_RECTANGLE   0x01
#define G_OBJ_SPRITE      0x02
#define G_SELECT_DL       0x04   /* …loadtxr 0x05-0x08, bg 0x09-0x0a, rendermode 0x0b */
```

and the macros:

```c
#define gSPObjMatrix(pkt, mptr)    gDma1p((pkt), G_OBJ_MOVEMEM, (mptr), 0, 23)
#define gSPObjSubMatrix(pkt, mptr) gDma1p((pkt), G_OBJ_MOVEMEM, (mptr), 2, 7)
#define gSPObjRectangleR(pkt, mptr) gDma0p((pkt), G_OBJ_RECTANGLE_R, (mptr), 0)
```

The ucode reads the object-matrix selector from the **low 16 bits** of the
command word (`0xDC070002` → `2` = sub matrix); this is confirmed independently
by DaedalusX64's HLE (`Ucode_S2DEX.h`, `DLParser_S2DEX_ObjMoveMem`: `index =
cmd0 & 0xFFFF`), which is the one reference HLE that runs this game's
backgrounds.

RT64's `S2DEX2_G_*` defines stopped at `OBJ_LDTX_RECT_R`/`RENDERMODE`; 0xDA and
0xDC fell through to `GBI_RDP::setup`'s table, which maps neither, so
`Interpreter::processDisplayLists` logged "unknown opCode" (suppressed in
release) and **advanced one word without doing anything**. The observed walk of
1821 commands is exactly consistent with that: 300 × (SETTIMG, SETTILE,
RDPLOADSYNC, LOADTILE, [ignored 0xDC], [ignored 0xDA]).

## 4. What was implemented (RT64)

* `rt64_gbi_s2dex.h/.cpp`: `objMoveMem`, `objSprite`, `objRectangle`,
  `objRectangleR`, plus a shared `drawObjRectangle` that reads the 24-byte
  `uObjSprite` and the 8-byte `uObjSubMtx`/24-byte `uObjMtx` **field by field
  from the guest's big-endian words** (RT64's RDRAM holds host-order words, so a
  direct struct cast swaps each 16-bit pair — and reverses the four `u8` fields
  of the sprite, so an explicit decode is the only safe reading), sets the render
  tile from the sprite (`fmt`, `siz`, `line = imageStride`, `tmem = imageAdrs`,
  `pal`), sets the tile size in the RDP's **1/4-texel** units
  (`(imageW >> 3) - 4`, u10.5 → inclusive `lrs`), and draws a texture rectangle.
  `imageW/imageH` are u10.5 and the sub-matrix supplies origin (s10.2) and base
  scale (u5.10); the object matrix supplies s15.16 rotation terms.
  **TODO (documented in the code):** the rotation terms are read but the rect is
  drawn axis-aligned; OB64's `...R` commands cannot rotate, so this is exact for
  it, but a rotated `gSPObjSprite` from another game will not rotate.
* `rt64_gbi_s2dex2.cpp/.h`: the four opcodes above registered on the S2DEX2 map,
  and both `reset` paths now initialise the object transform to the identity.
* `rt64_gbi_s2dex.cpp`: the three `objLoadTx{Sprite,Rect,RectR}` handlers
  previously ended in `assert(false)` with a `TODO: call doObjRectangle…`; they
  now perform the draw they were stubbed for.
* `rt64_rsp.h`: the S2D state gained the object matrix (identity by default).
* `OGRE_S2D_TRACE=1` prints the first 40 object commands (see the guide).

## 5. Verification: the geometry is exactly right

`OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 OGRE_S2D_TRACE=1`
prints, for the four `0x800A5110` submissions:

```
[s2d] rectR fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(0.00,0.00,1.0000,1.0000) rect=(0.0,0.0)-(16.0,16.0) cimg=0x00000400 texaddr=0x001D1030
[s2d] rectR fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(16.00,0.00,1.0000,1.0000) rect=(16.0,0.0)-(32.0,16.0) cimg=0x00000400 texaddr=0x001D1330
…
[s2d] rectR fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(304.00,0.00,1.0000,1.0000) rect=(304.0,0.0)-(320.0,16.0) cimg=0x00000400 texaddr=0x001D1030+20*0x300
[s2d] rectR … mtx=(0.00,16.00,…) …
```

i.e. a 20 × 15 grid of 16×16 rectangles covering the whole 320×240 colour image,
each sampling a different 768-byte njpeg macroblock, every sprite identical and
matching the game's own ROM constant at `0x8019A390` (`D_ovlE_8019A390`, ROM
`0x69630`: `objX=objY=0`, `scaleW=scaleH=1024`, `imageW=imageH=512` = 16 texels,
`imageStride=2`, `imageAdrs=0`, `imageFmt=1 (YUV)`, `imageSiz=2 (16b)`).

Before this change RT64 built **no** game framebuffer for those lists
(`[njwait] found=0`); now it does (`found=1`, `fbs` rising).

## 6. The YUV16 half: session 50's sampler could never have worked

Session 50 sampled YUV from `TMEM[(byteOffset ^ …) | 0x800]` (the **upper**
TMEM half) while RT64's `LOADTILE` wrote the tile **linearly** into the lower
4 KiB — so the luma plane was always zero and only chroma noise could ever come
out. (That is exactly what a dump showed: a colourful, luminance-free B5 image.)

The RDP's real model, from parallel-rdp's `sample_texel_yuv16`
(`parallel-rdp/shaders/texture.h`), is:

* one **luma byte per texel** at `tileOffset + stride*t + s`, in the **upper**
  TMEM half (`| 0x800`);
* one **U/V pair per two texels** at `tileOffset + stride*t + 2*(s>>1)` in the
  **lower** half, read as a u16 with `u` in the high byte and `v` in the low;
* `stride = tile.line << 3` (16 for `line=2`) — so one row is 16 luma bytes and
  8 chroma words, and a LOADTILE with `fmt=YUV` **de-interleaves** the source.
  RT64's generic linear `loadToTMEMCommon` cannot express that.

Implemented:

* `RDP::loadYUVTileToTMEM` (`rt64_rdp.cpp`, called from `loadTileOperation`'s
  non-deferred branch): per row, reads the 32-byte source row (8 words), writes
  the two luma bytes into the upper half and U/V into the lower half, applying
  the usual per-row 64-bit word flip (`^ 4`).
* `sampleTMEMYUV16` (`TextureDecoder.hlsli`) rewritten to that model, with the
  odd-row flip applied to the row-relative offset and luma/chroma read as bytes.
* `TMEMHasher::requiresRawTMEM` now returns true for a YUV16 tile: the format
  lives in both TMEM halves, so it must be sampled from raw TMEM rather than
  through a decoded single-plane RGBA image.

**The source layout is verified live, not inferred.** The RSP decoder's output
was read out of RDRAM during a run (bytes shown as RT64 stores them, host-order
words):

```
src row0: 27 9A 28 7A 24 96 25 7B 21 91 22 7C …
```

Reversing each 4-byte group gives the guest bytes `7A 28 9A 27`, i.e.
**`[U, Y(even), V, Y(odd)]`** — matching mupen64plus-rsp-hle's `GetUYVY`
(`u<<24 | y1<<16 | v<<8 | y2`). Decoding it as luma-first gives a violently
alternating luma (122, 40, 154, 39, …); as `[U,Y,V,Y]` it gives a smooth
luma (40, 39, 37, 36, …) and smooth chroma (U≈122, V≈152), which is what a
sepia cathedral looks like. Note this **corrects session 50's §2 prose**:
the word is `[U, Y0, V, Y1]`, not `[Y0, V, U, Y1]`.

The de-interleaved planes were then read back out of TMEM and are exactly right:

```
[yuv-load] luma:   28 27 25 24 22 21 21 21 23 24 24 26 27 28 29 2A
[yuv-load] chroma: 7A 9A 7B 96 7C 91 7D 90 7D 93 7C 98 7B 9A 7A 9B
```

(Those two `[yuv-load]`/`[load-tile]` probe blocks are **removed**; the kept
`OGRE_S2D_TRACE` is the documented diagnostic.)

## 7. The remaining wall: the sampled image, not the geometry

Everything upstream is now verified, and the background is still wrong: with the
current build the scene-`0x0D` composite shows the cathedral dialogue over
**blue horizontal banding** (an RDRAM dump of the colour image at exit renders
as bands) instead of black. Before this session it was black because nothing was
drawn; the banding proves the njpeg image now reaches the background.

Intermediate evidence, all from dumps of `OGRE_DUMP_RDRAM`:

* the njpeg source and the de-interleaved TMEM planes are correct (§6);
* the object rectangles and their targets are correct (§5);
* `0x80000400` (the draw's `G_SETCIMG`) contains `0x0001` per pixel (alpha set,
  colour zero) when the draw ran but no texture was sampled, and the assembled
  `'B5'` image at `0x80243E28` (320×240, pixels at +24 from the header at
  `0x80243E10`) alternates between uniformly black, chroma-only noise and
  garbled bands depending on how far the run got.

So the defect is in **how the YUV tile is sampled/uploaded**, not in the
geometry or the TMEM contents. Cheapest next checks, in order:

1. `G_OBJ_RECTANGLE_R`'s texture coordinates and the render tile's
   `uls/ult/lrs/lrt` (`OGRE_RECT_STATE=<y0>-<y1>` prints the tile the rect
   sampled: `fmt siz line tmem uls ult lrs lrt`), against `OGRE_S2D_TRACE`'s
   `stride=2 adrs=0` and the DL's `SETTILE t7 line=2 tmem=0`.
2. Whether the **raster** path or the **decode** path samples the tile. With
   `requiresRawTMEM` now true for YUV, the raster samples `gTMEM` directly; drop
   that change (one line) to A/B the decoded-texture path — the two give visibly
   different wrong pictures, which separates "my sampler is wrong" from "the
   upload/cache is wrong".
3. The chroma byte pair order in `sampleTMEMYUV16` (`u` at `chromaByte`,
   `v` at `chromaByte+1`). Swapping them changes the hue strongly and is a
   one-line A/B; the RDP's rule is `u` = high byte (parallel-rdp), but the
   *upload* of TMEM to the GPU is RT64's own.
4. The `dsdx`/`dtdy` convention. RT64's `RDP::drawRect` makes
   `texcoord_delta = dsdx * pixels / 1024`, so `1024` is 1 texel/pixel; the
   code uses exactly that. `GBI_S2DEX::bgCopy` uses `4 << 10` for what it
   believes is 1:1, so if the rect comes out 4× too zoomed, that is the
   discrepancy to explain.

## 8. What was run for verification

```sh
# the shortcut to the cathedral (movie skipped)
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 OGRE_SCENE_LOG=1 \
  OGRE_S2D_TRACE=1 OGRE_EXIT_AFTER_MS=9000 OGRE_DUMP_RDRAM=/tmp/dump.bin \
  ./build-app/ogrebattle64 assets/ogre64.z64

# default boot smoke (20 s) — clean, no asserts
OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64 assets/ogre64.z64
```

Both runs exit 0 with no assert/abort. `cmake --build build-app` and
`cmake --build build-null` are clean. `tools/rdram.py` (word/half/hexdump) was
used for every dump read; `docs/guides/app-build.md` §"Reading a dump" applies.

## 9. Files changed

* `tools/RT64/src/gbi/rt64_gbi_s2dex.{h,cpp}` — the S2DEX object commands, the
  sprite/matrix decode and the object draw; the `objLoadTx*` stubs.
* `tools/RT64/src/gbi/rt64_gbi_s2dex2.{h,cpp}` — the S2DEX2 opcodes
  (`G_OBJ_RECTANGLE`/`G_OBJ_SPRITE`/`G_OBJ_RECTANGLE_R`/`G_OBJ_MOVEMEM`), their
  registration, and the object-transform reset.
* `tools/RT64/src/hle/rt64_rsp.h` — the S2D object-transform state.
* `tools/RT64/src/hle/rt64_rdp.{h,cpp}` — `loadYUVTileToTMEM` and its call from
  `loadTileOperation`.
* `tools/RT64/src/shaders/TextureDecoder.hlsli` — `sampleTMEMYUV16` rewritten to
  the RDP's two-plane model.
* `tools/RT64/src/common/rt64_tmem_hasher.h` — YUV16 tiles require raw TMEM.
* `rt64-ob64.patch` — **refreshed and verified**: it is
  `git -C tools/RT64 diff HEAD -- . ':(exclude)src/contrib/plume'` (18 files),
  and applying it to a **clean** submodule worktree (`git worktree add --detach
  HEAD`) reproduces the dirty tree exactly (`diff` of the two `diff HEAD`
  outputs is empty). The session-50 copy is at `/tmp/rt64-ob64.patch.s50`.
* `docs/HANDOFF-2026-09-15-session51.md` (this file), plus `PLAN.md`,
  `DECISIONS.md`, `AGENTS.md`, `docs/README.md`, `docs/guides/app-build.md`.
* **No `app/` change was needed** — the fix is entirely inside RT64.
* **No probes left**: `grep -rn "probe51\|OGRE_YUV_LOAD_TRACE" tools/RT64/src/
  app/src/ Bank*Funcs/ RecompiledFuncs/` is empty, and both builds were rebuilt
  after the cleanup. The `[load-tile]`/`[yuv-load]` traces in §6 were probes and
  are gone; `OGRE_S2D_TRACE` is kept and documented as a normal diagnostic.

## 10. Corrections to the record (AGENTS §2)

* Session 50 §9's "the `0x800A5110` display list carries texture loads but no
  geometry" is **wrong** — corrected above and in `AGENTS.md`/`PLAN.md`. The
  list's `0xDA` command *is* the draw; session 50's decoder named it `G_MTX`
  because it decodes every list as F3DEX2.
* Session 50 §2's byte layout `byte0 = Y(s even), byte1 = V, byte2 = U,
  byte3 = Y(s odd)` is wrong: the live decoder output is `[U, Y(even), V,
  Y(odd)]` (§6).
* Session 50 §2/§3's sampler design (luma at `| 0x800` with an XOR-3 byte swap,
  chroma via a half-word XOR) cannot work with RT64's TMEM, because the tile load
  never wrote the upper half. Superseded by §6.
* The app's own DL decoder (`app/src/gbi.cpp`, used by `OGRE_DL_ANALYZE` /
  `OGRE_DL_DECODE`) is **F3DEX2-only and misleads on S2DEX2 lists**. It still
  reports this list as `tri1=0 … texrect=0`. Making it S2DEX2-aware (or having it
  report the GBI the renderer selected) is worth doing before the next person
  reads it.

## 11. Next leads, in order

1. **Finish the YUV16 sampling** (§7). The pipeline is correct up to TMEM; the
   bug is now one of addressing units, byte order, or the upload path.
2. Re-run the scene-`0x02` readback A/B (session 49 §4) once the image is right,
   and confirm the assembled `'B5'` image at `0x80243E28` looks like the
   cathedral (`docs/proofs/native-newgame-cathedral.png` shows the target).
3. The `'B5'` sub-images 1..3 (176×240, 320×144, 176×144) are decoded too; only
   sub0 is blitted by scene `0x0D` (session 50 §5), so check whether they matter.
4. The `G_OBJ_RECTANGLE`/`G_OBJ_SPRITE` rotation path (§4 TODO) if another scene
   uses the full object matrix (`S2DEX` and `S2DEXD` are both in the game's ucode
   table, so other scenes may use the other object commands).

No repo test harness exists; each check is a ROM-dependent run (the forced
shortcut lands in the cathedral ~0.4 s of wall time after boot at
`OGRE_SPEED=6`).
