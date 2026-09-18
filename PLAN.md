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

**Working rules for AI agents: [`AGENTS.md`](AGENTS.md).** Read it before you
start. It is short, and every rule in it is a session that lost time —
especially "ask the developer what the current scene should display instead of
inferring intent from the code" and "a previous handoff is a hypothesis, verify
it at instruction level".

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
  - ✅ **The game renders natively (session 28)**: the boot thread was not stuck
    in the game - it was **stranded by a scheduler bug in the runtime**. A thread
    parked in `wait_for_resumed` could consume a *message-queue poke* as if it
    were a handoff (both used the same `running` semaphore), so two game threads
    ran at once, the cooperative scheduler's invariants broke, and the priority-10
    boot thread was eventually left parked with an empty running queue while the
    per-VI service threads (pri 50-120) ping-ponged. Found with a new race-free
    scheduler event ring (`OGRE_SCHED_TRACE=1` -> `[sched-ring]`, with a global
    sequence number and the running queue after each event). Fixed by splitting
    the semaphores (`running` = strict handoff, `poke` = idle wake) and by
    refusing `NULL` queues in the queue helpers; the VI thread's queue snapshot
    also stopped dereferencing wild KSEG0 values (its guards now use the game's
    RDRAM window, `0x80000000..0x80800000`, not all of KSEG0 - that was an
    `EXC_BAD_ACCESS` on 3 of 4 input runs). Result: 6/6 no-input and 4/4 input
    runs reach the frame loop and submit the game's real display lists
    (`0x801C1520`/`0x801C80A0`, ~2.5/s) instead of only the boot blanking list,
    and the presented swap chain shows the **title screen**:
    `docs/proofs/native-game-title.png`. See
    `docs/HANDOFF-2026-09-12-session28.md`.
  - ✅ **The title runs at the game's 30fps (session 29)**: the "low, inconsistent
    frame rate" was the recompiler, not the game or the renderer. N64Recomp's
    poll-loop heuristic emitted `yield_self` inside ordinary loops: it excluded
    the branch **delay slot** from the loop body (so `bne ..., head` /
    `addiu $v1, $v1, 0x13C` looked loop-invariant) and treated a load's
    destination as a non-write (so a pointer chase looked like a spin on a fixed
    address). The game's malloc free-list walk (`func_80071A3C`), the allocator
    family and `func_80081B08`'s 36-entry object scan each yielded once per
    element; with `yield_self` sleeping for the next VI retrace (~16.7ms) that
    turned a microsecond loop into hundreds of milliseconds, so the frame rate
    was 2-8fps with 0.3-2.8s stalls. Fixed in the heuristic (include the delay
    slot; treat a load destination as a write unless the load's base is a
    `lui`-set constant) - `yield_self` fell 99 -> 14, all 14 being real poll
    loops (`func_80089A10`, `func_80075BC0`, ...). Result: **171 display lists in
    6.2s, median inter-frame gap 34ms (~30fps), no stall over 500ms** (was 8
    display lists in 8s). See `docs/HANDOFF-2026-09-12-session29.md`.
  - ✅ **The title's object table is populated (session 30)**: the "cube never
    falls" diagnosis was a recompilation bug, not a missing overlay. State 9's
    init calls `jal 0x801A1034`, and splat had emitted the *prologue* of
    `func_801A103C` as its own symbol, so N64Recomp compiled a 2-instruction
    stub that returned immediately and the 13 texture records at `base+0x6C0`
    (11 soldier objects + record 10, the object the script walks downward = the
    fall) were never created. `config.toml` now overrides
    `func_801A1034 = 0x13C`; with it all 13 records exist with real object
    pointers, `flag=255` on 0-10, and record 10 animating (`x=124 y=-144`).
    `OGRE_SCENE_TRACE=1` shows this live. See
    `docs/HANDOFF-2026-09-12-session30.md`.
  - ✅ **The intro no longer crashes; the game renders continuously (session
    31)**: the `func_801A1FCC` SIGSEGV was a stack-frame defect, not a missing
    scene. Two N64Recomp bugs, both now fixed:
    1. a `jal`/`j` into a body extended by `function_sizes` was still compiled
       and called as a standalone symbol, so `func_80198D28` (a false split
       inside `func_801989AC`) ran `func_801989AC`'s epilogue without its
       `0xB8` frame — leaking 0x10-0x38 of stack per call and corrupting every
       caller's saved `$s3` (the crash's `$s3 = 0x80000318`). Such targets now
       tail-call the containing function (24 sites redirected);
    2. a body whose last instruction is not a control transfer
       (`static_16_801984BC` → the shared epilogue `static_16_801985D8`) ran
       off its end instead of tail-calling the next function. Now emitted as a
       tail call + `return` (1726 sites), which unwinds the frame.
    Session 30's over-wide `func_80197C94` override (`0x948`, which swallowed
    the real function `func_801980A0`) is also corrected to `0x40C`. Result:
    **8s run exits 0 with 205 display lists** (was SIGSEGV at display list 1,
    t≈845ms), a 15s run reaches 395 display lists at t=14.2s (~28fps), and the
    per-frame lists carry real geometry (12-17 triangles, 27-32 `SETTIMG`).
    See `docs/HANDOFF-2026-09-12-session31.md`.
  - ✅ **A visual proof of the intro exists (session 31)**:
    `docs/proofs/native-intro-impact.png` (t≈3.3s: twelve soldiers, impact
    smoke, the block falling between them) and
    `docs/proofs/native-intro-title.png` (t≈8.4s: the N64 logo over the title
    screen), both swap-chain readbacks
    (`OGRE_CAPTURE_PRESENT=… OGRE_CAPTURE_AFTER=…`). Confirmed by eye: soldiers,
    falling block, block → N64 logo, title screen.
  - ✅ **The title screen's white band is fixed (session 35)**: the retail title
    screen has slowly scrolling clouds behind the logo; the port drew a solid
    white band across rows 64-128. The band was not a shader or a 3D/2D
    compositing bug: the game ends each cloud-layer strip loop with a tile that
    covers **zero texels** (`SETTILESIZE uls=0 ult=420 lrs=1276 lrt=420`, i.e.
    `lrt == ult`) and then draws six right-to-left `G_TEXRECT`s with the
    `RGB = ONE, ALPHA = TEXEL0` combiner (`FCFFFFFF FFFF73B9`). RT64 has to
    round an empty sampling rectangle up to one texel (an empty texture cannot
    be decoded), so those rectangles sampled one stale TMEM row - fully opaque
    in the layer-2 texture - and composited as opaque white. `RDP::drawTexRect`
    (and the browser renderer's `draw_texrect`) now skip a rectangle whose
    configured tile covers no texels, which is what the hardware shows: the
    retail title screen has clouds there and no band. Proofs:
    `docs/proofs/native-title-band-before-after.png` (crop) and the refreshed
    `docs/proofs/native-intro-title.png`. See
    `docs/HANDOFF-2026-09-14-session35.md`.
  - ✅ **Scene jumps work, and the title fog is fixed (session 36)**:
    `OGRE_SCENE=<name|hex>` now actually enters a screen. It had been a silent
    no-op — the poke has to land before the boot enters its first scene
    (~1.1 s), but `OGRE_SCENE_AFTER_MS` defaulted to 3000 — and
    `OGRE_SCENE_LOG=1` SIGBUSed on a stale scene-descriptor word at ~1 s. Both
    are fixed (`OGRE_SCENE_AFTER_MS` default 0, descriptor validated, new
    `OGRE_SCENE_TRACE=1`), and the scene table is now verified by capture:
    `title` = `0x04` (logo + PRESS START over the clouds), `intro` = `0x09`,
    `publishers` = `0x0A`, `story` = `0x0B`, `unit-info` = `0x0C`; `0x18`/`0x02`
    still crash on the unfinished cross-bank work. With the title reachable, the
    reported `native-title-fog-isolation.png` (a crop of the story screen) could
    be re-measured, and the `OGRE_FOG` repair turned out to be drawing nothing:
    its `G_SETTILESIZE`/`G_LOADTILE` extents were in texels rather than the GBI's
    quarter-texels (a 64x64 image declared as a ~16x16 tile), and only the first
    white group per frame was repaired, so the `©1999 QUEST` bottom sweep kept a
    dead tile. A follow-up round fixed the fog blinking (the sweep's scroll ran
    past the 64-texel image and the group was dropped; the draw tile now wraps).
    The fog's level is the game's own asset: the combiner is
    `RGB=ONE, ALPHA=TEXEL0`, so the overlay is white modulated by the layer
    image's intensity (5.1% mean / 20.4% peak) and `OGRE_FOG_SCALE` defaults to
    100 — a retail emulator capture measures the port at 113% of the retail fog
    contribution in the clean `©1999 QUEST` cloud region. Proofs:
    `docs/proofs/native-title-fog-isolation.png`,
    `docs/proofs/native-title-fog-quest-region.png`,
    `docs/proofs/native-title-band-isolation.png`. See
    `docs/HANDOFF-2026-09-14-session36.md`.
  - ✅ **New Game no longer crashes; the game reaches scene 0x0D (session 37)**:
    title → Start → New Game died in scene `0x02`'s update (`jal 0x80198D28`
    bound to the containing overlay-C body, reading N64 address 3 with `$a0`
    unset). That one call site is now dispatched through the bank map
    (`cross_bank.py dispatch --only`, wired into `make recomp`, plus a repair
    of the recompiler's tail-call emission at the site), resolving to bank
    unit E's real entry. Scene `0x0D`'s record 4 and record 14's two arena
    modules are newly compiled into unit C (five `function_sizes`), so the
    scene loads 9 bank records with zero stub calls. It then dies in list
    management (`func_80071950`) on a wild data pointer — the next wall,
    reproducible with no input at 1× speed. `OGRE_NO_AUDIO=1` separately
    crashes in early boot (queue-snapshot diagnostic). See
    `docs/HANDOFF-2026-09-14-session37.md`.
  - ✅ **Scene 0x0D init: two more dispatches, record-BSS zeroing, and the
    residue wall (session 38)**: the forced new-game run died in stale
    overlay-C code (`jal 0x801AFC2C` → `func_801AFAF4` while record 10 owns
    the address) — dispatched to bank unit C's real entry, same for
    `jal 0x801980A0` (fragment → unit E's real entry). Record loads now zero
    their segment-table BSS (the port never did; `0x0D`'s list anchors live
    in record 10's BSS). The remaining forced-`0x0D` crash is missing
    pre-state, proven by flag lifecycle probes: `0x0D` reads init flag
    `0x8019F794 == 0` because `0x02`'s single-frame writer never runs, while
    natural boot leaves that region nonzero — so the natural path
    (title → menu → `0x02` → `0x0D`) is required, and it is blocked at menu
    `0x18`'s pre-existing unit-D crash. See
    `docs/HANDOFF-2026-09-14-session38.md`.
  - ✅ **New Game plays into 0x0D and the natural path loops cleanly (session
    39)**: two more false merges fixed — `func_8019C5D4`'s `0x5D8` override
    swallowed the alternate entry `func_8019C69C` (menu `0x18`), and
    `func_ovlC_8023BDA8`'s `0x54` override merged a malloc-tail head with a
    shared-epilogue shim (the `0x0D` list-unlink crash). Splitting the latter
    exposed a latent N64Recomp bug (a body ending in `jal` with fall-through
    dropped its `after_N` label); fixed in `recompilation.cpp` and the patch
    regenerated. The natural tap path is now title → `0x02` → `0x0D` → `0x02`
    → `0x0D`, 0 stub calls, stopping at a NULL into `func_800988A0`. See
    `docs/HANDOFF-2026-09-14-session39.md`.
  - ✅ **0x0D re-entry characterised, and scripted-run tooling (sessions 40/41)**:
    the `0x0D` → `0x02` → `0x0D` re-entry and the ~28.7 s clean first visit are
    the game's own scripted loop (`0x02`'s enter hardcodes next = `0x0D`,
    `0x0D`'s leave – `func_80178B7C` – hardcodes next = `0x02`; the per-frame
    update picks `0x02` when `func_801C8884() == 2`). No keyboard input changes
    the visit length: `0x0D` is a fixed cutscene, not an interactive screen.
    Added `OGRE_TAP_MAX` (stop tapping after tap n) and `OGRE_TAP_BUTTON`
    (a per-press button schedule: press Start through the title, then A/B/Z/D-pad
    without a human). Re-characterised the re-entry crash: `func_ovlC_802399AC`
    is **not** a standalone function — it is the fall-through continuation of
    `func_ovlC_80239874` (verified at instruction level in `build/bankC.elf`),
    whose prologue is what stores the `a2` buffer at `sp+0x1EC`. The
    `jal 0x802399AC` at `0x8022D218` (`func_ovlC_8022D1CC`, only taken when the
    mode byte `D_8018FC39 == 2`) therefore enters mid-function with a 24-byte
    frame, so that slot is never written and `func_800988A0` gets `a1 = NULL`.
    Forcing the mode byte to 0 moves the crash (N64 `-8` in
    `func_ovlC_8023C894`), so the re-entry lacks more than that one branch.
    Two genuine mis-binding bugs were found and fixed on the way: the
    `func_801AFC2C` size override ran 4 bytes past the start of `func_801B00D0`
    (so the recompiler redirected every `jal 0x801B00D0` into it), and the
    `jal 0x801B00D0` sites are now dispatched to bank unit C's real
    `func_ovlC_801B00D0`. See `docs/HANDOFF-2026-09-14-session40.md` and
    `docs/HANDOFF-2026-09-14-session41.md`.
  - ✅ **`D_8018F1C0`'s writer found, and the re-entry's selector-2 branch is
    game data (session 42)**: `D_8018F1C0` is written by the scene-script VM
    `func_80170974` (`sh` to `0($s5)` at `0x80170ADC`, `$s5 = &D_8018F1C0`);
    session 41's audit missed it because every store goes through `$s5`, not
    through the symbol. Probes show the VM writing `1` at script pc 16 and `2`
    at pc 32, and the `0x0D` path reading exactly those
    (`func_80226FA8 F1C0=0x0001` / `0x0002`); `func_80178568`'s own
    `D_8018FC39` store is **not** taken. The selector comes from
    `func_8022683C(-5, n)`: `func_80227E64(n)` reads the decompressed descriptor
    table (asset `0x19A8804` = ROM `0x1F3CA54`) and returns the step's command —
    `n=1 → -7`, `n=2 → -3`. Step 2's `-3` → jtbl index 7 → handler `0x80226E30`
    → `func_80227700(2)` legitimately selects the crashing branch, while step
    1's `-7` → handler `0x80226AC4` → selector 0. So the `jal 0x802399AC` in
    `func_ovlC_8022D1CC` is a path the game's own data asks for. At that call
    the port has `sp=0x800C22A8`, `s0=0`, `*(sp+0x1EC)=0`, while visit 1's
    per-frame `func_ovlC_80239874` frames run at `sp=0x800C1BF0` (their `a2`
    slot lands at `0x800C1DDC`, `0x6B8` below the slot path B reads). Both
    scene-setup callbacks (`func_ovlC_80225A3C` sel 0, `func_ovlC_80226110`
    sel 2) run at the *same* depth, so that read address is fixed by the call
    chain and never written. Also established: the attract loop is
    `title ↔ {story(0x0B), unit-info(0x0C)}` and never enters `0x02`/`0x0D`
    (170 s tap-free run, exit 0), so `0x02`/`0x0D` is the New Game intro; the
    title's entry 3 leads to scene `0x17`, which is blocked *only* by two
    uncompiled bank records (17 `rom=0x069920 ram=0x80197B90 size=0x4D60`,
    18 `rom=0x1BA020 ram=0x80220F60 size=0x92B0`) — the concrete next step.
    Also corrected the scene model: scene ids dispatch through the accessor
    table `D_800AF028` hardcoded in `func_80075BC0`, and the descriptor's
    `+0x00`/`+0x04`/`+0x08`/`+0x0C`/`+0x10` are **enter / update / update-hook /
    leave / record-mask** (previous sessions had this half-wrong). New Game's
    `0x0D` enter `func_80178568` has **two modes** chosen by `D_8018F1C0`:
    `0`/bit15 = movie mode (DMAs records 10a/10b, installs the cutscene vtable
    `&D_801E5AC0`), non-zero = command mode (DMAs 14a/14b, runs the scripted
    command that sets `D_8018FC39`). Both are walls today: command mode dies at
    step 2 on the frame-less `jal 0x802399AC`, movie mode dies in
    `func_801AFC2C(0)` on session 38's decompressor wall because
    `0x8019F794 == 0` (and that flag is 0 even on the natural `title → Start`
    path). See `docs/HANDOFF-2026-09-14-session42.md`.
  - ✅ **Scene `0x17` is the Tutorial, and the last two bank records are
    compiled (session 43)**: records 17 (`rom=0x069920`) and 18
    (`rom=0x1BA020`) are now **bank unit B** (they are RAM-disjoint from each
    other, so one new unit holds both; `BANK_UNITS := A B C D E`). Scene `0x17`
    loads 21 + 125 functions with zero stubs and renders Deneb's tutorial
    dialogue (`docs/proofs/native-tutorial-dialogue.png`) — the title's
    **Tutorial** entry, verified by driving the menu (`Down` + `Start` →
    `[scene] id=0x0017`). The title menu itself is captured
    (`docs/proofs/native-title-menu.png`): `New Game` (cursor) / `Tutorial` /
    `Stereo` — so `func_80177A58` state 3 → `0x17` = Tutorial and state 2 →
    `0x12` = the save-only **Load Game** entry. Also corrected a load-bearing
    address in sessions 38/39/42: the movie-path word is **`0x80197794`**, not
    `0x8019F794` (`lui $v0,0x8019` + `lw $v0,0x7794($v0)` at `0x801B80B4/B8`).
    And measured the `D_8018F1C0` writer with a probe over all four store
    sites: only `0x80170ADC` fires — it is the script VM's **opcode 0x10**
    (`jtbl_80190758[15] = 0x80170AC0`), which sets `F1C0 = var[0]` **and**
    `F1C2 = 0x8002` at pc 16 (`var[0]=1`) and pc 32 (`var[0]=2`), each before
    the `0x02`/`0x0D` visit it causes; the stores that can write 0 never
    execute. **And the first `0x0D` visit is the New Game movie, which the port
    already renders**: with `F1C0 = 1` (session 42's "command mode" — a
    misnomer, it is the cutscene engine) a natural visit 1 runs its full 28.7 s
    (`0x0D` t=11063 ms → `0x02` t=39780 ms) and draws a sepia courtyard
    cutscene with the subtitle *"I promise I'll make you proud."*
    (`docs/proofs/native-newgame-cutscene.png`). So `var[0] = 1` is correct for
    step 1 and the movie-mode branch is not on the New Game path. The scene the
    developer identified as coming *after* the movie is the cathedral dialogue
    `Archbishop Odiron` / *"He who has learned the way of the sword and god's
    teachings,"* — i.e. **step 2**, which is exactly where the port dies
    (`func_ovlC_8022D1CC` path B → `jal 0x802399AC`). That wall is the next
    session's single goal. Also: dialogue/subtitle text is LZ-compressed
    (`tools/ogrelz.py` decodes `func_8007A110`'s format).
    See `docs/HANDOFF-2026-09-15-session43.md`.
  - 🚧 **Step 2 unblocked by restoring the N64's low-RDRAM (KUSEG) alias
    (session 44)**: the New Game **step table** is decoded (`func_80227E64(n)`
    → table asset `0x19A8804` at ROM `0x1F3CA54`, entry `n` = the step asset,
    whose LZ block starts at `rom+4`; the command is the low byte of the
    descriptor's last word). Steps 1..19: cmd `-7` (step 1, the movie), then
    `-3`/`-10`/`-4`; **every step ≥ 2 selects selector 2**, so
    `func_ovlC_8022D1CC`'s path B is normal code. Path B's `jal 0x802399AC`
    targets the **tail of the function whose prologue is `0x80239874`** (no
    `jr $ra` between them, raw ROM verified); the tail's last call is
    `func_800988A0(a0=sp+0x190, a1=*(sp+0x1EC))`, which *writes* to `a1` — the
    slot the prologue fills from its `a2`. The port reached it with
    `*(sp+0x1EC)=0`, and behind it the descriptor interpreter
    (`func_80227030` → `func_ovlC_802282D8`, first opcode `0x80000006`) called
    the display-list emitter `func_ovlC_8023C894` with `a3 = 0` — two stores to
    guest `0` and `8`. Tracing `a3` (queue slot `0x800E7A30` →
    `func_80227FF8`'s relocated-tag leak `0x08880000` → `0xFFFFFFFF` → `0`
    from `func_802329D0`) shows every value comes from *game* code, so retail
    hits the same stores; retail runs the opening, so the low window must be
    writable there. The R4300i's KUSEG is TLB-mapped and the boot ROM maps the
    low RDRAM window into it; the port's `recomp_mem_addr` computed
    `a - 0x80000000` and faulted ~2 GiB below the buffer. **Fix:** return
    `a & 0x003FFFFF` for `a < 0x80000000` (recorded in
    `n64modernruntime-n64recomp.patch`). Effect: the step-2 enter completes —
    the null build runs the scene 110 s with no crash (it died in one frame
    before), the movie still renders, Tutorial/attract unchanged. **Next wall:**
    the RT64 build dies in `do_send` (SIGBUS, guest `0xFE6E2C89`): the guest's PI
    state (`D_800AA400`/`D_800AA408`, never initialised because
    `osCreatePiManager` is stubbed since session 6) holds garbage at the step-2
    enter, and `func_800998C0` feeds it to `osSendMesg` from the asset-load chain
    (`func_ovlC_802282D8 → func_ovlC_8023BF50 → func_8009DBB8 → func_80089F80 →
    func_8008BC40`); the null build does not hit it. Suspect a ROM DMA landing in
    the wrong place (`func_8008BC40_recomp`).
    `tools/ogrelz.py` gained `--asset` and the corrected block-start rule.
    See `docs/HANDOFF-2026-09-15-session44.md`.
  - ✅ **Step 2 renders: the wall was a missing streamed module, not the PI
    manager (session 45)**: the game streams a **second** record-14 arena module
    (`bankRec14c`, ROM `0x2A8CF0` size `0x56A0`) to RAM `0x802395E0` — the RAM
    `bankRec14b` already occupies — for scene `0x0D` steps ≥ 2. The port had only
    rec14b, so N64Recomp bound unit C's 95 calls from records 14/14a into the
    arena as direct calls to rec14b's **body interiors** (`0x80239C24` is a
    mid-body point of rec14b's `func_ovlC_80239874`, no prologue; rec14c has a
    real `addiu sp,sp,-0x60` function there). The step-descriptor interpreter's
    opcode-42 `jal 0x80239C24` therefore ran a prologue-less body with its own
    0x78-byte frame: `sp` leaked `+0x238`, `s3`/`s6` became 0, the interpreter
    walked **guest 0** as its descriptor, read an out-of-bounds table word at
    `base+0x40000` (null build: 0 → harmless; RT64: `0x00010001` → a ~2.5 GB ROM
    DMA that overwrote `D_800AA400`/`D_800AA408` → the session-44 `do_send`
    SIGBUS). **Fix:** rec14b moved out of unit C into **unit F**, rec14c compiled
    as **unit G** (new splat/N64Recomp configs), so unit C's arena calls compile
    as `LOOKUP_FUNC` and the runtime's DMA-driven bank switch picks the resident
    module. Result: the movie visit runs 28.4 s, step 2 renders the **cathedral
    dialogue with Archbishop Odiron** on both renderers
    (`docs/proofs/native-newgame-cathedral.png`), exit 0, 0 stub calls.
    **Correction:** session 44's KUSEG mirror fixed a *symptom* of this
    mis-binding — with the calls dispatched the low-window stores no longer
    happen (RDRAM `0x0..0x40` is zero after the run); the mirror is now
    unexercised on this path and should be A/B'd next session.
    See `docs/HANDOFF-2026-09-15-session45.md`.
  - ❌ **A YUV16 decoder in RT64 was tried and reverted; the background is still
    black (session 49)**: the njpeg macroblock draw does use `fmt=1 siz=2` YUV16
    textures and `TextureDecoder.hlsli`'s `case G_IM_FMT_YUV:` does return
    `float4(0,0,0,1)`, but a decoder written this session turns the frame into a
    **corrupt magenta/green blob**, not the backdrop (A/B in
    `docs/HANDOFF-2026-09-15-session49.md` §2; the shader working tree is back to
    its original state). The mid-session claim that "the backdrop draws" was
    wrong — its capture's bright pixels were the dialogue box and the sprites,
    which are RGBA16 and always rendered. Session 48's "the background renders"
    is also wrong; its buffer-selection patch was never the wall.
    **Next lead:** check GLideN64 — the emulator upstream closed
    [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
    with — against RT64 before writing another decoder, and decide whether the
    game's readback should be one scene-entry later.
  - 🚧 **The game's CPU readback copies a zero framebuffer (session 49)**: the
    readback (`0x80199884`) runs 4×, all in scene `0x02`, and every copy reads
    zero (`probe52`, reverted: `src=0x80000400 fb0=0 fb1=… fb2=0`); the blit's
    source `0x80243E28` is zeros when sampled. This is **upstream of any decode**.
    A sync hook is in place (`ogre_sync_framebuffers()` →
    `State::syncFramebuffers()`, retire the pending workload and write rendered
    framebuffers back); it is a readback-correctness improvement, not a fix.
  - 🚧 **Why the game's own frame index lands on the placeholder (session 48)**:
    `D_800C4BB8` is the VI manager's "displayed buffer" word
    (written by `func_8007307C`, which `func_80089540` — N64 Thread 5 — calls
    from a message object; a `watch.sh --value` conditional watchpoint caught
    it). Session 49 measured that both that index and the njpeg target resolve to
    `0x000400`, so this is not the backdrop wall; the question is still open.
    See `docs/HANDOFF-2026-09-15-session48.md` §4.
  - ✅ **Boot straight to a New Game step (`OGRE_STEP`, session 48; corrected in
    session 53)**: the opening's steps are the scene-script word `D_8018F1C0`, so
    `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 ./build-app/ogrebattle64`
    reaches the cathedral ~2.2 s after boot instead of after the 28.7 s movie.
    **The step is *seeded* on the first frame the selected scene is active, not
    held every frame.** Session 48's per-frame hold overwrote the game's own
    advance: the moment the game wrote the next step the hold put the seeded one
    back, so the opening re-entered the cathedral forever ("the scene is running
    in a loop, restarting the cathedral scene" — developer, session 53). The
    seed is released as soon as the live step moves off it, or after a 1500 ms
    window. This also makes the background fix above reproducible without the
    flaky title-tap timing. See `docs/guides/app-build.md` (`OGRE_STEP`) and
    `docs/scenes.md`.
  - ✅ **The New Game opening advances past the cathedral (session 54)**, and
    session 53's "the cathedral never advances" is corrected: the advance is real
    and reachable. The maintained repro drives the real flow —
    `OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000
    OGRE_TAP_BUTTON="start,a,start,a,…"` — a Start summons the title menu
    (developer, session 54), the cursor starts on New Game, a second Start
    confirms it, and `A` advances the movie and the cathedral dialogue. Measured:
    `0x04 → 0x02(step 1) → 0x0D` (movie, 3.5 s) `→ 0x02(step 2) → 0x0D`
    (cathedral) `→ 0x07` at `t≈17.9 s` (4×). The engine's exit calls
    `func_80178CB0`, which does `D_8018F1C0 = 0` (`func_801723B4`, `0x801723D8`)
    and `D_800C4C26 = D_8018F1C2 = 0x0007` — so **the step word reading 0 at the
    handoff is correct game behaviour**, and the movie-vtable dispatch session 53
    saw belongs to the `OGRE_STEP` shortcut, which skips step 1 and leaves the
    exit on its `otherwise` arm. See `docs/HANDOFF-2026-09-16-session54.md`.
  - ✅ **Scene `0x07` is the name-entry form, and it renders (session 55)** —
    session 54's black screen is fixed and its diagnosis corrected. The scene's
    descriptor is **`D_8018FDAC`** (streamedB ROM `0x65CAC`; `func_80075BC0`'s
    accessor table maps id 7 → `func_8017B600` → it), **not** `D_8018FB98`;
    enter `func_8017B794`, update `func_8017B858`, hook `func_8017B9C8`, mask
    `0x00000002`. The enter chunk-DMAs a **0x8600-byte code module, ROM
    `0x712A0` → RAM `0x8019A7C0`** (67 × `0x200` chunks) and calls it. The
    module's RAM overlaps **record 3** (unit A) and **overlay C** (the main
    ELF), so N64Recomp bound the calls to overlay C's bodies at those addresses
    (of the module's 24 internal `jal` targets, **0** were main-ELF entries) and
    the form drew nothing. The module is now **bank unit H**
    (`config-bankH.yaml`/`.toml`, `BANK_UNITS` gains `H`; code/data split at ROM
    `0x783C0`, where splat also puts it), and the six calls from resident code
    into it are dispatched (`LOOKUP_FUNC`) from `make recomp`'s `--only` list.
    `tools/cross_bank.py`'s `overloaded()` had a bug that hid the range
    (`hi = min(c.hi)` instead of `max(c.hi)`); fixed. Verified: forced
    `OGRE_SCENE=0x07` draws the form (50 frames, 219302 non-black, was 0), and
    the title-driven opening reaches `0x07` at `t≈19.0 s`, draws it, and hands
    off to `0x02`/`0x0D` at `t≈20.7 s`. Proof
    `docs/proofs/native-newgame-name-entry.png`. See
    `docs/HANDOFF-2026-09-16-session55.md`.
  - ✅ **The opening runs through the whole New Game sequence, and the stale
    backdrop is fixed (sessions 56/57, developer-confirmed)**: name form →
    **date of birth** → **personality questions** (steps 3–8) → **scene `0x16`**
    (descriptor `0x8018FC00`, mask `0x400`) at `t≈44.7 s` (4×). The cathedral
    backdrop could come back with a **rectangular stale region** (the previous
    form's content, intermittent). **Session 56** found the artifact baked into
    the game's own display framebuffers and the readback destination
    `0x80243E28` holding a picture of the form, and concluded "render-vs-readback
    timing". **Session 57 corrects that: it is the readback *source selection*,
    not timing.** `tools/njpeg_readback.py` preferred RT64's scratch word `+8`,
    which is only re-set by a YUV-texture-then-colour-image pair and **never
    cleared**, so at the first pass of an assembly it still named the *previous*
    step's framebuffer — by then holding the previous screen (the form). The
    game's own `state[0x64]` (written at `0x80199B98` from the framebuffer table
    `D_800A8204`, indexed at `0x80199AF8`-`0x80199B68` by `D_800C4BB8`; it selects
    the buffer that is *not* displayed) named the correct sub-image at every pass
    measured. The patch now keeps the game's choice whenever `D_800C4BB8` matches
    a table entry and uses the scratch word only as the fallback (the session-48
    case). Measured over 10 runs / 120 stage-3 passes with `OGRE_NJREAD_LOG=1`:
    old rule wrong **13** (always pass 0, always the previous screen), new rule
    wrong **0**. The stale pass was always **pass 0** (destination `0x801AAE90`,
    240x320) — the developer reports the broken on-screen tile as the top-left of
    the 4-image 2x2 backdrop, and pass 0 is the only stale pass, so they are
    almost certainly the same chunk (inferred, not read out of the blit; the
    framebuffer's own chunk order is scrambled — see
    `docs/guides/njpeg-backgrounds.md`). **Live-console A/B** (dump while the dispatcher
    reports scene-`0x0D` step `0x021D8002`): `0x80243E28` is the name-entry form
    under the old rule (5/5) and the cathedral with the fix (5/5) —
    `docs/proofs/native-newgame-backdrop-old-rule.png` / `-backdrop-fixed.png`.
    The ordering hypothesis is also wrong: `sp_complete()` runs
    before `send_dl` (`events.cpp:422`/`:429`) but the game waits for the **DP**
    completion (`:432`, queue `0x800E8BF4`), so a game-thread handshake
    implemented for it never blocked once, even with the njpeg display list
    deliberately delayed 400 ms inside `send_dl`; it was reverted.
    `ogre_sync_framebuffers()` still forces the RDP's pixels back to RDRAM before
    the copy. Proofs: `docs/proofs/native-newgame-readback-stale-source.png` /
    `-correct-source.png`, `docs/proofs/native-newgame-cathedral-post-dob.png`.
    See `docs/HANDOFF-2026-09-16-session57.md` §1.
  - ✅ **Checkpoints: `save`/`load` in the live console (session 58, the
    developer's idea)** — a fast-testing instrument that removes the ~45-60 s
    New Game replay from every experiment. A checkpoint is the whole 8 MiB RDRAM
    image **plus the runtime's overlay state** (which recompiled body is mapped
    at each RAM address; RDRAM alone would restore a later bank's function map
    and run the wrong module's bodies), written/restored by the console commands
    `save [path]` / `load [path]` (`write_checkpoint`/`read_checkpoint` in
    `app/src/sdl_platform.cpp`; `recomp::overlays::get_overlay_state_blob` /
    `restore_overlay_state_blob`). Both wrap the file I/O in
    `ultramodern::checkpoint_pause_begin/end`, which parks every thread executing
    recompiled code at a function-entry boundary — the N64 threads are 1:1
    native, and without the park the image **tears** (first attempts: the
    checkpoint's own checksum did not match its bytes, first `0x300` bytes zero,
    while the game kept submitting RSP tasks during the write). Verified: file
    self-consistency, an in-run rewind that then plays forward (personality
    questions → `0x16`), and a cross-process load. `OGRE_CONSOLE_AT_MS` delays
    the watched-file read so a scripted run can leave the command file in place.
    See `docs/guides/app-build.md` → "Checkpoints" and
    `docs/HANDOFF-2026-09-16-session58.md` §1.
  - ✅ **Scene `0x16`'s module is compiled and the closing movie plays (session
    58)** — session 57's next lead, done. Scene `0x16` (descriptor `0x8018FC00`,
    mask `0x400`) chunk-DMAs ROM `0x244770` (0x7500, 59 × 0x200) → RAM
    `0x801D0860` (record 10's arena). It is now **bank unit I**
    (`config-bankI.yaml`/`.toml`: code `0x244770..0x24B3E0`, data to the module
    end `0x801D7D60`, 43 functions), and `bankRec10a` — the *other bank* of that
    RAM — moved out of unit C into **unit J** (`BANK_UNITS := … I J`), because a
    unit cannot hold overlapping records and unit C's own code calls into that
    range. Result: the scene loads, plays and **advances out of it** (repeated
    `0x02 → 0x0D → 0x16` cycles at t≈58.8/66.0/68.6/71.4 s at 4x, no stub
    calls; `20 streamed-overlay record(s), 1975 function(s) armed`).
    Three build-hygiene traps this exposed are fixed in the `Makefile`:
    `build/bank<U>/{asm,assets}` must be cleared before `splat split` (splat
    never deletes a removed segment's output), `RecompiledFuncs/`/`Bank*Funcs/`
    must be cleared before regenerating (N64Recomp never deletes a previous
    run's file, so a symbol that changed section leaves a conflicting definition
    behind), and a `data` subsegment needs an explicit following `bin` gap.
    `tools/gen_bank_syms.py` now also sees spimdisasm's `.Lovl<U>_<addr>`
    references. See `docs/HANDOFF-2026-09-16-session58.md` §2.
  - ⚠️ **The chapter-card crash, first reading (session 58) — SUPERSEDED by
    session 59** (the crash was the record-14 arena's third bank, mis-bound
    through unit C, not a missing engine init; the "engine word" it names is at
    `0x8022A994`, not `0x8023A994`). Kept for the trail: the
    developer's end-of-sequence crash reproduces deterministically: `SIGBUS` in
    `func_ovlC_8022C270` on N64 thread 4, right after a `0x02 → 0x0D` visit,
    with `D_8018F1C0 = 0x0431` = **step 1073** = the **"Prologue" chapter
    animation** (`docs/scenes.md` row 7). Corrected findings: the step table
    (ROM `0x1F3CA54`, asset `0x19A8804`) is a raw `u32` array of **1693**
    entries, so 1073 is *valid*, not an overrun; its descriptor's low opcode
    (`0x1B` < 31) goes to the interpreter's shared case (`0x80228E84` →
    `func_ovlC_8022C270`), which walks a table based at **`*(0x8023A994)`**.
    **The engine init DOES run** — `probe58` (tagged, reverted) showed
    `func_ovlC_8022D1CC` storing `malloc(0x1CB8)` = `0x8019F490` at `0x8022D1E8`
    once per sequence visit, **except the visit that faults** (t=75235 in the
    measured run). And the arena DMA covers the engine word every visit
    (`bankRec14c` 44 chunks → RAM `0x802395E0..0x8023EBE0`; the word is at
    `+0x13B4`), so the missing init leaves the module's own file data there
    (`0x46000086`/`0x3C01801D`/`0x58933768` — exactly the module images' words at
    that offset) and the handler dereferences it. **Next:** the step **command →
    setup selector** dispatch for command **6** (step 1073's command; the movie's
    are 5/1/1/5/1) — `func_80227700(sel)` picks `sel 0`=`func_ovlC_80225A3C` /
    `sel 2`=`func_ovlC_80226110`, both of which call the init; a mis-dispatch
    there is the session-42-class bug. `docs/HANDOFF-2026-09-16-session58.md`
    §3b/§3d.
  - ✅ **The chapter-card crash is FIXED — it was a missing third bank of the
    record-14 arena (session 59)**, and the opening now plays on past the movie
    into the post-movie story and scene `0x05`. The step's **`-8`** command
    (`func_ovlC_80227E64` maps the descriptor's last-word low byte 6 through the
    table at `0x8022ABE0`) selects the `func_ovlC_8022683C` arm at 0x802269DC,
    which DMAs **ROM `0x286BA0` (0x138F0) → RAM `0x8022ACB0`** — the chapter
    animation's module, which the port had no code for. `bankRec14a` (the *other*
    bank of that RAM, ROM `0x29A490`) was compiled into unit C, so the resident
    interpreter's `jal 0x8022C270`/`0x8022C6E4` and the callback's
    `jal 0x8022E3F0` were bound to rec14a's bodies; with the chapter module
    resident the interpreter ran rec14a's layout, left base/pc = 0 and walked
    guest 0. Fixed with **unit K** (the chapter module) + **unit L**
    (`bankRec14a` moved out of unit C), so those calls compile as `LOOKUP_FUNC` —
    plus `bankRec14a`'s ROM end, which was `0x2A82F0` while the game DMAs
    `0xE860` to `0x2A8CF0` (the truncated 0xA00 held its jump tables; N64Recomp
    aborted on `jtbl_ovlL_80239270`). Verified: the **Prologue card renders**
    (`docs/proofs/native-newgame-prologue-card.png`), the story after it renders
    (`-received-for-duty.png`, `-magnus-old-man.png`), scene `0x05` enters with
    **0 stub calls**, exit 0. Session 58's global addresses are corrected
    (`0x8022A970`/`78`/`94`, **not** `0x8023A9xx`). See
    `docs/HANDOFF-2026-09-16-session59.md`.
  - ✅ **Scene `0x05`'s module is compiled (session 59, unit M)** — its enter
    `func_8017B60C` DMAs ROM `0x79750` (0xDAD0) → RAM `0x8019A7C0`, the *other*
    bank of unit H's scene-`0x07` form module; before this the scene called
    `0x8019A7C0` and hit the streamed stub. Unit H's own extent is corrected from
    ROM `0x798A0` (0x8600, session 55's chunk-rounded figure) to `0x79750`
    (0x84B0, the enter's own `subu`). `app/src/bank_overlays.cpp`'s
    `is_known_module` now also requires the chunk's rom→ram delta, which is what
    had hidden this missing module (a plain ROM-range test matched unit H).
    Scene `0x05` shares scene `0x07`'s update/hook and its enter is a near-twin,
    so it looks like another UI/form screen; it rendered **black** until
    session 60 (next bullet).
  - ✅ **The map scene renders (session 60)** — scene `0x05` is the **world map**
    (developer): terrain, rivers, location labels, the route of dots, unit
    markers and the `Flama` cursor in a stone frame
    (`docs/proofs/native-newgame-map-scene.png`). It was black because **no
    frames were produced**: two calls in the scene update/hook
    (`func_8017B858` @0x8017B8A0 `jal 0x8019AF0C`; `func_8017B9C8` @0x8017BA10
    `jal 0x801A103C`) were compiled as "call the containing body + early
    `return`" (a `jal` into a size-overridden body interior), abandoning the
    caller's frame each time. The frame-pump thread (t4, `func_8008AFE0`) then
    read its saved `$s0`/`$s1` from the wrong stack slots; `$s1` is the
    message-type comparator (1) and became 0, so the type-1 dispatch stopped
    matching and the pump died after two frames — the signature is
    `D_800AEFA4` (frame counter) frozen while `D_800C4BCC` (VI retrace) keeps
    counting, with only 2 display lists submitted. Both targets are in the RAM
    the scene module occupies and are now in `make recomp`'s
    `cross_bank.py dispatch --only` list, which also triggers the tool's
    tail-call repair (call-and-continue); `0x8019AF0C` resolves to unit **M**'s
    state-1 handler (scene `0x05`'s enter sets `0x801977E8 = 1`; scene `0x07`'s
    sets `3` and takes `0x8019B340`, which is why the form already worked).
    Verified: forced `OGRE_SCENE=0x05` 2 → **124 display lists**; the natural
    route submits **2296** and draws the map, with the movie and the Prologue
    card unchanged. The no-`--only` full dispatch also fixes it (89 sites /
    7 extra targets) and is the way to clear the remaining backlog.
    `docs/HANDOFF-2026-09-16-session60.md`; `docs/scenes.md` row 10.
  - ✅ **The map's sprites are FIXED (session 65) — the cause was a linker
    artifact, not the renderer, the asset or the game's constants.** Unit M's
    `.data` subsegment was linked **8 bytes above** its ROM address, so every
    `lui/%lo` data reference in the recompiled module read 8 bytes too high —
    including the map's sprite table base: the ROM's builder
    (`func_ovlM_8019F83C`, ROM `0x7E804`) says `addiu v0,v0,0x6FD8`, the bank ELF
    said `0x6FE0`, and `nm` defined `D_ovlM_801A6FD8` at `0x801A6FE0` (150 of the
    unit's 480 address-named symbols were +8). **Every sprite descriptor was read
    one entry late**, which is exactly the "the two calls name each other's
    entries" of sessions 61–63: with the correct table at RAM `0x801A6FD8`,
    `a2=10` = `(32,32)` = **the party** (the full knight sheet frame), `a2=11` =
    `(16,11)` = **the shadow**, `a2=12` = `(144,23)` = **the date plate**, and
    `a2=6` = `(16,16)` = the cursor. The cause of the shift: `mips-linux-gnu-as`
    pads `.text` to 16 bytes and the module's code/data boundary (ROM `0x85838` =
    RAM `0x801A68A8`) is only 8-byte aligned, so the data subsegment started 8
    bytes high. **Fix (config only): the `asm` subsegment now ends at `0x85840`**
    (the 8 zeros there are the module's own padding and nothing references
    `0x801A68A8`), which makes the assembler's pad real bytes and restores every
    label. Result: the party is a 32x32 knight with its 16x11 shadow, the cursor
    is one 24x23 arrow, and the panel is the full `MONTH/DATE | Sombra | 1` plate
    (`docs/proofs/native-newgame-map-scene.png`, **replaced**; the broken render
    is kept as `docs/proofs/map-sprites-before-offby8.png`). **New guard:**
    `tools/elfcheck.py` / `make elf-rom-check` (ELF-vs-ROM byte comparison +
    address-named-symbol audit) — after the fix **all 13 bank ELFs and the main
    ELF are byte-identical to the ROM**; `make bank-recomp` runs it before
    recompiling. Session 64's renderer-side `line = 8` lead is **withdrawn**: for
    RGBA32 the RDP keeps 2 bytes per texel per TMEM half, so `line = 8` is 32
    texels per row — the sheet's row — and RT64's loader/sampler agree with
    angrylion (`docs/HANDOFF-2026-09-17-session65.md` §1–§4).
    **Developer-confirmed in game (same session, §8):** *"the scene looks
    perfect!"* — party, the 4-dot route, the red pin, the crossed swords, the
    cursor and the full `MONTH/DATE | Sombra | 1` panel all correct; the marker
    and route elements session 64 §8b could not see in forced captures are the
    same one-entry shift. Selecting the next mission **advances out of the map
    into the next scene**; **it crashes when the mission actually starts** — that
    is the next wall (ask the developer for the `[crash]`/`[snap]` block at that
    moment; `tools/scenemap.py transitions` should name the target scene).
  - 🚧 **Checkpoints are process-local, so they cannot be handed between runs
    (session 65).** A `save` + `load` pair **in the same run** at the map works
    (verified on the live map, scene `0x05`, with every N64 thread active). Loading
    the *same bytes* written by an **earlier process** (same binary, same build
    id) dies deterministically right after the "loaded" message: `SIGSEGV` in
    `do_send + 0x54C` on an N64 thread, with a garbage host address. The image
    contains **host pointers** — `OSThread::context` is declared "an actual
    pointer regardless of platform" and is written into guest RDRAM, one per N64
    thread (7 found in `/tmp/map-fixed.bin` at `OSThread::context` = `base+0x20`) — and the scheduler dereferences them after the rewind. So
    `/tmp/map-real.ckpt` and `/tmp/map-fixed.ckpt` (both from the developer's
    process) are **inert in any other run**, and the docs' "one-command replay"
    recipe only works within the run that saved. **Fix (a milestone, and worth it
    — checkpoints are the project's main accelerator):** rebuild the runtime's
    host state from the restored guest state — a per-thread `OSThread` →
    `UltraThreadContext*` registry installed by `osCreateThread` and rebound after
    `read_checkpoint`, plus a reset of the scheduler's blocked-thread host state
    (session 58's "cross-process load" claim is corrected in `docs/DECISIONS.md`
    and `docs/guides/app-build.md` → "Checkpoints").
  - 🚧 **The map's party: the two draws are identified (session 63); the wall is
    a zeroed texture buffer. Nothing landed.** *(Superseded by session 65: the
    entries were not "naming each other" — the whole table was read one entry
    late because unit M's data labels were 8 bytes high; the "zeroed buffer" was
    the shadow's own `+0x1068` region read through the 16x11 rect that was in
    fact the shadow's correct entry.)* The developer corrected sessions
    61/62: the party (Magnus, the knight) is the **second** builder call
    (`func_ovlM_801A2A7C`, ROM `0x81BFC`, `a2=0xA`, rect `(a0-16, a1-24)`) — the
    rect **above** the other — and it needs **`16x24`** (his hair is visible in
    some sprites); the **first** call (ROM `0x81B50`, `a2=0xB`, rect `(a0-8,a1)`)
    is the **shadow** and needs `16x11`. Live table values: slot 12 = `(16,24)`,
    slot 10 = `(16,11)`, slot 11 = `(144,23)` — so the two calls currently name
    each other's entries, which is why the shadow drew as the row of ~18
    ellipses (`144x23` over a `7x10`-texel window) and the knight as a small dark
    blob. **The window mechanism is settled:** the caller's `jal` delay-slot store
    (`$t5 = 0x1C028`) lands on the **same display-list word** the builder writes
    (`dl+0x3C`) and wins on **hardware** as well as in N64Recomp's output — not a
    port artifact, and there is no unidentified second writer (corrects session
    61 §4(a) / session 62 §1). **The wall:** with the rects and per-entry windows
    made consistent, both party sprites are clean, correctly-shaped **dark
    blocks**, and a live probe shows why — the knight's texture pointer
    (`0x8021AC88`, from `state 0x80197B18 -> 0x801F1570` field `+0x04` biased by
    `+0x1068`; the shadow's is `0x80264D00`, `+0x10E0`) points at a buffer whose
    words are **all zero** in the live image, and a zero RGBA32 texture with the
    map's alpha combiner renders as that block. The pointer address is faithful
    to the emitted list; the **content** is what is missing, so the next step is
    to watch that buffer from scene entry and find who fills it (or which field
    names a populated one). The **cursor** (`a2=6`) and the **date panel**
    (`a2=12`) are the same rect-vs-window class.
    `docs/HANDOFF-2026-09-16-session63.md`.

  - 🚧 **The map's party, reframed: the port is faithful, so stop editing its
    display lists (session 64).** *(Superseded by session 65: the four checks
    were right that the LZ decode, the RDRAM contents, the GBI and the *raw ROM*
    constants are faithful — but they do not cover the **bank ELF**, and unit M's
    data labels were 8 bytes high, so the *recompiled code* read the sprite table
    one entry late. §4's `line = 8` lead is also withdrawn: for RGBA32, `line = 8`
    is 32 texels per row. See `docs/HANDOFF-2026-09-17-session65.md`.)*
    *"How would an emulator handle it?"* Four
    independent checks say the port is not the defect: the LZ decoder is exact
    (13/13 assets end **on** their declared payload boundary), the port's
    `state[+0x04]` buffer is **21128/21128 bytes identical** to an offline decode
    of asset `0x01DD210A`, scene `0x05`'s ucode `0x8009F540` hashes (XXH3-64, raw,
    len `0x1390`) to RT64's **`F3DEX2.fifo 2.08`** entry so the GBI choice is
    right, and every constant was re-read out of the raw ROM. **Two session-61/62
    mechanisms are withdrawn:** N64Recomp does *not* run a `jal` delay slot twice
    (the duplicate is dead code after `goto after_N`), and the builder
    `func_ovlM_8019F83C` emits **no** `G_SETTILESIZE` at all — the word at
    `0x8019F990` is the TEXRECT's **s,t**, so there was never a second window
    writer. **New model, ROM-verified:** the knight is *not* an asset — the map's
    enter `malloc(0x18000)`s `state[+0x34]` (`0x8019A9E4`) and **composites** an
    RGB555 LUT over an 8-bit index image into it, a **32-texel-wide** RGBA32 sheet
    of **24 frames** (`0x1000` each = 8 directions x 3 frames, selected by
    `3*state[0x1DC] + f`); `state[+0x04]` is asset `0x01DD210A`, the shadow
    draw's `+0x1068` really is zero, and the only translucent-black art in any map
    asset is at `+0x10AC` of that same asset. `state` is a **heap pointer** — read
    `*(0x80197B18)`, never hardcode `0x801F1570`. **New top lead:** the sheet's
    row stride is 128 B (`line` 16) while the draw's render tile declares
    **`line=8`** — the game's own command, so this is a renderer-side question
    about `G_LOADBLOCK`'s block/tile-line interaction, not a game-data one (call
    1's `line=2` + `masks=3` + 7-texel window is *only* consistent read as
    16-bit, the same question). Next: compare RT64's `setTile`/`loadBlock`/
    sampler with GLideN64's and parallel-rdp's, and/or take a reference-emulator
    RDRAM at the same screen (RetroArch + Mupen64Plus-Next is installed, and
    GLideN64 ships `[OGREBATTLE64] graphics2D\enableTexCoordBounds=1` and
    `hack_Ogre64`). **No code landed.** See `docs/guides/emulator-first.md`;
    `docs/HANDOFF-2026-09-17-session64.md`.

  - 🚧 **Pre-state: the map needs army/save data that a forced entry never builds
    (session 64 addendum).** The developer's hypothesis is partly right and the
    split matters: the party's **map position** (`state[+0x40]`/`[+0x44]`,
    `state[+0x5C]`/`[+0x5E]`, read at the party draw's call site `0x801A18F0`),
    the unit markers, the route of dots, mission availability and the date panel
    are all keyed off the army record — so a forced/fresh entry plausibly lacks
    them. But the **sprite art is not**: the knight's 32-wide, 24-frame sheet is
    `malloc(0x18000)`d and composited *by the map's own enter*, which is why a
    forced scene still draws a correct knight sheet. **A fuller save state will
    not fix the party-sprite defect.** Checkpoints (`save`/`load`) already give
    "a save state at that screen", but **a retail save cannot be loaded yet** —
    **corrected in session 65: OB64 saves to a battery-backed cartridge save**
    (SRAM/FlashRAM/EEPROM), and the port still sets
    `entry.save_type = recomp::SaveType::None`, so the game sees no save device
    at all (the Controller Pak is only the *copy/backup* path, and scene `0x12`
    Load Game is documented as appearing once a *pak* save exists).
    **Route to a real map state:** drive it by hand and dump with the live
    console's number keys (`OGRE_KEY_1='c'`, `_2='save /tmp/map-real.ckpt'`,
    `_3='dump /tmp/map-real.bin'`) — an autoplayer stalls on the name-entry form
    (`0x07`) because **a synthetic tap landing in a text field opens a tooltip
    that blocks the advance** (developer); the recorded working shape is a few
    `start,a` pairs then a long run of `a`. **Result (developer dump, same
    session): the pre-state explanation is REFUTED for the sprite defect** — on a
    real playthrough to the map the shadow's source region
    (`state[+0x04]+0x1068`) is **still all zero**, and the knight sheet is present
    and identical. Only 9 of the state struct's 284 words differ (party position,
    tick counters, the sheet's heap address); the sprite source and the knight's
    direction do not. So the defect stays a **renderer/RDP tile-semantics**
    question, and pre-state only explains the markers/route/date. A whole-image
    diff is heap noise — compare the struct, not the dump. **And pre-state IS
    confirmed for the *mission/marker* layer**: from the real map the crossed
    swords (next mission), the red pins, the route dots and the cursor are all
    present, and all were **absent from forced captures** — so a forced scene is
    not a valid repro for anything keyed off the army record. **The party garble
    itself is the row stride, proven offline:** the sheet's frame 23 rendered at
    the declared 16 texels/row splits into two half-columns, and at the true 32
    texels/row it is a clean knight (`docs/proofs/party-sheet-{declared-stride-line8,correct-stride-line16}.png`).
    The draw declares `line=8` for a 128-byte row (ROM-verified) — so the open
    question is purely renderer-side: what the RDP does with a render tile's
    `line` for a `G_LOADBLOCK`-filled tile (GLideN64 keys textures at their image
    width and never consults it; RT64 honours it). **That is the next A/B.**
    `docs/HANDOFF-2026-09-17-session64.md` §8, §8b.

  - 🚧 **The map's sprites are missing/garbled (session 60, developer spec)** —
    the developer supplied the retail screenshot
    (`docs/proofs/map-reference/retail-map-screen.png`) and the screen's spec:
    the framed world map + the **party sprite**, the unit markers, a **cursor**
    (white arrow + red dot) and the bottom-right **date panel** (`MONTH`/`DATE`
    + e.g. `Sombra 1`); plus the map's **`R` menu** (1 Organize / 2 Hugo Report /
    3 Settings / 4 Save, with the two-slot save window, the overwrite prompt and
    the title's `Load Game` cursor default) — all written into `docs/scenes.md`.
    The port draws the terrain, the frame, the I8 road/label mask and the panel's
    month name, but **not the party/cursor/panel box**: the party is a row of
    ~18 repeated dark ellipses. Measured: the party rect is 144x23 px but its
    render tile is a **7x10-texel** window (`SETTILESIZE lrs=28 lrt=40`) of an
    RGBA32 sprite atlas, so the tile repeats; the atlas is asset `0x01DD210A`
    (decoded by `func_8007A110` from unit **M**'s entry into `0x80219C20`), and
    the port's decode matches the offline one byte for byte — the sampled region
    is genuinely transparent. So it reads as a **sprite descriptor the map never
    finished filling**, and the next step is to find what fills it (and whether
    retail animates the party/panel in, since the port's frame is static).
    `docs/HANDOFF-2026-09-16-session60.md` §7. **Session 61 corrected the cause**
    (§1): the `7x10` window **is** in the game's own list, the sprite table is
    static and byte-identical to ROM, and the builder
    `func_ovlM_8019F83C` is faithful — a rect always equals the entry's `(W,H)`
    it reads. The party draw asks for slot **11** (`0x801A7038` = `144x23`) when
    the knight's frames are slots **12-15** (`16x24`); the developer measured
    retail's marker at *10% of height / 5% of width* = **~16x24 px**, matching
    those slots exactly. The call site is `func_ovlM_801A2A7C`'s first builder
    call, ROM `0x81B50` (`addiu $a2, $zero, 0xB`). A **global** `base+8` A/B made
    the party exactly `16x24` but shifted *every* sprite, so it is not the fix; a
    **targeted** `0xB→0xC` shrank the bar to a compact sprite but is not proven.
    Open: the emitted window stays `7x10` even for a `(16,24)` entry, so the
    writer of that window is still unidentified. `docs/HANDOFF-2026-09-16-session61.md`.
  - ✅ **`OGRE_CONSOLE_ON_SCENE` / `_STEP` / `_CMD` (session 58)** — run one
    console command the moment a chosen scene (and step) is live, e.g.
    `OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_STEP=974
    OGRE_CONSOLE_ON_CMD='save /tmp/ck.ckpt'`. This makes checkpointing
    deterministic against the wall-clock route flakiness (a run whose taps
    started before a late title stayed in the attract loop) and is how the
    step-974 checkpoint was taken.
  - ✅ **After the chapter animation comes the post-movie story (session 59)** —
    the "received for duty with other soldiers" content (`General Godeslas` over
    rows of soldiers) and the `Grey-Haired Old Man` / `Magnus` dialogue render
    (`docs/proofs/native-newgame-received-for-duty.png`, `-magnus-old-man.png`),
    then the sequence reaches scene `0x05` (`docs/scenes.md` row 8/10).
  - ✅ **The mission's intro plays on the *natural* route (session 68) — the wall
    was a fourth bank of record 9's arena.** The suspend save (session 67) enters
    scene `0x03` without the sequence that precedes it, so it never exercised the
    module the map -> story -> mission route loads: **ROM `0x195430`
    (`0x2380`) -> RAM `0x80214FA0`**, a bank of the *same* RAM as units P and Q.
    Unit N's own loader (`func_ovlN_801AE880+0x764`, ROM `0x1036E4`) DMAs it and
    calls its entry `0x80215C38`; with no code for it the port logged
    `[bank] UNKNOWN module` + `[overlays] streamed function stub called @
    0x80215C38`, and the mission's intro hung on a garbled target-fort draw whose
    winning-condition NOTE never advanced. It is now **bank unit R**
    (`config-bankR.yaml`/`.toml`, `symbol_addrs-bankR.txt`), and the natural route
    runs the whole intro — camera pan, winning condition, panic back, losing
    condition, `MISSION START` — with **0 UNKNOWN modules and 0 stub calls**
    (`docs/proofs/native-mission-intro-win-natural.png`,
    `docs/proofs/native-mission-intro-fort-natural.png`). The route is
    `map (0x05) -> 0x02/0x0D story -> scene 0x16 (movie) -> 0x02/0x0D -> 0x03`.
    Also landed: two driving knobs for scripted runs, `OGRE_TAP_SCENE_BUTTON`
    (a scene-keyed tap schedule — wall-clock slots land on the map in one run and
    in the attract loop in the next) and the live console's `press <buttons>
    [polls] [x] [y]` (a synthetic pad press + analog stick), which is what made
    the map drivable (`press right` needs a long hold — the game polls input far
    faster than 60 Hz). See `docs/HANDOFF-2026-09-17-session68.md`.
  - ✅ **The Organize Screen renders (session 69) — scene `0x06`'s 355 KB module
    is now bank unit S.** Picking `Organize Screen` from the mission's `R` menu
    (hold `R`; entry 1 `Dispatch`, 5 `Mission Objective`, 7 `End`) entered scene
    `0x06` and bounced back to `0x03` in **68 ms** — the developer's *"the screen
    just reloads"*. The scene's enter `func_8017B6D0` DMAs **ROM `0x87220`
    (`0x56D60`) → RAM `0x8019A7C0 … 0x801F1520`** (no BSS) and `jal`s the module's
    entry `0x801C19B0`; the generic scene update `func_8017B858` and hook
    `func_8017B9C8` (shared by scenes `0x05`/`0x06`/`0x07`) dispatch on
    `*(0x801977E8)`, which the enter sets to **2**, so they `jal` `0x801C214C` and
    `0x801B7FC0`. All three stubbed out, so the screen was never built and the
    state machine took its done arm. The RAM is a **swappable arena** (record 7's
    `0x101D00`, record 3's `0x0EBBD0`, overlay C `0x1CE040`, units H and M — and
    the main unit has its *own* real function at `0x801B7FC0`), so all three calls
    are `cross_bank.py dispatch --only` targets. Now: 406 functions,
    `bankS.elf` byte-identical to the ROM with 1382 symbols at their addresses,
    **19 units / 29 records / 3094 functions**, developer-confirmed live
    (`docs/proofs/native-organize-screen.png`). See
    `docs/HANDOFF-2026-09-17-session69.md`.
  - ✅ **Test-automation premise settled (session 70): the port's *timeline* is
    wall-clock, but its *state* is reproducible to the byte — so an input
    recording must be anchored on game state, not on frame/poll index.** Measured
    with a temporary probe (scene-armed, poll-counted script: title → Load Game →
    map → three cursor steps left, then a sample of the map's `state` struct):
    **input polls are ~1:1 with VI retraces** (`min=1 max=2 mean=1.01` per
    retrace), which **corrects the "the game polls input far faster than 60 Hz"
    claim of session 68 and the console help** — a cursor step needs a long hold
    because the cursor integrates a small delta *per retrace*, not because the poll
    rate is high. Two runs at `OGRE_SPEED=4` differ **only** in the absolute clock
    (a 16-19 retrace boot offset that every later event keeps); normalised for it
    they are byte-identical at map entry and after the input (same hexdump, `dir`,
    `statehash80`). A third run at `OGRE_SPEED=2` is byte-identical too, except the
    free-running animation tick `state[+0x108]` (373 vs 374) — an assertion hazard,
    not a state divergence. The VI retrace clock is wall-clock
    (`events.cpp:237`), which is *why* absolute indices desynchronise. Consequence:
    an emulator **savestate** is a data oracle (guest addresses are identical —
    the port runs the ROM's instructions; `OSThread::context` host pointers are
    what cannot cross a process, session 65) and not something to load; an emulator
    **input recording** converts into state-anchored, poll-counted segments, and
    the game's own `.srm` remains the build-independent fixture for the map/menu
    moments (`tools/sramsave.py`; title → `0x12` → `0x05` re-verified). The probe
    was reverted; no files changed net. A `DECISIONS.md` is named by AGENTS rule 10
    but is absent from the tree. See `docs/HANDOFF-2026-09-17-session70.md`.
  - ✅ **The tutorial's practice stage runs its own module (session 71) — bank
    units T and U.** The developer's *"open the tutorial … pick an option … we are
    placed in a map and nothing happens, without enemies"* is scene `0x03` entered
    with the mode word `D_80193700 != 0` (descriptor **`0x8018F364`**, mask
    `0x0004038C` — records 2,3,7,8,9 **and 18**, vs the normal mission's
    `0x8018F350`/`0x0000038C`). That path loads a **second bank of record 18's
    arena** — ROM `0x1C32D0` (`0x5D50`) → RAM `0x8022A860` — which the port had no
    code for: four call sites (`0x8022A860`, `0x8022A88C`, `0x8022AA94`,
    `0x8022B36C`, from unit A's record 3 and unit N's record 7) hit the runtime's
    logging stub, 4608 calls in a 40 s run. The loader is record 18's
    `func_ovlB_80221D50` (RAM `0x80221D50`): it indexes a `0x28`-byte segment table
    at RAM `0x80229DDC` and a `0x1C`-byte mode table at `0x80229E88` by
    `D_80193700`, then `func_8009DA50(0x1C32D0, 0x8022A860, 0x5D50)` and `jalr`s
    the mode record's `+0x08` word. The module's data half is the **tutorial
    instruction text** (`Stationing unit will gather information.`, `This will end
    the instruction on Stronghold Command. Proceed?`, `This concludes the tutorial
    on Use Item command. Proceed?`), so the map was inert without it. Now **unit
    T** (`config-bankT.*`, entries forced for the three cross-record targets,
    BSS `0x802305B0..0x802305F0` in `RAM_END`) and **unit U** for the arena's
    mode-2 bank (ROM `0x1C9020`, `0x5020`, entry `0x8022A87C`; no observed route
    loads it, but it shares the RAM so the bank map must be able to evict unit T).
    Verified with `OGRE_SCENE=tutorial` + scene-keyed A taps:
    `0x17 → 0x02 → 0x0D → 0x03 (0x8018F364)`, **0 stub calls**, the tutorial
    instruction box renders and the lessons advance
    (`docs/proofs/native-tutorial-practice-field.png`, `-command.png`); the
    `down down` lesson selects mode 0 (the normal mission) and is also stub-free.
    See `docs/HANDOFF-2026-09-17-session71.md`.
  - ✅ **The mission renders (session 67).** The developer's **suspend save**
    (`assets/save-mission-1.srm`, third SRAM slot) resumes at **scene `0x03`**,
    the mission — descriptor `0x8018F350`, mask `0x38C` (records 2, 3, 7, 8, 9) —
    and it now enters, runs and draws: 3D terrain, cliffs, woods and rivers, the
    2D party sprite with its selection brackets, the `Stronghold` tooltip and the
    unit panel (`docs/proofs/native-mission-scene.png`,
    `docs/proofs/native-mission-unit-panel.png`). Reaching it took four fixes,
    all of the swappable-RAM class: (1) records **7/8/9** were uncompiled →
    **unit N**; (2) unit A's record 3 called `0x801AD6BC` (record 6's RAM) and —
    both records being in unit A — the recompiler bound it to unit A's record-6
    body, but scene `0x03` loads record 3 **without** record 6 → record 6 moved to
    **unit O** so the call dispatches; (3) cross-*record* `jal` targets are
    invisible to the per-record disassembler, and a `LOOKUP_FUNC` onto a
    non-entry is a **silent no-op** (`get_function` has no interior fallback), so
    the entries are declared in `symbol_addrs-bank{N,O,P,Q}.txt` (a subsegment
    split would insert the assembler's 16-byte `.text` padding and break the
    ELF-vs-ROM check); (4) record 9's arena streams **two more banks** the segment
    table does not describe — units **P** (ROM `0x171EC0`) and **Q** (ROM
    `0x165FE0`), both → RAM `0x80214FA0` — found from the port's stub log and a
    light `do_rom_read` probe (the full DMA trace perturbs the run onto the other
    bank). Also fixed: **`tools/cross_bank.py check-banks` was a silent no-op**
    (its YAML parser dropped the `name:` on `- name: bankRecX` lines), so the
    session-45 invariant had been unchecked; it now parses names, uses the scene
    record masks to tell "always loaded together" from a real split (1728 → 23),
    and allowlists the pre-existing unit-C backlog. See
    `docs/HANDOFF-2026-09-17-session67.md`.
  - ✅ **The battery save works end to end (session 66) — the chip is 32 KiB of
    SRAM, and the runtime now owns it.** Session 65's correction stands (the save
    is a battery, not a Controller Pak; the pak strings are the copy/backup
    feature) and is now resolved: `entry.save_type = recomp::SaveType::Sram`
    (`app/src/main.cpp`). Evidence for SRAM: mupen64plus's database
    (`SaveType=SRAM` for this ROM's CRC) and the game's own code — `func_8008A040`
    builds the save `OSPiHandle` with `baseAddress 0xA8000000` (physical
    `0x08000000`), the boot accessors `func_80074CF0..` read the whole 32 KiB in
    256-byte DMAs at offsets `0..0x7F00`, and `func_80074BF0` → `func_80074C58`
    writes it back the same way with direction 1. That path is the game's own
    non-bridged DMA (`func_8008BC40` → the `D_800AA408` queue), so
    `librecomp/src/pi.cpp`'s inline handler now picks the device from the
    `OSPiHandle` in each `OSIoMesg` (`pi_perform_dma`) instead of treating every
    transfer as ROM. Verified: no file → the game formats the blank battery and
    the port writes a real 32768-byte image (`QuestOG3` signature); restart → the
    game reloads it and does not rewrite it; a garbled image → the game repairs
    and rewrites it; a converted parallel-n64 save with progress → the game
    accepts it, and with it the **title's `Load Game` entry appears and its cursor
    defaults to it** (same taps with no save enter New Game): `0x04 → 0x12 →
    0x05`, dev-confirmed loaded. **New tool `tools/sramsave.py`** imports/exports
    emulator wrappings of the same 32 KiB (parallel-n64's container has the SRAM
    at `0x20800`, 32-bit byteswapped). New knob `OGRE_PREF_DIR` relocates the
    config/save dir. See `docs/guides/app-build.md` → "Saves" and
    `docs/HANDOFF-2026-09-17-session66.md`.
  - ⚠️ **Correction (developer, session 65): the game's own save is a
    *battery-backed cartridge save* (SRAM/FlashRAM/EEPROM), not a Controller Pak.**
    The pak strings are the *copy/backup* feature (`Data loaded to Game Pak.`).
    **Resolved in session 66** — the chip is SRAM; see the entry above.
  - 🚧 **The Controller Pak device is implemented (session 65) — this is the
    *copy/backup* path, not the save.** The missing link for it is the *pak menu*,
    which only unit H's scene can reach. `pak.cpp` in the
    (gitignored) runtime is no longer upstream's stub: it is a real device — a
    flat 32 KiB image in 32-byte blocks, block addresses `>= 1024` ignored (the
    bank/enable register, which is why libultra's own bank probe reports
    `banks == 1`), blocks 1..6 write-protected unless `force == 1`
    (`PFS_LABEL_AREA = 7`, `PFS_FORCE = 1`, read out of this ROM's own
    `__osContRamWrite`), a fresh image formatted like mupen64plus
    `format_mempak`, and the image persisted to
    `<config>/saves/<game id>.mpk` (interchangeable with emulator `.mpk` dumps).
    **Only three functions needed bridging**, because the game's PFS is libultra
    the ROM already contains and `make recomp` already compiles: the SI-touching
    `__osContRamRead` `0x80097BD0`, `__osContRamWrite` `0x80097DC0`,
    `__osPfsGetStatus` `0x80096EC0` (`symbol_addrs.txt` + N64Recomp's
    `reimplemented_funcs`). `osContInit` now reports a pak in controller 1
    (`Pak::ControllerPak`). **The gate:** the pak API is called from **unit H
    only** (the form/UI module) — `func_ovlH_8019C69C` and the three screens
    `0x8019DBA4`/`0x8019DC88`/`0x8019DEB4` — and the pak menu is *scene `0x07`'s
    descriptor callback `+0x18`* (`0x8018FDAC` → `func_8017BB28` → `jal
    0x8019C69C`; `+0x14` is the form). The map's module never calls the pak API,
    and a **forced** map entry's Save (R → right×3 → A → A → Yes) draws the map's
    slot window and the overwrite prompt and then stops: no device call (lldb
    breakpoints never hit), no DMA (`OGRE_DMA_TRACE_FULL=1`: unit H's `0x712A0`
    is never streamed), no scene change (`c`: `pending=0x0005`) and no pak-manager
    call. So a **natural** run to the map is required to exercise the device (a
    forced entry has no army/save state — the same caveat as the mission markers).
    Next: drive `R → Save → Yes` on a natural map and read the log
    (`[pak] formatted/loaded …`) and the on-screen message; drop a real 32 KiB
    `.mpk` at the path above for the Load half. See
    `docs/HANDOFF-2026-09-17-session65.md` §8–§9 and
    `docs/HANDOFF-2026-09-16-session58.md` §3c.
  - ⬜ **The save-system recon (session 58)**, kept for the entry-point map — but
    note its premise is **corrected in session 65**: the *save* is a battery-backed
    cartridge save (SRAM/FlashRAM/EEPROM); what follows describes the
    **Controller Pak copy/backup** path (`osPfs*`), not the save. 105
    `jal`s from the main segment into the PFS cluster
    (`0x8009616C`..`0x80097DC0`); the whole menu is ROM text
    (`0x790EC..0x7967C` — `Controller Pak Menu`, `Save`/`Load`/`Erase`,
    `Insert Controller Pak.`, `1 note 25 pages to save.`, `Data saved to
    Controller Pak.`, `Game Data 1`/`2`) which **exists in unit H's ROM half
    only** (unit M has just `No Data`), because the menu is unit H's code.
    See `docs/HANDOFF-2026-09-16-session58.md` §3c and `docs/scenes.md` row 9.
  - ✅ **Scene `0x16`'s content is confirmed** (session 58): the closing movie's
    five shots all render, in order, and match the developer's retail
    screenshots — `ATLUS USA / presents` → `Developed & licensed by Quest /
    Nintendo` → `Ogre Battle Saga / Episode VI` → `Person of Lordly Caliber`
    over the Lodis flower → a travel montage (campfire, the party on a cliff at
    sunset, the parchment world map with the red route and dagger). Proofs:
    `docs/proofs/native-newgame-movie-{atlus,quest,episode-vi,lodis,campfire,sunset,map}.png`;
    the shot list is recorded in `docs/scenes.md` row 6. Open (uninvestigated,
    and it may be the game's own letterbox rather than a defect): a 1-pixel
    vertical line and grey bands in some transition frames.
  - ✅ **Live debug console (session 56, developer's suggestion)**: the running
    game can now be queried on demand instead of only at a bounded run's exit —
    a watched command file (`OGRE_CONSOLE_FILE`, default
    `/tmp/ogre-console.txt`: command lines run and the file is removed) or the
    number keys `1`..`9` (`OGRE_KEY_<n>`). Commands: `r`/`rh`/`rb`/`rk`, `d`,
    `f`, `fb`, `s`, `k` (checksum for A/B), `w`, `c` (scene/descriptor/mask/step)
    and `dump` (the whole 8 MiB **at that instant** — the fix for "the exit dump
    is always too late"). It runs on the main thread, in
    `app/src/sdl_platform.cpp`; documented in `docs/guides/app-build.md` and
    `AGENTS.md` §4.
  - 🚧 **Why the game's own frame index lands on the placeholder is open
    (session 48)**: `D_800C4BB8` is the VI manager's "displayed buffer" word
    (written by `func_8007307C`, which `func_80089540` — N64 Thread 5 — calls
    from a message object; a `watch.sh --value` conditional watchpoint caught
    it). The fix uses the buffer the draw landed in rather than answering
    whether retail reaches the same mismatched state. See
    `docs/HANDOFF-2026-09-15-session48.md` §4.
  - ✅ **The cathedral scene's background renders correctly (sessions 46/47; 50
    implemented RT64's YUV16 decode; 51 implemented the missing S2DEX2 geometry;
    **52 fixed the YUV -> RGB conversion, and the backdrop is the cathedral**)**.
    `docs/proofs/native-newgame-cathedral.png` and
    `-cathedral-background.png` are the corrected captures; the pipeline below
    still describes how it gets there, and the "still open" note at the end is
    superseded:
    the step-2 display list's first `G_TRI2`
    quads are a full-screen 320x240 RGBA16 blit from guest `0x80243E28`, and that
    buffer was uniform (`0x0843` in RT64, `0` in the null build). The image is an
    **N64 JPEG** (`'HU'` container holding `'HUFF'` + numMB, asset `0x00183352` =
    ROM `0x7175A2`), and the pipeline is: CPU Huffman decode `func_8008B250` →
    **four `M_NJPEGTASK` (type 4) RSP tasks** (`ucode=0x8009ED80`/`0x7C0`, boot
    `0x8009ECB0`/`0xD0`, tables `0x800AC050`/`0xF0`, `data_size = mbs`,
    `yield_data_size = scale`) decoding **in place to 16-bit YUV** → a *second*
    gfx ucode `0x800A5110` drawing one 16x16 YUV16 texture per macroblock into
    `G_SETCIMG` → a CPU framebuffer readback → assembly of a `'B5'`-headed image
    at `0x80243E10` whose pixels are `0x80243E28`. **Session 47 fixed the
    decoder**: RSPRecomp's `text_address` is a *label base* and must be the RSP
    IMEM DMA address `0x1080` (not `0x8009ED80`, whose low 13 bits rotated every
    `j` target by `0x300`); each task now writes all `mbs` blocks, the scene no
    longer stalls, and the decoder is **on by default** (`OGRE_NJPEG=0` forces the
    stub). **Session 50 implemented RT64's YUV16 decode** (its `G_IM_FMT_YUV` arm
    returned black) and bounded the render wait on the RSP worker
    (`Application::waitForGameFramebuffers`, `OGRE_NJ_WAIT_MS`). **Session 51
    found why nothing was ever drawn and fixed it:** the `0x800A5110` ucode is
    **`S2DEX 2.08`** (`GBIUCode::S2DEX2`; the ucode text and data hashes match
    RT64's database exactly), so its per-macroblock `0xDC`/`0xDA` commands are
    **`G_OBJ_MOVEMEM` (the sub matrix) and `G_OBJ_RECTANGLE_R` (the draw)** — not
    F3DEX2 `G_MOVEMEM`/`G_MTX`, which is why the F3DEX2-named analyzer reported
    "no geometry". RT64's `GBI_S2DEX2` mapped neither command; both are
    implemented now, and a live trace shows **300 object rectangles tiling a
    320x240 framebuffer exactly** (one per njpeg macroblock). The YUV16 TMEM
    layout was redone too: a YUV `LOADTILE` de-interleaves into TMEM's upper
    (luma, one byte per texel) and lower (U/V pairs per two texels) halves, and
    the sampler follows the RDP's model (parallel-rdp `sample_texel_yuv16`) —
    session 50's sampler read luma from a half the load never wrote, so it could
    never have worked. The source word is `[U, Y(even), V, Y(odd)]`
    (mupen64plus-rsp-hle `GetUYVY`, verified against a live macroblock dump),
    converted with the matrix the game programs via `G_SETCONVERT`
    (`k0=175 k1=469 k2=423 k3=222`). **Session 52 found why the colours were
    wrong**: RT64 stored the raw unsigned 9-bit `k0..k3` fields and used them
    directly, while the hardware sign-extends each field and scales it
    `2*K+1` (GLideN64 `gDPSetConvert`, parallel-rdp `set_convert`), and the
    green channel paired `K1` with V instead of U. With both fixed the
    background is the cathedral (red carpet, stone walls, statues) and the
    assembled `'B5'` image at `0x80243E28` renders as the full scene.
    See `docs/HANDOFF-2026-09-15-session52.md`, `-session51.md` (§7 for the
    checks that localized it), and `-session50.md` / `-session47.md` /
    `-session46.md` for the decoder fix and the readings of the display list
    that session 51 §1/§6 corrects.
  - ✅ **The publisher screens now render correctly (session 32)**: the
    "Licensed by Nintendo", ATLUS and QUEST stills were drawn as 640x480
    (they are the game's hi-res mode) but scanned out with the runtime's dummy
    320x240 VI geometry, so only the top-left quarter of the RDP framebuffer
    reached the screen. The cause was a **mis-named libultra symbol**: the
    function at `0x80095820` that `symbol_addrs.txt` called `osViSetMode` is
    really `__osViSwapContext`, and the *real* `osViSetMode` (which stores the
    game's `OSViMode` pointer) is `func_800955C0` and was never named — so the
    runtime never learned the game's VI mode and stayed on its dummy mode.
    `func_800955C0` is now `osViSetMode`, `0x80095820` is `__osViSwapContext`
    (a no-op; the runtime's VI thread does the swap), and the game's
    640x480 `D_800AB9B0` mode now reaches RT64. Proofs:
    `docs/proofs/native-licensed-screen.png`,
    `docs/proofs/native-atlus-screen.png`,
    `docs/proofs/native-quest-screen.png`. See
    `docs/HANDOFF-2026-09-12-session32.md`.
  - ✅ **The mode-switch noise is gone (session 32)**: the 320x240 → 640x480
    switch showed ~0.2-0.5s of colour static before each still, because the VI
    went hi-res while the new framebuffer was only partly drawn and the game's
    blank (`osViBlack`, `func_80095B30`) was not bridged. With
    `osViBlack = 0x80095B30` the runtime honours the blank (`update_vi` sets
    `hStart = 0`, so RT64 clears instead of scanning out stale RDRAM). `osViFade`
    (`func_80095780`) is still unemulated, so fades snap. See
    `docs/HANDOFF-2026-09-12-session32.md` §1d.
  - ✅ **The late crash is fixed — streamed-overlay banks swap at the DMA
    (session 33)**: session 32 localised the ~95s abort to the game DMA-ing a
    different overlay bank over overlay C. The **streamed-segment table** (19
    records of 10 words at ROM `0x387C0`) and the **bank descriptor lists** (11
    banks at ROM `0x38AB8`, pointer array at `0x38AFC`) are now decoded, and the
    records the game swaps in at t≈95s are entries 2, 3 and 6. Because their
    RAM ranges overlap overlay C's, they are recompiled in a **separate unit**
    (`config-bankA.yaml` → `build/bankA.elf` → `config-bankA.toml` →
    `BankAFuncs/`) linked at their true RAM addresses with an `ovlA_` symbol
    prefix; `app/src/bank_overlays.cpp` registers a record's functions when the
    game DMA's it and drops whatever bank occupied that RAM. The runtime gained
    `notify_rom_read` (called from `recomp::do_rom_read`), `unload_overlapping_overlays`
    and `load_function_bank`. Verified: the 170s unattended run is now **exit 0,
    4850 display lists at 168s** (was exit 134 at display list 2770 / 94.8s);
    proof `docs/proofs/native-post-bankswap-present2805.png` (a rendered night
    castle scene at present 2805, after the swap). See
    `docs/HANDOFF-2026-09-12-session33.md`.
  - ✅ **The next attract scene's bank is identified (session 33)**: the attract
    loop is title → "lore" story movie → title → a variant screen; the second
    transition (t≈360.7s) runs scene `0x0C`, whose descriptor at `0x8018FB58`
    carries **record mask `0x3C00` = records 10, 11, 12, 13**. Uncompiled, its
    entry returned through the streamed stub and the scene bounced back to the
    title. Those four records are now **bank unit C** (`config-bankC.yaml`,
    `BankCFuncs/`, 848 functions). Units are partitioned by *RAM*, not by game
    bank: records whose RAM ranges overlap cannot share an ELF.
  - ✅ **Overlays stream more code into a per-record arena (session 33)**: a
    segment-table record is only the resident part. The record's `ram_end` word
    (`+0x04`) covers an arena the game fills on demand from code modules in the
    ROM gap before the next record. Record 10's arena modules
    (`bankRec10a` ROM `0x213AE0`→RAM `0x801D0860`, `bankRec10b` ROM
    `0x23B1F0`→RAM `0x801E6FD0`) hold the four functions scene `0x0C` called;
    they are compiled into unit C (147 + 90 functions) and registered by the
    same DMA hook. `tools/gen_bank_syms.py` defines the `D_ovlC_*` data labels
    splat leaves undefined in those `asm` sections.
  - ✅ **The unit-info screen renders (session 33)**: with records 10–13 and
    their arena modules armed (9 records / 1289 functions) scene `0x0C` has
    **zero stub calls** and draws the unit stat screens
    (`docs/proofs/native-unit-info-dragon-tamer.png`,
    `docs/proofs/native-unit-info-griffin.png`), instead of bouncing to the
    title. New debug knobs: `OGRE_SPEED=<n>` (scale the emulated clock),
    `OGRE_FORCE_SCENE=<hex>` (switch to a scene without waiting out the attract
    loop), `OGRE_CAPTURE_EVERY=<n>` (frame-sampling captures).
  - ⬜ **The title menu (New Game / Tutorial) still crashes on a cross-bank
    *direct* call (session 33)**: pressing Start reaches scene `0x18`, which
    asks for **record 1** (`mask 0x2`), and scene `0x02` (New Game / Tutorial)
    asks for **records 0 and 14** (`mask 0x4001`). All three are now compiled
    (record 1 = unit D, record 0 = unit E, record 14 in unit C; the app arms 12
    records / 1398 functions), but the crash persists: N64Recomp compiles a
    `jal` to a known function as a **direct** call, so overlay B's calls into a
    swappable bank range run overlay C's compiled body while a different bank is
    resident (lldb: `func_801989AC + 1062`; 58 such call edges, 53 targets).
    Fix: route calls that cross into a bank-swappable range through
    `get_function` (post-step over `RecompiledFuncs/*.c`, or move overlay C out
    of the main ELF), and seed each bank unit with the cross-bank targets as
    function entries. The attract loop (lore + unit-info) is unaffected.
  - ⬜ **Remaining uncompiled streamed records**: 4, 5, 16, 17, 18, plus each
    record's arena modules (overlay C/record 15 and units A/C/D/E are done).
    Record 17 loads at `0x80197B90` and needs a unit of its own.
  - ⬜ **19 more splat split symbols in overlay C (session 30)**: the same class
    of bug (a symbol cut out of the middle of a logical function makes the
    compiled function return without unwinding its frame, clobbering the
    caller's `$sp`/s-registers). All are now covered by `function_sizes`
    overrides, and since session 31 N64Recomp redirects calls to those split
    symbols back into the containing body, so the workaround is enforced
    instead of relying on the override alone.
  - ✅ **N64Recomp cuts a function that falls through into a discovered shared
    epilogue (session 30's wall) — fixed in session 31**: see the session-31
    entry above and `docs/HANDOFF-2026-09-12-session31.md` §2.
  - ⬜ **The native port needs `OGRE_NO_AUDIO=1` on hosts where
    `SDL_OpenAudioDevice` blocks (session 30, environment)**: the runtime calls
    it from the game start thread's `preinit`, so a blocking open wedges the
    whole boot (RDRAM stays all zero, no display lists). Gate added in
    `app/src/sdl_platform.cpp`.
  - ✅ **The native window renders (session 27)**: the session-26 "window is
    black" conclusion was a measurement artifact. The display is locked, so
    `screencapture -x` returns wallpaper and `screencapture -l<id>` returns the
    last frame the window server committed (a cycling clear colour stays one
    colour across seconds of captures). The new ground truth is
    `OGRE_CAPTURE_PRESENT=<path>`: RT64 GPU-reads back the exact swap-chain
    texture it presents (texture -> buffer readback added to plume's Metal
    backend; `rt64-plume-ob64.patch`). With that, the synthetic-frame probe
    shows the seven bars end to end in **both** paths - the RDRAM upload path
    and the real RDP display-list path - and the result is
    `docs/proofs/native-synth-frame-rdp.png`. Four display-list encoding bugs
    had to be fixed first: two-word commands were emitted as two `emit()` calls
    (null scissor, empty fill rect), `G_FILLRECT` had its operand halves
    swapped, coordinates were not 10.2 fixed point, and `G_RDPSETOTHERMODE` was
    pre-shifted so the cycle type was never `G_CYC_FILL`. Two presenter
    behaviours were also needed: RT64 only presents when the VI or its RDRAM
    copy changes, so a stalled boot freezes the window on an old frame
    (`OGRE_PRESENT_ALWAYS=1`); and its framebuffer manager does not know an
    RDP-only colour image, so the presenter uploaded empty RDRAM instead of the
    drawn render target (`OGRE_PRESENT_FBTARGET=1`). Both are turned on
    automatically by `OGRE_SYNTH_FRAME`. See
    `docs/HANDOFF-2026-09-12-session27.md`.
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

