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

## Browser port (WebAssembly)

In addition to the native port, the project is investigating a **WebAssembly /
browser build** (`docs/WEB-PORT.md`): run the recompiled game + N64ModernRuntime
under Emscripten with a **null renderer** first (no graphics), then decide
whether/how to render in the browser (WebGL2 vs WebGPU vs RT64-webgpu). The
immediate goal is proving the recompilation itself is portable, independent of
RT64 — which also sidesteps the native GPU-driver problems seen on older
hardware. RT64 remains the primary native renderer throughout. See
`docs/WEB-PORT.md`, `docs/WEB-PORT-REPORT.md` and `docs/WEB-PORT-DEPLOYMENT.md`.

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
  - ✅ **RT64 GBI question resolved (session 10)**: OB64's DLs are F3DEX2-compatible — the first gfx task DL is `0xDE`=G_DL → `0xA9EF0`, `0xE9`=FULLSYNC, `0xDF`=ENDDL, and RT64's hash-based auto-detection (`getGBIForUCode`) already matches the ucode to `GBIUCode::F3DEX2` (ucode enum 7), which parses them correctly. The session-9 "force F3DEX" override was wrong (plain F3DEX does not map 0xDE/0xDF) and was removed. No RT64 changes needed. (see `docs/HANDOFF-2026-08-29-session10.md`).
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
  - ✅ **Boot-stall root-caused & fixed (session 15)**: the session-14
    "VI-retrace message-queue deadlock" was a misread — the real cause was a
    **regression**: the session-8 `yield_self` poll-loop emission was missing
    from the vendored N64Recomp fork (reset to upstream; patch lost the hunks),
    so thread 3's spin at `0x80075FB8` starved the cooperative scheduler's
    drainer. Also fixed: the scheduler threw "No threads left to run" when all
    game threads blocked (now idles on external messages), `os*Mesg` drained
    the entire external-message backlog per call (now capped at 32), and the
    debug traces slowed the headless browser ~100× (now gated behind
    `OGRE_DEBUG_TRACES=1`). The web build now boots to a **stable, healthy
    idle state** — no crash, no deadlock, all queues draining at 60 Hz.
    **Next wall (game behavior):** thread 3 blocks on its own count-1 queue
    `0x800C6C98` (sender unknown — audio/AI path?), so the frame-producer
    thread (`func_800893C0` → `func_8008949C` → `osViSwapBuffer`) never runs
    and the game submits only the boot blanking DL. See
    `docs/HANDOFF-2026-08-31-session15.md`.
  - ✅ **First real game frame rendered in the browser (session 19)**: the
    WebGL2 renderer prototype was the only thing blocking rendering. Four bugs
    fixed in `app/src/web_renderer.cpp`: (1) `ogre_gfx_flush()` blocked the
    browser **main thread** on the renderer mutex while `send_dl()` held it for
    the whole DL analysis/execution — a contended pthread mutex cannot be waited
    on by a browser main thread, and this hard-froze the page the moment the
    title path advanced; (2) `queue_triangles()` re-locked that non-recursive
    mutex (already held by `send_dl`) and self-deadlocked on the first real draw
    command, permanently holding the lock; fixed structurally by recording into
    an execution-local buffer and publishing under a short lock; (3)
    `read_matrix()` decoded the N64 `Mtx` as 16 consecutive `s16` instead of
    4x4 `s16` integer parts + 4x4 `u16` fraction parts, so most matrices were
    all-zero and **every 3D triangle was rejected**; (4) segment registers stored
    the full base instead of its high byte. Also fixed R/B swap in
    `G_SET*COLOR` and the shader's `COMBINED` combiner input. Result: the title
    DL executes, its draws reach GL, the canvas shows real (brown/orange) game
    content, and the game submits frames continuously. **Project-model
    correction:** the sessions 16–18 "cb88 NULL frame-2 wall" is a red herring
    (that is the boot blanking task; steady-state frame tasks use key 4 →
    `cb84 = osViSwapBuffer`, enqueued by `func_80073AE4`, gated on
    `D_800E810C`), and the "heap-walk wedge" was a downstream symptom of the
    frozen main thread + deadlocked mutex. **Next:** implement
    `G_MOVEMEM MV_VIEWPORT` (still a no-op, so 3D geometry is partly
    off-screen), then regenerate the combiner mux tables from RT64's
    `rt64_color_combiner.h`. See `docs/HANDOFF-2026-09-10-session19.md`.
  - ✅ **The title scene renders in the browser (session 20)**: seven
    `app/src/web_renderer.cpp` bugs (viewport no-op, `Mtx` column-pair swap,
    ignored projection `MUL`, wrong modelview `MUL` order, F3DEX2's inverted
    `G_MTX` PUSH bit, `push_back(vector.back())` UB, empty/inverted rectangles
    drawn as full-screen covers) took the first title DL from 18/24 triangles
    rejected and a black canvas to 0 rejected and a stable ring of twelve
    sprite soldiers. The on-page console was also replaced by a bounded
    `window.ogreLog` ring buffer (per-line DOM writes were throttling the
    emulator). See `docs/HANDOFF-2026-09-10-session20.md`.
  - ✅ **The title sprites are decoded the way the RDP samples them (session 23)**:
    five decode bugs kept the title scene looking like scrambled slivers. Every
    `Vtx` field was read at its *logical* offset out of the runtime's
    byte-reversed rdram (the accessor rule is `off ^ 2` for halfwords), so the
    renderer actually decoded `(y,x) (flag,z) (t,s) (a,b,g,r)` - the sprite
    quads were transposed and their texture coordinates with them. `G_TEXTURE`'s
    scale (`sc = 0x8000` -> `s/64`, not `s/32`) was ignored; I8/I4 texels did not
    carry their intensity into alpha, so the `TEXEL0_ALPHA` mask was constantly
    1; the blend was guessed from `othermode_l & 0xFFF` with a heuristic that
    read the wrong bits (OB64's sprites are a two-cycle `FORCE_BL` XLU, so
    blending was off); and the texture was decoded with the *load*'s
    format/rect instead of the *render tile*'s (OB64 loads its 32x34 I4 mask as
    16 bytes/row of 8-bit texels). `Blender` now mirrors `rt64_blender.h` field
    for field and `ensure_tile_image()` decodes each tile's rect at the tile's
    format with `line * 8` as the stride. Verified by two new probes:
    `textures.cjs` (dump the decoded images and their `colour x mask`
    composites) and `spritecheck.cjs` (render one sprite and compare it with
    that composite) - they now match. `boot.cjs`: 21 680 non-black / 17 764
    colorful, twelve readable characters. `testdraw.cjs` was also repaired: its
    scratch address resolved through a segment to zeroed memory and its combiner
    word evaluated to 0, so the isolation probe had been drawing black; it now
    shows the synthetic 2x2 texture's four quadrants. A follow-up found the
    real reason the sprites looked like streaked columns: `G_FILLRECT` and
    `G_TEXRECT` take `lrx/lry` from **w0** and `ulx/uly` from **w1** (SDK and
    RT64), and the renderer had the two words swapped - so every full-screen
    rectangle decoded as inverted, the session-20 empty-rect guard dropped it,
    and **the game's per-frame clear never ran: frames accumulated on the
    canvas**. The intro also fades in from black through a full-screen
    PRIM-alpha rect drawn after the sprites, so the first rendered frame is
    black and its pixel counts are not a fidelity measure. Geometry, UVs and
    tile sizes are constant over 48 display lists. Against the reference
    screenshot from a real session the scene's layout now matches; one sprite's
    rendered pixels still do not match its texture pair (see the handoff's
    open item 0). Next: the idle trajectory (unchanged -
    ~half of all boots never build a real display list), a reference frame to
    compare against, and the remaining combiner inputs
    (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys). See
    `docs/HANDOFF-2026-09-11-session23.md`.
  - ✅ **The title sprites render correctly (session 24)**: the "green smudges"
    over the soldiers' helmets and the dim colour cast had two independent
    causes, both now fixed and both verified against the sprite's own
    `colour x mask` composite. (1) **Texture decode**: every texture byte was
    read at its *logical* offset out of the runtime's byte-reversed rdram, which
    returns a different byte of the same word - for the RGBA16 colour tile that
    swapped every horizontal texel pair, and for the I4 alpha mask it reversed
    the four bytes of every word (8 texels per group), landing the mask's alpha
    in the wrong places. RT64's `loadWord` (`RDRAM[(textureAddress + i) ^ 3]`) is
    the reference; `n64be16` now sits beside `n64h`/`n64b` and `OP_LOADTLUT` uses
    it too. (2) **Blender**: cycle 0 of the sprite render mode
    (`b0=[P=CC M=CC A=CC_A B=ONE]`) is `(P*a + M*b)/(a+b)` with `P == M`, i.e.
    exactly `P`, but the port of RT64's `Blender::runCycle` folded the numerator
    with `fmod(..., 1+8/255)` unconditionally. The fold is per channel, so a
    bright orange pixel at mask alpha 0.5 came out green - the smudges - and
    opaque pixels came out about half brightness. The fold is now skipped when
    `P == M`, where the division cancels it exactly. The viewport, which was
    correct only because it cancelled the same byte swap by hand behind a comment
    that described the hardware wrongly, now reads through `n64h` with the
    canonical `(x, y, z, 0)` indices. The settled canvas shows twelve readable
    soldiers where it used to be scrambled slivers. See `docs/DECISIONS.md`
    (session 24) and `docs/HANDOFF-2026-09-11-session24.md`.
  - ✅ **The native RT64 build runs on macOS again (session 24, native
    bring-up)**: the web build had become the project's only renderer
    (`web_renderer.cpp` 3 355 lines vs `renderer.cpp` 357, and sessions 19-24 all
    web work), so the native path was brought up before committing to a Linux
    move. The only blockers were two bugs in the app's own SDL glue, both
    macOS-only: `create_window` handed RT64 the `SDL_Window*` where plume casts to
    an `NSWindow*` (instant `EXC_BAD_ACCESS`) and left the `CAMetalLayer*` null -
    the `SDL_Metal_GetLayer` segfault it was stubbing around does not reproduce
    under sdl2-compat 2.32.70; and `poll_input` called `SDL_PumpEvents` from the
    game thread, which Cocoa rejects with `NSInternalInconsistencyException` (2 of
    3 runs aborted there). RT64 now initialises Metal (`api=3`), the window opens,
    and 3/3 runs survive. `docs/guides/linux-migration.md` is revised to
    "optional" accordingly - Linux is a preference (Vulkan is RT64's best-tested
    backend), not a requirement. **The game-side stall is unchanged and
    platform-independent**: every native run submits only its boot blanking
    display list, so the canvas stays black unless input is fed (the web probes
    press Enter every 5 s for exactly this reason). See `docs/DECISIONS.md`
    (session 24 native bring-up).
  - 🚧 **The boot thread's real stack is now readable, and the renderer half is
    proven (session 26)**: the previous "t4 blocks on `0x800C4C28`" was an
    artifact of the snapshot view - that queue *is* fed (the send trace shows
    `func_800891A0 → 0x800C4C28` dozens of times). The recompiled code now emits
    an entry hook (with `ctx->r31`, the caller's return address, and the
    function's name) and a return hook, and the runtime keeps a per-thread
    shadow call chain; `OGRE_CHAIN_HISTORY=3` shows the boot thread going
    `func_80071EB0 → func_8008A1B0 → func_80089660 → func_80089804` and never
    returning from the **frame submit**. Two further results: the session-15
    `func_80089A10_recomp` "yielding spin" fix is **dead code** (the recompiled
    spin is a strong symbol and direct calls link to it, so the boot thread
    really does execute `while (D_800E79A4 != 0);` in a cooperative scheduler);
    and a hand-built F3DEX2 display list (`OGRE_SYNTH_FRAME=1`,
    `app/src/synth_frame.cpp`) now reaches `send_dl`, where RT64 parses and
    executes every command (seven `setFillColor`s, seven `fillRect`s with the
    right colour image) - the window still shows black, so the remaining gap is
    presentation, not parsing or drawing. See
    `docs/HANDOFF-2026-09-12-session26.md`.
  - ✅ **Game audio is muted by default (session 22)**: the audio microcode is
    not emulated (the RSP audio task is only auto-completed), so what the game
    hands the AI interface is garbage and playing it is a wall of screeching.
    The AudioWorklet is still created and still drains the ring (the game's
    `get_frames_remaining()` backpressure is load-bearing), only the output is
    silenced. `?audio` on the URL, or `window.ogreAudio.setEnabled(true)`,
    unmutes it for audio work. See `docs/HANDOFF-2026-09-11-session22.md`.
  - ✅ **The intro's frame-31 "runaway walk" is fixed (session 21)**: OB64's
    display lists mix KSEG0 and **segmented** addresses, and the renderer
    resolved a segment as `(base_high_byte << 24) | offset` instead of RT64's
    `segment_base + offset`, so `DE000000 0E000000` (segment 14) sent the DL
    walker to zeroed rdram (4 M `G_NOOP`s) and `FD180000 0F000000` (segment 15)
    sent the texture decoder to garbage. `G_SETOTHERMODE_H/L` also never applied
    its data word (so every draw was `G_CYC_1CYCLE`), alpha's D selector read the
    wrong word, the DL walker ignored `G_MW_SEGMENT`, and the combiner was
    evaluated with position-independent selectors. With all of that fixed the
    title scene reaches ~39 display lists per 75 s (session 20 escaped at ~31)
    with the correct `G_CYC_2CYCLE` + bilerp + perspective state and a two-unit
    texture abstraction (colour from TEXEL1, alpha mask from TEXEL0). Next:
    pacing (half of all boots still stall at 1-8 display lists; ~3 gfx frames/s),
    the blend/render-mode decode, and a real TMEM/tile UV model. See
    `docs/HANDOFF-2026-09-11-session21.md`.

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

