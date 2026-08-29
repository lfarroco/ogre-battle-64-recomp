# WebAssembly Port — Implementation Report

> Companion to `docs/WEB-PORT.md`. This is the audit/implementation report
> produced before making invasive changes (plan §25 "Second"). Updated as work
> progresses.

Date: 2026-08-29
State: **Phase 2–5 in progress** (RT64 optional, null renderer, web entry)

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
