---
title: Guest memory map
summary: RDRAM layout, framebuffers, heap state pointers and the words that select a screen.
status: verified
session: 207
order: 10
---

# Guest memory map

The guest is an 8 MiB RDRAM machine. A logical address in the ROM's code is
KSEG0: `0x80000000 + physical offset`. The port's `recomp_mem_addr` also accepts
KSSEG, KSEG3 and the KUSEG low window. The runtime bridges translate guest
pointers themselves, so a bridge and the macros must agree. The port
zero-fills RDRAM at boot; retail does not.

The recompiled code is the ROM's instructions, so a register or stack slot that
nothing wrote holds the value it holds on retail. A zero in the port where retail
clearly has a value is a lead to the address map and to the runtime bridges. The
bridges translate guest pointers themselves, so they must agree with
`recomp_mem_addr`. Check the bank map before concluding that a zero is retail
behaviour (session 45).

## Framebuffers

The game's own framebuffers are `0x000400`, `0x025C00` and `0x04B400`. They are
320x240 RGBA5551 with a row pitch of 640 bytes. The world map adds three
640-wide targets in the heap: `0x1BA780`, `0x250800` and `0x2E6840`.

The VI's scanout address is not the RDP's colour-image address. An `OSViMode`
field `origin` is `0x280` for the standard NTSC modes, so the VI scans out from
`framebuffer + 0x280`, one 640-byte row into the image, while the RDP renders at
`framebuffer`. Resolve a scanout buffer by containment, not equality.

A rendered 16-bit framebuffer written back to RDRAM stores the value `V` at
`off ^ 2` (low byte) and `off ^ 2 + 1` (high byte). `MEM_H` reads the halfword
at logical `off` as `rd16(p + (off ^ 2))`.

A depth target is keyed by address, width and height. Scene `0x0A` creates a
640x481 depth image at `0x2E6840`, and the title names three 320x476 colour
targets at the same address. An address-only key attaches the grown depth
buffer and the framebuffer is incomplete.

## Heap pointers

Several screens keep their state behind a pointer, and the pointer changes per
run. Read the pointer from its global.

* `state = *(0x80197B18)` is the map's state. `func_ovlM_8019A7C0` stores a
  `malloc(0x470)` there at `0x8019A82C`. `0x801F1570` in the session 60/63 notes
  is one run's value and not an address to hardcode.
* `state = *(0x801AB870)` is the mission's state. The mission mover
  `func_ovlN_801BE4E0` reads its cursor at `+0x08` and `+0x0C`.

## Words that select a screen

`D_800C4C26` is the pending scene (`0x8000 | id`, plus the `0xFFFC`/`0xFFFE`
control values). `func_80075BC0` looks the id up in `D_800AF028`, writes the
descriptor to `D_800E8294`, and runs it. `D_800E810E` is the scene id that is
actually running.

`D_8018F1C0` is the New Game step word and `D_8018F1C2` is the next scene the
script VM requests. The step engine's own state is at `0x8022A970` (pc),
`0x8022A978` (descriptor base) and `0x8022A994` (engine struct). The code
addresses these with `lui at,0x8023` plus a negative immediate, so the
addition wraps: `0x80230000 + 0xFFFFA994 = 0x8022A994`. Reading `0x8023A9xx`
reads the DMA'd arena image instead.

The shared scene module branches on `0x801977E8`: scene `0x05` sets `1`, scene
`0x03` sets `2` and scene `0x07` sets `3`. Scene `0x03` and scene `0x06` select
their descriptors on `0x80193700`, and the tutorial selects its descriptor on
bit 3 of `0x80196B0C`.

## The `lui` immediate is not the address

A global outside the main data segment is addressed with `lui` plus a signed
16-bit offset. When the low half has bit 15 set, the assembler's `%hi` adds one
to the base and the offset is negative, so the effective address is `0x10000`
below the `lui` immediate followed by the low half:

```
lui at,0x800b        # %hi(0x800a83b8) = 0x800b
lw  v0,-31816(at)    # %lo = 0xffff83b8 -> 0x800b0000 - 0x7c48 = 0x800a83b8
```

A record that names `0x800b83b8` is naming the immediate and the low half, not
the address. The save globals are the worked example: the cached image is
`0x800a83b8`, the dirty flag `0x800a83bc` and the signature `0x800a8240`
(`docs/reference/save.md`). The asset cache word is `0x800abd70`
(`docs/reference/assets.md`).

Check an address against the ELF before registering it:
`mips-linux-gnu-nm build/ogrebattle64.elf | grep 800a83b8`. The symbol table
holds the resolved form. `docs/HANDOFF-2026-09-15-session48.md` records the
same correction for the framebuffer table (`0x800a8204`).

## When the game stops drawing

`D_800AEFA4` counts guest frame-pump iterations and `D_800C4BCC` counts VI
retraces. A frozen `D_800AEFA4` with a rising `D_800C4BCC` means the frame-pump
thread stopped. The usual cause is a call that abandoned the caller's frame,
which makes the pump read its saved registers from the wrong stack slots.

## Addresses

<!-- BEGIN GENERATED: addresses:memory -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x80000000` | `g_rdram_kseg0` |  | window | KSEG0 base: guest 0x80000000 + physical offset addresses the 8 MiB RDRAM. The port zero-fills RDRAM at boot. | - | high |
| `0x000400` | `g_framebuffer_a` |  | framebuffer | 320x240 RGBA5551, pitch 640. VI scanout is fb + 0x280 for the standard NTSC modes; resolve a scanout buffer by containment. | - | high |
| `0x025C00` | `g_framebuffer_b` |  | framebuffer | Second 320x240 RGBA5551 target; the njpeg assembly draws into it at 320x476. | - | high |
| `0x04B400` | `g_framebuffer_c` |  | framebuffer | Third 320x240 RGBA5551 target; its range follows 0x025C00, which the per-row writeback guard must respect. | 207 | high |
| `0x1BA780` | `g_worldmap_color_a` | 640-wide | framebuffer | World map target in the heap; VI fbSizeX=640 fbSizeY=476 fbSiz=2. Spans 0x1BA780..0x1E05C0, and the allocator reuses that range for the scene-0x02 njpeg buffers. | 202 | high |
| `0x250800` | `g_worldmap_color_b` | 640-wide | framebuffer | Second world-map target; the same 640x476 VI mode as 0x1BA780. | - | high |
| `0x2E6840` | `g_worldmap_color_c` | also a depth image in scene 0x0A | framebuffer | Third world-map target. Scene 0x0A names it for three 320x476 colour targets and for a 640x481 depth image, so a depth target is keyed by address, width and height. | 186 | high |
| `0x800E79B0` | `g_pad_report_0` | u16, Start = 0x1000 | buffer | Controller-0 report filled by the boot poll. The boot branch at 0x800721DC tests Start here to choose scene 0x18 instead of 0x09. | 88 | high |
| `0x800AEFA4` | `g_frame_counter` |  | global | Guest frame-pump counter. Frozen while 0x800C4BCC keeps counting means the pump thread stopped. | 60 | high |
| `0x800C4BCC` | `g_vi_retrace_counter` |  | global | VI retrace count. Compare with 0x800AEFA4 when the game stops producing frames. | 60 | high |
| `0x80193700` | `g_scene03_mode` |  | global | Non-zero selects scene 0x03 descriptor 0x8018F364 and scene 0x06 descriptor 0x8018FD98. Record 18 loader func_ovlB_80221D50 indexes 0x80229DDC by it. | 67-71 | high |
| `0x80196B0C` | `g_tutorial_mode` | bit 3 | global | Scene 0x17 accessor func_801862F0 picks descriptor 0x8018FE50 or 0x8018FE64 on bit 3. | 71 | high |
| `0x801977E8` | `g_scene_module_mode` | 1 map, 2 mission, 3 form | global | Scene 0x05 enter sets 1, scene 0x03 enter sets 2, scene 0x07 enter sets 3. The shared module update and hook branch on it. | 60 | high |
| `0x801AB870` | `g_mission_state_ptr` | heap pointer | global | state = *(u32*)0x801AB870. The mission mover func_ovlN_801BE4E0 reads the cursor at +0x08 and +0x0C. Read the pointer each run. | 128 | high |
| `0x8019EE70` | `g_mission_buffer` | 0xC000 bytes, 320x240 | buffer | Scene 0x03 enter allocates the mission render buffer here. | 67 | high |
<!-- END GENERATED: addresses:memory -->

## Evidence

`AGENTS.md` ("Verified facts": uninitialised state, the VI scanout address, the
map screen, the frame-pump signature). The framebuffer addresses and the
writeback byte order are also used by `docs/guides/emulator-first.md` §5.
Sessions 60, 64, 88, 128, 183, 186, 202 and 207 supply the individual rows.
