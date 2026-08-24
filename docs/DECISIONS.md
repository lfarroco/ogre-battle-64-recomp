# Technical Decisions

This is the running log of technical decisions for the Ogre Battle 64 PC port.
Each entry records what was decided, why, and when. New entries go on top.

---

## 2026-08-24 (session 2) — Libultra bridging: name-based replacement + platform decision

### Decision: adopt handoff Option 1 — name OB64's libultra functions in the ELF

The next milestone (see `LIBULTRA-BRIDGING.md`) is to make the runtime's native
`osXxx_recomp` services replace OB64's verbatim libultra. Mechanism is purely
name-based in N64Recomp (`reimplemented_funcs`), so the work is building the
OB64 libultra **address→name table** and adding it to `symbol_addrs.txt`,
then re-splat → rebuild ELF → re-run `make recomp`.

Rejected for now:
- **MMIO shim first (handoff option 3)**: only to be used surgically for
  registers the game still reads after reimplementation.
- **Address-based N64Recomp config**: viable fallback, but the canonical
  `symbol_addrs.txt` route also names the disassembly for future debugging.
- **Skipping the game's libultra entirely (option 4)**: too risky.

### Decision: this milestone is macOS- and Linux-equivalent; switch for the renderer phase

The libultra-bridging work is CPU-side and platform-independent — no reason to
switch machines for it. The RT64/renderer phase should move to Ubuntu (Vulkan
primary backend). Documented in `guides/linux-migration.md` with the exact
macOS-specific code to remove (`app/CMakeLists.txt` APPLE blocks, the
`__APPLE__` branch in `sdl_platform.cpp::create_window`, the RT64 DXC `libz.dylib`
symlink, Metal toolchain download).

### Decision: correct the function count

PLAN.md's "3659 functions" is stale; the recompiled output has **807 functions**
(802 `func_800xxxxx` + `main_recomp`/`recomp_entrypoint`/others).

---

## 2026-08-24 — Phase 3: first-boot app

### Decision: static recompilation pipeline (unchanged, restated)

Confirmed the project continues on the `N64Recomp` + `N64ModernRuntime` stack
(Zelda 64: Recompiled approach), not a matching decomp.

### Decision: app project layout

- New `app/` CMake project produces the `ogrebattle64` executable.
- `RecompiledFuncs/*.c` (recompiler output) is compiled into a static lib
  (`ogrebattle64_recomp`) and linked into the app.
- `tools/N64ModernRuntime` (ultramodern + librecomp) and `tools/RT64` are pulled
  in via `add_subdirectory`.
- `tools/RecompFrontend` (RmlUi-based menu UI) is intentionally **not** used yet;
  it adds heavy dependencies (RmlUi, lunasvg, GamepadMotion) and is only needed
  for the config/mod menu (Phase 7). Input is wired directly via SDL2 for now.

Rationale: keep the first-boot milestone minimal and debuggable. Adding the
frontend later is purely additive.

### Decision: RT64 as the renderer

RT64 (MIT) is the recommended renderer for N64ModernRuntime projects and the
same one used by Zelda64Recomp. It interprets RDP commands and provides the
`RendererContext` used by ultramodern. Added as submodule at `tools/RT64`.

Build options (same as Zelda64Recomp): `RT64_STATIC=ON`, `RT64_SDL_WINDOW_VULKAN=ON`,
`HLSL_CPU` compile definition. On macOS RT64 uses its **Metal** backend (the
`RT64_SDL_WINDOW_VULKAN` option is only meaningful on Linux).

### Decision: phased bring-up (null renderer first)

The app is structured so the renderer is swappable. For the first boot
validation we use a **null renderer** that:
- acknowledges VI register updates and display-list submissions without drawing,
- logs boot progress (VI swaps, RSP tasks, thread activity).

Rationale: Ogre Battle 64 renders everything through RDP, and RDP commands only
exist after the game's RSP microcode is recompiled. A null renderer lets us
validate the whole boot path (thread scheduler, libultra shims, PI DMA) before
RT64/RSP work lands; it also avoids coupling renderer bring-up with microcode
bring-up.

### Decision: ROM hash constant

`GameEntry.rom_hash` uses XXH3-64 of the full big-endian ROM
(`librecomp` hashes the post-byteswap contents):

```
XXH3_64(assets/ogre64.z64) = 0xbe6adaa5c3f8f7a9
```

### Decision: entrypoint

`GameEntry.entrypoint_address = 0x80070C00` (cart header entry), entrypoint
function = `recomp_entrypoint` (recompiled boot stub). `recomp::init()` already
handles IPL3 variable setup and the initial 1MB ROM DMA, and the boot stub
clears BSS and jumps to the game's `main_recomp`.

### Decision: streamed/overlay code stubs during bring-up

Functions outside the ELF (`0x8016C900+`, `0x84001120+`, etc.) are referenced
via runtime `get_function` lookups, which hard-fail if a function is unknown.
Until Phase 4 implements real overlay loading, the app registers **log-and-return
stubs** for all unresolved addresses via `recomp::overlays::add_loaded_function`.
This lets the boot path survive early references to streamed code.

---

## 2026-08-24 — First boot findings (critical)

### Decision: record the MMIO / libultra-verbatim problem

The app now builds and boots the runtime, but the game crashes on its first
hardware access:

- `recomp::mem_size` is **512 MiB** (not GB); the recompiled `MEM_W` macro maps
  MMIO reads like `0xA4800018` (SI status) to `rdram + ~0x24800000`
  (~580 MiB), which is outside the RW region → SIGBUS.
- More fundamentally, OB64's libultra is recompiled **verbatim** (generic
  `func_800xxxxx` symbols), so N64Recomp's name-based libultra reimplementation
  never fired and the runtime's `osXxx_recomp` services are not called by the
  game. Bridging this (see `HANDOFF-2026-08-24.md` options 1–4) is the next
  milestone.

### Decision: `start_game` must be delayed

Calling `recomp::start_game` before `recomp::start` makes the VI thread skip
its dummy-mode phase and crash on a null VI mode. The app now starts the game
from a thread delayed 500 ms.


### Decision: SDL2 dependency

The app uses SDL2 for window/input/audio. On macOS it is provided by Homebrew
(`brew install sdl2`, which today installs `sdl3` + `sdl2-compat`).

### Decision: licensing

- N64Recomp: MIT
- N64ModernRuntime: GPL-3.0
- RT64: MIT
- RecompFrontend: (see repo)

Because N64ModernRuntime is GPL-3.0 and is statically linked, the final
`ogrebattle64` app is **GPL-3.0**.
