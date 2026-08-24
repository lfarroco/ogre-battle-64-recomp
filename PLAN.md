# Ogre Battle 64: Person of Lordly Caliber — PC Port Project

Goal: a native Windows/Linux/macOS **port** of the N64 game **Ogre Battle 64:
Person of Lordly Caliber** (USA, Rev A) that runs the full game with modern
enhancements and modding support.

**Scope note:** this is a *PC port* project — NOT a byte-exact ("matching")
decompilation. We do not reconstruct the original C source or aim for a
byte-identical ROM. We use **static recompilation** (`N64Recomp`): the ROM's MIPS
code is automatically translated to C, then compiled natively against the
`N64ModernRuntime` runtime library. This is the same approach as Zelda 64:
Recompiled and Majora's Mask / Ocarina of Time PC ports.

**Legal note:** this is a "bring your own ROM" project. No copyrighted ROM data is
committed to this repository. You must provide a dump of your own cartridge.

---

## Goal

A native Windows/Linux/macOS port that runs the full game (menus, world map,
battles, cutscenes, audio, Controller Pak saves) with a modding framework.

## Current status (as of this session)

- ✅ ROM identified: `Ogre Battle 64 - Person of Lordly Caliber (USA) (Rev A)`,
  40 MB dump, 16-bit byte-swapped (`.n64`). Converted to big-endian `.z64`.
- ✅ Cart header decoded (official N64 layout): entry point `0x80070C00`,
  internal name `OgreBattle64`.
- ✅ Boot stub disassembled: clears BSS `0x800AEDB0..0x800E9C20`, stack at
  `0x800C6D60`, jumps to `main` at `0x8007F880`.
- ✅ splat-based disassembly (see `config.yaml`): main code segment ROM `0x1000..`
  mapped at `0x80070C00+`; ~185 KB of code split into `asm/`.
- ✅ ELF built (`make`): `build/ogrebattle64.elf` with relocations
  (`--emit-relocs`) and correct vram/ROM section mapping.
- ✅ **Full main-segment recompilation succeeds**: 807 functions recompiled to C
  (`RecompiledFuncs/`, regenerated with `make recomp`), and the generated C
  **compiles** against the runtime headers.
- 🚧 **Phase 3 (first boot app) in progress**:
  - ✅ `app/` CMake project created (ultramodern + librecomp + RT64 + SDL2 +
    `RecompiledFuncs`); **builds** on macOS (Intel + Metal toolchain).
  - ✅ RT64 added as submodule (`tools/RT64`); all RT64 contrib submodules
    initialized.
  - ✅ Game entry wired: `rom_hash=0xbe6adaa5c3f8f7a9`, entrypoint
    `0x80070C00`/`recomp_entrypoint`, base overlays + streamed-code stubs.
  - ✅ Platform callbacks: SDL2 window/input/audio, error box, threads.
  - ⬜ **Null renderer + stub RSP**: first boot validation (log VI swaps, boot
    progress). RT64 integration follows once the RSP microcode is recompiled.
  - ⬜ RSP microcode recompilation (F3DEX/L3DEX/S2DEX/audio) — see
    `docs/guides/rsp-microcode.md`.
  - ⬜ libultra/verbatim recomp bridging: OB64's libultra was recompiled verbatim
    (generic symbol names), so runtime-native services (VI/AI/PI/threads) need
    wiring. Plan + seed symbol table: `docs/LIBULTRA-BRIDGING.md`.
- ⬜ Streamed/overlay code segments (battle engine, cinematics) — after first boot.
- ⬜ Asset extraction (sprites, text, audio) — after first boot.


## Toolchain

| Tool | Purpose | Location |
|---|---|---|
| splat 0.50 (`splat64[mips]`) | ROM splitting / disassembly | `tools/venv` |
| spimdisasm | MIPS disassembler (used by splat) | via pip |
| mips-linux-gnu-binutils | assemble `.s` → `.o`, link ELF | Homebrew |
| N64Recomp (forked) | MIPS → C recompilation | `tools/N64Recomp` |
| N64ModernRuntime | recompiled game runtime (libultra shim, renderer) | `tools/N64ModernRuntime` |
| RecompFrontend | app shell (menus/input UI) | `tools/RecompFrontend` |

Our N64Recomp modifications (cop0 register support, TLB/ERET/cache instructions,
cross-function branch handling, overlay-target function lookup) are in
`n64recomp-ob64.patch`; apply with `git apply` after cloning upstream.

## Reproduce (macOS)

> Linux (Ubuntu) setup: see `docs/guides/linux-migration.md`. The commands below
> are the same except `brew install` → `apt install` equivalents (listed there).

```sh
# 1. Tools
brew install mips-linux-gnu-binutils cmake
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4

# 2. ROM: place your big-endian dump at assets/ogre64.z64

# 3. Disassemble + link + recompile
tools/venv/bin/splat split config.yaml   # regenerate asm/
make                                    # assemble .s, link ELF
make recomp                             # generate RecompiledFuncs/*.c
```

## Key technical findings

- **Byte order**: the dump is 16-bit byte-swapped (`.n64`). Convert to big-endian
  by swapping adjacent bytes (`assets/ogre64.z64`).
- **Header layout**: official Nintendo layout — entry point at offset `0x08`
  (`0x80070C00`), internal name at `0x20` (`OgreBattle64`), country at `0x3E`.
- **Memory map** (from boot stub + code analysis):
  - ROM `0x1000` ↔ vram `0x80070C00` (entry segment, `0x60` bytes)
  - main segment ROM `0x1060` ↔ `0x80070C60`, ending at BSS start `0x800AEDB0`
  - BSS `0x800AEDB0..0x800E9C20`, stack `0x800C6D60`, `main` at `0x8007F880`
- **libultra**: the main segment contains libultra (OS kernel, exception handler,
  TLB code). Its functions use cop0 registers beyond Status; supported via the
  `cop0_read`/`cop0_write`/`tlb_instruction`/`eret`/`cache_instruction` runtime
  shims added in the patch.
- **Streamed code**: functions at `0x8016C900+` and `0x840010BC` are referenced
  from the main segment but live in streamed/overlay data; calls to them are
  recompiled as runtime function lookups (`get_function`). Mapping those overlays
  is a later phase.

## Roadmap

1. **Phase 0 — toolchain & ROM** ✅
2. **Phase 1 — disassembly & ELF** ✅
3. **Phase 2 — recompile main segment** ✅
4. **Phase 3 — first boot** (runtime app: window, renderer, input, libultra shim)
5. **Phase 4 — overlays & streamed code** (battle engine, cinematics)
6. **Phase 5 — assets** (sprites, text, audio; custom extractor)
7. **Phase 6 — saves, audio, QoL** (Controller Pak, widescreen, controls)
8. **Phase 7 — modding framework & packaging**

## Phase 3 notes (first boot app)

The app is a CMake project that:

- `add_subdirectory`s `tools/N64ModernRuntime` (runtime libs) and
  `tools/N64Recomp` (headers).
- Compiles `RecompiledFuncs/*.c` (with `-I tools/N64Recomp/include`) into a lib.
- Implements the host callbacks in `recomp::Configuration` (see
  `librecomp/include/librecomp/game.hpp`):
  - `renderer_callbacks` (RT64 recommended; Vulkan/D3D12/Metal via RT64)
  - `rsp_callbacks` (use N64Recomp's `RSPRecomp` for the game's audio/gfx ucode)
  - `audio_callbacks`, `input_callbacks`, `gfx_callbacks`, `events_callbacks`,
    `error_handling_callbacks`, `threads_callbacks`
- Boot sequence: `recomp::select_rom(...)` → `recomp::start(cfg)` →
  `recomp::start_game(game_id, ...)`, or headless test:
  load ROM, create `recomp_context`, call `recomp_entrypoint(rdram, &ctx)`.
- The runtime's `recomp.h` context now carries `cop0_regs[32]`; the runtime
  provides `cop0_read/write`, `tlb_instruction`, `eret`, `cache_instruction`
  (no-ops), `osGetCount`/`osGetTime` etc.

Reference implementations to study: `Zelda64Recomp/Zelda64Recomp` (`src/`,
`us.rev1.toml`) and `N64Recomp/RecompFrontend`.

