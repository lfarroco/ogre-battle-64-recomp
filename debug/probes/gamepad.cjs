#!/usr/bin/env node
'use strict';

// Does a browser gamepad reach N64 controller 1?
//
// N64 controller 1 is slot 0, and it is the only controller this game reads. A
// pad that the shell places in slots 1-3 is invisible in game (the native build
// had the same defect: issue #13, where SDL opened the pad in slot 1).
//
// A headless browser exposes no real pad, so the probe installs a synthetic one
// before the page scripts run: `navigator.getGamepads` returns one connected pad
// whose `A` button is held. Nothing sends real events - the pad exists only in
// the array the shell polls. Then it reads the wasm input state
// (`Module._ogre_input_debug(slot)`) for every slot.
//
// Before the fix (`web.js` placed pads at `index % 3 + 1`) the run logs
// `[web:input] gamepad connected (slots 1-3)`, slot 0 stays an idle keyboard
// and slot 1 carries A. After it, the log says `pad 0 -> slot 0` and slot 0
// carries A (`0x8000`), which is what a player's pad does.
//
// Usage:
//   node debug/probes/gamepad.cjs [--secs 25] [--out gamepad] [--rom PATH] [--url URL]
//
// Exit codes: 0 the pad reached slot 0 carrying A, 2 it did not, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'gamepad');
const secs = h.num(opts.secs, 25);

// Button bit 0 (`A`) of the Standard Gamepad mapping; the shell maps it to the
// N64 `A` bit 0x8000.
const PAD_A_BIT = 0;
const N64_A = 0x8000;
const SYNTHETIC_PAD = `(() => {
  const button = (pressed) => ({ pressed: pressed, touched: pressed, value: pressed ? 1 : 0 });
  const pad = {
    id: 'probe synthetic pad (STANDARD GAMEPAD)',
    index: 0,
    connected: true,
    mapping: 'standard',
    timestamp: 0,
    axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, (_, i) => button(i === ${PAD_A_BIT})),
    vibrationActuator: null,
  };
  navigator.getGamepads = () => [pad, null, null, null];
})();`;

(async () => {
  const browser = await h.launch(cfg);
  const { page, errors } = await h.openPage(browser);
  await page.addInitScript(SYNTHETIC_PAD);
  await h.gotoPage(page, cfg);
  await h.loadRom(page, cfg);

  const samples = [];
  const t0 = Date.now();
  // Sample for a fixed window rather than stopping at the first good sample: the
  // shell's connect line is rate-limited to one per 5 s, so a short run would
  // report the slot masks and no log line to read.
  while (Date.now() - t0 < secs * 1000) {
    await h.sleep(1000);
    const s = await page
      .evaluate(() => {
        const has = typeof Module._ogre_input_debug === 'function';
        if (!has) return { has };
        const slots = [0, 1, 2, 3].map((i) => Module._ogre_input_debug(i));
        return {
          has,
          slots,
          connected: slots.map((v) => (v === 0xffff ? 0 : 1)),
          gfx: (document.getElementById('gfxstats') || {}).textContent || '',
        };
      })
      .catch((e) => ({ err: String(e.message || e) }));
    samples.push(s);
  }

  const state = await h.readState(page).catch(() => ({ gfx: '', status: '', tail: '' }));
  const full = await page.evaluate(() => (window.ogreLog ? window.ogreLog.text() : '')).catch(() => '');
  fs.writeFileSync(`${prefix}-pagelog.txt`, full);
  const padLines = [
    ...new Set(
      full
        .split('\n')
        .filter((l) => l.includes('[web:input]') && /gamepad|pad /.test(l))
        .map((l) => l.replace(/^\[stderr\] /, '').trim())
    ),
  ];

  const last = samples[samples.length - 1] || {};
  const exact = Boolean(last.has && last.slots && last.slots[0] === N64_A);
  const registered = samples.some((s) => s.has && s.slots && (s.slots[0] & N64_A) !== 0);

  // The keyboard is OR'd into the same slot, so a pad must not take it away:
  // hold the keyboard's `B` (`KeyZ`) while the synthetic pad holds `A`, and
  // require the slot-0 mask to carry both.
  const KEYMAP_B = 0x4000;
  await page.keyboard.down('KeyZ').catch(() => {});
  await h.sleep(500);
  const both = await page
    .evaluate(() => (typeof Module._ogre_input_debug === 'function' ? Module._ogre_input_debug(0) : null))
    .catch(() => null);
  await page.keyboard.up('KeyZ').catch(() => {});
  await h.sleep(500);
  const afterRelease = await page
    .evaluate(() => (typeof Module._ogre_input_debug === 'function' ? Module._ogre_input_debug(0) : null))
    .catch(() => null);
  const keyboardShares = both === (N64_A | KEYMAP_B) && afterRelease === N64_A;

  const ok = (exact || registered) && keyboardShares;

  h.writeStats(prefix, state);
  h.writeOut(prefix, 'status.txt', state.tail || '');
  h.writeErrors(prefix, errors);
  h.writeJson(prefix, 'result.json', {
    samples, padLines, exact, registered,
    keyboard: { both, afterRelease, expectedBoth: N64_A | KEYMAP_B, expectedAfter: N64_A, shares: keyboardShares },
  });

  console.log(`input export=${Boolean(last.has)} slots(last)=${JSON.stringify(last.slots || [])}`);
  console.log(`slot0 carried A in some sample: ${registered}; in the last sample: ${exact}`);
  console.log(`keyboard B while the pad holds A: slot0=0x${(both || 0).toString(16)} ` +
              `(want 0x${(N64_A | KEYMAP_B).toString(16)}), after release=0x${(afterRelease || 0).toString(16)} ` +
              `(want 0x${N64_A.toString(16)})`);
  for (const l of padLines.slice(0, 4)) console.log('  ' + l);
  if (!ok) {
    console.log('FAIL: the gamepad did not reach slot 0 (N64 controller 1) with the keyboard');
  }
  await browser.close().catch(() => {});
  process.exit(ok ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
