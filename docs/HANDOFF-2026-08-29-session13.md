# Handoff — 2026-08-29 (session 13): Milestone 6 workload analyzer + Milestone 7 WebGL2 renderer prototype; boot-stall root-cause leads

## TL;DR

- **Milestone 7 (browser renderer prototype) is implemented and verified in
  headless Chrome**: a WebGL2 renderer (`app/src/web_renderer.cpp`) parses the
  game's F3DEX2 display lists, executes them, decodes N64 textures, evaluates
  the general two-cycle color combiner in GLSL, and draws to a `<canvas>`. A
  synthetic test display list (fill rect + a 2×2 RGBA16 texture) renders
  correctly: grey fill + the four texture colors in the right places, no GL
  errors.
- **Milestone 6 (workload analysis) infrastructure is done** (`app/src/gbi.*`,
  `ogre_gfx_stats()`); real per-frame data is blocked by the game never leaving
  the title (it submits only the 32-command boot blanking DL on every platform).
- **Boot-stall root causes found (leads for next session)**:
  1. The game's frame producer (`func_8008949C`) waits for the VI framebuffer
     to match the frame target, but **the game never calls `osViSwapBuffer`**
     in this state (0 calls traced), so the VI stays on the dummy framebuffer.
  2. The task manager (`func_800893C0`) is gated by phase byte `D_800C4800`,
     whose advance function `func_8009A770` polls **SI_STATUS MMIO
     `0xA4040010` bit 0** — the runtime does not emulate SI_STATUS, so the
     phase never reaches the rendering state.
  3. Separately, **the game's threads freeze ~180 ms into a long native run**
     (wall-time `trace_millis` stops advancing while the renderer keeps
     swapping) — a racing scheduler/threading stall.
- **Emscripten gotchas solved**: macOS `/tmp`→`/private/tmp` breaks emcc's GL
  library build (use `EM_CACHE=/Users/momo/.cache/emscripten-ogre`), GL from
  pthreads doesn't work (render on the main thread instead), GLSL `#version`
  must be on line 1, rdram words are byte-reversed, F3DEX2 tile indices are in
  w1 bits 24-26, and physical addresses above 16 MiB were being truncated.

## What was built

### Milestone 6 — F3DEX2 workload analyzer (`app/src/gbi.{hpp,cpp}`)

- `gbi::walk_dl` — dependency-free F3DEX2 DL walker (G_DL push/branch, segment
  registers, G_BRANCH_Z 16-byte commands, loop/budget protection), shared by
  the analyzer and the renderer.
- `gbi::analyze_dl` / `gbi::format_summary` — command histogram, geometry
  (VTX/TRI1/TRI2/QUAD/TEXRECT/FILLRECT), texture loads (SETTIMG
  format/size/width counts, LOADTILE/LOADBLOCK/LOADTLUT/SETTILE/SETTILESIZE),
  unique combiner configs (full 64-bit SETCOMBINE values), OTHERMODE values,
  RDP state counts.
- Wired into the null renderer (`null_renderer.cpp`, `ogre_gfx_stats()` export)
  and mirrored to the web page (`#gfxstats` panel).

### Milestone 7 — WebGL2 renderer prototype (`app/src/web_renderer.cpp`)

- F3DEX2 DL execution: G_MTX/G_POPMTX (row-vector FixedMatrix), G_VTX,
  G_TRI1/TRI2/G_QUAD (modelview × projection, perspective divide, viewport,
  y-flip), G_TEXRECT/G_TEXRECTFLIP/G_FILLRECT, G_SETSCISSOR, blend (standard
  XLU), and the general 2-cycle combiner (RT64 mux layout) in the fragment
  shader.
- N64 texture decode on LOADTILE/LOADBLOCK/LOADTLUT: RGBA16/32, IA16/8/4,
  I8/I4, CI8/CI4 + TLUT. RGBA16 verified end-to-end.
- **Render-on-main-thread architecture**: the gfx pthread executes DLs into a
  `DrawCmd`/`TexUpload` queue; `app/web/web.js` polls `ogre_gfx_flush()` every
  16 ms on the browser main thread, which issues all GL. (Emscripten's
  pthread GL proxying failed in this app — see below.)
- `ogre_gfx_test_draw()` — synthetic DL (fill rect + textured rect) to validate
  the pipeline while the game only submits its boot blanking DL.
- Web shell: `<canvas id="game-canvas">`, WebGL2 context created on the main
  thread (`ogre_gfx_create_context`), handoff via `ogre_gfx_set_canvas`.

### Verification (headless Chrome, playwright; ROM `assets/ogre64.z64`)

```text
PASS WebGL2 context created and handed to the renderer
PASS renderer initialized (or DL submitted)
PASS canvas screenshot captured
PASS canvas has non-black content (after test_draw) — 3072 px, max=765
PASS no page errors
[test] canvas pixels: red=1024 green=1024 blue=512 white=512 (the 2x2 texture)
```

The native null build still boots and analyzes (tasks=1 dls=2 cmds=32
unknown=0) with the same DL content as before.

## Boot-stall analysis (why the title never advances — all platforms)

1. **The frame producer waits for the VI framebuffer.** Thread 17
   (`func_800893C0`) receives the per-VI frame wrapper from thread 3, and
   `func_8008949C` loops until `osViGetCurrentFramebuffer() == wrapper+0xC` (or
   the next framebuffer) before the task is enqueued to the SP loader queue
   `0x800B9C40`. Runtime VI tracing shows the game **never calls
   `osViSwapBuffer`** — the VI remains on the runtime dummy framebuffer
   (`0x80700000`), so the match never succeeds and, once the boot's single
   blanking task is done, no further tasks are submitted.
2. **The task manager is phase-gated.** `func_800893C0` checks `D_800C4800 &
   2` and skips the framebuffer wait + submission when set. The phase byte is
   driven by the VI-retrace handler `func_80088F08`, whose countdown calls
   `func_80098030` → `func_8009A770(0)` to advance. `func_8009A770` reads
   **SI_STATUS MMIO `0xA4040010` bit 0**; when set it writes the argument to
   `0xA4080000` and returns 0 (rendering phase); when clear it returns −1 and
   the countdown repeats forever. The runtime has no SI_STATUS emulation.
3. **Game threads freeze ~180 ms in.** In a 100 s native run, `trace_millis`
   (wall time) stopped advancing at ~179 ms while the renderer kept swapping —
   the game threads stop being scheduled. This races (different runs get
   different boot progress). Separate from the SI_STATUS gating.

Suggested next steps: emulate SI_STATUS (set bit 0 during controller reads or
periodically) so `func_8009A770` returns 0 and the game enters its rendering
phase; then chase the ~180 ms game-thread freeze (scheduler starvation /
drainer deadlock / debug-print slowdown — session 10's "gate the debug
logging" lead).

## Emscripten/WebGL gotchas (recorded for future sessions)

- **`EM_CACHE` must not live under `/tmp`** on macOS: `/tmp` → `/private/tmp`
  makes emcc's relative-path computation for its GL system library off by one
  (`system/lib/gl/gl.c` not found). Use
  `EM_CACHE=/Users/momo/.cache/emscripten-ogre` (created this session; the
  build requires write access there).
- **GL from pthreads failed** in this app: `emscripten_webgl_make_context_current`
  from the gfx worker returned INVALID_PARAM even proxied to the main thread.
  Adopted the render-on-main-thread architecture instead (works, no
  `-sOFFSCREENCANVAS_SUPPORT`).
- **GLSL `#version` must be the first line** — the raw-string shaders began
  with a newline and failed at line 2.
- **rdram words are byte-reversed** (LE on host; runtime `MEM_W` is a direct
  read). All DL/texture reads use a LE read (`rd32`/`rd16`).
- **F3DEX2 tile indices are in w1 bits 24-26** for SETTILE/SETTILESIZE/
  LOADTILE/LOADBLOCK/LOADTLUT (I initially read w0 — wrong). SETTILE's
  clamp/mirror fields are 2-bit (`cmt` w1[18:19], `cms` w1[8:9]); mask/shift
  fields are at w1[0:17]. See RT64 `GBI_RDP::setTile`.
- **Physical addresses above 16 MiB were truncated** (`addr & 0xFFFFFF`) in
  both the analyzer and the renderer's address resolution; fixed to
  `& 0x1FFFFFFF` (the port's rdram is 512 MiB).

## Files

- `app/src/gbi.hpp`, `app/src/gbi.cpp` — F3DEX2 walker + workload analyzer.
- `app/src/web_renderer.cpp` — WebGL2 renderer (M7 prototype).
- `app/src/null_renderer.cpp` — now runs the analyzer + `ogre_gfx_stats()`.
- `app/CMakeLists.txt` — WebGL2 flags/exports; web renderer source selection.
- `app/web/index.html`, `app/web/web.js` — canvas, WebGL2 context handoff,
  `ogre_gfx_flush` polling, `#gfxstats` panel.
- `docs/WEB-PORT.md`, `docs/WEB-PORT-REPORT.md` — milestone + report updates.
- Vendored runtime (`tools/N64ModernRuntime`, gitignored): temporary
  `OGRE_DEBUG_VI` tracing in `ultramodern/src/events.cpp` (env-gated; kept for
  future VI debugging). Re-apply with `n64modernruntime-ob64.patch` on re-clone
  and re-add if needed.

## Build notes

```sh
# native null (unchanged)
cmake -S app -B build-null -DCMAKE_BUILD_TYPE=Release -DOGRE_USE_RT64=OFF
cmake --build build-null -j

# wasm (WebGL2) — cache MUST NOT be under /tmp (macOS /tmp symlink bug)
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake -S app -B build-wasm
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j
```

Headless-Chrome tests: `/tmp/ogre-web-test/probe-webgl.cjs <rom>`
(regression: boot + renderer init + test_draw renders), `probe-final.cjs`,
`probe-grid.cjs`. Static server with COOP/COEP: `/tmp/ogre-web-test/serve.cjs`.

## Next steps

1. **Emulate SI_STATUS bit 0** so the game enters its rendering phase, then
   re-measure the workload (M6) and watch the real title screen render (M7).
2. **Chase the ~180 ms game-thread freeze** (scheduler/drainer; consider
   gating the runtime debug prints — session 10 lead).
3. Then: framebuffer/VI presentation in the web renderer (update_screen is a
   no-op; direct-to-canvas), TEXEL1/second-texture support, framebuffer-as-
   texture, and the renderer's remaining rough edges (LOADBLOCK dxt, lighting).
