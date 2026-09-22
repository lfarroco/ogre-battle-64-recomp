#!/usr/bin/env python3
"""Generate app/src/bank_funcs.inc from the bank units' recomp_overlays.inl files.

Each bank unit (config/banks/config-bank<U>.yaml / .toml, built by `make bank-recomp`)
recompiles a set of streamed-overlay records at their true RAM addresses, with
its own `ovl<U>_` symbol namespace so the units can be linked alongside each
other and the main unit.

The app cannot include a unit's `recomp_overlays.inl` directly: it defines
`section_table`/`num_sections`, which the main unit's copy also defines. Instead
this script flattens every `Bank*Funcs/recomp_overlays.inl` into one
self-contained table:

    extern "C" void <function>(uint8_t*, recomp_context*);      // declarations
    static const recomp::overlays::BankFunctionEntry k<U>_<seg>Functions[] = {
        { <absolute ram address>, <function> }, ...
    };
    static const BankRecord kBankRecords[] = {
        { <rom start>, <ram start>, <size>, <ram end>, k<U>_<seg>Functions, N },
        ...
    };

    <ram end> is the record's RAM end (exclusive) from the game's segment table
    (ROM 0x387C0, 0x28-byte entries); the loader zeroes [ram start + size,
    ram end) as BSS on every load. Records that are not segment-table entries
    (explicit chunk DMAs like bankRec10a/10b/14a/14b/14d and the scene modules)
    have no BSS: ram end is ram start + size for those (see RAM_END, which
    defaults that way) — except bankRec14d, whose bss the game itself zeroes.

app/src/bank_overlays.cpp uses it to register a record's functions when the game
DMA's it, and to drop them when another bank overwrites that RAM.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SECTION_TABLE_RE = re.compile(
    r"\{\s*\.rom_addr\s*=\s*0x([0-9A-Fa-f]+),\s*\.ram_addr\s*=\s*0x([0-9A-Fa-f]+),"
    r"\s*\.size\s*=\s*0x([0-9A-Fa-f]+),\s*\.funcs\s*=\s*(\w+),"
)
FUNC_ENTRY_RE = re.compile(
    r"\{\s*\.func\s*=\s*(\w+),\s*\.offset\s*=\s*0x([0-9A-Fa-f]+),"
    r"\s*\.rom_size\s*=\s*0x([0-9A-Fa-f]+)\s*\}"
)
FUNCS_ARRAY_RE = re.compile(r"static FuncEntry (\w+)\[\]\s*=\s*\{")


def parse_func_arrays(text: str) -> dict[str, list[tuple[str, int]]]:
    """name -> [(func_symbol, offset), ...]"""
    arrays: dict[str, list[tuple[str, int]]] = {}
    current: str | None = None
    for line in text.splitlines():
        m = FUNCS_ARRAY_RE.search(line)
        if m:
            current = m.group(1)
            arrays[current] = []
            continue
        if current is None:
            continue
        if line.strip() == "};":
            current = None
            continue
        m = FUNC_ENTRY_RE.search(line)
        if m:
            arrays[current].append((m.group(1), int(m.group(2), 16)))
    return arrays


# RAM end (exclusive) per segment-table record, parsed from the game's segment
# table at ROM 0x387C0 (0x28-byte entries {ram start, ram end, rom start,
# rom end, ...}). The record loader zeroes [ram start + rom size, ram end) as
# BSS on every load, and the port must do the same: game code in scene 0x0D
# reads those ranges as empty lists/tables and walks stale bytes as pointers
# otherwise. Keyed by ROM start; records absent here are explicit chunk DMAs
# with no BSS (ram end = ram start + size).
RAM_END: dict[int, int] = {
    0x066E30: 0x8019A690,  #  0 (unit E)
    0x069920: 0x8019C950,  # 17 (unit B)
    0x06E680: 0x801F1530,  #  1 (unit D)
    0x0E4910: 0x8019EE70,  #  2
    0x0EBBD0: 0x801AD5C0,  #  3 (unit A)
    0x0FA010: 0x8019F450,  #  4 (unit C)
    0x0FA600: 0x801B4D60,  #  6 (unit A)
    0x1F0A00: 0x801F7100,  # 10 (unit C)
    0x24BC70: 0x8020A300,  # 11 (unit C)
    0x25EE60: 0x802210E0,  # 12 (unit C)
    0x275820: 0x802258B0,  # 13 (unit C)
    0x281830: 0x80243DD0,  # 14 (unit C)
    0x286BA0: 0x8023E630,  # 14's chapter-animation bank (unit K, explicit DMA)
    0x1BA020: 0x80230600,  # 18 (unit B)
    0x1C32D0: 0x802305F0,  # 18's tutorial-practice bank (unit T, explicit DMA)
    0x1C9020: 0x8022F880,  # 18's mode-2 bank (unit U, no BSS: ram+size)
    0x1A4BE0: 0x802196B0,  #  9's settings/options-menu bank (unit V)
    0x177EF0: 0x8021E360,  #  9 arena bank (unit Y)
    0x188B80: 0x8021B610,  #  9 arena bank (unit Z)
    0x18F120: 0x8021B360,  #  9 arena bank (unit AA)
    0x1977B0: 0x8021A160,  #  9's shop/item screen (unit AB)
    0x19C730: 0x8021B530,  #  9 arena bank (unit AC)
    0x1A2BF0: 0x80216FB0,  #  9 arena bank (unit AD)
    0x1A9260: 0x8021E410,  #  9 arena bank (unit AE)
    0x1B2640: 0x8021CB50,  #  9 arena bank (unit AF)
    0x171EC0: 0x8021B010,  #  9 arena bank (unit P; size corrected in session 72)
    0x165FE0: 0x80220F50,  #  9 arena bank (unit Q)
    0x17F9E0: 0x8021E150,  #  9's combat bank (unit W)
    0x22A250: 0x801F70F0,  # 10's battle bank (unit X, no BSS: ram+size)
    0x101D00: 0x801F4050,  #  7 (unit N, the mission scene 0x03)
    0x145230: 0x801FDA90,  #  8 (unit N)
    0x14EC00: 0x80220F60,  #  9 (unit N)
}


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    out_path = root / "app" / "src" / "bank_funcs.inc"

    unit_dirs = sorted(p for p in root.glob("Bank*Funcs") if p.is_dir())
    if not unit_dirs:
        print("gen_bank_funcs: no Bank*Funcs/ dirs (run `make bank-recomp`)", file=sys.stderr)
        return 1

    # rom, ram, size, table name
    records: list[tuple[int, int, int, str]] = []
    declarations: dict[str, None] = {}
    tables: list[str] = []
    total_funcs = 0

    for unit_dir in unit_dirs:
        inl = unit_dir / "recomp_overlays.inl"
        if not inl.exists():
            print(f"gen_bank_funcs: {inl} missing", file=sys.stderr)
            return 1

        # BankAFuncs -> A
        unit = re.sub(r"Funcs$", "", re.sub(r"^Bank", "", unit_dir.name))

        text = inl.read_text()
        arrays = parse_func_arrays(text)

        for m in SECTION_TABLE_RE.finditer(text):
            rom = int(m.group(1), 16)
            ram = int(m.group(2), 16)
            size = int(m.group(3), 16)
            funcs = m.group(4)
            if funcs not in arrays:
                print(f"gen_bank_funcs: {inl}: missing function array {funcs}", file=sys.stderr)
                return 1

            seg = re.sub(r"_funcs$", "", re.sub(r"^section_\d+_", "", funcs))
            table = f"k{unit}_{seg}Functions"

            tables.append(f"static const recomp::overlays::BankFunctionEntry {table}[] = {{")
            for sym, offset in arrays[funcs]:
                declarations.setdefault(
                    sym, f'extern "C" void {sym}(uint8_t* rdram, recomp_context* ctx);'
                )
                tables.append(f"    {{ (int32_t)0x{ram + offset:08X}u, {sym} }},")
            tables.append("};")
            tables.append("")

            records.append((rom, ram, size, table))
            total_funcs += len(arrays[funcs])

    if not records:
        print("gen_bank_funcs: no sections found", file=sys.stderr)
        return 1

    out: list[str] = [
        "// GENERATED by tools/gen_bank_funcs.py from Bank*Funcs/recomp_overlays.inl.",
        "// Do not edit by hand; run `make bank-recomp`.",
        "",
        *sorted(declarations.values()),
        "",
        *tables,
        "static const BankRecord kBankRecords[] = {",
    ]
    for rom, ram, size, table in records:
        ram_end = RAM_END.get(rom, ram + size)
        if ram_end < ram + size:
            print(
                f"gen_bank_funcs: record rom=0x{rom:X} has ram_end below "
                f"ram+size; using ram+size",
                file=sys.stderr,
            )
            ram_end = ram + size
        out.append(
            f"    {{ 0x{rom:08X}u, (int32_t)0x{ram:08X}u, 0x{size:08X}u, "
            f"(int32_t)0x{ram_end:08X}u, {table}, ARRLEN({table}) }},"
        )
    out.append("};")
    out.append("")

    out_path.write_text("\n".join(out))
    print(
        "gen_bank_funcs: wrote %s (%d unit(s), %d record(s), %d function(s))"
        % (out_path.relative_to(root), len(unit_dirs), len(records), total_funcs)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
