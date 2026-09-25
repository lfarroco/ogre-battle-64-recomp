# App architecture

The PC port app lives in `app/` and is a CMake project that produces the
`ogrebattle64` executable.

## Layout

```
app/
  CMakeLists.txt          build setup (runtime, RT64, recompiled funcs, SDL2)
  src/
    main.cpp              entry point: window, game registration, boot
    game.hpp              game identity constants (ROM hash, entrypoint, ids)
    sdl_platform.{hpp,cpp} SDL2 window/input/audio/error callbacks, live console
    renderer.{hpp,cpp}    ultramodern::renderer::RendererContext implementation
    rsp.{hpp,cpp}         recomp::rsp::callbacks_t (microcode provider)
    overlays.{hpp,cpp}    base-section table registration
    bank_overlays.{hpp,cpp}
                          streamed bank units: registration, DMA-driven swap,
                          the function map behind `LOOKUP_FUNC`
    launcher.{hpp,cpp}    the start screen
    ui.{hpp,cpp}          the tabbed panel and bitmap font shared by the launcher
                          and the in-game overlay
    overlay.{hpp,cpp}     the in-game `ESC` window
    input_map.{hpp,cpp}   the rebindable controller map
    settings.{hpp,cpp}    player settings (GAME SPEED)
    crash_log.{hpp,cpp}   `error.log` and the fatal-signal handlers
    gbi.cpp, font.cpp, save_import.cpp, synth_frame.cpp
    null_renderer.cpp, main_web.cpp, web_platform.cpp, web_renderer.cpp
```

## Runtime flow

1. `main.cpp` creates the SDL window and resolves the config directory through
   `ogre::resolve_pref_dir()` (`launcher.cpp`: `SDL_GetBasePath`, so the ROM,
   `saves/`, `mods/`, `controls.cfg`, `settings.cfg` and `error.log` sit beside
   the executable; `SDL_GetPrefPath` is the fallback, and `OGRE_PREF_DIR`
   overrides both).
2. `recomp::register_game()` registers the game entry:
   - `entrypoint_address = 0x80070C00`, `entrypoint = recomp_entrypoint`
   - `rom_hash = 0xbe6adaa5c3f8f7a9` (XXH3-64 of the big-endian ROM)
   - `on_init_callback` registers the base sections and the streamed bank units.
3. `recomp::select_rom(path, game_id)` validates the user's ROM and stores a
   copy in the config dir.
4. `recomp::start(cfg)` blocks on the main thread:
   - spawns the **Game Start Thread** → `ultramodern::preinit` → waits for the
     game status;
   - its internal loop pumps the gfx `update_gfx` callback, which is how the
     app pumps SDL events on the main thread.
5. `recomp::start_game(game_id, "")` sets the status; the game thread runs
   `init()` (overlay load + first 1MB ROM DMA + IPL3 vars) then calls
   `recomp_entrypoint` → clears BSS → `main_recomp` → game boots.

## Callback responsibilities

| Callback | Source | Notes |
|---|---|---|
| `renderer_callbacks.create_render_context` | `renderer.cpp` | RT64 renderer (Metal/Vulkan/D3D12) wrapping `RT64::Application`; parses the game's display lists via RT64's GBI interpreters. `OGRE_GRAPHICS_API` pins the backend |
| `rsp_callbacks.get_rsp_microcode` | `rsp.cpp` | dispatches the recompiled njpeg (type 4) and audio (type 2) microcodes; `OGRE_NJPEG=0` / `OGRE_AUDIO_UCODE=0` force the stub, which completes a task without executing microcode |
| `audio_callbacks` | `sdl_platform.cpp` | SDL audio queue (`queue_samples` / `get_frames_remaining` / `set_frequency`) |
| `input_callbacks` | `sdl_platform.cpp` | keyboard (N64 mapping from `input_map.cpp`) + SDL GameController |
| `gfx_callbacks.update_gfx` | `main.cpp` | pumps SDL events on the main thread, and draws the overlay |
| `error_handling_callbacks` | `sdl_platform.cpp` | `SDL_ShowSimpleMessageBox` |
| `threads_callbacks` | `sdl_platform.cpp` | N64 thread names for debugging |

## Notes

- **libultra is bridged by name.** OB64's ELF has generic `func_800xxxxx` symbol
  names, so N64Recomp's libultra reimplementation (which matches symbols like
  `osCreateThread` → `osCreateThread_recomp`) does not fire on its own.
  `config/symbols/symbol_addrs.txt` names the libultra functions, and the runtime
  provides the native `osXxx_recomp` services. See `docs/LIBULTRA-BRIDGING.md`.
- **MMIO is redirected in the runtime.** Writes to `0xA4800000`-style addresses
  would otherwise land in the 4GB rdram mapping. The hardware-touching libultra
  functions the game uses (`osViSetMode`, `osContInit`, PI DMA, AI status, ...)
  are named and call the runtime's `osXxx_recomp` functions.
- **Swappable RAM.** The game DMAs streamed modules over each other at fixed RAM
  addresses. `bank_overlays.cpp` registers each unit's functions when the DMA
  arrives and drops whatever occupied that RAM; calls into a swappable range
  compile as `LOOKUP_FUNC` through that map. AGENTS.md rule 4 holds the failure
  mode when a unit also defines the range it calls into.
