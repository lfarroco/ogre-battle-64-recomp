# Session 159 handoff — the layers' decoded content and alpha agree with the native, so the title's band residual is in the composition; the two runs' draw lists differ

## Goal

Continue the browser (WebAssembly) renderer from session 158. That session left
two survivors for the title screen's band residual: (a) the alpha the sampler
uses for the cloud image `0x217508`, and (b) the destination the cloud blends
onto. Its suggested cheapest test was one native run with
`OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1` to compare the native's per-strip
decoded `mean`/`opaque` with the browser's `[GFX-TEXTRACE]` for the same strips:
equal numbers put the difference in the composition, different numbers put it in
the image content or alpha.

The session had to run that comparison, name the mechanism with a one-variable
A/B if the answer was (b), implement it and measure, or record the negative
result with the probe output and revert every probe. Timebox: 100 minutes.

## Result

1. **Session 158's tree claims hold.** `grep -rn probe158 app/ debug/` prints
   nothing and exits 1. The uncommitted diff is the accumulated work of sessions
   141..157 (`app/src/web_renderer.cpp` 244 lines, which is the `[GFX-TEXTRACE]`,
   `GFX-VI`/`GFX-WIDE`/presenter work and the session-145 RGBA32 decode fix) and
   carries no probe text, which matches session 158's "no renderer change" and
   "the probe was added and then removed". `/tmp/w156` still holds both series
   and the baseline recomputes exactly:

   ```
   python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1
     meanAbs=[2.08, 2.23, 2.74] meanSigned=[-2.03, -2.17, -2.67] exact=79.2% over 318x238
   ```

2. **Session 158's correction of session 157 is confirmed against the trace it
   names.** `/tmp/w158/probe-milestones.txt` holds 22 `rec` lines, all
   `img=0x249538`, all `rect=0,Y..200,Y+5` and `up=200x5`, with
   `win=0,uls..796,lrt` (for example `win=0,120..796,136`). A 200-pixel-wide
   rectangle over a window whose `lrs=796` is texels `0..199`: the `s` span is
   0..200 texels and there is no 320-texel span. `PLAN.md`'s status paragraph
   quotes session 157's number and corrects it in the same sentence, item 8(n)
   records the correction, and `docs/DECISIONS.md` has no 320-texel statement
   about this draw (its only "320 texels" hits are the tile-line-unit note of
   session 141 and the map-sheet note of session 64, both about other screens).

3. **The suggested native run cannot print the cloud's numbers as it stands.**
   `tex_trace::colour_mean` decodes CI8 and RGBA16 only
   (`app/src/renderer.cpp:637-646`: a non-indexed tile whose `siz` is not 2
   returns false), so `fmt=0 siz=3` RGBA32 draws print no `colour:` line. The
   first two native runs of this session printed 0 `colour:` lines for the 11546
   `0x80217508` draws they walked. The comparison therefore needed a decode of
   RGBA32, which this session added as a tagged probe (result 4) and reverted
   (result 8).

4. **The browser and the native decode the same content and the same alpha for
   every strip of every layer of this screen.** With the probe in place
   (`probe159`, one extra line per RGBA32 draw: the tile window decoded as
   `x0 = uls>>2, y0 = ult>>2, w = (lrs>>2)-x0+1` over `bytes_per_row()` =
   `(imgw+1)*4`, reporting the RGB mean, the mean alpha, the count with
   `alpha >= 128` and the count with `alpha == 255`), the native's numbers and
   the browser's `[GFX-TEXTRACE]` numbers were compared window by window:

   ```
   native windows: 113  browser draws: 559
   match 444 mismatch 0
   cloud windows: 54  with >1 native sample: 54
     ('217508', 0, 0, 319, 2) 53 samples distinct(rgbmean,opaque) [(56.8, 0)]
     ('217508', 0, 3, 319, 5) 53 samples distinct(rgbmean,opaque) [(58.7, 15)]
     ('217508', 0, 6, 319, 8) 53 samples distinct(rgbmean,opaque) [(61.3, 33)]
   ```

   A match is `|browser mean - native mean| < 0.15` and an equal `opaque` count.
   444 draw windows matched and none differed. The cloud image's content is
   constant over the 53 native frames the trace armed for (2 s at 24 display
   lists/s), and it equals the browser's decode of the same windows, so the
   cloud is a static image and both renderers hold the same RGBA32 bytes at
   `0x217508`. The same holds for `0x1E2918` (66.7/0, 67.1/18, 70.6/56 in
   both) and for the fog `0x249538` (`mean 0.0` in both, which is the fog's
   texel RGB: the fog's drawn colour comes from its combiner, not its texture).
   Survivor (a), the alpha the sampler uses for `0x217508`, is therefore
   excluded at the decode level: the browser's decoded RGB and alpha per window
   are the native's.

5. **The residual's shape, measured.** On the session-156 pair
   (`title-03.png` against `t.574.bin`), regressions of native on browser per
   channel-1 with the presenter's edge inset (320x240 coordinates):

   ```
   rows 120..189  all x     n=22400 web=82.0 nat=81.9 slope=1.001 corr=1.000
   rows 190..238  x 0..139  n=6860  web=23.3 nat=23.3 slope=1.000 corr=1.000
   rows 190..238  x>=140    n=8820  web=42.8 nat=53.8 slope=1.008 corr=0.858
   rows  80..119  all x     n=12800 web=128.6 nat=133.9 slope=0.918 corr=0.990
   ```

   The per-row difference in rows 186..239 is zero at rows 186..193, rises from
   row 194, peaks at +12.4 mean signed around rows 205..225, and is back to zero
   at row 234; the differing pixels start at a slanted left edge (row 200 at
   x≈168, row 210 at x≈152, row 225 at x≈184) and run to x=319. Over the 5116
   differing pixels of rows 195..232 the mean difference is +18.9 (native
   brighter). Two hypotheses are excluded by the same pixels: it is not a
   translation (the identity shift matches 1268 of 6384 samples, and each of
   +-1 in x and y matches 518 or fewer) and it is not an alpha scale at the
   composite (correlation with the browser's own value 0.011, and
   `d/(255-value)` has mean 0.089 with sd 0.049, which is not a constant). Every
   differing pixel is on a row and a column where the layer under test has
   partial alpha, and rows 120..189 are exact because `0x217508` is opaque
   there, which is session 157's and 158's finding restated with the decode now
   verified.

6. **The two runs' display lists are not the same list, which is why the answer
   came from the window decodes and not from a draw-for-draw comparison.** The
   browser's list is 135 textured draws; the native's at `t=10s` is 590:

   ```
   browser dl=2 (the first four lists have the same count): 135 draws
     sky 0x1C3EA8: 15 full-width 320x6 strips + 64x64 tiles at x=0,64,128,192,256,320
     cloud 0x217508: 53 full-width 320x3 strips, rows 80..239
     fog 0x249538: 23 strips of 200x5, rows 126..238
   native dl=51: 590 draws, 502 of them 0x217508, none degenerate
     sky: 64x64 tiles at x=0,13,77,141,205,269 (the first 13 wide: a 13-pixel scroll)
     cloud: 54 full-width 320x3 strips (rows 80..239) plus 7 columns of 72 pixels
            (x=0,42,114,186,258,330,402) each drawn as 1-pixel-high strips
   ```

   The 455 draws the browser does not have are the 72-pixel columns at
   `x = 402..474`, which lie outside the 320-wide colour image the title presents
   and inside a wider one the browser never sets, so they cannot reach the frame
   that is compared. The full-width cloud strips are 54 in both and cover rows
   80..239 with no row drawn twice. The two runs are at different points of the
   attract animation (the 13-pixel scroll), and that is not the source of the
   residual: session 156 measured adjacent native phases of the same screen as
   differing by 0.46-3.01 levels in these bands, and the difference here is
   8-19. It does mean a difference cannot be attributed to one layer by comparing
   the two runs' lists; a per-draw readback inside one renderer is the instrument
   that can.

7. **No renderer behaviour changed, so no before/after frame pair exists.** The
   probes only read state, and `git diff --stat app/` after the revert is the
   pre-session state (213 insertions, 37 deletions). The reverted build's fresh
   browser series is the frame the pre-session tree produces: its best pair over
   the native series is `[2.37,2.58,3.23] / 80.3%` with rows 80..119 `+7.99`,
   rows 190..238 `+6.60` and rows 120..189 `-0.03`, which is the baseline's shape
   inside the run-to-run spread (session 158's fresh series was
   `[2.32,2.52,3.14] / 81.3%`). The publisher still is `[0.87,0.84,0.75]` /
   99.6% and `boot.cjs` is `RENDERED` `nonBlack 25136`.

8. **The negative result and the probes' revert.** The session's answer to
   session 158's question is (b): the content and the alpha of every layer of
   this screen agree between the two renderers, so what remains is the
   composition — the destination a partial-alpha layer blends onto, or the order
   and coverage with which the list writes it. The mechanism is not named: the
   two runs' lists draw the same layers with different geometry and at different
   animation instants (result 6), so the one-variable A/B the session needed
   would first have to make those comparable. Both probes are reverted
   (`grep -rn probe159 app/ debug/` is empty, exit 1) and `git diff --stat app/`
   is back to `web_platform.cpp` 6 and `web_renderer.cpp` 244, the pre-session
   figures.

   The reference here is RT64's own render target; a match to it is a browser
   result and not a statement about retail hardware.

## Files changed

| file | change |
|---|---|
| `app/src/renderer.cpp` | the tagged probe (`probe159`: `probe159_rgba32`, a window decode of an RGBA32 tile, and one `[tex] probe159 img=... win=WxH rgbmean=... amean=... opaque=... full=...` line per RGBA32 draw) was added for the measurement and then removed. `git diff` on this file is empty. |
| `app/src/web_renderer.cpp` | the tagged probe (`probe159`: `TileState::trace_amean`/`trace_full`, the alpha statistics of the same decoded window, and two extra fields in the `[GFX-TEXTRACE]` line) was added and then removed. The file's diff is the pre-session 244 lines. |
| `docs/HANDOFF-2026-10-17-session159.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, decision row; item 8(n) records the exclusion of (a) and the list-structure finding |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or `tools/RT64` was
changed. No commit was made. No `docs/proofs/` image was added or replaced,
because no rendered frame changed.

A note on the tree's state after the verification: at 15:38, thirteen minutes
after the reverted `make web` finished, `app/src/web_renderer.cpp` gained a
`probe160` block (a TLUT-destination and CI-tile-tuple trace) and the
`G_TEXTRACE`-adjacent debug-flag bit 6 controls. This session did not write those
lines and did not revert them; they are another writer's work in the same
checkout. The verified build (15:25) was made from the reverted sources, and the
figures in this handoff belong to it.

## What was run for verification

- `grep -rn probe158 app/ debug/` — no output, exit 1.
- `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png
  /tmp/w156/nat2/t.574.bin --inset 1` — `meanAbs=[2.08,2.23,2.74]
  meanSigned=[-2.03,-2.17,-2.67] exact=79.2% over 318x238`.
- `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title
  OGRE_SCENE_LOG=1 OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1
  OGRE_EXIT_AFTER_MS=32000 ./build-dist/ogrebattle64 assets/ogre64.z64`
  — 0 `colour:` lines for `timg=0x80217508` (result 3). `OGRE_MOD_TEST=1` is
  required: without it the three mods in `build-app/mods` open the start screen
  and the run writes no capture.
- `OGRE_TEX_TRACE_AFTER_MS=10000 OGRE_TEX_TRACE_MS=2000 OGRE_TEX_TRACE_MAX=9000
  OGRE_EXIT_AFTER_MS=26000` on the same command — armed at `t=10002ms`, walked
  590 rectangles in `dl=51`, 30051 `probe159` lines, 113 distinct windows
  (`/tmp/w159/native-p159.txt`). A first attempt with `AFTER_MS=11200` and
  `EXIT_AFTER_MS=14000` never armed: the trace clock reaches about 9.8 s in a
  16 s run.
- `node debug/probes/shots.cjs --url
  'http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TEX_TRACE=1'
  --attempts 2 --secs 200 --shots 2 --gap 400 --out /tmp/w159/web`
  — `CAPTURED 2 frames (attempt 1)`, 559 `[GFX-TEXTRACE]` draws over five display
  lists (`/tmp/w159/web-milestones.txt`).
- The window-by-window comparison is a Python parse of those two files
  (`/tmp/w159/cmp159.py`): 444 matches, 0 mismatches.
- The band regressions and the shift/value tests are `tools/web_frame_cmp.py`'s
  `canvas_pixels`/`target_pixels` on the session-156 pair
  (`/tmp/w159/an159.py`, `/tmp/w159/an4.py`).
- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `web:
  build-wasm/ogrebattle64.wasm is ready`, three times: the pre-session build, the
  probe build, and the build after the revert.
- `make app -j8` — builds `build-dist/ogrebattle64` in this environment
  (`DIST_STATIC_SDL` is set for this checkout, so `make app` writes build-dist,
  not build-app), twice: the probe build and the build after the revert.
- `grep -rn probe159 app/ debug/` after the revert — no output, exit 1, and
  `git diff --stat app/` is `web_platform.cpp` 6 and `web_renderer.cpp` 244
  (213 insertions, 37 deletions), the pre-session figures; `git diff
  app/src/renderer.cpp` is empty.
- `node debug/probes/shots.cjs --url
  'http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1'
  --attempts 2 --secs 200 --shots 3 --gap 400 --out /tmp/w159/final`
  on the reverted build — `CAPTURED 3 frames`; the best pair over the 525-frame
  native series is `final-00` against `t.252` at
  `meanAbs=[2.37,2.58,3.23] meanSigned=[-2.35,-2.55,-3.18] exact=80.3%`, and its
  row profile is rows 80..119 `+7.99`, rows 190..238 `+6.60`, rows 120..189
  `-0.03` (native minus browser). That is the same shape as the recorded
  baseline and the same run-to-run spread session 158 measured on its own fresh
  series (`[2.32,2.52,3.14] / 81.3%`), because the captured browser frame lands
  at a different phase of the attract animation in each run.
- `python3 tools/web_frame_cmp.py /tmp/w147/final2-00.png
  /tmp/w146/nat/tt.140.bin --buffer 640x476 --target-size 640x480` — the
  publisher still is unchanged: `meanAbs=[0.87,0.84,0.75]
  meanSigned=[-0.87,-0.84,-0.75] exact=99.6% over 640x476`.
- `node debug/probes/boot.cjs --attempts 2 --secs 45 --out /tmp/w159/boot-final2`
  on the reverted build — `RENDERED {"w":640,"h":480,"total":307200,
  "nonBlack":25136,...}`. A first run of the same check reported `nonBlack
  24788` with `sawRealDl=false` on attempt 1, which is the boot screen's fade
  caught at a different frame; the re-run reproduces session 158's `25136`.

There is no repo test harness; each check is a ROM-dependent run. The native
series and the publisher target are session 156's and session 146's, still under
`/tmp/w156` and `/tmp/w146`, and were reused because no renderer behaviour
changed.

## Open

1. **The composition beneath the partial-alpha layers.** The content and alpha of
   `0x217508`, `0x1E2918`, `0x1C3EA8` and `0x249538` agree with the native
   (result 4), so what remains is the destination and the coverage. The
   instrument that can name it now is a per-draw readback inside one display
   list: read one target row back after each textured draw of the list (the
   browser's `flush_commands` loop has the draws in order) and print the row's
   pixels per draw, then invert the last blend against the native's texels for
   the same row. The native side of that comparison needs the same run's RDRAM
   (`OGRE_DUMP_RDRAM` or `OGRE_TEX_TRACE_DUMP`), not another frame, because the
   two runs' lists differ (result 6).
2. **The two runs' display lists are not the same list.** The browser draws the
   sky as 64x64 tiles at `x = 0,64,128,...` and the cloud as 53 full-width 320x3
   strips; the native at `t=10s` draws the sky at `x = 0,13,77,...` and adds 455
   draws of the cloud as 72-pixel columns at `x = 402..474`, outside the 320-wide
   colour image the title presents. The attract screen's own scroll is the reason
   the two lists differ; it is not a renderer difference, and session 156's
   0.46-3.01-level phase bound keeps it out of the residual. A draw-for-draw
   comparison needs the two sides at the same animation instant, which is why the
   next instrument reads one renderer's own target row against that same run's
   RDRAM.
3. **The unwritten last column and row, and the canvas's rounded corners.**
   Unchanged from session 157: cosmetic, and the canvas's CSS box needs the
   developer's intent.
4. **The LOADTLUT destination key** is still the load tile's `palette` field
   where RT64 uses its `tmem`; they agree for every traced load.
5. **The screens not compared against the reference** are unchanged.

## Correction (session 161)

The "renderer draws the cloud layers differently" conclusion here is corrected.
The two runs do share a scroll state: at the third display list of each run the
135 draws whose tile window is non-degenerate are identical in order, image,
rectangle, `s`, `t`, `dsdx`, `dtdy` and window (the `0x1C3EA8` partial tile is
`rect=0,16..63,80 s=32` in both). Every native list also has 456 draws with a
degenerate tile window (`ult == lrt`): the mirrored `0x1E2918` pieces and the
72-pixel `0x217508` columns. Those are the native app's `OGRE_FOG` repair pieces
(`app/src/renderer.cpp`, `nop_rect::Walker::fog_fix`), which the browser skips at
its own empty-tile guard, so the browser draws no fog layer. Two native series
differing only in `OGRE_FOG=0` give a fog contribution of 3.4 mean abs (rows
64..119 `5.68`), and the browser frame matches the fog-off series at
`meanAbs=0.052` (rows 0.046/0.072/0.044) against `2.349` against the fog-on
series. The residual is the missing fog layer, not the sampler, the blend or the
phase. See `docs/HANDOFF-2026-10-17-session161.md`.
