#!/usr/bin/env python3
"""Scan the OB64 disassembly for libultra fingerprints.

Parses asm/1060.s and produces, for every function:
  * MMIO register constants referenced (0xA4xxxxxx hardware registers)
  * cop0 register accesses (mfc0/mtc0 with register numbers)
  * direct callees (jal targets)

This is the data source for the OB64 libultra address->name table
(see docs/LIBULTRA-BRIDGING.md, "Identification methodology").

Usage: python3 tools/libultra_scan.py [asm/1060.s] [output.txt]
If output.txt is given the full report is written there too.
"""

import re
import sys
from pathlib import Path

DISASM = Path(sys.argv[1] if len(sys.argv) > 1 else "asm/1060.s")
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else None

MMIO_RE = re.compile(r"0x(A4[0-9A-Fa-f]{6})")
MFC0_RE = re.compile(r"mfc0\s+\$[a-z0-9]+\s*,\s*\$(\d+)\b")
MTC0_RE = re.compile(r"mtc0\s+\$[a-z0-9]+\s*,\s*\$(\d+)\b")
JAL_RE = re.compile(r"jal\s+(func_[0-9A-Fa-f]{8})")

# Cop0 register name map (see N64 cpu.h / libultra)
COP0_NAMES = {
    9: "Count", 11: "Compare", 12: "Status", 13: "Cause",
    14: "EPC", 18: "WatchLo", 20: "WatchHi", 8: "BadVAddr",
}

# MMIO register family map (top 4 hex digits identify the controller)
MMIO_FAMILIES = {
    0xA440: "VI", 0xA450: "AI", 0xA460: "PI", 0xA470: "RI",
    0xA480: "SI", 0xA404: "SP", 0xA408: "SP_PC", 0xA410: "DPC",
    0xA420: "DPS", 0xA430: "MI", 0xA000: "RDRAM",
}

def main():
    funcs = {}       # name -> dict
    order = []       # names in disasm order
    cur = None

    for line in DISASM.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"glabel (\S+)", line)
        if m:
            cur = {"name": m.group(1), "start": None, "end": None,
                   "mmio": set(), "cop0": set(), "calls": set()}
            funcs[cur["name"]] = cur
            order.append(cur["name"])
            continue

        if cur is None:
            continue

        if line.strip().startswith("endlabel"):
            cur = None
            continue

        im = re.match(r"\s*/\*\s*[0-9A-F]+\s+([0-9A-F]{8})\s+[0-9A-F]{8}\s*\*/\s*(.*)", line)
        if not im:
            continue
        vram = int(im.group(1), 16)
        if cur["start"] is None:
            cur["start"] = vram
        cur["end"] = vram + 4
        instr = im.group(2)

        for mm in MMIO_RE.findall(instr):
            cur["mmio"].add(int(mm, 16))
        for c0 in MFC0_RE.findall(instr) + MTC0_RE.findall(instr):
            cur["cop0"].add(int(c0))
        for jj in JAL_RE.findall(instr):
            cur["calls"].add(jj)

    lines = []

    def p(s=""):
        lines.append(s)

    p("# OB64 libultra fingerprint scan")
    p(f"# source: {DISASM}")
    p(f"# functions: {len(order)}")
    p()

    # ---- Functions touching MMIO, grouped by family -------------------------
    p("=" * 78)
    p("MMIO-touching functions (grouped by register family)")
    p("=" * 78)
    by_family = {}
    for name in order:
        f = funcs[name]
        for reg in sorted(f["mmio"]):
            fam = MMIO_FAMILIES.get(reg >> 16, "?")
            by_family.setdefault(fam, []).append((name, reg))
    for fam in sorted(by_family):
        p(f"\n[{fam}]")
        for name, reg in sorted(set(by_family[fam])):
            p(f"  {name}  0x{reg:08X}")

    # ---- Functions touching cop0 -------------------------------------------
    p()
    p("=" * 78)
    p("cop0-touching functions")
    p("=" * 78)
    for name in order:
        f = funcs[name]
        if f["cop0"]:
            regs = ", ".join(f"${r}({COP0_NAMES.get(r,'?')})" for r in sorted(f["cop0"]))
            p(f"  {name}  {regs}")

    # ---- Call-graph edges (function -> direct callees) ----------------------
    p()
    p("=" * 78)
    p("Call graph (caller -> direct callees)")
    p("=" * 78)
    for name in order:
        f = funcs[name]
        if f["calls"]:
            p(f"  {name} -> " + ", ".join(sorted(f["calls"])))

    # ---- Per-function summary (compact) -------------------------------------
    p()
    p("=" * 78)
    p("Per-function summary")
    p("=" * 78)
    for name in order:
        f = funcs[name]
        mmio = ",".join(f"0x{r:08X}" for r in sorted(f["mmio"]))
        cop0 = ",".join(str(r) for r in sorted(f["cop0"]))
        calls = ",".join(sorted(f["calls"]))
        p(f"{name} @0x{f['start']:08X} mmio[{mmio}] cop0[{cop0}] calls[{calls}]")

    text = "\n".join(lines)
    print(text)
    if OUT:
        OUT.write_text(text + "\n", encoding="utf-8")
        print(f"\n[written to {OUT}]", file=sys.stderr)


if __name__ == "__main__":
    main()
