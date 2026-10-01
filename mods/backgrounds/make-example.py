#!/usr/bin/env python3
"""Write the example HD backdrops `01.png` and `01-wide.png`.

Two images per mapping:

* `01.png` is the plain one, authored at the canvas aspect (1280x960 here).
* `01-wide.png` is the widescreen one, authored at 16:9 (1920x1080). The port
  scales the wide image to the canvas width without stretching it, so a 16:9
  source is not squeezed to the canvas shape.

Both are placeholders for real art. The wide one draws a round rose window as an
aspect check: it should look circular on screen.

    python3 mods/backgrounds/make-example.py            # write what is missing
    python3 mods/backgrounds/make-example.py --force    # overwrite

The game's backdrop canvas is 496x384. No third-party modules are used; the PNGs
are written with `zlib` and `struct`. Replace the images with real art and this
script is no longer needed.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The game's backdrop canvas (`kCanvasW`/`kCanvasH` in app/src/hd_backgrounds.cpp).
CANVAS_W, CANVAS_H = 496, 384


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


# The "01" label, as (x, y, w, h) fractions of the image.
LABEL = [
    (0.62, 0.78, 0.030, 0.004), (0.62, 0.78, 0.004, 0.140),
    (0.646, 0.78, 0.004, 0.140), (0.62, 0.916, 0.030, 0.004),
    (0.72, 0.78, 0.024, 0.004), (0.728, 0.784, 0.008, 0.136),
    (0.706, 0.892, 0.046, 0.006),
]

# The wide art keeps the label clear of the rose window.
LABEL_WIDE = [(gx + 0.18, gy, gw, gh) for gx, gy, gw, gh in LABEL]

PANE_COLS = [
    (210, 60, 70), (235, 170, 40), (60, 190, 120),
    (70, 130, 220), (170, 90, 210), (240, 120, 60),
]


def in_label(x: int, y: int, w: int, h: int, label=LABEL) -> bool:
    for gx, gy, gw, gh in label:
        if gx * w <= x < (gx + gw) * w and gy * h <= y < (gy + gh) * h:
            return True
    return False


def render_standard(w: int, h: int) -> bytes:
    """The plain placeholder: a pointed stained-glass arch."""
    px = bytearray(w * h * 4)
    cx = w / 2.0
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            t = y / h
            r = int(16 + 34 * t)
            g = int(12 + 24 * t)
            b = int(46 + 78 * t)

            dx = (x - cx) / (w * 0.45)
            dy = (y - h * 0.62) / (h * 0.42)
            glow = max(0.0, 1.0 - (dx * dx + dy * dy))
            r += int(120 * glow * glow)
            g += int(70 * glow * glow)
            b += int(20 * glow * glow)

            ax = abs(x - cx)
            arch_outer = ax < w * 0.24 and y > h * 0.16
            if y < h * 0.46:
                arch_outer = arch_outer and (
                    (x - cx + w * 0.16) ** 2 + (y - h * 0.46) ** 2 < (w * 0.16) ** 2
                    or (x - cx - w * 0.16) ** 2 + (y - h * 0.46) ** 2 < (w * 0.16) ** 2
                )
            if arch_outer:
                u = int((x - (cx - w * 0.20)) / (w * 0.05))
                v = int((y - h * 0.20) / (h * 0.06))
                r, g, b = PANE_COLS[(u * 7 + v * 3) % 6]
                if (x - (cx - w * 0.20)) % (w * 0.05) < 8 or (y - h * 0.20) % (h * 0.06) < 8:
                    r = g = b = 20
                inner = ax < w * 0.185 and y > h * 0.19
                if y < h * 0.46 and inner:
                    inner = (
                        (x - cx + w * 0.12) ** 2 + (y - h * 0.46) ** 2 < (w * 0.12) ** 2
                        or (x - cx - w * 0.12) ** 2 + (y - h * 0.46) ** 2 < (w * 0.12) ** 2
                    )
                if not inner:
                    r = int(r * 0.35)
                    g = int(g * 0.35)
                    b = int(b * 0.45)

            for px0 in (w * 0.11, w * 0.83):
                if px0 - w * 0.035 < x < px0 + w * 0.035 and y > h * 0.20:
                    shade = 70 + int(30 * (1.0 - abs(x - px0) / (w * 0.035)))
                    r, g, b = shade, shade - 12, shade + 10

            if x < 14 or x >= w - 14 or y < 14 or y >= h - 14:
                r, g, b = 255, 0, 200
            if in_label(x, y, w, h):
                r, g, b = 255, 255, 255

            px[i] = min(255, max(0, r))
            px[i + 1] = min(255, max(0, g))
            px[i + 2] = min(255, max(0, b))
            px[i + 3] = 255
    return bytes(px)


def render_wide(w: int, h: int) -> bytes:
    """The widescreen placeholder: a round rose window between two towers.

    The rose is a circle, which is also the aspect check: it should look
    circular on screen.
    """
    px = bytearray(w * h * 4)
    cx = w / 2.0
    # The wide view shows roughly canvas y 0.34..1.0, so the rose sits low
    # enough to be whole.
    rose_cy = h * 0.63
    rose_r = h * 0.24
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            t = y / h
            # Night sky, a little warmer near the floor.
            r = int(14 + 40 * t)
            g = int(10 + 26 * t)
            b = int(44 + 70 * t)

            # Floor: red carpet between stone margins.
            if y > h * 0.78:
                r, g, b = 96, 34, 30
                if (x // int(w * 0.09)) % 2 == 0:
                    r, g, b = 118, 44, 36
                if (x % int(w * 0.045)) < 3:
                    r, g, b = 60, 20, 18

            # Two tower pillars at the far sides.
            for px0 in (w * 0.085, w * 0.915):
                if abs(x - px0) < w * 0.035:
                    shade = 78 + int(34 * (1.0 - abs(x - px0) / (w * 0.035)))
                    r, g, b = shade, shade - 10, shade + 8

            # Side lancet windows. The wide view keeps these at its edges, so
            # they are the art's answer to the extra width.
            for px0 in (w * 0.17, w * 0.83):
                dx = abs(x - px0)
                lancet = dx < w * 0.022 and h * 0.30 < y < h * 0.68
                if y < h * 0.40:
                    lancet = lancet and (x - px0) ** 2 + (y - h * 0.40) ** 2 < (w * 0.022) ** 2
                if lancet:
                    r, g, b = PANE_COLS[int((y - h * 0.30) / (h * 0.05)) % 6]

            # The rose window: a circle, with radial wedges and lead lines.
            dx = x - cx
            dy = y - rose_cy
            dist = (dx * dx + dy * dy) ** 0.5
            if dist < rose_r:
                ang = math.atan2(dy, dx)
                wedge = int((ang + math.pi) / (math.pi / 6)) % 6
                ring = int(dist / (rose_r / 4))
                r, g, b = PANE_COLS[(wedge * 5 + ring * 2) % 6]
                # Lead: the spoke edges and the ring edges.
                spoke = abs(((ang + math.pi) % (math.pi / 6)) - math.pi / 12)
                if spoke < 0.035 or abs(dist % (rose_r / 4)) < rose_r * 0.02:
                    r = g = b = 18
                # A bright stone rim, one band inside the edge.
                if rose_r - dist < rose_r * 0.06:
                    r, g, b = 200, 190, 165

            if x < 14 or x >= w - 14 or y < 14 or y >= h - 14:
                r, g, b = 255, 0, 200
            if in_label(x, y, w, h, LABEL_WIDE):
                r, g, b = 255, 255, 255

            px[i] = min(255, max(0, r))
            px[i + 1] = min(255, max(0, g))
            px[i + 2] = min(255, max(0, b))
            px[i + 3] = 255
    return bytes(px)


# name, width, height, renderer. The wide image is authored in the widescreen
# aspect (16:9), which is the shape the user authors for; the port maps it into
# the game's canvas.
TARGETS = [
    ("01.png", 1280, 960, render_standard),
    ("01-wide.png", 1920, 1080, render_wide),
]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--force", action="store_true", help="overwrite existing images")
    args = ap.parse_args()

    for name, w, h, render in TARGETS:
        out = HERE / name
        if out.exists() and not args.force:
            print(f"{out.name} exists; not overwriting (use --force)")
            continue
        write_png(out, w, h, render(w, h))
        print(f"wrote {out.name} ({w}x{h})")


if __name__ == "__main__":
    main()
