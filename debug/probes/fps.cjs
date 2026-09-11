#!/usr/bin/env node
'use strict';

// Frame-rate measurement: retry the boot until the game is on the advanced
// trajectory, then sample the workload analyzer's `tasks=` counter over a
// window. The probe reads only #gfxstats, so the measurement cannot perturb the
// page the way the old per-line DOM logging did.
//
// Usage:
//   node debug/probes/fps.cjs [--attempts 8] [--window 10] [--secs 60]
//                             [--out fps] [--rom PATH] [--url URL]
//
// Exit codes: 0 measured, 2 never advanced, 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'fps');
const windowSecs = h.num(opts.window, 10);

function counters(gfx) {
  return String(gfx || '')
    .split('\n')
    .filter((l) => /^(exec|load|dls|rej):/.test(l))
    .join(' | ');
}

(async () => {
  const browser = await h.launch(cfg);
  let measured = null;
  let lastErrors = [];

  await h.attemptLoop(browser, cfg, async (attempt) => {
    const a = await h.runAttempt(browser, cfg, { secs: cfg.secs });
    lastErrors = a.errors;
    if (h.taskCount(a.state.gfx) < cfg.minTasks) {
      await a.page.close().catch(() => {});
      console.log(`attempt ${attempt}: idle trajectory (tasks=${h.taskCount(a.state.gfx)})`);
      return { ok: false };
    }

    const before = await h.readState(a.page);
    const t0 = Date.now();
    await h.sleep(windowSecs * 1000);
    const after = await h.readState(a.page).catch(() => before);
    const dt = (Date.now() - t0) / 1000;
    const frames = h.taskCount(after.gfx) - h.taskCount(before.gfx);
    measured = {
      attempt,
      windowSecs: dt,
      frames,
      fps: frames / dt,
      before: counters(before.gfx),
      after: counters(after.gfx),
      state: after,
      milestones: a.milestones,
    };
    console.log(`frames ${h.taskCount(before.gfx)} -> ${h.taskCount(after.gfx)} in ${dt.toFixed(1)}s = ${(frames / dt).toFixed(2)} fps`);
    await a.page.close().catch(() => {});
    return { ok: true };
  });

  if (measured) {
    h.writeStats(prefix, measured.state);
    h.writeMilestones(prefix, measured.milestones);
    h.writeErrors(prefix, lastErrors);
    h.writeJson(prefix, 'result.json', {
      fps: measured.fps,
      frames: measured.frames,
      windowSecs: measured.windowSecs,
      attempt: measured.attempt,
      countersBefore: measured.before,
      countersAfter: measured.after,
    });
    console.log(`FPS ${measured.fps.toFixed(2)}`);
  } else {
    h.writeErrors(prefix, lastErrors);
    console.log('NEVER ADVANCED');
  }

  await browser.close().catch(() => {});
  process.exit(measured ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
