# Session 140 handoff — the browser build ships as a static package for GitHub Pages

> **Summary.** Session 140 handoff — **the browser build ships as one static directory (`make web-dist`), and a vendored service worker supplies the cross-origin-isolation headers a static host cannot send.** Developer request: continue the web build, with the intention of a static build servable from GitHub Pages. The pthread build needs `SharedArrayBuffer`, which browsers expose only on a cross-origin-isolated page, and GitHub Pages cannot set the two response headers that provide it, so a `dist-web/` served plainly could not run the game at all. `app/web/coi-serviceworker.js` is coi-serviceworker v0.1.7 (MIT) vendored verbatim under a header comment (a diff against upstream proves the body is byte-identical); it adds `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy` to every response it serves and reloads the page once, and it does nothing where the server already sends them, so `debug/server.py` still installs no service worker (`service worker supported, not controlling`). `make web-dist` runs `make web` and writes page + shell + `audio-worklet.js` + the Emscripten glue and wasm + a generated `ogre-web-config.js` with `window.OGRE_WASM_DIR = "./"` + `.nojekyll`, and fails when `build-wasm/` holds an `ogrebattle64.*` artifact the package lacks. `app/web/ogre-web-config.js` holds the repository layout's `"../../build-wasm/"`, so one page serves both layouts and the build patches no source file. `.github/workflows/pages.yml` builds and publishes the package with `upload-pages-artifact@v5` + `deploy-pages@v5`, guarded by `vars.OGRE_DATA_REPO`, and the repository setting it needs (Settings → Pages → Source = GitHub Actions) was made this session. New probe `debug/probes/isolate.cjs`. Measured on a plain `python3 -m http.server --directory dist-web` (no COOP/COEP): `isolated true (first seen at 811 ms)`, `service worker controlling`, `wasm ready true`, and `boot.cjs` `RENDERED` at `nonBlack 239425 / colorful 20076` (`sawRealDl=true`), the same `nonBlack` count as the same build behind `debug/server.py` (`239425 / 19688`; the 388 differing colourful pixels are the title's fade advancing between the runs) (`docs/proofs/web-dist-title.png`). A Pages-like subpath (`/ogre-battle-64-recomp/`, isolated at 650 ms) and a gzip-encoding server (627 ms) both instantiate the 21 MB wasm, which rules out the `Content-Encoding` mismatch a header-copying worker can introduce. The session's second finding: the glue publishes `Module._ogre_*` after `onRuntimeInitialized` fires (509 ms on the local package, 2015 ms on the CI-built deployed one, while `moduleReady()` is true at 0 ms), so a ROM handed over inside that window made `Module.ccall("ogre_start_boot", …)` throw `TypeError: func is not a function` and the game never started; `app/web/web.js` now waits with `whenWasmExportsReady()` and `debug/lib/harness.cjs`'s `gotoPage` waits for the same export. The package carries no ROM data. The workflow ran green (runs `38576916888`/`38576918066` for the fix push, `WebAssembly` `38576917934`), the deployed site isolates at 1131 ms, and `boot.cjs` on it reports `nonBlack 239425 / colorful 20076` — the same `nonBlack` count as the local package and `debug/server.py`, so all three servers draw the same frame. Open: only headless runs on the deployed URL, CI and local Emscripten versions differ, and the first load fetches the whole wasm before a ROM is picked. See `docs/WEB-PORT-DEPLOYMENT.md` and `docs/DECISIONS.md`.

## Goal

Continue the browser port with the intention of a static build servable from
GitHub Pages (developer, session start). The renderer work of sessions 138 and
139 had made the game run in the browser and had left three renderer gaps; none
of them blocked deployment. What blocked deployment was the delivery: the build
produced `build-wasm/` and the page expected to be served from the repository
root by `debug/server.py`, which sets `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy`, and a static host cannot set response headers.

## Result

1. **`app/web/coi-serviceworker.js` (new)** is coi-serviceworker v0.1.7 (MIT,
   Guido Zuidhof and contributors) copied verbatim under a header comment that
   records the version, the licence, the upstream URL and the reason the file is
   in the tree. It is a service worker: it adds `Cross-Origin-Opener-Policy:
   same-origin` and `Cross-Origin-Embedder-Policy` to every response it serves,
   and the page reloads itself once when the worker takes control. That is what
   makes a host without header support able to run the pthread build.
2. **`app/web/index.html`** loads it with a `<script src>` tag in the head,
   because the worker registers `document.currentScript.src`. The same block
   loads `ogre-web-config.js`. A new guard in the inline script states why
   nothing happens when `SharedArrayBuffer` is absent, instead of letting the
   glue abort with a bare exception.
3. **`app/web/ogre-web-config.js` (new)** holds `window.OGRE_WASM_DIR =
   "../../build-wasm/"`, the repository layout's path. `index.html` falls back to
   the same value. `make web-dist` generates a copy of that one file with `"./"`,
   so one page serves both layouts and the build patches no source file.
4. **`make web-dist` (Makefile, new target)** runs `make web` and writes
   `dist-web/`: `index.html`, `web.js`, `audio-worklet.js`,
   `coi-serviceworker.js`, `ogrebattle64.js`, `ogrebattle64.wasm`, a generated
   `ogre-web-config.js` and `.nojekyll`. It fails when a file it expects is
   missing, and when `build-wasm/` holds an `ogrebattle64.*` artifact the package
   does not carry.
5. **`.github/workflows/pages.yml` (new)** builds the package on a hosted runner
   and deploys `dist-web/` with `actions/upload-pages-artifact` +
   `actions/deploy-pages` on every push to `main` and on `workflow_dispatch`. It
   needs `vars.OGRE_DATA_REPO` + `secrets.OGRE_DATA_TOKEN` for the generated game
   code, the same as `.github/workflows/wasm.yml`, and the build job is skipped
   without them.
6. **`debug/probes/isolate.cjs` (new)** measures the deployment contract:
   `crossOriginIsolated`, whether `SharedArrayBuffer` exists, whether a service
   worker controls the page, and whether the shell reached `WASM runtime
   initialized`. Exit 0 means the page is isolated.
7. **The package has no ROM data.** The page reads the visitor's own dump
   through the File API, writes it to `/rom.z64`, and never uploads it.
8. **`app/web/web.js` waits for the wasm exports before it uses them.** The glue
   publishes `Module._ogre_*` a moment *after* `onRuntimeInitialized` fires, and
   handing the page a ROM as soon as the runtime reports in made
   `Module.ccall("ogre_start_boot", …)` throw `TypeError: func is not a
   function`, which the page reported as `[web] Failed to read the ROM file` and
   never recovered from. `whenWasmExportsReady()` gates `initWebGL()` and
   `startBoot()` at both call sites; `initAudio()` still runs immediately so the
   `AudioContext` stays inside the user gesture. `debug/lib/harness.cjs`'s
   `gotoPage` waits for the same export.

## Evidence

### 1. The service worker is byte-identical to upstream

```sh
curl -sSfL -o /tmp/coi-upstream.js \
  https://raw.githubusercontent.com/gzuidhof/coi-serviceworker/master/coi-serviceworker.js
tail -n +20 app/web/coi-serviceworker.js > /tmp/coi-vendored-body.js
diff -u /tmp/coi-upstream.js /tmp/coi-vendored-body.js   # no output
```

The first 19 lines of the vendored file are the project's comment and the
upstream `/*! coi-serviceworker v0.1.7 ... licensed under MIT */` banner.

### 2. A plain static server isolates the page and runs the game

The server sends no COOP/COEP and serves `.wasm` as `application/wasm`:

```sh
make web-dist
python3 -m http.server --bind 127.0.0.1 --directory dist-web 8940 &
curl -sS -D - -o /dev/null http://127.0.0.1:8940/ogrebattle64.wasm
#   Content-type: application/wasm
#   (no Cross-Origin-Opener-Policy, no Cross-Origin-Embedder-Policy)

node debug/probes/isolate.cjs --url http://127.0.0.1:8940/ --secs 40
# url        http://127.0.0.1:8940/
# isolated   true (first seen at 811 ms)
# SharedArrayBuffer true
# secure context true
# service worker controlling
# wasm ready true
# ISOLATED (SharedArrayBuffer available)

node debug/probes/boot.cjs --url http://127.0.0.1:8940/ --attempts 2 --secs 90 \
  --out web-dist-fixed
# attempt 1: sawRealDl=true rendered=true
# RENDERED {"w":640,"h":480,"total":307200,"nonBlack":239425,"colorful":20076,...}
```

The capture is `docs/proofs/web-dist-title.png`; it is the title screen fully
drawn before its fade. The first version of this run reported `nonBlack 24508` in
an attempt whose sample landed on the frame the first real display list produced;
the numbers above are the re-run after the export race in §6 was fixed.

### 3. A/B against the header-setting development server

Same wasm, repository layout, `python3 debug/server.py` (COOP/COEP, no service
worker):

```sh
node debug/probes/isolate.cjs --url http://127.0.0.1:8931/app/web/index.html
# isolated   true (first seen at 484 ms)
# service worker supported, not controlling      <- the script registered nothing

node debug/probes/boot.cjs --url http://127.0.0.1:8931/app/web/index.html \
  --attempts 2 --secs 90 --out web-headers-fixed
# attempt 1: sawRealDl=true rendered=true
# RENDERED {"w":640,"h":480,"total":307200,"nonBlack":239425,"colorful":19688,...}
```

The `nonBlack` count is identical to the package's run through the service
worker (`239425`), and `colorful` differs by 388 of 307200 pixels, which is the
title's fade advancing between the two runs. An earlier pair of runs on the same
two servers (`24840 / 15076` behind the headers, `24508 / 10160` through the
worker) sampled the frame the first real display list drew, and they are what
session 139 recorded as the title regression; the numbers above are the re-runs
after the export race in §6 was fixed.
`service worker supported, not controlling` is the check that the development
flow installs no worker.

### 4. A GitHub-Pages-like subpath works

A project site is served from `/<repo>/`, so the page and the worker both live one
directory down. The package was served that way with the same header-less server:

```sh
mkdir -p /tmp/pages-sim/ogre-battle-64-recomp
cp -R dist-web/. /tmp/pages-sim/ogre-battle-64-recomp/
python3 -m http.server --bind 127.0.0.1 --directory /tmp/pages-sim 8941 &
node debug/probes/isolate.cjs --url http://127.0.0.1:8941/ogre-battle-64-recomp/
# isolated true (first seen at 650 ms), service worker controlling, wasm ready true
```

### 5. gzip-encoded responses survive the worker

A service worker that copies response headers onto a new `Response` can leave a
stale `Content-Encoding` on a body the browser has already decompressed. GitHub
Pages compresses text, so this was measured with a throwaway server
(`/tmp/gzip-server.py`, not committed) that gzip-encodes `.html`, `.js` and
`.wasm`:

```sh
curl -sS -D - -o /dev/null http://127.0.0.1:8942/ogrebattle64.wasm
#   Content-Type: application/wasm
#   Content-Encoding: gzip
#   Content-Length: 2981147
node debug/probes/isolate.cjs --url http://127.0.0.1:8942/
# isolated true (first seen at 627 ms), service worker controlling, wasm ready true
```

`wasm ready true` means the 21 MB module was fetched, decompressed and
instantiated through the worker.


### 6. The wasm exports are published after `onRuntimeInitialized`

Polling `typeof Module._ogre_start_boot` every 500 ms from the runtime's own log
line, on a fresh browser profile:

```text
local dist-web (http://127.0.0.1:8940/)        0 ms start=undefined    509 ms start=function  keys=18
deployed Pages (https://lfarroco.github.io/…)  0 ms start=undefined   2015 ms start=function  keys=18
```

`moduleReady()` (FS + `HEAPU8`) is true at 0 ms, so the page's existing readiness
test passes well before the exports exist. Every one of them is guarded with
`typeof !== "function"` except the boot call, so handing the page a ROM at that
moment produced, on the deployed build, every time:

```text
[web] ROM written to the virtual filesystem. Starting runtime...
[web] Failed to read the ROM file: TypeError: func is not a function
```

and the game never started. The deployed page's `ogrebattle64.js` and `.wasm`
differ from the local ones (`126453`/`21051216` bytes against
`126161`/`21039450`), because the workflow builds with
`mymindstorm/setup-emsdk@v14 version: latest` and this machine has emcc
`6.0.8-git`; the export window is 0.5 s in the local build and 2.0 s in the CI
one. A visitor clicking the ROM row seconds after load never saw it, which is why
the deployed site works by hand; the auto-start from a stored ROM has no such
delay, so the fix is not cosmetic.

The fix is `whenWasmExportsReady()` in `app/web/web.js` (100 ms poll on
`typeof Module._ogre_start_boot === "function"` and
`typeof Module._ogre_gfx_create_context === "function"`), which gates
`initWebGL()` and `startBoot()` in the ROM picker and in the stored-ROM
auto-start. `initAudio()` stays before it, so the `AudioContext` is still created
inside the user gesture. `debug/probes/isolate.cjs` and `boot.cjs` were re-run
after the fix; `debug/lib/harness.cjs`'s `gotoPage` now waits for the export as
well, because it otherwise hands the ROM to the page inside the same window.

### 7. The deployed site renders the same frame as the local ones

Three servers, the same probe, `--attempts 2 --secs 90`, all `sawRealDl=true`:

| server | nonBlack | colorful |
|---|---|---|
| `python3 -m http.server --directory dist-web 8940` (service worker) | 239425 | 20076 |
| `python3 debug/server.py` (COOP/COEP, no worker) | 239425 | 19688 |
| `https://lfarroco.github.io/ogre-battle-64-recomp/` (deployed) | 239425 | 20076 |

The identical `nonBlack` count is the result: the deployed build, the local
package and the development server are the same picture, and the differing
`colorful` counts are the title's fade advancing between the runs.
`debug/probes/isolate.cjs` reports the deployed URL `isolated true (first seen at
1131 ms)`, `service worker controlling`, `wasm ready true`.

## What the package is

```text
dist-web/index.html             a copy of app/web/index.html, unmodified
dist-web/web.js                 input, audio, ROM picker, boot
dist-web/audio-worklet.js       the game's audio ring buffer
dist-web/coi-serviceworker.js   COOP/COEP for hosts that cannot send them
dist-web/ogre-web-config.js     window.OGRE_WASM_DIR = "./"
dist-web/ogrebattle64.js        Emscripten glue
dist-web/ogrebattle64.wasm      game + N64ModernRuntime, 21039450 bytes
dist-web/.nojekyll              for a branch-based Pages deploy
```

`make web-dist` measured `dist-web (20M)`. `dist-web/` is gitignored.

## Files changed

| file | change |
|---|---|
| `app/web/coi-serviceworker.js` | new; coi-serviceworker v0.1.7, MIT, vendored with a header comment |
| `app/web/ogre-web-config.js` | new; the repository layout's `OGRE_WASM_DIR` |
| `app/web/index.html` | loads the two new scripts; the `SharedArrayBuffer` guard |
| `app/web/web.js` | `whenWasmExportsReady()`; the ROM picker and the stored-ROM auto-start wait for the `_ogre_*` exports before `initWebGL()`/`startBoot()` |
| `debug/lib/harness.cjs` | `gotoPage` tolerates the service worker's one reload and waits for `Module._ogre_start_boot` |
| `Makefile` | `web-dist` target: copies the package, fails on a missing artifact, and normalises the file modes with `chmod -R a+rX` |
| `.gitignore` | `dist-web/` |
| `.github/workflows/pages.yml` | new; `make web-dist` then `actions/upload-pages-artifact@v5` (`include-hidden-files: true`) + `actions/deploy-pages@v5` |
| `debug/probes/isolate.cjs` | new probe |
| `debug/package.json` | `npm run isolate` |
| `docs/WEB-PORT-DEPLOYMENT.md` | rewritten for the package, the worker, and Pages |
| `docs/guides/app-build.md`, `docs/guides/web-probes.md` | `make web-dist`; the plain-server route and the new probe |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status, decision, log, index |
| `docs/proofs/web-dist-title.png` | the `dist-web` title capture |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Verification run

- `make web-dist` — the Emscripten build was already current; the target
  reconfigured, built, and wrote and checked `dist-web/`.
- `debug/probes/isolate.cjs` — the plain server, the plain server behind gzip,
  the Pages-like subpath, `debug/server.py`, and finally the deployed site
  (`https://lfarroco.github.io/ogre-battle-64-recomp/`, isolated at 1131 ms,
  `wasm ready true`).
- `debug/probes/boot.cjs` — the plain server (`sawRealDl=true`, `nonBlack
  239425 / colorful 20076`), `debug/server.py` (`239425 / 19688`) and the
  deployed site (`239425 / 20076`), three attempts of two, 90 s each.
- The Pages workflow ran twice for the second push (runs `38576916888` and
  `38576918066`, both success, `WebAssembly` `38576917934` success), and
  `curl` on the deployed files shows `application/wasm` with
  `Content-Encoding: gzip` and no COOP/COEP, which is the configuration §5
  covers.
- No native build was run. No renderer source changed, so the session-139
  proofs still hold.

## Probes

No probe was left in the tree. One throwaway server (`/tmp/gzip-server.py`) and
one throwaway directory (`/tmp/pages-sim/`) were used for the checks in §4 and
§5, and four throwaway Playwright scripts for §6; none is in the repository.
`debug/probes/isolate.cjs` is a diagnostic that stays.

## Open

1. **The deployed site has been exercised, but only headless.** The workflow ran
   green twice for the second push, `isolate.cjs` reports the deployed URL
   cross-origin isolated at 1131 ms, and `boot.cjs` on it renders the same frame
   as the two local servers (§7). A hand-played pass on the deployed URL has not
   been done, and the two Emscripten versions (CI `latest` against this machine's
   `6.0.8-git`) are a difference the local runs cannot cover.
2. **The first load fetches the whole 21 MB wasm before the visitor picks a
   ROM.** `index.html` loads the glue at page load, which is what every local
   probe assumes. Loading it when the ROM picker is used would cut a visit that
   never starts the game to a few tens of KB.
3. **Session 139's renderer items are unchanged**: the render target's height and
   the canvas size do not follow the VI mode, framebuffer-as-texture is missing,
   and `K4`/`K5` return 0.
4. **The browser build still assigns gamepads to N64 controllers 2-4** (`PLAN.md`
   open work 13). A player on the deployed page has no pad in slot 0.
