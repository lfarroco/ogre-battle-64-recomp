# Handoff — 2026-09-25, session 103: the EXP Overflow mod

**Goal.** Implement the developer's requested "exp overflow" mod: when a
character's EXP crosses 100 in a battle result they level up, and the leftover
EXP (`total - 100`) should carry over instead of being zeroed.

**Result.** `mods/exp-overflow/` is a working code mod. A character at 89 EXP
who gains enough to cross 100 reaches level +1 and ends the result at **20 EXP**,
and the 20 survives the next battle. The same fight without the mod ends at 0.
The manifest ships it off by default; the player turns it on in the start
screen's MODS panel.

## 1. The game's behaviour, at instruction level

The battle-result state machine is `func_ovlJ_801D61C0` (streamed **bank unit
J**, record 10's arena). Its state 4, the level-up loop, is at `0x801D634C`:

```
801d6398: lbu   v0,50(s0)      ; participant +0x32 = working EXP
801d639c: sltiu v0,v0,100      ; EXP >= 100 -> level up
801d63a0: bnezl v0, next
801d63ac: jal   801d8e90       ; func_ovlJ_801D8E90: participant -> record
801d63b0: sb    zero,50(s0)    ; DELAY SLOT: participant EXP = 0
801d63b4: lbu   v0,246(s0)     ; roster index
801d63c8: jal   8016ebc4       ; func_8016EBC4: level the record up
801d63d0: jal   801d8dc4       ; func_ovlJ_801D8DC4: record -> participant
```

The sync routines fix the field identities: `func_ovlJ_801D8E90` copies
`participant+0x32 -> record+0x35` (EXP) and `participant+0x31 -> record+0x13`
(level); `func_ovlJ_801D8DC4` copies the other way. The record table starts at
`0x80193BE0`, stride `0x38`; record 1 is the first roster character, whose name
is at `0x80193C18`.

The participant array is `*(0x801CE8DC)` (the pointer expression is
`lui a1,0x801d; lw a1,-5924(a1)` in `func_ovlC_801C9008`; the address is
`0x801CE8DC`, **not** `0x801DE8DC` — sign-extending `0xE8DC` subtracts 5924).
Participant `n` is at `+n*248 + 452` and is live when the word at `+n*248 + 524`
is nonzero; its level is `+0x31`, EXP `+0x32`, roster index `+0xF6`.

Measured on the developer's attached suspend save (`scene_4_suspend.bin`), with
per-2 s RDRAM snapshots (`/tmp/ov3-*.bin`):

| snapshot | Dio | Montana / Redford / Cheyenne / Dillan |
|---|---|---|
| `ov3-68` | level 5, 50 EXP | level 4, **89 EXP** |
| `ov3-72` | level 5, 50 EXP | level **5**, **20 EXP** |
| `ov3-73` | level 5, 75 EXP | level 5, **20 EXP** (held) |

Without the mod the same fight ends the four at level 5, 0 EXP (the developer's
reported bug, and `/tmp/lv-exit.bin`).

## 2. Why the fix is not a hook on the result code

**Streamed bank hooks do not resolve.** The mod system looks a hook up through
`recomp::overlays::get_vrom_to_section_map()`, which the port fills from the
base ELF's sections (`.entry`, `.main`, `.streamedA/B/C`). The bank units in
`app/src/bank_overlays.cpp` are a separate dispatch (`load_function_bank`), and
the guide's "What a hook can reach" already says so. `func_ovlJ_801D61C0` is in
a bank, so it cannot be hooked.

**`func_8016EBC4` is in `.streamedB` but is unhookable.** Hooking it at entry or
at return loads and then faults during boot with
`Failed to find function at 0x46220003` (SIGSEGV / SIGBUS in `func_80071EB0`'s
main loop). Its neighbours hook cleanly — `func_8016E1FC`, `func_8016DE3C`, and
the example mod's `func_80177DCC` — so the fault is specific to that target, not
to streamed sections. This is reproducible with an empty hook body, so it is the
regeneration of the function, not the mod's code.

## 3. What landed

`mods/exp-overflow/src/exp_overflow.c` — one hook,
`RECOMP_HOOK("func_80072944")`, which runs once per frame (measured: 3072
entries over a 3203-frame run). Each tick:

1. **Remember.** If `*(0x801CE8DC)` is a valid RDRAM pointer, scan the 20
   participants; for any with `+0x32 >= 100`, store `+0x32 - 100` and the
   participant level against the roster index `+0xF6`.
2. **Restore.** For each remembered index, once `record+0x13 == level + 1`,
   write the remainder into `record+0x35` **and** re-find the participant with
   that roster index and write `participant+0x32`. Both are held for
   `TICK_WINDOW` (120) ticks.

The participant write is load-bearing. The first version wrote only the record:
the record read 20 for one snapshot (`ov2-30`) and was back to 0 by the next
(`ov2-31`), because a later participant → record sync put the zero back. Writing
the participant copy as well makes every later sync agree, and 20 survived the
following battle in the verified run.

The HUD/status display and the next battle both read the character record, so
the record write is what the player sees.

## 4. Hook targets tried and rejected

| target | result |
|---|---|
| `func_80075BC0` | called **once** (it is the scene-table setup, not a frame function); its regeneration also fails with `Failed to find function at 0x8007DA14` |
| `func_80089BC0`, `func_80072900`, `func_800728BC` | load cleanly but are boot-only (~13 entries in 25 s) |
| `func_80072398` | per frame (~2953 entries / 20 s) but the hook faults with SIGBUS |
| `func_8016EBC4` (entry and return) | faults during boot, see §2 |
| `func_80072944` | **used**: works, once per frame |

## 5. Verification

* Attached `scene_4_suspend.bin`, loaded through `OGRE_SAVE=/tmp/levelup.bin
  OGRE_SAVE_RESET=1`, played by hand (no autopresses) through the launcher,
  Load Game and the mission; per-2 s `dump` snapshots via the live console's
  watched file.
* Result: the four front-row soldiers of Dio's unit (roster indices 15, 12, 13,
  14) at 89 EXP crossed 100, reached level 5 and ended at 20 EXP; 20 survived
  the next battle (`/tmp/ov3-68/72/73.bin`).
* Instrumented build (scratch counters at `0x80600000..`): `captures=232`,
  `record writes=4`, `participant writes=4`, last remainder `20`
  (`/tmp/ov3-exit.bin`). The instrumentation is removed and the shipped source
  has no debug writes (`grep DBG mods/exp-overflow/src/exp_overflow.c` is
  empty).
* `functions.build-example-mods.sh mods/exp-overflow` builds
  `build/mods/exp-overflow.nrm`; the mod loads with no recompile error and the
  game boots (`/tmp/modfinal.log`, exit 0).
* The generated MIPS contains no `teq`/`break`/`divu`: the mod recompiler
  refuses the `teq` that GCC emits for a constant `divu` guard, so the record is
  matched by pointer rather than by `(record - base) / 0x38`.

## 6. Files changed

* `mods/exp-overflow/mod.toml`, `mod.ld`, `include/modding.h`,
  `src/exp_overflow.c`, `README.md` — new.
* `docs/DECISIONS.md` — the session-103 entry.
* `docs/README.md`, `PLAN.md` — index and status.
* `mods/exp-overflow/build/` and `build/mods/exp-overflow.nrm` are generated and
  gitignored.

## 7. Probes and scratch

* No probe touches `RecompiledFuncs/`, `Bank*Funcs/` or `app/`.
* Temporary scratch mod `mods/zztest/` (hook-target experiments),
  `build/mods/zztest.nrm` and `build-app/mods/zztest.nrm` are deleted.
* Temporary debug counters in `exp_overflow.c` are removed.

## 8. Open

* The mod is only verified against one save and one battle. A wider check would
  be a second level-up with a different remainder and a level-up inside a
  multi-character unit (the verified run already had four at once).
* The port's mod system still cannot hook bank functions. The clean fix for
  this mod (a hook on the result loop itself) needs the bank sections registered
  with `get_vrom_to_section_map()`, which is an app/runtime change and was not
  attempted here.
* `func_8016EBC4` remains unhookable. Whether that is an N64Recomp regeneration
  bug or a property of that function is not settled; the failure is recorded
  here so the next session does not re-derive it.
