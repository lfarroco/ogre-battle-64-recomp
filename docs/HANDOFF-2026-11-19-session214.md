# Session 214 handoff — issue #27, the cutscene's widescreen side bands

## Goal

Issue #27: "Certain scenes darken the original size when in wide screen mode."
The developer's follow-up locates it in the cutscenes and supplies a screenshot:
the first cutscene after New Game, with WIDESCREEN on, shows bands of unrelated
content on the left and right of the 4:3 picture. The developer's verdict is
that black is the correct content for those bands.

## Result

RT64's Expand aspect ratio is now used for the mission scene `0x03` only. Scene
`0x0D` (the dialogue/cutscene engine, which plays the New Game opening) is
presented at its own 320x240 and pillarboxed in the wide window, so the bands
are black. The mission still expands, and the browser build answers the same
question from the same list.

Session 114 added scene `0x0D` to the Expand set and wrote that the wide view
"reveals the sides of the game's own art". That reading is withdrawn: the
columns it saw are produced by the renderer, not authored by the game.

## Evidence

Reproduction, WS on, `OGRE_SPEED=1 OGRE_SCENE=title OGRE_TAP_MS=1500
OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_TAP_BUTTON=start`, target captures every
second (`OGRE_CAPTURE_TARGET`):

- The frame is a 428x240 target (`[capture] wrote ... (428x240 format=11)`),
  and its picture occupies columns 53..373, i.e. the game's 320 columns centred
  with a hard seam at each edge. Columns 0..52 and 374..427 hold the bands.
- The picture is 1:1, not stretched: the expanded frame's centre against a 4:3
  capture of the same shot (`OGRE_WIDESCREEN=off`, same route) measures a best
  horizontal scale of 1.01.
- The bands follow the frame's fade (left/centre/right means 0.03/0.04/0.05
  during the fade-in, 0.13/0.15/0.20 once it settles), so they are drawn each
  frame rather than left over.
- `[fbrender] addFramebuffer ... targetW=428 targetH=240` and a wide-pass fill
  at `rect=(0,0)-(427,239)` place the frame in the wide target. The cutscene's
  projections come out as `type=1 invRatio=1.000 screenScaleX=1.000
  viewport=(-214,-120 856x480)`: the scissor and viewport cover the frame, so
  `useWideViewport` in `rt64_framebuffer_renderer.cpp` maps the draw into the
  full wide viewport and stretches it horizontally.
- Forcing every projection to the centred mapping (`useWideViewport = false`,
  probe27) makes the two bands exactly black (region means `L 0.0 C 36.6
  R 0.0`). The bands are therefore the renderer's wide mapping of the scene's
  2D draws.

The game's cutscene frames are 2D across the whole 4:3 frame. RT64 has no
per-draw signal that says "this 2D draw is 4:3-authored" unless the game emits
the extended GBI commands (`G_EX_ASPECT_ADJUST`, `G_EX_ORIGIN_LEFT/RIGHT`), and
OB64 emits none. There is no renderer-side fix in this port that widens the
mission faithfully and keeps a 2D cutscene at 4:3, so the scene set is the
place to decide.

## What changed

- `app/src/settings.{hpp,cpp}`: `widescreen_expand_scenes()` and
  `widescreen_scene_expands(scene)` now own the expand set, `{0x03}` by
  default, with `OGRE_WS_SCENES=<hex>[,<hex>...]` as the developer override.
  The list lives in `settings.cpp` because the browser build compiles that file
  and not `widescreen.cpp`, and the two copies it used to carry must not drift.
- `app/src/widescreen.cpp`: `wants_expand()` calls the shared function; the
  local list, the `hd_backgrounds_covers_scene` rule and the now-unused
  includes are gone.
- `app/src/web_settings.cpp`: `web_widescreen_expand()` calls the shared
  function; its duplicate list is gone.
- `app/src/hd_backgrounds.{hpp,cpp}`: `hd_backgrounds_covers_scene` is removed
  (its two callers are gone). The `-wide` image is chosen only when the matched
  scene expands, so the pack now applies the plain image. The `settings.hpp`
  include is unconditional: it used to be native-only, and the web build failed
  on the new call with `no member named 'widescreen_mode'` until it was moved
  out of the guard.
- Docs: `PLAN.md` (the WIDESCREEN row and the HD-backgrounds bullet),
  `docs/DECISIONS.md` (the 2026-11-19 entry), `docs/scenes.md` (the New Game
  section), `docs/guides/hd-backgrounds.md`, `docs/STATUS-LOG.md`,
  `docs/README.md`.

## Verification

- Cutscene, widescreen on, after the change: the capture target is 320x240
  (`[capture] wrote ... (320x240 format=11)`), no `[widescreen]` aspect line
  fires for scene `0x000D`, and the presented window
  (`OGRE_CAPTURE_PRESENT`, 1280x720) shows the picture centred with black side
  bars: `docs/proofs/native-widescreen-cutscene-pillarbox.png`. Before the
  change the same frame's render target held the bands:
  `docs/proofs/native-widescreen-cutscene-expanded-428x240.png`.
- Mission, widescreen on, `OGRE_SAVE=assets/escort_suspend.bin OGRE_TAP_MS=1500
  OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:4,0x03:a:6"`: scene `0x0003`
  logs `[widescreen] aspect ratio expand (scene 0x0003, mode ON)`, its targets
  are 428x240, and the field fills the width with no bands.
- HD pack, widescreen on, `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2
  OGRE_BG_LOG=1`: `[hdbg] applied 'cathedral' (01)` for all four chunks, with
  no `wide` suffix.
- `make app` and `make web` both build clean (the web build for rule 15: this
  changes `settings.cpp`, `web_settings.cpp` and `hd_backgrounds.cpp`, all in
  the Emscripten source list).

## Probes and their revert

`tools/RT64/src/render/rt64_framebuffer_renderer.cpp` carried a `probe27`
instrumentation: a time-gated `[fbrender]` trace
(`OGRE_FBRENDER_TRACE_AFTER_MS`), the per-call converted rectangles, the
per-projection mapping (`invRatio`, `screenScaleX`, viewport), and an
`OGRE_PROBE27_CENTRE` switch that forced `useWideViewport = false`. The file was
restored with `git -C tools/RT64 checkout -- src/render/rt64_framebuffer_renderer.cpp`
and `grep -rn probe27 tools/RT64/src app/src` is empty. Nothing else in RT64 was
touched.

The gated trace is worth rebuilding if another scene needs this question
answered: the counters in `OGRE_FBRENDER_TRACE` are static and the first
fourteen lines are spent on the boot, so the scene under test never appears
without the time gate.

## Cathedral review (same session, developer request)

The cathedral dialogue (scene `0x0D` step 2) is the scene session 114 used to
justify expanding `0x0D`. It measures the same as the cutscene:

- In the old expanded frame the picture is native scale and centred: the best fit
  of the 4:3 framebuffer (from a live RDRAM dump of the same step) into the
  expanded target is scale 1.02 at column 52. The stretch model (`s=1.336`) fits
  at 9.83 against 2.65 for the centred one.
- Re-running the step with Expand re-enabled (`OGRE_WS_SCENES=0xD`) and the
  centred mapping forced (`probe27`) turns the side columns black (means `0.0`
  and `2.1`) and leaves the picture at columns 52..373. The columns the old
  Expand showed are therefore the renderer's wide mapping of the frame's 2D
  draws here too, not the game's 496x384 backdrop canvas. That canvas
  (`OGRE_BG_DUMP`) is a texture the game only ever displays 320x240 of.
- The scene now looks like `docs/proofs/native-widescreen-cathedral-pillarbox.png`
  (4:3, black side bars); the old look was
  `docs/proofs/native-widescreen-cathedral-expanded.png`, whose left edge carries
  a misaligned strip and whose right edge ends in a red/black wedge.
- With the HD pack installed the plain image is applied and the pack's own
  placeholder is what the backdrop shows, also pillarboxed.

The probe was added and reverted again for this check: `git -C tools/RT64
checkout -- src/render/rt64_framebuffer_renderer.cpp`, `grep -rn probe27` empty,
`make app` rebuilt.

## Open

- A test that reaches the cutscene through a checkpoint would make this one
  minute instead of one minute plus a 28.7 s movie; the route here is the
  title and a Start tap every 1500 ms.
- The HD pack's `-wide.png` variant has no scene to apply to any more. It
  stays implemented so a pack for an expanding scene (or an
  `OGRE_WS_SCENES` run) can use it, but its placeholder pair is currently
  unused.
