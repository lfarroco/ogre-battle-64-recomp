#!/usr/bin/env node
'use strict';

// A canvas screenshot series (`--interval-ms`, default one second) from the
// first scene, in the browser build. The native counterpart is
// `tools/intro_capture.py`, which writes the same layout from RT64's
// render-target readback.
//
// Usage:
//   node debug/probes/intro-capture.cjs [--seconds 30] [--interval-ms 1000]
//        [--out DIR] [--url URL] [--scene NAME] [--attempts 3] [--gpu 0]
//        [--rom PATH]
//
// Writes `<out>/frame-NN.png` (NN = 00.. seconds*1000/interval-1) and
// `<out>/manifest.json`. The default interval is one second; a shorter one
// samples the same window more densely, which is how a hold of a few presents
// (the QUEST still) is measured instead of guessed from one sample.
//
// Definition of t=0: the first frame of the game's first scene, i.e. the first
// `[scene]` line whose id is neither 0x0000 (the boot's placeholder) nor
// 0x19E8 (the dispatcher's uninitialised read). The browser presents black
// frames for several seconds between `present 1` and that scene, and starting
// the series there would spend 30 s capturing the runtime coming up. Every
// frame is scheduled at t=0 + i seconds with an absolute deadline, so a slow
// capture does not shift the rest of the series; a main-thread stall that
// swallows a deadline is recorded in `missed` instead of pushing the following
// frame late.
//
// Why in-page and not `locator.screenshot()`: a Playwright element screenshot
// of the running canvas cost 12.9-13.5 s per grab under load (session 176),
// which cannot sample a 1 s cadence. `canvas.toDataURL` reads the preserved
// drawing buffer (`app/src/web_renderer.cpp`, `preserveDrawingBuffer = true`)
// in a few ms, and the loop calls `Module._ogre_gfx_flush()` first so the
// buffer holds the frame the renderer just drew.
//
// The pixel counters are computed from a 160x120 `drawImage` of the canvas at
// the moment of capture (the same sample `tools/intro_capture.py` takes from
// the native readback), because a black capture has been the failure mode
// three times in this port's history. `nonBlack` counts samples with any
// channel > 8/255, `colourful` those with max-min > 24/255, and `white` those
// with all three channels > 200/255.
//
// An idle boot (the harness's "idle trajectory": only the boot blanking
// display list, so the canvas stays black) is retried, because it is a boot
// race and not a captured state.
//
// Exit codes: 0 wrote a series, 2 the game never drew, 1 fatal.

const fs = require('fs');
const path = require('path');
const h = require('../lib/harness.cjs');
const { chromium } = require('playwright');

const { opts } = h.parseArgs();

// The page forwards every `OGRE_*` query parameter to the wasm environment, so
// the scene and the speed knobs are set here rather than in the page.
function withKnobs(rawUrl) {
  const url = new URL(rawUrl);
  url.searchParams.set('OGRE_SPEED', '1');
  url.searchParams.set('OGRE_SCENE_LOG', '1');
  if (scene) {
    url.searchParams.set('OGRE_SCENE', scene);
  }
  return url.toString();
}

const cfg = h.resolveConfig(opts);
const seconds = h.num(opts.seconds, 30);
const intervalMs = Math.max(20, h.num(opts['interval-ms'], 1000));
const attempts = h.num(opts.attempts, 3);
const gpu = opts.gpu === undefined ? true : String(opts.gpu) !== '0';
const scene = typeof opts.scene === 'string' && opts.scene ? opts.scene : null;

// The page forwards every `OGRE_*` query parameter to the wasm environment, so
// the speed and scene knobs are part of the URL the page is loaded from.
cfg.url = withKnobs(cfg.url);

const outDir = path.isAbsolute(opts.out || '')
  ? opts.out
  : path.join(cfg.outDir, opts.out || 'intro-web');
fs.mkdirSync(outDir, { recursive: true });

// Runs in the page. Returns { frames, missed } with no pixel data: every frame
// is handed to the Node side through `__frameOut` as it is taken, so the
// series never has to be serialised in one block.
async function captureSeries(page, count, intervalMs) {
  return await page.evaluate(async ({ count, intervalMs }) => {
    const frames = [];
    const missed = [];
    const tmp = document.createElement('canvas');
    tmp.width = 160;
    tmp.height = 120;
    const tctx = tmp.getContext('2d', { willReadFrequently: true });

    // 160x120 RGBA as base64, so the native side can lay the two series out in
    // one contact sheet without decoding the full PNG in Python.
    function base64(u8) {
      let s = '';
      for (let i = 0; i < u8.length; i += 0x8000) {
        s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
      }
      return btoa(s);
    }

    function sample(canvas) {
      try {
        tctx.drawImage(canvas, 0, 0, 160, 120);
        const d = tctx.getImageData(0, 0, 160, 120).data;
        let nonBlack = 0, colourful = 0, white = 0;
        for (let i = 0; i < d.length; i += 4) {
          const r = d[i], g = d[i + 1], b = d[i + 2];
          if (r > 8 || g > 8 || b > 8) nonBlack++;
          if (Math.max(r, g, b) - Math.min(r, g, b) > 24) colourful++;
          if (r > 200 && g > 200 && b > 200) white++;
        }
        return { nonBlack, colourful, white, thumb: base64(d) };
      } catch (e) {
        return { error: String(e) };
      }
    }

    function lastScene() {
      if (!window.ogreLog || typeof window.ogreLog.find !== 'function') return null;
      const lines = window.ogreLog.find(/\[scene\] t=\d+ms id=0x[0-9A-Fa-f]+/);
      if (!lines.length) return null;
      const m = /\[scene\] t=(\d+)ms id=0x([0-9A-Fa-f]+)/.exec(lines[lines.length - 1]);
      return m ? { ms: Number(m[1]), id: m[2] } : null;
    }

    const t0 = performance.now();
    let i = 0;
    return await new Promise((resolve) => {
      async function tick() {
        // The page's own flush timer runs every 16 ms; calling it here makes
        // the buffer current at the instant of the capture.
        try {
          if (typeof Module._ogre_gfx_flush === 'function') Module._ogre_gfx_flush();
        } catch (e) { /* the renderer is not up yet on the first frames */ }

        const now = performance.now() - t0;
        // A main-thread stall (wasm compile on the first seconds) can pass
        // several deadlines at once. Record them as missed and keep the rest
        // of the series on its absolute schedule.
        while (i < count && now >= (i + 1) * intervalMs) {
          missed.push(i);
          i++;
        }
        if (i < count && now >= i * intervalMs) {
          const canvas = document.getElementById('game-canvas');
          const frame = {
            i,
            tMs: now,
            w: canvas ? canvas.width : 0,
            h: canvas ? canvas.height : 0,
            scene: lastScene(),
            stats: canvas ? sample(canvas) : { nonBlack: 0, colourful: 0, white: 0 },
          };
          try {
            frame.data = canvas ? canvas.toDataURL('image/png') : null;
          } catch (e) {
            frame.data = null;
            frame.error = String(e);
          }
          try {
            await window.__frameOut(frame);
          } catch (e) { /* the Node side is gone; keep capturing */ }
          delete frame.data;
          frames.push(frame);
          i++;
        }
        if (i >= count) return resolve({ frames, missed });
        const next = t0 + i * intervalMs - performance.now();
        setTimeout(tick, Math.max(0, next));
      }
      tick();
    });
  }, { count, intervalMs });
}

async function runAttempt(browser, attempt) {
  const attemptDir = path.join(outDir, `.attempt-${attempt}`);
  fs.rmSync(attemptDir, { recursive: true, force: true });
  fs.mkdirSync(attemptDir, { recursive: true });

  const { page, errors } = await h.boot(browser, cfg);
  try {
    await page.exposeBinding('__frameOut', (src, payload) => {
      if (!payload || !payload.data) return;
      const file = path.join(attemptDir, `frame-${String(payload.i).padStart(2, '0')}.png`);
      fs.writeFileSync(file, Buffer.from(payload.data.split(',')[1], 'base64'));
    });

    const t0 = Date.now();
    // "(present 1)" is the first frame the presenter draws; everything before
    // it is the runtime coming up, with no game output.
    await h.waitForLog(page, '(present 1)', 60000);
    const firstPresentMs = Date.now() - t0;

    // The game's first scene is the first frame worth capturing. The wait is
    // bounded: a boot that never dispatches a scene is captured from the first
    // present instead, and the retry below catches it as an all-black series.
    await page.waitForFunction(() => {
      if (!window.ogreLog) return false;
      return window.ogreLog.find(/\[scene\] t=\d+ms id=0x(?!0000|19E8)[0-9A-Fa-f]+/).length > 0;
    }, null, { timeout: 25000 }).catch(() => {});
    const firstSceneMs = Date.now() - t0;

    const count = Math.max(1, Math.round(seconds * 1000 / intervalMs));
    const series = await captureSeries(page, count, intervalMs);
    const state = await h.readState(page).catch(() => ({ gfx: '', tail: '' }));
    const tasks = h.taskCount(state.gfx);
    // The whole scene timeline, not just the tail: `find` walks the log ring.
    const scenes = await page.evaluate(() => {
      if (!window.ogreLog) return [];
      return window.ogreLog.find(/\[scene\] t=\d+ms id=0x[0-9A-Fa-f]+/);
    }).catch(() => []);
    const nonBlack = series.frames.reduce((n, f) => n + (f.stats && f.stats.nonBlack > 0 ? 1 : 0), 0);
    return { series, tasks, scenes, errors, attemptDir, firstPresentMs, firstSceneMs, nonBlack, attempt };
  } finally {
    await page.close().catch(() => {});
  }
}

(async () => {
  const browser = await chromium.launch({
    headless: true,
    args: gpu ? ['--enable-gpu', '--use-angle=metal', '--ignore-gpu-blocklist'] : [],
  });

  let good = null;
  let last = null;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      last = await runAttempt(browser, attempt);
    } catch (e) {
      console.log(`attempt ${attempt}: failed: ${String(e).split('\n')[0]}`);
      continue;
    }
    const { series, tasks, nonBlack } = last;
    console.log(`attempt ${attempt}: ${series.frames.length} frames, ` +
                `${series.missed.length} missed, ${nonBlack} with pixels, tasks=${tasks}`);
    for (const f of series.frames) {
      const s = f.stats || {};
      console.log(`  frame ${String(f.i).padStart(2, '0')} t=${(f.tMs / 1000).toFixed(2)}s ` +
                  `${f.w}x${f.h} scene=${f.scene ? '0x' + f.scene.id : '?'} ` +
                  `nonBlack=${s.nonBlack} colourful=${s.colourful} white=${s.white}`);
    }
    // "All black" is the idle-trajectory boot race, not a captured state: the
    // boot submitted only its blanking display list and never ran the game.
    // A boot that submitted real display lists keeps its series even when the
    // window was too short to contain a drawn frame, because retrying would
    // only repeat the same shorter-than-the-intro window.
    if (nonBlack > 0 || tasks >= 2) {
      good = last;
      if (nonBlack === 0) {
        console.log(`attempt ${attempt}: kept, the game advanced (tasks=${tasks}) but no frame drew`);
      }
      break;
    }
    fs.rmSync(last.attemptDir, { recursive: true, force: true });
    console.log(`attempt ${attempt}: the game drew nothing, retrying`);
  }
  await browser.close().catch(() => {});

  if (!good) {
    console.log('NEVER DREW');
    process.exit(2);
  }

  // Promote the good attempt and delete the rest.
  for (const entry of fs.readdirSync(outDir)) {
    if (entry.startsWith('.attempt-') && path.join(outDir, entry) !== good.attemptDir) {
      fs.rmSync(path.join(outDir, entry), { recursive: true, force: true });
    }
  }
  for (const entry of fs.readdirSync(outDir)) {
    if (/^frame-\d+\.png$/.test(entry)) fs.rmSync(path.join(outDir, entry));
  }
  for (const f of good.series.frames) {
    const src = path.join(good.attemptDir, `frame-${String(f.i).padStart(2, '0')}.png`);
    if (fs.existsSync(src)) {
      fs.renameSync(src, path.join(outDir, `frame-${String(f.i).padStart(2, '0')}.png`));
    }
    f.file = `frame-${String(f.i).padStart(2, '0')}.png`;
  }
  fs.rmSync(good.attemptDir, { recursive: true, force: true });

  const manifest = {
    mode: 'web',
    url: withKnobs(cfg.url),
    rom: cfg.rom,
    scene,
    gpu,
    seconds,
    intervalMs,
    t0: 'first frame of the first scene',
    firstPresentMs: good.firstPresentMs,
    firstSceneMs: good.firstSceneMs,
    attempt: good.attempt,
    frames: good.series.frames,
    missed: good.series.missed,
    tasks: good.tasks,
    scenes: good.scenes,
    errors: good.errors.slice(0, 5),
  };
  fs.writeFileSync(path.join(outDir, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  console.log(`CAPTURED ${good.series.frames.length} frames in ${outDir}`);
})().catch((e) => {
  console.error('FATAL', e);
  process.exit(1);
});
