# Owned items: location in the battery save and the live RDRAM copy

Read-only research; no tracked file changed. Addresses are from
`build/ogrebattle64.elf`. Scratch decoder `/tmp/decode_save.py` is not in the repo.

## 0. Corrections to `docs/symbols.md` (verified in the ELF)

* The save globals are at **0x800A8xxx**, not 0x800B8xxx: `lui r,0x800b` plus a
  negative `addiu` wraps into 0x800A0000 (`lui v0,0x800b; lw v0,-31816(v0)` at
  0x80075078 is 0x800A83B8). `g_save_signature` = **0x800A8240** (ASCII
  `QuestOG3`); `g_save_field_table` = **0x800A824C**, 13 entries x 0x1C, ending
  exactly at `g_save_image` = **0x800A83B8** (`sw` init 0x8007525C);
  `g_save_dirty` = **0x800A83BC**. Field layout (`func_800749C0` 0x80074A68-
  0x80074AA0, `func_8007485C` 0x80074920-0x80074958, `func_80074F9C`
  0x800750C0-0x800750F8): `+0x00` fn A, `+0x04` fn load, `+0x08` fn save,
  `+0x0C` offset 1, `+0x10` fn load 2, `+0x14` fn save 2, `+0x18` offset 2; the
  field address is `slot_buffer + 0x0C + offset` (offset 2 adds 0x1850).
* `func_80093060` copies **a0 -> a1** (`lb v0,0(a0)` 0x800930BC, `sb v0,0(a1)`
  0x800930C4), i.e. `(src, dst, len)`, the reverse of libultra `memcpy`.
* `func_800742F4` (0x800742F4) unpacks a bit stream from `a0` into RAM
  (`g_bitindex` increments from 0, 0x800743B4/0x800743C0); `func_80074494`
  (0x80074494) is its inverse, reading RAM and writing the stream to `a0`
  (0x8007450C-0x80074524). Codec state: 0x800AEFB0 src, 0x800AEFB4 bit index,
  0x800AEFB8 bit position, 0x800AEFC0 bit buffer.

## 1. Slot geometry and region map (re-verified)

* Slot `n`: 6224 bytes (0x1850) at device `0x10 + n*0x1850`; slot 15 is the
  19176-byte (0x4AE8) record at 0x30B0. Only slots 0, 1, 15 exist.
* `slot+0x00` two u16 checksums over `slot+0x0C .. slot+0x1850`, each seeded with
  the slot's device offset (`func_8007541C`). mission1 slot 0 stores
  `C70A 1611`; the recomputed values with seed 0x10 are `C70A 1611`.
* `slot+0x04` `QuestOG3`. `slot+0x0C` u32 "slot in use" stamp: 0 = empty
  (`func_80074AD4` 0x80074B80 tests it; `func_8007485C` 0x800748F4-0x80074914
  increments it, 0 -> 0xFFFFFFFF). All three real slot-0 saves carry 0x20646175.

Mission1 slot 0 (file offset = slot offset + 0x10):

| slot offset | contents |
|---|---|
| 0x000..0x003 | checksums |
| 0x004..0x00B | `QuestOG3` |
| 0x00C..0x00F | stamp 0x20646175 |
| 0x010..0x029 | 26-byte slot summary (`func_80074AD4` returns `slot + 4 + 12`) |
| 0x02A..0x184D | packed field blob, 49433 bits (6180 bytes) |
| 0x184E | unused (blob ends mid-byte) |
| 0x184F | 1 byte, the only field of save-table entry 2 |

Slot summary (`func_8016AF90`, 0x8016AF90): `+0x00` u8 count from
`func_8016FF14(0..4)`; `+0x01` u8 <- 0x801936CB; `+0x02` u8 <- 0x801936CA;
`+0x04..+0x07` <- first 4 bytes of 0x80196A48; `+0x08..+0x18` 17 bytes of
plain-ASCII leader name. mission1: `00 00 07 00 00 00 07 02 "Magnus"`, i.e.
summary+2 = 0x801936CA = 7 and summary+6..7 = 0x80196A48+2..3 = 7, 2; the roster
holds 11 named characters.

Save field table 0x800A824C, non-null entries:

| idx | fnA | fnLoad | fnSave | off1 | fnLoad2 | fnSave2 | off2 | block |
|---|---|---|---|---|---|---|---|---|
| 0 | 8016AF80 | 8016AF88 | 8016AF90 | 0x004 | - | - | - | 26-byte summary |
| 1 | 8016CE40 | 8016CEC4 | 8016CF64 | 0x01E | 8016D068 | 8016D110 | 0 | packed field blob |
| 2 | 80173B50 | 80173B60 | 80173B70 | 0x1843 | - | - | - | 1 byte <-> 0x8018F4A1 |
| 3 | 80185110 | 80185118 | 80185120 | 0x1844 | 80185128 | 80185984 | 0xECC | map blocks (record 15 only) |

Entries 4..12 are zero and skipped (`beqzl` 0x80074A74). Index 0's pointers are
`jr ra` stubs except `fnSave` = 0x8016AF90.

## 2. The packed field blob (descriptor table 0x80187464)

`func_8016CEC4` (load side, reached from the reader `func_800749C0`) calls
`func_800742F4(field, 0x80187464)` at 0x8016CEDC; `func_8016CF64` (save side)
calls `func_80074494(field, 0x80187464)` at 0x8016CFF0. The table is 21 entries
of 16 bytes `{u32 dest, u32 stride, u32 ops, u32 count}`; `ops` is 3-byte triples
`{dest byte offset, byte count (+0x80 = signed), bit width}` terminated by a
triple whose byte 1 is 0. Bits are read MSB first; blob bit 0 is the MSB of
`slot+0x2A`. Record 15 uses a second table at 0x80187638. Blob byte = bit >> 3, bit-in-byte = bit & 7 (MSB = 0).

| idx | dest (live) | stride | count | bits/rec | blob bit | block |
|---|---|---|---|---|---|---|
| 0 | 80193BE0 | 0x38 | 100 | 301 | 0 | character records (name +0x00 codec, level +0x13, EXP +0x35) |
| 1 | 80197210 | 0x19 | 30 | 137 | 30100 | unit/battalion records |
| 2 | 801969D8 | 0x0B | 10 | 64 | 34210 | |
| 3 | 800E9C08 | 0x12 | 1 | 61 | 34850 | boot-resident settings |
| 4 | 80196A48 | 0x10 | 1 | 96 | 34911 | 2 u16 + flag bytes |
| 5 | 80197830 | 0x06 | 121 | 29 | 35007 | |
| 6 | 80193698 | 0x3A | 1 | 432 | 38516 | 58-byte progress record |
| 7 | 80197730 | 0x01 | 100 | 7 | 38948 | |
| 8 | 801977F8 | 0x01 | 30 | 5 | 39648 | |
| 9 | 80193AD0 | 0x01 | 6 | 3 | 39798 | |
| 10 | 80190FA0 | 0x16C | 1 | 26 | 39816 | |
| 11 | 80190FA4 | 0x02 | 120 | 10 | 39842 | |
| 12 | 80191094 | 0x01 | 120 | 3 | 41042 | |
| 13 | 80195410 | 0x16C | 1 | 24 | 41402 | |
| 14 | 80195414 | 0x02 | 120 | 10 | 41426 | |
| 15 | 80195504 | 0x01 | 120 | 3 | 42626 | |
| 16 | 80196A78 | 0x94 | 1 | 1095 | 42986 | army record: name +0x00, **gold +0x14** |
| 17 | 80193AE0 | 0x04 | 40 | 13 | 44081 | |
| 18 | **80196B20** | **0x04** | **278** | **16** | **44601** | **owned item list** |
| 19 | 80196A58 | 0x01 | 32 | 8 | 49049 | |
| 20 | 80197188 | 0x01 | 16 | 8 | 49305 | |

Verification: re-encoding entries 0..17 (44601 bits) reproduces `slot+0x2A` ..
`slot+0x15F0` (5575 bytes) byte for byte for mission1 slot 0.

## 3. The owned item list in the save

Descriptor entry 18: dest 0x80196B20, stride 4, count 278, ops 0x80187448 =
`(00,02,09)` then `(03,01,07)`. From blob bit offset 44601, record `i`:

* bits `44601+16i .. +8` (9 bits, written as 2 bytes at record+0): item id.
* bits `44601+16i+9 .. +15` (7 bits, written as 1 byte at record+3): count.

Byte locations: the field starts at **bit 1 of blob byte 5575 (0x15C7)** =
`slot+0x15F1`, and ends at bit 0 of blob byte 6131 (0x17F3) = `slot+0x181D`.
4448 bits = 556 bytes of payload across 557 bytes (both ends partial, so records
are not byte-aligned). For slot 0 the first byte is file offset **0x1601**.

Item ids are **1-based into the item struct table at 0x8018C42C** (stride 0x20):
`name = *(u32 *)(0x8018C42C + id*0x20)`, category byte =
`*(u8 *)(0x8018C430 + id*0x20)`. `func_8016F5D0` (0x8016F5D0) returns that name
pointer (`lui 0x8019; lw -15316`), `func_8016F5E8` (0x8016F5E8) returns that
category byte, and the name-display site 0x80180480-0x80180488 calls
`func_8016F5D0` with the u16 at 0x801939FE. Table entry 0 is a dummy (`ff ff...`,
name pointer 0x8019039C), id 1 = "Short Sword" (name table at ROM 0x613D0),
ids 1..277 = items, id 278 = "None", id 0 = empty slot. `func_8016B6FC` scans the
278 slots for an id and returns 511 when absent.

Decoded mission1 slot 0 (17 records, in list order):

```
 0 117 Scipplay Staff x3      9   1 Short Sword   x9
 1 118 Arc Wand      x1     10 168 Plate Mail    x1
 2 127 Marionette    x2     11 275 Blue Sash     x1
 3 191 Robe          x3     12 159 Chain Mail    x8
 4 193 Magician's Robe x1   13 135 Round Shield  x8
 5 207 Witch's Dress x2     14 220 Iron Helm     x8
 6 232 Bandanna      x8     15 102 Short Bow     x6
 7 243 Spellbook     x3     16 155 Leather Armor x6
 8 253 Amulet        x5
```

`assets/escort_suspend.bin` slot 0 has 44 records; `assets/scene_5_suspend.bin`
slot 0 has the same 44 in the same order plus 5 more, the shape of an append-only
list. `mission2.bin` slot 0 has stamp 0, so the loader rejects it on the stamp.

## 4. Gold

* Live: **0x80196A8C** = 0x80196A78 + 0x14, 32-bit. New Game writes 1000:
  `li v0,1000` 0x80184F08, `sw v0,27276(at)` 0x80184F10 (27276 = 0x6A8C).
* Save: descriptor entry 16, offset 0x14, 4 bytes / 31 bits, blob bit
  42986 + 144 = **43130** (blob byte 5391 = 0x150F, bit 2) = `slot+0x1539` bit 2.
* Decoded: mission1 1000, escort_suspend 6540, scene_5_suspend 7871. Being a
  31-bit field, values above 0x7FFFFFFF do not survive a save round trip.

## 5. Live RDRAM addresses, with instruction proof

* **Item list = 0x80196B20**, stride 4, 278 records,
  `{u16 id +0x00, u8 ??? +0x02 (never saved), u8 count +0x03}`.
  * `func_8016B6FC` 0x8016B6FC: `lhu v0,0(a1)` 0x8016B70C with
    `a1 = 0x80196B20 + i*4` 0x8016B708, bound 278 0x8016B720, `addiu a1,a1,4`
    0x8016B728, "not found" sentinel 511 0x8016B72C.
  * `func_80172820` (add/remove count) 0x80172820: `lhu v1,27424(v1)` 0x80172850
    (27424 = 0x6B20), `lbu v0,27427(v0)` count 0x80172864, clamp 99
    0x8017286C-0x80172890, bound 278 0x801728B8-0x801728C0, writes
    `sh a0,27424(at)` 0x801728E8 and `sb a1,27427(at)` 0x801728F4.
  * Accessors: `func_8017069C` id (0x801706AC), `func_80170688` +3 count
    (0x80170698), `func_801706B0` +2 (0x801706C0).
  * New Game clears it: `addiu a0,a0,27424` 0x80185050, `li a1,1112` 0x80185058
    (1112 = 278*4) before `func_80093380` (memset).
* **Gold = 0x80196A8C** (`sw v0,27276(at)` 0x80184F10). The army record base
  0x80196A78 holds the 12-byte name "Blue Knights" in every RDRAM dump, including
  runs with no save loaded.
* Record 15 uses the separate descriptor table 0x80187638 (dest 0x80195580
  stride 0x34 count 100, plus 0x801974FE and 0x801951D0).

## 6. Proven at instruction level vs open

Proven: slot geometry and checksum seed; the save field table at 0x800A824C and
its 4 non-null entries; the blob descriptor table at 0x80187464 with the 21
destinations, strides, counts and bit widths; the item field's bit offset from a
byte-exact re-encoding of the first 5575 blob bytes; live item address 0x80196B20
with stride 4, capacity 278, id at +0 and count at +3; the 1-based id to name
mapping via `func_8016F5D0` and the name-display call site; live gold 0x80196A8C
and its 31-bit save field.

Open points, each with the cheapest runtime check (live console `OGRE_LIVE_CONSOLE=1`,
commands written into the watched file, `[console]` lines on stdout):

1. The list is what the item screen shows. Load mission1 slot 0, then
   `rh 80196B20 32`: the first halfwords must be `0075 0000 0003 0076 ...`.
2. Gold. Same run, `r 80196A8C 1` must read 1000 (6540 for escort_suspend); then
   `w 80196A8C 5000` and confirm the party screen shows 5000.
3. Live `+0x02` of an item record: read by `func_801706B0`, never saved, meaning
   unknown.
4. Rewriting. Write 0x80196B20 live and let the game save; the blob then carries
   the change. A save-file-only edit must be bit-spliced (556-byte field, both
   ends partial) and then have both header checksums recomputed with
   `tools/sramsave.py` `reseed_slot(slot, device_offset)`. `+0x02` cannot be set
   through a save file.
