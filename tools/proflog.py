#!/usr/bin/env python3
"""Read an `OGRE_PROFILE=1` run log and report the slow phase in named functions.

The profiler prints, once per second:

    [prof] T=9056   t1:80089BE4(100%) t3:80080EE8(99%) ...
    [prof]   entries/s: t1:0 t3:16938 ...
    [prof]   hot: 800862C0 x5664 800868DC x5664 ...

`T=` is the same `trace_millis` clock as the display-list submissions, and the
hot list is a one-second function-entry histogram for the whole emulated
machine, so a phase where the game runs at 2 fps instead of 30 shows up as a
hot list an order of magnitude longer than its neighbours.

    tools/proflog.py /tmp/ogre-lag-prof.log            # slow phase + hot list
    tools/proflog.py /tmp/ogre-lag-prof.log --dl       # join with display lists
    tools/proflog.py /tmp/ogre-lag-prof.log --names    # resolve every hot address
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROF_T = re.compile(r"^\[prof\] T=(\d+)\s+(.*)$")
ENTRIES = re.compile(r"entries/s:(.*)$")
HOT = re.compile(r"^\s*hot:(.*)$")
DL = re.compile(r"^\[renderer\] display list (\d+) at t=(\d+)ms")
PROC = re.compile(r"processDisplayLists (\d+) us \(dl (\d+)\)")
PAIR = re.compile(r"t(\d+):([0-9A-Fa-f]{8})(?:\((\d+)%\))?")
HOTITEM = re.compile(r"([0-9A-Fa-f]{8}) x(\d+)")


def parse(path):
    prof = []   # {t, threads:{tid:(func,pct)}, entries:{tid:n}, hot:[(func,count)]}
    dls = []    # (n, t)
    proc = {}
    cur = None
    with open(path, errors="replace") as fh:
        for line in fh:
            m = PROF_T.match(line)
            if m:
                cur = {"t": int(m.group(1)), "threads": {}, "entries": {}, "hot": []}
                for tid, func, pct in PAIR.findall(m.group(2)):
                    cur["threads"][int(tid)] = (int(func, 16), int(pct) if pct else 0)
                prof.append(cur)
                continue
            if cur is not None:
                m = ENTRIES.search(line)
                if m:
                    for tid, n in PAIR.findall(m.group(1)):
                        cur["entries"][int(tid)] = int(n)
                    continue
                m = HOT.match(line)
                if m:
                    cur["hot"] = [(int(f, 16), int(c)) for f, c in HOTITEM.findall(m.group(1))]
                    continue
            m = DL.match(line)
            if m:
                dls.append((int(m.group(1)), int(m.group(2))))
                continue
            m = PROC.search(line)
            if m:
                proc[int(m.group(2))] = int(m.group(1))
    return prof, dls, proc


def names_for(addrs):
    """Resolve a list of guest addresses with tools/symlook.py, in one call."""
    if not addrs:
        return {}
    args = ["python3", os.path.join(ROOT, "tools", "symlook.py")]
    args += [f"0x{a:08X}" for a in addrs]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        m = re.match(r"0x([0-9A-F]{8})\s+(.*)$", line.strip())
        if m:
            res[int(m.group(1), 16)] = m.group(2)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--dl", action="store_true", help="also report display-list rate per phase")
    ap.add_argument("--names", action="store_true", help="resolve hot addresses")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    prof, dls, proc = parse(args.log)
    if not prof:
        print("no [prof] lines: run with OGRE_PROFILE=1")
        return 1
    print(f"{len(prof)} profiler seconds, {len(dls)} display-list lines, {len(proc)} process timings")

    # Frame period per second, from the display list timestamps.
    per_s = Counter()
    for n, t in dls:
        per_s[t // 1000] += 1
    slow = sorted(s for s, c in per_s.items() if c <= 12)
    normal = sorted(s for s, c in per_s.items() if c > 12)
    print(f"seconds at <=12 display lists/s: {slow}")

    # The hot-list length is the cleanest slow-frame signal: it is total function
    # entries per second for the whole machine.
    lens = {p["t"] // 1000: sum(c for _, c in p["hot"]) for p in prof if p["hot"]}
    if lens:
        vals = sorted(lens.values())
        med = statistics.median(vals)
        print(f"hot-list entries/s: median {med:.0f}, max {max(vals)} at T={max(lens, key=lens.get)}s")

    # Report the profiled seconds that coincide with the slow phase.
    focus = set(slow)
    if not focus:
        # No display-list data: use the seconds whose hot list is far above median.
        focus = {s for s, v in lens.items() if v > 3 * med} if lens else set()
    for p in prof:
        sec = p["t"] // 1000
        if sec not in focus:
            continue
        print(f"\n--- T={p['t']}ms (second {sec}, {per_s.get(sec, '?')} lists/s)")
        rank = sorted(p["threads"].items(), key=lambda kv: -kv[1][1])
        shown = [(tid, f, pct) for tid, (f, pct) in rank if pct >= 20]
        if args.names:
            nm = names_for([f for _, f, _ in shown])
            for tid, f, pct in shown:
                print(f"    t{tid:<4} 0x{f:08X} {pct:3d}%  {nm.get(f, '?')}")
        else:
            print("    " + " ".join(f"t{tid}:{f:08X}({pct}%)" for tid, f, pct in shown))
        tot = sum(c for _, c in p["hot"])
        top = sorted(p["hot"], key=lambda kv: -kv[1])[:args.top]
        print(f"    hot entries/s total {tot}")
        if args.names:
            nm = names_for([f for f, _ in top])
            for f, c in top:
                print(f"      x{c:<8} 0x{f:08X}  {nm.get(f, '?')}")
        else:
            print("    hot: " + " ".join(f"{f:08X}x{c}" for f, c in top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
