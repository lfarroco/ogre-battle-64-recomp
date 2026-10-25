#!/usr/bin/env node
'use strict';

// Read the `OGRE_FRAME_DUMP=1` files out of a run.
//
// The web renderer's mission-intro diagnostic writes, at the draw that samples
// a picture strip (not at a timer):
//
//   /tmp/texsrc-<n>.bin  the strip image's source bytes, guest logical order
//   /tmp/tmem-<n>.bin    the 4 KiB TMEM the load left
//   /tmp/texdec-<n>.bin  the decoded RGBA8 image the draw samples
//
// A timed RDRAM dump cannot bracket that draw: the heap buffer is reused by the
// screens that follow, so the dump is either before the composition or after
// the reuse. These files exist only while the run is up, so this probe copies
// them out next to the page log.
//
// Usage:
//   node probes/framedump.cjs [--secs 240] [--out /tmp/framedump]
//                             [--query 'OGRE_FRAME_DUMP=1&OGRE_SAVE=...']

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'framedump');
const secs = h.num(opts.secs, 240);
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
  let samples = [];
  while (Date.now() - t0 < secs * 1000) {
    const state = await h.readState(page).catch(() => null);
    const lines = await page
      .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[GFX-FRAME-DUMP\]/) : []))
      .catch(() => []);
    dumps = lines.length;
    samples.push(
      `t=${((Date.now() - t0) / 1000).toFixed(1)} tasks=${
        state ? h.taskCount(state.gfx) : -1
      } dumps=${dumps}`
    );
    if (dumps >= 2) break;
    await h.sleep(1000);
  }

  const log = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, log + '\n');
  fs.writeFileSync(`${prefix}-samples.txt`, samples.join('\n') + '\n');
  h.writeErrors(prefix, errors);

  // The canvas at the same instant, so a decoded image and the frame it produced
  // are two views of one moment.
  await h.screenshotCanvas(page, `${prefix}-canvas.png`).catch(() => null);

  let copied = 0;
  for (let n = 0; n < dumps; n++) {
    for (const name of ['texsrc', 'tmem', 'texdec']) {
      const inside = `/tmp/${name}-${n}.bin`;
      const bytes = await readFsFile(page, inside).catch(() => null);
      if (!bytes) {
        console.log(`MISSING ${inside}`);
        continue;
      }
      fs.writeFileSync(`${prefix}-${name}-${n}.bin`, bytes);
      console.log(`${inside} -> ${prefix}-${name}-${n}.bin (${bytes.length} bytes)`);
      copied++;
    }
  }
  const dumpLines = log
    .split('\n')
    .filter((l) => l.includes('[GFX-FRAME-DUMP]') || l.includes('[GFX-FRAME-RECT]'))
    .map((l) => l.replace(/^\[stderr\] /, ''));
  fs.writeFileSync(`${prefix}-dumps.txt`, dumpLines.join('\n') + '\n');
  h.writeJson(prefix, 'result.json', { dumps, copied });
  console.log(`FRAME DUMPS ${dumps}; copied ${copied} file(s)`);
  await browser.close().catch(() => {});
  process.exit(copied > 0 ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
