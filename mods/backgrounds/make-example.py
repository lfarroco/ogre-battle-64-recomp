#!/usr/bin/env python3
"""Write the example HD backdrop `01.png` (the cathedral mapping).

This is a placeholder for real art. It exists so the pack has something to
replace the cathedral with, and so the swap is unmistakable on screen: the
image is 1280x960 (4x the game's 320x240 backdrop), and a large "01" marks it.

    python3 mods/backgrounds/make-example.py

No third-party modules are used; the PNG is written with `zlib` and `struct`.
Replace `01.png` with real art and this script is no longer needed.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

W, H = 1280, 960


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    # One filter byte (0 = None) per row.
    raw = b"".join(
        b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height)
    )

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    px = bytearray(W * H * 4)
    cx = W / 2.0

    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 4
            t = y / H
            # Night sky, darker at the top.
            r = int(16 + 34 * t)
            g = int(12 + 24 * t)
            b = int(46 + 78 * t)

            # A warm glow behind the altar, centred a third of the way up.
            dx = (x - cx) / (W * 0.45)
            dy = (y - H * 0.62) / (H * 0.42)
            glow = max(0.0, 1.0 - (dx * dx + dy * dy))
            r += int(120 * glow * glow)
            g += int(70 * glow * glow)
            b += int(20 * glow * glow)

            # A pointed arch: two circles plus a vertical span between them.
            ax = abs(x - cx)
            arch_outer = ax < W * 0.24 and y > H * 0.16
            if y < H * 0.46:
                arch_outer = arch_outer and (
                    (x - cx + W * 0.16) ** 2 + (y - H * 0.46) ** 2 < (W * 0.16) ** 2
                    or (x - cx - W * 0.16) ** 2 + (y - H * 0.46) ** 2 < (W * 0.16) ** 2
                )
            if arch_outer:
                # Stained glass: coloured panes on a leaded grid.
                u = int((x - (cx - W * 0.20)) / (W * 0.05))
                v = int((y - H * 0.20) / (H * 0.06))
                pane = (u * 7 + v * 3) % 6
                cols = [
                    (210, 60, 70), (235, 170, 40), (60, 190, 120),
                    (70, 130, 220), (170, 90, 210), (240, 120, 60),
                ]
                r, g, b = cols[pane]
                # Lead lines.
                if (x - (cx - W * 0.20)) % (W * 0.05) < 8 or (y - H * 0.20) % (H * 0.06) < 8:
                    r = g = b = 20
                # Inner dark surround.
                inner = ax < W * 0.185 and y > H * 0.19
                if y < H * 0.46 and inner:
                    inner = (
                        (x - cx + W * 0.12) ** 2 + (y - H * 0.46) ** 2 < (W * 0.12) ** 2
                        or (x - cx - W * 0.12) ** 2 + (y - H * 0.46) ** 2 < (W * 0.12) ** 2
                    )
                if not inner:
                    r = int(r * 0.35)
                    g = int(g * 0.35)
                    b = int(b * 0.45)

            # Two side pillars.
            for px0 in (W * 0.11, W * 0.83):
                if px0 - W * 0.035 < x < px0 + W * 0.035 and y > H * 0.20:
                    shade = 70 + int(30 * (1.0 - abs(x - px0) / (W * 0.035)))
                    r, g, b = shade, shade - 12, shade + 10

            # Bright frame, so a swap is obvious at a glance.
            if x < 14 or x >= W - 14 or y < 14 or y >= H - 14:
                r, g, b = 255, 0, 200

            # "01", drawn with rectangles, bottom right.
            glyphs = [
                # x0, y0, w, h for the strokes of '0' and '1'
                (0.62, 0.78, 0.030, 0.004), (0.62, 0.78, 0.004, 0.140),
                (0.646, 0.78, 0.004, 0.140), (0.62, 0.916, 0.030, 0.004),
                (0.72, 0.78, 0.024, 0.004), (0.728, 0.784, 0.008, 0.136),
                (0.706, 0.892, 0.046, 0.006),
            ]
            for gx, gy, gw, gh in glyphs:
                if gx * W <= x < (gx + gw) * W and gy * H <= y < (gy + gh) * H:
                    r, g, b = 255, 255, 255

            px[i] = min(255, max(0, r))
            px[i + 1] = min(255, max(0, g))
            px[i + 2] = min(255, max(0, b))
            px[i + 3] = 255

    out = Path(__file__).resolve().parent / "01.png"
    write_png(out, W, H, bytes(px))
    print(f"wrote {out} ({W}x{H})")


if __name__ == "__main__":
    main()
