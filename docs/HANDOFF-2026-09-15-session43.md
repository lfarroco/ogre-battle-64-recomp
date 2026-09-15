# Handoff — 2026-09-15, session 43: bank unit B (records 17/18) makes scene 0x17 the **Tutorial**, and the movie step the script never asks for

## Goal and result

Continue session 42's "what's next" list. This session finished item 3 (compile
bank records 17 and 18 so scene `0x17` can run), took item 1 (the movie-mode
wall) as far as instruction level, and answered item 2 (which `D_8018F1C0`
value the first `0x0D` visit should have) with a probe rather than a guess.

Result:

1. **Scene `0x17` runs.** Records 17 and 18 are compiled in a **new bank unit
   B** (`config-bankB.yaml` / `config-bankB.toml` / `BankBFuncs/`): the running
   port reports `[bank] loading overlay record rom=0x069920 … (21 functions)`
   and `rom=0x1BA020 … (125 functions)` when the scene enters, with **zero**
   `UNCOMPILED` reports, and the scene renders.
2. **Scene `0x17` is the Tutorial.** Capture:
   `docs/proofs/native-tutorial-dialogue.png` — a dialogue box over the dark
   blue textured backdrop, portrait of Deneb, text
   `Deneb` / `"Welcome!` / `Is this your first time here?`.
3. **The title menu is verified end-to-end.** Forcing nothing and letting the
   game run reaches the real menu (`docs/proofs/native-title-menu.png`):
   `New Game` (cursor), `Tutorial`, `Stereo` — exactly what the developer
   described. Driving **D-pad Down then Start** with `OGRE_TAP_BUTTON` enters
   scene `0x17` at `t=12389 ms`, i.e. the Tutorial entry. So
   `func_80177A58`'s selection state 3 → scene `0x17` = **Tutorial**, and
   state 2 → scene `0x12` = the save-only **Load Game** entry (see §2 for why
   the counter values shift when a save is absent).
4. **A load-bearing address in sessions 38/39/42 is wrong.** The word
   `func_ovlC_801B7EBC` reads is **`0x80197794`**, not `0x8019F794`. The
   instruction is literally `lui $v0, 0x8019` + `lw $v0, 0x7794($v0)`
   (`0x801B80B4`/`0x801B80B8`, `build/bankC/asm/1F0A00.s`). `0x7794 = 30612`,
   not `0xF794`. Every later section below uses the correct address.
5. **The script never asks for the movie step.** A probe over all four
   `D_8018F1C0` store sites in the scene-script VM `func_80170974` shows only
   ONE firing — `0x80170ADC`, which is **opcode `0x10`**
   (`jtbl_80190758[0x10-1] = 0x80170AC0`, read from ROM `0x66658`). Opcode
   `0x10` does `D_8018F1C0 = var[0]` **and** `D_8018F1C2 = 0x8002` (next scene
   = `0x02`) at `0x80170ACC`/`0x80170ADC`. It runs at script pc 16
   (`var[0] = 1`) and pc 32 (`var[0] = 2`), each **before** the `0x02`/`0x0D`
   visit it causes. The two stores that can write `0` (`0x80170B88`
   `& 0xBFFF`, `0x80170BC4` `sh $zero`) never execute.
6. **Developer oracle (recorded, this session):** the New Game opening starts
   with the *short intro movie* — i.e. the `F1C0 == 0` (movie) branch is the
   intended first `0x0D` visit. Combined with (5) that makes the wall sharp:
   the port enters `0x0D` in **command** mode on every visit because the
   script's own opcode 0x10 advances the step word to 1 *before* the first
   visit; nothing in the VM ever produces 0.

No game-logic repair was made (none is identified yet — see §5). The bank-unit
work is a real fix and is verified. All probes reverted by regen; both build
variants rebuilt; the runs below re-verified.

## 1. Bank unit B — records 17 and 18 (session 42's item 3)

Records 17 and 18 are RAM-disjoint from each other, so one new unit holds both:

| rec | ROM | RAM | size | ram_end (BSS end) | code/data split |
|---|---|---|---|---|---|
| 17 | `0x069920` | `0x80197B90` | `0x4D60` | `0x8019C950` (bss `0x60`) | code `..0x06DFA0`, data `..0x06E680` |
| 18 | `0x1BA020` | `0x80220F60` | `0x92B0` | `0x80230600` (bss `0x6400`) | code `..0x1C2D90`, data `..0x1C32D0` |

Both `ram_end`s come from the game's authoritative segment table at ROM
`0x387C0` (0x28-byte entries) and are now in `tools/gen_bank_funcs.py`'s
`RAM_END`. The code/data split follows the established convention: the code
range ends at the last `jr $ra` + its delay slot, rounded up to the next `0x10`
boundary (which swallows the same two zero padding words records 0/1 include).
Verified against `build/bankB/asm/*.s` after the split: rec17's last function
ends `0x8019C200 jr $ra` / `0x8019C204 addiu sp,sp,24`, rec18's
`0x80229CC8 jr $ra` / `0x80229CCC nop`.

Why each needs its own unit (they cannot join A/C/D/E):

- rec17 `0x80197B90..0x8019C950` overlaps records 0/1/2/15 (all at `0x80197B90`);
- rec18 `0x80220F60..0x80230600` overlaps unit C's records 12 (tail), 13 and 16.

Changes: `config-bankB.yaml`, `config-bankB.toml`, `Makefile`
(`BANK_UNITS := A B C D E`), `tools/gen_bank_funcs.py` (two `RAM_END` entries),
`app/src/bank_overlays.cpp` (records 17/18 `compiled = true`). Unit `B` was
unused, so no existing unit moved.

## 2. Scene `0x17` = Tutorial, and the title menu mapping

Forced scene runs (session 42 reported `0x17` entering, naming the two
uncompiled records, then idling with exit 0). With unit B armed:

```
OGRE_SCENE=0x17 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=40000 ./build-null/ogrebattle64
[scene] t=1316ms id=0x0017 descriptor=0x8018FE50 mask=0x00060000
[bank] loading overlay record rom=0x069920 ram=0x80197B90 size=0x4D60 (21 functions)
[bank] loading overlay record rom=0x1BA020 ram=0x80220F60 size=0x92B0 (125 functions)
… exit 0, no UNCOMPILED, no crash
```

The capture (`docs/proofs/native-tutorial-dialogue.png`) is the tutorial
greeting. Driving the real title menu confirms the routing:

```
OGRE_SPEED=4 OGRE_TAP_MS=1500 \
OGRE_TAP_BUTTON="start,start,start,start,start,start,start,down,start,start" \
OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=16000 ./build-app/ogrebattle64
[SDL] automation tap 6 at 9000ms buttons=start        ; title menu is up
[SDL] automation tap 7 at 10500ms buttons=down        ; cursor -> Tutorial
[SDL] automation tap 8 at 12000ms buttons=start       ; confirm
[scene] t=12389ms id=0x0017 descriptor=0x8018FE50 mask=0x00060000
```

### Where the selection counter comes from (the developer's follow-up question)

`func_80177A58` (title descriptor word 1) switches on `func_8019BD0C()`:
`1 → D_800C4C26 = D_8018F1C2` (New Game), `2 → 0x8012`, `3 → 0x8017`,
`4 → attract (0x0B/0x0C)`.

`D_801BA565` (the counter inside `func_8019BD0C`) is built in the title input
handler `func_8019B8CC` at `0x8019BB1C..0x8019BB50`:

```
lbu   $a0, 0x0($s1)          ; s1 -> the menu's highlight bitmask
v1 = first set bit index in 0..2         ; loop slti $v1, 3
D_801BA565 = v1 + 1
lh    $a0, 0x4($s0)          ; a0 = the menu's entry count word
if (a0 == 2 && D_801BA565 == 2) D_801BA565 = 3     ; ← the shift
```

The capture shows the three visible entries `New Game / Tutorial / Stereo`, and
the developer's answer says **Stereo is toggled with left/right, not a scene
link**, and that a save file adds a `Load Game` entry (making the order
`New Game / Load Game / Tutorial / Stereo`, Load Game default). That makes the
code self-consistent:

| bitmask bit | no save | with a save | raw counter | after the `a0==2` shift | `func_80177A58` state | scene |
|---|---|---|---|---|---|---|
| 0 | New Game | New Game | 1 | 1 | 1 | `0x02` (via `D_8018F1C2`) |
| 1 | Tutorial | Load Game | 2 | **3** (no save) / 2 (save) | 3 / 2 | `0x17` / `0x12` |
| 2 | — (Stereo is left/right, not in the bitmask) | Tutorial | 3 | 3 | 3 | `0x17` |

i.e. **scene `0x12` = Load Game** (reachable only with a Controller Pak save —
which is why session 36/42's *forced* `0x12` run dies on a near-NULL: a forced
entry never builds the save state), and **scene `0x17` = Tutorial** (verified
twice: forced, and via Down+Start).

## 3. The movie-mode flag is `0x80197794`, not `0x8019F794`

Sessions 38, 39 and 42 all cite `0x8019F794` as "the init flag `0x0D`'s
movie path reads". It is not. In `func_ovlC_801B7EBC` (bank unit C, record 10):

```
801B80B4  3C028019  lui   $v0, 0x8019
801B80B8  8C427794  lw    $v0, 0x7794($v0)     ; v0 = *(u32*)0x80197794
801B80BC  14400056  bnez  $v0, 0x801B8218      ; non-zero -> skip the load-now block
```

(`build/bankC/asm/1F0A00.s:11767-11768`; splat emits the label as
`D_ovlC_80197794`.) The address is `0x80197794`; only the *offset* `0x7794`
appears in the instruction. `0x8019F794` is an instruction address elsewhere
(`RecompiledFuncs/funcs_11.c:8354` is a comment for
`0x8019F794: addu $a3,$zero,$zero`), which is very likely how the wrong address
crept into the record.

Consumer/producer map for the corrected address (all in guest RAM, outside every
streamed record — it lives in the boot-resident main/overlay-B region):

| site | function | what it does |
|---|---|---|
| `0x801B80B4` | `func_ovlC_801B7EBC` (unit C, rec 10) | reads it; non-zero skips the asset load |
| `0x80198278`, `0x8019829C`, `0x801982C8` | `func_ovlE_801980A0` (unit E, rec 0) | writes a header word / a malloc'd pointer, then the result of `func_ovlE_80197B90` |
| `0x80198AB4`, `0x80198AD8`, `0x80198B04` | `func_ovlE_801988C8` (unit E, rec 0) | same shape, via `func_ovlE_801985C4` |
| `0x80234C58`, `0x80234DE8` | unit C, rec 14a arena | clears it to 0 |

It is a cached *hand-off pointer*, not a boolean "init flag": rec 0 (scene
`0x02`) writes it, rec 14a (scene `0x0D`) clears it, and rec 10 (the movie
path's code) consumes it. Any future probe must use `0x80197794`.

## 4. The `D_8018F1C0` writer, measured (session 42's item 2)

Session 42 probed only `0x80170ADC`. This session probed all four store sites in
`func_80170974` (`0x80170ADC`, `0x80170B00`, `0x80170B88`, `0x80170BC4`) and
also corrected two VM conventions that the previous notes state wrongly:

- the dispatch byte is **`sp+0x60`** (opcode) and **`sp+0x61`** is the operand
  — `lb $v1, 0x0($v0)` / `lb $a0, 0x1($v0)` at `0x801709D0/D4` store to
  `sp+0x60`/`sp+0x61`. Session 42's "the opcode byte" probe was reading the
  operand.
- `jtbl_80190758` is indexed by `opcode - 1` (`addiu $v1,$v0,-1` at
  `0x801709E4`); index 0 is unused, so **opcode 1** is the first handler.

Read straight from the ROM (`ROM 0x66658`, overlay B) index 15 → opcode `0x10`
→ `0x80170AC0`, which is the store handler. Measured:

```
[probe43] VM 0x80170ADC val=0x0001 F1C2=0x8002 pc=16 op=0x10 operand=0x00 base=0x801C0902
[probe43]   script@pc: 68B3 6901 7160 6801 1000 68B3 6900 7160 6800 6980 E104 6A00
[probe43] 0D-enter F1C0=0x0001 FC39=0x00 F1C2=0x8002
[probe43] VM 0x80170ADC val=0x0002 F1C2=0x8002 pc=32 op=0x10 operand=0x00 base=0x8019F4A2
[probe43] 0D-enter F1C0=0x0002 FC39=0x00 F1C2=0x8002
[crash] signal 11 on N64 thread 4
```

So:

- only `0x80170ADC` ever fires; `0x80170B00` (the `| 0x4000` variant, gated on
  `D_8018F318[opcode] == 0x10`), `0x80170B88` (`& 0xBFFF`) and `0x80170BC4`
  (`sh $zero`) never execute in this flow;
- the opcode-0x10 halfword is `0x1000` = (opcode `0x10`, operand `0x00`), so the
  operand really is 0 — the handler ignores it and always uses `var[0]`;
- the script is a **heap buffer** (`D_80197B38` / `D_80197B3C`, freed by
  `func_80170814`), and the two visits ran *different* buffers
  (`0x801C0902` visit 1, `0x8019F4A2` visit 2), chosen by `D_80197B23`;
- opcode 0x10 writes `D_8018F1C2 = 0x8002` at the same time, so this opcode is
  *the* New Game transition (`0x02` is the loader that sets next = `0x0D`).

`var[0]` is written by ordinary VM opcodes into `0x40($s1 + var*2)`
(`s1 = sp+0x10`); the three instructions that write it in this build are
`0x801711A4`, `0x80171594` and `0x801715D0`. Decoding the script's pc 0..15 to
see which one sets `var[0] = 1` — and whether the game intends that to happen
before or after the first `0x0D` visit — is the concrete next step (§5.1).

## 5. What's next (session 44)

1. **Decode the script's first 16 opcodes** (`func_80170974`, the heap buffer at
   `D_80197B38`/`D_80197B3C`). The question is not "what should the scene show"
   (the developer has answered: the intro movie first) but "which opcode should
   have set `var[0] = 0` for the movie step, and why does the port's script skip
   it". Cheap dump: at VM entry, when `D_80197B24 == 0`, log `a0` plus 24
   halfwords (the probe is in this session's log below) and decode with the
   `jtbl` table. Compare the two script buffers (`0x801C0902` vs `0x8019F4A2`) —
   they are different scripts, and knowing which one the New Game path should
   run is the likely root cause.
2. **The movie path itself** (if it is reached): after the address fix, re-run
   the force-movie-mode A/B (session 42 §6) and probe `0x80197794` at
   `func_ovlC_801B7EBC` and at the rec-0 writers. Session 42's decompressor
   crash may simply be a downstream symptom of the missed step.
3. **Command-mode step 2 / path B** (session 42 §3, unchanged and now
   lower priority if the movie is really step 0): `jal 0x802399AC` at
   `0x8022D218` enters `func_ovlC_80239874`'s continuation with `s0 = 0` and an
   unprimed `sp+0x1EC`. Concrete test: hardware watchpoint on
   `ultramodern::get_rdram_base() + 0xC2494`, or a temporary store check.
4. **Command-mode step 1 renders** — worth capturing now: in the port visit 1
   (command mode, selector 0) runs ~28.7 s healthy. Nothing has published what
   it draws. A capture of the natural (no extra taps) first `0x0D` visit would
   settle whether command mode is "the intro movie" after all, which would
   demote item 1. `OGRE_CAPTURE_PRESENT` + `OGRE_CAPTURE_EVERY` with only the
   Start taps needed to reach New Game.
5. Open and untouched: menu `0x18` natural entry (session-39 tail wall,
   `8017BB28 → 8019C69C`, N64 `0x14`), `OGRE_NO_AUDIO=1` early-boot crash,
   `osViFade`. Scene `0x12` needs a save-slot pre-state to test (Load Game).

## Verification (final binaries, all probes reverted via regen)

- `make recomp` clean; `grep -rl probe43 RecompiledFuncs/ Bank*Funcs/ app/` = 0;
  `build-null` + `build-app` rebuild clean.
- Unit B: `N64Recomp config-bankB.toml` clean; `gen_bank_funcs.py` reports
  5 unit(s), **17 record(s), 1867 function(s)** (was 15 records / 1721
  functions); `[bank] 17 streamed-overlay record(s), 1867 function(s) armed` at
  runtime.
- Forced scene `0x17` (40 s, `OGRE_SPEED=4`): records 17/18 load (21 + 125
  functions), no `UNCOMPILED`, exit 0.
- Title menu capture: `docs/proofs/native-title-menu.png` (New Game / Tutorial /
  Stereo, cursor on New Game).
- Tutorial capture: `docs/proofs/native-tutorial-dialogue.png` (Deneb).
- Title routing: `OGRE_TAP_BUTTON=…,down,start` → `[scene] t=12389ms id=0x0017`.
- New Game re-entry repro on the final binaries (60 s, `OGRE_SPEED=4`,
  `OGRE_TAP_MS=3000 OGRE_TAP_MAX=4`): `0x02 → 0x0D → 0x02 → 0x0D` then
  SIGSEGV on thread 4 — **unchanged** from session 42, as expected (no
  game-logic change was made).

No repo test harness exists; per the durable-test skill this is disclosed, not
fixed with a framework: each check is a ROM-dependent 12–170 s game run, so the
repro commands below are the maintained verification.

## Files changed (vs session 42)

- `config-bankB.yaml`, `config-bankB.toml` — new bank unit B (records 17, 18).
- `Makefile` — `BANK_UNITS := A B C D E`.
- `tools/gen_bank_funcs.py` — `RAM_END` entries for `0x069920` (`0x8019C950`)
  and `0x1BA020` (`0x80230600`).
- `app/src/bank_overlays.cpp` — records 17/18 `compiled = true`; `kScenes` gains
  `tutorial` = `0x0017` and the `title`/`menu` comments are corrected (the title
  screen itself carries the New Game / Tutorial / Stereo menu).
- `app/src/bank_funcs.inc` (generated, gitignored) — 17 records, 1867 functions.
- `PLAN.md`, `docs/DECISIONS.md`, `docs/README.md`, this file.
- `docs/proofs/native-title-menu.png`, `docs/proofs/native-tutorial-dialogue.png`.
- Temporary, all reverted by `make recomp`: `RecompiledFuncs/funcs_3.c`
  (a `probe43_store` helper + calls at `0x80170ADC`/`0x80170B00`/`0x80170B88`/
  `0x80170BC4`, and a script-buffer dump at `func_80170974` entry) and
  `RecompiledFuncs/funcs_6.c` (a `0x0D`-enter dump at `0x80178728`). Both files
  had `#include <stdio.h>` added for the probe.

## Repro commands

```sh
# bank unit B (records 17, 18) and the regenerated table
make build/bankB.elf
tools/N64Recomp/build/N64Recomp config-bankB.toml && python3 tools/gen_bank_funcs.py

# scene 0x17 = Tutorial: records 17/18 load, no stubs, exit 0
OGRE_SCENE=0x17 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=40000 ./build-null/ogrebattle64

# the same scene's visual (capture via RT64)
OGRE_SCENE=0x17 OGRE_SCENE_AFTER_MS=0 OGRE_SCENE_LOG=1 \
  OGRE_CAPTURE_PRESENT=/tmp/s17 OGRE_CAPTURE_AFTER=150 OGRE_CAPTURE_EVERY=60 \
  OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64

# the title menu + Tutorial entry, driven with no human
OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,start,start,start,start,start,start,down,start,start" \
  OGRE_SCENE_LOG=1 OGRE_CAPTURE_PRESENT=/tmp/tut OGRE_CAPTURE_AFTER=300 \
  OGRE_CAPTURE_EVERY=60 OGRE_EXIT_AFTER_MS=16000 ./build-app/ogrebattle64

# the New Game re-entry wall (unchanged)
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 \
  ./build-null/ogrebattle64

# the VM jump table (ROM), index 15 -> 0x80170AC0
python3 - <<'EOF'
import struct
d = open('assets/ogre64.z64','rb').read()
# jtbl_80190758: overlay B ROM 0x40E80 -> RAM 0x8016AF80
base = 0x40E80 + (0x80190758 - 0x8016AF80)
for i in (14, 15, 16):
    print("opcode 0x%02X -> 0x%08X" % (i + 1, struct.unpack_from('>I', d, base + 4*i)[0]))
EOF
```
