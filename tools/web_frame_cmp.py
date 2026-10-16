#!/usr/bin/env python3
"""Compare a browser canvas screenshot against RT64 render-target captures.

The browser build presents the VI's scanout into `#game-canvas`. The element's
inline CSS size is 640x480 and `emscripten_set_canvas_element_size` sets only the
drawing buffer, so a 320x240 VI mode is captured as each guest pixel duplicated
2x2 inside the element's 1 px border. RT64's side is `OGRE_CAPTURE_TARGET=<path>`
(see `tools/rt64_target.py`), one `<path>.<present>.bin` per captured present.

This tool crops the border, undoes the 2x duplication, and reports, for every
target given:

  mean abs / mean signed per channel   (browser - target, 0..255)
  exact                                the fraction of channel samples that match

With several targets it prints the best match, which is how a browser frame is
lined up with the native frame of the same screen (the two runs are not
frame-locked; a static screen makes any nearby target equivalent).

`--diff-out PATH` writes an amplified difference image (|difference| x 8) against
the best target, which is how the remaining difference is located on the screen.

Usage:
  python3 tools/web_frame_cmp.py CANVAS.png TARGET.bin [TARGET.bin ...]
                                [--diff-out PATH] [--flip]
  python3 tools/web_frame_cmp.py CANVAS.png '/tmp/nt/t.*.bin' --diff-out /tmp/d.png

The target's width and height are not in the file; every target is read as
320x240, which is the VI mode of the screens this tool has been used on. A
640-wide target needs the width passed instead (see the module constants).
"""
import argparse
import glob
import struct
import zlib

TARGET_W = 320
TARGET_H = 240


def read_png(path):
    """Minimal RGBA/RGB PNG reader -> (width, height, bytes, bytes_per_pixel)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos = 8
    idat = b""
    w = h = bitdepth = colortype = None
    while pos < len(data):
        (ln,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, bitdepth, colortype, comp, filt, inter = struct.unpack(">IIBBBBB", payload)
            if bitdepth != 8 or colortype not in (2, 6) or inter != 0:
                raise SystemExit(f"{path}: unsupported PNG (depth {bitdepth}, colour {colortype})")
            bpp = 3 if colortype == 2 else 4
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out, bpp


def canvas_pixels(path):
    """A `#game-canvas` screenshot -> (TARGET_W x TARGET_H) list of RGB tuples.

    Crops the element's 1 px border. A 2x duplicated drawing buffer (a 640x480
    element around a 320x240 buffer) is sampled at every second pixel; a 1:1
    element is read directly. The duplication is checked and reported, because a
    canvas that is neither is a different VI mode and needs its own width."""
    w, h, px, bpp = read_png(path)
    iw, ih = w - 2, h - 2
    if (iw, ih) == (TARGET_W * 2, TARGET_H * 2):
        step, off = 2, 1
    elif (iw, ih) == (TARGET_W, TARGET_H):
        step, off = 1, 1
    else:
        raise SystemExit(f"{path}: element {w}x{h} inner {iw}x{ih} is not "
                         f"{TARGET_W}x{TARGET_H} or its 2x")
    out = []
    doubled = 0
    total = 0
    for y in range(TARGET_H):
        row = []
        for x in range(TARGET_W):
            o = ((off + step * y) * w + (off + step * x)) * bpp
            row.append((px[o], px[o + 1], px[o + 2]))
            if step == 2:
                o2 = ((off + step * y) * w + (off + step * x + 1)) * bpp
                total += 1
                if px[o:o + 3] == px[o2:o2 + 3]:
                    doubled += 1
        out.append(row)
    return out, doubled, total


def target_pixels(path, flip=False):
    """An `OGRE_CAPTURE_TARGET` .bin -> RGB tuples (u16 channels, high byte)."""
    data = open(path, "rb").read()
    need = TARGET_W * TARGET_H * 8
    if len(data) < need:
        raise SystemExit(f"{path}: {len(data)} bytes, need {need} for {TARGET_W}x{TARGET_H}")
    out = []
    for y in range(TARGET_H):
        yy = (TARGET_H - 1 - y) if flip else y
        row = []
        for x in range(TARGET_W):
            o = (yy * TARGET_W + x) * 8
            r, g, b, _a = struct.unpack_from("<4H", data, o)
            row.append((r >> 8, g >> 8, b >> 8))
        out.append(row)
    return out


def compare(web, target):
    """(mean abs, mean signed, exact fraction) per channel."""
    sa = [0, 0, 0]
    ss = [0, 0, 0]
    exact = 0
    n = 0
    for y in range(TARGET_H):
        for x in range(TARGET_W):
            a, b = web[y][x], target[y][x]
            for c in range(3):
                d = a[c] - b[c]
                sa[c] += abs(d)
                ss[c] += d
                if d == 0:
                    exact += 1
            n += 1
    return [v / n for v in sa], [v / n for v in ss], exact / (n * 3)


def write_png(path, w, h, rows):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + \
            struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("canvas")
    ap.add_argument("targets", nargs="+")
    ap.add_argument("--diff-out", help="write |difference| x 8 against the best target")
    ap.add_argument("--flip", action="store_true", help="read every target bottom-up")
    args = ap.parse_args()

    web, doubled, total = canvas_pixels(args.canvas)
    if total:
        print(f"{args.canvas}: 2x duplication holds for {doubled}/{total} pixels")

    paths = []
    for pattern in args.targets:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    best = None
    for path in paths:
        t = target_pixels(path, args.flip)
        ma, ms, ex = compare(web, t)
        if best is None or sum(ma) < sum(best[1]):
            best = (path, ma, ms, ex, t)
        if len(paths) <= 4:
            print(f"  {path}: meanAbs={[round(v, 2) for v in ma]} "
                  f"meanSigned={[round(v, 2) for v in ms]} exact={ex * 100:.1f}%")
    path, ma, ms, ex, t = best
    print(f"best {path}: meanAbs={[round(v, 2) for v in ma]} "
          f"meanSigned={[round(v, 2) for v in ms]} exact={ex * 100:.1f}%")
    if args.diff_out:
        rows = bytearray()
        for y in range(TARGET_H):
            rows.append(0)
            for x in range(TARGET_W):
                a, b = web[y][x], t[y][x]
                rows += bytes(min(255, abs(a[c] - b[c]) * 8) for c in range(3))
        write_png(args.diff_out, TARGET_W, TARGET_H, rows)
        print(f"difference image -> {args.diff_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
