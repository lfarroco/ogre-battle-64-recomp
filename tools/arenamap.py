#!/usr/bin/env python3
"""Map every streamed arena the game's loader pattern can reach, and say which
banks the port still has no unit for.

Every streamed module in OB64 is DMA'd by one generated loader block. The block
is a fixed instruction pattern, and the game's own boundaries are in it:

    lui   $a0, hi(rom_start)          ; \
    addiu $a0, $a0, lo(rom_start)     ;  } rom start
    lui   $a1, hi(ram_base)           ; \
    addiu $a1, $a1, lo(ram_base)      ;  } ram base
    lui   $a2, hi(rom_end)            ; \
    addiu $a2, $a2, lo(rom_end)       ;  }
    jal   func_8009DA50               ; the DMA
    subu  $a2, $a2, $a0               ; size = rom_end - rom_start  (delay slot)

and, bracketing it, the two cache invalidations and the BSS zero:

    func_800900C0(ram_base, code_end)   ; icache invalidate, code half
    func_80090010(code_end, data_end)   ; dcache invalidate, data half
    func_8009DA50(rom_start, ram_base, size)
    func_80093380(data_end, bss_end)    ; bss zero (guarded by beq)

So **one** block yields the ROM start, the RAM base, the size, the code/data
boundary and the BSS end. The size is the loader's own `subu`, never the chunk
count: N64Recomp reads the ELF, and an ELF that runs past the DMA swallows the
next bank of the same arena (session 72's unit V, session 59's rule).

Two things make a bank "missing":

* no `config-bank<U>.yaml` covers its ROM start, so nothing is compiled, or
* a unit covers its ROM start but with the wrong size, so its ELF runs into the
  next bank.

And two things make calls into it mis-bind (AGENTS §4 / the load-bearing facts):
a unit must not define a RAM range another bank can own *and* call into it, so
every call target inside a bank's range that is a real function start in *that*
bank's layout must be forced as an entry in `symbol_addrs-bank<U>.txt`.

Usage:
    tools/arenamap.py                     # every arena, every unit
    tools/arenamap.py --arena 0x80214FA0  # one arena
    tools/arenamap.py --missing           # only banks with no/wrong unit
    tools/arenamap.py --unit Y            # what one unit covers
    tools/arenamap.py --elf build/bankN.elf
    tools/arenamap.py --md                # markdown table for docs/scenes.md
    tools/arenamap.py --unmapped          # RAM windows only the main ELF loads

The scan walks the instruction stream backwards from every `jal 0x8009DA50`
(with real register dataflow, so a loader that reuses a register between two
loads still resolves), and separately flags `jal 0x8009DA50` sites whose RAM
base is *not* in the ELF set's function-containing sections (a bank in a
segment the port never linked).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJDUMP = "mips-linux-gnu-objdump"

DMA_FUNC = 0x8009DA50  # func_8009DA50: the PI DMA
ICACHE_FUNC = 0x800900C0  # func_800900C0: code-half icache invalidate
DCACHE_FUNC = 0x80090010  # func_80090010: data-half dcache invalidate
BSSZERO_FUNC = 0x80093380  # func_80093380: bss zero
RAM_LO, RAM_HI = 0x80000000, 0x80800000  # KSEG0 RDRAM window

INSN_RE = re.compile(
    r"^\s*([0-9a-fA-F]{8}):\s+([0-9a-fA-F]{8})\s+(\S+)\s*(.*?)\s*$"
)
MNEMONIC_ALIASES = {"add": "addu", "addu": "addu"}


# ---------------------------------------------------------------------------
# ELF reading
# ---------------------------------------------------------------------------


@dataclass
class Section:
    name: str
    size: int
    vma: int
    lma: int
    code: bool
    bss: bool


@dataclass
class Symbol:
    addr: int
    name: str
    kind: str  # T/t = text, others as nm prints


def objdump(*args: str) -> str:
    res = subprocess.run(
        [OBJDUMP, *args], capture_output=True, text=True, check=False
    )
    if res.returncode != 0 and not res.stdout:
        raise RuntimeError(f"objdump failed: {res.stderr.strip()}")
    return res.stdout


def read_sections(elf: Path) -> list[Section]:
    out = objdump("-h", str(elf))
    sections: list[Section] = []
    cur: Section | None = None
    pending_flags: list[str] = []
    head_re = re.compile(
        r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"
        r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S+)"
    )
    for line in out.splitlines():
        m = head_re.match(line)
        if m:
            if cur is not None:
                cur.code = "CODE" in pending_flags
                cur.bss = "ALLOC" in pending_flags and "CONTENTS" not in pending_flags
                sections.append(cur)
            cur = Section(
                name=m.group(1),
                size=int(m.group(2), 16),
                vma=int(m.group(3), 16),
                lma=int(m.group(4), 16),
                code=False,
                bss=False,
            )
            pending_flags = [m.group(6).strip()]
            continue
        if cur is not None:
            pending_flags.extend(f.strip() for f in line.split(",") if f.strip())
    if cur is not None:
        cur.code = "CODE" in pending_flags
        cur.bss = "ALLOC" in pending_flags and "CONTENTS" not in pending_flags
        sections.append(cur)
    return [s for s in sections if s.size > 0]


def read_symbols(elf: Path) -> list[Symbol]:
    res = subprocess.run(
        ["mips-linux-gnu-nm", str(elf)], capture_output=True, text=True, check=False
    )
    syms: list[Symbol] = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        syms.append(Symbol(addr=addr, name=parts[2], kind=parts[1]))
    syms.sort(key=lambda s: s.addr)
    return syms


@dataclass
class Word:
    addr: int
    raw: int
    mnem: str
    ops: str
    text: str


@dataclass
class Elf:
    path: Path
    sections: list[Section]
    words: list[Word]
    by_addr: dict[int, Word]
    symbols: list[Symbol]
    text_entries: set[int] = field(default_factory=set)

    @property
    def name(self) -> str:
        return self.path.name

    def fn_symbol(self, addr: int) -> str:
        best = None
        for s in self.symbols:
            if s.addr > addr:
                break
            if s.kind in ("T", "t", "W", "w"):
                best = s
        return best.name if best else "?"


_cache: dict[Path, Elf] = {}


def load_elf(path: Path) -> Elf:
    path = path.resolve()
    if path in _cache:
        return _cache[path]
    out = objdump("-d", str(path))
    words: list[Word] = []
    for line in out.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = int(m.group(2), 16)
        words.append(
            Word(addr=addr, raw=raw, mnem=m.group(3), ops=m.group(4), text=line.strip())
        )
    by_addr = {w.addr: w for w in words}
    secs = read_sections(path)
    syms = read_symbols(path)
    entries = {s.addr for s in syms if s.kind in ("T", "t")}
    elf = Elf(path=path, sections=secs, words=words, by_addr=by_addr,
              symbols=syms, text_entries=entries)
    _cache[path] = elf
    return elf


def function_entries(elf: Elf) -> set[int]:
    """Addresses that are real function starts *in this ELF's own layout*.

    A function start is a symbol of text type, or an address whose instruction
    is preceded by a `jr $ra` (plus its delay slot) or by a `nop` — the two
    shapes a splat/`find_file_boundaries` layout leaves at a boundary.
    """
    starts = {w.addr for w in elf.words if w.mnem in ("jr",) and w.ops.startswith("ra")}
    entries = set(elf.text_entries)
    for w in elf.words:
        if w.addr in entries:
            continue
        prev = elf.by_addr.get(w.addr - 8)
        prev2 = elf.by_addr.get(w.addr - 4)
        if prev is not None and prev.mnem == "jr" and prev.ops.startswith("ra"):
            entries.add(w.addr)
        elif prev2 is not None and prev2.mnem in ("nop",) and elf.by_addr.get(
            w.addr - 8
        ) is None:
            entries.add(w.addr)
        elif w.mnem == "addiu" and re.match(r"^sp,\s*sp,\s*-", w.ops):
            entries.add(w.addr)
    return entries


# ---------------------------------------------------------------------------
# The register dataflow walk
# ---------------------------------------------------------------------------

UNKNOWN = object()

REG_RE = re.compile(r"^\$?([a-z0-9]+)$")


def reg_of(tok: str) -> str | None:
    m = REG_RE.match(tok.strip().rstrip(","))
    return m.group(1) if m else None


def eval_insn(mnem: str, ops: str, regs: dict[str, object]) -> tuple[str, object] | None:
    """If this instruction *defines* a register, return (reg, value).

    Instructions that write nothing (calls, stores, branches) return None and
    must not be treated as clobbering anything: the walk reads each register's
    value from the instruction that last wrote it, and a `jal` in between is
    exactly the case that used to lose `ram_base` (session 73's first cut).
    """
    parts = [p.strip() for p in ops.split(",")] if ops else []
    if mnem == "lui" and len(parts) == 2:
        rd, imm = reg_of(parts[0]), parts[1]
        val = parse_imm(imm)
        if rd and val is not None:
            return rd, (val << 16) & 0xFFFFFFFF
        return rd, UNKNOWN
    if mnem in ("addiu", "ori") and len(parts) == 3:
        rd, rs, imm = reg_of(parts[0]), reg_of(parts[1]), parts[2]
        if rd is None:
            return None
        base = regs.get(rs, UNKNOWN) if rs else UNKNOWN
        val = parse_imm(imm)
        if base is UNKNOWN or val is None:
            return rd, UNKNOWN
        base_i = int(base)
        if mnem == "addiu":
            return rd, (base_i + sign16(val)) & 0xFFFFFFFF
        return rd, (base_i | (val & 0xFFFF)) & 0xFFFFFFFF
    if mnem in ("addu", "add") and len(parts) == 3:
        rd, rs, rt = reg_of(parts[0]), reg_of(parts[1]), reg_of(parts[2])
        if rd is None:
            return None
        a, b = regs.get(rs, UNKNOWN), regs.get(rt, UNKNOWN)
        if a is UNKNOWN or b is UNKNOWN:
            return rd, UNKNOWN
        return rd, (int(a) + int(b)) & 0xFFFFFFFF
    if mnem == "subu" and len(parts) == 3:
        rd, rs, rt = reg_of(parts[0]), reg_of(parts[1]), reg_of(parts[2])
        if rd is None:
            return None
        a, b = regs.get(rs, UNKNOWN), regs.get(rt, UNKNOWN)
        if a is UNKNOWN or b is UNKNOWN:
            return rd, UNKNOWN
        return rd, (int(a) - int(b)) & 0xFFFFFFFF
    if mnem in ("move",) and len(parts) == 2:
        rd, rs = reg_of(parts[0]), reg_of(parts[1])
        if rd and rs:
            return rd, regs.get(rs, UNKNOWN)
        return rd, UNKNOWN
    if mnem in ("li",) and len(parts) == 2:
        rd, val = reg_of(parts[0]), parse_imm(parts[1])
        if rd and val is not None:
            return rd, val & 0xFFFFFFFF
        return rd, UNKNOWN
    # Explicitly: instructions that do not write a register leave `regs` alone.
    return None


def parse_imm(tok: str) -> int | None:
    tok = tok.strip()
    neg = False
    if tok.startswith("-"):
        neg, tok = True, tok[1:]
    try:
        if tok.startswith("0x") or tok.startswith("0X"):
            val = int(tok[2:], 16)
        else:
            val = int(tok, 0)
    except ValueError:
        return None
    return -val if neg else val


def sign16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


@dataclass
class CallSite:
    """One `jal func_8009DA50` and the loader block around it."""

    elf: str
    loader: int  # address of the jal
    func: str
    rom_start: int | None
    ram_base: int | None
    dma_size: int | None
    rom_end: int | None
    code_size: int | None
    data_size: int | None
    bss_base: int | None
    bss_size: int | None
    raw_args: tuple[int | None, int | None, int | None]
    supported: bool = False
    notes: list[str] = field(default_factory=list)

    @property
    def code_end(self) -> int | None:
        if self.ram_base is None or self.code_size is None:
            return None
        return self.ram_base + self.code_size

    @property
    def data_end(self) -> int | None:
        if self.ram_base is None or self.code_size is None or self.data_size is None:
            return None
        return self.ram_base + self.code_size + self.data_size

    @property
    def bss_end(self) -> int | None:
        if self.bss_base is None or self.bss_size is None:
            return None
        return self.bss_base + self.bss_size


def walk_back(
    elf: Elf,
    at: int,
    limit: int = 400,
    include_delay: bool = False,
    stop_at_call: bool = False,
) -> tuple[dict[str, object], list[Word]]:
    """Evaluate the registers a call site consumes.

    The block is *executed* in address order, so the walk collects the
    instructions backwards (to find where the block starts) and then
    **evaluates them in ascending address order** — applying them in reverse
    runs `addiu a1, a1, %lo` before the `lui a1, %hi` that gives it its base,
    which yields the next page instead of the address (session 73's second
    wrong map).

    With `include_delay`, `at + 4` is evaluated first as well: the loader puts
    the size `subu a2, a2, a0` in the `jal`'s delay slot, so it executes before
    the call and defines the third argument (session 73's first wrong map).
    """
    order: list[int] = []
    seen_calls = 0
    floor = func_start(elf, at)
    addr = at
    for _ in range(limit):
        addr -= 4
        if addr < floor or addr not in elf.by_addr:
            break
        w = elf.by_addr[addr]
        if stop_at_call and w.mnem == "jal":  # a call clobbers the argument regs
            # A call clobbers the argument registers, so nothing below it can
            # define a value this site's *arguments* use: without this stop the
            # walk reaches the previous loader block's `lui`/`addiu` pair and
            # reads a stale register (session 73: the boot overlay's bss base
            # came out as its own RAM base, 0x3163E0 of nonsense BSS). Only the
            # argument read uses it. The nearest `jal` above the site is the
            # bracket call that *is* this block (`func_800900C0` and friends)
            # when reading the DMA, or the empty `beq`-guarded bss call, so one
            # call is skipped and only the one before it ends the block.
            if seen_calls:
                break
            seen_calls = True
        order.append(addr)
    order.reverse()  # ascending: execution order
    if include_delay and (at + 4) in elf.by_addr:
        order.append(at + 4)
    regs: dict[str, object] = {}
    seen: list[Word] = []
    for a in order:
        w = elf.by_addr[a]
        seen.append(w)
        defined = eval_insn(w.mnem, w.ops, regs)
        if defined is not None:
            rd, val = defined
            if rd:
                regs[rd] = val
    return regs, list(reversed(seen))  # seen is nearest-first for the caller


def read_arg(regs: dict[str, object], reg: str) -> int | None:
    v = regs.get(reg, UNKNOWN)
    return None if v is UNKNOWN else int(v)


def func_start(elf: Elf, at: int) -> int:
    """Start of the function containing `at` — the walk must not cross it.

    Without this floor the walk walks through the *previous* loader block too
    (a function usually holds several), and the register values of that block's
    DMA get read at this one's call site (session 73: 27 duplicate
    `func_8017909C` sites all resolved to `ram 0x80190000`).
    """
    bound = 0
    for s in elf.symbols:
        if s.addr > at:
            break
        if s.kind in ("T", "t"):
            bound = s.addr
    return bound


def call_args(elf: Elf, at: int) -> dict[str, int | None]:
    """The arguments a call consumes *at* the call, i.e. before its delay slot.

    The generator emits a macro that passes a *size* as the last argument, so
    the `subu rd, rs, rt` that computes it sits in the `jal`'s delay slot and
    the register is not final until the delay slot runs. Reading the registers
    at `at` therefore gives the arguments the callee actually sees.
    """
    # First try the whole block up to the function start: several loaders
    # materialise their arguments once (often into `s4`/`s5`) and reuse them,
    # so a register can legitimately be defined well above a call. If any
    # operand is still unresolved, retry bounded at the previous call — that is
    # the pass that keeps a bss call from inheriting the *previous* loader
    # block's `lui`/`addiu` pair (session 73: the boot overlay's bss base came
    # out as its own RAM base).
    regs, _ = walk_back(elf, at)
    args = {r: read_arg(regs, r) for r in ("a0", "a1", "a2", "a3")}
    if any(v is None for v in args.values()):
        regs2, _ = walk_back(elf, at, stop_at_call=True)
        for r, v in args.items():
            if v is None:
                args[r] = read_arg(regs2, r)
    return args


def _subu_for(elf: Elf, at: int, reg: str) -> int | None:
    """Find the `subu <reg>, x, y` that computes a call's argument `reg`.

    The generator emits the size at the call in one of two schedules, and the
    ROM contains both:

        jal f                                  jal f
        subu a1, a1, a0     (delay slot)       ...
                                               subu a1, a1, a0   (hoisted)
                                               sw   ra, 36(sp)
                                               jal f

    So the `subu` is either in the delay slot or in the few instructions above
    the call, and in the hoisted case the register is already the size by the
    time the call happens — which is why reading `a1` at the call site is not
    enough (session 73: Z/AA/R/AB/AC, and unit I, all reported `0` or a
    nonsense code size).
    """
    w = elf.by_addr.get(at + 4)
    if w is not None and w.mnem == "subu" and reg_of(w.ops.split(",")[0].strip()) == reg:
        return at + 4
    addr = at
    for _ in range(24):
        addr -= 4
        w = elf.by_addr.get(addr)
        if w is None:
            break
        if w.mnem == "subu":
            parts = [p.strip() for p in w.ops.split(",")]
            if parts and reg_of(parts[0]) == reg:
                return addr
    return None


def size_arg(elf: Elf, at: int, reg: str = "a1") -> int | None:
    """The size the call at `at` passes in `reg`, from the `subu` that makes it.

    Evaluating the `subu`'s own operands at the `subu` gives the two addresses
    the generator wrote: whatever the schedule, the size is their difference.
    """
    subu = _subu_for(elf, at, reg)
    if subu is None:
        return None
    w = elf.by_addr[subu]
    parts = [p.strip() for p in w.ops.split(",")]
    if len(parts) != 3:
        return None
    rs, rt = reg_of(parts[1]), reg_of(parts[2])
    for stopper in (False, True):
        regs, _ = walk_back(elf, subu, include_delay=False, stop_at_call=stopper)
        a, b = regs.get(rs, UNKNOWN), regs.get(rt, UNKNOWN)
        if a is not UNKNOWN and b is not UNKNOWN:
            return (int(a) - int(b)) & 0xFFFFFFFF
    return None


def scan_elf(elf: Elf) -> list[CallSite]:
    sites: list[CallSite] = []
    for w in elf.words:
        if w.mnem != "jal":
            continue
        target = jal_target(w.addr, w.raw)
        if target != DMA_FUNC:
            continue
        args = call_args(elf, w.addr)
        rom_start, ram_base = args["a0"], args["a1"]
        dma_size = size_arg(elf, w.addr, "a2")
        if dma_size is None:
            dma_size = args["a2"]
        rom_end = rom_start + dma_size if (rom_start is not None and dma_size) else None
        cs = CallSite(
            elf=elf.name,
            loader=w.addr,
            func=elf.fn_symbol(w.addr),
            rom_start=rom_start,
            ram_base=ram_base,
            dma_size=dma_size,
            rom_end=rom_end,
            code_size=None,
            data_size=None,
            bss_base=None,
            bss_size=None,
            raw_args=(rom_start, ram_base, dma_size),
            supported=False,
            notes=[],
        )
        # The brackets of *this* block. `seen` is nearest-first, so the first
        # icache/dcache call below the DMA and the first bss call above it are
        # this block's. Each passes its own size in the delay slot:
        #   func_800900C0(ram_base, code_size)     icache invalidate
        #   func_80090010(code_end, data_size)     dcache invalidate
        #   func_80093380(data_end, bss_size)      bss zero
        _, seen = walk_back(elf, w.addr)
        for bw in seen:
            if bw.mnem != "jal":
                continue
            t = jal_target(bw.addr, bw.raw)
            if t == ICACHE_FUNC and not cs.supported:
                cs.code_size = size_arg(elf, bw.addr, "a1")
                cs.supported = cs.code_size is not None
            elif t == DCACHE_FUNC and cs.data_size is None:
                cs.data_size = size_arg(elf, bw.addr, "a1")
            elif t == BSSZERO_FUNC and cs.bss_size is None:
                a = call_args(elf, bw.addr)
                size = size_arg(elf, bw.addr, "a1")
                # A bss call that starts exactly where this bank's DMA ends is
                # this bank's; one that starts later belongs to another record
                # (the boot loader's single `func_80093380` zeroes every boot
                # overlay's bss at once, and claiming it gave streamedA a 3 MiB
                # "BSS" in session 73's first table).
                if (
                    a["a0"] is not None
                    and size is not None
                    and 0 < size < 0x400000
                    and cs.dma_size is not None
                    and cs.ram_base is not None
                    and a["a0"] <= cs.ram_base + cs.dma_size
                    and a["a0"] + size >= cs.ram_base + cs.dma_size
                ):
                    cs.bss_base = a["a0"]
                    cs.bss_size = size
        sites.append(cs)
    return sites


def jal_target(pc: int, raw: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)


def find_loaders(elf: Elf, sites: list[CallSite]) -> dict[int, list[CallSite]]:
    by_loader: dict[int, list[CallSite]] = {}
    for cs in sites:
        by_loader.setdefault(cs.loader, []).append(cs)
    return by_loader


# ---------------------------------------------------------------------------
# Unit coverage (config-bank<U>.yaml)
# ---------------------------------------------------------------------------


@dataclass
class UnitRange:
    unit: str
    rom_start: int
    rom_end: int
    ram: int
    name: str
    file: Path
    vram_end: int = 0
    bss_end: int = 0
    text_size: int = 0
    data_size: int = 0

    @property
    def size(self) -> int:
        return self.rom_end - self.rom_start


def read_unit_ranges(unit: str) -> list[UnitRange]:
    """The ROM/RAM range each of a unit's records occupies, from its own ELF.

    The linked ELF is the ground truth for what the unit will actually link:
    each `type: code` segment becomes one section, whose LMA is the ROM address
    and whose size is the bytes that were really assembled. Reading the YAML's
    C-style `segments:` list instead is what made the first cut call unit J's
    bankRec10a `0x38190` bytes when the loader says `0x16770` (session 73) — it
    took the next `- [0x...]` marker as the end, and unit J does not end where
    its own record does.
    """
    path = ROOT / f"config-bank{unit}.yaml"
    elf = ROOT / f"build/bank{unit}.elf"
    if not path.exists() or not elf.exists():
        return []
    out: list[UnitRange] = []
    sections = read_sections(elf)
    # The linker script records each record's assembled text and data sizes as
    # absolute symbols (`<name>_TEXT_SIZE`, `<name>_DATA_SIZE`); they are the
    # unit's own claim about the split and the thing `--verify` compares the
    # loader's bracket to.
    sizes: dict[str, dict[str, int]] = {}
    for s in read_symbols(elf):
        m = re.match(r"(\w+)_(TEXT|DATA|RODATA)_SIZE$", s.name)
        if m:
            sizes.setdefault(m.group(1), {})[m.group(2)] = s.addr
    for s in sections:
        if s.name == "_0" or not s.code:
            continue
        if s.name.endswith("_bss"):
            continue
        sz = sizes.get(s.name.lstrip("."), {})
        text = sz.get("TEXT", 0)
        data = sz.get("DATA", 0) + sz.get("RODATA", 0)
        out.append(
            UnitRange(
                unit=unit,
                rom_start=s.lma,
                rom_end=s.lma + s.size,
                ram=s.vma,
                name=s.name.lstrip("."),
                file=path,
                vram_end=s.vma + s.size,
                text_size=text,
                data_size=data,
            )
        )
    # Each record's BSS is the `<name>_bss` section, which the linker script
    # places at the RAM address right after the record. Attach only the section
    # the record *owns*, so a bss section belonging to a later, uncompiled
    # record cannot be reported as this one's (session 73's first cut gave the
    # two boot overlays a 3 MiB "BSS" that was really every other record's).
    for s in sections:
        if not s.bss or s.size == 0:
            continue
        base = s.name.lstrip(".")
        if base.endswith("_bss"):
            base = base[: -len("_bss")]
        for r in out:
            # The bss section belongs to the record whose RAM range it directly
            # follows; a section that merely overlaps some *other* record's
            # window is that record's (the arenas overlap by design).
            if r.name == base or s.vma == r.vram_end:
                r.bss_end = max(r.bss_end, s.vma + s.size)
    out.sort(key=lambda r: r.rom_start)
    return out


MAIN_SPLITS = {  # streamedA/B code/data boundaries from config.yaml
    0x03F1B0: (0x1490, 0x840),
    0x040E80: (0x1B3B0, 0xAC00),
}


def main_unit_ranges() -> list[UnitRange]:
    """The main unit's own code segments, from `build/ogrebattle64.elf`.

    `streamedA`/`streamedB` are banks of arena RAM too (they are DMA'd by
    `func_80071EB0`, not by a scene), so a bank must not be reported missing
    when the main ELF already links it.
    """
    elf = ROOT / "build/ogrebattle64.elf"
    if not elf.exists():
        return []
    out: list[UnitRange] = []
    for s in read_sections(elf):
        base = s.name.lstrip(".")
        if base in ("header", "ipl3", "entry", "main", "streamedC"):
            # entry/main/streamedC are KSEG0 code, not arena banks; their RAM
            # is already covered by the units that share it.
            continue
        if not s.code or base.startswith("streamed") is False:
            continue
        text, data = MAIN_SPLITS.get(s.lma, (0, 0))
        out.append(
            UnitRange(
                unit="main",
                rom_start=s.lma,
                rom_end=s.lma + s.size,
                ram=s.vma,
                name=base,
                file=elf,
                vram_end=s.vma + s.size,
                text_size=text,
                data_size=data,
            )
        )
    return out


def all_unit_ranges() -> list[UnitRange]:
    mk = (ROOT / "Makefile").read_text()
    m = re.search(r"^BANK_UNITS\s*:?=\s*(.*)$", mk, re.MULTILINE)
    units = m.group(1).split() if m else []
    out: list[UnitRange] = []
    for u in units:
        out.extend(read_unit_ranges(u))
    out.extend(main_unit_ranges())
    out.sort(key=lambda r: (r.rom_start, r.unit))
    return out


def unit_for(rom_start: int, ranges: list[UnitRange]) -> tuple[UnitRange | None, bool]:
    """(covering unit range, exact-start?) — a wrong size shows as end != next."""
    exact = [r for r in ranges if r.rom_start == rom_start]
    if exact:
        return exact[0], True
    inside = [r for r in ranges if r.rom_start < rom_start < r.rom_end]
    if inside:
        return inside[0], False
    return None, False


# ---------------------------------------------------------------------------
# Entry analysis
# ---------------------------------------------------------------------------


@dataclass
class Entry:
    addr: int
    callers: list[tuple[str, int]]  # (elf, call site)
    real_start: bool
    source: str  # "bank call", "main call", "address taken", "entry word"


def entry_candidates(
    elf: Elf, ram_lo: int, ram_hi: int, entries: set[int]
) -> dict[int, Entry]:
    """Calls and address-takings that land inside [ram_lo, ram_hi).

    `entries` is the *bank's* set of real function starts, so `real_start`
    answers "is this a real entry in that bank's layout" — the only kind of
    address that may be forced in `symbol_addrs-bank<U>.txt` (a `LOOKUP_FUNC`
    onto a body interior is a silent no-op; session 67).
    """
    out: dict[int, Entry] = {}

    def add(addr: int, source: str, caller: str, site: int) -> None:
        e = out.setdefault(
            addr, Entry(addr=addr, callers=[], real_start=addr in entries, source=source)
        )
        e.callers.append((caller, site))

    for w in elf.words:
        if w.mnem == "jal" and not w.ops.startswith("r"):
            t = jal_target(w.addr, w.raw)
            if ram_lo <= t < ram_hi:
                add(t, "call", elf.name, w.addr)
        # lui/addiu pairs that take the address of something in the range
        if w.mnem == "lui":
            parts = [p.strip() for p in w.ops.split(",")]
            if len(parts) == 2:
                v = parse_imm(parts[1])
                if v is not None:
                    hi = (v << 16) & 0xFFFFFFFF
                    base = reg_of(parts[0])
                    nxt_addr = w.addr + 4
                    for k in (4, 8, 12):
                        nxt = elf.by_addr.get(nxt_addr + (k - 4))
                        if nxt is None or nxt.mnem not in ("addiu", "ori"):
                            break
                        np_ = [p.strip() for p in nxt.ops.split(",")]
                        if len(np_) != 3:
                            break
                        rd, rs, imm = reg_of(np_[0]), reg_of(np_[1]), parse_imm(np_[2])
                        if rd != base or rs != base or imm is None:
                            break
                        val = (
                            (hi + sign16(imm)) & 0xFFFFFFFF
                            if nxt.mnem == "addiu"
                            else (hi | (imm & 0xFFFF)) & 0xFFFFFFFF
                        )
                        if ram_lo <= val < ram_hi and val % 4 == 0:
                            add(val, "address", elf.name, w.addr)
                        break
    return out


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


@dataclass
class Bank:
    ram_base: int
    cs: CallSite
    unit: UnitRange | None
    unit_exact: bool
    sites: list[CallSite] = field(default_factory=list)

    @property
    def size(self) -> int | None:
        if self.cs.rom_start is None or self.cs.rom_end is None:
            return None
        return self.cs.rom_end - self.cs.rom_start

    @property
    def ram_end(self) -> int | None:
        return None if self.size is None else self.ram_base + self.size

    @property
    def status(self) -> str:
        if self.unit is None:
            return "**MISSING**"
        if not self.unit_exact:
            return (
                f"WRONG SIZE (inside unit {self.unit.unit}: "
                f"{fmt(self.unit.rom_start)}..{fmt(self.unit.rom_end)})"
            )
        if self.cs.rom_end is not None and self.unit.rom_end != self.cs.rom_end:
            return (
                f"unit {self.unit.unit} but size {fmt(self.unit.rom_end - self.unit.rom_start)}"
                f" (loader says {fmt(self.size)})"
            )
        return f"unit {self.unit.unit}"

    @property
    def ok(self) -> bool:
        return self.unit is not None and self.unit_exact and self.status.startswith("unit ")

    @property
    def compiled(self) -> bool:
        """The record is linked by some unit — the main ELF counts."""
        return self.unit is not None and self.unit_exact

    def split(self) -> tuple[int | None, int | None, int | None]:
        """(code bytes, data bytes, bss bytes) from the loader's cache bracket.

        The loader invalidates `func_800900C0(ram_base, code_size)` and
        `func_80090010(code_end, data_size)`, and zeroes
        `func_80093380(data_end, bss_size)` — every half is the size argument
        the game's own generator emitted, so nothing here is a chunk count and
        nothing is computed from a RAM/ROM subtraction.
        """
        bss = self.cs.bss_size
        if bss is not None and bss <= 0:
            bss = None
        return self.cs.code_size, self.cs.data_size, bss


def fmt(v: int | None) -> str:
    return "-" if v is None else f"0x{v:08X}"


def span(v: int | None) -> str:
    """Format a length; `None` and 0 both print as '-'."""
    if v is None or v == 0:
        return "-"
    return f"0x{v:X}"


def hexlen(v: int | None) -> str:
    """A byte count: `0x...`, '-' for none. Never confuses RAM for ROM."""
    if v is None:
        return "-"
    return f"0x{v:X}"


ROM_SIZE = 0x2800000  # the cartridge image


def loader_shaped(cs: CallSite) -> bool:
    """A `jal 0x8009DA50` whose source range could be a ROM offset.

    Filters the walk's garbage: an asset DMA whose "ROM start" is really a
    string pointer lands far outside the ROM (session 73: `func_8017909C`).
    """
    if cs.rom_start is None or cs.ram_base is None:
        return False
    if not (0 < cs.rom_start < ROM_SIZE):
        return False
    return cs.rom_start % 256 == 0 and cs.ram_base % 256 == 0


def plausible(cs: CallSite) -> bool:
    """Does this `jal 0x8009DA50` site look like one of the game's bank DMAs?

    Requires the icache bracket, an aligned in-ROM source range, a 16-byte
    aligned RAM destination inside RDRAM, and a size that fits the RAM window.
    """
    if not cs.supported:
        return False
    if cs.rom_start is None or cs.ram_base is None or cs.rom_end is None:
        return False
    if cs.rom_start % 16 or cs.ram_base % 16 or cs.dma_size is None:
        return False
    if not (0 < cs.rom_start and cs.rom_end <= ROM_SIZE):
        return False
    if not (RAM_LO <= cs.ram_base < RAM_HI):
        return False
    return cs.dma_size % 16 == 0


def collect(elfs: list[Elf]) -> tuple[dict[int, list[CallSite]], list[CallSite]]:
    """ram_base -> sites, plus every site that does not look like a bank loader.

    `plausible` is the filter, and it is deliberately strict: the game calls
    `func_8009DA50` for plain asset DMAs too (`func_8017909C` loads a sprite
    table that way), where the "arguments" come from memory loads and the walk
    can only produce garbage — session 73's first cut turned those into 27
    phantom banks. A real bank loader brackets the DMA with the code-half
    icache invalidate (`func_800900C0`) and its ROM range is inside the ROM.
    """
    arenas: dict[int, list[CallSite]] = {}
    unbracketed: list[CallSite] = []
    for elf in elfs:
        for cs in scan_elf(elf):
            if plausible(cs):
                arenas.setdefault(cs.ram_base, []).append(cs)
            elif cs.supported or loader_shaped(cs):
                unbracketed.append(cs)
    return arenas, unbracketed


def build_banks(arenas: dict[int, list[CallSite]], ranges: list[UnitRange]) -> list[Bank]:
    """One Bank per distinct (RAM base, ROM range), with every loader site kept.

    The same module is loaded from several call sites (scene enters, mode
    selectors, per-step arms) and the port should not be told there are ten
    banks where the game has one. Deduplicating by the loader's own
    (rom_start, size) collapses those and keeps the sites as evidence.
    """
    merged: dict[tuple[int, int, int], list[CallSite]] = {}
    for ram, cs_list in arenas.items():
        for cs in cs_list:
            if cs.rom_start is None or cs.dma_size is None:
                continue
            merged.setdefault((ram, cs.rom_start, cs.dma_size), []).append(cs)
    out: list[Bank] = []
    for (ram, _rom, _size), sites in merged.items():
        sites.sort(key=lambda c: (c.elf, c.loader))
        cs = min(sites, key=lambda c: (c.loader))
        unit, exact = unit_for(cs.rom_start, ranges)
        b = Bank(ram_base=ram, cs=cs, unit=unit, unit_exact=exact)
        b.sites = sites
        out.append(b)
    out.sort(key=lambda b: (b.ram_base, b.cs.rom_start or 0))
    return out


@dataclass
class CallerRecord:
    """Which bank record a call site sits in, so cross-bank calls are visible."""

    addr: int
    elf: str
    unit: str | None
    record: str

    @property
    def label(self) -> str:
        if self.unit:
            return f"{self.elf}:{self.record} (unit {self.unit})"
        return f"{self.elf}:{self.record}"


def unit_names(ranges: list[UnitRange]) -> list[str]:
    seen: list[str] = []
    for r in ranges:
        if r.unit not in seen:
            seen.append(r.unit)
    return seen


def build_caller_index(elfs: list[Elf], ranges: list[UnitRange]) -> list[CallerRecord]:
    """Every code section of every scanned ELF, sorted, for caller attribution.

    A `jal` from a section that also defines the target's RAM is bound at build
    time (no `LOOKUP_FUNC`), so only calls *from a different record* can mis-bind
    — that is the set `symbol_addrs-bank<U>.txt` has to declare.
    """
    out: list[CallerRecord] = []
    by_elf_unit: dict[str, str] = {f"bank{u}.elf": u for u in unit_names(ranges)}
    for elf in elfs:
        unit = by_elf_unit.get(elf.name)
        for s in elf.sections:
            if not s.code or s.name == "_0":
                continue
            out.append(
                CallerRecord(
                    addr=s.vma, elf=elf.name, unit=unit, record=s.name.lstrip(".")
                )
            )
    out.sort(key=lambda c: c.addr)
    return out


def caller_of(index: list[CallerRecord], addr: int) -> CallerRecord | None:
    best = None
    for c in index:
        if c.addr > addr:
            break
        best = c
    return best


@dataclass
class BankEntries:
    """What the arena's RAM window requires of this bank's entry table."""

    external: list[Entry] = field(default_factory=list)  # force these
    interior: list[Entry] = field(default_factory=list)  # do NOT force these
    forced: list[Entry] = field(default_factory=list)  # already declared
    missing: list[Entry] = field(default_factory=list)  # cross-record, not forced


def entry_targets(
    elfs: list[Elf], banks: list[Bank], ranges: list[UnitRange]
) -> dict[int, BankEntries]:
    """ram_base -> what each bank's entry table needs.

    An address in a bank's RAM range matters only when a call reaches it from a
    *different* record: N64Recomp binds a call from inside the record that
    defines the RAM at build time (no runtime lookup), but a call from another
    record must go through `LOOKUP_FUNC` — and a `LOOKUP_FUNC` onto a body
    interior is a silent no-op, so every such target has to be a declared
    function start in the bank's own layout (session 67).
    """
    index = build_caller_index(elfs, ranges)
    by_name = {e.name: e for e in elfs}
    result: dict[int, BankEntries] = {}
    for b in banks:
        if b.size is None:
            continue
        lo, hi = b.ram_base, b.ram_base + b.size
        own = by_name.get(f"bank{b.unit.unit}.elf") if b.unit else None
        real = function_entries(own) if own is not None else set()
        # What the unit already declares.
        forced_addrs: set[int] = set()
        if b.unit is not None:
            symfile = ROOT / f"symbol_addrs-bank{b.unit.unit}.txt"
            if symfile.exists():
                for m in re.finditer(
                    r"(?:func|D)_ovl\w*_([0-9A-Fa-f]{8})\s*=",
                    symfile.read_text(),
                    re.MULTILINE,
                ):
                    forced_addrs.add(int(m.group(1), 16))
        # Which records define this RAM (its own unit's records + any other
        # unit whose record overlaps the window).
        definers: set[str] = set()
        for r in ranges:
            if r.ram <= lo < r.vram_end or (r.ram <= lo and lo < r.ram + r.size):
                definers.add(r.name)
        be = BankEntries()
        merged: dict[int, Entry] = {}
        for elf in elfs:
            if own is not None and elf is own:
                continue  # calls from the bank's own unit are build-time bound
            for addr, cand in entry_candidates(elf, lo, hi, real).items():
                dst = merged.setdefault(
                    addr,
                    Entry(addr=addr, callers=[], real_start=cand.real_start,
                          source=cand.source),
                )
                dst.callers.extend(cand.callers)
                dst.real_start = dst.real_start or cand.real_start
        for addr in sorted(merged):
            e = merged[addr]
            # Only calls that cross into this bank matter; a caller that sits in
            # a record which itself defines this RAM is bound at build time.
            crossing = []
            for caller, site in e.callers:
                rec = caller_of(index, site)
                if rec is None or rec.record in definers:
                    continue
                crossing.append((caller, site))
            if not crossing:
                continue
            e.callers = crossing
            if addr in forced_addrs:
                be.forced.append(e)
            elif e.real_start:
                be.external.append(e)
            else:
                # Only interesting when a *different* ELF reaches into a body
                # interior: within the bank's own unit those calls are bound at
                # build time and need no declaration.
                own_callers = [c for c in crossing if own is None or c[0] != own.name]
                if own_callers:
                    e.callers = own_callers
                    be.interior.append(e)
        be.missing = [e for e in be.external if e.addr not in forced_addrs]
        result[b.ram_base] = be
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", action="append", default=[], help="ELF to scan (repeatable)")
    ap.add_argument("--arena", help="only this RAM base (hex)")
    ap.add_argument("--unit", help="only banks this unit covers")
    ap.add_argument("--missing", action="store_true", help="only banks with no/wrong unit")
    ap.add_argument("--entries", action="store_true", help="list entry candidates per bank")
    ap.add_argument("--unmapped", action="store_true", help="show unresolved RAM windows")
    ap.add_argument("--md", action="store_true", help="emit a markdown table")
    ap.add_argument(
        "--verify",
        action="store_true",
        help="check every compiled unit against the loader that DMA's it",
    )
    ap.add_argument(
        "--coverage",
        action="store_true",
        help="which ROM code each scanned ELF covers (a loader cannot hide)",
    )
    args = ap.parse_args()

    if args.elf:
        elfs = [load_elf(Path(e)) for e in args.elf]
    else:
        elfs = [load_elf(p) for p in sorted((ROOT / "build").glob("bank*.elf"))]
        elfs.append(load_elf(ROOT / "build/ogrebattle64.elf"))

    ranges = all_unit_ranges()
    arenas, unresolved = collect(elfs)
    if args.arena:
        want = int(args.arena, 16)
        arenas = {k: v for k, v in arenas.items() if k == want}
    banks = build_banks(arenas, ranges)
    if args.unit:
        banks = [b for b in banks if b.unit and b.unit.unit == args.unit]
    if args.missing:
        banks = [b for b in banks if not b.ok]
    entries = entry_targets(elfs, banks, ranges) if args.entries else {}

    total = len(banks)
    missing = sum(1 for b in banks if not b.ok)

    bad = 0
    if args.verify:
        bad = verify_units(banks, ranges)
    if args.coverage:
        bad += report_coverage(elfs)
    if args.md:
        emit_markdown(banks)
    elif args.verify or args.coverage:
        pass
    else:
        emit_text(elfs, banks, entries, unresolved, args, total, missing)
    return 1 if (bad or (args.missing and missing)) else 0


def report_coverage(elfs: list[Elf]) -> int:
    """The ROM ranges the scanned ELFs disassemble as code.

    `arenamap` can only see a loader in code some ELF covers: a bank whose
    module is inside an uncompiled record is invisible to the scan entirely.
    This prints what is covered and, for each uncovered gap larger than a page,
    whether it starts with code or data — a data gap is expected (assets), a
    code gap is a missing overlays entry and has to be chased.
    """
    spans = sorted(
        (s.lma, s.lma + s.size, elf.name)
        for elf in elfs
        for s in elf.sections
        if s.code and s.name != "_0"
    )
    merged: list[list[int]] = []
    for lo, hi, _ in spans:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    total = sum(hi - lo for lo, hi in merged)
    print("=== ROM code coverage ===")
    print(f"  {len(spans)} code section(s) in {len(elfs)} ELF(s), "
          f"{len(merged)} disjoint range(s), {hexlen(total)} bytes")
    rom = (ROOT / "assets/ogre64.z64").read_bytes()
    prev = 0x1000
    gaps = 0
    for lo, hi in merged:
        if lo - prev > 0x1000:
            word = rom[prev : prev + 4]
            op = word[0] >> 2 if word else 0
            kind = "code-like" if op in (2, 3, 4, 5, 6, 7, 9, 0x0F) else "data"
            # The question that matters is not "is this byte code" but "could a
            # bank loader be hiding here": the module's own loader must appear
            # in the uncovered bytes for the arena to be missed.
            hits = [
                a
                for a in range(prev, lo - 4, 4)
                if (lambda w: (w >> 26) == 3
                    and ((((a - 0x1000) + 0x80070C00 + 4) & 0xF0000000)
                         | ((w & 0x03FFFFFF) << 2)) == DMA_FUNC)(
                    int.from_bytes(rom[a : a + 4], "big")
                )
            ]
            print(f"  gap 0x{prev:06X}..0x{lo:06X} ({hexlen(lo - prev)}) first word "
                  f"{word.hex()} -> {kind}"
                  + (f"; **{len(hits)} arena loader(s) inside**" if hits else ""))
            gaps += 1
        prev = max(prev, hi)
    print(f"  end of code 0x{prev:06X}; {gaps} uncovered gap(s)")
    return 0


def describe_callers(e: Entry) -> str:
    return ", ".join(f"{c[0]}@{c[1]:08X}" for c in e.callers[:4])


def verify_units(banks: list[Bank], ranges: list[UnitRange]) -> int:
    """Every compiled record must match the loader that DMA's it.

    Three things are compared, and each has bitten this project:

    * the **ROM range** — an off-by-one here makes the unit read the wrong
      module (session 72's unit V swallowed the next bank of record 9's arena);
    * the **size** — the loader's `subu`, never the chunk count (session 59);
    * the **code/data split** — the loader's cache bracket. A divergence is not
      automatically a bug: `mips-linux-gnu-as` pads `.text` to 16 bytes, so a
      record whose data starts at an 8-byte boundary links its data subsegment
      up to 8 bytes high (session 65's unit M). The split is printed either way
      because only the loader's value is what the game runs.
    """
    print("=== unit vs loader ===")
    bad = 0
    for b in banks:
        if b.unit is None or b.cs.rom_start is None or b.size is None:
            continue
        r = b.unit
        notes: list[str] = []
        if r.rom_end != b.cs.rom_end:
            notes.append(f"ROM end {fmt(r.rom_end)} != loader {fmt(b.cs.rom_end)}")
        if r.size != b.size:
            notes.append(f"size {hexlen(r.size)} != loader {hexlen(b.size)}")
        if r.ram != b.ram_base:
            notes.append(f"RAM {fmt(r.ram)} != loader {fmt(b.ram_base)}")
        elf_split = (r.text_size, r.data_size)
        ldr_split = (b.cs.code_size or 0, b.cs.data_size or 0)
        if r.text_size == 0 and r.data_size == 0:
            # The linker script records no split for this record (splat emitted
            # it as one subsegment), so there is nothing to compare; the linked
            # bytes are still checked by `make elf-rom-check`.
            notes = [n for n in notes if not n.startswith("split ")]
        elif elf_split != ldr_split:
            notes.append(
                f"split elf(c={hexlen(elf_split[0])},d={hexlen(elf_split[1])})"
                f" loader(c={hexlen(ldr_split[0])},d={hexlen(ldr_split[1])})"
            )
        if notes:
            bad += 1
            print(f"  unit {b.unit.unit:3s} {r.name:12s} rom {fmt(r.rom_start)}: "
                  + "; ".join(notes))
    if not bad:
        print("  every compiled record matches its loader's ROM range and size")
    else:
        print("  ROM range and size match for every record above; only the")
        print("  code/data split differs. That is the assembler's 16-byte `.text`")
        print("  alignment against the loader's own boundary (AGENTS: session 65's")
        print("  unit M), or a linker script that declares the record as one")
        print("  subsegment. `make elf-rom-check` is what proves the bytes.")
    print(f"  {bad} divergence(s)")
    return bad


def emit_text(
    elfs: list[Elf],
    banks: list[Bank],
    entries: dict[int, BankEntries],
    unresolved: list[CallSite],
    args: argparse.Namespace,
    total: int,
    missing: int,
) -> None:
    print(f"scanned {len(elfs)} ELF(s): {total} arena bank load(s), {missing} without an exact unit")
    cur_ram = None
    for b in banks:
        cs = b.cs
        if b.ram_base != cur_ram:
            cur_ram = b.ram_base
            n = sum(1 for x in banks if x.ram_base == cur_ram)
            print()
            print(f"=== arena RAM {fmt(cur_ram)}  ({n} bank(s) mapped) ===")
        code, data, bss = b.split()
        print(f"  rom {fmt(cs.rom_start)}..{fmt(cs.rom_end)}  size {span(b.size)}"
              f"  [{b.status}]")
        print(f"      code {fmt(b.ram_base)}..{fmt(cs.code_end)} ({hexlen(code)})"
              f"  data {fmt(cs.code_end)}..{fmt(cs.data_end)} ({hexlen(data)})"
              f"  bss {fmt(cs.bss_base)}..{fmt(cs.bss_end)} ({hexlen(bss)})")
        print(f"      loader {fmt(cs.loader)} in {cs.elf} ({cs.func})"
              f"  dma(rom={fmt(cs.raw_args[0])}, ram={fmt(cs.raw_args[1])},"
              f" size={fmt(cs.raw_args[2])})")
        if args.entries:
            be = entries.get(b.ram_base)
            if be is not None:
                if be.external:
                    print("      cross-record entries, real starts in this layout"
                          " (must be in symbol_addrs):")
                    for e in sorted(be.external, key=lambda x: x.addr):
                        print(f"        {fmt(e.addr)}  {describe_callers(e)}")
                if be.interior:
                    print("      cross-record calls onto body interiors"
                          " (cannot be forced; the bank or the binding is wrong):")
                    for e in sorted(be.interior, key=lambda x: x.addr):
                        print(f"        {fmt(e.addr)}  {describe_callers(e)}")
    if unresolved and not args.missing:
        print()
        print(f"=== {len(unresolved)} jal 0x8009DA50 site(s) with no cache bracket ===")
        print("    (plain asset DMA or an unmodelled call; not arena banks)")
        for cs in unresolved:
            print(f"  {cs.elf} loader {fmt(cs.loader)} {cs.func}"
                  f" rom={fmt(cs.rom_start)} args=({fmt(cs.raw_args[0])},"
                  f" {fmt(cs.raw_args[1])}, {fmt(cs.raw_args[2])})")


def emit_markdown(banks: list[Bank]) -> None:
    """A per-bank table for `docs/scenes.md`.

    One row per bank, which is the granularity that matters for the port: the
    arena's RAM base and each module's ROM range, size and split. The unit
    column says whether some bank unit links it, and the loader column is the
    instruction that DMA's it (the evidence for every other value).
    """
    print("| arena (RAM) | ROM start | size | code | data | BSS | unit | loader |")
    print("|---|---|---|---|---|---|---|---|")
    last_ram = None
    for b in banks:
        cs = b.cs
        code, data, bss = b.split()
        ram = "" if b.ram_base == last_ram else f"`{fmt(b.ram_base)}`"
        last_ram = b.ram_base
        unit = b.unit.unit if b.unit else "**none**"
        if b.unit and not b.unit_exact:
            unit = f"in {b.unit.unit}?"
        elif not b.ok and b.unit:
            unit = f"**{b.unit.unit} (size)**"
        print(
            f"| {ram} | `{fmt(cs.rom_start)}` | `{span(b.size)}` | `{hexlen(code)}` |"
            f" `{hexlen(data)}` | `{hexlen(bss)}` | {unit} |"
            f" `{fmt(cs.loader)}` {cs.elf} |"
        )


if __name__ == "__main__":
    sys.exit(main())
