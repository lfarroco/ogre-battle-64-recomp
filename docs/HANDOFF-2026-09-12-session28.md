# Handoff — 2026-09-12 — session 28: the game renders natively

## Outcome

Session 27 put a *synthetic* frame on the native screen and closed with one open
item: "the game still submits only its boot blanking display list". This session
found out why, fixed it, and the native build now renders **the game's own title
scene** (`docs/proofs/native-game-title.png`).

The cause was not a game bug and not a renderer bug. It was a **scheduler bug in
the runtime**: a thread parked in `wait_for_resumed` could be woken by a
*message-queue poke* instead of a real handoff, so two game threads ran at once,
the cooperative scheduler's invariants broke, and the boot thread (priority 10)
was eventually left parked with an empty running queue - starved forever while
the per-VI service threads ping-ponged at priority 50-120.

Three changes, in order of importance:

1. **Handoff and poke are now separate semaphores** (`UltraThreadContext::running`
   vs `::poke`). `wake_blocked_head` pokes `poke`; only a pop from the running
   queue or `resume_thread_and_wait` signals `running`. `wait_for_resumed` can
   therefore no longer return without a handoff. 6/6 native runs now reach the
   frame loop and submit 8 display lists where 4/6 used to strand in
   `func_80089804` with 1.
2. **The queue helpers refuse a `NULL` queue** instead of resolving
   `TO_PTR(NULLPTR)` out of bounds (`osSetThreadPri`/`osDestroyThread` can be
   called on a running thread, whose `queue` is `NULLPTR`).
3. **The VI thread's periodic queue snapshot stopped dereferencing wild KSEG0
   values.** Its guards accepted `0x80000000..0xA0000000`, but only the game's
   RDRAM window is a real `OSThread`; with input the game reached a state where a
   queue held `0x8606FF09`, the snapshot read past the committed mapping, and the
   VI thread took an `EXC_BAD_ACCESS` (3 of 4 tap runs). Guards are now
   `0x80000000..0x80800000`.

## The wall (how it was measured)

`OGRE_SCHED_TRACE=1` was added: every scheduler operation (insert/pop/remove/
resume/park/wake/swap) is recorded in a lock-free ring with a **global sequence
number** and the running queue after it, then dumped at exit (`[sched-ring]`).
The `[Sched]` printfs alone were not enough: they come from several host threads
and interleave through a buffered stdout, and the question ("who removed thread 3
from the running queue without resuming it?") is entirely about ordering.

What the ring showed on a stalling run:

```
189 park    t3 by t3  rq=[3,200]     <- boot thread parks (func_80089804 -> osSendMesg
                                        -> check_running_queue -> swap_to_thread)
190 wake    t17 by t17 rq=[3,200]
...
198 wake    t17 by t17 rq=[17,3,200]
199 insert  t17 by t17 rq=[17]        <- t3 and t200 vanish from the list
200 pop     t17 by t17 rq=[-]
...
```

and after event 189 there is **no `pop t3`, no `resume t3`, no `wake t3` for the
rest of the run**, while all other threads end up blocked on `recv` and the
running queue is empty. The `[snap] resumes:` line corroborates it: `t3=3` total
resumptions, all in the first 200 ms.

The spurious wake is visible directly as `park t17` immediately followed by
`wake t17` (a poke consumed as if it were a handoff). When that happens inside
`swap_to_thread`, the yielder keeps running *and* the target it "handed off to"
starts running: two game threads in the scheduler at once, which is what lets the
running queue lose entries.

`func_8008A1B0` is the frame submit (`func_80089804`) followed by the wait
(`func_80089A10`); the boot thread was parked inside the submit's `osSendMesg`
and never got another turn. `func_80089A10`'s generated code already contains the
`yield_self` poll-loop yield, so it was never the blocker.

## What changed

### Runtime — scheduler (`tools/N64ModernRuntime/ultramodern/`)

* `include/ultramodern/ultramodern.hpp` — `UltraThreadContext::poke` (a second
  `LightweightSemaphore`) with the comment that explains the handoff/poke split.
* `src/mesgqueue.cpp` — `wake_blocked_head` signals `poke`, not `running`. The
  idle path of `run_next_thread_and_wait` reaps `poke` (unchanged mechanism, new
  semaphore). The `[snap]` queue snapshot now guards blocked-head derefs with the
  RDRAM window (`0x80000000..0x80800000`), as do its `valid_ptr`, assettab,
  frame-dispatch and title-dispatch guards. `sched_traces_enabled()` (new,
  `OGRE_SCHED_TRACE=1`).
* `src/threadqueue.cpp` — `queue_to_ptr` refuses a `NULL`/non-KSEG0 queue (and
  every helper handles the refusal); `debug_running_queue_snapshot()` (new) fills
  a short tid list for the event ring.
* `src/threads.cpp` — `wait_for_resumed` records park/wake into the ring and
  prints under `OGRE_SCHED_TRACE`; `resume_thread` records resumes.
* `src/scheduling.cpp` — `swap_to_thread` records the swap.
* `src/function_trace.cpp` — the scheduler event ring itself
  (`debug_sched_event*`, `debug_dump_sched_ring`), 16 k events, each with the
  running queue after it.
* `include/.../ultramodern.hpp` — `debug_printf` is no longer compiled out; it is
  gated on `OGRE_SCHED_TRACE`, so the (already written) thread-queue traces are
  usable.

### App

* `app/src/sdl_platform.cpp` — at `OGRE_EXIT_AFTER_MS`, dump the scheduler event
  ring when `OGRE_SCHED_TRACE` is set (`OGRE_SCHED_TRACE_TID` filters it).

### Docs / proofs

* `docs/proofs/native-game-title.png` (animated title, sparkles + smoke) and
  `native-game-title-still.png` (the twelve soldiers) — captures of the presented
  swap chain, `OGRE_TAP_MS=4000 OGRE_CAPTURE_PRESENT=... OGRE_CAPTURE_AFTER=30`.
* `docs/DECISIONS.md`, `PLAN.md`, `docs/guides/app-build.md` — session-28 entries.

## Evidence

### The game's display lists reach the renderer

```
[renderer] display list 5 (type=1 ucode=0x8009F540 data=0x801C80A0)
[renderer] display list 6 (type=1 ucode=0x8009F540 data=0x801C1520)
[renderer] display list 7 (type=1 ucode=0x8009F540 data=0x801C80A0)
[renderer] display list 8 (type=1 ucode=0x8009F540 data=0x801C1520)
...                                   (50+ in a 20 s run, ~2.5/s)
```

`0x801C1520` / `0x801C80A0` alternating is the game's double-buffered frame pair;
`0x800C6500` is the boot blanking list that used to be the only one.

### The presented swap chain has the title scene

`OGRE_CAPTURE_PRESENT` frames: non-black sampled pixels climb 2109 -> 3195 ->
8857 -> 9218 across a 25 s run (the title fades/animates in), and the image is
the twelve-soldier ring with the smoke puff and sparkles.

### The boot thread's chain

```
# before (one of the 4/6 stall runs)
callchain t3 at exit (3 deep): func_80071EB0 func_8008A1B0 func_80089804
# after
callchain t3 at exit (16 deep): func_80071EB0 func_800E9CEC func_80080DC0 ...
```

(`func_800E9CEC` is overlay-A code, so the boot now runs the game's own overlay.)

## Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean |
| `cmake --build build-null -j 8` | clean |
| `cmake --build build-wasm -j 8` | clean |
| no-input native run, 8 s | 6/6 runs exit 0 with 8 display lists |
| tap run (`OGRE_TAP_MS=3000`), 12 s | 4/4 runs exit 0 with 9 display lists (was 1/4, the other 3 `SIGSEGV`) |
| presented frames | title scene captured and animating, `docs/proofs/native-game-title*.png` |
| session-27 probe regression | `OGRE_SYNTH_FRAME=1` still presents the seven bars |
| 20 s run | 50+ display lists, no crash |

## Still open

0. **"The window never changes" is no longer true**, so the presenter knobs
   (`OGRE_PRESENT_ALWAYS`, `OGRE_PRESENT_FBTARGET`) are diagnostics again and the
   probe still needs them. Session-27 open item 1 (make `FBTARGET` a default)
   still stands on its own merits.
1. **The 512 MiB RDRAM mapping is now the thing that bit us twice.** A stray
   KSEG0 value inside `0x80000000..0xA0000000` can point outside the committed
   game RAM; the queue snapshot did exactly that. The rest of the runtime still
   uses `0xA0000000` as "valid KSEG0" in a few places (`thread_ptr_valid` in
   `threadqueue.cpp`, the mesgqueue snapshot's `mq` list reads). Those are safe
   today (512 MiB is committed) but only just - audit before trusting them.
2. **The periodic queue snapshot is expensive and unconditional**: every ~1.5 s
   the VI thread prints ~40 lines, samples `last_func_vram` 100x at 200 us
   (`debug_sample_hot_loop`, ~20 ms) and walks the malloc graph. It should be
   behind an env gate (and probably off by default) before the port is used by
   anyone but us.
3. **The tap path's real target is untested**: this session only proves the game
   *renders* and *accepts* Start. Whether Start advances the title to the menu,
   and what breaks next, is the next measurement.
4. Untouched from sessions 24/26/27: the remaining combiner inputs
   (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys), coverage/alpha compare,
   framebuffer/VI indirection, the TMEM model, `G_LOADBLOCK`'s `dxt`, the
   `func_80089A10` spin (still real, still only yields because N64Recomp emits
   `yield_self`), and the `[snap]` retrace-handler/dispatcher walkers that print
   an empty list while the fan-out runs.
5. **Audio is still the stub microcode** (360 type-2 tasks in an 8 s run, all
   auto-completed), so the game's audio pipeline runs on garbage; do not use its
   timing as a fidelity signal.

## Repro

```sh
# build (RT64 + plume patches already applied in this checkout)
cmake --build build-app -j 8

# THE measurement: the game's own frames, captured off the presented swap chain
OGRE_TAP_MS=4000 OGRE_CAPTURE_PRESENT=/tmp/game OGRE_CAPTURE_AFTER=30 \
  OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64
# /tmp/game.31.ppm ... -> the title scene's twelve soldiers

# just the frame-loop health check (display-list count + exit code)
OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64 2>&1 | grep -c '\[renderer\] display list'

# scheduler forensics: the race-free event ring, filtered to the boot thread
OGRE_SCHED_TRACE=1 OGRE_SCHED_TRACE_TID=3 OGRE_EXIT_AFTER_MS=3000 \
  ./build-app/ogrebattle64 2>&1 | sed -n '/sched-ring/,$p'

# session-27 regression: the synthetic probe still presents the seven bars
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=900 OGRE_SYNTH_PERIOD=30 OGRE_NO_DUMMY_VI=1 \
  OGRE_CAPTURE_PRESENT=/tmp/frame OGRE_CAPTURE_AFTER=200 \
  OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
```

## Files changed (this session)

- `tools/N64ModernRuntime/ultramodern/include/ultramodern/ultramodern.hpp` —
  `poke` semaphore, scheduler-ring declarations, `sched_traces_enabled`,
  `debug_printf` gate.
- `tools/N64ModernRuntime/ultramodern/src/mesgqueue.cpp` — handoff/poke split,
  RDRAM-window guards in the snapshot, `sched_traces_enabled`.
- `tools/N64ModernRuntime/ultramodern/src/threadqueue.cpp` — `NULL`-queue guards,
  `debug_running_queue_snapshot`, ring events.
- `tools/N64ModernRuntime/ultramodern/src/threads.cpp` — park/wake/resume ring
  events + `[Sched]` traces.
- `tools/N64ModernRuntime/ultramodern/src/scheduling.cpp` — swap ring event.
- `tools/N64ModernRuntime/ultramodern/src/function_trace.cpp` — the scheduler
  event ring.
- `app/src/sdl_platform.cpp` — dump the ring at exit under `OGRE_SCHED_TRACE`.
- `n64modernruntime-ob64.patch` — regenerated.
- `docs/proofs/native-game-title.png`, `native-game-title-still.png` (new).
- `docs/HANDOFF-2026-09-12-session28.md` (new), `docs/DECISIONS.md`, `PLAN.md`,
  `docs/guides/app-build.md`.
