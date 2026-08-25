# Handoff — 2026-08-25 (session 6): boot stall diagnosed & fixed; the game now reaches its main loop

## TL;DR

- **The session-5 "stalls waiting for a second RSP task" was a red herring.** There is no
  missing second task: the first (sync) task's completion *does* run, but the game's boot
  thread then busy-spins and the runtime's **cooperative scheduler cannot preempt it**, so
  the SP/DP completion events (which would let the game proceed) are never delivered.
- **Three boot blockers were found and fixed** (all in the gitignored vendored runtime +
  N64Recomp — re-apply on re-clone; see "Changes" below):
  1. **`func_80089A10` busy-spin** (task-done wait) starves the external-message drainer →
     reimplemented as a yielding wait.
  2. **The game's own PI/DMA path is dead** because `osCreatePiManager_recomp` is an empty
     stub and the game's verbatim `osCreatePiManager` (which sets `D_800AA400`/
     `D_800AA408`) is dead code → `func_8008BC40` (the DMA-request starter) is
     reimplemented as a synchronous ROM read.
  3. **DMA byte order**: a raw `memcpy` of big-endian ROM bytes into RDRAM produces
     garbage pointers; `recomp::do_rom_read` (byte-swaps via `MEM_B`) is required.
- **Current state**: the game boots through the RSP task pipeline, the controller
  request/response path, and **703 streamed-overlay DMA loads**, and reaches its main loop
  (calling 18 distinct streamed functions, all log-and-return stubs). It then crashes on an
  **indirect call through a function-pointer table whose entries need streamed-overlay
  relocation** (the value read is a MIPS instruction, `lui $v0, 0x8019` = `0x3C028019`,
  not a pointer). **This is the start of Phase 4 (overlay loading + recompilation).**

## The three fixes (detail)

### 1. Task-done spin deadlock (`func_80089A10`)

Root cause: the runtime schedules game threads cooperatively (higher priority numbers
first). After submitting the first (sync) RSP task, the game thread (pri 10) enters
`func_80089A10`, a tight `bnez` spin on the task-done counter `D_800E79A4`. Because the
spin's branch targets the *function start* (not the branch's own address), N64Recomp does
**not** emit its `pause_self` yield for it, so the thread never yields. The task-done
pipeline needs the external-message **drainer** (pri 5) to deliver SP/DP completion events,
but the spinning game thread (pri 10 > 5) starves it → permanent deadlock after the first
RSP task.

Fix: reimplemented `func_80089A10` natively as a yielding wait:

```cpp
extern "C" void func_80089A10_recomp(uint8_t* rdram, recomp_context* ctx) {
    while (MEM_W(0, (gpr)(int32_t)0x800E79A4) != 0) {
        ultramodern::wait_for_external_message(rdram);
        ultramodern::check_running_queue(rdram);
    }
}
```

(Added to `reimplemented_funcs` in N64Recomp's `symbol_lists.cpp`.)

### 2. Dead PI manager → dead DMA (`func_8008BC40`)

Root cause: `osCreatePiManager_recomp` in the runtime is an **empty stub**. The game's
verbatim `osCreatePiManager` (0x8008B8C0) — the only writer of the game's PI globals
`D_800AA400` (manager active) and `D_800AA408` (DMA-completion queue) — is dead code.
The game's own DMA-request function `func_8008BC40` checks `D_800AA400` and returns −1
when it is 0, so the blocking DMA helpers `func_80089F80` / `func_8008A0F0` then
`osRecvMesg` forever on their completion queue. The boot's first streamed-code load
(`func_8009DA50`) deadlocks there.

Fix: reimplemented `func_8008BC40` natively as a synchronous ROM read. It reads the
game's `OSIoMesg` fields (mq@+4, dramAddr@+8, devAddr@+0xC, size@+0x10), calls
`recomp::do_rom_read`, and completes the request via `osSendMesg(mq, mesg, 0)` so the
caller's `osRecvMesg` returns immediately. This makes **all** game DMA paths
(`func_80089F80`, `func_8008A0F0`, …) work without the PI thread. (Added to
`reimplemented_funcs`; implementation in `librecomp/src/pi.cpp`.)

### 3. DMA byte order

A first attempt used `memcpy` from the ROM into RDRAM, which produced byte-swapped
garbage pointers (e.g. `0x1980023C`). RDRAM holds **host-order** 32-bit words (recompiled
`MEM_W` is a plain host deref) while the ROM is big-endian, so loads must go through
`recomp::do_rom_read` (which byte-swaps via `MEM_B`). After the fix the loaded jump table
at `0x8019A790` (from ROM `0x71280`) reads `0x80197C6C`… — correct pointers.


## Current state (verified this session)

- The game boots: renderer init → RSP threads → sync task (pipeline completes) →
  controllers (request/response via `0x800E7988` / `0x800E9B88`, SI event) →
  streamed-overlay DMA (703 `func_8008BC40` calls) → main loop.
- 18 distinct streamed functions are called (all log-and-return stubs):
  `0x800E9C20`, `0x800E9CEC`, `0x800EA714`, `0x800EA8E0`, `0x8016AF80` (the loaded overlay
  entry), `0x8016C900`, `0x8016CD50`, `0x8016CD90`, `0x8016CDCC`, `0x8016CE40`,
  `0x80173630`, `0x80173B50`, `0x80173B80`, `0x80177D94`, `0x80179080`, `0x8017BDE0`,
  `0x80184D90`, `0x80185110`.
- Then it crashes: `func_80075BC0` (jump-table builder, called from the main loop) reads a
  function pointer at `*(u32*)(*(u32*)(0x800E8294))` and gets `0x3C028019` (= `lui $v0,
  0x8019`, a MIPS instruction from the loaded streamed code). The streamed overlay's
  function-pointer tables need **relocation** (and the overlay's functions need
  recompilation/registration). **Phase 4 starts here.**
- `get_function` now has a **generic fallback**: unknown addresses in the streamed ranges
  `0x8016A000..0x80200000` and `0x84000000..0x84200000` return a logging no-op stub
  instead of hard-failing. This is what lets the game reach its main loop. Unknown
  addresses outside those ranges still hard-fail (with a host backtrace).

## Changes (vendored, gitignored — re-apply on re-clone)

N64Recomp (`tools/N64Recomp`, gitignored):
- `src/symbol_lists.cpp`: added `func_80089A10` and `func_8008BC40` to
  `reimplemented_funcs`.
- Rebuilt `tools/N64Recomp/build/N64Recomp`; re-ran `make recomp` (regenerates
  `RecompiledFuncs/` with `func_80089A10_recomp` / `func_8008BC40_recomp` calls).

N64ModernRuntime (`tools/N64ModernRuntime`, gitignored):
- `librecomp/src/recomp.cpp`: `func_80089A10_recomp`; drainer debug logs.
- `librecomp/src/pi.cpp`: `func_8008BC40_recomp` (synchronous ROM read).
- `librecomp/src/overlays.cpp`: `get_function` fallback stub for streamed ranges + host
  backtrace on hard-fail.
- Debug instrumentation (keep until boot is stable): `librecomp/src/ultra_translation.cpp`
  (`[mq]` logs with thread ids + `osCreateMesgQueue` logging), `librecomp/src/sp.cpp`
  (`[sp]` timestamp), `ultramodern/src/mesgqueue.cpp` (`do_send` watch list + blocked-on-
  receive log), `ultramodern/src/events.cpp` (timestamps), `ultramodern/src/scheduling.cpp`
  + `threads.cpp` (`[sch]` swap/resume logs), `ultramodern/include/ultramodern/
  ultramodern.hpp` (`trace_millis()`).

## How to reproduce

```sh
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release   # if reconfiguring
cmake --build build-app -j2
# Boot is flaky (segfaults right after renderer init ~50% of the time; rerun works):
for i in 1 2 3 4; do
  timeout -s KILL 40 stdbuf -oL -eL ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/boot.log 2>&1
  grep -q 'osSpTaskStartGo' /tmp/boot.log && break
  sleep 1
done
```

Expected (trimmed): `[renderer] RT64 renderer initialized (api=2)`, `[drainer] spawned`,
event registrations, the sync task pipeline (`osSpTaskStartGo` → `sp_complete` →
`dp_complete` → done message), controller request/response, hundreds of
`[pi] func_8008BC40 dir=0 …` lines, streamed-stub calls, then
`Failed to find function at 0x3C028019` (host backtrace → `func_80075BC0`).

## Next session

1. **Phase 4 — streamed overlays.** The loaded overlay at `0x8016AF80` (ROM `0x40E80`,
   size `0x265B0`) contains code and data with embedded pointers that need relocation.
   Work needed: (a) understand the game's overlay loader/relocation routine in the main
   segment; (b) either make the game's relocation pass work against host-order RDRAM, or
   reimplement the loader so the relocated tables are correct; (c) disassemble +
   recompile the overlay functions and register them (`add_loaded_function`) instead of
   stubs. The first indirect call to chase is `func_80075BC0` →
   `*(u32*)(*(u32*)(0x800E8294))`, where `0x800E8294 = func_80076150(...)`.
2. **RT64 GBI fix** (still pending): the first *real* display list will misparse until
   OB64's F3DEX-2.08 short-format GBI is handled (see `docs/guides/rsp-microcode.md`).
3. **Verify rendering** once real DLs flow (xwd capture).

