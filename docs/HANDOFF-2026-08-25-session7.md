# Handoff — 2026-08-25 (session 7): Phase 4 begins — streamed overlays A+B+C recompiled; boot passes the old crash and reaches the next overlay's data load

## TL;DR

- **The overlay format is now understood**: OB64's "streamed" overlays are **plain
  linked MIPS code+data** that the game DMA's verbatim to a **fixed** RAM
  address. Pointers inside the overlays are absolute and already correct for
  that address — **there is no runtime relocation pass** (the session-6 handoff's
  "relocation" framing was wrong; the real problem was that overlay *functions*
  were stubs, so data structures they were supposed to build stayed garbage).
- **Three overlays are now disassembled, recompiled, and registered**:
  - A: ROM `0x3F1B0` → RAM `0x800E9C20` (boot-resident, 30 functions)
  - B: ROM `0x40E80` → RAM `0x8016AF80` (boot-resident, 459 functions)
  - C: ROM `0x1CE040` → RAM `0x80197B90` (loaded on-demand, 232 functions)
- **Boot progress**: the game now runs ~1520 recompiled functions (was 807). It
  passes the session-6 crash (`func_80075BC0`'s indirect call through a
  function-pointer table), boots through the whole streamed-code load sequence,
  and then **spins on N64 threads 1 and 3 at ~95% CPU each** after loading the
  *next* overlay's data blocks (a `{4-byte size, data}` stream into RAM
  `0x801BD930`). No stub calls, no `get_function` hard-fail, no crash in the
  boot path (there is a separate **flaky VI-thread segfault**, likely the
  pre-existing renderer-init flakiness).
- **A `load_overlays` DMA-hook was tried and REVERTED**: the runtime's
  `load_overlays(rom, ram, size)` only works for whole-overlay DMAs. OB64 streams
  in 0x200-byte chunks, which makes `lower_bound > upper_bound` (inverted range)
  → the loop runs off the end of the section table and crashes. The overlays are
  instead registered eagerly from the app's GameEntry `on_init_callback`.

## The overlay format (finding)

The game keeps a **streamed-segment table at ROM `0x387C0`** describing every
on-demand overlay: `{ram_start, ram_end, rom_start, rom_end}` (and sub-ranges
for code/data/bss). Entries found so far:

| Overlay | ROM | RAM | Size | Notes |
|---|---|---|---|---|
| A | `0x3F1B0` | `0x800E9C20` | `0x1CD0` | boot-resident; loaded by `func_80071EB0` via `func_8009DA50` |
| B | `0x40E80` | `0x8016AF80` | `0x25FB0` | boot-resident; code to `0x80186330`, data (DLs, tables, Shift-JIS text) after |
| C | `0x1CE040` | `0x80197B90` | `0x229C0` | loaded on-demand; code to `0x801B8090`, data (credits text, DLs) after, bss `0x801BA550..0x801BA730` |
| (more) | `0x1F0A00` | `0x801F7100` | — | next overlay (not reached) |
| (more) | `0x1BA020` | `0x80220F60` | — | later overlay (not reached) |

Overlay B's data section contains RDP display lists, function-pointer tables
(e.g. `0x80170760`, `0x80170774`, ... at ROM `0x65200`), and text — all with
**absolute pointers already correct for the fixed load address**.

`0x80197B90` is **reused** by several overlays (the table has multiple entries
with that RAM base). Eager registration is fine for boot (only overlay C is
loaded there); a later phase must handle overlay *swapping* (unload/load on
demand).

## Changes (repo, committed)

- `config.yaml` — `streamedA`/`streamedB`/`streamedC` code segments with data
  subsegments (`0x40640`, `0x5C230`, `0x1EE540`) and bss (`streamedC`); bin
  gaps at `0x66E30`/`0x1F0A00` pinned to their ROM address as VMA (so they do
  not occupy RDRAM VMA space and overlap the code segments).
- `relocatable_sections.txt` — added `.streamedA`, `.streamedB`, `.streamedC`.
- `Makefile` — runs `tools/fix_cross_overlay_labels.sh` before linking (re-applies
  the label fix after every `splat split`).
- `tools/fix_cross_overlay_labels.sh` — **new**: exports overlay C's loop label
  `L8019EE70` (splat emits it as a file-local `.L` label that overlay B takes the
  address of; a local can't link across objects). Idempotent.
- `asm/` — new `3F1B0.s`, `40E80.s`, `1CE040.s`, `data/40640.data.s`,
  `data/5C230.data.s`, `data/1EE540.data.s`, `data/1F0A00.bss.s`; `1060.s` /
  `2E570.data.s` re-split (symbol name changes only: `D_8016AF80`→`func_8016AF80`,
  `D_66E30`→`D_00066E30`, a few new data labels).
- `undefined_funcs_auto.txt` / `undefined_syms_auto.txt` — regenerated (the
  boot overlays' functions are now defined; the list is the *later* overlays).

## Changes (vendored, gitignored — re-apply on re-clone)

N64ModernRuntime (`tools/N64ModernRuntime`, gitignored):
- `librecomp/src/recomp.cpp` — `init()` now calls
  `load_overlays(0x1000, entrypoint, 0x3E1B0)` (base `entry`+`main` sections
  only) instead of the full 1MB. A full 1MB load would register overlays A/B/C
  at the wrong linear-mapped addresses (`0x800AEDB0`, `0x800B0A80`, ...) and
  corrupt `section_addresses`, which recompiled overlay code reads via
  `RELOC_HI16`/`LO16`.
- `librecomp/src/overlays.cpp` — the `get_function` generic stub range widened
  from `0x8016A000..0x80200000` to `0x8016A000..0x80400000` (covers the
  `0x8020B050..0x8022F284` later-overlay functions referenced by overlay C).
- Debug instrumentation kept: `[mq]`/`[sch]`/`[ev]`/`[sp]`/`[pi]` logs, drainer
  logs, `trace_millis()`.

## How to reproduce

```sh
make            # rebuild ELF with overlay sections (runs fix_cross_overlay_labels.sh)
make recomp     # regenerate RecompiledFuncs (~1520 functions)
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release   # if reconfiguring
cmake --build build-app -j2
timeout 30 stdbuf -oL -eL ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/boot.log 2>&1
```

Expected: `[overlays] registered 5 code sections`, `registered boot streamed
overlays A+B+C`, the RSP/controller/audio pipeline, then a long `[pi]` DMA
sequence loading overlay C and the next overlay's data blocks (ROM
`0x213A2E4..0x21C3958` → RAM `0x801BD930`), then silence — the process stays at
~200% CPU (N64 threads 1 and 3 spinning). Exit is SIGKILL from `timeout` (or a
SIGSEGV from the flaky VI-thread crash; rerun).

## Next session

1. **Find the post-boot spin.** N64 threads 1 and 3 busy-spin after the last
   streamed data block is loaded. The DMA requests all come from
   `func_80089F80` (confirmed by logging `__builtin_return_address` from
   `func_8008BC40_recomp`). The data blocks are a `{4-byte size, data}` stream:
   `0x213A2E4`(0x580) → `0x2171D66`(0xAB8) → `0x213A2E4`(repeat) →
   `0x21A3C58`(0x2AC) → `0x21C2EDC`(0xD8, a table of 54 ROM pointers) →
   `0x21C2FF4`(0x38) → `0x21C2EDC`(repeat) → `0x21C3954`(0x38), all into RAM
   `0x801BD930` (an overlay-D region). Suspects: (a) the game is about to call
   overlay D's init (a stub → no-op → the game waits on a flag that never
   flips); (b) an audio/task-done flag in the audio thread. To find it: sample
   the native stacks of N64 threads 1/3, or print `func_80089A10`-style spin
   reimplementations. **Overlay D** (ROM `0x1F0A00` → RAM `0x801F7100`, per the
   segment table) is the next recompilation target.
2. **Fix the flaky VI-thread segfault** (`vi_thread_func` at `update_vi()`:
   `next_state->mode->comRegs` with a garbage `mode` pointer). Possibly the
   `set_dummy_vi`/`osViSetMode` race or the dummy-framebuffer handling; make it
   deterministic to debug.
3. **RT64 GBI fix** (still pending): force OB64's F3DEX-2.08 short-format GBI
   before real display lists flow (see `docs/guides/rsp-microcode.md`).

