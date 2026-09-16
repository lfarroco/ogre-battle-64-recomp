# Handoff — 2026-09-15, session 52: the cathedral background renders — the YUV→RGB conversion matrix was never applied

## Goal and result

**Goal (session 51's open wall):** the New Game cathedral (scene `0x0D` step 2)
drew its njpeg background, but the sampled colours were wrong — the backdrop
rendered as blue horizontal banding instead of the cathedral.

**Result: the backdrop is now the cathedral** — red carpet, stone walls, pillars,
statues, candles, thrones — matching retail. `docs/proofs/native-newgame-cathedral.png`
(dialogue frame) and `docs/proofs/native-newgame-cathedral-background.png`
(background only) are the corrected captures; the assembled `'B5'` image the game
reads back (`0x80243E28`) also renders as the full scene.

The wall was **not** in the geometry (session 51's fix is correct), the TMEM
layout, or the source bytes. It was in the **`G_SETCONVERT` YUV→RGB conversion**:
RT64 fed the raw unsigned 9-bit `k0..k3` fields straight into the matrix and
paired `K1` with the wrong chroma channel. The RDP sign-extends each 9-bit field
and scales it `2*K+1`, and the green row is `K1*U + K2*V`.

## 1. The two defects, at instruction/reference level

### 1a. The conversion coefficients were never sign-extended or scaled

The game programs the matrix with `G_SETCONVERT`; `OGRE_CONVERT_TRACE=1` reports
the fields exactly as the display list carries them:

```
[convert] n=2 k0=175 k1=469 k2=423 k3=222 k4=114 k5=42
```

RT64's `GBI_RDP::setConvert` (`tools/RT64/src/gbi/rt64_gbi_rdp.cpp:144`) extracts
the raw 9-bit fields (`p0(13,9)`, `p0(4,9)`, `((p0(0,4)<<5)|p1(27,5))`, `p1(18,9)`,
…) into `RDP::convertK[6]`; nothing sign-extends them. Session 50/51's sampler then
used `convertK[0..3]` as the coefficients directly.

Both reference renderers that draw this game's backgrounds apply a transform first:

* **GLideN64** (the plugin that fixed
  [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102),
  "Missing backgrounds in Ogre Battle 64 battles and also some cutscenes"):
  `gDPSetConvert` (`src/gDP.cpp:959`) stores
  `k0..k3 = (SIGN(k,9) << 1) + 1` and `k4,k5` raw; `glsl_CombinerProgramBuilderAccurate.cpp`
  `YUV_Convert` converts with `uConvertParams = gDP.convert.k0..k3`.
* **parallel-rdp** `set_convert` (`parallel-rdp/rdp_renderer.cpp:3521`):
  `constants.convert[0] = 2 * sext<9>(k0) + 1` … and `texture_convert_factors`
  (`shaders/texture.h:487`) applies them.

For the game's fields this is:

| field | raw 9-bit | sign-extended | coefficient `2*K+1` |
|---|---|---|---|
| K0 | 175 | 175 | **351** |
| K1 | 469 | −43 | **−85** |
| K2 | 423 | −89 | **−177** |
| K3 | 222 | 222 | **445** |

Session 51 used 175/469/423/222 directly, so the green row came out strongly
*positive* (`G = Y + 1.83·V' + 1.65·U'`) instead of subtracting chroma.

### 1b. The green row paired `K1` with V instead of U

Both references give the conversion as:

```
R = Y + (K0*(V-128) + 128) >> 8
G = Y + (K1*(U-128) + K2*(V-128) + 128) >> 8
B = Y + (K3*(U-128) + 128) >> 8
```

(`texture_convert_factors`: `g = texel.b + (factors.y*texel.r + factors.z*texel.g)`
where `.r = U-128`, `.g = V-128`; `YUV_Convert` is identical modulo the 9-bit
truncation.) Session 51's comment even declared `.r = V - 128, .g = U - 128`,
i.e. the opposite channel order from `sample_texel_yuv16` (`i16x4(u-0x80, v-0x80,
luma, luma)`), and the code paired `K1` with V.

## 2. What was changed

* `tools/RT64/src/shaders/TextureDecoder.hlsli` — `sampleTMEMYUV16` now:
  * resolves the fallback matrix **before** the transform, so "no matrix
    programmed" still means the standard NTSC JPEG values (175/469/423/222);
  * sign-extends each 9-bit field (`x >= 256 → x - 512`);
  * applies the hardware's `2*K+1` scaling to `K0..K3`;
  * computes `G = Y + (K1*U' + K2*V')/256`, `R = Y + K0*V'/256`,
    `B = Y + K3*U'/256`, and saturates the result to `[0,1]` (the RDP clamps the
    texture output; both references leave it to the combiner, and the pixel
    result here is identical because the final framebuffer write clamps anyway).
* `tools/RT64/src/shaders/RasterPS.hlsl` — the `k0..k3` values handed to the
  sampler are now the raw fields; the old `convertK[2] / 32.0f` → `* 32.0f`
  round-trip (a workaround for the K2 field's split encoding) is gone, since the
  GBI decode already reassembles K2.
* Kernel-side YUV comments corrected.

The `K4`/`K5` **combiner** inputs are unchanged and were already right: RT64 uses
`convertK[4]/255`, `convertK[5]/255`, exactly GLideN64's
`_FIXED2FLOATCOLOR(gDP.convert.k4, 8)`.

## 3. How the defect was localized

The session-51 §7 "ranked next checks" all came back clean, which is what pushed
the search into the conversion:

* **The geometry is right** — `OGRE_S2D_TRACE` still shows 300 rectangles tiling
  the 320×240 target.
* **The loader and TMEM planes are right** — a temporary probe in
  `RDP::loadYUVTileToTMEM` dumped all 16 de-interleaved rows for the first two
  macroblocks; every row is populated and the bytes match the source UYVY.
* **The blue was a red herring for the loader.** Session 51 §7 argued pure blue
  (`0x003F`) was impossible from valid YUV under the K0..K3 matrix; that argument
  assumed the *corrected* coefficient scale. With the raw unsigned coefficients
  the green row is large and positive, and the whole picture is teal/olive. This
  experiment changed **two** variables at once (the coefficient transform and the
  green-row pairing) because both are required independently by the two
  references, so it does not isolate which one produced the saturated blue — a
  CPU replay of the corrected sampler over the probed TMEM reproduces the
  maroon/red of the cathedral, and the observed banding disappears when the
  matrix is corrected with nothing else changed. What *is* isolated is that the
  loader, the TMEM planes, the UYVY source and the geometry were each verified
  correct, so the conversion matrix was the remaining variable. The exact stage
  that saturated the old, wrong-matrix output to `0x003F` (the game's own
  `(TEXEL0 - K4)*K5 + TEXEL0` combiner, the framebuffer's RGBA5551 write, or the
  njpeg readback) was not pinned down, and is moot now.

## 4. What was run for verification

```sh
# the shortcut to the cathedral (movie skipped), with the corrected proof capture
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_CAPTURE_PRESENT=/tmp/s52_ok OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=12000 \
  ./build-app/ogrebattle64 assets/ogre64.z64        # present 980 = the dialogue frame

# the assembled 'B5' image the game reads back (scene 0x02 readback)
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_EXIT_AFTER_MS=9000 OGRE_DUMP_RDRAM=/tmp/s52_ok.bin \
  ./build-app/ogrebattle64 assets/ogre64.z64
# 0x80243E28 renders as the cathedral interior (red carpet, thrones, statues)

# default boot smoke (20 s) — exit 0, no asserts
OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64 assets/ogre64.z64

# attract-flow non-regression (the change is in the shared RasterPS/sampler path)
OGRE_SCENE=title OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/s52_title \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=26000 \
  ./build-app/ogrebattle64 assets/ogre64.z64     # story screen + opening card render
```

`cmake --build build-app -j` and `cmake --build build-null -j` are clean.
`OGRE_CONVERT_TRACE`, `OGRE_S2D_TRACE`, `OGRE_FB_WRITEBACK`, `OGRE_RECT_STATE`
and `tools/rdram.py` were the diagnostics; `tools/njpeg_readback.py`'s readback
patch is unchanged and still locates the right buffer.

**Handoff §11.2 of session 51 is now confirmed**: the scene-`0x02` readback
assembles the cathedral into `0x80243E28`.

## 5. Files changed

* `tools/RT64/src/shaders/TextureDecoder.hlsli` — the conversion in
  `sampleTMEMYUV16`.
* `tools/RT64/src/shaders/RasterPS.hlsl` — pass the raw `k0..k3` fields.
* `docs/proofs/native-newgame-cathedral.png`,
  `docs/proofs/native-newgame-cathedral-background.png` — **replaced**: the
  previous files were the black/blue-band broken renders, which had been mistaken
  for correct (the top of the real scene is dark, so black looked plausible).
* `PLAN.md`, `DECISIONS.md`, `AGENTS.md`, `docs/README.md`,
  `docs/guides/app-build.md`, `docs/HANDOFF-2026-09-15-session52.md` (this file).
* `rt64-ob64.patch` — refreshed.
* **No `app/` change** — the fix is entirely inside RT64.
* **No probes left**: the temporary `probe52` block in
  `tools/RT64/src/hle/rt64_rdp.cpp` was removed and both builds rebuilt;
  `grep -rn probe52 tools/RT64/src/ app/src/ Bank*Funcs/ RecompiledFuncs/` is empty.

## 6. Corrections to the record (AGENTS §2)

* Session 51 §7's "the remaining defect is in how the YUV tile is
  sampled/uploaded, not the conversion" is **wrong**: the addressing, the
  de-interleaved TMEM layout and the loader were all correct; the defect was the
  conversion coefficients and the green-row channel pairing.
* The "pure blue is impossible from valid YUV" argument in §7 assumed the
  corrected coefficient scale; with the raw unsigned coefficients the blue is
  exactly what the pipeline produces.
* Session 48's `native-newgame-cathedral-background.png` "the background
  renders" conclusion was **a black background mistaken for correct** — the real
  backdrop is a fully lit cathedral. Session 49/50's "still black" readings were
  the same wall seen from the other side (nothing was drawn yet, then the wrong
  matrix).

## 7. Next leads, in order

1. **The app's display-list analyzer is F3DEX2-only** (session 51 §10, still
   open): `OGRE_DL_ANALYZE`/`OGRE_DL_DECODE` report this S2DEX2 list as
   "no geometry". Make it report the GBI the renderer selected (or at least
   decode S2DEX2's `0xDA`/`0xDC`) before the next reader is misled by it.
2. **The `'B5'` sub-images 1..3** (176×240, 320×144, 176×144) are decoded too;
   only sub0 is blitted by scene `0x0D` (session 50 §5). Check whether they
   matter.
3. **The `G_OBJ_RECTANGLE`/`G_OBJ_SPRITE` rotation path** (session 51 §4 TODO):
   the object matrix's rotation terms are read but the rect is drawn
   axis-aligned. `S2DEX` and `S2DEXD` are both in the game's ucode table, so
   another scene may use the full object matrix.
4. Re-check the `OGRE_NJ_WAIT_MS`/`waitForGameFramebuffers` timing (session 50)
   now that the image is right: the port still completes the emulated RSP task as
   soon as the list reaches RT64.

No repo test harness exists; each check is a ROM-dependent run (the forced
shortcut lands in the cathedral ~0.4 s of wall time after boot at
`OGRE_SPEED=6`).
