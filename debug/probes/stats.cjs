#!/usr/bin/env node
'use strict';

// Counter dump: boot, tap Start until the game submits a real display list (or
// the time budget runs out), then write the full renderer counters, the status
// line, and the interesting log lines to disk for offline reading.
//
// Usage:
//   node debug/probes/stats.cjs [--secs 75] [--tap-ms 6000] [--out stats]
//                               [--rom PATH] [--url URL]
//
// Exit codes: 0 always (this is a dump; check the files), 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'stats');

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.boot(browser, cfg);
  const milestones = new h.MilestoneCollector();

  const t0 = Date.now();
  let taps = 0;
  let nextTap = t0 + cfg.tapMs;
  let state = await h.readState(page);
  let best = null;

  while (Date.now() - t0 < cfg.secs * 1000) {
    state = await h.readState(page).catch(() => state);
    milestones.feed(state.tail);
    if (h.taskCount(state.gfx) >= cfg.minTasks) best = state;
    if (Date.now() >= nextTap) {
      nextTap += cfg.tapMs;
      await h.tap(page);
      taps++;
    }
    await h.sleep(1000);
  }

  const final = best || state;
  h.writeStats(prefix, final);
  h.writeMilestones(prefix, milestones);
  h.writeOut(prefix, 'logtail.txt', final.tail || '(empty)');
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', {
    sawRealDl: Boolean(best),
    taps,
    status: final.status,
    counters: String(final.gfx || '').split('\n').filter((l) => /^(exec|load|dls|rej):/.test(l)),
  });

  console.log('taps=', taps, 'sawRealDl=', Boolean(best));
  console.log((String(final.gfx || '').match(/tasks=\d+.*/) || ['(no gfxstats)'])[0]);
  await browser.close().catch(() => {});
  process.exit(0);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
