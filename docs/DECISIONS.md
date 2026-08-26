# Technical Decisions

This is the running log of technical decisions for the Ogre Battle 64 PC port.
Each entry records what was decided, why, and when. New entries go on top.

---

## 2026-08-25 (session 9) — The `func_8019FC68` DL-build crash was a recompilation bug; N64Recomp now emits fall-through tail calls; boot reaches real rendering

### Finding: KMC shared-epilogue functions lose their register restores

OB64's compiler (KMC) merges identical function epilogues into a separate symbol
that the preceding function **falls through into**. `func_8019ABC4`'s epilogue
ends with `lw $ra/$fp/$s7/$s6` at `0x8019AEFC..0x8019AF08` and then falls into
`func_8019AF0C`, a separately-symbolized shared epilogue that restores
`$s5..$s0` and `$sp`. N64Recomp treated the fall-through as the end of
`func_8019ABC4` and never ran the shared epilogue, so the callee-saved
registers and `$sp` were left clobbered. In the title path,
`func_8019FC68` (session-8 crash site) captured a heap pointer in `s0`, called
`func_8019ABC4`, then `free(s0)` — freeing a garbage value (`0x801bc666`) and
SIGSEGV'ing in `func_800712C4`.

### Decision: emit a fall-through tail call for shared-epilogue chains

`N64Recomp::recompile_function_impl` now, after processing a function's
instructions, emits `next_func(rdram, ctx); return;` when the function's code can
fall off the end of its range and the next function starts exactly at
`func.vram + words*4`. "Can fall off the end" means the last instruction is not
`jr`/`j`/`syscall`/`break`/`eret` (nor the delay slot of one of those, which
already emitted a return). Detected 23 genuine cases across all segments.

### Decision: process N64Recomp static functions to a fixpoint and register them by address

Static functions (created for cross-boundary branches) were neither registered in
`context.functions_by_vram` nor created in an order that guaranteed a static's
fall-through target (another static) existed before it was recompiled. `main.cpp`
now registers statics in `functions_by_vram` and processes them in a two-phase
fixpoint loop (create all known statics, then recompile the batch, repeat until no
new statics). This fixed `static_16_8019AB84` → `static_16_8019AB94` (a shared
epilogue chunk), which was otherwise lost. Total fall-through tail calls: 35.

### Outcome

Boot now passes the title-screen DL build (the `func_8019FC68` crash is gone),
runs ~1528 PI DMAs and 64 RSP tasks (including new `type=1` tasks), and reaches
real RT64 frame rendering. The next crash is in the **RT64 render thread** calling
`libvulkan_intel_hasvk.so` (the Intel Haswell Vulkan driver) inside
`FramebufferRenderer::submitRasterScene` — see
`docs/HANDOFF-2026-08-25-session9.md` for leads (driver vs. RT64 GBI vs. the
pre-existing flaky renderer-init race).



## 2026-08-25 (session 8) — The post-boot spin was a cooperative-scheduler deadlock; N64Recomp now emits `yield_self` for poll loops


### Finding: Thread 3's `.L80075FB8` spin on `D_800C4C26` starves the drainer (deadlock)

The session-7 "N64 threads 1+3 spin" is Thread 3 (`func_80075BC0`, the system
loop) waiting in a **pure spin** for the 16-bit word `D_800C4C26` to change
from `0xFFFF`/`0xFFFD`. The word is set to `0xFFFC` by the boot state-machine
callback `func_80072398` after 13 invocations. The callback is driven by the
game's own event chain — Thread 19 (`func_80088F08`) wakes on VI-retrace/AI
messages (`0x29A`/`0x29D` on `D_800E8B84`), dispatches via `func_800891A0` to
Thread 4 (`func_8008AFE0`, queue `D_800C4C28`), which calls the callback. The
VI thread enqueues the messages, but delivery requires the cooperative
**drainer** (pri 5) to run, and Thread 3's pure spin never yields → the whole
chain stalls forever. Same class as session 6's `func_80089A10` spin.

### Decision: make N64Recomp emit `yield_self` for poll loops (general fix)

Instead of reimplementing the spinning function natively (session 6's approach,
impractical for the 1390-byte `func_80075BC0`), N64Recomp's `print_branch` now
detects **poll loops** — backward conditional branches whose body has no
function calls and no stores, and contains a load whose base register is
loop-constant at the load site (cyclic last-write is `lui`, or never written in
the body) — and emits `yield_self(rdram);` before the loop-back goto.
`yield_self` (already in the runtime) waits for one external message and checks
the running queue, so the drainer can deliver the very event the poll awaits.

The heuristic was tuned against the whole codebase: it flags ~17 loops (the
`D_800C4C26` spin, the PI/DP status polls, overlay game-logic polls) while
correctly excluding data loops (strlen/list-walk/memcpy) whose load bases are
written by non-constant ops.

### Outcome

Boot now passes the old spin, runs through the overlay-D data loads, the
controller path, and the RSP pipeline, and reaches **title-screen display-list
building** (`func_801A1FCC`/`func_8019FC68`, overlay C). The next crash is
`func_8019FC68` at `0x8019FFF4`: it stores through `0x803ffa7b +
entry->[0x34]` where a graphics-object entry in the heap array at `0x803fefa0`
has a garbage `[0x34]` (varies per run), wrapping the address to unmapped
RDRAM. The array is allocated + zeroed by `func_801A1A2C`, so the garbage is
written later — root cause open (see session-8 handoff leads: likely an
audio-init / record-population step or a wrong splat function boundary for the
`nonmatching func_8019FC68, 0xF58` region).


## 2026-08-25 (session 7) — Phase 4: streamed overlays are plain linked code (no relocation); A+B+C recompiled and registered

### Finding: the streamed overlays need no runtime relocation

The session-6 handoff framed the next wall as "overlay relocation". That was
wrong. OB64's streamed overlays are **plain linked MIPS code+data** DMA'd to a
**fixed** RAM address (per the streamed-segment table at ROM `0x387C0`). All
pointers inside them (function-pointer tables at e.g. ROM `0x65200`, RDP
display lists, data tables) are absolute addresses already correct for that
load address. The real problem was that overlay *functions* were log-and-return
stubs, so the data structures they build at boot (e.g. the descriptor that
`func_80075BC0` dispatches through) were never created, and the game read raw
code bytes as function pointers (`0x3C028019` = `lui $v0, 0x8019`).

### Decision: disassemble + recompile the overlays into the same ELF, register them eagerly

- Added `streamedA` (ROM `0x3F1B0` → RAM `0x800E9C20`), `streamedB` (ROM
  `0x40E80` → RAM `0x8016AF80`), and `streamedC` (ROM `0x1CE040` → RAM
  `0x80197B90`) as splat `code` segments; bin gaps (`0x66E30`, `0x1F0A00`) are
  pinned to their ROM address as VMA so they don't collide in RDRAM VMA space.
- Recompiled the whole ELF together (807 → ~1520 functions). Cross-overlay
  absolute references (overlay B takes the address of overlay C's
  `.L8019EE70`) need the label exported globally; `tools/fix_cross_overlay_labels.sh`
  re-applies that after every `splat split` (wired into the Makefile).
- The app registers A+B+C via `load_overlays()` from the GameEntry
  `on_init_callback`; the runtime's `recomp::init()` now loads only the base
  sections (`entry`+`main`) so overlays aren't registered at the wrong
  linear-mapped addresses (which would corrupt `section_addresses`, read by the
  recompiled overlay code through `RELOC_HI16`/`LO16`).

### Decision: a `load_overlays` DMA hook was tried and REVERTED

Hooking `func_8008BC40_recomp` (the game's DMA request, reimplemented as a sync
ROM read) to call `load_overlays(dev_off, dramAddr, size)` is the "obvious" way
to register overlays as the game streams them. It fails for OB64 because the
game streams in **0x200-byte chunks**: `load_overlays` computes a bound range
from `rom`+`size`, and a chunk-sized range makes `lower_bound > upper_bound`
(inverted), so the load loop walks off the end of the section table and
segfaults. Registration therefore happens eagerly at boot instead.

### Outcome

The game now boots through the old `func_80075BC0` function-pointer-table crash
and runs ~1520 real functions through the whole streamed-overlay load sequence
before **spinning on N64 threads 1 and 3** after loading the next overlay's data
blocks into RAM `0x801BD930`. No stub calls, no `get_function` hard-fail.
Remaining: find that spin (overlay D at ROM `0x1F0A00` → RAM `0x801F7100` is the
likely next recompilation target), fix the flaky VI-thread segfault, then the
RT64 GBI fix.


## 2026-08-25 (session 6) — The boot stall was a scheduler busy-spin deadlock; three fixes unblock boot into the main loop

### Finding: "waits for a second RSP task" was wrong — the game thread busy-spins and starves the drainer

The session-5 conclusion (callback slots at 0x800B9E84 all zero → the sync task's
completion "does nothing" → the game waits for a second task) was **incorrect** on two
counts:

- The real callback-slot address is **0x800A9E84** (0x800B9E84 was a dump misrecord, off
  by 0x10000). The type-4 slot is registered at boot (`func_8008A1B0` →
  `func_800899D0(func_8008B110)`), and the sync task is **type 0**, which never dispatches
  to any callback anyway — so "dispatch does nothing" is expected, not a bug.
- The actual deadlock: after the sync task, the game thread enters `func_80089A10`, a tight
  `bnez` spin on the task-done counter `D_800E79A4`. The runtime's cooperative scheduler
  (higher priority numbers first) cannot preempt a spinning thread, so the spin (pri 10)
  starves the external-message **drainer** (pri 5) that must deliver the SP/DP completion
  events which eventually decrement the counter.

### Decision: reimplement the spin (`func_80089A10`) as a yielding wait

N64Recomp only emits its `pause_self` yield for *exact self-loops* (`j`/`b` to the
instruction's own address); this spin branches to the *function start*, so it was compiled
with no yield. Rather than special-casing the generator, we added `func_80089A10` to
N64Recomp's `reimplemented_funcs` and implemented `func_80089A10_recomp` in the runtime:
loop while `MEM_W(0, 0x800E79A4) != 0`, calling `wait_for_external_message` +
`check_running_queue` each iteration. This lets the game thread itself drain the SP/DP
completion events and hand off to the higher-priority RSP threads.

### Finding: the game's PI manager is dead, so every DMA deadlocks

`osCreatePiManager_recomp` is an **empty stub**. The game's verbatim `osCreatePiManager`
(0x8008B8C0) — the only writer of `D_800AA400`/`D_800AA408` — is dead code. The game's
DMA-request function `func_8008BC40` checks `D_800AA400` and bails when 0, so the blocking
DMA helpers (`func_80089F80`, `func_8008A0F0`) wait forever on their completion queue; the
first streamed-code load (`func_8009DA50`) deadlocks.

### Decision: reimplement `func_8008BC40` (DMA request) as a synchronous ROM read

`func_8008BC40_recomp` reads the game's `OSIoMesg` (mq@+4, dram@+8, dev@+0xC, size@+0x10),
calls `recomp::do_rom_read`, and completes the request via `osSendMesg` so the caller's
`osRecvMesg` returns immediately. One function fixes all game DMA paths without the PI
thread. **Important**: the ROM copy must go through `recomp::do_rom_read` (which
byte-swaps big-endian ROM bytes into host-order RDRAM words via `MEM_B`); a plain `memcpy`
produces byte-swapped garbage pointers.

### Decision: generic stub for not-yet-loaded streamed functions

`get_function` now returns a logging no-op stub for unknown addresses in the streamed
ranges (`0x8016A000..0x80200000`, `0x84000000..0x84200000`) instead of hard-failing. This
lets boot reach the main loop. It is a bring-up measure; Phase 4 will replace the stubs
with recompiled overlay functions.

### Outcome

The game now boots through the RSP pipeline, controllers, 703 streamed-overlay DMA loads,
and into its main loop (18 distinct streamed functions called, all stubbed). The next
crash is an **indirect call through a streamed-overlay function-pointer table that needs
relocation** (`func_80075BC0` reads `*(u32*)(*(u32*)(0x800E8294))` = `0x3C028019`, a MIPS
instruction, not a pointer). That is the start of **Phase 4 (overlay loading +
recompilation)**. See `docs/HANDOFF-2026-08-25-session6.md`.


## 2026-08-25 (session 5) — RT64 renderer integrated; the boot stalls waiting for a second RSP task

### Decision: replace the null renderer with RT64 (Vulkan on Linux)

The game has been submitting real gfx tasks since session 4, so the null
renderer has served its purpose. `app/src/renderer.cpp` now wraps
`RT64::Application` (the current RT64 HLE architecture with `Interpreter` /
`GBIManager` / `State`) behind ultramodern's `RendererContext`. Wiring copied
from N64Recomp/RecompFrontend's `rt64_render_context.cpp`: `Application::Core`
gets RDRAM, DMEM/IMEM slots, DPC registers, and the VI registers straight from
`ultramodern::renderer::get_vi_regs()`. RT64's render hooks are optional
(null-checked) and not wired.

Verified on the Intel HD 4400 (Mesa): RT64 sets up Vulkan, presents frames at
~60 Hz, and the game boots through the renderer unchanged. This also fixed the
SDL window: it must be created with `SDL_WINDOW_VULKAN` on Linux (and
`SDL_WINDOW_METAL` on macOS) or `SDL_Vulkan_CreateSurface` fails and RT64
segfaults during setup.

### Decision: gfx display lists are interpreted by RT64, not recompiled microcode

The runtime routes gfx tasks (`M_GFXTASK`) to the renderer's `send_dl`, never
through `get_rsp_microcode` — so `RSPRecomp` is **not** needed for the gfx
ucode. RT64 ships its own GBI interpreters (`src/gbi/`) and parses display
lists itself via `loadUCodeGBI` + `processDisplayLists`. RSPRecomp remains for
the **audio** ucode (task type 5) once audio tasks flow.

### Finding: OB64's ucode is "F3DEX fifo 2.08" (short-format opcodes) and RT64 misidentifies it as F3DEX2

Runtime OSTask dump: `ucode=0x8009F540 ucode_data=0x800AC140` →
ROM `0x2F940`/`0x3C540`; the data block contains
`"RSP Gfx ucode F3DEX fifo 2.08 ... Yoshitaka Yasumoto 1999 Nintendo"`.
The game's display lists use the F3DEX **short-format** opcode set
(`G_SETOTHERMODE_H=0xDE`, `G_SETOTHERMODE_L=0xDF`, `G_RDPFULLSYNC=0xE9`, ...).

RT64's `GBIManager::getGBIForUCode` (XXH3 hash database) matches this ucode to
`GBIUCode::F3DEX2` (verified in-app: `GBI matched: 7`). F3DEX2 semantics differ
(`0xDE` = `G_DL`, `0xDF` = `G_ENDDL`), so RT64 misparses OB64's DLs — the first
(sync) DL "branches" into MIPS code at RDRAM `0xA9EF0`. RT64 has no GBI map for
the F3DEX-2.08 short set. **Action item**: add/force the correct GBI before
real DLs flow (see handoff).

### Finding: the boot stalls after the first gfx task — the task-done dispatch has zero callbacks

The whole RSP task pipeline works end-to-end: `osSpTaskStartGo` → `sp_complete`
(→ `0x800E8BBC`) + `send_dl` + `dp_complete` (→ `0x800E8BF4`) →
`func_80089200` sends the task-done message to the task's `done_mq`
(`0x800E9BA8`) → the done pump `func_80089540` consumes and dispatches on the
message's command type. But the dispatch callback slots (`0x800B9E84/88/8C`)
are all **zero** at boot, so the first (sync) task's completion does nothing
and the game waits for a second task that never comes. The game's next boot
steps (`func_8009DA50` streamed DMA, then streamed funcs `0x800E9CEC`/
`0x800E9C20`) are never reached. See the handoff for the thread map and the
investigation path.

### Decision: keep the boot-time diagnostics in the vendored runtime for now

The gitignored runtime carries temporary `[sp]`/`[ev]`/`[mq]` debug prints
(task submission, event registration, SP/DP completion, targeted queue
activity). They are cheap, and removing them needs a full librecomp/ultramodern
rebuild; they are documented in the handoff and should be stripped once the
stall is fixed.

---

## 2026-08-25 (session 4) — First RSP task + real VI mode: boot is past the libultra bridge

### Decision: correct osSendMesg/osJamMesg identification

Session 3 named `0x800935A0 = osSendMesg`, but that function does a **front
insert** (`mq->first = (first+msgCount-1) % msgCount`), i.e. `osJamMesg`. The
game's real back-insert `osSendMesg` is `func_80093810` (36 call sites; the
audio/event request system uses it). Renaming `func_80093810 = osSendMesg`
makes audio-request responses go through the runtime's `osSendMesg`, which
**wakes blocked threads** — the missing piece that let the game thread proceed
past its first audio request.

### Decision: add an external-message drainer thread to the runtime

With VI events being delivered every frame, the boot still deadlocked: the
runtime only drains its external-message queue when a game thread calls
`osSendMesg`/`osRecvMesg`, and OB64's boot blocks every game thread on a queue
fed by the VI retrace event (its main thread busy-spins). Added a hidden
"drainer" game thread in `librecomp/src/recomp.cpp` (spawned after
`on_init_callback`) that loops `wait_for_external_message` +
`check_running_queue`. It must be **lower** priority (5) than the threads it
wakes, because this runtime schedules **higher** priority numbers first —
priority 30 starved the game thread (pri 10) in the running queue.

### Decision: name the osCont family, timers, events, and osSpGetStatus

- The PFS (Controller-Pak) cluster at `0x80096B90+` is *not* the osCont family
  (`__osSumcalc`/`__osIdCheckSum` etc.); the osCont cluster is the SI-touching
  one at `0x800900C0..0x800906C0`. Named `osContInit`, `osContStartQuery`,
  `osContGetQuery`, `osContStartReadData`, `osContGetReadData`.
- Timers: `osGetTime` 0x80094C90, `osSetTime` 0x80094D20, `osSetTimer`
  0x80094D40 (the handoff's `0x80094E34` is the timer *interrupt handler*, not
  osSetTimer).
- Events: `osViSetEvent` 0x80095560 and `osSetEventMesg` 0x80093940 — the game
  registered its VI-retrace→audio-queue message (0x29A) through its verbatim
  osViSetEvent, so the runtime never delivered it.
- `osSpGetStatus` 0x800939F0: the RSP task threads busy-wait on SP_STATUS; not
  dead code as session 3 assumed. Added to N64Recomp's `reimplemented_funcs`
  (N64RecompCLI rebuild required) + runtime `osSpGetStatus_recomp` (returns
  SP_STATUS_HALTED).

### Outcome

The game boots without crashing for the full observed window (~25 s), submits
its **first RSP gfx task** (`send_dl frame=1 type=1`), and sets a **real VI
mode** (dummy workloads stop; the null renderer swaps the game's framebuffers
at ~50-60 Hz). Next watch point: the streamed-code ROM DMA (`func_8009DA50`)
before Phase 4 overlay loading.

---

## 2026-08-24 (session 3) — First boot achieved: libultra bridging is working

### Decision: name-based libultra replacement confirmed and applied (3 batches)

Applied 24 names to `symbol_addrs.txt` (see `LIBULTRA-BRIDGING.md` "Progress").
The boot path now runs the runtime's native `osXxx_recomp` services:
`osInitialize`, `osCreateThread`/`osStartThread`/`osSetThreadPri`/
`osGetThreadPri`, `osCreateMesgQueue`/`osSendMesg`/`osRecvMesg`,
`osCreatePiManager`/`osCartRomInit`, `osSetIntMask`,
`osAiGetLength`/`osAiGetStatus`/`osAiSetFrequency`/`osAiSetNextBuffer`,
`osSpTaskLoad`/`osSpTaskStartGo`/`osSpTaskYield`/`osSpTaskYielded`,
`osCreateViManager`/`osViSetMode`/`osViSetSpecialFeatures`/`osViSwapBuffer`/
`osViGetCurrentFramebuffer`/`osViGetNextFramebuffer`, `osDpSetNextBuffer`,
`osGetCount`, `__osSetFpcCsr`, `osVirtualToPhysical`.

**Outcome:** the game **boots without crashing** on Linux — 7 N64 threads start,
the null renderer swaps VI buffers at ~50-60 Hz, and the game runs its init.
No MMIO shim (handoff option 3) was needed; naming the RSP-task family made the
remaining verbatim MMIO readers (`osSpGetStatus`, `osSpSetStatus`,
`__osAiDeviceBusy`, `osSiGetStatus`, `osDpGetStatus`) unreachable dead code.

### Decision: fix the runtime's initial-1MB DMA sign-extension bug

`recomp::init()` passed the entrypoint VRAM address to `do_rom_read` as a
zero-extended `uint64_t`, but recompiled memory accesses expect sign-extended
32-bit addresses (the `MEM_B`/`MEM_W` macros subtract `0xFFFFFFFF80000000`).
The DMA wrote ~4 GiB past rdram, so the game's data section never loaded and a
later verbatim VI helper null-deref'd. Fixed by sign-extending:

```cpp
recomp::do_rom_read(rdram, (gpr)(int32_t)entrypoint, 0x10001000, 0x100000);
```

This is a genuine N64ModernRuntime bug that other projects would hit with a
high `0x800xxxxx` entrypoint; our fix stays local to the vendored runtime.

### Decision: new analysis tool

Added `tools/libultra_scan.py` — parses `asm/1060.s` and reports, per function:
MMIO registers, cop0 registers, and direct callees. This replaces the one-off
manual scans with a reproducible inventory.

### Next session (see `docs/HANDOFF-2026-08-24.md` / `LIBULTRA-BRIDGING.md`)

- Name the `osCont*` family (`osContInit` etc.) — the game may be waiting on
  controller init; the game thread has not yet hit streamed-code stubs or
  submitted RSP tasks in the observed window.
- Watch for the first RSP task and the game's first real `osViSetMode`
  (the null renderer currently shows the runtime's dummy framebuffers).

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
