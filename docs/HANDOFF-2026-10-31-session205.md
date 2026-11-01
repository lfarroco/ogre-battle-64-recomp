# Session 205 handoff — the browser's mission audio is frame-paced

## Goal

Find why the browser build's sound works but is laggy on the mission screen
(the tutorial mission is one example), and check the native audio implementation
for the intended pacing.

## Result

The AudioWorklet is not the cause. The game's audio thread generates one
552-frame buffer per RSP completion, and a completion is delivered only while a
game thread is blocked or yielding. The mission's ~30 fps frame rate lowered the
buffer rate below the 58 buffers/s the 32 kHz DAC consumes; the music engine
advances per generated buffer, so the music dragged and the worklet wrote the
shortfall as silence.

The fix is a browser-only scheduling change in the runtime: `M_AUDTASK` runs on
the thread that submits it, under a mutex around the shared RSP DMEM image. The
completion is then queued before the game's `osRecvMesg`, so the audio thread
refills the 52 ms AI cushion in one visit instead of waiting for a frame
handoff. `OGRE_AUDIO_INLINE=0` restores the task-thread path for an A/B.

| screen | task-thread path | inline path |
|---|---|---|
| title | 56.3-58.0 buffers/s, 0-2.6% underrun | unchanged |
| mission scene `0x0003` | 40.8-57.4/s, 1.5-29.8% | 57.8/s, 0.1-0.4% |
| tutorial practice field | 57.0/s, 1.9% | 58.0/s, 0% |

The mission spread on the task-thread path is host load. A 60 s mission window
read 56.4/s and 3.0%; a 20 s window under load read 40.8/s and 29.8%.

The 30 s cathedral capture that looked like a regression was the backdrop still
assembling; the scene starts black and fades in (developer). At a 60 s settle
the inline build's backdrop is complete and matches the task-thread capture.
A second idea, posting the audio completion straight to `0x800C49E8` and
bypassing the task manager, left the title at scene `0x0004` for 180 s; that
path is reverted and is not used.

## Evidence

### The worklet is draining, the game produced too slowly

A 20-60 s window read the ring's `head` delta, the game's audio task lines and
the worklet's own counters:

```
title:  58.0 buffers/s = 32036 frames/s, underrun 0/480256 frames
mission 20 s, loaded host: 40.8 buffers/s = 22546 frames/s, underrun 286969/960512 = 29.8%
mission 20 s, idle host:   57.4 buffers/s = 31666 frames/s, underrun 14051/960512 = 1.5%
mission 60 s:              56.4 buffers/s = 31137 frames/s, underrun 85178/2881024 = 3.0%
tutorial field:            57.0 buffers/s = 31373 frames/s, underrun 13650/720384 = 1.9%
```

The inline build:

```
mission 20 s: 57.8 buffers/s = 31902 frames/s, underrun 3214/963072 = 0.3%
mission probe, 5 s: 0.1% of 240128 output frames (241 frames, 1 run)
tutorial field 15 s: 58.0 buffers/s = 32003 frames/s, underrun 0/1440256 = 0%
```

`frames` counts both channels and `underrunFrames` counts output frames, so the
starved share is `underrunFrames / (frames / 2)`. `levels` resets both on each
reply.

### The native audio implementation

`app/src/sdl_platform.cpp`:

- `queue_audio_samples` sends the game's int16 stereo buffer to
  `SDL_QueueAudio`. The SOUNDS mute and VOLUME scale the samples, and the queue
  length is unchanged by the gain.
- `get_audio_frames_remaining` returns `SDL_GetQueuedAudioSize() / sizeof(int16_t)`.
- The same runtime `ultramodern::audio_dma_busy()` compares that value with the
  52 ms cushion (`ultramodern/src/audio.cpp`), and the same
  `osAiGetStatus_recomp` reports `AI_STATUS_BUSY` (`librecomp/src/ai.cpp`).

Session 76 measured the game's audio thread polling `osAiGetLength` once per
video frame. Retail renders the mission at 60 fps (session 199), so 60 polls/s is enough
for 58 buffers/s; the 52 ms cushion keeps the SDL queue from starving. SDL's queue
includes its 512-frame device buffer (~16 ms), which the web ring does not, but
that is a small extra cushion, not a separate pacing mechanism. The browser's
mission at ~30 fps has the same frame-paced audio thread with fewer polls, and
the web platform has no device thread that can generate buffers. The inline task
removes the handoff from the audio thread's own cycle; it does not change the
DAC rate or the 52 ms cushion.

### The completion path

`func_80085908` is the game's audio thread. It submits an audio task, then waits
on `osRecvMesg(0x800C49E8)`; `func_80080F78` is the per-buffer callback that
sends to the audio task and waits for its completion. The runtime's
`task_thread_func` runs the task and calls `sp_complete()`, which posts
`OS_EVENT_SP` (`0x800E8BBC`, message `0x29B` in the title run). A task-manager
thread forwards that to the audio queue, and the audio thread generates the next
buffer. Every step is a game-thread handoff. The inline path keeps the same
`sp_complete()` message and the same task-manager path; it only runs the RSP
code before the audio thread's next receive, so the chain does not wait for a
frame handoff.

### The cathedral timing

The first 12 s capture was the scene's fade-in (developer). A 30 s A/B showed
the inline run's backdrop still missing upper-right macroblocks while the
task-thread run was complete, but the inline build at a 60 s settle is complete:
`/tmp/cathedral-inline60-normal.png`, 149442 bytes, against the task-thread
`/tmp/cathedral-noinline-normal.png`, 149491 bytes, which is byte-identical to
session 203's `docs/proofs/web-newgame-cathedral-session203.png`. The 30 s
difference was assembly time, not a lost completion.

The direct-completion variant kept the task thread but posted to `0x800C49E8`
instead of `sp_complete()`. The title reached scene `0x0004` and never advanced
to scene `0x0003` in 180 s. That variant is reverted.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final tree.
- `node --check app/web/audio-worklet.js debug/probes/audio.cjs`.
- `node debug/probes/audio.cjs --headed --secs 180 --wait-scene 0x0003
  --query 'OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'`
  read `scene 0x0003 after 40.7 s`, ring peak 563, output 0.4579/0.1185 and 0.1%
  underrun over 240128 output frames.
- The tutorial field read 58.0 buffers/s and 0% underrun; the title read 0.4%.
- `node debug/probes/scene.cjs --route cathedral --settle 60000` reached scene
  `0x000D`, ran at 40.09 tasks/s, and wrote the complete backdrop.
- `patches/n64modernruntime-ob64.patch` regenerated with
  `git -C tools/N64ModernRuntime diff --ignore-submodules=all`;
  `git apply --check --reverse` passes.
- `OGRE_AUDIO_INLINE=0` still reaches the mission, at the task-thread rate.

## Files changed

| file | change |
|---|---|
| `tools/N64ModernRuntime/ultramodern/src/events.cpp` | browser-only inline `M_AUDTASK` path in `submit_rsp_task`; `OGRE_AUDIO_INLINE=0` escape hatch |
| `tools/N64ModernRuntime/librecomp/src/rsp.cpp` | mutex around `run_task`, because the inline audio task shares the RSP DMEM image with the task thread's njpeg tasks |
| `patches/n64modernruntime-ob64.patch` | regenerated with both runtime changes |
| `app/web/audio-worklet.js` | `levels` reports `underruns`/`underrunFrames`; header documents the starved share |
| `debug/probes/audio.cjs` | reports the underrun share over a 5 s window; `--wait-scene` holds the measurement until a named scene and adds `OGRE_SCENE_LOG=1` |
| `docs/guides/web-probes.md` | the underrun fields and formula, the mission command, the measured rates |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md` | status, log, decision, index |
| `docs/HANDOFF-2026-10-31-session205.md` | this file |

## Probes used and reverted

- The direct-completion variant (`0x800C49E8` instead of `sp_complete()`) was
  measured, hung the title, and is reverted. The inline path is the landed fix,
  not a probe.
- No generated code was instrumented.

## Open

1. **The mission's video frame rate is still ~30 fps.** The inline path removes
   the audio coupling, but sessions 198/199 left the mission's remaining cost
   outside the flush loop. Raising the frame rate is a separate task.
2. **Listen on a real output device.** The probe measures the worklet's output,
   not the speakers. The mission and the tutorial battle should be checked by
   ear.
