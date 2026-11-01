# Session 204 handoff — the browser's sound

## Goal

Continue the web port with sound: make the browser build play the game's audio,
and prove which part of the path was silent.

## Result

The browser plays the game's own audio, on by default. The game side already
worked: the recompiled audio microcode runs in the wasm build and the ring in
`app/src/web_platform.cpp` carries real PCM at the game's 32 kHz rate. Six
page-side faults kept it silent, stale or distorted:

1. **The stored-ROM path never created an AudioContext.** `initAudio()` had one
   caller, the ROM file picker's `file.arrayBuffer().then(...)` continuation
   (`app/web/web.js`). A returning session auto-starts from the IDBFS copy
   without a file picker, so no context, no worklet, no sound.
2. **The context could not be resumed by a gesture.** `initAudio()`'s first
   statement was `if (audioContext) return`, so a context the browser had started
   suspended stayed suspended. The picker call is a promise continuation, not a
   user activation, so it could start suspended too.
3. **SOUNDS defaulted to off**, from the session-22 comment that said the audio
   microcode was not emulated. Session 75 recompiled it, and the wasm build runs
   it (`[rsp] task type 2 submitted (audio microcode)`).
4. **The worklet's ring view used `ringPtr / 2` as its byte offset.** Both
   `ogre_audio_state_ptr()` and `ogre_audio_ring_ptr()` are byte offsets, and
   `Int16Array(sab, byteOffset, length)` takes bytes. The view therefore started
   at half the ring's address and read unrelated wasm memory.
5. **The worklet wrote int16 magnitudes into a float output.** An AudioWorklet
   output is in `[-1, 1]`; the samples were not divided by 32768, so the browser
   clipped values in the thousands. This is the original "wall of screeching",
   and it was in `audio-worklet.js`, not in the game.
6. **The worklet started at ring index 0**, so a context that attached after the
   game had queued audio read the oldest counter in the ring, which may already
   have been overwritten, and played up to 0.74 s of stale samples.

One units bug is fixed as well: `get_frames_remaining` returned stereo frames,
while `ultramodern/src/audio.cpp` and the SDL implementation use **int16
samples for both channels**. The AI cushion was therefore 104 ms instead of the
52 ms session 76 measured.

The page now creates the worklet on every boot path (`startBoot()`), from the
file picker, and from the first pointer or key event; `initAudio()` is
idempotent and retries until the wasm exports exist. The worklet adopts the
producer's newest frame on `init` and on `resync` (posted when a suspended
context resumes), reads the ring at its advertised byte address, and scales each
sample by `1/32768`. SOUNDS is on by default, matching native; `?audio=0` and the
overlay mute. `debug/probes/audio.cjs` measures both the ring and the worklet's
output.

## Evidence

### The game side was already producing audio

The title's ring, read by `debug/probes/audio.cjs`'s 25 ms watcher (a single
poll misses a 552-frame buffer, which the worklet drains in 17 ms):

```
ring: peak 4237, rms 1489.4, rate 32000 Hz, 2207 nonzero samples, max buffered 1977 frames
context: running at 48000 Hz, worklet connected, sounds on
output: peak 0.1173, rms 0.0042
muted:  peak 0.0000 (drain 6992 frames)
unmuted again: peak 0.2937
AUDIO OK
```

The samples are the game's music, not noise. The captured ring's zero-crossing
rate is 3492 Hz against the native SDL disk capture's 3120 Hz, and its RMS is
2026 pre-gain against native's 345 at VOLUME 15 (≈2300 pre-gain). Native was
captured with `SDL_AUDIODRIVER=disk` (`SDL_DISKAUDIOFILE=/tmp/native-1x.raw`), so
the comparison did not use the machine's output device.

A temporary `probe204` timed the wasm audio microcode at 225-320 µs per task
with 1-3 ms spikes, so the ucode is not why the game's audio thread generated
only about one buffer per second while the host load average was 16. That rate
follows the frame pump; the probe's watcher is what makes the measurement
independent of it.

### The wrong ring address and the missing scale

Before both fixes, the worklet's own levels read `peak 0.9997` on a run whose
ring peak was 3514, and the probe's first output window read `0.0000`: the
processor was draining the ring (`tail` advanced 23736 frames) but sampling
unrelated memory, and every sample it did find was clipped. After the fixes the
same measurement reads `0.1173` and `0.2937`, and `0.1173 * 32768 = 3843`, which
is the ring's peak in that window. `tail` still advances while muted (6992
frames), so the game's AI pacing is unchanged.

The two line changes are in `app/web/audio-worklet.js`:
`new Int16Array(d.sab, d.ringPtr, d.ringFrames * 2)` and
`(ring[base] + (ring[next] - ring[base]) * frac) * INT16_TO_FLOAT`. The
`INT16_TO_FLOAT` constant is `1 / 32768`.

### The page-side faults

- `app/web/web.js`: the old `initAudio()` returned when `audioContext` was set
  and was called only from the picker continuation; the SOUNDS default was
  `webAudioSettings.enabled = false`; the log line still said the microcode was
  not emulated.
- The new `initAudio()` (line 1794) creates the context once, calls `resume()`
  when suspended, retries every 500 ms until `Module._ogre_audio_state_ptr` and
  `_ogre_audio_ring_ptr` exist, and only then loads the worklet. `startBoot()`
  calls it at line 1333; `installAudioGesture()` (line 1905) calls it from the
  first `pointerdown`/`keydown` and resumes on `visibilitychange`;
  `onAudioStateChange()` (line 1775) posts `resync` when the context returns to
  `running`.
- `app/web/audio-worklet.js` `resync()` sets `tail = head` and `readPos = head`,
  and `init` calls it after the state view is installed. A suspended context
  therefore discards the backlog instead of playing it late.
- `app/src/web_platform.cpp`: `get_audio_samples_remaining()` returns
  `(head - tail) * 2`. `ogre_audio_frames_available()` still reports frames for
  the page log.

### The stored-ROM path

Reloading the page in the same browser context (the runtime's IDBFS copy of the
validated ROM is in IndexedDB) auto-starts without a file picker:

```
[web] Stored ROM found - starting the game with the previously loaded ROM.
[web] Boot thread started. Waiting for milestones...
[web:audio] AudioWorklet connected (48000 Hz context, state running), game audio ON
[web:audio] game queued its first audio buffer (552 frames)
```

Before this change there was no `[web:audio]` line at all on that path, because
`initAudio()` had no call site reachable without the picker.

### Focus playback and the autoplay gate

The follow-up ask was to play when the tab is focused instead of after a click on
the canvas. The browser is the limit here: an AudioContext that was created
without a user gesture stays suspended, and `resume()` outside a gesture does not
start it. A context created from the ROM picker's `change` event can start; the
stored-ROM auto-start path has no gesture at all.

The page now retries `resume()` on `window.focus`, `pageshow` and
`visibilitychange` (in addition to the first pointer or key anywhere), so a
player who has interacted with the page once gets audio back when they return to
the tab. When the browser still refuses, a `SOUND PAUSED · CLICK TO RESUME`
button appears at the top of the screen. It lives inside `.screen`, so it is part
of the fullscreen element, and its click is the gesture that starts the context.
It is hidden while the context runs and while SOUNDS is off.

Tested by suspending the live context from the page: the hint appears
(`SOUND PAUSED · CLICK TO RESUME`), a click at `(8,8)` outside the canvas
resumes it, a dispatched `focus` event resumes it, the hint's own click resumes
it, and the hint is hidden after each. With SOUNDS off the hint stays hidden even
while suspended. A fresh page with no interaction cannot be tested in headless
Chromium (it reports user activation on load), so the first-load case rests on
the policy rule and on the hint being visible at startup.

### Follow-up 2: the silent title, tutorial and mission

The next report was that the intro had sound but the title, tutorial and mission
screens did not. The audio path was not the cause. Two cursor faults in the
worklet stalled it after the first drain:

1. **The fractional frame was dropped every quantum.** `Atomics.add(state, 1,
   Math.floor(consumedFrames))` advanced `tail` by the whole part only, so `tail`
   fell ~0.33 frame behind per 128-sample quantum. `pendingFrames` now carries
   the remainder and `tail` advances by the whole part of the accumulated value.
2. **A 1e-12 disagreement between the float `readPos` and the integer `tail`
   was treated as a desync.** After a drain `readPos` could be
   `255.99999999999852` against `tail = 256`; the `readPos < tail` guard then
   set `readPos = head` (552), which skipped every frame still queued. `tail`
   stayed behind, `head - tail` looked like a backlog, `audio_dma_busy()` stayed
   true, and the game's audio thread stopped generating. The probe's own cursors
   caught it: `head 2208, tail 256, readPos 2208, processCount 23k/s`. The guard
   now clamps `readPos` to `tail`; only a real underrun (`readPos + 1 >= head`)
   moves it to `head`.

### Headed versus headless

The task rate is frame-paced: the game submits one audio task per RSP round
trip, and a gfx task holds the RSP thread for the frame. Headless Chromium
renders through SwiftShader, so its 3D frames take hundreds of milliseconds and
the title produces 2-3 audio buffers/s; that is a harness property, not the
page. A headed Chromium on this machine (ANGLE Metal, Intel UHD 630) produces
57.4 audio tasks/s, 27.8 display lists/s and 31 758 ring frames/s at the title.
The browser's title ring is continuous music: RMS 2991 and peak 16 234 over
10 s, against the native SDL disk capture's pre-gain RMS ~2873 at 1x. The forced
tutorial (scene 0x17) measured output peak 0.52 / RMS 0.081, and the mission
briefing reached from the escort save (scene 0x03) measured 0.767 / 0.109 over
5 s. The final title probe after cleanup read output peak 0.325 / RMS 0.098,
muted peak 0, 64 584 frames drained while muted, unmuted again 0.495.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final tree.
- `node --check app/web/web.js app/web/audio-worklet.js debug/probes/audio.cjs`.
- `node debug/probes/audio.cjs --secs 240 --out /tmp/audio-probe5`: `AUDIO OK`,
  numbers above. A second run (`/tmp/audio-probe6`) read ring peak 2073, output
  0.0989, muted 0 with 2576 frames drained and unmuted 0.2451, also `AUDIO OK`.
  A third run after the focus/hint change (`/tmp/audio-probe7`) read ring peak
  1362, output 0.0737, muted 0 with 5888 frames drained and unmuted 0.2838,
  also `AUDIO OK`. The ring PCM and the worklet output are measured separately; mute must read
  exactly 0 while `tail` still advances, and unmute must return nonzero output.
  The worklet accumulates the levels on the audio thread, so the measurement is
  not affected by headless main-thread timer throttling (the first probe version
  used an `AnalyserNode` and sampled 3 times in a second).
- `node debug/probes/scene.cjs --route title --attempts 2 --out /tmp/title-regress`:
  scene `0x0004` after 36.8 s, 886 display lists, `29.85 tasks/s`, PNG written.
  A second capture on the final page (`/tmp/title-hint`) reached scene `0x0004`
  and wrote its PNG at `20.30 tasks/s` under load. Both runs had sound on by
  default, so they also check that a probe that never clicks still boots and
  renders.
- Headed Chromium (ANGLE Metal), final code:
  `node debug/probes/audio.cjs --headed --secs 180` read ring peak 9474, RMS
  3276, output peak 0.325 / RMS 0.098, muted 0 with 64 584 frames drained, and
  unmuted 0.495. `--headed --query OGRE_SCENE=tutorial` read ring peak 10 981,
  output 0.52 / 0.081. The escort-save route to scene `0x0003` read output 0.767
  / 0.109 over 5 s. A 30 s title sample held 58 audio tasks/s with output peak
  0.31-0.69.
- The reload test above.

## Files changed

| file | change |
|---|---|
| `app/web/web.js` | `initAudio()` is idempotent, retries for the wasm exports, resumes a suspended context; called from `startBoot()`, the picker, the first pointer/key, `focus`, `pageshow` and `visibilitychange`; `onAudioStateChange` posts `resync` on resume and updates the sound hint; SOUNDS defaults on; the click-to-resume hint; `window.ogreAudio.node` and `levels()` exposed for the probe; the stale "screeches" text replaced |
| `app/web/index.html` | the `.sound-hint` style; the button is created inside `.screen`, so it survives fullscreen |
| `app/web/audio-worklet.js` | the ring view uses the byte offset (`ringPtr`, not `ringPtr / 2`); samples are scaled by `1/32768`; `pendingFrames` keeps the fractional frame so `tail` tracks consumption exactly; the `readPos < tail` guard clamps instead of snapping to `head`; `resync()` adopts `head` for `readPos`/`tail` and `init` calls it; output level counters answer `{type: "levels"}` with `head`/`tail`/`readPos`/`processCount`; the header documents all of it |
| `app/src/web_platform.cpp` | `get_audio_samples_remaining()` returns int16 samples (`* 2`), matching SDL and `audio.cpp`; callback table updated |
| `debug/probes/audio.cjs` | new: ring watcher (peak/RMS/rate over 25 ms samples), worklet `levels()` windows, mute and unmute checks, `--secs`/`--query`/`--out` |
| `debug/lib/harness.cjs` | `launch()` takes `browserArgs` so the audio probe can pass `--autoplay-policy=no-user-gesture-required` |
| `docs/guides/web-probes.md` | the audio check, its two measurements and its exit codes |
| `docs/guides/app-build.md` | browser SOUNDS/VOLUME now default on and why the gesture matters |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `AGENTS.md` | status, log, decision, index and the probe entry |
| `docs/HANDOFF-2026-10-30-session204.md` | this file |

The pre-existing uncommitted session-202/203 changes (`app/src/web_renderer.cpp`,
`app/src/main_web.cpp`, `app/CMakeLists.txt`, `docs/scenes.md`, `tools/RT64`
dirt) are untouched.

## Probes used and reverted

`probe204` measured `audio_ucode_guard`'s wall time in `app/src/rsp.cpp`
(`<chrono>` around the call, one line every 8 tasks). It is reverted with
`git checkout -- app/src/rsp.cpp`; `grep -rn probe204 app/ RecompiledFuncs/
Bank*Funcs/` is empty. No generated code was instrumented.

The kept diagnostics are `window.ogreAudio.node`, `window.ogreAudio.levels()`,
the `[web:audio] context <state>` line, and `debug/probes/audio.cjs`.

## Open

1. **Listening on a real output device.** The probe measures the worklet's
   output, not the speakers; the developer should confirm the title, a mission
   and a battle by ear. The two arithmetic faults above make this the remaining
   check.
2. **Audio above 1× speed.** The game generates one buffer per frame-paced AI
   step, while the worklet drains at the DAC rate; native has the same
   limitation (`PLAN.md`: "Audio above 1× speed is untested").
3. **A stored SOUNDS=OFF stays off** on the next visit, by design. `?audio=1`
   overrides it for a run; the probes turn it back on themselves.
4. **A backgrounded tab** may suspend the context; the ring fills, the producer
   drops buffers (one `[web:audio] ring full` warning), and `resync` discards the
   backlog when it resumes. The game keeps rendering throughout.
