# Handoff — 2026-08-29 (session 11): WebAssembly port kickoff — RT64 optional, null renderer, native macOS boot fixed

## TL;DR

- **WebAssembly port plan documented** (`docs/WEB-PORT.md`) and implementation
  started: RT64 is now a CMake option, a **null renderer** exists
  (`app/src/null_renderer.cpp`), and the app has a separate **Emscripten
  branch** (no SDL, no RT64, pthread-based) with a web shell
  (`app/web/index.html` + `web.js`, ROM via File API).
- **The native app now boots Ogre Battle 64 on this macOS machine with the
  null renderer**: the game runs at 60 fps, submits its first RSP gfx task and
  display list to the renderer interface, and stays stable (verified 50+ s,
  3000+ VI swaps, no RT64, no GPU). That is plan Milestone 1 + the native half
  of Milestone 4.
- The macOS build was blocked by several **pre-existing stale/regressed
  states** that this session fixed:
  1. `build/ogrebattle64.elf` + `RecompiledFuncs/` were stale (Aug 24) — the
     libultra bridge names (`osInitialize`, `osSpGetStatus`, ...) were never in
     the ELF, so the game's unbridged MMIO reads crashed.
  2. Missing runtime fixes that were recorded in `n64modernruntime-ob64.patch`
     but **not present in the working tree**: the initial-1 MiB DMA entrypoint
     sign-extension (data was written at rdram+4.3 GiB!), the `load_overlays`
     0x3E1B0 base-section-only fix, and the **external-message drainer thread**
     (boot deadlocks without it).
  3. `osSpGetStatus` was not in N64Recomp's `reimplemented_funcs` (only
     `__osSpGetStatus`), so the game's copy kept its raw SP_STATUS MMIO read
     (rdram offset ~576 MiB — out of bounds).
  4. A broken debug probe in `recomp.cpp` read `rdram + 0x24800018`
     (584 MiB — past the 512 MiB usable region) and crashed every macOS boot.
- Also fixed: macOS `create_window` (WindowHandle struct), Makefile
  `fix-labels` ordering (the cross-overlay label fix must run **before**
  assembling), and CMake `CONFIGURE_DEPENDS` on the RecompiledFuncs glob.

## What was built (web port)

- `app/CMakeLists.txt`: `OGRE_USE_RT64` option (default ON; forced OFF on
  EMSCRIPTEN); null renderer when RT64 off; web source set + pthreads flags.
- `app/src/null_renderer.cpp` — null renderer with DL/VI instrumentation.
- `app/src/milestones.hpp` — milestone logging + poll buffer (`ogre_poll_milestones`).
- `app/src/main_web.cpp` — web entry: no-op `main()`, exported
  `ogre_start_boot()` (runs the boot on a pthread), ROM from `/rom.z64`.
- `app/src/web_platform.{hpp,cpp}` — no-op input/audio/events/threads callbacks.
- `app/web/index.html`, `app/web/web.js` — ROM picker + status page.
- Runtime (vendored, captured in the patch files):
  - `librecomp/src/recomp.cpp` — `__EMSCRIPTEN__` rdram allocation (`calloc(512 MiB)`).
  - `N64Recomp/CMakeLists.txt` + `LiveRecomp/live_generator_wasm.cpp` — wasm
    stub for the sljit-based live recompiler (code mods simply fail to load).
  - `ultramodern/src/threads.cpp` — no-op thread-name/priority under `__EMSCRIPTEN__`.
  - `ultramodern/include/ultramodern/renderer_context.hpp` — explicit
    `__EMSCRIPTEN__` `WindowHandle` alias.
  - `N64Recomp/src/symbol_lists.cpp` (both checkouts) — `osSpGetStatus` added
    to `reimplemented_funcs` (fixes the unbridged SP_STATUS read).

## Status by plan milestone

| Milestone | Status |
|---|---|
| M1 native null renderer boots without RT64 | ✅ stable (50+ s, 60 fps, first DL) |
| M2 Emscripten compilation | in progress (emscripten SDK installing; build-wasm next) |
| M3 browser runtime boot | pending |
| M4 browser display-list execution | pending (native half proven) |

## Verification

```sh
# native, null renderer (no RT64)
cmake -S app -B build-null -DCMAKE_BUILD_TYPE=Release -DOGRE_USE_RT64=OFF
cmake --build build-null -j
./build-null/ogrebattle64 assets/ogre64.z64
#   -> [RENDERER] null renderer initialized
#   -> [RSP] display list submitted (frame 1, type 1, ucode 0x8009F540, ...)
#   -> 60 fps VI swaps, stable
```

## Regenerating the recompilation on macOS (was broken)

```sh
tools/venv/bin/splat split config.yaml   # regenerates asm/ + assets/ (incl. 66E30.bin)
make                                      # rebuilds ELF (fix-labels runs before asm now)
make recomp                               # regenerates RecompiledFuncs (with libultra bridges)
cmake --build build-null -j
```

## Next steps

1. ~~Emscripten build~~ **done** — `emcmake cmake -S app -B build-wasm` builds
   `build-wasm/ogrebattle64.js` (95 KB) + `.wasm` (2.6 MB); exports
   `ogre_start_boot` / `ogre_poll_milestones`.
2. ~~Browser boot~~ **done** — headless Chrome (playwright) test: ROM loads,
   runtime + threads run, `[RSP] display list submitted (frame 1, type 1,
   ucode 0x8009F540, data_ptr 0x800C6500)` appears in the page status; stable
   at 60 fps. **Plan milestones M1–M4 are met.**
3. **Milestone 5**: real browser input/audio/save (web input is currently a
   no-op; the game's VI queue fills while it waits for controller input —
   `do_send FAILED queue=0x800E8B84 (full, 8/8)` — likely wasm speed vs 60 Hz
   VI rate; native does not show it).
4. Then the workload measurement + renderer decision (`docs/WEB-PORT.md` §24).

## Known issues / notes

- **clang -O3 codegen**: the game's recompiled functions contain the classic
  N64Recomp `MEM_*(addr - 0xFFFFFFFF80000000)` address pattern. Clang can fold
  that arithmetic differently than gcc; the Linux builds used gcc. If wasm
  (clang) miscompiles a function, compile the recomp lib with a lower opt level
  or adjust the macros (not yet observed — the boot got past every MEM_* site
  with clang at -O3 on macOS).
- The game currently submits **one** display list at boot and then idles at
  60 fps (title screen waiting for input) — fine for the milestone.
- `tools/RT64` submodule is not initialized in this checkout (the null-renderer
  build doesn't need it).
