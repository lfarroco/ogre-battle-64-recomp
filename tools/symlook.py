#!/usr/bin/env python3
"""Resolve guest VRAM addresses to function names using the linked bank/main ELFs.

`OGRE_PROFILE=1` reports its hot functions as bare guest addresses. Naming them
by hand means reading objdump output; this does it in one shot. The ELFs are the
same ones `make bank-recomp` links (build/bank*.elf, build/ogrebattle64.elf), and
every function symbol in them is named for its VRAM, so a lookup is exact for
function entry addresses. An address that is not a symbol is reported with the
nearest preceding symbol and the offset, which is what a loop *inside* a function
looks like in a sampler.

    tools/symlook.py 0x80214FA0 0x801B7FC0 0x8008AFE0
    tools/symlook.py --profile /tmp/ogre-lag.log      # every hot/core address in a log
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def elf_symbols(path):
    """[(addr, name)] for the FUNC symbols of one ELF, sorted by address."""
    try:
        out = subprocess.run(
            ["mips-linux-gnu-nm", "-n", "--defined-only", path],
            capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"symlook: cannot read {path}: {exc}", file=sys.stderr)
        return []
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        kind = parts[-2]
        if kind not in ("T", "t", "W", "w"):
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if 0x80000000 <= addr < 0x80800000:
            syms.append((addr, parts[-1]))
    syms.sort()
    return syms


def load_all():
    syms = []
    main = os.path.join(BUILD, "ogrebattle64.elf")
    if os.path.exists(main):
        syms += elf_symbols(main)
    for name in sorted(os.listdir(BUILD)) if os.path.isdir(BUILD) else []:
        if re.fullmatch(r"bank[A-Z]+\.elf", name):
            syms += elf_symbols(os.path.join(BUILD, name))
    syms.sort()
    return syms


def resolve(syms, addr):
    """(name, offset) for the symbol at or before addr, or (None, None)."""
    import bisect
    i = bisect.bisect_right(syms, (addr, "\uffff")) - 1
    if i < 0:
        return None, None
    base, name = syms[i]
    return name, addr - base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="*", help="guest addresses (0x80......)")
    ap.add_argument("--profile", metavar="LOG",
                    help="resolve the addresses a profile log mentions")
    args = ap.parse_args()

    syms = load_all()
    if not syms:
        print("symlook: no ELFs found in build/; run `make bank-recomp` first", file=sys.stderr)
        return 1

    addrs = [int(a, 16) for a in args.addr]
    if args.profile:
        text = open(args.profile, errors="replace").read()
        # [prof] T=... t3:80214FA0(87%) ... ; [prof]   hot: 80214FA0 x123 ...
        for m in re.finditer(r"\b(80[0-9A-Fa-f]{6})\b", text):
            addrs.append(int(m.group(1), 16))
    if not addrs:
        ap.error("give addresses, or --profile <log>")

    seen = set()
    for addr in addrs:
        if addr in seen:
            continue
        seen.add(addr)
        name, off = resolve(syms, addr)
        if name is None:
            print(f"0x{addr:08X}  (no symbol)")
        elif off:
            print(f"0x{addr:08X}  {name}+0x{off:X}")
        else:
            print(f"0x{addr:08X}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
