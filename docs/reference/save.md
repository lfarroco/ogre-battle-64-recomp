---
title: Battery save and the Controller Pak
summary: The 32 KiB SRAM image, its slot geometry and checksums, the access functions, and the suspend-save record.
status: verified
session: 212
order: 60
---

# Battery save and the Controller Pak

The game saves to a 32 KiB battery-backed SRAM image. The Controller Pak holds
copies and backups of the same save data. In session 66 the developer corrected
an earlier reading that named the pak as the save device, and `docs/scenes.md`
carries the corrected A/B on the title's Load Game entry.

Two independent sources name the chip as SRAM. The mupen64plus per-game database
entry for this ROM's CRC (`0ADAECA7 B17F9795`) sets `SaveType=SRAM` and
`Mempak=Yes`, and the game's own code builds the save `OSPiHandle` in
`func_8008A040` with `baseAddress 0xA8000000` (physical `0x08000000`). The ROM
contains no EEPROM or FlashRAM protocol.

The game reaches the SRAM through its own PI DMA path. `func_8008A0F0(devOffset,
dramAddr, size, dir)` builds an `OSIoMesg` and calls `func_8008BC40`, which
queues the message at `D_800AA408`. The runtime completes that queue inline and
takes the device from the `OSPiHandle` the message carries.

The port types the battery as SRAM (`entry.save_type = recomp::SaveType::Sram`)
and keeps the logical 32 KiB image at `<config>/saves/<game id>.bin`. The
`OGRE_SAVE` knob and the interchange recipes are in `docs/guides/app-build.md`.

## The device header and the slots

The image opens with a 16-byte device header, and the device signature `QuestOG3`
sits at device `0x04`. Slot 0 begins at device `0x10`, so its own `QuestOG3` is at
`0x14`.

A 6224-byte (`0x1850`) record is one save slot. Slot `n` begins at device
`0x10 + n*0x1850`.

Slot index 15 is the 19176-byte (`0x4AE8`) record at device `0x30B0`. It shares
the `QuestOG3` signature layout, with the magic at `0x30B4`.

Only slots 0 and 1 exist as 6224-byte records. Slot 2's computed offset
`0x10 + 2*0x1850` is `0x30B0`, the start of the record-15 region, so the next
record would land inside it. `tools/sramsave.py` derives the same two-slot count
from that arithmetic.

A blank battery still carries a valid record 15. A freshly formatted image holds
`QuestOG3` at `0x30B4` with a zero first word at `0x30B0`, and
`save_record15_validate` accepts that zero word without checking the record's
checksums.

A slot's first 4 bytes are two `u16` checksums, at slot `+0x00` and `+0x02`. The
signature occupies slot `+0x04`..`+0x0B`. The `u32` at slot `+0x0C` is the slot's
in-use stamp, and `save_slot_summary_load` treats a zero there as an empty slot.
The 26-byte summary sits at `slot + 0x0C + *(0x800A8258)`, which is
`slot + 0x10`.

## The checksums

`save_slot_validate(slot, buf)` (device `0x8007541C`) reads the 6224 bytes at
`*(0x800A83B8) + 0x10 + slot*0x1850` and returns 0 unless the 8 bytes at
`0x800A8240` equal `buf+4`. It then compares the two header `u16` with two
checksums over `buf+0x0C` for 6212 (`0x1844`) bytes, so the pair covers
`slot+0x0C` through `slot+0x1850`.

`checksum_sum_seeded` (`0x80075A84`) adds the region's bytes and the seed and
returns the low 16 bits. `checksum_bits_seeded` (`0x80075B00`) counts the region's
set bits, adds the seed and returns the low 16 bits.

The seed is the slot's own device offset, `0x10 + n*0x1850`. A record therefore
validates only in the slot whose offset seeded its checksums, and the same 6224
bytes are invalid in every other slot.

`checksum_sum` (`0x80075AC4`) and `checksum_bits` (`0x80075B60`) are the same two
loops with no seed. The game's copy-to-pak path uses that pair, so a note inside
a Controller Pak is checksummed with seed 0.

A note copied back out of a pak needs both halfwords reseeded for the battery
slot it lands in. `reseed_slot(slot, device_offset)` in `tools/sramsave.py`
rewrites them, and importing without that step made the game reject the image and
repair it to the blank format.

`save_record15_validate(buf)` (`0x80075578`) applies the same signature check to
the 19176-byte record. When the first word is non-zero, it compares the two
header `u16` with the seeded checksums over `buf+0x0C` for 19164 (`0x4ADC`)
bytes, seeded with `0x30B0`.

## The access path

`save_image_read(dst, dev_off, size)` (device `0x80074CF0`) copies a range out of
the cached image. When `*(0x800A83B8)` is null it mallocs `0x8000` bytes and
reads the whole device image in 256-byte DMAs, then copies `size` bytes from
`image + dev_off` into `dst`. Every later call reuses that cache.

`save_image_write(image)` (device `0x80074C58`) writes the whole image back. It
allocates a 16-byte handle holding `save_dma` and sends `0x8000` bytes in
256-byte chunks with direction 1. The function returns early while
`*(0x800C4800)` is set.

`save_commit(magic)` (device `0x80074BF0`) is the gate on a write. It calls
`save_image_write` only when the dirty flag `*(0x800A83BC)` is set and `magic`
equals `0x37081383`. It then clears the dirty flag and frees the cached image.

`save_slot_summary_load(slot)` (device `0x80074AD4`) mallocs 6224 bytes, calls
`save_slot_validate`, and reads the record with `save_image_read` into the
buffer. It returns a 26-byte copy of the summary when the in-use stamp at
`buf+0x0C` is non-zero, and returns NULL otherwise. Slot 15 takes the other arm:
it mallocs 19176 bytes, calls `save_record15_validate`, and reads the record from
device `0x30B0`.

`memcpy` (`0x80093060`) and `memcmp` (`0x80092F50`) are the two helpers this
cluster calls. `0x80093060` copies from its first argument to its second, which
is the reverse of the libultra argument order.

## The cached image and the global words

`0x800A83B8` holds the cached 32 KiB image, allocated by `save_image_read` and
freed by `save_commit`.

`0x800A83BC` is the dirty flag. The writers set it, and `save_commit` tests and
clears it.

`0x800A8240` is the 8-byte `QuestOG3` signature that `save_slot_validate`
compares against. It lives in BSS, is filled at boot, and every valid slot
carries the same bytes at `+0x04`.

`0x800A824C` is the save field table, 13 entries with a stride of `0x1C`, ending
at `0x800A83B8`. `func_800749C0` walks the entries, calls the load function at
entry `+0x04` (`lw -32176` at `0x80074A70`), and adds the entry's `+0x0C` offset
to the slot buffer (`lw -32168` at `0x80074A84`). Entry 0's offset word is at
`0x800A8258` and holds `4`, so the summary load reads `slot+0x10`.

`0x800C4800` is the write-skip flag that `save_image_write` tests. The same byte
is the phase byte the VI manager sets (`docs/HANDOFF-2026-08-31-session15.md`).

**Address form.** The code forms these addresses with `lui 0x800b` plus a
negative immediate, so the effective address is `0x800A8xxx`: `lw -31816` at
`0x80074C1C` (image), `lbu -31812` at `0x80074BF4` (dirty), `addiu -32192` at
`0x800754D4` (signature). `docs/symbols.md` and the session-78 handoff recorded
the `lui` immediate plus the low half as `0x800B8xxx`, which is off by
`0x10000`. The registry and this page use the resolved `0x800A8xxx` form.
`docs/notes-save-items.md` §0 has the same correction.

## The suspend save

Record 15 is the suspend save. The game writes it from bank unit A's suspend
action, which calls the slot writer `func_8007485C` at `0x8019F9F8`, with slot
14 and then slot 15. The load path reaches it through `func_800749C0(15)`, the
save-field reader that walks the field table.

The Load Game confirm handler `func_801A9B3C` (streamedC, `0x801A9B3C`) tests
that the slot it loaded was 15. It then calls `func_80074834(15)` at `0x801A9BCC`.

`func_80074834` (`.main`, `0x80074834`, `0x28` bytes) erases the suspend point.
It calls `func_80074F9C(15)` to blank the record's 12-byte header and then calls
`save_commit(0x37081383)` to write the image.

`func_80074F9C(15)` `memset`s the 12 bytes at `image + 0x30B0`, copies
`QuestOG3` into `+0x04`, copies the 12 bytes back and sets the dirty flag. The
19164-byte payload after the header is not touched.

A zero first word is what `save_record15_validate` reads as empty, so clearing
those 12 bytes deletes the suspend point.

The erase is gated at `0x801A9BA4` by `func_801707B4(0x80) == 0`. That function
reads 4 bytes from ROM `0x275D98C` into `0x80190F30`; the bytes `3a 34 32 00` are
the tail of a build-date string, so bit `0x80` is clear and the gate passes on
this ROM.

The debug Reset Control Deck path in bank unit H also calls `func_80074834` with
15. That path erases slots 0, 1, 14 and 15. No other bank unit calls
`func_80074834`, `func_80074F9C` or `func_80075360` with 15.

`mods/permanent-suspend` is a `RECOMP_PATCH` of `func_80074834`. It skips
`func_80074F9C` when the slot is 15 and keeps the commit, and every other slot
takes the original path. The mod is off by default (`enabled_by_default =
false`). Its README records that the debug Reset Control Deck path goes through
the same patched function, so that path keeps record 15 as well.

The mod was measured on the developer's `assets/saves/suspend_final_boss.n64`. A
vanilla load turns `0x30B0..0x30BB` from `dc419ec6 51756573 744f4733` into
`00000000 51756573 744f4733`, and the game still reaches scene `0x0003`. The
patched load leaves all 32 KiB byte-identical, and a second process loads the
same record again and reaches scene `0x0003`.

## The tool

`tools/sramsave.py` reads and writes the logical 32 KiB image. It has four
commands: `check`, `import`, `export` and `pak`.

`check` reports the detected wrapper and byte order, the device and slot-0
magics, the slot's first word, and each slot's checksum result. It also reports
the record at `0x30B0` as valid or invalid, and prints any magic that lies inside
that record's region.

`find_sram` scans every offset of a file for the `QuestOG3` device magic in
logical order and in 32-bit byteswapped order. `import` refuses a file whose save
it cannot find and never invents an image.

For a DexDrive `.N64` or a bare Controller Pak, `import` follows the pak's FAT to
the live notes and writes their slots into battery slots 0 and 1. Each slot is
reseeded for its device offset, and `--all` also takes the stale records that
deleted notes leave behind.

`reseed_slot(slot, device_offset)` recomputes both header halfwords, and
`record15_valid(sram)` applies the record-15 rule, including the zero-first-word
acceptance. `slot_checksum_ok(slot, device_offset)` is the read-only test behind
`check`.

`export` writes a 32768-byte 32-bit byteswapped image for an `.sra`-style
loader. `pak` lists the note records a pak holds, marks each live or stale, and
reports whether its embedded slot is valid for pak seed 0.

The browser build exports the same raw image from the file the runtime's saving
thread writes. `debug/probes/save.cjs` round-trips it byte for byte, and
`tools/sramsave.py check` on that file reports both slots' checksums and the
record at `0x30B0` as valid.

## Related facts

The RDRAM layout, the framebuffers and the heap pointer convention are in
[Guest memory map](memory.md). The title's Load Game entry and the Load Game
screen are in [What each screen must show](../scenes.md). The runtime's save
path, `OGRE_PREF_DIR`, `OGRE_SAVE` and the emulator interchange recipes are in
`docs/guides/app-build.md`.

## Addresses

<!-- BEGIN GENERATED: addresses:addresses -->
| address | name | note | kind | evidence | session | conf |
|---|---|---|---|---|---|---|
| `0x0030B0` | `g_save_record15` | device offset, 0x4AE8 bytes | struct | `save_slot_summary_load` slot-15 arm reads 19176 bytes here; a blank battery has `QuestOG3` at +4 with a zero first word. | 78 | high |
| `0x37081383` | `save_commit_magic` | value, not an address | constant | `save_commit` writes the cached image only when its argument equals this value; `func_80074834` passes it after blanking the header. | 212 | high |
| `0x80074834` | `save_slot_erase` | 0x28 bytes | function | Blanks a slot 12-byte header through `func_80074F9C(slot)` then commits `0x37081383`; the Load Game handler calls it with 15 at `0x801A9BCC`. | 212 | high |
| `0x800749C0` | `save_field_read` |  | function | Walks the 13-entry save field table and loads a slot fields; the suspend record is loaded through it; `mods/item-randomizer` hooks it. | 212 | high |
| `0x80074F9C` | `save_slot_blank_header` |  | function | `memset`s the 12-byte header at image+0x30B0, copies `QuestOG3` to +4, writes it back and sets the dirty flag. | 212 | high |
| `0x800C4800` | `g_save_write_skip` | byte | global | `save_image_write` returns early while this byte is set; sessions 13-15 read the same byte as the VI phase byte. | 78 | high |
| `0x801A9BCC` | `load_menu_suspend_erase` |  | code | `jal 0x80074834` with a0=15 in the Load Game confirm handler `func_801A9B3C` (streamedC). | 212 | high |
| `0x80074BF4` | `save_dirty_load` | lbu -31812 | code | Loads the dirty flag; `lui 0x800b` + the negative offset resolves to 0x800A83BC. | 217 | high |
| `0x80074C1C` | `save_image_load` | lw -31816 | code | Loads the cached image pointer; resolves to 0x800A83B8. | 217 | high |
| `0x800754D4` | `save_signature_addr` | addiu -32192 | code | Forms the signature address; resolves to 0x800A8240. | 217 | high |
<!-- END GENERATED: addresses:addresses -->

## Evidence

- `docs/symbols.md` ("Save (battery SRAM) — the whole access path") — the access functions, the slot geometry and the seed rule. Its global rows carried the `0x800B8xxx` register form, corrected here to `0x800A8xxx` (see "Address form").
- `docs/HANDOFF-2026-09-17-session66.md` — the chip is 32 KiB SRAM, the `OSPiHandle` device rule, the port's save type and the runtime's image file.
- `docs/HANDOFF-2026-09-18-session78.md` §1-§3 — the two-slot geometry, the seeded checksum rule, the DexDrive pak contents and the discriminating Load Game verification.
- `docs/HANDOFF-2026-11-01-session209.md` — the browser exports the same raw image, and `tools/sramsave.py check` validates both slots and the `0x30B0` record.
- `docs/HANDOFF-2026-11-02-session212.md` — record 15's writer and loader, the erase on load, the call sites and the permanent-suspend mod.
- `docs/DECISIONS.md` (2026-09-18 (78), 2026-11-02 (212)) — the durable statements of the seed rule and the suspend record.
- `docs/notes-save-items.md` §0-§1 — the resolved `0x800A8xxx` form of the globals and the field-table layout; `mods/reference/dump.toml` carries the same relocation targets.
- `tools/sramsave.py` — `find_sram`, `reseed_slot`, `record15_valid`, `notes_to_sram` and the four commands.
- `docs/guides/app-build.md` ("Saves — the cartridge battery (SRAM)") — the runtime's save file and the import recipes.
