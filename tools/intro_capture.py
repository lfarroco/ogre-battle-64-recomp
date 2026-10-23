#!/usr/bin/env python3
"""A screenshot series of the game's opening, native and in the browser.

The port has repeatedly produced captures that were black, stretched, or taken
at the wrong moment (sessions 128-181), so this tool measures the timing and
the pixels instead of trusting the capture:

  * one PNG every `--interval-ms` (default 1000), from the first scene, for
    `--seconds`;
  * a per-frame pixel count (`nonBlack`, `colourful`, `white`) so a blank run
    is visible in the output rather than discovered later;
  * the wall time of every frame, so the cadence can be checked;
  * the last scene id at each frame;
  * a `contact.png` grid of the whole series, and a `report.txt`.

Native and browser runs use different renderers, so the frames are not
byte-identical; they are the same screen at the same second of the run.

Usage:
  python3 tools/intro_capture.py                     # both, 30 s, 1 fps
  python3 tools/intro_capture.py --mode native
  python3 tools/intro_capture.py --mode web --seconds 60 --scene publishers
  python3 tools/intro_capture.py --interval-ms 100 --seconds 8 --mode web
  python3 tools/intro_capture.py --out /tmp/intro --mode web --software-gl

Native (`--mode native`)
  Runs `build-app/ogrebattle64` with `OGRE_SPEED=1 OGRE_NO_AUDIO=1` and
  RT64's `OGRE_CAPTURE_TARGET` readback, which writes the render target the VI
  sampled (not the swap chain: `OGRE_CAPTURE_PRESENT` yields black PPMs on this
  machine). The interval is RT64's `OGRE_CAPTURE_EVERY_MS=<interval-ms>`, the
  wall-clock knob: a fixed present count is not one second, because RT64
  presents per VI field in the 320x476 mode and per frame in the 320x240 one,
  which made one setting capture at 1 Hz and 2 Hz in different scenes (session
  182). Each capture line is timestamped as it arrives, so the report gives the
  real cadence; a dropped present shows up as a gap instead of being invisible.

Browser (`--mode web`)
  Runs `debug/probes/intro-capture.cjs`, which boots the wasm build in headless
  Chromium with the GPU backend (`--enable-gpu --use-angle=metal`). Without it
  the software rasterizer takes about 30 s of game time to leave the boot and
  the first 30 s are black. The probe captures the canvas in-page with
  `canvas.toDataURL` (a Playwright element screenshot costs 12.9 s per grab
  under load, session 176) and retries an idle boot.

`t = 0` is the first frame of the game's first scene in both modes: the capture
starts when the scene dispatcher runs, not when the runtime presents its first
(blank) frame. The browser presents black frames for several seconds before
that scene, so anchoring at the first present would spend the window on the
runtime coming up. The intro stills (scene 0x0A) follow: natively the QUEST
logo lands at about t = 20 s, and in the GPU browser at about t = 26 s, both
inside a 30 s window.

The output layout is:
  <out>/native/frame-00.png .. frame-NN.png, manifest.json, run.log
  <out>/web/frame-00.png    .. frame-NN.png, manifest.json
  <out>/contact-native.png, <out>/contact-web.png, <out>/report.txt
"""

from __future__ import annotations

import argparse
import array
import base64
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"

sys.path.insert(0, str(TOOLS))
import rt64_target  # noqa: E402  (the OGRE_CAPTURE_TARGET reader/writer)

CAPTURE_RE = re.compile(r"^\[capture\] wrote (.+) \((\d+)x(\d+) format=\d+\)$")
SCENE_RE = re.compile(r"^\[scene\] t=(\d+)ms id=0x([0-9A-Fa-f]+)")

THUMB_W, THUMB_H = 160, 120


# ---------------------------------------------------------------- png output


def write_png(path: Path, width: int, height: int, rgb_rows: bytes) -> None:
    """Write 8-bit RGB rows (each row already prefixed by filter type 0)."""
    png = b"\x89PNG\r\n\x1a\n"
    png += rt64_target.chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += rt64_target.chunk(b"IDAT", zlib.compress(rgb_rows, 6))
    png += rt64_target.chunk(b"IEND", b"")
    path.write_bytes(png)


def write_contact(path: Path, thumbs: list[bytes], columns: int = 6, gap: int = 2) -> None:
    """Tile 160x120 RGB thumbnails into one image, numbered left to right."""
    if not thumbs:
        return
    if len(thumbs) > 60:
        columns = 12
    rows = (len(thumbs) + columns - 1) // columns
    width = columns * THUMB_W + (columns + 1) * gap
    height = rows * THUMB_H + (rows + 1) * gap
    canvas = bytearray(b"\x10\x12\x16" * (width * height))
    for n, thumb in enumerate(thumbs):
        cx = gap + (n % columns) * (THUMB_W + gap)
        cy = gap + (n // columns) * (THUMB_H + gap)
        for y in range(THUMB_H):
            src = y * THUMB_W * 3
            dst = ((cy + y) * width + cx) * 3
            canvas[dst:dst + THUMB_W * 3] = thumb[src:src + THUMB_W * 3]
    out = bytearray()
    for y in range(height):
        out.append(0)
        out += canvas[y * width * 3:(y + 1) * width * 3]
    write_png(path, width, height, bytes(out))


# ---------------------------------------------------------------- native readback


def bin_thumb_and_stats(path: Path, width: int, height: int) -> tuple[bytes, dict]:
    """Downsample one `OGRE_CAPTURE_TARGET` image and count its pixels.

    The file is `width * height * 8` bytes of four little-endian 16-bit
    channels. The counters use the same 160x120 sample and the same thresholds
    as the browser probe, so the two reports are comparable.
    """
    data = path.read_bytes()
    need = width * height * 8
    if len(data) < need:
        raise SystemExit(f"{path}: {len(data)} bytes, need {need} for {width}x{height}")
    vals = array.array("H")
    vals.frombytes(data[:need])
    thumb = bytearray(THUMB_W * THUMB_H * 3)
    non_black = colourful = white = 0
    for ty in range(THUMB_H):
        sy = min(height - 1, ty * height // THUMB_H)
        for tx in range(THUMB_W):
            sx = min(width - 1, tx * width // THUMB_W)
            o = (sy * width + sx) * 4
            r, g, b = vals[o] >> 8, vals[o + 1] >> 8, vals[o + 2] >> 8
            if r > 8 or g > 8 or b > 8:
                non_black += 1
            if max(r, g, b) - min(r, g, b) > 24:
                colourful += 1
            if r > 200 and g > 200 and b > 200:
                white += 1
            t = (ty * THUMB_W + tx) * 3
            thumb[t:t + 3] = bytes((r, g, b))
    return bytes(thumb), {"nonBlack": non_black, "colourful": colourful, "white": white}


def capture_native(args) -> dict | None:
    out = args.out / "native"
    raw = args.out / ".native-raw"
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(raw, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    raw.mkdir(parents=True, exist_ok=True)

    app = Path(args.app)
    if not app.exists():
        for candidate in (REPO / "build-app" / "ogrebattle64", REPO / "build-dist" / "ogrebattle64"):
            if candidate.exists():
                app = candidate
                break
        else:
            raise SystemExit(f"no native build: {args.app} does not exist")
    rom = Path(args.rom)
    if not rom.exists():
        raise SystemExit(f"no ROM: {rom} does not exist")

    env = dict(os.environ)
    env.update({
        "OGRE_NO_AUDIO": "1",          # rule 14: never take the sound device
        "OGRE_MOD_TEST": "1",          # mods.json would otherwise force the start screen
        "OGRE_SPEED": "1",
        "OGRE_PREF_DIR": str(out / "pref"),
        "OGRE_CAPTURE_TARGET": str(raw / "t"),
        "OGRE_CAPTURE_AFTER": "0",
        "OGRE_CAPTURE_EVERY_MS": str(args.interval_ms),
        "OGRE_EXIT_AFTER_MS": str(args.seconds * 1000 + 10000),
        "OGRE_SCENE_LOG": "1",
    })
    if args.scene:
        env["OGRE_SCENE"] = args.scene

    print(f"native: {app} {rom}  (OGRE_CAPTURE_EVERY_MS={args.interval_ms}, "
          f"{args.seconds}s, OGRE_SPEED=1)")
    started = time.monotonic()
    proc = subprocess.Popen([str(app), str(rom)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
    watchdog = threading.Timer(args.seconds + 60, proc.kill)
    watchdog.daemon = True
    watchdog.start()

    captures: list[dict] = []
    scenes: list[dict] = []
    log_lines: list[str] = []
    last_scene: dict | None = None
    try:
        for line in proc.stderr:
            line = line.rstrip("\n")
            log_lines.append(line)
            now_ms = (time.monotonic() - started) * 1000.0
            scene = SCENE_RE.match(line)
            if scene:
                last_scene = {"ms": int(scene.group(1)), "id": scene.group(2),
                              "wallMs": now_ms}
                scenes.append(last_scene)
                continue
            cap = CAPTURE_RE.match(line)
            if cap:
                captures.append({
                    "path": Path(cap.group(1)),
                    "w": int(cap.group(2)),
                    "h": int(cap.group(3)),
                    "wallMs": now_ms,
                    "scene": last_scene,
                })
    finally:
        watchdog.cancel()
        proc.wait()

    (out / "run.log").write_text("\n".join(log_lines) + "\n")
    print(f"native: {len(captures)} readbacks, {len(scenes)} scene changes")

    # t=0 is the first capture whose scene is a real one. The boot presents a
    # placeholder scene 0x0000 (and sometimes 0x19E8, an uninitialised read)
    # before the dispatcher runs, and those frames carry no game output.
    start = 0
    for i, cap in enumerate(captures):
        if is_real_scene(cap["scene"]):
            start = i
            break

    frames = []
    thumbs = []
    count = max(1, args.seconds * 1000 // args.interval_ms)
    selected = captures[start:start + count]
    t0 = selected[0]["wallMs"] if selected else 0.0
    for cap in selected:
        name = f"frame-{len(frames):02d}.png"
        thumb, stats = bin_thumb_and_stats(cap["path"], cap["w"], cap["h"])
        rows = rt64_target.read_pixels(str(cap["path"]), cap["w"], cap["h"], "u16")
        write_png(out / name, cap["w"], cap["h"], bytes(rows))
        thumbs.append(thumb)
        frames.append({
            "i": len(frames),
            "tMs": cap["wallMs"] - t0,
            "w": cap["w"],
            "h": cap["h"],
            "scene": cap["scene"],
            "stats": stats,
            "file": name,
        })

    manifest = {
        "mode": "native",
        "app": str(app),
        "rom": str(rom),
        "scene": args.scene,
        "speed": 1,
        "seconds": args.seconds,
        "intervalMs": args.interval_ms,
        "captureEveryMs": args.interval_ms,
        "t0": "first frame of the first scene",
        "leadingFrames": start,
        "frames": frames,
        "scenes": scenes,
        "missed": [],
        "readbacks": len(captures),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    write_contact(args.out / "contact-native.png", thumbs)
    shutil.rmtree(raw, ignore_errors=True)
    return manifest


# ---------------------------------------------------------------- browser


def free_port(url: str) -> tuple[str, int]:
    parsed = urllib.parse.urlparse(url)
    return parsed.hostname or "127.0.0.1", parsed.port or (443 if parsed.scheme == "https" else 80)


def ensure_server(url: str, log_path: Path) -> subprocess.Popen | None:
    """Start `debug/server.py` when nothing answers on the URL's port."""
    host, port = free_port(url)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.5)
        if sock.connect_ex((host, port)) == 0:
            return None
    print(f"web: starting debug/server.py on {host}:{port}")
    log = log_path.open("wb")
    proc = subprocess.Popen([sys.executable, str(REPO / "debug" / "server.py"),
                             "--port", str(port), "--bind", host],
                            stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        time.sleep(0.1)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.5)
            if sock.connect_ex((host, port)) == 0:
                return proc
    proc.kill()
    raise SystemExit(f"debug/server.py did not start on {host}:{port}")


def capture_web(args) -> dict | None:
    out = args.out / "web"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    server = ensure_server(args.url, args.out / "server.log")
    cmd = [shutil.which("node") or "node",
           str(REPO / "debug" / "probes" / "intro-capture.cjs"),
           "--seconds", str(args.seconds),
           "--out", str(out),
           "--url", args.url,
           "--attempts", str(args.attempts),
           "--interval-ms", str(args.interval_ms)]
    if args.scene:
        cmd += ["--scene", args.scene]
    if args.rom:
        cmd += ["--rom", str(args.rom)]
    if args.software_gl:
        cmd += ["--gpu", "0"]
    print("web: " + " ".join(cmd))
    try:
        result = subprocess.run(cmd, cwd=str(REPO / "debug"))
    finally:
        if server is not None:
            server.terminate()
    if result.returncode != 0:
        print(f"web: probe exited {result.returncode}")
        if result.returncode == 2:
            return None
    manifest_path = out / "manifest.json"
    if not manifest_path.exists():
        return None
    manifest = json.loads(manifest_path.read_text())

    # The probe sends a 160x120 RGBA sample per frame so the contact sheet is
    # built without decoding 640x480 PNGs in Python. Move them out of the
    # manifest: 30 of them are 3 MB of base64 and make the file unreadable.
    thumbs = []
    for frame in manifest.get("frames", []):
        encoded = frame.get("stats", {}).pop("thumb", None)
        if not encoded:
            continue
        raw = base64.b64decode(encoded)
        rgb = bytearray(THUMB_W * THUMB_H * 3)
        for p in range(THUMB_W * THUMB_H):
            rgb[p * 3:p * 3 + 3] = raw[p * 4:p * 4 + 3]
        thumbs.append(bytes(rgb))
    if thumbs:
        write_contact(args.out / "contact-web.png", thumbs)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


# ---------------------------------------------------------------- report


def is_real_scene(scene) -> bool:
    """False for the boot's placeholder ids: 0x0000 and the 0x19E8 bad read."""
    return bool(scene) and scene.get("id") not in (None, "0000", "19E8")


def fmt_scene(scene) -> str:
    if not scene:
        return "?"
    return f"0x{scene['id']}"


def norm_scenes(scenes) -> list[dict]:
    """Native manifests carry dicts; the browser probe carries log lines."""
    out = []
    for scene in scenes or []:
        if isinstance(scene, dict):
            out.append(scene)
            continue
        m = re.search(r"t=(\d+)ms id=0x([0-9A-Fa-f]+)", str(scene))
        if m:
            out.append({"ms": int(m.group(1)), "id": m.group(2)})
    return out


def report(manifests: list[dict], out: Path) -> None:
    lines = []
    for m in manifests:
        if not m:
            continue
        frames = m["frames"]
        blank = [f for f in frames if not f["stats"].get("nonBlack")]
        gaps = [frames[i + 1]["tMs"] - frames[i]["tMs"] for i in range(len(frames) - 1)]
        worst = max(gaps) / 1000.0 if gaps else 0.0
        missed = m.get("missed") or []
        expected = max(1, m["seconds"] * 1000 // m.get("intervalMs", 1000))
        lines.append(f"{m['mode']}: {len(frames)}/{expected} frames, "
                     f"{len(blank)} blank, largest gap {worst:.2f}s"
                     + (f", missed {','.join(str(i) for i in missed)}" if missed else ""))
        if m.get("leadingFrames"):
            lines.append(f"  (skipped {m['leadingFrames']} leading frames before the first scene)")
        lines.append(f"  {'frame':>5} {'t(s)':>7} {'scene':>7} {'size':>9} "
                     f"{'nonBlack':>9} {'colour':>7} {'white':>6}  file")
        for f in frames:
            s = f["stats"]
            lines.append(
                f"  {f['i']:>5} {f['tMs'] / 1000.0:>7.3f} {fmt_scene(f['scene']):>7} "
                f"{f['w']}x{f['h']:<6} {s.get('nonBlack', 0):>9} {s.get('colourful', 0):>7} "
                f"{s.get('white', 0):>6}  {f.get('file', '')}")
        scenes = norm_scenes(m.get("scenes"))
        if scenes:
            shown = scenes[:12]
            text = ", ".join(f"0x{s['id']}@{s['ms']}ms" for s in shown)
            lines.append(f"  scenes: {text}" + (" ..." if len(scenes) > len(shown) else ""))
        lines.append("")
    text = "\n".join(lines)
    (out / "report.txt").write_text(text + "\n")
    print()
    print(text)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("native", "web", "both"), default="both")
    ap.add_argument("--seconds", type=int, default=30, help="capture window (default 30)")
    ap.add_argument("--interval-ms", type=int, default=1000,
                    help="ms between frames (default 1000); 100 samples a short hold")
    ap.add_argument("--out", type=Path, default=REPO / "debug" / "out" / "intro",
                    help="output directory (default debug/out/intro)")
    ap.add_argument("--scene", default=None,
                    help="force a scene by name (native OGRE_SCENE / ?OGRE_SCENE)")
    ap.add_argument("--app", default=str(REPO / "build-app" / "ogrebattle64"),
                    help="native executable")
    ap.add_argument("--rom", default=str(REPO / "assets" / "ogre64.z64"), help="ROM path")
    ap.add_argument("--url", default="http://127.0.0.1:8931/app/web/index.html",
                    help="browser build URL")
    ap.add_argument("--attempts", type=int, default=3, help="browser boots to try")
    ap.add_argument("--software-gl", action="store_true",
                    help="browser with the software rasterizer (much slower boot; see the docstring)")
    args = ap.parse_args(argv)

    if args.seconds < 1:
        ap.error("--seconds must be at least 1")
    if args.interval_ms < 20:
        ap.error("--interval-ms must be at least 20")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    manifests = []
    if args.mode in ("native", "both"):
        manifests.append(capture_native(args))
    if args.mode in ("web", "both"):
        manifests.append(capture_web(args))

    if not any(manifests):
        print("nothing captured")
        return 2
    report([m for m in manifests if m], args.out)
    print(f"output: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
