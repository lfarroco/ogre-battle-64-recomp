# Session 196 handoff — the mission field is not darker or flatter than native, and the rectangle switch leaves the other 2D screens alone

## Goal

Continue session 195's open item 1 (the other filtered 2D screens with
`OGRE_RECT_HALFPIX` on) and item 2 (the mission field reported as flatter and
darker than native).

## Result

1. **Session 195's item 2 is withdrawn: the mission field is not darker or
   flatter than native.** On the same NOTE screen (Castle Dunkweld, "Losing
   condition: Invasion of your headquarters. Death of OTA."), the browser's
   static field regions are within 1-3 levels of RT64's mean, and their
   standard deviation and gradient are **higher**, not lower. The field's left
   region (game pixels `0,140..45,230`) measures browser mean
   `[26.8, 28.4, 13.3]` against native `[26.3, 27.8, 12.4]`, standard deviation
   `[16.9, 16.0, 17.3]` against `[13.9, 12.6, 13.8]`, and mean absolute
   difference per channel `4.82`; the right region `5.25`, the bottom `6.19`,
   the top-left `1.20` and the top-right `2.09`. The two frames are different
   captures (browser canvas at 2x, native present at 3x), so the static field
   regions are the datum, the same method session 171 used. What made the
   earlier frame look dark is the briefing's own dim over the field and the
   screenshot's size, not a renderer difference: the browser's means are if
   anything 1 level brighter than native's.

2. **`OGRE_RECT_HALFPIX` changes no pixel on the boot intro or a publisher
   still.** Two forced-title attempts (`--scene title`) settled on scene
   `0x0009` (the boot soldiers) and `0x000A` (a publisher still), not `0x04`.
   The static frames of those screens are byte-identical between the switch-on
   and switch-off runs: the intro hold is `md5 6ffb91f869802ef55df3ca7e9371f6d5`
   in both, and the publisher hold is
   `md5 880a77a47535d93f3c453fcca926f26b` in both. The animated intro frames
   differ between the runs because the two boots are not phase-locked, so only
   the holds are compared. Those screens are point-sampled or 1:1 rectangles,
   where `floor(i + 0.5)` and `floor(i)` select the same texel; the briefing
   remains the positive control (`docs/proofs/web-mission-briefing-recthalfpix-on-session195.png`
   against `-off-session195.png`). The mission briefing's framed illustration is
   also clean on the current build
   (`docs/proofs/web-mission-briefing-illustration-session196.png`, a 4x crop of
   the Dunkweld frame): no green hairlines or outline.

3. **A live `OGRE_RECT_HALFPIX` switch is now debug flag bit 15.** The env
   switch is read once when the renderer is constructed, so the build-wide A/B
   is two runs. `ogre_gfx_debug_flags(32768)` now turns the half-step corner-UV
   shift off for the draws that follow, and clearing it turns the shift back on,
   so one screen can be captured both ways in one run. `debug/probes/recttoggle.cjs`
   is the probe: it waits for a scene regex, settles, then writes
   `-halfpix-on`, `-halfpix-off` and `-halfpix-on2`. The default is unchanged
   (bit 15 clear). `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` is clean.
   **The probe was not run to completion this session**: the host load stayed at
   11-28 (Spotlight's `mdworker_shared` and the Playwright GPU process), and two
   attempts waited 33 and 36 minutes without reaching scene `0x0003`; the game
   never drew the briefing. The switch is one expression in each of the two
   rectangle paths and compiles clean, but its same-run pair is the first thing
   to run on an idle host.

## Evidence

- **Mission field, browser against native.** Browser
  `docs/proofs/web-mission-briefing-dunkweld-session196.png` (640x481 canvas;
  game pixel `(x,y)` is canvas `(2x+1, 2y+2)`) against native
  `docs/proofs/native-mission-intro-dunkweld-session192.png` (960x720 present;
  game pixel `(x,y)` is `(3x, 3y)`). The two NOTE boxes and the framed
  illustration sit at the same place. Mean absolute difference per channel over
  the field regions: `4.82`, `5.25`, `6.19`, `1.20`, `2.09`; the means and
  spreads are in Result 1. `docs/proofs/web-mission-field-vs-native-session196.png`
  places the two frames side by side at 320x240 (browser left, native right).
- **The same reading on the Castle Burgund briefing.** Browser
  `docs/proofs/web-mission-briefing-recthalfpix-on-session195.png` against native
  `docs/proofs/native-mission-briefing-session195.png`: left field mean
  `[30,32,1]` against `[30,33,3]`, right `[28,29,9]` against `[29,30,11]`,
  meanAbs `6.90` and `5.67`. The bottom region's `11.76` includes the NOTE
  box and the illustration, not field.
- **The other-screen A/B.** Captures at
  `/tmp/w196-title-on/frame-NN.png` and `/tmp/w196-title-off/frame-NN.png`
  (`intro-capture.cjs --seconds 30/14 --interval-ms 1000 --scene title`); the
  identical hashes above are the static holds. The off run passes
  `OGRE_RECT_HALFPIX=0` in `--url`.
- **The field without the NOTE.**
  `docs/proofs/web-mission-field-dunkweld-session196.png` is the same route's
  wider map view (`f-01`); the terrain, river, road and castles are the same
  screen native draws before the briefing freezes it.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean after the code change.
- Browser route for the field: `atmark.cjs --wait 'id=0x0003' --shots 8
  --gap 2500 --timeout 420` with
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6,0x03:a:30`.
  Shot 1 is the whole map, shot 2 the briefing; shots 2-7 are identical, so the
  screen is static.
- The field numbers are computed offline from the PNG files (a small PNG reader,
  no PIL/numpy on this host).
- The same-run `recttoggle.cjs` A/B is **not** a passed check; it did not reach
  the screen under load. The build-wide evidence for the switch itself is
  session 195's.
- `git status --short` shows `app/src/web_renderer.cpp`,
  `debug/probes/recttoggle.cjs`, the docs and the four proof PNGs.
  `tools/RT64` still carries the local `OGRE_*` diagnostics it had before this
  session; this session did not touch it.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | debug flag bit 15 (`kDebugRectHalfpixOff`) and `rect_halfpix_enabled()`; `draw_texrect` and `draw_obj_rect` consult it; the flag comment block documents it |
| `debug/probes/recttoggle.cjs` | the live on/off/on A/B probe for one screen |
| `docs/proofs/web-mission-field-vs-native-session196.png` | browser and native field side by side at 320x240 |
| `docs/proofs/web-mission-field-dunkweld-session196.png`, `-briefing-dunkweld-session196.png` | the current build's map view and briefing |
| `docs/proofs/web-mission-briefing-illustration-session196.png` | the briefing's framed illustration at 4x, clean |
| `docs/HANDOFF-2026-10-26-session196.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/web-probes.md`, `docs/guides/app-build.md` | status, log, the session-196 durable row, index, the `recttoggle.cjs` row and the live bit-15 note on `OGRE_RECT_HALFPIX` |

## Probes used and reverted

None in generated code and none in `tools/RT64`. The two temporary scripts that
drove the mission route and the crop live in `/tmp` only; the kept probe is
`debug/probes/recttoggle.cjs`. `OGRE_RECT_HALFPIX` and debug flag bit 15 stay as
diagnostics.

## Open

1. **Run `debug/probes/recttoggle.cjs` on an idle host.** The expected result is
   the same on/off difference session 195 measured across two builds, now on one
   static screen. If the pair is identical, the flag is not reaching the draw
   paths and must be checked before the probe is trusted.
2. **The mission field's residual is dither and phase, not brightness.** The
   static regions are within 1-3 mean levels; the remaining `4.8-6.2` pointwise
   difference is the writeback dither plus the two captures' different phases.
   A frame-locked native/browser pair (same game time, not just the same NOTE)
   would settle whether any of it is renderer-side.
3. **Session 195's item 1 is closed for the boot intro and a publisher still,
   and for the briefing (session 195).** The title prologue and the world map's
   water/scrolling layers were not captured this session; both are point-sampled
   or 1:1, so the switch should not move them, and the same probe can be pointed
   at them when the host is idle. The title's `--scene title` force landed on
   scenes `0x09`/`0x0A` in both attempts rather than `0x04`; that force is worth
   re-checking before a title A/B is run.
4. **Session 195's item 3 is unchanged.** The framebuffer-as-texture copy
   (`OGRE_FBTEX`) still needs RT64's RDRAM re-upload and per-workload writeback,
   and no reached screen needs it now that the briefing's rectangles sample
   correctly.
