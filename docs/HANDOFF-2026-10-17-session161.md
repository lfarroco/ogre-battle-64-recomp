# Session 161 handoff — the title residual is the browser's missing `OGRE_FOG` repair, and the two runs share a scroll state

## Goal

Continue the browser (WebAssembly) renderer from session 160. Verify session
160, then decide whether the title screen's band residual is a renderer
difference or an attract-scroll state difference, using the per-draw `s`/`t` of
the scrolling layers rather than decoded image content.

Timebox: 80 minutes.

## Result

1. **Session 160 verifies, and the canvas leaves no pixel unwritten.**
   `grep -rn probe160 app/ debug/` prints nothing and exits 1.
   `git diff app/web/index.html` removes `border-radius:6px` from `#game-canvas`.
   `present_framebuffer` calls `resize_canvas(visible.width, visible.height)`
   (`app/src/web_renderer.cpp:2295`) and blits `frac_w = frac_h = 1.0` over a
   viewport of `canvas_width_ x canvas_height_`, so the quad covers NDC
   `[-1,1]^2` and the canvas is the visible area itself. Measured, not assumed:
   the publisher still's capture has inner size 639x475 and every one of its
   303525 pixels equals RT64's target at the same coordinate. The reproducer and
   its output:

   ```
   python3 tools/web_frame_cmp.py /tmp/w160/pub-final-00.png \
       /tmp/w146/nat/tt.140.bin --target-size 640x480 --canvas-size 639x475
     read as a 639x475 drawing buffer
     meanAbs=[0.0, 0.0, 0.0] meanSigned=[0.0, 0.0, 0.0] exact=100.0% over 639x475
   ```

   An unwritten pixel would hold the page background `(20,22,26)` or an older
   frame; none does. So `border-radius` was the last canvas pixel that was not
   game output, which is source-level support for session 160's decision that
   removing it is a rendering difference and not a display style.

2. **The two runs share a scroll state, and the per-draw `s`/`t` prove it.**
   The browser trace (`/tmp/w156/webtrace2-a1-pagelog.txt`) and the native trace
   (`/tmp/w156/nat-trace2.log`, `OGRE_TEX_TRACE=1` through `app/src/renderer.cpp`)
   were split into display lists. At the third list of each run the 135
   non-degenerate draws are identical in order, image, rectangle, `s`, `t`,
   `dsdx`, `dtdy` and tile window, and the sky image `0x1C3EA8`'s partial first
   tile is `rect=0,16..63,80 s=32` in both. The native list also holds 456 draws
   whose tile window is degenerate (`ult == lrt`): the mirrored sky pieces of
   `0x1E2918` (`s=1984`/`2016`, `dsdx=-1024`, `x=0..383`, rows 64..128) and the
   72-pixel cloud columns of `0x217508` (`x=0,42,...,402`). Dropping exactly
   those gives the browser's list:

   ```
   list 1: web 209, nat 591 (456 empty-tile) -> equal False
   list 2: web 135, nat 591 (456 empty-tile) -> equal True
   list 3: web 135, nat 590 (456 empty-tile) -> equal False
   ```

   The browser's first list is 209 traced draws because `trace_task` numbers two
   of the boot's tasks as `dl=2`; from list 2 onward the counts are 135/134.

   The other lists are different animation instants (the `0x1C3EA8` partial tile
   width differs: 63, 61, 43, 37, ... in the browser; 63, 38, 37, 36, ... in the
   native). So the two runs are the same scroll state at list 2 and at different
   instants elsewhere; the 13-pixel scroll session 159 measured was a comparison
   of list 2 or 3 in the browser against a later native list. The per-draw
   difference is presence, not a `s`/`t` offset: the native draws 456 fog pieces
   the browser does not.

3. **The 456 draws are the native app's `OGRE_FOG` repair, and the browser skips
   them on purpose.** All 456 have the degenerate tile window that
   `app/src/renderer.cpp`'s fog pass rewrites to sample the layer's own image
   (`namespace fog`, `nop_rect::Walker::fog_fix`) and that
   `web_renderer.cpp`'s `draw_texrect` rejects with the guard mirroring RT64's
   (`RDP::drawTexRect`, `tools/RT64/src/hle/rt64_rdp.cpp:1638`, which also
   returns). The native trace prints before that guard, which is why the native
   log lists them; RT64 draws them only because the app has already rewritten
   their tile setup. The browser has no equivalent pass, so its frame is missing
   the fog layer.

4. **One-variable A/B on the native side: the fog is the whole residual.** Two
   forced-title capture series with identical knobs, differing only in
   `OGRE_FOG=0`:

   ```
   OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title \
   OGRE_CAPTURE_TARGET=/tmp/w161/fogoff32/t OGRE_CAPTURE_AFTER=100 \
   OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=32000 ./build-dist/ogrebattle64 assets/ogre64.z64
   ```

   Same present index, fog on vs fog off: total mean abs 3.4 (max 10.75), rows
   64..119 `5.68`, rows 120..239 `3.00`, rows 0..63 `0.72`. Browser frame
   `title-03` against the fog-off series, best over 492 targets:

   ```
   best t.584.bin meanAbs=0.052 inset=1 | rows0-63 0.046 rows64-119 0.072 rows120-239 0.044
   ```

   The same browser frame against the fog-on series (session 156's `nat2`, same
   `OGRE_CAPTURE_TARGET` schedule) is `meanAbs=2.349`, which is session 156's
   `[2.08,2.23,2.74]` averaged, with rows 64..119 `4.15`. So the browser equals
   the native frame to 0.05 of a level once the fog repair is off, and differs by
   4.15 in the fog's own rows when it is on. The residual was the missing repair,
   not the sampler, the blend, the composition or the scroll phase.

5. **No renderer behaviour changed.** A browser port of the fog pass is a
   display-list pre-pass that rewrites tile setup in RDRAM (the native code is
   `nop_rect::Walker` with `fog_fix`, ~200 lines, plus the layer-table read at
   `D_801B80E0`), not a sampler change; it needs its own build and verification
   and did not fit the timebox. Nothing under `app/` was edited in this session,
   so `make web` was not required; no probe was added, so there is nothing to
   revert beyond session 160's already-reverted `probe160`.

## Files changed

| file | change |
|---|---|
| `docs/HANDOFF-2026-10-17-session161.md` | this file |
| `PLAN.md` | session-161 paragraph; correction to the session-156 paragraph |
| `docs/HANDOFF-2026-10-17-session156.md` .. `-159.md` | in-place correction notes |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | log entry, index row, correction row |

No file under `app/`, `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or
`tools/RT64` was changed. No commit was made. No probe was added.

## What was run for verification

- `grep -rn probe160 app/ debug/` — no output, exit 1.
- `git diff app/web/index.html` — `- border-radius:6px`.
- `python3 tools/web_frame_cmp.py /tmp/w160/pub-final-00.png
  /tmp/w146/nat/tt.140.bin --target-size 640x480 --canvas-size 639x475` —
  `[0.0,0.0,0.0] / 100.0% over 639x475`.
- `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png
  /tmp/w156/nat2/t.574.bin --inset 1` — `[2.08,2.23,2.74] / 79.2%` (session
  160's baseline reproduces).
- The per-draw comparison is `/tmp/w161_proof.py` over the two existing logs:
  it prints `list 2: web 135, nat 591 (456 empty-tile) -> equal True`.
- `make app` — `[100%] Built target ogrebattle64` (incremental, 10 s).
- The fog A/B runs above: fog on `/tmp/w161/fogon` (191 frames in 20 s) and
  `/tmp/w156/nat2` (525 frames), fog off `/tmp/w161/fogoff32` (492 frames in
  32 s).
- The frame comparisons are `/tmp/w161/cmp2.c` and `/tmp/w161/cmp3.c` (small C
  programs over the target `.bin`s; the reference tool is minutes per 500
  targets). Their numbers reproduce `web_frame_cmp.py`: `nat2` best is
  `2.349` = the mean of `[2.08,2.23,2.74]`.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **Port `OGRE_FOG` to the browser renderer.** `web_renderer.cpp` needs a
   pre-pass over each task's display list that performs what
   `app/src/renderer.cpp`'s `namespace fog` does: read `D_801B80E0`, take layer
   `i`'s record `+0xFC + i*0x24`, `+8` the image, rewrite the last
   `SETTIMG`/`SETTILE t7`/`LOADTILE`/`SETTILE t0`/`SETTILESIZE t0` before a
   degenerate-window rectangle so it samples the layer image. Expect the title
   to move from `0.052` to `0.0`-level against the native. The one-variable check
   is `OGRE_FOG=0` on the native series, already recorded here.
2. **The browser equality target is now 0.052**, so any other title difference
   larger than that is a new item; none is known.
3. **The screens not compared against the reference** are unchanged.
