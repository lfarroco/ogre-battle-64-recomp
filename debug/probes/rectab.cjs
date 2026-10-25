#!/usr/bin/env node
'use strict';

// One screenshot of the mission briefing's framed illustration, for the
// `OGRE_RECT_HALFPIX` A/B.
//
// Run it twice with the same route and once with `OGRE_RECT_HALFPIX=0` in the
// query. The briefing is static, so the two PNGs are the same screen and can be
// compared pixel for pixel. The arrival signal is the `[GFX-FRAME-DUMP]` line
// the strip draw already prints (`OGRE_FRAME_DUMP=1`), not a timer: the browser
// reaches the scene after a boot whose wall-clock length varies with host load.
//
// Usage:
//   node debug/probes/rectab.cjs --out /tmp/rect-on
//   node debug/probes/rectab.cjs --out /tmp/rect-off --query '...&OGRE_RECT_HALFPIX=0'

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'rectab');
const secs = h.num(opts.secs, 400);
const dumpsWanted = h.num(opts.dumps, 2);
const query = String(
  opts.query ||
    'OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1' +
      '&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6&OGRE_FRAME_DUMP=1'
);

function withQuery(url, extra) {
  return url + (url.includes('?') ? '&' : '?') + extra;
}

/** Read a file out of the wasm filesystem as a Buffer (base64, chunked). */
async function readFsFile(page, path) {
  const b64 = await page.evaluate((p) => {
    const data = Module.FS.readFile(p);
    let out = '';
    const chunk = 0x8000;
    for (let i = 0; i < data.length; i += chunk) {
      out += String.fromCharCode.apply(null, data.subarray(i, i + chunk));
    }
    return btoa(out);
  }, path);
  return Buffer.from(b64, 'base64');
}

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);
  await h.gotoPage(page, { ...cfg, url: withQuery(cfg.url, query) });
  await h.loadRom(page, cfg);

  const t0 = Date.now();
  let dumps = 0;
  const samples = [];
  while (Date.now() - t0 < secs * 1000) {
    const state = await h.readState(page).catch(() => null);
    const lines = await page
      .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[GFX-FRAME-DUMP\]/) : []))
      .catch(() => []);
    dumps = lines.length;
    const scene = await page.evaluate(() => window.ogreLog.find(/id=0x0003/).length).catch(() => 0);
    samples.push(
      `t=${((Date.now() - t0) / 1000).toFixed(1)} dumps=${dumps} scene3=${scene}`
    );
    if (dumps >= dumpsWanted) break;
    await h.sleep(1000);
  }

  // Two presents: the first records the frame after the dump, the second
  // presents it.
  await h.sleep(800);
  const shot = `${prefix}-briefing.png`;
  await h.screenshotCanvas(page, shot).catch(() => null);

  const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, log + '\n');
  fs.writeFileSync(`${prefix}-samples.txt`, samples.join('\n') + '\n');
  h.writeErrors(prefix, errors);

  let copied = 0;
  for (let n = 0; n < dumps; n++) {
    for (const name of ['texsrc', 'tmem', 'texdec']) {
      const inside = `/tmp/${name}-${n}.bin`;
      const bytes = await readFsFile(page, inside).catch(() => null);
      if (!bytes) continue;
      fs.writeFileSync(`${prefix}-${name}-${n}.bin`, bytes);
      copied++;
    }
  }
  const frames = log
    .split('\n')
    .filter((l) => l.includes('[GFX-FRAME-RECT]') || l.includes('[GFX-FRAME-DUMP]'))
    .map((l) => l.replace(/^\[stderr\] /, ''));
  fs.writeFileSync(`${prefix}-dumps.txt`, frames.join('\n') + '\n');
  h.writeJson(prefix, 'result.json', { dumps, copied, shot });
  console.log(`FRAME DUMPS ${dumps}; copied ${copied} file(s); shot ${shot}`);
  await browser.close().catch(() => {});
  process.exit(dumps >= dumpsWanted ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
