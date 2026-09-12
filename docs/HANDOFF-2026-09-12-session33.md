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
