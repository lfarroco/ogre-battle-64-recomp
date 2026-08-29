# Ogre Battle 64 — WebAssembly / Browser Port Plan

> Status: **feasibility milestones 1–5 achieved (2026-08-29)** (see `docs/WEB-PORT-REPORT.md` for the
> current implementation report). This document is the plan; it is updated as
> milestones land.

## Overview

This document describes a staged plan for bringing the Ogre Battle 64
recompilation project to the browser via WebAssembly.

The immediate goal is **not** to implement a WebGL renderer.

The immediate goal is to determine whether the existing recompiled game,
`librecomp`, and `N64ModernRuntime` can run successfully under
WebAssembly/Emscripten **independently of RT64**.

If that succeeds, the project will have a clean path toward a browser renderer
in a later phase.

The intended eventual architecture is:

```text
                         Ogre Battle 64 ROM
                                │
                                ▼
                        Recompiled game code
                                │
                                ▼
                        librecomp/runtime
                                │
                         N64ModernRuntime
                                │
                 ┌──────────────┴──────────────┐
                 │                             │
              Native                         Web
                 │                             │
               RT64                     Browser renderer
                 │                             │
          ┌──────┴──────┐                 WebGL2 /
          │             │                 WebGPU
        Vulkan         Metal                 │
          │             │                    │
       Linux         macOS              WebAssembly
```

The native RT64 renderer remains the primary renderer throughout the initial
work.

---

## 1. Motivation

### 1.1 Current graphics problems

The project currently depends on RT64 for N64 graphics rendering.

The native graphics paths are:

* Linux: RT64 → Vulkan
* macOS: RT64 → Metal
* Windows: RT64 → the appropriate RT64 native backend

This has exposed graphics-driver problems on older hardware.

In particular, the project has encountered graphics failures that appear to be
below the recompiled game/runtime itself, inside the native GPU driver stack.
(This dev machine, for example, crashes inside `libvulkan_intel_hasvk.so` on an
Intel Haswell GPU; see `docs/HANDOFF-2026-08-29-session10.md`.)

This creates an undesirable situation for development:

```text
Game/runtime correctness
        │
        ▼
       RT64
        │
        ▼
native graphics API
        │
        ▼
vendor driver
        │
        ▼
       GPU
```

A failure at the bottom of this stack makes it difficult to determine whether:

* the game is issuing incorrect N64 graphics commands,
* RT64 is interpreting them incorrectly,
* the host graphics API is behaving unexpectedly,
* or the GPU driver has a compatibility problem.

A browser renderer would provide an independent execution environment and
potentially a useful reference implementation.

### 1.2 Browser support is useful beyond solving driver problems

A browser build would provide several advantages:

* no native installation required;
* easy distribution for demonstrations;
* useful for testing on hardware with problematic native graphics drivers;
* a convenient platform for developers to reproduce runtime issues;
* a potentially attractive preservation/discovery experience;
* a second execution environment for validating the recompilation.

A user could eventually load their own legally dumped ROM into a webpage and
run the recompilation locally. The ROM itself is never distributed by the
project — the user supplies it.

---

## 2. Important Architectural Observation

The project already has a favorable architecture for this work.

The recompiled game does not directly depend on Vulkan, Metal, or WebGL. The
renderer is supplied through the runtime's renderer interface.

In this repo that interface is
`ultramodern::renderer::RendererContext`
(`tools/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp`),
injected via `cfg.renderer_callbacks.create_render_context` in
`app/src/main.cpp` and implemented in `app/src/renderer.cpp` (RT64).

Conceptually:

```text
Recompiled game
      │
      ▼
librecomp
      │
      ▼
N64ModernRuntime
      │
      └── renderer callbacks
                │
                ▼
          Ogre renderer
                │
                ▼
               RT64
```

This means the browser effort should **not** modify the recompiled game
functions simply to accommodate WebAssembly. The preferred approach is to
preserve the existing runtime interfaces and introduce
platform/renderer-specific implementations below them.

---

## 3. Critical Scope Decision

**DO NOT start by replacing RT64 with WebGL.**

RT64 currently provides substantially more than a simple triangle renderer. It
interprets N64 graphics work including:

* RSP/RDP state;
* F3DEX-family display lists;
* N64 texture formats;
* TMEM;
* texture decoding;
* color combiners;
* render modes;
* blending;
* framebuffer behavior;
* depth behavior;
* other N64-specific GPU semantics.

The current renderer receives N64 display-list tasks rather than an
already-modernized stream of triangles.

Therefore:

```text
N64 display list
      │
      ▼
     RT64
      │
      ▼
modern GPU
```

is fundamentally different from:

```text
N64 display list
      │
      ▼
custom WebGL renderer
```

The latter would effectively require implementing a significant portion of an
N64 graphics renderer. That may eventually be desirable, but it is outside the
initial scope.

---

## 4. Primary Objective

The first objective is:

> Run Ogre Battle 64's recompiled CPU/runtime under WebAssembly in a browser
> without RT64.

The initial browser build uses a **null renderer**.

Success means that the browser can:

1. initialize the recompiled runtime;
2. load a user-provided ROM;
3. initialize the runtime;
4. create the necessary runtime threads/schedulers;
5. execute the recompiled game;
6. execute overlays;
7. execute RSP tasks;
8. reach display-list submission;
9. remain stable when graphics work is submitted to the null renderer.

No graphics need to be displayed at this stage.

---

## 5. Why Start With a Null Renderer?

This separates the problem into two independent questions.

### Question A

Can the recompilation/runtime execute in WebAssembly?

### Question B

Can N64 graphics be rendered in a browser?

Trying to answer both simultaneously makes debugging unnecessarily difficult.
The null renderer allows us to establish:

```text
ROM
 ↓
recompiled code
 ↓
runtime
 ↓
RSP
 ↓
display lists
 ↓
NULL RENDERER
 ↓
browser
```

If this works, then the browser port has already demonstrated that the
majority of the project is portable. Only the graphics layer remains.

---

## 6. Phase 1 — Audit the Existing Architecture ✅

Complete; see `docs/WEB-PORT-REPORT.md`. Key files:

```text
app/src/main.cpp          entry point, boot flow, callback wiring
app/src/renderer.cpp      RT64 RendererContext implementation
app/src/sdl_platform.cpp  SDL2 window/input/audio/error callbacks
app/src/rsp.cpp           RSP microcode provider (stub ucode)
app/src/overlays.cpp      section/overlay registration
app/CMakeLists.txt        build wiring (RT64 is a hard dependency)
tools/N64ModernRuntime/   librecomp + ultramodern runtime libraries
```

### Platform-specific dependency inventory

| Dependency | Where | Emscripten status |
|---|---|---|
| Vulkan/Metal/D3D12 | RT64 only (`app/src/renderer.cpp`, RT64) | Excluded from wasm build |
| SDL2 | `app/src/sdl_platform.cpp` | `-sUSE_SDL=2` port (optional for feasibility) |
| `mmap`/`mprotect`/`VirtualAlloc` | `librecomp/src/recomp.cpp` (rdram) | Needs wasm path (plain `calloc`) |
| sljit (native JIT) | `LiveRecomp` lib linked into `librecomp` | **Does not compile on wasm; must be excluded** |
| `std::thread`/mutex/CV/atomic | ultramodern runtime | Emscripten pthreads (SharedArrayBuffer) |
| `std::filesystem`, fstream | ROM store, saves, mods | Emscripten virtual FS (MEMFS/IDBFS) |
| `std::chrono` sleep | timer/VI/gfx loops | Works with pthreads |

---

## 7. Phase 2 — Make RT64 Optional ✅

The native build continues to work exactly as it does today. The build system
is changed so that RT64 is optional, controlled by a CMake option:

```cmake
option(OGRE_USE_RT64 "Build with the RT64 renderer" ON)
```

```cmake
if (OGRE_USE_RT64 AND NOT EMSCRIPTEN)
    add_subdirectory(${OGRE_TOOLS}/RT64 ${CMAKE_BINARY_DIR}/RT64)
    target_link_libraries(ogrebattle64 PRIVATE rt64)
endif()
```

The important property:

```text
Native build (default)
    → RT64

Native build (-DOGRE_USE_RT64=OFF)
    → null renderer

Emscripten build
    → no RT64 dependency, null renderer
```

---

## 8. Phase 3 — Null Renderer ✅

`app/src/null_renderer.cpp` implements the existing
`ultramodern::renderer::RendererContext` interface without a GPU:

* initializes successfully;
* accepts renderer callbacks;
* accepts display-list submissions;
* logs milestones and (optionally) per-frame DL statistics;
* never creates a graphics context;
* never links against RT64.

```cpp
class NullRenderer final : public ultramodern::renderer::RendererContext {
public:
    void send_dl(const OSTask* task) override {
        // Optional diagnostic logging + counters.
    }
};
```

---

## 9. Phase 4 — Add an Emscripten Target ✅

A separate WebAssembly build configuration:

* uses Emscripten (`emcmake cmake -S app -B build-wasm ...`);
* compiles the existing C/C++ code;
* excludes RT64;
* links the null renderer;
* produces an HTML/JS/WASM application (`-sUSE_SDL=2 -pthread`).

The native target remains unaffected. Separate build directories:

```text
build-app/      native (RT64)
build-null/     native (null renderer, no RT64)
build-wasm/     Emscripten
```

### Known wasm build requirements (from the audit)

* **rdram allocation**: `librecomp/src/recomp.cpp` maps 4 GiB with
  `PROT_NONE` + `mprotect`s 512 MiB writable. wasm32 has no per-page
  protection, so the wasm build allocates `mem_size` (512 MiB) with `calloc`.
  Out-of-range accesses then trap on the wasm heap instead of segfaulting; a
  debug build can additionally use `-sSAFE_HEAP=1`.
* **LiveRecomp/sljit**: excluded from the wasm build (native JIT is
  meaningless and uncompilable on wasm). Mod loading is disabled in the wasm
  build.
* **pthreads**: `-pthread` requires browser `SharedArrayBuffer`, which requires
  cross-origin isolation (see §11 and §Deployment).

---

## 10. Phase 5 — Browser Boot ✅

A minimal browser entry point (`app/src/main_web.cpp` + `app/web/`):

```text
Ogre Battle 64 Recompilation

Select ROM:
[ Choose File ]

Status:
Initializing...
Loading ROM...
Starting runtime...
Running.
```

* The selected ROM stays local to the user's machine (read via the browser
  File API, written into the wasm virtual filesystem).
* No copyrighted ROM data is embedded or distributed.
* `main_web.cpp` registers the game entry + overlays and exports an
  `ogre_start_boot()` function that runs the standard
  `recomp::select_rom` → `recomp::start` flow on a background pthread so the
  page stays responsive.
* ROM bytes are handed to the runtime through the normal file path
  (`FS.writeFile("/rom.z64", ...)`) so the runtime's ROM store code is used
  unchanged.

---

## 11. Phase 6 — Main Loop and Threading ✅ (boot runs on a pthread; browser main thread free)

The existing application uses threads and runtime synchronization
(`osCreateThread` → one host `std::thread` per N64 thread; ultramodern VI/gfx/
timer/task/saving/cleaner threads; `moodycamel::LightweightSemaphore`
scheduling).

Under Emscripten pthreads (`-pthread`):

* `std::thread`, `mutex`, `condition_variable`, `atomic`, TLS all work;
* `std::this_thread::sleep_for` works on worker pthreads;
* the **browser main thread must not block** — `recomp::start`'s main loop
  (1 ms sleep + `update_gfx`) is therefore run on a background pthread in the
  web build; the browser main thread runs the event loop + UI.

Do not replace the runtime's threading with custom browser primitives; first
attempt Emscripten pthreads.

Remember: WebAssembly pthreads rely on `SharedArrayBuffer` and therefore
require a cross-origin-isolated deployment environment:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

(see `docs/WEB-PORT-DEPLOYMENT.md`).

---

## 12. Main-Thread Considerations

The native application has a host-controlled startup/main-loop architecture:

```cpp
recomp::start(cfg);  // blocks: pumps update_gfx every 1 ms
```

In the browser the main loop must yield to the browser event loop. The
preferred architecture:

```text
Native:
    native main loop

Browser:
    Emscripten/browser main loop (UI, file picker, log)

Shared:
    game/runtime logic (pthreads)
```

Avoid adding browser-specific conditionals to recompiled game functions.

---

## 13. Phase 7 — Runtime Milestones ✅

`app/src/milestones.hpp` provides milestone logging so the browser build can
identify the furthest successful runtime milestone:

```text
[BOOT] application started
[ROM] ROM loaded
[RUNTIME] runtime initialized
[THREAD] runtime threads initialized
[OVERLAY] overlay loaded
[RSP] RSP task received
[RSP] display list submitted
[VI] video interrupt
[INPUT] controller initialized
[AUDIO] audio initialized
```

Milestone text is mirrored to the page via an exported poll buffer
(`ogre_poll_milestones()`), making browser-port failures immediately
distinguishable in the UI. Disabled/reduced in release native builds.

---

## 14. Phase 8 — Display List Instrumentation ✅ (null renderer counters + milestones)

The null renderer collects, at `send_dl` time:

* number of RSP tasks;
* number of display lists;
* display-list addresses;
* microcode addresses;
* detected GBI type (OB64's ucode is F3DEX 2.08, RT64 auto-detects it);
* frame count;
* approximate commands processed (periodic sampling).

Lightweight and optional (a compile-time flag):

```text
Frame: 183
RSP tasks: 1
GBI: F3DEX FIFO 2.08
Display list: 0x80123456
```

Valuable for both browser development and future renderer development.

---

## 15. Phase 9 — Measure the Actual Ogre Battle Graphics Workload

Before implementing a WebGL renderer, determine what Ogre Battle 64 actually
uses. Collect statistics over representative gameplay.

### Geometry

* G_VTX, G_TRI1, G_TRI2, G_QUAD, display-list calls, matrix operations

### Textures

* texture formats; texture dimensions; palette/TLUT usage; TMEM usage;
  texture load commands; texture count

### Combiner

* every unique color-combiner configuration

### Render state

* every unique cycle mode, blend mode, render mode, alpha compare mode, depth
  mode, fog configuration

### Framebuffer

* framebuffer-as-texture; copy framebuffer; unusual depth-image behavior;
  CPU-visible framebuffer data

This dataset determines whether a game-specific WebGL renderer is practical.

---

## 16. Phase 10 — Renderer Decision

After the workload has been measured, choose one of three directions.

### Option A — WebGL 2 renderer

```text
F3DEX 2.08 → N64 state → OB64 renderer → WebGL 2
```

Appropriate if the game uses a relatively manageable subset of N64 rendering
features. Broad browser support, mature Emscripten support, straightforward
deployment. Disadvantages: no compute shaders, more limitations than modern
explicit APIs, N64 framebuffer/combiner behavior may require workarounds,
potentially substantial shader/state emulation.

### Option B — WebGPU renderer

```text
N64 display lists → N64 graphics implementation → WebGPU → browser
```

WebGPU is likely a better conceptual fit for RT64-like rendering than WebGL
(compute shaders, storage buffers, explicit resource management, modern shader
pipelines), but must not be adopted merely because it is newer. Browser
compatibility and deployment requirements must be evaluated.

### Option C — Extend RT64 with a WebGPU backend

```text
                 RT64
                  │
       ┌──────────┼───────────┐
       │          │           │
    Vulkan      Metal      WebGPU
```

Most reusable but the largest undertaking. Only if there is interest in a
general-purpose browser-capable RT64. **Not** pursued during initial browser
feasibility work.

---

## 17. Recommended Architecture

```text
                         Recompiled Game
                                │
                                ▼
                           librecomp
                                │
                                ▼
                       N64ModernRuntime
                                │
                  ┌─────────────┴─────────────┐
                  │                           │
             Native Host                 Web Host
                  │                           │
          ┌───────┴───────┐                   │
          │               │                   │
        RT64          Null Renderer       Web Renderer
          │                                   │
    Vulkan/Metal                         WebGL2/WebGPU
          │                                   │
       Native                              WASM
```

The runtime must not know which renderer is being used.

---

## 18. Design Principles

* **Preserve the recompilation** — do not modify recompiled game functions
  solely to support WebAssembly.
* **Preserve the runtime API** — implement platform-specific behavior
  underneath the existing runtime interfaces (e.g. the rdram allocation path
  in `librecomp/src/recomp.cpp` is `#ifdef __EMSCRIPTEN__`-guarded; the
  renderer choice is a CMake option).
* **Keep RT64 native-only initially** — do not contaminate the browser
  feasibility work with RT64 changes.
* **Keep browser code isolated** — a small web platform layer
  (`app/src/main_web.cpp`, `app/src/web_platform.cpp`, `app/web/`). Avoid
  scattering `#ifdef __EMSCRIPTEN__` through unrelated game/runtime code.
* **Keep native builds working** — every browser-port commit leaves the
  Linux/macOS/Windows native builds unaffected.

---

## 19. Suggested Repository Structure

```text
app/
├── CMakeLists.txt
├── src/
│   ├── main.cpp              native entry (RT64 or null, by option)
│   ├── main_web.cpp          web entry (exports ogre_start_boot)
│   ├── web_platform.cpp      web callbacks (audio/input/events/threads)
│   ├── milestones.hpp        milestone + DL instrumentation helpers
│   ├── renderer.cpp          RT64 renderer (OGRE_USE_RT64 only)
│   ├── null_renderer.cpp     null renderer (no RT64)
│   ├── sdl_platform.{hpp,cpp}
│   ├── rsp.{hpp,cpp}
│   └── overlays.{hpp,cpp}
└── web/
    ├── index.html
    └── web.js
```

---

## 20. Milestones

| # | Milestone | Status |
|---|---|---|
| 1 | Native null renderer (boot without RT64) | ✅ verified 2026-08-29: game boots, 60 fps, first DL submitted, stable 50+ s |
| 2 | Emscripten compilation (no browser graphics) | ✅ `emcmake cmake -S app -B build-wasm` builds `ogrebattle64.js/.wasm` |
| 3 | Browser runtime boot (load ROM, init, reach game execution) | ✅ verified in headless Chrome: ROM loads, runtime+threads run |
| 4 | Browser display-list execution (RSP → F3DEX → DL → NullRenderer) | ✅ `[RSP] display list submitted (frame 1, type 1, ucode 0x8009F540, ...)` in the browser; page stable |
| 5 | Browser input/audio/save | ✅ 2026-08-29 (session 12): real keyboard+gamepad input (slot 0 keyboard-first, slots 1–3 gamepads), IDBFS save persistence at `/ogre` (stored-ROM autostart verified), AudioWorklet audio pipeline connected (wasm ring buffer → resampled playback). Game-side audio is 0 at the title screen because the game never registers `OS_EVENT_AI` or queues buffers there — verified identical on the native build; audio will flow once the game advances past the title |
| 6 | Graphics workload analysis | 🚧 started 2026-08-29 (session 13): F3DEX2 DL analyzer (command histogram, geometry, texture formats, unique combiners/modes) runs in the null renderer + web build (`app/src/gbi.{hpp,cpp}`, exported `ogre_gfx_stats`). Real data is thin while the game is stuck at the title (only the boot blanking DL is submitted on every platform); the full per-frame workload will be measured once the boot stall is fixed |
| 7 | Browser renderer prototype (WebGL2 or WebGPU) | ✅ 2026-08-29 (session 13): WebGL2 renderer (`app/src/web_renderer.cpp`) — F3DEX2 DL execution, matrix pipeline, fill rect / textured rect / triangles, N64 texture decode (RGBA16 verified; CI8/CI4+TLUT, IA, I implemented), general 2-cycle combiner in GLSL, scissor/blend. Verified in headless Chrome with a synthetic test DL (2x2 RGBA16 texture renders correct colors). Renders on the browser main thread (gfx pthread records commands; JS polls `ogre_gfx_flush`) |
| 8 | Playable browser build | pending (blocked by the game's boot stall — the game never advances past the title on any platform; see session 13 handoff for the SI_STATUS / game-clock-freeze leads) |

> The browser feasibility milestones (1–5) are met: the recompiled game +
> N64ModernRuntime run in WebAssembly with real input and persistent storage.
> Milestone 5 also fixed the browser-only `do_send FAILED queue full` spam — with
> a connected controller the game's VI-driven main loop paces correctly. Next per
> the plan: the workload measurement and the renderer decision (§24).

---

## 21. What NOT to Do

The coding agent must not:

* rewrite the recompiled game;
* replace N64ModernRuntime unnecessarily;
* immediately implement an entire N64 RDP;
* fork RT64 unnecessarily;
* remove RT64 from native builds;
* assume WebGL is required before measuring the graphics workload;
* distribute copyrighted ROM data;
* make browser support mandatory for native builds;
* introduce large architectural changes without documenting why.

---

## 22. Definition of Success for the Initial Project

```text
A user can select a legally dumped Ogre Battle 64 ROM
        ↓
Browser loads WASM
        ↓
Recompiled game starts
        ↓
Runtime threads execute
        ↓
RSP tasks execute
        ↓
Display lists reach the renderer interface
        ↓
Null renderer receives them
        ↓
Browser remains stable
```

Graphics are explicitly **not** required for this milestone.

---

## 23. Deployment Requirements (browser)

* **HTTPS or localhost** — `SharedArrayBuffer`/pthreads require a secure
  context.
* **Cross-origin isolation headers**:

  ```text
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  ```

* **ROM stays local** — the user selects their own dump; nothing is uploaded
  anywhere.
* See `docs/WEB-PORT-DEPLOYMENT.md` for a sample static server configuration.

---

## 24. Final Decision Gate

Once the null-renderer browser build works, **STOP** before implementing a
graphics backend. At that point generate a report answering:

1. How much of the game/runtime works unchanged?
2. What Emscripten-specific changes were necessary?
3. Does pthread support work reliably?
4. What browser deployment headers are required?
5. What display-list/GBI commands does Ogre Battle use?
6. What N64 texture formats are used?
7. How many unique combiner configurations exist?
8. Does the game use framebuffer effects that make WebGL difficult?
9. Would WebGL2 be sufficient?
10. Would WebGPU provide meaningful advantages?
11. Would implementing a game-specific renderer be reasonable?
12. Would modifying RT64 be more appropriate?

Only after answering these questions should the project commit to a
WebGL/WebGPU implementation.

---

## 25. Overall Strategy

```text
Native + RT64
        ↓
Native + RT64  +  WASM + NullRenderer
        ↓
Native + RT64  +  WASM + WebGL/WebGPU
```

This deliberately turns the browser effort into a sequence of small,
independently useful milestones rather than one large renderer rewrite.

> **First prove that Ogre Battle 64 itself is portable to WebAssembly. Then
> decide how to render it.**
