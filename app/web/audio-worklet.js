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
// frames) so the producer can detect a full ring. Underruns output silence
// and snap the read position to the newest data.

class OgreAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.state = null;
    this.ring = null;
    this.mask = 0;
    this.readPos = 0;  // absolute float frame position (head/tail space)
    this.ready = false;
    this.port.onmessage = (event) => {
      const d = event.data;
      if (d && d.type === "init") {
        this.state = new Int32Array(d.sab, d.statePtr, 5);
        this.ring = new Int16Array(d.sab, d.ringPtr / 2, d.ringFrames * 2);
        this.mask = d.ringFrames - 1;
        this.readPos = 0;
        this.ready = true;
      }
    };
  }

  process(inputs, outputs) {
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
      // Need this sample and the next one for interpolation.
      if (this.readPos < tail || this.readPos + 1.0 >= head) {
        // Underrun: silence, and snap to the newest data so we never play
        // stale audio once the game resumes generating.
        left[i] = 0;
        right[i] = 0;
        this.readPos = head > 1 ? head - 1 : 0;
        continue;
      }

      const idx = Math.floor(this.readPos);
      const frac = this.readPos - idx;
      const base = (idx & mask) << 1;
      const next = ((idx + 1) & mask) << 1;

      left[i] = ring[base] + (ring[next] - ring[base]) * frac;
      right[i] = ring[base + 1] + (ring[next + 1] - ring[base + 1]) * frac;

      this.readPos += step;
      consumedFrames += step;
    }

    if (consumedFrames >= 1) {
      // Free the consumed frames for the producer (whole frames only).
      Atomics.add(state, 1, Math.floor(consumedFrames));
    }

    return true;
  }
}

registerProcessor("ogre-audio-processor", OgreAudioProcessor);
