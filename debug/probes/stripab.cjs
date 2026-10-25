#!/usr/bin/env node
'use strict';

// A/B/C of the mission intro's picture strips on one run and one screen.
//
// The strips' source image holds pure green texels with alpha 0
// (0x07C0 * 92x60, session 194), and the port's load and decode reproduce those
// bytes exactly. That leaves two explanations for the green lines on screen:
// the strip draws paint them anyway (blend/alpha path), or another draw paints
// them. Each frame below answers one question:
//
//   A  no flags          the default frame
//   B  flags 1           alpha blending off, so every combiner colour is
//                        written opaquely; the alpha-0 texels become visible
//                        if the strip draw is what covers them
//   C  flags 16          combiner bypassed, every draw outputs its own TEXEL0
//                        RGB (point sampled), which names the draw that owns
//                        the green pixels
//
// Usage:
//   node probes/stripab.cjs [--secs 240] [--out /tmp/stripab]

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'stripab');
const secs = h.num(opts.secs, 240);
const query = String(
  opts.query ||
    'OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1' +
      '&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6&OGRE_FRAME_DUMP=1'
);

function withQuery(url, extra) {
  return url + (url.includes('?') ? '&' : '?') + extra;
}

const setFlags = (flags) => {
  Module._ogre_gfx_debug_flags(flags);
  return flags;
};

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);
  await h.gotoPage(page, { ...cfg, url: withQuery(cfg.url, query) });
  await h.loadRom(page, cfg);

  const t0 = Date.now();
  let dumps = 0;
  while (Date.now() - t0 < secs * 1000) {
    dumps = await page
      .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[GFX-FRAME-DUMP\]/).length : 0))
      .catch(() => 0);
    if (dumps >= 2) break;
    await h.sleep(1000);
  }

  const shots = [];
  for (const [name, flags] of [
    ['a-default', 0],
    ['b-noablend', 1],
    ['c-texel0', 16],
  ]) {
    await page.evaluate(setFlags, flags);
    // Two flushes: the first records the frame with the new flags, the second
    // presents it.
    await h.sleep(900);
    const file = `${prefix}-${name}.png`;
    await h.screenshotCanvas(page, file).catch(() => null);
    shots.push(name);
    console.log(`${name}: flags=${flags}`);
  }
  await page.evaluate(setFlags, 0);

  const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  require('fs').writeFileSync(`${prefix}-pagelog.txt`, log + '\n');
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', { dumps, shots });
  console.log(`FRAME DUMPS ${dumps}; wrote ${shots.length} screenshot(s)`);
  await browser.close().catch(() => {});
  process.exit(shots.length === 3 ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
