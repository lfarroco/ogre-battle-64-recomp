# Handoff — 2026-09-10 — session 19: first real game frame rendered in the browser

## Outcome

- **The browser build now renders the game's own display list.** The title
  screen's DL (48 vertices → 24 triangles, 1 texrect, 1 fillrect, 24 texture
  loads, 38 matrix loads) executes, its draw commands reach WebGL2, and the
  canvas shows ~21 000 non-black pixels of game content instead of a uniformly
  black screen (reproduced 3 of 5 runs; the other 2 captured a dark animation
  frame — see *Evidence*). This is the first time OB64 graphics have reached the
  browser canvas.
- **The game now submits frames continuously** (`task 2, 3, 4, 5, …`, roughly
  one frame per flush cycle, `flushed` tracking `queued`), instead of exactly
  one boot blanking DL for the whole run.
- **Every root cause found this session was in the WebGL2 prototype renderer**
  (`app/src/web_renderer.cpp`) — not in the recompiled game, the scheduler, the
  heap allocator, or the emulation. Sessions 16–18's "cb88 / frame-2 wall" and
  "heap-walk wedge" theories did not survive contact with the renderer code
  (see *Corrected project model* below).
- No runtime (`tools/N64ModernRuntime`) changes were needed; the vendored
  patch is unchanged this session.

## The renderer bugs (all fixed in `app/src/web_renderer.cpp`)

1. **Browser main thread blocked on the renderer mutex → hard page freeze.**
   `send_dl()` held `mutex_` across the *entire* DL analysis and execution, and
   `ogre_gfx_flush()` (browser main thread, every 16 ms from `web.js`) took the
   same mutex with a blocking `std::lock_guard`. A contended pthread mutex
   cannot legally be waited on by a browser main thread, and the page
   hard-froze exactly when the title path advanced (main-thread heartbeat
   stopped, `page.evaluate` timed out, no `pageerror`). Fixed by `try_lock` +
   skip-the-tick, counted in `flush_skipped_`.
2. **Non-recursive mutex self-deadlock in `queue_triangles()`.** `send_dl()`
   already held `mutex_`, and `queue_triangles()` locked it again. The gfx
   pthread deadlocked on the first draw command a *real* DL produced and held
   the lock forever, so the main thread could never flush (canvas black) and
   the game's graphics thread stalled. Fixed structurally: the DL walk now
   records into an execution-local buffer (`ExecCtx::draws` / `ExecCtx::tex`)
   and only briefly locks to publish the results.
3. **`read_matrix()` mis-decoded the N64 `Mtx` layout.** A `Mtx` is a 4x4 array
   of `s16` integer parts followed by a 4x4 array of `u16` fraction parts; the
   old code read 16 consecutive `s16` values, i.e. only the integer halves, so
   most matrices decoded as all-zero and every 3D triangle was rejected as
   "behind the camera" (`w <= 0`). After the fix, rejection dropped from 24/24
   to ~18/24 triangles per frame (the remainder is the missing viewport).
4. **Segment registers stored the full base instead of its high byte.** The RSP
   keeps only `base >> 24` and resolves segmented addresses as
   `(segment << 24) | offset`; storing the full base made every segmented
   matrix/vertex address resolve to garbage.

Two smaller correctness fixes on the way:

- `G_SETPRIMCOLOR` / `G_SETENVCOLOR` / `G_SETFILLCOLOR` decoded `w1` with R and
  B swapped. `w1` is `(R<<24)|(G<<16)|(B<<8)|A`.
- The fragment shader's `COMBINED` input (combiner selector 0) unconditionally
  returned `0.0`; in cycle 2 it must return cycle 0's result, which turned every
  two-cycle combiner black.

## Evidence (browser probes, `/tmp/ogre-probe`)

Per-DL diagnostic line (now emitted for tasks ≤ 8 and every 120th task):

```
[GFX-DRAW] task 2: cmds=1004 queued=8 rejected=18 ndc_x=[-1.19,3.25] ndc_y=[-2.21,1.70]
           gl_ready=1 skipped=0 flushed=0/2056 analyze=0ms exec=1ms
[GFX-DRAW] task 3: cmds=1004 queued=16 rejected=36 ... flushed=8/2094
[GFX-FLUSH] first non-empty main-thread flush: 2 draw cmds, 24 textures uploaded
```

Canvas pixel histogram (320x240 canvas, decoded from a compositor screenshot):

| run | black | border | game content |
|---|---|---|---|
| before fixes | 307180 | 2208 | 0 |
| after fixes  | 286172 | 2208 | **21064** (brown/orange, `#420`/`#210`) |

The DL content is identical run to run (`[GFX-DRAW] task 2: cmds=1004
queued=8 rejected=18 ndc_x=[-1.19,3.25] ndc_y=[-2.21,1.70]`), but the capture
moment is not: the scene animates, so a screenshot can land on a mostly-dark
frame. Reproduced 3 of 5 runs at 21064 non-black pixels, 2 of 5 at ~260 (a dark
animation frame). The capture point in `probe-final.cjs` is 2.5 s after the
first real task.

Textures decode correctly (`[GFX-TEX] tex 1: 16x34 key=… nonzero=219/544
avgRGB=602`), so the remaining visual gap is transform/viewport, not asset
decode.

## Corrected project model

Sessions 16–18 concluded that the frame-2 wall was a NULL `cb88` callback
(`*(0x800A9E88)`) and that advanced runs wedge in the game heap walk. Both are
wrong or misleading:

- **The key-8 / `cb88` task is the boot blanking task, not the frame task.**
  `func_8008A1B0` enqueues it with `a3 = 0` → pipe `0x800B9C84` (tag 8) → the
  NULL `cb88`. It is *supposed* to be a no-op. The steady-state frame task uses
  `a3 = 1` → pipe `0x800B9C86` (tag 4) → `cb84 = 0x8008B110 = osViSwapBuffer`.
- **The real frame submission gate is `D_800E810C`.** `func_80072398` is the
  per-frame render function: the block at `0x800724E8` submits (builds the
  wrapper DL at `D_800E9BA0`, calls `func_80073AE4`, which enqueues the key-4
  task and swaps), and the block at `0x8007260C` runs the update and sets
  `D_800E810C = 1` (gated on `D_800AEFA0 ∈ {0, 0x83}`). "No frame 2" simply
  meant this loop was not running to completion.
- **How it is driven:** `func_80089990` registers the frame callback into
  `D_800AA090`; `func_8008AFE0` is the retrace driver that receives on
  `0x800C4C28` and calls `D_800AA090(D_800E79A4)` on key 1.
- The "heap-walk wedge" was a downstream symptom: the frozen browser main
  thread and the self-deadlocked renderer mutex stalled everything, and the
  allocator storm (`func_80071A3C`/`func_80071E74`) was the title screen
  legitimately building its display list.

## What is still open (ordered)

1. **Implement `G_MOVEMEM MV_VIEWPORT`** (currently `case OP_MOVEMEM: break;`).
   The RSP viewport (scale/translate/origin, `w1` → 4x `s16` scale + 4x `s16`
   translate) is never applied, so 3D geometry lands partly off-screen
   (`ndc_x` up to 3.25) and ~18/24 triangles are still rejected. This is the
   highest-value next step and should make the title screen recognisable.
2. **Combiner mux tables.** The GLSL selector mapping is an approximation and
   does not match RT64's `colorInputA/B/C/D`
   (`tools/RT64/src/shared/rt64_color_combiner.h`): e.g. C 7..15 are
   `COMBINED_ALPHA / TEXEL0_ALPHA / TEXEL1_ALPHA / PRIMITIVE_ALPHA /
   SHADE_ALPHA / ENV_ALPHA / LOD_FRACTION / PRIM_LOD_FRAC / K5`, and A/B/D have
   their own tables. Regenerate the mapping from that header.
   `src_rgb`/`src_a` also lack LOD_FRACTION, PRIM_LOD_FRAC, NOISE, K4/K5 and
   TEXEL1.
3. **Texture binding.** A single texture unit is used; `cmd.tex_scale/tex_origin`
   come from the *last* tile load, which is wrong when several tiles are live in
   one frame (the title DL loads 24 textures).
4. **Renderer performance.** `flush_commands()` calls `glGetUniformLocation`
   for ~35 uniforms per draw command. Cache the locations.
5. Untouched from earlier sessions: native-vs-browser divergence, latent
   unyielded poll loops, the milestone buffer's 32 KB truncation
   (`app/src/milestones.hpp` stops mirroring to the page once full — the
   `[stderr]` copy still works), and the stale hard-coded "boot-stall" message
   in `app/web/web.js`.

## Instrumentation added (kept, all capped/opt-in)

- `[GFX-DRAW]` per-DL summary: command count, queued/rejected draws, NDC extent,
  flush counts, analyze/exec ms.
- `[GFX-FLUSH]` first non-empty main-thread flush.
- `[GFX-TEX]` first 6 texture uploads with decoded-content statistics.
- `[GFX-CMD]` first 6 presented draw commands (combiner mux, prim/env, scissor,
  first vertex).
- `[GFX-REJ]` first 2 rejected triangles with the modelview/projection matrices.
- `[GFX-EXEC]` opt-in per-command DL trace: `OGRE_TRACE_DL=1`. **Off by default
  on purpose** — every trace line is a proxied stderr write from the gfx
  pthread, which throttled a healthy 1004-command DL enough to look like a
  hang.
- `ogre_gfx_stats()` now appends `exec:` and `load:` telemetry lines
  (non-perturbing atomics updated once per DL command) so a stalled walk can be
  located from the 1 Hz stats poll.

## Repro

```sh
# build (needs the Emscripten cache outside the repo)
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8

# serve the repo root on :8931 (COOP/COEP required for pthreads) — already
# running in this session as a static server rooted at /Users/momo/dev/ogre
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8931/app/web/index.html
```

Probe harness (scratch, outside the repo): `/tmp/ogre-probe/`
- `probe-final.cjs [attempts] [secsPerAttempt] [prefix]` — the probe to use: it
  retries until the run lands on the advanced trajectory, then saves
  `-canvas.png`, `-gfxstats.txt`, `-milestones.txt`, `-statustail.txt`.
- `probe-exec.cjs [prefix] [attempts]` — prints the `exec:`/`load:` telemetry.
- `probe-testdraw.cjs` — calls `Module._ogre_gfx_test_draw()` to isolate the GL
  path from the game's DL.
- `probe-long.cjs`, `probe-diag.cjs`, `probe-locate.cjs`, `probe-freeze.cjs`,
  `probe-render.cjs` — earlier diagnostics from this session.
- Analysis used here: `sips -s format bmp <png> --out out.bmp` then a small
  Python pass to histogram BGRA (`sips` ships with macOS; no PNG decoder needed
  in Node).

**Trajectory bifurcation is still real:** roughly one in two boots stays on the
idle trajectory (`titledisp idx=0`, one boot DL). Any probe must retry.

## Files changed (tracked)

- `app/src/web_renderer.cpp` — all of the above (renderer fixes + diagnostics).
- `tools/RT64` — pre-existing submodule pointer change, not touched this session.

## Next session

1. Implement the RSP viewport (`G_MOVEMEM MV_VIEWPORT`) and re-check triangle
   rejection / NDC extents.
2. Regenerate the combiner mux mapping from RT64's header.
3. Only then re-evaluate whether the title screen is recognisable and whether
   the long-standing "state divergence" questions from sessions 17–18 still
   exist (they may have been entirely renderer-side).
