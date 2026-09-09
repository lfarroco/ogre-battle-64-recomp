# Handoff — 2026-09-09 — session 18: title path advances, heap-walk wedge remains

## Outcome
- No rendered frame yet.
- The normal title path (`0x9`) now reproduces: title dispatch records `idx=0x9`, `statep=0x800AEFE0`, and display-list cursor `0x800E9BA0` advances into `0x801Cxxxx`.
- Graphics still show only the boot blanking display list. Advanced runs later become unresponsive to CDP input/status reads.
- Latest decisive run: `/tmp/ogre-probe/tap41-live.txt` reached `idx=0x9`, then wedged; final dump failed; `tap41-errors.txt` is `(none)`.

## Current blocker
The wedge signature is a worker pinned in the game heap search:
- `tap41`: sampled thread `t4`, last func `0x80071A3C`, then `0x80071E74`; `hotloop t4: 0x80071E74 x100`.
- Audio/producer state in the same snapshot: sampled thread `t3` blocked on send of `0x800E8B14` in `func_80080F78`; `t18` blocked on recv `0x800E8BBC`; `t5` blocked on recv `0x800E7988`.
- Do not treat `t3`/`t4` as fixed roles: thread IDs race across runs.

Important correction from this session:
- `yield_self` is sparse, not pervasive: roughly 100 sites across `RecompiledFuncs`, including the allocator walks and main title spin.
- Therefore allocator walks can interleave at yield points, but a host-preemption-everywhere model is wrong.
- A recursive heap mutex did not stop the wedge in tap25–tap28/tap41, so the remaining hypotheses are narrower:
  1. static heap-list corruption before the walk;
  2. mutation by a non-allocator writer;
  3. walk starts from state not covered by the snapshot heads;
  4. a long/slow yielding walk rather than a hard infinite loop.

## Evidence against static-cycle-only theory
- `heapchk` performs bounded 2D DFS from bucket heads and reports `CYCLEvia` if found.
- Retained wedge runs through tap41 show `no-cycle`, small graphs, and no `[heapwalk] TRIP` lines.
- This is suggestive, not conclusive: snapshots stop when the page becomes unresponsive, and the DFS only covers scanned heads/edges.

## Changes made

Tracked:
- `n64modernruntime-ob64.patch`: regenerated; `git diff --numstat` is `239/19`.
- Runtime instrumentation in `tools/N64ModernRuntime`:
  - `ultramodern/src/mesgqueue.cpp:360`: `heapchk`, bucket-head DFS.
  - `ultramodern/src/function_trace.cpp:19`: allocator entry ring.
  - `ultramodern/src/function_trace.cpp:74`: ~1 kHz allocator transition sampler.
  - `ultramodern/src/mesgqueue.cpp:430`: `audiocb` snapshot.
  - `ultramodern/src/threads.cpp:366`: recursive heap mutex and C entry points.
  - `N64Recomp/include/recomp.h:506`: lock/unlock declarations plus C cleanup-guard helper.

Ignored/generated and therefore fragile:
- `RecompiledFuncs/` is ignored by `.gitignore:9`.
- `build-wasm/` is ignored by `.gitignore:8`.
- Session edits exist in 10 ignored recompiled files: `funcs_0.c`, `funcs_1.c`, `funcs_4.c`, `funcs_5.c`, `funcs_6.c`, `funcs_8.c`, `funcs_9.c`, `funcs_12.c`, `funcs_13.c`, `funcs_14.c`.
- There are 13 heap-guard insertions plus two walk tripwires and two `stdio.h` includes.
- Explicit unlock-before-halt was added around both `pause_self` paths in `func_80070D20`.
- Re-list them with: `grep -RIl "SESSION-18" RecompiledFuncs/*.c`.

Build:
- `build-wasm/ogrebattle64.wasm` and `build-wasm/ogrebattle64.js` are dated Sep 8 21:06.
- No source file under `tools/N64ModernRuntime/ultramodern/src` or `RecompiledFuncs` is newer than the wasm.
- A fresh checkout/regeneration will lose the ignored `RecompiledFuncs` edits and wasm binary.

## Repro
- Server is currently responding `200` at `http://127.0.0.1:8931/`.
- Scratch probes live outside the repo: `/tmp/ogre-probe/probe-tap2.cjs`, `/tmp/ogre-probe/probe-tap.cjs`, `/tmp/ogre-probe/probe-delay.cjs`.
- Example: `node probe-tap2.cjs 40 5000 tapNN`.
- After editing runtime sources: `git -C tools/N64ModernRuntime diff HEAD > n64modernruntime-ob64.patch`.
- Wasm build used: `EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8`.

## Next session
1. First verify whether game-worker `printf` reaches `#status`; the missing `[heapwalk]` line may be an observability gap rather than proof the tripwire never fired.
2. Capture the walk start/current node through a channel that survives the wedge, or reduce sampling below 1s near the wedge window.
3. If the list is statically corrupt, identify the corrupting writer by catching `CYCLEvia` with node addresses/sizes.
4. Do not ship ignored-file edits as-is; if the mutex/tripwire approach survives, port it into the recompiler/generator/config or another tracked mechanism.
5. Untouched: latent unyielded loops elsewhere, wild-writer hunt, native-vs-browser behavior, leftovers from session 17.
