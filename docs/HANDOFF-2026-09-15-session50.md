# Handoff — 2026-09-15, session 50: RT64's YUV16 decode is fixed (the black backdrop's cause); the readback's render timing is now isolated

## Goal and result

**Goal:** the New Game cathedral (scene `0x0D` step 2) has a black background
(`docs/proofs/native-newgame-cathedral-background.png`).

**Result: the first of the two independent causes is fixed and in the tree.**
RT64's texture decoder returned **black for every YUV texel**
(`TextureDecoder.hlsli`: `case G_IM_FMT_YUV: return float4(0,0,0,1)`), and the
njpeg background is drawn as `fmt=1 siz=2` YUV16 macroblock textures — so the
colour image the game reads back was uniformly black *by construction*. That
decoder is now implemented from the RSP decoder's byte layout and the game's own
`G_SETCONVERT` matrix (below), and it is no longer a stub.

**The second cause is measured, not fixed: the game's CPU readback runs before
RT64 has rendered the draw.** The game submits the `0x800A5110` macroblock draw
and, because the port completes the emulated RSP task as soon as the display list
is handed to RT64, immediately copies the framebuffer. At that instant
`framebufferManager` holds 2 entries, neither of them a game framebuffer, and the
copy source (`0x80000400`, or the njpeg target `0x000400` via the readback patch)
holds 3 non-zero bytes out of 7 680. This session added a **bounded wait on the
RSP worker** (`Application::waitForGameFramebuffers`) so the game's own task wait
covers the render; it works (the wait reports `found=1` immediately once the
draw's workload is built) but the four copies at the start of scene `0x02` still
see zeros — see §5, which corrects session 49's reading of those copies.

## 1. The bug: RT64 decodes every YUV texel as black

`tools/RT64/src/shaders/TextureDecoder.hlsli` had, in **four** format switches
(`sampleTMEM4b`, `8b`, `16b`, `32b`):

```hlsl
case G_IM_FMT_YUV:
default:
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
```

The njpeg draw is `[yuv] setTextureImage fmt=1 siz=2 width=16 address=0x001D1030…`
(one 16x16 tile per macroblock) `→ [yuv-at] … cimg=0x00000400`, so every one of
those 302 draw calls painted black. The CPU readback then copies the framebuffer
it chose, which is black — and that is the black backdrop. Session 49's mid-run
"the backdrop draws" reading of a 1280x715 present capture was the dialogue box
and sprites, exactly as its own postmortem says.

## 2. The correct YUV16 layout (from the code that writes it, not by guessing)

The TMEM layout is pinned by the RSP decoder that produces it — mupen64plus-rsp-hle
`jpeg.c`, `jpeg_decode_OB` (Bobby Smiles' upstream fix for
[mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102),
the "Missing backgrounds in Ogre Battle 64" issue) → `GetUYVY`:

```c
static uint32_t GetUYVY(int16_t y1, int16_t y2, int16_t u, int16_t v)
{
    return (uint32_t)clamp_u8(u)  << 24 |
           (uint32_t)clamp_u8(y1) << 16 |
           (uint32_t)clamp_u8(v)  << 8  |
           (uint32_t)clamp_u8(y2);
}
```

and `EmitTilesMode2` stores those words at `address` and `address + 32` with
`address += 64` per iteration — eight 32-bit words per 16-texel line, two texel
rows per line. For a 32-bit word at byte offset `4*(2t + (s >> 1))` the bytes are:

```text
byte0 = Y(s even)   byte1 = V   byte2 = U   byte3 = Y(s odd)
```

The RDP samples it as two planes: the luma byte in the **upper** TMEM half
(`OR 0x800`) and the chroma pair in the lower half, with the same XOR-3 / XOR-4
swap `sampleTMEM16b` already uses for 16-bit texels. `sampleTMEMYUV16` implements
exactly that (RT64's own `sampleTMEM` computes a one-texel-per-16-bit address,
which is wrong for YUV, so YUV gets its own path).

**This is the layout to keep.** Session 49's blobs came from inventing a byte
pairing; the pairing here is the one the decoder writes.

## 3. The conversion matrix is the game's own

`RDP::setConvert` already received `G_SETCONVERT` values but nothing read them.
`OGRE_CONVERT_TRACE=1` shows the game programming exactly one matrix:

```text
[convert] n=2 k0=175 k1=469 k2=423 k3=222 k4=114 k5=42
```

The hardware applies it to the sign-extended chroma bytes as

```text
R = Y + (K0*V + 0x80) >> 8
G = Y + (K1*V + K2*U + 0x80) >> 8
B = Y + (K3*U + 0x80) >> 8
```

\> `sampleTMEMYUV16` applies that matrix, passed down from the pixel shader
(`RasterPS.hlsl` already has `instanceRDPParams[instanceIndex].convertK`), and
falls back to the same NTSC matrix encoded as `175/469/423/222` when nothing has
been programmed (the compute-shader decode path cannot see draw state). K2 is
carried as `k2/32` and scaled back inside, because `G_SETCONVERT`'s K2 field
straddles a bit boundary.

## 4. What was verified

* `make`/`cmake --build build-app` and `build-null` are clean.
* `OGRE_CONVERT_TRACE=1` confirms the matrix above.
* `OGRE_YUV_TRACE=1` confirms 30 `setTextureImage fmt=1 siz=2` per image with
  `cimg=0x000400`.
* The forced shortcut (`OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1`)
  still reaches scene `0x0D` and renders the cathedral dialogue (Montage of
  presented frames: Archbishop Odiron, statues, candles, characters).
* **The background is still black in that scene**, because of §5.

## 5. Correction to session 49: the four copies are not yet shown to be the njpeg readback

Session 49 (and this session's first reading) treated the four
`func_ovlE_8019976C` stage-3 copies as the njpeg readback. Measurement says
otherwise:

| copy | width x height | at wall time (forced shortcut) |
|---|---|---|
| 1 | 320 x 240 | t=1435 ms |
| 2 | 176 x 240 | t=1941 ms |
| 3 | 320 x 144 | t=2448 ms |
| 4 | 176 x 144 | t=2952 ms |

Scene `0x0D` does not start until t≈3745 ms in that run, and the `[njwait]`
lines only start reporting `found=1` after that. So all four copies run **before
the code that submits the YUV draw has run** — the copy's source is
`0x80000400` (the ROM placeholder / table entry 0) and the njpeg scratch target
is `0x000000` for the first. The YUV draw's display list does pass `state[0x64]`
to `G_SETCIMG` (`0x801992ec: lw v0, 100(a0)` feeding the `0xED` command), so
`state[0x64]` *is* the njpeg colour image — the question is why the copy runs
before that state exists, and whether these four copies are the njpeg readback
at all. **Do not build on "the readback copies the njpeg framebuffer" until the
copy's caller is identified at instruction level**; the sizes above
(320x240, 176x240, 320x144, 176x144) are a 4-way tile decomposition, not one
320x240 image.

## 6. The render-timing fix that is in the tree

`app/src/renderer.cpp` (`send_dl`) now calls
`Application::waitForGameFramebuffers(OGRE_NJ_WAIT_MS, default 500)` **after**
`processDisplayLists`, i.e. on the RSP worker thread, not the game thread. The
game's own wait for that RSP task therefore also covers the render, which is what
the CPU readback needs. A bounded wait on the *game* thread deadlocks (the
renderer thread cannot always advance while a game thread is blocked); the RSP
worker can wait safely, and the wait returns as soon as a game framebuffer exists.

`OGRE_NJ_WAIT_MS=0` disables it. Observability: `OGRE_SYNC_TRACE=1` prints
`[njwait] spins=… found=… ms=… fbs=… queueId=… workloadId=…` and, from
`State::syncFramebuffers`, `[syncfb] enter:`.

## 7. Files changed

* `tools/RT64/src/shaders/TextureDecoder.hlsli` — `sampleTMEMYUV16` (YUV16 TMEM
  layout + G_SETCONVERT conversion) and `sampleTMEMWithConvert`; the old
  `sampleTMEM` is a thin wrapper so the compute-shader decode path is unchanged.
* `tools/RT64/src/shaders/TextureSampler.hlsli` — thread the convert matrix
  through `clampWrapMirrorSample` → `sampleTextureLevel` → `sampleTexture`.
* `tools/RT64/src/shaders/RasterPS.hlsl` — pass `convertK[0..3]` for both texture
  stages.
* `tools/RT64/src/hle/rt64_state.{h,cpp}` — `State::waitForGameFramebuffers`;
  `OGRE_SYNC_TRACE` now also prints the game-framebuffer pairs RT64 builds
  (`[njpair]`).
* `tools/RT64/src/hle/rt64_application.{h,cpp}` — pass it through.
* `app/src/renderer.cpp` — the bounded wait in `send_dl`; keeps the
  `OGRE_CONVERT_TRACE` hook in the RT64 side.
* `tools/njpeg_readback.py` — comment only: the render wait now lives on the RSP
  worker (a game-thread wait can deadlock).
* `docs/HANDOFF-2026-09-15-session50.md` (this file), plus `PLAN.md`,
  `DECISIONS.md`, `AGENTS.md`, `docs/README.md` updates.
* `tools/RT64` remains dirty (the project's own patches); the new shader and
  state changes belong in `rt64-ob64.patch`, which **has not been refreshed this
  session** — do that before relying on a clean-submodule apply.
* **No temporary probes left**: `grep -rn "probe49\|probe50\|probe51\|p50call\|p50draw"
  app/src/ tools/RT64/src/ Bank*Funcs/ RecompiledFuncs/` is empty; `build-app` and
  `build-null` were rebuilt after the cleanup.

## 8. Next lead (in order)

1. **Identify the caller of the four stage-3 copies.** If they are not the njpeg
   readback, the njpeg pipeline's CPU copy is elsewhere and the YUV decode fix is
   what will make *that* one produce the backdrop. `func_ovlE_8019976C` is a
   stage machine on `D_8019A680` (`state+0x7E` stages 0/2/3/5), and `state+0x64`
   is the njpeg colour image; log `state+0x7E` and `state+0x64` at each call to
   `func_ovlE_8019976C` to see whether the stage-5 copy ever runs with the njpeg
   target set.
2. **Check the tile `line` field for the YUV tile.** `stride` in RT64 is
   `tile.line << 3`; the working RT64 fork cited for this class of background
   reportedly also needed a `line << 4` fix for YUV texrects. `OGRE_WORKLOAD_TRACE`
   / a one-line trace of `rdpTile.stride` for `fmt=YUV` settles whether the 16x16
   macroblock tile is read as 16 or 32 bytes per line.
3. **Then re-run the A/B in session 49 §4** (framebuffer histogram of `0x000400`)
   with the decode in place, and check the assembled `'B5'` image at `0x80243E28`
   (`42 35 …` header at `0x80243E10`, pixels at `0x80243E28`) — it is still
   uniform in the last 20 s dump.

### Diagnostics that were used and are still available

```sh
# the shortcut to the cathedral (movie skipped)
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_SCENE_LOG=1 OGRE_SYNC_TRACE=1 OGRE_CONVERT_TRACE=1 \
  OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64 assets/ogre64.z64
# -> [njwait] …, [syncfb] …, [njpair] …, [convert] …

# capture the presented frames (a 1x natural run reaches 0x0D at ~33 s, and the
# game reaches the movie first — tap Start to skip it, or use OGRE_STEP)
OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 OGRE_CAPTURE_PRESENT=/tmp/v \
  OGRE_CAPTURE_AFTER=500 OGRE_CAPTURE_EVERY=200 OGRE_EXIT_AFTER_MS=180000 \
  OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D ./build-app/ogrebattle64 assets/ogre64.z64
```

No repo test harness exists; each check is a ROM-dependent run (the forced
shortcut lands in the cathedral ~1.4 s after boot at `OGRE_SPEED=6`).
