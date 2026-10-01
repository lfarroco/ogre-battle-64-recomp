#!/usr/bin/env python3
"""Frame-gap analysis of a game run log, for "the game stutters" reports.

A run with `OGRE_DL_TRACE=1` prints one line per display-list submission:

    [renderer] display list 1023 at t=35795ms entries=0 (type=1 ucode=... )

The gap between consecutive submissions is the frame time. A steady screen has
a flat series; a stutter is a tail. This tool measures the tail and, when the
stutters repeat, the period they repeat at — a constant period ("like
clockwork") means one piece of periodic work, and a broad spread means the
work is data-dependent.

`--check` fails when the gaps are periodic, which is what a self-driving run
needs: session 116's stutter was the runtime's periodic `[snap]` queue
snapshot, one 33 -> 59 ms frame every 1500 ms.

Usage:
    tools/dlgaps.py run.log                      # summary + periodicity
    tools/dlgaps.py run.log --after 3000         # skip boot (the default)
    tools/dlgaps.py run.log --spike 1.5          # tail threshold (x p50)
    tools/dlgaps.py run.log --period 1500        # phase histogram period
    tools/dlgaps.py run.log --check              # exit 1 when periodic
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

DL_RE = re.compile(r"display list (\d+) at t=(\d+)ms")


def display_lists(path: Path) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    with path.open("r", errors="replace") as handle:
        for line in handle:
            match = DL_RE.search(line)
            if match is not None:
                out.append((int(match.group(1)), int(match.group(2))))
    return out


def percentile(sorted_values: list[int], fraction: float) -> int:
    if not sorted_values:
        return 0
    index = min(len(sorted_values) - 1, int(len(sorted_values) * fraction))
    return sorted_values[index]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="run log from OGRE_DL_TRACE=1")
    ap.add_argument("--after", type=int, default=3000, metavar="MS",
                    help="ignore subsmissions before this time, in ms (default 3000: "
                         "the boot's module streaming is not a stutter)")
    ap.add_argument("--spike", type=float, default=1.5, metavar="FACTOR",
                    help="a gap counts as a spike above FACTOR x p50 (default 1.5)")
    ap.add_argument("--period", type=int, default=1500, metavar="MS",
                    help="phase-histogram period in ms (default 1500)")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when the spikes repeat at a fixed period")
    args = ap.parse_args(argv)

    listings = display_lists(Path(args.log))
    if len(listings) < 3:
        print(f"{args.log}: {len(listings)} display list(s); "
              f"run with OGRE_DL_TRACE=1 and more than a few frames")
        return 2 if args.check else 0

    gaps = [(t, u - t) for (_, t), (_, u) in zip(listings, listings[1:])
            if u - t > 0 and t >= args.after]
    if len(gaps) < 3:
        print(f"{args.log}: no gaps after {args.after} ms "
              f"(the run ends at {listings[-1][1]} ms)")
        return 2 if args.check else 0

    values = sorted(g for _, g in gaps)
    median = statistics.median(values)
    spikes = [(t, g) for t, g in gaps if g > median * args.spike]

    span = listings[-1][1] - listings[0][1]
    print(f"{args.log}")
    print(f"  {len(listings)} display lists over {span} ms, "
          f"{len(gaps)} gaps after t={args.after} ms")
    print(f"  gap ms: p50={median:.0f} p90={percentile(values, 0.90)} "
          f"p99={percentile(values, 0.99)} max={values[-1]}")
    print(f"  spikes above {args.spike:g}x p50 ({median * args.spike:.0f} ms): "
          f"{len(spikes)}")

    # Phase histogram: a clockwork stutter puts its spikes in one bin of the
    # period. The bin width is a tenth of the period, so a period several
    # percent off still lands in one bin.
    if spikes:
        width = max(1, args.period // 10)
        phases = Counter((t % args.period) // width for t, _ in spikes)
        bin_index, bin_count = phases.most_common(1)[0]
        shared = bin_count / len(spikes)
        lo, hi = bin_index * width, (bin_index + 1) * width - 1
        print(f"  phase mod {args.period} ms: {shared * 100:.0f}% of the spikes "
              f"in the {lo}-{hi} ms bin ({bin_count}/{len(spikes)})")
        intervals = [b - a for (a, _), (b, _) in zip(spikes, spikes[1:])]
        if intervals:
            print(f"  spike spacing ms: p50={statistics.median(intervals):.0f} "
                  f"min={min(intervals)} max={max(intervals)}")
        periodic = len(spikes) >= 5 and shared >= 0.6
        print(f"  {'PERIODIC' if periodic else 'not periodic'}: "
              f"{'at least 60% of 5+ spikes share one phase bin' if periodic else 'no repeated phase'}")
        if args.check and periodic:
            return 1
    else:
        print("  no spikes: the frame time is flat")

    return 0


if __name__ == "__main__":
    sys.exit(main())
