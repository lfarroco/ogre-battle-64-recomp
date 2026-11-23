#!/usr/bin/env python3
"""Add the bundled mods' entry call sites to the recompiled game code.

The browser build cannot patch a function's prologue, so a bundled code mod is
dispatched from the function's own entry. This script inserts one guarded call
there:

    #ifdef __EMSCRIPTEN__
        ogre_web_mod_entry_hook(rdram, ctx, 0x80177DCC);
    #endif

for an entry hook, or

    #ifdef __EMSCRIPTEN__
        if (ogre_web_mod_replace(rdram, ctx, 0x80074834)) {
            return;
        }
    #endif

for a replacement, and declares the two helpers once per file. The target list
comes from `app/src/web_mods/instrumented.txt`, which `tools/gen_web_mods.py`
derives from the bundled mods' `.recomp_hook.*` / `.recomp_patch` sections, so a
new bundled mod with a hook is picked up by regenerating that file.

Native builds are untouched: `__EMSCRIPTEN__` is not defined, and the injected
text is inside the guard.

`make recomp` re-applies this after regenerating RecompiledFuncs/; `make web`
checks that every listed target is instrumented. A run on a tree with no
generated mod output removes any stale calls and says so.

Usage:
    python3 tools/web_mod_hooks.py            # apply
    python3 tools/web_mod_hooks.py --revert   # remove
    python3 tools/web_mod_hooks.py --check    # fail if a listed target is missing
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECOMP = ROOT / "RecompiledFuncs"
TARGETS = ROOT / "app" / "src" / "web_mods" / "instrumented.txt"

DECL_MARKER = "// web mods: bundled precompiled code mods."
DECL_BLOCK = f"""\
#ifdef __EMSCRIPTEN__
{DECL_MARKER}
void ogre_web_mod_entry_hook(uint8_t* rdram, recomp_context* ctx, unsigned int function_vram);
int ogre_web_mod_replace(uint8_t* rdram, recomp_context* ctx, unsigned int function_vram);
#endif
"""

ENTRY_MARKER = "// web mods: entry hooks for the bundled precompiled mods."
REPLACE_MARKER = "// web mods: a bundled precompiled mod may replace this function."

ENTRY_APPLIED_RE = re.compile(
    r"#ifdef __EMSCRIPTEN__\n    " + re.escape(ENTRY_MARKER) +
    r"\n    ogre_web_mod_entry_hook\(rdram, ctx, 0x[0-9A-Fa-f]{8}\);\n#endif\n"
)
REPLACE_APPLIED_RE = re.compile(
    r"#ifdef __EMSCRIPTEN__\n    " + re.escape(REPLACE_MARKER) +
    r"\n    if \(ogre_web_mod_replace\(rdram, ctx, 0x[0-9A-Fa-f]{8}\)\) \{\n"
    r"        return;\n    \}\n#endif\n"
)
DECL_APPLIED_RE = re.compile(
    r"#ifdef __EMSCRIPTEN__\n" + re.escape(DECL_MARKER) + r"\n.*?#endif\n",
    re.DOTALL,
)


def fail(message: str) -> None:
    print(f"web_mod_hooks.py: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_targets() -> list[tuple[str, int]] | None:
    if not TARGETS.is_file():
        return None
    targets: list[tuple[str, int]] = []
    for line in TARGETS.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2 or fields[0] not in ("entry", "replace"):
            fail(f"bad line in {TARGETS.relative_to(ROOT)}: {line}")
        targets.append((fields[0], int(fields[1], 16)))
    return targets


def function_block(source: str, name: str) -> tuple[int, int] | None:
    """Start and end offsets of a generated function's body preamble."""
    pattern = re.compile(
        r"RECOMP_FUNC void " + re.escape(name) +
        r"\(uint8_t\* rdram, recomp_context\* ctx\) \{\n"
        r"    recomp_trace_entry\([^\n]*\);\n"
    )
    match = pattern.search(source)
    if match is None:
        return None
    return match.start(), match.end()


def add_declaration(source: str) -> str:
    if DECL_MARKER in source:
        return source
    anchor = '#include "funcs.h"\n'
    index = source.find(anchor)
    if index < 0:
        fail("a RecompiledFuncs file has no funcs.h include")
    index += len(anchor)
    return source[:index] + DECL_BLOCK + source[index:]


def apply() -> None:
    targets = read_targets()
    if targets is None:
        # No generated mod output; make sure a previous run's calls are gone.
        revert(quiet=True)
        print("web_mod_hooks.py: no app/src/web_mods/instrumented.txt; "
              "no browser mod call sites added (run 'make web-mods' first)")
        return

    if not RECOMP.is_dir():
        fail("RecompiledFuncs/ is missing; run 'make recomp' after 'make web-mods'")

    # Remove a previous run's insertions first, so applying twice cannot stack
    # two calls on one function.
    revert(quiet=True)

    by_name: dict[str, list[str]] = {}
    for kind, vram in targets:
        by_name.setdefault(f"func_{vram:08X}", []).append(kind)

    found: set[str] = set()
    for path in sorted(RECOMP.glob("funcs_*.c")):
        source = path.read_text()
        original = source
        for name, kinds in by_name.items():
            block = function_block(source, name)
            if block is None:
                continue
            found.add(name)
            start, end = block
            insert = ""
            if "entry" in kinds:
                vram = int(name[len("func_"):], 16)
                insert += (
                    "#ifdef __EMSCRIPTEN__\n"
                    f"    {ENTRY_MARKER}\n"
                    f"    ogre_web_mod_entry_hook(rdram, ctx, 0x{vram:08X});\n"
                    "#endif\n"
                )
            if "replace" in kinds:
                vram = int(name[len("func_"):], 16)
                insert += (
                    "#ifdef __EMSCRIPTEN__\n"
                    f"    {REPLACE_MARKER}\n"
                    f"    if (ogre_web_mod_replace(rdram, ctx, 0x{vram:08X})) {{\n"
                    "        return;\n"
                    "    }\n"
                    "#endif\n"
                )
            source = source[:end] + insert + source[end:]
        if source != original:
            if DECL_MARKER not in source:
                source = add_declaration(source)
            path.write_text(source)

    missing = sorted(set(by_name) - found)
    if missing:
        fail(
            "these game functions are not in RecompiledFuncs/ (regenerate it with "
            "'make recomp'): " + ", ".join(missing)
        )
    print(f"web_mod_hooks.py: {len(targets)} call site(s) in place")


def revert(quiet: bool = False) -> None:
    changed = 0
    for path in sorted(RECOMP.glob("funcs_*.c")):
        source = path.read_text()
        updated = ENTRY_APPLIED_RE.sub("", source)
        updated = REPLACE_APPLIED_RE.sub("", updated)
        updated = DECL_APPLIED_RE.sub("", updated)
        if updated != source:
            path.write_text(updated)
            changed += 1
    if not quiet:
        print(f"web_mod_hooks.py: removed the call sites from {changed} file(s)")


def check() -> None:
    targets = read_targets()
    if targets is None:
        fail("app/src/web_mods/instrumented.txt is missing; run 'make web-mods'")
    exercised: set[tuple[str, int]] = set()
    for path in sorted(RECOMP.glob("funcs_*.c")):
        source = path.read_text()
        for match in ENTRY_APPLIED_RE.finditer(source):
            vram = int(re.search(r"0x([0-9A-Fa-f]{8})", match.group(0)).group(1), 16)
            exercised.add(("entry", vram))
        for match in REPLACE_APPLIED_RE.finditer(source):
            vram = int(re.search(r"0x([0-9A-Fa-f]{8})", match.group(0)).group(1), 16)
            exercised.add(("replace", vram))
    expected = set(targets)
    missing = expected - exercised
    stale = exercised - expected
    if missing:
        fail(
            "missing browser mod call site(s) for "
            + ", ".join(f"{kind} 0x{vram:08X}" for kind, vram in sorted(missing))
            + "; run 'make recomp' (after 'make web-mods')"
        )
    if stale:
        fail(
            "stale browser mod call site(s) for "
            + ", ".join(f"{kind} 0x{vram:08X}" for kind, vram in sorted(stale))
            + "; run 'make recomp'"
        )
    print(f"web_mod_hooks.py: all {len(expected)} call site(s) present")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--revert", action="store_true", help="remove the calls")
    group.add_argument("--check", action="store_true", help="verify the calls")
    args = parser.parse_args()
    if args.revert:
        revert()
    elif args.check:
        check()
    else:
        apply()


if __name__ == "__main__":
    main()
