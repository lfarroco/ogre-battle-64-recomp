#!/usr/bin/env python3
"""Define data labels a bank unit references but splat did not emit.

The arena modules (config/banks/config-bankC.yaml, bankRec10a/b) are streamed code compiled as
`asm` sections. When that code references a data label inside the same module via
a `%hi/%lo` pair, splat can create the *reference* without emitting the label's
definition, which leaves the unit's link with undefined symbols.

The generated name encodes the VRAM (`D_ovlC_801EFEAC`), and a unit is linked at
its true RAM addresses, so an absolute linker definition is exactly right — this
is the same idea as splat's own `undefined_syms_auto.txt`, restricted to the
labels that are actually missing.

Usage: gen_bank_syms.py <unit-letter>   (writes build/bank<U>/extra_syms.txt)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

DEF_RE = re.compile(
    r"^\s*(?:glabel|dlabel|alabel|jlabel|ehlabel|endlabel|enddlabel|nonmatching)\s+"
    r"(\w+)",
    re.M,
)
LOCAL_RE = re.compile(r"^\s*(\.L[0-9A-Fa-f]+):", re.M)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gen_bank_syms.py <unit-letter>", file=sys.stderr)
        return 2
    unit = sys.argv[1]

    root = Path(__file__).resolve().parent.parent
    asm_dir = root / f"build/bank{unit}/asm"
    if not asm_dir.is_dir():
        print(f"gen_bank_syms: {asm_dir} not found (run `make bank-split`)", file=sys.stderr)
        return 1

    texts = [p.read_text() for p in sorted(asm_dir.rglob("*.s"))]

    defined: set[str] = set()
    for text in texts:
        defined.update(m.group(1) for m in DEF_RE.finditer(text))
        defined.update(m.group(1) for m in LOCAL_RE.finditer(text))

    # A reference can be emitted under either name: splat's globals carry the
    # unit prefix (`D_ovlI_801D7D70`), while a `%hi/%lo` pair whose target is not
    # a global in the unit's layout can come out as spimdisasm's local label
    # (`.LovlI_801D7D70`). Both are the same absolute address and both need a
    # definition; session 58's unit I (`bankRec16`) links only with the `.L` form
    # handled (the module's tail words are not emitted as data labels).
    ref_re = re.compile(r"\.?L?D?_?ovl" + re.escape(unit) + r"_([0-9A-Fa-f]{8})\b")
    referenced: set[str] = set()
    for text in texts:
        referenced.update(m.group(0) for m in ref_re.finditer(text))

    missing = sorted(referenced - defined)

    out = root / f"build/bank{unit}/extra_syms.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        for name in missing:
            addr = int(name.rsplit("_", 1)[1], 16)
            f.write(f"{name} = 0x{addr:08X};\n")

    print(f"gen_bank_syms: unit {unit}: {len(missing)} absolute symbol(s) -> {out.relative_to(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
