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
    `RecompiledFuncs`); **builds** on Linux (Ubuntu).
  - ✅ RT64 added as submodule (`tools/RT64`); all RT64 contrib submodules
    initialized.
  - ✅ Game entry wired: `rom_hash=0xbe6adaa5c3f8f7a9`, entrypoint
    `0x80070C00`/`recomp_entrypoint`, base overlays + streamed-code stubs.
  - ✅ Platform callbacks: SDL2 window/input/audio, error box, threads.
  - ✅ **Null renderer + stub RSP**: first boot validation — the game now
    **boots without crashing** (2026-08-24, session 3): 7 N64 threads start,
    the VI thread swaps buffers at ~50-60 Hz, and the game runs its init.
  - ✅ **Libultra bridging (first 3 batches)**: 24 libultra functions named in
    `symbol_addrs.txt` so the runtime's native `osXxx_recomp` services replace
    OB64's verbatim libultra (see `docs/LIBULTRA-BRIDGING.md`). Also fixed a
    runtime bug in the initial 1MB DMA (sign-extension of the entrypoint).
  - ✅ **Libultra bridging (batches 4-6, session 4)**: osCont family + timers +
    event registration (`osViSetEvent`/`osSetEventMesg`) + `osSpGetStatus`
    named; corrected `osSendMesg`/`osJamMesg` (the game's primary send is
    `func_80093810` = `osSendMesg`). Added an external-message **drainer
    thread** to the runtime (blocked game threads otherwise deadlock waiting
    for VI/SP events). The game now boots cleanly, **submits its first RSP gfx
    task**, and sets a **real VI mode** (see `docs/LIBULTRA-BRIDGING.md`,
    session 4).
  - ✅ **RT64 renderer (session 5)**: the null renderer is replaced by a real
    RT64/Vulkan renderer (`app/src/renderer.cpp`, window created with
    `SDL_WINDOW_VULKAN`). It initializes, presents frames at ~60 Hz, and the
    game boots through it (see `docs/HANDOFF-2026-08-25-session5.md`).
  - ✅ **Boot stall diagnosed & fixed (session 6)**: the "stalls waiting for a
    second task" was wrong — the game thread busy-spins (`func_80089A10`) and
    starves the runtime's external-message drainer. Fixed by reimplementing the
    spin as a yielding wait, reimplementing the game's DMA-request path
    (`func_8008BC40`, whose PI-manager globals were dead) as a synchronous ROM
    read, and fixing DMA byte order (must use `recomp::do_rom_read`). The game
    now boots through the RSP pipeline, controllers, and 700+ streamed-overlay
    DMA loads into its main loop (streamed functions stubbed). **Next wall:
    streamed-overlay relocation/recompilation = Phase 4.**
    (see `docs/HANDOFF-2026-08-25-session6.md`).
  - ✅ **Phase 4 started — streamed overlays A+B+C recompiled (session 7)**:
    - ✅ Determined the overlay format: each streamed overlay is **plain linked
      MIPS code+data** DMA'd to a **fixed** RAM address; pointers inside are
      already absolute for that address (no runtime relocation pass). The
      boot-resident overlays are A (ROM 0x3F1B0 → RAM 0x800E9C20) and B
      (ROM 0x40E80 → RAM 0x8016AF80); a third, C (ROM 0x1CE040 → RAM
      0x80197B90), is loaded on-demand by the game's streamed loader (segment
      table at ROM 0x387C0).
    - ✅ `config.yaml` now disassembles A/B/C as code segments (+ data at
      0x40640/0x5C230/0x1EE540); the bin gaps (0x66E30, 0x1F0A00) are pinned to
      their ROM address so they don't occupy RDRAM VMA space. Overlay C needed
      `bss_size` + a `.bss` subsegment for its 0x801BA550 tail.
    - ✅ Recompiled together: 807 → ~1520 recompiled functions. Cross-overlay
      absolute references (e.g. overlay B taking the address of overlay C's
      `.L8019EE70`) are fixed by `tools/fix_cross_overlay_labels.sh`, wired into
      the Makefile so it re-applies after every `splat split`.
    - ✅ The app registers A+B+C via `load_overlays()` from the GameEntry
      on_init callback (the runtime's `recomp::init()` now loads only the base
      sections — a full 1MB load would register the overlays at wrong
      addresses). **A DMA-hook approach was tried and reverted**: `load_overlays`
      assumes whole-overlay DMAs, but OB64 streams in 0x200-byte chunks, which
      produces an inverted bound range.
    - ✅ Boot now passes the old `func_80075BC0` function-pointer-table crash
      (the entry is a real recompiled overlay-B function that returns a valid
      descriptor), and the game runs ~1520 real functions to load the next
      overlay's data blocks before **spinning on N64 threads 1+3** (≈200% CPU).
      No stub calls and no `get_function` hard-fail in the whole boot.
    (see `docs/HANDOFF-2026-08-25-session7.md`).
  - ⬜ **Fix RT64's GBI match**: OB64's ucode is "F3DEX fifo 2.08" (short-
    format opcodes 0xDE/0xDF/0xE9...), which RT64's hash DB misidentifies as
    F3DEX2 (0xDE=G_DL). Add/force the correct GBI before real display lists
    flow (see `docs/guides/rsp-microcode.md` + session-5 handoff).
- ⬜ Phase 4 next walls (session 8 findings):
  - ✅ **The post-boot spin is FIXED** (session 8): Thread 3's `.L80075FB8`
    pure spin on `D_800C4C26` starved the cooperative scheduler's drainer, so
    the VI-retrace events that advance the boot state machine were never
    delivered (same class as session 6's `func_80089A10`). N64Recomp now emits
    `yield_self(rdram);` for poll loops (backward call-free store-free branches
    reading a loop-invariant address), so the game thread yields and the
    drainer can run. Boot now progresses through the overlay-D loads, the
    controller path, and the RSP pipeline into **title-screen display-list
    building** (overlay C `func_801A1FCC` → `func_8019FC68`).
  - **Next crash: `func_8019FC68` at 0x8019FFF4** — stores through
    `0x803ffa7b + entry->[0x34]` where a graphics-object record in the heap
    array at `0x803fefa0` (`D_801B81D0`) has a garbage `[0x34]` (varies per
    run) → address wraps → SIGSEGV. Array is zeroed at alloc; root cause open
    (leads in `docs/HANDOFF-2026-08-25-session8.md`).
  - ✅ **The `func_8019FC68` crash is FIXED (session 9)** — root cause was a
    recompilation bug, not the game: OB64's KMC compiler merges identical
    epilogues into a separate symbol the preceding function falls through into
    (e.g. `func_8019ABC4` falls into `func_8019AF0C`), and N64Recomp never ran
    the shared epilogue, so callee-saved registers and `$sp` were left
    clobbered. N64Recomp now emits fall-through tail calls
    (`next_func(rdram, ctx); return;`) when a function's code falls off the end
    into the next function, and processes its static functions to a fixpoint
    (registering them by address). See
    `docs/HANDOFF-2026-08-25-session9.md`.
  - 🚧 **Boot now reaches real rendering** — after the fix, boot passes the
    title-screen DL build, runs ~1528 PI DMAs / 64 RSP tasks (including new
    `type=1` tasks), and RT64 renders frames. **Next wall:** crash in the RT64
    render thread calling the Intel Haswell Vulkan driver
    (`libvulkan_intel_hasvk.so`) inside `submitRasterScene` — may be the
    long-standing flaky renderer race, an RT64 GBI issue, or a driver bug
    (leads in the session-9 handoff).
  - **Flaky renderer-init segfault** (before any game code; doesn't reproduce
    under gdb) — likely the known renderer-init flakiness; needs the
    mode/framebuffer race fixed.
  - **RT64 GBI fix** (still pending) before real display lists.
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

