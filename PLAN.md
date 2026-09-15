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
  - 🚧 **The cathedral scene's background is missing: the decode is fixed, the
    readback is not (sessions 46-47)**: the step-2 display list's first `G_TRI2`
    quads are a full-screen 320x240 RGBA16 blit from guest `0x80243E28`, and that
    buffer is uniform (`0x0843` in RT64, `0` in the null build). The image is an
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
    stub). The background is still uniform because the readback source
    (a framebuffer) is: the open question is whether RT64 renders the `0x800A5110`
    YUV draw and whether it can write that rendered framebuffer back to RDRAM for
    a **CPU** read (`OGRE_DL_DECODE=all` around those four DLs, `RT64
    FramebufferManager::storeRAM`/`checkRAM`).
    See `docs/HANDOFF-2026-09-15-session47.md` (and `-session46.md` for the
    superseded reading of the decoder bug).
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

