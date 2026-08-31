# Handoff — 2026-08-31 (session 15): boot-stall root-caused & fixed — missing yield_self (regression), not a message-queue deadlock

## TL;DR

- **The session-14 "VI-retrace message-queue deadlock" theory is superseded.** The
  real root cause of the boot-stall was a **regression**: the session-8
  `yield_self` poll-loop emission was **gone from the vendored N64Recomp fork**
  (the fork had been reset to upstream and `n64recomp-ob64.patch` no longer
  contained the hunks). Thread 3's pure spin at `0x80075FB8` (waiting on
  `D_800C4C26`) never yielded, so the cooperative scheduler's drainer could not
  deliver VI retraces → the game's event chain froze. The session-14
  "blocking osSendMesg fan-out" reading was wrong: the fan-out sends with
  `OS_MESG_NOBLOCK` (`a2=0` in `func_800891A0`).
- **Four runtime/toolchain bugs compounded the stall. All are fixed and verified
  in the browser** (headless Chrome, `build-wasm`):
  1. N64Recomp: reimplemented `yield_self` emission for poll loops
     (`is_yield_poll_loop` + `emit_yield_self`) → 99 yields emitted across all
     segments, including both branches of thread 3's spin.
  2. Scheduler: `run_next_thread_and_wait` threw `No threads left to run!` when
     every game thread was blocked (a legitimate idle state; the drainer/VI
     thread wakes them). It now idles on external messages instead of throwing
     (killing the worker).
  3. `osSendMesg`/`osRecvMesg` drained the **entire** external-message backlog
     on every call; with a stalled game this grew unboundedly and each call took
     seconds (positive feedback). Drain is now capped at 32 messages/call.
  4. The chatty runtime debug traces (`[ev]`/`[mq]`/`[sch]`/`[drainer]`) slowed
     the headless browser ~100× (each printf is a JS proxy hop). Gated behind
     `OGRE_DEBUG_TRACES=1`; the periodic `[snap]` queue snapshot stays on.
- **Result:** the web build now boots to a **stable, healthy idle state** — no
  crash, no deadlock, every message queue draining, the VI retrace pipeline
  running at 60 Hz (snapshot: `0x800E8B84`/`0x800E7988`/`0x800C4C28` empty with
  their consumers blocked on recv — the correct resting state).
- **Next wall (game behavior, not runtime):** the game never advances past the
  boot blanking display list (still `tasks=1 dls=2 cmds=32`, no draw commands).
  The frame-producer thread (t17, `func_800893C0`, waits on `0x800E8B4C`) never
  runs because thread 3 (the system/audio scheduler) is blocked on a **count=1
  queue at `0x800C6C98`** it created itself and no one sends to. The phase byte
  `D_800C4800` stays 0. Leads below.

## The diagnosis (how we got here)

1. **Snapshot instrumentation** (kept in the runtime): the VI thread (a host
   thread) dumps every ~1.5 s: the OB64 queues (`[snap] mq=… count recv_wait
   send_wait`), per-thread block state (`BLOCKED on recv/send of …`), each
   thread's last recompiled function (`recomp_trace_func` emitted at every
   recompiled function entry; `debug_last_func_vram`), the retrace-handler list
   at `0x800E9178`, the dispatcher callback list at `0x800B39240`, and key game
   vars (`D_800C4800` phase, `D_800AEF98` boot state, `D_800C4C26` spin word,
   `D_800E8B10/12` fan-out masks, `D_800E918D` frame-producer byte,
   `D_800E79A4` RSP-done counter).
2. With traces on, every run stalled differently (racy): sometimes the VI-manager
   (t19) "stuck in `func_800891A0`" with `0x800E8B84` full, sometimes a worker
   died on `run_next_thread: running queue EMPTY!`, sometimes a
   `memory access out of bounds` (my first unguarded list dump — fixed).
3. **The key realization:** with traces **off** (gated), the exact same boot
   runs **fast** and reaches the healthy idle state — proving the stall was
   scheduler starvation + trace overhead, not game logic. The remaining "stuck"
   is the game's own wait.

## Changes (this session)

### N64Recomp fork (`tools/N64Recomp`, gitignored — re-apply via `n64recomp-ob64.patch`)
- `src/recompilation.cpp` — `is_yield_poll_loop` lambda (backward in-function
  branch; loop body call-free & store-free; ≥1 memory read whose base register
  is never written by a non-constant op in the body) + `emit_yield_self()`
  before the branch goto. Rebuilt `tools/N64Recomp/build/N64RecompCLI`;
  regenerated `RecompiledFuncs/` with `make recomp` (99 `yield_self(rdram);`
  emissions).
- `emit_function_start` gained a `function_vram` param; `CGenerator` emits
  `recomp_trace_func(rdram, 0x{:08X});` at every function entry (deadlock
  diagnostics). LiveGenerator emits nothing.
- `include/recompiler/generator.h`, `include/recompiler/live_recompiler.h`,
  `src/cgenerator.cpp`, `LiveRecomp/live_generator.cpp` updated accordingly.
- **`n64recomp-ob64.patch` regenerated** to include all of the above.

### N64ModernRuntime (gitignored — re-apply via `n64modernruntime-ob64.patch`)
- `ultramodern/src/threads.cpp` — `run_next_thread_and_wait` now idles on
  external messages when the running queue is empty (handles the
  "woken self in queue" case), instead of `run_next_thread` throwing.
- `ultramodern/src/mesgqueue.cpp` — drain cap (`MAX_DRAIN_PER_CALL = 32`) in
  `dequeue_external_messages`; per-thread block-state tracking
  (`thread_block_kind`/`thread_block_queue`, set in do_recv/do_send block
  loops); `debug_dump_queue_snapshot` (queue states, block states, handler/cb
  lists, game state vars); `debug_traces_enabled()` (env `OGRE_DEBUG_TRACES`).
- `ultramodern/src/function_trace.cpp` (**new**) — `recomp_trace_func` +
  `debug_last_func_vram`; added to `ultramodern/CMakeLists.txt`.
- `ultramodern/src/events.cpp` — periodic snapshot call in `vi_thread_func`;
  `[ev]`/`[vi-debug]` prints gated.
- `ultramodern/src/scheduling.cpp`, `threads.cpp` — `[sch]` prints gated.
- `ultramodern/include/ultramodern/ultramodern.hpp` — `OGRE_TRACE` macro,
  `debug_traces_enabled`, `debug_last_func_vram` declarations.
- `librecomp/src/ultra_translation.cpp`, `sp.cpp`, `recomp.cpp` — `[mq]`/`[sp]`/
  `[drainer]` prints gated.
- `N64Recomp/include/recomp.h` — declared `yield_self` and `recomp_trace_func`.
- **`n64modernruntime-ob64.patch` regenerated** (includes the new
  `function_trace.cpp` via intent-to-add).

### App (`app/`, tracked)
- `app/src/main.cpp`, `app/src/main_web.cpp` — enable `requeue_vi`/`requeue_ai`
  in `MessageQueueControl` so dropped VI retraces/AI events are retried instead
  of lost (keeps the frame state machine from missing retraces when a queue is
  momentarily full).

## Verification (web build, headless Chrome)

- `probe-full.cjs` (captures the full `#status` text, filter disabled): boot now
  reaches the healthy idle state and stays there 60 s — no
  `running queue EMPTY!`, no worker errors, no full queues.
- With `OGRE_DEBUG_TRACES=1` (add an `ENV`/`Module.ENV` hook or just run the
  same probe; the gate is `getenv`), the full `[mq]`/`[sch]`/`[ev]` story is
  still available for future debugging.
- Native `build-app` rebuilds cleanly; running it under this sandbox hangs at
  RT64 renderer init (headless/GPU sandbox) and needs a real session — expected
  to show the same boot progress as the browser (the runtime fixes are
  platform-independent).

## Next wall (leads)

The game is now waiting, not deadlocked. The boot state machine:
`thread 3 (func_80075BC0, the system/audio scheduler)` is **blocked on recv of
`0x800C6C98`** (a count=1 queue it created at boot T≈82 alongside `0x800C6CA8`/
`0x800C6490`; buffers `0x800C6CB0`/`0x800C6CD0`). No static code references
`0x800C6C98` — the sender computes it at runtime (audio/AI DMA path?). Until
that message arrives, thread 3 never sends to `0x800E8B4C` (`osSendMesg →
0x800E8B4C msg=0x800E7D90 flags=1`), which is what wakes the **frame producer**
thread (t17, `func_800893C0` → `func_8008949C` → registers retrace handler on
`0x800E8C2C`, waits for the VI framebuffer to match, calls `osViSwapBuffer`).
With no `osViSwapBuffer`, no real display lists are built → black screen.

Leads for the next session:
1. **Find the sender of `0x800C6C98`.** Enable `OGRE_DEBUG_TRACES=1` and grep
   the `[mq] osSendMesg` lines for a target whose value changes at the stall;
   or run with the snapshot and compare `0x800C6C98`'s `recv_wait`/`send_wait`
   over time. The queue is created by thread 3's audio-init path (the
   `0x800C4A00`/`0x800E9BF0`/`0x800C6CA8`/`0x800C6C98` cluster at boot T=81-82).
2. **AI event emulation:** the runtime has no AI register/DMA emulation at all
   (`0xA4500000` writes go nowhere; no `osAi*`). If `0x800C6C98` is an
   AI-buffer-done wait, the game will never advance without AI interrupt
   emulation. Milestone 5's note ("game never registers OS_EVENT_AI at the
   title") may be wrong or early — verify what the audio path needs.
3. **PRENMI (0x29D):** the phase byte `D_800C4800` stays 0; it is only set to 1
   by the 0x29D (PRENMI) message the runtime never sends. Native sessions 9-14
   reached 1864 RSP tasks without it, so it is probably not required for the
   title screen — but if the audio path stays blocked, injecting a synthetic
   PRENMI to `0x800E8B84` is a cheap experiment to see whether the boot
   proceeds (mark it clearly as a hack).
4. **Raw MMIO writes:** the game writes SI/AI registers (`0xA4040000…`,
   `0xA4500000…`) that the recompiled code maps into garbage rdram
   (`& 0x1FFFFFFF`), corrupting memory. Session 14 noted this; it may corrupt
   the very queue/audio state the boot waits on. A runtime MMIO write guard
   (trap/mask `0xA0000000-0xC0000000` writes) is worth adding eventually.
5. **Native validation:** run `build-app/ogrebattle64` on a machine with a real
   GPU (or under gdb to avoid the flaky renderer-init segfault) to confirm the
   native boot now passes the session-14 stall and reaches the same healthy
   title-screen state as the browser.

## Files

- `n64recomp-ob64.patch`, `n64modernruntime-ob64.patch` — regenerated (apply
  with `git apply` after re-cloning the vendored dirs).
- `app/src/main.cpp`, `app/src/main_web.cpp` — VI/AI requeue config.
- `docs/WEB-PORT.md` — milestone 8 note updated (see below).
- Probes in `/tmp/ogre-web-test/`: `probe-full.cjs` (full status capture),
  `probe-vi.cjs`, `probe-gfxstatus.cjs` (older).

---

## Round 2 addendum (same session): audio path unblocked — PI-manager emulation + MMIO/RSP fixes

### What changed since the first write-up

1. **MMIO scratch mapping (recomp.h `recomp_mem_addr`)** — all raw KSEG1/MMIO
   accesses (`0xA0000000-0xC0000000`: AI `0xA4500000`, SI `0xA4040000`, VI
   `0xA4400000`, PI `0xA4800000`) now alias into a reserved 8 MiB page at the
   end of rdram (scratch offset `0x1F800000`, heap shrunk accordingly in
   `librecomp/src/heap.cpp`). This fixed: (a) the wasm "memory access out of
   bounds" crashes from raw register reads, (b) the game silently corrupting
   heap memory with garbage MMIO writes, and (c) made `AI_STATUS` etc. read as
   zero ("idle"). **Critical detail:** N64 addresses flow through the recompiled
   code sign-extended to 64 bits, so the mapper must normalize to the low 32
   bits first (`(uint64_t)(uint32_t)addr`) — the first version (plain
   `addr - 0x80000000`) broke 64-bit native builds (masked only by wasm's 32-bit
   truncation).
2. **RSP DMA address bug (`librecomp/include/librecomp/rsp.hpp`)** —
   `dma_rdram_to_dmem`/`dma_dmem_to_rdram` built addresses as
   `(int32_t)(dram_addr + i + 0x80000000)` — a trick for the old macro that the
   normalized `recomp_mem_addr` breaks. Fixed with `to_kseg0()`; this was the
   crash during the audio RSP task burst.
3. **PI-manager emulation (`librecomp/src/pi.cpp` + mesgqueue do_send hook)** —
   the game's own (non-bridged) DMA path (`func_8008BC40`, called *directly* by
   `func_80089F80` etc.) reads the PI-manager globals `D_800AA400` (must be
   non-zero) and `D_800AA408` (the request queue). The runtime's
   `osCreatePiManager` stub never set them, so the recompiled `func_8008BC40`
   returned -1 and DMA callers blocked forever on their completion queue —
   **this was the real 0x800C6C98 stall** (round 1's open question). Now
   `osCreatePiManager_recomp` initializes the globals and `do_send` detects
   sends to the emulated request queue and completes each DMA synchronously
   (ROM copy + completion message to the OSIoMesg's `mq`).
4. **Snapshot hardening** — the watched-queue loop derefs `blocked_on_recv`
   heads through `TO_PTR`; overlapping audio queues (`0x800C6C98`/`0x800C6CA8`)
   put garbage in those fields and crashed the VI-thread snapshot. Guarded with
   a KSEG0 range check.
5. **PRNMI injection tooling** — `ogre_send_prnmi()` (exported) delivers a
   synthetic 0x29D to the VI-manager queue via a mutex-protected pending-send
   list drained by game threads (foreign-thread direct `do_send` races with the
   cooperative scheduler).

### Result

The boot now advances dramatically in the browser:
- The game's audio-path DMAs complete (inline DMA logs, `mq=0x800C6C98`).
- **77+ audio RSP tasks (type=2)** are submitted (the runtime RSP stub completes
  them instantly).
- Thread 3 runs its audio loop (`func_80080EE8`/`0x80080F78`) and waits on its
  audio queue `0x800E79C8` (registered as a retrace handler, fl=0x3).
- Thread 4 executes **overlay-C code** (`0x80199EEC` — the title-screen region).
- Boot state machine reaches `bootstate(D_800AEF98)=0x100`, `spin=0xFFFF`.
- No crashes in traces-off runs.

### Next wall (round 3 leads)

The game settles into a wait: `D_800C4800` (phase byte) stays 0, only the
blanking DL is submitted (`tasks=1`), and the **VI-manager (t19) stops draining
`0x800E8B84`** (the queue fills 8/8; retraces dropped). t19's last function is
`0x800891A0` (the retrace fan-out) while "not blocked in mesg" — it is stuck
mid-cycle (or parked waiting on a thread that never blocks). Candidates:
1. The fan-out walks the retrace-handler list at `0x800E9178`
   (`[0x800E79C8 fl=3, 0x800E7988 fl=1, 0x800C4C28 fl=3]`); if a node's `next`
   is garbage the walk loops/spins. The nodes live on the registering threads'
   stacks — verify none of those functions returned.
2. Thread 4 in overlay-C code (`0x80199EEC`) may be in an unyielded poll loop
   (the session-8 `yield_self` heuristic only converted ~17 loops; overlay-C
   may have more). The audio thread's mask-2 work (`0x800E8B12` fan-out, only
   fired by the 0x29D path) never runs, so thread 3 never triggers the frame
   producer (`0x800E8B4C` → t17 → `func_8008949C` → `osViSwapBuffer`).
3. The external-message backlog with `requeue_vi=true` still starves non-VI
   messages in the FIFO (PRNMI sits behind the retrace stream); the direct-send
   mechanism bypasses it, but the VI-manager must be draining for any of this to
   matter.

---

## Round 3 addendum (same session): VI-retrace pacing + audio auto-response

### What changed

1. **VI retrace requeue disabled** (`app/src/main.cpp`, `app/src/main_web.cpp`):
   requeuing dropped retraces kept the external-message backlog alive, which
   flooded the VI-manager's queue so it never blocked on recv — starving every
   lower-priority thread parked in the running queue (the cooperative scheduler
   only preempts to a strictly higher priority). On real hardware retraces are
   paced by the VI interrupt; dropping a retrace when the queue is full just
   makes the game wait for the next one.
2. **Audio-response auto-fix** (`mesgqueue.cpp do_recv`): the game's audio
   driver blocks on `0x800C49E8` waiting for the audio task's buffer to finish
   playing, which requires the RSP audio ucode (type-2 task, ucode
   `0x8009E050`) to produce output and the AI DMA to complete — neither
   emulated. When a thread blocks on `0x800C49E8`, a dummy response is queued
   through the pending-direct-sends list so the boot proceeds (audio stays
   silent until the ucode is recompiled via RSPRecomp).
3. **Pending-direct-sends + `ogre_send_prnmi`** (`mesgqueue.cpp`): a
   mutex-protected list of {mq,msg} pairs drained by game threads in
   `dequeue_external_messages` / `wait_for_external_message`, so a foreign
   thread (JS export) can inject messages without racing the cooperative
   scheduler. The synthetic PRENMI (0x29D) uses it.

### Result / verification

- Thread 3 (system/audio scheduler) now reaches its **normal audio loop**
  (`func_80080EE8` on `0x800E79C8`) — the `0x800C49E8` wait is auto-satisfied.
- The boot still does NOT advance past the title gate:
  `bootstate(D_800AEF98)=0x100`, `phase(D_800C4800)=0`, dispatcher callback
  list empty, frame-producer thread (t17) still waiting on `0x800E8B4C`, and
  only the boot blanking DL is submitted (`tasks=1`).
- A racy "memory access out of bounds" still occurs during the audio RSP task
  burst (kills one worker; the rest continue) — game-side, likely the audio
  task output processing reading a bad address.

### Next wall (leads)

The phase byte `D_800C4800` is only set to 1 by the VI-manager's 0x29D (PRENMI)
handler, which also fans out the mask-2 flags (`0x800E8B12`) that drive the
audio/graphics per-frame work. Synthetic PRNMI delivery works (the message
reaches `0x800E8B84`) but the phase still doesn't advance — verify the
VI-manager actually receives it (queue may be full/dropped) and that
`D_800E9188` (the PRENMI frame-count divisor) is non-zero, or the boot state
machine's `func_80072398` registration (D_800C4C26 == 1/2 path) may be the real
gate. Running the real audio ucode (RSPRecomp, `docs/guides/rsp-microcode.md`)
would produce real task output and likely unblock both the crash and the
0x800C49E8 response naturally.
