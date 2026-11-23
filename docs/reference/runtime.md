---
title: Runtime, threads and diagnostics
summary: The frame pump, the task queue, the RSP task types and the checkpoints and console a session uses to read live state.
status: verified
session: 202
order: 80
---

# Runtime, threads and diagnostics

The runtime runs the recompiled game image on the host threads described below.
It also supplies the diagnostics a session uses to read a live machine: the
console, checkpoints and a run log. This page covers the threads that run the
game, the frame pump and task queue that drive a frame, the RSP task types the
runtime serves, and the diagnostics that read state from a running game.

## Threads

The game's N64 threads are 1:1 native threads
([session 58](../HANDOFF-2026-09-16-session58.md) §1). A guest thread runs on its
own host thread, and the scheduler blocks and resumes that host thread.

The runtime boots the image once for the process. `recomp::start` returns when
`ultramodern::quit()` is called. `quit()` stops the runtime's own threads. The
game's threads are host threads running recompiled code, and `recomp::start` does
not join them ([app-build.md](../guides/app-build.md) → "Closing the window").
RESTART GAME ends the run and starts a new copy of the executable with the same
arguments, so the ROM boots from the beginning
([app-build.md](../guides/app-build.md) → "The in-game overlay"). The old process
leaves with `_Exit`, because `execv` would replace the image while the unjoined
game threads are alive.

The console's main thread is separate from the game threads. A read of 8 MiB of
RDRAM from that thread raced a game thread that was mid-frame (session 58). The
checkpoint path parks the game threads for that reason, and the browser's debug
thread dump reads the call chains of threads that have stopped
([session 202](../HANDOFF-2026-10-26-session202.md) → "Result").

## The frame pump

`func_8008AFE0` is the frame-pump entry on guest thread t4
([session 60](../HANDOFF-2026-09-16-session60.md) §1). The pump receives messages
from the VI thread, dispatches on the message type, and calls the scene frame
update `func_80072398` for a type-1 retrace. `func_80072398` builds the frame and
calls the running descriptor's update and hook, then processes the task queue.

The pump reads its saved registers from its own stack frame. A call that abandons
the caller's frame returns with a lower `$sp`, so the pump loads its saved `$s0`
and `$s1` from the wrong stack slots. `$s1` holds the message-type comparator
`1`. Once that value reads 0, the type-1 test never matches again, and the pump
stops after two frames (session 60).

The signature is `D_800AEFA4` frozen while `D_800C4BCC` keeps counting, with only
two display lists submitted, both during boot ([memory.md](memory.md) → "When the
game stops drawing"). `D_800AEFA4` counts pump iterations and `D_800C4BCC` counts VI
retraces, so a frozen pump leaves the retrace count rising.

Session 60 found the class in two calls that the recompiler emitted as "call the
containing body, then return". The early return abandoned the caller's frame.
Both call sites are now dispatched through `cross_bank.py dispatch --only`, which
compiles them as `LOOKUP_FUNC` and repairs the tail call (session 60 §3). The
guest `$sp` before and after each call is the check for another leak.

## The task queue

`func_80076F5C` is `task_queue_push`. It finds a free slot in `g_task_queue[6]`
at `0x800E82C8` and stores the task's `{id, callback, args}` there. Each slot is
`0xA8` bytes, and the callback is at `+0x10` ([symbols.md](../symbols.md) →
"Globals that pay off first").

`func_800765D8` is `task_queue_process`. It loops the six slots, copies a slot to
`g_current_task` at `0x800E7A30`, and calls the callback with `a0` = slot
([symbols.md](../symbols.md) → "Functions worth naming"). `func_80072398` calls
it after the descriptor's update ([symbols.md](../symbols.md) → "Functions worth
naming").

## RSP task types

The game submits three RSP task types. Type 1 is `M_GFXTASK`, type 2 is
`M_AUDTASK`, and type 4 is `M_NJPEGTASK`
([rsp-microcode.md](../guides/rsp-microcode.md),
[gbi.md](gbi.md) → "Microcodes").

The runtime routes type 1 to RT64's `send_dl`, and RT64 parses the display list
with its built-in GBI interpreters. The game's gfx ucode is F3DEX2 for the map
and most scenes, and S2DEX2 for the njpeg backdrop lists
([gbi.md](gbi.md) → "Microcodes").

`app/src/rsp.cpp` runs the recompiled njpeg microcode for type 4 and the
recompiled audio microcode for type 2. A task that is neither a gfx task nor one
of those two microcodes is served by the stub, so an un-recompiled task produces
no output ([rsp-microcode.md](../guides/rsp-microcode.md),
[AGENTS.md](../../AGENTS.md) rule 8). `tools/runlog.py` lists the tasks by type
and flags a non-gfx task that the stub swallowed. `--check` fails on one
([app-build.md](../guides/app-build.md) → "`tools/runlog.py`").

## Recompiling a microcode

RSPRecomp's `text_address` is a label base, not an address. It must equal the
RSP IMEM DMA address that the microcode was assembled for, masked `0x1FFF`. For
the boot loader's IMEM `0x080` that value is `0x1080`, and the bytes come from
`text_offset`. A wrong value rotates every `j` target by the difference, and the
recompiled microcode silently walks the wrong blocks: one `0x300`-byte block per
task instead of `mbs` (session 47), or an audio task that writes a zero buffer
(session 75). See [rsp-microcode.md](../guides/rsp-microcode.md) and
[session 75](../HANDOFF-2026-09-18-session75.md).

## Checkpoints

`save` and `load` in the live console write and restore a checkpoint. A
checkpoint holds the whole 8 MiB RDRAM image and the runtime's overlay state:
which recompiled body is mapped at each RAM address. RDRAM alone would restore
the data and leave the function map on a later bank, so the restored scene would
run the wrong module's bodies
([app-build.md](../guides/app-build.md) → "Checkpoints").

The file is a header, the overlay-state blob and the RDRAM image. `load` checks
the magic, version, sizes and checksum and refuses a file from another build
([app-build.md](../guides/app-build.md) → "Checkpoints").

`save` and `load` wrap the file I/O in
`ultramodern::checkpoint_pause_begin()` and `checkpoint_pause_end()`. Those calls
park every thread executing recompiled code at a function-entry boundary, then
release it. Without the park the image tears
([session 58](../HANDOFF-2026-09-16-session58.md) §1,
[AGENTS.md](../../AGENTS.md) rule 4).

A checkpoint is valid only in the process that wrote it, and only for the same
binary. The image holds host pointers, and the file carries an FNV-1a hash of the
running executable, so a rebuild invalidates it
([app-build.md](../guides/app-build.md) → "Checkpoints"). Save inside a stable
scene step, and check that `c` reports that step before `save`. A checkpoint
taken at a transition restores into the "no step" branch instead of continuing
the sequence.

## The live console

The console is off unless a run asks for it. A command can write guest RAM (`w`)
or restore a checkpoint (`load`), so a stock run polls no path and reads no
keyboard ([app-build.md](../guides/app-build.md) → "The live console").
`OGRE_LIVE_CONSOLE=1` turns both triggers on. Naming `OGRE_CONSOLE_FILE` or
`OGRE_CONSOLE_AT_MS` turns on the watched file, and naming `OGRE_KEY_<n>` turns
on that key.

The watched file defaults to `/tmp/ogre-console.txt`. When the file exists, its
lines run and the file is removed, so an external tool can drive a live run.
`OGRE_CONSOLE_AT_MS` delays every watched-file read until that many milliseconds
of wall clock have elapsed. The number keys `1`..`9` run
`OGRE_KEY_1`..`OGRE_KEY_9`, one command per press
([app-build.md](../guides/app-build.md) → "The live console").

The console runs on the main thread. RDRAM is available there, and a
multi-megabyte write cannot race the game thread
([app-build.md](../guides/app-build.md) → "The live console"). The commands are
`r`/`rh`/`rb`/`rk`, `d`, `f`, `fb`, `s`, `k`, `w`, `c`, `dump` and `save`/`load`.
`c` reports the scene, pending scene, descriptor and its record mask, step, next
and spin. `dump` writes the 8 MiB RDRAM image at that instant. The full command
list is in [app-build.md](../guides/app-build.md) → "The live console".

## Audio and timing

`OGRE_NO_AUDIO=1` skips opening an audio output device. The audio microcode still
runs, so a result that depends on the audio task is unaffected
([AGENTS.md](../../AGENTS.md) rule 14,
[app-build.md](../guides/app-build.md) → "Scripted runs and diagnostics"). Every
game run sets it.

`OGRE_TAP_MS`, `OGRE_EXIT_AFTER_MS` and `OGRE_SPEED` are wall-clock
([AGENTS.md](../../AGENTS.md) rule 9). `OGRE_TAP_MS` presses Start for 150 ms out
of every `n` ms. `OGRE_EXIT_AFTER_MS` prints the last recompiled function and the
live call chain of every game thread, then exits 0
([app-build.md](../guides/app-build.md) → "Scripted runs and diagnostics").
`OGRE_SPEED=n` scales the emulated clock, so a fixed wall-clock tap point moves
relative to the game. `OGRE_TRACE_HOOKS=1` slows boot several-fold, so a tap
schedule tuned at 1x misses the title ([AGENTS.md](../../AGENTS.md) rule 9).

## Addresses

<!-- BEGIN GENERATED: addresses:addresses -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x8008AFE0` | `frame_pump` | t4 | function | func_8008AFE0 is the t4 frame-pump entry; a caller that abandons its frame makes the pump read $s0/$s1 from the wrong slots and it stops. | 60 | high |
<!-- END GENERATED: addresses:addresses -->

## Evidence

- [session 60](../HANDOFF-2026-09-16-session60.md) — the t4 frame pump, the
  message-type dispatch, the stack-slot failure, the frozen `D_800AEFA4`
  signature and the two display lists.
- [session 58](../HANDOFF-2026-09-16-session58.md) — the 1:1 N64 threads, the
  torn checkpoint image and the overlay state.
- [session 202](../HANDOFF-2026-10-26-session202.md) — the game threads stopping
  while the main thread keeps presenting, and the browser thread dump.
- [app-build.md](../guides/app-build.md) — "Closing the window", "The in-game
  overlay", "Checkpoints", "The live console", "`tools/runlog.py`" and the
  environment-variable table.
- [rsp-microcode.md](../guides/rsp-microcode.md) — gfx tasks go to RT64, the
  njpeg and audio microcodes are recompiled, and every other task gets the stub;
  `text_address` is the IMEM label base.
- [gbi.md](gbi.md) — the F3DEX2 and S2DEX2 ucodes and their task types.
- [symbols.md](../symbols.md) — the task queue, its push and process functions,
  `g_current_task` and the frame update.
- [AGENTS.md](../../AGENTS.md) — rules 4, 8, 9 and 14, and the verified facts for
  the frame pump and the RSP task types.
