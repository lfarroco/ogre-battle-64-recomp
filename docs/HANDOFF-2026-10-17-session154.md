# Session 154 handoff — the framebuffer-as-texture check compares the drawn columns too, so it stops reporting loads the draw never covered

## Goal

Continue the web renderer (developer, session start). Session 153's open items
were two unverified input cases (a physical pad, two to four pads) and session
152's list: framebuffer-as-texture (instrumented, no screen needs it), the native
cathedral route, the `[GFX-FBTEX]` row test's column over-report, the canvas's
rounded corners and unwritten last column/row, the LOADTLUT `tmem` key, the
`fldRegs` x/y scale and offset, the title's moving-cloud bands, and the screens
not compared against the reference.

This session took the `[GFX-FBTEX]` column over-report. It is the one item with a
deterministic positive and negative control and no dependency on a game route, a
native capture or a hardware reference fact. It is a diagnostic-only change: no
rendered frame moves, so the round closes a recorded defect in the instrument
rather than in the picture, and the item that the instrument guards
(framebuffer-as-texture) keeps its measurement honest.

## Result

1. **The defect was measured before it was fixed.** `test_draw` gained a negative
   control under `ogre_gfx_debug_flags` bit 6 (64): the same 320x240 RGBA16 colour
   image as bit 5, a fill of columns 0..63 (rows 0..239), and a second tile load
   from texels 160..191 of rows 0..31 - the drawn rows, outside every drawn
   column. On the build with only the row test (`if (false &&` in front of the
   column test), the probe printed

   ```
   fbtex hits=2 lines=2
     [GFX-FBTEX] load tex=0xC02000 rect=32x32 stride=640 ... cols=[0,64) drawn cols=[0,128) ...
     [GFX-FBTEX] load tex=0xC02000 rect=32x32 stride=640 ... cols=[320,384) drawn cols=[0,128) ...
   ```

   The second line is a load whose bytes lie outside every column the fill wrote.
   On the fixed build the same command prints `fbtex hits=1 lines=1` and only the
   first line. `debug/probes/testdraw.cjs --expect-hits N` is the hard assertion.

2. **`note_fb_write` now records the byte columns a draw covers.** The x mapping is
   the inverse of `ndc_xf` (`2*x/width - 1`), so a vertex's pixel is
   `(pos + 1) / 2 * fb_width_px`, the same arithmetic the row test already used
   for y; x is not flipped, so no `height - 1 -` term. The byte column is that
   pixel times `bytes_per_texel(fb_siz)`. `FbWrittenRange` carries
   `col_min`/`col_max` (one past the last byte), unioned across the draws into one
   image; a draw whose x extent cannot be mapped (`fb_width_px == 0`, or a 4-bit
   colour image) clears the bound to `col_max = -1`, which means "unknown" and
   keeps the old row-only answer.

3. **`note_fb_texture` now bounds the rect to its own columns and intersects both
   axes.** The load's rect is `x0 .. x0+w` texels, which is `x0*bpp .. (x0+w)*bpp`
   bytes (`x0/2 .. (x0+w+1)/2` for a 4-bit image); the old test used the whole
   `stride`, so a 32-texel RGBA16 load out of a 640-byte-pitch image claimed 640
   bytes of each row. The row range also ends at the last rect row's own column
   span instead of one full stride past it. The column test is applied only when
   every rect row has the same column offset in the image (`stride % pitch == 0`)
   and the rect does not wrap the image row; otherwise the row-only answer stands,
   which over-reports and never under-reports. The `[GFX-FBTEX]` line prints both
   ranges now (`cols=[..)` and `drawn cols=[..)`), and its dedup key includes the
   rect origin, so two loads of one image at two rects are two lines.

4. **The positive control still fires and the off case is still silent.** With the
   narrower fill (columns 0..63 instead of the whole image), the bit-5 load at
   texels 0..31 still overlaps the drawn columns: `--flags 32 --expect-fbtex`
   prints `fbtex hits=1 lines=1`, `--flags 0 --expect-hits 0` prints `hits=0
   lines=0`, and `--flags 96 --expect-hits 1` prints `hits=1`. No game frame
   changed, so there is no before/after image; the only frame that differs is the
   synthetic one under debug flag bit 6, which is a probe.

5. **No real scene's count changed.** The forced scene `0x10` - the scene whose
   range-only hits session 152 removed with the row test (133 of them) - reports
   `0x10: tasks=52 hits=0`, and `0x04`/`0x0A` report `hits=0` each. A boot run on
   the build before the last format change reports `RENDERED nonBlack 25136 /
   colorful 19296` with `sawRealDl=true`; a later run took the idle trajectory
   (task count below 2) and still rendered.

6. **Session 153's claim 3 did not match the tree, and is corrected.** It says the
   `app/src/web_platform.cpp` header comment "is corrected to the native rule".
   The comment still read "slot 0 is the keyboard (always connected), slots 1-3
   are gamepads", the old model, on the committed tree. The `ogre_input_debug(int
   slot)` signature, the `0xFFFF` reply for a disconnected slot, the lowest-free
   slot fill and the slot-0 OR in `app/web/web.js` all verify as described, and
   `debug/probes/gamepad.cjs` exists and requires slot 0 to carry `A`
   (`0x8000`) and `0xC000` while it holds the keyboard's `B`. The stale comment is
   rewritten to the native rule this session.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `FbWrittenRange` gains `col_min`/`col_max`; `note_fb_write` computes the drawn byte columns from the vertices and `fb_width_px`; `note_fb_texture` takes `x0`, bounds the rect to its own byte columns, uses the rect's last row for the row range, requires overlapping columns when the stride is a multiple of the pitch, prints both ranges, and keys its log line by (texture, image, rect origin); `test_draw`'s positive fill is narrowed to columns 0..63 and bit 6 (64) adds the negative column control |
| `app/src/web_platform.cpp` | the file header records the native slot rule (each pad takes the lowest free slot; slot 0 carries the keyboard and the tap schedule) instead of "slot 0 is the keyboard, slots 1-3 are gamepads" |
| `debug/probes/testdraw.cjs` | `--expect-hits N` requires the exact `fbtex: hits=` count; the header records bit 6 and the flag |
| `docs/guides/web-probes.md` | the `testdraw` row and the framebuffer-as-texture recipe record bit 6 and `--expect-hits` |
| `PLAN.md` | session-154 paragraph in the browser section, the item-8 sub-list records the column test as closed, and the session-153 correction |
| `docs/STATUS-LOG.md`, `docs/README.md` | session entry and index row |
| `docs/HANDOFF-2026-10-17-session154.md` | this file |

No probe was added to generated code and nothing under `RecompiledFuncs/`,
`Bank*Funcs/` or `RspFuncs/` was touched, so there is no `probe154` tag to
revert. `tools/RT64` still carries session 150's fill clip uncommitted; this
session did not change it. No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean after the negative
  control alone, after the column fix, after the A/B revert, after restoring the
  fix, after the `web_platform.cpp` comment, and after the last `[GFX-FBTEX]`
  format change; the last run builds `ogrebattle64.wasm`.
- `python3 debug/server.py` (background, `http://127.0.0.1:8931`).
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex --out /tmp/154-base`
  on the session-153 build -> `fbtex hits=1 lines=1`, one `[GFX-FBTEX]` line.
- `node debug/probes/testdraw.cjs --flags 96 --out /tmp/154-precol` on the build
  with the negative control and the row-only test -> `fbtex hits=2 lines=1`
  (the line count was still the old dedup key).
- A/B with only the column comparison disabled (`if (false &&` in front of it),
  one variable: `node debug/probes/testdraw.cjs --flags 96 --out
  /tmp/154-ab-rowonly` -> `fbtex hits=2 lines=2`, and the second line is
  `cols=[320,384) drawn cols=[0,128)`.
- Restored build: `node debug/probes/testdraw.cjs --flags 96 --expect-hits 1
  --out /tmp/154-check96b` -> `fbtex hits=1 lines=1` and one line,
  `cols=[0,64) drawn cols=[0,128)`, exit 0.
- `node debug/probes/testdraw.cjs --flags 0 --expect-hits 0 --out
  /tmp/154-check0` -> `fbtex hits=0 lines=0`, exit 0.
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex --out
  /tmp/154-final32` on the final build -> `fbtex hits=1 lines=1`, exit 0, and
  `--flags 96 --expect-hits 1` prints the same line with
  `cols=[0,64) drawn cols=[0,128)`.
- `node debug/probes/boot.cjs --attempts 2 --secs 40 --out /tmp/154-boot` ->
  `attempt 1: sawRealDl=true rendered=true`, `RENDERED ... "nonBlack":25136,
  "colorful":19296`. A later run of the same probe on the final build took the
  idle trajectory this guide documents (`attempt 1: sawRealDl=false
  rendered=true`, `nonBlack 24468 / colorful 10160`), and `boot.cjs` passes on
  `rendered` alone.
- `node debug/probes/fbtex.cjs --scenes 0x04,0x0a --secs 18 --out /tmp/154-fbtex`
  -> `0x04: tasks=123 hits=0`, `0x0a: tasks=14 hits=0`, `TOTAL ... 0 over 2`.
- `node debug/probes/fbtex.cjs --scenes 0x10 --secs 22 --out /tmp/154-fbtex10` ->
  `0x10: tasks=52 hits=0`, `TOTAL ... 0 over 1`. A rerun on the final build
  (`--scenes 0x04,0x10 --secs 18`) took the idle trajectory (`tasks=22`/`1`) and
  still reported `hits=0` on both.
- `node --check debug/probes/testdraw.cjs`.

There is no repo test harness; every check above is a ROM-dependent browser run
of 20-60 s, and the probe server was a background process.

## Items not taken this session

- (a) The title's cloud bands. The developer states the port's clouds are not
  exact against retail yet, and the two runs' phases are not separable, so a
  12-16 level difference in rows 64..112 and 195..238 has no reference to compare
  against. It needs the developer to say which cloud image is right, or a
  reference-emulator capture of the same frame.
- (b) The fill scissor is settled by session 150's ParaLLEl-RDP capture; the
  browser's half-open scissor agrees with it. No browser change.
- (c) The LOADTLUT destination is keyed by `t.palette * 16`
  (`app/src/web_renderer.cpp`, `OP_LOADTLUT`), and RT64 writes TMEM at
  `loadTile.tmem << 3` (`tools/RT64/src/hle/rt64_rdp.cpp`, `loadTLUTOperation`).
  Every traced load is `tmem=0x100, palette=0`, so no screen has needed the
  difference. Closing it needs a ROM display list that loads two palettes to
  different `tmem` addresses and draws with both, and a statement about which
  TMEM region the RDP samples a CI tile from. That is a reference fact.
- (e) The canvas's last column/row and rounded corners. Cosmetic; the visible
  area is already exact. Changing the CSS `border-radius` is a design choice
  (rule 1), and changing the drawing buffer to the visible size changes the page
  for every screen.
- (f) A screen not yet compared against the reference. This needs a native
  `OGRE_CAPTURE_TARGET` run of the same screen on this machine and a browser
  capture of the same frame; the two runs' animation state has to agree, which is
  the same problem as (a) for every screen that animates.

## Open

1. Framebuffer-as-texture stays instrumented and unimplemented. The column test
   is stricter than the row test, so a screen that needs the feature is still
   reported; no screen reached has needed it.
2. Session 153's other open items stand: a physical pad on a real page, and two to
   four pads filling slots 1..3.
3. Session 152's native cathedral route (`OGRE_SCENE=new-game OGRE_STEP=2` plays
   `0x09 -> 0x0A -> 0x04` natively) is unresolved, so the browser cathedral
   backdrop still has no native frame to compare against.
4. The row test itself still over-reports one row at the top of a draw: `ndc_y`
   maps guest row `y` to `1 - 2y/H`, and `note_fb_write` inverts it to
   `H - 1 - wy`, which is `y - 1` for a vertex, so a draw covering rows 100..199
   is recorded as 99..199. The direction is over-report and the fill's `+1` hides
   it for a full-image fill, so it is left as it is and recorded here.
5. The remaining browser-renderer items, in the state they were in at the start:
   the title's cloud bands (needs a reference or developer intent), the LOADTLUT
   `tmem` key (needs a reference fact), framebuffer-as-texture (needs a screen),
   the canvas edge and corners (cosmetic, needs developer intent on the corners),
   the `fldRegs` x/y scale and offset (no browser change outstanding), and the
   screens not compared against the reference.
