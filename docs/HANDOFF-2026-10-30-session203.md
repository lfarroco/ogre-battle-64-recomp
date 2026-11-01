# Session 203 handoff — the browser's New Game cathedral backdrop

## Goal

Continue the web renderer from session 202's open item 4: the New Game cathedral
backdrop (scene `0x0D`, step 2) rendered as a green/magenta field in the browser,
then, after session 202's writeback guard, as a black backdrop. Measure the
S2DEX2 YUV draw itself and fix it.

## Result

The backdrop now renders (`docs/proofs/web-newgame-cathedral-session203.png`).
Two faults, both in `app/src/web_renderer.cpp`:

1. **The YUV load exchanged luma and chroma.** `tmem_load_yuv` read the njpeg
   macroblock word as `source.byte(word + 2)`/`+0` (luma) and `+3`/`+1` (U/V).
   Those are RT64's indices into its *native-order* RDRAM pointer
   (`RDP::loadYUVTileToTMEM` reads `word[0..3]` raw). `LoadSource::byte()` already
   applies the guest `^ 3` permutation, so the same indices name the logical bytes
   2/0/3/1, and the de-interleaved TMEM held chroma where the sampler reads luma.
   The load now reads logical `+0` U, `+1` Y even, `+2` V, `+3` Y odd.

2. **The writeback's row band was derived from the target's height, not the
   viewport the draw rendered through.** Rows were recorded as
   `(pos_y + 1) / 2 * fb.height`, while the draw renders through
   `glViewport(0, fb.height - vh, vw, vh)` with `vh = cmd.fb_height_px`. A 320x240
   image drawn into the 320x476 target that the title grew therefore recorded all
   476 rows as drawn, and the writeback copied 236 rows past the image over the
   next framebuffer's start. That made the next image's RDRAM reference stale, so
   its writeback was refused in turn. The band now uses the viewport mapping.

Two supporting changes keep the session-202 guard usable under the deferred
writeback:

- `fb.rdram_hash` (the reference the writeback compares against) is taken when a
  draw era into a framebuffer starts, which is RT64's `hashTracking` rule moved
  to the only place this renderer can apply it (it defers the writeback to the
  game's `ogre_sync_framebuffers()` request).
- A framebuffer with rows pending from an earlier batch is re-tested once per
  batch, and a mismatch drops those rows and starts a new era: RT64's
  `checkRAM(..., updateHashes = true)`. Without it one stale batch disabled an
  image for the rest of the run, which is why the first njpeg pass copied a black
  framebuffer every time.

## Evidence

### The YUV byte order

`OGRE_YUV_TRACE=1` prints the source bytes and both TMEM planes. Before and after
the fix, same route and same first macroblock:

```
[GFX-YUVLOAD] load 1: addr=0x1D1030 rowbytes=32 rows=16 tmem=0 line=2 fmt=1 siz=2
              src[0..23]=7A 28 9A 27 7B 25 96 24 ...  k=175,469,423,222
before:
[GFX-YUVDEC] decode 1: tmemrow0[0..7]=27 28 24 25 21 22 21 21
              luma=(9A 7A) chroma=(27 28) rgb0=(33,244,0) rgb1=(1,212,0)
after:
[GFX-YUVDEC] decode 1: tmemrow0[0..7]=7A 9A 7B 96 7C 91 7D 90
              luma=(28 27) chroma=(7A 9A) rgb0=(76,24,30) rgb1=(75,23,29)
```

The "after" rows are exactly the values session 51 verified from a live macroblock
dump for this format (`docs/HANDOFF-2026-09-15-session51.md` §6: luma
`28 27 25 24 22 21`, chroma `7A 9A 7B 96 7C 91`). The source bytes are unchanged
by the fix, so the RSP decode is the same; only the interpretation was wrong.
`rgb0=(33,244,0)` is the saturated green of the reported frame, and the
luminance-shaped magenta in it is the chroma plane read as luma.

The frame with the guard compiled out (so the writeback runs and the wrong decode
is what reaches the screen) is `docs/proofs/web-newgame-cathedral-green-session203.png`.

### The row band and the writeback reference

`[GFX-FBSYNC]` now prints the game's own readback state (`0x8019A680` →
`state[0x64]`, `+0x68`, `+0x70`, `+0x78`, `+0x7A`) beside the three game
framebuffers. The four njpeg passes copy 320x240, 176x240, 320x144 and 176x144 out
of `0x80000400`, and all four S2DEX2 tasks draw into `cimg=0x000400`
(`[GFX-S2D]` now prints one line per task with the target image, the way RT64's
`OGRE_S2D_TRACE` does).

Before the row fix, the first pass copied a black image:

```
sync 1: ramNonBlack: 0x000400=0 0x025C00=5746 0x04B400=27646
        state=0x801AACE0 src=0x80000400 dst=0x801AAE90 ... copy=320x240
[GFX-FBWRITE-STALE] fb 0x000400 320x476: RDRAM changed since the GPU copy
        (hash 79620924DCCA36B8 != 2561E009694D6AF2)
```

`2561E009694D6AF2` is the value recorded when the target was created: it is the
same in every run and never changed, because the image always had rows pending
from earlier scenes, so no new reference was taken. The skip dropped the S2DEX2
rows with them. The frame is
`docs/proofs/web-newgame-cathedral-black-session203.png`: the top-left sub-image
(320x240, the first pass) is black and the other three are correct.

After the row fix and the reference changes, all four passes read a full image:

```
sync 1: ramNonBlack: 0x000400=76800 0x025C00=14875 0x04B400=5746 ... copy=320x240
sync 2: ramNonBlack: 0x000400=76800 ...                              copy=176x240
sync 3: ramNonBlack: 0x000400=76800 ...                              copy=320x144
sync 4: ramNonBlack: 0x000400=76800 ...                              copy=176x144
```

`76800` is 320x240 dark pixels, so each pass's copy source holds the rendered
image. The five `[GFX-FBWRITE-STALE]` lines of the run name the *other*
framebuffers (`0x025C00`, `0x04B400`, `0x1BA780`, `0x250800`, `0x2E6840`) at the
visit's first sync: those images still had rows pending from earlier scenes and
their ranges had been reused, so the guard drops them. The njpeg target
`0x000400` is not among them.

The row fix also removes the overrun that caused the cascade. In the run before
it (whose S2DEX2 tasks draw into `0x025C00`), that image's writeback covered 285
rows and copied its lower, black part over `0x04B400`'s first rows: the sync line
reads `0x04B400=5747` at sync 1 and `0` at sync 2. After it, the writebacks cover
`rows=[0,240]` and `rows=[0,144]`, and `0x04B400` reads `27646` at all four
syncs.

### Regression screens

Same build, `scene.cjs`:

- `--route title`: scene `0x0004`, `29.71 tasks/s`, clouds and the prologue.
- `--route tutorial-field`: scene `0x0003`, `20.69 tasks/s`, the practice field,
  `Movement & Liberation` box and the tutorial dialogue.
- `--route mission-briefing`: scene `0x0003`, `19.73 tasks/s`, the NOTE box, the
  framed Castle Burgund illustration and the `MISSION` labels.

The tutorial battle from session 202's route needs a hand-driven lesson menu and
was not re-run; its fix is untouched (the writeback guard's skip and its
`[GFX-FBWRITE-STALE]` line are unchanged) and the YUV fix affects only YUV16
tiles.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final tree.
- `node debug/probes/scene.cjs --route cathedral --repeat 3 --repeat-gap 10000
  --points 160,120;40,120;300,120;560,60 --out /tmp/cath/ship` on the final
  binary reaches scene `0x000D` and the njpeg task (`log /GFX-S2D-CENSUS/ seen
  after 111.3 s`); the two proof frames above are its shots 2 and 3.
  The three shots are 12, 23 and 34 s after that line (`--shot-settle` 1.2 s plus
  `--repeat-gap` 10 s) and are three different screens: the "Ischka Military
  Academy" card still typing, the assembled backdrop without a dialogue box
  (`docs/proofs/web-newgame-cathedral-backdrop-session203.png`), and the
  Archbishop Odiron dialogue over it
  (`docs/proofs/web-newgame-cathedral-session203.png`, shot 3,
  `sha256 f18fbc9e…`).
- The three regression routes above on the same build.
- The black and green frames are from the same route with the intermediate
  builds (guard-only, then guard off), and each was a single-variable run.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `tmem_load_yuv` reads the logical `[U, Y(even), V, Y(odd)]` word; the writeback row band uses the draw's viewport; `snapshot_fb_ram_before_draw` takes the RDRAM reference at draw-era start and re-tests pending rows once per batch; `framebuffer_rdram_hash` takes a byte count; `OGRE_YUV_TRACE` (`[GFX-YUVLOAD]`/`[GFX-YUVDEC]`); `[GFX-S2D]` one line per S2DEX2 task with `cimg`/`cimg_w`; `[GFX-FBSYNC]` prints the game's readback source, destination, njpeg input and copy size |
| `debug/probes/scene.cjs` | a `cathedral` route and a `waitLog` route step (wait for a page-log line) |
| `docs/proofs/web-newgame-cathedral-session203.png` | the fixed cathedral, with the dialogue box |
| `docs/proofs/web-newgame-cathedral-backdrop-session203.png` | the fixed backdrop alone, before the dialogue box draws |
| `docs/proofs/web-newgame-cathedral-black-session203.png` | the black first pass, before the row/reference fixes |
| `docs/proofs/web-newgame-cathedral-green-session203.png` | the exchanged luma/chroma, with the writeback forced |
| `docs/HANDOFF-2026-10-30-session203.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/guides/web-probes.md` | status, log, decision, index, the `OGRE_YUV_TRACE` browser output and the route |

No `tools/RT64` file was edited. `m tools/RT64` in `git status` is the
pre-existing dirt.

## Probes used and reverted

The guard was compiled out once for the A/B (`if (false && fb.rdram_hash_valid)`)
and is restored; the tree has the guard. No probe was written into
`RecompiledFuncs/` or `Bank*Funcs/`. The kept diagnostics are the `OGRE_YUV_TRACE`
lines and the richer `[GFX-S2D]`/`[GFX-FBSYNC]` lines.

## Open

1. **The exact `3. Combat` lesson** (session 202's item 1). `5. Unit Commands`
   contains a battle and renders; `3. Combat` is two items above it in the
   C-Up/C-Down list.
2. **The writeback reference still hashes the whole target.** The row band is now
   correct, so the copied bytes stay inside the image, but the hash span is still
   `row_bytes * fb.height` and can cover a neighbouring image. A per-era hash over
   the drawn rows would narrow it. Nothing reachable uses it yet.
3. **`testdraw`'s positive control** (session 202's item 3): its scratch sits at
   12 MiB, above the 8 MiB guest RDRAM, so `fbtex hits=0` on a fresh build.
4. **The cathedral's second assembly.** The run reaches `0x02` again at
   `t≈156 s`; the backdrop for that visit was not captured.
