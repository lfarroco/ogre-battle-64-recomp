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
        self.path = path
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
        # A halfword needs the XOR-2 as well: guest bytes are reversed inside
        # each word (logical byte at `a` == `data[off(a) ^ 3]`), so the logical
        # halfword at an even `a` lives at `(a & ~1) ^ 2`. Reading `a & ~1`
        # returns the *neighbouring* halfword — the same defect the in-app
        # console had; session 58 fixed both.
        return struct.unpack_from("<H", self.data, self.off((addr & ~1) ^ 2))[0]

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


# --- `image`: render a region as an N64 texture ------------------------------
#
# Every session writes this loop again ("is the buffer zero, or is it an image
# someone forgot to draw?"). Formats match the RDP's `G_IM_FMT`/`G_IM_SIZ`.
# The output is an RGBA PNG (alpha preserved, so an IA/I image is not silently
# premultiplied to black), written with zlib only — no Pillow.

IMAGE_FORMATS = {
    "rgba16": (2.0, "RGBA 16b (5-5-5-1)"),
    "rgba32": (4.0, "RGBA 32b (8-8-8-8)"),
    "ia16": (4.0, "IA 16b"),
    "ia8": (1.0, "IA 8b (4+4)"),
    "ia4": (0.5, "IA 4b (3+1)"),
    "i8": (1.0, "I 8b"),
    "i4": (0.5, "I 4b"),
    "ci8": (1.0, "CI 8b (palette RGBA16)"),
}


def write_png_rgba(path: str, width: int, height: int, rgba: bytes) -> None:
    import zlib

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    blob = b"\x89PNG\r\n\x1a\n"
    blob += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    blob += chunk(b"IDAT", zlib.compress(raw, 6))
    blob += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(blob)


def texel_decoder(fmt: str, d: Dump, addr: int, palette: int | None):
    """Return (bytes_per_texel, decode(i) -> (r, g, b, a)) for a region."""
    def byte_at(i: int) -> int:
        return d.byte(addr + i)

    def rgb5551(v: int) -> tuple[int, int, int, int]:
        return (((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31,
                ((v >> 1) & 31) * 255 // 31, 255 if (v & 1) else 0)

    if fmt == "rgba16":
        return 2.0, lambda i: rgb5551((byte_at(i * 2) << 8) | byte_at(i * 2 + 1))
    if fmt == "rgba32":
        return 4.0, lambda i: (byte_at(i * 4), byte_at(i * 4 + 1),
                               byte_at(i * 4 + 2), byte_at(i * 4 + 3))
    if fmt == "ia16":
        return 4.0, lambda i: (byte_at(i * 4),) * 3 + (byte_at(i * 4 + 2),)
    if fmt == "ia8":
        def ia8(i: int) -> tuple[int, int, int, int]:
            v = byte_at(i)
            intensity = ((v >> 4) & 15) * 17
            return (intensity, intensity, intensity, (v & 15) * 17)
        return 1.0, ia8
    if fmt == "i8":
        return 1.0, lambda i: (byte_at(i),) * 3 + (255,)
    if fmt in ("i4", "ia4"):
        def nibble(i: int) -> int:
            v = byte_at(i // 2)
            return (v >> 4) & 15 if i % 2 == 0 else v & 15

        def i4(i: int) -> tuple[int, int, int, int]:
            v = nibble(i) * 17
            return (v, v, v, 255)

        def ia4(i: int) -> tuple[int, int, int, int]:
            v = nibble(i)
            intensity = ((v >> 1) & 7) * 36
            return (intensity, intensity, intensity, 255 if (v & 1) else 0)

        return 0.5, (ia4 if fmt == "ia4" else i4)
    if fmt == "ci8":
        if palette is None:
            raise SystemExit("ci8 needs --palette <addr>")
        return 1.0, lambda i: rgb5551(d.half(palette + byte_at(i) * 2))
    raise SystemExit("unknown --fmt %r (choose from %s)" % (fmt, ", ".join(IMAGE_FORMATS)))


def cmd_image(d: Dump, addr: int, width: int, height: int | None, fmt: str,
              offset: int, palette: int | None, out: str | None, scale: int) -> None:
    per_texel, decode = texel_decoder(fmt, d, addr + offset, palette)
    avail = len(d.data) - d.off(addr + offset)
    rows = int(avail // (per_texel * width))
    height = rows if not height else min(height, rows)
    if height <= 0:
        raise SystemExit("region too small for %d texels/row" % width)
    pixels = bytearray()
    values = []
    for y in range(height):
        for x in range(width):
            i = y * width + x
            r, g, b, a = decode(i)
            values.append((r + g + b) // 3)
            pixels += bytes((r, g, b, a))
    if scale > 1:
        scaled = bytearray()
        for y in range(height):
            row = pixels[y * width * 4:(y + 1) * width * 4]
            big = b"".join(row[x * 4:x * 4 + 4] * scale for x in range(width))
            for _ in range(scale):
                scaled += big
        pixels = scaled
        height *= scale
        width *= scale
    if out is None:
        out = "%s.0x%08X.%s.png" % (d.path, addr, fmt)
    write_png_rgba(out, width, height, bytes(pixels))

    uniq = len(set(values))
    mean = sum(values) / len(values)
    dark = sum(1 for v in values if v < 8) / len(values)
    print("%s  %s, %dx%d from 0x%08X+0x%X (%d row(s) available)  ->  %s"
          % (IMAGE_FORMATS[fmt][1], fmt, width // scale, height // scale, addr, offset,
             rows, out))
    print("  luminance: mean %.1f, min %d, max %d, %d unique value(s), %.1f%% near-black"
          % (mean, min(values), max(values), uniq, dark * 100.0))
    if uniq <= 2:
        print("  NOTE: (near-)uniform — this is a blank/fill buffer, not an image")


# --- `diff`: what did a run change, grouped by module ------------------------

def changed_runs(a: bytes, b: bytes, gap: int) -> list[tuple[int, int]]:
    """[start, end) byte runs where the two images differ, merging close runs."""
    runs: list[tuple[int, int]] = []
    i = 0
    n = min(len(a), len(b))
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        while i < n:
            if a[i] != b[i]:
                i += 1
                continue
            # Close a run only when `gap` equal bytes follow.
            j = i
            while j < n and j - i < gap and a[j] == b[j]:
                j += 1
            if j - i >= gap or j >= n:
                break
            i = j
        runs.append((start, i))
    return runs


def cmd_diff(path_a: str, path_b: str, min_run: int, limit: int, gap: int,
             as_json: bool) -> None:
    da, db = Dump(path_a), Dump(path_b)
    runs = [r for r in changed_runs(da.logical, db.logical, gap) if r[1] - r[0] >= min_run]
    total = sum(e - s for s, e in runs)
    print("A %s (%d bytes)" % (path_a, len(da.data)))
    print("B %s (%d bytes)" % (path_b, len(db.data)))
    if len(da.data) != len(db.data):
        print("WARNING: different sizes; compared the common prefix")
    print("%d byte(s) changed (%.2f%% of the dump) in %d run(s) (min-run %d, gap %d)"
          % (total, 100.0 * total / max(1, len(da.data)), len(runs), min_run, gap))

    try:
        import n64map
        m = n64map.load_map()
    except Exception:                                    # noqa: BLE001 - optional
        m = None

    def owner(addr: int) -> str:
        if m is None:
            return ""
        seg = m.by_vram(addr)
        if seg is None:
            if addr < 0x80001000:
                return "low RDRAM (exception vectors / KUSEG alias)"
            return "heap/stack/asset space"
        shared = [s for s in m.segments if s is not seg and s.vram is not None
                  and s.kind == "record" and s.vram <= addr < s.vram_end]
        label = seg.label()
        if shared:
            label += " (shared by %d record(s))" % len(shared)
        return label

    groups: dict[str, list[int]] = {}
    for start, end in runs:
        key = owner(0x80000000 + start)
        entry = groups.setdefault(key or "(unmapped)", [0, 0])
        entry[0] += end - start
        entry[1] += 1
    print("\nby region:")
    for key, (bytes_, count) in sorted(groups.items(), key=lambda kv: -kv[1][0]):
        print("  %10d byte(s)  %4d run(s)  %s" % (bytes_, count, key))

    if as_json:
        import json
        print(json.dumps([{"start": 0x80000000 + s, "end": 0x80000000 + e,
                           "owner": owner(0x80000000 + s)} for s, e in runs[:limit]]))
        return

    print("\nruns (first %d):" % limit)
    for start, end in runs[:limit]:
        addr = 0x80000000 + start
        first_a, first_b = da.byte(addr), db.byte(addr)
        print("  0x%08X..0x%08X  0x%-6X  A[0]=%02X B[0]=%02X  %s"
              % (addr, 0x80000000 + end, end - start, first_a, first_b,
                 owner(addr)))
    if len(runs) > limit:
        print("  ... %d more" % (len(runs) - limit))


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
    argv = sys.argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[0] == "diff":
        return main_diff(argv[1:])
    if len(argv) < 2 or (len(argv) < 3 and argv[1] != "banks"):
        print(__doc__)
        return 2
    dump, mode = argv[0], argv[1]
    d = Dump(dump)
    if mode == "find":
        cmd_find(d, argv[2])
        return 0
    if mode == "banks":
        cmd_banks(d, argv[2] if len(argv) > 2 else str(ROOT / "assets" / "ogre64.z64"))
        return 0
    if mode == "image":
        return main_image(d, argv[2:])
    addr = parse_addr(argv[2])
    if mode == "string":
        cmd_string(d, addr, int(argv[3], 0) if len(argv) > 3 else 64)
    elif mode == "hexdump":
        cmd_hexdump(d, addr, int(argv[3], 0) if len(argv) > 3 else 64)
    elif mode == "ptr":
        cmd_ptr(d, addr, int(argv[3], 0) if len(argv) > 3 else 4)
    elif mode in ("word", "half", "byte"):
        cmd_word(d, addr, int(argv[3], 0) if len(argv) > 3 else 1, mode)
    else:
        print(__doc__)
        return 2
    return 0


def main_image(d: Dump, args: list[str]) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="rdram.py <dump> image",
                                 description="Render an RDRAM region as an N64 texture (PNG).")
    ap.add_argument("addr")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240,
                    help="texels to render (default 240; 0 = as many as fit)")
    ap.add_argument("--fmt", default="rgba16", choices=sorted(IMAGE_FORMATS))
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0,
                    help="bytes to skip before the first texel (e.g. a header)")
    ap.add_argument("--palette", type=lambda s: parse_addr(s), default=None,
                    help="RGBA16 palette address (ci8)")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--scale", type=int, default=1)
    ns = ap.parse_args(args)
    cmd_image(d, parse_addr(ns.addr), ns.width, ns.height, ns.fmt, ns.offset,
              ns.palette, ns.out, ns.scale)
    return 0


def main_diff(args: list[str]) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="rdram.py diff",
                                 description="What did a run change, grouped by module.")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--min-run", type=int, default=1)
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("--gap", type=int, default=4,
                    help="merge runs separated by fewer than N equal bytes")
    ap.add_argument("--json", action="store_true")
    ns = ap.parse_args(args)
    cmd_diff(ns.a, ns.b, ns.min_run, ns.limit, ns.gap, ns.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
