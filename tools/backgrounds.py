#!/usr/bin/env python3
"""Extract the game's own backdrop as a reference image for an HD replacement.

The port writes the reference itself: with `OGRE_BG_DUMP=<dir>` set,
`app/src/hd_backgrounds.cpp` assembles the game's four `njpeg` sub-images into
the 496x384 backdrop canvas and writes one PNG per assembly, before the
`mods/backgrounds/` pack replaces anything. One run dumps every backdrop it
assembles, so the switch covers later backgrounds with no code change.

This tool drives that run and files the result:

    tools/backgrounds.py list
    tools/backgrounds.py extract 01            # the cathedral, from backgrounds.txt
    tools/backgrounds.py extract 01 --step 6   # another step of scene 0x0D
    tools/backgrounds.py extract --all         # a plain boot, keep every dump
    tools/backgrounds.py extract 01 --from /tmp/dump   # file an existing dump

Output defaults to `mods/backgrounds/reference/<id>-<name>-original.png`. The
port ignores that subdirectory when it loads the pack, so a reference cannot be
mistaken for a replacement.

The run needs the app and the ROM. `extract <id>` builds the route from the
mapping: scene `0x0D` is the New Game dialogue scene, reached with
`OGRE_SCENE=new-game` and `OGRE_STEP=<step>`; any other scene is forced with
`OGRE_SCENE`.

Usage: see `docs/guides/hd-backgrounds.md`.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "mods" / "backgrounds"
MAPPING = PACK / "backgrounds.txt"
REFERENCE = PACK / "reference"
DEFAULT_ROM = ROOT / "assets" / "ogre64.z64"

# `make app` writes `build-dist/` when the static SDL2 build is the default;
# `cmake --build build-app` writes `build-app/`. Use whichever exists.
APP_CANDIDATES = [
    ROOT / "build-app" / "ogrebattle64",
    ROOT / "build-dist" / "ogrebattle64",
    ROOT / "build-app" / "Release" / "ogrebattle64",
    ROOT / "build-dist" / "Release" / "ogrebattle64",
]


def default_app() -> Path:
    for candidate in APP_CANDIDATES:
        if candidate.is_file():
            return candidate
    return APP_CANDIDATES[0]

# Scene 0x0D is the dialogue/cutscene scene the New Game opening drives with
# OGRE_SCENE=new-game and OGRE_STEP=<step>. Any other scene has no such route, so
# the tool forces it and warns.
DIALOGUE_SCENE = 0x0D

# At OGRE_SPEED=8 the boot plus the preload reaches the first dialogue in a few
# wall-clock seconds; 20 s leaves room for a slow machine.
RUN_SECONDS = 20


def read_mapping(path: Path) -> list[tuple[str, str, int, int]]:
    entries: list[tuple[str, str, int, int]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0]
        fields = line.split()
        if len(fields) != 4:
            continue
        eid, name, scene, step = fields
        entries.append((eid, name, int(scene, 0) & 0x7FFF, int(step, 0) & 0x7FFF))
    return entries


def find_entry(eid: str) -> tuple[str, str, int, int]:
    for entry in read_mapping(MAPPING):
        if entry[0] == eid:
            return entry
    raise SystemExit(
        f"backgrounds.py: '{eid}' is not in {MAPPING.relative_to(ROOT)}; "
        f"run 'backgrounds.py list'"
    )


def run_dump(app: Path, rom: Path, dump_dir: Path, scene: int, step: int) -> None:
    if not app.is_file():
        raise SystemExit(f"backgrounds.py: no app at {app}; build it with 'make app'")
    if not rom.is_file():
        raise SystemExit(f"backgrounds.py: no ROM at {rom}; pass --rom <path>")

    env = dict(os.environ)
    env["OGRE_BG_DUMP"] = str(dump_dir)
    # Any install with .nrm mods shows the start screen; accept the ROM.
    env["OGRE_LAUNCHER_KEYS"] = "space"
    env["OGRE_NJPEG"] = "1"
    env["OGRE_SPEED"] = "8"
    env["OGRE_EXIT_AFTER_MS"] = str(RUN_SECONDS * 1000)
    if scene == DIALOGUE_SCENE:
        env["OGRE_SCENE"] = "new-game"
        env["OGRE_STEP"] = str(step)
    else:
        env["OGRE_SCENE"] = hex(scene)
        env.pop("OGRE_STEP", None)
        print(
            f"backgrounds.py: scene 0x{scene:02X} has no built-in route; forcing it. "
            f"A forced scene enters without the pre-state a natural path builds, so "
            f"the dump may be empty (docs/guides/app-build.md -> scene forcing).",
            file=sys.stderr,
        )

    print(f"backgrounds.py: running {app.name} with OGRE_BG_DUMP={dump_dir}")
    subprocess.run([str(app), str(rom)], env=env, check=False)


def file_dumps(dump_dir: Path, out_dir: Path, wanted: list[tuple[str, str, int, int]]) -> int:
    found = sorted(dump_dir.glob("original-step*.png"))
    if not found:
        print(
            f"backgrounds.py: no original-step*.png in {dump_dir}\n"
            f"  the run did not assemble a backdrop; check that it reached the scene\n"
            f"  and that the app was built with the HD-backgrounds hook",
            file=sys.stderr,
        )
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)
    wanted_by_step = {step: (eid, name) for eid, name, _scene, step in wanted}
    written = 0
    for png in found:
        step = int(png.stem.removeprefix("original-step"))
        eid, name = wanted_by_step.get(step, (None, None))
        if eid is None:
            dest = out_dir / f"step{step:04d}-original.png"
        else:
            dest = out_dir / f"{eid}-{name}-original.png"
        dest.write_bytes(png.read_bytes())
        try:
            shown = dest.relative_to(ROOT)
        except ValueError:
            shown = dest
        print(f"backgrounds.py: {shown}  (from {png.name})")
        written += 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="backgrounds.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="print the pack's mappings")

    ex = sub.add_parser("extract", help="dump and file the game's own backdrop")
    ex.add_argument("id", nargs="?", help="the mapping id, e.g. 01")
    ex.add_argument("--all", action="store_true", help="a plain boot; keep every dump")
    ex.add_argument("--from", dest="from_dir", type=Path,
                    help="file an existing dump directory instead of running")
    ex.add_argument("--scene", type=lambda s: int(s, 0), help="override the mapped scene")
    ex.add_argument("--step", type=lambda s: int(s, 0), help="override the mapped step")
    ex.add_argument("--out", type=Path, default=REFERENCE, help="output directory")
    ex.add_argument("--app", type=Path, default=None)
    ex.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    ex.add_argument("--dump", type=Path, help="keep the raw dump here (default: a temp dir)")

    args = ap.parse_args()
    if args.command == "extract" and args.app is None:
        args.app = default_app()

    if args.command == "list":
        for eid, name, scene, step in read_mapping(MAPPING):
            print(f"{eid}  {name:<12} scene=0x{scene:02X} step={step}")
        return 0

    # extract
    if args.all and args.id:
        raise SystemExit("backgrounds.py: --all takes no id")
    if not args.all and not args.id:
        raise SystemExit("backgrounds.py: pass an id (see 'backgrounds.py list') or --all")

    wanted: list[tuple[str, str, int, int]] = []
    if args.id:
        wanted = [find_entry(args.id)]

    if args.from_dir is not None:
        if not args.from_dir.is_dir():
            raise SystemExit(f"backgrounds.py: {args.from_dir} is not a directory")
        return file_dumps(args.from_dir, args.out, wanted)

    if args.dump is not None:
        dump_dir = args.dump
        dump_dir.mkdir(parents=True, exist_ok=True)
    else:
        import tempfile

        dump_dir = Path(tempfile.mkdtemp(prefix="ogre-bg-dump-"))

    if args.all:
        # A plain boot assembles whatever the run reaches.
        scene = args.scene if args.scene is not None else DIALOGUE_SCENE
        step = args.step if args.step is not None else 0
    else:
        _eid, _name, mapped_scene, mapped_step = wanted[0]
        scene = args.scene if args.scene is not None else mapped_scene
        step = args.step if args.step is not None else mapped_step

    run_dump(args.app, args.rom, dump_dir, scene, step)
    return file_dumps(dump_dir, args.out, wanted)


if __name__ == "__main__":
    sys.exit(main())
