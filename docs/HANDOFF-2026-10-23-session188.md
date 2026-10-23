# Session 188 handoff — the browser's tile-image cache allocated a GL texture per image per display list

## Goal

The developer reported that `http://127.0.0.1:8931/app/web/index.html`
freezes after 2–3 minutes with no input. Find the cause and fix it.

## Result

The WebGL renderer's tile-image cache never evicted a texture. Every
`G_LOADTILE`, `G_LOADBLOCK` and `G_LOADTLUT` increments `tmem_generation_`
(`app/src/web_renderer.cpp:6406`), and `ensure_tile_image` seeds the cache key
with that counter (`:6501`). The same tile image therefore received a different
key on every display list even when the decoded bytes were identical.
`flush_commands` created a GL texture for every key it had not seen (`:3753`),
and the only loop that deleted them was `shutdown()` (`:4679`). The
de-duplication in `ExecCtx::uploaded` is per display list, so it did not bound
the cache.

The fix sweeps the cache at the end of every flush (`:4333`). It keeps the keys
named by the batch's textured draws (`cmd.tex_key`, `cmd.tex_key1`) and deletes
every other entry. A key the batch does not name cannot be sampled later:
textures are uploaded in the same batch as their draws (`ExecCtx` and the tile
state are per display list), so a later list either reloads the image, which
bumps `tmem_generation_` and produces a new key, or reuses that generation's key
and queues its own upload. `glDeleteTextures` after the draws are issued is
safe; GL defers destruction until the issued draws stop referencing the
texture.

`ogre_gfx_stats()` now reports `tex: cache=N uploads=N released=N`, so the
bound is visible in `#gfxstats`.

## Measurements

The probe booted the page in headless Chromium with no input, loaded
`assets/ogre64.z64`, and wrapped `WebGL2RenderingContext.prototype.createTexture`
and `deleteTexture` in an init script. Software WebGL ran the game at about
0.75 display lists/s, so these runs reach fewer display lists than a hardware
browser does in the same wall time; the live count and the bytes are the
measurement, and they do not depend on the rate.

| build | t | display lists | texture loads | created | deleted | live | live bytes |
|---|---|---|---|---|---|---|---|
| before | 148 s | 111 | 5,062 | 5,064 | 1 | 5,063 | 20.3 MB |
| after | 150 s | 129 | 5,754 | 5,871 | 5,867 | 4 | 0.9 MB |
| after | 232 s | 279 | 10,690 | 10,686 | 10,679 | 7 | 4.4 MB |

A second after-fix run used Playwright's Chromium with `--enable-gpu
--use-angle=metal --ignore-gpu-blocklist`, which reached the rate a hardware
browser runs at. In 117 s it reached 2,798 display lists (about 24/s) with
332,175 textures created, 332,167 deleted and 8 live (5.6 MB, peak 8.0 MB).
`page.evaluate` round trips stayed at 1–200 ms and the page logged no errors.
Before the fix the cache kept every texture it created (the before-fix run
deleted 1 of 5,064), so those 332k would all have stayed live; at the measured
45.6 uploads per display list, a 60 Hz page passes that count in under two
minutes.

The live count before the fix grows with the texture-load counter, about 45.6
uploads and 0.18 MB per display list. At the game's 60 Hz that is about 2,700
uploads/s and 11 MB/s, roughly 2 GB of live GL textures by three minutes, plus
one driver object per texture on the browser main thread. The counter run before
the fix reached 5,063 live textures with one deletion; after the fix `live`
stays at the four permanent textures (the 1x1 white texture, the present
texture and the framebuffer targets) plus the occasional larger scene, and
`deleted` tracks `created`.

The JS heap stayed at 87.5 MB in the before-fix run, so the growth was entirely
in GL objects.

## Visual check

`debug/probes/shots.cjs --attempts 4 --secs 60 --shots 5 --gap 1500 --out
/tmp/shots-188` waited for a real display list and captured five frames at
`tasks=16..72` with `nonBlack=25288..70332`. The scene-`0x09` opening frame shows
the soldiers, the cube and the cloud background (`/tmp/shots-188-03.png`).

## Correction: `boot.cjs` reports the launcher overlay as "rendered"

`debug/probes/boot.cjs` measures the PNG of the `#game-canvas` element.
Playwright's element screenshot includes everything painted over that element,
and the session-187 shell places the launcher card over the canvas. A run now
reports `RENDERED` with `w=640 h=481 nonBlack=307840 colorful=7060` while
`#gfxstats` still reads `(no display lists submitted yet)`, and the saved
canvas PNG is the launcher. The `nonBlack=307840` in session 187's recorded
`boot.cjs` result is the same overlay, not a game frame. `shots.cjs` is the
render check for the current shell because it waits for `tasks>=2`. `boot.cjs`
was not changed.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | per-flush sweep of `gl_textures_`; `texture_cache_size`/`texture_uploads`/`texture_released` accessors; `tex:` line in `ogre_gfx_stats()` |
| `docs/HANDOFF-2026-10-23-session188.md` | this file |
| `docs/DECISIONS.md` | durable-decision row |
| `docs/STATUS-LOG.md`, `docs/README.md` | session entry and index row |
| `PLAN.md` | browser status paragraph and current-state pointer |

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean
  (`build-wasm/ogrebattle64.wasm`).
- 150 s no-input run before and after the fix, texture counters as above.
- 232 s no-input run after the fix: live 7, bytes 4.4 MB (peak 5.3 MB), display
  lists advancing (279 at the end).
- 117 s no-input run after the fix with `--enable-gpu --use-angle=metal`: 2,798
  display lists, 332,175 textures created, 332,167 deleted, 8 live / 5.6 MB,
  peak 8.0 MB, `page.evaluate` 1–200 ms.
- `debug/probes/shots.cjs` captured a rendered opening series.
- `debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`,
  `--flags 128 --expect-depth near` and `--flags 384 --expect-depth far`.

## Probes used and reverted

No probe was added to the tree. The two measurement scripts live in `/tmp`
(`/tmp/freeze-probe.cjs`, `/tmp/texcount-probe.cjs`); the second wraps the
WebGL2 prototype from Playwright and is not part of the repository. The `tex:`
stats line is a permanent diagnostic, not a probe.

## Open

- The cache is still rebuilt on every batch: an unchanged tile image is decoded
  and uploaded again on the next display list. A content-addressed key (hash of
  the TMEM bytes, with an LRU cap for animation) would remove that churn. It is
  a performance item, not the freeze.
- The GPU measurement uses headless Chromium with ANGLE Metal
  (`--enable-gpu --use-angle=metal`), and not the developer's full browser. The
  live-count bound does not depend on the frame rate: every batch sweeps what
  it did not name.
- `boot.cjs` needs a canvas-only capture (or a `tasks>=2` gate) before its
  `RENDERED` result means a game frame on the current shell.
