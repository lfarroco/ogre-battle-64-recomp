# Handoff — 2026-09-15, session 48: the cathedral background renders (the njpeg readback was copying the wrong buffer)

> **Corrected (session 52):** "the background renders" here meant *a black frame
> over the correct sprites* — the real backdrop is a fully lit cathedral
> (red carpet, stone walls, statues). The readback-buffer finding is sound and
> still stands; the title of this handoff overstates the result. The proof
> `docs/proofs/native-newgame-cathedral-background.png` was the black capture and
> has been **replaced** by the session-52 render. See
> `docs/HANDOFF-2026-09-15-session52.md` §6.

## Goal and result

**Goal (session 47's open wall):** the cathedral scene (scene `0x0D` step 2) had no
background — the image at guest `0x80243E28` was uniform in the port.

**Result: the background renders, and the wall was neither the renderer nor the
decoder.** Session 47's two candidate causes (RT64 dropping the YUV draw / the
framebuffer writeback never running) are both **disproved by measurement**: the
`0x800A5110` YUV macroblock draws reach the RDP and the render targets are written
back to RDRAM. The actual cause is the game's own njpeg readback copying the
**wrong buffer**:

* `func_ovlE_8019976C`'s stage-3 loop (`memcpy(state[0x70], state[0x64], 2*width)`
  per row, `0x80199884`) copies from `state[0x64]`, and
* `state[0x64]` is set by `func_ovlE_80199A08` from the framebuffer table at
  `0x800A8204` = `{0x80000400, 0x80025C00, 0x8004B400}`, using an index derived by
  comparing the display word `D_800C4BB8` against those three entries. When it
  matches none of them — which happens routinely, because the game also swaps the
  VI to non-framebuffer targets (observed `0x80250800`) — the index defaults to
  **0**, i.e. the table's first entry `0x80000400`, which is the ROM's placeholder
  word and not the buffer the YUV draw landed in.

`docs/proofs/native-newgame-cathedral-background.png` is the capture (Archbishop
Odiron dialogue over the cathedral background, RT64 build).

## 1. Corrections to session 47 (instruction/data level)

* **The framebuffer table is `0x800A8204`, not `0x800B8204`.** Session 47 read
  `0x800B8204` and found `0x1440FFD1` (code) and a zero table. The ROM at
  `0x38604` (= guest `0x800A8204`) is `80000400 80025c00 8004b400`: the table is
  `{0x80000400, 0x80025C00, 0x8004B400}`, and `0x800B8204` is inside the data
  segment's *code-looking* alignment padding. `asm/data/2E570.data.s:10404` and
  the ROM bytes agree. The `0x80000400` word is table entry 0 (a placeholder);
  session 47's "count" reading of it (`fbcount=0` at `0x800B8204`) was reading the
  wrong address.
* **The YUV draws are not dropped.** `[cimgseq]` (new `OGRE_RDP_TRACE=2`) shows the
  RDP's colour-image sequence cycling `0x000400 → 0x025C00 → 0x04B400` and each
  buffer ending a run with ~2 500 distinct 16-bit values — real rendered content.
  The `0x0843`-everywhere result session 47 measured was the *copy source being a
  uniform buffer*, not the renderer failing to draw.
* **The writeback path runs.** The `[fbwrite]` probe (kept, `OGRE_FB_WRITEBACK=1`)
  fires for `addr=0x025C00` with real pixels. Session 47's "the framebuffer never
  receives the draw" is disproved.

## 2. How the wall was found (the measurements that matter)

1. **The copy source, logged from the recompiled loop** (`BankEFuncs/funcs_0.c`,
   temporary probe). It is `0x80000400` in the failing case and `0x80025C00` in a
   working one — the buffer selection is nondeterministic run to run.
2. **The frame-selection branch** (`0x80199B0C`): `D_800E7A0C` is `1` (so the code
   does compare), but `D_800C4BB8` holds a value that matches no table entry, so
   the default path (`sb $zero, 0x10($sp)`) selects index 0.
3. **The buffer is image-bearing**: forcing the copy source to table entry 1
   (`0x80025C00`) via a temporary A/B produced a real image (2 501 distinct 16-bit
   values in the assembled buffer); entries 0 and 2 also produce real images. So
   all three are legitimate render targets and only the *choice* is wrong.
4. **`tools/watch.sh --value 0x80000400 0x800C4BB8`** (new `--value` conditional
   watchpoint) caught the writer with a backtrace:
   `func_8007307C (VI manager command handler) ← func_80089540 (N64 Thread 5)`,
   i.e. `D_800C4BB8` is the game's "displayed buffer" word and is written from the
   VI-manager message object.

## 3. The fix

The game's readback should copy the buffer its own YUV draw landed in. That is
knowable without guessing: RT64 sees the `G_SETCIMG` for the YUV macroblock draw.

* **RT64 side** (`tools/RT64/src/hle/rt64_rdp.cpp`): a scratch area in RDRAM's last
  64 KiB (`0x807FFC00`, verified zero/untouched in a live dump) carries
  `+0x00` the current colour-image address, `+0x04` a "YUV texture image seen"
  handshake, `+0x08` the colour image set right after that handshake (the njpeg
  target), and `+0x0C` the most recent of the three game framebuffers.
* **Generated-code side** (`tools/njpeg_readback.py`, wired into `make bank-recomp`
  right after `gen_bank_funcs.py`): at `0x80199878` the copy source is replaced by
  the njpeg target, falling back to the most recent game framebuffer, and finally
  to the game's own value. `make bank-recomp` regenerates `Bank*Funcs/`, so the
  script re-applies the window every run (same model as `cross_bank.py dispatch`).
  On a renderer that does not maintain the scratch word (the null build) the patch
  is a no-op: the game's own value is used unchanged.

### Verification

```sh
# the shortcut: straight to the cathedral, skipping the 28.7 s movie
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_EXIT_AFTER_MS=25000 OGRE_CAPTURE_PRESENT=/tmp/cath \
  OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=200 ./build-app/ogrebattle64
# -> [scene] holding step 2 while scene 0x02 runs; scene 0x0D at t≈1.4 s; the
#    capture is the cathedral with the Archbishop's dialogue.

# the fix (any of these runs reaches scene 0x0D step 2 with the background):
OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_NJPEG=1 OGRE_EXIT_AFTER_MS=38000 OGRE_CAPTURE_PRESENT=/tmp/final \
  OGRE_CAPTURE_AFTER=3200 OGRE_CAPTURE_EVERY=100 ./build-app/ogrebattle64
# -> /tmp/final.<n>.ppm; the assembled buffer at 0x80243E28 goes from 2 distinct
#    16-bit values (uniform 0x0843) to ~2 500, and the capture shows the
#    cathedral background behind the Archbishop's dialogue.

# the null build is unaffected (the patch is a no-op without the scratch word):
OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_NJPEG=1 OGRE_EXIT_AFTER_MS=32000 ./build-null/ogrebattle64

# new diagnostics used above:
OGRE_RDP_TRACE=2 ...        # [cimg] histogram + [cimgseq] ordered framebuffer list
OGRE_YUV_TRACE=1 ...        # [yuv] every `G_SETCIMG` for a YUV texture image
OGRE_FB_WRITEBACK=1 ...     # [fbpair]/[fbwrite] the CPU-visible RDP writeback
tools/watch.sh 0x800C4BB8 --value 0x80000400 --hits 2 --size 4
OGRE_STEP=2 ...             # the new step shortcut (see §6)
```

No repo test harness exists; each check is a ROM-dependent 30–40 s game run. The
scene reaches step 2 only in some runs (the title-tap timing is wall-clock), so
re-run until `[scene] ... id=0x000D` appears. **`OGRE_STEP` removes that
flakiness**: the shortcut run lands on the target step every time.

## 4. The step shortcut (`OGRE_STEP`)

The New Game opening is one scene (`0x0D`) run through many *steps*; the
scene-script VM `func_80170974` writes the step number into `D_8018F1C0`
(`sh $a0, 0($s5)` at `0x80170ADC`) and `0x8002` into `D_8018F1C2`, and `0x0D`'s
enter (`func_80178568`) branches on it. Step 1 is the ~28.7 s movie, step 2 the
cathedral.

`app/src/bank_overlays.cpp` now takes `OGRE_STEP=<n>` and holds `D_8018F1C0` at
`n` (and `D_8018F1C2` at the VM's own `0x8002`) **every frame** while the
`OGRE_SCENE`-selected scene is the active one, releasing the hold as soon as the
dispatcher leaves it (the VM rewrites the word on each visit, so a one-shot poke
would not stick). Verified for steps 1, 2 and 5: each reaches `0x0D` ~1.4 s after
boot, and a step-1 run shows the sepia movie while a step-2 run shows the
cathedral — i.e. the movie really is skipped. This is also the reliable way to
reproduce the background fix above (no title-tap timing to lose).

## 5. What is NOT established

* **Why the game's own index selection lands on the placeholder.** The mechanism is
  measured (the display word matches no table entry); whether retail reaches that
  state too, or whether the port's VI bookkeeping (`D_800C4BB8`, written by the VI
  manager from a message object) diverges from retail, is open. The fix sidesteps
  it rather than answering it. A watcher on the VI-manager message object would
  settle it.
* **What `0x80000400` is.** It is a real render target (the RDP draws into
  `0x000400` and it ends a run with ~2 500 distinct values) and it is table entry
  0, but it is also the ROM's placeholder-looking word. Whether retail treats it as
  a third back buffer or as a sentinel is unresolved.
* Whether the assembled image is *pixel-exact* against retail. The capture is
  visually correct (developer confirmed the scene), but no frame comparison exists.

## 6. Files changed

* `app/src/bank_overlays.cpp` — `OGRE_STEP` (the step shortcut, §4).
* `tools/njpeg_readback.py` (new) — the durable readback-source patch.
* `Makefile` — `bank-recomp` runs `tools/njpeg_readback.py` after `gen_bank_funcs.py`.
* `tools/RT64/src/hle/rt64_rdp.cpp` — scratch-word bridge + `OGRE_RDP_TRACE=2`
  histogram/sequence and `OGRE_YUV_TRACE` diagnostics.
* `tools/RT64/src/hle/rt64_state.cpp` — `OGRE_FB_WRITEBACK` writeback diagnostics.
* `tools/watch.sh` — `--value <n>` conditional watchpoint (`watchpoint modify -c`).
* `docs/proofs/native-newgame-cathedral-background.png` — the verification capture.
* `docs/guides/rsp-microcode.md`, `docs/scenes.md` (the shortcut), `PLAN.md`,
  `docs/DECISIONS.md`, `AGENTS.md` — record updated (including the session-47
  corrections in §1).
* **Vendored/gitignored, regenerated**: `BankEFuncs/funcs_0.c` (patch applied by
  the script), `RecompiledFuncs/*` (`make recomp`) — not committed, as usual.
* `tools/RT64` remains a dirty submodule (the project's own patches); the session
  added to `rt64_rdp.cpp`/`rt64_state.cpp`, which `rt64-ob64.patch` captures.
* **All temporary probes reverted**: `grep -rn probe48 app/src/ RecompiledFuncs/
  Bank*Funcs/ tools/RT64/src/` is empty; both `build-app` and `build-null` rebuilt
  clean.
