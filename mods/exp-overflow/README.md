# EXP Overflow

An Ogre Battle 64: Recomp code mod. It keeps the leftover EXP when a character
levels up.

## What it does

A character levels up at 100 EXP. The game's battle-result code adds the EXP a
fight awarded to the battle participant, and when the total is 100 or more it
zeroes the participant's EXP and levels the character up. The remainder is
thrown away:

| before | gain | the game | with this mod |
|---|---|---|---|
| level 4, 93 EXP | +27 | level 5, **0 EXP** | level 5, **20 EXP** |

Level and stats are untouched. Only the EXP left over from the level-up changes.

## Why it is not a one-line hook

The battle-result loop is `func_ovlJ_801D61C0` (0x801D6398: `exp >= 100` →
zero the participant's EXP → level up). It lives in **streamed bank unit J**
(record 10's arena). The port's mod system resolves a hook through
`recomp::overlays::get_vrom_to_section_map()`, which is built from the base
ELF's sections (`.entry`, `.main`, `.streamedA/B/C`) and not from
`app/src/bank_overlays.cpp`'s banks, so a hook there cannot load
(`docs/guides/app-build.md` → "What a hook can reach").

The level-up routine `func_8016EBC4` is in `.streamedB` and *looks* hookable,
but hooking it (entry or return) makes the port call a garbage function pointer
during boot (`Failed to find function at 0x46220003`, then SIGSEGV). Its
neighbours `func_8016E1FC` and `func_8016DE3C`, and the example mod's
`func_80177DCC`, all hook fine, so that target is the problem, not streamed
sections.

## How it works

One frame hook, `func_80072944` (once per frame, measured 3072 entries over a
3203-frame run), does two things each tick:

1. **Remember.** While a battle result is live (`*(0x801CE8DC)` is a valid RDRAM
   pointer, and the participants are the ones the result loop reads), any
   participant whose working EXP is 100 or more is remembered as
   `exp - 100`, keyed by its roster index (`participant + 0xF6`).
2. **Restore.** The result loop zeroes the participant's EXP, syncs it into the
   character record (`func_ovlJ_801D8E90`), levels the record up
   (`func_8016EBC4`), then syncs the record back
   (`func_ovlJ_801D8DC4`). Once the record shows `level + 1`, the mod writes the
   remainder into the record **and** into the still-live participant. The
   participant write matters: a record-only write is undone the next time the
   game syncs participant → record. Both copies are held for
   `TICK_WINDOW` (120) frames so a late sync cannot put the zero back.

The addresses, read at instruction level:

| what | where |
|---|---|
| character record `n` | `0x80193BE0 + n*0x38` (record 1 is the first roster character, name at 0x80193C18) |
| record level / EXP | `+0x13` / `+0x35` (bytes) |
| battle result state | `*(0x801CE8DC)` (bank unit J's data; `lui a1,0x801d; lw a1,-5924(a1)` in `func_ovlC_801C9008`) |
| result state word | `+0x6048` |
| participant `n` | `+n*248 + 452`, live when the word at `+n*248 + 524` is nonzero |
| participant level / EXP / roster index | `+0x31` / `+0x32` / `+0xF6` (bytes) |

## Building

```sh
make mod-syms        # writes mods/reference/dump.toml (needs the linked ELF)
make example-mods    # writes build/mods/exp-overflow.nrm
```

`make example-mods` uses a `mips-linux-gnu-gcc` on `PATH` when there is one and
falls back to `gcc-mips-linux-gnu` in a `debian:bookworm-slim` Docker
container. `tools/build-example-mods.sh mods/exp-overflow` builds just this mod.

## Installing

Copy `exp-overflow.nrm` into the client's `mods/` folder (next to the
executable, alongside `saves/`). The client lists it in the start screen's
**MODS** panel, where it can be turned on. The manifest ships it off by default,
so the package plays the vanilla game until the player opts in.

## Verification

Played against the attached suspend save (`Dio`'s unit, four front-row soldiers
at 89 EXP): with the mod they crossed 100, reached level +1 and ended at
**20 EXP**, and 20 survived the following battle (snapshots and a debug counter
confirm four record writes and four participant writes with remainder 20). The
same fight without the mod ends them at 0.

## Reading the code

* `src/exp_overflow.c` — the frame hook and the fix-up, with the rejected hook
  targets recorded.
* `include/modding.h` — the section macros (`RECOMP_HOOK`, `RECOMP_PATCH`, …).
* `mod.toml` — the manifest.
