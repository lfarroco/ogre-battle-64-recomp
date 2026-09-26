# Handoff — 2026-09-25, session 105: WIDESCREEN is a toggle

**Goal (developer):** the SETTINGS row offers `off` / `missions` / `always`, but
`missions` and `always` make no visible difference: with either one the window is
16:9 for the whole run and the non-mission scenes have black bars on the sides.
The split is a leftover from the first implementation, which resized the window at
every scene change. Replace the three options with one toggle.

**Result:** `WidescreenMode{Off,On}` and a two-option radio row that reads
`WIDESCREEN  [ ] OFF [x] ON`. `on` keeps the shipped `missions` behaviour: Expand
during the dispatcher's mission scene `0x03`, and 4:3 elsewhere inside the 16:9
window. The retired `missions` / `always` spellings are still read as `on`, in both
`settings.cfg` and `OGRE_WIDESCREEN`, so no saved value changes behaviour.

`always` (Expand everywhere, which stretched the boot's full-screen 2D stills) is
gone. Session 102's handoff §6 named it as the mode to drop; the toggle removes it
without changing the default.

---

## 1. What changed

| file | change |
|---|---|
| `app/src/settings.hpp` | `WidescreenMode{Off, On}`, `kWidescreenModeCount = 2`, comments |
| `app/src/settings.cpp` | `widescreen_mode_label` returns `OFF` / `ON`; `parse_widescreen` maps `on`/`1`/`true`/`missions`/`mission`/`always`/`2` to `On` and everything else to `Off`; the file's comment is `# widescreen: off or on.` |
| `app/src/widescreen.hpp`, `app/src/widescreen.cpp` | `wants_expand()` is `widescreen_mode() != WidescreenMode::Off && active_scene_id() == kMissionScene`; comments |
| `app/src/main.cpp` | the `update_gfx` comment no longer names the removed `Missions` mode (comment only) |
| `docs/guides/app-build.md` | the SETTINGS section, the window table, the `OGRE_WIDESCREEN` knob row and the example command |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and record |

No change was needed in `ui.cpp`, `launcher.cpp` or `overlay.cpp`. The radio row
builds its labels from `kWidescreenModeCount` and `widescreen_mode_label`, and the
launcher and overlay already step `RowKind::Widescreen` with `LEFT`/`RIGHT` and
`SPACE`.

The scene gate and the window shape are unchanged:

* `wants_expand` keeps the scene gate, so `on` is exactly the old `missions`.
* `window_aspect()` is unchanged, so `off` opens 4:3 and `on` opens 16:9.

## 2. Verification

Build: `make app` on macOS, which writes `build-dist/ogrebattle64`
(`DIST_STATIC_SDL ?= 1`, `Makefile:431`). Exit 0, no warnings from the changed
files.

| check | command | result |
|---|---|---|
| the row | `OGRE_PREF_DIR=/tmp/ws105 OGRE_LAUNCHER=1 OGRE_LAUNCHER_TAB=settings OGRE_LAUNCHER_SHOT=/tmp/ws105-settings.ppm ./build-dist/ogrebattle64` | draws `WIDESCREEN   [x] OFF [ ] ON`; boot logs `widescreen off` |
| the toggle and the file | `... OGRE_LAUNCHER_KEYS="Down,Right" ...` | writes `widescreen = on` in `/tmp/ws105b/settings.cfg`; a second boot logs `game speed x1, widescreen on` |
| the click/step path | the same run | `Down,Right` on the WIDESCREEN row sets the second option, which is `Panel::activate`'s existing radio step (`ui.cpp:796-808`) |
| retired spellings | `OGRE_WIDESCREEN=<v>` for `missions`, `always`, `on`, `1`, `2`, `off`, `0` | the first five log `widescreen on`; `off` and `0` log `widescreen off` |
| the scene gate | `OGRE_PREF_DIR=/tmp/ws105-run OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1 OGRE_WIDESCREEN=on OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_TAP_MS=1200 OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:4,0x03:a:6" OGRE_EXIT_AFTER_MS=40000 ./build-dist/ogrebattle64` | `aspect ratio expand (scene 0x0003, mode ON)` on each mission visit and `aspect ratio original (scene 0x0002, mode ON)` between them; two visits, exit 0 |
| the overlay | `OGRE_PREF_DIR=/tmp/ov105 OGRE_ROM=assets/ogre64.z64 OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_AT_MS=2500 OGRE_OVERLAY_KEYS="Down,Right" OGRE_CAPTURE_OVERLAY=/tmp/ov105.ppm OGRE_OVERLAY_SHOT_MS=1500 OGRE_EXIT_AFTER_MS=9000 ./build-dist/ogrebattle64` | the overlay draws `WIDESCREEN   [ ] OFF [x] ON`, writes `widescreen = on`, and re-fits the window once (`960x720 -> 1280x720 (window 16:9, mode ON)`) |

The proof `docs/proofs/native-widescreen-launcher.png` is the new SETTINGS row from
the launcher, and `docs/proofs/native-widescreen-overlay.png` is the same row in
the `ESC` overlay with `ON` selected. The other four `native-widescreen-*.png`
files from session 102 still show `ON` / `OFF` behaviour and were not recaptured.

No probes were used. Nothing under `RecompiledFuncs/`, `Bank*Funcs/` or `tools/`
was instrumented.

## 3. Open

* `on` gates Expand to scene `0x03`. The tutorial's practice stage is scene `0x17`
  and stays 4:3, as it did under `missions` (session 102 §6). Adding `0x17` to
  `wants_expand` is one line if the developer wants it.
* The `always` behaviour is gone. If a player wants Expand on a non-mission 3D
  scene, that scene has to join the gate.
