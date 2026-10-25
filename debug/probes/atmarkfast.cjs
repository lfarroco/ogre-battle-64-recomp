#!/usr/bin/env node
'use strict';

// Capture the canvas at every renderer flush around a scene transition, in the
// browser build, and report whether a near-white frame is presented.
//
// `atmark.cjs` waits for a log marker and then takes `--shots` Playwright
// element screenshots `--gap` ms apart. One element screenshot of the running
// canvas costs 0.95-9.5 s under load (session 176), so a nominal 250 ms series
// spans tens of seconds and misses a transition that lasts a few frames. This
// probe reads the canvas inside the page at the cadence the page flushes.
//
// Mechanism. The page calls `Module._ogre_gfx_flush()` from a 16 ms
// `setInterval` (app/web/web.js) and a second time from `updateGfxStatus()` in
// the same tick. The probe replaces that property with a wrapper which calls
// the original and then, while the series is open, takes one sample. Two calls
// less than 3 ms apart are coalesced, so the cadence is one sample per tick
// (~60 Hz). `requestAnimationFrame` is the fallback when the property cannot be
// replaced or the wrapper is never called; `result.json.mechanism` says which
// mechanism ran.
//
// A sample is `gl.readPixels` of the default framebuffer into a preallocated
// `Uint8Array` (`ogre_gfx_create_context` sets `preserveDrawingBuffer = true`),
// followed by one pass over the pixels. No Playwright screenshot runs in the
// loop, and `ogre_gfx_stats()` is never called (it takes the stats mutex).
//
// Arm and disarm are log-driven. The probe installs `window.ogreLog.onLine`
// before the ROM is handed to the page, so no polling and no `ogreLog.find()`
// full scan happens in the capture window. The marker is the scene line the
// port prints under `OGRE_SCENE_LOG=1`:
//
//   [stderr] [scene] t=12345ms id=0x0012 descriptor=0x... mask=0x...
//
// `--arm` (default `id=0x0012`, the Load Game scene) arms the series; `--disarm`
// (default `id=0x0003`) starts the disarm countdown, which ends `--after`
// distinct captures later. The armed window is also bounded by `--timeout`
// seconds after the arm line. `--pre` keeps the last N distinct captures taken
// before the arm line; pre-arm sampling starts at the first `[scene]` line, and
// `--no-capture` measures what that sampling costs.
//
// Metrics per sample (RGBA8, 0..255 per channel):
//   mean3        (r+g+b) / pixel, range 0..765
//   meanPC       mean3 / 3, range 0..255. The renderer's own `meanRGB` is the
//                same sum over a different range, so this name is kept distinct.
//   fracW240     fraction of pixels with min(r,g,b) >= 240
//   fracW250     fraction of pixels with min(r,g,b) >= 250
//   fracNeutral  fraction of pixels with max(r,g,b) - min(r,g,b) <= 8
//   nonBlack     count of pixels with any channel > 8
//   colorful     count of pixels with |r-g| + |g-b| + |r-b| > 24
//   checksum     two 32-bit FNV-style accumulators over every 8th pixel
//
// A white plate is `fracW240 >= 0.90 && meanPC >= 240`. The candidate band is
// `fracW240 >= 0.25`; `candidate` is set only when the frame is not already
// white, so the two counts do not overlap. A capture whose integer metrics
// (w, h, sum3 and the pixel counts) equal the previous capture increments `dup`
// and appends no record.
//
// Storage. `window.__fastmark.frames` keeps the most recent 240 records, and
// every record is streamed to Node through `page.exposeBinding` as it is taken,
// so the in-page truncation loses nothing on disk. `distinctFrames` counts every
// record in the series, pre-roll included; `armedFrames` and `preFrames` split
// it. `coverage` describes the armed window: its start and end on the page clock,
// its span, the number of armed captures, the largest and the mean gap between
// captures, and the effective capture rate. A gap covers the coalesced second
// flush of a tick and any main-thread stall.
//
// For a flagged frame (white or candidate) the probe takes one
// `canvas.toDataURL('image/png')` (capped by `--keep`), calls `gl.getError()`,
// and counts guest halfwords equal to `0xFFFD` in the Load Game colour image at
// 0x228080..0x24D880, sampling every 64th pixel (stride 0x80 bytes). The guest
// halfword at offset `off` is
// `heap[base + (off ^ 2)] | (heap[base + (off ^ 2) + 1] << 8)`, where `base` is
// the RDRAM pointer from the page's `[debug] wasm calloc(<n>) -> 0x...` line.
// That plate is the candidate session 170 found behind a presented white frame.
//
// Controls. Before the boot the metric function runs on a white and a black 2D
// canvas and asserts `fracW240` 1 and 0 (`result.json.controls.metric`). The
// WebGL context attributes are read once and recorded. `--no-capture` runs the
// same arm/disarm timeline with no `readPixels`; it disarms on the `--disarm`
// line because there are no captures to count.
//
// Usage:
//   node debug/probes/atmarkfast.cjs --out /tmp/w190-loadgame \
//     --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'
//
// Options:
//   --url URL        page to load (default: the harness's OGRE_URL)
//   --rom PATH       ROM handed to the page (default: assets/ogre64.z64)
//   --out PREFIX     output prefix (default: <OGRE_OUT>/atmarkfast)
//   --arm REGEX      scene-line regex that arms (default: id=0x0012)
//   --disarm REGEX   scene-line regex that starts the disarm countdown
//                    (default: id=0x0003)
//   --after N        distinct captures kept after the disarm line (default 2)
//   --pre N          distinct captures kept from before arming (default 20,
//                    0 disables pre-arm sampling)
//   --pre-from REGEX scene-line regex where pre-arm sampling starts (default:
//                    the first scene line; `id=0x0004` covers the title and the
//                    frames leading into 0x12)
//   --timeout SEC    bound on the armed window, and on the wait for the arm
//                    line (default 240)
//   --keep N         maximum PNGs written for flagged frames (default 40)
//   --no-capture     record the scene timeline only; no readPixels
//
// Outputs, under the output prefix:
//   <prefix>-frames.jsonl   one JSON record per distinct capture
//   <prefix>-frames.txt     the same records, one line each
//   <prefix>-w<k>.png       a flagged frame (`<prefix>-w01.png`, ...)
//   <prefix>-pagelog.txt    the page's ogreLog text
//   <prefix>-errors.txt     page errors
//   <prefix>-result.json    mechanism, timings, counts, coverage, scene timeline
//
// Exit codes: 0 the series was captured and no white frame was seen, 3 a white
// frame was captured, 2 the arm marker never arrived, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'atmarkfast');

const armSrc = typeof opts.arm === 'string' && opts.arm ? opts.arm : 'id=0x0012';
const disarmSrc = typeof opts.disarm === 'string' && opts.disarm ? opts.disarm : 'id=0x0003';
// `--pre-from` starts the pre-arm sampling at a scene line instead of the first
// one. `0x0012` is usually entered from `0x0004`, and sampling the boot's
// `0x0000` frames is both useless and a CPU cost on the route.
const preFromSrc = typeof opts['pre-from'] === 'string' ? opts['pre-from'] : '';
const after = Math.max(0, h.num(opts.after, 2));
const pre = Math.max(0, h.num(opts.pre, 20));
const keep = Math.max(0, h.num(opts.keep, 40));
const timeoutSecs = Math.max(1, h.num(opts.timeout, 240));
const noCapture = Boolean(opts['no-capture']) &&
  String(opts['no-capture']) !== '0' && String(opts['no-capture']) !== 'false';

// Fail before launching a browser when a regex is malformed.
try {
  new RegExp(armSrc);
  new RegExp(disarmSrc);
  if (preFromSrc) {
    new RegExp(preFromSrc);
  }
} catch (e) {
  console.error(`bad --arm/--disarm/--pre-from regex: ${e.message}`);
  process.exit(1);
}

// ---------------------------------------------------------------------------
// Page side. Everything below runs inside the page (serialised by
// page.evaluate), so it must be self-contained. It is evaluated in sloppy mode:
// a failed `Module._ogre_gfx_flush` assignment is silent, and the identity check
// after it is what detects the failure.
// ---------------------------------------------------------------------------
const INSTALL_PAGE = (args) => {
  const r1 = (v) => Math.round(v * 10) / 10;
  const r2 = (v) => Math.round(v * 100) / 100;
  const r3 = (v) => Math.round(v * 1000) / 1000;
  const r4 = (v) => Math.round(v * 10000) / 10000;

  const st = window.__fastmark = {
    armRe: new RegExp(args.arm),
    disarmRe: new RegExp(args.disarm),
    after: args.after,
    pre: args.pre,
    keep: args.keep,
    timeoutMs: args.timeoutMs,
    noCapture: args.noCapture,

    frames: [],          // distinct records of the armed window (last 240)
    preBuf: [],          // distinct records taken before the arm line
    sceneTimeline: [],   // every [scene] line, in order
    errors: [],

    samples: 0,          // captures taken (duplicates included)
    flushN: 0,           // wrapper calls (two per 16 ms tick)
    rafTicks: 0,
    coalesced: 0,        // wrapper calls skipped by the 3 ms rule
    dup: 0,              // captures whose integer metrics repeated
    distinct: 0,         // distinct records emitted (pre + armed)
    armedDistinct: 0,
    whiteTotal: 0, whiteArmed: 0,
    candTotal: 0, candArmed: 0,
    kept: 0,             // PNGs taken
    truncated: 0,        // records dropped from st.frames by the 240 cap

    armed: false, done: false,
    armMs: null, armWall: null, armScene: null,
    disarmMs: null, disarmScene: null, disarmReason: null, disarmLineMs: null,
    stopAt: null, armTimer: 0, raf: 0, wrapChecked: false,

    mechanism: 'none',
    gl: null, glPrev: null, glBuf: null, glW: 0, glH: 0, preserve: null,
    sawGfxContext: false, sawScene: false, observer: false,
    preArmed: false,
    preFromRe: args.preFrom ? new RegExp(args.preFrom) : null,
    lastScene: null, taskN: null,
    rdramBase: null, rdramWhiteMax: null, rdramHitFrames: 0,

    lastKey: '', lastCallMs: -1e9,
    armedSamples: 0, armedFirstPage: null, prevPage: null,
    gapMax: 0, gapSum: 0, gapN: 0,
  };

  const note = (text) => {
    if (st.errors.length < 40) st.errors.push(String(text).slice(0, 300));
  };

  const send = (msg) => {
    try {
      if (typeof window.__fastOut === 'function') {
        const p = window.__fastOut(msg);
        // The binding returns a promise; a write failure on the Node side must
        // not surface as an unhandled rejection in the page.
        if (p && typeof p.catch === 'function') p.catch(() => {});
      }
    } catch (e) { /* the Node side is gone; keep capturing */ }
  };

  // ---- pixel metrics -------------------------------------------------------
  const measure = (buf, w, h) => {
    const total = w * h;
    let sum3 = 0, n240 = 0, n250 = 0, neutral = 0, nonBlack = 0, colorful = 0;
    let h1 = 0x811c9dc5, h2 = 0x01000193;
    for (let p = 0, i = 0; p < total; p++, i += 4) {
      const r = buf[i], g = buf[i + 1], b = buf[i + 2];
      sum3 += r + g + b;
      const mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      if (mn >= 240) n240++;
      if (mn >= 250) n250++;
      if (mx - mn <= 8) neutral++;
      if (r > 8 || g > 8 || b > 8) nonBlack++;
      if (Math.abs(r - g) + Math.abs(g - b) + Math.abs(r - b) > 24) colorful++;
      if ((p & 7) === 0) {
        h1 = Math.imul(h1 ^ r, 16777619) >>> 0;
        h2 = Math.imul(h2 ^ ((g << 8) | b), 2654435761) >>> 0;
      }
    }
    const mean3 = total ? sum3 / total : 0;
    const f240 = total ? n240 / total : 0;
    const meanPC = mean3 / 3;
    const white = f240 >= 0.90 && meanPC >= 240;
    return {
      w, h, total, sum3,
      mean3: r3(mean3),
      meanPC: r3(meanPC),
      fracW240: r4(f240),
      fracW250: r4(total ? n250 / total : 0),
      fracNeutral: r4(total ? neutral / total : 0),
      nonBlack, colorful,
      checksum: h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0'),
      white,
      candidate: f240 >= 0.25 && !white,
      key: w + ':' + h + ':' + sum3 + ':' + n240 + ':' + n250 + ':' + neutral +
           ':' + nonBlack + ':' + colorful,
    };
  };

  // ---- metric control (runs before the ROM is handed over) -----------------
  const control = (() => {
    const c = document.createElement('canvas');
    c.width = 8;
    c.height = 8;
    const ctx = c.getContext('2d', { willReadFrequently: true });
    const run = (fill) => {
      ctx.fillStyle = fill;
      ctx.fillRect(0, 0, 8, 8);
      const m = measure(ctx.getImageData(0, 0, 8, 8).data, 8, 8);
      return {
        fill, mean3: m.mean3, meanPC: m.meanPC,
        fracW240: m.fracW240, fracW250: m.fracW250, white: m.white,
        checksum: m.checksum,
      };
    };
    const white = run('#ffffff');
    const black = run('#000000');
    return {
      white, black,
      ok: white.fracW240 === 1 && black.fracW240 === 0 &&
          white.meanPC === 255 && white.white === true && black.white === false,
    };
  })();

  if (!control.ok) {
    return { fatal: 'the metric control failed', control, observer: false };
  }

  // ---- canvas read ---------------------------------------------------------
  // The context is resolved only after the page reports that it created one:
  // calling getContext('webgl2') first would create a context without
  // preserveDrawingBuffer and the renderer would then reuse it.
  const ensureGl = () => {
    if (st.gl) return st.gl;
    if (!st.sawGfxContext && !st.sawScene) return null;
    const canvas = document.getElementById('game-canvas');
    if (!canvas) return null;
    const gl = canvas.getContext('webgl2');
    if (!gl) return null;
    st.gl = gl;
    try {
      const attrs = gl.getContextAttributes();
      st.preserve = attrs ? attrs.preserveDrawingBuffer === true : null;
      st.glPrev = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    } catch (e) {
      st.preserve = null;
      st.glPrev = null;
      note('context attributes: ' + e);
    }
    return gl;
  };

  const readFrame = () => {
    const gl = ensureGl();
    if (!gl) return null;
    const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
    if (!w || !h) return null;
    if (!st.glBuf || st.glW !== w || st.glH !== h) {
      st.glBuf = new Uint8Array(w * h * 4);
      st.glW = w;
      st.glH = h;
    }
    try {
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, st.glBuf);
    } catch (e) {
      note('readPixels: ' + e);
      return null;
    } finally {
      try { gl.bindFramebuffer(gl.FRAMEBUFFER, st.glPrev); } catch (e) { /* ignore */ }
    }
    return measure(st.glBuf, w, h);
  };

  // Sparse count of 0xFFFD over the Load Game colour image. The guest halfword
  // at offset `off` is `heap[base + (off ^ 2)] | (heap[base + (off ^ 2) + 1] << 8)`;
  // 64 pixels per sample at 2 bytes per pixel is a stride of 0x80 bytes.
  const PLATE_LO = 0x228080;
  const PLATE_HI = 0x24D880;
  const PLATE_STEP = 0x80;
  const countPlate = () => {
    const heap = Module.HEAPU8;
    const base = st.rdramBase;
    if (!heap || base === null) return null;
    let white = 0, samples = 0;
    for (let off = PLATE_LO; off < PLATE_HI; off += PLATE_STEP) {
      const i = base + (off ^ 2);
      if (i + 1 >= heap.length) break;
      const hw = heap[i] | (heap[i + 1] << 8);
      samples++;
      if (hw === 0xFFFD) white++;
    }
    return { white, samples };
  };

  // ---- report / finish -----------------------------------------------------
  const report = () => {
    const start = st.armedFirstPage !== null ? st.armedFirstPage : st.armMs;
    const end = st.disarmMs;
    const span = (start !== null && end !== null) ? r1(end - start) : null;
    return {
      mechanism: st.mechanism,
      armedMs: st.armMs,
      disarmedMs: st.disarmMs,
      armScene: st.armScene,
      armWallMs: st.armWall,
      disarmScene: st.disarmScene,
      disarmLineMs: st.disarmLineMs,
      disarmReason: st.disarmReason,
      armMarkerSeen: st.armMs !== null,
      distinctFrames: st.distinct,
      armedFrames: st.armedDistinct,
      preFrames: st.distinct - st.armedDistinct,
      whiteFrames: st.whiteTotal,
      whiteFramesArmed: st.whiteArmed,
      candidateFrames: st.candTotal,
      candidateFramesArmed: st.candArmed,
      samples: st.samples,
      flushCalls: st.flushN,
      rafTicks: st.rafTicks,
      coalesced: st.coalesced,
      dupSamples: st.dup,
      coverage: {
        startPageMs: start,
        endPageMs: end,
        spanMs: span,
        samples: st.armedSamples,
        distinctArmed: st.armedDistinct,
        maxGapMs: st.gapN ? r2(st.gapMax) : null,
        meanGapMs: st.gapN ? r2(st.gapSum / st.gapN) : null,
        effectiveHz: (span !== null && span > 0 && st.armedSamples > 1)
          ? r1(((st.armedSamples - 1) / span) * 1000)
          : null,
      },
      sceneTimeline: st.sceneTimeline,
      controls: {
        preserveDrawingBuffer: st.preserve,
        glReady: !!st.gl,
        glSize: [st.glW, st.glH],
      },
      rdram: {
        base: st.rdramBase,
        whiteMax: st.rdramWhiteMax,
        flaggedFrames: st.rdramHitFrames,
      },
      truncated: st.truncated,
      inPageFrames: st.frames.length,
      errors: st.errors,
    };
  };

  const finish = (reason) => {
    if (st.done) return;
    st.done = true;
    st.disarmReason = reason;
    st.disarmMs = r1(performance.now());
    if (st.armTimer) {
      clearTimeout(st.armTimer);
      st.armTimer = 0;
    }
    if (st.raf) {
      cancelAnimationFrame(st.raf);
      st.raf = 0;
    }
    send({ kind: 'done', summary: report() });
  };

  // ---- arm / disarm --------------------------------------------------------
  const arm = (entry) => {
    st.armed = true;
    st.armMs = r1(performance.now());
    st.armWall = Date.now();
    st.armScene = entry.id;
    st.frames = st.preBuf.slice();
    st.preBuf = [];
    st.armTimer = setTimeout(() => finish('timeout'), st.timeoutMs);
    send({ kind: 'arm', armMs: st.armMs, scene: entry.id, pre: st.frames.length,
           mechanism: st.mechanism });
  };

  const noteDisarm = (entry) => {
    if (st.disarmScene !== null || st.stopAt !== null) return;
    st.disarmScene = entry.id;
    st.disarmLineMs = r1(performance.now());
    // No captures to count in --no-capture, so the line itself closes the window.
    if (st.noCapture || st.after <= 0) {
      finish(st.noCapture ? 'disarm-line' : 'after');
      return;
    }
    st.stopAt = st.distinct + st.after;
  };

  // ---- sample / emit -------------------------------------------------------
  const emit = (m) => {
    const pageNow = performance.now();
    const rec = {
      n: st.distinct + 1,
      pre: !st.armed,
      flushN: st.flushN,
      tPage: r1(pageNow),
      tWall: Date.now(),
      scene: st.lastScene ? st.lastScene.id : null,
      sceneMs: st.lastScene ? st.lastScene.t : null,
      taskN: st.taskN,
      w: m.w, h: m.h,
      sum3: m.sum3,
      mean3: m.mean3,
      meanPC: m.meanPC,
      fracW240: m.fracW240,
      fracW250: m.fracW250,
      fracNeutral: m.fracNeutral,
      nonBlack: m.nonBlack,
      colorful: m.colorful,
      checksum: m.checksum,
      white: m.white,
      candidate: m.candidate,
      dup: st.dup,
    };
    st.distinct++;
    st.lastKey = m.key;

    if (!st.armed) {
      st.preBuf.push(rec);
      while (st.preBuf.length > st.pre) st.preBuf.shift();
    } else {
      st.frames.push(rec);
      if (st.frames.length > 240) {
        st.frames.splice(0, st.frames.length - 240);
        st.truncated++;
      }
      st.armedDistinct++;
    }

    if (m.white) {
      st.whiteTotal++;
      if (st.armed) st.whiteArmed++;
    }
    if (m.candidate) {
      st.candTotal++;
      if (st.armed) st.candArmed++;
    }

    if (m.white || m.candidate) {
      if (st.kept < st.keep) {
        try {
          rec.png = document.getElementById('game-canvas').toDataURL('image/png');
          st.kept++;
        } catch (e) {
          rec.png = null;
          note('toDataURL: ' + e);
        }
      }
      if (st.gl) {
        try { rec.glError = st.gl.getError(); } catch (e) { rec.glError = null; }
      }
      const plate = countPlate();
      rec.rdramBase = st.rdramBase;
      rec.rdramWhite = plate ? plate.white : null;
      if (plate) {
        st.rdramHitFrames++;
        if (st.rdramWhiteMax === null || plate.white > st.rdramWhiteMax) {
          st.rdramWhiteMax = plate.white;
        }
      }
    }

    send({ kind: 'frame', rec });
    if (st.stopAt !== null && st.distinct >= st.stopAt) finish('after');
  };

  const preSampling = () => st.pre > 0 && (st.preFromRe ? st.preArmed : st.sawScene);

  const sample = () => {
    if (st.done) return;
    const m = readFrame();
    if (!m) return;
    st.samples++;
    // Coverage is measured on captures, not on records: a repeated frame is
    // still a sample, and counting it keeps maxGapMs a statement about cadence.
    if (st.armed) {
      const now = r1(performance.now());
      if (st.armedSamples === 0) {
        st.armedFirstPage = now;
      } else {
        const gap = now - st.prevPage;
        st.gapN++;
        st.gapSum += gap;
        if (gap > st.gapMax) st.gapMax = gap;
      }
      st.prevPage = now;
      st.armedSamples++;
    }
    if (st.lastKey !== '' && m.key === st.lastKey) {
      st.dup++;
      return;
    }
    emit(m);
  };

  // ---- mechanisms ----------------------------------------------------------
  const startRaf = () => {
    if (st.raf || st.done) return;
    st.mechanism = 'raf';
    const tick = () => {
      if (st.done) {
        st.raf = 0;
        return;
      }
      st.rafTicks++;
      if (st.armed || preSampling()) sample();
      st.raf = requestAnimationFrame(tick);
    };
    st.raf = requestAnimationFrame(tick);
  };

  const installSampler = () => {
    if (st.noCapture) {
      st.mechanism = 'no-capture';
      return { mechanism: st.mechanism, wrap: false, gl: false, preserve: null };
    }
    const orig = Module._ogre_gfx_flush;
    if (typeof orig !== 'function') {
      note('Module._ogre_gfx_flush is not a function; using requestAnimationFrame');
      startRaf();
      return {
        mechanism: st.mechanism, wrap: false, gl: !!ensureGl(), preserve: st.preserve,
        reason: 'Module._ogre_gfx_flush is not a function',
      };
    }
    const wrapper = function () {
      // Counted before the original runs, so the first scene line (logged from
      // inside the original) already sees a nonzero count.
      st.flushN++;
      const r = orig();
      try {
        // The rAF fallback owns sampling once it starts; a wrapper that is still
        // called by the page must not sample as well.
        if (!st.done && st.mechanism === 'flush-wrap' &&
            (st.armed || preSampling())) {
          const now = performance.now();
          if (now - st.lastCallMs >= 3) {
            st.lastCallMs = now;
            sample();
          } else {
            st.coalesced++;
          }
        }
      } catch (e) {
        note('sample: ' + e);
      }
      return r;
    };
    try {
      Module._ogre_gfx_flush = wrapper;
    } catch (e) {
      note('assigning Module._ogre_gfx_flush threw: ' + e);
    }
    if (Module._ogre_gfx_flush !== wrapper) {
      note('Module._ogre_gfx_flush is not writable; using requestAnimationFrame');
      startRaf();
      return {
        mechanism: st.mechanism, wrap: false, gl: !!ensureGl(), preserve: st.preserve,
        reason: 'the property is not writable',
      };
    }
    st.mechanism = 'flush-wrap';
    const gl = ensureGl();
    return {
      mechanism: st.mechanism, wrap: true, gl: !!gl, preserve: st.preserve,
      flushN: st.flushN, coalesced: st.coalesced,
    };
  };

  // ---- log observer (no polling, no full-log scans) -------------------------
  const SCENE_RE = /\[scene\] t=(\d+)ms id=0x([0-9A-Fa-f]{1,4})/;
  const CALLOC_RE = /\[debug\] wasm calloc\(\d+\) -> (0x[0-9A-Fa-f]+)/;
  const RSP_RE = /display list submitted \(frame (\d+)/;

  const onLines = (parts) => {
    if (!parts) return;
    for (let i = 0; i < parts.length; i++) {
      let line = parts[i];
      if (!line) continue;
      if (line.lastIndexOf('[stderr] ', 0) === 0) line = line.slice(9);

      if (st.rdramBase === null) {
        const c = CALLOC_RE.exec(line);
        if (c) st.rdramBase = parseInt(c[1], 16);
      }
      if (!st.sawGfxContext && line.indexOf('[web:gfx] WebGL2 context handed') !== -1) {
        st.sawGfxContext = true;
      }
      const rsp = RSP_RE.exec(line);
      if (rsp) st.taskN = parseInt(rsp[1], 10);

      const m = SCENE_RE.exec(line);
      if (!m) continue;
      const entry = {
        id: '0x' + m[2].toUpperCase().padStart(4, '0'),
        t: parseInt(m[1], 10),
        pageMs: r1(performance.now()),
        wallMs: Date.now(),
      };
      st.sceneTimeline.push(entry);
      if (st.sceneTimeline.length > 4000) {
        st.sceneTimeline.splice(0, st.sceneTimeline.length - 4000);
      }
      st.lastScene = entry;
      st.sawScene = true;
      if (!st.preArmed && st.preFromRe && st.preFromRe.test(line)) {
        st.preArmed = true;
      }

      // A wrap that installed but is never called cannot sample: switch to the
      // rAF loop at the first scene line, which is seconds after the interval
      // that calls the flush starts.
      if (!st.wrapChecked) {
        st.wrapChecked = true;
        if (!st.noCapture && st.mechanism === 'flush-wrap' && st.flushN === 0) {
          note('the flush wrap installed but was never called; using requestAnimationFrame');
          startRaf();
        }
      }

      if (st.done) continue;
      if (!st.armed) {
        if (st.armRe.test(line)) arm(entry);
      } else if (st.disarmScene === null && st.stopAt === null && st.disarmRe.test(line)) {
        noteDisarm(entry);
      }
    }
  };

  if (window.ogreLog && typeof window.ogreLog.onLine === 'function') {
    window.ogreLog.onLine(onLines);
    st.observer = true;
  }

  st.report = report;
  st.finish = finish;
  st.installSampler = installSampler;
  st.control = control;

  return {
    observer: st.observer,
    mechanism: st.mechanism,
    control,
    arm: st.armRe.source,
    disarm: st.disarmRe.source,
  };
};

// One line per distinct frame in `<prefix>-frames.txt`.
function frameLine(r) {
  const parts = [
    `n=${r.n}`,
    `t=${(r.tPage / 1000).toFixed(3)}s`,
    `page=${r.tPage}ms`,
    `flushN=${r.flushN}`,
    `scene=${r.scene === null || r.scene === undefined ? '?' : r.scene}`,
    `sceneMs=${r.sceneMs === null || r.sceneMs === undefined ? '?' : r.sceneMs}`,
    `taskN=${r.taskN === null || r.taskN === undefined ? '?' : r.taskN}`,
    `${r.w}x${r.h}`,
    `mean3=${r.mean3}`,
    `meanPC=${r.meanPC}`,
    `w240=${r.fracW240}`,
    `w250=${r.fracW250}`,
    `neutral=${r.fracNeutral}`,
    `nonBlack=${r.nonBlack}`,
    `colorful=${r.colorful}`,
    `ck=${r.checksum}`,
  ];
  if (r.white) parts.push('WHITE');
  else if (r.candidate) parts.push('CANDIDATE');
  if (r.pre) parts.push('pre');
  if (r.dup) parts.push(`dup=${r.dup}`);
  if (r.glError !== undefined && r.glError !== null) parts.push(`glError=0x${(r.glError >>> 0).toString(16)}`);
  if (r.rdramWhite !== undefined && r.rdramWhite !== null) parts.push(`rdramWhite=${r.rdramWhite}`);
  if (r.pngFile) parts.push(`png=${r.pngFile}`);
  return parts.join(' ');
}

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);

  const framesFile = `${prefix}-frames.jsonl`;
  const textFile = `${prefix}-frames.txt`;
  const pngPrefix = `${prefix}-w`;
  fs.writeFileSync(framesFile, '');
  fs.writeFileSync(textFile, '');

  let nodeFrames = 0;
  const whites = [];
  const pngs = [];
  let armMsg = null;
  let doneMsg = null;
  let armResolve = null;
  let doneResolve = null;
  const armPromise = new Promise((resolve) => { armResolve = resolve; });
  const donePromise = new Promise((resolve) => { doneResolve = resolve; });

  await page.exposeBinding('__fastOut', (src, msg) => {
    if (!msg || typeof msg !== 'object') return;
    if (msg.kind === 'frame') {
      const rec = Object.assign({}, msg.rec || {});
      const png = rec.png || null;
      delete rec.png;
      try {
        if (png && pngs.length < keep) {
          const name = `${pngPrefix}${String(pngs.length + 1).padStart(2, '0')}.png`;
          const body = String(png).indexOf(',') >= 0 ? String(png).split(',')[1] : '';
          fs.writeFileSync(name, Buffer.from(body, 'base64'));
          rec.pngFile = name;
          pngs.push(name);
        }
        fs.appendFileSync(framesFile, JSON.stringify(rec) + '\n');
        fs.appendFileSync(textFile, frameLine(rec) + '\n');
      } catch (e) {
        rec.writeError = String((e && e.message) || e);
      }
      nodeFrames++;
      if (rec.white) whites.push(rec);
      return;
    }
    if (msg.kind === 'arm') {
      armMsg = msg;
      if (armResolve) armResolve(msg);
      return;
    }
    if (msg.kind === 'note') {
      console.log('page: ' + msg.text);
      return;
    }
    if (msg.kind === 'done') {
      doneMsg = msg;
      if (doneResolve) doneResolve(msg);
    }
  });

  let install = null;
  let sampler = null;
  let fatal = null;
  let armWait = false;

  try {
    await h.gotoPage(page, cfg);
    install = await page.evaluate(INSTALL_PAGE, {
      arm: armSrc,
      disarm: disarmSrc,
      preFrom: preFromSrc,
      after,
      pre,
      keep,
      timeoutMs: timeoutSecs * 1000,
      noCapture,
    });
    if (!install) throw new Error('the page install returned nothing');
    if (install.fatal) throw new Error('page install: ' + install.fatal);
    if (!install.observer) {
      throw new Error('window.ogreLog.onLine is unavailable; the arm marker cannot be observed');
    }

    await h.loadRom(page, cfg);
    sampler = await page.evaluate(() => window.__fastmark.installSampler());
    console.log(`sampler: mechanism=${sampler.mechanism} wrap=${Boolean(sampler.wrap)} ` +
                `gl=${Boolean(sampler.gl)} preserveDrawingBuffer=${sampler.preserve}`);

    armWait = await h
      .withTimeout(armPromise.then(() => true), timeoutSecs * 1000, 'arm')
      .catch(() => false);
    if (armWait) {
      console.log(`armed at ${armMsg ? armMsg.armMs : '?'}ms scene=${armMsg ? armMsg.scene : '?'} ` +
                  `pre=${armMsg ? armMsg.pre : '?'}`);
      doneMsg = await h
        .withTimeout(donePromise, (timeoutSecs + 60) * 1000, 'done')
        .catch(() => null);
      if (!doneMsg) console.log('the page never reported done; harvesting its state');
    }
  } catch (e) {
    fatal = e;
  }

  // Harvest whatever the page still has. This runs on every path, so the page
  // log is written even when the run failed.
  let pageReport = null;
  let pageLog = '';
  try {
    pageReport = await page.evaluate(
      () => (window.__fastmark && window.__fastmark.report ? window.__fastmark.report() : null)
    );
  } catch (e) { /* page gone */ }
  try {
    pageLog = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : ''));
  } catch (e) { /* page gone */ }

  const summary = pageReport || (doneMsg && doneMsg.summary) || {};
  const whiteFrames = summary.whiteFrames === undefined ? whites.length : summary.whiteFrames;
  const armed = Boolean(summary.armMarkerSeen) || armWait ||
    Boolean(doneMsg && doneMsg.summary && doneMsg.summary.armMarkerSeen);
  const preserve = summary.controls
    ? summary.controls.preserveDrawingBuffer
    : (sampler ? sampler.preserve : null);

  // A context without preserveDrawingBuffer may hand back a cleared buffer, so
  // a captured series would describe nothing. Recording the attribute is not
  // enough: a sampled run that reads it false is a failed run.
  if (!fatal && !noCapture && armed && preserve === false) {
    fatal = new Error('the WebGL2 context has preserveDrawingBuffer=false; readPixels ' +
                      'would read a buffer the browser is free to clear');
  }

  let code;
  if (fatal) code = 1;
  else if (!armed) code = 2;
  else if (whiteFrames > 0) code = 3;
  else code = 0;

  const coverage = summary.coverage || {
    startPageMs: null, endPageMs: null, spanMs: null, samples: 0,
    distinctArmed: 0, maxGapMs: null, meanGapMs: null, effectiveHz: null,
  };

  const result = {
    probe: 'atmarkfast',
    url: cfg.url,
    rom: cfg.rom,
    arm: armSrc,
    disarm: disarmSrc,
    preFrom: preFromSrc,
    after,
    pre,
    keep,
    timeoutSecs,
    noCapture,
    timeBase: 'tPage is performance.now() of the page document, ms',
    mechanism: summary.mechanism || (sampler && sampler.mechanism) || 'unknown',
    armedMs: summary.armedMs === undefined ? null : summary.armedMs,
    disarmedMs: summary.disarmedMs === undefined ? null : summary.disarmedMs,
    armScene: summary.armScene === undefined ? null : summary.armScene,
    disarmScene: summary.disarmScene === undefined ? null : summary.disarmScene,
    disarmReason: summary.disarmReason === undefined ? null : summary.disarmReason,
    armMarkerSeen: armed,
    distinctFrames: summary.distinctFrames === undefined ? nodeFrames : summary.distinctFrames,
    armedFrames: summary.armedFrames === undefined ? null : summary.armedFrames,
    preFrames: summary.preFrames === undefined ? null : summary.preFrames,
    whiteFrames,
    whiteFramesArmed: summary.whiteFramesArmed === undefined ? null : summary.whiteFramesArmed,
    candidateFrames: summary.candidateFrames === undefined ? null : summary.candidateFrames,
    candidateFramesArmed:
      summary.candidateFramesArmed === undefined ? null : summary.candidateFramesArmed,
    coverage,
    sceneTimeline: summary.sceneTimeline || [],
    controls: {
      metric: install ? install.control : null,
      preserveDrawingBuffer: preserve,
      preserveDrawingBufferAssert: preserve === true,
      glReady: summary.controls ? summary.controls.glReady : (sampler ? sampler.gl : null),
      glSize: summary.controls ? summary.controls.glSize : null,
    },
    sampler: sampler || null,
    counts: {
      samples: summary.samples === undefined ? null : summary.samples,
      flushCalls: summary.flushCalls === undefined ? null : summary.flushCalls,
      rafTicks: summary.rafTicks === undefined ? null : summary.rafTicks,
      coalesced: summary.coalesced === undefined ? null : summary.coalesced,
      dupSamples: summary.dupSamples === undefined ? null : summary.dupSamples,
      streamedFrames: nodeFrames,
      inPageFrames: summary.inPageFrames === undefined ? null : summary.inPageFrames,
      truncated: summary.truncated === undefined ? null : summary.truncated,
    },
    rdram: summary.rdram || { base: null, whiteMax: null, flaggedFrames: 0 },
    pngs,
    pageErrors: summary.errors || [],
    exitCode: code,
  };
  if (fatal) result.fatal = String((fatal && fatal.stack) || fatal).slice(0, 4000);

  try {
    h.writeOut(prefix, 'pagelog.txt', pageLog || '');
  } catch (e) { /* nothing left to write to */ }
  const pageErrors = result.pageErrors || [];
  h.writeErrors(prefix, errors.concat(pageErrors.map((line) => 'PAGE: ' + line)));
  h.writeJson(prefix, 'result.json', result);

  console.log(`mechanism=${result.mechanism}, distinct=${result.distinctFrames}, ` +
              `white=${result.whiteFrames}, candidate=${result.candidateFrames}, ` +
              `armScene=${result.armScene || 'none'}, disarmScene=${result.disarmScene || 'none'}`);
  console.log(`  pre=${result.preFrames} armed=${result.armedFrames} ` +
              `samples=${result.counts.samples} flushCalls=${result.counts.flushCalls} ` +
              `coalesced=${result.counts.coalesced} dup=${result.counts.dupSamples} ` +
              `span=${coverage.spanMs}ms maxGap=${coverage.maxGapMs}ms hz=${coverage.effectiveHz} ` +
              `pngs=${pngs.length} reason=${result.disarmReason || 'none'}`);
  if (!noCapture && result.controls.preserveDrawingBuffer !== true) {
    console.log(`  preserveDrawingBuffer=${result.controls.preserveDrawingBuffer}`);
  }
  if (result.whiteFrames > 0 && whites.length === 0) {
    console.log(`  ${result.whiteFrames} white frames were counted but no white record reached Node`);
  }
  for (const w of whites) {
    console.log(`  WHITE n=${w.n} t=${(w.tPage / 1000).toFixed(3)}s page=${w.tPage}ms ` +
                `scene=${w.scene} sceneMs=${w.sceneMs} ${w.w}x${w.h} ` +
                `mean3=${w.mean3} meanPC=${w.meanPC} fracW240=${w.fracW240} ` +
                `fracW250=${w.fracW250} fracNeutral=${w.fracNeutral} ` +
                `nonBlack=${w.nonBlack} colorful=${w.colorful} ` +
                `rdramWhite=${w.rdramWhite === undefined ? 'n/a' : w.rdramWhite} ` +
                `png=${w.pngFile || 'none'}`);
  }
  if (code === 2) console.log(`the arm marker ${armSrc} never arrived`);
  if (fatal) console.error(String((fatal && fatal.stack) || fatal));

  await page.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(code);
})().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
