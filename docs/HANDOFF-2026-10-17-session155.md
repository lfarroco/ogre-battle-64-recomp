# Session 155 handoff — the `[GFX-FBTEX]` row inversion was one row up, and the 25-scene sweep finds no screen that needs framebuffer-as-texture

## Goal

Continue the web renderer (developer, session start). Session 154 closed the
column over-report of the `[GFX-FBTEX]` detector and left two items: the row
over-report it recorded as open item 4, and the decision on
framebuffer-as-texture, which had only ever been measured on a few scenes.

This session (a) checked session 154's claims at file level, (b) fixed the row
inversion and made it a hard assertion, and (c) ran the full 25-scene sweep the
item needs, plus a natural route through the opening, and closed the item.

## Result

1. **Session 154's claims match the tree.** Checked at file level, quoting the
   working tree:

   - `FbWrittenRange` carries the two fields
     (`app/src/web_renderer.cpp`, `struct FbWrittenRange`):
     ```
     int col_min = 1 << 30;
     int col_max = -1;
     ```
     `note_fb_write` fills them from the vertices and `fb_width_px`
     (`const int wx = static_cast<int>((cmd.pos[i * 2] + 1.0f) * 0.5f * (float)cmd.fb_width_px);`,
     `cx = ... * bpt`, `col_max = max(col_max, cx + bpt)`, capped at
     `fb_row_bytes`), and unions them across the draws into one image
     (`r.col_min = std::min(r.col_min, col_min);`), with `col_max = -1` when a
     draw's x extent cannot be mapped. `note_fb_texture` takes `x0`, bounds the
     rect to `col_lo = x0 * bpp`, `col_hi = (x0 + w) * bpp`, ends the row range
     at `address + (y0 + height - 1) * stride + col_hi`, and skips the report
     when `stride % pitch == 0`, `load_col_hi <= pitch` and
     `load_col_hi <= r.col_min || r.col_max <= load_col_lo`. The dedup key
     includes `x0` and `y0`.
   - The bit-6 (64) negative control is in `test_draw`
     (`if ((g_debug_flags.load(...) & 64u) != 0)`), with the fill narrowed to
     columns 0..63 (`0xF6000000 | ((63 << 2) << 12) | (239 << 2)`) and the second
     load at texels (160,0)-(191,31).
   - `debug/probes/testdraw.cjs` parses `--expect-hits N`
     (`const expectHits = opts['expect-hits'] === undefined ? null : h.num(...)`)
     and makes it a hard check (`const hitsOk = expectHits === null || fbtexHits === expectHits;`,
     then `ok = ... && hitsOk`, `process.exit(ok ? 0 : 2)`).
   - The `app/src/web_platform.cpp` header comment records the native rule:
     "each connected pad takes the lowest free slot, and slot 0 - N64
     controller 1, the only controller this game reads - carries the keyboard and
     the scripted tap schedule as well (`app/web/web.js`, `updateGamepads`)".
   - Bit 6 was free. The committed renderer (`git show HEAD:app/src/web_renderer.cpp`)
     tests `& 1u`, `& 2u`, `& 4u`, `(>> 3) & 3` (bits 3-4) and `& 32u`, i.e.
     bits 0-5 (0x3F). No use of 64 or 128 exists in the tree.

2. **Session 154's open item 4 is confirmed, and the expression is not exactly
   `y - 1`.** `ndc_yf` maps guest row `y` to `1 - 2y/H` (`H` is
   `target_height_px`, which `record_state` also stores as `fb_height_px`). The
   old `note_fb_write` computed
   ```
   const int wy = static_cast<int>((cmd.pos[i*2+1] + 1.0f) * 0.5f * float(cmd.fb_height_px));
   const int gy = cmd.fb_height_px - 1 - wy;
   ```
   so `gy = H - 1 - trunc((p + 1) / 2 * H) = H - 1 - trunc(H - y)`. That is
   `y - 1` where the float product lands on or above the integer, and `y` where
   it lands just below it: a C survey over `H = 240` gives `gy == y - 1` for 194
   of 241 rows and `gy == y` for the other 47. A fill of rows 100..163 was
   therefore recorded as 99..163 on this build (`row_min` over-reported by one
   row, `row_max` exact), and the `std::max(gy, 0)` clamp hid the error for a draw
   whose top edge is row 0.

3. **The row inversion is exact now.** `note_fb_write` inverts with the y mirror
   of the column inverse, `edge = (1 - pos) / 2 * height`, treats a vertex as an
   edge (`row_min` is the top edge, `row_max` the bottom edge minus one, because a
   TEXRECT/FILLRECT lower-right corner is exclusive), and `std::lround`s the
   edge so a rect's authored integer comes back exactly (checked for every row of
   `H` = 240, 320, 476, 480 and 241). A fill of rows 100..163 is now recorded as
   `drawn rows=[100,163]`. Nothing else reads the value: it is the diagnostic's
   only consumer, so no rendered frame moves.

4. **The row bound is a deterministic assertion now, and it failed on the old
   arithmetic.** `test_draw`'s bit-6 branch gained a row control: a second colour
   image at `0x00C28000`, a `G_FILLRECT (0,100)-(63,163)`, and a load of rows
   98..99 (columns 0..31, inside the fill's columns, outside its rows). Two rows
   and not one because `draw_texrect` drops a tile with `lrt == ult` (RT64's
   degenerate-tile guard), so a one-row load never reaches the decode. A/B with
   one variable, the renderer's row inversion, both builds carrying the same
   fixture:

   - Old arithmetic:
     `node debug/probes/testdraw.cjs --flags 96 --expect-hits 1 --out /tmp/155-ab-prefix`
     ```
     fbtex hits=2 lines=2
       [GFX-FBTEX] load tex=0xC02000 rect=32x32 ... rows=[0,31] drawn rows=[0,239] cols=[0,64) drawn cols=[0,128) ...
       [GFX-FBTEX] load tex=0xC28000 rect=32x2 stride=640 ... rows=[98,99] drawn rows=[99,163] cols=[0,64) drawn cols=[0,128) ...
     FAIL: expected fbtex hits=1, read 2
     ```
     The second line's `drawn rows=[99,163]` is the recorded form of a fill of
     rows 100..163.
   - Fixed build:
     `node debug/probes/testdraw.cjs --flags 96 --expect-hits 1 --out /tmp/155-check96`
     ```
     fbtex hits=1 lines=1
       [GFX-FBTEX] load tex=0xC02000 rect=32x32 ... rows=[0,31] drawn rows=[0,239] cols=[0,64) drawn cols=[0,128) ...
     exit=0
     ```

5. **Framebuffer-as-texture: no reachable screen needs it, and the sweep is
   recorded.** The full 25-scene sweep on the build before the row fix (session
   154's renderer arithmetic; the new `test_draw` fixture is not part of any game
   scene), so it over-reports relative to the final build:

   ```
   node debug/probes/fbtex.cjs --scenes 0x00,...,0x18 --secs 18 --out /tmp/155-fbtex-all
   0x00: tasks=83 hits=0     0x01: tasks=29 hits=0     0x02: tasks=1 hits=0  (page error)
   0x03: tasks=1 hits=0  (page error)
   0x04: tasks=37 hits=0     0x05: tasks=76 hits=0     0x06: tasks=120 hits=0
   0x07: tasks=77 hits=0     0x08: tasks=85 hits=0     0x09: tasks=83 hits=0
   0x0a: tasks=56 hits=0     0x0b: tasks=3 hits=0      0x0c: tasks=12 hits=0
   0x0d: tasks=1 hits=0  (page error)                  0x0e: tasks=1 hits=0  (page error)
   0x0f: tasks=30 hits=0     0x10: tasks=77 hits=0     0x11: tasks=22 hits=0
   0x12: tasks=19 hits=0     0x13: tasks=136 hits=0    0x14: tasks=39 hits=0
   0x15: tasks=27 hits=0     0x16: tasks=75 hits=0     0x17: tasks=60 hits=0
   0x18: tasks=88 hits=0
   TOTAL framebuffer-as-texture loads: 0 over 25 entr(ies)
   ```
   21 of the 25 scenes produced display lists (3..136 tasks each). Four produced
   one task and a `memory access out of bounds` page error, so they say nothing:
   `0x02`, `0x03`, `0x0D` and `0x0E`. `0x02` and `0x0D` are the New Game loader
   and its engine, which need the pre-state a natural path builds (`0x02` traps in
   `func_8007A110` <- `func_ovlC_801B7EBC` <- `func_ovlC_801AFC2C` <-
   `func_80178568` <- `func_80075BC0`); `0x03` and `0x0E` have no known natural
   entry. `0x0B` submitted 3 display lists, which is the known idle trajectory,
   so its zero is weak evidence.

   The row fix can only shrink the recorded row set (`row_min` rises by at most
   one, `row_max` falls by at most one), so 0 hits on the old arithmetic implies
   0 on the fixed build. Re-checked on the fixed build anyway:
   `node debug/probes/fbtex.cjs --scenes 0x04,0x0a,0x10 --secs 22` →
   `0x04: tasks=39 hits=0`, `0x0a: tasks=25 hits=0`, `0x10: tasks=29 hits=0`,
   `TOTAL ... 0 over 3`.

6. **A natural route through the opening reports 0 hits too, over 196 display
   lists.** The forced scenes above carry none of the pre-state a natural path
   builds, so:
   ```
   node debug/probes/fbtex.cjs --queries 'OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a&OGRE_SCENE_LOG=1' --secs 30 --out /tmp/155-fbtex-natural
   q0: tasks=196 hits=0
   TOTAL framebuffer-as-texture loads: 0 over 1 entr(ies)
   ```
   The run's scenes are the boot (`0x19E8`), scene `0x00` and then `0x09` (the
   intro), `[scene] t=7659ms id=0x0009`. It does not reach the cathedral inside
   30 s, so this is the opening path, not the cathedral.

   With 21 scenes producing display lists, one natural path over 196 display
   lists, and the positive control firing, the item closes as no screen needs
   framebuffer-as-texture with this build, with the sweep recorded. The detector
   and its `fbtex: hits=N` counter stay, so a screen that does need it reports
   itself.

7. **The boot still renders.** `node debug/probes/boot.cjs --attempts 2 --secs 40`
   reports `RENDERED` (`nonBlack 25136`, `colorful 16528` on the first run and
   `colorful 18868` on a second), the same `nonBlack 25136` session 154 recorded.
   Both runs printed `sawRealDl=false`: `boot.cjs` stops at the first sample whose
   canvas has pixels, and this build's boot draws the publisher still before
   `#gfxstats` has counted two display lists, so the flag depends on when the
   sample lands. The pixel verdict is the check and it is unchanged.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `note_fb_write`'s row inversion is `(1 - pos) / 2 * height` with a vertex treated as an edge (`lround`), replacing `height - 1 - trunc((pos + 1) / 2 * height)`; `test_draw`'s bit-6 branch gains the row control (second colour image `0x00C28000`, fill rows 100..163, load rows 98..99); `kFb2Off` added and the `kFbOff` end-offset comment corrected to `0x00C27800`; the `ogre_gfx_debug_flags` comment records bit 6 |
| `debug/probes/testdraw.cjs` | the header records the two bit-6 negative controls |
| `docs/guides/web-probes.md` | the framebuffer-as-texture section records the row control and that the recorded rows are exact |
| `docs/guides/app-build.md` | the browser-diagnostics paragraph records both bit-6 negative controls and the row-off-by-one case |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, decision row; item 8 gains `(m)`, which closes the row fix and framebuffer-as-texture with the sweep, and `(l)` points at it |
| `docs/HANDOFF-2026-10-17-session155.md` | this file |

No probe was added to generated code and nothing under `RecompiledFuncs/`,
`Bank*Funcs/` or `RspFuncs/` was touched. `tools/RT64` still carries session 150's
fill clip uncommitted; this session did not change it. No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean three times: after
  the bit-6 row control alone (the A/B pre-fix build), after the row inversion,
  and the last one is the final tree (the only later edits are comments and docs,
  and the final build is the one every run below used except the sweep). A fourth
  invocation on the finished tree (docs only since the last build) relinks
  nothing: `[100%] Built target ogrebattle64`, and
  `build-wasm/ogrebattle64.wasm` keeps its 10:57 timestamp.
- `python3 debug/server.py` — already listening on `http://127.0.0.1:8931` from
  the previous session, so this session reused it. `curl -D -` on
  `app/web/index.html` returns `200`, `Server: SimpleHTTP/0.6 Python/3.14.7`,
  `Cross-Origin-Opener-Policy: same-origin`,
  `Cross-Origin-Embedder-Policy: require-corp`, `Cache-Control: no-store`, which
  is what `debug/server.py` sends and what the pthread build needs.
- `node debug/probes/fbtex.cjs --scenes 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18 --secs 18 --out /tmp/155-fbtex-all`
  → per-scene lines in §5, `TOTAL framebuffer-as-texture loads: 0 over 25 entr(ies)`,
  `wrote /tmp/155-fbtex-all.txt`.
- A/B pre-fix: `node debug/probes/testdraw.cjs --flags 96 --expect-hits 1 --out /tmp/155-ab-prefix`
  → `fbtex hits=2 lines=2`, `FAIL: expected fbtex hits=1, read 2`.
- Fixed: `node debug/probes/testdraw.cjs --flags 96 --expect-hits 1 --out /tmp/155-check96`
  → `fbtex hits=1 lines=1`, exit 0.
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex --out /tmp/155-check32`
  → `fbtex hits=1 lines=1`, exit 0.
- `node debug/probes/testdraw.cjs --flags 0 --expect-hits 0 --out /tmp/155-check0`
  → `fbtex hits=0 lines=0`, exit 0.
- `node debug/probes/fbtex.cjs --scenes 0x04,0x0a,0x10 --secs 22 --out /tmp/155-fbtex-final3`
  → `0x04: tasks=39 hits=0`, `0x0a: tasks=25 hits=0`, `0x10: tasks=29 hits=0`,
  `TOTAL ... 0 over 3`.
- `node debug/probes/fbtex.cjs --queries 'OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a&OGRE_SCENE_LOG=1' --secs 30 --out /tmp/155-fbtex-natural`
  → `q0: tasks=196 hits=0`, `TOTAL ... 0 over 1`.
- `node debug/probes/boot.cjs --attempts 2 --secs 40 --out /tmp/155-boot` →
  `attempt 1: sawRealDl=false rendered=true`,
  `RENDERED {... "nonBlack":25136,"colorful":16528}`;
  `--out /tmp/155-boot2` → `RENDERED ... "colorful":18868`.
- `node --check debug/probes/testdraw.cjs`.
- No native recompilation, no `make recomp`, no generated code touched. No native
  app run was needed: the change is in `app/src/web_renderer.cpp`, which only the
  Emscripten target compiles, and no frame moves. So there is no
  `OGRE_SPEED=1` capture this session.

There is no repo test harness; every check above is a ROM-dependent browser run
of 20-60 s (the sweep is 25 boots, about 9 minutes), and the probe server was an
already-running background process.

## Open

1. Framebuffer-as-texture is closed as a decision, not implemented. If a future
   screen reports a `[GFX-FBTEX]` line, the line names the texture address, the
   image address, the pitch, and both ranges; a screen whose fill or sprite is
   drawn and then loaded in the same display-list batch is the case to look at.
2. The four scenes that produce no display list under `OGRE_SCENE` (`0x02`,
   `0x03`, `0x0D`, `0x0E`) are unknown for this diagnostic. `0x02` and `0x0D`
   need the natural pre-state, so the tap route in §6 is the way to cover them;
   extending that route past the cathedral would give a natural `0x0D`.
3. Session 153's two unverified input cases stand: a physical pad on a real page,
   and two to four pads filling slots 1..3.
4. Session 152's native cathedral route (`OGRE_SCENE=new-game OGRE_STEP=2` plays
   `0x09 -> 0x0A -> 0x04` natively) is unresolved, so the browser cathedral
   backdrop still has no native frame to compare against.
5. The remaining browser-renderer items: the title's moving-cloud bands (needs a
   reference capture or developer intent), the LOADTLUT `tmem` key (needs a
   reference fact), the canvas's rounded corners and unwritten last
   column/row (cosmetic, needs developer intent on the corners), and the screens
   not compared against the reference. The `fldRegs` x/y scale and offset has no
   browser change outstanding. Framebuffer-as-texture is closed in item 1.
