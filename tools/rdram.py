#!/usr/bin/env python3
"""Read an `OGRE_DUMP_RDRAM=<path>` image with the runtime's byte order.

The runtime byte-reverses RDRAM (see `recomp.h`), so with `d` the image bytes:

    word  at guest address a : struct.unpack_from('<I', d, a & 0x1FFFFFFF)
    half  at guest address a : struct.unpack_from('<H', d, a & 0x1FFFFFFE)
    byte  at guest address a : d[(a & 0x1FFFFFFF) ^ 3]

(`docs/guides/app-build.md` -> "Reading a dump".) Every session re-derives this,
which is why it lives here.

Usage:
    tools/rdram.py <dump> word   <addr> [count]     # big-endian guest words
    tools/rdram.py <dump> half   <addr> [count]
    tools/rdram.py <dump> byte   <addr> [count]
    tools/rdram.py <dump> string <addr> [len]       # logical bytes, `@`-codes kept
    tools/rdram.py <dump> hexdump <addr> <len>
    tools/rdram.py <dump> find   <hexbytes|text>    # locate a byte pattern
    tools/rdram.py <dump> ptr    <addr> [depth]     # follow a guest pointer chain
    tools/rdram.py <dump> banks  [rom]              # which module is resident where

Addresses accept `0x`-prefixed hex or bare hex (N64 addresses are conventionally
`0x80xxxxxx`; bare `8018FC39` works too).

`banks` answers the one question that mis-binding walls turn on: *is the code in
RDRAM the code we compiled there?* The streamed-overlay arena at e.g.
`0x802395E0` holds different modules at different times (`bankRec14b` for scene
`0x0D`'s first visit, `bankRec14c` for the later steps), so "the port called the
wrong function" looks like exactly this: the bytes at the faulting address are a
*different module's*. For every record in `app/src/bank_funcs.inc`'s
`kBankRecords` (plus the uncompiled segment-table records in
`app/src/bank_overlays.cpp`) it compares the dump's logical bytes against the
ROM, prints the match ratio, and — when they differ — searches the ROM for the
bytes that *are* there and names the ROM offset they came from. The
`rom` argument defaults to `assets/ogre64.z64`.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

RDRAM_BASE = 0x80000000
RDRAM_SIZE = 0x800000


def parse_addr(text: str) -> int:
    text = text.strip()
    value = int(text, 16)
    if value < RDRAM_BASE:  # allow bare low addresses (RDRAM offsets)
        value += RDRAM_BASE
    return value


class Dump:
    def __init__(self, path: str) -> None:
        self.data = open(path, "rb").read()
        self.base = RDRAM_BASE
        # The whole dump in logical (guest) byte order: logical[off] ==
        # d.byte(0x80000000 + off). One transpose beats a per-byte accessor for
        # the whole-image scans `banks` does.
        self.logical = bytearray(len(self.data))
        self.logical[0::4] = self.data[3::4]
        self.logical[1::4] = self.data[2::4]
        self.logical[2::4] = self.data[1::4]
        self.logical[3::4] = self.data[0::4]

    def logical_at(self, addr: int, length: int) -> bytes:
        off = self.off(addr)
        return bytes(self.logical[off:off + length])

    def off(self, addr: int) -> int:
        off = addr - self.base
        if off < 0 or off >= len(self.data):
            raise ValueError("0x%08X is outside the dump (%d bytes)" % (addr, len(self.data)))
        return off

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, self.off(addr))[0]

    def half(self, addr: int) -> int:
        return struct.unpack_from("<H", self.data, self.off(addr & ~1))[0]

    def byte(self, addr: int) -> int:
        return self.data[self.off(addr) ^ 3]

    def raw(self, addr: int, length: int) -> bytes:
        # Logical bytes are XOR-3 within their word, so decode byte-wise.
        return bytes(self.byte(addr + i) for i in range(length))


def cmd_word(d: Dump, addr: int, count: int, size: str) -> None:
    read = {"word": d.word, "half": d.half, "byte": d.byte}[size]
    width = {"word": 8, "half": 4, "byte": 2}[size]
    step = {"word": 4, "half": 2, "byte": 1}[size]
    for i in range(count):
        a = addr + i * step
        value = read(a)
        extra = ""
        if size == "word":
            as_float = struct.unpack("<f", struct.pack("<I", value))[0]
            as_addr = " -> 0x%08X" % value if 0x80000000 <= value < 0x80800000 else ""
            extra = "  f=%g%s" % (as_float, as_addr)
        print("0x%08X = 0x%0*X%s" % (a, width, value, extra))


def cmd_string(d: Dump, addr: int, length: int) -> None:
    raw = d.raw(addr, length)
    print(repr(raw))
    print("".join(chr(c) if 32 <= c < 127 else "." for c in raw))


def cmd_hexdump(d: Dump, addr: int, length: int) -> None:
    for row in range(0, length, 16):
        chunk = d.raw(addr + row, min(16, length - row))
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print("0x%08X  %-47s  %s" % (addr + row, " ".join("%02X" % c for c in chunk), text))


def cmd_find(d: Dump, pattern: str) -> None:
    if all(c in "0123456789abcdefABCDEF" for c in pattern) and len(pattern) % 2 == 0:
        needle = bytes.fromhex(pattern)
    else:
        needle = pattern.encode()
    hits = 0
    start = 0
    while True:
        at = d.data.find(needle, start)
        if at < 0:
            break
        hits += 1
        addr = d.base + at
        # Which guest view is this? Words are stored byte-reversed, so a raw
        # search hits the logical-byte view at (addr ^ 3) for single bytes.
        print("0x%08X (file offset 0x%06X, logical view 0x%08X)"
              % (addr, at, (addr & ~3) ^ 3))
        if hits >= 40:
            print("... (stopping at 40 hits)")
            break
        start = at + 1
    print("%d hit(s)" % hits)


def cmd_ptr(d: Dump, addr: int, depth: int) -> None:
    current = addr
    for i in range(max(1, depth)):
        value = d.word(current)
        in_rdram = 0x80000000 <= value < 0x80800000
        print("[%d] *(0x%08X) = 0x%08X%s" % (i, current, value, "  (guest pointer)" if in_rdram else ""))
        if not in_rdram:
            break
        current = value


# --- `banks`: is the code in RDRAM the module we compiled there? -------------

ROOT = Path(__file__).resolve().parent.parent
BANK_RECORDS_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)u?,\s*\(int32_t\)0x([0-9A-Fa-f]+)u?,\s*0x([0-9A-Fa-f]+)u?,"
    r"\s*\(int32_t\)0x([0-9A-Fa-f]+)u?,"
)
STREAMED_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)u?,\s*\(int32_t\)0x([0-9A-Fa-f]+)u?,\s*0x([0-9A-Fa-f]+)u?,\s*(true|false)\s*\}"
)


def module_table() -> list[tuple[int, int, int, str]]:
    """(rom_start, ram_start, size, label) for every module the port knows."""
    out: list[tuple[int, int, int, str]] = []
    inc = ROOT / "app" / "src" / "bank_funcs.inc"
    if inc.exists():
        text = inc.read_text()
        for m in BANK_RECORDS_RE.finditer(text):
            rom, ram, size, _end = (int(m.group(i), 16) for i in range(1, 5))
            out.append((rom, ram, size, "compiled"))
    app = ROOT / "app" / "src" / "bank_overlays.cpp"
    if app.exists():
        text = app.read_text()
        for m in STREAMED_RE.finditer(text):
            rom, ram, size = (int(m.group(i), 16) for i in range(1, 4))
            compiled = m.group(4) == "true"
            if not compiled and not any(r == rom and a == ram for r, a, _, _ in out):
                out.append((rom, ram, size, "UNCOMPILED"))
    return out


def cmd_banks(d: Dump, rom_path: str) -> None:
    try:
        rom = open(rom_path, "rb").read()
    except OSError as exc:
        print("cannot read ROM %s: %s" % (rom_path, exc))
        return
    modules = module_table()
    if not modules:
        print("no module table: build the bank units first (app/src/bank_funcs.inc)")
        return
    probe = 0x1000
    print("dump %d bytes; %d module(s)" % (len(d.data), len(modules)))
    by_ram: dict[int, list[tuple[int, int, str]]] = {}
    for rom_off, ram, size, label in modules:
        by_ram.setdefault(ram, []).append((rom_off, size, label))
    for ram in sorted(by_ram):
        n = min(probe, min(size for _, size, _ in by_ram[ram]))
        try:
            live = d.logical_at(ram, n)
        except ValueError:
            print("0x%08X  not in the dump" % ram)
            continue
        hits = []
        for rom_off, size, label in by_ram[ram]:
            want = rom[rom_off:rom_off + n]
            same = sum(1 for a, b in zip(live, want) if a == b)
            hits.append((same / len(want) if want else 0.0, rom_off, size, label))
        hits.sort(reverse=True)
        best, best_rom, best_size, best_label = hits[0]
        line = "0x%08X  " % ram
        if best > 0.999:
            line += "resident: rom=0x%06X (%s, %d bytes, %d/%d bytes match)" % (
                best_rom, best_label, best_size, int(best * n), n)
        else:
            line += "NOT any known module (best 0x%06X %s, %.0f%% match)" % (
                best_rom, best_label, best * 100.0)
            # Name what *is* there: search the ROM for the live bytes. A window
            # that is all one byte (typically zeroed RAM) matches the ROM
            # anywhere, so only search a window with some variety in it.
            window = live[:64]
            if len(set(window)) > 2:
                at = rom.find(window)
                if at >= 0 and at <= ram and (ram - at) >= RDRAM_BASE:
                    base = ram - at
                    known = [m for m in modules if m[0] == at]
                    if known:
                        line += "\n             live bytes are ROM 0x%06X = the *start* of %s at RAM 0x%08X" % (
                            at, known[0][3], known[0][1])
                    else:
                        line += "\n             live bytes are ROM 0x%06X, i.e. a module whose ROM start is 0x%06X at RAM 0x%08X" % (
                            at, at, base)
                elif at >= 0:
                    line += "\n             live bytes occur at ROM 0x%06X but not as a module base (coincidental match)" % at
                elif at >= 0:
                    line += "\n             live bytes occur at ROM 0x%06X but not at or before this RAM" % at
                else:
                    line += "\n             live bytes are not in the ROM (patched, zeroed or not a ROM module)"
            else:
                line += "\n             live bytes are uniform (0x%02X): nothing loaded here" % (window[0] if window else 0)
        print(line)
        for ratio, rom_off, size, label in hits[1:]:
            if ratio > 0.999:
                print("             also matches rom=0x%06X (%s)" % (rom_off, label))


def main() -> int:
    if len(sys.argv) < 3 or (len(sys.argv) < 4 and sys.argv[2] != "banks"):
        print(__doc__)
        return 2
    dump, mode = sys.argv[1], sys.argv[2]
    d = Dump(dump)
    if mode == "find":
        cmd_find(d, sys.argv[3])
        return 0
    if mode == "banks":
        cmd_banks(d, sys.argv[3] if len(sys.argv) > 3 else str(ROOT / "assets" / "ogre64.z64"))
        return 0
    addr = parse_addr(sys.argv[3])
    if mode == "string":
        cmd_string(d, addr, int(sys.argv[4], 0) if len(sys.argv) > 4 else 64)
    elif mode == "hexdump":
        cmd_hexdump(d, addr, int(sys.argv[4], 0) if len(sys.argv) > 4 else 64)
    elif mode == "ptr":
        cmd_ptr(d, addr, int(sys.argv[4], 0) if len(sys.argv) > 4 else 4)
    elif mode in ("word", "half", "byte"):
        cmd_word(d, addr, int(sys.argv[4], 0) if len(sys.argv) > 4 else 1, mode)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
