# Session 151 handoff — the browser's scripted tap schedule now matches the native one (last slot sticks, `OGRE_TAP_MAX`), and framebuffer-as-texture is the next renderer item

## Goal

Continue the web renderer (developer, session start). Session 150 had just
settled the RDP fill-scissor question and fixed RT64, so this session went back
to the browser renderer's own open items.

## Result

1. **Fixed: the browser's `OGRE_TAP_BUTTON` schedule cycled forever; the native
   one's last slot sticks.** `app/web/web.js` advanced the slot with
   `names[slot % names.length]`, so a route could not stop tapping mid-run — and
   the browser has none of the native's scene-keyed knobs (`OGRE_TAP_SCENE`,
   `OGRE_TAP_NOT_SCENE`, `OGRE_TAP_SCENE_BUTTON`), so there was no other way to
   stop. The native advance is `slot = tap < count ? tap : count - 1`
   (`app/src/sdl_platform.cpp`), i.e. the last slot sticks, plus `OGRE_TAP_MAX`
   to stop after n presses. `web.js` now does both, so
   `OGRE_TAP_BUTTON="start,start,none"` presses Start twice and then stops
   tapping while the run continues.

   Verified: `OGRE_TAP_MS=1000&OGRE_TAP_BUTTON=start,none&OGRE_TAP_MAX=3` logs
   `[web:input] tap schedule done after 3 presses` and the run still renders
   (`debug/probes/palette.cjs`, 25 s, one kept frame).

   The guide's `OGRE_TAP_BUTTON` row said "cycled one per tap", which described
   the browser's old behaviour rather than the native's; it is corrected and the
   browser's `OGRE_TAP_MAX` is now documented there.

2. **The natural title route is racy in the browser, and the documented shortcut
   reaches the cathedral on both sides.** The session-54 route
   (`OGRE_SCENE=title OGRE_TAP_MS=1000 OGRE_TAP_BUTTON="start,a,start,a,…"`) sends
   its first presses before the browser's forced title is up: this session's run
   logged `[scene] t=8189ms id=0x0004`, so taps at 1.5/3/4.5 s were swallowed and
   the run sat on the title. The native's answer is `OGRE_TAP_SCENE_BUTTON`
   ("press Start up to five times *while the title runs*"), which the browser does
   not implement. `OGRE_SCENE=new-game OGRE_STEP=2` does reach the cathedral on
   both sides and waits for input there (native: `id=0x0002` at t=5.3 s, then
   `id=0x000D` at t=5.7 s), so a cathedral comparison does not need taps. Nothing
   was captured or compared this session.

3. **The next renderer item is framebuffer-as-texture, and here is where it
   lives.** The gfx thread decodes a `G_LOADTILE`/`G_LOADBLOCK` out of `rdram_` at
   record time (`decode_texture_rect` at `app/src/web_renderer.cpp:3827`, into
   `TexUpload::pixels`), while a colour image the same display list rendered into
   lives in a GL framebuffer and is only written back to RDRAM on the browser
   main thread at present or at a `sync_framebuffers` request
   (`readback_all_framebuffers`, called from `flush_commands`). A texture whose
   source range lies inside such an image therefore decodes stale bytes. The two
   candidate fixes to evaluate next:
   - a gfx-thread → main-thread handshake that writes the overlapping framebuffer
     back before the load is decoded (the existing `sync_request_`/`sync_done_`
     pair is the model, but the gfx thread must not block while it holds the
     renderer mutex the main thread needs);
   - deferring the decode of a load whose source overlaps a dirty framebuffer to
     the main thread, so the flush writes back first and the decode happens then.

## Files changed

| file | change |
|---|---|
| `app/web/web.js` | the tap schedule's last slot sticks and `OGRE_TAP_MAX` stops it, matching `app/src/sdl_platform.cpp`; the comment records the rule |
| `docs/guides/app-build.md` | the `OGRE_TAP_BUTTON` row no longer says "cycled one per tap", which was the browser's behaviour; it records the last-slot-sticks rule and the browser's `OGRE_TAP_MAX` |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session151.md` | this file |

No probe was added to generated code and nothing under `RecompiledFuncs/`,
`Bank*Funcs/` or `RspFuncs/` was touched. `tools/RT64` carries session 150's fill
clip (uncommitted; `patches/rt64-ob64.patch` regenerated, `make patch-check`
PASSes).

## Open

1. **Framebuffer-as-texture** (above): the last named renderer gap. No screen has
   been shown to need it yet, so the first step is to find one — a display list
   that loads a texture from an address it rendered into earlier in the same task
   (`[GFX-CIMG]` plus a `[GFX-TEXTRACE]` whose `timg` lies inside that target's
   range).
2. **The browser has no scene-keyed tap knobs**, so a route that must stop at a
   scene needs either `OGRE_SCENE`+`OGRE_STEP` forcing or a hand-tuned wall-clock
   schedule. That is input plumbing, not the renderer.
3. Session 148/149/150's items stand: the canvas's rounded corners and unwritten
   last column/row (cosmetic), the LOADTLUT `tmem` key, the `fldRegs` x/y scale
   and offset, the title's moving-cloud bands, and the screens that have not been
   compared against the reference (the cathedral backdrop, the world map's
   sprites, the tutorial practice field).
