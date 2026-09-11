#!/usr/bin/env node
'use strict';

// Timed screenshot series: retry the boot until the game is on the advanced
// trajectory (a real display list), then capture the canvas every `--gap` ms so
// an animating scene can be judged without trusting a single capture moment.
// Each frame is also decoded for its non-black / colorful pixel counts.
//
// Usage:
//   node debug/probes/shots.cjs [--attempts 6] [--secs 60] [--shots 8]
//                               [--gap 1200] [--out shots] [--rom PATH] [--url URL]
//
// Exit codes: 0 captured a series, 2 never advanced, 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'shots');
const shots = h.num(opts.shots, 8);
const gap = h.num(opts.gap, 1200);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  let series = null;
  let lastErrors = [];

  await h.attemptLoop(browser, cfg, async (attempt) => {
    const a = await h.runAttempt(browser, cfg, { secs: cfg.secs });
    lastErrors = a.errors;
    const advanced = h.taskCount(a.state.gfx) >= cfg.minTasks;
    if (!advanced) {
      await a.page.close().catch(() => {});
      console.log(`attempt ${attempt}: idle trajectory (tasks=${h.taskCount(a.state.gfx)})`);
      return { ok: false };
    }

    const frames = [];
    for (let i = 0; i < shots; i++) {
      if (i > 0) await h.sleep(gap);
      const file = `${prefix}-${String(i).padStart(2, '0')}.png`;
      const canvas = await h.screenshotCanvas(a.page, file);
      const px = canvas ? await h.analyzePng(decoder, canvas, { inset: 1 }).catch(() => null) : null;
      const state = await h.readState(a.page).catch(() => a.state);
      frames.push({ i, tasks: h.taskCount(state.gfx), px });
      console.log(`shot ${i}: tasks=${h.taskCount(state.gfx)} nonBlack=${px ? px.nonBlack : 'n/a'}`);
    }
    series = { attempt, frames, state: a.state, milestones: a.milestones };
    await a.page.close().catch(() => {});
    return { ok: true, attempt, frames };
  });

  if (series) {
    h.writeOut(prefix, 'txt', series.frames
      .map((f) => `shot ${f.i} tasks=${f.tasks} px=${f.px ? JSON.stringify(f.px) : 'n/a'}`)
      .join('\n') + '\n');
    h.writeStats(prefix, series.state);
    h.writeMilestones(prefix, series.milestones);
    h.writeErrors(prefix, lastErrors);
    h.writeJson(prefix, 'result.json', { captured: true, attempt: series.attempt, frames: series.frames });
    console.log(`CAPTURED ${shots} frames (attempt ${series.attempt})`);
  } else {
    h.writeOut(prefix, 'txt', '(never advanced past the boot display list)\n');
    h.writeErrors(prefix, lastErrors);
    console.log('NEVER ADVANCED');
  }

  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(series ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
