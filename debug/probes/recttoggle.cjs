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
//   node debug/probes/recttoggle.cjs --out /tmp/recttoggle \
//     --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6,0x03:a:30'
//
// The arrival default is `[GFX-FRAME-DUMP]`, the line the briefing's framed
// illustration prints, and the probe adds `OGRE_FRAME_DUMP=1` when the URL
// does not. That line arrives while the picture is on screen; `--settle` is the
// short wait after it. The scene line (`id=0x0003`) is worse: the briefing
// changes its NOTE page a few seconds later, and an on/off pair taken across
// that change compares two screens. Exit 0 means all three frames were written.
//
// `--flag N` is the debug bit the second shot clears: 32768 (15) is
// `OGRE_RECT_HALFPIX` off, 8192 (13) is the framebuffer-as-texture copy off.
// `--name` prefixes the three files (default `halfpix`).
//
// The browser is launched with the host GPU (`--enable-gpu --use-angle=metal`,
// the same args `intro-capture.cjs` uses). The shared harness launch, without
// those args, runs the game on SwiftShader and takes 30+ minutes to reach the
// mission under host load; `--gpu 0` restores it.

const { chromium } = require('playwright');
const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'recttoggle');
const waitRe = opts.wait ? new RegExp(opts.wait) : /\[GFX-FRAME-DUMP\]/;
const settleMs = h.num(opts.settle, 800);
const timeoutSecs = h.num(opts.timeout, 500);
const gpu = opts.gpu === undefined ? true : String(opts.gpu) !== '0';
// 32768 (bit 15) is `OGRE_RECT_HALFPIX` off; 8192 (bit 13) is the
// framebuffer-as-texture copy off. `--name` labels the three files.
const flag = h.num(opts.flag, 32768);
const name = String(opts.name || 'halfpix');
const url = new URL(String(opts.url || cfg.url));
if (!url.searchParams.has('OGRE_FRAME_DUMP')) {
  url.searchParams.set('OGRE_FRAME_DUMP', '1');
}
const pageUrl = url.toString();

(async () => {
  const browser = await chromium.launch({
    headless: !cfg.headed,
    args: gpu ? ['--enable-gpu', '--use-angle=metal', '--ignore-gpu-blocklist'] : [],
  });
  const { page, errors } = await h.openPage(browser);

  await h.gotoPage(page, { ...cfg, url: pageUrl });
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

  // The dump line fires inside the frame that draws the framed illustration.
  // The screen holds that picture for a few seconds, so half a second after the
  // line is the same page for all three shots. The toggle takes effect on the
  // next submitted display list.
  await shot(`${name}-on`, settleMs);
  await page.evaluate((f) => Module._ogre_gfx_debug_flags(f), flag);
  await shot(`${name}-off`, 700);
  await page.evaluate(() => Module._ogre_gfx_debug_flags(0));
  await shot(`${name}-on2`, 700);

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
