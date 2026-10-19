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
//                                  [--expect-hits N]
//
// `--flags N` calls `Module._ogre_gfx_debug_flags(N)` before the synthetic draw.
// `--flags 32` (bit 5) makes test_draw also render into a colour image and load
// a tile back out of that same address: the positive control for the
// framebuffer-as-texture diagnostic. `--flags 96` adds bit 6, two negative
// controls: a load from that image in the drawn rows but outside the drawn
// columns, and a load from a second image in a row its fill never drew (the row
// the pre-session-155 inversion reported). A rows-only test reports the first;
// an inversion one row up reports the second; the correct count is 1. With
// `--expect-fbtex` the probe exits 2 unless the run logged a `[GFX-FBTEX]` line,
// so a zero-hit scene sweep is told apart from a diagnostic that never fires;
// `--expect-hits N` additionally requires the exact `fbtex: hits=` count.
//
// `--flags 128` (bit 7) is the depth pipeline's control: two identical
// triangles at different depths, the NEAR one (red) drawn first and the FAR one
// (blue) second, with Z_CMP|Z_UPD in OTHERMODE_L. `--expect-depth near` requires
// the renderer's own `[GFX-DEPTH-TEST]` readback of that target to be red, i.e.
// the far triangle was rejected by the depth test. `--flags 384` (bits 7+8)
// clears Z_CMP and keeps Z_UPD; `--expect-depth far` then requires blue, which
// proves the assertion above is measuring the comparison and not draw order.
//
// `--flags 512` (bit 9) is the alpha-compare control: three identical
// rectangles with a combiner alpha of 0.2 on a black scratch target, drawn with
// G_AC_THRESHOLD and blend alpha 0 (rect 1, every fragment must pass),
// G_AC_THRESHOLD and blend alpha 1 (rect 2, every fragment must be discarded)
// and G_AC_DITHER (rect 3, a nonzero but partial count). `--expect-alpha`
// requires the renderer's own `[GFX-AC-TEST]` readback of each rectangle to
// match: `full`, `none` and `partial`.
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
const expectHits = opts['expect-hits'] === undefined ? null : h.num(opts['expect-hits'], 0);
const expectDepth = typeof opts['expect-depth'] === 'string' ? opts['expect-depth'] : null;
const expectAlpha = Boolean(opts['expect-alpha']);

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
  // The renderer's readback of the depth control's target: the last
  // `[GFX-DEPTH-TEST]` line carries `rgba=(r,g,b,a)`.
  const depthLines = [
    ...new Set(
      full
        .split('\n')
        .filter((l) => l.includes('GFX-DEPTH-TEST'))
        .map((l) => l.replace(/^\[stderr\] /, '').trim())
    ),
  ];
  const depthLast = depthLines.length ? depthLines[depthLines.length - 1] : '';
  const dm = /rgba=\((\d+),(\d+),(\d+),(\d+)\)/.exec(depthLast);
  const depthRgb = dm ? [Number(dm[1]), Number(dm[2]), Number(dm[3])] : null;
  // The alpha-compare control's three readbacks.
  const acLines = [
    ...new Set(
      full
        .split('\n')
        .filter((l) => l.includes('GFX-AC-TEST'))
        .map((l) => l.replace(/^\[stderr\] /, '').trim())
    ),
  ];
  const acCounts = [null, null, null, null];
  for (const l of acLines) {
    const m2 = /rect=(\d+).*lit=(\d+)\/(\d+)/.exec(l);
    if (m2) acCounts[Number(m2[1])] = { lit: Number(m2[2]), total: Number(m2[3]) };
  }
  const acVerdict = (i) => {
    const c = acCounts[i];
    if (!c) return 'missing';
    if (c.lit === 0) return 'none';
    if (c.lit === c.total) return 'full';
    return 'partial';
  };
  let alphaOk = true;
  if (expectAlpha) {
    const want = [null, 'full', 'none', 'partial'];
    for (let i = 1; i <= 3; ++i) {
      const got = acVerdict(i);
      if (got !== want[i]) {
        alphaOk = false;
        console.log(`FAIL: alpha control rect ${i} expected ${want[i]}, read ${got}`);
      }
    }
  }
  let depthOk = true;
  let depthSeen = 'none';
  if (expectDepth !== null) {
    if (!depthRgb) {
      depthOk = false;
      depthSeen = 'no [GFX-DEPTH-TEST] line';
    } else {
      const [r, g, b] = depthRgb;
      const nearWin = r > 200 && g < 60 && b < 60;   // red: the near triangle
      const farWin = b > 200 && r < 60 && g < 60;    // blue: the far triangle
      const got = nearWin ? 'near' : farWin ? 'far' : 'neither';
      depthSeen = `${got} rgb=(${r},${g},${b})`;
      depthOk = got === expectDepth;
    }
  }

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
    depthRgb,
    depthLines,
    acLines,
    acCounts: acCounts.slice(1),
  });

  // The screenshot can catch a later game present over the canvas (the boot's
  // blank frame is presented on every VI retrace), so the draw's own readback is
  // the second witness: `[GFX-AFTER]` reports the canvas right after the batch
  // that carried the synthetic list.
  const afterMatches = [...full.matchAll(/GFX-AFTER\] flush \d+: canvas after batch nonBlack=(\d+)\//g)];
  const afterLast = afterMatches.length ? Number(afterMatches[afterMatches.length - 1][1]) : 0;
  const rendered = Boolean((px && px.nonBlack > 0) || afterLast > 0);

  const hitsOk = expectHits === null || fbtexHits === expectHits;
  const ok = rendered && (!expectFbtex || fbtexLines.length > 0) && hitsOk && depthOk &&
             (!expectAlpha || alphaOk);
  console.log('test_draw exported=', r && r.has, 'returned=', r && r.ret, 'flags=', flags);
  console.log('canvas pixels:', px ? JSON.stringify(px) : 'n/a', `after-batch nonBlack=${afterLast}`);
  console.log(`fbtex hits=${fbtexHits} lines=${fbtexLines.length}`);
  for (const l of fbtexLines.slice(0, 4)) console.log('  ' + l);
  if (expectDepth !== null) {
    console.log(`depth control: expected ${expectDepth} to win, read ${depthSeen}`);
    for (const l of depthLines.slice(-2)) console.log('  ' + l);
  }
  if (expectFbtex && fbtexLines.length === 0) console.log('FAIL: expected a [GFX-FBTEX] line');
  if (!hitsOk) console.log(`FAIL: expected fbtex hits=${expectHits}, read ${fbtexHits}`);
  if (!depthOk) console.log(`FAIL: expected the ${expectDepth} triangle to win on the depth control`);
  if (expectAlpha) {
    console.log(`alpha control: threshold/full=${acVerdict(1)} threshold/none=${acVerdict(2)} ` +
                `dither/partial=${acVerdict(3)}`);
    for (const l of acLines.slice(-3)) console.log('  ' + l);
  }
  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
