# Handoff — 2026-09-14, session 42: `D_8018F1C0`'s writer found, and the re-entry's selector-2 branch is the game's own data

## Goal and result

Continue session 41's two open questions: (1) find the writer of
`D_8018F1C0` (the input word that selects the crashing branch), and (2) decide
whether the selector-2 branch is a legitimate game state. Both are answered,
and the answer re-frames the wall.

Result:

1. **The writer is the scene-script VM `func_80170974`** (overlay B): it stores
   `D_8018F1C0` through `$s5 = &D_8018F1C0` at `0x80170ADC` (`sh $a0, 0($s5)`,
   VM `var[0]`), with the derived stores at `0x80170B00` (`|0x4000`),
   `0x80170B88`, `0x80170BC4`. Session 41's "no recompiled function writes those
   values" was a symbol-name audit, so it could not see a register-relative
   store. Measured in-run: the VM writes `1` at script pc 16, `2` at pc 32.
2. **`1` then `2` is exactly what the scene-0x0D path reads** — `func_80226FA8`
   saw `F1C0=0x0001` on visit 1 and `0x0002` on visit 2. `func_80178568`'s own
   `D_8018FC39` store is *not* the producer (it needs bit 15 set or `F1C0 == 0`).
3. **The selector is data-driven, not a port artifact.** `func_80226FA8` calls
   `func_8022683C(-5, D_8018F1C0 & 0xFFF)`; `func_80227E64(n)` decompresses the
   descriptor asset for step `n` (asset table `0x19A8804` = ROM `0x1F3CA54`) and
   returns that step's command: **`n=1 → -7`, `n=2 → -3`**. Command `-7` →
   jump-table index 3 → handler `0x80226AC4` → `func_80227700(0)` (selector 0);
   command `-3` → index 7 → handler `0x80226E30` → `func_80227700(2)`
   (selector 2). The recompiler expands `jtbl_ovlC_8022ABA0` into a `switch`
   with exactly those targets (`BankCFuncs/funcs_3.c`), so the path the crash
   takes is the path the game's data asks for.
4. **The two visits are the New Game intro's step 1 and step 2, not the attract
   loop** (new measurement): a 170 s no-tap run at `OGRE_SPEED=8` cycles only
   `0x09 → 0x0A → 0x04 → 0x0B → 0x04 → 0x0C → 0x04 …`, never enters `0x02`/
   `0x0D`, and never crashes. Start at the title reaches `0x02` because the
   title's transition handler `func_80177A58` does `D_800C4C26 = D_8018F1C2`
   in state 1, and the *same VM store site* writes `D_8018F1C2 = 0x8002`
   (scene 0x02).
5. **Why it dies anyway**: at the crashing call the port has
   `sp=0x800C22A8`, `s0=0`, `*(sp+0x1EC)=0`; visit 1's per-frame
   `func_ovlC_80239874` frames ran at `sp=0x800C1BF0` and their `a2` slot landed
   at `0x800C1DDC` — `0x6B8` below the address path B reads. Both scene-setup
   callbacks run at the *same* depth (`sp=0x800C22C0`), so the read address is
   fixed by the call chain. The "leftover frame" that path B needs is never
   written by anything in this run.
6. **A free lead**: the title's other menu entries (scenes `0x12` and `0x17`)
   are not on the crashing path at all; `0x17` is blocked *only* by two
   uncompiled bank records (17 and 18), and the repo's own bank configs already
   say so. See §5.

No fix. All probes reverted via regen; both build variants rebuilt and the
runs below re-verified.

## 1. `D_8018F1C0`'s writer (the session-41 question)

Session 41 audited writers by grepping the symbol; every store in
`func_80170974` is `$s5`-relative, so none showed up. In
`asm/40E80.s` (overlay B):

```
80170994  lui   $s5, %hi(D_8018F1C0)
80170998  addiu $s5, $s5, %lo(D_8018F1C0)      ; s5 = &D_8018F1C0
80170ACC  sh    $v0, 0x2($s5)                  ; D_8018F1C2 = 0x8002
80170ADC  sh    $a0, 0x0($s5)                  ; D_8018F1C0 = var[0]
80170B00  sh    $v0, 0x0($s5)                  ;   var[0] | 0x4000
80170B8C  sh    $v0, 0x0($s5)                  ;   var[0] & ~0x4000
80170BC4  sh    $zero, 0x0($s5)                ;   0
```

`func_80170974` is the scene-script VM: `$s7` = script base (`a0`), `D_80197B24`
= program counter, `$s1 = sp+0x10` = a 16-bit operand stack, and the operand
bytes index an 8-entry variable file at `sp+0x50` (the VM reads it at
`0x80170A84` as `lhu (op&7)*2 + 0x40($s1)`). Dispatch is `jtbl_80190758` on the
byte at `script[pc*2]`.
The other writers are unchanged from session 41: `func_80171BB4` (`sh 0x8000`
under `D_80197B30 & 8`), `func_80178568` (`0`), `func_801788B8` (`0`), bank unit
C `func_ovlC_8022E3E4` (`0`).

Measured per-press (probe at `0x80170ADC`, `RecompiledFuncs/funcs_3.c`):

```
[probe42] VM 0x80170ADC F1C0=1 F1C2=0x8002 pc(D_80197B24)=16
[probe42] VM 0x80170ADC F1C0=2 F1C2=0x8002 pc(D_80197B24)=32
```

so the script runs 16 opcodes per title visit and bumps `var[0]` once per visit.

## 2. What the scene-0x0D path reads (visit 1 vs visit 2)

Probes in `BankCFuncs/funcs_13.c` (`func_ovlC_80226FA8` entry),
`funcs_3.c` (`func_ovlC_8022683C`), `funcs_0.c` (`func_ovlC_80227700`):

```
visit 1: 80226FA8 F1C0=0x0001
         8022683C cmd=-5 n=1 E64=-7 sp=0x800C6C78
         8022683C dispatch s0=-7 index=3 target-entry=0x8022ABAC
         80227700 sel := 0
         80225A3C enter sp=0x800C22C0
         8022D1CC enter sp=0x800C22A8 s0=0 sel=0 → path A → 8023A9AC
visit 2: 80226FA8 F1C0=0x0002
         8022683C cmd=-5 n=2 E64=-3 sp=0x800C6C78
         8022683C dispatch s0=-3 index=7 target-entry=0x8022ABBC
         80227700 sel := 2
         80226110 enter sp=0x800C22C0
         8022D1CC enter sp=0x800C22A8 s0=0 sel=2 → path B → 802399AC → NULL
```

`func_80178568`'s selector store (`0x80178748`) never fired: with `F1C0` bit 15
clear and non-zero the code takes the `func_80226FA8` branch instead. That also
retires session 41's "`0x8018F1C0 == 0x8002` with bit 15 set" reading — `0x8002`
is `D_8018F1C2` (offset +2), a *different* halfword.

### The command table is the game's own data

`func_8022683C(cmd, n)` overrides `cmd` with `func_80227E64(n)` when non-zero,
then dispatches `cmd + 0xA` through `jtbl_ovlC_8022ABA0` (`0x8022ABA0`, ROM
`0x286B20`, nine entries). Read straight from the ROM:

| index | cmd | handler | what it does |
|---|---|---|---|
| 3 | `-7` | `0x80226AC4` | `func_80227700(0)` @`0x80226BB4`, registers `func_ovlC_80225A3C` @`0x80226BC0` |
| 7 | `-3` | `0x80226E30` | `func_80227700(2)` @`0x80226F18`, registers `func_ovlC_80226110` @`0x80226F24` |
| 0 | `-10` | `0x80226E30` | same handler as `-3` |
| 5 | `-5` | `0x80226F98` | the epilogue (no-op) |

`func_80227E64(n)` loads asset `0x19A8804` (via `func_8009DAF4`, i.e.
`(id & 0x0FFFFFFF) + 0x594250` = ROM `0x1F3CA54`), indexes it as a u32 array,
then loads+decompresses the selected descriptor (`func_8007A7E0` =
size, `func_8007A110` = LZ decompress):

| n | descriptor id | ROM | command read back |
|---|---|---|---|
| 1 | `0x019AA27C` | `0x1F3E4CC` | `-7` |
| 2 | `0x019AA5B6` | `0x1F3E806` | `-3` |
| 7 | `0x019AAD06` | `0x1F3EF56` | (0x02 loader's step, see §4) |

So step 2's descriptor really does ask for the selector-2 scene state. This is
game data, not a mis-binding.

## 3. Why path B cannot work at this depth

`func_ovlC_8022D1CC` (frame `0x18`) does `jal 0x802399AC` at `0x8022D218` when
`D_8018FC39 == 2`. `0x802399AC` is the fall-through continuation of
`func_ovlC_80239874` (no prologue; verified again in `build/bankC.elf`): its
frame is written by that function's `addiu sp,sp,-0x238` prologue, which also
sets `s0 = a0` and `sw a2, 0x1EC(sp)`. The continuation reads `s0` at
`0x802399EC` and `a1 = *(sp+0x1EC)` at `0x80239C1C`.

Measured in visit 1 (the healthy per-frame path),
`func_ovlC_80239D68 → func_ovlC_80239874`:

```
80239D68 enter sp=0x800C1E28
80239874 enter sp=0x800C1BF0 a0=0x800C2040 a1=0x800C2120 a2=0x800C1EC0
802399AC enter sp=0x800C1BF0 s0=0x800C2040 slot(sp+0x1EC)=0x800C1EC0   → healthy
```

and on visit 2:

```
8022D1CC enter  sp=0x800C22A8 s0=0x00000000 sel=2
8022D1CC pathB  slot(sp+0x1EC)=0x00000000 s0=0x00000000
```

`func_ovlC_80239874` writes its slot at `sp+0x1EC = 0x800C1DDC`; path B reads
`sp+0x1EC = 0x800C2494`. The gap is `0x6B8`, and both scene-setup callbacks
(`func_ovlC_80225A3C` for selector 0, `func_ovlC_80226110` for selector 2) enter
at the *same* `sp = 0x800C22C0`, so nothing in the run moves the read address
onto a written slot. `s0` is equally unset: neither `func_80226110` nor
`func_8022D1CC` writes `s0`, so it is whatever the 0xC000 task dispatcher
(`func_80076F5C`'s table at `D_800E82C8`) left in it — zero here.

## 4. `0x02`/`0x0D` is the New Game intro, not the attract loop

Session 41's `0x0D` analysis never established which flow reaches the loop. A
tap-free run settles it:

```
OGRE_SPEED=8 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=170000 ./build-null/ogrebattle64
[scene] t=1424ms  id=0x0009 (intro)
[scene] t=2801ms  id=0x000A (publishers)
[scene] t=5195ms  id=0x0004 (title)
[scene] t=14991ms id=0x000B (story)
[scene] t=43320ms id=0x0004
[scene] t=53098ms id=0x000C (unit-info)
[scene] t=79497ms id=0x0004
[scene] t=89297ms id=0x000B ... 0x0004 → 0x000C → 0x0004 → 0x000B …
exit 0, no crash
```

The attract loop is `title ↔ {story, unit-info}` and never enters `0x02`/`0x0D`.
The New Game entry is a state machine on scene `0x04`'s descriptor:
`func_80177A58` (descriptor word 1; word 0 is the per-frame `func_80177A3C`,
which just calls overlay C's `func_8019B300`). It switches on
`func_8019BD0C()`:

```
v1 = func_8019BD0C()
v1 == 1 → func_80172388(); func_801723B4(); func_8017225C(1); func_80170814();
          D_800C4C26 = D_8018F1C2          ; ← the next scene comes from here
v1 == 2 → D_800C4C26 = 0x8012
v1 == 3 → D_800C4C26 = 0x8017
v1 == 4 → D_800C4C26 = 0x0B/0x0C (the attract cycle seen above)
```

`func_8019BD0C` returns 1 when the start button is down (it tests the button
word `D_800C4B28`), 4 when its selection counter `D_801BA565 == 0`, otherwise
`D_801BA565` (1..3). `D_8018F1C2` has several readers (`0x80178018`,
`0x80178CD0`, `0x8017B940`, `0x8017B9A0`, `0x80177AD0`), and `0x80177AD0` is the
one that copies it into `D_800C4C26` — matching the observed title → `0x02`
transition. And `D_8018F1C2 = 0x8002` (scene `0x02`) is written by the *same VM
store site* that writes `D_8018F1C0` — so the scene script programs both the
next scene and the step index, and the title's state-1 path applies them.
Title → `0x02` is therefore the New Game path, and `0x02` → `0x0D` → `0x02` →
`0x0D` is its scripted intro sequence, one step per visit.

### Side note (open, harmless)

The scene-`0x02` loader `func_ovlE_80198D28` reads `D_8018F1C0 & 0x3FFF = 7` on
both visits (`[probe42] E98D28 n=7 out0=0 out2=-1`), while the scene-`0x0D` path
reads `1`/`2` at its own moment. The loader and the 0x0D path run at different
times (the log cannot order them across threads), and the VM has other
`D_8018F1C0` stores (`|0x4000` at `0x80170B00`), so `0x4007`/`7` is probably a
different script opcode. It does not affect the `0x0D` command selection, which
is measured inside `func_ovlC_80226FA8` immediately before it calls
`func_8022683C`.

## 5. The title's other entries (forced probes, free win found)

`func_80177A58` sends `func_8019BD0C()` state 2 → scene `0x12` and state 3 →
scene `0x17`. Forcing those ids shows what they still need:

```
OGRE_SCENE=0x12 ... -> id=0x0012 descriptor=0x8018FBC0 mask=0x00008000
                       then SIGSEGV at N64 0x00000001 on thread 4
                       (fault = rdram base + 1: a near-NULL pointer; the
                        forced path has no title pre-state)
OGRE_SCENE=0x17 ... -> id=0x0017 descriptor=0x8018FE50 mask=0x00060000
                       [bank] UNCOMPILED streamed record 17: rom=0x069920
                              ram=0x80197B90 size=0x4D60
                       [bank] UNCOMPILED streamed record 18: rom=0x1BA020
                              ram=0x80220F60 size=0x92B0
                       then idle, exit 0 (nothing crashed — the records are
                       simply not registered, so their stubs return)
```

Scene `0x17` is blocked *only* by two uncompiled bank records, and the repo
already anticipates both: `config-bankD.yaml` ("records 0, 17 (both also at
`0x80197B90`, so they need their own units)"), `config-bankE.yaml` ("Records 17
(also `0x80197B90`) … are not part of this unit") and `config-bankC.yaml`
("records 18 (ROM `0x1BA020`) … are not part of this unit"). Both RAM ranges
collide with records already in a unit (`0x80197B90`: 0/1/2/15; `0x80220F60`:
records 12/13), so each needs its own unit — `BANK_UNITS := A C D E` leaves `B`
free. `tools/gen_bank_funcs.py` also needs a `RAM_END` entry per new record
(the segment table at ROM `0x387C0` has the authoritative
`{ram start, ram end, rom start, rom end}`), and `bank_overlays.cpp`'s
`kAllStreamedRecords` already lists 17/18 with `compiled = false` to flip.


## 6. The scene lifecycle, corrected (sessions 40/41 had the descriptor half-wrong)

The scene manager is `func_80075BC0`. Scene ids are **not** dispatched through
the descriptor's word 0; `func_80075BC0` hardcodes a table of *descriptor
accessors* at `D_800AF028` (0x800AF028 + id*4, ids ≥ 0x1F map to index 0) and
calls `D_800AF028[id]()` first:

| id | accessor | returns |
|---|---|---|
| 0x02 | `func_8017848C` | `&D_8018FC50` |
| 0x04 | `func_80177A18` | … |
| 0x06 | `func_8017B5DC` | `&D_8018FD84` / `&D_8018FD98` (on `D_80193700`) |
| 0x0D | `func_80178480` | `&D_8018FC3C` |
| 0x12 | `func_80178074` | … |
| 0x17 | `func_801862F0` | `&D_8018FE50` / `&D_8018FE64` (on `D_80196B0C` bit 3) |
| 0x18 | `func_8017BA54` | `&D_8018FDC0` |

The returned pointer is the scene descriptor (`D_800E8294`), and its words are
called from fixed points of the frame loop:

| descriptor | called from | meaning |
|---|---|---|
| `+0x00` | `0x80075F58` (`jalr`) | **enter** (once, at scene start) |
| `+0x04` | `0x8007265C` (`jalr`) | **per-frame update** (frame/render path) |
| `+0x08` | `0x80072544` (`jalr`) | second per-frame hook |
| `+0x0C` | `0x8007602C` (`jalr`) | **leave** (at scene end) |
| `+0x10` | `func_800761E4`/`func_80076324` | bank-record **mask** |

So for scene `0x0D`: enter `func_80178568`, updates `func_80178954` /
`func_80178B40`, leave `func_80178B7C`, mask `0x40007C14`. Session 41's
"`func_80178568` is the per-frame update" and "`func_80178954` is the per-frame
update" are both half-right: it is enter / update respectively, and the selector
logic therefore runs **once per visit**, not per frame.

### The New Game opening is a scripted sequence, not a single cutscene

`0x02`'s enter (`func_80178920`) sets `D_800C4C26 = 0x800D` (next = `0x0D`) and
`0x0D`'s leave (`func_80178B7C`) contains, verbatim:

```
if (D_8018FC39 != 0) func_801B00D0();
...
if (block->id == 0x0D) block->id = 0x02;      // 0x0D → 0x02, unconditionally
```

so any step whose selector is non-zero is sent back to `0x02` by the leave. The
step index (`D_8018F1C0`) and the *next scene* (`D_8018F1C2 = 0x8002`) are both
written by the scene-script VM `func_80170974` (§1), which advances 16 script
opcodes per visit. `0x0D`'s update ends a segment through
`func_801C8884()` → next scene `0x06` (`D_800C4C26 = 0x8006`), or state 5
(`func_80178CB0`, which forwards `D_8018F1C2`), or `0x02`. Scene `0x06`
(`&D_8018FD84`, mask `0x00000002`) is an overlay-B screen whose enter
`func_8017B6D0` copies display lists, DMAs a large block and calls
`func_801C19B0` — the shape of one of the form/question screens the game's
opening is expected to reach.

### `0x0D`'s enter has two modes, selected by `D_8018F1C0`

```
if (F1C0 & 0x8000) goto movie;          // bit 15
if (F1C0 != 0)     goto command;        // ← our run: F1C0 = 1, 2, …
movie:   D_8018FC39 = 2
         memcpy/memmove inside record 10, DMA ROM 0x213AE0 → 0x801D0860 (record 10a),
         DMA ROM 0x22A250 → 0x801E6FD0 (record 10b), func_801D0860()
             (D_801D0830 = &D_801E5AC0 — the cutscene/movie vtable)
         D_8018F1C0 = 0; func_801AFC2C(0); func_80073164(...); D_8022A9A0 = 0xFF
command: func_80073164(...); func_80226FA8()
             → func_8022683C(-5, F1C0 & 0xFFF)      (§3)
```

The two modes load *different* code: command mode DMAs records **14a/14b**
(`func_ovlC_8022D1CC`/`80239874`/`802399AC` live there), movie mode DMAs records
**10a/10b** and installs the vtable at `&D_801E5AC0`. `D_8022A9A0` is the
update's "segment finished" gate: `!= 0xFF` runs the cutscene body
(`func_801C5A40` + `func_801C5910`), `== 0xFF` runs the next-scene decision.

### A/B: forcing movie mode

Forcing the step word to `0` from `poll_scene` while scene `0x02` is active is
**not** a clean test — the `0x02` loader (`func_ovlE_80198D28`) reads the same
word and changes branch. It does confirm the mode split (the run then loads
record 10a, `rom=0x213AE0 ram=0x801D0860 size=0x16770`) and dies at N64 `0` on
thread 3 in `func_80178568 → func_ovlC_801AFC2C → func_ovlC_801B7EBC →
func_8007A110`, i.e. **session 38's decompressor wall**.

Forcing the branch inside `func_80178568` instead (a one-line `if (0)` on the
`bne $v1, $zero, L_80178878` at `0x80178738`, leaving the loader's own `F1C0`
alone) reproduces the *same* crash on the same chain. Both runs reached `0x0D`
through the natural `title → Start → 0x02` path, and the `0x02` loader does
reach its writer call in this run (measured `n=7 → out0=0, out2=-1 →
.L80198E2C → func_ovlE_801988C8`). So session 38's "the natural path has the
pre-state, only the forced path lacks it" does **not** hold for this path: the
flag `0x8019F794` is apparently still `0` here, which is why
`func_ovlC_801B7EBC` computes asset id `0x148` and the decompressor is handed a
bogus size.

So there are **two distinct walls** on the New Game opening, one per mode:

| mode | what runs | wall |
|---|---|---|
| command (`F1C0 != 0`) | records 14a/14b, scripted command, selector | step 2: `func_ovlC_8022D1CC` → `jal 0x802399AC` with `s0 = 0` and an unprimed frame (N64 `0` in `func_800988A0`) |
| movie (`F1C0 == 0`) | records 10a/10b, vtable `&D_801E5AC0` | `func_801AFC2C(0)` → session-38 decompressor wall (N64 `0` in `func_8007A110`, flag `0x8019F794` still 0) |

## Verification (final binaries, all probes reverted via regen)

- `make recomp` + `make bank-recomp` clean (RC 0); `bank_funcs.inc` 1721
  functions, 4 units, 15 records; `grep -rl probe RecompiledFuncs/ Bank*Funcs/`
  = 0; `build-null` + `build-app` rebuild clean.
- Re-entry repro on the final binaries, identical to session 41:
  `0x02` @12457 ms → `0x0D` @12589 ms → `0x02` @41301 ms → `0x0D` @41426 ms →
  SIGSEGV N64 `0` on thread 4.
- Tap-free attract run (170 s, `OGRE_SPEED=8`): `0x09 → 0x0A → 0x04 → 0x0B →
  0x04 → 0x0C → 0x04 …`, exit 0, no crash, no `0x02`/`0x0D`.
- Forced scene runs (40 s, `OGRE_SPEED=4`): `OGRE_SCENE=0x12` enters `0x12` and
  dies at N64 `0x00000001` on thread 4; `OGRE_SCENE=0x17` enters `0x17`, reports
  uncompiled records 17/18, and exits 0 without crashing.
- Two mode A/B runs (§6), both reverted: `OGRE_POKE_F1C0=0` (step word poked at
  scene `0x02`) and the `func_80178568` branch force. Both reach `0x0D` and die
  at N64 `0` on thread 3 in `func_80178568 → func_ovlC_801AFC2C →
  func_ovlC_801B7EBC → func_8007A110` after loading record 10a.

No repo test harness exists; per the durable-test skill this is disclosed, not
fixed with a framework: each check is a ROM-dependent 12–170 s game run, so the
repro commands below are the maintained verification.

## Files changed (vs session 41)

- `PLAN.md` — session-42 status entry.
- `docs/HANDOFF-2026-09-14-session42.md` — this file.
- Temporary, both reverted: `app/src/bank_overlays.cpp` `OGRE_POKE_F1C0`
  diagnostic (removed) and a one-line `if (0)` on `func_80178568`'s
  `0x80178738` branch in `RecompiledFuncs/funcs_6.c` (removed by regen).
- Probes used, all reverted by regen:
  `RecompiledFuncs/funcs_1.c` (`func_801788B8`), `funcs_3.c` (`func_80170974`
  stores), `funcs_6.c` (`func_80178568` selector store);
  `BankCFuncs/funcs_0.c` (`func_ovlC_80239874`, `func_ovlC_80227700`),
  `funcs_1.c` (`func_ovlC_80225A3C`, `func_ovlC_80226110`),
  `funcs_3.c` (`func_ovlC_8022683C`), `funcs_12.c` (`func_ovlC_8022D1CC`,
  `func_ovlC_80239D68`), `funcs_13.c` (`func_ovlC_80226FA8`),
  `funcs_14.c` (`func_ovlC_802399AC`), `BankEFuncs/funcs_0.c`
  (`func_ovlE_80198D28`). No tracked source changed for the probes.

## What's next (session 43)

1. **Chase the flag behind the movie-mode wall first.** The movie mode is the
   path that loads records 10a/10b and the cutscene vtable — i.e. the game's own
   intro-movie engine, the thing the opening is supposed to play. It dies in
   `func_ovlC_801B7EBC` because `0x8019F794 == 0` sends it down the wrong-asset
   branch (id `0x148`, bogus size, NULL dst from `func_80070F30`). Session 38
   assumed a natural path leaves that flag set; this run reaches `0x0D` through
   `title → Start → 0x02` and the flag is still 0, even though the `0x02`
   loader's writer call *is* reached (`n=7 → func_ovlE_801988C8`). Next: probe
   `0x8019F794` at the loader's writer (`func_ovlE_801988C8`) and at
   `func_ovlC_801B7EBC`, and find out whether the writer is mis-dispatched or
   its store is conditional. That is a small, self-contained wall and it is on
   the intended path.
2. **Decide which `D_8018F1C0` value the first `0x0D` visit should have.**
   `1`/`2` (command mode) is what the script VM writes today, and it leads to
   the frame-less-continuation wall at step 2; `0` (movie mode) leads to the
   intro-movie engine. If the scripted steps are meant to be the *question*
   screens rather than the movie, the sequence may be starting one step early —
   probe the VM's script (`$s7` / `D_80197B24`) at the first store and decode
   the opcode.
3. **Compile bank records 17 and 18 so scene `0x17` can run** (still valid, and
   cheap): record 17 `rom=0x069920 ram=0x80197B90 size=0x4D60`; record 18
   `rom=0x1BA020 ram=0x80220F60 size=0x92B0`. One new unit per record (unit `B`
   is unused), a `RAM_END` entry per record in `tools/gen_bank_funcs.py`, and
   the two `compiled = false` flags in `kAllStreamedRecords`.
4. **Settle whether the command-mode path B is retail-reachable.** Everything on
   that path is the game's own code and data, and the read address is fixed by
   the call chain, so if nothing writes `0x800C2494` the value is `0` on
   hardware too. Concrete test: hardware watchpoint on
   `ultramodern::get_rdram_base() + 0xC2494` set after boot, or a temporary
   store check over the run.
5. **Drive the title's selection** so entries 2/3 (scenes `0x12`/`0x17`) are
   reached *with* the title's pre-state. `D_801BA565` is recomputed from a
   highlight bitmask in the title's input handler (`0x8019BB4C`), so poking the
   counter alone is overwritten — move the highlight (an `OGRE_TAP_BUTTON`
   D-pad schedule) or poke the mask.
6. Open and untouched: menu `0x18` natural entry (session-39 tail wall,
   `8017BB28 → 8019C69C`, N64 `0x14`), `OGRE_NO_AUDIO=1` early-boot crash,
   `osViFade`.

## Repro commands

```sh
# re-entry wall on the final binaries: visit 1 clean, visit 2 dies N64 0 in 800988A0
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64

# the attract loop never enters 0x02/0x0D and never crashes
OGRE_SPEED=8 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=170000 ./build-null/ogrebattle64

# the title's other entries: 0x12 dies on a near-NULL (forced pre-state),
# 0x17 names the two uncompiled records it needs and then idles
OGRE_SCENE=0x12 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=40000 ./build-null/ogrebattle64
OGRE_SCENE=0x17 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=40000 ./build-null/ogrebattle64

# the command table (ROM), and the jtbl the recompiler expands
#   jtbl_ovlC_8022ABA0 @ RAM 0x8022ABA0 = ROM 0x281830+(0x8022ABA0-0x802258B0) = 0x286B20
#   descriptor table  @ asset 0x19A8804 -> ROM 0x1F3CA54; entries 1..15 -> ROM 0x1F3Exxxx
python3 - <<'EOF'
import struct
d=open('assets/ogre64.z64','rb').read()
j=0x281830+(0x8022ABA0-0x802258B0)
print([hex(struct.unpack_from('>I',d,j+4*i)[0]) for i in range(9)])
t=0x19A8804+0x594250
print([hex(struct.unpack_from('>I',d,t+4*i)[0]) for i in range(8)])
EOF
```
