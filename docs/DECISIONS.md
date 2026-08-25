# Technical Decisions

This is the running log of technical decisions for the Ogre Battle 64 PC port.
Each entry records what was decided, why, and when. New entries go on top.

---

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
