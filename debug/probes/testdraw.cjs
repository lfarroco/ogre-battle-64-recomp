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
// `--flags 1024` (bit 10) is the writeback-dither control: two 320x240 RGBA16
// images, each filled with 0x214D, one drawn with G_CD_MAGICSQ and one with
// G_CD_DISABLE. `--expect-dither` requires the renderer's own
// `[GFX-DITHER-TEST]` readbacks to show the magic-square pattern on the first
// (both 0x214D and 0x298F present, nothing else) and only 0x214D on the second.
//
// `--flags 2048` (bit 11) marks the synthetic list's texture uploads, and
// `--repeat N` calls test_draw N times without yielding so the lists share one
// flush batch. `--gap MS` yields between calls so each list lands in its own
// flush and the age-windowed GL texture cache is what keeps the upload.
// `--expect-texcache` requires the renderer's `[GFX-TEXPROBE]` lines to show
// one decode miss, one upload, and N-1 decode hits and reuses of the same
// content key.
//
// `--flags 4096` (bit 12) makes each call write a different red texel, so every
// call has a different source. `--flags 6144 --repeat N --gap 64
// --expect-sourcechange` (bit 12 plus bit 11 for the probe lines) is the
// negative control: N distinct content keys, N decode misses, no hit, which is
// what a source key missing a byte would fail.
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
const expectDither = Boolean(opts['expect-dither']);
const expectTexcache = Boolean(opts['expect-texcache']);
const expectSourceChange = Boolean(opts['expect-sourcechange']);
const repeat = h.num(opts.repeat, 1);
const gap = h.num(opts.gap, 0);

(async () => {
  const browser = await h.launch(cfg);
  const decoder = await h.pngAnalyzer(browser);
  const { page, errors } = await h.boot(browser, cfg);

  // Let the runtime hand the canvas to the renderer and start flushing.
  await h.sleep(warmup);

  // `repeat > 1` calls the synthetic draw several times without yielding, so the
  // lists land in one flush batch and their identical tile images exercise the
  // content-addressed cache across display lists (the per-list dedupe would
  // hide a same-list duplicate). `--gap MS` yields between the calls instead,
  // so each list lands in its own flush and the same test exercises the
  // age-windowed GL texture cache rather than the same-batch reuse.
  const r = await page.evaluate(async ({ f, n, gap }) => {
    const has = typeof Module._ogre_gfx_test_draw === 'function';
    if (has && typeof Module._ogre_gfx_debug_flags === 'function') {
      Module._ogre_gfx_debug_flags(f);
    }
    let ret = null;
    for (let i = 0; i < n; ++i) {
      if (has) ret = Module._ogre_gfx_test_draw();
      if (gap > 0 && i + 1 < n) {
        await new Promise((resolve) => setTimeout(resolve, gap));
      }
    }
    return {
      has,
      ret,
      gfx: document.getElementById('gfxstats').textContent,
      gfxStatus: document.getElementById('gfx-status').textContent,
    };
  }, { f: flags, n: repeat, gap }).catch((e) => ({ err: String(e) }));

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
  // The writeback-dither control's two readbacks. `no_dither`/`dithered` count
  // the expected 16-bit words; `other` must be 0.
  const ditherLines = [
    ...new Set(
      full
        .split('\n')
        .filter((l) => l.includes('GFX-DITHER-TEST'))
        .map((l) => l.replace(/^\[stderr\] /, '').trim())
    ),
  ];
  const dither = {};
  for (const l of ditherLines) {
    const m3 = /probe=(\d+) pattern=(\d+) wrote=(\d+) no_dither=(\d+) dithered=(\d+) other=(\d+)/.exec(l);
    if (m3) {
      dither[Number(m3[1])] = {
        pattern: Number(m3[2]),
        wrote: Number(m3[3]) === 1,
        noDither: Number(m3[4]),
        dithered: Number(m3[5]),
        other: Number(m3[6]),
      };
    }
  }
  let ditherOk = true;
  if (expectDither) {
    const on = dither[1];
    const off = dither[2];
    ditherOk = Boolean(on && off &&
                       on.wrote && on.pattern === 0 && on.noDither > 0 &&
                       on.dithered > 0 && on.other === 0 &&
                       off.wrote && off.pattern === 3 && off.noDither > 0 &&
                       off.dithered === 0 && off.other === 0);
    if (!ditherOk) {
      console.log('FAIL: writeback dither control', JSON.stringify(dither));
    }
  }
  // Keep every occurrence (the same key is uploaded once and reused N-1
  // times); only the `[stderr]` copy is counted, because the page log also
  // mirrors each line a second time without the prefix.
  const texProbeLines = full
    .split('\n')
    .filter((l) => l.includes('[stderr]') && l.includes('GFX-TEXPROBE'))
    .map((l) => l.replace(/^\[stderr\] /, '').trim());
  const texProbe = texProbeLines.map((l) => {
    const m4 = /key=(0x[0-9A-Fa-f]+) (\d+)x(\d+) result=(\w+)(?: decode=(\w+))?/.exec(l);
    return m4
      ? {
          key: m4[1],
          w: Number(m4[2]),
          h: Number(m4[3]),
          result: m4[4],
          decode: m4[5] || 'unknown',
        }
      : null;
  }).filter(Boolean);
  let texcacheOk = true;
  if (expectTexcache) {
    const uploads = texProbe.filter((p) => p.result === 'upload').length;
    const reuses = texProbe.filter((p) => p.result === 'reuse').length;
    const decodeHits = texProbe.filter((p) => p.decode === 'hit').length;
    texcacheOk = uploads >= 1 && reuses >= Math.max(0, repeat - 1) &&
                 decodeHits >= Math.max(0, repeat - 1);
    if (!texcacheOk) {
      console.log(`FAIL: texture cache expected 1 upload + ${repeat - 1} reuse + ` +
                  `${repeat - 1} decode hit, read ${uploads} upload / ${reuses} reuse / ` +
                  `${decodeHits} decode hit`);
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
    ditherLines,
    dither,
    texProbeLines,
    texProbe,
  });

  // The screenshot can catch a later game present over the canvas (the boot's
  // blank frame is presented on every VI retrace), so the draw's own readback is
  // the second witness: `[GFX-AFTER]` reports the canvas right after the batch
  // that carried the synthetic list.
  const afterMatches = [...full.matchAll(/GFX-AFTER\] flush \d+: canvas after batch nonBlack=(\d+)\//g)];
  const afterLast = afterMatches.length ? Number(afterMatches[afterMatches.length - 1][1]) : 0;
  // Several flushes follow the synthetic batch (the game keeps presenting), and
  // the last `[GFX-AFTER]` is usually a later game frame with nothing on the
  // canvas. The test drew if any flush after the batch reported content, or if
  // an offscreen control's own readback proved its target was written.
  const afterMax = afterMatches.length
    ? Math.max(...afterMatches.map((m) => Number(m[1])))
    : 0;
  const rendered = Boolean((px && px.nonBlack > 0) || afterMax > 0 ||
                           (expectDither && ditherOk) || (expectDepth && depthOk) ||
                           (expectAlpha && alphaOk));

  // Source-change negative control (`--flags 4096 --gap N`): every call writes
  // a different blue texel, so every call must be a decode miss with its own
  // content key. A stale hit shows as a repeated key or a decode hit.
  let sourceOk = true;
  const sourceKeys = new Set(texProbe.map((p) => p.key));
  const sourceMisses = texProbe.filter((p) => p.decode === 'miss').length;
  const sourceHits = texProbe.filter((p) => p.decode === 'hit').length;
  if (expectSourceChange) {
    sourceOk = sourceKeys.size >= repeat && sourceMisses >= repeat && sourceHits === 0;
    if (!sourceOk) {
      console.log(`FAIL: source-change control expected ${repeat} distinct keys and misses, ` +
                  `read ${sourceKeys.size} keys / ${sourceMisses} miss / ${sourceHits} hit`);
    }
  }

  const hitsOk = expectHits === null || fbtexHits === expectHits;
  const ok = rendered && (!expectFbtex || fbtexLines.length > 0) && hitsOk && depthOk &&
             (!expectAlpha || alphaOk) && (!expectDither || ditherOk) &&
             (!expectTexcache || texcacheOk) && (!expectSourceChange || sourceOk);
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
  if (expectDither) {
    const on = dither[1], off = dither[2];
    console.log(`dither control: magicsq=${on ? `${on.noDither}/${on.dithered}/${on.other}` : 'missing'} ` +
                `disable=${off ? `${off.noDither}/${off.dithered}/${off.other}` : 'missing'}`);
    for (const l of ditherLines.slice(-2)) console.log('  ' + l);
  }
  if (expectTexcache) {
    const uploads = texProbe.filter((p) => p.result === 'upload').length;
    const reuses = texProbe.filter((p) => p.result === 'reuse').length;
    const decodeHits = texProbe.filter((p) => p.decode === 'hit').length;
    console.log(`texture cache control: repeat=${repeat} gap=${gap} upload=${uploads} ` +
                `reuse=${reuses} decode_hit=${decodeHits}`);
    for (const l of texProbeLines.slice(0, 4)) console.log('  ' + l);
  }
  if (expectSourceChange) {
    console.log(`source-change control: repeat=${repeat} gap=${gap} keys=${sourceKeys.size} ` +
                `miss=${sourceMisses} hit=${sourceHits}`);
    for (const l of texProbeLines.slice(0, 4)) console.log('  ' + l);
  }
  await decoder.close().catch(() => {});
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
