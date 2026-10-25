# Session 191 handoff — the browser has the native Esc overlay's options

## Goal

Continue the browser port. The developer asked for the same overlay options the
native build has in the in-game Esc panel, and allowed either a web UI with the
same options or a port of the overlay itself.

## Result

The browser page now has an Esc overlay with the native panel's tabs and rows.
The native overlay is a separate SDL window because RT64 owns the game's swap
chain (`app/src/overlay.cpp`); the browser has no second window, so the page owns
the panel as a fixed overlay inside `#stage`. The native panel's values are
shared where the runtime owns them and page-side where the browser input path is
JavaScript.

- **MAIN**: Start Game (`ALREADY RUNNING`), the loaded ROM name, and Exit Game.
  Exit Game syncs IDBFS and reloads with `?menu`; the stored ROM is not
  auto-started in that mode, so the launcher returns. The ROM row is
  informational (the native overlay's ROM row is read-only while the game runs).
- **MODS**: shown disabled, as it is in the native overlay where the running game
  cannot load a mod. The browser has no mod files to toggle.
- **CONTROLS**: the same 14 N64 rows, keyboard and gamepad columns, field
  descriptions, Reset bindings, and the L-stick/R-stick/menu-navigation notes.
  Click a row and press a key or a pad button; Backspace clears the key, Delete
  clears the pad, Esc cancels. Bindings persist in `localStorage`.
- **SETTINGS**: GAME SPEED 1/2/4/8, WIDESCREEN Off/On, WINDOW
  Windowed/Fullscreen, SOUNDS On/Off, VOLUME 0..100, MOUSE ENABLED Off/On. The
  experimental RESOLUTION and MSAA rows appear only when the native gate is on
  and are disabled with "NATIVE RENDERER ONLY", because the WebGL2 renderer has
  no resolution scale or multisample target.
- **DEBUG**: Chaos Frame with its LOW/NEUTRAL/HIGH band and the active scene id.
- **Input**: Esc or the topbar Options button opens it; Esc resumes. Keyboard
  and pad drive the panel (Up/Down, Left/Right, Enter/Space, Tab, pad D-pad/A/B,
  LB/RB switch tab, Back opens it). The game sees an idle controller while the
  panel is up; the game itself keeps running, as it does natively.

### The runtime bridge

`app/src/settings.cpp` is now compiled into the wasm target. `main_web.cpp` calls
`ogre::web_settings_load()`, which is the native `load_settings("/ogre")` call
before `recomp::start`, so `settings.cfg` on the IDBFS mount gives the same
GAME SPEED, WIDESCREEN, MOUSE ENABLED, display, sound and volume defaults and the
same `OGRE_*` overrides. `app/src/web_settings.cpp` exposes:

- `_ogre_web_settings_game_speed` / `_set_game_speed`
- `_ogre_web_settings_widescreen` / `_set_widescreen`
- `_ogre_web_settings_mouse` / `_set_mouse`
- `_ogre_web_settings_display_experiments`
- `_ogre_web_chaos_frame`
- `_ogre_web_active_scene`
- `_ogre_web_widescreen_expand`
- `_ogre_web_mouse_move` / `_click` / `_leave`

GAME SPEED calls the runtime's `set_speed_multiplier`; MOUSE ENABLED publishes to
`map_cursor.cpp` and `mission_cursor.cpp`; the two experimental rows read the
native gate. The page's pointer events on `#game-canvas` forward to the two
cursor modules, and `web_platform.cpp`'s `get_input` now ORs the map/mission
click and edge direction into controller 0 exactly as `sdl_platform.cpp` does.

SOUNDS and VOLUME are the page's AudioWorklet because the browser's audio path
is JavaScript. The worklet gained a gain message; the native `audio_gain_percent`
is not used. This is the one settings group that is not the native `settings.cfg`
value.

### Widescreen

WIDESCREEN is the native value. On an expanding scene (`0x03`, `0x0D`, an
HD-background-covered scene, or `OGRE_WS_SCENES`) the renderer counter-scales the
projection's x row by `3/4`, the RT64 `ProjectionProcessor` ratio for a 16:9
window against the 4:3 target; the page puts a 16:9 CSS box on the canvas. On
other scenes the canvas keeps its 4:3 box. The CSS class was tested with a forced
export; the 3D wide frame was not captured (see Open).

## Evidence

### Final-build page and bridge

`docs/proofs/web-options-settings-session191.png` and
`-web-options-controls-session191.png` are the final build at 1280x720. The
settings proof sets GAME SPEED 4, WIDESCREEN On and MOUSE ENABLED On through the
real exports before opening the panel, so the rows read `[x] 4`, `[x] ON` and
`[x] ON`.

`node /tmp/w191/bridge.cjs` on the final build lists all 13 exports present and
reads `before {speed:1, wide:0, mouse:0, chaos:-1, scene:65535, expand:0}` and
`after {speed:8, wide:1, mouse:1}`. The `chaos=-1`/`scene=65535` values are
correct before a game boots (no RDRAM image), not failures.

`node /tmp/w191/wide.cjs` forces the two exports and measures the canvas CSS box:
class on `855.33 x 482` (16:9), class off `642 x 482` (4:3), no page errors.

### Page interaction

The page's navigation, binding capture, audio slider, fullscreen and Exit Game
routes were exercised with the current page files and the session-140 `dist-web`
wasm (`http://127.0.0.1:8932`), because every attempt of the current wasm landed
on the known idle trajectory (see Open).

- `node /tmp/w191/ui.cjs` on that page:
  `open` true, MAIN rows, SETTINGS rows, CONTROLS capture
  `APRESS A KEY OR PAD BUTTON...`, binding A `KeyX` -> `KeyG` -> reset `KeyX`,
  DEBUG `GAME NOT RUNNING`, Esc closes. No page errors.
- `node /tmp/w191/menu.cjs`: SOUNDS On and VOLUME cell 3 read
  `{enabled:true, volume:30}`, the row reads `VOLUME---|-------30%`, and Exit Game
  navigates to `...?menu` with the launcher visible and
  `status: "Ready. Select a ROM to begin."` (`Stored ROM found` is absent).
- The first version used a modal `<dialog>`. Entering fullscreen from the WINDOW
  row hid the panel: a modal dialog is in the browser's top layer, and the
  fullscreen element `#stage` does not include it. The panel is now a normal
  fixed overlay inside `#stage`; `/tmp/w191/full.cjs` after the change reads
  `fullscreen=true fsId=stage`, panel `display=flex`, rect `0,0,1280,720`, and
  the screenshot shows the panel over the fullscreen stage.

### Boot

`EM_CACHE=/Users/momo/dev/ogre/.emcache make web` is clean. The final wasm is
`21196311` bytes. The final build was not driven into a game scene: every
attempt (and a rebuilt unmodified HEAD with the old page, as a control) logged
`start_game`, initialized the renderer, and then submitted no display list
(`tasks=0`, the known idle trajectory) under host load averages 11-13. The
session-140 wasm boots on the same page in the same browser, so the page's input
and overlay paths are exercised, but the final build's game frame is not
captured. This is not a regression proved against session 190; it is the current
environment's idle trajectory, and a run on an idle host is the next check.

## Files changed

| file | change |
|---|---|
| `app/src/web_settings.cpp`, `app/src/web_settings.hpp` | new bridge: settings load, getters/setters, Chaos Frame, scene id, widescreen expansion, pointer forwarding |
| `app/src/main_web.cpp` | load the shared `settings.cfg` before `recomp::start` |
| `app/src/web_platform.cpp` | map/mission click and edge bits in `get_input` |
| `app/src/web_renderer.cpp` | widescreen projection x-scale per display list; synthetic control resets it |
| `app/CMakeLists.txt` | compile `web_settings.cpp` and `settings.cpp` into the wasm target; export the new functions |
| `app/web/index.html` | options overlay, tabs/rows CSS, 16:9 canvas class, Options button, fullscreen placement |
| `app/web/web.js` | overlay model/navigation/capture, bindings in localStorage, settings bridge calls, pointer forwarding, `?menu`, audio gain, widescreen class |
| `app/web/audio-worklet.js` | VOLUME gain message and per-sample gain |
| `docs/proofs/web-options-settings-session191.png`, `-web-options-controls-session191.png` | final-build proofs |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and decision rows |
| `docs/guides/app-build.md` | browser overlay and settings note |
| `docs/HANDOFF-2026-10-25-session191.md` | this file |

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean; final wasm `21196311`
  bytes.
- `node --check app/web/web.js`, `node --check app/web/audio-worklet.js`.
- `/tmp/w191/bridge.cjs`: all 13 exports present, values change and restore.
- `/tmp/w191/finalui.cjs`: final-build SETTINGS rows read GAME SPEED 4,
  WIDESCREEN ON, MOUSE ENABLED ON; no page errors.
- `/tmp/w191/wide.cjs`: 16:9/4:3 canvas CSS class, no page errors.
- `/tmp/w191/ui.cjs`, `/tmp/w191/menu.cjs`, `/tmp/w191/full.cjs`: page
  interaction on the session-140 wasm, no page errors.
- `debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`:
  `fbtex hits=1 lines=1`, exit 0; `--flags 512 --expect-alpha`:
  `threshold/full=full threshold/none=none dither/partial=partial`, exit 0.
- `debug/probes/logmode.cjs`: `PASS` (`?log` off/on, no page errors).
- `grep -rn "probe191" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is empty. No
  generated code or ROM data changed.

## Probes used and reverted

None in generated code. Two bring-up environment switches
(`OGRE_WEB_NO_SETTINGS`, `OGRE_WEB_NO_CURSOR_INPUT`) were added while isolating
the boot stall, and removed before the final build; `grep` for both is empty.
`tools/RT64` was already dirty from an earlier session and is unchanged by this
work.

## Open

- The final build's game frame is not captured. Every attempt landed on the
  known idle trajectory under host load 11-13, and a rebuilt unmodified HEAD did
  the same; the session-140 wasm boots. Re-run `boot.cjs` on an idle host.
- The 3D widescreen projection has no frame capture. Its scene gate mirrors
  `widescreen.cpp`, its CSS box is measured, and `web_widescreen_expand()` is
  exported, but the mission/dialogue frame is unrun.
- MODS is disabled in the browser, and RESOLUTION/MSAA report "NATIVE RENDERER
  ONLY" when the native gate is on.
- SOUNDS/VOLUME are page-side (AudioWorklet gain), not the native settings.cfg
  values; the browser's audio still screeches until the RSP audio microcode is
  emulated.
- GAMEPAD navigation and mounting were exercised only through the code paths and
  the synthetic controls; no physical pad was attached in this session.
