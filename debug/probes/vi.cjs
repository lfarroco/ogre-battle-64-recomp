#!/usr/bin/env node
'use strict';

// Runtime pacing: retry the boot until the game is on the advanced trajectory,
// briefly enable the runtime's debug traces, and measure how many VI retraces /
// buffer swaps it delivers per second against the game's gfx-frame rate.
//
// Session 20 baseline: 29.4 retrace/s (target 60) and 3.5 gfx frames/s.
//
// Usage:
//   node debug/probes/vi.cjs [--attempts 8] [--window 4000] [--secs 60]
//                            [--out vi] [--rom PATH] [--url URL]
//
// Exit codes: 0 measured, 2 never advanced, 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'vi');
const windowMs = h.num(opts.window, 4000);

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

    const t0 = Date.now();
    const enabled = await a.page.evaluate(() => {
      if (typeof Module !== 'undefined' && typeof Module._ogre_set_trace_enabled === 'function') {
        Module._ogre_set_trace_enabled(1);
        return true;
      }
      return false;
    }).catch(() => false);
    if (!enabled) {
      console.log('ogre_set_trace_enabled is not exported by this build; cannot measure VI rate');
      await a.page.close().catch(() => {});
      return { ok: false };
    }

    await h.sleep(windowMs);
    const res = await a.page.evaluate(() => {
      const txt = window.ogreLog.text();
      Module._ogre_set_trace_enabled(0);
      const count = (re) => (txt.match(re) || []).length;
      return {
        retrace: count(/\[vi-debug\] retrace ->/g),
        setevent: count(/\[vi-debug\] osViSetEvent/g),
        swap: count(/\[vi-debug\] osViSwapBuffer/g),
        snaps: count(/\[snap\]/g),
        tasks: (document.getElementById('gfxstats').textContent.match(/tasks=(\d+)/) || [, '0'])[1],
        tailLen: txt.length,
      };
    }).catch((e) => ({ err: e.message }));
    const dt = (Date.now() - t0) / 1000;
    await h.sleep(500); // let the disable call land in the log before closing

    measured = {
      attempt,
      windowSecs: dt,
      ...res,
      retracePerSec: res.retrace / dt,
      swapPerSec: res.swap / dt,
      snapPerSec: res.snaps / dt,
      state: a.state,
      milestones: a.milestones,
    };
    console.log(`window=${dt.toFixed(1)}s ${JSON.stringify(res)}`);
    console.log(`  -> retrace/s = ${(res.retrace / dt).toFixed(1)}, swap/s = ${(res.swap / dt).toFixed(2)}, snap/s = ${(res.snaps / dt).toFixed(2)}`);
    await a.page.close().catch(() => {});
    return { ok: true };
  });

  if (measured) {
    h.writeStats(prefix, measured.state);
    h.writeMilestones(prefix, measured.milestones);
    h.writeErrors(prefix, lastErrors);
    h.writeJson(prefix, 'result.json', measured);
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
