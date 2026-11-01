# Session 208 handoff — the browser reaches GAME SPEED 2x/4x; the main-thread flush was pacing the game

## Goal

The developer reports that the browser build stutters at GAME SPEED 2x and 4x
and that the speed it reaches is not close to the requested one, while the
native build is fine at the same settings. Find the mechanism in the browser
path, fix it, and measure the result.

## Result

The browser now holds the requested speed and the display frame rate:

| GAME SPEED | guest frames/s before | after | rAF p95 before | after |
|---|---|---|---|---|
| 1x | 59.4 (target 60) | 60.0 | 16.67 ms | 16.67 ms |
| 2x | 119.8 (target 120) | 119.7-120.2 | 50.00 ms | 16.67 ms |
| 4x | 185.4-188.8 (target 240) | 234.5-240.7 | 50.00 ms | 16.67 ms |
| 8x | not measured | 414.8 (target 480) | — | 16.67 ms |

Two faults in `app/src/web_renderer.cpp`:

1. **`flush_commands()` held the renderer mutex across its entire GL batch.**
   The gfx pthread publishes every display list (`publish_exec`) and every VI
   present (`request_present`) through that mutex, and the runtime posts
   `dp_complete` as soon as `send_dl` returns, so the guest waited for the
   GL work. The lock now covers only the queue exchange (swap the texture and
   draw queues, copy the present/sync requests) and is released before any GL
   call. `fb_writeback_pending_` and `present_requests_` became atomics because
   the main thread now clears/reads them outside the lock.

2. **Every draw uploaded ~66 uniforms, 1.88 million WebGL uniform calls per
   second at 4x.** `draw_uniform_key()` (FNV-1a over every value the per-draw
   block uploads, including the live renderer state the block reads) is
   computed per draw; when it equals the previous draw's, the block is skipped.
   The GL program keeps the values. The key is reset per flush because
   `blit_to_canvas()` rewrites the same program's uniforms at the end of one.
   Uniform calls at 4x fell to 495 353/s (-74%; `uniform1i` 1 222 113 -> 446 713)
   and the mean flush cost from 8.0/12.8/11.4 ms (1x/2x/4x) to 2.2/3.9/5.4 ms.

## Evidence

Measurement is one page with the speed changed live, so every pair is the same
screen and the same run. Route: `OGRE_SAVE=assets/escort_suspend.bin
OGRE_SCENE=title` plus the page's scene-keyed taps for Load Game, `OGRE_NO_AUDIO=1`;
speed through `Module._ogre_web_settings_set_game_speed(n)` (the SETTINGS tab's
export), confirmed by the runtime's `[timer] game speed multiplier = n` line.
Headless Chromium with `--enable-gpu --use-angle=metal`, viewport 900x700, on
this machine (i9-9880H, 8 physical cores).

- **Guest rate.** The frame-pump counter `D_800AEFA4` and the VI counter
  `D_800C4BCC` are read as u32 little-endian at RDRAM offsets `0xAEFA4` and
  `0xC4BCC` through `Module._ogre_guest_rdram(0)`. They advanced together in
  every sample: 59.4-60.5/s at 1x, 117.7-120.2/s at 2x, 185.4-192.8/s at 4x
  before the fixes. The 4x shortfall was 20-23% of the target.
- **Display cadence.** A `requestAnimationFrame` recorder on the page measured
  mean 22-27 ms and p95 50 ms at 2x/4x before (1x: 16.67 ms throughout), i.e.
  the page missed vsyncs. After: mean 16.67-17.80 ms and p95 16.67 ms in eleven
  of twelve windows (33.33 ms at 4x once); the worst single frame in a 5 s window
  was 33-63 ms at 4x.
- **The game, not the emulated clock, was the ceiling.** With
  `Module._ogre_gfx_flush` replaced by a no-op at 4x (the gfx pthread still walks
  every display list), the guest reached **239.9 frames/s** and **120.2
  display lists/s**, against 197.1 frames/s and 27.0 DL/s with the real flush.
  So the recompiled game can run at 4x; the main-thread flush was what stopped
  it.
- **Where the flush time went.** A CDP CPU profile at 4x (main thread, 200 us
  sampling) before fix 2: 16.3% idle, 13.6% `wasm-to-js`, 10.3% the
  `std::unordered_map<std::string_view,GLint>` lookup in `uloc`, 6.7%
  `uniform1i`, 5.9% `getWasmTableEntry`, 3.7% `XXH3`, 2.4% `bufferData`. Wrapping
  `WebGL2RenderingContext.prototype.uniform*` on the page counted 1 875 153
  uniform calls/s at 28 440 `drawArrays`/s, i.e. 66 uniform uploads per draw.
- **The DL rate is not the frame rate.** At 1x the guest runs 60 frames/s but
  submits only ~20-30 display lists/s; the mission's scene is redrawn on a
  subset of frames. Session 199's per-second display-list count is the render
  rate, not the game's frame rate.

## Verification

- `EM_CACHE=<workspace>/.em-cache make web` clean; `build-wasm/ogrebattle64.wasm`
  rebuilt. No native build is affected (`web_renderer.cpp` is only in the
  Emscripten source list).
- `debug/probes/scene.cjs` captures after the change: `title`, `tutorial-field`,
  `mission-briefing` (1x) and the `cathedral` route's title-card frame. The title
  frame is visually identical to its pre-change capture. The tutorial field
  (sprites, text box, minimap) and the mission briefing (map, castle inset, NOTE
  text) have the same composition and colour as their pre-change captures, with
  the animated `MISSION` band at a different phase. The cathedral route's
  assembled backdrop was not re-captured to completion (the 50 s-settle attempt
  did not reach it in 12 minutes); the macroblock rectangles it draws go through
  the same draw loop as the mission briefing's rectangles, which render
  correctly. Uniform caching changes no pixels on the screens that were
  compared.
- The speed/rate numbers above were measured four times at 1x/2x/4x (the last
  on the final build, after a whitespace-only edit) and once at 8x; the 4x guest
  rate is 234.5, 237.3, 238.1 and 240.7 across the four runs. Eleven of the
  twelve 5 s windows held rAF p95 at 16.67 ms; one 4x window read 33.33 ms (two
  missed frames in 5 s), so the 4x page is at 60 Hz with an occasional missed
  vsync, not frame-locked.

## Files changed

- `app/src/web_renderer.cpp` — the flush's lock scope, the two atomics, and
  `draw_uniform_key()` plus the per-draw block it guards.

## Probes

None were added to the tree. The measurements used throwaway scripts under
`/tmp` (`measure2.cjs`, `glcount.cjs`, `profile.cjs`, `rendereroff.cjs`,
`throttle2.cjs`, `raf.cjs`) and the existing `debug/probes/scene.cjs`. Nothing to
revert.

## Open

- **8x still falls 14% short** (414.8 of 480 frames/s) with the display at
  60 Hz, so the remaining ceiling is CPU. The next candidates are the per-draw
  GL state calls (`glEnable`/`glDisable`/`glScissor`/`glViewport` are issued per
  draw: 90 243 `viewport`, 90 207 `scissor` and 271 138 `enable`/`disable` per
  second at 4x) and `reference_era_rows`, which XXH3-hashes every guest row a
  draw covers.
- `ogre_gfx_flush` is still called twice per 16 ms timer tick (the interval
  callback and `updateGfxStatus`). It is cheap now, but the second call is
  redundant.
- The 1x display cadence was already 16.67 ms before this session, so the
  2x/4x stutter was not a 1x problem that scaled.
