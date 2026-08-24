# Libultra bridging plan (next milestone)

Status: **planned** (2026-08-24, second session). This document captures the
findings from the first boot investigation and the concrete plan for the next
milestone: making the runtime's native `osXxx` services replace OB64's verbatim
libultra, so the game can boot.

The previous session's handoff (`HANDOFF-2026-08-24.md`) identified the problem;
this document turns that into an actionable, staged plan with the seed data we
already have.

---

## TL;DR

- The app builds and boots the runtime, but the game crashes with **SIGBUS** on
  its first hardware access (`osSiGetStatus` reading `0xA4800018` from the
  rdram mapping at ~580 MiB, beyond the 512 MiB RW region).
- Root cause: OB64's libultra was recompiled **verbatim** (generic
  `func_800xxxxx` symbols), so N64Recomp's name-based libultra reimplementation
  never fired and the runtime's `osXxx_recomp` services are never called.
- The fix (handoff Option 1): **name the libultra functions in the ELF** so
  N64Recomp's `reimplemented_funcs` matching kicks in and the game calls the
  runtime's native services.
- The core work is **building the OB64 libultra address→name table**. We
  already identified a seed set (~20 functions) from the disassembly; the rest
  follow from MMIO/cop0 fingerprinting + reference matching + call-graph
  closure.
- **This milestone is platform-independent** — it can be done on macOS or Linux.

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

| Addr | Likely function | Basis | Confidence |
|---|---|---|---|
| 0x80090140 | `osInvalI/DCache` | `cache 0x19` loop, 0x2000 chunking | high |
| 0x80090780 | `osVirtualToPhysical` | kseg0/kseg1 mask/add range logic | high |
| 0x80093060 | `bcopy` | memmove overlap → forward/backward copy | high |
| 0x80093380 | `bzero` | swl/swr unaligned clear; called first by boot stub | high |
| 0x800946C0 | SI family (`osContInit`/`__osSiRawStartDma`) | SI_STATUS&3 check + PIF DMA + cache-flush 0x40 | medium |
| 0x8009A6D0 | cop0 Cause read (`mfc0 $13`) | | high |
| 0x8009A6E0 | `osGetCount` (`mfc0 $9`) | in reimplemented set | high |
| 0x8009A6F0 | `__osGetSR`/`osGetSR` (`mfc0 $12`) | | high |
| 0x8009A700 | `osSetCompare` (`mtc0 $11`) | | high |
| 0x8009A710 | `__osSetFpcCsr` (`cfc1/ctc1 $31`) | in reimplemented set | high |
| 0x8009A720 | `__osSetSR` (`mtc0 $12`) | | medium |
| 0x8009A730 | cop0 $18 (WatchLo) writer | | low |
| 0x8009A740 | `osSpGetStatus` (SP_STATUS 0xA4040010 & 0x1C) | | high |
| 0x8009A760 | `osSpSetStatus` (write SP_STATUS) | | high |
| 0x8009A770 | `__osSpSetPc` (SP_STATUS&1 → SP_PC 0xA4080000) | in reimplemented set | high |
| 0x8009C350 | `osDpGetStatus` (DPC_STATUS 0xA410000C) | | high |
| 0x8009C370 | `osSiGetStatus` (SI_STATUS 0xA4800018 & 3) | | high |
| 0x8009D3A0-3E0 | `fabs.d` / `fabs.s` / `sqrt.d` / `ceil.s` / `floor.s` | FP intrinsics | high |

Note: `osSiGetStatus`, `osSpGetStatus`, `osSpSetStatus`, `osDpGetStatus` are
**not** in the reimplemented set — they stay verbatim. That is fine as long as
nothing calls them; the reimplemented higher-level functions (e.g. `osContInit`,
`osSpTask*`) bypass them. If game code calls them directly, handle per-case.

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


