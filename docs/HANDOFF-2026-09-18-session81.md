# Handoff — 2026-09-18, session 81: the cutscenes' flickering right/bottom line

**Two RT64 changes landed (§5, §7), and the line is gone.** The first sizes a game
framebuffer from the VI instead of the frame's drawn rectangle; the second
presents only the 319x239 area the game actually draws, which is what removes the
visible line. §2 is the anatomy, §3 the reference-emulator findings, §6 the
dead end (do not re-walk it), §8 the evidence and the escape hatch.

Corrections made during the session, in order, because each was measured and the
previous reading did not survive:

1. The first draft blamed stale *render-target* edge pixels; the port's RDRAM is
   not an oracle (framebuffers are written back only when the game CPU-reads
   them), so that reading was withdrawn.
2. The next blamed the render target's height churn as *the* cause of the visible
   line; the checkpoint-aligned A/B showed the settled-cathedral alternation is
   identical before and after that change. It is a real latent defect and was
   kept for that reason, but it is not this line.
3. The developer then reported the line still present, which is correct; §7 is
   the measurement that explains it and the change that removes it.

## 1. The symptom

Several cutscenes (cathedral dialogue; the Magnus/old-man dialogue in
`docs/proofs/native-newgame-magnus-old-man.png`) show a line at the right and
bottom of the displayed picture that flickers — measured as the bottom 3 px rows
and the right ~2 px columns alternating between content and black, one frame in
three.

## 2. Anatomy of the line (all measured)

* The picture is the game's 320x240 VI framebuffer — for these cutscenes one of
  **three njpeg macroblock buffers the VI rotates**, `0x000400` / `0x025C00` /
  `0x04B400` (each 0x25800 = 320x240x2 bytes apart). The scene is letterboxed
  inside it (picture rows ~19..214, columns ~0..318).
* **The game's own scissor and clear are `(0,0)-(1276,956)` = 319x239 pixels**
  (`OGRE_DL_DECODE=all` at the cathedral: 10 306 commands use that scissor; the
  only `FILLRECT` is that same rectangle; 5 commands use a full 320x240 one). So
  the game's drawing stops one pixel short on the right and at the bottom: the
  framebuffer's **last column (319) and last row (239) are never written by this
  scene**.
* Those pixels therefore keep leftover content. A dump watch through the boot
  (`/tmp/lw_04..12.bin`, t=4..12 s, `scene=0x0D step=2` by t≈9 s) shows the
  leftover is present **in `0x400` already at t=4 s and never appears in the
  other two buffers**:

  | dump | `0x400` r239 / c318 / c319 | `0x25C00` | `0x4B400` |
  |---|---|---|---|
  | t=4 s | 8 / 8 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
  | t=8..12 s | 8 / 8 / 1 | 0 / 0 / 1 | 0 / 0 / 1 |

  (values are 5-bit means; 8 ≈ 66/255 = content, 0/1 = black.) The writer is the
  boot's **njpeg macroblock draw**, which uses a full-screen scissor and lands in
  one buffer (entry 0, per `tools/njpeg_readback.py`'s index note) — so the three
  rotating buffers disagree at those pixels.
* **Nothing regenerates it.** Zeroing row 239 and columns 318/319 of all three
  buffers with live console `w` writes left them black at +6 s, +11 s and +15 s
  (`/tmp/band_before.bin` vs `/tmp/band_after{1,2,3}.bin`).
* Hence the flicker: the VI rotates the buffers, so the presented edge toggles
  between "one buffer's leftover" and "the other two buffers' black" — one frame
  in three, exactly as measured (15 of 40 settled-cathedral frames;

A 1-pixel edge line is invisible on a CRT: a real TV's overscan crops several
pixels, which is why the game's own off-by-one scissor never mattered. The port
displays the full 320x240, so it shows pixels the game never draws.

## 3. The reference implementations (kept for the presentation model)

* **mupen64plus-core** (vendored at `tools/RT64/src/contrib/mupen64plus-core`)
  only forwards the raw VI registers to the video plugin
  (`src/plugin/plugin.c:263-264`). No notion of "the area this frame drew".
* **GLideN64** (`src/VI.cpp`): `findBuffer(*REG.VI_ORIGIN & 0xffffff)` /
  `FrameBuffer_CopyFromRDRAM(*REG.VI_ORIGIN & 0xffffff, ...)`; `VI_UpdateSize()`
  derives the size from V_START/V_END + VI_WIDTH (OB64: `vRegion 0x002501FF`
  -> 237 -> `*1.0126582` -> 240; width 320).
* **angrylion-rdp-plus** (`src/core/n64video/vi.c:649`):
  `frame_buffer = *vi_reg_ptr[VI_ORIGIN] & 0xffffff;`, scan `line = y * vi_width_low`.
  -> both present a **fixed VI window into RDRAM from `VI_ORIGIN`**; RT64 instead
  presents a render target and (still, see §6) computes its base as
  `origin - one row`.
* **GLideN64's OB64 hooks are in this 2D path** (`src/uCodes/S2DEX.cpp`):
  `hack_Ogre64` routes a YUV `gSPObjRectangleR` to a direct RDRAM macroblock copy
  clipped to `min(16, ci_width - ulx)` x `min(16, scissor.lry - uly)` — note it
  clips by the **scissor**, i.e. the reference also treats the scissor as the
  bound — and `graphics2D\enableTexCoordBounds=1` guards "garbage due to fetching
  out of texture bounds" (RT64 has no equivalent of that one).

## 4. Measurement setup

* Fast path: `OGRE_SCENE=new-game OGRE_STEP=2 OGRE_SPEED=8 OGRE_NJPEG=1`.
* Consecutive presented frames: `OGRE_CAPTURE_PRESENT=... OGRE_CAPTURE_EVERY=1`
  (with `OGRE_CAPTURE_AFTER` high enough to be past the boot).
* The target the presenter samples: `OGRE_CAPTURE_TARGET=...` (or
  `OGRE_PRESENT_TRACE=1` for `texSize=`).
* Live RDRAM at a chosen moment: the console (`OGRE_CONSOLE_AT_MS`, `dump`, `c`).
* Game command stream / scissors: `OGRE_DL_DECODE=all`.
* A checkpoint (`OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_STEP=2
  OGRE_CONSOLE_ON_CMD='save /tmp/cath.ckpt'`, then `load`) aligns two runs on game
  state, which is what the earlier invalid A/Bs lacked.

## 5. Landed change 1 — framebuffer height from the VI

`tools/RT64/src/hle/rt64_framebuffer.h`:

```cpp
inline uint32_t framebufferHeightForDisplay(uint32_t drawnBottom, uint32_t viHeight) {
    const uint32_t saneViHeight = (viHeight > 0) ? std::min<uint32_t>(viHeight, 1024) : 0;
    return std::max(drawnBottom, saneViHeight);
}
```

used at `rt64_workload_queue.cpp:435`, `rt64_state.cpp:551` and `:1294`.
Reason: the RDP snaps a copy/fill rect's fractional bits (`lrx |= 3; lry |= 3`),
so frames whose draws stopped at row 238 produced a **239-row** render target
while others produced 240/241, and the presenter samples the whole target.
Measured after: the target is **320x240 in 1361/1361** and **2465/2465**
consecutive frames (before: 320x239/240 alternating), and the stale-edge line on
the black frames right after a checkpoint `load` is gone. It does **not** affect
the settled-cathedral line (§7).

## 6. Dead end — do not re-walk it

Making `VI::fbAddress()` return `VI_ORIGIN` unchanged (as both references do)
was probed (`probe81`, reverted) and is **not** the fix for this line: the
presenter then finds no framebuffer at the origin, falls back to uploading the
RDRAM copy (`[present] scratch path (no framebuffer for 0x700280)`), and the line
is still there, one row lower. The origin offset also needs a row offset in the
presenter's sampler (`VideoInterfacePS.hlsl` has no offset term — `LowerRight =
videoResolution / textureResolution`, sampled from the texture's row 0), so it is
a separate fidelity project, not this fix.

## 7. Landed change 2 — present only the area the game draws

`tools/RT64/src/render/rt64_vi_renderer.cpp` gains a file-local
`visibleFramebufferSize(vi)` (fbSize minus `overscanCrop()`, default **1 px** on
the right/bottom, `OGRE_OVERSCAN=<0..8>` to override, `0` = previous behaviour).
It is used for

* `pushConstants.videoResolution` (so the shader's `LowerRight` becomes
  `319/320 x 239/240`: the outer strip's uv lands past it and is zeroed by the
  existing `outsideBorder` term -> black), and
* `viViewRect` in `getViewportAndScissor` (so the image is drawn 1:1 into the
  319x239 region; the presenter's `clearColor` supplies the outer line).

Result at the settled cathedral, `EVERY=1`: **120 of 120 consecutive frames have
no band** (before: 15 of 40), with the picture intact and aligned
(`/tmp/after_crop.png`). This is not a fabricated game state — the game's own
scissor draws exactly this area; a CRT cropped it too.

## 8. Verification and regressions

* Band count, settled cathedral, consecutive presented frames:
  **120/120 clean with the crop** (default), **28/71 with `OGRE_OVERSCAN=0`**
  (the same A/B, old behaviour), 15/40 on the pre-fix build. So the crop is what
  removes it, and the knob is the escape hatch.
* Regressions (all exit 0, scene timelines unchanged): boot -> intro (`0x09`,
  1.19 s) -> publishers (`0x0A`, 10.6 s) -> title (`0x04`, 26.9 s) -> load game
  (`0x12`, 30.8 s) -> map (`0x05`, 37.4 s) via `tools/run-save.sh prologue` and
  the session-78 tap recipe; the intro renders, the cathedral renders, the title
  and map captures in `/tmp/reg2` are unchanged apart from the 1-px edge.
* `git -C tools/RT64 diff --stat`: the baseline (23 files, 1314/28) plus this
  session's three edits: `+framebufferHeightForDisplay` (3 call sites) and
  `+visibleFramebufferSize` (VI renderer). The `probe81` edit is the only thing
  added and then removed; `grep -rl probe81 tools/RT64/` is empty.

## 9. Open / next

* If exact parity with the references is wanted later: the `VI_ORIGIN` row
  offset (§6) and GLideN64's `enableTexCoordBounds` equivalent for the S2DEX2
  macroblock rects.
* The overscan amount is a judgement call: 1 px is exactly what the game's own
  scissor excludes. A CRT cropped more; if the developer wants the softer CRT
  framing, `OGRE_OVERSCAN=<n>` already does it.

## 10. Files and evidence

* Landed: `tools/RT64/src/render/rt64_vi_renderer.cpp`,
  `tools/RT64/src/hle/rt64_framebuffer.h`,
  `tools/RT64/src/hle/rt64_workload_queue.cpp`,
  `tools/RT64/src/hle/rt64_state.cpp`. Docs: `PLAN.md`, `docs/README.md`, this
  file.
* Evidence in `/tmp`: `v4` (120 post-fix frames, 0 bands), `verify3` (40 pre-fix
  frames, 15 bands), `lw_04..12.bin` (when the leftover appears),
  `band_before/after{1,2,3}.bin` (it is not regenerated), `reg2` (title/map
  regressions), `after_crop.png`, `edge_trace.log` (the scissor histogram).
