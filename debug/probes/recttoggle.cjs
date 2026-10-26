#!/usr/bin/env node
'use strict';

// One screen, three frames: the rectangle half-step shift on, off, and on
// again, with all three taken in one run.
//
// `OGRE_RECT_HALFPIX` is read once when the renderer is constructed, so the
// build-wide A/B is two runs. Debug flag bit 15 (`ogre_gfx_debug_flags(32768)`)
// is read per draw and turns the shift off from the next draw on, which removes
// the run-to-run phase difference from the comparison. The briefing is static,
// so the on/off pair should show only the rectangle sampling change.
//
// Usage:
//   node debug/probes/recttoggle.cjs --wait 'id=0x0003' --settle 9000 --out /tmp/recttoggle \
//     --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6,0x03:a:30'
//
// `--wait` is the scene (or any log) regex the route ends at; `--settle` is the
// wait after it before the first frame, so the briefing's NOTE is up and the
// page has stopped changing. Exit 0 means all three frames were written.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'recttoggle');
const waitRe = opts.wait ? new RegExp(opts.wait) : /id=0x0003/;
const settleMs = h.num(opts.settle, 8000);
const timeoutSecs = h.num(opts.timeout, 500);
const url = String(opts.url || cfg.url);

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);

  await h.gotoPage(page, { ...cfg, url });
  await h.loadRom(page, cfg);

  const t0 = Date.now();
  let fired = false;
  while ((Date.now() - t0) / 1000 < timeoutSecs) {
    fired = await page
      .evaluate((re) => window.ogreLog.find(new RegExp(re)).length > 0, waitRe.source)
      .catch(() => false);
    if (fired) {
      break;
    }
    await page.waitForTimeout(200);
  }

  const shots = [];
  async function shot(name, waitMs) {
    await page.waitForTimeout(waitMs);
    const png = await h.screenshotCanvas(page);
    if (png) {
      fs.writeFileSync(`${prefix}-${name}.png`, png);
      shots.push(name);
    }
  }

  // The briefing redraws continuously; one second is more than a flush. The
  // settle wait covers the NOTE animation after the scene line.
  await shot('halfpix-on', settleMs);
  await page.evaluate(() => Module._ogre_gfx_debug_flags(32768));
  await shot('halfpix-off', 1500);
  await page.evaluate(() => Module._ogre_gfx_debug_flags(0));
  await shot('halfpix-on2', 1500);

  const log = await page.evaluate(() => window.ogreLog.text()).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, log);
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', { wait: waitRe.source, fired, shots });
  console.log(
    `${fired ? 'MARKER' : 'marker not seen'} ${waitRe.source}; shots ${shots.join(',')}`
  );
  await browser.close().catch(() => {});
  process.exit(shots.length === 3 ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
