# Handoff — 2026-09-18, session 80: the mission's post-battle lag is fixed with a one-site, declarative recompiler override

> **Read §1 and §3.** Session 79 diagnosed the developer's *"after a battle …
> it lags badly"* as one recompiler artifact: N64Recomp's poll-loop heuristic
> injects a **blocking** `yield_self()` into the mission's nearest-target search
> `func_ovlN_801B2F4C` at its backward branch `0x801B3008`, once per candidate
> entity, and each `yield_self` blocks until an external message arrives (≈ one
> VI retrace, 16.7 ms). Session 79 left it unfixed because every *broad* repair
> it tried deadlocked the boot.
>
> This session's repair is **scoped and declarative**: a new N64Recomp config
> option, `yield_work_loop_branches`, lists `0x801B3008` and the recompiler
> emits **no** yield at a listed backward branch. Regenerating changes exactly
> **one** generated file (`BankNFuncs/funcs_3.c`, one line removed) and **52 →
> 51** yield sites; every other generated file is byte-identical, and the boot
> is unchanged. **The developer drove the tutorial-practice route to a battle on
> the fixed build (`OGRE_SPEED=8`) and confirms it runs perfectly** (§3, §8) —
> that route previously fell to 1.4 display lists/s after combat. §2 corrects
> session 79's description of *how* the failed attempts died (they never reached
> scene `0x09`).

## 1. Result

`config-bankN.toml` now carries:

```toml
yield_work_loop_branches = [0x801B3008]
```

A new option threaded through N64Recomp as
`Config::yield_work_loop_branches` → `Context::yield_work_loop_branches` →
the first line of `is_yield_poll_loop` (`tools/N64Recomp/src/recompilation.cpp`):

```cpp
auto is_yield_poll_loop = [&](uint32_t branch_target) -> bool {
    if (std::find(context.yield_work_loop_branches.begin(),
                  context.yield_work_loop_branches.end(), instr_vram) !=
        context.yield_work_loop_branches.end()) {
        return false;
    }
    // ... unchanged heuristic ...
};
```

`instr_vram` is the backward branch's own VRAM, so the option is exact: it
suppresses the yield at that one branch and nowhere else. **The heuristic itself
is untouched** — no register bookkeeping was changed, no site changed
classification because of a rule change, and the emitted C for the other 51
sites is byte-identical (proved in §3).

**Why not the general fix.** Session 79's three broad repairs (emit only for
loop bodies without an interior conditional branch; the same classifier with a
non-blocking `yield_self_nowait`; "correct" the heuristic's register
bookkeeping) all deadlocked the boot. The reason is structural: in this runtime
only one game thread executes at a time (the baton passes through
`resume_thread_and_wait` / `run_next_thread_and_wait`), and `yield_self` is also
the external-message drain point, so removing or unblocking the *wrong* yield
starves the boot's message chain. Until a discriminator is proven against the
boot, the safe correction is the one-site declaration — it is in bank unit N
(the mission module, streamed record 7) and cannot execute during boot.

**Why suppressing is faithful.** The loop is a pure scan: it terminates on a
list element loaded from an address the loop itself advances
(`lw $v0, 0($v0)`, `$v0 = $a0 + $v1`, `$a0 += 4`; §4). It makes no calls and no
stores, and it is bounded by the list's `-1` terminator, so a work-loop yield
was never needed for progress — the injected yield was pure cost. Retail has no
yield there at all.

## 2. Correction to session 79: the failed attempts died *before* the intro

Session 79 §5 says all three attempts "stall in the intro scene (scene `0x09`,
one display list)". **That is not what the logs show.** The four experiment logs
still in `/tmp` (`ogre-lag-{fix,fixboot,verify,restore}.log`) each contain
**three** `[scene]` lines and end at scene **`0x00`**, t≈1.0 s — scene `0x09`
is never entered. Re-reading them also identifies the mechanism more sharply:

| log | scenes | sends to boot queue `0x800C4C28` | max `extbacklog` |
|---|---|---|---|
| stalled attempt (`fixboot`) | 3 (ends `0x00` @1.029 s) | **1** | **4372** |
| stalled attempt (`fix`) | 3 (ends `0x00` @1.024 s) | **1** | 4103 |
| stalled attempt (`verify`) | 3 (ends `0x00` @1.131 s) | **1** | 4733 |
| stalled attempt (`restore`) | 3 (ends `0x00` @1.140 s) | **1** | 3472 |
| **baseline** (`final`) | 10 (reaches `0x03` @47.3 s) | **4** | **0** |

So the deadlock is a **message-starvation at the boot worker's queue, ~0.3 s
before the intro scene**: the retrace forwarder `t19` sends `msg=0x800E8B10` to
`0x800C4C28` once and then parks on `recv` of `0x800E8B84`, and the external
retrace backlog grows to thousands because no game thread drains it. The
scheduler snapshot at the stall (baseline has the *same* parked-thread state at
T=877 ms, but with `resumes: t3=209 t4=73 t19=62` versus the stalled
`t3=5 t4=2 t19=10`) shows the difference is that threads **stop being resumed**,
not that a particular thread is in the wrong function. That is consistent with
session 79's conclusion that `yield_self` is load-bearing as a drain point, and
it is why the repair was not allowed to touch any boot-path site.

## 3. Verification

Everything below was run this session.

* **Exactly one generated file changes.** SHA-256 manifests of every file under
  `RecompiledFuncs/` and all `Bank*Funcs/` were taken before and after
  `make recomp && make bank-recomp` (145 files). Exactly
  `BankNFuncs/funcs_3.c` differs; nothing is added or removed. `grep -c
  yield_self BankNFuncs/funcs_3.c` → **3** (was 4); the tree-wide count is
  **51** (was 52). The mission loop now reads:

  ```c
      // 0x801B3008: bne         $v0, $a3, L_801B2F9C
      if (ctx->r2 != ctx->r7) {
      // 0x801B300C: addu        $v0, $a0, $v1
          ctx->r2 = ADD32(ctx->r4, ctx->r3);
              goto L_801B2F9C;
      }
  ```

  `make recomp` alone (main unit, `config.toml` has no such key) leaves
  `RecompiledFuncs/` byte-identical, which is the independent check that the
  heuristic is unchanged for unlisted branches.

* **Boot unchanged.** `OGRE_TAP_MS=0 OGRE_EXIT_AFTER_MS=52000 tools/run-lag.sh
  --tag s80boot` (log `/tmp/ogre-lag-s80boot.log`) reaches the same scene
  timeline as session 79's baseline run: intro `0x09` @1.5 s → publishers
  `0x0A` @11.0 s → title `0x04` @27.3 s → tutorial `0x17` @42.6 s → new-game
  loader `0x02` @51.2 s → `0x0D` → mission `0x03` @51.7 s, 1464 display lists.
  No stall, no stub calls.

* **Both app builds** rebuilt from the regenerated tree (`build-app`,
  `build-null`; Release). `tools/N64Recomp/build/N64Recomp` and `RSPRecomp` were
  rebuilt from the patched sources; `n64recomp-ob64.patch` regenerated (§6).

* **Mission measurement (developer-driven).** The developer played the fixed
  build at `OGRE_SPEED=8` with `OGRE_PROFILE=1` and per-display-list timing
  (`tools/run-lag.sh --tag s80mission`, log `/tmp/ogre-lag-s80mission.log`,
  battery = the normal config dir), driving the tutorial-practice route to a
  battle — the route that produced session 79's onset at t≈110 s. Measured on
  that log:

  * `tools/proflog.py` reports **0 seconds at ≤12 display lists/s** over the 132
    profiled seconds / 13465 display-list lines (session 79's log: a sustained
    1.4 lists/s phase).
  * `0x801B2F4C` appears **nowhere** in the log or in any `[prof]` hot-function
    histogram (session 79: `t4:801B2F4C` at 93–96 % for the slow seconds).
  * Submitted display-list intervals after the opening (t ≥ 13 s, n = 12358):
    **p50 = 6 ms, p90 = 21 ms, p99 = 31 ms** (session 79's slow phase: ~717 ms).
    The run reaches 13949 display lists.
  * `frame:` lines show `D_800AEFA4` (frame counter) advancing with
    `D_800C4BCC` (VI retrace), i.e. frames are produced throughout.
  * The scene timeline crosses the mission `0x03` **eight times** (13.4, 29.4,
    44.8, 51.6, 67.1, 76.6, 106.5, 126.7 s) over the 129 s run (at
    `OGRE_SPEED=8`, i.e. ~17 minutes of emulated game time), with no slowdown,
    then returns to the tutorial `0x17` at 129.3 s.

  *One outlier, a different phenomenon and not the reported bug:* a single
  **1014 ms** display-list gap at t=62.5 s, inside scene `0x0D`. The stall
  snapshot taken during it shows the frame-pump thread `t4` cycling the
  **malloc/alloc family** (`800719E8`, `80071950`, `80071A3C`, `80070F30`
  interleaved with `800EAF1C`/`80184214`) — an allocator burst, not
  `0x801B2F4C`. It is one gap in an otherwise 6 ms-median run, and the only
  interval over 143 ms in the whole log. Recorded as next lead (B) in §7, not
  claimed as fixed. Every other >100 ms gap in the log is one of the sequence's
  own `0x02`/`0x0D` transitions (~102–143 ms, occurring at each of the six
  transitions).

  Developer verdict: see §8.

## 4. Instruction-level evidence for the classification

`build/bankN.elf`, `func_ovlN_801B2F4C`; the loop head is `0x801B2F9C`, the
backward branch `0x801B3008`:

```
801b2f9c  lw   a1, 0(v0)        ; list element (base $v0 varies)
801b2fa0  sll  v0, a1, 2
801b2fa4  addu v0, v0, t1       ; t1 = 0x801F0CD0, the object table
801b2fa8  lw   v1, 0(v0)        ; object pointer
801b2fac  lw   v0, 0(v1)        ; flags
801b2fb0  andi v0, v0, 0x11
801b2fb4  bne  v0, t0, L_801B2FF8
...
801b2fbc  lwc1 $f2, 8(s0)       ; <- the load the heuristic keys on ($s0 invariant)
...
801b2ff8  lw   v1, 168(s0)      ; re-read the list base
801b2ffc  addiu a0, a0, 4       ; advance the cursor
801b3000  addu v0, a0, v1
801b3004  lw   v0, 0(v0)        ; next element, from an address the loop advanced
801b3008  bne  v0, a3, L_801B2F9C
801b300c  addu v0, a0, v1       ; delay slot: next iteration's cursor base
```

The heuristic's criterion is "some load in the body has a loop-invariant base",
which `lwc1 $f2, 8($s0)` satisfies ($s0 is set once at entry, at `0x801B2F54`)
even though the loop's exit condition comes from `0x801B3004`'s load at a
*varying* address. That is the misclassification; the branch is a list walk, not
a wait on a fixed location. (A stricter "the branch value must come from an
invariant-address load" rule would reject this loop, but §1's note and §5's
scheduler evidence are why the heuristic was not changed this session.)

## 5. What was run, and the state of the tree

* `make recomp`, `make bank-recomp` (regenerated `RecompiledFuncs/`,
  `Bank*Funcs/`, `app/src/bank_funcs.inc`; `cross_bank.py check-banks` OK,
  `njpeg_readback` re-applied), then `cmake --build build-app -j8` and
  `cmake --build build-null -j8`.
* `tools/N64Recomp` rebuilt (`cmake --build build --target N64RecompCLI`;
  note the executable target is `N64RecompCLI`, output name `N64Recomp` — the
  `N64Recomp` target alone is the static library and does **not** relink the
  CLI).
* Two app runs: the boot assertion (`/tmp/ogre-lag-s80boot.log`) and the
  developer-driven mission run (`/tmp/ogre-lag-s80mission.log`).
* **No probes.** No generated file carries a tag, nothing was patched by hand,
  and no submodule other than `tools/N64Recomp` was touched (its diff is exactly
  the regenerated patch). `git status --short` shows only intended changes plus
  the pre-existing `tools/RT64` dirt.

## 6. Files changed

* `config-bankN.toml` — the new `yield_work_loop_branches = [0x801B3008]` key
  and its explanation.
* `n64recomp-ob64.patch` — regenerated; previous hunks byte-identical, new hunks
  in `src/config.h`, `src/config.cpp`, `src/main.cpp`,
  `include/recompiler/context.h`, `src/recompilation.cpp` (plus the
  `<algorithm>` include). Apply with
  `git -C tools/N64Recomp apply ../../n64recomp-ob64.patch`.
* `tools/N64Recomp/src/{config.h,config.cpp,main.cpp,recompilation.cpp}`,
  `tools/N64Recomp/include/recompiler/context.h` — the option plumbing
  (vendored, gitignored; carried by the patch).
* `docs/HANDOFF-2026-09-18-session80.md` (this file), `PLAN.md`,
  `docs/DECISIONS.md`, `docs/README.md`.

Generated but gitignored (regenerated by the two make targets):
`BankNFuncs/funcs_3.c` (the one changed file), `app/src/bank_funcs.inc`.

## 7. Next leads

1. **The general fix, with the boot as the assertion.** The mission site is
   fixed, but session 79's scan (`tools/yieldcheck.py`-style) stands: 51 sites
   remain, each a blocking wait a loop can execute per iteration. The
   discriminators worth prototyping are (a) "the branch's tested value is loaded
   from an address the loop itself advances" (rejects this site, §4) and (b)
   "the branch's tested value is loaded from an invariant address" (keeps the
   canonical `lw v0,0(a0); bne v0,zero,head` polls and the boot's scene-word /
   RSP-status / DP-status spins). Any such change must be tested against the
   boot; §2's data says the failure mode is an early message-starvation, so
   watch `extbacklog` and the `0x800C4C28` send count, not just the scene
   timeline.
2. **Find the real reason a non-blocking pump cannot stand in for the block.**
   The boot's drain chain through `yield_self` and `run_next_thread_and_wait`'s
   idle path is the open question (session 79 §5.2). `OGRE_SCHED_TRACE=1` on the
   baseline's first second, compared against a `yield_self_nowait` build, names
   the message that stops being delivered.
3. **Exercise the other 51 sites.** 20 are in bank unit N (the mission), where
   the same lag class may exist for longer lists. `tools/run-lag.sh` +
   `tools/proflog.py --names` is the cheap detector, and the new option is the
   cheap fix once a site is confirmed.
4. **The allocator burst (new, from §3's outlier).** A single 1014 ms
   display-list gap at t=62.5 s in scene `0x0D` has `t4` cycling the malloc
   family (`800719E8`/`80071950`/`80071A3C`/`80070F30`), not `0x801B2F4C`. The
   heuristic's own comment records that `func_80071A3C`'s free-list walk was
   *already* made not to yield (a `load_written[base]` rejection), so a long
   walk now runs to completion while holding the baton — worth checking whether
   that is the cost here, and whether the list got long because of a leak.
   `tools/proflog.py`, `[snap] alloctrace`/`alloctrans` and the live console are
   the tools.

## 8. Developer verdict

**Confirmed fixed.** The developer drove the tutorial-practice route to a battle
on the fixed build at `OGRE_SPEED=8` (`--tag s80mission`, log
`/tmp/ogre-lag-s80mission.log`) and reports **"it runs perfectly"** — no
post-battle slowdown, on the route that previously collapsed to 1.4 display
lists/s. That matches the log evidence in §3 (0 slow seconds, `0x801B2F4C` never
hot, p50 6 ms / p99 31 ms display-list intervals over 129 s and eight mission
crossings).

