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
    sdl_platform.{hpp,cpp} SDL2 window/input/audio/error callbacks
    renderer.{hpp,cpp}    ultramodern::renderer::RendererContext implementation
    rsp.{hpp,cpp}         recomp::rsp::callbacks_t (microcode provider)
    overlays.{hpp,cpp}    section table registration + streamed-code stubs
```

## Runtime flow

1. `main.cpp` creates the SDL window and registers the runtime config path
   (`SDL_GetPrefPath`, subfolder `ogrebattle64`).
2. `recomp::register_game()` registers the game entry:
   - `entrypoint_address = 0x80070C00`, `entrypoint = recomp_entrypoint`
   - `rom_hash = 0xbe6adaa5c3f8f7a9` (XXH3-64 of the big-endian ROM)
   - `on_init_callback` registers streamed-code stubs after `init_overlays()`.
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
| `renderer_callbacks.create_render_context` | `renderer.cpp` | RT64-based renderer (Vulkan on Linux) wrapping `RT64::Application`; parses the game's display lists via RT64's GBI interpreters |
| `rsp_callbacks.get_rsp_microcode` | `rsp.cpp` | currently a stub ucode that completes tasks without executing microcode |
| `audio_callbacks` | `sdl_platform.cpp` | SDL audio queue (`queue_samples` / `get_frames_remaining` / `set_frequency`) |
| `input_callbacks` | `sdl_platform.cpp` | keyboard (N64 mapping) + SDL GameController |
| `gfx_callbacks.update_gfx` | `main.cpp` | pumps SDL events on the main thread |
| `error_handling_callbacks` | `sdl_platform.cpp` | `SDL_ShowSimpleMessageBox` |
| `threads_callbacks` | `sdl_platform.cpp` | N64 thread names for debugging |

## Notes / open items

- **libultra is recompiled verbatim.** OB64's ELF has generic `func_800xxxxx`
  symbol names, so N64Recomp's libultra reimplementation (which matches symbols
  like `osCreateThread` → `osCreateThread_recomp`) did not kick in. The first
  boot will show how much of the runtime's native services (VI/AI/PI/threads)
  the recompiled libultra can actually reach; this is the core bring-up work.
- **MMIO is currently memory.** Writes to `0xA4800000`-style addresses land in
  the 4GB rdram mapping instead of hardware. Hardware-touching libultra
  functions that the runtime reimplements (`osViSetMode`, `osContInit`, PI DMA,
  ...) may need to be redirected to the runtime's `osXxx_recomp` functions.
