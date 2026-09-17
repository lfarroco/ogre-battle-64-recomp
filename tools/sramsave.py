#!/usr/bin/env python3
"""Import/export Ogre Battle 64 battery saves between emulators and the port.

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

This tool finds the SRAM by its magic at any offset and any of those byte
orders, so `import` does not need to be told the layout.

Usage:
    tools/sramsave.py check  <file> [...]        # identify + validate
    tools/sramsave.py import <in> <out.bin>      # -> logical 32 KiB for the port
    tools/sramsave.py export <in.bin> <out>      # -> 32 KiB byteswapped (.sra-like)

`import` never invents data: it refuses a file whose SRAM it cannot find, and
`check` says which byte order and container offset it detected.
"""

import sys

SRAM_SIZE = 0x8000
MAGIC = b"QuestOG3"
MAGIC_OFF_DEVICE = 0x04
MAGIC_OFF_SLOT0 = 0x14


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
    off = 0x10 + slot * 0x1850
    return int.from_bytes(sram[off:off + 4], "big")


def describe(path):
    data = open(path, "rb").read()
    print(f"{path}: {len(data)} bytes")
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
    print(f"  device magic at 0x04: {'ok' if device_ok else 'MISSING'}; "
          f"slot 0 magic at 0x14: {'ok' if slot0_ok else 'empty/absent'}"
          f" (slot 0 header word 0x{slot_header_word(sram):08X})")
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
    src, dst = args
    found = find_sram(open(src, "rb").read())
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


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, rest = argv[1], argv[2:]
    if cmd == "check" and rest:
        return cmd_check(rest)
    if cmd == "import" and len(rest) == 2:
        return cmd_import(rest)
    if cmd == "export" and len(rest) == 2:
        return cmd_export(rest)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
