#!/usr/bin/env python3
"""How complete is the recompilation? Measure it, statically and per run.

There is no single "recomp %", because the question has four different answers
and they are measured in different ways. This tool reports all four, and is
explicit about which are certain and which are bounded by what a run happened to
touch.

1. **Code bytes** -- how much of the ROM that the game executes as code is inside
   a linked ELF's `CONTENTS` sections. Offline, from the ELFs and the ROM
   (`tools/arenamap.py`'s model: a `CODE` section's LMA is its ROM address); the
   uncovered gaps are classified so a data gap can be told from a missing module.
2. **Modules** -- how many of the code modules the ROM can load have a compiled
   unit. The denominator is what the ROM itself names: every `jal 0x8009DA50`
   arena load (arenamap), every segment-table record (`kAllStreamedRecords`),
   and the explicit chunk-DMA modules the port has already had to add (records
   10a/10b/14a-d/10s/16b/07/05/06/18b/18c). The last group is the honest
   caveat: **a module nobody has streamed yet has no loader to find** (session
   73's table-driven record 16, session 82's battle fragment), so "modules" is
   100% of *known* modules, and a run log is what extends the known set.
3. **Dispatch** -- how many of the addresses the port actually dispatches
   (`LOOKUP_FUNC`) can resolve. Offline, from `tools/stubmap.py`: a target no
   module registers can never resolve, so 0 of those is the meaningful number.
   The 288 indirect `jalr` sites are outside any static measurement.
4. **Execution** -- how much of the recompiled code a run *executed*. This is the
   only one that needs the game: `OGRE_COVER=1` makes the runtime print every
   distinct recompiled function address it entered (the counters already existed
   for `OGRE_PROFILE`; the dump is the addition), and this tool joins that list
   with the registered entries. It answers "which modules/scenes has this route
   actually exercised", which no offline tool can know.

Usage:
    tools/recompcov.py                       # 1-3 (the offline measurement)
    tools/recompcov.py --no-scan             # 1-3, skipping the slow ELF scan
    tools/recompcov.py --log /tmp/run.log    # + 4, from an OGRE_COVER=1 run
    tools/recompcov.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import stubmap  # noqa: E402

ROM_PATH = ROOT / "assets" / "ogre64.z64"
COVER_RE = re.compile(r"^\[cover\]\s+(.*)$")
COVER_ENTRY_RE = re.compile(r"^0x([0-9A-Fa-f]{8})(?:\s+(\d+))?\s*$")
COVER_SUMMARY_RE = re.compile(
    r"^(\d+) distinct recompiled function\(s\) entered \((\d+)/(\d+) table slots\)"
)
LOG_STUB_RE = re.compile(r"streamed function stub called @ 0x([0-9A-Fa-f]{8})")
LOG_UNKNOWN_RE = re.compile(r"UNKNOWN module rom=0x([0-9A-Fa-f]+) ram=0x([0-9A-Fa-f]+)")
LOG_SCENE_RE = re.compile(r"^\[scene\] t=(\d+)ms id=0x([0-9A-Fa-f]+)")
LOG_LOADED_RE = re.compile(r"loading overlay record rom=0x([0-9A-Fa-f]+) ram=0x([0-9A-Fa-f]+)")


def code_coverage():
    """Compiled ROM bytes vs the code span, via arenamap's ELF/section model."""
    try:
        import arenamap
    except Exception as exc:  # pragma: no cover - import failure is reported
        return {"error": f"cannot import tools/arenamap.py: {exc}"}
    elfs = [arenamap.load_elf(p) for p in sorted((ROOT / "build").glob("bank*.elf"))]
    elfs.append(arenamap.load_elf(ROOT / "build/ogrebattle64.elf"))
    spans = sorted((s.lma, s.lma + s.size) for elf in elfs
                   for s in elf.sections if s.code and s.name != "_0")
    merged: list[list[int]] = []
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    covered = sum(hi - lo for lo, hi in merged)
    lo0, hi0 = merged[0][0], merged[-1][1]
    rom = ROM_PATH.read_bytes()
    gaps = []
    prev = lo0
    for lo, hi in merged:
        if lo - prev > 0x1000:
            word = rom[prev:prev + 4]
            op = (word[0] >> 2) if word else 0
            gaps.append({"lo": prev, "hi": lo, "size": lo - prev,
                         "kind": "code-like" if op in (2, 3, 4, 5, 6, 7, 9, 0x0F) else "data"})
        prev = max(prev, hi)
    span = hi0 - lo0
    return {
        "sections": len(spans),
        "elfs": len(elfs),
        "covered": covered,
        "span_lo": lo0,
        "span_hi": hi0,
        "span": span,
        "gaps": gaps,
        "pct": 100.0 * covered / span if span else 0.0,
    }


def module_coverage(modules):
    """Units vs the modules the ROM names (arenamap's arena scan)."""
    bank_records = [m for m in modules if m.kind == "bank"]
    return {
        "bank_records": len(bank_records),
        "units": len({m.unit for m in bank_records}),
        "base_sections": len([m for m in modules if m.kind == "main"]),
        "functions": sum(len(m.entries) for m in modules),
        "addresses": len({a for m in modules for a in m.entries}),
        "code_bytes": sum(m.size for m in modules),
    }


def arena_coverage():
    """Loader-pattern banks with and without a unit, from arenamap."""
    try:
        import arenamap
    except Exception:
        return None
    elfs = [arenamap.load_elf(p) for p in sorted((ROOT / "build").glob("bank*.elf"))]
    elfs.append(arenamap.load_elf(ROOT / "build/ogrebattle64.elf"))
    ranges = arenamap.all_unit_ranges()
    arenas, _unresolved = arenamap.collect(elfs)
    banks = arenamap.build_banks(arenas, ranges)
    return {"total": len(banks), "missing": sum(1 for b in banks if not b.ok)}


def dispatch_coverage(modules, sites):
    by_target = stubmap.grouped_sites(sites)
    unregistered = [a for a in by_target
                    if stubmap.unregistered(stubmap.classify(a, modules))]
    indirect = [s for s in sites if s.kind == "indirect"]
    return {
        "static_targets": len(by_target),
        "static_sites": sum(1 for s in sites if s.kind == "lookup"),
        "unresolvable": unregistered,
        "indirect_sites": len(indirect),
    }


def parse_log(path: Path):
    """(executed {addr: count}, summary, scenes, loaded records, problems).

    `saw_runlog` records whether the file actually carried the app's run-log
    lines. A cover-only file (`OGRE_COVER=<path>` writes just the census) must
    not be read as "this run had no stub hits" -- absence of the lines is not
    evidence of their absence in the run.
    """
    executed: dict[int, int] = {}
    summary = None
    scenes: list[int] = []
    loaded: set[tuple[int, int]] = set()
    stubs: dict[int, int] = {}
    unknown: set[tuple[int, int]] = set()
    saw_runlog = False
    with path.open(errors="replace") as fh:
        for line in fh:
            if "[cover]" in line:
                m = COVER_RE.match(line)
                if not m:
                    continue
                body = m.group(1).strip()
                s = COVER_SUMMARY_RE.match(body)
                if s:
                    summary = {"distinct": int(s.group(1)), "slots_used": int(s.group(2)),
                               "slots": int(s.group(3))}
                    continue
                e = COVER_ENTRY_RE.match(body)
                if e:
                    executed[int(e.group(1), 16)] = int(e.group(2) or 0)
                continue
            if "[bank]" in line or "[overlays]" in line:
                saw_runlog = True
            if "[scene]" in line:
                m = LOG_SCENE_RE.match(line)
                if m:
                    scenes.append(int(m.group(2), 16))
                continue
            if "loading overlay record" in line:
                m = LOG_LOADED_RE.search(line)
                if m:
                    loaded.add((int(m.group(1), 16), int(m.group(2), 16)))
                continue
            if "streamed function stub called" in line:
                m = LOG_STUB_RE.search(line)
                if m:
                    a = int(m.group(1), 16)
                    stubs[a] = stubs.get(a, 0) + 1
                continue
            if "UNKNOWN module" in line:
                m = LOG_UNKNOWN_RE.search(line)
                if m:
                    unknown.add((int(m.group(1), 16), int(m.group(2), 16)))
    return executed, summary, scenes, loaded, stubs, unknown, saw_runlog


def execution_coverage(modules, executed, summary):
    registered_by_addr: dict[int, list[stubmap.Module]] = {}
    for m in modules:
        for a in m.entries:
            registered_by_addr.setdefault(a, []).append(m)
    hit = sorted(a for a in executed if a in registered_by_addr)
    stray = sorted(a for a in executed if a not in registered_by_addr)

    # Sibling banks of one arena register the *same* addresses, so an executed
    # address credits every module that lists it -- including banks that were
    # never resident. The per-module counts are therefore upper bounds for any
    # module that shares its RAM; only a module at 0% is definitive (it cannot
    # have run). The run's `[bank] loading overlay record` lines in stdout are
    # what says which banks were actually loaded.
    shared: set[tuple[str, str]] = set()
    for m in modules:
        for other in modules:
            if other is m or (other.unit, other.name) == (m.unit, m.name):
                continue
            if m.ram < other.hi and other.ram < m.hi:
                shared.add((m.unit, m.name))

    per_module: dict[str, dict] = {}
    for m in modules:
        key = f"{m.unit}:{m.name}"
        entry = per_module.setdefault(key, {"unit": m.unit, "module": m.name,
                                            "kind": m.kind, "registered": 0,
                                            "executed": 0,
                                            "shared_ram": (m.unit, m.name) in shared})
        entry["registered"] += len(m.entries)
        entry["executed"] += sum(1 for a in m.entries if a in executed)
    for entry in per_module.values():
        entry["pct"] = (100.0 * entry["executed"] / entry["registered"]
                        if entry["registered"] else 0.0)
    return {
        "summary": summary,
        "executed_distinct": len(executed),
        "executed_registered": len(hit),
        "executed_unregistered": len(stray),
        "unregistered_addrs": [f"0x{a:08X}" for a in stray[:40]],
        "registered_addresses": len(registered_by_addr),
        "pct": (100.0 * len(hit) / len(registered_by_addr)
                if registered_by_addr else 0.0),
        "modules": sorted(per_module.values(), key=lambda e: (-e["executed"], e["module"])),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", help="run log from an OGRE_COVER=1 run")
    ap.add_argument("--no-scan", action="store_true",
                    help="skip the ELF scan (fast; omits the ROM byte figures)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--top", type=int, default=12, help="modules to list")
    ap.add_argument("--zero", type=int, default=18,
                    help="never-executed modules to list")
    args = ap.parse_args()

    modules = stubmap.load_modules()
    sites = stubmap.collect_sites()
    result: dict = {"modules": module_coverage(modules),
                    "dispatch": dispatch_coverage(modules, sites)}
    if not args.no_scan:
        result["code"] = code_coverage()
        result["arenas"] = arena_coverage()

    lines: list[str] = []

    def say(*a):
        lines.append(" ".join(str(x) for x in a))

    m = result["modules"]
    say("== modules ==")
    say(f"  {m['bank_records']} bank record(s) across {m['units']} unit(s), "
        f"{m['base_sections']} base section(s)")
    say(f"  {m['functions']} registered function entr(y/ies), "
        f"{m['addresses']} distinct address(es)")
    if result.get("arenas"):
        a = result["arenas"]
        say(f"  loader-pattern arenas: {a['total'] - a['missing']}/{a['total']} "
            f"have a unit" + (f", {a['missing']} MISSING" if a["missing"] else ""))
    if "code" in result:
        c = result["code"]
        if "error" in c:
            say(f"  code bytes: {c['error']}")
        else:
            say(f"  compiled ROM bytes: 0x{c['covered']:X} of the code span "
                f"0x{c['span_lo']:06X}..0x{c['span_hi']:06X} (0x{c['span']:X})")
            say(f"  CODE COVERAGE: {c['pct']:.2f}%  "
                f"({len(c['gaps'])} gap(s): "
                + ", ".join(f"0x{g['lo']:06X}..0x{g['hi']:06X} {g['kind']}"
                            for g in c["gaps"]) + ")")

    d = result["dispatch"]
    say("== dispatch ==")
    say(f"  {d['static_targets']} static dispatch target(s) from "
        f"{d['static_sites']} LOOKUP_FUNC site(s)")
    say(f"  unresolvable (no module registers the address): "
        f"{len(d['unresolvable'])}"
        + (": " + ", ".join(f"0x{x:08X}" for x in d["unresolvable"][:8])
           if d["unresolvable"] else ""))
    say(f"  {d['indirect_sites']} indirect jalr site(s) -- outside any static "
        f"measurement; a run's stub log is the only check")

    if args.log:
        path = Path(args.log)
        if not path.exists():
            print(f"recompcov: no such log: {path}", file=sys.stderr)
            return 1
        executed, summary, scenes, loaded, stubs, unknown, saw_runlog = parse_log(path)
        if not executed:
            say("== execution ==")
            say(f"  {path} has no [cover] lines -- run with OGRE_COVER=<path>")
            result["execution"] = {"error": "no [cover] lines"}
        else:
            e = execution_coverage(modules, executed, summary)
            result["execution"] = e
            result["run"] = {
                "log": str(path),
                "scenes": sorted(set(scenes)),
                "records_loaded": len(loaded),
                "stub_hits": sum(stubs.values()),
                "stub_addrs": [f"0x{a:08X}" for a in sorted(stubs)],
                "unknown_modules": [f"rom 0x{r:06X} ram 0x{m:08X}"
                                    for r, m in sorted(unknown)],
                "saw_runlog": saw_runlog,
            }
            zero = [m for m in e["modules"] if m["executed"] == 0 and m["registered"]]
            result["run"]["never_executed"] = [
                {"unit": m["unit"], "module": m["module"],
                 "registered": m["registered"]} for m in zero]
            say("== execution (this run) ==")
            say(f"  {e['executed_distinct']} distinct recompiled function(s) entered")
            say(f"  {e['executed_registered']} of {e['registered_addresses']} "
                f"registered address(es) executed -> EXECUTION COVERAGE "
                f"{e['pct']:.2f}%")
            if e["executed_unregistered"]:
                say(f"  {e['executed_unregistered']} entered address(es) are not in "
                    f"the registered set (patch/base symbols, a mirrored window, or a "
                    f"direct call bound into another bank's body): "
                    + ", ".join(e["unregistered_addrs"][:6]))
            if summary and summary["slots_used"] > summary["slots"] * 0.9:
                say(f"  WARNING: the runtime's counter table is "
                    f"{summary['slots_used']}/{summary['slots']} full; distinct "
                    f"functions may have been dropped (raise kFuncCountSlots)")
            if scenes:
                say("  scenes entered: "
                    + ", ".join(f"0x{s:02X}" for s in sorted(set(scenes))))
            if stubs or unknown:
                say(f"  PROBLEMS: {sum(stubs.values())} stub hit(s) at "
                    + ", ".join(f"0x{a:08X}" for a in sorted(stubs))
                    + f"; {len(unknown)} UNKNOWN module(s)")
            elif saw_runlog:
                say("  no stub hits and no UNKNOWN module in this run")
            else:
                say("  (cover-only dump: the file has no run-log lines, so stub / "
                    "UNKNOWN status is not observable here -- capture stdout too)")
            say(f"  modules by executed functions (top {args.top})"
                f"{'  [s] = shares RAM with a sibling bank, so its count is an upper bound' if any(m['shared_ram'] for m in e['modules'][: args.top]) else ''}:")
            for entry in e["modules"][: args.top]:
                say(f"    {entry['executed']:5d}/{entry['registered']:5d} "
                    f"{entry['pct']:6.1f}%  {entry['unit']:>5} {entry['module']}"
                    + ("  [s]" if entry["shared_ram"] else ""))
            if zero:
                say(f"  NEVER EXECUTED by this route: {len(zero)} module(s) / "
                    f"{sum(m['registered'] for m in zero)} registered function(s) -- "
                    f"the untested surface. This is the *definitive* half of the "
                    f"per-module list: a module at 0 entered nothing at all.")
                for m in sorted(zero, key=lambda m: -m["registered"])[: args.zero]:
                    say(f"    {m['registered']:5d} function(s)  "
                        f"{m['unit']:>5} {m['module']}")
                if len(zero) > args.zero:
                    say(f"    ... and {len(zero) - args.zero} more module(s)")

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
