#!/usr/bin/env python3
"""Compare a browser canvas screenshot against RT64 render-target captures.

The browser build presents the VI's scanout into `#game-canvas`. RT64's side is
`OGRE_CAPTURE_TARGET=<path>` (see `tools/rt64_target.py`), one `<path>.<present>.bin`
per captured present.

Two things about the browser element matter for the comparison:

* The element's inline CSS box surrounds the drawing buffer with a 1 px border,
  so the screenshot is cropped by one pixel on each side.
* The browser scales the drawing buffer to the CSS box. While the box is an
  integer multiple of the buffer (index.html declares 640x480 and the 320x240 VI
  mode is a 2x upscale) that scale is lossless and the tool just undoes it.
  `--buffer WxH` handles the case where it is not: the drawing buffer was WxH and
  the box was the screenshot's inner size, and the tool undoes the browser's
  nearest-neighbour scale by sampling the inner image at
  `floor((X + 0.5) * W / inner_w)` / `floor((Y + 0.5) * H / inner_h)`. That rule
  was verified against a 640x476 buffer in a 640x480 box (`docs/HANDOFF-2026-10-16-session148.md`).

The tool reports, for every target given:

  mean abs / mean signed per channel   (browser - target, 0..255)
  exact                                the fraction of channel samples that match

With several targets it prints the best match, which is how a browser frame is
lined up with the native frame of the same screen (the two runs are not
frame-locked; a static screen makes any nearby target equivalent).

`--shift-search N` prints the mean abs difference for every whole-pixel shift of
the canvas against the best target in `[-N, N]`, which is how a vertical or
horizontal offset is distinguished from a resample.

`--inset N` drops N pixels from each side of both the canvas buffer and the
target before comparing, which is how the area a presenter never writes (the
VI's visible area is the `fbSize` minus the overscan crop, so the canvas's last
column and row keep whatever the resize left) is kept out of the measurement.

`--diff-out PATH` writes an amplified difference image (|difference| x 8) against
the best target, which is how the remaining difference is located on the screen.

Usage:
  python3 tools/web_frame_cmp.py CANVAS.png TARGET.bin [TARGET.bin ...]
                                [--target-size WxH] [--buffer WxH]
                                [--shift-search N] [--diff-out PATH] [--flip]
  python3 tools/web_frame_cmp.py CANVAS.png '/tmp/nt/t.*.bin' --diff-out /tmp/d.png

The target's width and height are not in the file. `--target-size WxH` gives
them; without it every target is read as 320x240, which is the VI mode of the
screens this tool was first used on.
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


def parse_size(text):
    try:
        w, h = (int(v) for v in text.lower().split("x"))
    except ValueError:
        raise SystemExit(f"--target-size/--canvas-size/--buffer wants WxH, got {text!r}")
    if w < 1 or h < 1:
        raise SystemExit(f"--target-size/--canvas-size/--buffer wants positive sides, "
                         f"got {text!r}")
    return w, h


def canvas_pixels(path, size, buffer=None):
    """A `#game-canvas` screenshot -> the drawing buffer's pixels.

    Crops the element's 1 px border. `size` is the drawing buffer the screenshot
    shows, and the inner image must be that buffer either 1:1 or at 2x (the
    lossless CSS upscale index.html declares); a 2x image is sampled at every
    second pixel. With `buffer = (bw, bh)` instead, the inner image is the
    browser's scale of a `bw x bh` drawing buffer to the element's CSS box, and
    the result is that buffer: buffer row `r` comes from the screenshot row the
    CSS box scaled it to, the inverse of `floor((Y + 0.5) * bh / ih)`."""
    w, h, px, bpp = read_png(path)
    iw, ih = w - 2, h - 2
    if buffer is not None:
        tw, th = buffer
        # Invert the CSS scale: for every drawing-buffer row/column, the captured
        # row/column that displays it (the first one, when the box is larger).
        def first_source(count, inner):
            table = {}
            for v in range(inner):
                n = int((v + 0.5) * count / inner)
                if 0 <= n < count and n not in table:
                    table[n] = v
            missing = [n for n in range(count) if n not in table]
            if missing:
                raise SystemExit(f"{path}: a {count}-texel side does not fit in "
                                 f"{inner} pixels; the --buffer size is too large")
            return table
        rows_of = first_source(th, ih)
        cols_of = first_source(tw, iw)
    else:
        tw, th = size
        if (iw, ih) == (tw * 2, th * 2):
            step = 2
        elif (iw, ih) == (tw, th):
            step = 1
        else:
            raise SystemExit(f"{path}: element {w}x{h} inner {iw}x{ih} is neither "
                             f"{tw}x{th} nor its 2x; pass the buffer the element "
                             f"shows as --canvas-size, or --buffer if it was scaled")
    out = []
    doubled = 0
    total = 0
    for y in range(th):
        row = []
        sy = 1 + (rows_of[y] if buffer is not None else step * y)
        for x in range(tw):
            sx = 1 + (cols_of[x] if buffer is not None else step * x)
            o = (sy * w + sx) * bpp
            row.append((px[o], px[o + 1], px[o + 2]))
            if buffer is None and step == 2:
                o2 = (sy * w + sx + 1) * bpp
                total += 1
                if px[o:o + 3] == px[o2:o2 + 3]:
                    doubled += 1
        out.append(row)
    return out, doubled, total


def target_pixels(path, size, flip=False):
    """An `OGRE_CAPTURE_TARGET` .bin -> RGB tuples (u16 channels, high byte)."""
    tw, th = size
    data = open(path, "rb").read()
    need = tw * th * 8
    if len(data) < need:
        raise SystemExit(f"{path}: {len(data)} bytes, need {need} for {tw}x{th}")
    out = []
    for y in range(th):
        yy = (th - 1 - y) if flip else y
        row = []
        for x in range(tw):
            o = (yy * tw + x) * 8
            r, g, b, _a = struct.unpack_from("<4H", data, o)
            row.append((r >> 8, g >> 8, b >> 8))
        out.append(row)
    return out


def crop(grid, inset):
    """Drop `inset` pixels from every side of a pixel grid."""
    if inset <= 0:
        return grid
    h = len(grid) - 2 * inset
    if h < 1 or len(grid[0]) - 2 * inset < 1:
        raise SystemExit(f"--inset {inset} leaves nothing of a {len(grid[0])}x{len(grid)} grid")
    return [row[inset:len(row) - inset] for row in grid[inset:len(grid) - inset]]


def compare(web, target, dx=0, dy=0):
    """(mean abs, mean signed, exact fraction) per channel over the overlap.

    The canvas grid and the target are compared row for row from their top-left
    (both are the guest framebuffer's rows in order); a `dx`/`dy` shifts the
    canvas by that many pixels first, which is how a whole-pixel offset is
    separated from a resample. The overlap of the two grids is used, so a 640x476
    drawing buffer can be compared against a 640x480 target."""
    th = min(len(web), len(target))
    tw = min(len(web[0]), len(target[0]))
    sa = [0, 0, 0]
    ss = [0, 0, 0]
    exact = 0
    n = 0
    for y in range(th):
        yy = y + dy
        if yy < 0 or yy >= len(web):
            continue
        for x in range(tw):
            xx = x + dx
            if xx < 0 or xx >= len(web[0]):
                continue
            a, b = web[yy][xx], target[y][x]
            for c in range(3):
                d = a[c] - b[c]
                sa[c] += abs(d)
                ss[c] += d
                if d == 0:
                    exact += 1
            n += 1
    if n == 0:
        return [0.0, 0.0, 0.0], [0.0, 0.0, 0.0], 0.0
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
    ap.add_argument("--target-size", default=f"{TARGET_W}x{TARGET_H}",
                    help="the target .bin's size, WxH (default 320x240)")
    ap.add_argument("--canvas-size",
                    help="the drawing buffer the screenshot shows, WxH "
                         "(default: the target size, or its 2x as a lossless upscale)")
    ap.add_argument("--buffer", help="the drawing buffer the element's CSS box "
                                     "scaled, WxH; undoes a non-integer scale")
    ap.add_argument("--inset", type=int, default=0, metavar="N",
                    help="drop N pixels from each side of both grids before comparing, "
                         "e.g. the column and row the presenter never writes")
    ap.add_argument("--shift-search", type=int, default=0, metavar="N",
                    help="print the mean abs difference for every whole-pixel shift in [-N, N]")
    ap.add_argument("--diff-out", help="write |difference| x 8 against the best target")
    ap.add_argument("--flip", action="store_true", help="read every target bottom-up")
    args = ap.parse_args()

    size = parse_size(args.target_size)
    buffer = parse_size(args.buffer) if args.buffer else None
    canvas_size = parse_size(args.canvas_size) if args.canvas_size else size
    web, doubled, total = canvas_pixels(args.canvas, canvas_size, buffer)
    if total:
        print(f"{args.canvas}: 2x duplication holds for {doubled}/{total} pixels")
    if buffer:
        print(f"{args.canvas}: read as a {buffer[0]}x{buffer[1]} drawing buffer "
              f"(the captured element is its CSS box)")
    else:
        print(f"{args.canvas}: read as a {canvas_size[0]}x{canvas_size[1]} drawing buffer")
    if args.inset:
        web = crop(web, args.inset)
    cw, ch = len(web[0]), len(web)

    paths = []
    for pattern in args.targets:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    best = None
    for path in paths:
        t = crop(target_pixels(path, size, args.flip), args.inset)
        ma, ms, ex = compare(web, t)
        cw, ch = min(cw, len(t[0])), min(ch, len(t))
        if best is None or sum(ma) < sum(best[1]):
            best = (path, ma, ms, ex, t)
        if len(paths) <= 4:
            print(f"  {path}: meanAbs={[round(v, 2) for v in ma]} "
                  f"meanSigned={[round(v, 2) for v in ms]} exact={ex * 100:.1f}%")
    path, ma, ms, ex, t = best
    print(f"best {path}: meanAbs={[round(v, 2) for v in ma]} "
          f"meanSigned={[round(v, 2) for v in ms]} exact={ex * 100:.1f}% "
          f"over {cw}x{ch}")
    if args.shift_search:
        n = args.shift_search
        print("shift search (mean abs over the three channels):")
        for dy in range(-n, n + 1):
            row = []
            for dx in range(-n, n + 1):
                a, _s, _e = compare(web, t, dx, dy)
                row.append(sum(a) / 3)
            marker = "  <-" if (dy == 0 and min(row) == row[n]) else ""
            print(f"  dy={dy:+d}: " + " ".join(f"{v:6.2f}" for v in row) + marker)
        print(f"  (columns are dx = {-n}..{n})")
    if args.diff_out:
        rows = bytearray()
        for y in range(ch):
            rows.append(0)
            for x in range(cw):
                a, b = web[y][x], t[y][x]
                rows += bytes(min(255, abs(a[c] - b[c]) * 8) for c in range(3))
        write_png(args.diff_out, cw, ch, rows)
        print(f"difference image -> {args.diff_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
