# Session 162 handoff — the `OGRE_FOG` repair is ported to the browser display-list path

## Goal

Continue the browser (WebAssembly) renderer from session 161. Verify session 161's
fog-off claim, then port the native `OGRE_FOG` repair
(`app/src/renderer.cpp`, `nop_rect::Walker::fog_fix` + `namespace fog::repair`)
to `app/src/web_renderer.cpp`, and measure the title frame against the native
fog-on and fog-off series.

Timebox: 100 minutes.

## Result

1. **Session 161 verifies exactly, from its own artifacts.**
   `/tmp/w161/canvas-title03.rgb` against `/tmp/w156/nat2` gives
   `best t.573.bin meanAbs=2.349` (bands 0.046/4.153/2.668) and against
   `/tmp/w161/fogoff32` `best t.584.bin meanAbs=0.052` (0.046/0.072/0.044), so
   the fog repair alone accounts for the title residual. The repository tool
   reproduces both: `tools/web_frame_cmp.py /tmp/w156/web/title-03.png
   /tmp/w156/nat2/t.573.bin --inset 1` is `[2.08,2.23,2.74] meanSigned
   [-2.03,-2.17,-2.67] exact=79.2%`, and the same frame against
   `fogoff32/t.584.bin` is `[0.05,0.05,0.06] exact=94.8%`.

2. **The repair rule, which is the spec that was ported.** OB64 draws each
   scrolling fog/cloud layer as `G_TEXRECT`s with the white combiner
   (`G_SETCOMBINE` w1 = `0xFFFF73B9`, RGB ONE / ALPHA TEXEL0). The draws inherit
   the last `G_SETTILESIZE t0` of the cloud loop, whose window covers no texels
   (`lrs == uls` or `lrt == ult`), so the sample collapses onto one stale TMEM
   row. The layer table at `D_801B80E0` holds a pointer; layer `i`'s record is at
   `table + 0xFC + i*0x24`, and its first word is an image record whose logical
   byte 2 is `fmt`, byte 3 is `siz`, the 16-bit fields at logical 4 and 6 are
   height and width, and `+8` is the image data. A white-combiner texrect with a
   zero-area window is repaired by rewriting, on the same RDRAM bytes the
   renderer will parse, (a) the inherited `G_SETTIMG` to the layer image's
   `fmt`/`siz`/width and address, (b) `G_SETTILE t7` and `G_SETTILE t0` to that
   image at TMEM 0 with `line = bytesPerRow/8` (tile 0 carries
   `maskt`/`masks` = log2 of the image's height/width so the sweep's `s` past 64
   wraps), (c) the `G_LOADTILE` to the whole image, and (d) `G_SETTILESIZE t0`
   to `((width-1)*4) << 12 | ((height-1)*4)` in quarter-texel units. Layer
   indices advance per repaired group: 0 for the first, then 2, 3, .... A group
   whose rect `s`/`t` is negative cannot be repaired and keeps the loop's own row
   with a one-texel `SETTILESIZE`. Native references: `renderer.cpp:239-408`
   (the walker's state: `fog_settimg`, `fog_settile7`, `fog_loadtile`,
   `fog_settile0`, `fog_tilesize`, `fog_white = (w1 == 0xFFFF73B9)`,
   `fog_layer`), `renderer.cpp:1003-1219` (`kLayerTable`, `layer_image`,
   `read_image`, `repair`), and `renderer.cpp:1707-1749` (the per-list call).
   The default `OGRE_FOG_SCALE=100` path uses the asset address directly; the
   scratch staging is only for non-default scale values.

3. **The port is in `app/src/web_renderer.cpp` and produces the same draws.** A
   new `FogWalker` + `fog_repair_group` + `fog_repair_display_list` mirror the
   native walker byte for byte (same last-command tracking, same white test, same
   zero-area test, same layer table reads, same rewrite, same layer advance). It
   runs on the submitted list in RDRAM in `execute_dl` before `gbi::walk_dl`, so
   the browser decodes the repaired bytes exactly as RT64 decodes the native's
   repair. The title now queues 592 draws per list (the native trace has 591/590;
   the browser had 135 before), and the log prints
   `[GFX-FOG] task 2: repaired 2 fog group(s)`.
   Knob: `OGRE_FOG=0` disables it; default on. The page query string already
   publishes every `OGRE_*` name to `ENV` (`app/web/web.js`), so
   `?OGRE_FOG=0` works without a shell change.

4. **Measured against fresh native series, the band residual is gone.**
   Native fog-on: `/tmp/w162/nat-fog` (397 frames); native fog-off:
   `/tmp/w162/nat-fogoff` (587 frames); browser `shots.cjs --shots 40 --gap 400`
   on `?OGRE_SCENE=title`. Minimum over all pairs, `--inset 1`:

   ```
   fog ON   browser vs native fog-on   : t.177 meanAbs=[0.09,0.09,0.12] exact=91.5%
            bands 0.014 / 0.273 / 0.060 (rows 0-63 / 64-119 / 120-239)
   fog OFF  browser vs native fog-off  : t.279 meanAbs=[0.03,0.03,0.03] exact=97.3%
            bands 0.042 / 0.011 / 0.026
   control  fog-on canvas vs fog-off   : 2.740 (bands 0.014 / 5.660 / 2.753)
   control  fog-off canvas vs fog-on   : 2.733 (bands 0.042 / 5.685 / 2.711)
   ```

   The fog-on comparison was `[2.08,2.23,2.74]` in session 161 and is
   `[0.09,0.09,0.12]` now. The 0.273 in rows 64..119 is not a geometry or phase
   error: the per-band fog overlay (canvas fog-on minus canvas fog-off) is
   `-0.683 / 5.691 / -0.007 / 6.719` against the native overlay's
   `-0.674 / 5.705 / -0.007 / 6.637`, and every differing pixel is 0-3 levels
   (78.6% exact, 17.1% one level, 3.0% two, 0.8% three). The band-minimising
   native frame `t.185` gives 0.074 in rows 64..119 with 92.6% exact and all
   differences exactly one level. That is the browser's 8-bit GL blend rounding
   against RT64's float-to-RGBA16 conversion, and it shows only where the fog
   blends: the fog-free rows 0..63 are 98.5% exact.

5. **No regression in the other browser screens.** The publisher still is
   `[0.0,0.0,0.0] / 100.0%` over 639x475 against `/tmp/w146/nat/tt.140.bin`, with
   no `[GFX-FOG]` line in its page log. `boot.cjs` is `RENDERED` with
   `nonBlack=25104` (session 160's number), and the attract story's fill-only
   capture keeps the black letterbox and the `(74,90,0)` interior, with no
   `[GFX-FOG]` line. So the repair fires on the title and nowhere on those
   screens.

## The repair, in one paragraph

For each submitted display list, before the walker reads it, walk the list and
track the last `G_SETTIMG`, `G_SETTILE t7`, `G_LOADTILE`/`G_LOADBLOCK`,
`G_SETTILE t0`, `G_SETTILESIZE t0` and the last `G_SETCOMBINE`. When a
`G_TEXRECT`/`G_TEXRECTFLIP` follows a combiner whose second word is
`0xFFFF73B9`, it is a fog piece; if its inherited tile window is zero-area, read
layer `layer`'s image record from the table at `D_801B80E0`
(`table + 0xFC + layer*0x24` -> image record -> `fmt`, `siz`, width, height,
`+8` data), rewrite the inherited `SETTIMG`/`SETTILE t7`/`SETTILE t0`/`LOADTILE`/
`SETTILESIZE t0` to sample that whole image (with `masks`/`maskt` so the wrap
pieces past the image edge wrap instead of clamping), and advance `layer` so the
next group reads the next record (0, 2, 3, ...). A group with a negative `s` or
`t` keeps a one-row tile instead, because its coordinates name a row the game
itself left in TMEM.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `constexpr kFogLayerTable`, `FogImage`, `fog_layer_image`, `fog_read_image`, `fog_repair_group`, `FogWalker`, `fog_repair_display_list`; one call in `execute_dl` before `gbi::walk_dl` |
| `docs/HANDOFF-2026-10-17-session162.md` | this file |
| `PLAN.md` | Status bullet; Browser-port session-162 paragraph; open-work item 8 subitem (o) closes the title fog item |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | log entry, index row, decision row for the port and the knob |
| `docs/guides/app-build.md` | browser knob paragraph: `OGRE_FOG=0` |
| `docs/proofs/web-title-fog-before-after.png` | pre-port browser title (session 160's capture, same attract instant) above the fog-on browser frame |
| `docs/proofs/web-title-fog-vs-native.png` | browser fog-on frame above native `t.177` |
| `docs/proofs/web-title-fog-diff.png` | amplified difference of browser fog-on against native `t.177` |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made. No probe was added, so
there is nothing to revert; `grep -rn probe162 app/ debug/` is empty.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — `[100%] Built target
  ogrebattle64`, `web: build-wasm/ogrebattle64.wasm is ready`.
- `python3 debug/server.py` (already running on `http://127.0.0.1:8931`).
- Browser title series, repair on:
  `OGRE_URL='http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1'
  node debug/probes/shots.cjs --shots 40 --gap 400 --out /tmp/w162/web-fog`.
  40 frames captured; tasks=16. With `&OGRE_FOG=0`, 40 frames, tasks=32. On this
  build each run's first flush takes the whole shot window, so each series is one
  presented instant repeated; the frame is paired with every native target, which
  is what the minima above are.
- Native fog-on:
  `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title
  OGRE_SCENE_LOG=1 OGRE_CAPTURE_TARGET=/tmp/w162/nat-fog/t OGRE_CAPTURE_AFTER=100
  OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=32000 ./build-dist/ogrebattle64
  assets/ogre64.z64` (397 frames in 32 s). Native fog-off: the same with
  `OGRE_FOG=0` and `/tmp/w162/nat-fogoff` (587 frames).
- Comparisons: `tools/web_frame_cmp.py /tmp/w162/web-fog-03.png
  /tmp/w162/nat-fog/t.177.bin --inset 1 --canvas-size 319x239` and the same
  against `/tmp/w162/nat-fogoff/t.279.bin`; the 500-target minima use
  `/tmp/w162/torgb.py` (a canvas PNG to raw RGB through the tool's own reader)
  and `/tmp/w161/cmp3` (session 161's C comparator, same math as the tool; a
  500-target Python search is minutes per frame). Both routes agree on the
  session-161 numbers.
- Publisher: `node debug/probes/palette.cjs --query
  'OGRE_SCENE=publishers&OGRE_SCENE_LOG=1' --out /tmp/w162/pub --secs 40
  --attempts 4 --shots 2`, then `tools/web_frame_cmp.py /tmp/w162/pub-00.png
  /tmp/w146/nat/tt.140.bin --target-size 640x480 --canvas-size 639x475` —
  `[0.0,0.0,0.0] / 100.0%`.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` — `RENDERED`,
  `nonBlack=25104`.
- `node debug/probes/fills.cjs --query 'OGRE_SCENE=story&OGRE_SCENE_LOG=1'
  --out /tmp/w162/fills --secs 70` — `OK`, fills at `(0,0,0)` for the letterbox
  rows and `(74,90,0)` for the scissored interior, no `[GFX-FOG]` line.
- `grep -rn probe162 app/ debug/` — no output, exit 1.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The fog-on band residual is 0.273 in rows 64..119 against a 0.014 fog-free
   band, and it is blend rounding.** The fog overlay itself matches the native to
   0.02 per band, and the differing pixels are 0-3 levels with the browser one
   level below the native on about 10% of them. The browser blends in 8-bit GL;
   RT64 blends in float and converts to RGBA16. Closing it means matching RT64's
   colour conversion for blended draws, which is a renderer-wide change and not
   part of the fog repair.
2. **`OGRE_FOG_SCALE` is not ported.** The browser always samples the game's own
   image (`scale = 100`), which is the native default. Only a non-default scale
   (the scratch-staged copy) is missing.
3. **The 40-shot browser series samples one presented frame** because the first
   flush on this build takes the whole 16 s window (592 draws, 4373 commands).
   The comparison is still frame-to-frame; a series of many distinct browser
   instants needs a faster presenter, not a change to the repair.
4. The screens not compared against the reference are unchanged from session 161.
