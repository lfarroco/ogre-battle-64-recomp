#!/usr/bin/env python3
"""Import/export Ogre Battle 64 battery saves between emulators, pak dumps and the port.

OB64 saves to **32 KiB of SRAM** (the cartridge battery) -- see
`app/src/main.cpp`'s `entry.save_type` comment for the evidence. The port keeps
that image byte-for-byte in

    <config>/saves/ogrebattle64-us-rev1.bin        (macOS: ~/Library/Application Support/ogrebattle64/saves/)

and other ports/emulators use the same 32 KiB of logical bytes, but the *file*
wrapping differs:

  * **logical / port / mupen64plus `.srm`-style**: 32768 bytes, the bytes the
    game sees. Two magics identify it: "QuestOG3" at 0x04 (device signature)
    and "QuestOG3" at 0x14 (slot 0 header, after the u32 checksum at 0x10).
  * **32-bit byteswapped** (`parallel-n64`, and some `.sra` writers): every
    4-byte group is reversed, so the magic reads "seuQ3GOt".
  * **combined save-media image**: some emulators/tools dump every N64 save
    device into one file. The observed parallel-n64 dump is 0x48800 bytes with
    the SRAM as the 0x8000 bytes at **0x20800** (FlashRAM 0x20000 + EEPROM 0x800
    before it), byteswapped.
  * **DexDrive `.N64` Controller Pak dump**: a 0x1040-byte DexDrive header
    (`"123-456-STD"` at 0x00) followed by a 32 KiB **Controller Pak** image --
    NOT the battery. The game's *copy/backup* feature writes one 25-page note
    per save, named `OgreBATTLE64 <n>`, and a note holds a verbatim copy of a
    battery save slot, so the battery state is recoverable from it. The copy is
    checksummed with seed 0, though, while the battery validator seeds a slot's
    two u16 header checksums with the slot's own device offset, so `import`
    reseeds them for the slot each note lands in (see `reseed_slot`).

This tool finds the SRAM by its magic at any offset and any of those byte
orders, so `import` does not need to be told the layout. A DexDrive/pak file is
detected by the `123-456-STD` marker (or by being exactly one 32 KiB pak) and
converted through the notes' embedded slots instead.

Usage:
    tools/sramsave.py check  <file> [...]        # identify + validate
    tools/sramsave.py import <in> <out.bin>      # -> logical 32 KiB for the port
    tools/sramsave.py export <in.bin> <out>      # -> 32 KiB byteswapped (.sra-like)
    tools/sramsave.py pak    <in> [--all]        # list the notes a pak holds

`import` never invents data: it refuses a file whose save it cannot find, and
`check` says which wrapper, byte order and container offset it detected. For a
DexDrive/pak file, `import` uses the notes the pak's own filesystem says are
**live** (the FAT chain) and writes them, reseeded, into battery slot 0 then 1 --
the battery has only two save slots, because slot index 2's offset `0x30B0` is
where the game's 19176-byte map record begins. `--all` also emits the stale
records left behind by deleted notes (still capped at two); `pak` lists them.
"""

import sys

SRAM_SIZE = 0x8000
MAGIC = b"QuestOG3"
MAGIC_OFF_DEVICE = 0x04
MAGIC_OFF_SLOT0 = 0x14
SLOT_BASE = 0x10          # first save slot starts here, after the 16-byte device header
SLOT_STRIDE = 0x1850      # func_8007541C: slot n = 0x10 + n*0x1850
DEVICE_HEADER = b"\x00\x00\x00\x00" + MAGIC + b"\x00\x00\x00\x00"
# A slot's first 4 bytes are TWO u16 checksums over slot+0x0C..slot+0x1850
# (0x1844 bytes), each **seeded with the slot's device offset** (0x10+n*0x1850):
#   +0x00 = (sum of the bytes)       + offset   (func_80075A84)
#   +0x02 = (count of set bits)      + offset   (func_80075B00)
# `func_8007541C` recomputes both and rejects the slot on either mismatch, which
# is why a note copied straight out of a pak (checksummed with seed 0 by the
# game's own copy path) must be reseeded for the battery slot it lands in.
CHECKSUM_OFF = 0x0C
POPCOUNT = bytes(bin(i).count("1") for i in range(256))
# Slot index 15 is special: a 19176-byte (0x4AE8) record at device 0x30B0, which
# is what the game writes at the map (so even a "blank" battery carries a magic
# at 0x30B4). Same two checksums, region +0x0C..0x4AE8, seed 0x30B0.
RECORD15_OFF = 0x30B0
RECORD15_SIZE = 0x4AE8
# ...which is also why the battery has only TWO save slots: slot index 2's offset
# *is* the map record's start, so slots 2.. cover the same RAM.
SLOT_COUNT = (RECORD15_OFF - SLOT_BASE) // SLOT_STRIDE

# --- DexDrive .N64 wrapper + the Controller Pak (PFS) layout it holds --------
DEX_MAGIC = b"123-456-STD"
DEX_HEADER = 0x1040       # DexDrive header; the pak image follows it
PAK_SIZE = 0x8000
PAGE_SIZE = 0x100
FAT_OFF = 0x100           # page 1: u16 BE next-page per page; page 2 is its mirror
DIR_OFF = 0x300           # page 3: 8 x 32-byte directory entries; page 4 mirror
FAT_FREE = 0x0003
FAT_END = 0x0001
FIRST_DATA_PAGE = 5
GAME_CODE = b"NOBE"       # OB64's ROM game code
PUB_CODE = b"EB"
DIR_ENTRY_SIZE = 32
# Every save note the game allocates is 25 pages (6400 bytes, "1 note 25 pages
# to save") and its payload is [0x20 bytes of save context][the battery slot].
NOTE_SLOT_OFF = 0x20
NOTE_PAGES = 25


def swap32(data):
    out = bytearray(len(data))
    for i in range(0, len(data) - 3, 4):
        out[i:i + 4] = data[i:i + 4][::-1]
    return bytes(out)


def find_sram(data):
    """Return (offset, byte_order, sram_bytes) or None."""
    limit = len(data) - SRAM_SIZE
    for offset in range(0, max(limit, 0) + 1):
        dev = data[offset + MAGIC_OFF_DEVICE:offset + MAGIC_OFF_DEVICE + len(MAGIC)]
        if dev == MAGIC:
            return offset, "logical", data[offset:offset + SRAM_SIZE]
        if swap32(dev) == MAGIC:
            return offset, "byteswapped32", swap32(data[offset:offset + SRAM_SIZE])
    return None


def slot_header_word(sram, slot=0):
    """The u32 at the start of a slot (0x10 + slot*0x1850). It varies with the
    slot's content (blank slot 0 is 0x4046120E, filled slot 0 0xC70A1611 in the
    session-66 saves) but has not been verified to be a checksum, so it is only
    reported, never checked."""
    off = SLOT_BASE + slot * SLOT_STRIDE
    return int.from_bytes(sram[off:off + 4], "big")


# --- DexDrive / Controller Pak parsing --------------------------------------

def looks_like_pak(data):
    """A positive Controller Pak signature, so a bare 32 KiB file can be told from
    a battery image in *either* byte order: a PFS pak mirrors its page-1 index
    table at page 2 and leaves `0x0003` free markers in it."""
    if len(data) < PAK_SIZE:
        return False
    if data[FAT_OFF:FAT_OFF + PAGE_SIZE] != data[FAT_OFF + PAGE_SIZE:FAT_OFF + 2 * PAGE_SIZE]:
        return False
    free = FAT_FREE.to_bytes(2, "big")
    return any(data[FAT_OFF + 2 * p:FAT_OFF + 2 * p + 2] == free
               for p in range(FIRST_DATA_PAGE, 128))


def find_pak(data):
    """Return (pak_bytes, description) for a DexDrive .N64 or bare pak image, else None.

    A bare 32 KiB file is ambiguous -- it is exactly one battery image *or* one
    Controller Pak -- so the battery wins: if the device magic is there in either
    byte order this is a battery image, never a pak, and otherwise the pak has to
    show its own filesystem signature."""
    if data[:len(DEX_MAGIC)] == DEX_MAGIC and len(data) >= DEX_HEADER + PAK_SIZE:
        pak = data[DEX_HEADER:DEX_HEADER + PAK_SIZE]
        return pak, f"DexDrive .N64 ({len(data)} bytes: 0x{DEX_HEADER:X} header + 32 KiB pak)"
    if len(data) == PAK_SIZE and find_sram(data) is None and looks_like_pak(data):
        return data, "bare 32 KiB Controller Pak image"
    return None


def _fat(pak):
    """Page -> next page, as the pak's inode table stores it (u16 BE at 0x100+2*p)."""
    return {p: int.from_bytes(pak[FAT_OFF + 2 * p:FAT_OFF + 2 * p + 2], "big")
            for p in range(128)}


def live_notes(pak):
    """The notes the pak's filesystem says exist: FAT chains of exactly NOTE_PAGES
    pages, in the order their first page appears.

    A chain head is an allocated data page no other allocated page points at.
    Pages 0..4 are the ID/inode/directory area, so they are never heads (page 0's
    FAT halfword doubles as the index checksum at 0x101)."""
    fat = _fat(pak)
    allocated = {p for p in range(FIRST_DATA_PAGE, 128) if fat[p] not in (FAT_FREE,)}
    heads = []
    for p in sorted(allocated):
        if not any(q != p and fat[q] == p for q in allocated):
            heads.append(p)

    notes = []
    for head in heads:
        pages, cur = [], head
        while cur not in pages and FIRST_DATA_PAGE <= cur < 128 and len(pages) <= 128:
            pages.append(cur)
            nxt = fat[cur]
            if nxt == FAT_END or nxt == FAT_FREE or nxt >= 128:
                break
            cur = nxt
        if len(pages) == NOTE_PAGES:
            notes.append({"start_page": head, "pages": pages,
                          "data": b"".join(pak[p * PAGE_SIZE:(p + 1) * PAGE_SIZE] for p in pages)})
    return notes


def all_records(pak):
    """Every 25-page-aligned note payload in the pak, live or stale (a deleted
    note's pages are marked free but their bytes survive)."""
    records = []
    page = FIRST_DATA_PAGE
    while page + NOTE_PAGES <= 128:
        if pak[page * PAGE_SIZE + NOTE_SLOT_OFF + 4:page * PAGE_SIZE + NOTE_SLOT_OFF + 4 + len(MAGIC)] == MAGIC:
            records.append({"start_page": page, "pages": list(range(page, page + NOTE_PAGES)),
                            "data": pak[page * PAGE_SIZE:(page + NOTE_PAGES) * PAGE_SIZE]})
        page += NOTE_PAGES
    return records


def note_name(pak, note):
    """The directory entry's note name, decoded from the pak codepage.

    That codepage is digits at 0x10..0x19, 'A'..'Z' at 0x1A..0x33 and ' ' at
    0x0F: note 0 in every one of these dumps reads `OgreBATTLE64 1`. Only the
    ASCII subset is decoded; anything else is escaped so the name stays honest."""
    for e in range(8):
        off = DIR_OFF + e * DIR_ENTRY_SIZE
        ent = pak[off:off + DIR_ENTRY_SIZE]
        if ent[:4] != GAME_CODE or ent[4:6] != PUB_CODE:
            continue
        start = int.from_bytes(ent[6:8], "big")
        if start != note["start_page"]:
            continue
        out = []
        for b in ent[16:32]:
            if b == 0:
                break
            if 0x10 <= b <= 0x19:
                out.append(chr(ord("0") + b - 0x10))
            elif 0x1A <= b <= 0x33:
                out.append(chr(ord("A") + b - 0x1A))
            elif b == 0x0F:
                out.append(" ")
            else:
                out.append(f"\\x{b:02X}")
        return "".join(out)
    return None


def note_slot(note):
    """The battery save slot a note carries, or None if the note does not hold one.

    The payload is [0x20 bytes of save context][the slot]: the slot's u32 header
    word sits at 0x20 and its "QuestOG3" magic at 0x24, exactly where the SRAM
    slot has them (+0x00 and +0x04)."""
    off = NOTE_SLOT_OFF
    data = note["data"]
    if data[off + 4:off + 4 + len(MAGIC)] != MAGIC:
        return None
    return data[off:off + SLOT_STRIDE]


def slot_checksums(slot):
    """(sum, set-bit count) of the region a slot's two u16 header checksums cover."""
    total = 0
    bits = 0
    for byte in slot[CHECKSUM_OFF:SLOT_STRIDE]:
        total += byte
        bits += POPCOUNT[byte]
    return total & 0xFFFF, bits & 0xFFFF


def slot_checksum_ok(slot, device_offset):
    """Whether a slot's header checksums match what the battery validator expects
    at `device_offset` (0x10 + n*0x1850)."""
    total, bits = slot_checksums(slot)
    stored_sum = int.from_bytes(slot[0:2], "big")
    stored_bits = int.from_bytes(slot[2:4], "big")
    return (stored_sum == (total + device_offset) & 0xFFFF
            and stored_bits == (bits + device_offset) & 0xFFFF)


def reseed_slot(slot, device_offset):
    """Rewrite a slot's two u16 header checksums for a battery slot at
    `device_offset`.

    A note in a Controller Pak carries the same 0x1850-byte slot the battery
    does, but the game's copy-to-pak path checksums it with seed 0
    (`func_80075AC4`/`func_80075B60`), so the value is right for `offset 0` and
    wrong for every battery slot. Re-seeding is exactly what restoring the note
    to the battery has to do."""
    total, bits = slot_checksums(slot)
    out = bytearray(slot)
    out[0:2] = ((total + device_offset) & 0xFFFF).to_bytes(2, "big")
    out[2:4] = ((bits + device_offset) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def record15_valid(sram):
    """Slot index 15 is not a 6224-byte slot: it is the 19176-byte (`0x4AE8`)
    record at device `0x30B0` that the game writes at the map (so even a "blank"
    battery carries a magic at `0x30B4`). `func_80075578` requires the `QuestOG3`
    magic, and then — only when the record's first word is **non-zero** — the two
    checksums over `+0x0C..0x4AE8` seeded with `0x30B0`. A zero first word is
    accepted as it stands (`beqz` at `0x80075620`), which is the state a blank or
    freshly-written battery is in."""
    rec = sram[RECORD15_OFF:RECORD15_OFF + RECORD15_SIZE]
    if rec[4:4 + len(MAGIC)] != MAGIC:
        return False
    if int.from_bytes(rec[0:4], "big") == 0:
        return True
    total = 0
    bits = 0
    for byte in rec[CHECKSUM_OFF:]:
        total += byte
        bits += POPCOUNT[byte]
    return (int.from_bytes(rec[0:2], "big") == (total + RECORD15_OFF) & 0xFFFF
            and int.from_bytes(rec[2:4], "big") == (bits + RECORD15_OFF) & 0xFFFF)


def notes_to_sram(notes):
    """Lay the notes' slots out the way the battery does: a 16-byte device header,
    then slot 0..n-1 at 0x10 + n*0x1850, with each slot's checksums reseeded for
    the slot it lands in.

    Only two slots exist. Slot 2's offset, `0x30B0`, is where the game's
    19176-byte map record (`RECORD15_SIZE`) begins, so it covers the rest of the
    battery; extra notes are dropped with a warning rather than written into it."""
    image = bytearray(SRAM_SIZE)
    image[0:len(DEVICE_HEADER)] = DEVICE_HEADER
    placed = 0
    for note in notes:
        slot = note_slot(note)
        if slot is None:
            continue
        if placed >= SLOT_COUNT:
            print(f"  note at page {note['start_page']}: the battery has only "
                  f"{SLOT_COUNT} save slots (slot 2 is the map record's region), skipped",
                  file=sys.stderr)
            continue
        off = SLOT_BASE + placed * SLOT_STRIDE
        image[off:off + SLOT_STRIDE] = reseed_slot(slot, off)
        placed += 1
    return bytes(image), placed


# --- commands ----------------------------------------------------------------

def describe(path):
    data = open(path, "rb").read()
    print(f"{path}: {len(data)} bytes")

    pak = find_pak(data)
    if pak is not None:
        pak_bytes, kind = pak
        print(f"  {kind}; not the battery itself")
        notes = live_notes(pak_bytes)
        stale = all_records(pak_bytes)
        print(f"  {len(notes)} live note(s) in its filesystem, "
              f"{len(stale)} note record(s) present in total")
        for i, note in enumerate(notes):
            name = note_name(pak_bytes, note)
            slot = note_slot(note)
            if slot is None:
                print(f"    live note {i}: pages {note['pages'][0]}..{note['pages'][-1]}"
                      f" ({len(note['pages'])} pages), name {name or '(none)'}, "
                      f"no battery slot")
                continue
            seeded0 = slot_checksum_ok(slot, 0)
            print(f"    live note {i}: pages {note['pages'][0]}..{note['pages'][-1]}"
                  f" ({len(note['pages'])} pages), name {name or '(none)'}, "
                  f"battery slot yes, checksums {'pak-seeded (0)' if seeded0 else 'NOT valid for pak seed 0'}")
        for note in stale:
            if any(n["start_page"] == note["start_page"] for n in notes):
                continue
            print(f"    stale record: pages {note['pages'][0]}..{note['pages'][-1]} "
                  f"(left by a deleted note; `import --all` keeps it)")
        if not notes:
            print("  no live note carries a battery slot: not importable")
            return None
        return notes_to_sram(notes)[0]

    if len(data) == SRAM_SIZE:
        kind = "a bare 32 KiB image"
    else:
        kind = f"a combined/containered image ({len(data) / 1024:.0f} KiB)"
    found = find_sram(data)
    if found is None:
        print(f"  no OB64 SRAM signature found ({kind}); not importable")
        return None
    offset, order, sram = found
    print(f"  {kind}; SRAM at file offset 0x{offset:X}, byte order '{order}'")
    device_ok = sram[MAGIC_OFF_DEVICE:MAGIC_OFF_DEVICE + len(MAGIC)] == MAGIC
    slot0_ok = sram[MAGIC_OFF_SLOT0:MAGIC_OFF_SLOT0 + len(MAGIC)] == MAGIC
    magic_at = [off for off in range(SLOT_BASE, SRAM_SIZE - 4, 4)
                if sram[off + 4:off + 12] == MAGIC]
    slots = [off for off in magic_at if off < RECORD15_OFF]
    print(f"  device magic at 0x04: {'ok' if device_ok else 'MISSING'}; "
          f"slot 0 magic at 0x14: {'ok' if slot0_ok else 'empty/absent'}"
          f" (slot 0 header word 0x{slot_header_word(sram):08X})")
    print(f"  {len(slots)} save slot(s) of {SLOT_COUNT} carrying a magic, "
          f"at {[hex(o) for o in slots]}")
    for off in slots:
        print(f"    slot {(off - SLOT_BASE) // SLOT_STRIDE} @0x{off:04X}: checksums "
              f"{'ok' if slot_checksum_ok(sram[off:off + SLOT_STRIDE], off) else 'MISMATCH (the game will reject this slot)'}")
    if sram[RECORD15_OFF + 4:RECORD15_OFF + 12] == MAGIC:
        print(f"    the map record @0x{RECORD15_OFF:04X} ({RECORD15_SIZE} bytes, "
              f"slot index 15): {'valid' if record15_valid(sram) else 'INVALID'}")
    inside = [off for off in magic_at if off > RECORD15_OFF]
    if inside:
        print(f"    (a magic at {[hex(o) for o in inside]} lies inside that record's region)")
    nonzero = sum(1 for b in sram if b)
    print(f"  {nonzero} non-zero bytes in the 32 KiB image")
    return sram


def cmd_check(args):
    ok = True
    for path in args:
        if describe(path) is None:
            ok = False
    return 0 if ok else 1


def cmd_import(args):
    want_all = "--all" in args
    rest = [a for a in args if a != "--all"]
    if len(rest) != 2:
        print("usage: sramsave.py import [--all] <in> <out.bin>", file=sys.stderr)
        return 2
    src, dst = rest
    data = open(src, "rb").read()

    pak = find_pak(data)
    if pak is not None:
        pak_bytes, _ = pak
        notes = all_records(pak_bytes) if want_all else live_notes(pak_bytes)
        if not notes:
            print(f"{src}: no battery-slot note found in the pak; refusing to invent an image",
                  file=sys.stderr)
            return 1
        image, placed = notes_to_sram(notes)
        if placed == 0:
            print(f"{src}: the pak's notes carry no battery slot; refusing to invent an image",
                  file=sys.stderr)
            return 1
        open(dst, "wb").write(image)
        print(f"{src} -> {dst}: 32768 bytes ({placed} note(s) -> battery slot "
              f"{', '.join(str(n) for n in range(placed))})")
        return 0

    found = find_sram(data)
    if found is None:
        print(f"{src}: no OB64 SRAM signature found; refusing to invent an image", file=sys.stderr)
        return 1
    offset, order, sram = found
    open(dst, "wb").write(sram)
    print(f"{src} -> {dst}: 32768 bytes (SRAM at 0x{offset:X}, {order})")
    return 0


def cmd_export(args):
    src, dst = args
    data = open(src, "rb").read()
    if len(data) != SRAM_SIZE:
        print(f"{src}: expected exactly {SRAM_SIZE} bytes (a logical image), got {len(data)}", file=sys.stderr)
        return 1
    open(dst, "wb").write(swap32(data))
    print(f"{src} -> {dst}: 32768 bytes (32-bit byteswapped for .sra-style loaders)")
    return 0


def cmd_pak(args):
    want_all = "--all" in args
    rest = [a for a in args if a != "--all"]
    if len(rest) != 1:
        print("usage: sramsave.py pak [--all] <in>", file=sys.stderr)
        return 2
    src = rest[0]
    data = open(src, "rb").read()
    pak = find_pak(data)
    if pak is None:
        print(f"{src}: not a DexDrive .N64 or a bare 32 KiB Controller Pak image", file=sys.stderr)
        return 1
    pak_bytes, kind = pak
    print(f"{src}: {kind}")
    notes = all_records(pak_bytes) if want_all else live_notes(pak_bytes)
    if not notes:
        print("  no note records found")
        return 1
    live_starts = {n["start_page"] for n in live_notes(pak_bytes)}
    for note in notes:
        slot = note_slot(note)
        name = note_name(pak_bytes, note)
        state = "live" if note["start_page"] in live_starts else "stale (deleted note)"
        if slot is None:
            print(f"  page {note['pages'][0]:3d}..{note['pages'][-1]:3d}  {state:22s} "
                  f"name {name or '(name lost)':16s} no battery slot")
            continue
        seeded0 = slot_checksum_ok(slot, 0)
        print(f"  page {note['pages'][0]:3d}..{note['pages'][-1]:3d}  {state:22s} "
              f"name {name or '(name lost)':16s} "
              f"battery slot, checksums {'pak-seeded (0)' if seeded0 else 'NOT seed-0 valid'}")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, rest = argv[1], argv[2:]
    if cmd == "check" and rest:
        return cmd_check(rest)
    if cmd == "import" and len(rest) >= 2:
        return cmd_import(rest)
    if cmd == "export" and len(rest) == 2:
        return cmd_export(rest)
    if cmd == "pak" and rest:
        return cmd_pak(rest)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
