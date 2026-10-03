#!/usr/bin/env python3
"""Check the Item Randomizer's roster scan against the game's own `+0x02` byte.

`mods/item-randomizer/src/item_randomizer.c` reimplements `func_8016B774`, the
game's rebuild of the `+0x02` byte of both owned-item lists from the roster. That
byte is what the item screen prints first and what the equip test compares
(`sltu(+0x02, +0x03)`), so the mod writes it after it replaces an id. This tool
is the check on the reimplementation: it rebuilds the same column from an
`OGRE_DUMP_RDRAM` image and diffs it against the byte the game has in RAM.

A `mismatch` of 0 on a dump taken with the party's items loaded is what says the
mod's reconstruction is the game's answer.

    tools/refscan.py <dump>            # summary: counts and any mismatch
    tools/refscan.py <dump> --list     # every occupied record, scan vs game
    tools/refscan.py <dump> --item 117 # the records that credit one item

Addresses and offsets are the ones in the mod and in `docs/notes-item-equipped.md`
(the disassembly of `func_8016B774` at `0x8016B774`, size 0x29C, `.streamedB`):

    clear 0x80193AE2 + i*4   i = 39..0     the 40-slot list's `+0x02`
    clear 0x80196B22 + i*4   i = 277..0    the 278-slot list's `+0x02`
    units 0x80197210, stride 0x19, 30 records, gate `+0x01 & 1`,
          10 byte ids at `+0x0D`, searched in the 40-slot list
    chars 0x80193BE0, stride 0x38, 100 records, gate `+0x11`,
          four `lhu` ids at `+0x2A/2C/2E/30` and four class ids, searched in the
          278-slot list
    class 0x80187C62, stride 0x48, key byte `+0x79`, ids at `+0x00 + i*2`
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rdram import Dump  # noqa: E402

LIST_EQUIP = 0x80196B20
SLOTS_EQUIP = 278
LIST_CONS = 0x80193AE0
SLOTS_CONS = 40
STRIDE = 4
USE = 2

ROSTER_UNITS = 0x80197210
UNIT_STRIDE = 0x19
UNIT_COUNT = 30
UNIT_GATE = 0x01
UNIT_IDS = 0x0D
UNIT_ID_COUNT = 10

ROSTER_CHARS = 0x80193BE0
CHAR_STRIDE = 0x38
CHAR_COUNT = 100
CHAR_GATE = 0x11
CHAR_CLASS = 0x12
CHAR_IDS = 0x2A
CHAR_ID_COUNT = 4

CLASS_TABLE = 0x80187C62
CLASS_STRIDE = 0x48
CLASS_KEY = 0x79


# `Dump`'s accessors are the byte-order rules for an `OGRE_DUMP_RDRAM` image.
# `Dump.half` applies the XOR-2 as well as the XOR-3 (a guest halfword at an
# even address lives at `(addr & ~1) ^ 2`), which is the defect session 58 fixed
# in the in-app console; a local reimplementation of it here read 516 for 117.
class Reader:
    def __init__(self, path: str) -> None:
        self.dump = Dump(path)

    def byte(self, addr: int) -> int:
        return self.dump.byte(addr)

    def half(self, addr: int) -> int:
        return self.dump.half(addr)

    def word(self, addr: int) -> int:
        return self.dump.word(addr)


def name_of(reader: Reader, list_index: int, item_id: int) -> str:
    """The item's name, resolved the way each list's own reader does."""
    if list_index == 0:
        if item_id < 1 or item_id > 277:
            return "None" if item_id == 278 else "-"
        table, stride, name_off = 0x8018C42C, 0x20, 0x00
    else:
        if item_id < 1 or item_id > 40:
            return "-"
        table, stride, name_off = 0x8018E6EC, 0x0C, 0x00
    ptr = reader.word(table + item_id * stride + name_off)
    if not (0x80000000 <= ptr < 0x80800000):
        return "?"
    out = bytearray()
    off = reader.dump.off(ptr)
    while True:
        ch = reader.dump.logical[off]
        if ch == 0 or len(out) > 64:
            break
        out.append(ch)
        off += 1
    return out.decode("latin-1")


class Scan:
    def __init__(self, reader: Reader) -> None:
        self.reader = reader
        self.ref = ([0] * SLOTS_EQUIP, [0] * SLOTS_CONS)
        self.credit = {}
        self.chars = 0
        self.units = 0
        self._run()

    def _add(self, list_index: int, item_id: int, source: str) -> None:
        if item_id < 1:
            return
        limit = SLOTS_EQUIP if list_index == 0 else SLOTS_CONS
        if item_id >= limit:
            return
        self.ref[list_index][item_id] += 1
        self.credit.setdefault((list_index, item_id), []).append(source)

    def _class_id(self, index: int, cls: int, slot: int) -> int:
        r = self.reader
        entry = CLASS_TABLE + (cls & 0xFF) * CLASS_STRIDE
        if r.byte(CLASS_TABLE + index * CLASS_STRIDE + CLASS_KEY) == (cls & 0xFF):
            entry = CLASS_TABLE + index * CLASS_STRIDE
        return r.half(entry + slot * 2) & 0xFFFF

    def _run(self) -> None:
        r = self.reader
        for record in range(UNIT_COUNT):
            base = ROSTER_UNITS + record * UNIT_STRIDE
            if r.byte(base + UNIT_GATE) & 1 == 0:
                continue
            self.units += 1
            for i in range(UNIT_ID_COUNT):
                self._add(1, r.byte(base + UNIT_IDS + i), "unit%d+0x%X" % (record, UNIT_IDS + i))
        for record in range(CHAR_COUNT):
            base = ROSTER_CHARS + record * CHAR_STRIDE
            if r.byte(base + CHAR_GATE) == 0:
                continue
            self.chars += 1
            cls = r.byte(base + CHAR_CLASS)
            for i in range(CHAR_ID_COUNT):
                self._add(0, r.half(base + CHAR_IDS + i * 2) & 0xFFFF,
                          "char%d+0x%X" % (record, CHAR_IDS + i * 2))
            for i in range(CHAR_ID_COUNT):
                self._add(0, self._class_id(i, cls, i), "char%d class%d" % (record, i))


def sweep(reader: Reader, scan: Scan, show_list: bool) -> int:
    mismatches = 0
    for list_index, (base, slots) in enumerate(
        ((LIST_EQUIP, SLOTS_EQUIP), (LIST_CONS, SLOTS_CONS))
    ):
        occupied = 0
        for slot in range(slots):
            addr = base + slot * STRIDE
            item_id = reader.half(addr)
            if item_id == 0:
                continue
            occupied += 1
            use = reader.byte(addr + USE)
            ref = scan.ref[list_index][item_id] if item_id < len(scan.ref[list_index]) else -1
            flag = ""
            if ref != use:
                mismatches += 1
                flag = "  <-- MISMATCH"
            if show_list or flag:
                print("list%d slot %3d  id %4d  scan %3d  game %3d  %s%s"
                      % (list_index, slot, item_id, ref, use,
                         name_of(reader, list_index, item_id), flag))
        if show_list:
            print("list%d occupied %d" % (list_index, occupied))
    print("characters loaded %d, units loaded %d" % (scan.chars, scan.units))
    print("mismatches %d" % mismatches)
    return mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--list", action="store_true", help="print every occupied record")
    ap.add_argument("--item", type=lambda v: int(v, 0), default=None,
                    help="name the records that credit one item id")
    args = ap.parse_args()

    reader = Reader(args.dump)
    scan = Scan(reader)

    if args.item is not None:
        for list_index in (0, 1):
            key = (list_index, args.item)
            print("list%d item %d (%s): %d credit(s)"
                  % (list_index, args.item, name_of(reader, list_index, args.item),
                     scan.ref[list_index][args.item]
                     if args.item < len(scan.ref[list_index]) else 0))
            for source in scan.credit.get(key, []):
                print("   %s" % source)
        return 0

    return 1 if sweep(reader, scan, args.list) else 0


if __name__ == "__main__":
    sys.exit(main())
