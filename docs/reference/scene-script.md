---
title: Scene script VM and the step table
summary: How the New Game sequence runs 1693 scripted steps through scenes 0x02 and 0x0D.
status: verified
session: 59
order: 30
---

# Scene script VM and the step table

## The flow

The New Game opening is not 200 hand-linked scenes. It is a scripted sequence
that visits scene `0x02` and scene `0x0D` once per step:

```
title (0x04) -> 0x02 -> 0x0D -> 0x02 -> 0x0D -> ...
```

`0x02` is a one-frame loader (`func_80178920`) that sets the next scene to
`0x0D`. `0x0D` runs the step, then sets the next scene from the script. The
attract loop is a different flow (`title <-> 0x0B / 0x0C`) and never enters
`0x02` or `0x0D`.

## The VM

`func_80170974` is the scene-script VM. It runs 16 script opcodes per visit.
Opcode `0x10` stores its first operand into `D_8018F1C0` and the next scene into
`D_8018F1C2`. Opcode `0xFF` stores an operand into the Chaos Frame
(`0x801936C9`). The jump table for `0xFF` is at `0x80190BC8`.

The scene manager copies `D_8018F1C2` into `D_800C4C26` after the visit, so the
script chooses the next scene by writing one word. Scene descriptors are in
`docs/scenes.md`; `tools/scenemap.py` prints all 25.

## Scene 0x0D

`0x0D`'s descriptor is `0x8018FC3C`: enter `func_80178568`, update
`func_80178954`, hook `func_80178B40`, leave `func_80178B7C`, mask
`0x40007C14`.

The enter selects one of two engines on `D_8018F1C0`:

* `0` or bit 15 set: movie mode. It DMAs records 10a/10b and installs the
  cutscene vtable `&D_801E5AC0`.
* Non-zero: the cutscene engine the New Game opening uses. It DMAs 14a/14b and
  calls `func_80226FA8`, which calls `func_8022683C(-5, D_8018F1C0 & 0xFFF)`.

Session 42 called the second mode "command mode". Session 43's render shows it
drawing the New Game movie, so that label is wrong.

## The step table

The scripted step table is asset `0x19A8804`, at ROM `0x1F3CA54`. It is a `u32`
payload size `0x1A74`, then 1693 `u32` asset ids (1499 distinct). The index is
`D_8018F1C0 & 0xFFF`. A payload is the step's LZ-compressed descriptor.

`func_80227FF8` loads and decompresses a step descriptor into `0x8022A978` and
rewrites `0x088800NN` words with `u16` entries from the table at `0x80196F80`.
`func_802282D8` interprets the descriptor from `0x8022A970`.

## The step command

`func_ovlC_80227E64` reads the descriptor's last word. When its high byte is
`0xFF`, the low byte (1..8) indexes the table at `0x8022ABE0`, which maps it to
one of `-3 -6 -5 -4 -7 -8 -9 -10`. `func_ovlC_8022683C` then indexes
`0x8022ABA0` with `command + 10` to pick the setup arm, and that arm performs the
bank DMA.

The movie's last words (bytes 5, 1, 1, 5, 1) map to commands `-7 -3 -3 -7 -3`.
The chapter animation's step 1073 has last word `0xFF000006`, byte 6, so its
command is `-8`. Session 58's "command 6" was the raw byte.

## Decoded ranges

* Steps **1..19** are the opening's movie, cathedral dialogue and forms
  (`docs/HANDOFF-2026-09-15-session44.md` §1).
* Steps **970..974** are the closing movie's five phases. The first-word
  opcodes are `80000006`, and the phase count matches the shot count.
* Steps **1073..1077** are the "Prologue" chapter animation's five phases. The
  module switches on `(step - 1073) & 0xFFFF < 5`.

Step 1073 is a valid entry in the 1693-entry table. It is not an overrun. Its
`-8` arm streams ROM `0x286BA0` (`0x138F0`) to RAM `0x8022ACB0`, which is bank
unit K.

## Addresses

<!-- BEGIN GENERATED: addresses:script -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x80170974` | `scene_script_vm` |  | function | Runs 16 script opcodes per visit. Opcode 0x10 stores var[0] into D_8018F1C0 and the next scene into D_8018F1C2. | 42 | high |
| `0x80170ACC` | `vm_store_next_scene` |  | code | Opcode 0x10 store site: writes 0x8002 into D_8018F1C2. | 42 | high |
| `0x80170ADC` | `vm_store_step` |  | code | Opcode 0x10 store site: writes the step word D_8018F1C0. | 42 | high |
| `0x80190BC8` | `jump_table_opcode_ff` |  | table | Opcode 0xFF jump table. That opcode stores a script operand into the Chaos Frame 0x801936C9. | - | medium |
| `0x8022ABA0` | `step_setup_table` | indexed by command + 10 | table | Selects the setup arm and the bank DMA that arm performs. | 59 | high |
| `0x8022ABE0` | `step_command_table` | 8 entries | table | Maps the descriptor last word low byte 1..8 to commands -3 -6 -5 -4 -7 -8 -9 -10. | 59 | high |
| `0x19A8804` | `step_table_asset` |  | asset | Asset id whose payload is the scripted step table at ROM 0x1F3CA54. | 44 | high |
| `0x1F3CA54` | `step_table_rom` | payload 0x1A74 | rom | u32 payload size then 1693 u32 asset ids, 1499 distinct, indexed by step & 0xFFF. | 44 | high |
<!-- END GENERATED: addresses:script -->

## Evidence

`docs/HANDOFF-2026-09-15-session44.md` §1, `-session42.md`,
`-session59.md` §1 and §4c, and `docs/symbols.md` for the engine functions.
`tools/scenemap.py` prints the 25 scene types, every writer of the pending-scene
word, and the step table, from the ROM in about 3 s.
