# WebAssembly Port — Implementation Report

> Companion to `docs/WEB-PORT.md`. This is the audit/implementation report
> produced before making invasive changes (plan §25 "Second"). Updated as work
> progresses.

Date: 2026-08-29
State: **Phase 2–10 complete (Milestones 1–5 achieved)**; Milestone 6 started
(workload analyzer) and **Milestone 7 prototype achieved (WebGL2 renderer)**

---

## 1. Existing Architecture

### 1.1 Boot flow (native, `app/src/main.cpp`)

```text
main()
  init_sdl()                        → SDL video/audio/controller/events
  create_window()                   → SDL_WINDOW_METAL/VULKAN window
  register_config_path(SDL_GetPrefPath)   → config dir for ROM store/saves
  register_game(entry)              → rom_hash, entrypoint 0x80070C00
  register_base_overlays()          → section table (recomp_overlays.inl)
  select_rom(path, game_id)         → validate hash, copy ROM to config dir
  recomp::start(cfg)                → BLOCKS: allocates rdram, spawns game
                                      thread, pumps update_gfx every 1 ms
  [separate std::thread] start_game(game_id) → sets status; game thread boots
```

### 1.2 Renderer injection

The renderer is a pure virtual interface
`ultramodern::renderer::RendererContext`
(`tools/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp`):

```cpp
class RendererContext {
    virtual bool valid() = 0;
    virtual bool update_config(...) = 0;
    virtual void enable_instant_present() = 0;
    virtual void send_dl(const OSTask* task) = 0;          // display list
    virtual void send_dummy_workload(uint32_t fb_address) = 0;
    virtual void update_screen() = 0;                      // VI retrace
    virtual void shutdown() = 0;
    virtual uint32_t get_display_framerate() const = 0;
    virtual float get_resolution_scale() const = 0;
};
```

It is created through the `renderer_callbacks_t.create_render_context`
callback. `app/src/renderer.cpp` implements it with `RT64::Application`
(Vulkan on Linux, Metal on macOS). The runtime's gfx thread
(`ultramodern/src/events.cpp::gfx_thread_func`) calls `send_dl` /
`update_screen` / `send_dummy_workload` from a queue of actions produced by the
VI thread and `submit_rsp_task`.

### 1.3 ROM data flow

1. `recomp::select_rom(path, game_id)` (`librecomp/src/recomp.cpp`): reads the
   file, pads to 4 bytes, detects/undoes 16-bit or 32-bit byteswap, XXH3-64
   hash-checks against `rom_hash`, then **writes a copy** to
   `config_path / game_id + ".z64"`.
2. `recomp::start` → game thread → `wait_for_game_started` →
   `load_stored_rom(game_id)` **re-reads the file from disk**, hash-checks, and
   calls `set_rom_contents`.
3. Game DMA is served from the in-memory copy by
   `recomp::do_rom_read`/`do_rom_pio` (`librecomp/src/pi.cpp`); the initial
   1 MiB boot DMA is `recomp::do_rom_read(rdram, entrypoint, 0x10001000, 0x100000)`.

### 1.4 Threading

- `recomp::start` spawns a **game start thread** which runs `preinit`
  (events/gfx/task/VI threads, timers, audio, thread cleaner) and then waits
  for the game status.
- Every N64 `osCreateThread` spawns a host `std::thread`
  (`ultramodern/src/threads.cpp::_thread_func`), synchronized with
  `moodycamel::LightweightSemaphore`.
- Host threads: gfx, SP task, VI, timer, thread cleaner, saving, game start.
- `recomp::start`'s main loop is a 1 ms `sleep_for` + `update_gfx` pump.

### 1.5 rdram

`recomp::start` allocates `allocation_size` (4 GiB) with `mmap PROT_NONE`
(POSIX) / `VirtualAlloc PAGE_NOACCESS` (Windows), then `mprotect`s
`mem_size` (512 MiB) read/write. All game memory access goes through
`MEM_B/H/W` macros and `TO_PTR` which index `rdram + (addr - 0xFFFFFFFF80000000)`.

---

## 2. RT64 Dependency Points

| Point | File | What breaks without RT64 |
|---|---|---|
| `add_subdirectory(tools/RT64)` | `app/CMakeLists.txt` | build |
| `target_link_libraries(... rt64)` | `app/CMakeLists.txt` | link |
| RT64 include dirs (5+ paths) | `app/CMakeLists.txt` | compile of `renderer.cpp` |
| `#include "hle/rt64_application.h"` etc. | `app/src/renderer.cpp` | compile |
| `RT64::Application` usage | `app/src/renderer.cpp` | — |

Everything else (runtime, recompiled game) is RT64-free. `renderer.cpp` is the
only TU that touches RT64, so making the renderer a build-time choice is
straightforward.

---

## 3. Emscripten Compatibility Issues

| # | Issue | Where | Plan |
|---|---|---|---|
| 1 | `mmap(4 GiB, PROT_NONE)` + `mprotect` | `librecomp/src/recomp.cpp` | wasm path: `calloc(mem_size, 1)` under `#ifdef __EMSCRIPTEN__`; no guard pages (wasm traps on OOB) |
| 2 | sljit native JIT (`LiveRecomp`) linked into `librecomp` | `librecomp/CMakeLists.txt`, `N64Recomp/LiveRecomp` | exclude from wasm build; stub the mods/live-recompiler entry points |
| 3 | SDL2 `find_package` | `app/CMakeLists.txt` | emscripten: use `-sUSE_SDL=2` port instead |
| 4 | `WindowHandle` type | `renderer_context.hpp` (`__linux__` branch) | emscripten defines `__linux__`, so `WindowHandle = SDL_Window*`; null renderer ignores it — verify |
| 5 | Browser main thread must not block | `recomp::start` main loop | run boot on a pthread; JS main thread owns UI/event loop |
| 6 | `std::filesystem` ROM store | ROM select/store, saves, mods | works on MEMFS virtual FS; ROM written to `/rom.z64` by JS, then normal file-based flow |
| 7 | `std::quick_exit` | `ultramodern/src/error_handling.cpp` | supported by emscripten libc++; verify |
| 8 | Audio/input callbacks | `app/src/sdl_platform.cpp` | initial web build uses no-op callbacks (M5 will add real ones) |

---

## 4. Threading Concerns

- Emscripten pthreads (`-pthread`) provide `std::thread`/mutex/CV/atomic/TLS on
  top of Web Workers + `SharedArrayBuffer`.
- The VI/gfx/timer threads are timing-driven (`sleep_until`); on wasm these
  become worker-thread sleeps — should behave like native.
- The scheduling model (LightweightSemaphore hand-off between N64 threads)
  uses futex-like waits — works under pthreads.
- **Browser main thread**: `recomp::start`'s 1 ms loop must not run on the JS
  main thread (page would freeze, input/audio dead). The web build spawns a
  pthread to run `recomp::start`.
- `PTHREAD_POOL_SIZE` must cover: game start + gfx + task + VI + timer +
  cleaner + saving + up to 7+ N64 game threads → 16 is safe.
- `-sMAXIMUM_MEMORY` must allow a ≥512 MiB heap (rdram) plus runtime overhead;
  plan for 1–2 GiB with `-sALLOW_MEMORY_GROWTH`.

---

## 5. Filesystem / ROM-Loading Concerns

- The runtime writes the ROM to the config dir and re-reads it at boot
  (`load_stored_rom`). On wasm, MEMFS is in-memory and non-persistent: the JS
  layer writes the user's ROM to `/rom.z64`, `select_rom` validates it, and
  the normal file flow runs unchanged. No runtime changes needed for ROM
  loading.
- Saves/mods dirs are created under the config path on MEMFS; persistence
  (IDBFS) is deferred to Milestone 5.
- `SDL_GetPrefPath` works under emscripten (virtual path).

---

## 6. Proposed Files

### Modify

| File | Change |
|---|---|
| `app/CMakeLists.txt` | `OGRE_USE_RT64` option; EMSCRIPTEN branch (`-sUSE_SDL=2`, pthreads flags, no RT64, web sources); select renderer TU |
| `tools/N64ModernRuntime/librecomp/src/recomp.cpp` | `#ifdef __EMSCRIPTEN__` rdram allocation |
| `tools/N64ModernRuntime/librecomp/CMakeLists.txt` | make `LiveRecomp` optional (excluded on wasm) |
| `tools/N64ModernRuntime/librecomp/src/mods.cpp` (+ `recomp.cpp` mods glue) | guard live-recompiler init on wasm |
| `tools/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp` | explicit `__EMSCRIPTEN__` `WindowHandle` alias (if needed) |
| `n64modernruntime-ob64.patch` | capture the runtime changes |
| `PLAN.md`, `docs/README.md` | pointer to the web-port plan |

### Create

| File | Purpose |
|---|---|
| `docs/WEB-PORT.md` | the plan |
| `docs/WEB-PORT-REPORT.md` | this report |
| `docs/WEB-PORT-DEPLOYMENT.md` | browser deployment (COOP/COEP headers, static server) |
| `app/src/null_renderer.cpp` | null renderer + DL instrumentation |
| `app/src/milestones.hpp` | milestone + instrumentation helpers |
| `app/src/main_web.cpp` | web entry (no SDL; exports `ogre_start_boot`) |
| `app/src/web_platform.cpp` | web no-op callbacks (audio/input/events/threads/error) |
| `app/web/index.html` | ROM picker + status page |
| `app/web/web.js` | Module glue, FS ROM write, milestone polling |

---

## 7. Verification Plan

1. **Native + RT64**: incremental build in `build-app/` — must still build/run.
2. **Native + null**: fresh `build-null/` with `-DOGRE_USE_RT64=OFF` — must
   build and boot the game to the null renderer without RT64.
3. **WASM**: `emcmake cmake -S app -B build-wasm` — must compile the runtime +
   recompiled funcs + web entry, link a `.js/.wasm` artifact.
4. **Browser**: serve `app/web/` + `build-wasm` output with COOP/COEP headers,
   select a ROM, observe milestones through `ogre_poll_milestones()` up to
   display-list submission.

## 8. Results (2026-08-29)

All four verification steps pass:

1. ✅ Native + RT64 builds and links (macOS; running hits the machine's
   known GPU-driver issue — that is the motivation for this port).
2. ✅ Native + null boots the game: `[RENDERER] null renderer initialized`,
   game runs at 60 fps, `[RSP] display list submitted (frame 1, type 1,
   ucode 0x8009F540, data_ptr 0x800C6500)`, stable 50+ s, no RT64, no GPU.
3. ✅ `emcmake cmake -S app -B build-wasm` produces `ogrebattle64.js`
   (95 KB) + `ogrebattle64.wasm` (2.6 MB); exports `ogre_start_boot` /
   `ogre_poll_milestones`.
4. ✅ Headless-Chrome test (playwright): the page loads, the ROM is written
   to the wasm FS, the boot pthread runs, and the status area shows
   `[RSP] display list submitted (frame 1, ...)`; the page keeps running at
   60 fps. **Milestones 1–4 of the plan are met.**

### Wasm-specific fixes required (beyond the plan's initial list)

| File | Fix |
|---|---|
| `librecomp/src/mods.cpp` | `IS_WASM` arch branch (no native code patching) |
| `librecomp/include/librecomp/mods.hpp` | `HookDefinition` hash 64-bit assert → 32-bit fallback |
| `librecomp/src/overlays.cpp` | `execinfo.h`/`backtrace` guarded out on wasm |
| `librecomp/src/recomp.cpp` | `std::free` for the wasm rdram path |
| `ultramodern/src/threads.cpp` | no-op thread name/priority on wasm (no `sys/prctl.h`) |
| `N64Recomp/CMakeLists.txt` + `LiveRecomp/live_generator_wasm.cpp` | sljit-based live recompiler → no-op stub on wasm |
| `ultramodern/include/ultramodern/renderer_context.hpp` | explicit `__EMSCRIPTEN__` `WindowHandle` |

### Browser behavior notes

- The game boots further in the browser than the ROM-loaded milestone shows:
  after the first DL it keeps running at 60 fps, but the game's VI message
  queue fills (`do_send FAILED queue=0x800E8B84 (full, 8/8)`) — the game is
  waiting for input it never receives (web input is a no-op). Native does not
  show the queue-full behavior; the difference is likely wasm execution-speed
  vs. the fixed 60 Hz VI rate. Investigate with Milestone 5 (real input/audio).

---

## 9. Milestone 5 Results (2026-08-29, session 12)

Real input/audio/save are implemented and verified in headless Chrome
(playwright, chromium headless shell):

| Area | Implementation | Verified |
|---|---|---|
| Input | `app/src/web_platform.cpp` atomics + exported `ogre_input_set()`; JS captures keyboard (slot 0) + gamepads (slots 1–3) in `app/web/web.js` | `ogre_input_debug()` shows pressed bits (A+UP=0x8800, START=0x1000); the game's controller thread polls SI messages at 60 Hz; **`do_send FAILED` queue-full spam is gone (0 occurrences)** |
| Audio | wasm ring buffer (`queue_samples`/`get_frames_remaining`/`set_frequency`) + AudioWorklet (`app/web/audio-worklet.js`) reading the SharedArrayBuffer directly, resampling game rate → context rate | AudioWorklet connects and initializes; the game sets its rate (32000 Hz) through the callback. The game queues **no samples at the title screen**: it never registers `OS_EVENT_AI` and its audio thread waits on a queue nothing sends to — **verified identical on the native null build** (same event registrations, no audio). Audio will flow when the game advances past the title |
| Save | IDBFS mounted at the config dir `/ogre` (wasm virtual FS) + periodic `FS.syncfs`; boot falls back to the previously validated stored ROM when no new file is picked | Page reload auto-starts the game from the persisted ROM (`/ogre/ogrebattle64-us-rev1.z64`) |

Build-system changes for M5:

- `app/CMakeLists.txt`: **fixed 1 GiB heap** (`-sINITIAL_MEMORY=1073741824`, no
  `ALLOW_MEMORY_GROWTH`) so the SharedArrayBuffer handed to the AudioWorklet
  never moves; `-lidbfs.js` for IDBFS; new exports
  (`ogre_input_set`, `ogre_input_debug`, `ogre_audio_state_ptr`,
  `ogre_audio_ring_ptr`, `ogre_audio_frames_available`).
- `app/web/index.html`: runtime trace filter keeps the chatty
  `[sch]/[mq]/[ev]/[renderer]/[VI]/[drainer]` debug prints out of the page
  status (overridable via `window.OGRE_STATUS_FILTER`); controls hint.

Verification run (headless Chrome, ROM = `assets/ogre64.z64`):

```text
PASS boot reaches display-list submission
PASS keyboard input reaches wasm (mask=0x8800)
PASS Start (Enter) bit reaches wasm (mask=0x1000)
PASS do_send FAILED does not keep growing after boot (0 -> 0)
PASS controller reports connected
PASS AudioWorklet connected
PASS page still responsive after ~35 s
PASS no page errors
PASS reload auto-starts from persisted stored ROM (IDBFS)
(info) game audio flow = 0 at title (native identical; see above)
```

Notes:

- The game's title screen does not advance on Start/A/B in this state — on the
  native null build too (single DL, same thread/message pattern). Determining
  why (likely needs visual feedback from a renderer, or game-flow analysis of
  the title loop) is the natural next debugging step.
- The native null build segfaults at process exit (after clean renderer
  shutdown) — pre-existing, not web-related.
- Rebuilding the wasm target requires a writable emscripten cache
  (`EM_CACHE=/tmp/emscripten-cache`; the default under `/usr/local/Cellar` is
  not writable in the sandboxed environment).

## 10. Milestones 6-7 Results (2026-08-29, session 13)

### Milestone 6 — graphics workload analysis (started)

`app/src/gbi.{hpp,cpp}` implement a dependency-free F3DEX2 display-list walker
(`walk_dl`, shared by the analyzer and the renderer) plus a workload analyzer
(`analyze_dl`, `format_summary`). It walks DLs in execution order (G_DL
push/branch, segment registers, loop protection) and counts commands, geometry
(G_VTX/TRI1/TRI2/QUAD/TEXRECT/FILLRECT), texture loads (SETTIMG formats,
LOADTILE/LOADBLOCK/LOADTLUT/SETTILE/SETTILESIZE), unique combiner configs,
OTHERMODE values and RDP state.

The analyzer runs in both the native null renderer and the web build, exported
as `ogre_gfx_stats()` (a snapshot string) and mirrored to the web page
(`#gfxstats` panel). Verified in headless Chrome and natively.

**Real data so far is thin**: the game (on every platform, native and web)
submits only its boot blanking DL — 32 commands: 5×SET*COLOR, SETPRIMDEPTH,
SETCONVERT, SETKEYR/GB, 8×SETTILE, 8×SETTILESIZE, 1×SETCOMBINE, ENDDL — and
then idles at the title (see §11 boot-stall analysis). The per-frame workload
measurement (plan §15) therefore waits on the boot fix; the analyzer is ready
to capture it.

### Milestone 7 — WebGL2 renderer prototype (achieved)

`app/src/web_renderer.cpp` implements `RendererContext` with WebGL2 for the
Emscripten build. Features:

- F3DEX2 DL execution via the shared `gbi::walk_dl` walker: matrices
  (G_MTX/G_POPMTX, row-vector FixedMatrix), vertices (G_VTX/G_MODIFYVTX),
  G_TRI1/TRI2/G_QUAD through the full matrix pipeline (modelview × projection,
  perspective divide, viewport, y-flip), G_TEXRECT/G_TEXRECTFLIP/G_FILLRECT,
  scissor, blend (standard XLU modes), and the general two-cycle N64 color
  combiner evaluated in the fragment shader (A−B)×C+D per channel, with the
  RT64 mux layout.
- N64 texture decode on G_LOADTILE/G_LOADBLOCK/G_LOADTLUT: RGBA16/32, IA16/8/4,
  I8/I4, CI8/CI4 + TLUT (RGBA16/IA16), with a GL texture cache. RGBA16 verified
  end-to-end; the other formats are implemented and unit-testable.
- Rendering happens on the **browser main thread**: the gfx pthread executes
  DLs into a command queue (`DrawCmd`/`TexUpload`), and web.js polls the
  exported `ogre_gfx_flush()` (16 ms interval) which issues the GL calls. This
  was required because Emscripten's pthread→main-thread GL proxying
  (`emscripten_webgl_make_context_current` from a worker) failed with
  INVALID_PARAM in this app (see §12).
- `ogre_gfx_test_draw()` builds a synthetic DL (fill rect + 2×2 RGBA16 textured
  rect) to validate the pipeline while the game only submits its boot blanking
  DL. Verified in headless Chrome: the canvas shows the grey fill and the four
  texture colors (red/green/blue/white) in the right places, no GL errors.

### 11. Boot-stall analysis (why the game never leaves the title — leads for the next session)

The game never submits per-frame display lists on any platform (native null,
web; 1 DL in a 100 s native run). Investigation found:

1. **The game's frame producer waits on the VI framebuffer.** `func_8008949C`
   (the task-manager path, called by `func_800893C0` on thread 17) spins until
   `osViGetCurrentFramebuffer()` or `osViGetNextFramebuffer()` equals the
   frame's target framebuffer (task-wrapper `+0xC`). The runtime traces showed
   `osViSwapBuffer` is **never called** by the game in this build state, so the
   VI stays on the dummy framebuffer (`0x80700000`) and the match never
   succeeds (the check bails; the task still gets submitted once via the boot
   path).
2. **The task manager is gated by a phase byte.** `func_800893C0` skips the
   framebuffer wait + task submission when `D_800C4800 & 2` is set. The byte is
   driven by the VI-retrace handler `func_80088F08`'s countdown, which calls
   `func_80098030` → `func_8009A770(0)` to advance the phase.
3. **`func_8009A770` polls SI_STATUS MMIO `0xA4040010` bit 0** and returns
   phase 0 (rendering) only when that bit is set; otherwise it returns −1 and
   the countdown repeats. The runtime does not emulate SI_STATUS, so the game
   can never reach the rendering phase. (The raw read would be an out-of-range
   rdram access; it appears the read returns 0 in practice without crashing —
   the phase machine just never advances.)
4. **The game's threads freeze ~179 ms into a 100 s native run** (`trace_millis`
   is wall time and stops advancing while the renderer keeps swapping) — a
   scheduler/threading stall that races (run-to-run variance in how far the
   boot gets). This is a separate runtime issue from the SI_STATUS gating.

**Next-session candidates**: emulate SI_STATUS bit 0 (set it during controller
reads / on a timer) so `func_8009A770` returns 0 and the game enters its
rendering phase; investigate the game-thread freeze at ~180 ms (scheduler
starvation, session 10's debug-print slowdown, or a deadlock in the drainer).

### 12. Emscripten/WebGL gotchas (session 13)

- **macOS `/tmp` is a symlink to `/private/tmp`**, which breaks emcc's
  relative-path computation when compiling its GL system library
  (`system/lib/gl/gl.c`) — the path is off by one and clang cannot find the
  file. Fix: use a cache directory not under `/tmp`, e.g.
  `EM_CACHE=/Users/momo/.cache/emscripten-ogre`. (The old `EM_CACHE=/tmp/...`
  note in earlier sessions must be updated.)
- **GL calls from pthreads failed** (`emscripten_webgl_make_context_current`
  returned INVALID_PARAM for a main-thread-created context, even when proxied).
  Solution: render on the main thread (gfx pthread records commands; JS polls
  `ogre_gfx_flush()`). No `-sOFFSCREENCANVAS_SUPPORT` needed.
- **GLSL `#version` must be the first line** of the shader source; raw strings
  that begin with a newline compile-fail at line 2.
- **rdram words are stored byte-reversed** (little-endian on the host; the
  runtime's `MEM_W` is a direct read). All DL/texture reads use a LE read.
- **F3DEX2 tile indices live in w1 bits 24-26** (not w0) for SETTILE /
  SETTILESIZE / LOADTILE / LOADBLOCK / LOADTLUT; SETTILE's clamp/mirror fields
  are 2-bit (`cmt` = w1 bits 18-19, `cms` = w1 bits 8-9) and the mask/shift
  fields sit at w1 bits 0-17. See RT64 `GBI_RDP::setTile`.
- **Address resolution must not truncate physical addresses to 24 bits** — the
  port's rdram is 512 MiB, so addresses above 16 MiB (e.g. a scratch test DL at
  0x1FE00000) were silently wrapped. Fixed to `& 0x1FFFFFFF`.

### Build note

The web (wasm) build now needs the WebGL flags:
`-sUSE_WEBGL2=1 -sMAX_WEBGL_VERSION=2` and the exports
`_ogre_gfx_create_context`, `_ogre_gfx_set_canvas`, `_ogre_gfx_flush`,
`_ogre_gfx_test_draw`. Rebuild with a writable emscripten cache outside
`/tmp` (see §12).
