#!/usr/bin/env node
'use strict';

// Capture a frame with only the RDP's fill rectangles drawn, so a fill colour
// can be read as pixels. `ogre_gfx_debug_flags` bit 1 stops *triangle* recording
// after three draws; `draw_fill_rect` is not gated, so a display list's whole
// fill sequence lands and the canvas shows the final fill state without the
// scene on top. The attract story is the worked example: its four fills end with
// the letterbox colour, which the scene then covers in a normal frame.
//
// Usage:
//   node debug/probes/fills.cjs --query 'OGRE_SCENE=story&OGRE_SCENE_LOG=1' \
//        --out /tmp/fills --secs 70
//
// Writes `<prefix>.png` (the canvas, 2x for a 320x240 VI mode) and
// `<prefix>-pagelog.txt`. The page log's `[GFX-RECT] fillrect` lines carry the
// decoded fill colour, the G_SETFILLCOLOR word, the colour image's size and the
// scissor, so the two are read together.

const fs = require('fs');
const path = require('path');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'fills');
const secs = h.num(opts.secs, 70);
const query = opts.query || '';

function withQuery(url, extra) {
  if (!extra) return url;
  return url + (url.includes('?') ? '&' : '?') + extra;
}

/** Set debug bit 1 and wait for a task that was executed after the change. */
const isolateFills = () => {
  const executed = () => {
    const m = /(?:^|\n)exec: task=(\d+)/.exec(document.getElementById('gfxstats').textContent);
    return m ? +m[1] : -1;
  };
  const before = executed();
  Module._ogre_gfx_debug_flags(2);
  return new Promise((resolve) => {
    let waited = 0;
    const tick = () => {
      waited += 250;
      if (executed() > before || waited >= 12000) {
        resolve({ before, after: executed(), waited });
      } else {
        setTimeout(tick, 250);
      }
    };
    tick();
  });
};

(async () => {
  const browser = await h.launch(cfg);
  const page = (await h.openPage(browser)).page;
  await h.gotoPage(page, { ...cfg, url: withQuery(cfg.url, query) });
  await h.loadRom(page, cfg);
  const t0 = Date.now();
  let shot = null;
  while (Date.now() - t0 < secs * 1000 && shot === null) {
    const state = await h.readState(page).catch(() => null);
    if (state && h.taskCount(state.gfx) >= 2) {
      const info = await page.evaluate(isolateFills).catch((e) => ({ error: String(e) }));
      console.log('isolated fills:', JSON.stringify(info));
      await h.sleep(1500);
      const png = await h.screenshotCanvas(page);
      if (png) {
        fs.writeFileSync(`${prefix}.png`, png);
        shot = path.resolve(`${prefix}.png`);
      }
    }
    await h.sleep(1000);
  }
  const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, log);
  await browser.close().catch(() => {});
  console.log(shot ? `OK ${shot}` : 'NO FRAME');
  process.exit(shot ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
