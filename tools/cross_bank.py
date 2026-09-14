#!/usr/bin/env python3
"""Cross-bank call routing (Phase 4).

The streamed-overlay *bank records* the game DMA's reuse the RAM of overlays the
main ELF also contains (overlay A/B/C) and of each other. N64Recomp binds a
`jal` to a function it knows about as a **direct C call**, so a call from
resident code into one of those RAM ranges is bound at build time to whichever
body the main ELF happens to have there (usually overlay C) — even when a
different bank is resident at run time. That is the session-33 crash
(`func_801989AC` running while a bank is loaded).

This tool implements the two halves of the fix:

  write-seeds      Every function entry a bank unit's own disassembly already
                   found, inside RAM another overlay/bank can also occupy, is
                   forced into that unit's `splat` symbol_addrs. The address is
                   then an entry point in the unit too, so the runtime's
                   `get_function` can resolve a call into that RAM whichever
                   bank is resident. (Entries the unit did *not* find are left
                   alone: splitting an existing body at an arbitrary address
                   would create a function with no prologue.)

  rewrite          Rewrite the main unit's generated C so a call that crosses
                   into a swappable RAM range is emitted as `LOOKUP_FUNC(0xADDR)`
                   instead of a direct call. N64Recomp does this by itself when
                   a `jal` target is defined in several bank units, but the main
                   unit alone cannot know that a bank exists, so the transform
                   has to happen after code generation.

Both halves use one region model: a RAM range is *swappable* when two or more
overlay/bank sections can live at the same address. The bank sides of the model
come from each unit's `config-bank<U>.yaml` (so a unit can be edited without
touching this file); the main side comes from the linker map.

Usage:
    tools/cross_bank.py write-seeds     # -> build/bank<U>/symbol_addrs.txt
    tools/cross_bank.py dispatch        # -> edits RecompiledFuncs/*.c in place
    tools/cross_bank.py dispatch --only 0xADDR[,0xADDR...]
                                        # dispatch just these targets (repeatable
                                        # --only); an --only target with no bank
                                        # entry or no call site is a hard error
                                        # (with no bank data at all it warns and
                                        # leaves the tree alone instead)
    tools/cross_bank.py revert          # undo `dispatch` for entry targets; an
                                        # interior (`--only`) target keeps its
                                        # lookup — `make recomp` regenerates the
                                        # pristine direct call instead
    tools/cross_bank.py report          # analysis only, writes nothing
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_bank_funcs import parse_func_arrays  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RECOMP_DIR = ROOT / "RecompiledFuncs"
MAP_PATH = ROOT / "build" / "ogrebattle64.map"

# The overlay sections of the main ELF that share their RAM with streamed bank
# records. Everything else (entry, main, the boot-resident overlay A/B) is
# linear and never swapped.
MAIN_OVERLAYS = ["streamedA", "streamedB", "streamedC"]

FUNC_DEF_RE = re.compile(
    r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{$"
)
TRACE_RE = re.compile(r"recomp_trace_entry\(rdram, 0x([0-9A-Fa-f]{8}),")
CALL_RE = re.compile(r"^(\s*)(\w+)\(rdram, ctx\);$")
LOOKUP_RE = re.compile(r"^\s*LOOKUP_FUNC\(0x([0-9A-Fa-f]{8})\)\(rdram, ctx\);$")
TRACE_RETURN_RE = re.compile(r"^(\s*)recomp_trace_return\(rdram, \d+\);$")
RETURN_RE = re.compile(r"^(\s*)return;$")
AFTER_LABEL_RE = re.compile(r"^\s*after_(\d+):$")
JAL_COMMENT_RE = re.compile(r"^\s*// 0x([0-9A-Fa-f]{8}): jal\s+0x([0-9A-Fa-f]{8})")
FUNC_VRAM_RE = re.compile(r"^func_(?:ovl[A-Z]+_)?([0-9A-Fa-f]{8})$")
GLABEL_RE = re.compile(r"^(?:glabel|alabel)\s+(\w+)", re.M)
VRAM_SUFFIX_RE = re.compile(r"_([0-9A-Fa-f]{8})$")
NAME_FORMAT_RE = re.compile(r'^\s*symbol_name_format:\s*"([^"]+)"', re.M)


class ToolError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Region model
# ---------------------------------------------------------------------------

def main_section_ranges() -> dict[str, tuple[int, int]]:
    """The main ELF's streamed section ranges, from the linker map."""
    if not MAP_PATH.exists():
        raise ToolError(f"{MAP_PATH.relative_to(ROOT)} missing (run `make`)")
    ranges: dict[str, tuple[int, int]] = {}
    header_re = re.compile(r"^(\.\w+)\s+0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+load address")
    for line in MAP_PATH.read_text().splitlines():
        m = header_re.match(line)
        if not m:
            continue
        name = m.group(1).lstrip(".")
        if name in MAIN_OVERLAYS:
            vram, size = int(m.group(2), 16), int(m.group(3), 16)
            ranges[name] = (vram, vram + size)
    missing = [n for n in MAIN_OVERLAYS if n not in ranges]
    if missing:
        raise ToolError(f"sections {missing} not found in {MAP_PATH.name}")
    return ranges


def parse_yaml_segments(path: Path) -> list[dict]:
    """The segments of a splat config: [{type, name, start, vram}, ...].

    A deliberately small parser: it only needs top-level `segments:` list items
    and their scalar fields, which is exactly what the bank configs are.
    """
    segments: list[dict] = []
    in_segments = False
    cur: dict | None = None
    for line in path.read_text().splitlines():
        if not in_segments:
            if line.startswith("segments:"):
                in_segments = True
            continue
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if line.startswith("  - ") or line.startswith("  -["):
            if cur is not None:
                segments.append(cur)
            cur = {}
            rest = line[4:].strip()
            m = re.match(r"\[(0x[0-9A-Fa-f]+)\]", rest)
            if m:
                cur["type"] = "end"
                cur["start"] = int(m.group(1), 16)
            continue
        if cur is None:
            continue
        m = re.match(r"([a-z_]+):\s*(.+)$", stripped)
        if m:
            key, value = m.group(1), m.group(2).strip()
            if key == "type":
                cur["type"] = value
            elif key == "name":
                cur["name"] = value
            elif key == "subsegments":
                cur["subsegments"] = value
            elif key in ("start", "vram") and value.startswith("0x"):
                cur[key] = int(value, 16)
            elif key == "end" and value.startswith("0x"):
                cur["end"] = int(value, 16)
    if cur is not None:
        segments.append(cur)
    return segments


def bank_unit_configs() -> dict[str, dict]:
    """unit letter -> {ranges: [(ram lo, hi)], name_format: str}."""
    out: dict[str, dict] = {}
    for path in sorted(ROOT.glob("config-bank*.yaml")):
        unit = path.stem.replace("config-bank", "")
        segments = parse_yaml_segments(path)
        # A record's extent ends where the next segment's ROM position starts.
        ranges: list[tuple[int, int]] = []
        for i, seg in enumerate(segments):
            if seg.get("type") != "code" or "vram" not in seg or "start" not in seg:
                continue
            if i + 1 >= len(segments):
                raise ToolError(
                    f"{path.name}: cannot determine the extent of segment "
                    f"{seg.get('name')} (no following segment)"
                )
            size = segments[i + 1]["start"] - seg["start"]
            ranges.append((seg["vram"], seg["vram"] + size))
        fmt = NAME_FORMAT_RE.search(path.read_text())
        out[unit] = {
            "ranges": sorted(ranges),
            "name_format": fmt.group(1) if fmt else "func_$VRAM",
        }
    if not out:
        raise ToolError("no config-bank*.yaml found")
    return out


def bank_function_entries() -> dict[str, set[int]]:
    """unit -> the function entry addresses its own disassembly found.

    Read from the split output (`glabel`/`alabel` in build/bank<U>/asm), which
    is the only place that knows the unit's own function boundaries. Falls back
    to what the unit's recompiled table registered when no split is present.
    """
    funcs = main_functions()
    result: dict[str, set[int]] = {}
    for unit in bank_unit_configs():
        asm_dir = ROOT / f"build/bank{unit}/asm"
        entries: set[int] = set()
        if asm_dir.is_dir():
            for path in sorted(asm_dir.rglob("*.s")):
                for name in GLABEL_RE.findall(path.read_text()):
                    m = VRAM_SUFFIX_RE.search(name)
                    if m and "func" in name:
                        entries.add(int(m.group(1), 16))
        if not entries:
            # Not split yet: the generated C's definitions are the entry set.
            for vram, name in funcs.items():
                if name.startswith(f"func_ovl{unit}_"):
                    entries.add(vram)
        result[unit] = entries
    return result


def region_table() -> list[dict]:
    """All regions: main overlays + each bank unit's code sections."""
    regions = []
    for name, (lo, hi) in main_section_ranges().items():
        regions.append({"kind": "main", "owner": f"main:{name}", "name": name, "lo": lo, "hi": hi})
    for unit, cfg in bank_unit_configs().items():
        for lo, hi in cfg["ranges"]:
            regions.append(
                {
                    "kind": "bank",
                    "owner": f"bank:{unit}",
                    "name": f"{unit}:{lo:08X}",
                    "unit": unit,
                    "lo": lo,
                    "hi": hi,
                }
            )
    return regions


def overloaded(regions: list[dict]) -> list[dict]:
    """The ranges where at least two regions (of different owners) can live."""
    out = []
    for r in regions:
        competitors = [
            o
            for o in regions
            if o is not r and o["owner"] != r["owner"] and o["lo"] < r["hi"] and r["lo"] < o["hi"]
        ]
        if not competitors:
            continue
        lo = max(r["lo"], max(c["lo"] for c in competitors))
        hi = min(r["hi"], min(c["hi"] for c in competitors))
        if lo < hi:
            out.append({"region": r, "lo": lo, "hi": hi, "competitors": competitors})
    return out


def swappable_ranges(regions: list[dict]) -> list[tuple[int, int]]:
    """Merged [lo, hi) ranges in which the resident code is ambiguous."""
    spans = sorted((o["lo"], o["hi"]) for o in overloaded(regions))
    merged: list[list[int]] = []
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return [(lo, hi) for lo, hi in merged]


def in_ranges(addr: int, ranges) -> bool:
    return any(lo <= addr < hi for lo, hi in ranges)


# ---------------------------------------------------------------------------
# Main unit C analysis
# ---------------------------------------------------------------------------

def main_functions() -> dict[int, str]:
    """vram -> function name from the main unit's generated C."""
    if not RECOMP_DIR.is_dir():
        raise ToolError(f"{RECOMP_DIR.relative_to(ROOT)} missing (run `make recomp`)")
    funcs: dict[int, str] = {}
    for path in sorted(RECOMP_DIR.glob("*.c")):
        cur: str | None = None
        for line in path.read_text().splitlines():
            m = FUNC_DEF_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            if cur is not None:
                t = TRACE_RE.search(line)
                if t:
                    funcs.setdefault(int(t.group(1), 16), cur)
                    cur = None
    if not funcs:
        raise ToolError("no function definitions found in RecompiledFuncs/*.c")
    return funcs


# ---------------------------------------------------------------------------
# write-seeds
# ---------------------------------------------------------------------------

def seed_names(unit: str, spec: dict, addr: int) -> str:
    """The unit's namespaced function name for a seed.

    splat prepends `func_` to a function symbol's generated name (the config's
    `symbol_name_format` only supplies the suffix), so `ovlA_$VRAM` becomes
    `func_ovlA_80197B90` — the same name the unit's own disassembly uses, which
    keeps the seed a pure rename of an existing symbol.
    """
    if "$VRAM" in spec["name_format"]:
        return "func_" + spec["name_format"].replace("$VRAM", f"{addr:08X}")
    return f"func_{addr:08X}"


def entry_looks_real(unit: str, addr: int) -> bool:
    """Whether the unit's code at `addr` opens like a function.

    A MIPS function starts with a stack frame (`addiu $sp, $sp, -N`) or a jump
    table dispatch (`lui`+`lw`+`jr`) or falls straight through to `jr $ra`. A
    body interior usually does not. `write-seeds` uses this to refuse an address
    that would split an existing body — that split makes N64Recomp emit a
    fragment whose `goto`s reference the sibling's labels and the app does not
    compile (`use of undeclared label 'after_4'`).
    """
    path = ROOT / f"build/bank{unit}/asm"
    if not path.is_dir():
        return True
    needle = f"{addr:08X}"
    for asm in sorted(path.rglob("*.s")):
        lines = asm.read_text().splitlines()
        for i, line in enumerate(lines):
            if "glabel" not in line and "alabel" not in line:
                continue
            if not line.rstrip().endswith(needle):
                continue
            # The next real instruction line is a `/* rom vram word */ insn`.
            for nxt in lines[i + 1:i + 12]:
                m = re.search(r"\*/\s+(\S+)", nxt)
                if m is None:
                    continue
                op = m.group(1)
                if op in ("addiu", "daddiu", "lui", "jr", "j", "sw", "sd"):
                    return True
                return False
    return True


def cross_bank_calls(swap_ranges, groups) -> dict[int, list[str]]:
    """Primary vram -> the `jal` target addresses that must dispatch at run time.

    The *source* address of the `jal` (from the generated comment) is what gets
    dispatched, not the callee name: N64Recomp compiles a `jal` to a known
    function as a direct call, and for a size-overridden target it compiles the
    call as the containing body instead (`jal 0x80198D28` becomes
    `func_801989AC`). The `jal` address is therefore the true target, and it is
    also the only address that is guaranteed to be an entry point in the bank
    that is resident when the call runs.
    """
    targets: dict[int, list[str]] = {}
    for path in sorted(RECOMP_DIR.glob("*.c")):
        cur_addr = None
        jal_target = None
        for line in path.read_text().splitlines():
            m = FUNC_DEF_RE.match(line)
            if m:
                cur_addr, jal_target = None, None
                continue
            if cur_addr is None:
                t = TRACE_RE.search(line)
                if t:
                    cur_addr = int(t.group(1), 16)
                    continue
            j = JAL_COMMENT_RE.match(line)
            if j is not None:
                jal_target = int(j.group(2), 16)
                continue
            if CALL_RE.match(line) is None or jal_target is None or cur_addr is None:
                continue
            if not in_ranges(jal_target, swap_ranges):
                jal_target = None
                continue
            if any(lo <= jal_target < hi and lo <= cur_addr < hi for lo, hi in groups):
                # The caller and the callee can be resident together.
                jal_target = None
                continue
            targets.setdefault(jal_target, []).append(path.name)
            jal_target = None
    return targets


def compute_seeds(regions, swap_ranges, clean_only: bool = True):
    """unit -> {vram: seed name} for the entry points that unit must expose.

    An address is seeded into every unit whose RAM covers it, so whichever
    region is resident can resolve a call to it:

    * every function the unit's own disassembly found in RAM another overlay or
      bank can occupy (a pure rename — the unit already has the entry), and
    * every `jal` target of a cross-bank call. The target is a function the game
      calls directly, so the address really is an entry point in the layouts
      that contain the caller; forcing splat to treat it as one gives the
      recompiler a real function there instead of a body interior.
    """
    entries = bank_function_entries()
    clean_entries = bank_function_entries()
    specs = bank_unit_configs()
    targets = set(cross_bank_calls(swap_ranges, [(o["lo"], o["hi"]) for o in overloaded(regions)]))
    seeds: dict[str, dict[int, str]] = {u: {} for u in specs}
    rejected: list[tuple[str, int]] = []

    for unit, spec in specs.items():
        # Only entry points the unit's *clean* disassembly (no seeds applied)
        # already has. Forcing splat to split an existing body at a new address
        # makes the recompiler emit the fragment with `goto`s whose labels live
        # in the sibling fragment ("use of undeclared label 'after_4'"), so a
        # target that is a body interior in this unit's layout is left to the
        # direct-call path instead.
        candidates = set(clean_entries.get(unit, set()))
        if not clean_only:
            candidates |= set(entries.get(unit, set()))
        for addr in sorted(candidates):
            if not in_ranges(addr, swap_ranges):
                continue
            if not any(lo <= addr < hi for lo, hi in spec["ranges"]):
                continue
            if not entry_looks_real(unit, addr):
                rejected.append((unit, addr))
                continue
            seeds[unit][addr] = seed_names(unit, spec, addr)
    if rejected:
        print(f"cross_bank: {len(rejected)} address(es) refused: not a function prologue here")
        for unit, addr in rejected[:20]:
            print(f"cross_bank:   unit {unit} 0x{addr:08X}")
    return seeds


def write_seeds(regions, swap_ranges) -> int:
    seeds = compute_seeds(regions, swap_ranges)
    total = 0
    for unit in sorted(seeds):
        out = ROOT / f"build/bank{unit}/symbol_addrs.txt"
        out.parent.mkdir(parents=True, exist_ok=True)
        entries = sorted(seeds[unit].items())
        with out.open("w") as f:
            f.write(
                "// GENERATED by tools/cross_bank.py write-seeds.\n"
                "// Function entries of this unit that sit in RAM another overlay or\n"
                "// bank can also occupy, so a cross-bank `jal` target is an entry point\n"
                "// here too and get_function() resolves it whichever bank is resident.\n"
                "// See docs/DECISIONS.md (session 34).\n"
            )
            for vram, name in entries:
                f.write(f"{name} = 0x{vram:08X};\n")
        total += len(entries)
        print(f"cross_bank: unit {unit}: {len(entries)} seed symbol(s) -> {out.relative_to(ROOT)}")
    return total


# ---------------------------------------------------------------------------
# rewrite
# ---------------------------------------------------------------------------

def defined_functions() -> set[str]:
    """Function names the main unit defines (a direct call needs the body)."""
    names: set[str] = set()
    for path in sorted(RECOMP_DIR.glob("*.c")):
        for line in path.read_text().splitlines():
            m = FUNC_DEF_RE.match(line)
            if m:
                names.add(m.group(1))
    return names


FIRST_OP_RE = re.compile(r"^\s*// 0x[0-9A-Fa-f]{8}:\s+(\w+)")

# A main-unit definition is only a revertible direct-call target when it opens
# like a function (same prologue set as `entry_looks_real`). A same-address
# fragment (`func_80198D28`, `func_801AB740`: first op `and`/`andi`) is never
# what the recompiler originally emitted for the `jal`, so restoring a direct
# call to it would invent a third binding — neither the original nor the
# dispatch. Those lookups are left in place (see the size-override note in
# `rewrite_main`); `make recomp` regenerates the pristine call instead.
ENTRY_OPS = {"addiu", "daddiu", "lui", "jr", "j", "sw", "sd"}


def def_first_ops() -> dict[str, str]:
    """Defined function name -> its first MIPS op (fragment detection)."""
    ops: dict[str, str] = {}
    for path in sorted(RECOMP_DIR.glob("*.c")):
        cur: str | None = None
        for line in path.read_text().splitlines():
            m = FUNC_DEF_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            if cur is not None:
                op = FIRST_OP_RE.match(line)
                if op is not None:
                    ops.setdefault(cur, op.group(1))
                    cur = None
    return ops


def revert_main() -> int:
    """Undo `rewrite` by turning lookup calls back into their direct calls.

    A lookup is always emitted immediately after the `jal` comment it came from,
    so a lookup whose address differs from that comment was not a call in the
    original code (see the size-override note in `rewrite_main`) and is left
    alone. The restored direct call names the address's function; `get_function`
    and a direct call reach the same body whenever the address is registered,
    which is the only case the rewrite creates. A same-address fragment
    definition is not a valid restore target and is left dispatched.
    """
    defined = defined_functions()
    first_ops = def_first_ops()
    reverted = 0
    skipped: list[int] = []
    changed = 0
    for path in sorted(RECOMP_DIR.glob("*.c")):
        lines = path.read_text().splitlines(keepends=True)
        jal_target = None
        out = []
        dirty = False
        for line in lines:
            j = JAL_COMMENT_RE.match(line)
            if j is not None:
                jal_target = int(j.group(2), 16)
            m = LOOKUP_RE.match(line)
            if (m is not None and jal_target is not None
                    and int(m.group(1), 16) == jal_target
                    and f"func_{m.group(1)}" in defined):
                if first_ops.get(f"func_{m.group(1)}") in ENTRY_OPS:
                    out.append(f"    func_{m.group(1)}(rdram, ctx);\n")
                    reverted += 1
                    dirty = True
                    jal_target = None
                    continue
                skipped.append(jal_target)
                jal_target = None
            out.append(line)
        if dirty:
            path.write_text("".join(out))
            changed += 1
    print(f"cross_bank: revert: {reverted} lookup call(s) in {changed} file(s) restored")
    for addr in sorted(set(skipped)):
        print(f"cross_bank: revert: 0x{addr:08X} kept dispatched (same-address fragment, not a revertible entry)")
    return reverted


def rewrite_main(regions, only: set[int] | None = None) -> int:
    """Emit a runtime dispatch for every cross-bank `jal` in the main unit.

    A call is left alone when the caller and the callee can be resident
    together: addresses in the same overlapped group belong to the same resident
    bank (a bank's records are loaded and evicted as one), so the caller's own
    section keeps those bindings correct. Every other `jal` into a swappable
    range is bound by the recompiler to whatever the main ELF has there and must
    dispatch at run time.

    The dispatched address is the `jal`'s own target (`LOOKUP_FUNC` on the
    comment's address), and only targets some bank unit registers are rewritten;
    the rest keep their direct call. That gate is what keeps the rewrite from
    replacing a working binding with a lookup that would miss the function map.

    `only` restricts the rewrite to an explicit target set (single-target
    dispatch): an `--only` address with no bank entry is a hard error instead of
    a kept binding, so a targeted fix can never silently stay on the crashing
    direct call. Targets outside the set are left alone.
    """
    swap_ranges = swappable_ranges(regions)
    groups = [(o["lo"], o["hi"]) for o in overloaded(regions)]

    # Addresses a bank unit registers in its function table. `make bank-recomp`
    # writes exactly this set back into `bank_function_entries()`, so it grows as
    # the seeds are applied — see the build order in docs/guides/app-build.md.
    resolvable: set[int] = set()
    for _unit, entry_set in bank_function_entries().items():
        resolvable |= entry_set

    if only and not resolvable:
        # Fresh checkout that never ran the bank targets: no bank data at all,
        # so there is nothing to dispatch to. Warn and leave the recompiler's
        # bindings alone instead of failing the build.
        print(
            "cross_bank: rewrite: no bank entries registered yet "
            "(`make bank-recomp` not run); leaving --only target(s) undispatched"
        )
        return None
    if only:
        missing = sorted(a for a in only if a not in resolvable)
        if missing:
            raise ToolError(
                "target(s) have no bank entry: "
                + ", ".join(f"0x{a:08X}" for a in missing)
                + " (re-run after `make bank-recomp`)"
            )

    changed_files = 0
    replaced = 0
    repaired = 0
    unresolved: dict[int, int] = {}
    per_target: dict[int, int] = {}
    already: dict[int, int] = {}

    for path in sorted(RECOMP_DIR.glob("*.c")):
        lines = path.read_text().splitlines(keepends=True)
        # Labels are function-scoped, so a file-wide used set can only
        # over-approximate: the fresh label can never collide in its function.
        file_labels = set()
        for text in lines:
            lm = AFTER_LABEL_RE.match(text)
            if lm is not None:
                file_labels.add(int(lm.group(1)))
        cur_name = None
        cur_addr = None
        jal_target = None
        jal_line = -1
        out_lines = []
        dirty = False
        i = 0
        while i < len(lines):
            line = lines[i]
            m = FUNC_DEF_RE.match(line)
            if m:
                cur_name, cur_addr = m.group(1), None
                jal_target = None
                out_lines.append(line)
                i += 1
                continue
            lk = LOOKUP_RE.match(line)
            if lk is not None and jal_target is not None and int(lk.group(1), 16) == jal_target:
                # The `jal`'s call is already a lookup (a previous dispatch, or
                # the recompiler's own dynamic call): consume the `jal` so a
                # later direct call cannot steal it, and record the target as
                # dispatched for the `--only` check.
                already[jal_target] = already.get(jal_target, 0) + 1
                jal_target = None
                out_lines.append(line)
                i += 1
                continue
            if cur_name is not None and cur_addr is None:
                t = TRACE_RE.search(line)
                if t:
                    cur_addr = int(t.group(1), 16)
            j = JAL_COMMENT_RE.match(line)
            if j is not None:
                jal_target = int(j.group(2), 16)
                jal_line = len(out_lines)
            call = CALL_RE.match(line) if (jal_target is not None and cur_addr is not None) else None
            if call is None:
                out_lines.append(line)
                i += 1
                continue
            caller_inside = any(
                lo <= jal_target < hi and lo <= cur_addr < hi for lo, hi in groups
            )
            if not (in_ranges(jal_target, swap_ranges) and not caller_inside):
                out_lines.append(line)
                jal_target = None
                i += 1
                continue
            if only is not None and jal_target not in only:
                out_lines.append(line)
                jal_target = None
                i += 1
                continue
            indent = call.group(1)
            if jal_target not in resolvable:
                # No bank entry: keep the binding the recompiler chose.
                out_lines.append(line)
                unresolved[jal_target] = unresolved.get(jal_target, 0) + 1
                dirty = True
                jal_target = None
                i += 1
                continue
            out_lines.append(f"{indent}LOOKUP_FUNC(0x{jal_target:08X})(rdram, ctx);\n")
            replaced += 1
            per_target[jal_target] = per_target.get(jal_target, 0) + 1
            dirty = True
            # A `jal` to a size-overridden (body-interior) target is emitted by
            # the recompiler as a call to the containing body followed by an
            # early `return`, abandoning the caller's epilogue — on the hardware
            # the callee returns to the instruction after the delay slot. When
            # that shape is present (early return + the delay-slot emission
            # duplicated after it), repair it to the standard call-and-continue
            # shape; a lookup that misses the map is a no-op stub, so falling
            # through is safe either way. A true tail call at the function's end
            # has no duplicated delay slot and keeps its `return`.
            if (i + 2 < len(lines) and TRACE_RETURN_RE.match(lines[i + 1]) is not None
                    and RETURN_RE.match(lines[i + 2]) is not None):
                delay = out_lines[jal_line + 1 : -1]
                dup = lines[i + 3 : i + 3 + len(delay)]
                if dup == delay and delay:
                    fresh = 0
                    while fresh in file_labels:
                        fresh += 1
                    file_labels.add(fresh)
                    out_lines.append(f"{indent}    goto after_{fresh};\n")
                    out_lines.extend(dup)
                    out_lines.append(f"{indent}after_{fresh}:\n")
                    i += 3 + len(delay)
                    repaired += 1
                    jal_target = None
                    continue
            jal_target = None
            i += 1
        if dirty:
            path.write_text("".join(out_lines))
            changed_files += 1

    print(
        f"cross_bank: rewrite: {replaced} cross-bank call(s) in {changed_files} file(s) "
        f"-> LOOKUP_FUNC"
    )
    print(
        f"cross_bank: rewrite: {len(per_target)} distinct target(s) dispatched"
    )
    if repaired:
        print(
            f"cross_bank: rewrite: {repaired} tail-call site(s) repaired to "
            f"call-and-continue"
        )
    if unresolved:
        print(
            f"cross_bank: rewrite: {len(unresolved)} target(s) have no bank entry yet "
            f"(kept as direct calls; re-run after `make bank-recomp` to dispatch them)"
        )
        for addr in sorted(unresolved):
            print(f"cross_bank:   pending 0x{addr:08X} ({unresolved[addr]} call site(s))")
    if only:
        for addr in sorted(only):
            if addr in per_target:
                continue
            if addr in already:
                print(f"cross_bank: rewrite: 0x{addr:08X} already dispatched ({already[addr]} site(s))")
    return per_target, already


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

def report(regions, swap_ranges) -> None:
    print("cross_bank: swappable RAM ranges (merged):")
    for lo, hi in swap_ranges:
        print(f"  0x{lo:08X}..0x{hi:08X}  (0x{hi - lo:X} bytes)")
    print("cross_bank: region overlaps:")
    for o in sorted(overloaded(regions), key=lambda o: (o["lo"], o["region"]["name"])):
        names = ",".join(sorted({c["name"] for c in o["competitors"]}))
        print(
            f"  {o['region']['name']:14s} 0x{o['lo']:08X}..0x{o['hi']:08X} overlaps {names}"
        )
    seeds = compute_seeds(regions, swap_ranges)
    entries = bank_function_entries()
    for unit in sorted(seeds):
        print(
            f"cross_bank: unit {unit}: {len(entries[unit])} entr(y/ies), "
            f"{len(seeds[unit])} seed candidate(s)"
        )
    funcs = main_functions()
    print(
        "cross_bank: main-unit function entries in swappable ranges: "
        f"{sum(1 for a in funcs if in_ranges(a, swap_ranges))}"
    )


def parse_only(argv: list[str]) -> set[int] | None:
    """`--only ADDR[,ADDR...]` (repeatable): restrict `dispatch` to targets."""
    only: set[int] = set()
    args = argv
    while args:
        flag, args = args[0], args[1:]
        if flag != "--only" or not args:
            raise ToolError(f"usage: cross_bank.py dispatch [--only 0xADDR[,0xADDR...]]")
        addrs, args = args[0], args[1:]
        for part in addrs.split(","):
            try:
                only.add(int(part.strip(), 16))
            except ValueError:
                raise ToolError(f"bad address '{part.strip()}' (want hex like 0x80198D28)")
    return only or None


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in ("write-seeds", "dispatch", "revert", "report"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd = sys.argv[1]
    try:
        regions = region_table()
        swap_ranges = swappable_ranges(regions)
        if not swap_ranges:
            raise ToolError("no swappable RAM ranges; check the region model")
        if cmd == "write-seeds":
            if len(sys.argv) != 2:
                raise ToolError("usage: cross_bank.py write-seeds")
            n = write_seeds(regions, swap_ranges)
            print(f"cross_bank: wrote {n} seed symbol(s)")
        elif cmd == "dispatch":
            only = parse_only(sys.argv[2:])
            result = rewrite_main(regions, only)
            if result is None:
                return 0
            per_target, already = result
            if only:
                missing = sorted(a for a in only if a not in per_target and a not in already)
                if missing:
                    raise ToolError(
                        "target(s) matched no call site: "
                        + ", ".join(f"0x{a:08X}" for a in missing)
                    )
        elif cmd == "revert":
            if len(sys.argv) != 2:
                raise ToolError("usage: cross_bank.py revert")
            revert_main()
        else:
            if len(sys.argv) != 2:
                raise ToolError("usage: cross_bank.py report")
            report(regions, swap_ranges)
    except ToolError as exc:
        print(f"cross_bank: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
