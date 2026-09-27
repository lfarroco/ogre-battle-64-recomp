# Session 109 handoff — the browser build runs again, and the renderer gap is measured

## Goal

Developer question: the browser renderer was left unfinished when the project
moved to the native port; can that work resume now that the native builds play
the game? The chosen first step was to repair the browser runtime, then decide
the renderer.

## Result

The wasm build compiles and runs the game again. Two defects blocked it, and
neither was in the renderer:

1. **A compile error.** `app/src/bank_overlays.cpp` included the native-only
   `sdl_platform.hpp` (added 2026-09-16 by `2781fc9` for the live console), and
   `sdl_platform.cpp` is not in the Emscripten source list. `cmake --build
   build-wasm` failed on it. The include and its one call site are now
   `#ifndef __EMSCRIPTEN__`; the browser build loses only the
   `OGRE_CONSOLE_ON_CMD` trigger.
2. **The boot "freeze" was a process exit.** `app/src/main_web.cpp` registered
   `entry.save_type = recomp::SaveType::None`, while `app/src/main.cpp`
   registers `Sram`. The game's first SRAM DMA at boot takes the
   `!recomp::sram_allowed()` branch in `librecomp/src/pi.cpp:143`, calls
   `message_box` and `ULTRAMODERN_QUICK_EXIT()`. The thread snapshot and the
   growing retrace backlog after it were the runtime's host threads outliving
   the game, and not a lost scheduler wakeup. `main_web.cpp` now registers
   `Sram`, with `main.cpp`'s evidence cited in the comment.

The renderer half of the assessment: `app/src/web_renderer.cpp` renders the
title's sprite layer and mangles the title's 3D content and the menu screens.
It is missing the S2DEX2 list type, YUV16 textures and the RDRAM framebuffer
indirection. The route recommendation is in `docs/DECISIONS.md`.

## Evidence

### 1. The build error

```
/Users/momo/dev/ogre/app/src/bank_overlays.cpp:14:10: error: "To use the
emscripten port of SDL use -sUSE_SDL or -sUSE_SDL=2"
#include "sdl_platform.hpp"  // ogre::console::exec (OGRE_CONSOLE_ON_CMD)
```

`git log -S'sdl_platform.hpp' -- app/src/bank_overlays.cpp` gives `2781fc9`
(2026-09-16, the live console). The last recorded wasm artifact before this
session was `build-wasm/ogrebattle64.wasm` from 2026-09-14, so the build had
been broken for eleven days.

### 2. The boot freeze is the SRAM exit

A traced boot (`OGRE_STATUS_FILTER = /$^/` and a 2,000,000-line page log, see
"Probes") ends the game's own trace at T=352 ms:

```
[mq] T=352 osCreateMesgQueue mq=0x800C6C80 buf=0x800C6C98 count=1 (thread 3)
[pi] inline DMA dram=0x8024F3A0 dev=0x00000000 size=0x100 type=0xF handle=0x800BE110 base=0x08000000
Attempted to use SRAM saving with other save type
Exiting with exit status '1'. Function pi_perform_dma, at file .../librecomp/src/pi.cpp:143
```

`base=0x08000000` is `recomp::sram_base`. After that line only the VI thread's
`[vi-debug] retrace` lines and the periodic `[snap]` dumps appear, which is why
the earlier reading of the run looked like a scheduler stall:

- `extbacklog` grows at the VI rate (118 at T=1998 ms, +90 per 1.5 s snapshot)
  because no game thread is left to drain it.
- `running_queue: t200(pri5)`, `resumes:` frozen, `retrace(D_800C4BCC)` frozen
  at 0xC-0x15: the game is gone, and the host VI/audio/gfx threads keep running.
- `mq=0x800E8B84 count=0/8 recv_wait=t19`: t19 was blocked when the process
  exited.

The native null build on the same tree submits 2100 display lists in a 20 s
wall-clock run at `OGRE_SPEED=4`, so the runtime itself is healthy.

### 3. Verification after the two fixes

`EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j8`
links (`ogrebattle64.wasm` is 21 MB). Served by `python3 debug/server.py`:

| check | command | result |
|---|---|---|
| first real frame | `node probes/boot.cjs --attempts 2 --secs 50 --out sram-fix` | `RENDERED`, 24840 non-black / 15108 colourful pixels of 307200; canvas is the title (`docs/proofs/web-title.png`) |
| sustained frames | `node probes/progress.cjs --attempts 1 --secs 80` | `maxTasks=40`, `badWalk=no`; 1142 triangles, 137 texrects, 1234 `SETTIMG` (RGBA16, RGBA32, CI8, I8), 5 unique combiners |
| New Game flow | the throwaway driver in "Probes" | 792 display lists in 100 s, all ucode `0x8009F540`; the game sits in scene `0x04` (`titledisp: idx(D_800E810E)=0x0004`) and the 3D content renders wrong (`docs/proofs/web-opening-broken.png`) |

The audio microcode runs in the browser build too. `app/src/rsp.cpp` is shared,
`RspFuncs/audio_ucode.cpp` is in the build, and a run logs
`[rsp] task type=2 submitted (audio microcode)` and
`[web:audio] game is queueing audio (21 frames buffered, muted)`. The web layer
still mutes by default (`app/web/web.js`, session 22) and `?audio` unmutes it.
`PLAN.md`'s "its audio ... still points at the stub" was stale.

### 4. What the WebGL2 prototype cannot draw

Three gaps, all on the game's normal path:

1. **No S2DEX2.** `WebGLRenderer::send_dl` always runs the F3DEX2 walker, and
   `app/src/gbi.hpp` defines `OP_MTX = 0xDA` and `OP_MOVEMEM = 0xDC`. In an
   S2DEX2 list those two opcodes are `G_OBJ_RECTANGLE_R` (the njpeg macroblock
   draw) and `G_OBJ_MOVEMEM` (session 51). An S2DEX2 list is therefore walked
   as F3DEX2 matrix and movemem commands.
2. **No YUV16.** `grep -ci yuv app/src/web_renderer.cpp` is 0. The njpeg
   macroblock textures are YUV16, and RT64 needed the two-plane TMEM load and
   the sign-extended `G_SETCONVERT` matrix before the cathedral rendered
   (sessions 50-52).
3. **No RDRAM framebuffer indirection.** The renderer draws to the canvas and
   `update_screen()` is empty. Scene `0x02` assembles its 2x2 background from
   CPU copies of the framebuffer in RDRAM (`func_ovlE_8019976C`, session 57),
   so those bytes are never written.

The rendered title shows gap 3 indirectly: the sprite layer draws and the fog
layer does not, and the fog is the `OGRE_FOG` repair that `app/src/renderer.cpp`
applies for RT64 only (session 35).

## Renderer recommendation

Recorded as a durable decision in `docs/DECISIONS.md`. In short: the WebGL2
prototype is not the route to a browser build that plays the game as the native
build does, and the converging route is RT64 with a WebGPU backend. The
decision entry carries the cost figures and the alternative.

## Files changed

| file | change |
|---|---|
| `app/src/bank_overlays.cpp` | `#ifndef __EMSCRIPTEN__` around the `sdl_platform.hpp` include and the `console::exec` call |
| `app/src/main_web.cpp` | `entry.save_type = recomp::SaveType::Sram`, with the evidence comment |
| `docs/proofs/web-title.png` | the browser title frame (`boot.cjs` canvas) |
| `docs/proofs/web-opening-broken.png` | the browser in the opening flow, renderer output wrong |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/WEB-PORT.md`, `docs/README.md` | status, decision and index |

No probe was left in the tree. `git status --short` shows the two app sources,
the four documents, the two proofs and the pre-existing dirty `tools/RT64`
submodule.

## Probes

All throwaway, in `/tmp`, none committed:

- `/tmp/ogre-trace2.cjs` — boots with `window.OGRE_STATUS_FILTER = /$^/` and a
  2,000,000-line `ogreLog`, so `[sch]`, `[mq]`, `[ev]` and `[vi-debug]` survive.
  This is what showed the exit line.
- `/tmp/ogre-newgame.cjs` — taps `Enter` then `x` every 1.5 s (the native
  `OGRE_TAP_BUTTON="start,a,..."` schedule) and writes canvas frames plus the
  page log to a directory.
- The earlier `PTHREAD_POOL_SIZE=16 -> 32` A/B was rebuilt and reverted; the
  stall was identical, so the pool is not the cause.

## Open

1. **The browser renderer route.** See `docs/DECISIONS.md`.
2. **The browser build is not in CI**, which is why it stayed broken for eleven
   days. A `build-wasm` job on the release runners would catch the next
   native-only include.
3. **Browser frame rate.** The title runs at about 12.5 frames/s in headless
   Chromium against 30 natively. The renderer's `ogre_gfx_flush` skipped 2
   flushes in 40 tasks (`renderer busy (gfx pthread inside send_dl)`), and the
   rest is wasm speed. Not measured further.
4. **Scene-index observability in the browser.** The scene trace
   (`OGRE_SCENE_TRACE`) is native-only (`sdl_platform.cpp`), so the browser runs
   here were read from the periodic `titledisp` snapshot instead.
