#!/usr/bin/env python3
"""Decompress an OB64 LZ block (the format `func_8007A110` implements).

Format, read off the disassembly at `0x8007A110` (`build/ogrebattle64.elf`):

    u32be decompressed_size
    then tokens, each introduced by one flag byte `b`:
      b & 0x80 : back-reference, length = ((b >> 3) & 0xF) + 3,
                 offset = ((b & 7) << 8) | next_byte
      b & 0x40 : literal run,    length = (b & 0x3F) + 1
      b & 0x20 : zero run,       length = (b & 0x1F) + 2
      b & 0x10 : long back-ref,  length = 4 + (b & 0xF) + ((hi >> 2) & 0x30),
                 offset = ((hi & 0x3F) << 8) | lo        (two bytes follow)
      b == 0   : far back-ref,   length = next_byte + 5,
                 offset = (next_byte << 8) | next_byte    (three bytes follow)
      b == 1   : 0xFF-fill run of `next_byte + 3`
      b == 2   : zero run of `next_byte + 3`
    Back-references copy from `dst - offset - 1` (overlapping copies are legal
    and must be byte-at-a-time).

Usage: ogrelz.py <rom> <rom_start_hex> [size_hex]   (dumps the text it finds)
"""

from __future__ import annotations

import re
import sys


def decompress(src: bytes, pos: int, limit: int = 0x400000) -> tuple[bytes, int]:
    size = int.from_bytes(src[pos:pos + 4], "big")
    if size == 0 or size > limit:
        raise ValueError("implausible size 0x%X" % size)
    p = pos + 4
    out = bytearray()
    while len(out) < size:
        if p >= len(src):
            raise ValueError("ran off the end at %d/%d" % (len(out), size))
        b = src[p]
        p += 1
        if b & 0x80:
            length = ((b >> 3) & 0xF) + 3
            off = ((b & 7) << 8) | src[p]
            p += 1
            s = len(out) - off - 1
            if s < 0:
                raise ValueError("back-ref before start")
            for i in range(length):
                out.append(out[s + i])
        elif b & 0x40:
            length = (b & 0x3F) + 1
            out += src[p:p + length]
            p += length
        elif b & 0x20:
            out += b"\x00" * ((b & 0x1F) + 2)
        elif b & 0x10:
            hi = src[p]
            lo = src[p + 1]
            p += 2
            off = ((hi & 0x3F) << 8) | lo
            length = 4 + (b & 0x0F) + ((hi >> 2) & 0x30)
            s = len(out) - off - 1
            if s < 0:
                raise ValueError("back-ref before start")
            for i in range(length):
                out.append(out[s + i])
        elif b == 0:
            n = src[p]
            hi = src[p + 1]
            lo = src[p + 2]
            p += 3
            off = (hi << 8) | lo
            s = len(out) - off - 1
            if s < 0:
                raise ValueError("back-ref before start")
            for i in range(n + 5):
                out.append(out[s + i])
        elif b == 1:
            n = src[p]
            p += 1
            out += b"\xff" * (n + 3)
        elif b == 2:
            n = src[p]
            p += 1
            out += b"\x00" * (n + 3)
        else:
            raise ValueError("unknown token 0x%02X" % b)
    return bytes(out[:size]), p


# Well-known OB64 text bytes (the ASCII story/prologue blocks are uncompressed,
# so a decoded text bank should contain the same `@`-control-code vocabulary).
TEXT_RE = re.compile(rb"[\x20-\x7e@\r\n\t]{16,}")


def looks_like_text(data: bytes) -> bool:
    printable = sum(1 for c in data if 0x20 <= c < 0x7F or c in (0x0A, 0x0D, 0x09, 0x00))
    return printable >= len(data) * 0.85


def scan(src: bytes, start: int, end: int, min_size: int = 0x40, max_size: int = 0x40000):
    hits = []
    for p in range(start, end - 4, 1):
        size = int.from_bytes(src[p:p + 4], "big")
        if size < min_size or size > max_size:
            continue
        try:
            out, _ = decompress(src, p)
        except Exception:
            continue
        if len(out) != size or not looks_like_text(out):
            continue
        hits.append((p, size, out))
    return hits


def asset_rom(asset_id: int) -> int:
    """ROM offset of an asset's 4-byte size header.

    `func_8009DAF4` computes exactly this (`(id & 0x0FFFFFFF) + 0x594250`, from
    `lui v0,0x59` / `addiu v0,v0,0x4250` at `0x8009DB50`), reads the word there
    and returns it. That word is the asset's *payload* size: the loader
    (`func_8009DBB8`) copies `size` bytes starting at `rom + 4` into a buffer,
    and the LZ block (`func_8007A7E0` size, `func_8007A110` decode) therefore
    starts at **rom + 4**, not at rom. Session 44 measured this; starting the
    decoder at rom decodes nothing (every block begins 00 00 xx xx).
    """
    return (asset_id & 0x0FFFFFFF) + 0x594250


def decompress_asset(src: bytes, asset_id: int):
    """Decompress one game asset by id -> (bytes, payload_size, lz_size)."""
    rom = asset_rom(asset_id)
    payload_size = int.from_bytes(src[rom:rom + 4], "big")
    out, _ = decompress(src, rom + 4)
    return out, payload_size, rom


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--asset":
        rom = sys.argv[2]
        src = open(rom, "rb").read()
        for arg in sys.argv[3:]:
            asset_id = int(arg, 16)
            rom_off = asset_rom(asset_id)
            try:
                out, payload, _ = decompress_asset(src, asset_id)
            except Exception as exc:
                print("asset 0x%08X rom 0x%06X: %s" % (asset_id, rom_off, exc))
                continue
            print("=== asset 0x%08X rom 0x%06X payload 0x%X -> 0x%X bytes ==="
                  % (asset_id, rom_off, payload, len(out)))
            for m in TEXT_RE.finditer(out):
                print("   %s" % m.group().decode("ascii", "replace"))
            if not TEXT_RE.search(out):
                line = " ".join("%02X" % b for b in out[:32])
                print("   (no text; first 32 bytes: %s)" % line)
        return 0

    rom, start = sys.argv[1], int(sys.argv[2], 16)
    size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x20000
    src = open(rom, "rb").read()
    hits = scan(src, start, start + size)
    print("ogrelz: %d text block(s) in ROM 0x%X..0x%X" % (len(hits), start, start + size))
    for p, n, out in hits:
        print("\n=== ROM 0x%06X -> 0x%X bytes ===" % (p, n))
        for m in TEXT_RE.finditer(out):
            print("   %s" % m.group().decode("ascii", "replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
