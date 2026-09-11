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
node debug/probes/boot.cjs --attempts 10 --secs 45
node debug/probes/progress.cjs --attempts 6 --secs 75   # how far the intro gets
```

| Path | Purpose |
|---|---|
| `server.py` | threaded static server on `:8931` with the COOP/COEP headers |
| `lib/harness.cjs` | shared boot / retry / sampling / pixel / output helpers |
| `probes/*.cjs` | the probes (see the guide's table) |
| `out/` | captured artifacts (gitignored — never commit screenshots) |
Requirements: Node ≥ 18, `npm install` here, `build-wasm/` built, and your own
ROM at `assets/ogre64.z64` (or `--rom` / `OGRE_ROM`). The ROM is never committed.
