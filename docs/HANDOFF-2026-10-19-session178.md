# Session 178 handoff — the boot intro's soldier trails were the fill quad's second triangle

## Goal

The developer reported that the opening animation in the browser build
(`http://127.0.0.1:8931/app/web/index.html`) leaves a triangle-shaped region on
the right of the screen where the soldiers' sprites are not cleaned up as the
soldiers run away, and asked whether the page was serving the latest build.
Timebox: one round.

The scene is the boot intro, `0x09` (`docs/scenes.md`: "boot intro: soldiers,
falling cube, Nintendo 64 logo").

## Result

### 1. The page serves the current working tree and the current build

`debug/server.py` serves the repository root (`/Users/momo/dev/ogre`), so
`/app/web/index.html` is the working-tree file, not a copy. The served
`build-wasm/ogrebattle64.wasm` hashed to
`58dbb29e0dc7c8b7e7a800ffaf811d013da149927bc50e996fa07300e487162d`, the same
bytes as the local build, and that build (08:22) was newer than every file under
`app/src` and `app/web` (newest, `web_renderer.cpp`, 08:21). The build the
developer's screenshot came from was the one before this session's fix,
`26198c885c3228858bd7f97cd94287add88d43508e71588209ddb518a6221517`. Both the page and the
wasm are served with `Cache-Control: no-store`, so a reload picks up a rebuild.
A browser tab that was open before a rebuild keeps the old wasm until it is
reloaded.

### 2. The trails are one missing triangle of the per-frame fill

The intro submits, per frame, exactly 27 draws: one full-screen black
`G_FILLRECT` (0,0)-(319,239), 24 triangles (12 soldier quads, two triangles
each) and 2 texrects. The draws go to a different 320x240 colour image every
frame (0x025C00, 0x04B400, 0x000400, repeating), so the game triple-buffers, and
each buffer is cleared and redrawn every third frame.

`draw_fill_rect` names its four corners top-left, bottom-left, bottom-right,
top-right and then emitted the 6-vertex list as `(0,1,2)` plus `(3,0,1)`.
`(3,0,1)` is top-right, top-left, bottom-left: the second triangle repeats the
first triangle's left edge instead of using the quad's other diagonal
(`(0,2,3)`). Both triangles therefore cover the left half of the rectangle and
the right half is never rasterized.

RT64 renders a `G_CYC_FILL` rectangle as a clear of the whole rectangle:
`tools/RT64/src/render/rt64_framebuffer_renderer.cpp:1620` turns the call into
`InstanceDrawCall::Type::FillRect`, whose command-list path is `clearColor` (and
`clearDepth` only when there is no colour target). The browser was drawing the
fill through the triangle pipeline, where the missing half matters.

Because the fill is the frame's clear, the right half of every buffer kept every
earlier frame's right half. The ring of soldiers runs outward from the cube, so
each earlier ring stayed on screen; the frame filled with a wedge of repeated
soldiers clipped by the quad's diagonal. That is the developer's "triangle on
the right".

### 3. Evidence

- The renderer's own colour image, read with `glReadPixels` at present time for
  the frame's fill (temporarily coloured magenta): the fill's colour landed in
  1/3 of the 320x240 image, on one side of the quad's diagonal, and the rest
  held the earlier frames' content (alpha 0.43, the intro's fade). The same
  measurement with `OGRE_DEPTH=0` read the same, so the depth test is not
  involved.
- The fill's six vertices, logged from the flush: `(-1,1) (-1,-1) (1,-1) (1,1)
  (-1,1) (-1,-1)`, i.e. `(0,1,2)` then `(3,0,1)`.
- The canvas is not the cause: a `glClear` of a distinct colour before every
  present left no pixel of that colour, so the present blit covers the canvas.
- The game's draw list is not the cause: 27 draws per frame, none larger than a
  soldier quad or the two texrects.
- Native RT64 does not show it: `OGRE_CAPTURE_PRESENT=/tmp/nat-intro` captured
  the same intro (`p.155` shows three soldiers per corner and the cube).

### 4. The fix

`app/src/web_renderer.cpp`, `draw_fill_rect`: the 6-vertex list uses the index
table `{0, 1, 2, 0, 2, 3}` instead of `i % 4`, so the two triangles split the
quad on its own diagonal. This is the pattern the S2DEX object rectangle
(`{0,1,3,0,3,2}`) and the texrect already use.

## Verification

- Browser intro, after the fix: the frame at task 146 shows three soldiers per
  corner and the cube (the native picture), and the running-out phase shows no
  wedge. Captured with headless Chromium (`--enable-gpu --use-angle=metal`) and
  the page's own `canvas.toDataURL` per browser frame. The frame is
  `docs/proofs/web-intro-fill-quad-fixed.png`.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`:
  `fbtex hits=1 lines=1`; the log now reads `drawn rows=[0,239]` for the fill.
- `--flags 128 --expect-depth near` -> `rgba=(255,0,0,0)`; `--flags 384
  --expect-depth far` -> `rgba=(0,0,255,0)`; `--flags 512 --expect-alpha` ->
  `threshold/full=full threshold/none=none dither/partial=partial`.
- `node debug/probes/shots.cjs --shots 5 --gap 1200`: five frames captured, the
  intro renders (the cube with its soldiers and dust).
- `make web` clean.

Not measured: the screens whose fills are not full-screen (the attract story's
letterbox fills, session 150; the publisher stills) have not been re-captured
since the quad got its second triangle. They are expected to be unchanged in
content and now cleared over the whole rectangle, which is RT64's behaviour.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `draw_fill_rect`: the fill quad's two triangles are `(0,1,2)` and `(0,2,3)` |
| `PLAN.md` | the fix, its evidence and the unmeasured screens |
| `docs/DECISIONS.md` | the decision row |
| `docs/STATUS-LOG.md` | the session entry |
| `docs/HANDOFF-2026-10-19-session178.md` | this file |
| `docs/proofs/web-intro-fill-quad-fixed.png` | the fixed intro frame (task 146) |
| `build-wasm/` | rebuilt by `make web` |

## Probes used and reverted

All diagnostic code was tagged `probe178` and is gone
(`grep -rn "probe178\|P178\|p178" app/` is empty):

- a per-draw log (kind, target address, NDC extent, UV, texture key) and a
  per-task colour-target log, in `queue_triangles`;
- the fill coloured magenta for tasks 150-152, and a two-colour variant of the
  fill's two triangles;
- `glReadPixels` probes at present time (single pixels, three pixels, and a
  24x18 grid of the fill's coverage), and a `glReadPixels` right after the fill's
  `glDrawArrays`;
- a `glClear` of green before every present, to test the blit's coverage;
- a widened `[GFX-RECT]` log window and a `% 10` present-log cadence.

## Method notes

- Headless Chromium without `--enable-gpu` renders through SwiftShader and the
  boot intro advances at about 1 display list per second; with
  `--enable-gpu --use-angle=metal` it runs at the game's own rate, which is what
  made a frame-accurate capture of the intro possible.
- A page-level `requestAnimationFrame` hook that calls `canvas.toDataURL()`
  captures every browser frame (`preserveDrawingBuffer` is true), including the
  intro frames that pass while the main thread is still busy with the boot.
  Playwright screenshots sample too slowly for this scene.
- RDRAM/CPU-side tooling is not needed for a renderer-side trail: reading the
  colour image at present time separates "the game did not submit it" from "the
  renderer did not draw it" in one run.
