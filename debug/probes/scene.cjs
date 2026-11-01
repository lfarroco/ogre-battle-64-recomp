#!/usr/bin/env node
'use strict';

// Reaches a screen and captures it, in one command.
//
// The route to a screen used to be a throwaway Playwright script every time: a
// boot, a title-menu key sequence, a scene-keyed tap schedule or an A-press
// loop through a dialogue, a settle wait, then a screenshot and a page log. This
// probe owns those four things so a session can ask for a frame instead of
// rebuilding the harness. It is the "what does this screen look like" tool; the
// per-screen comparisons in `recttoggle.cjs` / `stripab.cjs` stay their own
// probes.
//
// Usage:
//   node debug/probes/scene.cjs --route tutorial-field --out /tmp/s/field
//   node debug/probes/scene.cjs --route mission-briefing --out /tmp/s/mission
//   node debug/probes/scene.cjs --route title --shots 0,16 --points 12,160
//   node debug/probes/scene.cjs --scene 0x0003 \
//       --query 'OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title' --out /tmp/s/x
//
// Options:
//   --route NAME        a route from ROUTES below (default: title)
//   --scene 0x0003      wait for this scene instead of the route's own
//   --query 'A=B&C=D'   extra OGRE_* query parameters (URL-encoded as given)
//   --save PATH         add OGRE_SAVE=<path> (a repository-relative URL, e.g.
//                       assets/escort_suspend.bin); the page fetches it
//   --out PREFIX        output prefix (default debug/out/scene-<route>)
//   --shots 0,16        debug-flag values to capture, one PNG each
//                       (0 normal, 8 TEXEL1 raw, 16 TEXEL0 raw; --raw = 0,8,16)
//   --raw               shorthand for --shots 0,8,16
//   --points x,y;x,y    sample these screen pixels in every shot and write them
//                       to <prefix>-samples.json
//   --points-space guest|canvas
//                       guest (default) means a 320x240 reference scaled to the
//                       drawing buffer; canvas is the buffer's own pixels
//   --settle MS         wait after the scene line before the first shot
//                       (route default: 2000; the field and the briefing use
//                       12000 because their fades finish late)
//   --shot-settle MS    wait after each debug-flag change (default 1200)
//   --repeat N          capture the flag set N times (default 1), one
//                       <prefix>-<tag>-N.png each, to catch an animation phase
//   --repeat-gap MS     wait between repeats (default 1000)
//   --attempts N        boots to try when one lands on the idle trajectory
//   --log-cap N         raise window.OGRE_LOG_CAP (default 20000 lines) so a
//                       long route keeps its later GFX-* lines in -log.txt
//   --headed            show the browser window
//   --url URL / --rom PATH
//                       overrides (same as the other probes)
//
// Output files, all from one run:
//   <prefix>-<tag>.png      one per --shots value (tag: normal/texel1/texel0/fN)
//   <prefix>-samples.json   the sampled pixels per shot, and the canvas size
//   <prefix>-log.txt        the page log (scene timeline, GFX-* lines)
//   <prefix>-errors.txt     page errors
//   <prefix>-meta.json      route, URL, scene, timings, gfx stats
//
// Exit 0: the scene was reached and at least one shot was written.
// Exit 2: the scene was never reached; the logs are still written.
// Exit 1: fatal (chromium missing, no URL, ...).

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');
const h = require('../lib/harness.cjs');

const ROUTES = {
  // Forced title scene. The menu overlay is reachable from here; the attract
  // loop is not driven by this route.
  title: {
    query: 'OGRE_SCENE=title',
    scene: '0x0004',
    settle: 2000,
    notes: 'forced title scene',
    steps: [],
  },

  // Castle Burgund mission briefing, from the escort save. The page's
  // scene-keyed tap schedule starts a new game and presses A through Load Game;
  // `title:start:6` opens the menu, `0x12:a:6` takes Load Game.
  'mission-briefing': {
    query:
      'OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_TAP_MS=2000' +
      '&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6',
    scene: '0x0003',
    settle: 12000,
    notes: 'mission briefing via the escort suspend save and URL taps',
    steps: [],
  },

  // The tutorial practice stage (Jangurri). The title menu opens the tutorial,
  // the tutorial dialogue advances with A, and its 0x02/0x0D sequence enters
  // scene 0x03. This is the route that reaches the field's grass.
  'tutorial-field': {
    query: 'OGRE_SCENE_LOG=1',
    scene: '0x0003',
    settle: 12000,
    notes: 'title menu -> tutorial (0x17) -> A through the dialogue -> practice field',
    steps: [
      { waitScene: '0x0004', timeout: 70 },
      { sleep: 2500 },
      { press: 'Enter', hold: 200 },
      { sleep: 2500 },
      { press: 'ArrowDown', hold: 200 },
      { sleep: 1200 },
      { press: 'Enter', hold: 200 },
      { waitScene: '0x0017', timeout: 15 },
      // Step 0x17 -> 0x02 -> 0x0D -> 0x03 is wait-gated on the scene line, and
      // the box advances with one A per press, so polling is enough.
      { pressUntilScene: 'KeyX', scene: '0x0003', interval: 1400, max: 90 },
    ],
  },

  // Forced tutorial scene, for the dialogue box without the field.
  tutorial: {
    query: 'OGRE_SCENE=tutorial',
    scene: '0x0017',
    settle: 4000,
    notes: 'forced tutorial scene',
    steps: [],
  },

  // The New Game cathedral dialogue (scene 0x0D, step 2). Its backdrop is the
  // njpeg + S2DEX2 path: the game submits type-4 RSP tasks, then per macroblock
  // an S2DEX2 `G_OBJ_RECTANGLE_R` into a colour image, then copies that image
  // back with the CPU and blits the assembled picture. The page's scene-keyed
  // taps open the title menu and pick New Game; the route then waits for the
  // first `[GFX-S2D-CENSUS]` line, which is the njpeg macroblock task, and
  // settles while the passes assemble. A wall-clock settle cannot do this: the
  // opening movie and the "Ischka Military Academy" card before the cathedral
  // make the arrival vary by 20 s between runs.
  cathedral: {
    query: 'OGRE_NO_AUDIO=1&OGRE_TAP_MS=1500&OGRE_TAP_SCENE_BUTTON=title:start:6,title:a:1',
    scene: '0x000D',
    settle: 12000,
    notes: 'title menu -> New Game -> the opening movie -> the cathedral dialogue',
    steps: [
      { waitScene: '0x0004', timeout: 80 },
      { waitLog: 'GFX-S2D-CENSUS', timeout: 300 },
    ],
  },

  // The cathedral's second visit (scene 0x0D step 4): it asks the date of birth
  // after the name form, so the assembly's pass 0 reads a framebuffer that the
  // form scene used. Native called this the stale-backdrop rectangle and fixed
  // it in sessions 56/57; this route is the browser regression target for a
  // black readback there. The form confirms with START then A (session 55's
  // natural run read scene 0x07 -> scene 0x02 1.7 s apart under `start,a`).
  // The route has already passed the first 0x0D, so the final scene wait is the
  // second visit.
  'cathedral-birth': {
    query:
      'OGRE_NO_AUDIO=1&OGRE_TAP_MS=1500&OGRE_TAP_SCENE_BUTTON=title:start:6,title:a:1' +
      ',0x07:start:1,0x07:a:8',
    scene: '0x000D',
    settle: 15000,
    notes: 'title menu -> New Game -> cathedral -> name form -> cathedral (date of birth)',
    steps: [
      { waitScene: '0x0004', timeout: 80 },
      { waitLog: 'GFX-S2D-CENSUS', timeout: 300 },
      { sleep: 12000 },
      { pressUntilScene: 'KeyX', scene: '0x0007', interval: 1400, max: 60 },
      { sleep: 20000 },
    ],
  },

  // The natural name-entry form, reached by pressing A through the cathedral
  // dialogue. The scene-keyed schedule presses START then A eight times once the
  // form is active; `settle` then shows whether that opened the Yes/No prompt or
  // confirmed it (the final wait already passed 0x07, so a scene change during
  // the settle is visible in the capture and the timeline).
  'name-entry': {
    query:
      'OGRE_NO_AUDIO=1&OGRE_TAP_MS=1500&OGRE_TAP_SCENE_BUTTON=title:start:6,title:a:1' +
      ',0x07:start:1,0x07:a:8',
    scene: '0x0007',
    settle: 6000,
    notes: 'title menu -> New Game -> cathedral -> A through the dialogue -> name form',
    steps: [
      { waitScene: '0x0004', timeout: 80 },
      { waitLog: 'GFX-S2D-CENSUS', timeout: 300 },
      { sleep: 12000 },
      { pressUntilScene: 'KeyX', scene: '0x0007', interval: 1400, max: 60 },
    ],
  },

  // The same cathedral with A pressed through the opening movie. Pressing A
  // while the movie plays makes the loader pick the other game framebuffer for
  // the backdrop assembly (`src=0x80025C00`, the 320x476 target at 0x025C00),
  // which is the target whose whole-image RDRAM range contains 0x04B400. This
  // is the case the per-row writeback guard fixes: the old whole-image hash saw
  // 0x04B400's writeback as a change to 0x025C00 and refused the first pass, so
  // the backdrop's top-left quadrant was black.
  'cathedral-skip': {
    query:
      'OGRE_NO_AUDIO=1&OGRE_TAP_MS=1500&OGRE_TAP_SCENE_BUTTON=title:start:6,title:a:1' +
      ',0x0D:a:8',
    scene: '0x000D',
    settle: 6000,
    notes: 'title menu -> New Game -> A through the movie -> the cathedral assembly',
    steps: [
      { waitScene: '0x0004', timeout: 80 },
      { waitLog: 'GFX-S2D-CENSUS', timeout: 300 },
    ],
  },
};

const FLAG_TAGS = { 0: 'normal', 8: 'texel1', 16: 'texel0' };
const KEY_HOLD_MS = 180;

function parseArgs(argv) {
  const opts = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) continue;
    const eq = a.indexOf('=');
    if (eq !== -1) opts[a.slice(2, eq)] = a.slice(eq + 1);
    else if (i + 1 < argv.length && !argv[i + 1].startsWith('--')) opts[a.slice(2)] = argv[++i];
    else opts[a.slice(2)] = true;
  }
  return opts;
}

function normScene(id) {
  if (typeof id === 'number') return '0x' + id.toString(16).toUpperCase().padStart(4, '0');
  const s = String(id || '').trim();
  const n = /^0x/i.test(s) ? parseInt(s, 16) : parseInt(s, 10);
  return Number.isFinite(n) ? '0x' + n.toString(16).toUpperCase().padStart(4, '0') : s.toUpperCase();
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function deadline(ms, what) {
  return new Promise((_, reject) => setTimeout(() => reject(new Error(`TIMEOUT: ${what} (${ms} ms)`)), ms));
}

async function sceneLines(page) {
  return await page
    .evaluate(() => (window.ogreLog ? window.ogreLog.find(/\[scene\]/) : []))
    .catch(() => []);
}

async function lastScene(page) {
  // The dispatcher's own id, so a long run whose first 20000 log lines have
  // been evicted (the `[rsp]` task lines fill the ring in about a minute) can
  // still be waited on. The log remains the fallback for an export that is not
  // built in.
  const id = await page
    .evaluate(() =>
      (typeof Module !== 'undefined' && typeof Module._ogre_web_active_scene === 'function')
        ? Module._ogre_web_active_scene()
        : -1)
    .catch(() => -1);
  if (id >= 0 && id !== 0xFFFF) return normScene(id);
  const lines = await sceneLines(page);
  const m = lines.length ? /id=0x([0-9a-fA-F]+)/.exec(lines[lines.length - 1]) : null;
  return m ? normScene('0x' + m[1]) : '';
}

async function waitScene(page, scene, timeoutSec, log) {
  const want = normScene(scene);
  const t0 = Date.now();
  while ((Date.now() - t0) / 1000 < timeoutSec) {
    if ((await lastScene(page)) === want) {
      log(`scene ${want} after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
      return true;
    }
    await sleep(250);
  }
  log(`scene ${want} not reached in ${timeoutSec} s (last: ${(await lastScene(page)) || 'none'})`);
  return false;
}

async function press(page, key, hold = KEY_HOLD_MS) {
  await page.keyboard.down(key);
  await sleep(hold);
  await page.keyboard.up(key);
}

async function pressUntilScene(page, key, scene, interval, max, log) {
  const want = normScene(scene);
  for (let i = 0; i < max; i++) {
    if ((await lastScene(page)) === want) {
      log(`scene ${want} after ${i} ${key} presses`);
      return true;
    }
    await press(page, key);
    await sleep(interval);
  }
  return (await lastScene(page)) === want;
}

async function runRoute(page, route, scene, log) {
  for (const step of route.steps) {
    if (step.waitScene && !(await waitScene(page, step.waitScene, step.timeout || 30, log))) return false;
    if (step.waitLog) {
      // A route step that waits for a `[GFX-...]` line in the page log. The log
      // is the only clock some screens have: the njpeg backdrop is drawn several
      // seconds after the scene line, and how long the opening movie takes
      // varies with host load.
      const pattern = String(step.waitLog);
      const timeout = step.timeout || 60;
      const t0 = Date.now();
      let seen = false;
      while ((Date.now() - t0) / 1000 < timeout) {
        const n = await page
          .evaluate((src) => (window.ogreLog ? window.ogreLog.find(new RegExp(src)).length : 0),
                    pattern)
          .catch(() => 0);
        if (n > 0) {
          seen = true;
          break;
        }
        await sleep(250);
      }
      log(`log /${pattern}/ ${seen ? 'seen' : 'not seen'} after ` +
          `${((Date.now() - t0) / 1000).toFixed(1)} s`);
      if (!seen) return false;
    }
    if (step.sleep) await sleep(step.sleep);
    if (step.press) await press(page, step.press, step.hold);
    if (step.pressUntilScene) {
      if (!(await pressUntilScene(page, step.pressUntilScene, step.scene, step.interval || 1400,
                                  step.max || 60, log))) {
        return false;
      }
    }
  }
  if (scene && !(await waitScene(page, scene, 120, log))) return false;
  return true;
}

function flagTag(v) {
  return FLAG_TAGS[v] || `f${v}`;
}

function outputPrefix(opts, route) {
  const name = opts.out || path.join(__dirname, '..', 'out', `scene-${opts.route || 'title'}`);
  fs.mkdirSync(path.dirname(path.resolve(name)), { recursive: true });
  return path.resolve(name);
}

async function capture(page, flagsList, prefix, points, pointsSpace, shotSettle,
                       repeat, repeatGap, log) {
  const out = { canvas: null, shots: [] };
  for (let rep = 0; rep < repeat; rep++) {
    for (const flags of flagsList) {
      await page.evaluate((f) => Module._ogre_gfx_debug_flags(f), flags).catch(() => {});
      await sleep(shotSettle);
      const tag = flagTag(flags) + (rep ? `-${rep}` : '');
      const file = `${prefix}-${tag}.png`;
      const png = await page.locator('#game-canvas').screenshot({ path: file }).catch(() => null);
      if (!png) {
        log(`shot ${tag}: screenshot failed`);
        continue;
      }
      const sample = await page
        .evaluate(({ pts, space }) => {
          const c = document.getElementById('game-canvas');
          const g = document.createElement('canvas');
          g.width = c.width;
          g.height = c.height;
          const x = g.getContext('2d');
          x.drawImage(c, 0, 0);
          const sx = space === 'guest' ? c.width / 320 : 1;
          const sy = space === 'guest' ? c.height / 240 : 1;
          return {
            w: c.width,
            h: c.height,
            samples: pts.map(([px, py]) => {
              const cx = Math.min(c.width - 1, Math.max(0, Math.round(px * sx)));
              const cy = Math.min(c.height - 1, Math.max(0, Math.round(py * sy)));
              return { x: px, y: py, canvas: [cx, cy], rgba: Array.from(x.getImageData(cx, cy, 1, 1).data) };
            }),
          };
        }, { pts: points, space: pointsSpace })
        .catch((e) => ({ err: String(e) }));
      out.canvas = { w: sample.w, h: sample.h };
      out.shots.push({ tag, flags, file, samples: sample.samples || [], err: sample.err });
      log(`shot ${tag}: ${file}${sample.err ? ' (sample failed: ' + sample.err + ')' : ''}`);
    }
    if (rep + 1 < repeat) await sleep(repeatGap);
  }
  await page.evaluate(() => Module._ogre_gfx_debug_flags(0)).catch(() => {});
  return out;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  // A route name is optional: `--scene`/`--query` alone is the one-off form and
  // must not inherit the title route's query.
  const routeName = opts.route || (opts.query || opts.scene ? '' : 'title');
  const route = routeName ? ROUTES[routeName] : undefined;
  if (opts.route && !route) {
    console.error(`unknown route '${opts.route}'; routes: ${Object.keys(ROUTES).join(', ')}`);
    process.exit(1);
  }
  if (!route && !opts.query && !opts.scene) {
    console.error('pass --route, or --scene/--query');
    process.exit(1);
  }
  const base = route || { query: '', scene: '', settle: 2000, steps: [] };
  const scene = opts.scene || base.scene;
  const flagsList = (opts.raw ? '0,8,16' : opts.shots || '0')
    .toString()
    .split(',')
    .map((v) => parseInt(v, 10))
    .filter((v) => Number.isFinite(v));
  const points = String(opts.points || '')
    .split(';')
    .filter(Boolean)
    .map((p) => p.split(',').map((v) => parseFloat(v)))
    .filter((p) => p.length === 2 && p.every((v) => Number.isFinite(v)));
  const pointsSpace = opts['points-space'] === 'canvas' ? 'canvas' : 'guest';
  const settle = opts.settle !== undefined ? parseInt(opts.settle, 10) : base.settle;
  const shotSettle = opts['shot-settle'] !== undefined ? parseInt(opts['shot-settle'], 10) : 1200;
  const repeat = Math.max(1, parseInt(opts.repeat || '1', 10));
  const repeatGap = parseInt(opts['repeat-gap'] || '1000', 10);
  const attempts = parseInt(opts.attempts || '3', 10);
  const logCap = Math.max(0, parseInt(opts['log-cap'] || '0', 10) || 0);
  const prefix = outputPrefix(opts, route);

  const cfg = h.resolveConfig(opts);
  if (!cfg.url) {
    console.error('no URL: pass --url or set OGRE_URL');
    process.exit(1);
  }
  let query = [base.query, opts.save ? `OGRE_SAVE=${opts.save}` : '', opts.query || '']
    .filter(Boolean)
    .join('&');
  // The scene line is the route's clock; every route asks for it.
  if (!/OGRE_SCENE_LOG=/.test(query)) query = (query ? query + '&' : '') + 'OGRE_SCENE_LOG=1';
  const url = cfg.url + (cfg.url.includes('?') ? '&' : '?') + query;

  const lines = [];
  const log = (m) => {
    console.log(m);
    lines.push(m);
  };
  const errors = [];
  const started = Date.now();
  const browser = await chromium.launch({
    headless: !opts.headed,
    args: ['--enable-gpu', '--use-angle=metal', '--ignore-gpu-blocklist'],
  });
  let reached = false;
  let captured = null;
  let state = null;
  try {
    for (let attempt = 1; attempt <= attempts && !reached; attempt++) {
      const page = await browser.newPage({ viewport: { width: 900, height: 700 } });
      // The `[rsp]` task line fills the 20000-line ring in about a minute, so a
      // long route's later `GFX-*` lines are otherwise evicted before the log
      // is written. `--log-cap` raises the ring for a run that needs them.
      if (logCap > 0) {
        await page.addInitScript((cap) => { window.OGRE_LOG_CAP = cap; }, logCap);
      }
      page.on('pageerror', (e) => errors.push(`PAGEERROR: ${e.message}`));
      page.on('console', (m) => {
        if (m.type() === 'error') errors.push(`CONSOLE: ${m.text().slice(0, 300)}`);
      });
      try {
        log(`attempt ${attempt}: ${url}`);
        await Promise.race([h.gotoPage(page, { ...cfg, url }), deadline(45000, 'page ready')]);
        await Promise.race([h.loadRom(page, cfg), deadline(45000, 'ROM load')]);
        reached = await runRoute(page, base, scene, log);
        if (!reached) {
          fs.writeFileSync(`${prefix}-log-attempt${attempt}.txt`,
                           await page.evaluate(() => window.ogreLog.text()).catch(() => ''));
          await page.close();
          continue;
        }
        log(`settling ${settle} ms`);
        await sleep(settle);
        // One frame-cadence window, so the log states the screen's rate.
        const before = await h.readState(page);
        const t0 = Date.now();
        await sleep(3000);
        const after = await h.readState(page);
        const dt = (Date.now() - t0) / 1000;
        const tasks = h.taskCount(after.gfx) - h.taskCount(before.gfx);
        log(`tasks ${h.taskCount(before.gfx)} -> ${h.taskCount(after.gfx)} in ${dt.toFixed(1)} s ` +
            `= ${(tasks / dt).toFixed(2)}/s`);
        state = { gfx: after.gfx, status: after.status };
        captured = await capture(page, flagsList, prefix, points, pointsSpace, shotSettle,
                                 repeat, repeatGap, log);
        if (captured && points.length) {
          fs.writeFileSync(`${prefix}-samples.json`, JSON.stringify({
            canvas: captured.canvas,
            points,
            pointsSpace,
            shots: captured.shots,
          }, null, 2) + '\n');
        }
        fs.writeFileSync(`${prefix}-log.txt`, await page.evaluate(() => window.ogreLog.text()).catch(() => ''));
        const timeline = (await sceneLines(page)).slice(-8);
        fs.writeFileSync(`${prefix}-meta.json`, JSON.stringify({
          route: routeName || '(scene/query)', url, scene: normScene(scene), settleMs: settle,
          elapsedMs: Date.now() - started, canvas: captured ? captured.canvas : null,
          shots: captured ? captured.shots.map((s) => ({ tag: s.tag, flags: s.flags, file: s.file })) : [],
          repeat, repeatGap,
          sceneTimeline: timeline, gfx: state ? state.gfx : '', status: state ? state.status : '',
        }, null, 2) + '\\n');
        await page.close();
      } catch (e) {
        errors.push(`attempt ${attempt}: ${e.message}`);
        log(`attempt ${attempt} failed: ${e.message}`);
        await page.close().catch(() => {});
      }
    }
  } finally {
    await browser.close().catch(() => {});
  }
  fs.writeFileSync(`${prefix}-errors.txt`, errors.length ? errors.join('\\n---\\n') : '(none)\\n');
  if (!reached) {
    console.error(`never reached ${normScene(scene)}; see ${prefix}-log-attempt*.txt`);
    process.exit(2);
  }
  console.log(`wrote ${prefix}-*.png, -samples.json? ${points.length ? 'yes' : 'no'}, -log.txt, -meta.json`);
}

main().catch((e) => {
  console.error('FATAL', e);
  process.exit(1);
});
