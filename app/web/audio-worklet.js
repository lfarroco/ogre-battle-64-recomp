// Ogre Battle 64 — audio consumer AudioWorklet processor.
//
// Reads interleaved stereo int16 samples straight out of the wasm heap (a
// SharedArrayBuffer, fixed 1 GiB — see app/CMakeLists.txt) using the ring
// buffer published by app/src/web_platform.cpp, and resamples from the game's
// sample rate to the AudioContext rate with linear interpolation.
//
// Ring layout (see app/src/web_platform.cpp):
//   state (Int32Array at ogre_audio_state_ptr()):
//     [0] = head          producer cursor, absolute uint32 frame counter
//     [1] = tail          consumer cursor, absolute uint32 frame counter
//     [2] = rate          game sample rate (Hz)
//     [3] = enabled       1 once the game has queued audio
//     [4] = ring_frames   capacity (power of two)
//   ring (Int16Array at ogre_audio_ring_ptr()):
//     interleaved L/R, index = (counter & (ring_frames - 1)) * 2 + channel
//
// The worklet is the only consumer and advances `tail` itself (by whole
// frames) so the producer can detect a full ring. On `init` and on `resync` it
// adopts the producer's newest frame and advances `tail` to it: audio queued
// before the context started (or while it was suspended) is stale, and the
// game must not see that backlog as frames waiting to play. Underruns output
// silence and snap the read position to the newest data.
//
// The ring holds int16 samples; an AudioWorklet output is float in [-1, 1], so
// every sample is scaled by 1/32768. Without that scaling the browser clips
// int16 magnitudes (thousands) as if they were full-scale floats, which is the
// distortion the session-22 "screeches" note describes (session 204).
//
// MUTED BY DEFAULT IN THIS FILE: the page sends the real state (`muted: !enabled`,
// on by default since the game's own audio microcode is recompiled and running).
// A muted worklet still runs the whole drain path (same `tail` bookkeeping, same
// underrun behaviour) and only zeroes the samples it writes to the output - the
// game's audio thread must keep seeing identical backpressure either way.
//
// `{type: "levels"}` asks for a measurement of what the processor wrote since
// the previous request: `peak`, `rms` and `frames`, accumulated in `process()`
// (after the VOLUME gain) and reset on each reply. It runs on the audio thread,
// so a probe's measurement is unaffected by main-thread timer throttling.
// The same reply carries `underruns` (runs of starved output) and
// `underrunFrames` (output frames written as silence). `frames` counts both
// channels and `underrunFrames` counts frames, so the starved share is
// `underrunFrames / (frames / 2)`. A mission frame holds the game's audio
// thread, so this is the number that shows the game producing fewer buffers
// than the DAC consumes; see `docs/guides/web-probes.md`.

const INT16_TO_FLOAT = 1 / 32768;

class OgreAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.state = null;
    this.ring = null;
    this.mask = 0;
    this.readPos = 0;  // absolute float frame position (head/tail space)
    // Fractional frames between `readPos` and the whole-frame `tail`. Discarding
    // it every render quantum (the old `Atomics.add(tail, floor(consumed))`)
    // made `tail` lag by ~0.33 frame per 128-sample quantum, so the producer's
    // `head - tail` grew without any audio waiting and `audio_dma_busy()` throttled
    // the game's audio thread (session 204 follow-up).
    this.pendingFrames = 0;
    this.ready = false;
    this.muted = true;  // the page sends the SOUNDS setting in `init`
    this.gain = 1;      // the VOLUME row, 0..1
    // Output levels since the last `levels` request (the audio thread owns
    // these; the page only reads the reply).
    this.levelPeak = 0;
    this.levelSumSq = 0;
    this.levelCount = 0;
    this.processCount = 0;
    // Underruns are the worklet's own record of a starved ring: output samples
    // written as silence because the producer had not queued two frames to
    // interpolate. `underruns` counts runs, `underrunFrames` counts samples.
    this.underrunFrames = 0;
    this.underruns = 0;
    this.inUnderrun = false;
    this.port.onmessage = (event) => {
      const d = event.data;
      if (d && d.type === "init") {
        this.state = new Int32Array(d.sab, d.statePtr, 5);
        // Both pointers are byte offsets in the wasm heap. Int16Array's second
        // argument is a byte offset too; dividing it by two moved the view into
        // unrelated wasm data, which is what the old build played.
        this.ring = new Int16Array(d.sab, d.ringPtr, d.ringFrames * 2);
        this.mask = d.ringFrames - 1;
        this.readPos = 0;
        this.muted = d.muted !== false;
        this.gain = typeof d.gain === "number" ? Math.max(0, Math.min(1, d.gain)) : 1;
        this.ready = true;
        this.resync();
      } else if (d && d.type === "mute") {
        this.muted = d.muted !== false;
      } else if (d && d.type === "gain") {
        this.gain = typeof d.gain === "number" ? Math.max(0, Math.min(1, d.gain)) : 1;
      } else if (d && d.type === "resync") {
        this.resync();
      } else if (d && d.type === "levels") {
        const count = this.levelCount;
        this.port.postMessage({
          type: "levels",
          peak: this.levelPeak,
          rms: count ? Math.sqrt(this.levelSumSq / count) : 0,
          frames: count,
          head: this.state ? Atomics.load(this.state, 0) : -1,
          tail: this.state ? Atomics.load(this.state, 1) : -1,
          readPos: this.readPos,
          processCount: this.processCount,
          underruns: this.underruns,
          underrunFrames: this.underrunFrames,
        });
        this.levelPeak = 0;
        this.levelSumSq = 0;
        this.levelCount = 0;
        this.underruns = 0;
        this.underrunFrames = 0;
      }
    };
  }

  // Discard everything the producer has queued and start at its newest frame.
  // Called when the consumer attaches and when a suspended context resumes;
  // both would otherwise play up to a ring (0.74 s) of audio from before.
  resync() {
    if (!this.state) {
      return;
    }
    const head = Atomics.load(this.state, 0);
    Atomics.store(this.state, 1, head);
    this.readPos = head;
    this.pendingFrames = 0;
  }

  process(inputs, outputs) {
    this.processCount++;
    const output = outputs[0];
    const left = output[0];
    const right = output[1];
    const n = left.length;
    if (!this.ready || !this.ring) {
      left.fill(0);
      right.fill(0);
      return true;
    }

    const state = this.state;
    const head = Atomics.load(state, 0);
    const tail = Atomics.load(state, 1);
    const rate = Atomics.load(state, 2);
    const enabled = Atomics.load(state, 3);

    if (!enabled || rate === 0) {
      left.fill(0);
      right.fill(0);
      return true;
    }

    const step = rate / sampleRate;
    const mask = this.mask;
    const ring = this.ring;
    let consumedFrames = 0;

    for (let i = 0; i < n; i++) {
      // `tail` is a whole frame count and `readPos` is a float sum of
      // `rate / sampleRate` steps, so the two can disagree by ~1e-12 after a
      // drain (255.99999999999852 against 256). Treating that as a desync and
      // snapping to `head` discarded every frame still queued and left `tail`
      // behind forever, which throttled the game's audio thread; clamp to the
      // oldest unconsumed frame instead.
      if (this.readPos < tail) {
        this.readPos = tail;
      }
      // Need this sample and the next one for interpolation.
      if (this.readPos + 1.0 >= head) {
        // Underrun: silence, and start again at the producer's newest frame so
        // we never play stale audio once the game resumes generating.
        left[i] = 0;
        right[i] = 0;
        this.readPos = head;
        this.underrunFrames++;
        if (!this.inUnderrun) {
          this.underruns++;
          this.inUnderrun = true;
        }
        continue;
      }

      const idx = Math.floor(this.readPos);
      const frac = this.readPos - idx;
      const base = (idx & mask) << 1;
      const next = ((idx + 1) & mask) << 1;

      left[i] = (ring[base] + (ring[next] - ring[base]) * frac) * INT16_TO_FLOAT;
      right[i] = (ring[base + 1] + (ring[next + 1] - ring[base + 1]) * frac) * INT16_TO_FLOAT;
      this.inUnderrun = false;

      this.readPos += step;
      consumedFrames += step;
    }

    // Free the consumed frames for the producer. The fractional frame is kept
    // for the next quantum; dropping it every call made `tail` lag behind what
    // was actually consumed, and the producer's queue-depth check then saw a
    // backlog that was not there.
    this.pendingFrames += consumedFrames;
    const wholeFrames = Math.floor(this.pendingFrames);
    if (wholeFrames >= 1) {
      Atomics.add(state, 1, wholeFrames);
      this.pendingFrames -= wholeFrames;
    }

    if (this.gain !== 1) {
      // VOLUME row, applied to the normalized samples above.
      for (let i = 0; i < n; i++) {
        left[i] *= this.gain;
        right[i] *= this.gain;
      }
    }

    if (this.muted) {
      // Drain, don't play. Doing it here (rather than skipping the loop above)
      // keeps the tail/readPos bookkeeping - and therefore the game's view of
      // the ring - byte-for-byte identical to the unmuted path.
      left.fill(0);
      right.fill(0);
    }

    // What this processor actually sent, for `debug/probes/audio.cjs`.
    for (let i = 0; i < n; i++) {
      const l = left[i];
      const r = right[i];
      const a = l < 0 ? -l : l;
      const b = r < 0 ? -r : r;
      if (a > this.levelPeak) this.levelPeak = a;
      if (b > this.levelPeak) this.levelPeak = b;
      this.levelSumSq += l * l + r * r;
    }
    this.levelCount += n * 2;

    return true;
  }
}

registerProcessor("ogre-audio-processor", OgreAudioProcessor);
