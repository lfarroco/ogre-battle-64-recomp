# Handoff — 2026-09-25, session 106: SOUNDS and VOLUME in the SETTINGS tab

**Goal (developer):** add a sound control under SETTINGS in the overlay —

```
sounds [x] on [ ] off
volume ----------| (0-100%)
```

with `|----------` at 0% and `-----|-----` at 50%.

**Result:** the SETTINGS tab has an `AUDIO` section with `SOUNDS  [x] ON [ ] OFF`
and `VOLUME  ----------| 100%`. `LEFT`/`RIGHT`/`SPACE` step both; a click sets a
radio marker or a slider cell. The gain is applied in `queue_audio_samples`, and
a mute queues silence rather than nothing, so the runtime's queue-depth pacing is
unchanged. Both values are saved in `<config>/settings.cfg`, with
`OGRE_SOUNDS` / `OGRE_VOLUME` as one-run overrides. The launcher shows the same
rows, because both screens draw one `ui::Panel`.

---

## 1. What changed

| file | change |
|---|---|
| `app/src/settings.hpp` | `SoundMode{On,Off}` (row order, `On` = option 0 and default), `sound_mode_label` / `sound_mode` / `sound_mode_index` / `set_sound_mode` / `sound_mode_text`; the VOLUME constants `kVolumeStepPercent = 5`, `kVolumeCells = 11`, `kVolumeDefaultPercent = 100` and `volume_percent` / `volume_handle_cell` / `volume_percent_for_cell` / `volume_text` / `set_volume_percent`; `audio_gain_percent()` |
| `app/src/settings.cpp` | the two live values and the atomic `g_audio_gain` (`publish_audio_gain()` after every write and in `load_settings`); `parse_sound` (off spellings only, default `On`) and `parse_volume` (int, clamped 0..100, default 100); the file's `sounds = ` and `volume = ` lines; `OGRE_SOUNDS` / `OGRE_VOLUME` overrides; the startup line now also prints `sounds <mode>, volume <n>%` |
| `app/src/ui.hpp` | `RowKind::Sound`, `RowKind::Volume` |
| `app/src/ui.cpp` | the SETTINGS tab's `AUDIO` section and two rows; `layout_sound`; `SliderLayout` / `layout_volume` (the bar's text plus one click rect per cell); `left_text` / `right_text_for`; the `activate` arms (SOUNDS wraps, VOLUME steps and clamps); the `choose_option` arms (SOUNDS option, VOLUME cell); the draw arm (bar as one text run, cell rects into the panel geometry) |
| `app/src/launcher.cpp`, `app/src/overlay.cpp` | the two row kinds added to the `LEFT`/`RIGHT` lists |
| `app/src/sdl_platform.cpp` | `queue_audio_samples` scales (100 = the buffer unchanged, below 100 = a scaled copy in a scratch buffer, 0 = silence); `#include "settings.hpp"` |
| `docs/guides/app-build.md` | the `OGRE_SOUNDS` / `OGRE_VOLUME` knob rows and a "Sounds and volume (the SETTINGS tab)" section |
| `docs/proofs/native-overlay-settings.png`, `docs/proofs/native-launcher-settings.png` | refreshed; both were stale and still showed the retired `OFF` / `MISSIONS` / `ALWAYS` row |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and record |

`app/src/web_platform.cpp` is unchanged, so the browser build still plays at full
volume. That build compiles neither `settings.cpp` nor the panel, has no settings
UI, and reads no `settings.cfg`, so there is nothing there to change yet.

## 2. The UI

The slider is `kVolumeCells` = 11 characters, because the request's three
examples are 11 characters. The handle sits on `volume_handle_cell(percent)` =
`(percent * 10 + 50) / 100`, so:

| percent | bar |
|---|---|
| 0 | `\|----------` |
| 50 | `-----|-----` |
| 65 | `-------\|---` |
| 100 | `----------\|` |

`volume_text` appends ` <n>%`, so the drawn row is `-------|--- 65%`. The bar is
one `TextLayer`, so the handle is visible whether or not the row is selected; the
click rects are one cell each, from the same fixed-cell metrics (`Font::kCellWidth`
= 6) the font draws with. A click on cell `n` sets `n * 100 / 10` percent; a click
anywhere else on the row is the ordinary row activation, which steps +5%.

`SOUNDS` reads `[x] ON [ ] OFF`, the order the request wrote. The enumerators are
in row order (`On = 0`, `Off = 1`) so the existing `RowKind` conventions hold:
option index = display order, and index 0 is the default, as on GAME SPEED
(`1`) and WIDESCREEN (`OFF`). The file stores the names, so the enum values are
not persisted.

## 3. Why a mute queues silence

`ultramodern::audio_dma_busy()` (`tools/N64ModernRuntime/ultramodern/src/audio.cpp:108`)
polls `get_frames_remaining()` to decide whether the N64 AI is still playing, and
the game's audio thread waits on it. A mute that returned early from
`queue_audio_samples` would report an empty queue on every buffer, so the AI would
never look busy and the game would generate audio as fast as it could. The
callback therefore always queues the same number of frames: the game's buffer
unchanged at gain 100, a scaled copy at 1..99, a zeroed copy at 0. The scaling
writes into a static scratch buffer and not in place, because `samples` points
into the game's RDRAM and the next AI buffer may already be live there.

## 4. Verification

Built with `cmake --build build-app -j 8` (the target used for the captures),
`cmake --build build-null -j 8` and `cmake --build build-dist --target
ogrebattle64` (what `make app` writes on this machine, `DIST_STATIC_SDL ?= 1`);
all three clean. The `build-dist` binary was also driven once through the
overlay (`Down,Down,Down,Left,Left` → `volume = 90`) to rule out the
stale-binary trap sessions 99, 102 and 104 record.

The panel, through the real `ESC` path (`OGRE_OVERLAY_AT_MS`, `OGRE_OVERLAY_TAB`,
`OGRE_OVERLAY_KEYS`, `OGRE_CAPTURE_OVERLAY`):

* SETTINGS tab, no keys: `SOUNDS  [x] ON [ ] OFF`, `VOLUME  ----------| 100%`
  (`docs/proofs/native-overlay-settings.png`). The launcher's SETTINGS tab draws
  the same rows (`docs/proofs/native-launcher-settings.png`).
* `Down,Down,Down,Left,Left,Left,Left,Left,Left,Left`: `/tmp/audio4/settings.cfg`
  reads `volume = 65`, and the capture's bar reads `-------|--- 65%` (counted on a
  3× crop of the PPM, because the handle's cell is not readable at 1×).
* `Down,Down,Space`: `/tmp/audio3/settings.cfg` reads `sounds = off`.
* A change writes the whole file, so the two new keys appear beside the old ones.

The loader, from `[settings]` lines:

* `sounds = off` / `volume = 40` → `sounds off, volume 40%`.
* `volume = 250` → `volume 100%`; `sounds = maybe` → `sounds on`.
* A file with neither key (`game_speed`/`widescreen` only) → `sounds on, volume
  100%`, which is the old behaviour.
* `OGRE_SOUNDS=off OGRE_VOLUME=30` over a file that says `on` / `100` logs both
  override lines and a final `sounds off, volume 30%`, and the file still reads
  `sounds = on` / `volume = 100` afterwards.
* `OGRE_VOLUME=200` logs `volume 100%`.

The scaling, with a temporary `probe106` (see §5) that printed, every 120 queued
buffers, the gain, the sample count, the peak of the game's samples, the peak
after the gain, and the device's queued byte count:

| run | gain | samples | peak_in | peak_out | queued (int16) |
|---|---|---|---|---|---|
| `OGRE_VOLUME=50` | 50 | 1104 | 20097 | 10048 | 7842 |
| `OGRE_VOLUME=50` | 50 | 1104 | 16245 | 8122 | 6794 |
| `OGRE_VOLUME=50` | 50 | 1104 | 3335 | 1667 | 8719 |
| `OGRE_VOLUME=0` | 0 | 1104 | 20846 | 0 | 7842 |
| `OGRE_VOLUME=0` | 0 | 1104 | 16222 | 0 | 6794 |
| `OGRE_VOLUME=0` | 0 | 1104 | 3420 | 0 | 8719 |

So gain 50 is half the amplitude, and gain 0 is silence that still queues 1104
samples per buffer and leaves a non-empty device queue, which is the pacing
claim. The `queued` figures repeat across the two runs because the game's
generation is paced by that same depth.

The boot battery, with the scaled path active (`OGRE_VOLUME=35 OGRE_SPEED=4
OGRE_EXIT_AFTER_MS=30000 ./build-dist/ogrebattle64`): app exit 0,
`tools/runlog.py --check` **PASS** — no crash, no stub call, no unknown module,
no bad RSP exit, no stubbed non-gfx task; 970 type-2 audio tasks all served by
the recompiled audio microcode, so the scaled queue did not stall the audio
thread.

## 5. Probes

One probe: `probe106`, in `queue_audio_samples` (`app/src/sdl_platform.cpp`). It
was a print-only block after the queue call and changed no behaviour. It is
removed; `grep -rl probe106 app/src RecompiledFuncs Bank*Funcs` prints nothing,
and the app was rebuilt after the removal. No generated code was touched.

`tools/RT64` shows as dirty in `git status` (` m tools/RT64`). That is the
submodule's pre-existing state (it carries the port's renderer changes); this
session changed no file in it.

## 6. Not verified

* A mouse click on a slider cell. The overlay's synthetic-input hooks only
  synthesize keys, so the click path (`hit_test` → `RowKind::RowOption` →
  `choose_option`) is reasoned from the existing radio rows, not exercised. The
  cell rects use the same metrics as the drawn bar.
* The browser build. It has no settings UI and no `settings.cfg`, so it plays at
  full volume; nothing was changed there.
* A human listening to 50%. The scaling is measured on the samples, which is the
  whole of the path between the game's buffer and `SDL_QueueAudio`.
