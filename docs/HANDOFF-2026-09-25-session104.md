# Handoff — 2026-09-25, session 104: the ROM tab is gone, and the panel's position is fixed

**Goal.** Two developer requests for the shared `ui::Panel` that the start screen
(`app/src/launcher.cpp`) and the `ESC` overlay (`app/src/overlay.cpp`) both draw:

1. Remove the ROM tab. It is redundant, because a start screen with no ROM
   selected can offer the picker on the START GAME tab instead.
2. The UI moves depending on how tall the active tab's content is. Make it
   static, with the CONTROLS tab's position as the reference, so the header and
   the tab bar stay at the top.

**Result.** Both are in. The tab bar is now `START GAME / MODS / CONTROLS /
SETTINGS / DEBUG`; the START GAME tab's second row is the ROM row (`[ ] No ROM` /
`[x] ROM loaded`), and with no ROM that row is the selected one, so `SPACE` or a
click opens the picker as before. `measure_panel` now also measures the layout
reference tab (`kLayoutReferenceTab = Tab::Controls`, the tallest) and reports
`metrics.layout_height`; both callers centre the block and place their footer
with that, and `draw_panel` sizes the panel background with it. Measured on five
launcher captures at 2560x1440: the title occupies rows 48..117, the rule
202..205, the panel background starts at row 302 and the footer at 1382..1417 on
**every** tab. The pre-change binary, built for the A/B by stashing only these
four source files, put the title at row 446 on START GAME (446..515, one row of
panel), row 423 on SETTINGS (423..492) and row 48 on CONTROLS (48..117): the
header moved by up to 398 px between tabs.

## 1. Removing the ROM tab

`ui::Tab::Rom` is deleted (the enum is `Start, Mods, Controls, Settings, Debug,
Count`). `Panel::build_rows` (new; `rebuild_rows` was refactored into it) puts the
`RowKind::Rom` row on the START GAME tab, after `RowKind::Start`:

- `select_first()` already picks the first **selectable** row, and
  `selectable()` refuses `RowKind::Start` while `rom_` is empty, so the no-ROM
  case lands on the ROM row with no extra code.
- `RowKind::Rom`'s left text changed from `[x] Loaded!` to `[x] ROM loaded`,
  which reads as a ROM row next to `Start Game` instead of a whole tab.
- `launcher.cpp`'s error hint is now `PRESS SPACE ON THE ROM ROW TO TRY AGAIN`,
  and the `rom` value of `OGRE_LAUNCHER_TAB` is gone from the env parser: it
  selected the ROM tab before this change and now selects nothing.

## 2. A fixed layout height

The panel's height is `tab bar + rows + hint`, and `rows` depends on the tab, so
centring the block with it moved the header every time the tab changed.
`PanelMetrics` gained `layout_height`:

```
layout_height = max(active tab's height, reference tab's height)
```

`measure_panel` computes both with one shared helper, `layout_rows` (anonymous
namespace in `ui.cpp`), so the measured and the drawn row heights cannot drift.
The helper takes the row list and a `keep_lines` flag: the active tab's wrapped
right-column lines are kept for `draw_panel`, and the reference tab's are
discarded because only its total is read. This also removed the duplicated row
measurement that `measure_panel` carried inline.

`Panel::layout_rows_` holds the reference tab's rows. `rebuild_rows` builds it
with the same `build_rows(kLayoutReferenceTab)` call as the active tab's, so the
two row lists are always built from one code path.

Both callers use `metrics.layout_height` for `block_height` and for the footer's
`cursor_y`, and `draw_panel` fills the background with it. A tab taller than
CONTROLS (a mod list with many options) still grows the block, because
`layout_height` is a `max` and not a constant.

Not changed: the launcher's error block is still added to `block_height`, so a
rejected ROM re-centres the block as before. That is a separate state, not a tab.

## 3. What was run

Every capture below is from `build-dist/ogrebattle64`. **`make app` writes
`build-dist/`, not `build-app/`** (`DIST_STATIC_SDL ?= 1`, Makefile line 431;
already recorded in sessions 99 and 102). The first verification round in this
session read `build-app/ogrebattle64` and measured the *stale* binary, which is
how the wrong `reference_height` looked plausible for a few minutes. The
before/after pair is a real A/B: `git stash push -- app/src/ui.hpp app/src/ui.cpp
app/src/launcher.cpp app/src/overlay.cpp`, `make app`, three captures, then `git
stash pop` and `make app` again. The stashed build was only ever a capture
binary; the tree at the end of the session is the new code.

```sh
# five tabs, one PPM each (the ROM next to the exe is found automatically)
for tab in start mods controls settings debug; do
  OGRE_PREF_DIR=/tmp/ogre-ui OGRE_LAUNCHER=1 OGRE_LAUNCHER_TAB=$tab \
    OGRE_LAUNCHER_SHOT=/tmp/ogre-ui-shots/$tab.ppm ./build-dist/ogrebattle64
done

# the no-ROM case: an empty cwd (so no repo assets/), a fresh pref dir (so no
# stored ROM) and OGRE_LAUNCHER=1 (so no exe-adjacent ROM is searched for)
mkdir -p /tmp/ogre-empty-cwd && cd /tmp/ogre-empty-cwd
env -u OGRE_ROM OGRE_PREF_DIR=/tmp/ogre-ui-empty OGRE_LAUNCHER=1 \
  OGRE_LAUNCHER_SHOT=/tmp/ogre-ui-shots/norom.ppm \
  /Users/momo/dev/ogre/build-dist/ogrebattle64
```

A row-by-row diff of the five PPMs against the CONTROLS capture finds the first
difference at row 332, which is the tab bar's text (the active tab's label
changes colour), and the last at row 1321, the bottom of CONTROLS' own rows.
Rows 1322..1439 are byte-identical across all five, which covers the panel
background's bottom edge at 1329 and the footer band at 1382..1417. The panel
background occupies rows 302..1329 on every tab, because `layout_height` reserves
the CONTROLS height; only the rows inside it change. (The `norom` capture differs
again at row 1417, the footer, because its `SAVED TO:` path is a different
string.)

- `docs/proofs/native-launcher-start.png` — START GAME with no ROM: `[ ] Start
  Game` dimmed and `[ ] No ROM` selected, the picker hint on its right column.
- `docs/proofs/native-launcher-controls.png` — the reference layout.
- `native-launcher-mods.png`, `native-launcher-settings.png` — refreshed; the old
  files showed the removed ROM tab and the old centred position.
- `native-launcher-rom.png` — deleted; the tab it showed no longer exists. No
  living doc referenced it (only the session-92 handoff).
- In-game overlay, `OGRE_OVERLAY=1 OGRE_OVERLAY_TAB=settings|controls
  OGRE_CAPTURE_OVERLAY=…`: both captures have content in rows 66..1365 and differ
  first at row 242, the active tab's label. The header and tab bar are at the same
  y on CONTROLS and SETTINGS.
- Functional: `OGRE_LAUNCHER=1 OGRE_LAUNCHER_KEYS=space` logs `[launcher] accepted
  …` and boots (`[boot] start_game...`), so the START GAME row still starts the
  game; `OGRE_OVERLAY_KEYS=tab,tab,up,down` opens and navigates the overlay with
  no error.
- `cmake --build build-null --target ogrebattle64` also compiles (`ui.cpp`,
  `launcher.cpp`, `overlay.cpp`), and `make app` is warning-free.

## 4. Files changed

- `app/src/ui.hpp`, `app/src/ui.cpp` — `Tab::Rom` removed; `kLayoutReferenceTab`;
  `Panel::build_rows`, `layout_rows_`, `right_text_for(const Row&)`, `rows()`,
  `layout_rows()`; `layout_rows` helper; `PanelMetrics::layout_height`.
- `app/src/launcher.cpp` — tab docs, error hint, `OGRE_LAUNCHER_TAB`, and the
  block/footer math on `layout_height`.
- `app/src/overlay.cpp` — the block/footer math on `layout_height`.
- `docs/guides/app-build.md` — the tab list, the ROM row, `OGRE_LAUNCHER_TAB`'s
  values, the per-tab capture loop, and the overlay's greyed tabs.
- `docs/proofs/native-launcher-{start,controls,mods,settings}.png` refreshed;
  `native-launcher-rom.png` deleted.

## 5. Probes

One temporary `[dbg] rows=… layout=… h=… ref=…` `fprintf` in `launcher.cpp`,
added to diagnose the stale-binary reading and reverted. `grep -rn 'dbg\]'
app/src/` is empty and the tree was rebuilt after the revert.

## 6. Not verified

- No human has seen the fixed frame on a real window; the evidence is PPM
  readbacks.
- A tab taller than CONTROLS was not exercised (no mod with enough config rows is
  installed in this tree), so `layout_height`'s `max` arm has not been seen
  growing the block in a capture.
- The DEBUG tab is capturable in the launcher (`OGRE_LAUNCHER_TAB=debug` is
  parsed) and its capture is in the five-tab run, but the live overlay DEBUG
  readout was not re-captured this session.
