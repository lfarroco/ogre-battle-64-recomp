# `debug/` — headless-browser probes

Tools for driving the WebAssembly build in headless Chromium: boot it, feed it a
ROM, pump controller input, and measure frames, VI pacing, canvas pixels, and
hangs.

These used to be throwaway scripts in `/tmp/ogre-probe` that only ran on one
machine (they hard-coded a Playwright install and a ROM path). This directory is
the reproducible version.

**Full documentation: [docs/guides/web-probes.md](../docs/guides/web-probes.md).**

## Quick start

```sh
# 1. build the wasm (see docs/guides/app-build.md)
EM_CACHE=... cmake --build build-wasm -j

# 2. serve the repo root with COOP/COEP (required for SharedArrayBuffer)
python3 debug/server.py

# 3. install the probe dependency (once)
cd debug && npm install

# 4. run a probe
node debug/probes/scene.cjs --route tutorial-field --raw --points '12,160' --out /tmp/field
node debug/probes/boot.cjs --attempts 10 --secs 45
node debug/probes/progress.cjs --attempts 6 --secs 75   # how far the intro gets
node debug/probes/intro-capture.cjs --seconds 30        # 1 fps series of the opening
node debug/probes/intro-capture.cjs --seconds 8 --interval-ms 100  # dense series
```

`scene.cjs` is the one-command screen capture: a named route (title, mission
briefing, tutorial field, forced tutorial) boots the build, drives the menu and
dialogue, waits for the `[scene]` line, and writes a PNG per debug-flag value,
sampled pixels, the page log and the scene timeline. Add a route in its `ROUTES`
table instead of writing another throwaway script.

The opening is normally captured through `python3 tools/intro_capture.py`, which
runs this probe and its native counterpart and writes both series with a
per-frame pixel report. See
[app-build.md](../docs/guides/app-build.md#capturing-the-opening-native-and-in-the-browser).

| Path | Purpose |
|---|---|
| `server.py` | threaded static server on `:8931` with the COOP/COEP headers |
| `lib/harness.cjs` | shared boot / retry / sampling / pixel / output helpers |
| `probes/textures.cjs` | dump the decoded tile images + `colour x mask` composites |
| `probes/spritecheck.cjs` | render one sprite and compare it with its textures |
| `probes/intro-capture.cjs` | one canvas PNG per `--interval-ms` (default 1000) from the first scene, plus per-frame pixel counts |
| `probes/*.cjs` | the probes (see the guide's table) |
| `out/` | captured artifacts (gitignored — never commit screenshots) |

Requirements: Node ≥ 18, `npm install` here, `build-wasm/` built, and your own
ROM at `assets/ogre64.z64` (or `--rom` / `OGRE_ROM`). The ROM is never committed.
