# Handoff — 2026-09-11 — session 22: game audio is muted by default

## Outcome

The browser build no longer plays game audio unless asked. Loading a ROM used to
produce a wall of screeching; now the page is silent by default and says so in
the log:

```
[web:audio] AudioWorklet connected (48000 Hz context, state running), game audio MUTED - add ?audio to the URL to hear it
[stderr] [web:audio] game sample rate set to 32000 Hz
[stderr] [web:audio] game queued its first audio buffer (736 frames)
[web:audio] game is queueing audio (1005 frames buffered, muted)
```

Why the noise: the game's **audio microcode is not emulated**. The RSP audio
task is auto-completed (session 15, `app/src/rsp.cpp` is a stub), so whatever the
game hands the AI interface is not real audio - the host ring gets garbage and
the AudioWorklet dutifully resamples it. The fix for that is Phase 6 work; this
session only stops it from being audible.

Only JS changed (`app/web/`), so no wasm rebuild was needed.

## What changed

| File | Change |
|---|---|
| `app/web/web.js` | `urlFlag()` helper; `audioEnabled = urlFlag("audio") === true` (**false** by default); the init message carries `muted`; `window.ogreAudio.enabled()`/`setEnabled(bool)`; the "audio is flowing" watch now reports `muted` |
| `app/web/audio-worklet.js` | `this.muted` (default `true`, set from the init message, switchable via a `mute` message); a muted worklet runs the **entire** drain path and only zeroes the output |
| `app/web/index.html` | controls help now says audio is muted and how to enable it (`?audio`), next to `?log` |
| `docs/DECISIONS.md` | session-22 entry recording the decision and why the ring still drains |
| `PLAN.md` | browser status notes audio is muted by default |

## Two invariants this change preserves deliberately

The audio path is load-bearing for the *game's* flow - session 15 had to unblock
it (audio auto-response, PI-manager emulation, RSP DMA fixes) to get the boot
moving, and `get_frames_remaining()` backpressure is part of that. So:

1. the AudioWorklet is **still created, connected and draining** - only the
   samples written to the output are silenced;
2. the worklet's bookkeeping (`tail`, `readPos`, the underrun snap, the
   `consumedFrames` accounting) is untouched: mute zeroes the output *after* the
   normal loop, rather than skipping it.

An earlier version of the change short-circuited the loop and set `tail = head`
(consume everything immediately). That was dropped: it changes what the game
observes about the ring, which is the one thing this change must not do.

## Verified

- `logmode.cjs` (page contract): PASS.
- `stats.cjs` default boot: the MUTED line above, the game still queues audio
  (736 frames, 1005 buffered) and the run reached 28 display lists - i.e. the
  mute changed nothing about game progress.
- `stats.cjs --url '…/index.html?audio'`: `game audio ENABLED (?audio)`, so the
  opt-in path works. (Expect screeching there; that is the point of the flag.)

## Using it

```js
window.ogreAudio.enabled()         // false by default
window.ogreAudio.setEnabled(true)  // unmute a running page
```

`?audio` / `?audio=1` on the URL starts unmuted; `?audio=0` is explicit mute.
Both flags compose with `?log`.

## When audio comes back (Phase 6)

1. Run the game's audio microcode instead of the auto-response (RSPRecomp via
   `rsp_callbacks`), so `queue_audio_samples()` receives real PCM.
2. Check the sample-rate path: the game sets 32000 Hz and the worklet resamples
   to the AudioContext rate (48000 Hz here).
3. Then `?audio` becomes the way to listen while tuning, and the flag can be
   inverted (or removed) once the output is trustworthy.

## Open questions carried over

Unchanged from `docs/HANDOFF-2026-09-11-session21.md` (the idle trajectory is
still the gate: `vi.cjs` retraces at 59.2/s but the game swaps buffers at
0.25/s, and the `[snap]` evidence points at t5/t16 never being sent to).
