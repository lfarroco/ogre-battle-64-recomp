#!/usr/bin/env node
'use strict';

// Battery save import/export check for the browser build.
//
// The page imports a battery with `?OGRE_SAVE=<url>` (app/web/web.js) and
// exports the runtime's image from the topbar "Export save" button, which reads
// `/ogre/saves/ogrebattle64-us-rev1.bin` out of the wasm FS and downloads it.
// The exported bytes are the raw 32 KiB image the native build reads from
// `<config>/saves/<game id>.bin`.
//
// This probe checks the whole round trip on the static package, which is the
// GitHub Pages layout (plain host, COOP/COEP supplied by the service worker):
//
//   make web-dist
//   cp assets/escort_suspend.bin dist-web/
//   python3 -m http.server --bind 127.0.0.1 --directory dist-web 8940 &
//   node debug/probes/save.cjs \
//     --url 'http://127.0.0.1:8940/?OGRE_SAVE=escort_suspend.bin&OGRE_SCENE=title'
//
// Exit codes: 0 round trip exact, 2 mismatch/failure, 1 fatal.

const fs = require('fs');
const path = require('path');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const source = path.resolve(opts.save ||
  path.join(__dirname, '..', '..', 'assets', 'escort_suspend.bin'));
const prefix = h.outPrefix(cfg, opts.out || 'save');
const SAVE_NAME = 'ogrebattle64-us-rev1.bin';

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);
  const report = { url: cfg.url, source, checks: {}, errors: [] };

  try {
    await h.gotoPage(page, cfg);

    report.checks.disclaimer = await page.evaluate(() => {
      const el = document.querySelector('.launcher-disclaimer');
      const text = el ? el.textContent.replace(/\s+/g, ' ').trim() : '';
      return { present: Boolean(el), text };
    });
    report.checks.exportButton = await page.evaluate(() =>
      Boolean(document.getElementById('export-save-btn')));

    await h.loadRom(page, cfg);
    await page.waitForFunction(() => window.ogreSave &&
      Module.FS && Module.FS.analyzePath &&
      Module.FS.analyzePath(window.ogreSave.path).exists, null, { timeout: 30000 });

    report.checks.fsImage = await page.evaluate(() => {
      const bytes = Module.FS.readFile(window.ogreSave.path);
      return { path: window.ogreSave.path, size: bytes.length };
    });
    report.checks.importLog = await page.evaluate(() =>
      window.ogreLog.tail(4000).split('\n').filter((line) =>
        line.indexOf('[web:save]') >= 0 && line.indexOf('imported') >= 0));

    const optionsRow = await page.evaluate(() => {
      if (!window.ogreOptions) return '';
      window.ogreOptions.open('main');
      const body = document.getElementById('options-body');
      return body ? body.textContent.replace(/\s+/g, ' ').trim() : '';
    });
    report.checks.optionsRow = optionsRow.indexOf('Export battery save') >= 0;
    await page.evaluate(() => window.ogreOptions && window.ogreOptions.close());

    const [download] = await Promise.all([
      page.waitForEvent('download', { timeout: 30000 }),
      page.click('#export-save-btn'),
    ]);
    const downloadedPath = await download.path();
    const downloaded = fs.readFileSync(downloadedPath);
    report.download = {
      name: download.suggestedFilename(),
      size: downloaded.length,
      path: `${prefix}-${SAVE_NAME}`,
    };
    fs.writeFileSync(report.download.path, downloaded);

    const expected = fs.readFileSync(source);
    report.checks.filename = download.suggestedFilename() === SAVE_NAME;
    report.checks.size = downloaded.length === 32768;
    report.checks.magic = downloaded.slice(4, 12).toString('latin1') === 'QuestOG3';
    report.checks.exact = downloaded.equals(expected);
    report.checks.status = await page.evaluate(() =>
      document.getElementById('status').textContent.trim());

    const ok = report.checks.disclaimer.present &&
      report.checks.disclaimer.text.indexOf('Experimental') >= 0 &&
      report.checks.exportButton && report.checks.optionsRow &&
      report.checks.filename && report.checks.size && report.checks.magic &&
      report.checks.exact && errors.length === 0;

    report.ok = ok;
    h.writeJson(prefix, 'result.json', report);
    console.log(JSON.stringify(report.checks, null, 2));
    console.log(`exported ${report.download.size} bytes as ${report.download.name}`);
    console.log(ok ? 'SAVE ROUND TRIP OK' : 'SAVE ROUND TRIP FAILED');
    process.exitCode = ok ? 0 : 2;
  } finally {
    await browser.close().catch(() => {});
  }
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
