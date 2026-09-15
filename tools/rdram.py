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

Addresses accept `0x`-prefixed hex or bare hex (N64 addresses are conventionally
`0x80xxxxxx`; bare `8018FC39` works too).
"""

from __future__ import annotations

import struct
import sys

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


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    dump, mode = sys.argv[1], sys.argv[2]
    d = Dump(dump)
    if mode == "find":
        cmd_find(d, sys.argv[3])
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
