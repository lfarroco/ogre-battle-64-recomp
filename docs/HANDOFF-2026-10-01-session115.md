# Session 115 — the unit-command menu's slow frame is the fifth injected work-loop yield (`0x80204C48`)

Date: 2026-10-01. Follows session 112, which fixed the same class in the unit
list.

## Goal

Developer report: *"the game slowdowns when the 'unit menu' is open. this happens
when we select a unit, and a menu with some options ('commands', 'unit commands',
'stronghold commands', 'status') show up"*, reproducible by going to the tutorial
practice stage. The developer offered to drive the game.

## Result

`func_ovlN_80204C08` (bank unit N, record 9) carried a recompiler-injected
blocking `yield_self` at its loop's backward branch `0x80204C48`. The loop walks
36-byte entries in an array, counts the entries whose word at `+8` equals an id,
and exits when that count equals a signed byte in the game's state struct. In the
captured menu state it exits after five iterations, so the call paid four waits
of about one VI retrace each, and the frame-pump thread `t4` sat in that
function.

With the unit-command menu open (`Commands` tooltip, `LEADER Deneb`) the mission
ran at **37 display lists/s, p50 21 ms, p90 50 ms**, against **75/s, p50 10 ms,
p90 24 ms** before the menu opened. A host stack sample put **3893 of `t4`'s 7071
samples (55 %)** in `yield_self` under `func_ovlN_80204C08` over the 14 slow
seconds.

`config/banks/config-bankN.toml` now lists `0x80204C48` in
`yield_work_loop_branches`. Regeneration removes exactly one site tree-wide
(48 → 47). The fixed build runs the same route with the same four presses at
**96-106 display lists/s, p50 8 ms, p90 10-12 ms**, no sample mentions the
function, and its coverage census still shows it running
(`[cover] 0x80204C08 2064`). The developer confirmed the fix live.

No generated file, no app source and no submodule was hand-edited.

---

## 1. Reproduction

The checkpoint the developer saved for this session does not replay (see §6), so
the runs use the developer's battery save and session 112's tap recipe:

```sh
OGRE_SAVE=assets/scene_5_suspend.bin OGRE_SAVE_RESET=1 OGRE_TAP_MS=2000 \
  OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:1,0x03:a:4" \
  OGRE_SCENE_LOG=1 OGRE_DL_TRACE=1 OGRE_PROFILE=1 OGRE_COVER=<path> \
  ./build-app/ogrebattle64 assets/ogre64.z64
```

Pre-fix scene timeline (`/tmp/ogre-menu-mission/run.log`): `0x09` at 3.7 s →
`0x0A` 13.8 s → `0x04` 21.1 s → `0x17` 22.8 s → `0x02`/`0x0D` 27.6 s → `0x03`
27.8 s (descriptor `0x8018F350`, mask `0x0000038C`). The four `0x03` presses land
at 28, 30, 32 and 34 s. Display-list periods, from the `[renderer] display list
N at t=` lines:

| window | lists | rate | p50 | p90 | max |
|---|---|---|---|---|---|
| mission before the presses | 464 | 75/s | 10 ms | 24 ms | 103 ms |
| mission after the presses (menu open) | 992 | 37/s | **21 ms** | **50 ms** | 185 ms |

`docs/proofs/native-mission-unit-command-menu-slow.png` is the captured screen at
the slow point: the command-icon row with the `Commands` tooltip, the selected
party sprite and the unit panel (`Unit No.01`, `Autonomous`, `LEADER Deneb`).
RT64's presented-frame capture wrote it through the live console's `snap`
command (`/tmp/ogre-menu-mission/now.bin`, present index 2389).

The same run's `[prof]` lines name the function in the first slow second
(`T=32879 t4:80204C08(39%)`, `T=33879 … (58%)`), and the runtime's own watchdog
recorded `[snap] hotloop t4: 0x80204C08 x100`.

## 2. The measurement that names the cost

`OGRE_PROFILE` reports the most recently entered function and not where the
thread is spending time (session 112 §1), so the diagnosis rests on host stack
samples taken once a second while the game ran (`sample <pid> 1 -file …`,
`debug/menu-probe.sh`). Fourteen of the 28 samples in the slow window put `t4`
in `yield_self`:

```
func_8008AFE0 (frame pump)
  func_80072398
    func_800765D8
      func_ovlN_802043BC
        func_ovlN_80204C08
          yield_self
            ultramodern::wait_for_external_message
```

Per-second fractions ran 18 % to 82 %; the sum is 3893 of 7071 `t4` samples.

## 3. Instruction-level evidence

`build/bankN.elf`, section `.bankRec9`:

```
80204c08  li    t0,-1
80204c0c  move  a3,zero
80204c10  andi  a1,a1,0xff          ; the id to match
80204c14  lui   t1,0x8021
80204c18  lw    t1,0x3690(t1)       ; t1 = state, loaded once
80204c1c  andi  v0,a3,0xff          ; <- loop head, index & 0xFF
80204c20  sll   v1,v0,3
80204c24  addu  v1,v1,v0
80204c28  sll   v1,v1,2             ; v1 = index * 36
80204c2c  addu  a2,a0,v1            ; a0 = array base
80204c30  lw    v0,8(a2)            ; entry word at +8
80204c34  beql  v0,a1,80204c3c
80204c38  addiu t0,t0,1             ; count a match
80204c3c  lb    v1,0x68(t1)         ; the exit threshold
80204c40  sll   v0,t0,24
80204c44  sra   v0,v0,24
80204c48  bne   v0,v1,80204c1c      ; <- the injected yield's branch
80204c4c  addiu a3,a3,1             ; delay slot: next entry
80204c50  lbu   v0,3(a2)
80204c54  jr    ra
```

The body makes no call and no store. The invariant-base load the heuristic keys
on is `lb $v1, 0x68($t1)` at `0x80204C3C`, the loop's own exit threshold; `$t1`
comes from one load at `0x80204C18` and the loop never writes it. Raw ROM check
(rule 8, check 5): record 9 is ROM `0x14EC00` → RAM `0x801FDA90`, so RAM
`0x80204C48` is ROM `0x155DB8` = `0x1443FFF4` (`bne $v0, $v1, 0x80204C1C`), delay
slot `0x24E70001` (`addiu $a3, $a3, 1`). `0x80204C3C` is ROM `0x155DAC` =
`0x81230068`.

Three call sites reach it, all with the same arguments: `a0 = state[+0x60]` (the
array base) and `a1 = state[+0x66]` (the id), from `func_ovlN_80204230` at
`0x80204300` and from `func_ovlN_802043BC` at `0x802048B4` and `0x80204B20`. The
frame-pump path is `func_8008AFE0 → func_80072398 → func_800765D8 →
func_ovlN_802043BC`.

## 4. Why this is a work loop and not a poll

The exit threshold `state[+0x68]` is written by the caller immediately before the
call, at `0x80204874` (`sb $a1, 0x68($v0)`). The caller computes that value in its
own scan of the *same* array at `0x8020484C..0x80204868`, which carries no
`yield_self` and terminates on data (`word@+0xC == $a2`). The same array is
therefore complete when this code runs, and the callee's scan cannot wait for
another thread to extend it.

The iteration count confirms it. `snap` wrote `/tmp/ogre-menu-mission/now.bin`
while the menu was open; reading it with the runtime's byte order gives
`state = *(0x80213690) = 0x800E91D0`, `state[+0x60] = 0x801F3A90`,
`state[+0x66] = 0`, `state[+0x67] = 4` and `state[+0x68] = 1`. Running the loop
over that array: the entries whose word at `+8` equals `0` are at indices 0 and
4, so the exit fires at index 4 — **five iterations, four taken back edges**, and
each taken back edge blocked for about one VI retrace (16.7 ms at `OGRE_SPEED=1`).
Four waits per call are 67 ms, which is the measured p90.

The exit threshold is not a status word another thread produces: it is the number
of matching entries in an array that the frame that calls this function has
already built. This is the session-80/90/98/112 class.

## 5. The fix and the regeneration

`config/banks/config-bankN.toml`:

```toml
yield_work_loop_branches = [0x801B3008, 0x801DA2C0, 0x801D0C08, 0x8020A910, 0x80204C48]
```

with the loop, the invariant load, the captured state and the measurement in the
comment.

* Tree-wide `yield_self(rdram)` sites **48 → 47**; `BankNFuncs/funcs_2.c` **6 → 5**.
* The generated loop is now:
  ```c
      // 0x80204C48: bne         $v0, $v1, L_80204C1C
      if (ctx->r2 != ctx->r3) {
          // 0x80204C4C: addiu       $a3, $a3, 0x1
          ctx->r7 = ADD32(ctx->r7, 0X1);
              goto L_80204C1C;
      }
  ```
* `make bank-recomp` ran `cross_bank.py check-banks`: `check OK`.
* `make elf-rom-check`: **0 differing bytes** for all 34 bank ELFs and the main
  ELF, and every address-named symbol at its named address (4221 in the main
  ELF).
* `cmake --build build-app -j8`, `cmake --build build-null -j8`.

## 6. Verification

| check | result |
|---|---|
| pre-fix, same save and presses | 37 display lists/s, p50 21 ms, p90 50 ms; 3893/7071 `t4` samples in `yield_self` under `func_ovlN_80204C08` |
| fixed run 1 (`/tmp/ogre-menu-fixed/run.log`, 80 s) | mission after the presses: 1478 lists over 30 s, p50 **8 ms**, p90 **16 ms**; **0 of 30 samples** park `t4` in that function; one 1017 ms gap is a single list with 199078 entries (the pre-fix run has 332992- and 302814-entry lists) |
| fixed run 2 (`/tmp/ogre-menu-fixed2/run.log`, 95 s) | mission 23.3 s, presses 24-30 s: 715 lists at **106/s, p50 8 ms, p90 10 ms**, then 3448 lists at **96/s, p50 8 ms, p90 12 ms**; **0 of 33 samples** mention `func_ovlN_80204C08` |
| fixed run 3 (`/tmp/ogre-menu-fixed3/run.log`, 100 s) | mission 60.3 s, presses 58-64 s: 541 lists at p50 9 ms, p90 21 ms, then 897 lists at p50 **9 ms**, p90 **19 ms** |
| the path still runs | `[cover] 0x80204C08 1946` (run 1) and `2064` (run 2) |
| `tools/runlog.py --check` on the fixed app run | **PASS**: no crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx task |
| `build-null`, 45 s boot, `tools/runlog.py --check` | **PASS** |
| `make elf-rom-check`, `cross_bank.py check-banks` | 0 differing bytes; check OK |
| **developer confirmation** | **"it's fixed!!"** |

The fixed run 1's mission also reached 49 lists/s over a 30 s window that
includes the mission's own ~300k-entry bursts, so its rate is not comparable to
the 8 ms median; the medians and the stack samples are the measurement.

### The checkpoint saved for this session does not replay

The developer saved a checkpoint (`/tmp/ogre-menu-probe/menu-open.ckpt`) just
before the menu opens, so the menu could be driven with one press. Loading it in
a fresh process restores RDRAM and the overlay state and reports the right scene
(`load … scene 0x0009 step=0 -> scene 0x0003 step=0`), but the machine then
stops: **no display list and no RSP task is submitted after the load**, and every
game thread sits in `wait_for_resumed`. The runtime's own counters show the
retrace clock advancing while the frame counter is frozen
(`frame: retrace(D_800C4BCC)=21191 last(D_800AEFA4)=8656`). A press of `a`
produced a 3-byte RDRAM diff (two counters), so the game was not running.
`write_checkpoint`/`read_checkpoint` carry the 8 MiB RDRAM image and
`recomp::overlays`' blob; the runtime's thread and message state is native and is
not part of either. Session 58's checkpoint use was a dialogue screen waiting for
input, where no message was in flight. A checkpoint taken in an active scene
therefore does not resume. `debug/menu-replay.sh` was written for that route and
deleted again.

## 7. What was run

* `debug/menu-probe.sh` (new): launches `build-app/ogrebattle64` with
  `OGRE_SCENE_LOG`, `OGRE_DL_TRACE`, `OGRE_PROFILE` and `OGRE_COVER`, binds
  `OGRE_KEY_1=snap`, `OGRE_KEY_2=save`, `OGRE_KEY_3=cover`, and runs
  `sample <pid> 1` in a loop for the life of the process. All slow-screen
  measurements in this session come from it.
* Four runs of the save + tap route (one pre-fix, three on the fixed build) and
  one run of the checkpoint route (`/tmp/ogre-menu-load`).
* The live console (`snap`, `c`, `cover`) through `/tmp/ogre-console.txt`; the
  RDRAM images `/tmp/ogre-menu-mission/now.bin` and `menu.bin`; the presented
  frames `/tmp/ogre-shot.2389.ppm` and `4026.ppm`.
* `mips-linux-gnu-objdump -d build/bankN.elf`, a raw-ROM word read, and the
  generated C at `BankNFuncs/funcs_2.c`.
* `make bank-recomp`, `make elf-rom-check`, `cmake --build build-app`,
  `cmake --build build-null`, `tools/runlog.py --check`.
* **No probes.** Nothing carries a probe tag, no generated file was hand-edited,
  and no submodule was touched. The app binary was not rebuilt between the
  checkpoint write and the load attempt, because the checkpoint's build id is a
  hash of the executable.

## 8. Files changed

* `config/banks/config-bankN.toml` — `0x80204C48` appended to
  `yield_work_loop_branches`, with the loop, the exit threshold, the captured
  state and the measurement in the comment.
* `debug/menu-probe.sh` — the launcher and host-stack sampler described in §7.
* `docs/proofs/native-mission-unit-command-menu-slow.png` — the captured slow
  screen.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, this
  file.

Generated and gitignored, refreshed by the two make targets: `BankNFuncs/**`,
`RecompiledFuncs/**`, `app/src/bank_funcs.inc`, `build-app/**`, `build-null/**`.
`git status --short` shows the tracked files above and the pre-existing
`m tools/RT64`.

## 9. Next leads

1. **The remaining 47 `yield_self` sites** (`grep -rn yield_self Bank*Funcs/
   RecompiledFuncs/`). The five landed are `0x801B3008` (session 80),
   `0x801DA2C0` (90), `0x801D0C08` (98), `0x8020A910` (112) and `0x80204C48`
   (this session). The shape is unchanged: read the top of the slow thread's
   stack, grep that bank file for `yield_self`, and confirm the loop is a work
   loop before listing its branch.
2. **A checkpoint does not replay an active scene** (§6). Either the runtime's
   thread and message state has to join the blob, or `save`/`load` should refuse
   a state whose threads are not all blocked in `osRecvMesg`.
3. **`debug/menu-probe.sh` is the reusable form of this diagnosis.** It is one
   command for log, stack samples, `snap`, `save` and `cover`, and it needs no
   agreed timing: the slow second is captured whenever it happens.
4. **Session 90's general discriminator is still unlanded** (rule (b): the
   backward branch's tested value must come transitively from the
   invariant-address load). This site compares the accumulator *against* that
   load, so the rule does not decide it; the caller's own yield-free scan of the
   same array does. Per-site listing stays necessary.
