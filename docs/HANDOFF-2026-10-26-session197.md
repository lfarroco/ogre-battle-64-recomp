# Session 197 handoff — the same-run rectangle A/B, and the framebuffer copy with RT64's stale-RDRAM test

## Goal

Continue session 196's open item 1 (a same-run `OGRE_RECT_HALFPIX` A/B on the
briefing) and session 195/196's item 3 (the framebuffer-as-texture copy), using
subagents for the independent audits.

## Result

1. **The same-run A/B now runs, and the blocker was the browser launch.** The
   shared probe harness launches Chromium without GPU args, so the wasm game
   renders through SwiftShader and took 33-36 minutes without reaching the
   mission under host load. `debug/probes/recttoggle.cjs` now launches its own
   browser with `--enable-gpu --use-angle=metal --ignore-gpu-blocklist` (the
   args `intro-capture.cjs` already used); the briefing is reached in about
   seven minutes. `--gpu 0` restores the SwiftShader path.

2. **The same-run A/B confirms session 195 on one build and one screen.** With
   the route that holds the briefing (`title:start:6,0x12:a:6`, no `0x03` taps)
   and `[GFX-FRAME-DUMP]` as the arrival signal, one run gives:

   | frame | green-dominant pixels in the illustration rect | seam rows |
   |---|---|---|
   | `OGRE_RECT_HALFPIX` on | 0 / 7872 | 0 / 2016 |
   | off (debug bit 15, `ogre_gfx_debug_flags(32768)`) | 399 / 7872 | 175 / 2016 |
   | on again | 0 / 7872 | 0 / 2016 |

   The on and off shots are 700 ms apart on the same static page; the on/on2
   pair measures `meanAbs 2.44` from the screen's own animation, and the green
   count is unchanged. Proofs:
   `docs/proofs/web-mission-briefing-recttoggle-on-session197.png`,
   `-off-session197.png` and `-zoom-session197.png` (on left, off right; the
   off frame has the band-seam hairlines and the green frame/pin outline).

   A scene-line trigger (`id=0x0003`) is wrong for this screen: the briefing
   changes its NOTE page a few seconds after entry, and the first attempt's on
   and off shots were two different pages (one with the framed illustration,
   one the "Death of Prince Yumil" page). The picture's `[GFX-FRAME-DUMP]` line
   is the arrival signal.

3. **The framebuffer-as-texture copy now applies RT64's `checkRAM` test, and
   `OGRE_FBTEX=1` renders the briefing's NOTE and label correctly.** RT64 refuses
   a tile copy when `XXH3(RDRAM) != Framebuffer::RAMHash`
   (`hle/rt64_rdp.cpp:177-196`), and `FramebufferManager::checkRAM`/
   `uploadRAM` re-upload changed RDRAM (`rt64_framebuffer_manager.cpp:909-964`).
   The browser now keeps the same hash on each `GlFramebuffer`
   (`rdram_hash`/`rdram_hash_valid`), refreshed by `upload_framebuffer` and
   `readback_framebuffer`, and refuses the GPU rectangles when the current
   RDRAM differs:

   - on the gfx thread, `fetch_fb_source` hashes the image before publishing the
     draws, so a stale image costs one hash and no flush round trip
     (`[GFX-FBTEX-STALE]`);
   - on the main thread, `build_image_source` repeats the check before it
     substitutes `exact_rects`, as a second guard.

   The copy stays opt-in; `OGRE_FBTEX=1` with the new guard measures, on the
   same screen and run, `meanAbs 0.057` over the NOTE box and `0.024` over the
   label against the copy-off frame (max 1), and `7.28` over the animated field
   against a `3.92` same-state animation floor. The log has four
   `[GFX-FBTEX-STALE] source 0x228080` lines, zero "no flush serviced the copy"
   lines and no crash. The old behaviour was the white bars of session 193.
   Proofs: `docs/proofs/web-mission-briefing-fbtex-guarded-on-session197.png`
   and `-off-session197.png`.

4. **The read-only audits corrected the record.** The top-of-file limitation
   list was stale: TEXEL1, `G_TEXTURE_GEN`, lighting and LOADBLOCK `dxt` are
   implemented; only point lighting, `ZMODE_DEC` and the VI origin offset
   remain. The list and the related FBTEX comments are corrected. The audits
   also found, but did not reach on any screen:
   - `G_TEXRECTFLIP` applies half of `dv` while its quad interpolates
     `dv*h/w`, so the half-step is wrong for a non-square flip; the browser's
     flip mapping also differs from RT64's transposed quad. No log or doc shows
     OB64 issuing a flipped texrect.
   - A copy-mode `G_TEXRECT` does not reproduce RT64's `dsdx >>= 2` and
     `lrx |= 3`/`lry |= 3` (`rt64_rdp.cpp:1685-1690`), and a fractional rect
     does not reproduce RT64's `vFractionOffset` (`:1586-1590`).
   - The title residual still open after session 162 is rows ~81..119 (the top
     edge of the repaired fog, a ±1-3 level wave), not the session-159
     rows 195..232. Its next instrument is a post-repair native draw census
     plus a per-draw browser row readback.

## Evidence

- **Rect A/B command:**
  `node debug/probes/recttoggle.cjs --timeout 1500 --out /tmp/w197-rect4/r
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'`.
  It prints `MARKER \[GFX-FRAME-DUMP\]; shots halfpix-on,halfpix-off,halfpix-on2`,
  exit 0. The probe adds `OGRE_FRAME_DUMP=1`.
- **FBTEX command:** the same with
  `--flag 8192 --name fbtex` and `OGRE_FBTEX=1` added to the query. `--flag`
  is the debug bit the second shot sets (8192 = bit 13, the copy-off switch;
  32768 = bit 15, the rectangle half-step off), and `--name` labels the files.
  The run prints `MARKER \[GFX-FRAME-DUMP\]; shots fbtex-on,fbtex-off,fbtex-on2`,
  exit 0.
- **The FBTEX log:** `/tmp/w197-fbtex2/r-pagelog.txt` has 4
  `[GFX-FBTEX-STALE] source 0x228080` lines, 24 `[GFX-FBTEX]` load reports,
  0 sync timeouts and 0 `memory access out of bounds`.
- **The same screen, three states:** the rect run's on/off pair and the FBTEX
  run's on/off pair are the same Castle Burgund briefing. The illustration is
  clean in both on frames; the copy-on frame has readable NOTE and label text.
- **The forced-map attempt:** `recttoggle.cjs --wait 'id=0x0005'
  --settle 4000 --timeout 900 --url '...?OGRE_SCENE=map&OGRE_SCENE_LOG=1'`
  wrote no screenshot before it was stopped at about nine minutes; no
  `[scene] id=0x0005` line means the force did not take.
- **The audits' references:** RT64 `checkRAM`/`uploadRAM` at
  `tools/RT64/src/hle/rt64_framebuffer_manager.cpp:909-964`, the tile-copy hash
  at `hle/rt64_rdp.cpp:177-196`; the flip/copy-mode/fractional differences at
  `hle/rt64_rdp.cpp:1582-1602`, `:1685-1690`, `:1586-1590`; the title target at
  `docs/HANDOFF-2026-10-17-session163.md` §4 and `-session164.md` Open 2.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean after the code change
  (and again after the comment corrections); the wasm is `21209913` bytes on the
  first build and is relinked by the second.
- Both probes exit 0 with all three frames written.
- No generated code (`RecompiledFuncs/`, `Bank*Funcs/`) and no `tools/RT64`
  file was edited. `grep -rn "probe19" app/ debug/ RecompiledFuncs/ Bank*Funcs/`
  is empty of new probes; the only kept instruments are the existing flags, the
  `[GFX-FBTEX-STALE]` line and `debug/probes/recttoggle.cjs`.
- `git status --short` shows the files in the table below and the pre-existing
  `m tools/RT64`.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `GlFramebuffer::rdram_hash`/`rdram_hash_valid`; `framebuffer_rdram_hash`, `publish_fb_ram_state`, `fb_ram_changed`; the gfx-thread pre-check in `fetch_fb_source` and the main-thread guard in `build_image_source`; `[GFX-FBTEX-STALE]`; corrected limitation and FBTEX comments |
| `debug/probes/recttoggle.cjs` | GPU launch (`--gpu 0` optional), `[GFX-FRAME-DUMP]` arrival default, `--flag`/`--name`, `OGRE_FRAME_DUMP=1` auto-added |
| `docs/proofs/web-mission-briefing-recttoggle-{on,off,zoom}-session197.png` | the same-run rectangle A/B |
| `docs/proofs/web-mission-briefing-fbtex-guarded-{on,off}-session197.png` | the copy with the hash guard and the copy-off control |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/web-probes.md`, `docs/guides/app-build.md` | status, log, the session-197 durable row, index, the `recttoggle` update and the FBTEX hash rule |
| `docs/HANDOFF-2026-10-26-session197.md` | this file |

## Probes used and reverted

None in generated code and none in `tools/RT64`. The read-only audits changed
nothing. `OGRE_RECT_HALFPIX`, debug bits 13/15, `recttoggle.cjs` and
`[GFX-FBTEX-STALE]` stay as diagnostics. `OGRE_FBTEX` stays opt-in; the hash
guard does not make the whole-image copy cheap when it does fire.

## Open

1. **`G_TEXRECTFLIP`'s half-step is wrong when width != height** (it subtracts
   `0.5*dv` where the quad interpolates `dv*h/w`), and the browser's flip
   mapping differs from RT64's. No reached screen is known to issue one; a
   census of `G_TEXRECTFLIP` in a natural run is the cheap next check.
2. **Copy-mode `G_TEXRECT` and fractional rectangle coordinates** do not match
   RT64's `dsdx >>= 2`/edge pad and `vFractionOffset`. Session 195's
   "S2DEX macroblocks are `G_CYC_COPY`" conflicts with its own trace
   (`cycle=0`); a cycle-type census on the njpeg path decides it.
3. **The title residual is rows ~81..119**, the top edge of the repaired fog, a
   ±1-3 level wave. Next: a native post-repair draw census for one row plus a
   per-draw browser row readback (session 196's subagent report has the exact
   insertion points).
4. **FBTEX remains opt-in.** The guard makes the copy correct on the briefing
   and avoids the white bars, but a copy still costs a whole-image readback
   when RDRAM matches. The RT64 parity model (re-upload on a mismatch) is not
   implemented; it is not needed for any reached screen.
5. **The world map/title rectangle A/B** was not completed. The GPU launch
   makes the probe cheap, but a forced-map attempt
   (`OGRE_SCENE=map`, `--wait 'id=0x0005'`, `--timeout 900`) wrote no frames in
   about nine minutes, so the scene force is the blocker, not the rectangle
   switch. The title has the same force problem: `--scene title` landed on
   scenes `0x09`/`0x0A` in session 196. Both need a played state or a fixed
   force before the A/B is meaningful.
