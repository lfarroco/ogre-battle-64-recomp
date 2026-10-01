# Session 116 — the every-1.5 s stutter is the runtime's unconditional periodic `[snap]` snapshot

Date: 2026-10-01. Follows session 115 (the unit-command menu's fifth injected
work-loop yield).

## Goal

Player bug report: *"Every couple of seconds I experience a stutter, like
clockwork. Game does run, but with the stutter non-stop."* The reporter assumes
the Windows build, because most players are there. Find obvious improvements;
the machine available is a Mac. The reporter suggested looking at Zelda 64:
Recompiled for tips.

## Result

The stutter is `ultramodern::debug_dump_queue_snapshot`, called
**unconditionally every 90 VI frames (1.5 s)** from `vi_thread_func`. It is a
hang-diagnosis dump added by this project (the upstream runtime has no such
call), and session 28 had already flagged it as "expensive and unconditional ...
should be behind an env gate (and probably off by default)". It is now behind
`OGRE_SNAP`, and off by default.

Measured on macOS, 40 s runs, frame gap between consecutive display-list
submissions, steady state (t > 3 s), at the publisher stills and title:

| run | DLs | p50 gap | p90 gap | max gap | `[snap]` lines | spikes > 1.5x p50 | their phase mod 1500 ms |
|---|---|---|---|---|---|---|---|
| before (`base.log`) | 1121 | 33 ms | 35 ms | 866 ms | 26 | 27 | **23 of 27 in the 800-899 ms bin** |
| after (`fix.log`) | 1147 | 33 ms | 35 ms | 316 ms | 0 | 2 | 2 boot spikes, no fixed phase |
| after, `OGRE_SNAP=1` (`on.log`) | 1006 | 33 ms | 48 ms | 952 ms | 25 | 40 | degradation returns |
| after, default again (`off2.log`) | 1091 | 33 ms | 35 ms | 334 ms | 0 | 9 | scattered, no fixed phase |

Before the fix the spikes are 33 ms -> 59 ms, one frame every 1499-1502 ms, and
23 of the 27 spikes land in the same 100 ms phase bin of a 1500 ms period. That
is the reported "like clockwork". Enabling the snapshot with `OGRE_SNAP=1`
brings the degradation back, so the snapshot is the cause and not a coincidence
of the period.

The cost is the snapshot's own contents, not one call inside it:
`ultramodern::debug_sample_hot_loop(4)` sleeps 100 times at 200 us (20 ms
alone), `debug_dump_call_chain` runs for every parked thread,
`debug_dump_alloc_ring`/`debug_dump_alloc_trans` walk the allocator, the heap
graph walk visits up to 2000 edges per bucket, and ~80 lines are printed. It all
runs on the VI thread, whose own comment says it "should be prioritized over
every other thread in the application, as it's what allows the game to generate
new audio and gfx lists", so the retrace it exists to observe is delivered late.

## Why Windows is worse

`debug_sample_hot_loop` uses `std::this_thread::sleep_for(200us)` per sample.
macOS honors that closely. Windows resolves it to the system timer: SDL2 sets
`timeBeginPeriod(1)` unless `SDL_HINT_TIMER_RESOLUTION` says otherwise
(`tools/SDL2-static/SDL2-2.32.10/src/timer/windows/SDL_systimer.c:49-63`, default
period 1 ms), so 100 samples cost ~100 ms instead of ~20 ms. This is the
mechanism by which a hitch that measures 26 ms here would be several times
larger on the reporter's machine. It is inferred from the SDL source and the
Windows timer model; it is **not** measured on Windows in this session. The fix
removes the dependence on timer granularity either way.

## Files changed

- `patches/n64modernruntime-ob64.patch` — the gate, in
  `ultramodern/src/events.cpp`. `tools/N64ModernRuntime/` is vendored
  (gitignored) and the patch is the tracked copy of it, so the patch is the
  change; the vendored tree carries the same edit for local builds.
  - `snapshot_period_frames()` reads `OGRE_SNAP` once outside the VI loop:
    unset/`0`/`off`/`false`/`no` is 0 (disabled); `1`/`on`/`true`/`yes` is the
    default 90 VI frames; a number > 1 is that period. Anything else is disabled,
    so a typo cannot silently re-enable it.
  - the call site became `if (snapshot_period > 0 && ++snapshot_counter >= snapshot_period)`.
- `docs/guides/app-build.md` — the `OGRE_SNAP` row in the knob table, the
  existing "periodic `[snap]` snapshot" troubleshooting item now names the gate,
  and a "A repeated stutter" section documents `tools/dlgaps.py`.
- `tools/dlgaps.py` — a new offline tool that reads an `OGRE_DL_TRACE=1` series
  and reports the gap distribution plus the phase histogram of the spikes, so
  "the game stutters every couple of seconds" becomes a number and a period.
  `--check` exits 1 when 5 or more spikes share one tenth of `--period`.
- `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` — the
  record.

No generated code changed, no ROM data, no app source. No probes were added to
generated code, so nothing needed reverting.

## Verification

- `/tmp/stutter/{base,fix,on,off2}.log` — four 40 s runs, `OGRE_DL_TRACE=1
  OGRE_EXIT_AFTER_MS=40000`, each with its own fresh `OGRE_PREF_DIR`. Only the
  snapshot gate differs between them (rule 5).
- `tools/runlog.py --check /tmp/stutter/off2.log` on the fixed binary:
  `PASS: no crash, no stub call, no unknown module, no bad RSP exit, no stubbed
  non-gfx task`, 1091 display lists.
- `tools/dlgaps.py` on all four logs: `base.log` reports 27 spikes, 85 % of them
  in one 1500 ms phase bin, `PERIODIC` and exit 1; `fix.log` and `off2.log`
  report 2 and 9 spikes with no repeated phase, `not periodic` and exit 0. The
  same analysis was first done ad hoc, and the tool is its reusable form.
- `cmake --build build-app --target ogrebattle64` relinked `build-app/ogrebattle64`
  from the edited vendored tree.
- Patch fidelity, so the change survives the documented `git apply` path:
  `git -C tools/N64ModernRuntime diff` reproduced
  `patches/n64modernruntime-ob64.patch` exactly before the edit (modulo the
  nested `N64Recomp` submodule pointer, which the patch does not carry);
  `git worktree add` of the pinned commit `589bbf0` plus
  `git apply --check patches/n64modernruntime-ob64.patch` succeeded; and
  `git apply --check -R` in the vendored tree succeeded, so the tree equals the
  patch's post-image. The "new blank line at EOF" warning `git apply` prints is
  pre-existing (the original patch produces it too).

## Evidence at instruction level

`tools/N64ModernRuntime/ultramodern/src/events.cpp` `vi_thread_func`:

```
        if (++snapshot_counter >= 90) {
            snapshot_counter = 0;
            ultramodern::debug_dump_queue_snapshot(events_context.rdram);
        }
```

`ultramodern::debug_sample_hot_loop` in `function_trace.cpp:599-611` is
`for (int i = 0; i < 100; i++) { ... std::this_thread::sleep_for(std::chrono::microseconds(200)); }`.

The snapshot's own body is `mesgqueue.cpp:183-467`: 16 watched queues, 10
threads with `debug_last_func_vram` and `debug_dump_call_chain`, the dispatcher
and retrace-handler list walks, `debug_dump_running_queue`,
`debug_sample_hot_loop(4)`, the 4-bucket heap-cycle DFS (edge cap 2000), and
`debug_dump_alloc_ring`/`debug_dump_alloc_trans`.

## Still open

- The `OGRE_SNAP` gate removes the periodic dump in the normal case, so a *hang*
  now produces no `[snap]` output unless the developer ran with `OGRE_SNAP=1`.
  A hang-triggered dump (watch the frame counter and dump once after it has not
  advanced for ~3 s) would keep the diagnosis at zero periodic cost. It was not
  written, because a long streamed-module load already looks like a stall for
  seconds (session 98) and would fire the dump at the worst moment.
- The Windows figure above is inferred. Confirming it needs one run on the
  reporter's machine: `OGRE_DL_TRACE=1` with `OGRE_SNAP` unset against
  `OGRE_SNAP=1`, comparing the gap series the same way.
- `debug_sample_hot_loop` remains a poor sampler on Windows (100 samples over
  ~100 ms) whenever it is enabled. If the snapshot is used there, the sampler
  should count ticks off a real clock instead of sleeping 200 us.
