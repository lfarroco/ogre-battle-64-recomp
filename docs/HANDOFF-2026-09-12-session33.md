# Session 33 — Phase 4: the ~95s bank swap is fixed; the 170s run completes

Goal (from session 32 §6): **Phase 4 — overlay sections.** Recompile the
streamed overlays, teach the port to swap a bank on the game's DMA, and get past
the t≈95s abort.

Done: the streamed-overlay **segment table** and **bank descriptors** are
decoded (19 records / 11 banks), the bank the game swaps in at t≈95s (records
2, 3, 6) is recompiled in a second unit and registered on the DMA, and the
unattended 170s run is now **exit 0, 4850 display lists at 168s** (was exit 134
at display list 2770 / 94.8s).

## 0. State at the start

```sh
OGRE_NO_AUDIO=1 OGRE_EXIT_AFTER_MS=15000 ./build-app/ogrebattle64   # exit 0, ~400 display lists
OGRE_NO_AUDIO=1 OGRE_EXIT_AFTER_MS=170000 ./build-app/ogrebattle64  # exit 134 at t≈94.8s, dl 2770
```

Session 32 §3 localised the abort to `streamed function stub called @
0x801AD5C0` + a NULL function-pointer call, DMA'd in over overlay C at t≈95s.

## 1. The streamed-overlay model, decoded

### 1a. The segment table (ROM `0x387C0`) is 19 records of 0x28 bytes

`func_800761E4` loads one record per iteration; `func_80076430` matches the
game's requested bitmask to a **bank descriptor**. Field layout
(`[ram_start, ram_end, rom_start, rom_end, bss_start, bss_end, code_start,
code_end, data_start, data_end]`):

| word | meaning | consumer |
|---|---|---|
| +0x00 / +0x04 | RAM start (DMA dest) / RAM end (incl. bss) | `func_8009DA50` |
| +0x08 / +0x0C | ROM start / ROM end | `func_8009DA50` |
| +0x10 / +0x14 | bss start / end | `func_80093380` (bzero) |
| +0x18 / +0x1C | code range | `func_800900C0` (icache invalidate) |
| +0x20 / +0x24 | data range | `func_80090010` (dcache invalidate) |

Verified against overlay C (record 15: ROM `0x1CE040`→`0x1F0A00`, RAM
`0x80197B90`, code to `0x801B80A0`, data to `0x801BA550`, bss 0x1E0) — exactly
`config.yaml`'s `streamedC`.

### 1b. The bank descriptors (ROM `0x38AB8`, pointers at ROM `0x38AFC`)

Byte lists of record indices terminated by `0xFF`; `func_80076430` picks the
descriptor whose record set matches the requested mask. **11 banks:**

| id | records |
|---|---|
| 0 | 2, 1 |
| 1 | 0, 2, 3, 7, 8, 9 |
| 2 | 0, 2, 4, 10, 11, 12, 13, 14 |
| 3 | 0, 14 |
| 4 | 0, 2, 3, 6 |
| 5 | 15 (overlay C) |
| 6 | 0, 1, 2, 7, 8, 9, 10, 11, 12, 13, 14 |
| 7 | 2, 4, 10, 11, 12, 13, 16 |
| 8 | 17, 18 |
| 9 | 0, 2, 3, 7, 8, 9, 18 |
| 10 | 0, 2, 1, 18 |

Records are shared between banks and banks overlap in RAM. The t≈95s swap is
**bank 4 = {0,2,3,6}**; only 2, 3 and 6 are actually DMA'd (record 0's RAM range
is overwritten by record 2, and the run shows no DMA of ROM `0x66E30` at that
point).

## 2. splat's overlay support is unusable here (negative result)

splat/spimdisasm have first-class overlay support (`exclusive_ram_id`,
`overlayCategory`, per-overlay symbol namespaces), which is the "obvious"
one-ELF mechanism. **It drops every branch-label definition:** tagging
`streamedC` with an `exclusive_ram_id` makes `asm/1CE040.s` contain 0 of its
1650 `.Lxxxx:` labels (references remain → ~3843 undefined symbols at link).
Same with C in its own group. Overlay C must stay a plain global segment, so a
single ELF cannot hold both banks at their true VMAs.

## 3. The fix: a second recompilation unit + a DMA-driven bank swap

### 3a. Recompiling the bank records separately

| file | what |
|---|---|
| `config-bank.yaml` (new) | splat config: records 2/3/6 at their **true RAM VMAs** (0x80197B90 / 0x8019EE70 / 0x801AD5C0), `symbol_name_format: "ovl_$VRAM"`, ROM gaps as `bin`s, output under `build/bank/` |
| `Makefile` | `make bank` (splat + assemble + link `build/ogrebank.elf`), `make bank-recomp` (+ N64Recomp + generator) |
| `config-bank.toml` (new) | N64Recomp for the bank ELF → `BankFuncs/` |
| `tools/gen_bank_funcs.py` (new) | flattens `BankFuncs/recomp_overlays.inl` into `app/src/bank_funcs.inc` |

Why this works: the overlays are pre-linked absolute, so linking them at their
true RAM addresses means every reference is already correct — the bank sections
are **not** relocatable and never touch `section_addresses`/`RELOC_HI16`. The
`ovl_` prefix only keeps the 204 generated symbols from colliding with overlay
C's names in the app binary.

Function detection: 40 (rec 2) + 96 (rec 3) + 68 (rec 6) = **204 functions**;
the linked section sizes are exactly the table's (0x72C0 / 0xE440 / 0x7700),
which confirms the code/data splits.

### 3b. Runtime: swap the function map on the DMA

New in `librecomp` (declared in `overlays.hpp`):

* `notify_rom_read(rom_offset, ram, size)` — called from `recomp::do_rom_read`
  (both the inline-PI path and `func_8008BC40_recomp` feed through it). It finds
  the streamed section (`ram_addr >= 0x800E0000`) that contains the DMA's ROM
  offset **and** whose `ram - rom` delta matches, then unloads whatever bank is
  resident in that RAM and `load_overlay()`s it at the section base.
* `unload_overlapping_overlays(ram, size)` — like `unload_overlays`, but permits
  the *partial* overlap a bank swap produces (the old one asserts).
* `load_function_bank(ram, size, entries, count)` / `is_function_bank_loaded()`
  — the app supplies a bank that is not a section of this ELF;
  `unload_overlapping_overlays` drops it too, so reloading overlay C later
  cleanly evicts the bank.
* `set_streamed_dma_hook()` — the app's hook, called after the section scan.

`app/src/bank_overlays.cpp` installs the hook and maps
`(rom_offset, ram)` → record using `kBankRecords` from `bank_funcs.inc`; on a
match it calls `load_function_bank()` with the record's 40/96/68 functions.

**Bug found on the way (worth remembering):** the first version compared the
*chunk's* destination against the section's loaded base. The game copies in
0x200-byte chunks, so chunk 2 looked like a new load: overlay A was re-registered
at `0x800E9E20`, `0x800EA020`, … and the real load unloaded — boot died at once
with a NULL function pointer in overlay B. The "already resident" test must be
per-section (`is_section_loaded`), and the load address must be the section
base, not the chunk destination.

## 4. Result

| check | session 32 | now |
|---|---|---|
| 170s unattended | exit 134, dl 2770 at t≈94.8s | **exit 0, dl 4850 at t≈168s** (~29fps) |
| bank swap at t≈95.1s | `streamed function stub called @ 0x801AD5C0` | `[bank] loading overlay record rom=0xE4910/0xEBBD0/0xFA600` (40/96/68 funcs) |
| render after the swap | crash | night castle scene, `docs/proofs/native-post-bankswap-present2805.png` (present 2805) |
| 15s run | exit 0, 400 dl at 14233ms | exit 0, ~370 dl at 13.5s (run-to-run/GPU variance; not chased) |
| 5s run | exit 0 | exit 0 |
| `build-null`, `build-wasm` | clean | clean |

Capture recipe used (unchanged):

```sh
OGRE_NO_AUDIO=1 OGRE_CAPTURE_PRESENT=/tmp/post OGRE_CAPTURE_AFTER=2800 \
  OGRE_EXIT_AFTER_MS=103000 ./build-app/ogrebattle64
# sips -s format png /tmp/post.2805.ppm --out docs/proofs/native-post-bankswap-present2805.png
```

## 5. Build order (new step)

```sh
make recomp        # main unit -> RecompiledFuncs/
make bank-recomp   # bank unit -> build/ogrebank.elf, BankFuncs/, app/src/bank_funcs.inc
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release && cmake --build build-app -j
```

`BankFuncs/` and `app/src/bank_funcs.inc` are generated and now gitignored.

## 6. Files changed (this session)

* `config-bank.yaml`, `config-bank.toml`, `tools/gen_bank_funcs.py` (new).
* `Makefile` — `bank` / `bank-split` / `bank-recomp`.
* `app/CMakeLists.txt` — `ogrebattle64_bank` static lib, `bank_overlays.cpp`.
* `app/src/bank_overlays.{cpp,hpp}` (new), `app/src/bank_funcs.inc` (generated).
* `app/src/main.cpp`, `app/src/main_web.cpp` — call `register_bank_overlays()`.
* `tools/N64ModernRuntime/librecomp/include/librecomp/overlays.hpp`,
  `.../src/overlays.cpp`, `.../src/pi.cpp` — bank API + DMA hook.
* `n64modernruntime-ob64.patch` — regenerated.
* `.gitignore`, `docs/guides/app-build.md`, `PLAN.md`, `docs/DECISIONS.md`,
  `docs/proofs/native-post-bankswap-present2805.png`, this file.

## 7. Next

1. **Compile the rest of the banks.** Only bank 4's records are recompiled;
   banks 0/1/2/3/6/7/8/9/10 still fall to the streamed stub when first loaded.
   Adding the remaining records to `config-bank.yaml` (ROM gap
   `0x66E30..0x1CE040` plus `0x1F0A00..0x281830`) and re-running
   `make bank-recomp` is mechanical now, but each record needs its code/data
   split from the table, and splat mis-detects function boundaries in places
   (records 7..10 are much larger: 0x43530 / 0x99D0 / 0x173E0 / 0x230E0 bytes).
2. **The bank swap is currently "latest load wins".** It does not consult the
   game's bank descriptors, so a DMA that happens to match a record's ROM/delta
   swaps it in. Reading the descriptor lists out of RDRAM (they are plain data
   at `0x800A86B8`+) would make the swap authoritative and let it unload exactly
   the outgoing bank.
3. Then re-run the 170s run and find the next wall. `OGRE_DEBUG_TRACES=1` now
   also logs `[overlays] bank swap:` lines.
4. Still open from sessions 29–32: `osViFade` unemulated (fades snap), the
   runtime blocking the game start thread on `SDL_OpenAudioDevice` on some hosts
   (`OGRE_NO_AUDIO=1`), and the session-31 idle-stall oddity.

---

# Addendum — the attract loop's second variant (the "unit info" screen)

The attract loop the user described is title → "lore" story movie → title → a
*variant* screen; the current run happens to pick "unit info". Unattended, that
variant was **bouncing straight back to the title**. This addendum covers finding
and fixing it.

## 8. Scene `0x0C`: what the game asked for

The scene dispatcher (asm/1060.s @0x80075E50) stores the scene id at
`D_800E810E` and the scene descriptor at `D_800E8294`; the descriptor's `+0x10`
word is the record mask handed to the loader `func_800761E4`.
`app/src/bank_overlays.cpp` now dumps all three the first time it meets an
uncompiled record:

```
[bank] UNCOMPILED streamed record 10: rom=0x1F0A00 ram=0x801AD5C0 size=0x230E0
[bank]   scene=0x000C descriptor=0x8018FB58 record mask=0x00003C00
... records 11, 12, 13 ...
[overlays] streamed function stub called @ 0x801E6FD0 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE3E8 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE600 (not yet loaded)
[overlays] streamed function stub called @ 0x801EE4A0 (not yet loaded)
```

`mask=0x3C00` = records **10, 11, 12, 13**; `func_80076430` matches it to
descriptor id 2 (`{0,2,4,10,11,12,13,14}`). Those four are now **bank unit C**
(`config-bankC.yaml`, `BankCFuncs/`, 848 functions).

## 9. The second streaming mechanism: a per-record code *arena*

Registering 10–13 was not enough — the same four calls still stubbed. Two
diagnostics settled it:

* the stub path now dumps the first words at the address, and they were **real
  MIPS code** (`lui/addiu/jr $ra`, function prologues);
* a DMA trace filtered to the arena showed the code arriving by PI DMA:
  `dram=0x801E6FD0 dev=0x0023B1F0 size=0x200`.

So a segment-table record is only the **resident part** of an overlay. The
record's `ram_end` word (`+0x04`) is much larger than code+data+bss: the space
above the record is an arena the game fills on demand from code modules in the
ROM gap between that record's `rom_end` and the next record's `rom_start`. For
record 10:

| module | ROM | size | RAM |
|---|---|---|---|
| table record 10 | `0x1F0A00` | `0x230E0` | `0x801AD5C0` |
| `bankRec10a` | `0x213AE0` | `0x16770` | `0x801D0860` |
| `bankRec10b` | `0x23B1F0` | `0x09580` | `0x801E6FD0` |

All four stubs are in `bankRec10b`. Both modules are plain 0x200-chunk copies, so
they are compiled as ordinary RAM-disjoint sections of unit C (147 + 90
functions) and registered by the same DMA hook. `tools/gen_bank_syms.py` handles
splat leaving 361 `D_ovlC_*` data labels referenced-but-undefined in those `asm`
sections (absolute linker definitions; the name encodes the VRAM).

Two latent bugs were fixed on the way:

* `is_function_bank_loaded` keyed on the RAM address, but records 6 and 10 (and
  7, 10) load at the *same* `0x801AD5C0` — record 10 was being skipped as
  "already loaded". It is keyed on ROM start now.
* the diagnostics now know overlays C (record 15) and units A/C are compiled, so
  they only report genuinely uncompiled records.

## 10. Debugging knobs (new)

Waiting ~6 minutes for scene `0x0C` made iteration painful, so:

| knob | effect |
|---|---|
| `OGRE_SPEED=<n>` | scales the emulated clock (CPU counter **and** VI retrace schedule) by `n` (1..64). A timed attract sequence completes in `1/n` of the wall time. `OGRE_SPEED=8` reaches scene `0x0C` in ~42s wall instead of ~360s. |
| `OGRE_FORCE_SCENE=<hex>` (+ `OGRE_FORCE_SCENE_AFTER_MS`, default 3000) | pokes the attract scene id `*(u16*)(D_800C4BBC+4)` until `D_800E810E` reports the scene active, so a run switches to that scene as soon as the scene's own state machine allows. |
| `OGRE_CAPTURE_EVERY=<n>` | with `OGRE_CAPTURE_PRESENT`, captures only every nth present — a long run becomes a slideshow instead of one 3 MB PPM per frame. |

## 11. Result

| check | before | now |
|---|---|---|
| scene `0x0C` stubs | 4 | **0** |
| unit-info screen | bounced to the title | **renders and advances** — `docs/proofs/native-unit-info-dragon-tamer.png`, `docs/proofs/native-unit-info-griffin.png` |
| records armed | 7 / 1052 functions | **9 / 1289 functions** |
| natural unattended loop | — | `OGRE_SPEED=4`, 280 s wall ≈ 1100 s game: **exit 0, 29080 display lists, 13 bank/arena loads, 0 stubs**, and the unit-info screen comes up on its own — `docs/proofs/native-unit-info-fighter-natural.png` |
| `build-null`, `build-wasm` | clean | clean |

## 12. Next after this addendum

1. The arena modules mean **compiling a table record is not enough**: each
   record's arena modules (ROM gap to the next record) must be compiled too.
   The DMA hook already registers them; the `[bank] loading overlay record …`
   logs name them, and `kAllStreamedRecords` names uncompiled table records.
2. Records 0, 1, 4, 5, 14, 16, 17, 18 and their arena modules are still
   uncompiled (0/1/17 share `0x80197B90` and need units of their own).

---

# Addendum 2 — New Game / Tutorial (scene `0x18` and `0x02`)

Pressing Start on the title screen opens the New Game / Tutorial menu; selecting
either crashed the game (SIGSEGV). This is **partly** the unmapped banks, and
partly a second, structural problem.

## 13. Which banks the menu path asks for

`OGRE_TAP_MS=700` (a Start tap) reaches it in seconds with `OGRE_SPEED=8`, and
the uncompiled-record diagnostic named both scenes:

```
[bank] UNCOMPILED streamed record 1: rom=0x06E680 ram=0x80197B90 size=0x2C20
[bank]   scene=0x0018 descriptor=0x8018FDC0 record mask=0x00000002
...
[bank] UNCOMPILED streamed record 0:  rom=0x066E30 ram=0x80197B90 size=0x2AF0
[bank]   scene=0x0002 descriptor=0x8018FC50 record mask=0x00004001
[bank] UNCOMPILED streamed record 14: rom=0x281830 ram=0x802258B0 size=0x5370
```

* scene `0x18` (the title menu) → **record 1** (descriptor id 0, `{2,1}`)
* scene `0x02` (New Game / Tutorial) → **records 0 and 14** (descriptor id 3,
  `{0,14}`)

Both are now compiled: **bank unit D** (record 1) and **bank unit E** (record 0)
— they load at `0x80197B90` like record 2, so each needs its own unit — and
record 14 joined unit C (RAM-disjoint from its records). The app now arms
**12 records / 1398 functions** across units A, C, D, E.

A debug aid added here: installing `SIGSEGV`/`SIGBUS`/`SIGABRT` handlers that
dump the runtime's shadow call chain, because a cross-bank crash dies with a raw
segfault and the Release build omits frame pointers (lldb's `bt` shows only
frame #0).

## 14. The crash that remains: compile-time-bound cross-bank calls

Compiling records 0/1/14 was necessary but not sufficient. lldb shows the fault
inside **overlay C's** recompiled body while a *different* bank is resident:

```
* thread #54, name = 'N64 Thread 3', stop reason = EXC_BAD_ACCESS (address=0x8fded000)
    frame #0: ogrebattle64`func_801989AC + 1062
```

`func_801989AC` belongs to the **main unit's overlay C** (`streamedC`). N64Recomp
compiles a `jal` to a function it knows to a *direct* C call, so a call from
resident code (overlay B, or main) into overlay C's address range is bound at
build time to C's body:

```
RecompiledFuncs/funcs_1.c:  func_80178920 (overlay B)
    // 0x8017892C: jal 0x80198D28
    func_801989AC(rdram, ctx);      <- bound to overlay C, not the resident bank
```

Scanning the main unit's generated C for calls from outside
`[0x80197B90, 0x801BA550)` (overlay C's range) into it gives **58 call edges to
53 distinct targets** — every one of them must dispatch at runtime, because the
function at that address depends on which bank is resident. Patching that one
`jal 0x80198D28` call to `LOOKUP_FUNC` moved the crash (to a display-list write
in `func_800737A0`), confirming the mechanism: the runtime lookup returned the
stub because record 1's disassembly has no entry at `0x80198D28` (splat merged it
into `func_ovlD_80198A6C`).

So two things are needed to finish the menu path:

1. **Route calls in the main unit that cross into a swappable bank range through
   `get_function`.** A post-step over `RecompiledFuncs/*.c` (like
   `tools/fix_cross_overlay_labels.sh`) can rewrite
   `func_XXXXXXXX(rdram, ctx)` → `LOOKUP_FUNC(0xXXXXXXXX)(rdram, ctx)` when the
   callee is in a bank-swappable range and the caller is not in the same bank.
   The alternative — moving `streamedC` (and A/B) out of the main ELF into bank
   units — removes the bindings by construction but is a larger change.
2. **Seed each bank unit's disassembly with the cross-bank entry points**, so
   every target of those 53 edges is a real function entry in whichever bank can
   be resident there. A per-unit `symbol_addrs` file generated from the main
   unit's cross-bank targets does this; splat then emits `func_<vram>` entries
   that N64Recomp registers, and `get_function` resolves them.

Until then, scene `0x18`/`0x02` still segfault, but the attract loop (lore and
unit-info) is unaffected: a 130 s `OGRE_SPEED=4` run is exit 0 with 0 stubs and
0 crashes.


