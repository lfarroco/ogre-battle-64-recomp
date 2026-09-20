# Handoff — 2026-09-20, session 90: the "Use Item" unit-command screen runs at full speed

**Goal (developer):** the game is *"extremely slow"* in the **Use Item** section
under **Unit Commands**. It reproduces in the tutorial, and the developer can
drive it at `OGRE_SPEED=4`.

**Result:** the screen's per-frame lookup `func_ovlN_801DA248` (bank unit N, the
mission module) was carrying an injected blocking `yield_self` at its outer
loop's backward branch `0x801DA2C0`. The frame-pump thread `t4` was parked there
99 % of every slow second, the display-list period was 237–275 ms, and
`config-bankN.toml` now lists `0x801DA2C0` in `yield_work_loop_branches` (the
session-80 mechanism). Regenerating changes **one** generated file, the boot is
unchanged, the screen's `p99` display-list interval is 34 ms, and the developer
confirms it is **normal**.

This is session 80's failure class a second time: N64Recomp's poll-loop
heuristic misclassified a work loop and the blocking yield became the frame
period.

---

## 1. Reproduction and measurement

The developer drove a profiled run of the **pre-fix** build:

```sh
OGRE_PREF_DIR=.ogre-prefs-item OGRE_SPEED=4 OGRE_PROFILE=1 OGRE_DL_TRACE=1 \
  OGRE_SCENE_LOG=1 OGRE_COVER=/tmp/cover-item-dev.txt \
  ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/ogre-item-dev.log 2>&1
```

Scene timeline: intro `0x09` → publishers `0x0A` → title `0x04` → tutorial
`0x17` → new-game `0x02`/`0x0D` → practice `0x03` (`0x8018F364`) at `t=30.4 s`
→ tutorial `0x17`/`0x02`/`0x0D` → mission `0x03` (`0x8018F350`) at
`t=125.9 s`. Nothing is logged at the screen boundary, so the onset is read from
the display-list intervals (below).

The slow phase starts at **`t=137954 ms`** and lasts to the window close
(~155 s). Display-list gaps in that phase are **237–275 ms**; before it they are
about 6 ms.

The profiler names the cost directly. From `T=139209` to the end of the run
every `[prof] T=` line reads:

```
[prof] T=139209 t1:80089BE4(100%) t3:80080EE8(99%) t4:801DA248(99%) t5:8007284C(98%) ...
[prof]   entries/s: t1:0 t3:40196 t4:17578 t5:1158 ...
```

`t4` is the frame-pump thread, and `0x801DA248` first appears in a `[prof] T=`
line at `T=138209` — the first full second of the slow phase. It never appears
before it. The scheduler's own hot-loop detector agrees: the run's terminal
`[snap]` queue snapshot at `T=139304` carries `hotloop t4: 0x801DA248 x100`.

## 2. Instruction-level evidence

`build/bankN.elf`, `func_ovlN_801DA248` (record 7, `.bankRec7`):

```
801da248  lbu   a3, 4(a0)          ; the key: one byte of the caller's struct
801da24c  move  a2, zero           ; outer index
801da250  lui   a1, 0x8019
801da254  addiu a1, a1, 0x69D8     ; 10 records, 0xB bytes apart
801da258  lbu   v1, 1(a1)          ; record flags
801da25c  andi  v0, v1, 1
801da260  beqzl v0, 801da2bc       ; bit 0 clear -> skip the record
801da264  addiu a2, a2, 1
801da268  lw    v0, 0(a0)          ; <- invariant base ($a0 is the argument)
801da26c  andi  v0, v0, 8          ;    tests flag bit 3, not the exit condition
...
801da294  addu  v0, a1, v1
801da298  lbu   v0, 2(v0)          ; inner scan over 5 entries of the record
801da29c  bnel  v0, a3, 801da2ac
801da2a0  addiu v1, v1, 1
801da2ac  slti  v0, v1, 5
801da2b0  bnez  v0, 801da298       ; inner backward branch (no yield emitted)
801da2b4  addu  v0, a1, v1
801da2b8  addiu a2, a2, 1
801da2bc  slti  v0, a2, 10
801da2c0  bnez  v0, 801da258       ; outer backward branch -- yield_self emitted here
801da2c4  addiu a1, a1, 11         ; delay slot: advance to the next record
801da2c8  li    v0, -1
801da2cc  jr    ra
```

The body contains **no `jal` and no store**, and the loop's termination is the
immediate bound `slti $a2, 0xA`. It is a pure 10×5 table scan.

`is_yield_poll_loop` (`tools/N64Recomp/src/recompilation.cpp:516`) classifies a
backward branch as a poll when some load in the body has a base register that is
never written non-constantly. Here that load is `lw $v0, 0($a0)` at `0x801DA268`:
`$a0` is the function's only argument and is never written. The load tests a flag
bit once per record; it is not what the branch tests. The branch tests
`$v0 = slti($a2, 10)`, an immediate bound. Session 80 §7 lists exactly this
discriminator as the general fix that was not attempted: *the branch's tested
value must come transitively from the invariant-address load*.

The generated C before the fix (`BankNFuncs/funcs_2.c`) put the injection inside
the taken branch, so it ran on every iteration except the last — **up to nine
blocking waits per call**. `yield_self` waits for one external message
(`tools/N64ModernRuntime/ultramodern/src/scheduling.cpp:80`), which at
`OGRE_SPEED=4` is ~4.2 ms. The measured 258 ms frame is ~61 messages, i.e. about
seven calls to this function per frame — the item list.

## 3. The fix

`config-bankN.toml`:

```toml
yield_work_loop_branches = [0x801B3008, 0x801DA2C0]
```

The option already existed (session 80): the branch's own VRAM at the top of
`is_yield_poll_loop` returns `false`, so the emission is suppressed at that one
site and the heuristic is untouched everywhere else. No recompiler source and no
patch changed this session.

`make recomp && make bank-recomp` regenerated the tree. SHA-256 manifests of the
151 generated files (before and after) differ in **exactly one file**,
`BankNFuncs/funcs_2.c`; the tree-wide `yield_self` count is **51 → 50**, and bank
unit N's is 19 → 18. The generated loop now reads:

```c
    // 0x801DA2C0: bne         $v0, $zero, L_801DA258
    if (ctx->r2 != 0) {
        // 0x801DA2C4: addiu       $a1, $a1, 0xB
        ctx->r5 = ADD32(ctx->r5, 0XB);
            goto L_801DA258;
    }
```

`cross_bank.py check-banks` reported `check OK`. `build-app` was rebuilt.

## 4. Verification

* **Boot unchanged.** `OGRE_EXIT_AFTER_MS=53000 OGRE_SCENE_LOG=1` on the fixed
  build (`/tmp/ogre-boot-fix.log`) reaches the same timeline as the pre-fix
  build: `0x09` → `0x0A` → `0x04` → `0x17` → `0x02` → `0x0D` → `0x03`.
  `tools/runlog.py --check` gives **PASS** (no crash, no stub call, no unknown
  module, no bad RSP exit, no stubbed non-gfx task). The branch is in bank unit
  N, which is not resident during boot, so this is an assertion and not the
  reason.

* **The screen is fast (developer-driven, fixed build).** Same environment as
  §1 with `OGRE_COVER=/tmp/cover-item-fixed.txt` and log
  `/tmp/ogre-item-fixed.log`. Scene `0x03` display-list intervals:

  | run | frames | p50 | p90 | p99 | max |
  |---|---|---|---|---|---|
  | pre-fix `/tmp/ogre-item-dev.log` | 6660 | 13 ms | 31 ms | **209 ms** | **275 ms** |
  | fixed `/tmp/ogre-item-fixed.log` | 3924 | **8 ms** | **14 ms** | **34 ms** | **155 ms** |

  The fixed run's largest scene-`0x03` gap (155 ms) is a `0x02`/`0x0D`
  transition, not a work frame. **`0x801DA248` appears in no `[prof] T=` line of
  the fixed run** (pre-fix: 99 % of `t4` for 13 consecutive seconds).

* **The screen was actually reached.** `0x801DA248` is present in
  `/tmp/cover-item-fixed.txt` (1650 entered functions), so the function — and
  therefore the path that calls it — executed. The developer confirms the screen
  is normal.

* **One unrelated crash, pre-existing.** Both runs end, after the coverage dump
  on window close, with `[crash] host pc func_8007F8E4 + 0x2F8`. Session 85
  recorded the same teardown crash. It is after the dumps and is not this bug.

## 5. What was run

* Two developer-driven runs at `OGRE_SPEED=4` with
  `OGRE_PROFILE=1 OGRE_DL_TRACE=1 OGRE_SCENE_LOG=1 OGRE_COVER=…`
  (`/tmp/ogre-item-dev.log`, `/tmp/ogre-item-fixed.log`).
* One bounded boot assertion on the fixed build (`/tmp/ogre-boot-fix.log`).
* `make recomp`, `make bank-recomp` (with `cross_bank.py check-banks`),
  `cmake --build build-app -j8`.
* **No probes.** Nothing is tagged, no generated file was hand-edited, and no
  submodule was touched. `git status --short` shows only `config-bankN.toml`
  (plus the pre-existing `tools/RT64` dirt).

## 6. Files changed

* `config-bankN.toml` — `0x801DA2C0` added to `yield_work_loop_branches`, with
  the loop's addresses and the measured cost in the comment.
* `docs/HANDOFF-2026-09-20-session90.md` (this file), `PLAN.md`,
  `docs/DECISIONS.md`, `docs/README.md`.

Generated but gitignored (regenerated by the two make targets):
`BankNFuncs/funcs_2.c` (the one changed file), `app/src/bank_funcs.inc`.

## 7. Next leads

1. **The same shape is one branch away: `func_ovlN_80204C08` (`0x80204C08`,
   backward branch `0x80204C48`) has the identical signature** — no calls, no
   stores, and an invariant-base load `lb $v1, 104($t1)` that *is* compared
   against a running match count, so the heuristic's key is the loop's bound.
   `t4` was 45–74 % of samples in it during the two seconds at the onset of this
   same screen (`T=136209`, `T=137209`), and 44–51 % during ordinary mission play
   (`T=39209`–`T=42210`) with **no** visible slowdown. Its yield is therefore not
   landed. If another screen stutters with `0x80204C08` hot, the fix is the same
   one-line addition.
2. **Generalize the discriminator, not the site list.** Session 80 §7's rule (b)
   — the backward branch's tested value must come transitively from the
   invariant-address load — rejects both this site and the mission's
   `0x801B3008`, because both branch on an immediate bound. It must still be
   prototyped against the boot (session 79's three broad repairs all starved it)
   before it can replace per-site listing.
3. **The scrollback is free.** `tools/proflog.py --names` on
   `/tmp/ogre-item-dev.log` is the whole diagnosis for this class: the frame-pump
   thread's `[prof]` line names the function, and `grep yield_self` in that
   bank's generated file confirms the injection.
