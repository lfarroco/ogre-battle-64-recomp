# Handoff — 2026-08-25 (session 9): title-screen DL-build crash fixed (N64Recomp fall-through tail calls); boot now reaches real rendering (next wall: RT64/Vulkan render thread)

## TL;DR

- **The session-8 wall (`func_8019FC68` SIGSEGV) is fixed.** Root cause was NOT a
  game bug — it was a **recompilation bug**: OB64's compiler (KMC) merges identical
  function epilogues into a separate symbol that the preceding function **falls
  through into** (e.g. `func_8019ABC4`'s epilogue at `0x8019AEFC..0x8019AF08`
  restores `$ra/$fp/$s7/$s6`, then falls through into `func_8019AF0C`, which
  restores `$s5..$s0` and `$sp`). N64Recomp treated `func_8019AF0C` as a separate
  function and **never ran it** when recompiling `func_8019ABC4`, so the callee-saved
  registers and `$sp` were left clobbered. In the title path,
  `func_8019FC68` calls `func_8019ABC4` between capturing `s0` (a heap pointer
  returned by `func_8009DF48`, a resource loader) and `free(s0)` — so `free` got a
  garbage pointer (`0x801bc666`, `func_8019ABC4`'s own `s0 = a3` value) and SIGSEGV'd.
- **Fix: N64Recomp now emits a "fall-through tail call"** — when a function's code
  can fall off the end of its instruction range (last instruction is not
  `jr`/`j`/`syscall`/`break`, nor the delay slot of one of those) and the next
  function starts exactly at `func.vram + words*4`, emit
  `next_func(rdram, ctx); return;`. This makes shared-epilogue chains run correctly.
- **A second, deeper fix was needed**: static functions (N64Recomp's synthetic
  `static_<section>_<addr>` functions created for cross-boundary branches) were
  never registered in `context.functions_by_vram`, and were created+recompiled in a
  single sorted pass — so a static that *falls through* into another static
  (e.g. `static_16_8019AB84` → `static_16_8019AB94`, a shared epilogue chunk) could
  not see its target. `main.cpp` now (a) registers statics by address, and
  (b) processes statics in a **two-phase fixpoint loop** (create all known statics,
  then recompile the batch, repeat until no new statics).
- **Result: boot now passes the title-screen DL build** (the `func_8019FC68`
  crash is gone) and reaches **real rendering**: ~1528 PI DMAs, 64 RSP tasks
  (including new `type=1` tasks, previously never reached), repeated
  `osSpTaskStartGo` calls, and RT64 actually rendering frames.
- **New wall:** crashes in the **RT64 render thread** (`RT64::WorkloadQueue` →
  `FramebufferRenderer::submitRasterScene` → `libvulkan_intel_hasvk.so`, the Vulkan
  driver for the Intel Haswell GPU). This is the same class as the long-standing
  "flaky renderer-init segfault" — the game now feeds real display lists to RT64 and
  the driver/RT64 combo crashes. Next session should attack this (see leads).

## The root cause (session-8 crash), in detail

`func_8019FC68` (overlay C, title-screen sprite/text layout) does:
1. `s0 = func_8009DF48(id)` — loads a resource; returns a heap pointer.
2. `func_8019ABC4(...)` — **clobbered `s0`** (and `s1..s5`, `$sp`) in the old build.
3. `free(s0)` at `0x801A000C` → freed a garbage pointer → SIGSEGV in
   `func_800712C4` (the heap free).

`func_8019ABC4`'s epilogue is split across two symbols (KMC shared-epilogue
pattern):
```
0x8019AEFC: lw $ra, 0x54($sp)      ; func_8019ABC4's last instructions
0x8019AF00: lw $fp, 0x50($sp)
0x8019AF04: lw $s7, 0x4C($sp)
0x8019AF08: lw $s6, 0x48($sp)
0x8019AF0C: lw $s5, 0x44($sp)      ; <-- func_8019AF0C (separate symbol)
0x8019AF10: lw $s4, 0x40($sp)
0x8019AF14: lw $s3, 0x3C($sp)
0x8019AF18: lw $s2, 0x38($sp)
0x8019AF1C: lw $s1, 0x34($sp)
0x8019AF20: lw $s0, 0x30($sp)
0x8019AF24: jr $ra
0x8019AF28: addiu $sp, $sp, 0x58
```
The recompiled `func_8019ABC4` ended after `lw $s6` and returned without restoring
`s5..s0` or `$sp`. Evidence (debug prints in the recompiled code):
```
[dbg] 8019FFB0 s0(loadres)=803ff1e0          ; s0 = valid resource pointer
[dbg] 8019FFEC afterABC4 s0=801bc666 ...     ; func_8019ABC4 left s0 = its own a3
[dbg] AF0C sp=800c2068 ... save[0x30]=801bc666  ; save slot had been overwritten
```
The `func_8019A1B4` / `func_8019A7C0` / `func_8019A884` split is a **real
3-function chain** (overlay B `jal`s `func_8019A7C0`/`func_8019A884` and takes
their addresses for callback registration), *not* a splat artifact — the functions
share one frame layout and cross-jump into each other (loop-back from
`func_8019A7C0` to `0x8019A730` inside `func_8019A1B4`, `func_8019A884` calls
`func_8019A1B4` recursively). So they must stay separate symbols; the static
function web N64Recomp builds around them is legitimate and must be recompiled
correctly.

## The fixes (N64Recomp, vendored + gitignored)

`tools/N64Recomp/src/recompilation.cpp` (`recompile_function_impl`):
- After the instruction loop, detect a fall-through into the next known function:
  - `last_ends_flow` if the last instruction is `jr`/`j`/`syscall`/`break`/`eret`,
    or if it is the delay slot of one of those (the usual `jr $ra` +
    `addiu $sp, $sp, N` epilogue already emits a return).
  - If not flow-ending, look up `func.vram + words*4` in
    `context.functions_by_vram`; on a same-section, non-empty, non-ignored,
    non-stubbed hit, emit `next(rdram, ctx);` + `return;`.
- Initial run: 23 fall-through tail calls (after the delay-slot refinement); with
  the static registration fix: 35.

`tools/N64Recomp/src/main.cpp` (static function handling):
- When creating a static function, register it in
  `context.functions_by_vram[addr]` (it was only in `context.functions`, so the
  fall-through detection and branch tail-call detection could not see it).
- Restructured the per-section static loop into a **fixpoint**: phase 1 creates
  Function objects for every currently-known static address; phase 2 recompiles
  that batch (discovering more statics, which are appended and handled by the next
  while iteration). Without this, `static_16_8019AB84` was recompiled before
  `static_16_8019AB94` existed, so its fall-through into the shared epilogue was
  lost.

`n64recomp-ob64.patch` — regenerated from the vendored working tree
(`git -C tools/N64Recomp diff > n64recomp-ob64.patch`; apply with
`git -C tools/N64Recomp apply ../../n64recomp-ob64.patch`).



## Verification

`make` → `make recomp` → `cmake --build build-app -j2` →
`timeout 90 ./build-app/ogrebattle64 assets/ogre64.z64`.

- The old `func_8019FC68` crash is gone.
- Boot now: 1528 `[pi]` DMAs, 64 `osSpTaskStartGo` calls (including `type=1`
  tasks with `ucode=0x8009F540`, previously unreached), the game submits real
  display lists and RT64 renders frames.
- Crashes are now in the **RT64 render thread** under gdb:
  `RT64::WorkloadQueue::threadRenderFrame` → `submitRasterScene` →
  `libvulkan_intel_hasvk.so` (Vulkan driver). Plain runs usually crash earlier
  (the pre-existing flaky renderer-init/Vulkan crash) but occasionally reach the
  same render path. Note: the plain-run crashes may be the same Vulkan driver issue
  manifesting at different points.

## Leads for the next session (the RT64 render-thread crash)

1. **Characterize the RT64/Vulkan crash.** Run under gdb; get the faulting
   instruction in `libvulkan_intel_hasvk.so`. The dev GPU is Intel HD Graphics
   4400 (Haswell, Vulkan via `hasvk`). It may be (a) an RT64 bug triggered by
   OB64's F3DEX display lists, (b) the long-standing "flaky renderer init" race
   (see `docs/HANDOFF-2026-08-25-session7.md` lead 2: `vi_thread_func` at
   `update_vi()` with a garbage `mode` pointer, `set_dummy_vi`/`osViSetMode` race),
   or (c) a driver bug on this old GPU. Try `VK_ICD_FILENAMES` pointing at
   `libvulkan_lvp.so` (Lavapipe, the software rasterizer) to see if the crash is
   driver-specific.
2. **RSP microcode.** The new `type=1` tasks use `ucode=0x8009F540`; the runtime
   still uses the stub microcode (`rsp.cpp`), so the game's display lists may be
   partially/incorrectly generated. The RT64 GBI fix from
   `docs/guides/rsp-microcode.md` (force F3DEX 2.08 short-format GBI) is still
   pending and may be relevant to render correctness.
3. **Two remaining "falls through to unknown" recompilation notices** (from the
   fall-through detector, not currently crashing): `func_80093060` →
   `0x8009337C` and `func_8009953C` → `0x800996C4`. These look like symbols whose
   ELF size is too small (the function's code continues past the symbol end) or
   fall-through targets that aren't function starts. If a crash later points at
   these, the symbol boundaries in `asm/1060.s` need checking.
4. **Clean up the `yield_self` poll-loop fix interaction** if any spin appears
   later — the new tail calls change scheduling, so watch for new stalls.

## Changes (repo, to be committed)

- `n64recomp-ob64.patch` — regenerated (now includes the fall-through tail-call
  emission + static fixpoint + `functions_by_vram` registration).
- `docs/DECISIONS.md` — new entry (this session).
- `docs/HANDOFF-2026-08-25-session9.md` — this file.
- `PLAN.md` — status update (boot reaches real rendering; next wall is the RT64
  render thread).

## Changes (vendored, gitignored — re-apply on re-clone)

N64Recomp (`tools/N64Recomp`):
- `src/recompilation.cpp` — fall-through tail-call emission in
  `recompile_function_impl` (plus the earlier `yield_self` poll-loop work).
- `src/main.cpp` — static functions registered in `functions_by_vram`; two-phase
  fixpoint static processing loop.
- (All other vendored changes from earlier sessions remain.)
- Rebuilt `tools/N64Recomp/build/N64Recomp`; re-ran `make recomp`
  (`RecompiledFuncs/` is gitignored).

## How to reproduce

```sh
git -C tools/N64Recomp apply ../../n64recomp-ob64.patch   # if re-cloned
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4
make            # rebuild ELF
make recomp     # regenerate RecompiledFuncs (gitignored)
cmake --build build-app -j2
timeout 90 stdbuf -oL -eL ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/boot.log 2>&1
# or under gdb (avoids the flaky renderer-init crash):
gdb -batch -ex 'set pagination off' -ex 'handle SIGPIPE nostop noprint' \
    -ex 'run assets/ogre64.z64' -ex 'bt 15' ./build-app/ogrebattle64
```

Expected: boot through the overlay loads, controller path, the RSP pipeline,
the title-screen DL build (`func_801A1FCC`/`func_8019FC68` — no longer crashes),
then repeated RSP tasks and RT64 frame rendering, then the RT64 render-thread
SIGSEGV (Vulkan driver, `libvulkan_intel_hasvk.so`).
