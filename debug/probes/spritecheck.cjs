#!/usr/bin/env node
'use strict';

// Compare ONE rendered sprite with the texture pair that produced it.
//
// The title-scene sprite path is hard to judge from a screenshot alone: the
// decoder, the combiner, the render mode and the vertex transform all have to
// agree before a 20x37 character lands on screen correctly. This probe removes
// the ambiguity by asking the renderer to draw only the first sprite of the
// list (ogre_gfx_debug_flags bit 1), then putting the canvas crop and the
// "colour x mask" composite of that same frame's first two loads side by side.
//
// If the two halves look alike, the whole path is right; if they differ, the
// difference says which half is wrong (shape => mask/alpha, hue => colour).
//
// The intro is NOT static: the ring zooms in over the first ~40 display lists,
// so a comparison made on the first rendered frame compares the wrong moment.
// `--settle MS` waits that long after the scene starts before isolating the
// sprite; the sprite's position/scale keeps animating, but its texture pair and
// the combiner output should match at any time.
//
// Usage:
//   node debug/probes/spritecheck.cjs [--attempts 3] [--secs 90] [--settle 30000]
//                                     [--scale 8] [--out sprite]
//
// Exit codes: 0 compared, 2 nothing captured, 1 fatal.

const fs = require('fs');
const h = require('../lib/harness.cjs');

const { opts } = h.parseArgs();
const cfg = h.resolveConfig(opts);
const prefix = h.outPrefix(cfg, opts.out || 'sprite');
const scale = h.num(opts.scale, 8);
const settle = h.num(opts.settle, 0);

// Runs in the page: isolates the first sprite, waits for frames to land, then
// reads the canvas back and builds the comparison image.
const COMPARE = ({ scale }) => {
  const canvasEl = document.getElementById('game-canvas');
  const gl = canvasEl.getContext('webgl2');
  const W = canvasEl.width;
  const H = canvasEl.height;

  // A frame must be *executed* after the flags change: the game stalls for
  // seconds at a time (and the canvas then keeps whatever the last, faded
  // frame left), so clearing and waiting a fixed time can read a black canvas.
  const execTask = () => {
    const m = /(?:^|\n)exec: task=(\d+)/.exec(document.getElementById('gfxstats').textContent);
    return m ? +m[1] : -1;
  };
  const before = execTask();
  Module._ogre_gfx_debug_flags(2);   // bit 1: only the first sprite
  // Clear once, then let the renderer's 16 ms flush loop draw the isolated
  // sprite: clearing again in the readback frame would read a buffer that no
  // flush has drawn into yet.
  gl.clearColor(0, 0, 0, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);

  return new Promise((resolve) => {
    let waited = 0;
    const tick = () => {
      waited += 250;
      if (execTask() > before || waited >= 12000) {
        requestAnimationFrame(() => {
        // readPixels is bottom-up; flip into a top-down RGBA buffer.
        const raw = new Uint8Array(W * H * 4);
        gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, raw);
        const px = new Uint8ClampedArray(W * H * 4);
        for (let y = 0; y < H; y++) {
          const src = (H - 1 - y) * W * 4;
          px.set(raw.subarray(src, src + W * 4), y * W * 4);
        }
        // Bounding box of lit pixels. The threshold (not just "non-zero")
        // keeps the isolated sprite from being swamped by the black fill's
        // rounding artifacts at the screen edges.
        let x0 = W, y0 = H, x1 = -1, y1 = -1;
        const lit = (o) => px[o] + px[o + 1] + px[o + 2] > 36;
        for (let y = 0; y < H; y++) {
          for (let x = 0; x < W; x++) {
            const o = (y * W + x) * 4;
            if (lit(o)) {
              if (x < x0) x0 = x;
              if (x > x1) x1 = x;
              if (y < y0) y0 = y;
              if (y > y1) y1 = y;
            }
          }
        }

        // The first two decoded images of the frame = this sprite's mask
        // (TEXEL0, alpha) and colour (TEXEL1, rgb).
        const i32 = (p) => new Int32Array(Module.HEAPU8.buffer, p, 1)[0];
        const ptrs = [4, 4, 4, 4, 4].map(() => Module._malloc(4));
        const [wP, hP, tP, fP, sP] = ptrs;
        // The pair the most recent textured draw actually sampled. The decoded
        // ring (ogre_gfx_debug_tex) spans frames, so its first pair is not
        // necessarily what was drawn.
        const load = (i) => {
          const fn = Module._ogre_gfx_debug_last_tex || Module._ogre_gfx_debug_tex;
          const p = fn(i, wP, hP, tP, fP, sP);
          if (!p) return null;
          const w = i32(wP), hh = i32(hP);
          if (!w || !hh) return null;
          return { w, h: hh, rgba: new Uint8ClampedArray(Module.HEAPU8.buffer, p, w * hh * 4).slice() };
        };
        const mask = load(0);
        const colour = load(1);
        ptrs.forEach((p) => Module._free(p));

        // Alpha histogram of the mask (TEXEL0): OB64's I4 mask is decoded as
        // `nibble * 255 / 15`, so a binary mask must show only 0 and 255. A
        // spread of intermediate levels means the mask is being decoded with
        // the wrong format - which would render the sprite semi-transparent.
        const alphaHist = {};
        if (mask) {
          for (let i = 3; i < mask.rgba.length; i += 4) {
            const a = mask.rgba[i];
            alphaHist[a] = (alphaHist[a] || 0) + 1;
          }
        }
        const rgbMax = { r: 0, g: 0, b: 0 };
        if (colour) {
          for (let i = 0; i < colour.rgba.length; i += 4) {
            rgbMax.r = Math.max(rgbMax.r, colour.rgba[i]);
            rgbMax.g = Math.max(rgbMax.g, colour.rgba[i + 1]);
            rgbMax.b = Math.max(rgbMax.b, colour.rgba[i + 2]);
          }
        }

        const out = document.createElement('canvas');
        const cw = x1 >= x0 ? x1 - x0 + 1 : 0;
        const ch = y1 >= y0 ? y1 - y0 + 1 : 0;
        const pad = 4;
        const spriteW = cw * scale;
        const spriteH = ch * scale;
        const pairW = mask && colour ? Math.min(mask.w, colour.w) * scale : 0;
        const pairH = mask && colour ? Math.max(mask.h, colour.h) * scale : 0;
        out.width = Math.max(spriteW + pairW + pad * 3, 1);
        out.height = Math.max(spriteH, pairH) + pad * 2;
        const ctx = out.getContext('2d');
        ctx.fillStyle = '#202020';
        ctx.fillRect(0, 0, out.width, out.height);

        // Left: the rendered sprite crop (a light background so dark pixels
        // inside the sprite are distinguishable from the empty canvas).
        ctx.imageSmoothingEnabled = false;
        for (let y = 0; y < ch; y++) {
          for (let x = 0; x < cw; x++) {
            const o = ((y0 + y) * W + (x0 + x)) * 4;
            ctx.fillStyle = 'rgb(' + px[o] + ',' + px[o + 1] + ',' + px[o + 2] + ')';
            ctx.fillRect(pad + x * scale, pad + y * scale, scale, scale);
          }
        }
        // Right: colour x mask, the sprite the combiner should produce.
        if (mask && colour) {
          const sw = Math.min(mask.w, colour.w);
          const sh = Math.max(mask.h, colour.h);
          for (let y = 0; y < sh; y++) {
            for (let x = 0; x < sw; x++) {
              const mo = ((y % mask.h) * mask.w + (x % mask.w)) * 4;
              const co = ((y % colour.h) * colour.w + (x % colour.w)) * 4;
              const a = mask.rgba[mo + 3] / 255;
              const r = Math.round(colour.rgba[co] * a);
              const g = Math.round(colour.rgba[co + 1] * a);
              const b = Math.round(colour.rgba[co + 2] * a);
              ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
              ctx.fillRect(pad * 2 + spriteW + x * scale, pad + y * scale, scale, scale);
            }
          }
        }
        Module._ogre_gfx_debug_flags(0);
        resolve({
          freshFrames: execTask() > before,
          alphaHist, rgbMax,
          bbox: [x0, y0, x1, y1],
          canvasSize: [W, H],
          mask: mask ? [mask.w, mask.h] : null,
          colour: colour ? [colour.w, colour.h] : null,
          dataUrl: out.toDataURL('image/png'),
        });
      });
        return;
      }
      setTimeout(tick, 250);
    };
    setTimeout(tick, 250);
  });
};

(async () => {
  const browser = await h.launch(cfg);
  let result = null;
  let settled = false;
  const run = await h.attemptLoop(browser, cfg, async () => {
    settled = false;
    const a = await h.runAttempt(browser, cfg, {
      stopOnAdvanced: false,
      onSample: async (s) => {
        if (s.tasks < 2) return false;
        if (settle > 0 && !settled) {
          settled = true;   // wait for the intro's zoom-in before comparing
          await h.sleep(settle);
        }
        const r = await s.page.evaluate(COMPARE, { scale });
        if (!r || !r.dataUrl) return false;
        result = { ...r, state: s.state, milestones: s.milestones };
        return true;
      },
    });
    await a.page.close().catch(() => {});
    return { ok: Boolean(result) };
  });

  if (result) {
    fs.writeFileSync(`${prefix}-compare.png`, Buffer.from(result.dataUrl.split(',')[1], 'base64'));
    h.writeStats(prefix, result.state);
    h.writeMilestones(prefix, result.milestones);
    h.writeJson(prefix, 'result.json', {
      bbox: result.bbox,
      canvasSize: result.canvasSize,
      mask: result.mask,
      colour: result.colour,
    });
    console.log(`left = rendered sprite (bbox ${result.bbox.join(',')} of ${result.canvasSize.join('x')}), ` +
                `right = colour ${result.colour} x mask ${result.mask}`);
    console.log('read after a fresh display list:', result.freshFrames);
    console.log('mask alpha histogram:', JSON.stringify(result.alphaHist));
    console.log('colour max RGB:', JSON.stringify(result.rgbMax));
    console.log(`wrote ${prefix}-compare.png (attempt ${run.attempt})`);
  } else {
    console.log('NOTHING CAPTURED');
    const last = run.results[run.results.length - 1] || {};
    console.log('last gfx:', (last.state && last.state.gfx || '').split('\n')[0]);
    console.log('errors:', (last.errors || []).slice(0, 2).join(' | ') || '(none)');
  }

  await browser.close().catch(() => {});
  process.exit(result ? 0 : 2);
})().catch((e) => {
  console.error('FATAL', e.message);
  process.exit(1);
});
