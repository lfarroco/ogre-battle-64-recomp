# Session 187 handoff — the browser page is a game shell, not a debug dump

## Goal

Developer request: opening `http://127.0.0.1:8931/app/web/index.html` showed
"lots of text, a console and other stuff"; keep the game area and make the page
a more beautiful, professional interface.

## Result

`app/web/index.html` is now a game shell: a slim top bar (brand, state pill,
Controls, Load save, fullscreen), a metal bezel around the canvas, and one
muted status line under it. The launcher is an overlay card on the canvas with
Select ROM, Load save, and drag-and-drop. It fades out when the boot starts.

Removed from the page: the title paragraph, the build/renderer status
paragraph, the controls paragraph, the "full log is kept in memory" paragraph,
the visible graphics-workload `<pre>`, and the visible `#gfx-status` line. The
`?log` developer console remains, as a fixed drawer with a Close log button.

Preserved so the existing probes and save path keep working:

- `#rom-input`, `#save-input`, `#game-canvas`, `#status`, `#gfxstats` and
  `#gfx-status` keep their ids and their event contracts.
- `#gfxstats` and `#gfx-status` are hidden but are still updated by `web.js`.
  `debug/probes/*.cjs` read their `textContent`.
- `#status` is still the element the `?log` mode mirrors into, and the default
  mode is one line. `debug/probes/logmode.cjs` passes.
- `#game-canvas` keeps the 1 px border that `tools/web_frame_cmp.py` crops and
  keeps no border radius (session 160).
- The Emscripten build still receives the page's query-string `OGRE_*` knobs.

The canvas gained `box-sizing: content-box`. The page sets
`* { box-sizing: border-box; }` for its layout, and with border-box
`emscripten_set_element_css_size()` would make the 1 px border part of the
declared width and height, so a 640x476 drawing buffer would be scaled into a
638x474 content box. Measured with content-box: a 639x475 buffer has a
639x475 `clientWidth`/`clientHeight` and a 641x477 border box; a 640x480
buffer at 2x has a 642x482 border box. Content-box restores the 1:1 buffer-to-
CSS mapping session 148 established.

`web.js` gained a small shell hook. `window.ogreUI` provides `setPhase`,
`setStatus`, `dismissLauncher` and `showLauncher`; `web.js` calls them at boot
start, renderer start, first drawn frame, stall, and error. The status text is
curated ("Ready. Select a ROM to begin.", "Starting the game…", "Running",
"Still loading…"), so the page no longer prints runtime log lines by default.
Autoplay/audio messages continue to go to `window.ogreLog`.

Other shell behaviour:

- Drag and drop on the launcher writes the dropped file through `#rom-input`
  and dispatches `change`, so the existing loader runs.
- `Controls` opens a native `<dialog>` with the keyboard and gamepad maps and
  an advanced note. Keyboard input is not pushed to the game while a dialog is
  open.
- Fullscreen scales the stage by an integer-limited fit and hides the
  understage; the canvas keeps its own drawing-buffer size.
- The layout is checked at 390x844, 768x1024 and 1280x720, with no horizontal
  overflow. Below 620 px the state pill is hidden and the launcher card reduces
  to the title, one line of copy and the two actions.

## Evidence

- `node --check app/web/web.js` clean.
- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean.
- `debug/probes/logmode.cjs --out /tmp/logmode-187`: `PASS`.
  Default page: `statusLines=1`, `logMode=false`; `?log`: `logMode=true`.
- `debug/probes/boot.cjs --attempts 3 --secs 45 --out /tmp/boot-187`:
  `RENDERED {"w":640,"h":481,"nonBlack":307840,"colorful":6947}`.
  The crop leaves the drawing buffer because the canvas is content-box with a
  1 px border.
- Manual boot at 1280x720: no page errors; launcher visibility `hidden` after
  boot; status pill `playing`; `#gfxstats` reached `tasks=519`. At that point
  the canvas was a 639x475 buffer in a 639x475 content box (641x477 with
  border).
- Fullscreen: `document.fullscreenElement` is `#stage`, `--fs-scale` 1.5 at
  1280x720, canvas 960x720, understage hidden.
- Proofs: `docs/proofs/web-ui-launcher-session187.png` (1280x720 launcher) and
  `docs/proofs/web-ui-game-session187.png` (natural boot, prologue story;
  `sha256 f87ec97a…`).

## Files changed

| file | change |
|---|---|
| `app/web/index.html` | game shell, launcher, status pill, controls dialog, fullscreen, log drawer, content-box canvas, responsive rules |
| `app/web/web.js` | `ogreUI` phase/status hooks, launcher dismissal, error/stall messages, dialog keyboard guard |
| `docs/proofs/web-ui-launcher-session187.png` | launcher proof |
| `docs/proofs/web-ui-game-session187.png` | natural-boot proof |
| `docs/HANDOFF-2026-10-23-session187.md` | this file |
| `PLAN.md` | browser status note |
| `docs/DECISIONS.md` | UI-shell decision row |
| `docs/STATUS-LOG.md`, `docs/README.md` | session entry and index row |

## Probes used and reverted

None. No generated code, ROM data, renderer source or submodule was touched.
`tools/RT64` was already modified when the session started.

## Open

- The shell is not pinned by a golden image. A future layout change must
  re-check the content-box mapping and re-run `logmode.cjs` and `boot.cjs`.
- The native and browser renderer items in `PLAN.md` are unchanged.
