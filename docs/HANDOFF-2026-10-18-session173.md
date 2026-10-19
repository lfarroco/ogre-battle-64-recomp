# Session 173 handoff — the browser's depth target is keyed by the depth image, and the mission's "depth flattens the top band" is not a depth effect

## Goal

Find and fix the ordering problem behind session 172's result — with `OGRE_DEPTH=1`
the mission `0x03` frame's top rows 0..38 measured `9.52` mean abs against native
`/tmp/w168/nat2/t.800` instead of `0.93`, and the picture was the same with z
inverted or the z scale zeroed — and, if it is fixed, enable the depth test by
default.

Timebox: 100 minutes (the round ran to about 115; one extra capture series is the
reason and it produced the discriminating measurement).

## Result

1. **Hypothesis A is confirmed as the structure and fixed.** The browser's depth
   buffer was a child of the colour framebuffer (`GlFramebuffer::depth_rb`, one per
   `G_SETCIMG` address, cleared to 1.0 when created). RT64's depth
   `RenderTarget` is keyed by the **depth image** — `rt64_state.cpp:1354-1365`
   builds `fbKey.depthTargetKey = RenderTargetKey(depthFb->addressStart,
   depthFb->width, depthFb->siz, Framebuffer::Type::Depth)` from
   `framebufferManager.get(depthImg.address, G_IM_SIZ_16b, colorImg.width, ...)`,
   separately from `fbKey.colorTargetKey`, and `rt64_framebuffer_manager.cpp:171`
   creates that target from the framebuffer that `everUsedAsDepth`. The mission
   does exactly what the hypothesis says: the new permanent `[GFX-DEPTH] pair`
   census lists the distinct (colour image, depth image) pairs with the draw each
   first appeared on, and scene `0x03` reads `pairs=11` with **one depth image
   (0x3350C0) shared by four colour images**:

   | pair | colour | depth | first draw |
   |---|---|---|---|
   | 8 | 0x3350C0 | 0x3350C0 | 5266 |
   | 9 | 0x025C00 | 0x3350C0 | 5267 |
   | 10 | 0x04B400 | 0x3350C0 | 6751 |
   | 11 | 0x000400 | 0x3350C0 | 8235 |

   **Fix:** the depth renderbuffers now live in
   `std::map<uint64_t, GlDepthTarget> depth_targets_`, keyed by the `G_SETZIMG`
   address (`depth_target_key`: `1<<32 | zimg` for a display list that sent
   `G_SETZIMG`, the colour address otherwise, so a list with no depth image keeps
   the old per-colour behaviour). The flush attaches the matching renderbuffer to
   the colour FBO with `glFramebufferRenderbuffer` before the draw and skips the
   call when it is already attached. The buffer is sized from the colour target
   and grown, never deleted, when several colour targets of different sizes share
   one depth image (RT64 asserts `depth >= colour`). It is cleared to 1.0 once, on
   the first draw that uses it; after that its contents persist across colour-image
   switches, as RT64's do.
2. **The pair census also shows the mission's first pass renders colour into the
   depth image's own address** (pair 8: colour `0x3350C0`, depth `0x3350C0`, one
   draw). RT64 keeps those as two separate targets (`Color` and `Depth` keys), so
   nothing aliases on either side; the colour image at `0x3350C0` is written back
   to RDRAM and the depth values are not.
3. **The discriminating measurement: with the same build and the same intro
   phase, the mission frame is pixel-identical with the depth test on and off.**
   `testdraw`-style flag bit 9 (`512`) inverts `depth_enabled_` at runtime, and a
   `/tmp/w173/drive173ab.cjs` run captured five interleaved pairs 350-400 ms apart
   (same phase, same camera, same portrait animation):

   | pair | whole frame | rows 0..38 | rows 200..238 | rows 40..180 |
   |---|---|---|---|---|
   | 0 | 1.482 | 0.160 | 3.421 | 0.404 |
   | 1..4 | **0.000** | **0.000** | **0.000** | **0.000** |

   Pairs 1..4 are identical to the last bit; pair 0 is the pair whose toggle landed
   inside a flush. The `[GFX-DEPTH]` census over the same run confirms both states
   occurred (`zcmp=zupd` grew to 14 243 over draws 29 200..31 200 and then froze at
   draws 31 600..32 800). So on the gameplay frames this round captured, the depth
   test changes nothing: the game draws back to front, and `LESS` against a cleared
   buffer reproduces the painter's order exactly.
4. **Session 172's "depth flattens the top band" is a phase artifact.** The
   browser's top band measures `9.73` mean abs against native `t.800` with the test
   **on**, and (Result 3) the identical pixels mean the same `9.73` with the test
   **off**. Session 172's `0.93` came from a run whose capture landed on a
   different intro phase: rows 0..38 contain the note box's slide-in, the
   "Castle Burgund" portrait and the MISSION text's scroll, so the band's content
   is a function of capture time. The band's remaining 9.7 is therefore a
   non-depth difference (the band's own content at that moment), not a depth
   ordering failure, and the session-172 item is withdrawn as a depth finding.
5. **One cause of a *false* "no depth effect" is excluded.** A colour-only
   framebuffer makes `GL_DEPTH_TEST` a no-op and would produce exactly the
   identical frames of Result 3. A permanent one-shot diagnostic now states the
   attachment: on the first depth-tested draw the flush logs
   `[GFX-DEPTH] first depth-tested draw: depthRenderbuffer=11 status=0x8CD5
   colour=0xC4E000 depth image=0xC74000` (0x8CD5 = `FRAMEBUFFER_COMPLETE`, and the
   object name is non-zero), so the attachment is real. The synthetic control
   (Result 6) is the second witness.
6. **The synthetic control still passes.** `testdraw.cjs --flags 128
   --expect-depth near` -> `rgba=(255,0,0,0)` (the far triangle is rejected), exit
   0; `--flags 384 --expect-depth far` -> `rgba=(0,0,255,0)`, exit 0, both on the
   cleaned build.
7. **The depth test now ships on by default.** `OGRE_DEPTH=0` turns it off (the
   A/B), and the flag bit 9 runtime switch used for Result 3 was removed with the
   rest of the probe. The rest of session 172's pipeline is unchanged: RT64's
   `G_SETZIMG`/`G_SETPRIMDEPTH` decode, `screen_z = pos.z/w * vscale[2]/1024 +
   vtrans[2]/1024`, `LESS`/`ALWAYS` from `zCmp`, the F3D depth clip, and the
   `[GFX-DEPTH]` census.

## Evidence

- RT64 depth target key: `tools/RT64/src/hle/rt64_state.cpp:1354-1365`
  (`depthFb = framebufferManager.get(depthImg.address, G_IM_SIZ_16b, colorImg.width,
  colorHeight)`, `depthFb->everUsedAsDepth = true`),
  `:1389-1401` (`fbKey.depthTargetKey = RenderTargetKey(depthFb->addressStart,
  depthFb->width, depthFb->siz, Framebuffer::Type::Depth)`),
  `tools/RT64/src/hle/rt64_framebuffer_manager.cpp:171-179` and `:37-53`
  (`FramebufferManager::get` keys the framebuffer by address),
  `tools/RT64/src/render/rt64_render_target_manager.cpp:86-101`
  (`RenderFramebufferStorage::setup` asserts `depth >= colour` and builds
  `colorDepthWrite`/`colorWriteDepthRead`).
- RT64 rect and prim depth are unchanged from session 172
  (`hle/rt64_rdp.cpp` `rectPosFloats` z 0 / w 1, `setPrimDepth`,
  `render/rt64_raster_shader.cpp:317` `LESS`).
- Mission pair census and depth-target creation: `/tmp/w173/on-a2-page.txt`
  (`pair #8..#11`, `depth target #5 for SETZIMG addr=0x3350C0`, `pairs=11`).
- A/B frames: `/tmp/w173/ab-a1-on{0..4}.png`, `ab-a1-off{0..4}.png`,
  `ab-a1-page.txt` (the census `zcmp` progression), comparator
  `/tmp/w173/proof173.py` and the inline diff in this round's log.
- Session-172-style mission comparison on the traced build:
  `/tmp/w173/on-a2-00.png` -> `meanAbs 7.53/6.93/7.29`, `exact 0.3609`,
  top cliff `9.73`, whole frame `7.25`; the paired frame `on-a2-01.png` ->
  `6.84/6.21/6.57`, `0.3626`, cliff `9.73`, whole `6.54`.
- Attachment check: `/tmp/w173/ctl128-pagelog.txt`
  (`[GFX-DEPTH] first depth-tested draw: depthRenderbuffer=11 status=0x8CD5`).

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `GlDepthTarget` + `depth_targets_`/`depth_needs_clear_`/`depth_target_key`/`depth_target_for`; `ensure_framebuffer` no longer owns a depth buffer (colour-only FBO); the flush attaches the draw's depth image and clears it once; `shutdown` deletes the depth renderbuffers; `[GFX-DEPTH] depth target #N` and `pair #N colour= depth= first_draw=` lines and `pairs=` in the census; the one-shot `first depth-tested draw` attachment check; `OGRE_DEPTH` now defaults **on** (`OGRE_DEPTH=0` turns it off) |
| `docs/HANDOFF-2026-10-18-session173.md` | this file |
| `PLAN.md` | 8(u) result |
| `docs/STATUS-LOG.md` | session 173 entry |
| `docs/README.md` | session 173 row |
| `docs/DECISIONS.md` | decision 173 |

No commit was made. The temporary probe `probe173` (an `OGRE_BAND_TRACE` draw
trace with a top-band depth/colour readback, and flag bits 9/10 for a runtime
depth toggle and trace arming) was removed and the build rebuilt;
`grep -rn "probe173\|p173\|OGRE_BAND_TRACE" app/ debug/ RecompiledFuncs/
Bank*Funcs/` is empty. `git status --short` shows `app/src/web_renderer.cpp` and
the docs changed by this round plus the earlier sessions' modified files.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` (five incremental builds;
  the final one is the cleaned build).
- `node debug/probes/testdraw.cjs --flags 128 --expect-depth near` -> exit 0,
  `rgba=(255,0,0,0)`; `--flags 384 --expect-depth far` -> exit 0,
  `rgba=(0,0,255,0)`.
- Mission route (`/tmp/w173/drive173.cjs`,
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`):
  two single-capture runs (depth on: `nonBlack=303208`; the band-traced run the
  same) and one A/B run `/tmp/w173/drive173ab.cjs` (five on/off pairs).
- `docs/proofs/web-mission-depth-on.png` is the depth-on mission frame
  `/tmp/w173/on-a2-00.png`.

### What was not verified

- The tutorial `0x17`, title `0x04` and world map `0x05` frames were **not**
  re-captured this round: the A/B run that would have covered the early mission
  phase re-booted twice under load and was killed at the timebox. So the
  "no regression" claim rests on the mission A/B (identical pixels on the
  gameplay frames) and on the synthetic control, not on those three screens.
- The `OGRE_DEPTH=0` and default-on builds were not compared on a *separate*
  run at the same phase; the A/B toggle inside one run is the stronger test and
  is what Result 3 reports.

## Open

1. **The top band's remaining 9.7 mean abs is not depth.** It is the band's
   content at the captured phase: the note box's slide-in, the portrait and the
   scrolling MISSION text make rows 0..38 phase-dependent. A frame-locked native
   pair (the same intro step in both renderers) is the way to measure that band;
   session 172's open item 8(t) says the same thing.
2. **`ZMODE_DEC` is still not implemented** (unchanged from session 172); the
   mission has `dec=0`.
3. **The depth image is still not written back to or uploaded from RDRAM**
   (unchanged). The game's `G_SETZIMG` words carry no usable size, so the buffer
   is sized from the colour target.
4. **`a_w` is still not in `gl_Position`** (unchanged from session 172).
5. **The tutorial, title and map captures still need a re-measure on an idle
   machine** (also open since session 172).
