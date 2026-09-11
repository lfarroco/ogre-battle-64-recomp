'use strict';

// Shared harness for the headless-browser probes in debug/probes.
//
// Every probe does the same four things: launch Chromium, load the wasm build,
// feed it a ROM, and pump input until the game submits a *real* (non-boot)
// display list. This module owns that boilerplate plus the measurement helpers
// (state sampling, milestone accumulation, PNG pixel analysis) so a probe only
// has to describe what it measures.
//
// See docs/guides/web-probes.md for the page contract these probes rely on.

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const REPO_ROOT = path.resolve(__dirname, '..', '..');

function env(name, fallback) {
  const v = process.env[name];
  return v === undefined || v === '' ? fallback : v;
}

const DEFAULTS = {
  url: env('OGRE_URL', 'http://127.0.0.1:8931/app/web/index.html'),
  rom: env('OGRE_ROM', path.join(REPO_ROOT, 'assets', 'ogre64.z64')),
  outDir: env('OGRE_OUT', path.join(REPO_ROOT, 'debug', 'out')),
  // The workload analyzer prints `tasks=N` in #gfxstats; the boot task itself
  // is task 1, so >= 2 means the game submitted its own display list.
  minTasks: 2,
  attempts: 5,
  secs: 60,
  tapMs: 5000,
};

// Lines worth keeping from ogreLog. `[snap]` queue dumps are deliberately not
// matched: they are multi-KB and drown everything else.
const MILESTONE_RE = /GFX-|RSP\]|RENDERER|web:gfx|renderer busy|display list/;

const READY = 'WASM runtime initialized';
const BOOTED = 'Boot thread started';

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function num(value, fallback) {
  const n = parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

/** Parse `--key value`, `--key=value`, and `--flag` into an options object. */
function parseArgs(argv = process.argv.slice(2)) {
  const opts = {};
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith('--')) {
      positional.push(arg);
      continue;
    }
    const eq = arg.indexOf('=');
    if (eq !== -1) {
      opts[arg.slice(2, eq)] = arg.slice(eq + 1);
    } else if (i + 1 < argv.length && !argv[i + 1].startsWith('--')) {
      opts[arg.slice(2)] = argv[++i];
    } else {
      opts[arg.slice(2)] = true;
    }
  }
  return { opts, positional };
}

/** Merge CLI options over the environment defaults. */
function resolveConfig(opts = {}) {
  return {
    url: opts.url || DEFAULTS.url,
    rom: opts.rom || DEFAULTS.rom,
    outDir: opts['out-dir'] || DEFAULTS.outDir,
    minTasks: num(opts['min-tasks'], DEFAULTS.minTasks),
    attempts: num(opts.attempts, DEFAULTS.attempts),
    secs: num(opts.secs, DEFAULTS.secs),
    tapMs: num(opts['tap-ms'], DEFAULTS.tapMs),
    headed: Boolean(opts.headed),
  };
}

function launch(cfg = {}) {
  return chromium.launch({ headless: !cfg.headed });
}

/** Resolve the output prefix for a probe and make sure its directory exists.
 *  `--out name` is relative to the output directory; an absolute `--out /path/prefix`
 *  is used as-is. */
function outPrefix(cfg, name) {
  const prefix = path.isAbsolute(name) ? name : path.join(cfg.outDir, name);
  fs.mkdirSync(path.dirname(prefix), { recursive: true });
  return prefix;
}

function withTimeout(promise, ms, label) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('TIMEOUT:' + label)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function attachDiagnostics(page) {
  const errors = [];
  page.on('pageerror', (e) =>
    errors.push('PAGEERROR: ' + e.message + '\nSTACK: ' + String(e.stack || '(no stack)').slice(0, 3000)));
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push('CONSOLE: ' + m.text().slice(0, 300));
  });
  return errors;
}

async function openPage(browser) {
  const page = await browser.newPage();
  const errors = attachDiagnostics(page);
  return { page, errors };
}

async function waitForLog(page, needle, timeout = 30000) {
  await page.waitForFunction(
    (s) => window.ogreLog && window.ogreLog.contains(s), needle, { timeout });
}

/** Navigate to the build and wait for the wasm runtime to come up. */
async function gotoPage(page, cfg) {
  await page.goto(cfg.url);
  await waitForLog(page, READY);
}

/** Hand the ROM to the page's file input and wait for the boot thread. */
async function loadRom(page, cfg) {
  await page.setInputFiles('#rom-input', cfg.rom);
  await waitForLog(page, BOOTED);
}

/** Full boot: page up, ROM loaded, boot thread started. */
async function boot(browser, cfg) {
  const { page, errors } = await openPage(browser);
  await gotoPage(page, cfg);
  await loadRom(page, cfg);
  return { page, errors };
}

/** Read the renderer counters, the status line, and the recent log tail. */
async function readState(page) {
  return await page.evaluate(() => {
    const gfx = document.getElementById('gfxstats');
    const gfxStatus = document.getElementById('gfx-status');
    const status = document.getElementById('status');
    return {
      gfx: gfx ? gfx.textContent : '',
      gfxStatus: gfxStatus ? gfxStatus.textContent : '',
      status: status ? status.textContent : '',
      tail: window.ogreLog ? window.ogreLog.tail(4000) : '',
    };
  });
}

function taskCount(gfx) {
  const m = /tasks=(\d+)/.exec(gfx || '');
  return m ? parseInt(m[1], 10) : 0;
}

/** A single Start tap (down + up quickly) so the title branch samples button-up. */
async function tap(page, key = 'Enter') {
  await page.keyboard.press(key).catch(() => {});
}

async function screenshotCanvas(page, file) {
  const locator = page.locator('#game-canvas');
  return await (file ? locator.screenshot({ path: file }) : locator.screenshot()).catch(() => null);
}

/** Accumulates interesting log lines across samples so a tail slice cannot lose them. */
class MilestoneCollector {
  constructor(re = MILESTONE_RE) {
    this.re = re;
    this.lines = [];
    this.seen = new Set();
  }

  feed(text) {
    for (const line of String(text || '').split('\n')) {
      const key = line.replace(/^\[stderr\] /, '');
      if (this.re.test(key) && !this.seen.has(key)) {
        this.seen.add(key);
        this.lines.push(key);
      }
    }
  }

  get length() {
    return this.lines.length;
  }

  text() {
    return this.lines.join('\n');
  }
}

/**
 * Boot once and pump input until a stop condition.
 *
 * `hooks.onSample(sample)` runs every poll. Return `true` to stop early (for a
 * probe that has captured what it needs); otherwise the loop stops by itself
 * when the game submits a real display list (`tasks >= minTasks`), unless
 * `hooks.stopOnAdvanced` is `false` (use that when the first real frame may
 * still be black and the probe needs to keep sampling).
 *
 * Sample: { page, state, tasks, taps, elapsed, milestones, n }
 * Result: { page, errors, milestones, taps, stopped, reason, state, elapsed }
 */
async function runAttempt(browser, cfg, hooks = {}) {
  const { page, errors } = await boot(browser, cfg);
  const t0 = Date.now();
  const milestones = new MilestoneCollector(hooks.milestoneRe);
  const deadline = t0 + (hooks.secs || cfg.secs) * 1000;
  const minTasks = hooks.minTasks || cfg.minTasks;

  let taps = 0;
  let nextTap = t0 + cfg.tapMs;
  let n = 0;
  let state = { gfx: '', gfxStatus: '', status: '', tail: '' };
  let stopped = false;
  let reason = 'timeout';

  while (Date.now() < deadline) {
    state = await readState(page).catch(() => state);
    milestones.feed(state.tail);
    const tasks = taskCount(state.gfx);
    const sample = { page, state, tasks, taps, elapsed: Date.now() - t0, milestones, n: ++n };

    if (hooks.onSample && (await hooks.onSample(sample))) {
      stopped = true;
      reason = 'hook';
      break;
    }
    if (hooks.stopOnAdvanced !== false && tasks >= minTasks) {
      stopped = true;
      reason = 'advanced';
      break;
    }
    // Tap only until the game is alive: a capture probe wants the scene to run,
    // not to keep skipping it forward.
    if (tasks < minTasks && Date.now() >= nextTap) {
      nextTap += cfg.tapMs;
      await tap(page);
      taps++;
    }
    await sleep(hooks.pollMs || 1000);
  }

  return { page, errors, milestones, taps, stopped, reason, state, elapsed: Date.now() - t0 };
}

/**
 * Retry `attemptFn(attempt)` until it returns `{ ok: true }`.
 *
 * This is not optional politeness: roughly one in two boots lands on an idle
 * trajectory (stuck at ~3 display lists) and every probe that measures a real
 * frame must retry, not settle for "a display list appeared".
 */
async function attemptLoop(browser, cfg, attemptFn) {
  const results = [];
  for (let attempt = 1; attempt <= cfg.attempts; attempt++) {
    const result = await attemptFn(attempt);
    results.push({ attempt, ...result });
    if (result && result.ok) return { ok: true, attempt, ...result, results };
  }
  return { ok: false, results };
}

/** Off-screen page used to decode PNG screenshots into pixel statistics. */
async function pngAnalyzer(browser) {
  const page = await browser.newPage();
  await page.setContent('<img id="decode">');
  return page;
}

/** Decode a canvas screenshot in the browser and count what is actually drawn.
 *  Accepts a PNG Buffer (from a screenshot call) or an already-encoded base64 string.
 *
 *  `opts.inset` crops that many pixels off each edge before counting. Use
 *  `inset: 1` for `#game-canvas` screenshots: the element has a 1px CSS border,
 *  and its ~2 276 gray pixels otherwise read as a rendered frame on a black
 *  canvas (the real signal is `colorful`, since the border is grayscale). */
async function analyzePng(decoder, buffer, opts = {}) {
  const base64 = Buffer.isBuffer(buffer) ? buffer.toString('base64') : buffer;
  const inset = opts.inset || 0;
  return await withTimeout(decoder.evaluate(async ({ data, inset }) => {
    const img = document.getElementById('decode');
    img.src = 'data:image/png;base64,' + data;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.naturalWidth;
    c.height = img.naturalHeight;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0);
    const w = Math.max(0, c.width - inset * 2);
    const h = Math.max(0, c.height - inset * 2);
    const d = ctx.getImageData(inset, inset, w, h).data;
    let nonBlack = 0;
    let colorful = 0;
    const hist = {};
    for (let i = 0; i < d.length; i += 4) {
      const r = d[i], g = d[i + 1], b = d[i + 2];
      if (r || g || b) nonBlack++;
      if (Math.abs(r - g) + Math.abs(g - b) + Math.abs(r - b) > 24) colorful++;
      const k = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
      hist[k] = (hist[k] || 0) + 1;
    }
    const top = Object.entries(hist)
      .sort((x, y) => y[1] - x[1])
      .slice(0, 6)
      .map(([k, v]) => '#' + (+k).toString(16).padStart(3, '0') + 'x' + v);
    return { w, h, total: w * h, nonBlack, colorful, top };
  }, { data: base64, inset }), 15000, 'png');
}

function writeOut(prefix, suffix, data) {
  fs.writeFileSync(`${prefix}-${suffix}`, data);
}

function writeJson(prefix, suffix, value) {
  fs.writeFileSync(`${prefix}-${suffix}`, JSON.stringify(value, null, 2) + '\n');
}

function writeErrors(prefix, errors) {
  writeOut(prefix, 'errors.txt', errors && errors.length ? errors.join('\n---\n') : '(none)');
}

function writeStats(prefix, state) {
  writeOut(prefix, 'gfxstats.txt', (state.gfx || '') + '\n---\n' + (state.gfxStatus || ''));
}

function writeMilestones(prefix, collector) {
  writeOut(prefix, 'milestones.txt', collector.text());
}

module.exports = {
  REPO_ROOT,
  DEFAULTS,
  MILESTONE_RE,
  READY,
  BOOTED,
  parseArgs,
  num,
  resolveConfig,
  launch,
  outPrefix,
  withTimeout,
  attachDiagnostics,
  openPage,
  waitForLog,
  gotoPage,
  loadRom,
  boot,
  readState,
  taskCount,
  tap,
  screenshotCanvas,
  MilestoneCollector,
  runAttempt,
  attemptLoop,
  pngAnalyzer,
  analyzePng,
  writeOut,
  writeJson,
  writeErrors,
  writeStats,
  writeMilestones,
  sleep,
};
