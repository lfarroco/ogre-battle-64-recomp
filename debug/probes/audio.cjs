#!/usr/bin/env node
'use strict';

// Browser audio: does the game's PCM reach the page's output?
//
// The web port runs the game's own audio microcode (app/src/rsp.cpp), which
// fills each AI buffer with int16 PCM at the game's 32 kHz rate. The runtime
// hands those buffers to app/src/web_platform.cpp, which copies them into a
// ring in wasm memory; app/web/audio-worklet.js resamples the ring to the
// AudioContext rate and writes the output. This probe measures each link:
//
//   1. the ring's state (`head`, `tail`, rate, enabled) and the peak/RMS of the
//      PCM queued in it (the game side);
//   2. the samples the AudioWorklet actually writes: `window.ogreAudio.levels()`
//      reads peak/RMS that the processor accumulated on the audio thread, after
//      resampling, the VOLUME gain and the SOUNDS mute. Measuring in the
//      worklet avoids the analyser's 42 ms window and main-thread throttling;
//   3. the mute path: SETTINGS > SOUNDS off must produce silence while the ring
//      keeps draining.
//
// It does not need a key press: the probe passes
// `--autoplay-policy=no-user-gesture-required` so headless Chromium starts the
// AudioContext without a gesture. In a real browser the page's first pointer or
// key event does that (app/web/web.js -> installAudioGesture).
//
// Usage:
//   node debug/probes/audio.cjs
//   node debug/probes/audio.cjs --secs 180 --query 'OGRE_SCENE=tutorial'
//   node debug/probes/audio.cjs --out /tmp/audio-probe
//
// Options: --secs (boot/audio wait), --query (extra page knobs), --wait-scene
//          (hold the measurement until this scene id, e.g. 0x0003), --out, --rom,
//          --url, --headed. The scene defaults to the title, which has music.
//
// Exit codes: 0 the pipeline carries audio and mutes, 2 no PCM within --secs,
//             1 fatal (Chromium missing, page error, …).

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
cfg.browserArgs = ['--autoplay-policy=no-user-gesture-required'];
const prefix = h.outPrefix(cfg, opts.out || 'audio');
const waitSecs = h.num(opts.secs, 180);
const waitScene = opts['wait-scene'] ? String(opts['wait-scene']).toLowerCase() : '';
let query = opts.query === undefined ? 'OGRE_SCENE=title' : String(opts.query);

// A scene wait needs the scene timeline. The scene line is `[scene] ... id=0xNNNN`.
if (waitScene && !/OGRE_SCENE_LOG=/.test(query)) {
  query += (query ? '&' : '') + 'OGRE_SCENE_LOG=1';
}
if (query) {
  cfg.url += (cfg.url.includes('?') ? '&' : '?') + query;
}

function normScene(value) {
  const n = /^0x/i.test(value) ? parseInt(value, 16) : parseInt(value, 10);
  return Number.isFinite(n) ? '0x' + n.toString(16).toLowerCase().padStart(4, '0') : String(value).toLowerCase();
}

const sleep = h.sleep;

// Read the ring's counters and the PCM currently buffered in it. `head` and
// `tail` are absolute uint32 frame counters; the ring index is `counter & mask`.
async function readRing(page, maxFrames = 12000) {
  return await page.evaluate((maxFrames) => {
    if (!Module._ogre_audio_state_ptr || !Module._ogre_audio_ring_ptr) {
      return { available: false };
    }
    const state = new Int32Array(Module.HEAPU8.buffer, Module._ogre_audio_state_ptr(), 5);
    const ring = new Int16Array(Module.HEAPU8.buffer, Module._ogre_audio_ring_ptr(), 65536);
    const head = Atomics.load(state, 0);
    const tail = Atomics.load(state, 1);
    const rate = Atomics.load(state, 2);
    const enabled = Atomics.load(state, 3);
    const ringFrames = Atomics.load(state, 4);
    const mask = ringFrames - 1;
    const buffered = head - tail;
    const frames = Math.min(buffered, maxFrames);
    let peak = 0;
    let sum = 0;
    let nonzero = 0;
    let count = 0;
    for (let i = 0; i < frames; i++) {
      const idx = (head - frames + i) >>> 0;
      const base = (idx & mask) << 1;
      for (let ch = 0; ch < 2; ch++) {
        const v = ring[base + ch];
        const a = Math.abs(v);
        if (a > peak) peak = a;
        if (v !== 0) nonzero++;
        sum += v * v;
        count++;
      }
    }
    return {
      available: true, head, tail, buffered, rate, enabled, ringFrames,
      sampled: frames,
      peak, rms: count ? Math.sqrt(sum / count) : 0,
      nonzeroFraction: count ? nonzero / count : 0,
      node: !!(window.ogreAudio && window.ogreAudio.node),
      soundOn: !!(window.ogreAudio && window.ogreAudio.enabled && window.ogreAudio.enabled()),
      contextState: window.ogreAudio && window.ogreAudio.node
        ? window.ogreAudio.node.context.state : null,
      contextRate: window.ogreAudio && window.ogreAudio.node
        ? window.ogreAudio.node.context.sampleRate : null,
    };
  }, maxFrames);
}

// The ring drains in one AudioWorklet quantum (17 ms for a 552-frame buffer),
// so a single poll usually sees an empty ring. This page-side watcher samples
// `[tail, head)` every 25 ms and keeps the peaks it saw, which is what "the
// game is producing audio" means even on a loaded machine where one buffer
// arrives per second.
async function startRingWatch(page) {
  await page.evaluate(() => {
    if (window.__ogreRingWatchTimer) {
      clearInterval(window.__ogreRingWatchTimer);
    }
    const watch = { peak: 0, rms: 0, bufferedMax: 0, rate: 0, samples: 0, nonzero: 0 };
    window.__ogreRingWatch = watch;
    function tick() {
      if (!Module._ogre_audio_state_ptr || !Module._ogre_audio_ring_ptr) {
        return;
      }
      const state = new Int32Array(Module.HEAPU8.buffer, Module._ogre_audio_state_ptr(), 5);
      const ring = new Int16Array(Module.HEAPU8.buffer, Module._ogre_audio_ring_ptr(), 65536);
      const head = Atomics.load(state, 0);
      const tail = Atomics.load(state, 1);
      const rate = Atomics.load(state, 2);
      const mask = Atomics.load(state, 4) - 1;
      const buffered = head - tail;
      if (buffered > watch.bufferedMax) {
        watch.bufferedMax = buffered;
      }
      watch.rate = rate;
      const frames = Math.min(buffered, 2048);
      let sum = 0;
      for (let i = 0; i < frames; i++) {
        const idx = (head - frames + i) >>> 0;
        const base = (idx & mask) << 1;
        for (let ch = 0; ch < 2; ch++) {
          const v = ring[base + ch];
          const a = Math.abs(v);
          if (a > watch.peak) watch.peak = a;
          if (v !== 0) watch.nonzero++;
          sum += v * v;
        }
      }
      if (frames > 0) {
        const r = Math.sqrt(sum / (frames * 2));
        if (r > watch.rms) watch.rms = r;
      }
      watch.samples++;
    }
    window.__ogreRingWatchTimer = setInterval(tick, 25);
  });
}

async function readRingWatch(page) {
  return await page.evaluate(() => window.__ogreRingWatch || null);
}

async function stopRingWatch(page) {
  return await page.evaluate(() => {
    if (window.__ogreRingWatchTimer) {
      clearInterval(window.__ogreRingWatchTimer);
      window.__ogreRingWatchTimer = null;
    }
    return window.__ogreRingWatch || null;
  });
}

// The worklet keeps the peak and RMS of the samples it wrote since the previous
// `levels` request, on the audio thread, after VOLUME and the mute. Measuring it
// there avoids the analyser's 42 ms window and the main thread's timer
// throttling, both of which miss a 17 ms buffer on a loaded machine.
async function readLevels(page) {
  return await page.evaluate(() => window.ogreAudio.levels());
}

// Reset the counters, wait on this thread, then read the window.
async function measureWindow(page, ms) {
  await readLevels(page);
  await sleep(ms);
  return await readLevels(page);
}

// Wait until the worklet output carries a nonzero sample (the game generates a
// 552-frame buffer at a time, so this checks repeatedly rather than once).
async function waitForOutput(page, ms) {
  const t0 = Date.now();
  let last = { peak: 0, rms: 0, frames: 0 };
  while (Date.now() - t0 < ms) {
    last = await measureWindow(page, 1000);
    if (last.peak > 0) {
      return last;
    }
  }
  return last;
}

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.boot(browser, cfg);

  // Optional: hold the measurement until a named screen is up. The mission
  // needs its save and taps, and measuring the title's music instead was the
  // first version of this check (session 205).
  if (waitScene) {
    const want = normScene(waitScene);
    const t0 = Date.now();
    let last = '';
    while ((Date.now() - t0) / 1000 < waitSecs) {
      const lines = await page
        .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[scene\]/) : []))
        .catch(() => []);
      last = lines.length ? lines[lines.length - 1] : '';
      if (last.toLowerCase().includes('id=' + want)) {
        break;
      }
      await sleep(250);
    }
    if (!last.toLowerCase().includes('id=' + want)) {
      console.log(`scene ${want} not reached within ${waitSecs} s (last: ${last || 'none'})`);
      h.writeErrors(prefix, errors);
      await browser.close().catch(() => {});
      process.exit(2);
    }
    console.log(`scene ${want} after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  }

  const result = {
    url: cfg.url,
    ring: null,
    output: { peak: 0, rms: 0, frames: 0 },
    muted: { peak: 0, rms: 0, frames: 0 },
    unmutedAgain: { peak: 0, rms: 0, frames: 0 },
    waitedSecs: 0,
    ok: false,
  };

  const t0 = Date.now();
  await startRingWatch(page);
  let ring = { available: false };
  let watch = null;
  while ((Date.now() - t0) / 1000 < waitSecs) {
    watch = await readRingWatch(page).catch(() => watch);
    if (watch && watch.peak > 0 && watch.rate > 0) {
      break;
    }
    await sleep(1000);
  }
  result.waitedSecs = (Date.now() - t0) / 1000;
  result.ringWatch = watch;

  if (!watch || watch.peak === 0) {
    ring = await readRing(page).catch(() => ring);
    result.ring = ring;
    result.log = await page.evaluate(() => window.ogreLog ? window.ogreLog.tail(3000) : '');
    h.writeJson(prefix, 'result.json', result);
    h.writeErrors(prefix, errors);
    console.log(`NO AUDIO after ${result.waitedSecs.toFixed(1)} s: watch=${JSON.stringify(watch)} ring=${JSON.stringify(ring)}`);
    console.log('see ' + prefix + '-result.json');
    await browser.close().catch(() => {});
    process.exit(2);
  }

  // The game has queued nonzero PCM. Give the worklet time to see it, then
  // measure the page's output.
  result.ring = await readRing(page);
  if (result.ring.soundOn === false) {
    // A stored SOUNDS=OFF from an earlier session would keep the probe silent;
    // turn it back on so the measurement covers the audio path itself.
    await page.evaluate(() => window.ogreAudio.setEnabled(true));
    await sleep(500);
    result.ring = await readRing(page);
  }
  result.output = await waitForOutput(page, 12000);

  // The worklet's own underrun counters over a fixed window. `frames` counts
  // both channels, `underrunFrames` counts output frames, so the share is
  // `underrunFrames / (frames / 2)`. This separates "the page is silent" from
  // "the game produced fewer buffers than the DAC consumed": the second is what
  // a low frame rate does to the mission's audio (see docs/guides/web-probes.md).
  result.underrun = await measureWindow(page, 5000);
  const outputFrames = result.underrun.frames / 2;
  result.underrunPercent = outputFrames > 0
    ? 100 * result.underrun.underrunFrames / outputFrames : 0;

  // SOUNDS off: the worklet still drains (tail advances) but writes silence.
  const beforeMute = await readRing(page);
  await page.evaluate(() => window.ogreAudio.setEnabled(false));
  await sleep(500);
  result.muted = await measureWindow(page, 1500);
  const afterMute = await readRing(page);

  // SOUNDS on again: the next generated buffer must come through.
  await page.evaluate(() => window.ogreAudio.setEnabled(true));
  await sleep(500);
  result.unmutedAgain = await waitForOutput(page, 12000);

  result.drainedWhileMuted = afterMute.head - beforeMute.head;
  result.ringAfter = afterMute;
  result.ok = watch.peak > 0 && result.output.peak > 0 &&
              result.muted.peak === 0 && result.muted.frames > 0 &&
              result.drainedWhileMuted > 0 && result.unmutedAgain.peak > 0;
  result.log = await page.evaluate(() => window.ogreLog ? window.ogreLog.tail(3000) : '');
  await stopRingWatch(page);

  h.writeJson(prefix, 'result.json', result);
  h.writeOut(prefix, 'pagelog.txt', result.log);
  h.writeErrors(prefix, errors);

  console.log(`ring: peak ${watch.peak}, rms ${watch.rms.toFixed(1)}, ` +
              `rate ${watch.rate} Hz, ${watch.nonzero} nonzero samples, ` +
              `max buffered ${watch.bufferedMax} frames`);
  console.log(`context: ${result.ring.contextState} at ${result.ring.contextRate} Hz, ` +
              `worklet ${result.ring.node ? 'connected' : 'MISSING'}, sounds ${result.ring.soundOn ? 'on' : 'off'}`);
  console.log(`output: peak ${result.output.peak.toFixed(4)}, rms ${result.output.rms.toFixed(4)}`);
  console.log(`underrun: ${result.underrunPercent.toFixed(1)}% of ${outputFrames} output frames ` +
              `(${result.underrun.underrunFrames} frames, ${result.underrun.underruns} runs)`);
  console.log(`muted:  peak ${result.muted.peak.toFixed(4)} (drain ${result.drainedWhileMuted} frames)`);
  console.log(`unmuted again: peak ${result.unmutedAgain.peak.toFixed(4)}`);
  console.log(result.ok ? 'AUDIO OK' : 'AUDIO FAILED');
  console.log('result: ' + prefix + '-result.json');
  await browser.close().catch(() => {});
  process.exit(result.ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
