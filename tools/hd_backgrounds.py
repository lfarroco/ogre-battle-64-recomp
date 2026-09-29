#!/usr/bin/env python3
"""Call the app's HD-background hook from the njpeg readback.

The game assembles every large dialogue/cutscene backdrop on the njpeg path:
`func_ovlE_8019976C` (bank unit E) copies one sub-image per pass out of a game
framebuffer into a fixed buffer, and `func_ovlE_80199D30` later merges the four
buffers into the `'B5'` image the blit reads. Bank units are not covered by the
mod hook map (`docs/guides/app-build.md` -> "What a hook can reach"), so a `.nrm`
code mod cannot reach this code. The port calls in from the generated unit
instead, next to the existing `ogre_sync_framebuffers()` call that
`tools/njpeg_readback.py` places at the copy's source load.

This script inserts `ogre_hd_background(rdram)` at the end of the pass copy, in
`func_ovlE_8019976C`, between the store that bumps the pass counter and the
common return label `L_80199954`:

    0x80199950  sb   $v0, 0x7E($v1)      # state[0x7E]++: this pass is done
    >>> ogre_hd_background(rdram);
    L_80199954:                          # every other path jumps here directly

The call is therefore reached only on the path that copied pixels, after the
pixels are in the destination buffer, and before the merge runs. The app-side
function reads the game's state object at 0x8019A680 for the destination,
dimensions and header flag, and writes the pack's image if the current
(scene, step) is mapped. The symbol is defined for every build
(`app/src/hd_backgrounds.cpp`) and no-ops when no pack is installed, so the null
and web builds are unchanged.

It is a code *generation* fix, like `tools/njpeg_readback.py`: `make bank-recomp`
regenerates `Bank*Funcs/`, so the insertion is re-applied on every run.

Usage:
    python3 tools/hd_backgrounds.py           # apply (idempotent)
    python3 tools/hd_backgrounds.py --revert  # remove
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "BankEFuncs" / "funcs_0.c"

# The end of the stage-3 row copy: the pass counter store, then the return
# label that the other state-machine paths jump to directly.
ANCHOR = (
    "    // 0x80199950: sb          $v0, 0x7E($v1)\n"
    "    MEM_B(0X7E, ctx->r3) = ctx->r2;\n"
    "L_80199954:\n"
)

CALL_MARKER = "// hd backgrounds: the pass's pixels are in their buffer now."

CALL = (
    "    " + CALL_MARKER + "\n"
    "    // Let the image pack replace them (app/src/hd_backgrounds.cpp). A no-op\n"
    "    // when no pack is installed.\n"
    "    ogre_hd_background(rdram);\n"
)

DECL_MARKER = "// hd backgrounds: replace a copied backdrop with a pack image."

DECL = (
    DECL_MARKER
    + "\n"
    + "#ifdef __cplusplus\n"
    + 'extern "C" void ogre_hd_background(uint8_t* rdram);\n'
    + "#else\n"
    + "void ogre_hd_background(uint8_t* rdram);\n"
    + "#endif\n"
)

# Matched from the marker through the call, so --revert can remove it again.
APPLIED_RE = re.compile(
    r"\n    " + re.escape(CALL_MARKER) + r".*?\n    ogre_hd_background\(rdram\);\n",
    re.DOTALL,
)
DECL_APPLIED_RE = re.compile(
    r"\n"
    + re.escape(DECL_MARKER)
    + r"\n#ifdef __cplusplus\nextern \"C\" void ogre_hd_background\(uint8_t\* rdram\);\n"
    + r"#else\nvoid ogre_hd_background\(uint8_t\* rdram\);\n#endif\n",
    re.DOTALL,
)


def apply() -> int:
    src = TARGET.read_text()
    if CALL_MARKER in src:
        print("hd_backgrounds: already applied")
        return 0
    if src.count(ANCHOR) != 1:
        print(
            f"hd_backgrounds: error: expected exactly one anchor in "
            f"{TARGET.relative_to(ROOT)} (found {src.count(ANCHOR)}); did the "
            f"recompiler output change?",
            file=sys.stderr,
        )
        return 1
    header = '#include "recomp.h"\n'
    if header not in src:
        print(f"hd_backgrounds: error: no {header!r} in {TARGET.relative_to(ROOT)}",
              file=sys.stderr)
        return 1
    src = src.replace(header, header + DECL, 1)
    src = src.replace(ANCHOR, ANCHOR.replace("L_80199954:\n", CALL + "L_80199954:\n"), 1)
    TARGET.write_text(src)
    print(f"hd_backgrounds: patched {TARGET.relative_to(ROOT)}")
    return 0


def revert() -> int:
    src = TARGET.read_text()
    src, n1 = APPLIED_RE.subn("\n", src)
    src, n2 = DECL_APPLIED_RE.subn("\n", src)
    if (n1 + n2) == 0:
        print("hd_backgrounds: nothing to revert")
        return 0
    TARGET.write_text(src)
    print(f"hd_backgrounds: reverted {n1 + n2} block(s) in {TARGET.relative_to(ROOT)}")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--revert":
        return revert()
    if len(sys.argv) != 1:
        print("usage: hd_backgrounds.py [--revert]", file=sys.stderr)
        return 2
    return apply()


if __name__ == "__main__":
    sys.exit(main())
