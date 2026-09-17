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
    tools/cross_bank.py check           # FAIL if any unit still calls directly
                                        # into a RAM range another bank can own

`check` is the build-time guard for the class session 45 lost five sessions to.
A direct C call into a *swappable* range is bound at build time to one bank's
layout; when the other bank is resident it runs that other module's bodies with
the wrong frame and register contract (`bankRec14b`'s prologue-less tail at
`0x80239C24` vs `bankRec14c`'s real function there). The rule is: a swappable
range must not be *defined* in the unit whose code calls into it — give each
bank its own unit and let N64Recomp emit `LOOKUP_FUNC`. Calls *within* one
record are fine (that bank is resident whenever the caller runs).
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
# Scene record masks
# ---------------------------------------------------------------------------
#
# A direct call from one record into another record of the *same* unit is only
# wrong when the game can have the caller resident while the target record is
# not: the target RAM then holds a different bank and the call runs the wrong
# body. Whether two records can be split is a property of the scene descriptors'
# `+0x10` record-mask words (bit N = segment-table record N), which the game's
# loader uses to decide what to DMA — so the check below reads them instead of
# flagging every same-unit cross-record call (1728 of those, most of them safe
# because the records are always loaded together).
#
# This table is that ROM data, from `tools/scenemap.py scenes`: every scene
# descriptor's mask (both branches of the indirect accessors, so scenes 3 and 6
# appear twice). It is static ROM data; `tools/scenemap.py` regenerates it.
SCENE_DESCRIPTOR_MASKS: dict[int, int] = {
    0x800A872C: 0x0000000C,  # scene 0 (boot)
    0x8018FB04: 0x00000001,  # 1
    0x8018FB40: 0x00008000,  # 4 (title)
    0x8018FD70: 0x00000002,  # 5 (map)
    0x8018FD84: 0x00000002,  # 6 branch A
    0x8018FD98: 0x00040002,  # 6 branch B
    0x8018FDAC: 0x00000002,  # 7 (name-entry form)
    0x8018FB70: 0x00008000,  # 9
    0x8018FB84: 0x00008000,  # 10 (publishers)
    0x8018F380: 0x0000004C,  # 11 (attract story)
    0x8018FB58: 0x00003C00,  # 12
    0x8018FC3C: 0x40007C14,  # 13 (0x0D, the step engine)
    0x8018FB2C: 0x00003C00,  # 14
    0x8018F3A0: 0x0000000C,  # 15
    0x8018FB98: 0x00008000,  # 16 (closing movie)
    0x8018FBAC: 0x00008000,  # 17 (tutorial)
    0x8018FBC0: 0x00008000,  # 18
    0x8018FBD4: 0x00008000,  # 19
    0x8018FBE8: 0x00013C14,  # 20
    0x8018FB18: 0x00000001,  # 21
    0x8018FC00: 0x00000400,  # 22 (0x16)
    0x8018FC64: 0x40004000,  # 8
    0x8018F350: 0x0000038C,  # 3 branch A (the mission)
    0x8018F364: 0x0004038C,  # 3 branch B
    0x8018FDC0: 0x00000002,  # 24
}

RECORD_NAME_RE = re.compile(r"bankRec(\d+)")


def scene_record_sets() -> list[frozenset[int]]:
    """The record set each scene loads, from the descriptor mask words."""
    return [frozenset(n for n in range(32) if (mask >> n) & 1)
            for mask in SCENE_DESCRIPTOR_MASKS.values()]


def record_number(name: str | None) -> int | None:
    """`bankRec14b` -> 14, `bankRec3` -> 3; None when the name is not a record."""
    m = RECORD_NAME_RE.match(name or "")
    return int(m.group(1)) if m else None


def records_always_co_loaded(caller_rec: str | None, target_rec: str | None,
                             scene_sets: list[frozenset[int]]) -> bool:
    """True when no scene loads `caller_rec` without `target_rec`.

    A call whose records cannot be identified is reported (returns False): the
    check must fail safe, not silently skip an unknown layout.
    """
    caller_n = record_number(caller_rec)
    target_n = record_number(target_rec)
    if caller_n is None or target_n is None:
        return False
    return not any(caller_n in s and target_n not in s for s in scene_sets)


# Accepted, pre-existing hazards. The mask-aware check (session 67) is the first
# version that could see this class at all — `parse_yaml_segments` dropped the
# `name:` on `- name: bankRecX` lines before, so every record was named "?" and
# the same-record test was vacuously true. Turning it on exposes a backlog in
# unit C that predates this session and has not been observed to misbehave (its
# records are loaded together by the scenes that use them); the mission wall
# (`unit A rec3 -> rec6`) was fixed by moving record 6 to unit O.
#
# Each line is `<caller vram> <target vram>` — the pair `check` would otherwise
# fail on. Anything not listed is a hard failure, so a *new* violation still
# stops `make bank-recomp`. Removing entries as the unit-C records are split
# into non-calling units is the cleanup path (see config-bankF.yaml).
KNOWN_HAZARDS_PATH = ROOT / "tools" / "cross_bank_known_hazards.txt"


def known_hazards() -> set[tuple[int, int]]:
    if not KNOWN_HAZARDS_PATH.exists():
        return set()
    out: set[tuple[int, int]] = set()
    for line in KNOWN_HAZARDS_PATH.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            out.add((int(parts[0], 16), int(parts[1], 16)))
    return out


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
            # A segment can start on the item line itself (`- name: bankRec3`,
            # `- type: bin`). Without this the name is dropped, every record is
            # named "?" and `check`'s same-record test becomes vacuously true --
            # which is exactly how unit A's call from record 3 into record 6's
            # RAM (the mission wall, session 67) went unreported.
            stripped = rest
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


def bank_regions() -> list[dict]:
    """Each bank unit's code sections. Needs only the configs, not the map."""
    regions = []
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


def region_table() -> list[dict]:
    """All regions: main overlays + each bank unit's code sections."""
    regions = []
    for name, (lo, hi) in main_section_ranges().items():
        regions.append({"kind": "main", "owner": f"main:{name}", "name": name, "lo": lo, "hi": hi})
    regions.extend(bank_regions())
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
        # The overlap extends to the highest competitor end, not the lowest:
        # `r` overlaps every competitor listed, so the ambiguous span runs from
        # the latest start to the earliest *of the ends that can still cover
        # that start*. Taking min(c["hi"]) under-reports whenever the region's
        # own end is the largest (session 55: unit H's module, 0x8019A7C0..
        # 0x801A2DC0, overlaps streamedC 0x80197B90..0x801BA550 and record 3
        # 0x8019EE70..0x801AD5C0, but the min end 0x8019EE50 truncated the span
        # to 0x8019F450 — so the module's own calls were invisible to the
        # rewrite). Clamp to `r`'s end so the span stays a sub-range of `r`.
        hi = min(r["hi"], max(c["hi"] for c in competitors))
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
    for addr in sorted(per_target):
        print(f"cross_bank:   dispatched 0x{addr:08X} ({per_target[addr]} site(s))")
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


# ---------------------------------------------------------------------------
# check: no direct call into a swappable range from outside its own record
# ---------------------------------------------------------------------------

STATIC_VRAM_RE = re.compile(r"^static_\d+_([0-9A-Fa-f]{8})$")


def symbol_vram(name: str) -> int | None:
    """The guest address a generated symbol name encodes, or None."""
    m = FUNC_VRAM_RE.match(name)
    if m:
        return int(m.group(1), 16)
    m = STATIC_VRAM_RE.match(name)
    if m:
        return int(m.group(1), 16)
    return None


def unit_records(unit: str) -> list[tuple[int, int, str]]:
    """(ram_lo, ram_hi, name) for one unit's code segments, from its config."""
    path = ROOT / f"config-bank{unit}.yaml"
    segments = parse_yaml_segments(path)
    out: list[tuple[int, int, str]] = []
    for i, seg in enumerate(segments):
        if seg.get("type") != "code" or "vram" not in seg or "start" not in seg:
            continue
        if i + 1 >= len(segments):
            continue
        out.append((seg["vram"], seg["vram"] + (segments[i + 1]["start"] - seg["start"]),
                    seg.get("name", "?")))
    return out


def direct_calls(path: Path) -> list[tuple[int, str, int, int]]:
    """(caller_vram, caller_name, target_vram, line_no) for direct calls."""
    out = []
    caller_vram: int | None = None
    caller_name = "?"
    pending = False
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        m = FUNC_DEF_RE.match(line)
        if m:
            caller_name, caller_vram, pending = m.group(1), None, True
            continue
        if pending:
            t = TRACE_RE.search(line)
            if t:
                caller_vram = int(t.group(1), 16)
                pending = False
        if LOOKUP_RE.match(line):
            continue
        m = CALL_RE.match(line)
        if m and caller_vram is not None:
            target = symbol_vram(m.group(2))
            if target is not None:
                out.append((caller_vram, caller_name, target, line_no))
    return out


def record_of(addr: int, records: list[tuple[int, int, str]]) -> str | None:
    for lo, hi, name in records:
        if lo <= addr < hi:
            return name
    return None


def check(regions, swap_ranges, strict_main: bool = False) -> int:
    """Assert no unit calls directly into a swappable range it does not own.

    Bank units are a hard failure: a unit must never define a swappable range
    *and* call into it from another of its records (session 45's wall). The main
    unit's overlay code has a long-standing backlog of the same shape — it cannot
    know a bank exists, so `dispatch` rewrites the targets that matter and the
    rest stay on the recompiler's bindings (sessions 33-41) — so those are
    reported but only fail with `--strict`.
    """
    bank_bad: list[str] = []
    bank_known: list[str] = []
    main_bad: list[tuple[int, str]] = []
    bank_total = 0
    scene_sets = scene_record_sets()
    known = known_hazards()

    for unit_dir in sorted(ROOT.glob("Bank*Funcs")):
        if not unit_dir.is_dir():
            continue
        unit = unit_dir.name[len("Bank"):-len("Funcs")]
        if not (ROOT / f"config-bank{unit}.yaml").exists():
            continue
        records = unit_records(unit)
        for path in sorted(unit_dir.glob("*.c")):
            for caller, caller_name, target, line_no in direct_calls(path):
                if not in_ranges(target, swap_ranges):
                    continue
                bank_total += 1
                caller_rec = record_of(caller, records)
                target_rec = record_of(target, records)
                if caller_rec == target_rec:
                    continue  # same bank: resident whenever the caller runs
                # A different record of the same unit is only a hazard when a
                # scene can load the caller without the target (see
                # scene_record_sets); otherwise both are resident together.
                if target_rec is not None and records_always_co_loaded(
                        caller_rec, target_rec, scene_sets):
                    continue
                line = (f"{path.relative_to(ROOT)}:{line_no}: {caller_name} (0x{caller:08X}) calls "
                        f"0x{target:08X} directly, but that RAM is swappable")
                if (caller, target) in known:
                    bank_known.append(line)
                else:
                    bank_bad.append(line)

    for path in sorted(RECOMP_DIR.glob("*.c")):
        for caller, caller_name, target, line_no in direct_calls(path):
            if in_ranges(target, swap_ranges):
                main_bad.append((target, f"{path.relative_to(ROOT)}:{line_no}: {caller_name} "
                                         f"(0x{caller:08X}) calls 0x{target:08X} directly"))

    for line in bank_bad[:60]:
        print(f"cross_bank:   {line}")
    if bank_known:
        print(f"cross_bank: bank units: {len(bank_known)} accepted pre-existing hazard call(s) "
              f"(listed in {KNOWN_HAZARDS_PATH.relative_to(ROOT)})")
    print(f"cross_bank: bank units: {bank_total} direct call(s) into swappable RAM, "
          f"{len(bank_bad)} outside their own record")
    if main_bad:
        targets = sorted({t for t, _ in main_bad})
        print(f"cross_bank: main unit: {len(main_bad)} direct call(s) into swappable RAM across "
              f"{len(targets)} target(s) (known backlog; `dispatch --only` fixes them one by one)")
        for t in targets[:12]:
            print(f"cross_bank:   0x{t:08X}  ({sum(1 for u, _ in main_bad if u == t)} site(s))")
        if len(targets) > 12:
            print(f"cross_bank:   ... and {len(targets) - 12} more")

    if bank_bad:
        print("cross_bank: check FAILED - a unit defines a swappable range and calls into it.")
        print("cross_bank:   fix: move the target record into a unit that does not call it, so")
        print("cross_bank:   N64Recomp emits LOOKUP_FUNC (see config-bankF.yaml / session 45).")
        return 1
    if main_bad and strict_main:
        print("cross_bank: check FAILED (--strict): main-unit calls into swappable RAM remain.")
        return 1
    print("cross_bank: check OK - no bank unit calls into a swappable range it does not own")
    return 0


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
    if len(sys.argv) < 2 or sys.argv[1] not in (
            "write-seeds", "dispatch", "revert", "report", "check", "check-banks"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd = sys.argv[1]
    try:
        if cmd == "check-banks":
            regions = bank_regions()
        else:
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
        elif cmd == "check":
            args = sys.argv[2:]
            strict = "--strict" in args
            args = [a for a in args if a != "--strict"]
            if args:
                raise ToolError("usage: cross_bank.py check [--strict]")
            return check(regions, swap_ranges, strict)
        elif cmd == "check-banks":
            # `check` without the linker map: used by `make bank-recomp`, which
            # may run before the main ELF has ever been linked.
            if len(sys.argv) != 2:
                raise ToolError("usage: cross_bank.py check-banks")
            return check(regions, swap_ranges, False)
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
