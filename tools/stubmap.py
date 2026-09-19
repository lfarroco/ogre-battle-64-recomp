#!/usr/bin/env python3
"""Find the dispatch targets that can fall through to the runtime's
streamed-function stub, and name what points at them.

The port's remaining whack-a-mole bug class is a call that reaches
`recomp::overlays::get_function` for an address that is not in the function map
at that moment. The runtime then logs

    [overlays] streamed function stub called @ 0x80219594 (not yet loaded)

and returns a no-op, so the calling scene silently does nothing -- session 82's
neutral encounter, session 73's settings menu and session 55's form were all
this shape. Waiting for a run to hit each one is the whack-a-mole; this tool
asks the same question of the build artifacts, for every dispatch site at once.

Three things decide whether a dispatched address resolves.

1. **Is the RAM owned?** A streamed module owns a RAM range only while the game
   has DMA'd it there. `app/src/bank_funcs.inc` is the exact table the runtime
   registers from (`on_streamed_dma` -> `load_function_bank`), and the main
   ELF's `RecompiledFuncs/recomp_overlays.inl` section table is what
   `load_overlays(0x1000, entrypoint, 0x3E1B0)` installs for the base sections
   (it is the only place a *reimplemented* function like `osSetIntMask_recomp`
   appears at all -- the generated C has no body for it). An address inside no
   module's code is a call into RAM nothing compiles: the missing-bank class
   (session 45's `bankRec14c`, session 82's battle setup fragment).
2. **Does the module register the address?** A module's map is its function
   table. An address in a module that does not list it is a *body interior* in
   that layout, so the call runs into the middle of another function -- or, if
   that bank is the one the caller itself runs from, into a body of the caller's
   own bank.
3. **Was the module registered at all?** A module can be resident and still
   absent from the map when the DMA that loaded it did not match a
   `kBankRecords` entry (wrong rom->ram delta, or a load path that bypasses
   `notify_rom_read`). Only a run can show that, so `stubmap.py log <run.log>`
   reads the stub hits and identifies the resident module from the *live bytes
   the runtime already printed* at the stub address, by matching them against
   each candidate's ROM image (`[overlays] @0xADDR: <4 words>`).

The report layers the targets by how much is actually known:

* **DEFINITE** -- either **no module registers the address at all** (so no run
  state can resolve the lookup: `func_map` is only ever filled from those
  tables), or the bank the caller executes from has no entry at the target while
  the address is inside that bank's own DMA'd code. The first is the session-82
  shape -- a bank the port has no record for is resident, and the call lands
  where a compiled module's layout has no function. (A same-bank `jal` normally
  compiles to a body call rather than a lookup, so the second form is rare.)
* **MISSING FUNCTION** -- a bank that can be resident does not register the
  address, but its *own ROM bytes* at that address open like a function
  prologue. Either the split missed a function there (one reachable only through
  a data pointer is exactly the case a disassembler cannot find), or the port
  has no record for that bank at all. This is the shortest list to act on.
* **UNRESOLVABLE** -- no compiled module's *code* covers the address.
* **BANK SELECTION** -- a bank that can be resident lacks the entry, but the
  bytes there are a body interior: the caller disposed of another bank's
  address. Which bank of an arena is resident is chosen by the game's scene
  data, so these are candidates a run confirms.
* **INERT** -- the missing bank's segment-table record is never loaded by a
  scene that also loads the caller's record, so it cannot be resident then.
* **RESOLVABLE**, and **RSP IMEM** (0x84000000: recompiled microcode text the CPU
  never runs; `get_function` accepts the window, so it would log as a stub).

Not every "missing entry" is a bug, and the tool does not pretend otherwise: a
target absent from a bank that a scene can never have resident while the caller
runs cannot be hit, and which bank of one record is resident is decided by the
game's data, not by the build. `tools/cross_bank.py` remains the guard for the
*wrong-binding* half (a direct call bound to one bank's layout); this tool covers
the *unbound* half -- the dispatch that finds nothing.

Commands (all offline; only `log` needs a run log):

    tools/stubmap.py report                 # every dispatched target, ranked
    tools/stubmap.py report --strict        # exit 1 on a live gap
    tools/stubmap.py report --json          # machine-readable
    tools/stubmap.py report --verbose       # also list inert gaps and resolved
    tools/stubmap.py target 0x80219594      # deep dive: owners, bodies, callers,
                                            #   data pointers, scenes
    tools/stubmap.py log /tmp/run.log       # attribute a run's stub hits
    tools/stubmap.py pointers               # data words pointing at risky targets
                                            #   (jalr / callback tables)

The call-site model comes from the generated C: N64Recomp annotates every `jal`
with its own address and target (`// 0x801AFE88: jal 0x80219594`), so caller
function, call line and target are all recoverable. `LOOKUP_FUNC(0xADDR)` is the
form that really goes through `get_function`; a direct `func_XXXXXXXX(rdram,
ctx);` call does not (that is the wrong-layout-binding class
`tools/cross_bank.py check` guards, reported here only as a count).
`LOOKUP_FUNC(ctx->rN)` is a `jalr` whose target comes from a register, usually a
data pointer: `pointers` and a run log are how those are reached.

This is an analysis tool: it writes nothing and changes no build input.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from cross_bank import SCENE_DESCRIPTOR_MASKS, record_number  # noqa: E402

BANK_FUNCS_INC = ROOT / "app" / "src" / "bank_funcs.inc"
BANK_OVERLAYS_CPP = ROOT / "app" / "src" / "bank_overlays.cpp"
MAP_PATH = ROOT / "build" / "ogrebattle64.map"
ROM_PATH = ROOT / "assets" / "ogre64.z64"
RECOMP_DIR = ROOT / "RecompiledFuncs"

# Everything at or above this RAM address can be swapped by a streamed record;
# below it are the base sections (entry/main), which are linear and never move.
STREAMED_LO = 0x800E0000
# `get_function` also accepts this window as "streamed" (see
# librecomp/src/overlays.cpp). It is RSP IMEM: the game's microcode text is
# disassembled there as MIPS because it sits in a code segment, and the CPU
# never runs it. A dispatch into it is expected to stub.
IMEM_LO, IMEM_HI = 0x84000000, 0x84200000

FUNC_DEF_RE = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{$")
TRACE_RE = re.compile(r"recomp_trace_entry\(rdram, 0x([0-9A-Fa-f]{8}),")
JAL_COMMENT_RE = re.compile(r"^\s*// 0x([0-9A-Fa-f]{8}): jal\s+0x([0-9A-Fa-f]{8})")
LOOKUP_RE = re.compile(r"^\s*LOOKUP_FUNC\(0x([0-9A-Fa-f]{8})\)\(rdram, ctx\);")
LOOKUP_REG_RE = re.compile(r"^\s*LOOKUP_FUNC\(ctx->(\w+)\)\(rdram, ctx\);")
CALL_RE = re.compile(r"^(\s*)(\w+)\(rdram, ctx\);$")
FUNC_VRAM_RE = re.compile(r"^func_(?:ovl[A-Z]+_)?([0-9A-Fa-f]{8})$")

BANK_ARRAY_RE = re.compile(
    r"^static const recomp::overlays::BankFunctionEntry (\w+)\[\] = \{$"
)
BANK_ENTRY_RE = re.compile(r"^\s*\{ \(int32_t\)0x([0-9A-Fa-f]+)u, (\w+) \},$")
BANK_RECORD_RE = re.compile(
    r"^\s*\{ 0x([0-9A-Fa-f]+)u, \(int32_t\)0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, "
    r"\(int32_t\)0x([0-9A-Fa-f]+)u, (\w+), ARRLEN\(\5\) \},"
)
SECTION_RE = re.compile(
    r"^(\.\w+)\s+0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+load address 0x([0-9A-Fa-f]+)"
)
STREAMED_RECORD_RE = re.compile(
    r"^\s*\{ 0x([0-9A-Fa-f]+)u, \(int32_t\)0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, "
    r"(true|false)\s*\},\s*//\s*(\d+)"
)
MAIN_INL = RECOMP_DIR / "recomp_overlays.inl"
INL_FUNC_ARRAY_RE = re.compile(r"^static FuncEntry (\w+)\[\] = \{$")
INL_FUNC_ENTRY_RE = re.compile(
    r"^\s*\{ \.func = (\w+), \.offset = 0x([0-9A-Fa-f]+), "
    r"\.rom_size = 0x([0-9A-Fa-f]+) \},$"
)
INL_SECTION_RE = re.compile(
    r"^\s*\{ \.rom_addr = 0x([0-9A-Fa-f]+), \.ram_addr = 0x([0-9A-Fa-f]+), "
    r"\.size = 0x([0-9A-Fa-f]+), \.funcs = (\w+),"
)
MAIN_SECTION_NAME_RE = re.compile(r"^section_\d+_(\w+)_funcs$")

LOG_WORDS_RE = re.compile(r"^\[overlays\]\s+@0x([0-9A-Fa-f]{8}): ((?:[0-9A-Fa-f]{8} ?)+)\s*$")
LOG_STUB_RE = re.compile(r"^\[overlays\] streamed function stub called @ 0x([0-9A-Fa-f]{8})")
LOG_CHAIN_RE = re.compile(r"^\[snap\] callchain t(-?\d+) (.*?) \((\d+) deep, outermost first\): (.*)$")
LOG_UNKNOWN_RE = re.compile(
    r"^\[bank\] UNKNOWN module rom=0x([0-9A-Fa-f]+) ram=0x([0-9A-Fa-f]+)"
)
LOG_UNCOMPILED_RE = re.compile(
    r"^\[bank\] UNCOMPILED streamed record (\d+): rom=0x([0-9A-Fa-f]+) "
    r"ram=0x([0-9A-Fa-f]+) size=0x([0-9A-Fa-f]+)"
)
LOG_LOADED_RE = re.compile(
    r"^\[bank\] loading overlay record rom=0x([0-9A-Fa-f]+) ram=0x([0-9A-Fa-f]+) "
    r"size=0x([0-9A-Fa-f]+)"
)


class ToolError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Module model: every RAM range the game can have code in, and the addresses it
# registers while resident.
# ---------------------------------------------------------------------------


@dataclass
class Module:
    kind: str  # 'main' | 'bank'
    name: str  # 'streamedC' | 'bankRec9ab'
    unit: str  # 'main' | 'AB'
    rom: int
    ram: int
    size: int  # code size (the DMA's); 0 for a BSS-only section
    ram_end: int  # end of the RAM span, BSS included
    entries: set[int] = field(default_factory=set)
    names: dict[int, str] = field(default_factory=dict)
    record: int | None = None

    @property
    def hi(self) -> int:
        return self.ram_end

    @property
    def code_hi(self) -> int:
        return self.ram + self.size

    def owns_code(self, addr: int) -> bool:
        return self.ram <= addr < self.code_hi

    def rom_of(self, addr: int) -> int | None:
        """The ROM offset this address's bytes come from, if in the code half."""
        if not self.owns_code(addr):
            return None
        return self.rom + (addr - self.ram)


def streamed_record_table() -> list[tuple[int, int, int, bool]]:
    """`kAllStreamedRecords` (record number -> rom, ram, size, compiled).

    That table is the game's segment-table view of every streamed record; it is
    what gives a main-ELF section its record number (overlay C is record 15).
    """
    if not BANK_OVERLAYS_CPP.exists():
        return []
    out: list[tuple[int, int, int, bool]] = []
    in_table = False
    for line in BANK_OVERLAYS_CPP.read_text().splitlines():
        if line.startswith("static const StreamedRecord kAllStreamedRecords[]"):
            in_table = True
            continue
        if in_table:
            if line.startswith("};"):
                break
            m = STREAMED_RECORD_RE.match(line)
            if m:
                out.append((int(m.group(1), 16), int(m.group(2), 16),
                            int(m.group(3), 16), m.group(4) == "true"))
    return out


def parse_bank_funcs_inc() -> list[Module]:
    """The runtime's own table: app/src/bank_funcs.inc (write-seeds/[bank])."""
    if not BANK_FUNCS_INC.exists():
        raise ToolError(f"{BANK_FUNCS_INC.relative_to(ROOT)} missing (run `make bank-recomp`)")
    arrays: dict[str, set[int]] = {}
    names: dict[str, dict[int, str]] = {}
    cur: str | None = None
    records: list[tuple[int, int, int, int, str]] = []
    for line in BANK_FUNCS_INC.read_text().splitlines():
        m = BANK_ARRAY_RE.match(line)
        if m:
            cur = m.group(1)
            arrays[cur] = set()
            names[cur] = {}
            continue
        m = BANK_ENTRY_RE.match(line)
        if m and cur is not None:
            addr = int(m.group(1), 16)
            arrays[cur].add(addr)
            names[cur][addr] = m.group(2)
            continue
        if line.startswith("};"):
            cur = None
            continue
        m = BANK_RECORD_RE.match(line)
        if m:
            records.append((int(m.group(1), 16), int(m.group(2), 16),
                            int(m.group(3), 16), int(m.group(4), 16), m.group(5)))
    if not records:
        raise ToolError(f"no BankRecord rows parsed from {BANK_FUNCS_INC.name}")

    def unit_of(array: str) -> str:
        m = re.match(r"k([A-Z]+)_", array)
        return m.group(1) if m else "?"

    def record_of(array: str) -> int | None:
        m = re.search(r"bankRec\w+Functions", array)
        return record_number(m.group(0)) if m else None

    out = []
    for rom, ram, size, ram_end, array in records:
        out.append(Module(
            kind="bank",
            name=array.replace("Functions", "").removeprefix("k" + unit_of(array) + "_"),
            unit=unit_of(array), rom=rom, ram=ram, size=size, ram_end=ram_end,
            entries=arrays.get(array, set()), names=names.get(array, {}),
            record=record_of(array),
        ))
    return out


def linker_sections() -> list[tuple[str, int, int, int, bool]]:
    """Every main-ELF section: (name, rom, ram, size, is_bss), from the map.

    Used for the BSS spans (which register nothing) and the section names; the
    *function entries* come from the generated `recomp_overlays.inl` instead,
    because a reimplemented function (`osSetIntMask_recomp`) has a table entry
    but no generated C definition.
    """
    if not MAP_PATH.exists():
        raise ToolError(f"{MAP_PATH.relative_to(ROOT)} missing (run `make`)")
    out: list[tuple[str, int, int, int, bool]] = []
    for line in MAP_PATH.read_text().splitlines():
        m = SECTION_RE.match(line)
        if not m:
            continue
        name = m.group(1).lstrip(".")
        out.append((name, int(m.group(4), 16), int(m.group(2), 16),
                    int(m.group(3), 16), name.endswith("_bss")))
    if not out:
        raise ToolError(f"no sections parsed from {MAP_PATH.name}")
    return out


def main_inl_table():
    """The main unit's real registration table.

    Returns (sections, name_to_vram) where sections is
    [(name, rom, ram, size, entries)] from `RecompiledFuncs/recomp_overlays.inl`.
    This is the table `load_overlays(0x1000, entrypoint, 0x3E1B0)` installs, so
    it -- not the generated C -- is what `get_function` can resolve.
    """
    if not MAIN_INL.exists():
        raise ToolError(f"{MAIN_INL.relative_to(ROOT)} missing (run `make recomp`)")
    arrays: dict[str, list[tuple[str, int]]] = {}
    name_to_vram: dict[str, int] = {}
    cur: str | None = None
    sections: list[tuple[int, int, int, str]] = []
    for line in MAIN_INL.read_text().splitlines():
        m = INL_FUNC_ARRAY_RE.match(line)
        if m:
            cur = m.group(1)
            arrays[cur] = []
            continue
        m = INL_FUNC_ENTRY_RE.match(line)
        if m and cur is not None:
            arrays[cur].append((m.group(1), int(m.group(2), 16)))
            continue
        if line.startswith("};"):
            cur = None
            continue
        m = INL_SECTION_RE.match(line)
        if m:
            sections.append((int(m.group(1), 16), int(m.group(2), 16),
                             int(m.group(3), 16), m.group(4)))
    if not sections:
        raise ToolError(f"no section rows parsed from {MAIN_INL.name}")
    out = []
    for rom, ram, size, array in sections:
        m = MAIN_SECTION_NAME_RE.match(array)
        name = m.group(1) if m else array
        entries = set()
        for fname, offset in arrays.get(array, []):
            addr = ram + offset
            entries.add(addr)
            name_to_vram[fname] = addr
        out.append((name, rom, ram, size, entries))
    return out, name_to_vram


def load_modules() -> list[Module]:
    modules = parse_bank_funcs_inc()
    table = streamed_record_table()
    inl_sections, _names = main_inl_table()
    for name, rom, ram, size, entries in inl_sections:
        mod = Module(kind="main", name=name, unit="main", rom=rom, ram=ram,
                     size=size, ram_end=ram + size, entries=set(entries))
        mod.names = {a: f"{name}+0x{a - ram:X}" for a in mod.entries}
        for rec, (r, rm, _sz, _c) in enumerate(table):
            if r == rom and rm == ram:
                mod.record = rec
                break
        modules.append(mod)
    # BSS spans: they hold no code and register nothing, but a dispatch into one
    # is worth naming (it is where a stale address would land).
    for name, _rom, ram, size, is_bss in linker_sections():
        if not is_bss:
            continue
        modules.append(Module(kind="main", name=name, unit="main", rom=0,
                              ram=ram, size=0, ram_end=ram + size))
    return modules


# ---------------------------------------------------------------------------
# Call-site model
# ---------------------------------------------------------------------------


@dataclass
class Site:
    where: str  # 'RecompiledFuncs/funcs_3.c:14795'
    kind: str  # 'lookup' | 'direct' | 'indirect'
    target: int | None  # the address the dispatch reaches (None for indirect)
    caller_addr: int | None
    caller_name: str
    jal: int | None  # the `jal`'s own operand in the ROM
    unit: str  # the recompilation unit the caller was built in


def unit_of_path(path: Path) -> str:
    name = path.parent.name
    if name.startswith("Bank") and name.endswith("Funcs"):
        return name[len("Bank"):-len("Funcs")]
    return "main"


def collect_sites() -> list[Site]:
    sites: list[Site] = []
    _, name_to_vram = main_inl_table()
    paths = sorted(RECOMP_DIR.glob("*.c"))
    for unit_dir in sorted(ROOT.glob("Bank*Funcs")):
        if unit_dir.is_dir():
            paths.extend(sorted(unit_dir.glob("*.c")))
    for path in paths:
        rel = str(path.relative_to(ROOT))
        unit = unit_of_path(path)
        caller_name = "?"
        caller_addr: int | None = None
        want_addr = False
        jal: int | None = None
        for line_no, line in enumerate(path.read_text().splitlines(), 1):
            if line.startswith("RECOMP_FUNC void "):
                m = FUNC_DEF_RE.match(line)
                caller_name = m.group(1) if m else "?"
                caller_addr, want_addr, jal = None, True, None
                continue
            if want_addr:
                t = TRACE_RE.search(line)
                if t:
                    caller_addr = int(t.group(1), 16)
                    want_addr = False
            if "jal" in line:
                j = JAL_COMMENT_RE.match(line)
                if j:
                    jal = int(j.group(2), 16)
            if "LOOKUP_FUNC" in line:
                m = LOOKUP_RE.match(line)
                if m:
                    sites.append(Site(f"{rel}:{line_no}", "lookup", int(m.group(1), 16),
                                      caller_addr, caller_name, jal, unit))
                    jal = None
                    continue
                if LOOKUP_REG_RE.match(line):
                    sites.append(Site(f"{rel}:{line_no}", "indirect", None,
                                      caller_addr, caller_name, jal, unit))
                    jal = None
                continue
            if jal is not None and "rdram, ctx);" in line:
                m = CALL_RE.match(line)
                if m:
                    name = m.group(2)
                    if name in name_to_vram:
                        target = name_to_vram[name]
                    else:
                        v = FUNC_VRAM_RE.match(name)
                        target = int(v.group(1), 16) if v else None
                    sites.append(Site(f"{rel}:{line_no}", "direct", target,
                                      caller_addr, caller_name, jal, unit))
                    jal = None
    return sites


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


@dataclass
class Verdict:
    addr: int
    owners: list[Module]  # modules whose DMA'd *code* covers addr
    missing: list[Module]  # owners whose function map has no entry at addr
    registering: list[Module]
    bss_owners: list[Module]  # modules whose BSS/data span covers addr

    @property
    def status(self) -> str:
        if IMEM_LO <= self.addr < IMEM_HI:
            return "imem"
        if not self.owners:
            return "unowned"
        if self.missing:
            return "partial"
        return "ok"


def body_containing(module: Module, addr: int) -> int | None:
    """The nearest registered entry at or below addr (the body it lands in)."""
    best = None
    for e in module.entries:
        if e <= addr and (best is None or e > best):
            best = e
    return best


def classify(addr: int, modules: list[Module]) -> Verdict:
    # Ownership for *dispatch* is the code half only: a module whose BSS covers
    # the address registers nothing there, so it can never resolve the call (and
    # counting its whole segment as an owner hid session 82's gap inside record
    # 10's BSS span).
    owners = [m for m in modules if m.owns_code(addr)]
    bss_owners = [m for m in modules if m.code_hi <= addr < m.hi]
    return Verdict(addr, owners,
                   [m for m in owners if addr not in m.entries],
                   [m for m in owners if addr in m.entries], bss_owners)


def describe(module: Module) -> str:
    tag = f"unit {module.unit}" if module.kind == "bank" else "main ELF"
    rec = f", record {module.record}" if module.record is not None else ""
    return f"{module.name} ({tag}{rec}) rom=0x{module.rom:06X} ram=0x{module.ram:08X}"


def name_at(module: Module, addr: int | None) -> str:
    if addr is None:
        return "?"
    return module.names.get(addr, f"0x{addr:08X}")


def body_desc(module: Module, addr: int) -> str:
    """Where `addr` lands in this module's layout, in words."""
    if not module.owns_code(addr):
        return "past the module's DMA size (its BSS/data)"
    body = body_containing(module, addr)
    if body is None:
        return "below every registered entry"
    return f"inside {name_at(module, body)} (0x{body:08X}, +0x{addr - body:X})"


def scenes_for_record(n: int) -> list[int]:
    return sorted(d for d, mask in SCENE_DESCRIPTOR_MASKS.items() if (mask >> n) & 1)


_CO_RESIDENT_CACHE: dict[tuple[int | None, int | None], bool] = {}


def co_resident(a: int | None, b: int | None) -> bool:
    """Can a module of record `a` be resident while one of record `b` runs?

    Derived from the scene descriptors' record masks, the same model as
    `cross_bank.py check`: if no scene loads both records, the two banks never
    coexist, so a target missing from one cannot be hit from the other. An
    unknown record (a base section that is always resident) fails safe as True.
    """
    key = (a, b)
    if key in _CO_RESIDENT_CACHE:
        return _CO_RESIDENT_CACHE[key]
    if a is None or b is None or a == b:
        ok = True
    else:
        ok = bool(set(scenes_for_record(a)) & set(scenes_for_record(b)))
    _CO_RESIDENT_CACHE[key] = ok
    return ok


def caller_module(site: Site, modules: list[Module]) -> Module | None:
    if site.caller_addr is None:
        return None
    for m in modules:
        if m.unit == site.unit and m.owns_code(site.caller_addr):
            return m
    return None


def same_window_gap(site: Site, addr: int, modules: list[Module]) -> Module | None:
    """The caller's own resident bank, when a call can stub inside its code.

    A `LOOKUP_FUNC` is consulted even for a call into the *caller's own* RAM
    window (the recompiler cannot bind a bank's `jal` to a body that only exists
    in another bank's layout). The module resident at an address inside the
    caller's own DMA'd code is the caller's module -- the caller is executing
    from that RAM -- so if that module does not register the target, the call
    stubs **whatever the game's scene data does**. No scene modelling is needed
    for this class, which is why it is the definite one.

    Only the code half counts. Above a record's DMA size sits its BSS, which is
    exactly where the game loads the *other* banks of the arena (record 14's
    chapter modules at 0x8022ACB0/0x802395E0), so an address there is resolved
    by whichever bank was loaded -- a candidate, not a definite.
    """
    cm = caller_module(site, modules)
    if cm is None or not cm.owns_code(addr) or addr in cm.entries:
        return None
    return cm


def looks_like_entry(rom: "Rom", module: Module, addr: int) -> bool | None:
    """Do the missing module's *own* ROM bytes at `addr` open like a function?

    This is the sharp end of the candidate triage. An address the module never
    registered is either a body interior in its layout (the caller dispatched
    another bank's address -- harmless, the resident bank simply is not this one)
    or a genuine function start the split missed, which happens for a body
    reachable only through a data pointer / callback table -- and that one *is*
    a stub the moment the bank is resident. The ROM distinguishes them.

    Returns None when the address is past the module's DMA size (no bytes).
    """
    words = rom.words_at(module, addr, 1)
    if words is None:
        return None
    w = int.from_bytes(words, "big")
    op = w >> 26
    rs, rt = (w >> 21) & 0x1F, (w >> 16) & 0x1F
    if op == 9 and rs == 29 and rt == 29 and (w & 0x8000):  # addiu sp,sp,-N
        return True
    # The prologue set `cross_bank.entry_looks_real` uses for a body that opens
    # with a dispatch or a save instead of a frame.
    return op in {2, 3, 8, 15, 43, 63}  # j, jal, jr, lui, sw, sd


def unregistered(verdict: Verdict) -> bool:
    """No module registers the address at all -- `get_function` can never resolve it.

    `func_map` is filled only from registration tables: the main ELF's section
    table (base + streamedA/B/C), the patch table (unused in this app) and the
    bank records. So if no module in the model lists the address, no run state
    can make the lookup succeed: every dispatch to it stubs (above
    `STREAMED_LO`) or hard-exits in `get_function`. This is the one completely
    static certainty, and it is the session-82 shape -- a bank the port has no
    record for is resident, and the call lands in the code of a module whose
    layout never had a function there.
    """
    return not verdict.registering and STREAMED_LO <= verdict.addr < IMEM_LO


def target_assessment(addr: int, sites: list[Site], modules: list[Module], verdict: Verdict):
    """(same_window, live, inert, bss_of_caller): definite, then candidates."""
    same: dict[tuple[str, str], Module] = {}
    bss_of_caller: dict[tuple[str, str], Module] = {}
    live, inert = [], []
    for s in sites:
        m = same_window_gap(s, addr, modules)
        if m is not None:
            same[(m.unit, m.name)] = m
            continue
        cm = caller_module(s, modules)
        if cm is not None and cm.code_hi <= addr < cm.hi:
            bss_of_caller[(cm.unit, cm.name)] = cm
    for m in verdict.missing:
        risk = False
        for s in sites:
            cm = caller_module(s, modules)
            if co_resident(cm.record if cm else None, m.record):
                risk = True
                break
        (live if risk else inert).append(m)
    return list(same.values()), live, inert, list(bss_of_caller.values())


# ---------------------------------------------------------------------------
# ROM access (log-mode resident inference, data-pointer scan)
# ---------------------------------------------------------------------------


class Rom:
    def __init__(self) -> None:
        if not ROM_PATH.exists():
            raise ToolError(f"{ROM_PATH.relative_to(ROOT)} missing")
        self.data = ROM_PATH.read_bytes()

    def words_at(self, module: Module, addr: int, count: int = 4) -> bytes | None:
        off = module.rom_of(addr)
        if off is None or off + 4 * count > len(self.data):
            return None
        return self.data[off:off + 4 * count]

    def find_word(self, addr: int, limit: int = 32) -> list[int]:
        """ROM offsets holding `addr` as a big-endian word."""
        pat = addr.to_bytes(4, "big")
        out: list[int] = []
        start = 0
        while len(out) < limit:
            i = self.data.find(pat, start)
            if i < 0:
                break
            out.append(i)
            start = i + 1
        return out

    def find_bytes(self, needle: bytes, limit: int = 8) -> list[int]:
        out: list[int] = []
        start = 0
        while len(out) < limit:
            i = self.data.find(needle, start)
            if i < 0:
                break
            out.append(i)
            start = i + 1
        return out


def rom_owner(modules: list[Module], off: int) -> str:
    for m in modules:
        if m.rom and m.rom <= off < m.rom + m.size:
            return f"inside {m.name} (unit {m.unit}) +0x{off - m.rom:X}"
    for rec, (rom, ram, size, _c) in enumerate(streamed_record_table()):
        if rom <= off < rom + size:
            return f"record {rec}'s ROM span (rom 0x{rom:06X}, ram 0x{ram:08X})"
    return "outside every compiled module's ROM range"


def resident_candidates(rom: Rom, addr: int, words: bytes, modules: list[Module]):
    """Which module's ROM image has exactly these bytes at this address?

    `words` is the `[overlays] @0xADDR: ...` line the runtime prints on a stub
    hit -- live RDRAM. A module whose own bytes at the same address match was the
    resident one, whether or not it registered the address; that distinction is
    the whole diagnosis (resident-but-unregistered is a load-path bug, a
    registered module's absence a wrong-layout dispatch).
    """
    return [m for m in modules if rom.words_at(m, addr, len(words) // 4) == words]


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------


def grouped_sites(sites: list[Site]) -> dict[int, list[Site]]:
    out: dict[int, list[Site]] = {}
    for s in sites:
        if s.kind == "lookup" and s.target is not None:
            out.setdefault(s.target, []).append(s)
    return out


def report(args) -> int:
    quiet = args.json

    def say(*a, **k):
        if not quiet:
            print(*a, **k)

    modules = load_modules()
    rom = Rom()
    sites = collect_sites()
    by_target = grouped_sites(sites)
    indirect = [s for s in sites if s.kind == "indirect"]
    direct = [s for s in sites if s.kind == "direct"]

    verdicts = {a: classify(a, modules) for a in by_target}
    assess = {a: target_assessment(a, by_target[a], modules, verdicts[a])
              for a in by_target}

    def prologue_gaps(a: int) -> list[Module]:
        """Missing banks that have a real function prologue at `a` in their ROM."""
        out = []
        for m in assess[a][1] + assess[a][0]:
            if looks_like_entry(rom, m, a):
                out.append(m)
        return out

    def bucket(pred):
        return sorted((a for a, v in verdicts.items() if pred(a, v)),
                      key=lambda a: (-len(by_target[a]), a))

    definite = bucket(lambda a, v: assess[a][0] or unregistered(v))
    rest = {a: v for a, v in verdicts.items() if a not in set(definite)}
    strong = sorted((a for a in rest if prologue_gaps(a)),
                    key=lambda a: (-len(by_target[a]), a))
    strong_set = set(strong)
    unowned = sorted((a for a, v in rest.items()
                      if v.status == "unowned" and a not in strong_set),
                     key=lambda a: (-len(by_target[a]), a))
    partial = sorted((a for a, v in rest.items()
                      if v.status == "partial" and assess[a][1]
                      and a not in strong_set),
                     key=lambda a: (-len(by_target[a]), a))
    inert = sorted((a for a, v in rest.items()
                    if v.status == "partial" and not assess[a][1]),
                   key=lambda a: (-len(by_target[a]), a))
    ok = [a for a, v in rest.items() if v.status == "ok"]
    imem = bucket(lambda a, v: v.status == "imem")

    n_bank = sum(1 for m in modules if m.kind == "bank")
    n_main = sum(1 for m in modules if m.kind == "main")
    dispatched = len(sites) - len(indirect) - len(direct)
    say(f"stubmap: modules: {n_main} main section(s), {n_bank} bank record(s)")
    say(f"stubmap: static dispatch target(s): {len(by_target)} "
        f"({len(definite)} definite, {len(strong)} missing-function candidate(s), "
        f"{len(unowned)} unresolvable, {len(partial)} bank-selection candidate(s), "
        f"{len(inert)} inert, {len(ok)} resolvable, {len(imem)} IMEM)")
    say(f"stubmap: call site(s): {len(sites)} (statically dispatched {dispatched}, "
        f"bound direct {len(direct)}, indirect jalr {len(indirect)})")

    def site_line(s: Site) -> str:
        extra = ""
        if s.jal is not None:
            extra = (f" (jal 0x{s.jal:08X})" if s.jal == s.target
                     else f" (jal 0x{s.jal:08X}: size-overridden to 0x{s.target:08X})")
        caller = (f"{s.caller_name}(0x{s.caller_addr:08X})" if s.caller_addr
                  else s.caller_name)
        return f"{caller}  {s.where}{extra}"

    def module_json(m: Module, a: int) -> dict:
        return {"module": m.name, "unit": m.unit, "record": m.record,
                "rom": f"0x{m.rom:06X}", "ram": f"0x{m.ram:08X}",
                "body": (f"0x{body_containing(m, a):08X}"
                         if body_containing(m, a) is not None else None),
                "in_bss": not m.owns_code(a), "where": body_desc(m, a)}

    def entry_json(a: int, v: Verdict) -> dict:
        same, live, inert_gaps, bss = assess[a]
        return {
            "addr": f"0x{a:08X}",
            "status": v.status,
            "sites": [
                {"caller": s.caller_name,
                 "caller_addr": f"0x{s.caller_addr:08X}" if s.caller_addr else None,
                 "where": s.where,
                 "jal": f"0x{s.jal:08X}" if s.jal is not None else None}
                for s in by_target[a]
            ],
            "same_window": [module_json(m, a) for m in same],
            "missing_live": [module_json(m, a) for m in live],
            "missing_inert": [module_json(m, a) for m in inert_gaps],
            "missing_prologue": [m.name for m in prologue_gaps(a)],
            "caller_bss": [f"{m.name} (unit {m.unit})" for m in bss],
            "present": [f"{m.name} (unit {m.unit})" for m in v.registering],
            "owners": [f"{m.name} (unit {m.unit})" for m in v.owners],
            "bss_spans": [f"{m.name} (unit {m.unit})" for m in v.bss_owners],
            "near": [describe(m) for m in modules if m.ram <= a < m.ram + 0x20000],
        }

    result = {
        "modules": {"main": n_main, "bank": n_bank},
        "targets": {
            "definite_gaps": [f"0x{a:08X}" for a in definite],
            "missing_function_candidates": [f"0x{a:08X}" for a in strong],
            "unresolvable": [f"0x{a:08X}" for a in unowned],
            "bank_selection_candidates": [f"0x{a:08X}" for a in partial],
            "inert_gaps": [f"0x{a:08X}" for a in inert],
            "resolvable": len(ok),
            "imem": [f"0x{a:08X}" for a in imem],
        },
        "sites": {"dispatched": dispatched, "direct": len(direct),
                  "indirect": len(indirect)},
        "detail": [],
    }

    def print_group(title: str, addrs: list[int], note: str):
        if not addrs:
            return
        say()
        say(f"== {title} ({len(addrs)} target(s)) ==")
        if note:
            say(f"   {note}")
        for a in addrs:
            v = verdicts[a]
            same, live, inert_gaps, bss = assess[a]
            say(f"\n0x{a:08X}  {len(by_target[a])} site(s)")
            for s in by_target[a][: args.max_sites]:
                say(f"    {site_line(s)}")
            if len(by_target[a]) > args.max_sites:
                say(f"    ... and {len(by_target[a]) - args.max_sites} more site(s)")
            for m in same:
                say(f"    STUBS IN THE CALLER'S OWN BANK: {describe(m)} -- "
                    f"{body_desc(m, a)}")
            if unregistered(v):
                say("    NO MODULE REGISTERS THIS ADDRESS: no run state can resolve "
                    "it -- the resident bank here must be one the port has no "
                    "record for")
            for m in prologue_gaps(a):
                say(f"    MISSING FUNCTION: {describe(m)} opens like a function "
                    f"prologue at this address but is not in its table -- "
                    f"{body_desc(m, a)}")
            if v.status == "unowned":
                for m in modules:
                    if m.ram <= a < m.ram + 0x20000:
                        say(f"    RAM also used by {describe(m)} (+0x{a - m.ram:X})")
                for m in v.bss_owners:
                    say(f"    inside {m.name}'s BSS span (unit {m.unit}), which "
                        f"registers nothing here")
            else:
                if v.registering:
                    say("    present in: "
                        + ", ".join(f"{m.name}({m.unit})" for m in v.registering))
                for m in live:
                    say(f"    CANDIDATE GAP in {describe(m)} -- {body_desc(m, a)}")
                for m in inert_gaps:
                    say(f"    inert gap in {describe(m)} -- {body_desc(m, a)} "
                        f"(record {m.record} is never loaded with the callers')")
            for m in bss:
                say(f"    note: the caller's own {m.name}({m.unit}) has no code this "
                    f"far up; another bank of the arena must provide it")
            result["detail"].append(entry_json(a, v))

    print_group("DEFINITE: the function map can never have this address",
                definite,
                "Either no compiled module registers it at all (so no run state "
                "resolves the lookup), or the bank the caller executes from has no "
                "entry there.")
    print_group("MISSING FUNCTION: a bank that can be resident opens with a prologue "
                "at the address but does not register it",
                strong,
                "A function the split missed (indirect-only entry), or a bank the "
                "port has not compiled at all. Runner-up to definite.")
    print_group("UNRESOLVABLE: no compiled module owns this RAM", unowned, "")
    print_group("BANK SELECTION: a bank that can be resident with the caller lacks "
                "the entry, but the bytes are a body interior there",
                partial,
                "Same-record banks are chosen by scene data, so a run confirms "
                "these; `stubmap.py log` is the arbiter.")
    print_group("RESOLVABLE", ok if args.verbose else [],
                "Registered by every module that can be resident.")
    if inert and (args.verbose or args.json):
        print_group("INERT: the missing banks cannot be resident with the caller",
                    inert, "")

    if imem:
        say()
        say(f"== RSP IMEM ({len(imem)} target(s)) ==")
        say("   Addresses in 0x84000000..0x84200000: recompiled microcode text that")
        say("   the RSP runs from IMEM, not the CPU. get_function accepts the window,")
        say("   so these log as stubs if the CPU ever executes that text.")
        for a in imem:
            say(f"   0x{a:08X}  {len(by_target[a])} site(s)")

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        say()
        if not definite and not unowned and not partial:
            print("stubmap: no definite, unresolvable or candidate static dispatch "
                  "target.")
        print("stubmap: `stubmap.py target 0xADDR` for owners, bodies and callers; "
              "`stubmap.py log <run.log>` attributes a run's hits (and the "
              f"{len(indirect)} indirect jalr site(s), which only a run can name); "
              "`--verbose` lists inert gaps.")

    if args.strict and (definite or unowned or strong):
        return 1
    return 0


# ---------------------------------------------------------------------------
# target
# ---------------------------------------------------------------------------


def target(args) -> int:
    modules = load_modules()
    rom = Rom()
    sites = collect_sites()
    for spec in args.addrs:
        a = int(spec, 16)
        v = classify(a, modules)
        sits = [s for s in sites if s.target == a and s.kind == "lookup"]
        print(f"=== 0x{a:08X}  status: {v.status} ===")
        if v.status == "imem":
            print("  RSP IMEM window: microcode text, not CPU RDRAM")
        if not v.owners:
            print("  owners: none -- no compiled module's RAM range covers this address")
            for m in modules:
                if m.ram <= a < m.ram + 0x40000:
                    print(f"    RAM also used by {describe(m)} (+0x{a - m.ram:X})")
            for rec, (r, rm, sz, _c) in enumerate(streamed_record_table()):
                if rm <= a < rm + sz:
                    print(f"    record {rec} maps this RAM (rom 0x{r:06X}, "
                          f"size 0x{sz:X})")
        for m in v.owners:
            have = "entry OK" if a in m.entries else "MISSING"
            prov = m.rom_of(a)
            print(f"  owner {describe(m)}  [{have}]  {body_desc(m, a)}  "
                  f"{f'rom 0x{prov:06X}' if prov is not None else 'rom -'}")
            if m.record is not None:
                sc = scenes_for_record(m.record)
                print(f"      record {m.record} loaded by scene(s): "
                      + (", ".join(f"0x{d:08X}" for d in sc) if sc else "none"))
        same, live, inert, bss = target_assessment(a, sits, modules, v)
        for m in same:
            print(f"  DEFINITE: {describe(m)} is the caller's own resident bank and "
                  f"has no entry here -- {body_desc(m, a)}")
        for m in live:
            strong = " (opens with a PROLOGUE here: a function the split missed)" \
                if looks_like_entry(rom, m, a) else ""
            print(f"  CANDIDATE GAP: {describe(m)} -- a caller's record can be loaded "
                  f"with record {m.record}{strong}")
        for m in inert:
            print(f"  inert gap: {describe(m)} -- no scene loads record {m.record} "
                  f"with the caller's record")
        for m in bss:
            print(f"  note: the caller's own {m.name}({m.unit}) has no code this far "
                  f"up; another bank of the arena must provide it")
        if sits:
            print(f"  dispatched from {len(sits)} site(s):")
            for s in sits[: args.max_sites]:
                cm = caller_module(s, modules)
                rec = f"record {cm.record}" if cm and cm.record is not None else "base"
                extra = f" (jal 0x{s.jal:08X})" if s.jal and s.jal != a else ""
                print(f"    {s.caller_name}(0x{s.caller_addr:08X}) [{rec}]  "
                      f"{s.where}{extra}")
            if len(sits) > args.max_sites:
                print(f"    ... and {len(sits) - args.max_sites} more")
        direct = [s for s in sites if s.target == a and s.kind == "direct"]
        if direct:
            print(f"  also bound as a direct call at {len(direct)} site(s) "
                  f"(wrong-layout binding, not a stub):")
            for s in direct[:5]:
                print(f"    {s.caller_name}(0x{s.caller_addr:08X})  {s.where}")
        ptrs = rom.find_word(a)
        if ptrs:
            print(f"  data pointer word(s) in the ROM: {len(ptrs)}"
                  + (" (showing 8)" if len(ptrs) > 8 else ""))
            for off in ptrs[:8]:
                print(f"    rom 0x{off:06X}  {rom_owner(modules, off)}")
        if not sits and not direct and not ptrs:
            print("  no static caller and no data word: reached by jalr/computed "
                  "address only (`stubmap.py log`, `stubmap.py pointers`).")
        print()
    return 0


# ---------------------------------------------------------------------------
# log
# ---------------------------------------------------------------------------


def log_command(args) -> int:
    modules = load_modules()
    rom = Rom()
    path = Path(args.path)
    if not path.exists():
        raise ToolError(f"{path} not found")

    words: dict[int, bytes] = {}
    hits: dict[int, int] = {}
    chains: dict[int, list[str]] = {}
    unknown: set[tuple[int, int]] = set()
    uncompiled: set[tuple[int, int, int, int]] = set()
    loaded: set[tuple[int, int]] = set()
    last_stub: int | None = None

    with path.open(errors="replace") as f:
        for line in f:
            if "[overlays]" in line:
                m = LOG_WORDS_RE.match(line)
                if m:
                    try:
                        words[int(m.group(1), 16)] = bytes.fromhex(
                            m.group(2).replace(" ", ""))
                    except ValueError:
                        pass
                    continue
                m = LOG_STUB_RE.match(line)
                if m:
                    a = int(m.group(1), 16)
                    hits[a] = hits.get(a, 0) + 1
                    last_stub = a
                    continue
                continue
            if "[snap]" in line:
                m = LOG_CHAIN_RE.match(line)
                if m and last_stub is not None:
                    frames = m.group(4).strip()
                    # A long run interleaves other threads' output into this
                    # line; the chain is whatever precedes the next marker.
                    frames = frames.split("[", 1)[0].strip()
                    if frames and frames != "(no live frames)":
                        chains.setdefault(last_stub, []).append(
                            f"t{m.group(1)}: {frames}")
                continue
            if "[bank]" not in line:
                continue
            m = LOG_UNKNOWN_RE.match(line)
            if m:
                unknown.add((int(m.group(1), 16), int(m.group(2), 16)))
                continue
            m = LOG_UNCOMPILED_RE.match(line)
            if m:
                uncompiled.add((int(m.group(1)), int(m.group(2), 16),
                                int(m.group(3), 16), int(m.group(4), 16)))
                continue
            m = LOG_LOADED_RE.match(line)
            if m:
                loaded.add((int(m.group(1), 16), int(m.group(2), 16)))

    print(f"stubmap: {path} -- {sum(hits.values())} stub hit(s), "
          f"{len(hits)} distinct address(es); the port registered "
          f"{len(loaded)} record(s) in this run")

    if unknown:
        print(f"\nstubmap: {len(unknown)} UNKNOWN module DMA(s) -- ROM loaded into RAM "
              f"no unit compiles (the missing-bank class):")
        for r, ram in sorted(unknown):
            others = [m for m in modules if m.ram == ram]
            print(f"    rom 0x{r:06X} -> ram 0x{ram:08X}   "
                  + (f"other banks of this RAM: "
                     + ", ".join(f"{m.name}({m.unit})" for m in others)
                     if others else "no compiled bank at this base"))
            rec = record_of_rom(r)
            if rec is not None:
                print(f"      rom 0x{r:06X} is inside streamed record {rec}")
    if uncompiled:
        print(f"\nstubmap: {len(uncompiled)} UNCOMPILED streamed record(s) "
              f"(kAllStreamedRecords marks them compiled=false):")
        for rec, r, ram, size in sorted(uncompiled):
            print(f"    record {rec}: rom 0x{r:06X} ram 0x{ram:08X} size 0x{size:X}")

    if hits:
        print()
    sites = collect_sites()
    by_target = grouped_sites(sites)

    for a in sorted(hits, key=lambda x: (-hits[x], x)):
        v = classify(a, modules)
        print(f"0x{a:08X}  {hits[a]} hit(s)  status: {v.status}")
        if a in words:
            live = words[a]
            print(f"    live words: {live.hex().upper()}")
            matches = resident_candidates(rom, a, live, modules)
            for m in matches:
                reg = ("registers this address" if a in m.entries
                       else "does NOT register this address")
                print(f"    RESIDENT: {describe(m)} -- bytes match its ROM image, "
                      f"it {reg}")
                if (m.rom, m.ram) in loaded:
                    print(f"      the run did register it "
                          f"(`loading overlay record rom=0x{m.rom:06X} "
                          f"ram=0x{m.ram:08X}`); a later load must have evicted "
                          f"its entries, or the lookup happened before that write")
                elif a in m.entries:
                    print("      the run has NO `loading overlay record` line for "
                          "it: the DMA never matched kBankRecords, so its load "
                          "bypassed on_streamed_dma")
                else:
                    print(f"      in this layout 0x{a:08X} is not a function: "
                          f"{body_desc(m, a)}")
            if not matches:
                offs = rom.find_bytes(live)
                print("    RESIDENT: no compiled module's ROM image has these bytes "
                      "here")
                if v.owners:
                    print("      owners were: " + "; ".join(describe(m) for m in v.owners))
                if offs:
                    for off in offs[:4]:
                        print(f"      live bytes occur at rom 0x{off:06X} -- "
                              f"{rom_owner(modules, off)}")
        for s in by_target.get(a, [])[: args.max_sites]:
            print(f"    dispatched from {s.caller_name}(0x{s.caller_addr:08X})  "
                  f"{s.where}")
        if a not in by_target:
            print("    no static dispatch site: reached through a jalr/computed "
                  "address (`stubmap.py pointers` lists data words)")
        for c in chains.get(a, [])[:2]:
            print(f"    callchain {c}")
        print()

    print("stubmap: a stub whose bytes match a module that DOES register the address "
          "is a load-path bug; one whose bytes match a module that does not is a "
          "wrong-layout dispatch (the caller used another bank's address).")
    return 0


def record_of_rom(off: int) -> int | None:
    for rec, (rom, _ram, size, _c) in enumerate(streamed_record_table()):
        if rom <= off < rom + size:
            return rec
    return None


# ---------------------------------------------------------------------------
# pointers
# ---------------------------------------------------------------------------


def pointers(args) -> int:
    """Data words in the ROM that point at a streamed function entry.

    `jalr` targets come from a register, so no `jal` comment names them. The
    pointers live in data tables (callbacks, vtables, dispatch tables), so this
    scans the ROM for a 4-byte aligned word that is a *registered entry* in at
    least one module -- that filter is what makes it a pointer list instead of
    every stray byte pair that looks like an address -- and reports the ones the
    banks that can be resident do not all register.
    """
    modules = load_modules()
    rom = Rom()
    entries: set[int] = set()
    for m in modules:
        entries |= m.entries
    by_target = grouped_sites(collect_sites())

    pat = re.compile(rb"\x80[\x0e-\x3f][\x00-\xff][\x00-\xff]")
    found: dict[int, list[int]] = {}
    for m in pat.finditer(rom.data):
        off = m.start()
        if off % 4:
            continue
        a = int.from_bytes(m.group(0), "big")
        if STREAMED_LO <= a < IMEM_LO and a in entries:
            found.setdefault(a, []).append(off)

    risky = {a: offs for a, offs in found.items() if classify(a, modules).status != "ok"}

    print(f"stubmap: {len(found)} aligned word(s) in the ROM point at a registered "
          f"entry; {len(risky)} of them are not registered by every candidate bank")
    for a in sorted(risky, key=lambda x: (-len(risky[x]), x)):
        v = classify(a, modules)
        note = ("no module owns it" if v.status == "unowned"
                else "missing in " + ", ".join(f"{m.name}({m.unit})" for m in v.missing))
        stat = f"; {len(by_target[a])} static dispatch site(s)" if a in by_target else ""
        print(f"\n0x{a:08X}  {len(risky[a])} pointer word(s)  [{note}]{stat}")
        for off in risky[a][: args.limit]:
            print(f"    rom 0x{off:06X}  {rom_owner(modules, off)}")
        if len(risky[a]) > args.limit:
            print(f"    ... and {len(risky[a]) - args.limit} more")
    return 0


# ---------------------------------------------------------------------------
# cli
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="stubmap.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="cmd")

    p = sub.add_parser("report", help="rank every statically dispatched target")
    p.add_argument("--strict", action="store_true",
                   help="exit 1 on a definite gap, an unresolvable target, or a "
                        "missing-function candidate")
    p.add_argument("--json", action="store_true", help="print JSON only")
    p.add_argument("--verbose", action="store_true",
                   help="also list inert gaps and fully resolved targets")
    p.add_argument("--max-sites", type=int, default=6,
                   help="call sites to print per target (default 6)")
    p.set_defaults(func=report)

    p = sub.add_parser("target", help="deep dive on one or more addresses")
    p.add_argument("addrs", nargs="+", help="guest addresses, e.g. 0x80219594")
    p.add_argument("--max-sites", type=int, default=12)
    p.set_defaults(func=target)

    p = sub.add_parser("log", help="attribute the stub hits in a run log")
    p.add_argument("path", help="run log (stdout of a build-app run)")
    p.add_argument("--max-sites", type=int, default=4)
    p.set_defaults(func=log_command)

    p = sub.add_parser("pointers",
                       help="data words in streamed RAM pointing at risky targets")
    p.add_argument("--limit", type=int, default=8, help="locations per target")
    p.set_defaults(func=pointers)

    args = parser.parse_args()
    if args.cmd is None:
        parser.print_help()
        return 2
    try:
        return args.func(args)
    except ToolError as exc:
        print(f"stubmap: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
