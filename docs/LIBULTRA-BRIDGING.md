# Libultra bridging plan (next milestone)

Status: **in progress** (2026-08-24, third session on Linux). The game now boots
with the null renderer and the runtime's native services — see "Progress" below.
This document captures the plan and the identification work.

The previous session's handoff (`HANDOFF-2026-08-24.md`) identified the problem;
this document turns that into an actionable, staged plan with the seed data we
already have.

---

## TL;DR

- The app builds and boots the runtime, but the game crashed with **SIGBUS** on
  its first hardware access (`osSiGetStatus` reading `0xA4800018` from the
  rdram mapping at ~580 MiB, beyond the 512 MiB RW region).
- Root cause: OB64's libultra was recompiled **verbatim** (generic
  `func_800xxxxx` symbols), so N64Recomp's name-based libultra reimplementation
  never fired and the runtime's `osXxx_recomp` services are never called.
- The fix (handoff Option 1): **name the libultra functions in the ELF** so
  N64Recomp's `reimplemented_funcs` matching kicks in and the game calls the
  runtime's native services.
- **Progress (2026-08-24, session 3):** 24 functions named (3 batches), the
  runtime's initial-1MB-DMA sign-extension bug fixed, and the game **boots
  without crashing**: 7 N64 threads start, the VI thread swaps buffers at ~50-60
  Hz, and the game runs its init (still before the first RSP task in the
  observed window).
- **This milestone is platform-independent** — it can be done on macOS or Linux.

---

## Progress (session 3, 2026-08-24)

### Runtime fix: initial 1MB DMA

`recomp::init()` passed the entrypoint VRAM address to `do_rom_read` as a
**zero-extended** `uint64_t`, but the `MEM_B` macro (and all recompiled memory
accesses) expects **sign-extended** 32-bit VRAM addresses. The DMA therefore
wrote 4 GiB past rdram (into unrelated memory) and the game's data section was
never loaded into rdram, which later caused a bogus null-deref in
`func_800955C0`. Fix in `tools/N64ModernRuntime/librecomp/src/recomp.cpp`:

```cpp
recomp::do_rom_read(rdram, (gpr)(int32_t)entrypoint, 0x10001000, 0x100000);
```

(The IPL3-var writes in `init()` already use `int32_t` constants and were
correct; only the DMA call was wrong.)

### Confirmed libultra address→name table (applied to `symbol_addrs.txt`)

| Addr | Name | Basis | Session |
|---|---|---|---|
| 0x80098050 | `osInitialize` | main's first call; PIF cart-ID read + SR/FPCSR setup | 3 |
| 0x80094860 | `osCreateThread` | main's 2nd call; OSThread layout init | 3 |
| 0x80094A20 | `osStartThread` | state 1/8→2, run queue | 3 |
| 0x80094950 | `osSetThreadPri` | sets thread->pri (0x4), requeues if runnable | 3 |
| 0x80094930 | `osGetThreadPri` | returns thread->pri (0x4) | 3 |
| 0x80093570 | `osCreateMesgQueue` | self-ref queue pointers + count + msgBuf | 3 |
| 0x800935A0 | `osSendMesg` | full-check + append at (first+valid) | 3 |
| 0x800936E0 | `osRecvMesg` | empty+NOBLOCK check; blocks via __osRunningThread | 3 |
| 0x8008B8C0 | `osCreatePiManager` | two mesg queues + thread create | 3 |
| 0x8008BD30 | `osCartRomInit` | OSPiHandle init + PI bus timing | 3 |
| 0x8008B820 | `__osSetIntMask`/`osSetIntMask` | MI_INTR_MASK + SR + __OSGlobalIntMask | 3 |
| 0x8008C410 | `osAiGetLength` | reads AI_LEN | 3 |
| 0x8008C420 | `osAiGetStatus` | reads AI_STATUS | 3 |
| 0x8008C430 | `osAiSetFrequency` | 0x3F000000/0x4F000000 freq math, AI_DACRATE/BITRATE | 3 |
| 0x8008C550 | `osAiSetNextBuffer` | __osAiDeviceBusy + osVirtualToPhysical + AI_DRAM_ADDR/LEN | 3 |
| 0x80093A00 | `osSpTaskLoad` | bcopy + virt_to_phys + SP regs | 3 |
| 0x80093C0C | `osSpTaskStartGo` | osSpGetStatus + osSpSetStatus | 3 |
| 0x80093C40 | `osSpTaskYield` | osSpSetStatus | 3 |
| 0x80093C60 | `osSpTaskYielded` | osSpGetStatus | 3 |
| 0x80095220 | `osCreateViManager` | mesg queues + viMgrMain thread + event | 3 |
| 0x80095820 | `osViSetMode` | VI regs 0xA4400004-34, 0x308-byte mode copy | 3 |
| 0x80095610 | `osViSetSpecialFeatures` | feature-bit AND/ORs | 3 |
| 0x800957D0 | `osViSwapBuffer` | sets framep (ctx+0x4), state bit 0x10 | 3 |
| 0x800951A0 | `osViGetCurrentFramebuffer` | reads __osViCurrContext->framep | 3 |
| 0x800951E0 | `osViGetNextFramebuffer` | reads __osViNextContext->framep | 3 |
| 0x8009A630 | `osDpSetNextBuffer` | osDpGetStatus wait + DPC writes | 3 |
| 0x8009A6E0 | `osGetCount` | `mfc0 $9` | 2 |
| 0x8009A710 | `__osSetFpcCsr` | cfc1/ctc1 $31 | 2 |
| 0x80090780 | `osVirtualToPhysical` | kseg0/kseg1 mask + range logic | 2 |
| 0x8007F880 | `main` (renamed `main_recomp`) | — | — |

Names in `symbol_addrs.txt`; the disassembly (`asm/1060.s`) now carries the
labels; N64Recomp renames them to `<name>_recomp` and the runtime provides the
implementations.

---

## Progress (session 4, 2026-08-25) — first RSP task submitted

**Milestone: the game boots cleanly, initializes controllers/audio, submits its
first RSP gfx task (`[renderer] send_dl frame=1 type=1`), and sets a real VI
mode (dummy workloads stop).** All threads run; no crashes in a 25 s window.

### What unblocked the boot

1. **osCont + timer family (batch 4)** — `osContInit` 0x80090470 (it was in the
   *PFS cluster* at 0x80096B90? **no** — that cluster is the Controller-Pak
   family: `__osSumcalc` 0x80097140 / `__osIdCheckSum` 0x80097174 / PFS; the
   real osCont cluster is the SI-touching one at 0x800900C0..0x800906C0):
   - `osContInit` 0x80090470 (retry-timer + PIF query; called from the game
     thread via `func_80089C60`)
   - `osContStartQuery` 0x800901F0, `osContGetQuery` 0x80090270,
     `osContStartReadData` 0x80090290, `osContGetReadData` 0x80090318
   - `osGetTime` 0x80094C90, `osSetTime` 0x80094D20, `osSetTimer` 0x80094D40
     (**not** 0x80094E34 — that is the timer interrupt handler `__osSetTimerIntr`).
2. **Event registration (batch 5)** — the boot trace showed the game thread
   blocked on an **audio request** that is answered by the audio thread
   (`func_80088F08` → `func_800891A0`) only when it receives the **VI retrace
   message (0x29A)**. The game registered that via its *verbatim* `osViSetEvent`
   (0x80095560), so the runtime never delivered it. Naming `osViSetEvent`
   0x80095560 + `osSetEventMesg` 0x80093940 routes event registration through
   the runtime (`ultramodern/src/events.cpp`).
3. **osSendMesg correction** — the earlier identification was backwards:
   - `0x800935A0` does a **front insert** (`first = (first+msgCount-1) % msgCount`)
     ⇒ it is **`osJamMesg`** (not osSendMesg).
   - `func_80093810` (36 call sites, the game's primary send, used by the
     audio/event request system) is the real **`osSendMesg`**. Naming it makes
     the audio responses go through the runtime's `osSendMesg` (which **wakes
     blocked threads**), unblocking the game thread's request.
4. **External-message drainer thread (runtime fix)** — with everything named,
   the VI event was delivered every frame but only drained when a game thread
   called `osSendMesg`/`osRecvMesg`. OB64's boot blocks every game thread on a
   queue fed by the VI event (main thread busy-spins), so nothing drained it.
   Added a hidden "drainer" game thread (`librecomp/src/recomp.cpp`,
   `start_external_message_drainer`) that loops
   `wait_for_external_message` + `check_running_queue`, spawned in
   `wait_for_game_started` after `on_init_callback`. **Priority 5** (this
   runtime schedules *higher* numbers first) so it yields to the threads it
   wakes (game pri 10, audio 120, RSP 100-111, DMA 50).
5. **`osSpGetStatus` (batch 6 + runtime)** — the game's RSP task threads
   (`func_80089358`, `func_80089200`) busy-wait on SP_STATUS (0xA4040010) via
   verbatim `func_800939F0` before each task — *not* dead code as session 3
   assumed. Added `osSpGetStatus` to N64Recomp's `reimplemented_funcs`
   (rebuild N64RecompCLI) and `osSpGetStatus_recomp` to the runtime's `sp.cpp`
   (returns `SP_STATUS_HALTED`; the runtime's RSP is never busy).

### Confirmed table additions (session 4)

| Addr | Name | Basis |
|---|---|---|
| 0x80090470 | `osContInit` | retry timer + `__osPackRequestData` + SI DMA + `__osContGetInitData` |
| 0x800901F0 | `osContStartQuery` | get-access + `__osPackRequestData` + DMA write/read |
| 0x80090270 | `osContGetQuery` | thin wrapper over `__osContGetInitData` |
| 0x80090290 | `osContStartReadData` | get-access + `__osPackReadData` + DMA write/read |
| 0x80090318 | `osContGetReadData` | PIF response parse (errno/button/stick) |
| 0x80094C90 | `osGetTime` | `__osDisableInt` + `osGetCount` + 64-bit base |
| 0x80094D20 | `osSetTime` | stores time base |
| 0x80094D40 | `osSetTimer` | 8-arg ABI matching runtime `osSetTimer_recomp` |
| 0x80095560 | `osViSetEvent` | stores mq/msg/retrace in `__osViNextContext` |
| 0x80093940 | `osSetEventMesg` | event table 0x800E8218 + AI immediate-send |
| 0x800935A0 | `osJamMesg` | **corrected**: front insert |
| 0x80093810 | `osSendMesg` | **corrected**: back insert, primary send |
| 0x800939F0 | `osSpGetStatus` | SP_STATUS read, used by RSP task threads |

### Current boot state (session 4, verified on Linux)

- Game boots without crashing for the full observed window (~25 s). 8 threads
  start (7 game + the runtime drainer id 200). The game thread runs its init
  (audio requests answered via the VI-retrace/audio-thread cycle).
- **First RSP gfx task submitted** (`[renderer] send_dl frame=1 type=1`); the
  game sets a **real VI mode** (dummy workloads stop; the null renderer swaps
  the game's framebuffers at ~50-60 Hz).
- The game then proceeds to load streamed code (`func_8009DA50` ROM DMA) — the
  next watch point before Phase 4 overlay loading.


---

## Why this is the blocker (recap)

1. `recomp::mem_size = 512 MiB`; the recompiled `MEM_W` macro maps
   `0xA4800018` → `rdram + 0x24800018` (~580 MiB), which is `PROT_NONE` → SIGBUS
   (`tools/N64Recomp/include/recomp.h:95`, `recomp.cpp` mmap/mprotect).
2. Even if the SIGBUS were papered over, the verbatim libultra reads/writes MMIO
   registers that no hardware backs, so the game would hang waiting for SI/VI/PI
   responses that never come.
3. N64Recomp's libultra replacement is purely **name-based**
   (`elf.cpp:85` checks `reimplemented_funcs.contains(name)`). OB64's ELF has no
   `osCreateThread`-style names, so nothing fires.
4. splat's `libultra_symbols: True` is a dead end for OB64: spimdisasm's
   `N64LibultraSyms` table only has 9 data symbols at fixed
   `0x800001A0..0x8000031C` addresses that OB64 doesn't use.

## How the runtime replacement works (the mechanism we're targeting)

- A function whose ELF symbol matches `N64Recomp::reimplemented_funcs`
  (`tools/N64Recomp/src/symbol_lists.cpp`, **116 entries**) is renamed to
  `<name>_recomp`, marked `reimplemented`/`ignored`, and **no C body is
  generated** — only a prototype.
- Callers (which previously did `func_800xxxxx(rdram, ctx)`) now emit
  `<name>_recomp(rdram, ctx)` — a direct call into the runtime's implementation
  (`librecomp/src/ultra_translation.cpp`, `vi.cpp`, `cont.cpp`, `pi.cpp`, ...).
- The overlay tables (`recomp_overlays.inl`) still list the function as
  `{.func = <name>_recomp, .rom_size = 0}`, so function-pointer calls
  (`get_function(addr)`) also resolve — including thread entry points
  (`run_thread_function` in `librecomp/src/recomp.cpp:514` calls
  `get_function(addr)`).
- The runtime's `osXxx_recomp` functions operate on `rdram` pointers passed in
  and use their own runtime state (host threads, message queues, VI/AI state),
  so they do **not** depend on OB64's libultra globals being named.

---

## Goal

Game boots with the **null renderer** using the runtime's native VI/AI/PI/thread
services: threads start, VI modes set + buffers swap at 60 Hz, controllers
init, first RSP tasks are submitted (stubbed to complete). This unblocks the
next phases (RSP microcode + RT64 renderer).

---

## The plan

### Step 0 — Housekeeping (quick, do first)
1. Remove the debug probe from `tools/N64ModernRuntime/librecomp/src/recomp.cpp`
   (mmap/mprotect prints + `rdram+0x24800018` probe read, lines ~888-892).
2. Fix the stale function count in docs/PLAN comments: the recompiled output has
   **807 functions** (802 `func_800xxxxx` + `main_recomp`/`recomp_entrypoint`/
   others), not 3659.
3. Update this doc + `DECISIONS.md` as the work proceeds.

### Step 1 — Build the OB64 libultra address→name table (the core work)
See *Identification methodology* below. Deliverable: a verified
`(vram, name)` list applied to **`symbol_addrs.txt`** entries like:

```
osCreateThread = 0x8009XXXX; // type:func
osContInit = 0x800946C0;     // type:func
```

We only need names for the **116 `reimplemented_funcs` entries that OB64
actually calls** (plus optionally naming `bzero`/`bcopy`/cop0 helpers for
debugging). Functions not in the reimplemented set stay verbatim, which is fine.

### Step 2 — Regenerate & verify
```sh
tools/venv/bin/splat split config.yaml   # re-splat with new symbols
make                                     # assemble + link ELF
make recomp                              # regenerate RecompiledFuncs/
```
Verify:
- `grep osCreateThread_recomp RecompiledFuncs/funcs.h` → prototypes exist.
- `recomp_overlays.inl` lists `osXxx_recomp` entries with `rom_size = 0`.
- Former callers now emit `osXxx_recomp(rdram, ctx)` instead of
  `func_800xxxxx(rdram, ctx)`.

### Step 3 — Boot iteration (null renderer)
```sh
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j4
./build-app/ogrebattle64 assets/ogre64.z64
```
Expected first success: game proceeds past SI, runtime threads start, VI modes
set + buffers swap, controllers init — visible in runtime/`[renderer]`/
`[overlays]` logs.

Iterate on failures in this likely order:
1. Wrong/edge function names → refine the table.
2. Remaining verbatim MMIO callers reached from game code → either add a runtime
   implementation, or add a **narrow MMIO shim** for only the registers the game
   actually reads (e.g. VI_CURRENT, PI_STATUS). This is handoff Option 3 done
   surgically, not as the primary approach.
3. Runtime gaps (e.g. `osVoice*`, PFS entries OB64 uses that the runtime stubs) →
   implement or log-and-return.
4. Thread-related crashes → verify OSThread/mesg layout compat (expected fine:
   OB64's libultra matches the standard 2.0.x layout the runtime assumes) and
   stack sizes.

### Step 4 — After first boot: RSP + RT64
- Per `docs/guides/rsp-microcode.md`: locate ucode boundaries (F3DEX @ 0x3C686,
  L3DEX @ 0x3D6A6, S2DEX @ 0x3DA96/0x3DE97; audio ucode TBD), write `RSPRecomp`
  configs, implement `get_rsp_microcode` in `app/src/rsp.cpp`.
- Replace the null renderer (`app/src/renderer.cpp`) with the RT64-based one.
- **This is where the Linux recommendation applies** — see
  `docs/guides/linux-migration.md`.

---

## Identification methodology

1. **Seed anchors** (see table below) — verified from the disassembly.
2. **MMIO fingerprinting**: scan the recompiled C for `0xA4xxxxxx` register
   constants (SI/VI/AI/PI/SP/DP/MI). Already done: 17 functions.
3. **cop0 fingerprinting**: functions using `mfc0/mtc0` with register numbers
   (Count=9, SR=12, Cause=13, Compare=11, WatchLo=18, ...) map to the
   `osGetCount`/`osSetCompare`/`__osGetSR`-family.
4. **Reference matching**: OB64's libultra is a standard Nintendo-precompiled
   build. Compare each OB64 function against a canonical libultra disassembly
   with names (e.g. from a decomp that ships `libultra` asm, or a standalone
   libultra symbol repo) using:
   - exact byte match (fast first pass),
   - normalized instruction match (resolve branch targets, mask immediates),
   - signature match (MMIO addresses, cop0 regs, called-function graph).
5. **Call-graph closure**: functions called only by confirmed libultra functions
   (and not by game code) are likely libultra too.
6. **Manual verification** of candidates against the known libultra source
   (the runtime's `ultra_translation.cpp` + public libultra sources).

---

## Seed table (identified this session)

> Status update (session 3): the table in the "Progress" section above is the
> current authoritative confirmed list. This original seed table is kept as the
> identification record; entries that are now applied to `symbol_addrs.txt` are
> marked ✔. The remaining entries are candidates for the `osCont*` family and
> the thread/timer cluster (next session).

| Addr | Likely function | Basis | Confidence | Status |
|---|---|---|---|---|
| 0x80090140 | `osInvalI/DCache` | `cache 0x19` loop, 0x2000 chunking | high | not yet named (dead after osInitialize) |
| 0x80090780 | `osVirtualToPhysical` | kseg0/kseg1 mask/add range logic | high | ✔ named |
| 0x80093060 | `bcopy` | memmove overlap → forward/backward copy | high | verbatim (fine) |
| 0x80093380 | `bzero` | swl/swr unaligned clear; called first by boot stub | high | verbatim (fine) |
| 0x800946C0 | `__osSiRawStartDma` | SI_STATUS&3 check + PIF DMA + cache-flush 0x40 | medium | not named (dead after osInitialize) |
| 0x8009A6D0 | cop0 Cause read (`mfc0 $13`) | | high | not named (dead) |
| 0x8009A6E0 | `osGetCount` (`mfc0 $9`) | in reimplemented set | high | ✔ named |
| 0x8009A6F0 | `__osGetSR`/`osGetSR` (`mfc0 $12`) | | high | not named (dead) |
| 0x8009A700 | `osSetCompare` (`mtc0 $11`) | | high | not named (no runtime def; verbatim is fine) |
| 0x8009A710 | `__osSetFpcCsr` (`cfc1/ctc1 $31`) | in reimplemented set | high | ✔ named |
| 0x8009A720 | `__osSetSR` (`mtc0 $12`) | | medium | not named (dead) |
| 0x8009A730 | cop0 $18 (WatchLo) writer | | low | not named (dead) |
| 0x8009A740 | `osSpGetStatus` (SP_STATUS 0xA4040010 & 0x1C) | | high | dead (RSP task family named) |
| 0x8009A760 | `osSpSetStatus` (write SP_STATUS) | | high | dead (RSP task family named) |
| 0x8009A770 | `__osSpSetPc` (SP_STATUS&1 → SP_PC 0xA4080000) | in reimplemented set | high | ✔ named |
| 0x8009C350 | `osDpGetStatus` (DPC_STATUS 0xA410000C) | | high | dead (osDpSetNextBuffer named) |
| 0x8009C370 | `osSiGetStatus` (SI_STATUS 0xA4800018 & 3) | | high | dead (osInitialize named) |
| 0x8009D3A0-3E0 | `fabs.d` / `fabs.s` / `sqrt.d` / `ceil.s` / `floor.s` | FP intrinsics | high | verbatim (fine) |

Note: the verbatim MMIO readers (`osSiGetStatus`, `osSpGetStatus`,
`osSpSetStatus`, `osDpGetStatus`, `__osAiDeviceBusy`) are now **unreachable**
because every caller is reimplemented. No MMIO shim was needed so far. If game
code reaches one directly, handle per-case (name it + add a runtime stub, or add
a narrow MMIO shim).

## Other MMIO-touching functions (candidate libultra, from the scan)

| Addr | MMIO | Notes |
|---|---|---|
| 0x8008BA50 | PI (0xA46x) | review |
| 0x8008B820 | MI (0xA43x) | review |
| 0x8008C410 / 0x8008C420 | AI (0xA45x) | likely `osAiSetNextBuffer` / `osAiSetFrequency` |
| 0x800939F0 | SP (0xA40x) | review |
| 0x80095820 | VI (0xA44x, 0x308 bytes) | likely `osViSetMode`-family |
| 0x80098280 | PI | review |
| 0x800997F0 / 0x800998E0 / 0x80099A50 | PI | likely `osPiReadIo`/`osPiStartDma`/`osPiGetStatus` |
| 0x80099BC0 | AI | review |

---

## Risks / open questions

1. **Libultra revision mismatch** with whatever reference we match against →
   more manual identification (bounded, a few hours).
2. **Direct MMIO pokes from game code** — the 17 candidates above are mostly
   libultra, but a few need review.
3. **OSThread/mesg layout compat** with the runtime — expected fine (standard
   libultra 2.0.x), verify at the first thread crash.
4. **Save type** (PFS/EEPROM/Flash) is deferred to Phase 6; `SaveType::None` is
   fine for boot.
5. **Plan.md's "3659 functions" is stale** — actual recompiled count is 807.

---

## Linux migration

See `docs/guides/linux-migration.md` for the full list of macOS-specific code
to remove and the Ubuntu toolchain setup. **This milestone does not require the
switch** — it is CPU-side and platform-independent.


