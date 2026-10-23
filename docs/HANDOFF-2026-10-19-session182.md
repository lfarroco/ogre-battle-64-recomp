# Session 182 handoff — one screenshot per second of the opening, native and in the browser

## Goal

Developer request: *"build a tool that capture 1 screenshot every 1s, in the
first 30s of this game (1x speed). that should help the work to fix the intro
quest logo."* Ten sessions had lost time to captures that were black,
stretched, or taken at the wrong moment, so the tool had to measure its own
output rather than just produce files.

## Result

New `tools/intro_capture.py` writes 30 PNGs, one per second, from the game's
first scene, in both renderers, with a contact sheet and a per-frame pixel
report. `t = 0` is the first frame of the first scene, not the runtime's first
(blank) present.

```sh
python3 tools/intro_capture.py              # both, 30 s, 1 fps
python3 tools/intro_capture.py --mode native
python3 tools/intro_capture.py --mode web
```

Output: `debug/out/intro/native/frame-NN.png`, `debug/out/intro/web/frame-NN.png`,
`contact-native.png`, `contact-web.png`, `report.txt`, one `manifest.json` per
mode. The directory is under `debug/out/`, which is gitignored.

Measured at `OGRE_SPEED=1` on this machine:

| mode | frames | largest gap | QUEST logo |
|---|---|---|---|
| native (`build-app/ogrebattle64`) | 30/30 | 1.04-1.27 s | frames 23-24, `white=17519 colourful=1281` |
| browser (GPU, `build-wasm`) | 30/30 | 1.03 s | frames 24-25, `white=17489 colourful=1309` |

Both series include the whole opening: the soldier intro (`0x09`), the
publisher stills (`0x0A`: Nintendo 64, Licensed by Nintendo, ATLUS), the QUEST
logo, and the title.

**The browser presents the reference QUEST frame at this sampling.** Frames 24
and 25 of two separate runs hold the white plate, the three coloured q's and the
wordmark. Session 181 measured `coloured=0` on the presents it sampled; this is
not a contradiction of that per-present reading, it means the failing presents
are not every present. The handoff's own frames are transient (gitignored), and
the check is one command.

### 1. A present count is not a clock (the native cadence)

The first version used `OGRE_CAPTURE_EVERY=30`, which is 1 s at the 30 presents/s
the 320x240 scenes run at. A 30 s run measured gaps of `1.00`, `1.02` and `1.03` s
up to frame 24, then `0.60` s for the title frames after the 320x240 -> 320x476
switch: RT64 presents per VI field in the 320x476 mode and per frame in the
320x240 one, so one present count is 2 Hz there.

`tools/RT64/src/hle/rt64_present_queue.cpp` gained
`OGRE_CAPTURE_EVERY_MS=<n>`: capture the first present, then one present every
`n` ms of wall time. It sits beside `OGRE_CAPTURE_EVERY` and the present-count
path is unchanged (`every == 0` still captures every present). With
`OGRE_SPEED=1` the wall clock is the game clock, so `1000` is one second. The
maximum gap over 30 s then measured 1.04 s and 1.27 s in two runs (the 1.27 s
gap is a VI-mode transition, not accumulated drift: the rest are 1.00-1.03 s).

### 2. The anchor is the first scene, not the first present

The browser presents black frames for 2-4 s between `(present 1)` and its first
scene, and the software rasterizer for about 23 s, so anchoring the window at
the first present spent it on the runtime coming up. Both modes now start at the
first `[scene]` line whose id is neither `0x0000` (the boot placeholder) nor
`0x19E8` (the dispatcher's uninitialised read). Native frame 0 is the first
frame of `0x09`; browser frame 0 is the first frame of `0x09`.

### 3. The browser half

`debug/probes/intro-capture.cjs` is a probe in the `debug/probes` set. It boots
the wasm build through `debug/lib/harness.cjs`, waits for the first real scene,
and then captures in-page: `Module._ogre_gfx_flush()` followed by
`canvas.toDataURL('image/png')`, one frame per second on an absolute schedule. A
Playwright element screenshot cost 12.9 s per grab under load (session 176) and
cannot sample 1 Hz.

- Per frame it sends the PNG and a 160x120 RGBA sample through a
  `page.exposeBinding`; the Python side builds the contact sheet from the sample
  and drops it from the manifest.
- `nonBlack / colourful / white` use the same thresholds as the native side
  (`>8/255`, `max-min > 24/255`, all channels `> 200/255`), so the two reports
  are comparable.
- A frame whose deadline the main thread missed is listed in `missed`; the rest
  of the series keeps its absolute schedule.
- An idle boot (all frames black) is retried, up to `--attempts` (default 3).
- It launches Chromium with `--enable-gpu --use-angle=metal
  --ignore-gpu-blocklist`. Verified with `WEBGL_debug_renderer_info`: the
  headless shell reports `ANGLE (Intel, ANGLE Metal Renderer: Intel(R) UHD
  Graphics 630)`. Without those flags (SwiftShader) one 40 s run presented its
  first non-black frame at 22.8 s and entered `0x09` at game time 27.6 s.
- The tool starts `debug/server.py` when nothing answers the URL, and stops it
  afterwards.

### 4. The tool does not create the QUEST frame (A/B)

The probe calls `_ogre_gfx_flush()` before each capture. That is the same call
the page's 16 ms timer makes, but the series is a measurement of the renderer,
so it was checked: with the flush disabled the same frames 24-25 still carry
`white=17489 colourful=1309`. Nothing about the series depends on it.

### 5. What this says about the QUEST fix

The reference frame presents in the browser at 1 fps in two runs, on the current
tree (session 179's uncommitted `combiner_uses_texture` change is in it). So the
open item is the presents session 181 sampled, and the tool is the instrument
for finding which ones fail: the per-frame `white/colourful` pair separates the
three states (black plate + q's: `white=0, colourful>0`; white plate + q's +
wordmark: `white>17000, colourful>1000`; plate and wordmark only: `white>17000,
colourful=0`).

The web title frames that follow the stills are garbage stripes
(`contact-web.png` frames 26-29), which is the browser title fault recorded in
`PLAN.md` open item 8 and not this session's subject.

## Verification

- `python3 tools/intro_capture.py` (both modes): native 30/30, browser 30/30,
  `report.txt` printed above.
- Frames inspected: `native/frame-23.png` and `web/frame-24.png` are the
  reference QUEST logo; `contact-native.png` and `contact-web.png` hold the whole
  sequence.
- The A/B in §4: one browser run with the per-capture flush removed produced the
  same QUEST frames; the flush was restored.
- `make patch-check` PASS after regenerating `patches/rt64-ob64.patch`
  (`src/hle/rt64_present_queue.cpp` is the only file that changed, +44/-13 in the
  patch).
- `cmake --build build-app --target ogrebattle64 -j` clean (RT64 relinked).
- No probe remains: the only instrumentation added was a temporary `dbg` field
  in `debug/probes/intro-capture.cjs` while the scene lookup was wrong, and it
  was removed; `grep -n "dbg" debug/probes/intro-capture.cjs` is empty.
- Not run: `make web`. No file in `app/CMakeLists.txt`'s Emscripten list,
  `app/web/`, `app/src/web_renderer.cpp`, `web_platform.cpp` or `gbi.cpp`
  changed, and `app/CMakeLists.txt:107` sets `OGRE_USE_RT64 OFF` under
  Emscripten, so the RT64 knob is not in the wasm build.

## Files changed

| file | change |
|---|---|
| `tools/intro_capture.py` | new: native + browser capture, conversion, pixel report, contact sheet, server management |
| `debug/probes/intro-capture.cjs` | new: the browser half |
| `tools/RT64/src/hle/rt64_present_queue.cpp` | `OGRE_CAPTURE_EVERY_MS`, wall-clock capture interval (submodule; uncommitted, as the rest of the port's RT64 changes) |
| `patches/rt64-ob64.patch` | regenerated for that change |
| `docs/guides/app-build.md` | the knob row and the "Capturing the opening" section |
| `docs/guides/web-probes.md` | the probe in the run list and the table |
| `debug/README.md` | the probe row and the tool pointer |
| `PLAN.md` | session status bullet; open item 19 notes the 1 fps result |
| `docs/DECISIONS.md` | the anchor and wall-clock cadence decision |
| `docs/STATUS-LOG.md` | session entry |
| `docs/README.md` | handoff index row |
| `docs/scenes.md` | the QUEST still's row |
| `docs/HANDOFF-2026-10-19-session182.md` | this file |

No recompiled code was touched. `app/src/web_renderer.cpp` keeps session 179's
uncommitted change, which was already in the tree at session start.
