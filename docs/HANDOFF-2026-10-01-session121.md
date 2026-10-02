# Handoff — 2026-10-01, session 121: the SETTINGS tab's DISPLAY rows, and a pad that drives both panels

## Goal

Developer request, after a survey of the tree and the open GitHub issues: *"look
for next steps. I don't know if there's any obvious improvements to do, or if we
can start looking at implementing mods and settings"*, then, from the offered
directions, **"Graphics + pad settings rows"**.

Two items, as agreed:

1. Expose the rest of RT64's graphics configuration as SETTINGS rows.
2. Let a gamepad navigate the launcher and the overlay, and open the overlay.

Follow-up within the session: the developer asked for RESOLUTION and MSAA to be
kept behind feature flags for now, and reported visual artifacts from a run with
both on.

## Result

**Landed and verified on `build-app/ogrebattle64` (macOS, Metal) and
`build-null/ogrebattle64`.** The SETTINGS tab now carries WINDOW beside
WIDESCREEN; RESOLUTION and MSAA are implemented on the same path but ship behind
`OGRE_DISPLAY_EXPERIMENTS=1`, because the developer ran the game with them and
saw visual artifacts that a later session will handle. The values persist in
`settings.cfg`, apply live from the `ESC` overlay, and print one
`[renderer] config:` line per change. A pad drives both panels and `Back` opens
and closes the overlay.

## Why this was the cheap half of the work

The plumbing already existed and was unused. Evidence, read at source level:

- `ultramodern::renderer::GraphicsConfig`
  (`tools/N64ModernRuntime/ultramodern/include/ultramodern/config.hpp:12-55`)
  holds `res_option`, `wm_option`, `hr_option`, `api_option`, `ar_option`,
  `msaa_option`, `rr_option`, `hpfb_option`, `rr_manual_value` and `ds_option`.
- The port sets only two of them: `ar_option` (`app/src/widescreen.cpp:171-173`)
  and `api_option` (`app/src/renderer.cpp:824-845`, from `OGRE_GRAPHICS_API`).
  The rest sit at the all-zero default of `static GraphicsConfig
  graphic_config{}` (`tools/N64ModernRuntime/ultramodern/src/renderer_context.cpp:42`),
  that is `Resolution::Original`, `WindowMode::Windowed`, `Antialiasing::None`.
- `RT64Renderer::update_config` (`app/src/renderer.cpp:917-940`) already applied
  all three on a runtime change: `setFullScreen` for `wm_option`,
  `set_application_user_config` for the rest, and `updateMultisampling()` when
  `msaa_option` changed. No renderer work was needed.
- The panel already had the row machinery: a radio row is a `RowKind`, a
  `layout_*` helper, a `*_text()` function and one branch in `activate`.

So the change is settings persistence, one shared radio-row helper, and the pad
path.

## The DISPLAY rows

`app/src/settings.{hpp,cpp}`:

| row | enumerators (display order) | `GraphicsConfig` field | shipped? |
|---|---|---|---|
| RESOLUTION | `Native`, `Double`, `Auto` | `res_option` = `Original`, `Original2x`, `Auto` | no, behind `OGRE_DISPLAY_EXPERIMENTS` |
| MSAA | `Off`, `Msaa2X`, `Msaa4X`, `Msaa8X` | `msaa_option` | no, behind the same flag |
| WINDOW | `Windowed`, `Fullscreen` | `wm_option` | yes |

`settings.cfg` gains `resolution`, `msaa` and `window`; `OGRE_RESOLUTION`,
`OGRE_MSAA` and `OGRE_WINDOW` override the file for one run and do not write it,
the rule `OGRE_WIDESCREEN` already used. A missing or unparsable value is the
default, which is the value every earlier build drew, so an existing
`settings.cfg` behaves exactly as before.

### RESOLUTION and MSAA are gated

The developer ran the game with both on and saw visual artifacts, and asked for
them behind a feature flag until a later session handles it. So:

- `display_experiments_enabled()` is true when `OGRE_DISPLAY_EXPERIMENTS` is
  `1`/`on`/`true`/`yes`, **or** when `OGRE_RESOLUTION`/`OGRE_MSAA` is set, so the
  debug spellings work on their own. It is read once per process.
- With the flag off, `applied_resolution()`/`applied_antialias()` return
  `Native`/`Off` and `publish_graphics` applies those, whatever the file or the
  environment says. `resolution_mode()`/`antialias_mode()` report the applied
  values.
- The two rows are pushed into the SETTINGS tab only when the gate is on
  (`Panel::build_rows`), so a player cannot reach them.
- A configured value is **not** dropped: `write_settings` still writes the
  configured `resolution`/`msaa`, so re-enabling the flag restores it. The file
  records what was configured; the boot log says what was applied, and why:
  `resolution 2x and msaa 4x are experimental and not applied; set
  OGRE_DISPLAY_EXPERIMENTS=1 to enable them`.
- WINDOW is not gated: the artifacts were reported for resolution and MSAA only.

The artifacts themselves are not characterised. They are `PLAN.md` open work 14:
the first step is a captured A/B of a named screen at native vs `2x` and at MSAA
off vs `4x`.

**One config, two writers.** `settings.cpp`'s `publish_graphics()` and
`widescreen.cpp`'s `widescreen_update()` both start from
`ultramodern::renderer::get_graphics_config()` and change only their own fields.
`GraphicsConfig`'s defaulted `operator<=>` makes an unchanged publish a no-op, so
the setter does not enqueue a config action for a value that is already live.

`app/src/renderer.cpp` now prints the applied configuration from
`set_application_user_config`, which is the one place every path goes through:

```
[renderer] config: resolution 2x (mult 2.00), msaa 4x, window fullscreen
```

The names follow `ultramodern::renderer::Resolution` (`Original`/`Original2x`/
`Auto`), not RT64's own enum order; a first attempt used RT64's order and printed
`windowinteger` for `2x`, which the run log caught.

**Not exposed.** `GRAPHICS API` is read when the renderer context is created, so
it stays `OGRE_GRAPHICS_API`. `REFRESH RATE` and `HIGH-PRECISION FRAMEBUFFER`
remain at their defaults.

## The pad

`app/src/ui.{hpp,cpp}`:

- `Panel::PadCommand` (`Up`, `Down`, `Left`, `Right`, `Accept`, `Back`,
  `TabPrev`, `TabNext`) and `Panel::pad_command_for_button(int)`:
  D-pad -> the four directions, `A`/`START` -> `Accept`, `B` -> `Back`,
  `LB`/`RB` -> `TabPrev`/`TabNext`.
- `Panel::pad_command(PadCommand)` applies one and returns the `RowAction` the
  caller must run (`Play`, `QuitGame`, `BrowseRom`, …).
- `SDL_CONTROLLER_BUTTON_BACK` is deliberately not in the table. It is the
  overlay's open/close button, and only while the live map binds nothing to it
  (`pad_button_is_bound`, `app/src/input_map.{hpp,cpp}`), so opening the menu can
  never also press an N64 button in the game.

`app/src/launcher.cpp` and `app/src/overlay.cpp` handle
`SDL_CONTROLLERBUTTONDOWN` through that table. A rebind capture keeps the pad to
itself while it is armed. `app/src/overlay.cpp` also closes on `Back` while
visible, and the game already receives an idle pad while the overlay is open
(`get_input`, `app/src/sdl_platform.cpp:1875-1886`), so panel navigation does not
move the game's own cursor.

**Refactor that fell out.** The list of rows `LEFT`/`RIGHT` steps was written out
three times (the launcher's `step_option`, the overlay's key handler, and the pad
path), and the radio layout was a five-way switch in three places. Both are now
one place each: `Panel::row_is_steppable` and the `is_radio_row`,
`radio_row_current`, `radio_row_count`, `radio_row_layout`, `store_radio_row`,
`step_radio_row` helpers in the anonymous namespace of `app/src/ui.cpp`.

The CONTROLS tab gained one section line documenting the panel's own controls
(developer wording, after a first pass shipped two shorter `PAD:` lines):

```
MENU PAD NAVIGATION: D-PAD MOVE, A SELECT, SELECT MENU, LB/RB SWITCH TAB
```

That line is 63 characters and the left column is two fifths of a 900 px panel,
so it does not fit there. A section row has no description, so `layout_rows` now
wraps a section title to the whole panel width (`full_width`) and `draw_panel`
draws the wrapped lines; a one-line section keeps the height it had before. This
is the only layout change, and it shortened the CONTROLS tab by one row, which is
the tab that fixes every other tab's panel height (`kLayoutReferenceTab`).

### Scripted runs need a pad on this machine

This machine has no gamepad (`docs/HANDOFF-2026-09-20-session92.md` §9 recorded
the same for rebinding), so the pad path is driven by two new hooks that push
real `SDL_CONTROLLERBUTTONDOWN` events: `OGRE_LAUNCHER_PAD=<tag>,…` (one event per
150 ms) and `OGRE_OVERLAY_PAD=<tag>,…` (the same, delivered whether or not the
overlay is visible, so a script's first `back` opens it). Tags are the
`controls.cfg` gamepad tags from `input_map.cpp`'s table (`a`, `b`, `back`,
`dup`, `dleft`, `lb`, …), parsed by `pad_button_from_name`.

## Fullscreen: the game drew small in the top-left corner

Developer report: *"the game shows up small, in the upper left corner. all the
rest is black"*.

Cause, measured with a temporary probe (`probe122`: the SDL window size and pixel
size once a second, plus RT64's swap-chain size; both reverted):

- RT64's fullscreen is a raw `SetWindowPos` to the monitor rect on Windows
  (`rt64_application_window.cpp:180-229`, the `_WIN32` arm) and
  `[NSWindow toggleFullScreen:]` on macOS (`:230-231` via plume's
  `CocoaWindow::toggleFullscreen`). Neither goes through SDL, so SDL's cached
  window size does not follow the transition.
- `widescreen_update` reported a mode change on its **first** call by design
  (`!g_have_last_mode`), so `main.cpp` called `widescreen_fit_window` on the first
  frame of every run. In a run that started fullscreen, that frame is after RT64
  set the window to the display size, and the fit derives a 4:3 width from the
  reported height.
- macOS: SDL knows the window is fullscreen (measured flags `0x20003625` include
  `SDL_WINDOW_FULLSCREEN`), so it ignores the resize. The measured window stayed
  1680x1050 points / 3360x2100 px for the whole run, and no `[widescreen] window`
  line was logged. This machine never showed the defect.
- Windows: SDL did not perform the transition, so it applied `SDL_SetWindowSize`
  and the borderless fullscreen window became the 4:3 shape anchored where RT64
  put it, the monitor's origin: a small picture in the top-left, desktop around
  it. That is the report.

Fix, both in `app/src/widescreen.cpp`:

- The first `widescreen_update` call adopts the current mode without reporting a
  change. The window already has that shape: `create_window` calls
  `widescreen_fit_window` right after creating it, with the mode `load_settings`
  left in place.
- `widescreen_fit_window` returns early while the live `GraphicsConfig` asks for
  fullscreen, and logs `[widescreen] fullscreen: the renderer owns the window's
  shape` once. It reads the port's own `wm_option` rather than SDL's flags,
  because the Windows path never sets `SDL_WINDOW_FULLSCREEN`.
- Leaving fullscreen is the mirror case: the window comes back with the rect RT64
  saved before the transition, which may predate a WIDESCREEN change made while
  fullscreen. `RT64Renderer::update_config` now calls the new
  `widescreen_request_refit()` after `setFullScreen(false)`, and the next frame's
  `widescreen_update` reports a change, so the shape is re-derived once the
  window is back under SDL's control. Doing the fit from `update_config` itself
  would be an SDL call off the main thread.

Measured on macOS after the fix:

| run | result |
|---|---|
| `OGRE_WINDOW=fullscreen` | window 1680x1050 pt / 3360x2100 px for the whole run, `[widescreen] fullscreen: the renderer owns the window's shape` once, **no** `[widescreen] window` line, `[renderer] config: … window fullscreen` |
| fullscreen, WIDESCREEN toggled on under the overlay | still no resize |
| fullscreen, WIDESCREEN on, then WINDOWED under the overlay | `960x715 -> 1271x715 (window 16:9, mode ON)` on the frame after the exit |
| windowed, WIDESCREEN toggled on | `960x720 -> 1280x720 (window 16:9, mode ON)`, unchanged |
| stock run | `tools/runlog.py --check` PASS; `build-null` builds |

**Not verified on Windows.** This machine is macOS, where the defect never
reproduced, so the report itself is only explained by the mechanism above. The
re-test is a Windows run: the log must show the skip line and no
`[widescreen] window` line, and the picture must fill the display. If it does not,
the next suspect is RT64's plume swap chain on Windows (the port no longer
touches the window's geometry in fullscreen, so what is left is RT64's own
sizing), and the instrument is plume's D3D12 swap chain size against
`GetClientRect`.

## Verified

| check | result |
|---|---|
| SETTINGS tab draws the rows | `docs/proofs/native-launcher-settings.png`; `RESOLUTION [x] NATIVE [ ] 2X [ ] AUTO`, `MSAA [x] OFF [ ] 2X [ ] 4X [ ] 8X`, `WINDOW [x] WINDOWED [ ] FULLSCREEN` under a `DISPLAY` heading |
| keys change them | `OGRE_LAUNCHER_TAB=settings OGRE_LAUNCHER_KEYS="Down,Down,Right,Down,Right,Down,Right"` gives `resolution = 2x`, `msaa = 2x`, `window = fullscreen` in `settings.cfg` |
| the pad changes them | the same sequence as `OGRE_LAUNCHER_PAD="ddown,ddown,dright,ddown,dright,ddown,dright"` writes the same three lines |
| the pad opens and navigates the overlay | `OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_PAD="back,ddown,ddown,dright"` logs `[overlay] shown`, then `[renderer] config: resolution 2x …`, then `[input] overlay open: game input suppressed`; `docs/proofs/native-overlay-settings.png` is that frame (`window = fullscreen`, the shipped rows) |
| `Back` toggles | `OGRE_OVERLAY_PAD="back"` -> one `[overlay] shown`; `back,back` -> `shown` then `hidden` (4/4 runs); `back,back,back` -> `shown`, `hidden`, `shown` |
| the values reach RT64 when the flag is on | `OGRE_DISPLAY_EXPERIMENTS=1 OGRE_RESOLUTION=2x OGRE_MSAA=4x OGRE_WINDOW=fullscreen` logs `[renderer] config: resolution 2x (mult 2.00), msaa 4x, window fullscreen`, exits 0 |
| the gate ignores them when it is off | a `settings.cfg` with `resolution = 2x`, `msaa = 4x`, `window = fullscreen` boots as `resolution native, msaa off, window fullscreen` and logs `resolution 2x and msaa 4x are experimental and not applied; set OGRE_DISPLAY_EXPERIMENTS=1 to enable them` |
| the gate keeps the configured values | changing WINDOW while gated rewrites `window = fullscreen` and leaves `resolution = 2x`, `msaa = 8x` in the file |
| the rows follow the gate | `docs/proofs/native-launcher-settings.png` has two DISPLAY rows; `docs/proofs/native-launcher-settings-experiments.png` (`OGRE_DISPLAY_EXPERIMENTS=1`) has four |
| widescreen does not clobber them | `OGRE_DISPLAY_EXPERIMENTS=1 OGRE_WS_SCENES=4 OGRE_WIDESCREEN=on OGRE_RESOLUTION=2x OGRE_MSAA=4x` logs the aspect change and then a config line that still reads `resolution 2x (mult 2.00), msaa 4x, window windowed` |
| no regression with defaults | stock 25 s run: `tools/runlog.py --check` PASS, 655 display lists, no crash, no stub call, no unknown module, no stubbed non-gfx task |
| the null-renderer variant builds and runs | `cmake --build build-null` clean; `OGRE_RESOLUTION=2x OGRE_MSAA=4x OGRE_WINDOW=fullscreen` boots and exits 0 |
| the browser build is unaffected | `app/CMakeLists.txt`'s `EMSCRIPTEN` source list has no `settings.cpp`, `ui.cpp`, `launcher.cpp` or `overlay.cpp` |

### Loose end

One `back,back` scripted run logged `[overlay] shown`, `[overlay] hidden`, and a
third `[overlay] shown` later in the run (line 922 of
`/tmp/gs10/run.log`). Four repetitions of the same command and a single-`back` run
produced one `shown` and one `hidden` each, and `back,back,back` produced exactly
three lines. No mechanism was found. Both transitions print a line, so the
toggle is easy to re-check; if it recurs, the next instrument is a log of the
event that opened the overlay.

## Files changed

- `app/src/settings.{hpp,cpp}` — `ResolutionMode`, `AntialiasMode`,
  `DisplayMode`, their parsers, labels, setters and `publish_graphics()`; three
  new `settings.cfg` keys; three new `OGRE_*` overrides; the boot line lists the
  new values.
- `app/src/ui.{hpp,cpp}` — three `RowKind`s, the shared radio-row helpers,
  `row_is_steppable`, `PadCommand`, `pad_command_for_button`, `pad_command`, the
  DISPLAY section, the two CONTROLS pad lines.
- `app/src/launcher.cpp` — `OGRE_LAUNCHER_PAD`, `SDL_CONTROLLERBUTTONDOWN`
  handling, `step_option` now uses `row_is_steppable`.
- `app/src/overlay.cpp` — `OGRE_OVERLAY_PAD`, controller-button handling (Back
  open/close, navigation), the key handler now uses `row_is_steppable`.
- `app/src/input_map.{hpp,cpp}` — `pad_button_from_name`, `pad_button_is_bound`.
- `app/src/renderer.cpp` — the `[renderer] config:` line, and the
  `widescreen_request_refit()` call when the renderer leaves fullscreen.
- `app/src/widescreen.{hpp,cpp}` — the first-call change, the fullscreen guard and
  `widescreen_request_refit`.
- The gate described above (`OGRE_DISPLAY_EXPERIMENTS`), added after the first
  pass shipped the two rows to players.
- `docs/guides/app-build.md` — the DISPLAY section, the pad controls under the
  CONTROLS tab, the new knobs in the scripted-run table, two new examples, the
  SETTINGS tab row list, the proofs line.
- `docs/proofs/native-launcher-settings.png`,
  `docs/proofs/native-launcher-settings-experiments.png` (new),
  `docs/proofs/native-overlay-settings.png`,
  `docs/proofs/native-launcher-controls.png` — refreshed.
- `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, this file.

No probe was added, and no generated code was edited. `git status --short` shows
only these files (plus the `tools/RT64` submodule state that was already
modified when the session started).
