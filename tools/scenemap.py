#!/usr/bin/env python3
"""The game's scene registry, its transitions, and the scripted step table.

This answers the question *"do we have to link the scenes by hand?"* with
numbers, and it regenerates the tables earlier sessions found one address at a
time. It reads only the ROM and the build trees — no run needed.

Three layers, and only the first two are code:

  1. **The registry** — `func_80075BC0` (the scene manager) writes **25** scene
     descriptors' accessor pointers into `D_800AF028[0..24]` from an immediate
     list. Each accessor is a 1-3 instruction stub returning a descriptor
     address; the descriptor is 5 words: `+0x00` enter, `+0x04` update,
     `+0x08` hook, `+0x0C` leave, `+0x10` bank-record mask. All of it is static
     ROM data (`streamedB`, ROM `0x40E80` + (vram - `0x8016AF80`)).
  2. **The transitions** — every store into `D_800C4C26` (the pending scene
     word) in the whole game: **39 sites in 30 functions**, of which 21 store a
     statically-known value. That is the *entire* graph of scene-to-scene code.
     The rest are script-driven (the scene-script VM writes `D_8018F1C2` and the
     transition handler copies it), which is why the count stays this small.
  3. **The content** — the scripted step table is asset `0x19A8804` (ROM
     `0x1F3CA54`): a `u32` payload size then **1693** `u32` per-step asset ids.
     The 200+ dialogues are *steps in this table*, not scene types.

Verified against the docs (session 64): id 5 -> descriptor `0x8018FD70`, enter
`0x8017B60C`, mask `0x2` (the map); id 7 -> `0x8018FDAC`, enter `0x8017B794`
(the name-entry form); id 13 (`0x0D`) -> `0x8018FC3C`, mask `0x40007C14`.

Usage:
    tools/scenemap.py                 # all three tables
    tools/scenemap.py scenes          # id -> accessor -> descriptor -> 5 words
    tools/scenemap.py transitions     # every writer of the pending scene word
    tools/scenemap.py steps           # the scripted step table summary
    tools/scenemap.py scenes --dump /tmp/map3.bin   # read descriptors live

Cross-check: `tools/guestmap.py <descriptor>` maps a descriptor address to its
ROM offset and names the owning segment.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from n64map import load_map  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ROM = ROOT / "assets" / "ogre64.z64"

SCENE_MANAGER = 0x80075BC0          # builds D_800AF028 at boot
SCENE_REGISTRY = 0x800AF028
PENDING_SCENE = 0x800C4C26
STEP_TABLE_ASSET = 0x19A8804        # ROM 0x1F3CA54
SCENE_COUNT = 25

ELF_MAIN = ROOT / "build" / "ogrebattle64.elf"
ELF_BANKS = [ROOT / ("build/bank%s.elf" % c) for c in "ABCDEFGHIJKLM"]

INSN = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$")
FUNC = re.compile(r"^([0-9a-f]{8}) <(.+)>:")


def signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def disassemble(elf: Path) -> str:
    if not elf.exists():
        return ""
    return subprocess.run(["mips-linux-gnu-objdump", "-d", str(elf)],
                          capture_output=True, text=True).stdout


def parse(text: str):
    """Yield (pc, op, args, func) for every instruction in an objdump dump."""
    func = "?"
    for line in text.splitlines():
        mf = FUNC.match(line)
        if mf:
            func = mf.group(2)
            continue
        m = INSN.match(line)
        if m:
            yield int(m.group(1), 16), m.group(3), m.group(4), func


# --- layer 1: the registry ---------------------------------------------------

def scene_registry(main: str) -> dict[int, int]:
    """scene id -> accessor address, read out of the scene manager's store list."""
    out: dict[int, int] = {}
    hi = lo = None
    at = None
    for pc, op, args, _ in parse(main):
        if not (SCENE_MANAGER <= pc < SCENE_MANAGER + 0x700):
            continue
        if op == "lui":
            reg, val = args.split(",")
            if reg.strip() == "at":
                at = int(val, 16) << 16
            if reg.strip() == "v0":
                hi, lo = int(val, 16) << 16, None
        elif op == "addiu" and args.startswith("v0,v0,"):
            lo = signed16(int(args.split(",")[2]) & 0xFFFF)
        elif op == "sw" and at is not None:
            m = re.match(r"v0,(-?\d+)\(at\)", args)
            if m and hi is not None and lo is not None:
                addr = at + signed16(int(m.group(1)) & 0xFFFF)
                idx = (addr - SCENE_REGISTRY) // 4
                if 0 <= idx < SCENE_COUNT:
                    out[idx] = hi + lo
    return out


def descriptors(main: str) -> dict[int, int | None]:
    """scene id -> descriptor address, resolved from each accessor's body.

    An accessor is `lui v0,HI; jr ra; addiu v0,v0,LO` — the `addiu` is the *delay
    slot* and sits after the `jr`, so a scan that stops at `jr` finds nothing.
    Read the whole 3-4 instruction window instead.
    """
    insns = sorted((pc, op, args) for pc, op, args, _ in parse(main))
    # An ELF can disassemble the same vram twice (overlays share RAM), so dedupe
    # by pc — otherwise a second copy's instructions overwrite the pair.
    dedup: list[tuple[int, str, str]] = []
    seen: set[int] = set()
    for pc, op, args in insns:
        if pc in seen:
            continue
        seen.add(pc)
        dedup.append((pc, op, args))
    insns = dedup
    out: dict[int, int | None] = {}
    for sid, acc in scene_registry(main).items():
        # Stop after the instruction following `jr` — that delay slot holds the
        # `addiu`. Reading a fixed window instead runs into the *next* accessor's
        # `lui v0,...`, which resets the low half and loses the pair.
        win: list[tuple[str, str]] = []
        stop = False
        for pc, op, args in insns:
            if pc < acc:
                continue
            if pc >= acc + 0x18:
                break
            win.append((op, args))
            if stop:
                break
            if op == "jr":
                stop = True
        hi = lo = None
        for op, args in win:
            if op == "lui" and args.startswith("v0,"):
                hi, lo = int(args.split(",")[1], 16) << 16, None
            elif op == "addiu" and args.startswith("v0,v0,") and hi is not None:
                lo = signed16(int(args.split(",")[2]) & 0xFFFF)
                break
        out[sid] = (hi + lo) if (hi is not None and lo is not None) else None
    return out


def read_words(rom: bytes, vram: int, count: int, dump: bytes | None = None):
    if dump is not None:
        base = vram & 0x1FFFFFFF
        # runtime byte order: logical word at a is LE at (a-0x80000000)
        return [int.from_bytes(bytes(dump[base + i * 4:base + i * 4 + 4]), "little")
                for i in range(count)]
    seg = load_map().vram_to_rom(vram)
    if seg is None or seg + count * 4 > len(rom):
        return None
    return [int.from_bytes(rom[seg + i * 4:seg + i * 4 + 4], "big") for i in range(count)]


def cmd_scenes(dump_path: str | None) -> None:
    rom = ROM.read_bytes()
    dump = Path(dump_path).read_bytes() if dump_path else None
    main = disassemble(ELF_MAIN)
    reg = scene_registry(main)
    desc = descriptors(main)
    if dump:
        print("# descriptors read from %s (live RDRAM)" % dump_path)
    else:
        print("# descriptors read from the ROM (streamedB)")
    print("# %d scene types.  mask = the bank records the scene needs resident."
          % len(reg))
    print()
    print(" id    accessor     descriptor   enter       update      hook        leave       mask")
    for sid in sorted(reg):
        d = desc.get(sid)
        if d is None:
            print(" %2d    0x%08X   (indirect accessor)" % (sid, reg[sid]))
            continue
        w = read_words(rom, d, 5, dump)
        if w is None:
            print(" %2d    0x%08X   0x%08X   (vram not mapped)" % (sid, reg[sid], d))
            continue
        print(" %2d    0x%08X   0x%08X   %s  0x%08X"
              % (sid, reg[sid], d, "  ".join("0x%08X" % x for x in w[:4]), w[4]))
    print()
    print("Note: ids >= 0x1F map to index 0 (the scene manager clamps), and the\n"
          "descriptor's `update`/`hook` for the map and the form are the SAME\n"
          "function pair (0x8017B858 / 0x8017B9C8) — they differ only in `enter`.")


# --- layer 2: transitions ----------------------------------------------------

def cmd_transitions() -> None:
    elfs = [ELF_MAIN] + ELF_BANKS
    hi_reg = PENDING_SCENE & 0xFFFF0000
    lo_off = PENDING_SCENE & 0xFFFF
    total = static = 0
    print("# every store into D_800C4C26 (the pending scene word) in the game.")
    print("# A static value of 0xFFFC/0xFFFD/0xFFFE is a *control* word, not a scene id.")
    print()
    print(" %-18s %-10s %-24s %s" % ("unit", "address", "function", "value"))
    for elf in elfs:
        text = disassemble(elf)
        if not text:
            continue
        lui: dict[str, int] = {}
        hist: list[tuple[str, str]] = []
        for pc, op, args, func in parse(text):
            if op == "lui":
                parts = args.split(",")
                if len(parts) == 2:
                    lui[parts[0].strip()] = int(parts[1], 16) << 16
            if op in ("sh", "sw", "sb"):
                m = re.match(r"(\w+),(-?\d+)\((\w+)\)", args)
                if m and lui.get(m.group(3)) == hi_reg and (int(m.group(2)) & 0xFFFF) == lo_off:
                    src, val = m.group(1), None
                    for hop, hargs in reversed(hist):
                        im = re.match(r"^(?:li\s+(\w+),(0x[0-9a-f]+)|addiu\s+(\w+),zero,(0x[0-9a-f]+))$",
                                      hop + " " + hargs)
                        if im:
                            r = im.group(1) or im.group(3)
                            if r == src:
                                val = im.group(2) or im.group(4)
                                break
                        if re.match(r"^(addu|or|move|and|sll|sra|lhu|lbu|lw|slt|sltu|jal|jalr)\b",
                                    hop + " " + hargs) and re.search(r"\b%s\b" % re.escape(src),
                                                                     hargs):
                            break
                    total += 1
                    static += val is not None
                    print(" %-18s 0x%08X %-24s %s" % (elf.name, pc, func, val or "(computed)"))
            hist.append((op, args))
            if len(hist) > 10:
                hist.pop(0)
    print()
    print("%d store sites; %d carry a statically-known value. The computed ones are\n"
          "the script-driven transitions (the VM writes D_8018F1C2 and the handler\n"
          "copies it), which is why this graph stays small." % (total, static))


# --- layer 3: the scripted step table ---------------------------------------

def cmd_steps() -> None:
    rom = ROM.read_bytes()
    rom_off = (STEP_TABLE_ASSET & 0x0FFFFFFF) + 0x594250
    size = int.from_bytes(rom[rom_off:rom_off + 4], "big")
    n = size // 4
    ids = [int.from_bytes(rom[rom_off + 4 + i * 4:rom_off + 8 + i * 4], "big") for i in range(n)]
    print("step table: asset 0x%08X  ROM 0x%06X  payload 0x%X = %d u32 entries"
          % (STEP_TABLE_ASSET, rom_off, size, n))
    print("  entries are per-step asset ids; the VM indexes it with D_8018F1C0 & 0xFFF.")
    print("  asset id range: 0x%08X .. 0x%08X" % (min(ids), max(ids)))
    print("  distinct ids: %d" % len(set(ids)))
    print()
    print("  These %d steps are the game's *scripted narrative* — the 200+ dialogues\n"
          "  are entries here, driven by the same few scene types (mainly 0x0D), not\n"
          "  200 separate scene links. Decoding the step-descriptor opcode format\n"
          "  once covers all of them." % n)


def main() -> int:
    argv = sys.argv[1:]
    dump = None
    if "--dump" in argv:
        i = argv.index("--dump")
        dump = argv[i + 1]
        del argv[i:i + 2]
    what = argv[0] if argv else "all"
    if what in ("scenes", "all"):
        cmd_scenes(dump)
        print()
    if what in ("transitions", "all"):
        cmd_transitions()
        print()
    if what in ("steps", "all"):
        cmd_steps()
    if what not in ("scenes", "transitions", "steps", "all"):
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
