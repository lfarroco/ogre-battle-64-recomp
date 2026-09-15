#!/usr/bin/env python3
"""Point the njpeg readback at the buffer its own draw landed in.

Background (session 48)
-----------------------
The New Game opening's step-2 background is an N64 JPEG: the `M_NJPEGTASK`
microcode decodes it into YUV macroblocks, the game draws those macroblocks with
a YUV texture image into a 320x240 colour image, and then the CPU copies that
framebuffer into the image it blits (`func_ovlE_8019976C`, the stage-3 row loop
at 0x80199884: `memcpy(state[0x70], state[0x64], 2*width)` per row).

`state[0x64]` is written by `func_ovlE_80199A08` from the game's framebuffer
table at guest `0x800A8204` = `{0x80000400, 0x80025C00, 0x8004B400}` (verified
against the ROM at `0x38604`), using an index it derives by comparing the word
`D_800C4BB8` against those three entries. When `D_800C4BB8` matches none of them
(observed: the game also swaps to non-framebuffer targets, e.g. `0x80250800`),
the index defaults to 0 and the copy reads the table's *first* entry -- which is
the ROM's `0x80000400` placeholder, not the buffer the YUV draw landed in. The
result is a uniform background (`0x0843` in RT64, `0` in the null build) even
though the decoded YUV is correct and the draw runs.

What this patch does
--------------------
RT64 records, in a scratch word in RDRAM's last 64 KiB (unused by OB64), the
colour image the YUV macroblock draw actually landed in -- by identity, and via
a "YUV texture image seen" handshake (see `rt64_rdp.cpp`,
`OGRE_NJPEG_SCRATCH`). This patch rewrites the stage-3 copy so its source is
that word, falling back to the most recent game framebuffer the RDP rendered
into, and finally to the game's own value. That is exactly the buffer the game
just drew, which is what the copy is for.

It is a code *generation* fix, like `cross_bank.py`: `make bank-recomp`
regenerates `Bank*Funcs/`, so the window is re-applied on every run. On a
renderer that does not maintain the scratch word (the null renderer) the patch
is a no-op and the game's own value is used, unchanged.

Usage:
    python3 tools/njpeg_readback.py           # apply (idempotent)
    python3 tools/njpeg_readback.py --revert  # remove
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "BankEFuncs" / "funcs_0.c"

# The instruction the patch is anchored to: `lw $s0, 0x64($a0)` -- the load of
# the copy source in func_ovlE_8019976C's stage-3 loop.
ANCHOR = "    // 0x80199878: lw          $s0, 0x64($a0)\n    ctx->r16 = MEM_W(ctx->r4, 0X64);\n"

# The CPU is about to copy a framebuffer here. RT64 renders into a Vulkan/Metal
# target and only writes it back to RDRAM when a workload is retired, so the
# readback would copy RDRAM as it was *before* the draw. Ask the renderer to
# retire the pending workload and write its framebuffers back first. The symbol
# is a no-op on renderers without render targets (null, web), so the null build
# is unchanged.
#
# The render wait itself lives on the RSP worker (app/src/renderer.cpp,
# Application::waitForGameFramebuffers): the port completes the emulated RSP task
# as soon as the display list is handed to RT64, so waiting here on the game
# thread can deadlock. This call only does the writeback for whatever the
# renderer has finished by now.
SYNC = """
    // njpeg readback: make the RDP's rendered pixels visible in RDRAM first.
    ogre_sync_framebuffers();
"""

PATCH = """
    { // njpeg readback source: use the buffer the YUV macroblock draw landed in.
      // `state[0x64]` comes from the game's framebuffer-table index, which
      // defaults to entry 0 (a ROM placeholder) whenever the display word does
      // not match a table entry -- see tools/njpeg_readback.py.
        {
            const uint8_t* s = (const uint8_t*)(rdram + 0x7FFC00);
            uint32_t njpegTarget = (uint32_t)s[8] | ((uint32_t)s[9] << 8) | ((uint32_t)s[10] << 16) | ((uint32_t)s[11] << 24);
            if (njpegTarget == 0) {
                njpegTarget = (uint32_t)s[12] | ((uint32_t)s[13] << 8) | ((uint32_t)s[14] << 16) | ((uint32_t)s[15] << 24);
            }
            if (njpegTarget != 0) {
                ctx->r16 = njpegTarget;
            }
        }
    }
"""

MARKER = "// njpeg readback source: use the buffer the YUV macroblock draw landed in."
SYNC_MARKER = "// njpeg readback: make the RDP's rendered pixels visible in RDRAM first."

# The block this patch inserts, matched by its marker through its closing brace,
# so --revert can remove it from an already-patched file.
APPLIED_RE = re.compile(
    r"\n    \{ " + re.escape(MARKER) + r".*?\n    \}\n", re.DOTALL
)
SYNC_APPLIED_RE = re.compile(
    r"\n    " + re.escape(SYNC_MARKER) + r"\n    ogre_sync_framebuffers\(\);\n", re.DOTALL
)


def apply() -> int:
    src = TARGET.read_text()
    if MARKER in src:
        print("njpeg_readback: already applied")
        return 0
    if src.count(ANCHOR) != 1:
        print(
            f"njpeg_readback: error: expected exactly one anchor in {TARGET.relative_to(ROOT)} "
            f"(found {src.count(ANCHOR)}); did the recompiler output change?",
            file=sys.stderr,
        )
        return 1
    # The C entry point lives in the app (app/src/renderer.cpp); declare it here
    # so the generated unit can call it.
    header = '#include "recomp.h"\n'
    if header not in src:
        print(f"njpeg_readback: error: no {header!r} in {TARGET.relative_to(ROOT)}", file=sys.stderr)
        return 1
    if SYNC_MARKER not in src:
        src = src.replace(
            header,
            header
            + "// njpeg readback: ask the renderer to write its render targets back to RDRAM.\n"
            + "#ifdef __cplusplus\nextern \"C\" void ogre_sync_framebuffers();\n#else\nvoid ogre_sync_framebuffers();\n#endif\n",
            1,
        )
    src = src.replace(ANCHOR, SYNC + ANCHOR + PATCH, 1)
    TARGET.write_text(src)
    print(f"njpeg_readback: patched {TARGET.relative_to(ROOT)}")
    return 0


def revert() -> int:
    src = TARGET.read_text()
    src, n1 = APPLIED_RE.subn("\n", src)
    src, n2 = SYNC_APPLIED_RE.subn("\n", src)
    if (n1 + n2) == 0:
        print("njpeg_readback: nothing to revert")
        return 0
    TARGET.write_text(src)
    print(f"njpeg_readback: reverted {n1 + n2} block(s) in {TARGET.relative_to(ROOT)}")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--revert":
        return revert()
    if len(sys.argv) != 1:
        print(__doc__.strip().splitlines()[-2].strip(), file=sys.stderr)
        return 2
    return apply()


if __name__ == "__main__":
    raise SystemExit(main())
