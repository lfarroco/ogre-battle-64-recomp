#!/usr/bin/env node
'use strict';

// Sweep scenes and report every framebuffer-as-texture load: a `G_LOADTILE` /
// `G_LOADBLOCK` whose decoded source range overlapped a colour image that a
// recorded draw had written and that the renderer had not written back to RDRAM
// yet. The renderer reports those as `[GFX-FBTEX]` and counts them in
// `#gfxstats` (`fbtex: hits=N`).
//
// The browser renderer decodes a texture from RDRAM on the gfx thread, while the
// pixels of a colour image live in a GL framebuffer object until the browser
// main thread writes them back, so such a load reads stale bytes. This probe
// finds the screens that need the feature before it is implemented.
//
// Each scene gets its own boot, because `OGRE_SCENE` is consumed before the boot
// enters its first scene (docs/guides/app-build.md -> "Start debug runs at the
// title screen"). A forced scene has none of the pre-state a natural path
// builds, so a scene that does not reach its own display lists here is unknown,
// not clean.
//
// Usage:
//   node debug/probes/fbtex.cjs --scenes 0x00,0x04,0x0b --secs 20 --out /tmp/fbtex
//   node debug/probes/fbtex.cjs --queries 'OGRE_SCENE=new-game&OGRE_STEP=2||OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a' --secs 40
//
// `--queries` (`||` separated) runs arbitrary query strings instead of the
// scene list, which is how a natural route (`OGRE_TAP_*`, `OGRE_STEP`) is
// checked. Writes `<prefix>-result.json` and `<prefix>.txt` (one block per
// entry).

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'fbtex');
const secs = h.num(opts.secs, 20);
const scenes = String(
  opts.scenes ||
    '0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,' +
      '0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18'
)
  .split(',')
  .map((s) => s.trim())
  .filter(Boolean);

// Each entry is {label, query}. `--queries` wins when given.
const entries = opts.queries
  ? String(opts.queries)
      .split('||')
      .map((q) => q.trim())
      .filter(Boolean)
      .map((q, i) => ({ label: `q${i}`, query: q }))
  : scenes.map((s) => ({ label: s, query: `OGRE_SCENE=${encodeURIComponent(s)}&OGRE_SCENE_LOG=1` }));

function withQuery(url, extra) {
  return url + (url.includes('?') ? '&' : '?') + extra;
}

(async () => {
  const browser = await h.launch(cfg);
  const rows = [];
  for (const { label, query } of entries) {
    const { page, errors } = await h.openPage(browser);
    const row = { scene: label, query, hits: 0, lines: [], sceneLines: [], tasks: 0, error: null };
    try {
      await h.gotoPage(page, { ...cfg, url: withQuery(cfg.url, query) });
      await h.loadRom(page, cfg);
      const t0 = Date.now();
      while (Date.now() - t0 < secs * 1000) {
        await h.sleep(1500);
      }
      const state = await h.readState(page).catch(() => null);
      if (state) {
        row.tasks = h.taskCount(state.gfx);
        const m = /fbtex: hits=(\d+)/.exec(state.gfx || '');
        if (m) row.hits = parseInt(m[1], 10);
        row.lines = (state.tail || '')
          .split('\n')
          .filter((l) => l.includes('GFX-FBTEX'))
          .map((l) => l.replace(/^\[stderr\] /, ''));
        row.sceneLines = (state.tail || '')
          .split('\n')
          .filter((l) => l.includes('[scene]'))
          .slice(-3)
          .map((l) => l.replace(/^\[stderr\] /, ''));
      }
      // The tail is bounded, so read the whole ring for the FBTEX lines. The
      // page log carries each milestone twice (stderr plus the mirror), so
      // dedupe by the stripped line.
      const full = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
      fs.writeFileSync(`${prefix}-${label}-pagelog.txt`, full);
      const seen = new Set(row.lines);
      for (const l of full.split('\n')) {
        if (!l.includes('GFX-FBTEX')) continue;
        const line = l.replace(/^\[stderr\] /, '').trim();
        if (!seen.has(line)) {
          seen.add(line);
          row.lines.push(line);
        }
      }
      row.hits = Math.max(row.hits, row.lines.length);
    } catch (e) {
      row.error = String(e.message || e);
    }
    if (errors.length) row.error = (row.error ? row.error + '; ' : '') + errors.slice(0, 2).join('; ');
    rows.push(row);
    console.log(
      `${label}: tasks=${row.tasks} hits=${row.hits}` +
        (row.lines.length ? ` first="${row.lines[0].slice(0, 170)}"` : '') +
        (row.error ? ` error=${row.error.slice(0, 120)}` : '')
    );
    await page.close().catch(() => {});
  }
  await browser.close().catch(() => {});

  const text = rows
    .map((r) =>
      [
        `${r.scene}: tasks=${r.tasks} hits=${r.hits} query=${r.query}`,
        ...r.sceneLines.map((l) => `  scene: ${l}`),
        ...r.lines.map((l) => `  ${l}`),
        r.error ? `  error: ${r.error}` : null,
      ]
        .filter(Boolean)
        .join('\n')
    )
    .join('\n');
  fs.writeFileSync(`${prefix}.txt`, text + '\n');
  h.writeJson(prefix, '-result.json', { scenes: rows });
  const total = rows.reduce((n, r) => n + r.hits, 0);
  console.log(`TOTAL framebuffer-as-texture loads: ${total} over ${rows.length} entr(ies)`);
  console.log(`wrote ${prefix}.txt`);
  process.exit(0);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
