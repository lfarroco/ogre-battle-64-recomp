# Headless-browser probes (`debug/`)

The web port is a WebAssembly build that drives a canvas and talks to the page
through `app/web/`. The probes in `debug/` boot that build in headless Chromium,
feed it a ROM, pump controller input, and measure what comes out — frame rate,
VI pacing, pixels on the canvas, renderer counters, and hangs.

They are the reproducible form of the throwaway scripts that used to live in
`/tmp/ogre-probe` (see the session handoffs), with the hard-coded paths and the
machine-specific Playwright install removed.

## Prerequisites

- The wasm build: `build-wasm/ogrebattle64.js` + `.wasm`
  (see [app-build.md](app-build.md); `EM_CACHE=... cmake --build build-wasm -j`).
- Node ≥ 18 and the probe dependency:

  ```sh
  cd debug && npm install      # Playwright (Chromium)
  ```

  `npm install` downloads Chromium on first use; set
  `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1` if it is already in
  `~/Library/Caches/ms-playwright`.
- Your own ROM at `assets/ogre64.z64` (or pass `--rom` / set `OGRE_ROM`).
  **Never commit the ROM** — the probes only reference its path.

## Serve the build

The wasm pthreads need `SharedArrayBuffer`, which needs a cross-origin-isolated
page, so a plain static server is not enough for the repository layout:

```sh
python3 debug/server.py          # http://127.0.0.1:8931, COOP/COEP + no-store
python3 debug/server.py --port 9000 --bind 0.0.0.0 --verbose
```

`server.py` serves the repository root with `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy`, sends `application/wasm` for `.wasm`, and sets
`Cache-Control: no-store` so a rebuild only needs a page reload. It is threaded,
because the pthread workers fetch several files at once.

The static package (`make web-dist`) works on a server that sends no COOP/COEP at
all, which is the deployed configuration:

```sh
make web-dist
python3 -m http.server --bind 127.0.0.1 --directory dist-web 8940
# http://127.0.0.1:8940/ , and the page is isolated about 0.8 s in
```

`app/web/coi-serviceworker.js` registers, reloads the page once and serves the two
headers itself. A probe reaches a URL on that server with `--url`; it must tolerate
the one self-reload. `debug/lib/harness.cjs`'s `gotoPage` does: it retries the
navigation when the first document reports that `SharedArrayBuffer` is missing,
and it treats the page as ready only once `Module._ogre_start_boot` is a function,
because the glue publishes the `_ogre_*` exports about 0.5 s (local package) to
2 s (deployed build) *after* the runtime's own `WASM runtime initialized` line.

See [WEB-PORT-DEPLOYMENT.md](../WEB-PORT-DEPLOYMENT.md) for the same headers on
nginx and other hosts, and for the package layout.

## Run a probe

```sh
node debug/probes/boot.cjs --attempts 10 --secs 45   # render check + pixel counts
node debug/probes/progress.cjs --attempts 6 --secs 75 # how far the intro gets
node debug/probes/shots.cjs --shots 8 --gap 1200     # timed screenshot series
node debug/probes/intro-capture.cjs --seconds 30     # 1 fps series of the opening
node debug/probes/intro-capture.cjs --seconds 8 --interval-ms 100  # dense series
node debug/probes/fps.cjs --window 10                # gfx frames/s
node debug/probes/vi.cjs --window 4000               # VI retrace/swap rate
node debug/probes/stats.cjs                          # dump renderer counters
node debug/probes/diag.cjs                           # hang characterisation
node debug/probes/logmode.cjs                        # page-log contract smoke test
node debug/probes/testdraw.cjs                       # synthetic-draw GL isolation
node debug/probes/testdraw.cjs --flags 32 --expect-fbtex  # framebuffer-as-texture control
node debug/probes/testdraw.cjs --flags 2048 --repeat 4 --gap 64 --expect-texcache  # tile-cache keep window and decode reuse
node debug/probes/testdraw.cjs --flags 6144 --repeat 3 --gap 64 --expect-sourcechange  # changed source must miss
node debug/probes/atmarkfast.cjs --arm 'id=0x0012' --disarm 'id=0x0003' --after 2  # per-flush canvas series
node debug/probes/fbtex.cjs --scenes 0x04,0x0b --secs 20  # framebuffer-as-texture sweep
node debug/probes/gamepad.cjs --secs 14               # a pad must reach N64 controller 1
node debug/probes/textures.cjs --attempts 3 --secs 60 # decoded tile images
node debug/probes/spritecheck.cjs --out sprite        # one sprite vs its textures
node debug/probes/rdram.cjs --query 'OGRE_RAM_KEY_AT_MS=52000&OGRE_RAM_KEY_FULL=1'  # guest RDRAM out of the wasm FS
node debug/probes/isolate.cjs --url URL              # cross-origin isolation
```

`npm run boot|progress|shots|fps|vi|stats|diag|logmode|testdraw|fbtex|gamepad|textures|spritecheck|isolate` and
`npm run serve` work from inside `debug/`.

| Probe | Measures | Exit 0 means |
|---|---|---|
| `boot.cjs` | boot → real display list → canvas pixels (non-black, colorful, top colours) | the game rendered |
| `progress.cjs` | highest `tasks=` reached over a full time budget, plus any `GFX-ESCAPE`/`GFX-RUNAWAY` bad-walk line | the intro reached `--min-tasks` on some boot with no bad walk |
| `shots.cjs` | canvas PNG series + per-frame pixel counts | a series was captured |
| `intro-capture.cjs` | one canvas PNG per `--interval-ms` (default 1000) from the first scene, with `nonBlack / colourful / white` per frame, the frame's scene id, and the scene timeline; retries an idle boot | a series was written (exit 2: the game never drew) |
| `fps.cjs` | `tasks=` delta over a window | a rate was measured |
| `vi.cjs` | `[vi-debug] retrace/swap`, `[snap]`, tasks/s | a rate was measured |
| `stats.cjs` | full `#gfxstats` / `#gfx-status` / milestone dump | (always; read the files) |
| `diag.cjs` | main-thread heartbeat + `Module.print` rate + wedge forensics | (always; read the files) |
| `progress.cjs` | `tasks=` high-water mark, `GFX-ESCAPE`/`GFX-RUNAWAY` | (see its own row above) |
| `logmode.cjs` | default one-line status vs `?log`, `ogreLog` API | the page-log contract holds |
| `testdraw.cjs` | `Module._ogre_gfx_test_draw()` on the canvas; `--flags N` sets `ogre_gfx_debug_flags` first, `--flags 32` adds the framebuffer-as-texture positive control, and `--flags 96` adds the column and row negative controls; `--repeat N` and `--gap MS` exercise the tile caches, and `--flags 4096` changes the source between calls | the synthetic draw rendered (and, with `--expect-fbtex`, a `[GFX-FBTEX]` line was logged; with `--expect-hits N`, exactly N hits were counted; with `--expect-texcache`/`--expect-sourcechange`, the decode and upload counters matched) |
| `atmarkfast.cjs` | the presented canvas at every `ogre_gfx_flush` (~60 Hz), armed and disarmed by `[scene]` lines, with `mean3`/`meanPC`/`fracW240`/`fracW250` per distinct frame and a sparse 0xFFFD count in the Load Game colour image; `--no-capture` records scene timing only | a series was captured and no frame was white (exit 3: a white frame was captured; exit 2: the arm marker never arrived) |
| `fbtex.cjs` | every `[GFX-FBTEX]` line and the `fbtex: hits=` counter over scene ids (`--scenes`) or arbitrary query strings (`--queries '...\|\|...'`) | (always; read the files) |
| `gamepad.cjs` | a synthetic pad's `A` reach into the wasm input slots (`Module._ogre_input_debug(slot)`) and the keyboard sharing slot 0 with it | the pad reached slot 0 (N64 controller 1) and the keyboard's `B` still registered |
| `textures.cjs` | the last 24 decoded tile images (RGB + alpha) and each `colour x mask` composite | the images decoded |
| `spritecheck.cjs` | one rendered sprite vs that composite, side by side | the rendered sprite matches its textures |
| `palette.cjs` | a forced scene's canvas plus the `[GFX-TEXTRACE]` lines (decoded colours, TLUT entries) | a frame bright enough by `--min-colorful` was kept |
| `fills.cjs` | a frame with only the RDP's fill rectangles drawn (`ogre_gfx_debug_flags(2)`), so a `G_FILLRECT` colour is read as pixels; pair it with the page log's `[GFX-RECT] fillrect` lines | a frame was captured |
| `rdram.cjs` | a guest-RDRAM image (`OGRE_RAM_KEY_AT_MS` + `OGRE_RAM_KEY_FULL=1`), read out of `Module.FS` and written next to the page log for `tools/rdram.py` | the dump appeared and held 8 MiB |
| `framedump.cjs` | the `OGRE_FRAME_DUMP=1` files (the picture band's source bytes, the TMEM the load left, the decoded image the draw samples) plus the page log and a canvas shot | the files appeared in the wasm filesystem |
| `stripab.cjs` | one screen, three frames: default, alpha blending off (`ogre_gfx_debug_flags(1)`), and combiner bypassed with TEXEL0 raw (flags 16) | three screenshots were written |
| `rectab.cjs` | the mission briefing's framed illustration, one shot per run, for the `OGRE_RECT_HALFPIX` A/B. The arrival signal is the `[GFX-FRAME-DUMP]` line the strip draw prints (`OGRE_FRAME_DUMP=1`), not a timer, because the browser's boot length varies with host load; the briefing is static, so two runs are the same screen. Run it once with the default and once with `&OGRE_RECT_HALFPIX=0` in `--query` (`--secs` 400 is enough on an idle host, ~100 s) | `FRAME DUMPS 2` and a `<prefix>-briefing.png` |
| `recttoggle.cjs` | one screen, three frames in one run with a live debug bit on, off and on again. The arrival default is the briefing's `[GFX-FRAME-DUMP]` line (`OGRE_FRAME_DUMP=1` is added), because a scene line crosses the briefing's NOTE-page change; `--flag N` selects the bit (32768 = `OGRE_RECT_HALFPIX` off, 8192 = the `OGRE_FBTEX` copy off) and `--name` prefixes the files. It launches Chromium with `--enable-gpu --use-angle=metal` (the SwiftShader default takes 30+ minutes to reach the mission; `--gpu 0` restores it). The route goes in `--url`; `--settle` is the wait after the marker | three screenshots were written (`<name>-on`, `-off`, `-on2`) |
| `isolate.cjs` | `crossOriginIsolated`, `SharedArrayBuffer`, whether a service worker controls the page, `WASM runtime initialized` | the page is isolated, so the pthread build can start |

### The framebuffer-as-texture check

The renderer keeps each `G_SETCIMG` colour image in a GL framebuffer object and
writes it back to RDRAM on the browser main thread, at present or at a
`sync_framebuffers` request. A `G_LOADTILE`/`G_LOADBLOCK` whose source bytes a
*recorded* draw has written and that has not been written back yet is therefore
decoded from RDRAM bytes the draw never put there. The gfx thread reports that as
`[GFX-FBTEX]` (once per distinct texture/image pair, up to 24), counts it in
`#gfxstats` as `fbtex: hits=N`, and nothing is drawn differently.

`fbtex.cjs` sweeps for it. Each entry gets its own boot, because `OGRE_SCENE` is
consumed before the boot enters its first scene:

```sh
node debug/probes/fbtex.cjs --scenes 0x00,0x04,0x0b --secs 18 --out /tmp/fbtex
node debug/probes/fbtex.cjs --queries 'OGRE_SCENE=new-game&OGRE_STEP=2&OGRE_SCENE_LOG=1' --secs 60
```

The same sweep also reports each run's `[GFX-SAMPLER]` census: textured draws
by `G_TF_*` filter and how many sample a masked (wrapping/mirroring) tile. That
is how the screens were checked for the sampler port (session 164): the forced
title and world map are point-sampled, the publisher still is point-sampled with
no masks, and the tutorial `0x17` is the reached bilinear screen.

A zero-hit sweep only means something if the check can fire, and
`testdraw.cjs --flags 32` is that control: debug flag bit 5 makes the synthetic
list render into a colour image at hidden RDRAM `0x00C02000` and then load a tile
back out of that same address. `--flags 96` adds bit 6, two loads that must not
be reported. The first is from texels 160..191 of the same image: the fill covers
columns 0..63 only, so that load is in the drawn rows and outside every drawn
column. The second is from a second colour image at `0x00C28000` whose fill
covers rows 100..163 only, and it reads rows 98..99, inside the fill's columns and
outside every row the fill drew. The old `height - 1 - wy` inversion recorded the
fill as rows 99..163, so that load was reported too.

```sh
node debug/probes/testdraw.cjs --flags 32 --expect-fbtex --out /tmp/fbtex-control
# -> fbtex hits=1 lines=1, plus the [GFX-FBTEX] line for tex=0xC02000
node debug/probes/testdraw.cjs --flags 96 --expect-hits 1 --out /tmp/fbtex-cols
# -> fbtex hits=1 lines=1; 2 with the column test disabled, and 2 with the row
#    inversion one row up
node debug/probes/testdraw.cjs --flags 0 --out /tmp/fbtex-off   # -> hits=0 lines=0
```

The check tests the drawn *rows* and *columns*, not only the byte range. A colour
image is sized to `max(viewport, VI height, scissor bottom + 1)` and the game
writes one row fewer than that, so the heap immediately past the image lies
inside its declared range; a range-only test reports those loads, and reading
RDRAM is correct for them. The columns are the same question one axis over: a
draw covers a rect, so a load in one of its rows but outside its columns reads
bytes the draw did not write, and the `[GFX-FBTEX]` line prints both ranges
(`cols=[..)` and `drawn cols=[..)`). The rows are exact: `note_fb_write` inverts
`ndc_yf` as `(1 - pos) / 2 * height` and treats a vertex as an edge, so a fill of
rows 100..163 is recorded as 100..163.

### The tile-cache controls

`testdraw.cjs --flags 2048` marks the synthetic list's uploads, and the
renderer prints one `[GFX-TEXPROBE]` line per key with `result=upload|reuse`
and `decode=hit|miss`. `--repeat 3` calls the synthetic list three times in one
flush batch and `--expect-texcache` requires one upload, two reuses, one decode
miss and two decode hits. `--gap 64` yields between calls so each list lands in
its own flush; the same expectation then tests the age-windowed GL texture
cache (an entry survives eight flushes) rather than the same-batch path.

`--flags 4096` (with bit 11, so `--flags 6144`) writes a different source texel
on every call. `--repeat 3 --gap 64 --expect-sourcechange` requires three
distinct content keys and three decode misses: a source key that omitted a byte
would serve the first image again and fail here.

The renderer's `#gfxstats` line reports `tex: cache=N bytes=N uploads=N
reused=N released=N decodes=N decode_hits=N`. Under `OGRE_TEX_TRACE=1`, every
decode-cache hit is re-decoded and compared, and a difference prints
`[GFX-TEXCACHE-MISMATCH]` (bounded to 8). A trace run of a natural screen must
report zero.

### The per-flush canvas series

Playwright's element screenshot costs 0.95-9.5 s with the game running
(session 176), so `atmark.cjs`'s nominal 250 ms series actually spans tens of
seconds and cannot see a frame that lasts one present. `atmarkfast.cjs` wraps
`Module._ogre_gfx_flush` inside the page, reads the default framebuffer with
`gl.readPixels` after each flush, and keeps one record per distinct frame. It
arms and disarms on the `[scene]` lines (`OGRE_SCENE_LOG=1` is required) and
writes `-frames.jsonl`, `-frames.txt`, `-result.json` and a PNG for a flagged
frame.

The white test is `fracW240 >= 0.90 && meanPC >= 240`, not the renderer's
`meanRGB` (which is a sum over non-black pixels). The colour matters: guest
RGBA5551 `0xFFFD` decodes to `(255,255,247)`, so a `min >= 248` test would miss
it. A flagged frame also gets `gl.getError()` and a sparse count of guest
halfwords equal to `0xFFFD` at the Load Game colour image
`0x228080..0x24D880`, read from the wasm heap at the base printed by
`[debug] wasm calloc(...) -> 0x...`.

```sh
node debug/probes/atmarkfast.cjs --pre 0 --timeout 120 \
  --arm 'id=0x0012' --disarm 'id=0x0003' --after 2 --out /tmp/loadgame \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'
```

Exit 3 means a white frame was captured; the flagged record's `meanPC`, mode
colour and `rdramWhite` then separate a guest white plate (`(255,255,247)`,
`rdramWhite > 0`) from the renderer's missing-texture fallback
(`(255,255,255)`, `rdramWhite = 0`).

### The browser gamepad check

N64 controller 1 is slot 0, and it is the only controller this game reads. A
connected pad takes the lowest free slot and the keyboard is OR'd into slot 0
(`app/web/web.js` `updateGamepads`, the rule `app/src/sdl_platform.cpp` uses), so
one pad lands in controller 1 and does not take the keyboard away from it.

A headless browser exposes no real pad, so `gamepad.cjs` installs a synthetic one
with `page.addInitScript` before the page scripts run:
`navigator.getGamepads()` returns one connected `standard`-mapping pad holding
`A`. Nothing sends input events; the pad exists only in the array the shell polls.
The probe then reads `Module._ogre_input_debug(slot)` for all four slots every
second.

```sh
node debug/probes/gamepad.cjs --secs 14 --out /tmp/gamepad
# -> slot0=0x8000 (the pad's A), slots 1..3 0xFFFF (no device)
#    [web:input] gamepad connected (pad 0 -> slot 0, slot 1 free, slot 2 free, slot 3 free)
#    keyboard B while the pad holds A: slot0=0xc000, after release=0x8000
```

Exit 0 requires both: slot 0 carried `A` at some sample, and the slot-0 mask was
`0xC000` (`A` from the pad, `B` from `KeyZ`) while the probe held the key. The
second half is what catches the regression this probe was written for: with the
pad pass writing slot 0 and the keyboard pass writing it before it, a pad made
the keyboard dead. `Module._ogre_input_debug(slot)` replies `0xFFFF` for a slot
with no connected device and the button mask otherwise.

### Reading the pixel counts

`#game-canvas` has a 1 px CSS border, and a canvas screenshot includes it. The
probes crop that border (`inset: 1`) before counting, so an **empty canvas
reports `nonBlack ≈ 32, colorful = 0`** while a drawn frame reports thousands.
`colorful` is the robust signal — the border is grayscale — which is why
`boot.cjs --min-pixels` (default 200) is applied to `nonBlack` and the
`colorful` count is printed alongside it for confirmation.

The screenshot's inner size is the element's CSS box, which the renderer sets to
an integer multiple of the drawing buffer (`[GFX-PRESENT] canvas resized to
639x475 (css 639x475, x1)`): the drawing buffer is the VI's visible area, so a
319x239 visible area is captured 2x (638x478) and a 639-wide one 1:1. Every pixel
of the buffer is written by the present blit (session 160), so `inset: 1` crops
only the element's border. `tools/web_frame_cmp.py` turns a screenshot back into drawing-buffer
pixels for a comparison against a native `OGRE_CAPTURE_TARGET` capture
(`docs/guides/app-build.md` -> "Comparing a browser frame with a native
capture").

### Configuration

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--url` | `OGRE_URL` | `http://127.0.0.1:8931/app/web/index.html` | page to load |
| `--rom` | `OGRE_ROM` | `<repo>/assets/ogre64.z64` | ROM handed to `#rom-input` |
| `--out NAME` | — | the probe's name (`boot`, `shots`, …) | output prefix; an absolute path is used as-is |
| `--out-dir DIR` | `OGRE_OUT` | `<repo>/debug/out` | where relative output prefixes are written |
| `--attempts` | — | 5 | boot retries (see the bifurcation below) |
| `--secs` | — | 60 | per-attempt time budget |
| `--tap-ms` | — | 5000 | Start-tap interval while advancing the title |
| `--min-tasks` | — | 2 | `tasks=` that counts as a real display list |
| `--headed` | — | off | run Chromium with a window (debugging) |

Each probe also writes `<prefix>-result.json` with its machine-readable verdict
(`diag.cjs` writes a live trace instead — it characterises a run rather than
scoring it).

## Why every probe retries the boot

Roughly **one in two boots lands on an idle trajectory**: it stalls at about
three display lists and never submits the game's own list. The workload analyzer
prints `tasks=N` in `#gfxstats`, where the boot task is 1, so `tasks >= 2` means
a real display list arrived. `harness.attemptLoop()` re-runs the whole boot until
that happens; without it, a probe reports a false negative about half the time.
This is the single most important behaviour the probes inherited from the
`/tmp` scripts.

## Boot from a battery save — the `OGRE_SAVE` page knob

`?OGRE_SAVE=<url>` fetches a 32 KiB port battery image and installs it as this
page's SRAM battery before the runtime reads it (session 168). The page writes
`/ogre/saves/ogrebattle64-us-rev1.bin` — the path `pi.cpp`'s `init_saving` ->
`read_save_file` reads (`librecomp/src/pi.cpp:288/414/435-440`) — from the
IDBFS mount, `FS.syncfs(false)`s it, and holds `ogre_start_boot()` until the
import settles. Both boot paths wait: the stored-ROM auto-start and a ROM picked
with `#rom-input`.

```sh
# assets/escort_suspend.bin is served at /assets/escort_suspend.bin, and the page
# is served from /app/web/, so a relative URL resolves from the server root.
node debug/probes/shots.cjs --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE_LOG=1' --secs 120 --shots 4 --gap 3000 --out /tmp/web-save
```

The page log then holds:

```
[web:save] OGRE_SAVE=assets/escort_suspend.bin -> /ogre/saves/ogrebattle64-us-rev1.bin (imported after the /ogre mount, before the boot)
[web:save] persistent storage mounted at /ogre
[web:save] OGRE_SAVE=assets/escort_suspend.bin: fetching /assets/escort_suspend.bin
[web:save] imported assets/escort_suspend.bin (32768 bytes) -> /ogre/saves/ogrebattle64-us-rev1.bin
...
[save] loaded /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)
```

Notes:

- The import runs on every page load and overwrites the persisted battery, so a
  run always starts from the named image. Without the knob, a battery the game
  wrote earlier stays in IndexedDB and is loaded instead.
- A failed fetch logs `[web:save] OGRE_SAVE=<url>: import failed: ... - booting
  without it` and the boot proceeds with a blank battery.
- `#save-input` on the page ("Load save") writes a chosen 32 KiB image through the
  same code path and reloads the page, which then boots from it without a query
  string.

`OGRE_TAP_SCENE_BUTTON` is the scene-keyed tap schedule on the page:
`<scene>:<buttons>[:<count>],...` with the `OGRE_SCENE` names (`title`, `intro`,
`publishers`, `story`, `unit-info`, `tutorial`, `menu`, `new-game`) or a hex id,
`+`-joined buttons, and unlimited presses when the count is absent. It replaces
`OGRE_TAP_BUTTON` while set and presses once per `OGRE_TAP_MS` while the named
scene is active; the scene id comes from the page's own `[scene]` lines, so the
route does not depend on how long the boot or a scene took. The mission save's
route, which reaches scene `0x03`:

```sh
?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6
```

`OGRE_TAP_SCENE` and `OGRE_TAP_NOT_SCENE` are still native-only.

## The page contract

The probes read the page, so these are an interface: changing them breaks every
probe (and `logmode.cjs` is there to catch it).

- `#rom-input` — the ROM file input.
- `#gfxstats`, `#gfx-status` — renderer counters; `tasks=N` is the frame counter.
- `#game-canvas` — what `shots.cjs` / `boot.cjs` screenshot.
- `#status` — one line by default; gets the `log-mode` class under `?log`.
- `window.ogreLog` — the bounded (20 000 line) runtime log, with
  `length()`, `head(n)`, `tail(n)`, `text()`, `find(re)`, `contains(s)`,
  `save()`, `clear()`, `show(bool)`. Probes read this instead of the DOM; the
  page does no per-line DOM work (session 20: that was throttling the emulator).
- Optional wasm exports: `Module._ogre_set_trace_enabled(0|1)` (`vi.cjs`),
  `Module._ogre_gfx_test_draw()` (`testdraw.cjs`).
- `window.ogreAudio` — `enabled()` / `setEnabled(bool)`. **Game audio is muted
  by default** (the audio microcode is not emulated, so the samples screech);
  the worklet still drains the ring so the game sees the same backpressure.
  `?audio` on the URL starts unmuted. A default boot logs
  `game audio MUTED - add ?audio to the URL to hear it`, which is what a probe
  should assert if it ever needs to care.

## Output

Artifacts go to `debug/out/` (gitignored) as `<name>-<suffix>`:

| Suffix | Written by | Contents |
|---|---|---|
| `canvas.png`, `page.png` | boot, shots, testdraw | canvas / whole-page screenshots |
| `-NN.png` | shots | numbered frames |
| `gfxstats.txt` | boot, shots, fps, vi, stats | counters + status |
| `milestones.txt` | boot, shots, fps, vi, stats | capped `GFX-`/`RSP`/renderer lines |
| `live.txt` | boot, stats, diag | per-second samples |
| `logtail.txt` | stats | recent `ogreLog` tail |
| `errors.txt` | all | page errors and console errors |
| `result.json` | most | machine-readable verdict |

Never commit `debug/out/` contents: screenshots of a copyrighted game are as
uncommittable as the ROM.

## Baseline (session 21)

| Measurement | Value |
|---|---|
| `boot.cjs` | `RENDERED`, ring of twelve sprites, 0/24 triangles rejected, `nonBlack≈7 990`, `colorful≈5 850` |
| black canvas (the idle trajectory) | `nonBlack≈32`, `colorful=0` |
| `progress.cjs` | ~39 display lists in 75 s with no bad walk (session 20 escaped at ~31); `--min-tasks 35` (session 20 escaped at ~31) is the current bar |
| `shots.cjs` | canvas byte-identical across frames 1.5 s apart (deterministic frame) |
| `fps.cjs` | 2.5–3.5 gfx frames/s |
| `vi.cjs` | 59.2 retrace/s (target 60), **0.25 buffer swaps/s** - the VI thread is fine, the game thread is standing still |

`fps.cjs`/`vi.cjs` both need a boot that leaves the idle trajectory (session
21: `fps.cjs` missed on 4/4 attempts, `vi.cjs` still measured the rates because
they are host-side).

Session-20 baseline, for comparison: `nonBlack≈6 100`, `colorful≈5 000` (the
sprite combiner was being evaluated with the wrong cycle type then).

### Which probe to reach for

- **Did it render?** `boot.cjs` (stops at the first rendered frame).
- **How far does it get?** `progress.cjs` — `boot.cjs` stops early, and
  `stats.cjs` boots **once** (it is a counter dump, not an attempt loop), so a
  low `tasks=` there can just be the idle trajectory.
- **What is the renderer's state?** `stats.cjs` + `ogreLog` (`[GFX-TASK]`,
  `[GFX-CMD]`, `[GFX-ESCAPE]`).

## Adding a probe

Put it in `debug/probes/` and build it from `debug/lib/harness.cjs`:
`boot()`, `runAttempt()`, `attemptLoop()`, `readState()`, `MilestoneCollector`,
`pngAnalyzer()`/`analyzePng()`, and the `write*()` helpers. Parse flags with
`parseArgs()` + `resolveConfig()`, take the output prefix from `outPrefix()`, and
exit `0` on success and `2` on a measured failure so probes compose in a shell.

The `diag`, `freeze`, `locate` and `tap`-variant scripts from the `/tmp` era are
deliberately not carried over: they were one-off hunts for bugs whose findings
are in `docs/HANDOFF-*` and `docs/DECISIONS.md`. Rebuild them from the harness if
the bugs ever come back.

## Troubleshooting

- **`Cannot create SharedArrayBuffer` / pthread init failure** — the page is not
  cross-origin isolated; serve it with `debug/server.py`, not `python3 -m http.server`.
- **Every attempt reports the idle trajectory** — check `build-wasm/` is current,
  the ROM path is right, and the server root is the repo root.
- **`window.ogreLog` is undefined** — `app/web/index.html`/`web.js` is from an
  older build (before session 20's log ring buffer).
- **Chromium will not start** — run `cd debug && npx playwright install chromium`.
