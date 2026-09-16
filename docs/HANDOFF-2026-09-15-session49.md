# Handoff — 2026-09-15, session 49: the cathedral background is still black (a wrong YUV16 decode was tried and reverted)

> **Superseded (session 52):** the background renders. The session-50/51
> sampler/geometry work plus session 52's `G_SETCONVERT` conversion fix made
> `docs/proofs/native-newgame-cathedral-background.png` a correct cathedral
> capture — the file referenced below has been **replaced**, so the "is black"
> readings in this handoff describe the state on 2026-09-15 session 49, not the
> current file. See `docs/HANDOFF-2026-09-15-session52.md`.

## Goal and result

**Goal:** the New Game cathedral (scene `0x0D` step 2) has no background
(`docs/proofs/native-newgame-cathedral-background.png` is black).

**Result: the background is still black. This session did not fix it.** A YUV16
decoder was written for RT64's texture shader on the hypothesis that the black
backdrop was RT64 decoding every YUV texel as black. That hypothesis is
**wrong**, the decoder was **wrong**, and it was **reverted** (the RT64 working
tree is back to its pre-session shader). The only useful outputs are the
measurements in §2 and the A/B method in §4.

## 1. What was tried and why it was wrong

`TextureDecoder.hlsli` had:

```hlsl
case G_IM_FMT_YUV:
default:
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
```

The njpeg macroblock draw genuinely does use `fmt=1 siz=2` YUV16 textures
(`[yuv] setTextureImage fmt=1 siz=2 width=16 address=0x001D1030…`, one per
macroblock), so "the shader returns black for exactly this format" looked like
the wall. A decoder was added (`sampleTMEMYUV16`, byte pairs `(V0,Y0) (U0,Y1)`,
NTSC matrix, even texel takes the previous byte as chroma).

**It is wrong.** With it the YUV draws land in the framebuffer as a **corrupt
blob** (banded magenta/green/yellow noise over a green field), not the cathedral —
see the A/B in §2. The byte pairing and/or the TMEM byte offsets are wrong; the
author (this session) never validated the decode against a known-correct YUV
implementation, only against "the picture is no longer uniform", which is not
evidence of correctness.

**It has been reverted.** `git -C tools/RT64 diff src/shaders/TextureDecoder.hlsli`
is empty; `build-app` was rebuilt from the original shader and the cathedral is
still black (baseline re-confirmed, §4).

## 2. The A/B that settles it (one variable, comparable timing)

Baseline = pristine shader; fixed = the (wrong) YUV16 decoder. Both:
`OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1` (forced step, so
scene `0x0D` enters at ≈1.15–1.25 s in both). Framebuffer `0x000400` read from
`OGRE_DUMP_RDRAM`, decoded as RGBA16, 320x240 (76 800 pixels):

| build | exit | scene 0x0D | distinct values | near-black (`<=0x10`) |
|---|---|---|---|---|
| baseline | 1 450 ms | yes | 5 | 76 314 |
| baseline | 2 500 ms | yes | 5 | 76 573 |
| YUV decoder | 1 400 ms | (0x0D at t=1.17 s) | 1 259 | 64 439 |
| YUV decoder | 1 600 ms | (0x0D at t=1.17 s) | 1 244 | 64 904 |
| YUV decoder | 2 000 ms | yes | 5 | 75 435 |
| YUV decoder | 2 600 ms | yes | 5 | 76 023 |

So the decoder changes the framebuffer from black to **garbage** in a narrow
window, and the final screen (dialogue frame) is black either way. The rendered
`0x000400` frame at t≈1.45 s with the decoder is visibly a corrupt blob, not the
cathedral. **This is not a fix and the "the backdrop draws" claim made mid-session
was wrong** — it came from a 1280x715 present capture whose bright pixels were the
dialogue box and the sprites (RGBA16, which always rendered), read as "the
backdrop is there".

## 3. What is established about the wall (unchanged by this session)

* The njpeg **RSP decode** is correct (session 47): all `mbs` blocks decode; the
  texture bytes at draw time are plausible YUV (`nz=563/768`, e.g.
  `27 9a 28 7a` at `0x001D1030`).
* The YUV macroblock draws reach the renderer and target colour image `0x000400`
  (`OGRE_YUV_TRACE=1`, `OGRE_RDP_TRACE=2` → `[cimgseq]`).
* The game's own CPU readback (`func_ovlE_8019976C` stage-3, `0x80199884`,
  `memcpy(state[0x70], state[0x64], 2*width)` per row) runs **4×**, all during
  scene `0x02`, and **every one copies a zero framebuffer** (`probe52`, reverted:
  `src=0x80000400 fb0=0 fb1=… fb2=0`). The blit's source `0x80243E28` is zeros
  when sampled (`nz(first4K)=0`).
* Session 48's scratch-word patch and `tools/njpeg_readback.py` are in place;
  both the game's index and the njpeg target resolve to `0x000400`, so the patch
  is not the wall (but it is also not harmful).
* **Upstream context:** [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
  ("Missing backgrounds in Ogre Battle 64 battles and also some cutscenes… a
  known issue with many emus") was closed in 2012 by Bobby Smiles' **RSP-side**
  `jpeg_decode_OB` (`mupen64plus-rsp-hle/src/jpeg.c`). That is the half this port
  already has. If Mupen+GLideN64 renders the backdrop correctly today, the
  renderer half works there too — which is worth checking against GLideN64's
  YUV handling before writing another decoder here.

## 4. The A/B recipe (reusable)

`OGRE_CAPTURE_PRESENT` is unreliable for this question: RT64 only presents when
the VI or the screen hash changes, so a 4 s run captured 2–5 frames and the
capture at t≈1.45 s is easy to miss. `OGRE_PRESENT_ALWAYS=1` does capture
steadily but stalls the game on the title screen. **Use `OGRE_DUMP_RDRAM` plus a
framebuffer histogram instead:**

```sh
# baseline vs a candidate change, same forced-step timing
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 OGRE_SCENE_LOG=1 \
  OGRE_DUMP_RDRAM=/tmp/x.bin OGRE_EXIT_AFTER_MS=1450 ./build-app/ogrebattle64
# then, offline: 320x240 RGBA16 histogram of guest 0x000400
#   (5 distinct / 76k near-black = background absent; ~1 200 distinct in a
#    magenta/green blob = a *wrong* decode; a real image needs a visual check)
```

The forced scene is racy: about 1 run in 3 never reaches `0x0D` at all. Check
`[scene] … id=0x000D` in the log before comparing anything, and compare runs whose
`0x0D` entry time matches.

## 5. Files changed

* `docs/HANDOFF-2026-09-15-session49.md` (this file).
* `PLAN.md`, `docs/DECISIONS.md`, `AGENTS.md`, `docs/scenes.md`,
  `docs/guides/rsp-microcode.md` — the record, corrected: the background is still
  black, the YUV16 decode attempt is recorded as **tried and reverted**.
* **Reverted, not left in the tree**: the YUV16 decoder in
  `tools/RT64/src/shaders/TextureDecoder.hlsli`
  (`git -C tools/RT64 diff src/shaders/TextureDecoder.hlsli` is empty).
* **Kept from this session** (unchanged by the revert, both harmless and
  documented): `tools/njpeg_readback.py`'s `ogre_sync_framebuffers()` call plus
  `app/src/renderer.cpp`, `app/src/null_renderer.cpp`, `app/src/web_renderer.cpp`
  and `tools/RT64/src/hle/rt64_state.{h,cpp}` + `rt64_application.{h,cpp}`
  (`State::syncFramebuffers()`: retire the pending workload and write rendered
  framebuffers back to RDRAM before the copy). It did **not** fix the black
  background either — it is a correctness improvement to the readback, not a fix,
  and it can be dropped without affecting anything else.
* `docs/proofs/native-newgame-cathedral-backdrop-yuv.png` — **deleted**: it was
  presented as proof the backdrop draws and does not show that (its bright pixels
  are the dialogue box and sprites).
* `rt64-ob64.patch` — **refreshed** (`git -C tools/RT64 diff` over the modified
  files, minus the `src/contrib/plume` submodule entry; verified with
  `git apply --check` against a stashed tree). The old file had drifted since
  session 47 and was missing the `OGRE_NJPEG_SCRATCH` bridge and the session-48
  diagnostics, so `git -C tools/RT64 apply ../../rt64-ob64.patch` over a clean
  submodule (the recipe in `docs/guides/app-build.md:27`) would not reproduce the
  current port. The previous version is at `/tmp/rt64-ob64.patch.bak` for this
  session only.
* **All temporary probes reverted**: `grep -rn "probe49\|probe50\|probe51\|probe52"
  app/src/ tools/RT64/src/ BankEFuncs/ RecompiledFuncs/` is empty; `build-app` and
  `build-null` rebuilt; `tools/runlog.py` reports no problems on both.

## 6. Corrections to the mid-session record

This session briefly wrote the opposite of §2 into `PLAN.md`, `AGENTS.md`,
`docs/scenes.md`, `docs/guides/rsp-microcode.md` and `docs/DECISIONS.md` ("the
backdrop draws", "the renderer half was missing until session 49"). Those claims
were wrong and have been corrected. Session 48's "the background renders" remains
wrong as well, but for a different reason: it is black, and neither the
buffer-selection patch nor this session's shader work changes that.

## 7. Next lead (do this before writing another decoder)

The renderer-side YUV decode is **not proven to be the problem**: the game's
readback copies a zero framebuffer, which is upstream of any decode. Check, in
order:

1. **Does GLideN64 (the emulator that fixed issue #102) render this backdrop?**
   Its YUV path is the reference for what "correct" looks like; if it does, diff
   its handling against RT64's rather than inventing a decode.
2. **Why is `0x000400` zero at every readback?** The readback and the draw are one
   scene-entry apart; decide from the game's own script (`0x02` → `0x0D` visits,
   `docs/HANDOFF-2026-09-15-session44.md` §1) whether the copy should be one entry
   later, or whether RT64 needs to make a framebuffer current at `G_SETCIMG` time.
3. Only then revisit the YUV16 decode — and validate it against a known-correct
   implementation (parallel-rdp's `sample_texel_yuv16`, angrylion's `tex.c`
   `tmem_formatting = 0` path), with a **visual** check, not a histogram.
