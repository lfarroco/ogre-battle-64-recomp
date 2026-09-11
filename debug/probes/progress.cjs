#!/usr/bin/env node
'use strict';

// Intro-progress probe: boot, tap Start, and watch how far the *game* gets -
// the display-list counter and the renderer's runaway diagnostics - rather than
// whether a single frame rendered.
//
// `boot.cjs` stops at the first rendered frame, so it cannot answer "does the
// scene advance past beat N?". This probe keeps sampling for the whole budget,
// records the highest `tasks=` seen, and fails the attempt when the renderer
// reports a corrupt walk:
//
//   [GFX-ESCAPE]   the DL walker stepped outside the low 32 MiB of rdram after
//                  a G_DL, i.e. it followed a bad target (session 20's
//                  "runaway walk" - the session-21 segmentation bug).
//   [GFX-RUNAWAY]  the walk exhausted its 4M-command budget without G_ENDDL.
//
// Usage:
//   node debug/probes/progress.cjs [--attempts 6] [--secs 90] [--min-tasks 35]
//                                  [--out progress] [--headed]
//
// Exit codes: 0 = the game reached --min-tasks on some boot without a bad walk;
// 2 = it did not (see the written result.json), 1 = fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'progress');
// Session 20's renderer escaped the display list at ~task 31, so anything
// comfortably past that proves the walk is no longer going off the rails.
const want = h.num(opts['min-tasks'], 35);
// The runaway is a *renderer* symptom; match both diagnostics plus the frame
// milestones this probe reports on.
const RE = /GFX-(ESCAPE|RUNAWAY|TASK|DRAW|WORKLOAD)|display list submitted/;

(async () => {
  const browser = await h.launch(cfg);
  const attemptResults = [];

  const run = await h.attemptLoop(browser, cfg, async (attempt) => {
    let maxTasks = 0;
    let badWalk = null;
    let samples = 0;
    const res = await h.runAttempt(browser, cfg, {
      secs: cfg.secs,
      // Never stop on "a display list appeared": this probe wants progress.
      stopOnAdvanced: false,
      milestoneRe: RE,
      onSample: async (s) => {
        samples++;
        maxTasks = Math.max(maxTasks, s.tasks);
        const escaped = s.milestones.lines.find((l) => /GFX-(ESCAPE|RUNAWAY)/.test(l));
        if (escaped && badWalk === null) {
          badWalk = escaped;
        }
        // Done as soon as the target is reached or a bad walk is proven.
        return maxTasks >= want || badWalk !== null;
      },
    });
    const ok = maxTasks >= want && badWalk === null;
    console.log(`attempt ${attempt}: maxTasks=${maxTasks} badWalk=${badWalk ? 'yes' : 'no'} ` +
                `samples=${samples} reason=${res.reason}`);
    if (badWalk) {
      console.log(`  ${badWalk}`);
    }
    attemptResults.push({ attempt, maxTasks, badWalk, samples, reason: res.reason });
    return {
      ok,
      page: res.page,
      errors: res.errors,
      milestones: res.milestones,
      state: res.state,
      maxTasks,
      badWalk,
    };
  });

  const best = run.ok ? run : run.results[run.results.length - 1];
  h.writeStats(prefix, best.state);
  h.writeMilestones(prefix, best.milestones);
  h.writeOut(prefix, 'logtail.txt', best.state.tail || '(empty)');
  h.writeErrors(prefix, best.errors);
  h.writeJson(prefix, 'result.json', {
    reached: run.ok,
    wanted: want,
    attempts: attemptResults,
    counters: String(best.state.gfx || '').split('\n').filter((l) => /^(exec|load):/.test(l)),
  });

  await browser.close().catch(() => {});
  if (run.ok) {
    console.log(`REACHED tasks>=${want} on attempt ${run.attempt}`);
    process.exit(0);
  }
  console.log(`NO PROGRESS: best maxTasks=${attemptResults.reduce((m, r) => Math.max(m, r.maxTasks), 0)}`);
  process.exit(2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
