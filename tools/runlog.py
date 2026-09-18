#!/usr/bin/env python3
"""One-screen summary of a game run log (`ogrebattle64 ... > run.log 2>&1`).

Every session greps a 20k-line raw log for the same handful of facts. This
prints them at once, and — with `--check` — turns them into an assertion so
`make smoke` can fail a run instead of a human having to read it:

* the **scene timeline** (`[scene] t=...ms id=0xNNNN`) with names;
* the **RSP task table**: counts per type, and every *non-gfx* task spelled out.
  A non-gfx task served by `stub microcode` is a port gap (session 46's
  `M_NJPEGTASK` background decode was four of them, and they were invisible
  inside 23k lines of gfx chatter);
* **problems**: `[crash]`, `streamed function stub`, `UNKNOWN module`,
  `Failed to execute task`, `RSP ucode ... exited unexpectedly`;
* the **bank loads** and the display-list cadence.

Usage:
    tools/runlog.py run.log                 # human summary
    tools/runlog.py run.log --json          # machine-readable
    tools/runlog.py run.log --check         # exit 1 on any problem (smoke test)
    tools/runlog.py run.log --tasks 8       # show up to 8 non-gfx task lines
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

SCENES = {
    0x00: "boot/none",
    0x02: "new-game loader",
    0x04: "title",
    0x09: "intro",
    0x0A: "publishers",
    0x0B: "attract story",
    0x0C: "attract unit-info",
    0x0D: "new-game step (0x0D)",
    0x12: "load game",
    0x17: "tutorial",
    0x18: "menu 0x18",
}

RE_SCENE = re.compile(r"\[scene\]\s+t=(\d+)ms\s+id=0x([0-9A-Fa-f]+)")
RE_TASK = re.compile(
    r"\[rsp\]\s+task type=(\d+)\s+ucode=0x([0-9A-Fa-f]+).*?"
    r"ucode_size=0x([0-9A-Fa-f]+)\s+ucode_data=0x([0-9A-Fa-f]+)\s+"
    r"ucode_data_size=0x([0-9A-Fa-f]+)\s+data_ptr=0x([0-9A-Fa-f]+)\s+"
    r"data_size=0x([0-9A-Fa-f]+)")
# The reason is one of a known few, and log lines interleave mid-line with
# other traces, so match the alternatives instead of "anything in parens".
RE_TASK_SERVED = re.compile(
    r"\[rsp\]\s+task type (\d+) submitted \((stub microcode|njpeg microcode[^)]*|audio microcode[^)]*)\)")
RE_BANK = re.compile(r"\[bank\]\s+loading overlay record rom=0x([0-9A-Fa-f]+) "
                     r"ram=0x([0-9A-Fa-f]+) size=0x([0-9A-Fa-f]+) \((\d+) functions\)")
RE_DL = re.compile(r"\[renderer\]\s+display list (\d+) at t=(\d+)ms")
RE_CRASH = re.compile(r"\[crash\]")
RE_STUB = re.compile(r"streamed function stub")
RE_UNKNOWN = re.compile(r"UNKNOWN module")
RE_TASKFAIL = re.compile(r"Failed to execute task")
RE_RSPEXIT = re.compile(r"RSP ucode \d+ exited unexpectedly")

PROBLEMS = (
    ("crashes", RE_CRASH, "the process died (see the [crash] block + call chain)"),
    ("stub calls", RE_STUB, "a call landed on an address the port has no function for"),
    ("unknown modules", RE_UNKNOWN, "a PI DMA loaded a module the port does not know"),
    ("failed tasks", RE_TASKFAIL, "the runtime refused an RSP task"),
    ("bad ucode exits", RE_RSPEXIT, "a recompiled RSP ucode returned a non-Broke reason"),
)


def summarize(path: str) -> dict:
    text = Path(path).read_text(errors="replace")
    lines = text.splitlines()

    scenes: list[dict] = []
    for m in RE_SCENE.finditer(text):
        ms, sid = int(m.group(1)), int(m.group(2), 16)
        entry = {"t_ms": ms, "id": sid, "name": SCENES.get(sid, "?")}
        if not scenes or scenes[-1]["id"] != sid or scenes[-1]["t_ms"] != ms:
            scenes.append(entry)

    tasks = [{"type": int(m.group(1)), "ucode": int(m.group(2), 16),
              "ucode_size": int(m.group(3), 16), "ucode_data": int(m.group(4), 16),
              "ucode_data_size": int(m.group(5), 16), "data_ptr": int(m.group(6), 16),
              "data_size": int(m.group(7), 16)}
             for m in RE_TASK.finditer(text)]
    served = Counter(m.group(2) for m in RE_TASK_SERVED.finditer(text))
    # Served-by-kind per task type. The N64 task types are M_GFXTASK=1 (RT64
    # parses the display lists itself, so a stub is expected), M_AUDTASK=2 and
    # M_NJPEGTASK=4 — the latter two are non-gfx and must run real microcode.
    served_by_type: dict[int, Counter] = {}
    for m in RE_TASK_SERVED.finditer(text):
        served_by_type.setdefault(int(m.group(1)), Counter())[m.group(2)] += 1
    per_type: dict[int, Counter] = {}
    for task in tasks:
        per_type.setdefault(task["type"], Counter())["seen"] += 1

    banks = [{"rom": int(m.group(1), 16), "ram": int(m.group(2), 16),
              "size": int(m.group(3), 16), "functions": int(m.group(4))}
             for m in RE_BANK.finditer(text)]

    dls = [(int(m.group(1)), int(m.group(2))) for m in RE_DL.finditer(text)]
    problems = {name: len(rx.findall(text)) for name, rx, _ in PROBLEMS}

    # Non-gfx tasks are the interesting ones: graphics (type 1) goes to RT64
    # regardless, so only types 2/4 (audio, njpeg) run through the RSP microcode.
    # A "served by" kind that contains "stub" (bare `stub microcode`, or e.g.
    # `audio microcode disabled; stub`) means the work never happened.
    non_gfx = [t for t in tasks if t["type"] != 1]
    gfx_stubbed = sum(count for kind, count in served_by_type.get(1, Counter()).items()
                      if "stub" in kind)
    non_gfx_stubbed = sum(count for ttype, kinds in served_by_type.items()
                          if ttype != 1 for kind, count in kinds.items()
                          if "stub" in kind)
    return {
        "path": path,
        "lines": len(lines),
        "scenes": scenes,
        "tasks": tasks,
        "tasks_per_type": {str(k): v["seen"] for k, v in sorted(per_type.items())},
        "tasks_served": dict(served),
        "tasks_served_by_type": {str(k): dict(v) for k, v in sorted(served_by_type.items())},
        "non_gfx_tasks": non_gfx,
        "gfx_tasks": per_type.get(1, Counter()).get("seen", 0),
        "gfx_stubbed": gfx_stubbed,
        "non_gfx_stubbed": non_gfx_stubbed,
        "banks": banks,
        "dl_count": len(dls),
        "dl_first": dls[0] if dls else None,
        "dl_last": dls[-1] if dls else None,
        "problems": problems,
    }


def print_human(s: dict, max_tasks: int, show_banks: bool) -> None:
    print("%s  (%d lines)" % (s["path"], s["lines"]))

    if s["scenes"]:
        first, last = s["scenes"][0], s["scenes"][-1]
        print("\nscenes  (%d entries, t=%.1fs .. %.1fs)"
              % (len(s["scenes"]), first["t_ms"] / 1000.0, last["t_ms"] / 1000.0))
        for e in s["scenes"]:
            print("  t=%8.3fs  0x%02X  %s" % (e["t_ms"] / 1000.0, e["id"], e["name"]))
    else:
        print("\nscenes  none (boot may not have reached the dispatcher)")

    print("\nRSP tasks")
    if s["tasks_per_type"]:
        parts = ", ".join("type %s x%d" % (k, v) for k, v in s["tasks_per_type"].items())
        print("  %s" % parts)
    else:
        print("  none")
    if s["tasks_served"]:
        print("  served by: %s" % ", ".join("%s x%d" % (k, v)
                                            for k, v in sorted(s["tasks_served"].items())))
    if s["gfx_stubbed"]:
        print("  note: %d gfx task(s) are stubbed for the RSP — RT64 parses their "
              "display lists itself, so this is expected" % s["gfx_stubbed"])
    if s["non_gfx_stubbed"]:
        print("  WARNING: %d non-gfx task(s) served by the stub — that work never "
              "happens (audio = type 2, njpeg = type 4)" % s["non_gfx_stubbed"])
    if s["non_gfx_tasks"]:
        print("  non-gfx tasks (%d) — these run through the RSP microcode, so a stub "
              "means the work never happens:" % len(s["non_gfx_tasks"]))
        for t in s["non_gfx_tasks"][:max_tasks]:
            print("    type=%d ucode=0x%08X/0x%X data=0x%08X/0x%X "
                  "data_ptr=0x%08X size=0x%X"
                  % (t["type"], t["ucode"], t["ucode_size"], t["ucode_data"],
                     t["ucode_data_size"], t["data_ptr"], t["data_size"]))
        if len(s["non_gfx_tasks"]) > max_tasks:
            print("    ... %d more" % (len(s["non_gfx_tasks"]) - max_tasks))
    else:
        print("  no non-gfx tasks")

    print("\nrenderer")
    if s["dl_first"]:
        print("  %d display list(s), first %d at t=%.3fs, last %d at t=%.3fs"
              % (s["dl_count"], s["dl_first"][0], s["dl_first"][1] / 1000.0,
                 s["dl_last"][0], s["dl_last"][1] / 1000.0))
    else:
        print("  no [renderer] display list lines (null build, or the game never "
              "reached a frame)")

    if s["banks"]:
        print("\nbank loads  (%d)" % len(s["banks"]))
        seen = {}
        for b in s["banks"]:
            seen[(b["rom"], b["ram"])] = b
        for (rom, ram), b in list(seen.items())[-12:] if not show_banks else seen.items():
            print("  rom=0x%06X ram=0x%08X size=0x%-6X %d functions"
                  % (rom, ram, b["size"], b["functions"]))
        if len(seen) > 12 and not show_banks:
            print("  ... %d more distinct record(s)" % (len(seen) - 12))

    print("\nproblems")
    bad = False
    for name, _rx, why in PROBLEMS:
        count = s["problems"][name]
        if count:
            bad = True
            print("  %-16s %4d   %s" % (name, count, why))
    if not bad:
        print("  none")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when any problem line is present")
    ap.add_argument("--tasks", type=int, default=6, metavar="N",
                    help="non-gfx tasks to list (default 6)")
    ap.add_argument("--banks", action="store_true", help="list every bank load")
    args = ap.parse_args(argv)

    s = summarize(args.log)
    if args.json:
        print(json.dumps(s, indent=2))
    else:
        print_human(s, args.tasks, args.banks)

    if args.check:
        total = sum(s["problems"].values())
        if total:
            print("\nFAIL: %d problem line(s)" % total)
            return 1
        if s["non_gfx_stubbed"]:
            print("\nFAIL: %d non-gfx task(s) served by the stub microcode "
                  "(their work never happens)" % s["non_gfx_stubbed"])
            return 1
        print("\nPASS: no crash, no stub call, no unknown module, no bad RSP exit, "
              "no stubbed non-gfx task")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
