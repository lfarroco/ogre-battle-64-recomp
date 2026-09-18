# Handoff — 2026-09-18, session 79: the mission's post-battle lag is a `yield_self` the recompiler injected once per entity — **cause found, repair NOT landed**

> **Read §1 and §6.** The developer's report — *"in the mission … after a battle /
> when an enemy unit is destroyed and the effect ends, the game lags badly;
> it draws and responds, just very slowly"* — is reproduced and measured. The
> cause is not game logic and not the renderer: N64Recomp's poll-loop heuristic
> classifies the mission's **nearest-target search** as a `while (flag);` spin and
> injects `yield_self()` **per loop iteration**, and `yield_self` blocks until an
> external message arrives (≈ one VI retrace, 16.7 ms). A scan over ~22 entities
> therefore costs ~0.4–0.7 s per frame: **30 fps → 1.4 fps**, which is exactly the
> reported lag.
>
> **No repair is landed.** Three candidate repairs were built and all three broke
> the boot (the intro scene stops producing frames; `t4`, the frame pump, sits
> blocked on its message queue forever). Everything was reverted; the tree and
> `tools/N64Recomp` are back at their recorded baseline (§6). The next session
> should start from §5, which is the evidence about what a repair must preserve.

## 1. Reproduction and measurement

The developer drove the port interactively with per-display-list timing on
(`tools/run-lag.sh` → `OGRE_DL_TRACE=1 OGRE_SCENE_LOG=1`; log
`/tmp/ogre-lag-live.log`, 2.4 MB) on the tutorial-practice route. The slowdown
starts **inside scene `0x03`**, at `t≈110.4 s`:

```
display list 3207 at t=110409ms ...     30 lists/s up to here
display list 3208 at t=111484ms ...     717 ms
display list 3209 at t=112614ms ...     1130 ms
```

The interval between submissions jumps from 33 ms to ~717 ms and stays there
(1.4 lists/s). What the same log rules out:

| observation | value | excludes |
|---|---|---|
| `processDisplayLists <us>` | 3.8–6.2 ms throughout, max 10.6 ms | the renderer — it is doing normal work |
| `[overlays] streamed function stub` | 0 | a missing bank / logging stub |
| `[bank] UNKNOWN module` | 0 | an unknown module with no eviction |
| `[bank] loading overlay` at the onset | none (last load 1.5 s earlier) | a bank re-stream |
| frame period | 717 ms with ~6 ms of work | a hidden wait in the render path |

The frame period is ~20× the normal one, and function-entry activity *falls*
during it: `entries/s` for the frame-pump thread `t4` goes 317 795 → 36 050 →
6 219 → 3 343. So the game is not doing 20× the work — it is **waiting**.

## 2. Where it waits

`OGRE_PROFILE=1` (per-second dominant function per thread; log
`/tmp/ogre-lag-prof.log`) names the function, and `tools/proflog.py --names`
resolves it:

```
[prof] T=116056 t4:80184214(53%)   entries/s t4:317795      <- healthy mission
[prof] T=117056 t4:801B2F4C(50%)   entries/s t4:36050       <- onset
[prof] T=118056 t4:801B2F4C(93%)
[prof] T=119056 t4:801B2F4C(96%)
[prof] T=120056 t4:801B2F4C(96%)
```

`t4` is the frame-pump thread (`func_8008AFE0`); `0x801B2F4C` is in bank unit N
(the mission module, record 7). The hot-function histogram for the slow seconds
shows ~13 000 calls/s each to `func_8009CFE0` and the other helpers — i.e. the
scan is running and each iteration is yielding:

```
hot: 800862C0 x16672 800868DC x16672 80071950 x14188 801C2C7C x13936
     80086838 x13649 80082B64 x13539 8009CFE0 x13147 801C9008 x12588
```

## 3. The function, at instruction level

`func_ovlN_801B2F4C` (bank unit N, `BankNFuncs/funcs_3.c`) is a nearest-target
search: `obj+0xA8` is a list of object indices terminated by `-1`; for each
candidate whose flags match `0x11` it computes the squared distance from
`obj+0x8`/`obj+0x10`, keeps the nearest, and then converts the direction to an
angle with `func_8009CFE0` (a loop-based `atan2`, fixed 12-iteration polynomial —
not the cost).

`build/bankN.elf` disassembly of the loop, with `tools/N64Recomp`'s own view:

```
801b2f9c  lw   a1,0(v0)          <- list element
801b2fa0  sll  v0,a1,2
801b2fa4  addu v0,v0,t1
801b2fa8  lw   v1,0(v0)          <- object from the table at 0x801F0CD0
801b2fac  lw   v0,0(v1)          <- its flags
801b2fb0  andi v0,v0,0x11
801b2fb4  bne  v0,t0,801b2ff8    <- interior branch: not a match, next
...
801b2fe8  bc1f 801b2ff8          <- interior branch: keep the best so far
801b2ff8  lw   v1,168(s0)        <- re-read the list pointer
801b2ffc  addiu a0,a0,4
801b3000  addu v0,a0,v1
801b3004  lw   v0,0(v0)
801b3008  bne  v0,a3,801b2f9c    <- the loop's backward branch
801b300c  addu v0,a0,v1          <- delay slot
```

and the generated C at the backward branch:

```c
    // 0x801B3008: bne         $v0, $a3, L_801B2F9C
    if (ctx->r2 != ctx->r7) {
        yield_self(rdram);                 // <-- the whole bug
        ctx->r2 = ADD32(ctx->r4, ctx->r3);
            goto L_801B2F9C;
    }
```

That line is `BankNFuncs/funcs_3.c:55419` in the build that boots. The loop
yields once per entity, so a 22-entity scan pays 22 external-message waits.

**Why the heuristic accepts this loop.** `is_yield_poll_loop`
(`tools/N64Recomp/src/recompilation.cpp`) asks whether some load in the body uses
a *loop-invariant* base register. Two loads qualify here, both benignly:
`lw a1,0(v0)` ($v0 is set before the loop and by the delay slot) and
`lwc1 $f2,8(s0)` ($s0 is set once at function entry and never written in the
loop). The body is otherwise ordinary distance arithmetic. The heuristic has no
notion of "this loop is doing work", only "some load's address looks fixed".

## 4. Cost model, confirmed

`yield_self` is `wait_for_external_message()` then `check_running_queue()`
(`tools/N64ModernRuntime/ultramodern/src/scheduling.cpp`), and
`wait_for_external_message` blocks on the external-message queue
(`mesgqueue.cpp`). External messages are produced at hardware rates, so the wait
is ~one VI retrace (16.7 ms). The observed 717 ms frame period is consistent with
a scan of tens of entities each waiting one retrace (and with the queued backlog
at the onset).

This is not the session-77 shape (a bounded poll that can never be satisfied) and
not session 72's render-bound median. It is a poll implemented as a *blocking
sleep* called from a loop that has real work to do.

## 5. What a repair must preserve — three failures, and what they rule out

All three attempts below produced a build whose **boot stalls in the intro scene**
(scene `0x09`, one display list, run to the exit timeout). The snapshot always
looks the same:

```
[snap] thread t4 BLOCKED on recv of queue 0x800C4C28    (t4 = the frame pump)
[snap] resumes: t1=6 t3=5 t4=2 t5=1 t16=3 t17=4 t18=1 t19=10 t50=0 t200=9
```

So the following are all true and matter for the next attempt:

| attempt | change | result |
|---|---|---|
| A | Emit `yield_self` **only** for loops with no interior conditional branch (the mission loop has two), i.e. no yield at all for 45 sites | **boot stalls.** 52 → 7 sites |
| B | Same classifier, but emit a new non-blocking `yield_self_nowait` (`wait_for_external_message_timed(0)` + `check_running_queue`) for the work loops | **boot stalls.** 7 blocking + 37 nowait |
| C | Keep the original emission, but fix the heuristic's register bookkeeping (`load_written[base]` rejection; cop1 loads write `ft`, not `rt`) | **boot stalls.** 52 → 44 sites |

Conclusions, in order of confidence:

1. **`yield_self` is load-bearing beyond being a yield.** It is the *only*
   routine that drains the runtime's external-message queue on behalf of a
   running thread (`deliver_pending_direct_sends` + `wait_dequeue` + `do_send`).
   With no yields anywhere, `t4` waits forever for a message nobody pumps. A
   repair must keep pumping.
2. **A non-blocking variant is not sufficient** (attempt B): delivering only
   *already queued* messages starves the boot. Either something in the boot's
   progress genuinely needs the block (e.g. `do_send`'s back-pressure requeue
   path), or the message that matters is produced only while the thread is
   parked. That has not been determined yet — it is the next experiment.
3. **The register bookkeeping is more tangled than it looks** (attempt C): even
   the two "obviously correct" fixes removed 8 sites and broke the boot, so at
   least one loop whose base *is* load-written is a real poll on the boot path.
   Do not treat `is_yield_poll_loop` as a pure function of the load set without
   testing the boot.
4. Therefore the discriminator has to be **behavioural or scoped**, not a
   stronger static guess:
   * **Best lead:** make the *runtime* yield adapt. `yield_self` could wait only
     when the caller has nothing else runnable (or wait with a very short
     timeout) and otherwise return; measure the boot and the mission. That keeps
     one emission site and one message pump.
   * **Alternative:** scope the change to the one function
     (`0x801B2F4C`) rather than to a heuristic — a one-site experiment that keeps
     every other yield byte-identical, so the boot cannot regress. It is a
     targeted fix, not a general one, but it is testable in one build.
   * The *general* fix (the heuristic needs a real notion of "work loop") is
     worth doing, but only with the boot as an assertion — this session's three
     attempts are the evidence that the intuition "a work loop does not need to
     wait" is incomplete.

## 6. What was run, and the state of the tree

* Reproduction: two interactive runs (unprofiled `/tmp/ogre-lag-live.log`,
  profiled `/tmp/ogre-lag-prof.log`), both reaching the mission and both showing
  the 30 → 1.4 fps collapse in scene `0x03` after combat.
* Offline: `mips-linux-gnu-objdump -d build/bankN.elf --start-address=0x801B2F4C
  --stop-address=0x801B3130`; the generated C at `BankNFuncs/funcs_3.c:55277`
  and `:55419`; `func_8009CFE0` in `build/ogrebattle64.elf`.
* **Reverted, deliberately.** `tools/N64Recomp`'s diff is byte-identical to
  `n64recomp-ob64.patch` again (`diff n64recomp-ob64.patch <(git -C
  tools/N64Recomp diff)` is empty), and the two runtime files touched
  (`ultramodern/src/scheduling.cpp`, `N64Recomp/include/recomp.h`) are restored.
  `Bank*Funcs/`, `RecompiledFuncs/` and both app builds were regenerated from the
  baseline; verify with `grep -c yield_self BankNFuncs/funcs_3.c` → 4 and
  `grep -n yield_self_nowait Bank*Funcs/*.c RecompiledFuncs/*.c` → empty.
* **No probes remain**: nothing in generated code or `app/` carries a tag; the
  three experimental builds were fully regenerated away.

New tools (kept — they are the cheap profile for this class of report):

* `tools/run-lag.sh` — boot with per-display-list timing, scene log, DMA trace
  and streamed-module reporting to a log file, while keeping the developer's
  battery unless `--save` is given.
* `tools/proflog.py` — read an `OGRE_PROFILE=1` log: per-second frame rate, the
  slow phase, each thread's dominant function, and the hot-function histogram,
  with `--names` resolving addresses through `tools/symlook.py`.
* `tools/symlook.py` — guest VRAM → function name from the linked ELFs.

Two provisional audits (`yieldcheck.py`, `loopsurvey.py`) were written while
trying to classify the yield sites from the generated C and then deleted: the
ELF disassembly is the better answer for that question, and neither tool was
what found the bug.

## 7. Files changed

* `tools/run-lag.sh`, `tools/proflog.py`, `tools/symlook.py` (**new**).
* `tools/yieldcheck.py`, `tools/loopsurvey.py` (**new**, provisional).
* `docs/HANDOFF-2026-09-18-session79.md` (this file), `PLAN.md`,
  `DECISIONS.md`, `docs/README.md`.

**No functional change is landed.** `git status --short` shows only these plus
the pre-existing `tools/RT64` dirt.

## 8. Next leads

1. **Try the scoped fix first** (one site, `0x801B2F4C`): emit a non-blocking
   yield there only, keep all 51 other sites exactly as they are, boot, then
   drive the mission to a battle and read the frame rate with
   `tools/run-lag.sh`. If the boot survives and the mission holds 30 lists/s,
   the diagnosis is confirmed end-to-end and the general fix can be designed
   against a working reference.
2. **Find out why a non-blocking pump starves the boot** (attempt B): run the
   boot with `OGRE_SCHED_TRACE=1` and compare the scheduler ring against the
   baseline for the first second — which message stops being delivered, and to
   which queue. That answers whether a repair can be non-blocking at all.
3. **The developer's other observation is worth its own run**: *"in both times
   the lag happened after combat … after the effect where the enemy unit is
   destroyed ends."* The scan runs when units carry a target list; the specific
   state that makes it expensive (list length, or the flag that starts the
   search) is not yet pinned to an address. `func_ovlN_801B2F4C`'s caller and
   `obj+0xA8`'s writer are the places to look.
4. **Same class elsewhere.** `yield_self` appears 52 times in the generated
   tree (before any change); each is a blocking wait that a loop can execute per
   iteration. `tools/yieldcheck.py --func <name>` prints a site's loop for
   inspection. The ones inside bank unit N (20 of the 52) are the mission's, and
   the lag was found in one of them; the others have not been exercised.
