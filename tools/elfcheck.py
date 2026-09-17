#!/usr/bin/env python3
"""Compare every linked ELF's bytes against the ROM at the same ROM offset.

The recompiler reads the *bank ELFs* (and the main unit's ELF), not the ROM, so
an instruction whose bytes differ from the ROM becomes a different instruction
in the port. The assembler and linker are supposed to reproduce the ROM byte for
byte; when they do not, the difference is invisible until a scene misbehaves.

Session 65 is the reason this exists. `mips-linux-gnu-as` aligns `.text` to 16
bytes and pads the section to that boundary. Unit M's code/data boundary is ROM
0x85838, which is only 8-byte aligned, so the generated object was 8 bytes too
long, the linker placed the *data* subsegment 8 bytes high, and **every data
label landed 8 bytes above its ROM address**. Only the `lui/%lo` immediates that
reference those labels changed (150 of unit M's 480 address-named symbols were
+8), so the ELF looked fine in a diff of the code. At runtime the map's sprite
table `D_ovlM_801A6FD8` was read at 0x801A6FE0, shifting every sprite descriptor
by one entry: the party drew a 16x11 crop of the 32x32 knight sheet, the shadow
drew as a 144x23 band, the cursor and the date plate were wrong sizes, and the
month name came from the wrong table entry.

`--syms` additionally audits that every symbol whose name ends in eight hex
digits is *defined* at that address (`D_ovlM_801A6FD8` -> 0x801A6FD8). A nonzero
`--allow` sum means a section is misplaced (fixed with the config's `align` /
subsegment boundary, never by editing the generated `.s`).

Usage:
    tools/elfcheck.py                 # every build/*.elf, byte comparison
    tools/elfcheck.py --syms          # also the symbol-name audit
    tools/elfcheck.py build/bankM.elf # one ELF
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJDUMP = "mips-linux-gnu-objdump"
NM = "mips-linux-gnu-nm"

SECTION_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+"
)
SYM_RE = re.compile(r"^[0-9a-f]{8} \w (\w+_)?(?:ovl[A-Z]?_)?([0-9A-F]{8})$")


def elf_sections(elf: Path):
    """Sections that hold file bytes, with their ROM address (LMA) and offset."""
    out = subprocess.run([OBJDUMP, "-h", str(elf)], capture_output=True, text=True).stdout
    sections = []
    cur = None
    for line in out.splitlines():
        m = SECTION_RE.match(line)
        if m:
            if cur is not None:
                sections.append(cur)
            cur = {
                "name": m.group(1),
                "size": int(m.group(2), 16),
                "lma": int(m.group(4), 16),
                "off": int(m.group(5), 16),
                "contents": False,
            }
        elif cur is not None and "CONTENTS" in line:
            cur["contents"] = True
    if cur is not None:
        sections.append(cur)
    return [s for s in sections if s["contents"] and s["size"] > 0]


def check_bytes(elf: Path, rom: bytes) -> int:
    data = elf.read_bytes()
    total = differing = 0
    for s in elf_sections(elf):
        rompart = rom[s["lma"] : s["lma"] + s["size"]]
        elpart = data[s["off"] : s["off"] + s["size"]]
        if len(rompart) != len(elpart):
            print(f"  {elf.name}: section {s['name']} runs past the ROM")
            differing += 1
            continue
        total += len(rompart)
        n = sum(1 for a, b in zip(rompart, elpart) if a != b)
        if n:
            first = next(i for i, (a, b) in enumerate(zip(rompart, elpart)) if a != b)
            print(
                f"  {elf.name}: {s['name']} @ROM 0x{s['lma'] + first:X} "
                f"{n}/{len(rompart)} bytes differ (elf "
                f"{elpart[first:first + 8].hex()} vs rom {rompart[first:first + 8].hex()})"
            )
        differing += n
    print(f"{elf.name}: {differing} differing bytes of {total}")
    return differing


def check_symbols(elf: Path) -> int:
    out = subprocess.run([NM, str(elf)], capture_output=True, text=True).stdout
    total = bad = 0
    deltas: dict[int, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr, name = parts[0], parts[2]
        if ".NON_MATCHING" in name:
            continue
        m = re.search(r"_([0-9A-F]{8})$", name)
        if m is None:
            continue
        total += 1
        want = int(m.group(1), 16)
        got = int(addr, 16)
        if got != want:
            bad += 1
            deltas[got - want] = deltas.get(got - want, 0) + 1
    if bad:
        print(f"  {elf.name}: {bad}/{total} address-named symbols misplaced: {deltas}")
    else:
        print(f"{elf.name}: {total} address-named symbols all at their named address")
    return bad


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    want_syms = "--syms" in sys.argv
    if args:
        elfs = [Path(a) for a in args]
    else:
        elfs = sorted(ROOT.glob("build/bank*.elf")) + [ROOT / "build/ogrebattle64.elf"]
    rom_path = ROOT / "assets/ogre64.z64"
    if not rom_path.exists():
        print(f"elfcheck: {rom_path} not found (bring your own ROM)", file=sys.stderr)
        return 2
    rom = rom_path.read_bytes()
    bad = 0
    for elf in elfs:
        if not elf.exists():
            print(f"elfcheck: {elf} not found; run `make bank` first", file=sys.stderr)
            return 2
        bad += check_bytes(elf, rom)
        if want_syms:
            bad += check_symbols(elf)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
