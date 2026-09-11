#!/usr/bin/env node
'use strict';

// Page-log smoke test: the default page keeps #status to a single line (no
// per-line DOM work) while window.ogreLog holds the full bounded log; ?log turns
// the mirror back on. Also checks that the ogreLog API the probes use exists.
//
// Usage:
//   node debug/probes/logmode.cjs [--out logmode] [--url URL]
//
// Exit codes: 0 contract intact, 2 contract broken, 1 fatal.

const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'logmode');

function withLogFlag(url) {
  return url + (url.includes('?') ? '&' : '?') + 'log';
}

(async () => {
  const browser = await h.launch(cfg);
  const results = [];

  for (const [label, url] of [['default', cfg.url], ['?log', withLogFlag(cfg.url)]]) {
    const { page, errors } = await h.openPage(browser);
    await h.gotoPage(page, { url });
    await h.waitForLog(page, h.READY);
    const r = await page.evaluate(() => {
      const status = document.getElementById('status');
      return {
        lines: ogreLog.length(),
        head: ogreLog.head(1),
        statusLen: status.textContent.length,
        statusLines: status.textContent.split('\n').length,
        logMode: status.classList.contains('log-mode'),
        hasSave: typeof ogreLog.save === 'function',
        hasFind: typeof ogreLog.find === 'function',
        hasTail: typeof ogreLog.tail === 'function',
        hasContains: typeof ogreLog.contains === 'function',
        found: ogreLog.find(/WASM runtime/).length,
      };
    }).catch((e) => ({ err: e.message }));
    results.push({ label, ...r, errors });
    console.log(`${label}: ${JSON.stringify(r)} errors=${errors.length}`);
    await page.close().catch(() => {});
  }

  const api = results.every((r) => r.hasSave && r.hasFind && r.hasTail && r.hasContains && r.lines > 0);
  const defaultOneLine = results[0] && results[0].statusLines === 1 && !results[0].logMode;
  const logMirrors = results[1] && results[1].logMode === true;

  h.writeJson(prefix, 'result.json', { api, defaultOneLine, logMirrors, results });
  h.writeErrors(prefix, results.flatMap((r) => r.errors || []));

  const ok = api && defaultOneLine && logMirrors;
  console.log(ok ? 'PASS' : `FAIL api=${api} defaultOneLine=${defaultOneLine} logMirrors=${logMirrors}`);
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
