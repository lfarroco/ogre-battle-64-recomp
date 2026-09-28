# Session 112 — the unit list's slow screen is the session-80 class a fourth time

Date: 2026-09-27. Follows session 98, which fixed the same class in the mission
field and named the unit list as the deterministic trigger.

## Goal

Developer report: *"when we have multiple units in a single location, and we
press `A` to select one of them, a list of units opens. when that list is long,
the game slows down a lot"*, with a battery save (`scene_5_suspend.bin`, 32768
bytes, SHA-256 `858d7810c6b351ea000cb0e8698870e7c20c577e2c1f14e6c6716d016d81edc0`)
that resumes into the situation. The save loads through the default `Load Game`
entry and resumes in scene `0x03` (the mission).

## Result

The mission's unit list runs `func_ovlN_80208E84` (bank unit N, record 9). Its
inner loop at `0x8020A814..0x8020A910` walks five 1-byte records and keeps the
maximum of one byte per record. Its bound is `a3 = a1 + 5`, set at `0x8020A80C`;
it is a work loop. N64Recomp's poll-loop heuristic keyed on the loop-invariant
load `lbu $v0, 4($s6)` at `0x8020A820` and injected a blocking `yield_self()` at
the loop's back edge `0x8020A910`, so every taken back edge paid about one VI
retrace. `config/banks/config-bankN.toml` now lists that branch in
`yield_work_loop_branches`. Regeneration removes exactly one site tree-wide
(49 → 48).

Fixed build, same save, same list-open trigger: the mission runs at ~98 display
lists/s with a median 8 ms gap, against 12–13/s and a median 79 ms gap before.
The developer confirmed the fix.

No code, no generated file and no submodule was hand-edited.

---

## 1. The measurement, and why the sampling profiler could not make it

A profiled run (`OGRE_PROFILE=1 OGRE_SCENE_LOG=1 OGRE_DL_TRACE=1`) reproduced the
slow phase twice, at the two moments the developer opened the list. Display-list
periods went from 8 ms to 74–113 ms and held there while the list was open, and
`[prof]` put the frame-pump thread `t4` in `0x80207230` at 79–99 %.

`0x80207230` is not the slow function. `recomp_trace_entry` writes
`last_func_vram[tid]` on every entry
(`tools/N64ModernRuntime/ultramodern/src/function_trace.cpp:262`) and
`recomp_trace_return` does not restore it, so the profiler reports the most
recently entered function and not where the thread is spending time.
`func_ovlN_80207230` is 108 instructions with one loop bounded by `li $a2, 50`
and it calls nothing, so it cannot hold the thread for 79 ms.

The host stack sample settled it. `sample <pid> 4` on the running process, taken
while the list was open, put 1608 of 1670 samples of `N64 Thread 4` in
`yield_self`:

```
func_8008AFE0 (frame pump)
  func_80072398
    func_80180BFC
      func_ovlN_80208E84            1670 samples
        yield_self                  1608 samples
          ultramodern::wait_for_external_message
            semaphore_wait_trap
```

The remaining samples are the same function's own work.

## 2. The loop, at instruction level

`build/bankN.elf`, section `.bankRec9`:

```
8020a80c  addiu a3,a1,5          ; the bound is the base pointer + 5
8020a814  lbu   v1,0(a1)         ; <- loop head: one byte per record
          ...                    ; flag tests, table lookups, keep a maximum in s4
8020a820  lbu   v0,4(s6)         ; the invariant base the heuristic keys on
8020a844  bnezl v1,0x8020a90c    ; each skip path advances a1 in the delay slot
          ...
8020a908  addiu a1,a1,1
8020a90c  slt   v0,a1,a3         ; the exit test
8020a910  bnez  v0,0x8020a814    ; <- the injected yield's branch
8020a914  nop                    ; delay slot
```

`$a1` starts at `0x80197212 + s0*17` and advances one byte per iteration, so the
loop reads five entries and stops. The generated C put the wait inside the taken
side:

```c
    // 0x8020A910: bne         $v0, $zero, L_8020A814
    if (ctx->r2 != 0) {
        yield_self(rdram);
            goto L_8020A814;
    }
```

`yield_self` blocks until an external message arrives, about one VI retrace
(16.7 ms), so each scan paid four waits and the observed 79 ms median gap is
about five retraces.

Raw ROM check (rule 8, check 5): `0x8020A910` = `0x1440FFC0` and its delay slot
`0x8020A914` = `0x00000000` in `assets/ogre64.z64`, at ROM `0x15BA80`
(record 9, ROM `0x14EC00` → RAM `0x801FDA90`).

## 3. Why the heuristic fires, and why this is not a poll loop

`is_yield_poll_loop` (`tools/N64Recomp/src/recompilation.cpp:517`) returns true
when any load in the loop body uses a base register that the body never writes
non-constantly. The body reads `lbu $v0, 4($s6)` at `0x8020A820`; `$s6` is the
caller's struct, set before the loop, and the loop never writes it. The loop's
exit is `slt $v0, $a1, $a3` against a pointer the loop itself advances, so the
invariant load is not the poll condition. Listing `0x8020A910` in
`yield_work_loop_branches` suppresses the emission at that one branch and leaves
the heuristic untouched everywhere else.

## 4. Which bank owns that address

`0x80208E84` and `0x8020A910` both sit in record 9's window (`0x801FDA90`,
size `0x173E0`), which unit N owns (`app/src/bank_funcs.inc`, ROM `0x14EC00`).
Other banks share parts of that RAM, so the address alone does not name the
function: `tools/guestmap.py 0x80207230` lists unit C's `bankRec11` as another
mapping, and the run's own bank loads select unit N. Scene `0x03`'s record mask
`0x0000038C` is records 2, 3, 7, 8 and 9; `bank_funcs.inc` registers
`0x80208E84` only for unit N.

## 5. The fix and the regeneration

`config/banks/config-bankN.toml`:

```toml
yield_work_loop_branches = [0x801B3008, 0x801DA2C0, 0x801D0C08, 0x8020A910]
```

with a comment recording the loop, the branch and the measurement.

`make recomp && make bank-recomp`:

* Tree-wide `yield_self(rdram)` sites **49 → 48**; `BankNFuncs/funcs_3.c` **3 → 2**.
* `make recomp` runs `$(N64RECOMP) config/config.toml` only, and `bank-recomp`
  loops over `config/banks/config-bank<U>.toml` (Makefile lines 81 and 352), so
  the bank config change cannot alter `RecompiledFuncs/`.
* `cross_bank.py check-banks`: `check OK - no bank unit calls into a swappable
  range it does not own`.
* `make elf-rom-check`: **0 differing bytes** for all 34 bank ELFs and the main
  ELF, and every address-named symbol at its named address (4221 in the main
  ELF).

## 6. Verification

| check | result |
|---|---|
| same save, same list-open trigger, pre-fix (`/tmp/s112-noprof.log`) | 12–13 display lists/s from the first second of scene `0x03`, median gap 79 ms |
| same save, same trigger, fixed (`/tmp/s112-fixed.log`) | **1032 display lists in 10.5 s of scene `0x03`** (~98/s), median gap **8 ms**, max 40 ms |
| host stack sample, pre-fix | 1608/1670 `t4` samples in `yield_self` under `func_ovlN_80208E84` |
| fixed log | 0 `[crash]`, 0 `streamed function stub`, 0 `UNKNOWN module` |
| `OGRE_COVER=/tmp/s112-cover-fixed.txt` | `[cover] 0x80208E84 273`, `0x80207230 1372`, `0x801D0B78 19`: the work still runs, only the wait is gone |
| `build-null` boot, 45 s + `tools/runlog.py --check` | **PASS**: no crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx task |
| `make elf-rom-check` | 0 differing bytes, all symbols at their named addresses |
| `cross_bank.py check-banks` | OK |
| **developer confirmation** | **the slowdown is fixed** |

The pre-fix and fixed runs differ only in the built binary; both drive the save
through `title → 0x12 Load Game → 0x03` and open the list once. The A/B rests on
the display-list series and the host sample, not on the profiler's address.

## 7. What was run

* `tools/sramsave.py check` on the developer's save; `assets/scene_5_suspend.bin`
  is a gitignored copy of it.
* Four pre-fix display-list runs (`/tmp/s112-route.log`, `/tmp/s112-repro1.log`,
  `/tmp/s112-noprof.log`, `/tmp/s112-samp.log`), one developer-driven profiled
  run (`/tmp/s112-dev.log`), one `OGRE_PROFILE_CHAINS` run
  (`/tmp/s112-chains.log`), one host sample (`/tmp/s112-sample.txt`), and the
  fixed-build run `/tmp/s112-fixed.log`.
* `tools/proflog.py`, `tools/symlook.py`, `tools/guestmap.py`,
  `mips-linux-gnu-objdump -d build/bankN.elf`, a raw-ROM word read.
* `make recomp`, `make bank-recomp`, `make elf-rom-check`,
  `cmake --build build-app`, `cmake --build build-null`, `tools/runlog.py --check`.
* **No probes.** Nothing carries a `probe112` tag, no generated file was edited,
  and no submodule was touched.

## 8. Files changed

* `config/banks/config-bankN.toml` — `0x8020A910` appended to
  `yield_work_loop_branches`, with the loop, the invariant load and the
  measurement in the comment.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, this
  file.

Generated and gitignored, refreshed by the two make targets: `BankNFuncs/**`,
`RecompiledFuncs/**`, `app/src/bank_funcs.inc`, `build-app/**`, `build-null/**`.
`git status --short` shows `config/banks/config-bankN.toml`, the docs above and
the pre-existing `m tools/RT64`.

`assets/scene_5_suspend.bin` is a copy of the developer's battery save and is
gitignored (`*.bin`); it is not part of the change.

## 9. Next leads

1. **Use a host stack sample for a slow screen.** The `OGRE_PROFILE` sampler
   reports the most recently entered function, so it names an entry-heavy
   function rather than the one holding the thread. `sample <pid> 4 -file …`
   during the slow state gives the real stack, and it named
   `func_ovlN_80208E84` in one run after the profiler had named two wrong
   addresses.
2. **The remaining 48 `yield_self` sites** are enumerated by
   `grep -n yield_self Bank*Funcs/ RecompiledFuncs/`. The three earlier fixes are
   `0x801B3008` (session 80), `0x801DA2C0` (session 90) and `0x801D0C08`
   (session 98); this one is `0x8020A910`. The shape is unchanged: read the top
   of the slow thread's stack, grep its bank file for `yield_self`, confirm the
   loop is bounded by a work counter and not a status word.
3. **`func_ovlN_80204C08` (branch `0x80204C48`)** is still the named-but-unlanded
   candidate from session 90. Do not list it without a slow second that names it.
