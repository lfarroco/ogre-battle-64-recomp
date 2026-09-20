# Handoff — 2026-09-20, session 92: a CONTROLS tab, and an in-game Esc overlay

**Goal (developer):** the start screen loads the ROM and manages mods; add a tab
for controller input. Pressing `ESC` while playing should bring up an overlay
with the same content as the launcher, with the ROM and MODS settings disabled
and the CONTROLS tab active. The ASCII look of the launcher stays; the
Zelda64Recomp overlay is "too web-y".

**Result:** the start screen is now a tabbed panel (**START GAME**, **ROM**,
**MODS**, **CONTROLS**) and the CONTROLS tab shows and rebinds one row per N64
button, for keyboard and gamepad, with a reset row. Bindings are data
(`app/src/input_map.cpp`), persisted to `<config>/controls.cfg`, and the input
callbacks read them every poll. `ESC` during play opens a borderless
always-on-top SDL window placed over the game window, drawn with the same panel
and font; while it is open the game receives an idle pad instead of the keyboard.
The developer chose a layered window (not an RT64 render pass) and
"suppress game input only" (the game keeps running behind the overlay).

---

## 1. The input map (`app/src/input_map.{hpp,cpp}`)

The mapping was hardcoded in `sdl_platform.cpp`: `keyboard_buttons()` named 14
scancodes, `gamecontroller_buttons()` named 12 SDL buttons plus the right-stick C
layer. It is now a `InputMap` with one `ButtonBinding` per N64 button: one
`SDL_Scancode` and one gamepad source (`None`, a `SDL_GameControllerButton`, or a
right-stick direction).

* **Defaults reproduce the old map exactly.** Keys `X Z C / Return / Q E /
  Up Down Left Right / I J K L`; pad `A B L-Stick Start LB RB` plus the D-pad,
  `X`/`Y` for C-UP/C-DOWN, and nothing for C-LEFT/C-RIGHT (the always-on right
  stick covers those). `N64ButtonInfo::field` carries the field-map action text
  from the developer's GameFAQs description and is the CONTROLS tab's second
  column.
* **The right stick stays a fixed extra layer** in `sdl_platform.cpp`, in
  addition to whatever the map binds to a C button, so the default pad behaviour
  is unchanged.
* **`ESC` is reserved** (`is_reserved_key`): the capture refuses it, because the
  overlay owns it.
* **Persistence** is plain text at `<config>/controls.cfg` (`input_map_path`).
  Key names are `SDL_GetScancodeName`/`FromName`; gamepad names come from a local
  table, so the file does not depend on `SDL_GameControllerGetStringForButton`.
  A missing or unreadable file leaves the defaults. Writes go through
  `update_input_map()`, which replaces the live map under a mutex and writes the
  file.
* **Threading:** `read_input_map()` copies the map under a mutex. Input is polled
  on the game thread and the UI edits on the main thread.

`keyboard_buttons()` is now `keyboard_buttons_from_map(read_input_map())` plus
the scripted-tap layer; `gamecontroller_buttons()` is
`gamecontroller_buttons_from_map(...)` plus the fixed right stick.

## 2. The shared tabbed panel (`app/src/ui.{hpp,cpp}`)

`TextLayer`, `wrap_text`, `chars_per_line`, `truncate_to_width` and the panel
model moved out of `launcher.cpp` into `ui.cpp`, so the launcher and the overlay
draw the same thing.

* `ui::Panel` has a `Mode` (`Launcher` or `Overlay`) and four tabs. Rows are
  rebuilt per tab: START GAME (`[x] Start Game`), ROM (`[x] Loaded!` plus the
  file name), MODS (one row per mod, then its visible options — unchanged from
  session 91), CONTROLS (one row per N64 button: left column `NAME  KEY <key>`,
  right column `PAD <pad> - <field action>`; then a reset row and two fixed-stick
  notes).
* Keys: `TAB` / `SHIFT+TAB` switch tabs, `UP`/`DOWN` move, `SPACE` activates,
  `LEFT`/`RIGHT` step a mod option, `ENTER` plays, mouse clicks select a tab or a
  row.
* Rebinding: `SPACE` on a binding row arms a capture. The next keydown sets the
  keyboard slot; a gamepad button sets the pad slot; `BACKSPACE` clears the
  keyboard slot, `DELETE` the pad slot, `ESC` cancels. The hint line changes while
  a capture is armed.
* In `Overlay` mode `tab_enabled()` is true only for CONTROLS, so the tab bar is
  drawn with the other three greyed and `TAB` does not move off CONTROLS.
* `measure_panel()` exists because the whole screen is centred before its top
  edge is chosen; `draw_panel()` calls it. The panel background is drawn before
  the tab bar, which is what makes an active tab visible on top of it.

Two layout changes were needed because CONTROLS is 18 rows: the two trailing
fixed-stick notes are one line each, row/section padding is smaller, and the
launcher's "SAVED TO:" footer follows the panel instead of being pinned to the
window bottom (at 720p the pinned footer overlapped the last rows).

## 3. The launcher change

`run_launcher` keeps ROM discovery, the drop/picker handlers, the error block and
the footer; the list is `ui::Panel`. The rebind capture consumes every keydown
while armed. A `PadList` (`sdl_platform.hpp`) opens controllers so the CONTROLS
tab can capture a gamepad button; a 200 ms grace after arming stops the button
that opened the capture from binding itself.

## 4. The in-game overlay (`app/src/overlay.{hpp,cpp}`)

RT64 owns the game window and has no UI pass, so the overlay is a second SDL
window: `SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SKIP_TASKBAR`, sized and positioned from
`SDL_GetWindowPosition/Size` of the game window and re-synced each visible frame.
`SDL_SetWindowOpacity` (default 0.90, `OGRE_OVERLAY_OPACITY`) is what lets the
game show through; SDL has no per-pixel alpha for a normal window. The window is
created on the first `ESC` and destroyed in `overlay_shutdown()`.

* Main-thread only: `overlay_handle_event()` is called for every event from
  `pump_sdl_events`, and `overlay_render()` from `update_gfx`, both on the main
  thread, which is what SDL's Cocoa backend requires.
* **Input suppression:** `overlay_visible()` is an atomic flag; `get_input`
  returns `true` with `buttons=0, x=0, y=0` while it is set, so the game sees an
  idle controller rather than a disconnected one. The first suppression logs
  `[input] overlay open: game input suppressed`.
* `ESC` opens when hidden and closes when visible; closing calls
  `SDL_RaiseWindow(game_window)` to hand the keyboard back.
* The panel is `ui::Panel(Mode::Overlay)`: CONTROLS active, START GAME / ROM /
  MODS disabled.

## 5. The controller-enumeration fix in `pump_sdl_events`

SDL reports already-connected controllers with `SDL_CONTROLLERDEVICEADDED`
events pushed at `SDL_InitSubSystem` time. The launcher's event loop drained
those, and since session 91 the start screen is shown whenever a mod is
installed, so a pad connected before launch was invisible to the game. The event
pump now enumerates `SDL_NumJoysticks()` once on its first call and opens each
`SDL_IsGameController` device into slots 1..3 (slot 0 is keyboard-first), the same
assignment the `SDL_CONTROLLERDEVICEADDED` branch makes.

## 6. Test knobs added

| knob | effect |
|---|---|
| `OGRE_LAUNCHER_TAB=<start\|rom\|mods\|controls>` | open the start screen on that tab |
| `OGRE_LAUNCHER_KEYS=<name>,...` | push one synthetic keydown per 150 ms through the real handler |
| `OGRE_LAUNCHER_SHOT=<path>` | write the renderer as a PPM and quit |
| `OGRE_LAUNCHER_SHOT_MS=<n>` | delay the shot (default: after the scripted keys) |
| `OGRE_OVERLAY=1` | open the overlay at startup |
| `OGRE_OVERLAY_AT_MS=<n>` | push one synthetic `ESC` n ms after init |
| `OGRE_OVERLAY_KEYS=<name>,...` | push one synthetic keydown per 150 ms while visible |
| `OGRE_OVERLAY_OPACITY=<0.2..1.0>` | window opacity |
| `OGRE_CAPTURE_OVERLAY=<path>` | write the overlay renderer as a PPM, once |

## 7. What was run for verification

* All four tabs captured (`OGRE_LAUNCHER_TAB=... OGRE_LAUNCHER_SHOT=...`); the
  PPMs were converted with `sips` and are
  `docs/proofs/native-launcher-{start,rom,mods,controls}.png`.
* **Rebind, end to end, in the launcher:**
  `OGRE_LAUNCHER_KEYS="Tab,Tab,Tab,Space,p"` produced
  `<pref>/controls.cfg` with `a.key = P`, and the capture shows `KEY P`.
* **The overlay opens from the real Escape path:**
  `OGRE_OVERLAY_AT_MS=4000 OGRE_CAPTURE_OVERLAY=/tmp/overlay2.ppm` logged
  `[overlay] window ready`, `[overlay] shown`,
  `[input] overlay open: game input suppressed`, and wrote the capture
  (`docs/proofs/native-overlay-controls.png`).
* **Rebind through the overlay:** `OGRE_OVERLAY_AT_MS=4000
  OGRE_OVERLAY_KEYS="Space,p"` wrote `a.key = P` into a fresh config dir.
* **Compositing:** `screencapture -x -R200,177,1280,720` with the overlay up
  shows the panel, with the frontmost window behind it — the overlay window is
  always-on-top and the correct size and place. (The window in front in that
  capture was this session's browser, not the game, so what is proven is
  ordering and geometry, not the pixels of the game underneath.)
* **`controls.cfg` is read at boot:** a run with the file present logs
  `[input] loaded <pref>/controls.cfg`.
* **No boot regression:** a bounded baseline run reaches scene `0x09` at
  2305 ms and `0x0A` at 11754 ms (session 91's baseline: 1938 ms / 11370 ms); the
  launcher's `OGRE_TEST_DROP` path still accepts the ROM and boots.
* `cmake --build build-app` and `cmake --build build-null` both link.

No probe was left in the tree; the only temporary instrumentation (a per-row
layout dump) was removed.

## 8. Files changed

* `app/src/input_map.hpp`, `app/src/input_map.cpp` — new: the binding model, the
  file format, and the button-state conversion.
* `app/src/ui.hpp`, `app/src/ui.cpp` — new: `TextLayer`, the text helpers, the
  tabbed `Panel`, and the panel drawing.
* `app/src/overlay.hpp`, `app/src/overlay.cpp` — new: the layered overlay window.
* `app/src/sdl_platform.hpp`, `app/src/sdl_platform.cpp` — the input callbacks
  read the map, `get_input` gates on the overlay, `PadList`, the controller
  enumeration fix.
* `app/src/launcher.cpp` — the tabbed panel; the moved code is now in `ui.cpp`.
* `app/src/main.cpp` — `load_input_map()` after `resolve_pref_dir()`,
  `overlay_init()` after `create_window()`, `overlay_render()` in `update_gfx`,
  `overlay_shutdown()`.
* `app/CMakeLists.txt` — the three new translation units.
* `docs/proofs/native-launcher-*.png`, `docs/proofs/native-overlay-controls.png`.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/guides/app-build.md`, this file.

## 9. Open leads

1. **The overlay does not pause the game** (the developer's choice). The game
   keeps simulating behind it, so nothing stops a battle from continuing. A
   pause path would need a runtime hook, not a UI change.
2. **The overlay is a separate window, so it does not follow the game into
   macOS fullscreen** (another Space cannot be overlaid), and `SDL_SetWindowOpacity`
   is uniform: the text is dimmed by the same factor as the background. Per-pixel
   alpha would need `NSWindow` `setOpaque:NO`/clear background on macOS.
3. **Gamepad rebinding is code-verified only.** This machine has no gamepad, so
   `first_pressed_pad_button` and the pad half of the map were never observed
   with hardware. The pad defaults are copied from the old hardcoded map, so they
   are as correct as that was.
4. **The overlay's `ESC` is a raw keydown.** If the game ever maps Escape
   itself, the two would both act; no N64 button currently uses it.
5. **The start screen's `q` still quits**, and `Q` is also L's default key. In
   the launcher that is harmless; in the overlay `q` is not a quit key.
6. **A fullscreen game window is not covered.** `SDL_GetWindowPosition/Size` of
   a fullscreen window on macOS does not describe a rect another window can sit
   over.

## Addendum — the developer's two text changes

After the first pass the developer asked for two wording changes, both in
`ui.cpp`:

* the START GAME row's second column reads `Press ENTER or SPACE to start` when a
  ROM is ready (it was empty; `CHOOSE A ROM FIRST` is unchanged when none is);
* the launcher's hint line ends at `LEFT/RIGHT CHANGE` — `ENTER PLAY` is gone,
  because the row now says how to play.

The four launcher proofs were recaptured with the final build, and both variants
were rebuilt.
