#!/usr/bin/env python3
"""Convert a 16-bit byte-swapped N64 ROM (.n64) to big-endian (.z64)."""
import sys


def convert(src: str, dst: str) -> None:
    with open(src, "rb") as f:
        data = f.read()
    out = bytearray(len(data))
    for i in range(0, len(data) - 1, 2):
        out[i] = data[i + 1]
        out[i + 1] = data[i]
    with open(dst, "wb") as f:
        f.write(out)
    print(f"wrote {dst} ({len(data)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <in.n64> <out.z64>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
