# Handoff — 2026-09-25, session 98: the escort mission's slow field is the session-80 class a third time

**Goal (developer):** *"during gameplay, it becomes slow during an escort
mission (battery save attached)"*. The developer supplied the battery save and
drove a profiled run of the port.

**Result:** the mission field's slow seconds are `func_ovlN_801D0B78` (bank unit
N, record 7) carrying a recompiler-injected blocking `yield_self` at its
object-list skip branch `0x801D0C08`. `config/banks/config-bankN.toml` now lists
that branch in `yield_work_loop_branches`. Regeneration removes exactly one
`yield_self` (50 → 49 tree-wide). On the fixed build the function never appears
in a `[prof]` line for the frame-pump thread and the longest run of consecutive
≥ 50 ms frame gaps falls from 20 to 2.

No code, no generated file and no submodule was hand-edited. This is session 80's
one-site mechanism, applied a third time.

---

## 1. The save and the route

`scene_4_suspend.bin`, 32768 bytes, SHA-256
`7bb8916399e9b682ebf4258119c7ef9c47e9f89ab9a83cc2d8a912c179b31ec6`, is a raw
battery image: `QuestOG3` at `0x04` (the device
magic) and at `0x14` (slot 0's `+4` magic, the layout `docs/DECISIONS.md` (78)
records). It was copied to `assets/escort_suspend.bin` (gitignored) and imported
with `OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1`.

Driving: `OGRE_TAP_MS=1200
OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:4"`. The scene timeline is

```
0x09 intro -> 0x0A publishers -> 0x04 title -> 0x12 Load Game -> 0x03 mission
```

with `0x03` at descriptor `0x8018F350`, mask `0x0000038C`. That is the **suspend
route** of sessions 67/68 (the save resumes inside the mission), not the natural
map → briefing → mission route, which streams unit R.

The first three profiled runs did not reproduce the slowdown: 100 s, 152 s and
187 s of play in scene `0x03`, all at p50 8 ms / p99 34 ms display-list
intervals. The fourth run did, in the seconds before the developer closed the
window.

## 2. The measurement

Run 4 (`/tmp/ogre-escort4.log`), `OGRE_PROFILE=1 OGRE_DL_TRACE=1
OGRE_SCENE_LOG=1`, 61 s of play:

* Scene `0x03` runs from `t=53285 ms`. No scene change during the slow phase.
* Display-list gaps in `t=57304..59921 ms` are **80–183 ms**, against 8 ms
  before and after. The phase ends by itself at `t≈59921 ms`.
* `[prof]` puts the frame-pump thread `t4` in one function as it slows:

```
[prof] T=57000  t4:801D0B78(40%)
[prof] T=57999  t4:801D0B78(64%)
[prof] T=59000  t4:801D0B78(93%)
[prof] T=60000  t4:801D0B78(91%)
[prof] T=61000  t4:80184214(47%)   <- normal frame loop again
```

* The terminal snapshot repeats `[snap] hotloop t4: 0x801D0B78 x100`.

`0x801D0B78` is one of the 50 `yield_self` sites (the pre-fix tree), namely
`BankNFuncs/funcs_1.c:26745`, `func_ovlN_801D0B78`, branch `0x801D0C08`.

The profiler's address is a **function entry**, not a raw PC: `debug_profile_start`
and `debug_sample_hot_loop` both read `last_func_vram[tid]`, which
`recomp_trace_entry` writes (`tools/N64ModernRuntime/ultramodern/src/function_trace.cpp:717`,
`:591`).

## 3. The loop, at instruction level

`build/bankN.elf`, section `.bankRec7`:

```
801d0b78  addiu sp,sp,-48              ; prologue
801d0b98  lw    v0,160(s0)             ; s0 = the caller's struct
801d0b9c  lw    a0,60(v0)              ; *(s0+0xA0)+0x3C = a scratch buffer
801d0bb8  lw    s4,168(s0)             ; s4 = the list at s0+0xA8
801d0bc0  move  s3,zero                ; s3 = index
801d0bc4  sll   v0,s3,2                ; <- loop top
801d0bc8  addu  v0,v0,s4
801d0bcc  lw    v1,0(v0)               ; v1 = list[s3]
801d0bd0  li    v0,-1
801d0bd4  bne   v1,v0,801d0be4         ; list[s3] == -1 -> return 1
801d0be4  lw    v0,128(s0)             ; s0[0x80] = the caller's own id
801d0be8  beq   v1,v0,801d0d18         ; skip self
801d0bf0  lui   s2,0x801f
801d0bf4  addu  s2,s2,v0
801d0bf8  lw    s2,3280(s2)            ; s2 = *(0x801F0CD0 + id*4), the object
801d0bfc  lw    v0,0(s2)               ; object flags word
801d0c00  li    v1,49
801d0c04  andi  v0,v0,0x31
801d0c08  bnel  v0,v1,801d0bc4         ; *** the injected yield's branch ***
801d0c0c  addiu s3,s3,1                ; delay slot: next entry
801d0c10  ...                          ; distance test (jal 0x8009C780), then
801d0c80  jal   80070f30               ;   malloc(w*h), zero it, and
801d0d08  jal   801d08a0               ;   func_ovlN_801D08A0(buf, obj[0x14])
```

The loop is a scan of a `-1`-terminated object-id list. It advances `$s3` by one
entry per iteration and exits on the sentinel, and each iteration tests a
different object's flags word. It is a work loop, not a poll loop.

The generated C put the wait inside the taken side of the `bnel`:

```c
    // 0x801D0C08: bnel        $v0, $v1, L_801D0BC4
    if (ctx->r2 != ctx->r3) {
        yield_self(rdram);
        ctx->r19 = ADD32(ctx->r19, 0X1);
        goto L_801D0BC4;
    }
```

so it ran once per *skipped* entry. `yield_self` blocks until an external message
arrives, about one VI retrace, so a call costs (list length) × retrace. The list
length is the mission state, which is why the stall appeared at random, grew and
then cleared.

Raw ROM check (rule 8, check 5): `0x801D0C08` = `0x5443FFEE` and its delay slot
`0x801D0C0C` = `0x26730001` in `assets/ogre64.z64`, at ROM
`0x101D00 + (0x801D0C08 - 0x801AD5C0)`.

## 4. Which bank owns that address

`0x801D0B78` sits in RAM that several bank units share (`kN_bankRec7`,
`kJ_bankRec10a`, `kI_bankRec16`, `kAH_bankRec10s`, `kS_bankRec06`), so the
address alone does not name the function that ran. Three checks agree on bank
unit N:

1. `app/src/bank_funcs.inc` — the runtime's registration table — has exactly one
   entry for `0x801D0B78`: `{ 0x801D0B78u, func_ovlN_801D0B78 }`.
2. Run 4's bank-load timeline: the last load into that RAM before the stall is
   at `t=51984 ms`, `rom=0x101D00 ram=0x801AD5C0 size=0x43530 (300 functions)`,
   which is unit N's record 7. Record 10 (`rom=0x213AE0`, RAM `0x801D0860`) loads
   only at `t=27638` and `t=46258`, both before it; unit AH (`rom=0x23A370`) does
   not load in this run at all.
3. The blocking wait exists only in unit N's generated code: unit AH has no
   `yield_self` site anywhere, and no unit other than N has one at `0x801D0C08`.
   The stall is a blocking wait (93 % of a second in one function with 8 → 180 ms
   frame gaps), so it needs that `yield_self`.

## 5. The fix and the regeneration

`config/banks/config-bankN.toml`:

```toml
yield_work_loop_branches = [0x801B3008, 0x801DA2C0, 0x801D0C08]
```

with a comment recording the loop, the branch and the measurement. The option
already existed (session 80) and suppresses the emission at that one branch; the
heuristic is untouched everywhere else.

`make recomp && make bank-recomp`:

* Tree-wide `yield_self(rdram)` sites **50 → 49**; `BankNFuncs/funcs_1.c` **4 → 3**
  (the remaining three are `func_ovlN_801DA2D4` and `func_ovlN_801DE610`).
* The bank unit N change cannot affect the main unit: running `make recomp` with
  the **old** `config-bankN.toml` produces `RecompiledFuncs/` byte-identical to
  the new config's. `make recomp` is idempotent (a second run changes no file).
* `make recomp` did refresh four files the Sep-22 tree had stale
  (`RecompiledFuncs/funcs_14.c`, `-15`, `-16`, `-17`). That is the documented
  regeneration order catching up, and it is not this change: `recomp` reads
  `config/config.toml`, never `config/banks/config-bank*.toml`.
* `cross_bank.py check-banks`: OK, no bank unit calls into a swappable range it
  does not own.
* `make elf-rom-check`: **0 differing bytes** for all 13 bank ELFs and the main
  ELF, and every address-named symbol at its named address.

## 6. Verification

| check | result |
|---|---|
| `cmake --build build-app -j8` | clean; `BankNFuncs/*.o` rebuilt 14:44, binary 14:45 |
| bounded boot, `OGRE_EXIT_AFTER_MS=45000` + `tools/runlog.py --check` | **PASS**: no crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx task; 142 display lists |
| fixed-build mission replay, same save, 180 s, 44 scene transitions | `0x801D0B78` in **0 of 180** `[prof]` t4 lines (pre-fix run 4: 4 of 61) |
| frame gaps, fixed vs pre-fix | longest run of consecutive gaps ≥ 50 ms: **2 vs 20**. The two are the scene-`0x03`→`0x02`→`0x0D` handoff, where 6–7 modules stream in (`t=57943`/`58125`); pre-fix run 4 has the same handoff hitches *plus* the sustained phase |
| whole-run percentiles | fixed p50 8 / p90 13 / p99 34 ms, pre-fix p50 8 / p90 12 / p99 35 ms. The percentiles do **not** separate the builds; the tallies above do |
| `OGRE_COVER=/tmp/cover-escort-fixed.txt` | `[cover] 0x801D0B78 276` among 1498 functions: the scan still runs, so the mission is functionally unchanged and only the wait is gone |
| crashes / stubs | 0 `[crash]`, 0 `streamed function stub`, 0 `UNKNOWN module` |
| **developer confirmation** | **the escort mission plays fast on the fixed build, and the separate slowdown that used to happen when opening the list of units is gone too** |

**Developer-confirmed.** The escort mission is normal on the fixed build, and
**opening the list of units no longer slows the game** — a second symptom of the
same site. That second symptom also names a deterministic trigger for this class:
the loop walks the list at `obj+0xA8`, and opening the unit list makes that list
long, so the pre-fix build stalls there without waiting for the mission state to
drift on its own. The earlier "random during the mission" is the same loop with a
list whose length varies with play.

The A/B for the timing statistics rests on the address absenting itself from the
profiler and on the gap-run tally, because the fixed run's play diverged from
run 4's; the developer's report is the confirming measurement.

## 7. What was run

* Four developer-driven profiled runs of the suspect mission
  (`/tmp/ogre-lag-escort1.log`, `/tmp/ogre-lag-escort2.log`,
  `/tmp/ogre-escort3.log`, `/tmp/ogre-escort4.log`; run 4 reproduced).
* One fixed-build replay, `/tmp/ogre-escort-fixed.log` and
  `/tmp/cover-escort-fixed.txt`.
* `tools/proflog.py` on every log, `tools/guestmap.py 0x801D0B78`,
  `tools/symlook.py`, `mips-linux-gnu-objdump -d build/bankN.elf`, a raw-ROM
  word read, `make recomp`, `make bank-recomp`, `make elf-rom-check`,
  `cmake --build build-app -j8`, `tools/runlog.py --check`.
* **No probes.** Nothing carries a `probe98` tag; `grep -rl probe98
  RecompiledFuncs/ Bank*Funcs/ app/` is empty, and no submodule was touched.

## 8. Files changed

* `config/banks/config-bankN.toml` — `0x801D0C08` appended to
  `yield_work_loop_branches`, with the loop and the measurement in the comment.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, this
  file.

Generated and gitignored, refreshed by the two make targets: `BankNFuncs/**`,
`RecompiledFuncs/**`, `app/src/bank_funcs.inc`, `build-app/**`.
`git status --short` shows `config/banks/config-bankN.toml`, the docs above and
the pre-existing `m tools/RT64`.

`assets/escort_suspend.bin` is a copy of the developer's battery save and is
gitignored (`*.bin`); it is not part of the change.

## 9. Next leads

1. **`func_ovlN_80204C08` (branch `0x80204C48`)** is still the named-but-unlanded
   candidate from session 90 (45–74 % of `t4` at a screen onset, 44–51 % during
   ordinary mission play). Run 4's snapshot has one `x1` line for it. Do not list
   it without a slow second that names it.
2. **The list at `obj+0xA8`** is the quantity the stall scales with. If this
   class returns in the mission field, the first measurement that helps is the
   list length at the slow moment (`obj` is the caller's struct; the caller is
   `func_ovlN_801D1A..` at `0x801D1AA4`, which passes the same struct in `$a0`).
   **Opening the list of units is a deterministic way to make that list long**
   (developer, session 98), so it is the reproducer to use for any future site of
   this shape, in place of waiting for the mission state to drift.
3. **The remaining 49 `yield_self` sites** are enumerated in this session's notes;
   the two prior fixes are `0x801B3008` (session 80) and `0x801DA2C0`
   (session 90). The reusable shape is unchanged: read `t4`'s `[prof]` line,
   grep the named function's bank file for `yield_self`, list the branch.
