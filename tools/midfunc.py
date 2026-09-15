#!/usr/bin/env python3
"""Report "shared tail" functions: function labels that the previous function
falls through into, with their callers and the register/frame contract the tail
expects.

Why this exists
---------------
`splat` puts a label wherever a `jal` points, and the recompiler then emits a C
function for it — but the target is often *not* a function: it is the tail of the
function above it, entered by falling through (there is no `jr $ra` between
them). A `jal` to such a label is real code that inherits a frame and registers
from the head function. Session 44 lost most of a session to two of these
(`func_ovlC_802399AC`, the tail of `func_ovlC_80239874`, reads `*(sp+0x1EC)` and
the `f` regs; `func_ovlC_8023C894`, the tail of `func_ovlC_8023C824`, needs `a3`
as the display-list cursor). The report makes them a two-second lookup.

Usage:
    tools/midfunc.py                    # every asm tree in the repo
    tools/midfunc.py BankCFuncs         # only trees whose path matches
    tools/midfunc.py --all              # also list tails with no callers
"""

from __future__ import annotations

import os
import re
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GLABEL_RE = re.compile(r"^glabel\s+(\S+)")
ENDLABEL_RE = re.compile(r"^endlabel\s+(\S+)")
INSN_RE = re.compile(
    r"/\*\s*([0-9A-F]{5,8})\s+([0-9A-F]{8})\s+[0-9A-F]{8}\s*\*/\s+(\S+)(?:\s+(.*))?$"
)

# Mnemonics that leave the function (directly, or in the delay slot of one).
TRANSFER_RE = re.compile(r"^(jr|j|jal|jalr|b|beq|bne|blez|bgtz|bltz|bgez|bc1|bnel|beql|bgezl|bltzl|bgtzl|blezl)$")

CALLEE_SAVED = ["$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7", "$fp", "$ra",
                "$fs0", "$fs1", "$fs2", "$fs3", "$fs4", "$fs5", "$fs6", "$fs7",
                "$f20", "$f22", "$f24", "$f26", "$f28", "$f30"]

# Caller-saved registers the head leaves live for the tail. $a0-$a2 are the
# ordinary arguments, so they are not reported; anything else (notably $a3, the
# $t regs and $v0/$v1) is a value the tail inherits *by accident of codegen*,
# which is exactly the contract that breaks a direct `jal` to the tail.
CALLER_SAVED = (["$v0", "$v1", "$a3"] +
                ["$t%d" % i for i in range(10)] + ["$at"])

STORE_MNEMONICS = {"sw", "sh", "sb", "swl", "swr", "swc1", "sdc1", "sdr", "swc1"}


class Insn:
    __slots__ = ("addr", "mnemonic", "text")

    def __init__(self, addr: int, mnemonic: str, text: str) -> None:
        self.addr = addr
        self.mnemonic = mnemonic
        self.text = text

    def reads_writes(self) -> tuple[set[str], set[str]]:
        """Approximate source/destination registers for the contract report."""
        ops = [o.strip() for o in self.text.split(",")] if self.text else []
        regs = re.findall(r"\$[a-z0-9]+", self.text or "")
        base = re.search(r"[-0-9A-Fx]+\((\$[a-z0-9]+)\)", self.text or "")
        reads: set[str] = set(regs)
        writes: set[str] = set()
        if self.mnemonic in STORE_MNEMONICS:
            pass  # all operands are sources
        elif ops and ops[0].startswith("$") and not self.mnemonic.startswith("b"):
            writes.add(ops[0])
        if base:
            reads.add(base.group(1))
        return reads - writes, writes


def asm_trees() -> list[str]:
    trees = [os.path.join(REPO, "asm")]
    build = os.path.join(REPO, "build")
    if os.path.isdir(build):
        for entry in sorted(os.listdir(build)):
            candidate = os.path.join(build, entry, "asm")
            if os.path.isdir(candidate):
                trees.append(candidate)
    return trees


def parse_file(path: str):
    """-> (functions, jal_sites); a function is (name, file, [Insn])."""
    functions = []
    jal_sites = []
    current = None
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            m = GLABEL_RE.match(line)
            if m:
                current = (m.group(1), path, [])
                functions.append(current)
                continue
            if ENDLABEL_RE.match(line):
                current = None
                continue
            m = INSN_RE.search(line)
            if not m or current is None:
                continue
            addr = int(m.group(2), 16)
            mnemonic, rest = m.group(3), (m.group(4) or "").strip()
            insn = Insn(addr, mnemonic, rest)
            current[2].append(insn)
            if mnemonic in ("jal",):
                target = rest.split()[0] if rest else ""
                jal_sites.append((target, addr, path))
    return functions, jal_sites


def ends_with_transfer(insns: list[Insn]) -> bool:
    # The last line is usually the delay slot of the branch/return before it, so
    # look at the last two instructions.
    for insn in insns[-2:]:
        if TRANSFER_RE.match(insn.mnemonic):
            return True
    return False


def contract(insns: list[Insn]) -> tuple[str, str]:
    """(frame + callee-saved reads, inherited caller-saved reads)."""
    written: set[str] = set()
    saved, inherited = [], []
    for insn in insns[:12]:
        reads, writes = insn.reads_writes()
        slot = re.search(r"([-0-9A-Fx]+)\(\$sp\)", insn.text or "")
        if slot and insn.mnemonic.startswith("l"):
            saved.append("%s($sp)" % slot.group(1).replace("0x", ""))
        for reg in sorted(reads - written):
            if reg in CALLEE_SAVED and reg != "$sp":
                saved.append(reg)
            elif reg in CALLER_SAVED:
                inherited.append(reg)
        written |= writes

    def dedup(items):
        seen, out = set(), []
        for item in items:
            if item not in seen:
                seen.add(item)
                out.append(item)
        return ", ".join(out[:8]) if out else "-"

    return dedup(saved), dedup(inherited)


def main() -> int:
    filters = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv

    functions = []
    jal_sites = []
    for tree in asm_trees():
        if filters and not any(f in tree for f in filters):
            continue
        for root, _dirs, files in os.walk(tree):
            for name in sorted(files):
                if name.endswith(".s"):
                    fns, jals = parse_file(os.path.join(root, name))
                    functions.extend(fns)
                    jal_sites.extend(jals)

    callers: dict[str, list[tuple[int, str]]] = defaultdict(list)
    for target, addr, path in jal_sites:
        callers[target].append((addr, os.path.relpath(path, REPO)))

    # Group by file so "the previous function" is meaningful.
    by_file: dict[str, list] = defaultdict(list)
    for name, path, insns in functions:
        by_file[path].append((name, insns))

    tails = []
    for path, fns in by_file.items():
        for i in range(1, len(fns)):
            prev_name, prev_insns = fns[i - 1]
            name, insns = fns[i]
            if not prev_insns or not insns:
                continue
            if ends_with_transfer(prev_insns):
                continue
            tails.append((name, prev_name, path, insns))

    print("midfunc: %d function label(s), %d 'jal' site(s), %d fall-through tail(s)%s"
          % (len(functions), len(jal_sites), len(tails),
             " (with callers)" if not show_all else ""))
    print()
    rows = []
    for name, head, path, insns in tails:
        sites = callers.get(name, [])
        if not sites and not show_all:
            continue
        rows.append((len(sites), name, head, path, insns, sites))
    rows.sort(key=lambda r: (-r[0], r[1]))
    for count, name, head, path, insns, sites in rows:
        print("%s  <- tail of %s   [%s]" % (name, head, os.path.relpath(path, REPO)))
        print("    first: %s" % "; ".join("%s %s" % (i.mnemonic, i.text) for i in insns[:3]))
        saved, inherited = contract(insns)
        print("    reads before writing: frame/callee-saved %s | inherited caller-saved %s"
              % (saved, inherited))
        print("    %d jal site(s): %s" % (count, ", ".join(
            "0x%08X (%s)" % (a, p) for a, p in sites[:6]) + (" ..." if count > 6 else "")))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
