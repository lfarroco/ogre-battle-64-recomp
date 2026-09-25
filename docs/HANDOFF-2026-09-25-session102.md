# Handoff — 2026-09-25, session 102: WIDESCREEN (a port feature, not a mod)

**Goal (developer):** from the mod-idea list, implement widescreen during
missions, then fix two follow-ups: the 4:3 run's window opened 16:9 with
pillarbox bars, and the question of whether the large 2D backgrounds can show
more to the sides.

**Result:** a WIDESCREEN row in the shared SETTINGS tab with three modes —
`OFF` (default), `MISSIONS` and `ALWAYS`. `MISSIONS` turns RT64's Expand aspect
ratio on only while the dispatcher's active scene is `0x03` (the mission), so the
mission field is hor+ 16:9 and the 4:3 screens keep their authoring. The **window
shape follows the mode and never changes while the game runs**: `OFF` opens a
4:3 window, `MISSIONS`/`ALWAYS` open a 16:9 window in which the 4:3 scenes are
pillarboxed. The value is saved in `<config>/settings.cfg` as
`widescreen = off|missions|always`, and `OGRE_WIDESCREEN` overrides it for one
run without writing the file.

The large 2D backgrounds cannot show more to the sides: the art is 4:3. See §4.

This is not a mod. The mod API (`mods/skip-boot-logos/`) hooks a named
recompiled guest function and pokes RDRAM; this is app code plus RT64
configuration. It is implemented directly in the app.

---

## 1. What RT64 already provided

RT64 has the widening; the port only had no way to select it.

* `tools/RT64/src/hle/rt64_workload_queue.cpp:134-211` derives
  `aspectRatioScale` from the window and `aspectRatioTarget` from
  `userConfig.aspectRatio` (`Expand` = `max(window ratio, source ratio)`), then
  sets `resolutionScale = {multiplier * aspectRatioScale, multiplier}`.
* `tools/RT64/src/render/rt64_projection_processor.cpp:85-115` decides per
  projection whether to counter-scale: `adjustAspectRatio` is true for
  `G_EX_ASPECT_ADJUST`, or for `G_EX_ASPECT_AUTO` when the projection covers the
  full scissor width and is wider than tall. HLE F3DEX2 lists get
  `aspectMode = G_EX_ASPECT_AUTO` (`rt64_transform_group.h:25`), so the 3D
  mission view is counter-scaled by `1/scale` and looks hor+, while a 2D element
  drawn without such a projection is stretched horizontally.
* The port already forwarded the option: `app/src/renderer.cpp:123`
  (`app->userConfig.aspectRatio = to_rt64(config.ar_option)`) and
  `app/src/renderer.cpp:917` (`update_config` calls `app_->updateUserConfig` and
  discards framebuffers when the ratio changed).
* `ultramodern::renderer::set_graphics_config` (`config.hpp:86`) is the runtime
  entry: it stores the config and flags an `UpdateConfigAction`
  (`renderer_context.cpp:45`), which the renderer thread consumes in
  `events.cpp:444`.

## 2. What landed

| file | change |
|---|---|
| `app/src/widescreen.{hpp,cpp}` (new) | `widescreen_update()`: computes the wanted `ar_option` from `widescreen_mode()` and `active_scene_id()`, reads the live `GraphicsConfig`, and calls `set_graphics_config` only when the value differs. Returns whether the **mode** changed, which is the only thing that re-fits the window. `widescreen_fit_window(window)` sets the width from the current height and the mode's shape (4:3 for `OFF`, 16:9 otherwise); `widescreen_initial_window_size` is what `create_window` creates with. It is the only writer of `ar_option` in the app |
| `app/src/settings.{hpp,cpp}` | `WidescreenMode{Off,Missions,Always}`, `widescreen_mode_label`, `widescreen_mode`, `widescreen_mode_index`, `set_widescreen_mode`, `widescreen_mode_text`; the `widescreen` key in `settings.cfg`; `OGRE_WIDESCREEN` as the one-run override. The loader now matches a `key = value` line by key: the old parser returned the first integer anywhere in the file, which a second key made ambiguous |
| `app/src/ui.{hpp,cpp}` | `RowKind::Widescreen`; the GAME SPEED radio group generalised to `layout_radio` over `RadioToken`/`RadioLayout`, so both rows share one measure/draw/click layout; the SETTINGS tab lists GAME SPEED then WIDESCREEN; `choose_option` and `activate` handle both rows |
| `app/src/launcher.cpp`, `app/src/overlay.cpp` | `LEFT`/`RIGHT` step the WIDESCREEN row (the row-kind checks that gated GAME SPEED now include it) |
| `app/src/main.cpp` | `ogre::widescreen_update()` in `update_gfx`, beside `poll_scene()`; when it reports a mode change, `ogre::widescreen_fit_window(g_platform.window)` |
| `app/src/sdl_platform.cpp` | `create_window` takes its size from `widescreen_initial_window_size` and calls `widescreen_fit_window` once afterwards, because the window manager hands back 715 for a requested 720 |
| `app/CMakeLists.txt` | `src/widescreen.cpp` in the native source list (the web build has no UI and never calls it) |

The mode, not a ratio, is the unit of the setting because the window and the
scene gate are both mode properties. `widescreen.cpp` writes `ar_option` only on
a change, so a steady scene costs one `GraphicsConfig` comparison per frame and
the renderer sees one `update_config` per transition.

### The window-shape rule, and the version that was rejected

The first version sized the window from the *effective aspect* at every change,
so a `missions` run grew the window when a mission started and shrank it when a
story scene did. The developer rejected it: *"the window changes its size while
the game is running. if using any form of widescreen, the 4:3 should have black
bars on the sides on the non-widescreen scenes"*. The shipped rule:

| mode | window | a 4:3 scene | the mission |
|---|---|---|---|
| `OFF` | 4:3 | fills the window, no bars | 4:3, no bars |
| `MISSIONS` | 16:9 | pillarboxed, black bars on the sides | fills the width |
| `ALWAYS` | 16:9 | stretched to the width | stretched to the width |

A scene transition resizes nothing. Changing the mode from the launcher or the
`ESC` overlay re-fits the window once. A manual resize or a double-click to fill
the screen is left alone (the developer double-clicked during a run and confirmed
it behaved).

## 3. Verification

Build: `make app`. On this machine that writes `build-dist/ogrebattle64`, because
`Makefile:431` is `DIST_STATIC_SDL ?= 1`. The first A/B ran the stale
`build-app/ogrebattle64` (Sep 17 configure, no widescreen) and showed no
`[widescreen]` line; that is the session-99 trap in the decisions log, and the fix
is the one it records. All runs below use `build-dist/ogrebattle64`.

Run for the mission: `OGRE_PREF_DIR=/tmp/ws-<mode>
OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1 OGRE_WIDESCREEN=<mode>
OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_TAP_MS=1200
OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:4,0x03:a:6"
OGRE_CAPTURE_PRESENT=/tmp/ws-<mode> OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=100
OGRE_EXIT_AFTER_MS=40000` (the session-98 suspend route to scene `0x03`).

| check | result |
|---|---|
| settings.cfg round trip | `Down,Right` in the launcher writes `widescreen = missions`; the next boot logs `game speed x1, widescreen missions` |
| in-game overlay | `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="Down,Right"` wrote `widescreen = missions` in a running game and re-fit the window once (`953x715 -> 1271x715`) |
| the scene gate | `missions` logs `aspect ratio expand (scene 0x0003, mode MISSIONS)` on every `0x03` visit and `original` on each `0x02`/`0x0D` between them (three visits, exit 0). `off` logs no transition |
| no scene resizes the window | after the boot fit, the only `window` line in a 40 s `missions` run is the initial one; the captures stay 1271x715 across the mission and the story scenes |
| the 4:3 window | `off` opens 953x715 and presents 953x715; the extreme columns are non-black (brightness 37/39 at the title), so there is no pillarbox |
| the 16:9 window | `missions`/`always` open 1271x715 and present 1271x715 |
| the picture | the Load Game book in `missions` has black side bars and in `off` fills the window: `native-widescreen-pillarbox.png`, `native-widescreen-43-window.png`. A battle in the mission fills the width in `missions` and is 4:3 in `off`: `native-widescreen-mission-expand.png`, `-mission-43.png` |
| the row | `native-widescreen-launcher.png` |

One thing the captures show and must not be misread: the `MISSION` banner row
clips at both screen edges **in 4:3 too**. It is the game's own scrolling
marquee.

## 4. The large 2D backgrounds cannot show more to the sides

The developer asked whether screens with a large background (the New Game
cathedral) can "render more to the sides" in widescreen. The answer is no, and
the evidence is:

* **The art is 4:3.** Every njpeg background's dimensions are in its own header;
  the inventory (`docs/guides/njpeg-backgrounds.md`) is 75 assets, 42 of them
  320x240. The widest asset in the whole ROM is **336x256** (2 assets), then
  336x176 (1): at most 5% wider than the screen. There is no 16:9 artwork to
  reveal.
* **Nothing is off-screen in the final blit.** The backdrop is assembled in
  `0x80243E28` and drawn as twelve horizontal strips of 20 texels, each loading
  the source's full width: `OGRE_DL_DECODE=all` at the cathedral step shows
  `LOADTILE t7 uls=0 ult=N lrs=1276 lrt=N+20` (1276 quarter-pixels = 319 px) with
  `SETTILESIZE t0 0,N..1276,N+20` and a `G_TRI2` per strip. Every column of the
  320-px source is loaded and drawn into the full-screen scissor
  (`SETSCISSOR 0,0..1276,956`).
* Therefore a widescreen presentation of these screens is one of: a horizontal
  stretch (`ALWAYS` today, and what RT64's framebuffer scaling does), a uniform
  zoom that fills the width and crops the top and bottom, an edge extension that
  replicates the outer columns into the bars, or the pillarbox the shipped
  `MISSIONS`/`ALWAYS` mode gives. The first is distorted; the others need either
  a renderer change or new art. None of them "shows more".

What *does* gain width is 3D: the mission's terrain and battles, because the
projection is counter-scaled and the extra columns are genuinely rendered.

## 5. Files changed and proofs

`app/src/widescreen.hpp`, `app/src/widescreen.cpp` (new),
`app/src/settings.hpp`, `app/src/settings.cpp`, `app/src/ui.hpp`,
`app/src/ui.cpp`, `app/src/launcher.cpp`, `app/src/overlay.cpp`,
`app/src/main.cpp`, `app/src/sdl_platform.cpp`, `app/CMakeLists.txt`,
`docs/guides/app-build.md` (the `OGRE_WIDESCREEN` knob and the
"Widescreen (the SETTINGS tab)" section), `PLAN.md`, `docs/DECISIONS.md`,
`docs/README.md`, `docs/STATUS-LOG.md`.
Proofs: `docs/proofs/native-widescreen-launcher.png`,
`native-widescreen-pillarbox.png`, `native-widescreen-43-window.png`,
`native-widescreen-mission-expand.png`, `native-widescreen-mission-43.png`
(converted from the PPMs with `sips -s format png`).

No probes were used. Nothing under `RecompiledFuncs/`, `Bank*Funcs/`, `app/` or
`tools/` was instrumented, so there is nothing to revert.

## 6. Open

* `ALWAYS` is shipped and stretches the 2D screens. If that is not wanted, drop
  the mode from the radio row (`kWidescreenModeCount`, the labels and
  `parse_widescreen`).
* The tutorial's practice stage is scene `0x17`, so `MISSIONS` leaves it 4:3.
  Adding `0x17` to `wants_expand` is one line if the developer wants it.
* The synthetic `OGRE_OVERLAY_KEYS` test harness delivered one extra key in one
  of three runs (the mode reached `always` instead of `missions`). It is the
  harness's event injection, not the panel: two further runs stepped exactly
  once.
* Widescreen was not run with audio, on Windows, or on Linux.
