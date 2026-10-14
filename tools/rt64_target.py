#!/usr/bin/env python3
"""Convert an RT64 `OGRE_CAPTURE_TARGET` readback into a PNG.

`OGRE_CAPTURE_TARGET=<path>` writes `<path>.<n>.bin`: the render target the VI
renderer sampled, at 8 bytes per texel (`docs/guides/app-build.md`, "Capturing
the native output"). On this machine the target is `R16G16B16A16_UNORM`, i.e.
four little-endian 16-bit channels per texel, so reading the first four bytes of
each texel as `R8G8B8A8` produces noise. This tool is the reader for that file.

The width and height are not in the file; give them on the command line. The
pixel count is `filesize / 8`, so a 320x240 target is 614400 bytes.

Usage:
  python3 tools/rt64_target.py /tmp/t.540.bin /tmp/t.540.png 320 240
  python3 tools/rt64_target.py <bin> <png> <w> <h> [--format u16|u8|f16]

  u16 (default)  four 16-bit unsigned normalized channels; the high byte is used
  u8             first four bytes as R,G,B,A (HDR off)
  f16            four IEEE half floats, clamped to 0..1
"""
import argparse
import struct
import sys
import zlib


def read_pixels(path: str, w: int, h: int, fmt: str) -> bytearray:
    data = open(path, "rb").read()
    need = w * h * 8
    if len(data) < need:
        raise SystemExit(f"{path}: {len(data)} bytes, need {need} for {w}x{h}")
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # PNG filter type 0
        for x in range(w):
            o = (y * w + x) * 8
            if fmt == "u16":
                r, g, b = struct.unpack_from("<3H", data, o)
                rows += bytes((r >> 8, g >> 8, b >> 8))
            elif fmt == "u8":
                rows += data[o : o + 3]
            else:
                r, g, b = struct.unpack_from("<3e", data, o)
                rows += bytes(
                    max(0, min(255, int(v * 255.0 + 0.5))) for v in (r, g, b)
                )
    return rows


def chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bin")
    ap.add_argument("png")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("--format", choices=("u16", "u8", "f16"), default="u16")
    args = ap.parse_args()

    rows = read_pixels(args.bin, args.width, args.height, args.format)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", args.width, args.height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    with open(args.png, "wb") as f:
        f.write(png)
    print(f"{args.bin} -> {args.png} ({args.width}x{args.height}, {args.format})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
