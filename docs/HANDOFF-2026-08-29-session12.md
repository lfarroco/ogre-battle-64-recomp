# Handoff — 2026-08-29 (session 12): Milestone 5 — real browser input, audio pipeline, save persistence

## TL;DR

- **Milestone 5 (browser input/audio/save) is implemented and verified in
  headless Chrome.** The web build now has real keyboard + gamepad input, an
  AudioWorklet audio pipeline reading the game's samples straight from the wasm
  SharedArrayBuffer, and IDBFS save persistence with stored-ROM auto-start on
  reload.
- **The browser-only `do_send FAILED queue=0x800E8B84 (full, 8/8)` spam is
  gone.** The root cause was exactly what session 11 suspected: the game's
  VI-driven main loop was spinning while waiting for a controller that was
  never reported as connected. With a real controller (keyboard slot 0), the
  game's main loop paces correctly at 60 Hz.
- **The game queues no audio at the title screen — on any platform.** Session 12
  confirmed the native null build behaves identically: the game never registers
  `OS_EVENT_AI` and its audio thread waits on a queue nothing sends to. The
  web audio *infrastructure* is complete and connected; game-side audio will
  flow once the game advances past the title.
- The title screen does not advance on Start/A/B in this state (native too).
  Figuring out why is the next debugging step (likely needs a renderer for
  visual feedback, or game-flow analysis).

## What was built (Milestone 5)

### Input (real keyboard + gamepad)

- `app/src/web_platform.cpp`: atomic per-controller state (`buttons`, stick,
  `connected`); exported `ogre_input_set(controller, buttons, x, y, connected)`
  (called from JS) + `ogre_input_debug()` (test helper). `get_input` /
  `get_connected_device_info` read the atomics on the game thread. Slot 0 is
  the keyboard (always connected, parity with native); slots 1–3 are gamepads.
- `app/web/web.js`: `keydown`/`keyup`/`blur` capture with the same key mapping
  as the native SDL build (X=A, Z=B, C=Z, Enter=Start, Q=L, E=R, arrows=D-pad,
  I/K/J/L=C buttons); `requestAnimationFrame` loop polls
  `navigator.getGamepads()` for slots 1–3 (standard mapping, parity with SDL).

### Audio (AudioWorklet pipeline)

- `app/src/web_platform.cpp`: 32k-frame int16 stereo ring buffer in wasm
  memory; `queue_samples` (drop-newest when full), `get_frames_remaining`
  (frames), `set_frequency` (logs rate changes; the game sets 32000 Hz).
  Exported `ogre_audio_state_ptr()` / `ogre_audio_ring_ptr()` (byte offsets)
  and `ogre_audio_frames_available()` (test helper). head/tail are absolute
  uint32 counters; ring index = `counter & (ring_frames - 1)`.
- `app/web/audio-worklet.js` (new): `OgreAudioProcessor` reads the ring from
  the SharedArrayBuffer via `Atomics`, resamples game rate → context rate with
  linear interpolation, advances `tail` itself, outputs silence + snap on
  underrun.
- `app/CMakeLists.txt`: **fixed 1 GiB heap** (`-sINITIAL_MEMORY=1073741824`,
  `ALLOW_MEMORY_GROWTH` removed) so the SAB never moves; new exports.
- `app/web/web.js`: creates `AudioContext` + worklet inside the ROM-picker
  user gesture (autoplay), posts `{sab, statePtr, ringPtr, ringFrames}` to the
  worklet.

### Save persistence (IDBFS)

- `app/CMakeLists.txt`: `-lidbfs.js`; `IDBFS` in EXPORTED_RUNTIME_METHODS.
- `app/web/web.js`: mounts IDBFS at `/ogre` (the runtime config dir), periodic
  `FS.syncfs` (10 s + beforeunload); if `/ogre/ogrebattle64-us-rev1.z64`
  exists after syncfs load, auto-starts the boot with the stored ROM.
- `app/src/main_web.cpp`: boot falls back to the previously validated stored
  ROM when no new `/rom.z64` file was picked (reload persistence).

### Shell / UX

- `app/web/index.html`: controls hint; status filter keeps the runtime's chatty
  `[sch]/[mq]/[ev]/[renderer]/[VI]/[drainer]` debug prints out of the page
  status (overridable via `window.OGRE_STATUS_FILTER` for debugging).

## Verification (headless Chrome, playwright; ROM `assets/ogre64.z64`)

```text
PASS boot reaches display-list submission
PASS keyboard input reaches wasm (mask=0x8800)
PASS Start (Enter) bit reaches wasm (mask=0x1000)
PASS do_send FAILED does not keep growing after boot (0 -> 0)
PASS controller reports connected
PASS AudioWorklet connected
PASS page still responsive after ~35 s
PASS no page errors
PASS reload auto-starts from persisted stored ROM (IDBFS)
(info) game audio flow = 0 at title (native identical; see below)
```

Runtime traces confirm the game's controller thread polls SI messages at 60 Hz
(`do_send OK queue=0x800E9B88 ...` every VI) and its main thread is VI-driven at
60 Hz.

## Why there is no game audio at the title (native identical)

- The game registers `OS_EVENT_SP/DP/SI` (events 4/9/5) but **never
  `OS_EVENT_AI` (6)** → the runtime never sends AI-complete messages
  (`events.cpp` line 254 gates on `events_context.ai.mq != NULLPTR`).
- The game's audio thread (thread 3) received one message at boot then blocks
  on a queue nothing sends to.
- `osAiSetFrequency` is called (32000 Hz) but `osAiSetNextBuffer` never is.
- The native null build shows the exact same event registrations and no audio.
  Conclusion: silent title is current game behavior in this boot state, not a
  web regression. Revisit once the game advances past the title.

## Open questions / next steps

1. **Title screen does not advance on Start/A/B** (web AND native). The game's
   input thread polls at 60 Hz and receives real button state, so input works;
   the game's title logic just doesn't progress. Needs visual feedback (a
   renderer) or game-flow analysis of the title loop (what state/condition the
   title waits on — possibly a save check, a timer, or an overlay condition).
2. **Graphics workload measurement + renderer decision** (WEB-PORT.md §15-16,
   §24) — the next plan milestone after M5.
3. Native null build segfaults at process exit (after clean renderer shutdown)
   — pre-existing, not web-related.

## Build notes

- Wasm build: `emcmake cmake -S app -B build-wasm` then `cmake --build
  build-wasm -j`. **The emscripten cache must be writable**: the default under
  `/usr/local/Cellar/emscripten/6.0.8/libexec/cache` is not writable in this
  sandbox; use `EM_CACHE=/tmp/emscripten-cache` (seed by copying the Cellar
  cache).
- Native builds (RT64 + null) are unaffected: none of the changed sources are
  in their target lists (web-only files).
- `tools/RT64` submodule remains uninitialized (not needed for the web build).
- Test scripts from this session live in `/tmp/ogre-web-test/`
  (`verify.cjs`, `probe-*.cjs`); they use the playwright install at
  `/Users/momo/dev/fatos/node_modules/playwright` and the cached chromium
  headless shell. Serve the repo root with COOP/COEP headers (see
  `docs/WEB-PORT-DEPLOYMENT.md`).
