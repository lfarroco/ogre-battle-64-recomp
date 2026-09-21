# Handoff — 2026-09-21, session 96: the defeat crash localised, and the title sprite's green border localised

No fix landed for either item. Two diagnostics are in place and every probe is
reverted.

## 1. The crash after a lost battle: `func_800862C0`, guest `0x80086328`

**Symptom (developer):** SIGBUS after being defeated in a battle. Reason
`SIGBUS (10)`, `fault instruction ogrebattle64+0x16428A`, `fault address
0x1A1DF7004`, `rdram base 0x121EA7000`.

**Result: the faulting instruction is identified; the writer is not.**

- `+0x16428A` is `func_800862C0+0x59A`, the `lw $s6, 4($a1)` at guest
  **`0x80086328`**. The base `$a1` was loaded at `0x80086320` from `+0x7C` of
  the effect object (`s0 = a0`), so the guest address is `0xFFF50004`.
  `recomp_mem_addr` maps it to `rdram + 0x7FF50004`, about 2 GiB into the
  mapping, which is the SIGBUS.
- **`0x800862C0` is the effect-record drain.** Its only caller is
  `func_80085D00` (`0x80085D64`), from `func_80087590`; the manager is
  `D_800A9E54` -> `+0x34`, with 32 effect objects at stride `0x8C` from
  `0x80131D90` and a 256-slot `0x1C`-byte record pool (`0x80134510`..
  `0x801360F0`) whose free list is `manager+0x2C`. `+0x7C` is the live record
  list head, `+0x80` the tail, and `0x800867B8` copies a record's `next` into
  the head before looping, so the garbage can equally be a **record's `next`**.
- `0xFFF50000` is `-11.0` as 16.16, and the free records hold `+11.0`
  (`0x000B0000`, record type 11) at `+0x8`; the corrupt value looks like a
  misread record field rather than a pointer.
- The crash log's renderer tail switches ucode `0x8009F540` -> `0x800A08D0`
  three frames before the fault, so it lands on a **scene/mode transition**, not
  mid-battle. `spin(D_800C4C26) = 0xFFFF` and `ind = 0x8018F350` (scene `0x03`).
- **Not slow exhaustion:** the heap cursor `0x800E9BA0` is `0x803564E0` at 147 s
  of emulated time and `0x8035EAF0` at 1555 s, 34 KB apart, so nothing
  accumulates toward the crash.
- **The main unit still makes 743 `jal`s into swappable RAM** across ~200 caller
  functions (`make cross-bank-check`; the tool's own count is 951 with the
  tail/redirect shapes). The effect subsystem itself has none — no caller in
  `0x80084000..0x80089000` carries a cross-bank site — so the write that
  corrupts the list comes from some other resident function.
- **Two developer reproductions did not crash** (147 s and 237 s emulated,
  against 1555 s for the crash), including a boss loss, which is the kind of
  loss that crashed.

**`probe96` is armed for the next occurrence.** It lives in
`RecompiledFuncs/funcs_14.c` (gitignored, tagged `probe96`, reverted by
`make recomp`; do NOT run `make recomp` while it is the open lead). It reports
the first head/tail that is non-zero and not KSEG0, or a broken/cyclic record
free list or live chain, printing the object index, the whole 32-object table
and the offending record's words, writing `/tmp/probe96-rdram.bin`, then exits
96 instead of faulting. A mapped-but-outside-the-pool value is logged and the
run continues, because that is the survivable form. A boot smoke test is clean.

## 2. The title sprite's green border: `Blender::run`, not the texture

**Symptom (developer):** a transparent sprite shows a bright green background in
the parts that should be transparent, for a few frames, only on a type's first
appearance. It is a **regression**: clean on 09-17/09-18.

**Repro, from a plain boot, no gameplay:**

```sh
cd /Users/momo/dev/ogre
cp .repro/battery-pristine.bin .repro/saves/ogrebattle64-us-rev1.bin
OGRE_PREF_DIR="$PWD/.repro" OGRE_SPEED=8 OGRE_CAPTURE_PRESENT=/tmp/g/g \
  OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=14000 ./build-app/ogrebattle64 .repro/rom.z64
# the defect colour is exactly (0,132,0): px.count(b'\x00\x84\x00')
```

Six of six baseline runs show it, on 1-3 presents of ~175, presents 15-18,
1701-10107 px. Frames kept: `/tmp/greencap/g.18.ppm` (10107 px, PNG
`/tmp/greencap/frame18.png`), `/tmp/gd2/g.16.ppm` (1701 px) with
`/tmp/gd2.log` (`OGRE_DL_DECODE=all`). In the clean neighbour present those same
pixels are black, so the sprite is composited opaquely where its mask is 0; the
colour texture's masked-out border is the green.

**What it is:**

- The mask and the combiner are **correct**. The I8 source at `0x1CBC70`
  (16x34, body 0xFF, border 0x00) and the TMEM `loadTileOperation` wrote are
  byte-exact; a probe writing `(TEXEL0.a, combinerColor.a, 0)` shows the two
  agree at every pixel of every frame and no pixel has combiner alpha 1 over
  mask alpha 0.
- The failure is downstream: the draw is composited with a blend factor of 1
  instead of the sprite's alpha. `resultAlpha.a = combinerColor.a;` before
  `return true;` in `RasterPS.hlsl` removes it 3/3 runs; restricting that to
  `!alphaBlend` draws does **not**, so the failing draw has `alphaBlend == true`
  and the wrong factor.
- Tie-break: with `resultColor.b = resultAlpha.a;` for
  `alphaBlend && usesTexture0 && siz <= 1`, present 17 of run `bl1` is 10107 px
  of exactly `(0,132,255)` and zero of `(0,132,0)`, so `finalAlpha` was 1.0.
- The CPU's `call.shaderDesc.otherMode.L` for those draws (`0x0C184240`,
  `0x00504240`) decodes to `P1 = CC, M1 = FB, A1 = CC_A`, which takes the
  `M == FB` arm and yields `finalAlpha = combinerAlpha = 0`. **The word the
  shader evaluated did not match the word the CPU recorded for that draw** — a
  params/index divergence, which is also why gating the fold moved the region
  but never removed it.
- **Correction to the first reading:** `finalAlpha = 1.0` has *two* sources in
  `tools/RT64/src/shared/rt64_blender.h`: line 419
  (`1.0f - fromInputA(A, ...)`, the `P == FB` arm) **and** the general `else`
  at line 438, which sets `finalAlpha = 1.0f` unconditionally. Blue 255
  therefore proves only that the draw did **not** take the `M == FB` arm; the
  "P == FB" attribution is not proven. The conclusion is unaffected, because the
  CPU's word would have taken the M arm.
- **The naming experiment eliminates the params-slot theory.** With the tagged
  draws writing `R = resultAlpha.a`, `G = combinerColor.a` and `B` = one byte of
  `rp.omL`, all four defect sizes (1701 / 4752 / 5355 / 10107 px) come back with
  blend alpha 1.0, combiner alpha 0, and `rp.omL = 0x00504240` (byte 2 `0x50`,
  byte 3 `0x00`). The CPU fills `0x00504240` (`omH 0x00008CFF`/`0x00018CFF`,
  `cycleType 0 = G_CYC_1CYCLE`, `forceBlend` true, `bi 0x0050`) for 1024 draws in
  the same runs. So the slot is not stale, zero-filled or swapped, and
  `rt64_state.cpp:476` -> `rt64_framebuffer_renderer.cpp:1575` ->
  `rt64_workload.cpp:213` is exonerated. **Every defect pixel is the 1-cycle
  `0x00504240` family; none is the cube's 2-cycle `0x0C184240`.**
- **The decode is confirmed, so the contradiction is real.** RT64's own shifts
  (`rt64_blender.h:68-82`) and enums (`:18-37`: `PM_CC_OR_BLENDER = 0`,
  `PM_FRAMEBUFFER_COLOR = 1`; `A_CC_ALPHA = 0`; `B_ONE_MINUS_A = 0`) give
  `bi = 0x0050` -> `P0 = CC_OR_BLENDER`, `M0 = FRAMEBUFFER_COLOR`,
  `A0 = CC_ALPHA`, `B0 = ONE_MINUS_A`. Then `duplicateInput1MA` is false (P != M)
  and `anyInputIsZero` is false (`A0` is 0 not 3, `B0` is 0 not 3), so
  `passthrough` is false and `framebufferColor` is true, which selects the
  `M == PM_FRAMEBUFFER_COLOR` arm and returns
  `finalAlpha = fromInputA(A_CC_ALPHA) = combinerColor.a = 0`
  (`rt64_blender.h:421-424`). The measured value is 1.0.
- **So `finalAlpha` becomes 1.0 after `Blender::run`.** That is the next thing to
  read, and it needs no GPU run: `src/shaders/RasterPS.hlsl:240` sets
  `resultAlpha = 1.0f;` and `:245` sets `resultAlpha.a = resultColor.a;`
  conditionally, and the caller assigns `pixelAlpha = resultAlpha` (`:328`).
  Determine which of those applies to a 1-cycle `forceBlend` draw whose mask
  alpha is 0, and whether the pipeline's `srcAlpha` comes from `resultAlpha.a` or
  from `pixelAlpha`. If the 1.0 is set in that shader code, the cause is in
  `RasterPS.hlsl`, which is in the project's RT64 diff and dated 09-15 — i.e.
  **before** the window the developer remembers as clean, which is worth putting
  back to them.

**Eliminated, 3-run A/Bs each (all still green):** texture-cache miss
(0 `useTexture` failures), the decoded-texture path (forcing
`TMEMHasher::requiresRawTMEM`), a stale decode (re-uploading every decode),
shader-compile timing (`ubershadersOnly`), and the session-81 framebuffer-overlap
mechanism (`tileCopyUsed = 0` on the sprite tiles).

**Regression boundaries (facts, not hypotheses):**

- The 09-18 "new upstream base" is **falsified**. `tools/RT64` reflog: `52f7160`
  at 09-17 19:50, reset to `4337374` at 09-18 11:30; `52f7160`'s parent is
  `4337374` and `4337374` is still the base, so `d00afce` only un-committed the
  project patch. There is no upstream revision to bisect against.
- `src/shared/rt64_blender.h` is not in the diff between the 09-17 state and
  now, so the blender is not the regression.
- The net change `52f7160` -> now is 19 files / 538 insertions / 75 deletions.
  In `rt64_state.cpp` that is only `framebufferHeightForDisplay`
  (`:551`/`:1294`) plus the njpeg readback; `rt64_workload_queue.cpp` is a
  one-line sizing change; `rt64_vi_renderer.cpp` is session 81 (09-19). The
  njpeg/YUV work is 09-15, before the clean window. **None of the 19 files
  touches the params/index path.**
- So if the regression is in RT64 at all, **session 81's framebuffer sizing is
  the only post-09-17 candidate**, by changing a frame's framebuffer-pair split
  and therefore the number and order of per-pair draw sets. It is untested.

**Static follow-up (this session):** `renderParams.omL/omH` are copied verbatim
from `shaderDesc.otherMode.L/H` (`rt64_state.cpp:1227-1228`) and
`OtherMode::blenderInputs()` is `(L >> 16) & 0xFFFF`
(`tools/RT64/src/shared/rt64_other_mode.h:22-24`), so the value written into a
slot is correct; the mismatch must be in **which slot the shader reads**. The
instance index is `call.callDesc.callIndex`
(`rt64_framebuffer_renderer.cpp:1562`), assigned from
`workload.gameCallCount++` (`rt64_state.cpp:476`), and `drawData.renderParams` is
appended per call in the same loop (`rt64_state.cpp:1232`), so a single pass is
in step. `gameCallCount` exists on `Workload`, `Projection`
(`rt64_projection.h:25`) and `FramebufferPair` (`rt64_framebuffer_pair.h:43`,
zeroed by `FramebufferPair::reset`). The remaining suspect is the per-pass
binding: `gConstants.renderIndex` selecting the params range for a second
pass/pair.

**Next experiment, one build:** in the shader, log `gConstants.renderIndex`,
`instanceRenderIndices[renderIndex].instanceIndex`, `rp.omL` and `rp.omH` for the
tagged draws, and compare against the CPU's `callDesc.callIndex` and
`otherMode` for the same `submissionFrame`, per instance, on a present that shows
the defect. Differ: the params slot or its range base is stale, and session 81's
sizing is where to look for why the frame's draw set changes. Match: the
divergence is in how `call.shaderDesc.otherMode` is built, which moves the
regression earlier than the developer's recollection.

**Reference emulator not used.** The only pre-regression binary
(`~/Downloads/ogre-battle-64-recomp/ogrebattle64`, 09-20 00:24) is Mach-O
**arm64** and this host is x86_64, so it exits 126; RetroArch with
`mupen64plus_next` screenshots one frame per launch and there is no ffmpeg here
to extract frames from the recorded mkv, so a 1-3-frame defect in ~800 is a
lottery. This is the strongest remaining check for "does retail show it".

## Files and tree

- **No tracked file changed.** `git status --short` is ` m tools/RT64` and
  `?? .repro/`. `git -C tools/RT64 diff --stat` is `25 files changed,
  1375 insertions(+), 34 deletions(-)`, byte-identical to the pre-session
  figure, so `rt64-ob64.patch` does not need regenerating. (The
  `OGRE_FBRENDER_TRACE` knob in `rt64_framebuffer_renderer.cpp` is pre-existing
  project code, not a leftover probe.)
- `RecompiledFuncs/funcs_14.c` carries `probe96` only (gitignored).
- `.repro/` holds the crash-time battery (`battery-pristine.bin`), the ROM and
  `settings.cfg`; delete or ignore it when done.
- `build-app/ogrebattle64` is rebuilt from the reverted source and carries
  `probe96`.

## 3. Bisect: the defect is present in every source state that can be built here

**Step A — RT64 at the 09-17 pin, app at HEAD: GREEN.** The submodule reflog
holds the 09-17 19:50 state as a commit, `52f7160` ("organize screen", a child of
the current base `4337374`), so the whole RT64 axis can be tested in one build:
stash the working tree, `git -C tools/RT64 checkout 52f7160`, rebuild, run the
repro 3x, restore and rebuild. Result: **18864 green pixels over 3 runs**
(1701 + 5355 on presents 16/19, 1701 + 10107 on 16/18, and a clean run), the same
defect sizes. The RT64 changes since 09-17 therefore neither cause nor fix it.
The one file in that delta that is near the failure, `RasterPS.hlsl`, changed
only in the session-52 YUV convert scaling (`convertK[2] / 32.0f` removed, plus a
comment), which does not touch alpha.

**Step B — app at `fd56aaa` (09-19 21:54), RT64 at HEAD: inconclusive.** Run 1
was green (1701 px on present 14), but runs 2 and 3 stalled early (68 and 0
captures; run 3 submitted only 14 display lists), because the older app tree
does not honour this repro's knob set the same way. Bisecting the app axis this
way needs the app and its knob set moved together, so it was not pursued.

**Consequence.** Every source state that can be built on this machine shows the
defect, including RT64 as of 09-17. For this route the "clean a few days ago"
recollection is therefore not reproducible from the code, and the oldest
pre-regression *binary* is Mach-O arm64 (`exit 126` on this x86_64 host). The
cheapest way to settle it is the reference emulator on this same boot frame,
which is still the open item in section 2.

**Mechanism, sharpened by the bisect work.** `OtherMode::cycleType()` reads `H`
(`rt64_other_mode.h:26`) and `forceBlend()` reads `L & FORCE_BL` with
`FORCE_BL = 0x4000` (`rt64_f3d_defines.h:100`). The failing word `0x00504240`
has `FORCE_BL` set and `cycleType() == G_CYC_1CYCLE`, so `Blender::run` calls
`runCycle(..., lastCycle = (combinerCycles == 1) = true, forceBlend = true)` —
which skips the early return and reaches the `M == PM_FRAMEBUFFER_COLOR` arm,
returning `finalAlpha = combinerColor.a = 0`. The measured 1.0 is *exactly* what
the skipped early return produces (`rt64_blender.h:372-383`:
`lastCycle && !forceBlend` -> `finalAlpha = 1.0f` with `blenderColor` = the
combiner colour, i.e. the sprite drawn opaque and its colour texture's green
border visible). So the shader behaves as if `forceBlend()` were false for a
word whose `L` has `FORCE_BL` set. The next instrument is therefore in the
shader at the tagged draws: log `otherMode.forceBlend()`,
`otherMode.cycleType()`, `combinerCycles` and `blenderInputs()` and compare each
against the CPU's value for the same `omL`. That separates "the word reaching
the shader is not the CPU's word after all" from "the same word evaluates
differently on the two sides".
