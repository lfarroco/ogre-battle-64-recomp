# Handoff — 2026-09-20, session 93: a SETTINGS tab with GAME SPEED, changeable while playing

**Goal (developer):** add a SETTINGS tab to the start screen with a single
setting, `GAME SPEED [x] 1 [ ] 2 [ ] 4 [ ] 6 [ ] 8`. If it is easy, let the
player open the in-game `ESC` overlay and change the speed while playing.
Mid-session the developer asked to drop 6 and 8: *"6 and 8 are way too fast and
input doesnt work properly, too hard to aim the cursors"*. The tab offers **1, 2
and 4**.

**Result:** the shared `ui::Panel` has a fifth tab whose only row is a radio
group. The value is the runtime's emulated-clock multiplier, the quantity
`OGRE_SPEED` already sets, persisted as `<config>/settings.cfg` and applied at
startup. `ESC` opens the same panel over the running game with CONTROLS **and**
SETTINGS enabled, so the speed changes live; the guest clock is continuous
across a change.

---

## 1. The settings module (`app/src/settings.{hpp,cpp}`)

`settings.cpp` mirrors `input_map.cpp`: a process-wide live value, a plain-text
file under the config dir, and a setter that applies the change and writes the
file.

* The offered table is `{1, 2, 4}` (`kGameSpeedCount`). `game_speed_value()`
  indexes it, `game_speed_index()` returns the index of a live value (or -1 when
  the value came from `OGRE_SPEED` outside the table), and `nearest_game_speed()`
  snaps a file value the table no longer offers (a stale `8` becomes `4`).
* `load_settings(pref_dir)` reads `<config>/settings.cfg`, then applies the
  result to the runtime with `ultramodern::set_speed_multiplier()`. `main.cpp`
  calls it next to `load_input_map()` after `resolve_pref_dir()` and before
  `recomp::start()`.
* **`OGRE_SPEED` wins for one run.** The runtime already reads it at static-init
  time; `load_settings` prefers it, logs `[settings] OGRE_SPEED=<n> overrides the
  saved game speed`, and does not write the file. Without it the file is the
  starting value, and a fresh config dir stays at 1 and writes nothing.
* The file is `game_speed = <n>`, with a tolerant parser (a `#` comment, a bare
  integer on its own line, or the `key = value` form all work).
* The live value is read and written only from the main thread (the launcher
  loop and the overlay's `update_gfx` callback), so it needs no lock.

## 2. The SETTINGS tab (`app/src/ui.{hpp,cpp}`)

* `Tab::Settings` is the fifth tab; `tab_enabled()` returns true for it in both
  `Mode::Launcher` and `Mode::Overlay`. In the overlay, `TAB` now cycles between
  CONTROLS and SETTINGS (the other three stay disabled and greyed).
* `RowKind::GameSpeed` is one row. The left column reads `GAME SPEED`; the right
  column is the radio group.
* The radio group is not a wrapped text run, so `layout_game_speed()` lays it out
  and both `measure_panel` (for the row height) and `draw_panel` (for the markers
  and their click rects) call it. The result is a pure function of
  `(font, scale, width, current value)`, so the measured and the drawn row agree.
  The option that matches the live speed is always drawn warm (as is the whole
  row when selected).
* `Panel::Geometry` gained `options` (one absolute `SDL_Rect` per radio marker)
  and `Panel::Hit` gained `Kind::RowOption`. `hit_test` checks the markers before
  the whole-row rects, and both the launcher and the overlay call
  `Panel::choose_option(row, option)` for that hit. A click on `4` sets 4.
* `Panel::activate()` on the row steps the group (`LEFT`/`RIGHT`/`SPACE`), with
  wrap at the ends like a mod option row. A live `OGRE_SPEED` value outside the
  table starts the walk at the first step.
* The overlay's key handler did not handle `LEFT`/`RIGHT` at all; it does now,
  for a GameSpeed row (and an Option row, though the overlay has none). The
  overlay hint reads `TAB SWITCHES TAB   UP/DOWN SELECT   LEFT/RIGHT CHANGE   ESC
  RESUMES THE GAME`; the launcher's is unchanged.

## 3. The runtime setter and the continuous clock (`timer.cpp`)

`n64modernruntime-ob64.patch` adds `ultramodern::set_speed_multiplier(uint32_t)`
next to the existing `get_speed_multiplier()`.

`counter_per_ms` was a `static const` derived from a `static const`
`speed_multiplier` read once from `OGRE_SPEED`, so a change was impossible. The
clock is now piecewise linear in wall time:

* `speed_start_us` / `speed_start_ticks` anchor where the current multiplier took
  effect, and `time_now()` is `start_ticks + (host_us - start_us) * (46875 *
  multiplier) / 1000`.
* `set_speed_multiplier()` closes out the current segment (earning the ticks at
  the **old** rate), moves the anchor, then stores the new multiplier. With the
  anchors at zero the formulas are byte-for-byte the old ones, so a run that
  never changes speed is numerically unchanged.
* Without the anchors a change from 1 to 4 would add the whole elapsed run to the
  guest clock at once and fire every armed `OSTimer`. That is what motivated the
  anchors, not a game-logic repair.
* The three values are one snapshot published under a **seqlock**: the writer
  holds an odd `speed_seq` for the length of the update, and readers retry
  instead of using a torn snapshot. `get_speed_multiplier()` is a plain relaxed
  atomic load.

**The mutex cost the boot.** The first version put a `std::mutex` around the
three read paths. It is measurable: at `OGRE_SPEED=4` the boot reached scene
`0x09` at 7.28 s / 6.51 s, then 8.38 s, against the pre-change binary's
6.08 s / 5.33 s / 5.29 s. A tagged probe that removed only the reader locks
(`probe93`, reverted by the seqlock rewrite) reached `0x09` at 3.34 s / 4.01 s,
which implicated the lock rather than the build. With the seqlock, three
alternating runs of the new and pre-change binaries are within 50 ms (scene
`0x09`: 3.94/3.91, 3.93/3.96, 3.96/3.91 s; scene `0x0A`: 6.56/6.54, 6.53/6.57,
6.56/6.55 s). The pre-change binary is the packaged `dist/ogre-battle-64-recomp`
build from before this session.

Audio at a multiplier above 1 is the pre-existing `OGRE_SPEED` behaviour; every
documented `OGRE_SPEED` run turned audio off, and this session did not test it.

## 4. The proof the guest clock is continuous

`set_speed_multiplier()` logs the new multiplier with the guest tick and the host
time at the change:

```
[timer] game speed multiplier = 2 (guest tick 149131593, host us 3181474)
[timer] game speed multiplier = 4 (guest tick 157474686, host us 3270467)
```

`3,181,474 us * 46.875 = 149,131,594` (the run began at 1). The next change is
`+88,993 us` at multiplier 2: `88,993 * 93.75 = 8,343,094`, and the logged delta
is `8,343,093`. Each segment earns ticks at its own rate, and the change itself
moves nothing. A naive implementation would have reported the second change at
about `2 * elapsed` instead.

## 5. Test knobs added

| knob | effect |
|---|---|
| `OGRE_LAUNCHER_TAB=settings` | (extended) open the start screen on the SETTINGS tab |
| `OGRE_OVERLAY_TAB=<controls\|settings>` | open the overlay's panel on that tab |
| `OGRE_OVERLAY_SHOT_MS=<n>` | delay `OGRE_CAPTURE_OVERLAY` by `n` ms after the overlay is shown, so the capture shows what `OGRE_OVERLAY_KEYS` changed (0 keeps the first-frame behaviour) |

## 6. What was run for verification

* `docs/proofs/native-launcher-settings.png`: the launcher's SETTINGS tab,
  `GAME SPEED  [ ] 1 [ ] 2 [x] 4` in the tab bar
  `START GAME / ROM / MODS / CONTROLS / SETTINGS`.
* `docs/proofs/native-overlay-settings.png`: the overlay's SETTINGS tab after a
  scripted `Right`, showing `[x] 2`; the other three tabs are greyed, CONTROLS
  and SETTINGS are not.
* **Step and persist (launcher):** `OGRE_LAUNCHER_KEYS="Right,Right,Right"` on the
  SETTINGS tab logged `2`, `4` and `[timer] game speed multiplier = 6`, and wrote
  `game_speed = 6` (run before the options were cut to 1/2/4).
* **Change while playing (overlay):** `OGRE_OVERLAY_AT_MS=2500
  OGRE_OVERLAY_TAB=settings OGRE_OVERLAY_KEYS="Right,Right"` logged the 1 -> 2 and
  2 -> 4 changes with continuous guest ticks and wrote `game_speed = 4` to a fresh
  config dir.
* **Saved value applies at startup:** a config dir holding `game_speed = 8`
  reaches scene `0x0A` (publishers) and the title `0x04` in a 12 s run, while
  `game_speed = 1` from the same recipe is still in `0x09` (intro); the stale 8 is
  loaded as 4 first.
* **`OGRE_SPEED` precedence:** with `settings.cfg` at 8 and `OGRE_SPEED=1`, the
  log shows the override and the file still reads `game_speed = 8`.
* **Defaults are unchanged:** a fresh config dir logs `[settings] game speed x1`
  and writes no `settings.cfg`; a run with no `OGRE_SPEED` at speed 1 matches the
  pre-change boot sequence (`0x00 -> 0x19E8 -> 0x00 -> 0x09`).
* **Regression:** the maintained 45 s title-route run
  (`OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1
  OGRE_EXIT_AFTER_MS=45000`) exits 0 and `tools/runlog.py --check` prints PASS: no
  crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx
  task.
* **Patch:** `n64modernruntime-ob64.patch` was regenerated from
  `git -C tools/N64ModernRuntime diff` (minus the `N64Recomp` submodule line) and
  applies clean to a worktree of pristine `589bbf0` (`git apply --check`).
* `build-app` links. The generated game code was not touched, so `make recomp` /
  `make bank-recomp` were not needed.

No probe is left in the tree: `grep -r probe93` is empty, and the temporary
instrumentation was the reader-lock probe above.

## 7. Files changed

* `app/src/settings.hpp`, `app/src/settings.cpp` — new: the offered speeds, the
  live value, the file, and the startup load.
* `app/src/ui.hpp`, `app/src/ui.cpp` — `Tab::Settings`, `RowKind::GameSpeed`,
  `layout_game_speed()`, `choose_option()`, the option hit rects, the
  CONTROLS+SETTINGS overlay policy, the updated overlay hint.
* `app/src/launcher.cpp` — the SETTINGS tab in `OGRE_LAUNCHER_TAB`, `LEFT`/`RIGHT`
  for the row, the radio-marker click hit.
* `app/src/overlay.cpp` — `LEFT`/`RIGHT` for the row, the marker click hit,
  `OGRE_OVERLAY_TAB`, `OGRE_OVERLAY_SHOT_MS`, the window title
  `Ogre Battle 64: Options`.
* `app/src/main.cpp` — `load_settings()` next to `load_input_map()`.
* `app/CMakeLists.txt` — the new translation unit.
* `tools/N64ModernRuntime/ultramodern/src/timer.cpp`,
  `tools/N64ModernRuntime/ultramodern/include/ultramodern/ultramodern.hpp`,
  `n64modernruntime-ob64.patch` — the runtime setter, the piecewise-linear clock
  and the seqlock. The vendored `tools/N64ModernRuntime` tree is gitignored and
  changed through the patch.
* `docs/proofs/native-launcher-settings.png`,
  `docs/proofs/native-overlay-settings.png`.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/guides/app-build.md`,
  `packaging/README-dist.txt`, this file. (`docs/README.md`'s handoff index is
  curated by hand and stops at session 90, so it was left alone.)

## 8. Open leads

1. **6 and 8 are still reachable through `OGRE_SPEED`**, where the SETTINGS tab
   cannot show them selected (no marker is checked, and the first `LEFT`/`RIGHT`
   snaps to an offered value). The developer's reason for dropping them is cursor
   aiming, not the runtime.
2. **The overlay does not pause the game** (session 92's choice), so a speed
   change takes effect in the middle of whatever is running. The clock continuity
   is what makes that safe for timers; a scene mid-load was not tested.
3. **Audio above 1x is untested.** `OGRE_SPEED` runs always turned audio off.
4. **The tab has no reset row**, unlike CONTROLS. A second setting would make the
   tab's shape worth revisiting, including whether a section header belongs above
   the row.
5. **The marker click was not exercised with a real mouse.** The geometry, the
   hit order and the handler are code-verified; the keyboard path is what was run.
