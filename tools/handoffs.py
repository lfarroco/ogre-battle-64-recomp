#!/usr/bin/env python3
"""List `docs/HANDOFF-*.md` newest-first with each file's first heading.

The handoffs are this project's per-session record and the only place a fresh
agent can pick up a session's goal, evidence and open walls. `docs/README.md`
curates a few of them; this prints the whole sequence so "which is the newest"
and "what did session N set out to do" are one command (`make handoffs`).
"""

from __future__ import annotations

import glob
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sort_key(path: str) -> tuple[str, int]:
    name = os.path.basename(path)
    date = re.search(r"(\d{4}-\d{2}-\d{2})", name)
    session = re.search(r"session(\d+)", name)
    return (date.group(1) if date else "", int(session.group(1)) if session else 0)


def main() -> int:
    paths = glob.glob(os.path.join(REPO, "docs", "HANDOFF-*.md"))
    for path in sorted(paths, key=sort_key, reverse=True):
        with open(path, encoding="utf-8", errors="replace") as handle:
            first = handle.readline().lstrip("# ").strip()
        print("%-44s %s" % (os.path.basename(path), first))
    print("\n%d handoff(s)." % len(paths))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
