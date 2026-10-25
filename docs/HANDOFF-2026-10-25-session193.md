# Session 193 handoff — the mission briefing's loads read RDRAM correctly, and the framebuffer copy is not their source

## Goal

Continue session 192's open item 1: implement the framebuffer-as-texture tile
copy so the mission start screen's framed illustration stops showing green
lines.

## Result

The copy was implemented, and a same-screen A/B shows it must not be enabled
with the renderer's current model. It also corrects session 192's reading of
what those loads are.

1. **The copy exists and does not write RDRAM.** `OGRE_FBTEX=1` makes a texture
   load whose source range lies inside a colour image the RDP has drawn ask the
   browser main thread for a private copy of that image's rendered pixels. The
   gfx thread publishes the draws recorded so far, the main thread reads the
   colour image's framebuffer object into a buffer in the guest byte layout, and
   the load copies TMEM out of that buffer (`LoadSource::shadow`). Nothing is
   written to RDRAM, so the scene-0x12 sprite-cache corruption is not reachable
   from this path. `[GFX-FBTEX-SYNC]` reports each copy.

2. **The copy is wrong for colour image `0x228080`, and the reason is the
   missing RDRAM re-upload.** On the same static briefing screen, with the route
   `title:start:6,0x12:a:6` and no scene-0x03 taps:

   - copy off: the NOTE reads "Winning condition: Taking over enemy
     headquarters." and the label reads "Castle Burgund"
     (`docs/proofs/web-mission-briefing-copy-off-session193.png`);
   - copy on: the NOTE text and the label become white bars
     (`docs/proofs/web-mission-briefing-fbtex-on-session193.png`).

   A dump of the copy itself (`ogre_gfx_debug_fb_source`) decodes as a 320x240
   image that is **white** everywhere, with a few coloured pixels at the right
   edge (`docs/proofs/web-mission-fbtex-gpu-copy-session193.png`). The GPU copy
   of `0x228080` is a white frame, while RDRAM at the load time holds the
   CPU-composed briefing. The `[GFX-FBCONTENT]` line states the substitution:
   `image 0x228080 320x240: exact rect x=[0,319) y=[1,240)`, seven full-image
   rect draws, so the whole image is replaced.

   RT64's tile copy is authoritative only because RT64 re-uploads RDRAM into the
   framebuffer when the CPU changed it (`State::checkRDRAM` ->
   `FramebufferManager::checkRAM`/`uploadRAM`) and writes the framebuffer back at
   the end of every workload (`renderToRDRAM`, then `hashTracking`). Its
   `RAMHash` guard refuses the tile copy exactly when RDRAM changed since that
   writeback. This renderer has neither the re-upload nor the per-workload
   writeback, so for this screen RDRAM is the authoritative source and the load
   must read it; the copy stays off.

3. **Session 192's premise is corrected.** The 24 `[GFX-FBTEX]` loads at scene
   `0x03` are not reading stale pixels for the briefing. With the copy off, the
   same screen's text and label render from those loads' RDRAM bytes, and they
   are readable. The green lines around the framed illustration are present with
   the copy on and off, so they are not a stale-RDRAM defect.

4. **A mid-list RDRAM writeback reproduces the known heap crash.** The first
   implementation flushed the image's drawn rectangles into RDRAM at the load.
   The mission run then died in `func_80071950` (`func_800712C4` <-
   `func_ovlN_801D3110` <- `func_ovlN_801AE880` <- `func_ovlA_8019F5FC` <-
   `func_800765D8` <- `func_80072398` <- `func_8008AFE0`), the function session
   170 named when a writeback put pixels over the sprite cache at `0x243E10`.
   Reverted. `readback_framebuffer` and the draw-loop row tracking are again the
   pre-session row-band code.

5. **Default behaviour is unchanged from session 192.** The mission briefing
   renders as before (readable NOTE, framed Castle Dunkweld, green lines) and the
   run does not crash. The copy machinery is opt-in.

## Evidence

- Same-screen A/B, one run, `ogre_gfx_debug_flags(8192)` toggled between frames:
  `fbtex: hits=0 copies=256` (on), then `copies=267` (off), then `copies=267`
  (on again). The screenshots are the two proofs above.
- The copy bytes: `ogre_gfx_debug_fb_source` returned 153600 bytes for
  `0x228080` (320x240 RGBA16, pitch 640). The decode writes each guest 16-bit
  value from `b[off^2] | b[off^2+1] << 8`; the result is white.
- `[GFX-FBCONTENT]` names the substituted rects: `x=[0,319) y=[1,240)`, seven
  entries (one per rect draw) for `0x228080`.
- The first copy took 2341-2636 ms under host load 19; the retry design (a second
  request after a 2 s timeout) reduced stale loads to 0 on the copy-enabled runs.
  `[GFX-FBTEX-SYNC] ... no flush serviced the copy` is the timeout, and the
  backoff skips further copies until a flush happens.
- Copy-enabled mission run: 0 `[GFX-FBTEX]` stale lines, 0 crashes, final frames
  are the mission field.
- Default mission run (`/tmp/w193-final-*`): scene `0x03`, 0 crashes, 0 copies,
  the briefing of `web-mission-briefing-session193.png`.
- The crash with the writeback version:
  `memory access out of bounds` at `func_80071950`, immediately after
  `[GFX-FBSYNC] wrote back ...` in the mission run.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` (the `cmake --build
  build-wasm` path) is clean; the exported `_ogre_gfx_debug_fb_source` is in
  `app/CMakeLists.txt`'s `EXPORTED_FUNCTIONS` list.
- Browser route used for the A/B and the proof:
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`.
  With no `0x03` entry the briefing stays on screen, which is what makes the
  A/B possible. With `0x03:a:30` the run reaches the field.
- `git status --short` shows only `app/CMakeLists.txt`,
  `app/src/web_renderer.cpp` and the docs/proofs.
- No generated code (`RecompiledFuncs/`, `Bank*Funcs/`) was touched and
  `tools/RT64` is unchanged.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | opt-in framebuffer-source copy (`OGRE_FBTEX`, `LoadSource`, `fetch_fb_source`, `build_image_source`, `service_fb_source`, debug flag bit 13); `[GFX-FBTEX-SYNC]`/`[GFX-FBCONTENT]` diagnostics; `ogre_gfx_debug_fb_source`; `fbtex: hits=/copies=` stats; default unchanged (copy off, row-band writeback) |
| `app/CMakeLists.txt` | export `_ogre_gfx_debug_fb_source` |
| `docs/proofs/web-mission-briefing-session193.png` | the briefing in the shipped default build (copy off) |
| `docs/proofs/web-mission-briefing-copy-off-session193.png` | the A/B pair's copy-off frame, same run and screen as the copy-on frame |
| `docs/proofs/web-mission-briefing-fbtex-on-session193.png` | the same screen with `OGRE_FBTEX=1`: white bars in the NOTE and label |
| `docs/proofs/web-mission-fbtex-gpu-copy-session193.png` | the 153600-byte copy decoded as 320x240 RGBA16: a white frame |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md` | status, log and decision 175 |
| `docs/HANDOFF-2026-10-25-session193.md` | this file |

## Probes used and reverted

The mid-list RDRAM writeback was added to the load path and reverted in the same
session; the final source has no write from a load into RDRAM. The copy path, the
`OGRE_FBTEX`/`OGRE_FBWRITE` switches, debug flag bit 13 and the
`ogre_gfx_debug_fb_source` export stay as opt-in diagnostics: the copy path is
the building block for the re-upload model, and the switches are the A/B that
proves the current default. No generated code carries a probe.

## Open

1. **The green lines are not stale RDRAM.** They persist with the copy on and
   off. The border is built from the 1-row loads: `[GFX-FBTEX]` shows them as
   `fmt=4 siz=2` and `fmt=0/2 siz=2`, one row each, and the green sits on the
   seams between the strips. The next check is the tile decode and sampling of
   those strips (`OGRE_TEX_TRACE`, `[GFX-TEXTRACE]`, `ogre_gfx_debug_tex`), and
   a comparison with RT64's `TextureDecoder.hlsli` path, before changing the
   decoder.
2. **Enable the copy only after the re-upload model exists.** At the start of a
   display list, a changed RDRAM range must be uploaded into the colour image's
   GPU copy and `RAMHash` updated (RT64 `checkRAM`/`uploadRAM`); the framebuffer
   must also be written back at the end of every workload (`renderToRDRAM`), or
   the re-upload discards pixels the GPU has not flushed. The white copy dump and
   the A/B pair above are the acceptance evidence: with the model in place,
   `OGRE_FBTEX=1` must render the briefing at least as well as the copy-off
   screenshot.
3. The field is still flatter and darker than native. Its light is blue
   (`amb=77,77,127`) and the mission is 3D; it is not explained by the frame
   path.
