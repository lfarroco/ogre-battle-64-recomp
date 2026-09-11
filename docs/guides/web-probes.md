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
page, so a plain static server is not enough:

```sh
python3 debug/server.py          # http://127.0.0.1:8931, COOP/COEP + no-store
python3 debug/server.py --port 9000 --bind 0.0.0.0 --verbose
```

`server.py` serves the repository root with `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy`, sends `application/wasm` for `.wasm`, and sets
`Cache-Control: no-store` so a rebuild only needs a page reload. It is threaded,
because the pthread workers fetch several files at once.

See [WEB-PORT-DEPLOYMENT.md](../WEB-PORT-DEPLOYMENT.md) for the same headers on
nginx and other hosts.

## Run a probe

```sh
node debug/probes/boot.cjs --attempts 10 --secs 45   # render check + pixel counts
node debug/probes/progress.cjs --attempts 6 --secs 75 # how far the intro gets
node debug/probes/shots.cjs --shots 8 --gap 1200     # timed screenshot series
node debug/probes/fps.cjs --window 10                # gfx frames/s
node debug/probes/vi.cjs --window 4000               # VI retrace/swap rate
node debug/probes/stats.cjs                          # dump renderer counters
node debug/probes/diag.cjs                           # hang characterisation
node debug/probes/logmode.cjs                        # page-log contract smoke test
node debug/probes/testdraw.cjs                       # synthetic-draw GL isolation
```

`npm run boot|progress|shots|fps|vi|stats|diag|logmode|testdraw` and
`npm run serve` work from inside `debug/`.

| Probe | Measures | Exit 0 means |
|---|---|---|
| `boot.cjs` | boot → real display list → canvas pixels (non-black, colorful, top colours) | the game rendered |
| `progress.cjs` | highest `tasks=` reached over a full time budget, plus any `GFX-ESCAPE`/`GFX-RUNAWAY` bad-walk line | the intro reached `--min-tasks` on some boot with no bad walk |
| `shots.cjs` | canvas PNG series + per-frame pixel counts | a series was captured |
| `fps.cjs` | `tasks=` delta over a window | a rate was measured |
| `vi.cjs` | `[vi-debug] retrace/swap`, `[snap]`, tasks/s | a rate was measured |
| `stats.cjs` | full `#gfxstats` / `#gfx-status` / milestone dump | (always; read the files) |
| `diag.cjs` | main-thread heartbeat + `Module.print` rate + wedge forensics | (always; read the files) |
| `progress.cjs` | `tasks=` high-water mark, `GFX-ESCAPE`/`GFX-RUNAWAY` | (see its own row above) |
| `logmode.cjs` | default one-line status vs `?log`, `ogreLog` API | the page-log contract holds |
| `testdraw.cjs` | `Module._ogre_gfx_test_draw()` on the canvas | the synthetic draw rendered |

### Reading the pixel counts

`#game-canvas` has a 1 px CSS border, and a canvas screenshot includes it. The
probes crop that border (`inset: 1`) before counting, so an **empty canvas
reports `nonBlack ≈ 32, colorful = 0`** while a drawn frame reports thousands.
`colorful` is the robust signal — the border is grayscale — which is why
`boot.cjs --min-pixels` (default 200) is applied to `nonBlack` and the
`colorful` count is printed alongside it for confirmation.

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
