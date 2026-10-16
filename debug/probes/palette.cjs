#!/usr/bin/env node
'use strict';

// Capture a scene's canvas plus the renderer's own `GFX-TEXTRACE` log, which
// now carries the TLUT entries a CI decode read and the four most common
// decoded colours. Used to compare what the browser decoded with what it drew.
//
// Usage:
//   node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_TEX_TRACE=1' \
//        --out /tmp/pal --secs 40 [--min-colorful 20000] [--shots 3]

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'palette');
const secs = h.num(opts.secs, 40);
const attempts = h.num(opts.attempts, 3);
const shots = h.num(opts.shots, 3);
const minColorful = h.num(opts['min-colorful'], 20000);
const query = opts.query || '';

function withQuery(url, extra) {
  if (!extra) return url;
  return url + (url.includes('?') ? '&' : '?') + extra;
}

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  let kept = 0;
  for (let attempt = 1; attempt <= attempts && kept < shots; attempt++) {
    const { page } = await h.openPage(browser);
    const url = withQuery(cfg.url, query);
    await h.gotoPage(page, { ...cfg, url });
    await h.loadRom(page, cfg);
    const t0 = Date.now();
    while (Date.now() - t0 < secs * 1000 && kept < shots) {
      const state = await h.readState(page).catch(() => null);
      const tasks = state ? h.taskCount(state.gfx) : -1;
      if (tasks >= 2) {
        const png = await h.screenshotCanvas(page);
        if (png) {
          const px = await h.analyzePng(decoder, png, { inset: 1 }).catch(() => null);
          if (px && px.colorful >= minColorful) {
            fs.writeFileSync(`${prefix}-${String(kept).padStart(2, '0')}.png`, png);
            console.log(`kept ${kept}: ${JSON.stringify(px)}`);
            kept++;
          }
        }
      }
      await h.sleep(1500);
    }
    const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
    fs.writeFileSync(`${prefix}-a${attempt}-pagelog.txt`, log);
    await page.close().catch(() => {});
  }
  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  console.log(kept ? `OK ${kept} frame(s)` : 'NO FRAME');
  process.exit(kept ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
