#!/usr/bin/env node
'use strict';

// Hang characterisation: distinguish (a) the browser main thread being blocked,
// (b) the game/gfx worker being wedged, (c) the page merely being slow.
//
// Instruments the page with a main-thread heartbeat and a Module.print call
// counter, samples both every second while tapping Start, and runs a post-run
// forensics pass to see whether the main thread is alive at all.
//
// This probe deliberately does *not* retry the idle trajectory: it is for
// characterising a run, including a bad one.
//
// Usage:
//   node debug/probes/diag.cjs [--secs 45] [--tap-ms 5000] [--out diag]
//                              [--rom PATH] [--url URL]
//
// Exit codes: 0 always (read the live file), 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'diag');
const live = `${prefix}-live.txt`;

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);

  // Heartbeat installed before any page script runs.
  await page.addInitScript(() => {
    window.__hb = 0;
    window.__hbMaxGap = 0;
    window.__hbLast = Date.now();
    setInterval(() => {
      const now = Date.now();
      const gap = now - window.__hbLast;
      if (gap > window.__hbMaxGap) window.__hbMaxGap = gap;
      window.__hbLast = now;
      window.__hb++;
    }, 100);
  });

  await h.gotoPage(page, cfg);
  await h.loadRom(page, cfg);

  // Count Module.print calls (worker stdout liveness).
  await page.evaluate(() => {
    window.__pc = 0;
    window.__printRate = 0;
    if (window.Module && typeof window.Module.print === 'function') {
      const orig = window.Module.print;
      window.Module.print = function (t) {
        window.__pc++;
        return orig.call(window.Module, t);
      };
    }
    setInterval(() => {
      const n = window.__pc;
      setTimeout(() => { window.__printRate = window.__pc - n; }, 1000);
    }, 1000);
  });

  const lines = [];
  const append = (s) => {
    lines.push(s);
    require('fs').writeFileSync(live, `=== diag ${new Date().toISOString()} ===\n` + lines.join('\n') + '\n');
  };

  const t0 = Date.now();

  async function sample(tag) {
    const res = { tag, hb: null, hbGap: null, pc: null, printRate: null, status: null };
    try {
      const probe = await h.withTimeout(page.evaluate(() => ({
        hb: window.__hb, hbGap: window.__hbMaxGap, pc: window.__pc, printRate: window.__printRate,
      })), 4000, 'js');
      Object.assign(res, probe);
    } catch (e) {
      res.hb = 'JS-TIMEOUT(' + e.message + ')';
    }
    const state = await h.withTimeout(h.readState(page), 6000, 'state').catch(() => null);
    res.status = state ? (state.status || '').length : 'STATE-TIMEOUT';
    append(`--- ${tag} t=${((Date.now() - t0) / 1000).toFixed(1)}s statusBytes=${res.status} hb=${res.hb} hbGap=${res.hbGap} printCalls=${res.pc} printRate=${res.printRate}`);
    if (state) {
      const interesting = String(state.tail || '').split('\n')
        .filter((l) => /titledisp|framedisp|hotloop|resumes|thread t|game:|cursors|retrace|heapchk|heapB|alloctrace|alloctrans|audiocb|heapwalk|SAMPLE|WEDGE|GFX-|RSP\]/.test(l))
        .slice(-40);
      if (interesting.length) append(interesting.join('\n'));
    }
    return res;
  }

  let taps = 0;
  let seq = 0;
  let wedged = false;
  let nextTap = t0 + cfg.tapMs;
  await sample('boot');

  while (Date.now() - t0 < cfg.secs * 1000) {
    await h.sleep(1000);
    if (Date.now() - t0 >= cfg.secs * 1000) break;
    const r = await sample('s' + (++seq));
    if (r.status === 'STATE-TIMEOUT' && typeof r.hb === 'string') {
      wedged = true;
      break;
    }
    if (Date.now() < nextTap) continue;
    nextTap += cfg.tapMs;
    try {
      await h.withTimeout(page.keyboard.press('Enter'), 5000, 'pressEnter');
      taps++;
      if (taps % 2 === 0) await h.withTimeout(page.keyboard.press('x'), 5000, 'pressX');
    } catch (e) {
      append(`--- INPUT FAILED at t=${((Date.now() - t0) / 1000).toFixed(1)}s after ${taps} taps: ${e.message}`);
      wedged = true;
      break;
    }
  }

  append(`--- POST-RUN forensics (taps=${taps}, wedged=${wedged})`);
  for (let i = 0; i < 3; i++) {
    try {
      const v = await h.withTimeout(page.evaluate(() => ({ hb: window.__hb, hbGap: window.__hbMaxGap, pc: window.__pc })), 5000, 'post');
      append(`--- POST ${i}: main thread ALIVE hb=${v.hb} hbGap=${v.hbGap} printCalls=${v.pc}`);
    } catch (e) {
      append(`--- POST ${i}: main thread UNRESPONSIVE (${e.message})`);
    }
    await h.sleep(1000);
  }

  h.writeErrors(prefix, errors);
  console.log('taps:', taps, 'wedged:', wedged);
  console.log('live file:', live);
  await browser.close().catch(() => {});
  process.exit(0);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
