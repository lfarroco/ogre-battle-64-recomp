#!/usr/bin/env node
'use strict';

// WebGL path isolation: call the exported synthetic draw
// (Module._ogre_gfx_test_draw) and capture the canvas. If this renders, the GL
// pipeline is fine and the game's display list is the problem; if it is black
// too, the GL path itself is broken.
//
// Usage:
//   node debug/probes/testdraw.cjs [--warmup 12000] [--settle 3000] [--out testdraw]
//                                  [--rom PATH] [--url URL] [--flags N] [--expect-fbtex]
//
// `--flags N` calls `Module._ogre_gfx_debug_flags(N)` before the synthetic draw.
// `--flags 32` (bit 5) makes test_draw also render into a colour image and load
// a tile back out of that same address: the positive control for the
// framebuffer-as-texture diagnostic. With `--expect-fbtex` the probe exits 2
// unless the run logged a `[GFX-FBTEX]` line, so a zero-hit scene sweep is told
// apart from a diagnostic that never fires.
//
// Exit codes: 0 test_draw rendered (and, with --expect-fbtex, the control fired),
// 2 it did not, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'testdraw');
const warmup = h.num(opts.warmup, 12000);
const settle = h.num(opts.settle, 3000);
const flags = h.num(opts.flags, 0);
const expectFbtex = Boolean(opts['expect-fbtex']);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  const { page, errors } = await h.boot(browser, cfg);

  // Let the runtime hand the canvas to the renderer and start flushing.
  await h.sleep(warmup);

  const r = await page.evaluate((f) => {
    const has = typeof Module._ogre_gfx_test_draw === 'function';
    if (has && typeof Module._ogre_gfx_debug_flags === 'function') {
      Module._ogre_gfx_debug_flags(f);
    }
    let ret = null;
    if (has) ret = Module._ogre_gfx_test_draw();
    return {
      has,
      ret,
      gfx: document.getElementById('gfxstats').textContent,
      gfxStatus: document.getElementById('gfx-status').textContent,
    };
  }, flags).catch((e) => ({ err: String(e) }));

  await h.sleep(settle); // several flush ticks
  const canvas = await h.screenshotCanvas(page, `${prefix}-canvas.png`);
  const px = canvas ? await h.analyzePng(decoder, canvas, { inset: 1 }).catch(() => null) : null;
  const state = await h.readState(page).catch(() => ({ gfx: '', gfxStatus: '', status: '', tail: '' }));
  const full = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, full);
  const fbtexLines = [
    ...new Set(
      full
        .split('\n')
        .filter((l) => l.includes('GFX-FBTEX'))
        .map((l) => l.replace(/^\[stderr\] /, '').trim())
    ),
  ];
  const m = /fbtex: hits=(\d+)/.exec(state.gfx || '');
  const fbtexHits = m ? parseInt(m[1], 10) : 0;

  h.writeStats(prefix, state);
  h.writeOut(prefix, 'status.txt', state.tail || '');
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', {
    exported: Boolean(r && r.has),
    ret: r && r.ret,
    pixels: px,
    flags,
    fbtexHits,
    fbtexLines,
  });

  const ok = Boolean(px && px.nonBlack > 0) && (!expectFbtex || fbtexLines.length > 0);
  console.log('test_draw exported=', r && r.has, 'returned=', r && r.ret, 'flags=', flags);
  console.log('canvas pixels:', px ? JSON.stringify(px) : 'n/a');
  console.log(`fbtex hits=${fbtexHits} lines=${fbtexLines.length}`);
  for (const l of fbtexLines.slice(0, 4)) console.log('  ' + l);
  if (expectFbtex && fbtexLines.length === 0) console.log('FAIL: expected a [GFX-FBTEX] line');
  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
