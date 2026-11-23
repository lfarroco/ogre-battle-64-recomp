---
title: Scene dispatch and the 25 scene types
summary: The accessor table, the descriptor lifecycle, the pending-scene word and the scene-versus-step distinction.
status: verified
session: 213
order: 50
---

# Scene dispatch and the 25 scene types

`func_80075BC0` is the scene dispatcher. It runs from the game's system loop
and selects one of 25 scene types. The selected type's accessor returns a
five-word descriptor. The descriptor carries the functions that run the visit
and the bank-record mask the visit needs.

The descriptor table is in [`docs/scenes.md`](../scenes.md) → "Scene
descriptors". It lists every descriptor the tool resolves, plus the alternate
descriptors the game selects at runtime. This page covers the dispatch mechanism
around those descriptors.

## The accessor table

There are exactly 25 scene types, `0x00` through `0x18`. At boot
`func_80075BC0` writes 25 accessor pointers into `D_800AF028[0..24]` from an
immediate list. Each accessor is a 1-3 instruction stub that returns a
descriptor address. The descriptors are static ROM data in `streamedB`, at
ROM `0x40E80 + (vram - 0x8016AF80)`.

An entry in `D_800AF028` is an accessor, and the enter function is descriptor
word 0. The selector logic in an enter therefore runs once per visit. The
accessor for scene `0x02` returns `0x8018FC50`; the accessor for scene `0x0D`
returns `0x8018FC3C`.

The dispatcher indexes the table by scene id. The index is clamped: ids `0x1F`
and above map to index 0. The dispatcher records the running id in
`D_800E810E`, masked with `0x1F`.

The tool's output reproduces the hand-checked rows: id 5 (map) returns
`0x8018FD70`, id 7 (form) returns `0x8018FDAC`, and id 13 (`0x0D`) returns
`0x8018FC3C`.

`tools/scenemap.py scenes` resolves 20 accessors statically. Ids 2, 3, 6, 8
and 23 branch inside the accessor and print as `(indirect accessor)`. The tool
prints that label when it cannot resolve a descriptor address from the stub's
instruction window.

Some scenes select between two descriptors on a game-state word: scene `0x03`
and scene `0x06` use `D_80193700`, and scene `0x17` uses bit 3 of
`0x80196B0C`. The descriptor for each id, with its hooks and mask, is the table
in [`docs/scenes.md`](../scenes.md).

## The descriptor lifecycle

The accessor returns a pointer to five words:

| word | meaning | called from |
|---|---|---|
| `+0x00` | enter, once at scene start | `0x80075F58` (`jalr`) |
| `+0x04` | per-frame update | `0x8007265C` (`jalr`) |
| `+0x08` | second per-frame hook | `0x80072544` (`jalr`) |
| `+0x0C` | leave, at scene end | `0x8007602C` (`jalr`) |
| `+0x10` | bank-record mask | `func_800761E4`, `func_80076324` |

The dispatcher stores the accessor's return in `D_800E8294`. The frame function
`func_80072398` runs the words at `+0x04` and `+0x08` on every frame; the frame
pump is in [`docs/reference/runtime.md`](runtime.md) → "The frame pump". The
enter at `+0x00` runs once at scene start, and the leave at `+0x0C` runs at
scene end. The dispatcher registers `func_80072398` as the frame callback at
`0x80075F94`.

The enter and leave calls sit in the dispatcher, and the update and hook calls
sit in the frame function. The selector logic therefore runs once per visit.

The mask at `+0x10` is the set of streamed bank records the scene needs
resident. It is a bitmask: the scene-`0x05` map mask is `0x2`, and scene
`0x0D`'s mask is `0x40007C14`. Scene `0x03`'s mission descriptor carries mask
`0x38C`, which is records 2, 3, 7, 8 and 9.

Scene `0x0D`'s descriptor is `0x8018FC3C`: enter `func_80178568`, update
`func_80178954`, hook `func_80178B40`, leave `func_80178B7C`, mask
`0x40007C14`. Its enter selects one of two engines on `D_8018F1C0`, so the
engine choice is made once per visit.

## The pending-scene word

A scene change is a 16-bit request in `D_800C4C26`. The game's own transitions
store `0x8000 | id` there. The word also takes the control values `0xFFFC` and
`0xFFFE`; `tools/scenemap.py` classifies those as control words.

Every store into `D_800C4C26` in the game is 39 sites in 30 functions. 21 sites
store a statically-known value. The rest are script-driven: the scene-script VM
writes `D_8018F1C2` and the transition handler copies it. These 39 sites are the
complete scene-to-scene code.

The dispatcher loop reads the root id `D_800E8214` at its top (`lhu` at
`0x80075DA8`). Before each enter it stores `0xFFFF` into `D_800C4C26`
(`0x80075F54`). It then spins while the word still reads `0xFFFF`
(`0x80075FB8`). A word with bit 15 set is a request: the dispatcher clears the
bit (`andi 0x7FFF` at `0x800760C4`), writes the word back (`0x800760CC`),
stores the id into the scene block (`0x800760D8`) and jumps to the loop top
(`0x800760DC`).

Two script words select the next transition. `D_8018F1C0` is the New Game step
word, and `D_8018F1C2` is the next scene the VM requests. Opcode `0x10` writes
both at `0x80170ADC` and `0x80170ACC`. The title transition `func_80177A58`
copies `D_8018F1C2` into `D_800C4C26` at `0x80177AD0`.

After the script VM visit, the scene manager copies `D_8018F1C2` into
`D_800C4C26`. The script therefore chooses the next scene by writing one word.

`D_800E810E` is the id the dispatcher is running. `D_800E8294` holds the
descriptor pointer for that id.

## Scene versus step

The opening uses a scene visit and a scripted step as separate units. Scene
`0x02` is a one-frame loader: its descriptor is `0x8018FC50`, and its enter
`func_80178920` sets the next scene to `0x0D`. Scene `0x0D` runs one scripted
step per visit and then sets the next scene from the script. The New Game flow
is:

```
title (0x04) -> 0x02 -> 0x0D -> 0x02 -> 0x0D -> ...
```

The script VM `func_80170974` runs 16 opcodes per visit and maintains the step
index in `D_8018F1C0`. The attract loop is a different flow (`title` <->
`0x0B`/`0x0C`) and never enters `0x02` or `0x0D`.

The 25 scene types are reusable. The 1693-entry step table holds the game's
scripted content, and the 200+ dialogues are steps in that table. Decoding the
step format once covers every dialogue. The table's format, the opcode set and
the decoded step ranges are in
[`docs/reference/scene-script.md`](scene-script.md).

## The boot branch

The boot chooses its first scene with one test at `0x800721DC`. The test is a
`bnez` on the masked pad word. Its delay slot loads `0x18`, and the instruction
after the branch loads `0x09` when the branch is not taken. The test reads the
controller-0 report `D_800E79B0` and checks the Start bit `0x1000`.
`D_800E79B0` is filled by the boot's own poll (`func_8007297C`'s loop at
`0x80072A58`); a tap can miss that poll, so the menu is reached with a held
Start. A held Start selects scene `0x18`, the Controller Pak menu. Otherwise the
boot enters scene `0x09`, the boot intro. The chosen id is stored to
`D_800E8214` at `0x800721F0`.

Scene `0x18`'s descriptor is `0x8018FDC0`, mask `0x2`: enter `func_8017BA60`,
update `func_8017BB28`, hook `func_8017BB54`. Scene `0x09`'s descriptor is
`0x8018FB70`, mask `0x8000`.

## Forcing a scene with `OGRE_SCENE`

`OGRE_SCENE=<name|hex>` stores a scene request in `D_800C4C26` when the game
changes scene. The request uses the game's own form, `0x8000 | id`, so the
running scene's leave hook runs and the next scene's enter loads its banks. The
app writes the request only while a scene past the boot's `0x00` runs and the
word reads `0xFFFF`.

The request therefore lands at the first transition after the boot scene:
about 6.4 s at 1x and about 2.7 s at 8x. A forced scene enters without the
pre-state a natural path builds. The handoff records when a result came from a
forced entry. `OGRE_SCENE_TRACE=1` prints the request word and the root id, so
a request that misses the transition is visible.

The names in the app's table are `title` (`0x04`), `intro` (`0x09`),
`publishers` (`0x0A`), `story` (`0x0B`), `unit-info` (`0x0C`), `tutorial`
(`0x17`), `menu` (`0x18`) and `new-game` (`0x02`). A hex id works for a scene
with no name.

## Enumerating the scene graph

`tools/scenemap.py` prints all three layers from the ROM. The `scenes`
subcommand prints id, accessor, descriptor, enter, update, hook, leave and mask
for all 25 scene types. The `transitions` subcommand lists every store into
`D_800C4C26`, with the unit, address, function and value, and marks the computed
ones. The `steps` subcommand summarises the scripted step table. The tool reads
only the ROM and the build trees, and it finishes in about 3 s.

`tools/scenemap.py scenes --dump <rdram>` reads the descriptors from a live
RDRAM image instead of the ROM. That is the check for a descriptor the
dispatcher filled at runtime. `tools/guestmap.py <descriptor>` maps a descriptor
address to its ROM offset and names the owning segment.

`OGRE_SCENE_LOG=1` prints one `[scene]` line per change with the id, descriptor
and mask. New scene names come from running the attract loop and reading that
line. The live console's `c` command prints the same scene, descriptor and
mask.

## Addresses

<!-- BEGIN GENERATED: addresses:addresses -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x800AF028` | `g_scene_accessors` | 25 entries | table | AGENTS.md rule 3: the scene-id accessor table hardcoded in func_80075BC0; docs/scenes.md: 25 accessor stubs fill [0..24]. | - | high |
| `0x800721DC` | `boot_scene_branch` | held-Start bnez | code | Session 88: bnez at 0x800721DC tests D_800E79B0 bit 0x1000; delay slot sets 0x18, fall-through sets 0x09. | 88 | high |
| `0x80075DA8` | `scene_dispatch_read_root` | lhu | code | Session 213: lhu v0,-32236(v0) with v0=0x800F0000 reads D_800E8214 at the dispatcher loop top. | 213 | high |
| `0x80075F54` | `scene_dispatch_clear_request` | store 0xFFFF | code | Session 213: sh s1,19494(at) stores 0xFFFF into D_800C4C26 before each enter. | 213 | high |
| `0x80075FB8` | `scene_dispatch_wait_request` | spin | code | Session 213: lhu v0,19494(v0) / beq v0,s1 spins while the request word reads 0xFFFF. | 213 | high |
| `0x800760C4` | `scene_dispatch_clear_bit15` | andi 0x7FFF | code | Session 213: andi v0,v0,0x7fff clears bit 15 of a scene request before the id is stored. | 213 | high |
| `0x800760D8` | `scene_dispatch_store_id` | sh v0,4(v1) | code | Session 213: stores the requested id into the scene block; 0x800760DC jumps to the loop top. | 213 | high |
| `0x800760DC` | `scene_dispatch_reenter` | loop top | code | Session 213: jumps to the dispatcher loop top after the requested id is stored at 0x800760D8. | 213 | high |
| `0x80075F58` | `scene_dispatch_enter_call` | descriptor +0x00 | code | Session 42: jalr at 0x80075F58 calls descriptor +0x00, the enter that runs once at scene start. | 42 | high |
| `0x8007265C` | `scene_dispatch_update_call` | descriptor +0x04 | code | Session 42: jalr at 0x8007265C calls descriptor +0x04, the per-frame update. | 42 | high |
| `0x80072544` | `scene_dispatch_hook_call` | descriptor +0x08 | code | Session 42: jalr at 0x80072544 calls descriptor +0x08, the second per-frame hook. | 42 | high |
| `0x8007602C` | `scene_dispatch_leave_call` | descriptor +0x0C | code | Session 42: jalr at 0x8007602C calls descriptor +0x0C, the leave at scene end. | 42 | high |
| `0x800E8214` | `g_root_scene_id` | root id | global | Session 213: read at the dispatcher loop top (0x80075DA8); session 88: boot stores 0x18 or 0x09 here at 0x800721F0. | 213 | high |
<!-- END GENERATED: addresses:addresses -->

## Evidence

- `AGENTS.md` (rules 3, 4, 9 and 13) — the accessor table, the descriptor
  words and callers, the console's scene line, and the forcing request and
  timing.
- `docs/scenes.md` ("How the game is organised", "Scene descriptors", "Boot and
  attract") — the 25 types, the 39 stores, the full descriptor table and the
  boot branch.
- `docs/reference/runtime.md` ("The frame pump") — `func_80072398` calls the
  running descriptor's update and hook.
- `docs/reference/scene-script.md` — the VM, the two script words and the step
  table in full.
- `docs/reference/memory.md` ("Words that select a screen") — the pending word,
  the active id, the descriptor word and the descriptor selection.
- `docs/HANDOFF-2026-11-02-session213.md` — the dispatcher instruction
  addresses, the `0xFFFF` handshake and the `OGRE_SCENE` request.
- `docs/HANDOFF-2026-09-19-session88.md` — the boot-Start branch at
  `0x800721DC` and scene `0x18`.
- `docs/HANDOFF-2026-09-16-session60.md` — the frame callback registration and
  the frame function's descriptor calls.
- `docs/HANDOFF-2026-09-15-session44.md` — the scripted step table and the
  `0x02`/`0x0D` sequence.
- `docs/HANDOFF-2026-09-14-session42.md` — the descriptor call sites, the
  enter/update/hook/leave mapping and `D_800E8294`.
- `docs/HANDOFF-2026-09-14-session36.md` — the scene block copy and the
  `D_800E810E` id.
- `docs/HANDOFF-2026-08-25-session8.md` — the dispatcher's system loop and its
  spin on the request word.
- `docs/reference/addresses.tsv` — the registered rows for the script words and
  the engine functions.
- `tools/scenemap.py` — the three subcommands and `--dump`.
