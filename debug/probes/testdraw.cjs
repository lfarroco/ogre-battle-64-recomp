#!/usr/bin/env node
'use strict';

// WebGL path isolation: call the exported synthetic draw
// (Module._ogre_gfx_test_draw) and capture the canvas. If this renders, the GL
// pipeline is fine and the game's display list is the problem; if it is black
// too, the GL path itself is broken.
//
// Usage:
//   node debug/probes/testdraw.cjs [--warmup 12000] [--settle 3000] [--out testdraw]
//                                  [--rom PATH] [--url URL]
//
// Exit codes: 0 test_draw rendered, 2 it did not (or is not exported), 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'testdraw');
const warmup = h.num(opts.warmup, 12000);
const settle = h.num(opts.settle, 3000);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  const { page, errors } = await h.boot(browser, cfg);

  // Let the runtime hand the canvas to the renderer and start flushing.
  await h.sleep(warmup);

  const r = await page.evaluate(() => {
    const has = typeof Module._ogre_gfx_test_draw === 'function';
    let ret = null;
    if (has) ret = Module._ogre_gfx_test_draw();
    return {
      has,
      ret,
      gfx: document.getElementById('gfxstats').textContent,
      gfxStatus: document.getElementById('gfx-status').textContent,
    };
  }).catch((e) => ({ err: String(e) }));

  await h.sleep(settle); // several flush ticks
  const canvas = await h.screenshotCanvas(page, `${prefix}-canvas.png`);
  const px = canvas ? await h.analyzePng(decoder, canvas, { inset: 1 }).catch(() => null) : null;
  const state = await h.readState(page).catch(() => ({ gfx: '', gfxStatus: '', status: '', tail: '' }));

  h.writeStats(prefix, state);
  h.writeOut(prefix, 'status.txt', state.tail || '');
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', { exported: Boolean(r && r.has), ret: r && r.ret, pixels: px });

  const ok = Boolean(px && px.nonBlack > 0);
  console.log('test_draw exported=', r && r.has, 'returned=', r && r.ret);
  console.log('canvas pixels:', px ? JSON.stringify(px) : 'n/a');
  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
