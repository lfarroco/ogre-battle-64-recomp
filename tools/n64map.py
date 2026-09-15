#!/usr/bin/env python3
"""Offline guest <-> ROM address map for the port (no game run needed).

Every session re-derives `vram = rom - 0x1060 + 0x80070C60`, then asks "which
record owns this address / is this a function entry?". This module answers that
from the repo's own build inputs:

* `config.yaml`                 -> the main + boot-resident/streamed overlays
* `config-bank*.yaml`           -> the streamed records (per unit)
* `app/src/bank_funcs.inc`      -> the runtime's record table, with unit +
                                   record names (this is the authoritative
                                   "who owns this RAM" for the bank units)
* `app/src/bank_overlays.cpp`   -> the main-ELF overlays (`kMainOverlayModules`)
* `build/*.elf` (via mips-nm)   -> symbols; falls back to the `recomp_trace_entry`
                                   lines in `RecompiledFuncs/`/`Bank*Funcs/`

Used by `tools/guestmap.py` and `tools/rdram.py` (the `image`/`diff` subcommands
annotate addresses with the owning record).
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = ROOT / "assets" / "ogre64.z64"
RDRAM_BASE = 0x80000000
RDRAM_END = 0x80800000


def parse_addr(text: str) -> int:
    """`0x...`, bare hex, or decimal. Low values stay low (ROM offsets)."""
    text = text.strip()
    value = int(text, 0) if text.lower().startswith("0x") else int(text, 16)
    return value


@dataclass
class Segment:
    """A mapped ROM range, optionally with a VRAM address."""
    name: str
    rom: int
    size: int
    vram: int | None
    kind: str                      # "main" | "overlay" | "record"
    unit: str = ""                 # bank units: A..G
    source: str = ""               # where the entry came from

    @property
    def rom_end(self) -> int:
        return self.rom + self.size

    @property
    def vram_end(self) -> int:
        return (self.vram or 0) + self.size

    def label(self) -> str:
        if self.unit and self.kind == "record":
            return "unit %s %s" % (self.unit, self.name)
        return "%s (%s)" % (self.name, self.kind)


@dataclass
class Symbol:
    addr: int
    name: str
    kind: str                      # nm type letter
    source: str

    @property
    def is_code(self) -> bool:
        return self.kind in "TtWw"

    @property
    def role(self) -> str:
        """`code` or `data`, from the nm type and the project's name prefixes.

        splat puts the main segment's data in a code section, so the type letter
        alone calls `D_8009ED80` (the NJPEG microcode) "text"; the `D_`/`jtbl_`
        prefixes are the reliable signal.
        """
        if self.name.startswith(("D_", "jtbl_")) or not self.is_code:
            return "data"
        return "code"

    @property
    def is_section_marker(self) -> bool:
        """splat emits `foo_TEXT_START`/`foo_VRAM` per segment; not real symbols."""
        return bool(re.search(r"_(TEXT|DATA|RODATA|BSS|VRAM|ROM)(_START|_END)?$", self.name))


@dataclass
class Map:
    segments: list[Segment] = field(default_factory=list)
    _symbols: list[Symbol] | None = None

    # -- construction --------------------------------------------------------
    def add(self, seg: Segment) -> None:
        self.segments.append(seg)

    # -- lookups -------------------------------------------------------------
    def by_vram(self, addr: int) -> Segment | None:
        """The most specific segment covering a VRAM address."""
        best: Segment | None = None
        for seg in self.segments:
            if seg.vram is None:
                continue
            if seg.vram <= addr < seg.vram_end:
                if best is None or seg.size < best.size:
                    best = seg
        return best

    def by_rom(self, off: int) -> Segment | None:
        """The most specific segment covering a ROM offset."""
        best: Segment | None = None
        for seg in self.segments:
            if seg.rom <= off < seg.rom_end:
                if best is None or seg.size < best.size:
                    best = seg
        return best

    def vram_to_rom(self, addr: int) -> int | None:
        seg = self.by_vram(addr)
        if seg is None or seg.vram is None:
            return None
        return seg.rom + (addr - seg.vram)

    def rom_to_vram(self, off: int) -> int | None:
        seg = self.by_rom(off)
        if seg is None or seg.vram is None:
            return None
        return seg.vram + (off - seg.rom)

    def in_rdram(self, addr: int) -> bool:
        return RDRAM_BASE <= addr < RDRAM_END

    # -- symbols -------------------------------------------------------------
    def symbols(self) -> list[Symbol]:
        if self._symbols is None:
            self._symbols = load_symbols()
        return self._symbols

    def symbol_at(self, addr: int, elf: str | None = None) -> Symbol | None:
        best: Symbol | None = None
        for sym in self.symbols():
            if sym.addr != addr or "NON_MATCHING" in sym.name:
                continue
            if elf is not None and sym.source != elf:
                continue
            if best is None or (sym.kind in "TtWw" and best.kind not in "TtWw"):
                best = sym
        return best

    def nearest_symbol(self, addr: int, elf: str | None = None) -> tuple[Symbol, int] | None:
        best: tuple[Symbol, int] | None = None
        for sym in self.symbols():
            if sym.addr > addr:
                continue
            if elf is not None and sym.source != elf:
                continue
            delta = addr - sym.addr
            if best is None or delta < best[1]:
                best = (sym, delta)
        return best

    def symbols_near(self, addr: int, window: int, elf: str | None = None) -> list[Symbol]:
        out = [s for s in self.symbols()
               if abs(s.addr - addr) <= window and (elf is None or s.source == elf)]
        out.sort(key=lambda s: s.addr)
        return out


def expected_elf(seg: "Segment | None") -> str | None:
    """The ELF whose layout owns this segment (bank units overlap the main ELF)."""
    if seg is None:
        return None
    if seg.kind == "record" and seg.unit:
        return "bank%s.elf" % seg.unit
    if seg.kind in ("main", "overlay"):
        return "ogrebattle64.elf"
    return None


# --- parsers ----------------------------------------------------------------

_NAME_RE = re.compile(r"^\s*-?\s*name:\s*(\S+)")
_START_RE = re.compile(r"^\s*start:\s*(0x[0-9A-Fa-f]+|\d+)")
_VRAM_RE = re.compile(r"^\s*vram:\s*(0x[0-9A-Fa-f]+|\d+)")
_TYPE_RE = re.compile(r"^\s*type:\s*(\S+)")

# { rom, ram, size, ram_end, kA_bankRec2Functions, ARRLEN(...) },
_BANK_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)u,\s*\(int32_t\)0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,"
    r"\s*\(int32_t\)0x([0-9A-Fa-f]+)u,\s*(\w+),"
)
# { 0x1CE040u, 0x229C0u, (int32_t)0x80197B90 },   // streamedC
_MAIN_OVERLAY_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*\(int32_t\)0x([0-9A-Fa-f]+)u\s*\}"
)


def _read(path: Path) -> str:
    try:
        return path.read_text()
    except OSError:
        return ""


def _parse_splat_segments(text: str, kind: str, source: str) -> list[Segment]:
    """Named segments from a splat config (name/type/start/vram per entry).

    Only the top-level list under `segments:` is parsed: entries there sit at
    one indentation level, while `subsegments:` items sit deeper. Unnamed
    entries (`- type: bin`, `- [0x2800000]`) are kept as *boundaries* so a
    segment's size is the next entry's start — otherwise a named segment would
    swallow the unassigned ROM gap that follows it.
    """
    lines = text.splitlines()
    start_idx = None
    for i, line in enumerate(lines):
        if line.strip() == "segments:" and not line.startswith((" ", "\t")):
            start_idx = i
            break
    if start_idx is None:
        return []

    entries: list[dict] = []
    cur: dict | None = None
    dash_indent: int | None = None
    for line in lines[start_idx + 1:]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            break
        m = re.match(r"^(\s*)-\s*(.*)$", line)
        if m and (dash_indent is None or len(m.group(1)) == dash_indent):
            dash_indent = len(m.group(1))
            if cur is not None:
                entries.append(cur)
            cur = {"name": None, "type": None, "start": None, "vram": None}
            rest = m.group(2).strip()
            for key, rx in (("name", r"name:\s*(\S+)"), ("type", r"type:\s*(\S+)")):
                mm = re.match(rx, rest)
                if mm:
                    cur[key] = mm.group(1)
                    break
            mm = re.match(r"\[(0x[0-9A-Fa-f]+)\]", rest)
            if mm:
                cur["start"] = int(mm.group(1), 16)
            continue
        if cur is None:
            continue
        if cur["start"] is None:
            mm = _START_RE.match(line)
            if mm:
                cur["start"] = int(mm.group(1), 0)
                continue
        if cur["vram"] is None:
            mm = _VRAM_RE.match(line)
            if mm:
                cur["vram"] = int(mm.group(1), 0)
                continue
        if cur["type"] is None:
            mm = _TYPE_RE.match(line)
            if mm:
                cur["type"] = mm.group(1)
    if cur is not None:
        entries.append(cur)

    positioned = [e for e in entries if e["start"] is not None]
    out: list[Segment] = []
    for i, entry in enumerate(positioned):
        if entry["name"] is None or i + 1 >= len(positioned):
            continue
        size = positioned[i + 1]["start"] - entry["start"]
        if size <= 0:
            continue
        out.append(Segment(name=entry["name"], rom=entry["start"], size=size,
                           vram=entry["vram"], kind=kind, source=source))
    return out


def _parse_bank_records(text: str) -> list[Segment]:
    out: list[Segment] = []
    for m in _BANK_RE.finditer(text):
        rom, ram, size, ram_end, table = (m.group(i) for i in range(1, 6))
        unit = ""
        um = re.match(r"k([A-G])_", table)
        if um:
            unit = um.group(1)
        rec = re.sub(r"^k[A-G]_", "", table)
        rec = re.sub(r"Functions$", "", rec)
        out.append(Segment(name=rec, rom=int(rom, 16), size=int(size, 16),
                           vram=int(ram, 16), kind="record", unit=unit,
                           source="app/src/bank_funcs.inc"))
    return out


def _parse_main_overlays(text: str) -> list[Segment]:
    out: list[Segment] = []
    for m in _MAIN_OVERLAY_RE.finditer(text):
        rom, size, ram = (int(m.group(i), 16) for i in range(1, 4))
        out.append(Segment(name="overlay@0x%06X" % rom, rom=rom, size=size, vram=ram,
                           kind="overlay", source="app/src/bank_overlays.cpp"))
    return out


def load_map() -> Map:
    m = Map()
    for path, kind in ((ROOT / "config.yaml", "main"),
                       *((ROOT / ("config-bank%s.yaml" % u), "record")
                         for u in "ABCDEFG")):
        if not path.exists():
            continue
        for seg in _parse_splat_segments(_read(path), kind, path.name):
            if seg.name in ("header",):
                continue
            m.add(seg)

    # The runtime's record table is authoritative for the bank units (it is what
    # bank_overlays.cpp matches DMAs against), so it supplies the record names,
    # units, sizes and ram_end.
    for seg in _parse_bank_records(_read(ROOT / "app" / "src" / "bank_funcs.inc")):
        m.add(seg)

    # The main-ELF overlays (A/B/C) are sized by `kMainOverlayModules`, which is
    # what the runtime recognises loads with; a splat config's "next segment"
    # boundary can disagree by the unassigned gap that follows.
    overlays = _parse_main_overlays(_read(ROOT / "app" / "src" / "bank_overlays.cpp"))
    by_rom = {seg.rom: seg for seg in overlays}
    for seg in m.segments:
        if seg.rom in by_rom and seg.kind == "main":
            seg.size = by_rom[seg.rom].size
            seg.source = "%s + %s" % (seg.source, by_rom[seg.rom].source)
            del by_rom[seg.rom]
    m.segments.extend(by_rom.values())

    kept: dict[int, Segment] = {}
    for seg in m.segments:
        if seg.kind == "record" and seg.source.startswith("config-bank"):
            # Drop the splat copy when the runtime table has the same record.
            if any(s.rom == seg.rom and s.kind == "record"
                   and s.source == "app/src/bank_funcs.inc" for s in m.segments):
                continue
        kept.setdefault(seg.rom, seg)
    m.segments = list(kept.values())
    return m


# --- symbols ----------------------------------------------------------------

_TRACE_RE = re.compile(r'recomp_trace_entry\(rdram,\s*(0x[0-9A-Fa-f]+),\s*"([^"]+)"')


def load_symbols() -> list[Symbol]:
    """Function/data symbols, ELF first, generated C as the fallback."""
    syms: list[Symbol] = []
    nm = _find_nm()
    if nm:
        for elf in sorted((ROOT / "build").glob("*.elf")):
            try:
                out = subprocess.run([nm, "-n", str(elf)], capture_output=True,
                                     text=True, timeout=60).stdout
            except (OSError, subprocess.SubprocessError):
                continue
            for line in out.splitlines():
                parts = line.split()
                if len(parts) < 3:
                    continue
                try:
                    addr = int(parts[0], 16)
                except ValueError:
                    continue
                syms.append(Symbol(addr=addr, name=parts[2], kind=parts[1], source=elf.name))
    if not syms:
        for pattern in ("RecompiledFuncs/*.c", "Bank*Funcs/*.c"):
            for src in sorted(ROOT.glob(pattern)):
                for m in _TRACE_RE.finditer(_read(src)):
                    syms.append(Symbol(addr=int(m.group(1), 16), name=m.group(2),
                                       kind="T", source=src.name))
    syms.sort(key=lambda s: s.addr)
    return syms


def _find_nm() -> str | None:
    for candidate in ("mips-linux-gnu-nm", "nm"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, timeout=20)
            return candidate
        except (OSError, subprocess.SubprocessError):
            continue
    return None
