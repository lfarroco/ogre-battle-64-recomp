---
title: Streamed banks and their units
summary: Every streamed module's ROM range, RAM base, code/data split and the bank unit that compiles it.
status: verified
session: 82
order: 40
---

# Streamed banks and their units

A streamed bank is a module the game DMAs into a RAM window at runtime. Several
banks share one window, so the resident module decides which code runs at those
addresses. Bank code compiles into `Bank{A..Z,AA..AH}Funcs/` (34 units) and is
registered in `app/src/bank_overlays.cpp`.

## The loader

Every bank is DMA'd by one generated loader block. Its own instructions carry
the game's boundaries:

```asm
lui a0,%hi(rom_start); addiu a0,a0,%lo(rom_start)
lui a1,%hi(ram_base);  addiu a1,a1,%lo(ram_base)
lui a2,%hi(rom_end);   addiu a2,a2,%lo(rom_end)
jal func_8009DA50
subu a2,a2,a0                     ; size = rom_end - rom_start
```

The block is bracketed by `func_800900C0(ram_base, code_size)` (icache),
`func_80090010(code_end, data_size)` (dcache) and
`func_80093380(data_end, bss_size)` (bss). A bank's size is its `subu`, not a
chunk count. The code/data split is the cache bracket's, not a guess.

## Table

| arena (RAM) | ROM start | size | code | data | BSS | unit | what |
|---|---|---|---|---|---|---|---|
| `0x800E9C20` | `0x0003F1B0` | `0x1CD0` | `0x1490` | `0x840` | shared | main (streamedA) | boot overlay (ROM-only) |
| `0x8016AF80` | `0x00040E80` | `0x25FB0` | `0x1B3B0` | `0xAC00` | shared | main (streamedB) | boot overlay: scene registry, dialogue text (ROM-only) |
| `0x8019A7C0` | `0x000712A0` | `0x84B0` | `0x7120` | `0x1390` | - | H | scene `0x07` name/birthday form |
| `0x8019A7C0` | `0x00079750` | `0xDAD0` | `0xC0F0` | `0x19E0` | - | M | **scene `0x05`, the map** |
| `0x8019A7C0` | `0x00087220` | `0x56D60` | `0x53920` | `0x3440` | - | S | **scene `0x06`, the Organize Screen** |
| `0x801D0860` | `0x00213AE0` | `0x16770` | `0x15260` | `0x1510` | - | J | record 10's arena, non-`0x16` scenes |
| `0x801D0860` | `0x0023A370` | `0xE80` | `0xE70` | `0x10` | - | AH | **the battle/neutral-encounter setup fragment** |
| `0x801D0860` | `0x00244770` | `0x7500` | `0x6C70` | `0x890` | - | I | **scene `0x16`, the closing movie** |
| `0x801E6FD0` | `0x0022A250` | `0x10120` | `0xF920` | `0x800` | - | X | **combat / battle** |
| `0x801E6FD0` | `0x0023B1F0` | `0x9580` | `0x8D00` | `0x880` | - | C | record 10's main bank |
| `0x80214FA0` | `0x00165FE0` | `0xBEE0` | `0x9BD0` | `0x2310` | - | Q | mission bank (suspend route) |
| `0x80214FA0` | `0x00171EC0` | `0x6030` | `0x4090` | `0x1FA0` | - | P | mission bank |
| `0x80214FA0` | `0x00177EF0` | `0x7AF0` | `0x3E00` | `0x3CF0` | - | Y | not yet identified |
| `0x80214FA0` | `0x0017F9E0` | `0x91A0` | `0x2930` | `0x6870` | - | W | **combat** |
| `0x80214FA0` | `0x00188B80` | `0x65A0` | `0x64F0` | `0xB0` | - | Z | not yet identified |
| `0x80214FA0` | `0x0018F120` | `0x6310` | `0x61F0` | `0x120` | - | AA | not yet identified |
| `0x80214FA0` | `0x00195430` | `0x2380` | `0x22F0` | `0x90` | `0x20` | R | enemy/unit constructor (natural route) |
| `0x80214FA0` | `0x001977B0` | `0x4F80` | `0x4810` | `0x770` | - | AB | **shop** |
| `0x80214FA0` | `0x0019C730` | `0x64C0` | `0x6410` | `0xB0` | - | AC | not yet identified |
| `0x80214FA0` | `0x001A2BF0` | `0x1FF0` | `0x1860` | `0x790` | `0x40` | AD | not yet identified |
| `0x80214FA0` | `0x001A4BE0` | `0x4680` | `0x4320` | `0x360` | - | V | **settings** |
| `0x80214FA0` | `0x001A9260` | `0x93E0` | `0x8FE0` | `0x400` | - | AE | not yet identified |
| `0x80214FA0` | `0x001B2640` | `0x79E0` | `0x72B0` | `0x730` | `0x20` | AF | not yet identified |
| `0x802210E0` | `0x00275820` | `0x47D0` | - | - | - | C (bankRec13) | scene `0x14`'s arena, low half |
| `0x802210E0` | `0x00279FF0` | `0x7840` | - | - | - | **AG (bankRec16)** | **scene `0x14`'s own module, not in the segment table** |
| `0x8022ACB0` | `0x00286BA0` | `0x138F0` | `0x13180` | `0x770` | - | K | chapter animation (the "Prologue" card) |
| `0x8022ACB0` | `0x0029A490` | `0xE860` | `0xDE00` | `0xA60` | - | L | the same arena's other bank |
| `0x802395E0` | `0x002A8CF0` | `0x56A0` | `0x5620` | `0x80` | - | G | step >= 2's dialogue engine bank |
| `0x802395E0` | `0x002AE390` | `0xA7E0` | `0xA600` | `0x1E0` | - | F | the same arena's other bank |

Record 13 and record 16 are listed without a split because neither has a loader
`subu`. The game loads them from a descriptor table, so their sizes come from
the segment table's `ram_end`. Record 16's `0x7840` is what the port's own
`UNCOMPILED` log line printed.

"shared" means the boot loader zeroes the boot overlays' BSS with its own
`func_80093380` calls, which span more than one record and start at the end of
the last one's data, so no single record owns that range. The game's per-record
BSS, as `tools/gen_bank_funcs.py`'s `RAM_END` records it, is what the port
reproduces.

There is no uncompiled bank on a reachable path. `arenamap.py` finds 28 bank
loads across the ELFs; 27 are compiled and the 28th is the `0xE80` fragment
below. `--coverage` proves the negative for loader-pattern banks: the one
remaining uncovered ROM gap (`0x0DDF80..0x0E4910`, a display-list and texture
blob) contains no `jal 0x8009DA50`.

## Record 16

Booting scene `0x14` printed:

```
[bank] UNCOMPILED streamed record 16: rom=0x279FF0 ram=0x802258B0 size=0x7840
[overlays] streamed function stub called @ 0x80226B7C (not yet loaded)   ... x3
```

Record 16 is not a segment-table record. Entry 13 declares
`rom 0x275820..0x279FF0 -> ram 0x802210E0`, which is unit C; the game then loads
a second module over the same arena from `0x279FF0`, at `0x802258B0`. The size
is the segment table's `ram_end` for entry 13 (`0x8022D0F0`) minus `0x802258B0`.

The window matters beyond its three stubs: 125 distinct call targets across
eight units reach into it (banks B, C, F, G, K, L, N, T and the main ELF),
because three other arenas overlap it. It is bank unit AG (`bankRec16b`). With
unit AG in, scene `0x14` renders with 0 stubs.

## The 0xE80 fragment

Scene `0x0D`'s enter `func_80178568` and scene `0x0E`'s enter `func_80177754`
both stream the same fragment, `rom 0x0023A370` (`0xE80`) to RAM `0x801D0860`.
The game's boundaries are code `0xE70`, data `0x10`, no BSS.

It is its own module, not a data copy of record 10b: unit J's `bankRec10a` ends
at `0x23A250`, unit X's battle bank starts at `0x23A370`, and unit C's
`bankRec10b` starts at `0x23B1F0`. It builds the battle's participant table:
`func_801D1508` scans the 30 unit slots at `0x801F0CD0` and tags a free one, and
`func_801D0AAC` reads the 16-byte table at `0x801D16D0`.

Without it, normal battles still work because their units already exist on the
field. A neutral encounter's wild unit is created here, so the battle starts
with an empty enemy side and an instant victory. It is bank unit AH.

## Table-driven loads

Record 18's bank (unit T, the tutorial practice stage) and record 16 load
without a literal loader block. `func_ovlB_80221D50` indexes a `0x28`-byte
descriptor table at RAM `0x80229DDC` by `D_80193700` and reads `ram`,
`rom_start`, `rom_end`, `bss` and `entry` from the entry. The addresses are
data, not immediates. `arenamap.py` reports those as `jal 0x8009DA50` sites
without a cache bracket. The tutorial's other table at `0x80229E88` selects
mode 2 (unit U).

## Overlap and unit assignment

Do not compile a swappable RAM range into the unit whose code calls into it. If
two records occupy the same RAM, put both in other units. N64Recomp then emits
`LOOKUP_FUNC(addr)` for the call sites, and the runtime's DMA-driven bank map
picks the resident module. If a caller's unit also defines the range, the call
binds at build time to one bank's bodies, and the other bank's callers run them
with the wrong frame and register contract.

`python3 tools/cross_bank.py check-banks` fails when a unit defines a RAM range
another bank can own and calls into it from another of its records.
`make cross-bank-check` is the full audit.

An unknown record is never evicted, so the previous bank's body keeps running at
those addresses. A frozen screen with no stub in the log can be an
`UNKNOWN module` line.

## Addresses

<!-- BEGIN GENERATED: addresses:banks -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x8009DA50` | `bank_dma` |  | function | The DMA that the generated loader block calls; a2 is rom_end - rom_start, so that difference is the size. | 73 | high |
| `0x800900C0` | `icache_invalidate_range` |  | function | Loader bracket for the code range, before the DMA. | 73 | high |
| `0x80090010` | `dcache_writeback_range` |  | function | Loader bracket for the data range, before the DMA. | 73 | high |
| `0x80229DDC` | `bank_descriptor_table` | 0x28-byte entries | table | func_ovlB_80221D50 indexes it by D_80193700 and reads ram, rom_start, rom_end, bss and entry. Records 16 and 18 load without a literal loader block. | 73 | medium |
| `0x80229E88` | `tutorial_mode_table` |  | table | Selects the tutorial mode-2 module (unit U). | 71 | medium |
<!-- END GENERATED: addresses:banks -->

## Evidence

`tools/arenamap.py` prints this table from the ELFs. `tools/guestmap.py` maps a
ROM or RAM address to its owning record, and `tools/rdram.py <dump> banks` says
which module is resident in each window. Sessions 59, 71, 72, 73, 82 and 88
supply the individual corrections.
