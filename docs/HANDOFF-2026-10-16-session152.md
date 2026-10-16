# Session 152 handoff — the browser now reports a framebuffer-as-texture load instead of silently decoding stale RDRAM, and no reachable screen needs one

## Goal

Continue the web renderer (developer, session start). Session 151's open item 1
was framebuffer-as-texture: the gfx thread decodes a `G_LOADTILE`/`G_LOADBLOCK`
out of RDRAM at record time (`decode_texture_rect`, `app/src/web_renderer.cpp`),
while a colour image the same display list rendered into is still in a GL
framebuffer and reaches RDRAM only when the browser main thread writes it back
(`readback_all_framebuffers`, at present or at a `sync_framebuffers` request), so
such a load decodes bytes the draw never wrote. Session 151 named two candidate
fixes and no screen that needed one. This session built the detector first, as
session 151 said to.

## Result

1. **The renderer reports the condition: `[GFX-FBTEX]`, and `fbtex: hits=N` in
   `#gfxstats`.** The gfx thread already knows everything the test needs: a
   recorded draw's `DrawCmd` carries the colour image it renders into
   (`fb_address`, `fb_row_bytes`, `fb_height_px`, `fb_siz`), and a texture decode
   knows the guest byte range it reads (`t.image_address`, the row stride and the
   tile's rect). `fb_written_` holds one entry per colour image a recorded draw
   has written since the last writeback, with the guest rows those draws covered.
   `note_fb_texture` fires when a decode's range overlaps such an image **and**
   the rows it reads intersect the rows a draw actually wrote. The ranges are
   dropped when the main thread publishes a writeback, through the new
   `fb_writeback_epoch_` counter, so a load that reads pixels already in RDRAM is
   not reported.

2. **The row test is required, and the first version of the check was wrong
   without it.** A range-only test reported 142 hits (scene `0x0A` 9, scene
   `0x10` 133) that are all the same false positive: RT64 and the browser size a
   colour image to `max(viewport, VI height, scissor bottom + 1)`, so a 640x480
   mode's target is 640x**481** rows while the game writes rows 0..479
   (`[GFX-FBSYNC] wrote back 0x1BA780 640x481 siz=2 pitch=1280 guestRows=[0,479]`).
   The heap immediately past the image holds the game's own textures, and the
   reads were at byte offset `614488` (row 480.07) and `614456` (row 480.04):
   inside the declared image, outside everything rendered. Reading RDRAM is
   correct there. The refined check reports `rows=[480,480] drawn rows=[0,479]`
   and does not fire.

3. **A positive control makes a zero-hit sweep mean something.** `test_draw`
   (`Module._ogre_gfx_test_draw`) now appends, under `ogre_gfx_debug_flags` bit 5,
   a colour image at hidden RDRAM `0x00C02000` (`G_SETCIMG` 320 wide RGBA16), a
   red `G_FILLRECT` over it, and a `G_LOADTILE` + `G_TEXRECT` that samples that
   same address. `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex`
   prints `fbtex hits=1 lines=1` and

   ```
   [GFX-FBTEX] load tex=0xC02000 rect=32x32 stride=640 fmt=0 siz=2 reads colour
   image 0xC02000 pitch=640 h=240 siz=2 rows=[0,31] drawn rows=[0,239] by an
   earlier draw in this batch; the decode read RDRAM, not the rendered pixels
   ```

   The same probe with `--flags 0` prints `fbtex hits=0 lines=0`, so the control
   is the flag and not the frame.

4. **No reachable screen needs framebuffer-as-texture.** Two sweeps, both on the
   final build, both with the control firing:

   - all 25 scene ids, one forced boot each (`OGRE_SCENE=0x00`..`0x18`, 18 s
     each): `TOTAL framebuffer-as-texture loads: 0 over 25 scene(s)`. 19 of the
     25 submitted a real display list (`tasks` 2..107); the forced `0x04` entry
     submitted only the boot task, and `0x01`/`0x02`/`0x03`/`0x0D`/`0x0E`
     submitted none or trapped out of bounds, as before.
   - three natural routes, 120 s each: the unforced boot (351 display lists;
     intro `0x09` at t=8.3 s, publishers `0x0A` at t=69.6 s, title `0x04` at
     t=88.4 s), `OGRE_SCENE=new-game&OGRE_STEP=2` (443 display lists; scene
     `0x02` at t=9.1 s, `0x0D` at t=10.9 s — the New Game cathedral), and a
     `OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,...` route (471 display
     lists; `0x09` → `0x0A` → `0x04`): 0 hits each.

   The instrument stays in the tree, so a screen that does need the feature
   reports itself: the counter is in `#gfxstats` and the first 24 distinct
   `(texture, image)` pairs print once.

5. **The native route session 151 recorded for the cathedral does not work in
   this build, and the browser's does.** Session 151 wrote that
   `OGRE_SCENE=new-game OGRE_STEP=2` reaches the cathedral on both sides
   (native `id=0x0002` at t=5.3 s, then `id=0x000D` at t=5.7 s). With
   `OGRE_MOD_TEST=1` (required for a scripted native run: without it the app
   prints `[launcher] waiting for a ROM` and writes no capture even with the ROM
   on the command line) the native run logs `forcing scene 0x02`, `forced scene
   0x02 is active`, `seeded step 2 while scene 0x02 runs`, and then
   `[scene] t=2700ms id=0x0009` → `id=0x000A` → `id=0x0004`: the forced New Game
   loader is overridden by the boot's own scene sequence and the run lands in the
   attract loop. The same query in the browser reaches `0x0D` (cathedral). This
   is why no browser-vs-native frame comparison of the cathedral was made this
   session; the browser capture exists (`/tmp/web-cath-00.png`, the cathedral
   floor and the angel statue) but there is no native frame of the same screen
   to compare it with.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | the `[GFX-FBTEX]` diagnostic (`fb_written_`, `note_fb_write`, `note_fb_texture`, `fb_writeback_epoch_`, `fbtex_hits()`), the `fbtex: hits=` line in `ogre_gfx_stats()`, and the `test_draw` positive control under debug flag bit 5 |
| `debug/probes/fbtex.cjs` | new probe: sweep scene ids or arbitrary query strings and report every `[GFX-FBTEX]` line, with a positive-control note |
| `debug/probes/testdraw.cjs` | `--flags N` and `--expect-fbtex`, so the synthetic control is one command |
| `debug/package.json` | `npm run fbtex` |
| `docs/guides/web-probes.md` | the `fbtex.cjs` row, the `testdraw.cjs` flags, the positive-control recipe |
| `docs/guides/app-build.md` | the browser diagnostics paragraph records `[GFX-FBTEX]`, `fbtex: hits=` and debug flag bit 5 |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session152.md` | this file |

No probe was added to generated code and nothing under `RecompiledFuncs/`,
`Bank*Funcs/` or `RspFuncs/` was touched. `tools/RT64` carries session 150's fill
clip (uncommitted; this session did not change it, and `make patch-check` PASSes,
so every working-tree change there is described by `patches/rt64-ob64.patch`).
No `probe152`-tagged code exists anywhere.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` clean (four times; the sweeps
  in finding 4 ran on the build before the last, which differs from the final one
  by a comment only, and the final build re-ran the control and the three-scene
  check below).
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex` → `hits=1 lines=1`
  and the `[GFX-FBTEX]` line above; `--flags 0` → `hits=0 lines=0`.
- `node debug/probes/fbtex.cjs --secs 18 --out /tmp/fbtex-sweep2` → `0 over 25`.
- `node debug/probes/fbtex.cjs --secs 120 --queries 'OGRE_SCENE_LOG=1||
  OGRE_SCENE=new-game&OGRE_STEP=2&OGRE_SCENE_LOG=1||OGRE_TAP_MS=1200&
  OGRE_TAP_BUTTON=start,a,start,a,a,...&OGRE_SCENE_LOG=1'` → `0 over 3`.
- `node debug/probes/fbtex.cjs --scenes 0x0a,0x10,0x04 --secs 22` → 0 on the
  final build (`tasks` 15/21/29), including the two scenes whose range-only hits
  the row test removes. An earlier run of the same two scenes gave `0x0A` 17
  display lists and `0x10` none; sweep 2's `0x10` entry is `tasks=16`.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` → `RENDERED`
  (`nonBlack 25136 / colorful 18868`).
- Native route check: `OGRE_NO_AUDIO=1 OGRE_MOD_TEST=1 OGRE_BG=0
  OGRE_SCENE=new-game OGRE_STEP=2 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=20000
  ./build-app/ogrebattle64 assets/ogre64.z64` (and the same with `OGRE_SPEED=1`,
  and with `OGRE_CAPTURE_TARGET`/`OGRE_CAPTURE_PRESENT`) → the `0x02 → 0x09 →
  0x0A → 0x04` sequence above, at both speeds.

## Open

1. **Framebuffer-as-texture is instrumented and unneeded so far, not
   implemented.** Every screen this session reached reads RDRAM correctly at
   every texture load. `fbtex: hits` in `#gfxstats` and the `[GFX-FBTEX]` lines
   are how a screen that needs the feature is found: run one there, and the log
   names the texture address, the image's address and pitch, and the two row
   ranges. Do not implement the feature before that, and do not guess which
   screen needs it - session 134/136's framebuffer tile copies were a *depth*
   image overlapping heap, and this detector reports colour images the game
   rendered into.
2. **The native cathedral route needs re-deriving** (finding 5). The browser side
   works, so the comparison is one command away once the native side reaches
   `0x0D` with `OGRE_STEP=2`.
3. The `[GFX-FBTEX]` row test compares against the whole *image* rows a draw
   covered, not the columns, so it can report a load that lands in a drawn row but
   outside the drawn columns. That direction is the safe one (it over-reports), and
   no hit occurred, so it was left as it is.
4. Session 148/149/150/151's items stand: the canvas's rounded corners and
   unwritten last column/row (cosmetic), the LOADTLUT `tmem` key (latent: every
   traced load is `tmem=0x100`, `palette=0`, where the browser's `palette * 16`
   bank and RT64's `tmem << 3` agree), the `fldRegs` x/y scale and offset (RT64
   uses them only in `VI::fbSize`, which the browser ports, and not in
   `VideoInterfacePS` or `cropRectangle`), the title's moving-cloud bands, and the
   screens not compared against the reference (the cathedral backdrop, the world
   map's sprites, the tutorial practice field).
