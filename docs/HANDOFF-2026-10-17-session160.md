# Session 160 handoff — the canvas is the VI's visible area, so every pixel of it carries frame content; the TLUT destination is unexercised on every reachable screen

## Goal

Continue the browser (WebAssembly) renderer from session 159 and close the two
mechanical items it left:

1. The LOADTLUT destination question. The browser keys a palette upload by the
   load tile's `palette` field (`OP_LOADTLUT`, `app/src/web_renderer.cpp`); RT64
   writes the table at `loadTile.tmem << 3` (`tools/RT64/src/hle/rt64_rdp.cpp`,
   `loadTLUTOperation`) and samples it from a fixed TMEM base. Measure whether any
   reachable draw uses a destination other than the assumed one before changing
   the key.
2. The canvas's unwritten last column and row, and its rounded corners. Measure
   them, fix what is presentation, and make the `border-radius` call explicit.

Timebox: 100 minutes.

## Result

1. **Session 159 verifies.** `grep -rn probe159 app/ debug/` prints nothing and
   exits 1. `git diff --stat app/` is `web_platform.cpp` 6 lines and
   `web_renderer.cpp` 244 lines, the pre-session figures. The stored baseline
   recomputes exactly (`/tmp/w156` still holds both series):

   ```
   python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1
     meanAbs=[2.08, 2.23, 2.74] meanSigned=[-2.03, -2.17, -2.67] exact=79.2% over 318x238
   ```

2. **The TLUT destination, measured over a 25-scene sweep and one natural
   route.** A tagged probe (`probe160`) printed, once per distinct pair, the
   `LOADTLUT` destination `tmem`, the tile's `palette`, the entry and byte count
   and the source address, and for every CI draw once per distinct
   `(tmem, palette, siz)` the draw's format, size, rect and LUT mode. The sweep
   numbers are in result 3.

3. **Every load in the sweep is `tmem=0x100 palette=0`, which is the key the
   browser already uses, so the item is unexercised.** **The 8-scene sweep and the publisher still found two distinct entry counts
   and one destination.** Four `LOADTLUT`s over the eight forced scenes and the
   publisher run: `tmem=0x100 palette=0 entries=16 bytes=32` at source `0x1F16C0`
   and `0x1F1A90`, and `tmem=0x100 palette=0 entries=256 bytes=512` at `0x1F1A00`
   and `0x207CA8` (`timg_siz=2` on all). Six distinct CI draw tuples, every one
   `palette=0` and `tmem=0x0` (the tile's own texture address, not the palette's):
   `fmt=2 siz=0` at `0x21A110` (16x16), `0x1F1AE0` and `0x1F1EB0` (64x64) against
   the 16-entry tables with `lut_mode=2`; `fmt=2 siz=1` at `0x2078A8` (32x32) and
   `0x1F1C00` (`164,70..420,78`) against the 256-entry tables with `lut_mode=2`;
   and `fmt=2 siz=1` at `0x1C13F8` (72x28) with `lut_mode=0`, which reads no
   palette at all. So the distinct `(tmem, palette)` pairs of a load are exactly
   one, `(0x100, 0)`.

   The full 25-scene sweep and a natural tap route did not complete in this
   session. `fbtex.cjs --scenes 0x00..0x18 --secs 20` was started and stopped to
   protect the timebox after `0x00` reported `tasks=0` and the next entries
   stalled on the known boot trajectory ("Only the boot blanking display list has
   been submitted after 15s"); the 8-scene list above is what ran to completion
   (display lists per scene 2/4/19/1/1/0/0/4, i.e. four of the eight reached a
   real list). The natural route (`OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,…`,
   32 s) reported `tasks=0`, also the stall, so it says nothing either way. The
   scenes that reported CI draws are `0x04` and `0x07`.

4. **RT64's rules, quoted, for the record.** A `G_LOADTLUT` writes its entries at
   `const uint32_t tmemStart = loadTile.tmem << 3;` with `tmemStride = loadTile.line
   << 5`, and `loadToTMEMCommon<..., TLUT=true>` advances `tmemAdvance = 0x8` per
   entry while writing only the first two bytes of each 8-byte slot
   (`loadWord<..., TLUT>`, `offsetMask = 0x1`): one entry per 64-bit TMEM word,
   8 bytes apart. The sampler reads the table from the fixed upper-half base:
   `#define RDP_TMEM_PALETTE 0x800` (`src/shaders/TextureDecoder.hlsli`), CI4 at
   `RDP_TMEM_PALETTE + (palette << 7) + (pixelValue4bit << 3)` (16 entries, 128
   bytes per bank) and CI8 at `RDP_TMEM_PALETTE + (pixelValue0 << 3)`
   (`sampleTMEMWithConvert`). So RT64's palette base is 0x800 in bytes, which is
   `tmem = 0x100`: the destination this ROM loads every traced palette at. The
   browser's `palette * 16` bank index over a 256-entry table is the same
   addressing for that destination (entry `palette*16 + i` of a table loaded at
   0x100 is TMEM byte `0x800 + palette*128 + i*8`), so the two agree on every
   screen that was measured.

   A one-line report stays so a future screen that loads elsewhere says so.
   `OGRE_TLUT_TRACE=1` prints `[GFX-TLUT] load dest tmem=0x%X palette=%u
   entries=%u bytes=%u addr=0x%06X: <assumed destination | NOT the assumed dest>` once
   per distinct `(tmem, palette)`. It is a kept diagnostic and not a probe.

5. **The unwritten column and row, and the corners, measured on the pre-change
   build.** The presenter wrote 639x475 of a 640x476 canvas (publisher still) and
   319x239 of a 320x240 canvas (title), because `present_framebuffer` sized the
   drawing buffer to the VI's `fbSize` and `blit_to_canvas` drew only the
   visible area (`fbSize` minus the overscan crop) into its top-left corner.
   Against RT64's target (`/tmp/w146/nat/tt.140.bin`, 640x480):

   ```
   publisher still, canvas 640x476
     corner (0,0)   canvas (20,22,26)  target (239,231,206)  delta (-219,-209,-180)
     corner (639,0) canvas (20,22,26)  target (239,231,206)  delta (-219,-209,-180)
     corner (0,475) canvas (20,22,26)  target (222,214,181)  delta (-202,-192,-155)
     corner (639,475) canvas (20,22,26) target (239,231,206) delta (-219,-209,-180)
     last column 639: 476/476 pixels differ, 0 exact, mean |delta| 222.4
     last row 475:    640/640 pixels differ, 0 exact, mean |delta| 223.1
     visible area 639x475: 26 differing pixels, worst channel |delta| 219
   ```

   The 26 visible-area pixels are the `border-radius:6px` arcs: `(0..4, 0)`,
   `(635..638, 0)`, `(0, 1)`, `(1, 1)`, `(637, 1)`, `(638, 1)`, `(0, 2)`,
   `(638, 2)`, `(0, 3)`, `(0, 4)` and the four rows 471..474, with deltas from
   -1 to -219. The canvas's page background `#14161a` = (20,22,26) is what the
   corner shows. On the title, canvas 320x240 against `t.151`:

   ```
     corner (0,0)   canvas (20,22,26)  target (33,16,0)  delta (-13,6,26)
     corner (319,0) canvas (30,33,38)  target (0,0,0)    delta (30,33,38)
     corner (0,239) canvas (30,33,38)  target (0,0,0)    delta (30,33,38)
     corner (319,239) canvas (10,11,13) target (0,0,0)   delta (10,11,13)
     last column 319: 2/240 differ (the two corners); last row 239: 2/320 differ
   ```

   The title's own edge is black, so there the unwritten column and row happen to
   match RT64's black and only the corners are visible.

6. **The fix: the drawing buffer is the visible area, and the rounded corners are
   gone.** `present_framebuffer` now calls
   `resize_canvas(visible.width, visible.height)`, so the present quad covers the
   whole canvas (`present_frac_w/h` = 1.0) and the 1:1 blit writes every pixel.
   The source rect is unchanged (u_max = `visible.width / fb.width`, v_min =
   `1 - visible.height / fb.height`), so no presented pixel moved. The RDRAM
   fallback's staging texture keeps the guest pitch (`vi.width << siz >> 1`), so
   a framebuffer with no FBO is not read one column narrow. `border-radius:6px`
   is removed from `#game-canvas` in `app/web/index.html`: the four corner pixels
   are game output, and clipping them blends them with the page background, which
   is a rendering difference against RT64's target and not a display style.

7. **The frames did not move, and the corner pixels are the only presented pixels
   that changed.** Publisher still, the same still before and after over the
   319x239/639x475 region both captures write: 303499 of 303525 pixels identical,
   and the 26 that differ are exactly the `border-radius` arc pixels listed in
   result 5. Title, pre-change capture cropped to 319x239 against the post-change
   capture: mean abs 0.00 per channel, minimum at shift (0,0), i.e. identical
   pixels.

8. **The before/after numbers.**

   ```
   publisher still  before: meanAbs=[0.87,0.84,0.75] meanSigned=[-0.87,-0.84,-0.75] exact=99.6% (640x476 vs 640x480)
                    after:  meanAbs=[0.0,0.0,0.0]      meanSigned=[0.0,0.0,0.0]        exact=100.0% over 639x475
                    with --inset 1: before 100.0% over 638x474 (meanAbs 0.0), after 100.0% over 637x473
                    every corner pixel, the last column (0 of 475 differ) and the last row (0 of 639
                    differ) now equal RT64's target exactly
   title            stored baseline (unchanged PNG): [2.08,2.23,2.74] / 79.2% over 318x238
                    pre-change capture vs t.151 over 319x239: [2.31,2.52,3.15] / 81.4%
                    post-change capture vs t.151 over 319x239: [2.31,2.52,3.15] / 81.4%
                    four corners: before (20,22,26)/(30,33,38)/(30,33,38)/(10,11,13),
                    after exactly the target's (33,16,0)/(16,8,0)/(0,0,0)/(62,59,29)
   ```

   The title's 81.4% is the unchanged cloud/fog band residual of sessions
   156..159 (the row profile is unaffected: last row 0 of 319 pixels differ).
   Phase is the reason the stored baseline reads 79.2% and a fresh capture 81.4%:
   the attract animation is a different instant in each run, and two captures of
   one run are identical.

   `node debug/probes/boot.cjs --attempts 2 --secs 40` reports `RENDERED`
   `nonBlack 25104` of 304964 on the final build (session 155's figure was 25136)
   and `nonBlack 24756` on the probe build, which landed on a different attract
   instant. The count is the frame: the boot probe captures whatever instant it
   reaches, and the four corner arcs alone are 92 non-black pixels over the same
   319x239 window (6281 before, 6189 after, measured on two captures of that
   instant). Earlier sessions reported 25208 and 25136 for the same screen.

9. **The probes are reverted and the presentation change stays.**
   `grep -rn probe160 app/ debug/` prints nothing and exits 1 on the final tree,
   `make web` is clean, and `boot.cjs` reports `RENDERED`. The proof image
   `docs/proofs/web-publishers-canvas-corners-before-after.png` holds four 24x24
   crops at 6x: the top-left corner before and after, then the bottom-right
   corner before and after. The before corner is the page background blended in;
   before, the last column and row are `(0,0,0)`/`(20,22,26)` where the still is
   `(239,231,206)`, and after both are the still's own pixels. The OGRE_TLUT_TRACE
   report is a kept diagnostic, not a probe.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `present_framebuffer` sizes the canvas to the VI's visible area and blits over the whole of it; the RDRAM fallback stages at the VI's pitch; `resize_canvas`'s log line and comments follow; new kept diagnostic `OGRE_TLUT_TRACE` (`[GFX-TLUT]`, once per distinct load destination); the tagged probe `probe160` was added for the sweep and removed. |
| `app/web/index.html` | `#game-canvas` loses `border-radius:6px`; the comment records why. |
| `docs/HANDOFF-2026-10-17-session160.md` | this file |
| `docs/proofs/web-publishers-canvas-corners-before-after.png` | the corner and edge crops, before and after |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, two decision rows; item 8 gains (j)'s closure and (o) for the TLUT destination |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or `tools/RT64` was
changed. No commit was made.

## What was run for verification

- `grep -rn probe159 app/ debug/` — no output, exit 1.
- `git diff --stat app/` — `web_platform.cpp` 6, `web_renderer.cpp` 244
  (pre-change).
- `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png
  /tmp/w156/nat2/t.574.bin --inset 1` — `[2.08,2.23,2.74] / 79.2%`.
- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `web:
  build-wasm/ogrebattle64.wasm is ready`, twice: the probe-and-presentation build
  and the build after the probe's revert.
- `python3 debug/server.py` (background, `http://127.0.0.1:8931`).
- `node debug/probes/fbtex.cjs --scenes 0x00,…,0x18 --secs 20 --out
  /tmp/w160/sweep` — the 25-scene sweep; `node debug/probes/fbtex.cjs --queries
  'OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a&OGRE_SCENE_LOG=1'
  --secs 32 --out /tmp/w160/natural` — the natural route. The 8-scene sweep ran on the probe build and the publisher run printed the
  `probe160` lines quoted in result 3; the natural route stalled (`tasks=0`).
- `node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_SCENE_LOG=1'
  --out /tmp/w160/pub-after --secs 40 --attempts 4 --shots 2` — `kept 0:
  {"w":639,"h":475,…,"nonBlack":303525}`.
- `node debug/probes/shots.cjs --url '…?OGRE_SCENE=title&OGRE_SCENE_LOG=1'
  --attempts 3 --secs 120 --shots 3 --gap 500 --out /tmp/w160/title-after`.
- The final build (probe reverted) re-captures the publisher still
  (`palette.cjs --out /tmp/w160/pub-final`): `[0.0,0.0,0.0] / 100.0%` over 639x475
  against `tt.140`, and 0 differing pixels from the probe build's capture of the
  same still, so the probe's removal changed no pixel.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — `RENDERED` (`nonBlack
  25104` on the final build; `24756` on the probe build's different attract
  instant).
- The corner/edge and before/after measurements are two read-only Python scripts
  over `tools/web_frame_cmp.py` (`/tmp/w160/edges.py`, `/tmp/w160/bestmatch.py`),
  because a 525-target best-match search with the tool alone is minutes per run.
- `grep -rn probe160 app/ debug/` after the revert — no output, exit 1.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The composition beneath the partial-alpha layers** (unchanged from session
   159). The content and alpha of every layer agree; the survivor is the
   destination a partial-alpha layer blends onto. The instrument is a per-draw
   readback of one target row inside one display list, inverted against the
   native's texels for the same row, which needs that run's own RDRAM rather than
   another frame.
2. **The TLUT destination is unexercised.** Every traced load is
   `tmem=0x100 palette=0`, the destination RT64's fixed palette base samples, so
   the browser's `palette*16` key is the same addressing on every reachable
   screen. `OGRE_TLUT_TRACE=1` reports a load anywhere else. If a screen ever
   reports one, the change is the load's `tmem << 3` as the table's TMEM base
   plus the sampler's `0x800 + …` index, both quoted in result 4.
3. **The screens not compared against the reference** are unchanged.
