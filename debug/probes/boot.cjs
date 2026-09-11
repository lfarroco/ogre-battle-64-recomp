#!/usr/bin/env node
'use strict';

// Definitive render check: retry the boot until the game submits a real
// (non-boot) display list *and* the canvas actually has pixels on it, then save
// the canvas/page screenshots, the renderer counters, and the milestone lines.
//
// "Rendered" is measured, not assumed: the canvas PNG is decoded in the browser
// and the non-black / colorful pixel counts are reported.
//
// Usage:
//   node debug/probes/boot.cjs [--attempts 5] [--secs 60] [--min-pixels 200]
//                              [--out boot] [--rom PATH] [--url URL] [--headed]
//
// Exit codes: 0 rendered, 2 no render, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'boot');
const minPixels = h.num(opts['min-pixels'], 200);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);

  let captured = null; // the first attempt that put pixels on the canvas
  let lastErrors = [];

  const run = await h.attemptLoop(browser, cfg, async (attempt) => {
    const samples = [];
    let attemptBest = null; // best record from a run that at least reached a real DL

    const a = await h.runAttempt(browser, cfg, {
      // Keep sampling after the real display list arrives: the first frame is
      // often still black while the scene settles.
      stopOnAdvanced: false,
      onSample: async (s) => {
        const canvas = await h.screenshotCanvas(s.page);
        const px = canvas ? await h.analyzePng(decoder, canvas, { inset: 1 }).catch(() => null) : null;
        samples.push(`t=${(s.elapsed / 1000).toFixed(1)} taps=${s.taps} tasks=${s.tasks} px=${px ? JSON.stringify(px) : 'n/a'}`);
        const record = { canvas, px, tasks: s.tasks, taps: s.taps, state: s.state, milestones: s.milestones };
        if (px && px.nonBlack > minPixels) {
          attemptBest = record;
          await s.page.screenshot({ path: `${prefix}-page.png` }).catch(() => {});
          return true; // rendered - stop this attempt
        }
        if (!attemptBest && s.tasks >= cfg.minTasks) attemptBest = record;
        return false;
      },
    });

    h.writeOut(prefix, 'live.txt', `=== attempt ${attempt} ===\n` + samples.join('\n') + '\n');
    lastErrors = a.errors;
    const sawRealDl = h.taskCount(a.state.gfx) >= cfg.minTasks;
    await a.page.close().catch(() => {});

    const rendered = Boolean(attemptBest && attemptBest.px && attemptBest.px.nonBlack > minPixels);
    if (rendered && !captured) captured = { ...attemptBest, attempt };
    console.log(`attempt ${attempt}: sawRealDl=${sawRealDl} rendered=${rendered}`);
    return { ok: rendered, attemptBest, sawRealDl };
  });

  if (captured) {
    if (captured.canvas) fs.writeFileSync(`${prefix}-canvas.png`, captured.canvas);
    h.writeStats(prefix, captured.state);
    h.writeMilestones(prefix, captured.milestones);
    h.writeErrors(prefix, lastErrors);
    h.writeJson(prefix, 'result.json', {
      rendered: true,
      attempt: captured.attempt,
      tasks: captured.tasks,
      taps: captured.taps,
      pixels: captured.px,
    });
    console.log(`RENDERED ${JSON.stringify(captured.px)} (attempt ${captured.attempt})`);
  } else {
    const best = (run.results.find((r) => r.attemptBest) || {}).attemptBest;
    if (best) {
      h.writeStats(prefix, best.state);
      h.writeMilestones(prefix, best.milestones);
    } else {
      h.writeOut(prefix, 'gfxstats.txt', '(never reached a real display list)');
      h.writeMilestones(prefix, new h.MilestoneCollector());
    }
    h.writeErrors(prefix, lastErrors);
    h.writeJson(prefix, 'result.json', { rendered: false });
    console.log('NO RENDER');
  }

  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(captured ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
