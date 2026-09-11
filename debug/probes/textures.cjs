#!/usr/bin/env node
'use strict';

// Dump the images the renderer actually decoded for the game's tiles.
//
// The texture pipeline is the hardest part of the title scene to reason about
// (load format vs. sampling format, LOADTILE vs LOADBLOCK row stride, TMEM
// addressing), so this probe renders the last few decoded images to PNGs with a
// checkerboard behind them - alpha is visible, and a wrong stride or a
// wrong format is obvious by eye.
//
// Usage:
//   node debug/probes/textures.cjs [--attempts 4] [--secs 60] [--scale 8] [--out tex]
//
// Exit codes: 0 dumped, 2 nothing to dump, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'tex');
const scale = h.num(opts.scale, 8);

const DUMP = ({ scale }) => {
  const count = Module._ogre_gfx_debug_tex_count ? Module._ogre_gfx_debug_tex_count() : 0;
  const i32 = (ptr) => new Int32Array(Module.HEAPU8.buffer, ptr, 1)[0];
  const ptrs = [4, 4, 4, 4, 4].map(() => Module._malloc(4));
  const [wP, hP, tP, fP, sP] = ptrs;
  const out = [];
  const all = [];
  for (let i = 0; i < count; i++) {
    const p = Module._ogre_gfx_debug_tex(i, wP, hP, tP, fP, sP);
    if (!p) continue;
    const w = i32(wP);
    const hh = i32(hP);
    if (!w || !hh) continue;
    const rgba = new Uint8ClampedArray(Module.HEAPU8.buffer, p, w * hh * 4).slice();
    all.push({ w, h: hh, rgba });
    const mk = (mode) => {
      const c = document.createElement('canvas');
      c.width = w * scale;
      c.height = hh * scale;
      const ctx = c.getContext('2d');
      for (let y = 0; y < hh; y++) {
        for (let x = 0; x < w; x++) {
          const o = (y * w + x) * 4;
          let r, g, b;
          if (mode === 'rgb') {
            r = rgba[o]; g = rgba[o + 1]; b = rgba[o + 2];
          } else {
            const a = rgba[o + 3] / 255;
            const bg = ((x >> 1) + (y >> 1)) % 2 ? 90 : 140;
            r = Math.round(rgba[o] * a + bg * (1 - a));
            g = Math.round(rgba[o + 1] * a + bg * (1 - a));
            b = Math.round(rgba[o + 2] * a + bg * (1 - a));
          }
          ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
          ctx.fillRect(x * scale, y * scale, scale, scale);
        }
      }
      return c.toDataURL('image/png');
    };
    out.push({
      i, w, h: hh,
      tile: i32(tP),
      fmt: i32(fP),
      siz: i32(sP),
      rgbUrl: mk('rgb'),
      alphaUrl: mk('alpha'),
      first: Array.from(rgba.slice(0, 16)).map((v) => v.toString(16).padStart(2, '0')).join(' '),
    });
  }

  // What one sprite *should* look like: the combiner takes colour from TEXEL1
  // (the second load of the pair) and alpha from TEXEL0 (the first). Both are
  // sampled over the same texel range (u 0..19 for the title sprites), so the
  // composite is colour.pixels[c] * mask.alpha[c] for the mask's first columns.
  const composite = (a, b, sw, sh) => {
    const c = document.createElement('canvas');
    c.width = sw * scale;
    c.height = sh * scale;
    const ctx = c.getContext('2d');
    for (let y = 0; y < sh; y++) {
      for (let x = 0; x < sw; x++) {
        const mo = ((y % a.h) * a.w + (x % a.w)) * 4;
        const co = ((y % b.h) * b.w + (x % b.w)) * 4;
        const al = a.rgba[mo + 3] / 255;
        const r = Math.round(b.rgba[co] * al);
        const g = Math.round(b.rgba[co + 1] * al);
        const bl = Math.round(b.rgba[co + 2] * al);
        ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + bl + ')';
        ctx.fillRect(x * scale, y * scale, scale, scale);
      }
    }
    return c.toDataURL('image/png');
  };
  const pairs = [];
  for (let i = 0; i + 1 < all.length; i += 2) {
    const a = all[i];
    const b = all[i + 1];
    const sw = Math.min(a.w, b.w);
    const sh = Math.max(a.h, b.h);
    pairs.push({ i, w: sw, h: sh, url: composite(a, b, sw, sh) });
  }
  ptrs.forEach((p) => Module._free(p));
  let timg = '';
  try {
    const s = Module._ogre_gfx_stats ? Module.UTF8ToString(Module._ogre_gfx_stats()) : '';
    timg = s.split('\n').filter((l) => l.startsWith('load:')).join(' ');
  } catch (e) {
    timg = '';
  }
  return { out, pairs, timg };
};

(async () => {
  const browser = await h.launch(cfg);
  let dumped = null;

  const run = await h.attemptLoop(browser, cfg, async () => {
    const a = await h.runAttempt(browser, cfg, {
      stopOnAdvanced: false,
      onSample: async (s) => {
        if (s.tasks < 3) return false;
        const dump = await s.page.evaluate(DUMP, { scale });
        if (!dump || !dump.out || dump.out.length < 2) return false;
        // The canvas at the same instant, so a decoded texture can be compared
        // with what the renderer actually put on screen for it.
        const canvas = await h.screenshotCanvas(s.page);
        dumped = { ...dump, state: s.state, milestones: s.milestones, canvas };
        return true;
      },
    });
    await a.page.close().catch(() => {});
    return { ok: Boolean(dumped) };
  });

  if (dumped) {
    const lines = [];
    dumped.out.forEach((t, n) => {
      const base = `${prefix}-${String(n).padStart(2, '0')}-${t.w}x${t.h}-t${t.tile}-f${t.fmt}s${t.siz}`;
      fs.writeFileSync(`${base}-rgb.png`, Buffer.from(t.rgbUrl.split(',')[1], 'base64'));
      fs.writeFileSync(`${base}-alpha.png`, Buffer.from(t.alphaUrl.split(',')[1], 'base64'));
      lines.push(`${base}-{rgb,alpha}.png  fmt=${t.fmt} siz=${t.siz} tile=${t.tile} first16=[${t.first}]`);
    });
    dumped.pairs.forEach((p) => {
      const file = `${prefix}-pair-${String(p.i).padStart(2, '0')}-${p.w}x${p.h}.png`;
      fs.writeFileSync(file, Buffer.from(p.url.split(',')[1], 'base64'));
      lines.push(`${file}  (colour x mask composite - what one sprite should look like)`);
    });
    if (dumped.canvas) fs.writeFileSync(`${prefix}-canvas.png`, dumped.canvas);
    h.writeOut(prefix, 'list.txt', lines.join('\n') + '\n');
    h.writeStats(prefix, dumped.state);
    h.writeMilestones(prefix, dumped.milestones);
    console.log(`DUMPED ${dumped.out.length} textures + ${dumped.pairs.length} composites  ${dumped.timg || ''}`);
    lines.forEach((l) => console.log('  ' + l));
  } else {
    console.log('NO TEXTURES');
  }

  await browser.close().catch(() => {});
  process.exit(dumped ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
