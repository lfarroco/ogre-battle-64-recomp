#!/usr/bin/env python3
"""Guest <-> ROM address lookup, offline, from the repo's build inputs.

This is the "which segment/record is this, what is the ROM offset, is it a
function entry, and which *other* records share this RAM" tool. It answers
without booting the game, which is what makes it useful for triage: several
records deliberately overlap (`0x80197B90` is record 2 in unit A, record 17 in
unit B, record 1 in unit D and record 0 in unit E — only the game's DMA says
which is live), and "the call ran the wrong bank's body" is this project's most
common bug class (AGENTS §4).

Usage:
    tools/guestmap.py 0x8009ED80 0x1FB8FC ...     # describe each address
    tools/guestmap.py --vram 0x801B7EBC
    tools/guestmap.py --rom 0x2F180
    tools/guestmap.py --list                      # the whole segment/record table
    tools/guestmap.py 0x802395E0 --context 4      # +-N symbols around the address

Addresses are auto-detected: anything >= 0x80000000 is a VRAM address,
otherwise it is tried as a ROM offset first and then as a VRAM address. Use
`--rom`/`--vram` to force one.

The map comes from `tools/n64map.py` (config.yaml, config-bank*.yaml,
app/src/bank_funcs.inc, app/src/bank_overlays.cpp, build/*.elf).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from n64map import (RDRAM_BASE, Map, Segment, expected_elf,  # noqa: E402
                    load_map, parse_addr)


def fmt_rom(off: int) -> str:
    return "0x%06X" % off


def describe(m: Map, value: int, force: str | None, context: int) -> None:
    is_vram = force == "vram" or (force is None and value >= RDRAM_BASE)
    seg: Segment | None = None
    rom: int | None = None
    vram: int | None = None

    if is_vram:
        vram = value
        seg = m.by_vram(vram)
        rom = m.vram_to_rom(vram)
    else:
        seg = m.by_rom(value)
        rom = value if seg is not None else None
        vram = m.rom_to_vram(value) if seg is not None else None
        if seg is None:                       # not a ROM offset: try VRAM
            seg = m.by_vram(value)
            if seg is not None:
                vram = value
                rom = m.vram_to_rom(value)

    if is_vram:
        print("0x%08X  VRAM" % value)
    else:
        print("0x%08X  ROM" % value)
    if seg is None:
        print("    not in any mapped segment (heap, stack, or streamed asset space)")
    else:
        plus = ""
        if seg.vram is not None and vram is not None:
            plus = " +0x%X" % (vram - seg.vram)
        print("    %s rom %s%s (size 0x%X, %s)"
              % (seg.label().ljust(28), fmt_rom(seg.rom), plus, seg.size, seg.source))
    if rom is not None:
        print("    rom %s" % fmt_rom(rom))
    if vram is not None and m.in_rdram(vram):
        print("    vram 0x%08X" % vram)

    # Function-entry check. The ELF matters: bank units are linked at RAM ranges
    # the main ELF also uses, so e.g. 0x801B7EBC has a main-ELF symbol (inside
    # overlay C's layout) *and* a unit-C symbol (record 10's layout) — only the
    # game's DMA says which module is live.
    if vram is not None and m.in_rdram(vram):
        elf = expected_elf(seg)
        exact = m.symbol_at(vram, elf)
        if exact is not None:
            kind = exact.role
            print("    symbol: %s (%s, exact entry, %s)" % (exact.name, kind, exact.source))
        else:
            near = m.nearest_symbol(vram, elf)
            if near is not None:
                sym, delta = near
                print("    symbol: %s +0x%X  (NOT a function entry, %s)"
                      % (sym.name, delta, sym.source))
            elif elf is not None:
                print("    symbol: none in %s (raw code/data without a label)" % elf)
        # Symbols at the same address from *other* ELFs: the other bank layout.
        for sym in m.symbols():
            if sym.addr == vram and (elf is None or sym.source != elf) \
                    and "NON_MATCHING" not in sym.name and not sym.is_section_marker:
                print("    also:   %s (%s) — a different module's layout shares this RAM"
                      % (sym.name, sym.source))
        if context > 0:
            for sym in m.symbols_near(vram, 0x2000, elf)[: context * 2]:
                mark = "->" if sym.addr == vram else "  "
                print("       %s 0x%08X  %s" % (mark, sym.addr, sym.name))

    # Which *other* records can own this RAM? (swappable-bank warning)
    if vram is not None and m.in_rdram(vram):
        others = []
        for other in m.segments:
            if other is seg or other.vram is None or other.kind != "record":
                continue
            if other.vram <= vram < other.vram_end:
                others.append(other)
        if others:
            print("    shared RAM: %d other record(s) map here — which one is live is "
                  "decided by the game's DMA:" % len(others))
            for other in others:
                print("       %s  rom %s size 0x%X" % (other.label().ljust(28),
                                                       fmt_rom(other.rom), other.size))
    print()


def list_table(m: Map) -> None:
    print("segments (sorted by vram, then rom):")
    for seg in sorted(m.segments, key=lambda s: (s.vram or 0, s.rom)):
        vram = "0x%08X" % seg.vram if seg.vram is not None else "        -"
        print("  vram %s  rom %s  size 0x%-7X  %s  [%s]"
              % (vram, fmt_rom(seg.rom), seg.size, seg.label(), seg.source))
    syms = m.symbols()
    elfs = sorted({s.source for s in syms})
    print("\n%d symbol(s) from %s"
          % (len(syms), ", ".join(elfs) if elfs else "no ELF — run `make` to link them"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="*", help="one or more addresses")
    ap.add_argument("--vram", action="store_true", help="force VRAM interpretation")
    ap.add_argument("--rom", action="store_true", help="force ROM interpretation")
    ap.add_argument("--list", action="store_true", help="print the segment/record table")
    ap.add_argument("--context", type=int, default=0, metavar="N",
                    help="also print symbols within +-0x2000 (N per side)")
    args = ap.parse_args(argv)

    force = "vram" if args.vram else ("rom" if args.rom else None)
    m = load_map()
    if args.list:
        list_table(m)
        return 0
    if not args.addresses:
        ap.print_help()
        return 2
    for text in args.addresses:
        describe(m, parse_addr(text), force, args.context)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
