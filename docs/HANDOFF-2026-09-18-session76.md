# Handoff — 2026-09-18, session 76: **audio latency fixed (AI busy bit + cushion)**

> Follow-up to session 75 (which made native audio play). The developer reported
> a 1–2 s lag between picture and sound. The cause is one hardcoded register:
> `osAiGetStatus` always returned 0, so the game's audio thread never waited for
> the DAC and produced ~3.5 % more audio than was played, growing the output
> queue without bound. It now returns `AI_STATUS_BUSY` **with a ~52 ms cushion**,
> and the queue holds at ~2–57 ms instead of growing to seconds — at the exact
> DAC rate, with no underruns.

## Goal and result

**Goal (developer):** the 1–2 s delay between picture and sound.

**Result:** output latency is ~50–70 ms (a deliberate cushion plus SDL's device
buffer) instead of a queue growing to 2 s a minute. The game generates at exactly
the DAC rate (58.0 buffers/s measured, against 32000/552 = 58.0), the queue never
runs empty, and the headless path is unaffected.

**There is a trap here, and it cost the first attempt:** the obvious fix
(`busy while anything is still queued`) removes the latency but makes the music
*drag and tear*. Read §2 before changing the threshold.

## 1. Measurement: the queue grew without bound

A temporary probe in `queue_audio_samples` / `get_audio_frames_remaining`
(`probe76`) printed the SDL queued-audio depth and the poll rate. Over a 70 s run
the queue grew **linearly**: 22 ms at the first push, 2 070 ms at the end
(~30 ms/s), resetting only when the music stream restarted. The game polled
`osAiGetLength` exactly **60 times per second** — one per video frame — so its
audio thread was paced by the *frame* rate, not the DAC.

The device was opened correctly (`want` 32000 Hz S16SYS stereo → `have`
identical), so this was not a sample-rate mismatch.

## 2. The cause and the trap

The game's audio thread is `func_80085908` (`RecompiledFuncs/funcs_0.c`, the
caller of `osAiSetNextBuffer` at `0x800859BC`):

```
L_8008595C:
  call *(*0x800ADAC8 + 4)      ; per-iteration update
  s0 = osAiGetStatus()
  v0 = osAiGetLength()
  s0 &= 0x80000000             ; AI_STATUS_BUSY
  if (s0 != 0) goto L_8008595C ; wait for the AI
  ... generate and osAiSetNextBuffer ...
  j L_8008595C                 ; tight loop, no other wait
```

`librecomp/src/ai.cpp` had

```cpp
ctx->r2 = 0x00000000; // Pretend the audio DMAs finish instantly
```

so the loop never waited and ran at the video frame rate: **552 stereo frames ×
60 = 33 120 frames/s** against a 32 000 Hz DAC — the 3.5 % surplus accumulated in
the SDL queue. The buffer size confirms the intent: 32 000 / 552 = **58.0
buffers/s**, the *audio* period, so on hardware the busy bit is what supplies the
pace.

**The trap.** Reporting `busy while queued != 0` (the literal hardware analogue)
does pin the queue — but `probe77` showed it produced **34 buffers/s** with a
push onto an *empty* queue every time, i.e. a **40 % underrun**. The music engine
advances per generated buffer, so the music both dragged (34/58 = 59 % speed) and
tore at the gaps. That was the behaviour the developer heard as "a little slow
with some tearing".

The reason: on hardware the CPU polls this register in a tight loop (millions of
times per 17 ms buffer). In the recompiled port the same loop only iterates
**~120 times/s** — coarser than one buffer — so clearing the bit exactly when the
queue empties means the game notices up to ~8 ms *late*, and the period quantises
to 2 or 3 polls (60/s or 40/s), never the needed 58/s.

## 3. The fix: a cushion on the busy bit

`ultramodern::audio_dma_busy()` reports the AI idle once the queued audio falls
below a **52 ms** cushion, so the game tops the queue up *before* the device runs
dry. `osAiGetStatus_recomp` returns `0x80000000` (`AI_STATUS_BUSY`) while busy.

Measured sweep at 32 kHz (`probe77`, 12 s runs):

| cushion | pushes/s (target 58.0) | pushes onto an empty queue | queue depth |
|---|---|---|---|
| 0 ms (literal busy bit) | 34.0 | 68 of 68 | 0 ms |
| 36 ms | 58.5 | 15 | 0 ms |
| **52 ms** | **58.0** | **0** | **13–56 ms** |
| 69 ms | 58.0 | 0 | 24–78 ms |

52 ms is ~3 of OB64's buffers and the lowest setting that never starves the
device. Final: `kCushionMs = 52` in `ultramodern/src/audio.cpp`, with the sweep
recorded in the comment so nobody "simplifies" it back to 0.

Files (submodule; carried in `n64modernruntime-ob64.patch`):

* `ultramodern/include/ultramodern/ultramodern.hpp` — declares
  `bool ultramodern::audio_dma_busy();`.
* `ultramodern/src/audio.cpp` — implements it from the platform callback and the
  current sample rate.
* `librecomp/src/ai.cpp` — `osAiGetStatus_recomp` reports `AI_STATUS_BUSY`.
* `n64modernruntime-ob64.patch` regenerated. **Generate it with the nested
  submodule excluded**, or it picks up a bogus `N64Recomp` `Subproject commit`
  hunk:
  `git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp' > n64modernruntime-ob64.patch`
  (27 file diffs = the previous 26 + `librecomp/src/ai.cpp`).

## 4. Verification (what was run)

| check | result |
|---|---|
| queue depth, 70 s device-open, before | 22 ms → **2 070 ms** (~30 ms/s); `osAiGetLength` polled 60×/s |
| queue depth, naive busy bit | 17.2 ms flat, but **34 pushes/s, 68/68 onto an empty queue** (caught by `probe77`) |
| queue depth, final 52 ms cushion, 3× 30 s | **57.9–58.1 pushes/s**, **0 empty pushes**, queue 2–57 ms, polled ~120×/s |
| device-open 25 s | exit 0, no crash, no bailed task |
| `OGRE_NO_AUDIO=1` 15 s + `runlog.py --check` | **PASS** (no crash/stub/UNKNOWN, 0 stubbed non-gfx) |
| `cmake --build build-app` / `build-null` | clean |
| `n64modernruntime-ob64.patch` | regenerated; `git apply --check --reverse` OK; file list = previous + `librecomp/src/ai.cpp` |
| probes reverted | `grep -rl 'probe7[67]' app/` is empty; `app/src/sdl_platform.cpp` byte-identical to HEAD |

`probe76` (SDL queue depth, poll rate, opened spec) and `probe77` (push rate,
empty-push count, queue min/max) were temporary and are fully reverted.

Headless is unchanged: with `OGRE_NO_AUDIO=1` no device is opened, so
`get_frames_remaining()` is 0, `audio_dma_busy()` is false and the loop behaves
exactly as before. (The overproduced queue still exists in headless runs —
harmless, nothing consumes it.)

## 5. Files changed

* `tools/N64ModernRuntime/{librecomp/src/ai.cpp, ultramodern/src/audio.cpp,
  ultramodern/include/ultramodern/ultramodern.hpp}` — the busy bit + cushion
  (submodule; recorded in the patch).
* `n64modernruntime-ob64.patch` — regenerated (the only way the submodule change
  survives a re-clone).
* `PLAN.md`, `docs/DECISIONS.md`, this file.

## 6. Next leads

1. **The remaining latency is deliberate**: ~52 ms of cushion (so the game's
   coarse polling never starves the device) plus SDL's 512-frame device buffer
   (~16 ms). To shrink it, the *right* lever is the game's poll rate: if the
   audio loop could iterate faster than one buffer, the cushion could go to 0 and
   the latency to one buffer (~17 ms). Nothing observed currently limits the loop
   rate except its own ~8 ms per iteration (`func_80085908`'s update call), so a
   future session could profile that call.
2. **`get_remaining_audio_bytes()` reports twice the queued bytes.** The platform
   callback returns the queued *int16* count and `ultramodern/src/audio.cpp`
   multiplies by `2 * sizeof(int16_t)` = 4 (correct only if the callback returned
   *frames*). It only feeds the game's `s2` scheduling hint now, not the pacing
   gate, so it is cosmetic — but worth aligning if another game depends on
   `osAiGetLength` for buffer sizing.
3. Still open from session 75: the runtime wakes the audio thread with a dummy
   response on queue `0x800C49E8`; pacing no longer depends on it, so firing the
   real `OS_EVENT_AI` message when a buffer drains would let that be deleted.
