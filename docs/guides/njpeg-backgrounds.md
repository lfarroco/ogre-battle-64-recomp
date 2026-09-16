# The game's njpeg backgrounds (and the ones like the New Game cathedral)

Other large backgrounds in Ogre Battle 64 are built by the **same machinery** as
the New Game opening's cathedral, and the dominant one is the **same size**, so
the source-selection rule that produced the session-56/57 stale-backdrop
rectangle applies to all of them. Read this before debugging any large
background that shows part of the previous screen.

## The pipeline

1. **Asset** — an `njpeg` blob in the ROM: magic `'HU'`, then `u16` width and
   `u16` height (big-endian), then `'HUFF'`, then a `u16` macroblock count.
2. **Decode** — the game submits an `M_NJPEGTASK` (RSP task type 4) that decodes
   the blob to 16-bit YUV in place (`docs/guides/rsp-microcode.md`,
   `docs/HANDOFF-2026-09-15-session47.md`).
3. **Draw** — the gfx ucode `0x800A5110` (**S2DEX 2.08**) draws the macroblocks
   as `G_OBJ_RECTANGLE_R`s into a game framebuffer
   (`docs/HANDOFF-2026-09-15-session51.md`).
4. **CPU readback** — `func_ovlE_8019976C` (bankE) copies the drawn framebuffer
   out, **four passes per assembly**, each with its own decode+draw, into four
   fixed heap buffers.
5. **Blit** — the game draws the assembled image.

Session 57's fix lives in step 4: the copy's source must be the **game's own
`state[0x64]`** (its framebuffer-table choice), never RT64's scratch word, which
is stale at the first pass of every assembly. That rule is in
`tools/njpeg_readback.py` and applies to every background on this path, not just
the cathedral.

## Asset format and the full inventory

The dimensions are in the blob, so the whole set is enumerable:

```sh
python3 - <<'EOF'
from pathlib import Path
d = Path('assets/ogre64.z64').read_bytes()
o = d.find(b'HUFF'); n = 0
while o != -1:
    w = int.from_bytes(d[o-4:o-2], 'big'); h = int.from_bytes(d[o-2:o], 'big')
    mb = int.from_bytes(d[o+4:o+6], 'big')
    assert mb == (w // 16) * (h // 16), "macroblock count must match w/h"
    print(f"asset rom 0x{o-6:06X}  {w}x{h}  mb={mb}")
    n += 1; o = d.find(b'HUFF', o + 1)
print(n, "njpeg assets")
EOF
```

`0x{o-6}` is the asset start (the `u16` size field precedes the `'HU'` magic).
**Every one of the 75 assets satisfies `mb == (w/16)*(h/16)`** — that is the
check that the header layout is right; if a future extraction disagrees, the
layout changed.

| size | macroblocks | count | asset ROM (first few) |
|---|---|---|---|
| 320x240 | 300 | 42 | 0x59439E, 0x59A632, 0x5A0E0A … |
| 224x192 | 168 | 3 | 0x810862, 0x817BA2, 0x820022 |
| 208x192 | 156 | 3 | 0x8145AA, 0x81BF6A, 0x8242C2 |
| 336x256 | 336 | 2 | 0x800C8E, 0x803EAE |
| 320x160 | 200 | 2 | 0x7985E2, 0x7A422E |
| 320x96 | 120 | 2 | 0x7E56BA, 0x1D8F56E |
| 176x240 | 165 | 2 | 0x72256C, 0x7D1B9E |
| 80x240 | 75 | 2 | 0x759F06, 0x796E3A |
| 336x176 | 231 | 1 | 0x80B896 |
| 320x208 | 260 | 1 | 0x724DB4 |
| 320x176 | 220 | 1 | 0x75B346 |
| 320x144 | 180 | 1 | 0x7D4EE6 |
| 320x112 | 140 | 1 | 0x7EFA1E |
| 192x240 | 180 | 1 | 0x7E229A |
| 192x96 | 72 | 1 | 0x7E788A |
| 176x208 | 143 | 1 | 0x72987C |
| 176x144 | 99 | 1 | 0x7D91D6 |
| 160x240 | 150 | 1 | 0x7A21B6 |
| 160x160 | 100 | 1 | 0x7A73CE |
| 144x240 | 135 | 1 | 0x7EDFC6 |
| 144x112 | 63 | 1 | 0x7F1166 |
| 128x240 | 120 | 1 | 0x1D8DC0E |
| 128x96 | 48 | 1 | 0x1D90BA6 |
| 80x176 | 55 | 1 | 0x75E3EE |
| 80x160 | 50 | 1 | 0x79BBE2 |

**42 of the 75 are 320x240** — the same size as the New Game backdrop pieces —
which is the answer to "do the other large backgrounds use the same size?" for
the dominant case. The rest are smaller pieces (sub-images, portraits, partial
backgrounds). All of them go through the same decode/draw/readback path.

## What one assembly copies (measured, session 57)

`func_ovlE_8019976C`'s row loop is
`memcpy(state[0x70], state[0x64], 2*state[0x78])` per row, `state[0x7A]` rows,
source stride a fixed `0x280` (640 bytes = 320 px), destination stride
`2*state[0x78]`. Four passes per assembly, into four fixed buffers:

| pass | destination | `state[0x78]` x `state[0x7A]` |
|---|---|---|
| 0 | `0x801AAE90` | 240 x 320 |
| 1 | `0x801D06B0` | 240 x 176 |
| 2 | `0x801E50D0` | 144 x 320 |
| 3 | `0x801FB8F0` | 144 x 176 |

**Pass 0 is the one that goes stale** — it is the first pass after a step change,
so RT64's scratch word still names the previous step's target. The developer
reports the broken on-screen tile as the **top-left** of the scene's four-image
2x2 composition, and pass 0 is the only stale pass, so they are almost certainly
the same tile — but note that this is *inferred*, not verified from the blit.

**Do not read the framebuffer as the scene.** At the cathedral step
`0x80000400` contains four background chunks in a 2x2 arrangement with black
seams (`docs/proofs/native-newgame-backdrop-chunks.png`), but the developer
confirms the chunks are **out of order** — the arrangement in the framebuffer is
*not* the on-screen arrangement. Reordering (and rescaling) the chunks is what
the readback and the following blit are for, and the four copies taken together
are exactly what makes the finished backdrop. Any statement of the form "chunk X
is the top-left of the scene" has to come from the blit that consumes those
buffers, not from this render.

## Traps that cost this project sessions

* **The three game framebuffers are contiguous 320x240 buffers** —
  `D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`, `0x25800` apart. Render
  `0x80000400` for 320 rows and rows 240-319 are the *next* buffer's content (in
  session 57's dump, the name-entry form — which is why
  `docs/proofs/native-newgame-backdrop-chunks.png` shows a form strip below the
  chunks; that strip is **not** part of the background). Any over-read or
  mis-sourced read shows the neighbour, which is why "the backdrop holds the
  previous screen" always looked like a timing bug.
* **RT64's scratch word `0x807FFC08` is not a valid readback source.** It is only
  re-set by a YUV-texture-then-colour-image pair and is never cleared.
* **The readback is ordered by the game's DP-completion wait**
  (`events.cpp:432`), not by `sp_complete()` and not by
  `waitForGameFramebuffers`/`OGRE_NJ_WAIT_MS` (a no-op once the game's buffers
  exist). `ogre_sync_framebuffers()` forces the RDP's pixels back to RDRAM before
  the copy.

## Checking another background

```sh
OGRE_NJREAD_LOG=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000   OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a"   OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64 assets/ogre64.z64   > /dev/null 2> /tmp/njread.log
grep njread /tmp/njread.log
```

One line per pass. For the New Game cathedral, `sig` must cycle
`573FF47B8A5A3279`, `48892A76C362A4BA`, `3F79B6732153B2FD`, `38BCBEBE5B555C3C`,
and `alt` (what the old scratch-first rule would have read, `0` when the two
agree) must be `0`. A non-zero `alt` at the first pass of an assembly is the
stale-backdrop bug; its value names the previous screen.

To inspect the *result*, drive the live console from game state (a wall-clock
`dump` schedule shifts the tap route) — see `docs/guides/app-build.md` →
`OGRE_NJREAD_LOG`.

## Open questions (for whoever needs them)

* **Which on-screen position each pass feeds.** The four destination buffers are
  refreshed per assembly and are not live long enough to dump with the console,
  so the mapping has to be read out of the blit that consumes them (it walks the
  buffer pointers the pipeline leaves in its state object, `0x8019A680`). Until
  that is done, do not assert "pass 0 is the top-left" — the framebuffer's own
  chunk order is scrambled relative to the screen (developer, session 57).
* The measured pairs are the **transposes** of four asset sizes that exist in the
  ROM (320x240, 176x240, 320x144, 176x144) — 320x240 is the dominant one — so each
  pass plausibly reads one of those assets, but the copy's row-length/row-count
  reading of `state[0x78]`/`state[0x7A]` is not pinned down here either.
* Whether every one of the 75 assets reaches `func_ovlE_8019976C`, or some take a
  different (non-readback) path; the step table
  (`docs/HANDOFF-2026-09-15-session44.md` §1) maps the opening's steps to assets.

## See also

* `docs/HANDOFF-2026-09-16-session57.md` — the stale-source fix, the live-console
  A/B, and the 2x2 tile identification.
* `docs/guides/rsp-microcode.md` — the `M_NJPEGTASK` decoder and RSPRecomp.
* `docs/HANDOFF-2026-09-15-session51.md` — the S2DEX2 object commands and the
  YUV16 two-plane tile format.
