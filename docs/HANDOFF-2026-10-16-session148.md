# Session 148 handoff — the publisher still sat two rows low because the canvas element's CSS box did not match its drawing buffer, and the browser resampled 640x476 into 640x480

## Goal

Continue the web renderer (developer, session start). Session 147's open item 1
was the publisher still: the browser canvas against RT64's `tt.140` target was
`meanAbs=[8.26,9.53,15.92]`, a whole-row shift search minimised at `dy=-1`
(`6.35` against `11.43` at `dy=0`), and the differing pixels were swaps of
neighbouring still colours. The two named candidates were the 640x480 canvas
element around a `visible=639x475` VI mode (a 476 -> 480 CSS stretch) and the
unapplied `fldRegs` x/y scale and offset.

## Result

1. **The offset is the element's CSS box, and it is a resample, not a renderer
   fault.** `app/web/index.html` pins `#game-canvas` at
   `style="width:640px; height:480px"`, and `present_framebuffer` sizes only the
   *drawing buffer* (`emscripten_set_canvas_element_size`), which for the
   publisher still's VI mode is 640x476. The browser scales the drawing buffer to
   the CSS box, and `image-rendering: pixelated` makes that a nearest-neighbour
   resample, so the frame on screen was the 476-row buffer stretched over 480
   rows. Nothing about the VI's origin, the `fldRegs` scale/offset or the
   presenter's sampling is involved: session 146's `visibleFramebufferSize` crop
   and session 142's exclusive texrect corners are correct.

2. **The measurement identifies the resample exactly.** On the session-147
   capture `docs/proofs/web-publishers-atlus-lutmode-fix.png` (element inner
   640x480) against RT64's `/tmp/w146/nat/tt.140.bin` (640x480), canvas row `Y`
   equals target row `floor((Y + 0.5) * 476 / 480)` for every row: mean abs
   `[0.87, 0.84, 0.75]` per channel and 99.6% of channel samples identical, and
   the crop offset is confirmed the same way (offsets 0 and 2 give `10.49` and
   `11.27`). The alternatives do not fit: a constant shift is best at `dy=+2`
   with `7.26` (the resample's average over the frame), and the identity is
   `10.38`. Session 147's own shift minimum (`dy=-1`, `6.35`) is not reproducible
   on this PNG with the documented 1-pixel border crop; on a resampled frame no
   single whole-row shift is a sharp minimum, because the best constant shift
   still leaves 6.4 `meanAbs` against the resample model's 0.82.

3. **The bounding box agrees with that and corrects two session-147 numbers.**
   The ATLUS strokes (colour `(57,90,165)`) are `x 156..471, y 200..283` in the
   canvas against `x 156..471, y 198..281` in RT64 — the same size, two rows
   lower, which is the stretch at that row and not a sampling or palette fault.
   Session 147 recorded `157..472 x 201..284` for the canvas (one pixel right,
   its crop) and counted the native strokes as 2459. Both frames have **9836**
   stroke pixels in the 640-wide space; 2459 is the native target subsampled by
   two.

4. **Fixed: the CSS box now follows the drawing buffer at a whole multiple.**
   `resize_canvas` (`app/src/web_renderer.cpp`) calls
   `emscripten_set_element_css_size("#game-canvas", width * scale, height * scale)`
   with `css_scale_for(height) = max(1, min(2, 480 / height))`. A 320x240 buffer
   keeps the 2x box index.html declared, so the title capture is unchanged; the
   publisher still's 640x476 buffer is shown 1:1. `[GFX-PRESENT]` prints the box:
   `canvas resized to 640x476 (css 640x476, x1) for the VI mode`.

5. **Verified on the fixed build.** The forced publisher still captures at inner
   640x476 and matches RT64's target at `meanAbs=[0.87,0.84,0.75]`, 99.6% exact
   over 640x476, with the whole-pixel shift search minimising at exactly
   `(dx=0, dy=0) = 0.82` and every other shift of the 5x5 grid at `>= 6.80`. The
   title (320x240 buffer, 2x box) is unchanged at `meanAbs=[2.30,2.50,3.12]
   exact=81.3%` against `t.460` (session 147: `[2.32,2.51,3.12] / 81.2%`).
   `boot.cjs` `RENDERED nonBlack 25208 / colorful 18264`; `testdraw.cjs`
   `exported=true returned=1`.

6. **What is left on that screen is the pixels no presenter writes, not a
   rendering difference.** The whole-frame per-channel mean signed difference is
   `-0.87/-0.84/-0.75`, and this session first read that as a uniform one-level
   darkening. The per-pixel histogram says otherwise: 99.63% of the 640x476
   pixels are identical, and the differing ones are almost all the buffer's last
   column and last row (476 + 640 - 1 = 1115 pixels), which the presenter never
   writes because the VI's visible area is 639x475; they keep whatever the canvas
   resize left. Fifteen more pixels sit on the element's rounded border, mostly
   at row 0 and column 0. Over the visible area (`--inset 1`: rows 1..474,
   columns 1..638), 27 channel samples of 907236 differ (0.0030%), in 9 pixels:
   the four corner pixels `(1,1)`, `(638,1)`, `(1,474)`, `(638,474)` (the
   `border-radius:6px` clip, 72..108 levels each) and five pixels at +-1/+-2.
   `meanAbs` there is `0.0005` and 99.997% of channel samples are identical, so
   the browser now draws that still pixel for pixel with RT64's render target. An
   earlier note in this session (a uniform one-level darkening from the RDP's RGB
   dither) is withdrawn: the dither never entered into it.

## Evidence

### 1. The model comparison

`docs/proofs/web-publishers-atlus-lutmode-fix.png` against
`/tmp/w146/nat/tt.140.bin`, mean absolute difference over the three channels
(640x480 element inner vs 640x480 target):

| model of the canvas row for CSS row `Y` | meanAbs | exact |
|---|---|---|
| identity (`dy = 0`) | 10.38 | 64.6% |
| constant shift, best (`Y - 2`) | 7.26 | 76.6% |
| the CSS scale, `floor((Y + 0.5) * 476 / 480)` | **0.82** (`0.87/0.84/0.75`) | **99.6%** |
| the CSS scale, `round(Y * 476 / 480)` | 0.88 | 99.4% |
| bilinear CSS scale | 2.57 | 77.2% |

The per-row argmin over `Y - 8 .. Y + 1` (rows with a verbatim copy in the
target) is also the stretch seen through the still's 2-row source duplication:
`y - 0` at the top, `y - 2` from row 58 to 297, `y - 3` from 298 to 420, `y - 4`
to the bottom, and each of those steps is a whole source pair.

The fixed build, measured with the same tool (`--canvas-size 640x476
--target-size 640x480`), against the same target:

```
  /tmp/w146/nat/tt.140.bin: meanAbs=[0.87, 0.84, 0.75] meanSigned=[-0.87, -0.84, -0.75] exact=99.6% over 640x476
shift search (mean abs over the three channels):
  dy=-2:  13.67  13.31  13.27  13.22  13.18
  dy=-1:  13.36   9.90   6.80  10.03  13.28
  dy=+0:  13.49   6.98   0.82   7.32  13.82  <-
  dy=+1:  13.40  10.16   7.26  10.69  14.13
  dy=+2:  13.30  13.35  13.71  14.07  14.45
  (columns are dx = -2..2)
```

The session-147 capture read back through the same scale (`--buffer 640x476`)
gives the identical `[0.87,0.84,0.75] / 99.6%`, which is the same content with the
browser's scale undone.

### 2. The reference is reproducible

A fresh native run of the same screen
(`OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=publishers
OGRE_CAPTURE_TARGET=/tmp/w148/nat/tt OGRE_CAPTURE_AFTER=60 OGRE_CAPTURE_EVERY=20
OGRE_EXIT_AFTER_MS=9000 ./build-app/ogrebattle64 assets/ogre64.z64`) logs
`[scene] forced scene 0x0A is active` and `t=2640ms id=0x000A` and writes
`tt.80` .. `tt.180`. Its `tt.140.bin` is byte-identical to session 146's
(`cmp`), so the still is static and the reference is not run-dependent.

### 3. The browser's geometry on the fixed build

From `/tmp/w148/pub-a1-pagelog.txt`:

```
[GFX-VI] VI scanout 640x476 origin=0x1BAC80 status=0x00003252 width=640
         hStart=0x006C02EC vStart=0x002301FD xScale=0x00000400 yScale=0x02000800
[GFX-PRESENT] canvas resized to 640x476 (css 640x476, x1) for the VI mode
[GFX-FBSYNC] wrote back 0x1BA780 640x481 siz=2 pitch=1280
[scene] t=2898ms id=0x000A
```

The three 640-wide colour images the mode renders into are
`0x1BA780`/`0x250800`/`0x2E6840`, each 640x481 `siz=2` `pitch=1280`, which are
the world map's heap targets in `AGENTS.md`. The VI origin `0x1BAC80` is
`0x1BA780 + 0x500` (one 640-wide 16-bit row), which is the row RT64's
`VI::fbAddress` subtracts before `FramebufferManager::find`, so both renderers
resolve the same colour image and sample its first row.

## Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` | clean; `built target ogrebattle64` |
| the publisher still, fixed | `node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_SCENE_LOG=1&OGRE_TEX_TRACE=1' --out /tmp/w148/pub --secs 120 --attempts 2 --shots 2 --min-colorful 60000` | 2 frames, element inner 640x476, strokes `(57,90,165)` 9836 px, `[scene] id=0x000A` |
| the still against RT64 | `python3 tools/web_frame_cmp.py /tmp/w148/pub-00.png /tmp/w148/nat/tt.140.bin --canvas-size 640x476 --target-size 640x480 --shift-search 2` | `meanAbs=[0.87,0.84,0.75] exact=99.6%`, minimum at `(0,0)` = `0.82` |
| the session-147 capture, scale undone | `python3 tools/web_frame_cmp.py docs/proofs/web-publishers-atlus-lutmode-fix.png /tmp/w146/nat/tt.140.bin --buffer 640x476 --target-size 640x480` | same numbers: `[0.87,0.84,0.75] / 99.6%` |
| the native reference, re-taken | native run above (`/tmp/w148/nat/tt.*.bin`) | `cmp /tmp/w148/nat/tt.140.bin /tmp/w146/nat/tt.140.bin` identical |
| the visible area, no edges | `python3 tools/web_frame_cmp.py /tmp/w148/pub-00.png /tmp/w148/nat/tt.140.bin --canvas-size 640x476 --target-size 640x480 --inset 1` | `meanAbs=[0.0,0.0,0.0] exact=100.0% over 638x474` (27 differing channel samples of 907236 by hand count) |
| the title, no regression | `node debug/probes/palette.cjs --query 'OGRE_SCENE=title&OGRE_SCENE_LOG=1' --out /tmp/w148/title --secs 70 --shots 1 --min-colorful 100000` then `python3 tools/web_frame_cmp.py /tmp/w148/title-00.png '/tmp/w146/nt/t.*.bin'` | 2x duplication 76787/76800, best `t.460`: `meanAbs=[2.30,2.50,3.12] exact=81.3%` against session 147's `[2.32,2.51,3.12] / 81.2%` |
| boot regression | `node debug/probes/boot.cjs --attempts 3 --secs 70 --out /tmp/w148/boot` | `RENDERED nonBlack 25208 / colorful 18264` (a fade-in frame) |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000 --out /tmp/w148/testdraw` | `exported=true returned=1`, `colorful 61564` |

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `resize_canvas` sets the element's CSS box to an integer multiple of the drawing buffer (`emscripten_set_element_css_size`, `css_scale_for` over the new `kCanvasCssHeight = 480`), and `[GFX-PRESENT]` prints the box; the comment above `resize_canvas` records the measurement |
| `app/web/index.html` | a comment on `#game-canvas` saying the renderer owns the CSS box from the first present |
| `tools/web_frame_cmp.py` | `--canvas-size WxH` (the drawing buffer the screenshot shows, 1:1 or its 2x), `--target-size WxH`, `--buffer WxH` (undo a non-integer CSS scale by the inverse of `floor((Y + 0.5) * bh / ih)`), `--inset N`, `--shift-search N`; the comparison is now over the overlap of the canvas buffer and the target |
| `.gitignore` | `.emcache/`, the in-tree Emscripten cache `make web EM_CACHE=.emcache` writes |
| `docs/proofs/web-publishers-atlus-css-box-fix.png` | new; the still on the fixed build (element inner 640x476) |
| `docs/proofs/web-publishers-atlus-css-box-side-by-side.png` | new; RT64's target left, the fixed browser capture right, both cropped to 476 rows |
| `docs/proofs/web-publishers-atlus-css-box-diff.png` | new; `|difference| x 8` against RT64's target (near black; the 0.4% that differ) |
| `docs/proofs/web-title-css-box-fix.png` | new; the title on the fixed build (no regression) |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session148.md` | this file |

## Probes

- No probe was added to generated code and none was reverted; nothing under
  `RecompiledFuncs/`, `Bank*Funcs/` or `RspFuncs/` was touched.
- The throwaway analysis lived in `/tmp/w148/` (`analyse.py`, `models.py`,
  `rowmap*.py`, `bbox.py`, `sbs*.py`): the row mapping, the model comparison and
  the side-by-side image. `tools/web_frame_cmp.py` now does the first two, so the
  analysis is repeatable from the repository.
- `.emcache/` is new in the tree. `make web`'s default cache
  (`~/.cache/emscripten-ogre`) is outside the workspace and its
  `symbol_lists/*.json.lock` is refused under the agent file sandbox
  (`PermissionError`), so the build was run with `EM_CACHE=/Users/momo/dev/ogre/.emcache`.
  The directory is gitignored; it holds only Emscripten's sysroot cache.
- `tools/RT64` is dirty with the port's own renderer changes from earlier
  sessions; nothing in it was touched this session.

## Open

1. **The canvas element's rounded corners clip four content pixels, and the
   presenter's unwritten last column and row read as differences against a whole
   render target.** `#game-canvas` carries `border-radius:6px`, so the four
   corner pixels of the frame are blended with the border and differ from RT64's
   target by 72..108 levels each. The last column and row (639 of 640 and 475 of
   476) are never written by `blit_to_canvas`, because the visible area is the
   VI's `fbSize` minus the overscan crop; RT64's own window is shrunk to the
   visible size by `removeBlackBorders` instead, so this only shows up when the
   canvas is compared against a full `OGRE_CAPTURE_TARGET`. Both are cosmetic,
   and neither is a rendering difference. `tools/web_frame_cmp.py --inset N`
   keeps them out of a measurement.
2. **The pixel-level agreement is what the next screen should be tested with.**
   The publisher still is now exact over its visible area, so a screen with
   something the still does not exercise (the njpeg backdrop, the map's sprites,
   framebuffer-as-texture) separates the remaining renderer gaps; the title's
   two moving-cloud bands are the same measurement problem as before.
3. Session 146's other items stand: the two moving-cloud bands on the title (the
   developer states the port's clouds are not exact against retail yet, so the
   two runs' phases are not separable); the native's saved game speed x4 against
   the browser's x1; framebuffer-as-texture (a `G_LOADTILE` whose source is a
   colour image rendered in the same task reads RDRAM); and
   `OGRE_SCENE=publishers` landing on scene `0x0009` in some native runs
   (this session's native run and browser run both read `0x000A`).
4. The LOADTLUT destination is still keyed on the load tile's `palette` field
   (`bank = t.palette * 16`) where RT64 uses the load tile's `tmem`
   (`loadTile.tmem << 3`). The two agree for `tmem=0x100` with `palette=0`, which
   is every traced load so far.
5. The `fldRegs` x/y scale and offset are still not applied, but this session
   shows the publisher still did not need them: the whole difference was the CSS
   box.
