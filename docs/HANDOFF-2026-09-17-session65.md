# Handoff — 2026-09-17, session 65: **the map's sprites were a linker artifact** — unit M's data labels were 8 bytes high

> **Read §10 first if you are here for save/load.** The developer corrected the
> premise at the end of this session: **OB64's save is a battery-backed cartridge
> save (SRAM/FlashRAM/EEPROM), *not* a Controller Pak.** The Controller Pak — and
> everything §9 implements — is the **copy/backup** device (the strings
> `Controller Pak Menu`, `1 note 25 pages to save.`, `Data loaded to Game Pak.`).
> The actual blocker for the map's R → Save is therefore
> `entry.save_type = recomp::SaveType::None` in `app/src/main.cpp`.

## Goal and result

**Goal (developer):** continue session 64, whose top lead was renderer-side (the
map's party sheet "declares `line = 8` for a 128-byte row"). "Let me know if you
need help with info, screenshots or dumps."

**Result: the wall is down, and it was never a renderer or game-data problem.**

* **Session 64's top lead is withdrawn** (§1). For a 32-bit RGBA texture the RDP
  keeps **2 bytes per texel per TMEM half**, so `line = 8` is 64 bytes per
  half-row = **32 texels** — exactly the sprite sheet's row. RT64's loader and
  sampler implement the same arithmetic, and `G_LOADBLOCK`'s `dxt = 0x80` is 16
  source words = 128 source bytes per TMEM row. Nothing was wrong with `line`.
* **The real defect is in the port's *build*: unit M's data symbols were linked 8
  bytes above their ROM addresses** (§2). Every `lui/%lo` pair that reads a data
  label therefore read 8 bytes too high. The sprite table's base is
  `D_ovlM_801A6FD8`; the port's code read `0x801A6FE0`, **shifting every sprite
  descriptor by one entry**. That is why the party drew a 16x11 crop of the 32x32
  knight sheet, why the shadow drew as a 144x23 band of repeated ellipses, why
  the cursor was a row of eight small arrows, and why the date panel was a
  `MONTH` sliver next to a black bar showing the wrong month (`Flama` instead of
  `Sombra`).
* **Fixed in the config, not in generated code** (§3): the `asm` subsegment now
  ends at ROM `0x85840` so the assembler's 16-byte `.text` padding is real ROM
  bytes. The map now draws the party, its shadow, the cursor and the full
  `MONTH/DATE | Sombra | 1` plate (`docs/proofs/native-newgame-map-scene.png`,
  replaced; the old render kept as `docs/proofs/map-sprites-before-offby8.png`).
* **New guard + audit** (§4): `tools/elfcheck.py` / `make elf-rom-check` compares
  every linked ELF byte-for-byte with the ROM and checks every address-named
  symbol; `make bank-recomp` runs it before recompiling. After the fix **all 13
  bank ELFs and the main ELF are byte-identical to the ROM** (session 64 could
  not have seen this: its lead was about the *renderer*).

## 1. Withdrawn: the `line = 8` "half the sheet's row" lead

Session 64 §4/§4b read `line = 8` as 8×8 = 64 bytes = 16 RGBA32 texels and
concluded the sampler walks half the sheet's 128-byte row. **For a 32-bit RGBA
texture that is the wrong model of TMEM.** The RDP splits each texel across the
two TMEM halves: 2 bytes in the low half, 2 in the high, and the tile's `line`
(in 64-bit words) is the row stride of *each* half.

* angrylion `rdp/tmem.c` `get_tmem_idx`: `tbase = (tile.line * t) + tile.tmem`
  (64-bit words), `sshorts = s` for 16/32-bit, `tidx = (tbase << 2) + sshorts`
  (16-bit words), with the odd-row `^= 2` swap. For `line = 8` that is 64 bytes
  per half-row = **32 texels** in each half.
* RT64's sampler (`src/shaders/TextureDecoder.hlsli` `sampleTMEMWithConvert`) is
  the same: `tmemShift = 2` for RGBA32, `pixelAddress = y*stride + ((x<<2)>>1)`
  with `stride = tile.line << 3`, and the high half read with `| 0x800`.
* RT64's `LOADBLOCK` (`src/hle/rt64_rdp.cpp` `loadBlockOperation`) advances one
  TMEM row every `0x800 / dxt` 64-bit words: `dxt = 0x80` → 16 words = **128
  source bytes** = the sheet's row, written into `line = 8` = 64-byte TMEM rows.
  Load and sample agree exactly.

Session 64's offline "declared-stride" render modelled the sheet as linear 4-byte
texels in RDRAM, which is not the TMEM layout, so the fracture it showed is an
artifact of that model. The hit ratio it used to argue for a 16-bit read
(`masks = 3` wrapping at 8, `line = 2`, a 7-texel window) is equally explained by
32-bit **because call 1 is the 16x11 shadow, not the knight** (§2).

**No renderer change was needed and none was made.** The GLideN64 leads
(`enableTexCoordBounds`, `hack_Ogre64`) may still be relevant to other 2D
elements; they are not needed for the party, cursor or panel.

## 2. The actual bug: unit M's data labels were +8, so every data read was +8

### The evidence chain

1. **The port's rects were the *wrong sprites*, sized as their neighbours.**
   `OGRE_DL_DECODE=all` on a forced scene `0x05` (before the fix) showed:
   * party draw, `a2 = 11` → `TEXRECT ulx=568 uly=476 lrx=1144 lry=568`
     = (142,119)-(286,142) = **144x23** from `state[+4]+0x1068`;
   * party draw, `a2 = 10` → `TEXRECT ulx=536 uly=380 lrx=600 lry=424`
     = (134,95)-(150,106) = **16x11** from the composited sheet;
   * date plate, `a2 = 12` → `TEXRECT ulx=600 uly=812 lrx=664 lry=908` = **16x24**
     of the 144x24 CI plate.
2. **The runtime sprite table said those sizes really were entries 11/10/12.**
   Reading the port's own RDRAM dumps (`/tmp/map-real.bin`, `/tmp/map3.bin`) as
   guest bytes at `0x801A6FE0` gives entry 10 `(16,11)`, 11 `(144,23)`,
   12 `(16,24)` — i.e. the port's code and data agreed with each other.
3. **The ROM does not.** The builder `func_ovlM_8019F83C` builds the table
   address at `0x8019F870`/`0x8019F874`; the **raw ROM** word at ROM `0x7E804` is
   `24426FD8` (`addiu v0, v0, 0x6FD8`), while the **bank ELF** the recompiler
   reads had `24426FE0`. `build/bankM/asm/79750.s` *names* it correctly
   (`%lo(D_ovlM_801A6FD8)`) but `mips-linux-gnu-nm` defined
   `D_ovlM_801A6FD8` at **`0x801A6FE0`** — its own name + 8.
4. **The whole `.data` subsegment was +8.** `nm` audit of every unit: bank M had
   **150 of 480** address-named symbols 8 bytes high; the other 12 units and the
   main ELF had **0**.
5. **Why.** `mips-linux-gnu-as` aligns `.text` to 16 and pads the section to that
   boundary (verified with a two-instruction test file: size 0x10, `2**4`). The
   config's code/data boundary is ROM `0x85838` = RAM `0x801A68A8`, which is
   8-byte aligned, so the `asm` subsegment (0xC0E8 bytes) assembled to 0xC0F0 and
   the linker placed `[0x85838, data]` 8 bytes high. Splat's `align`/`subalign`
   knobs do not help (the pad is *inside* the input section) — tried and reverted.
6. **What the ROM's table actually is** (read at RAM `0x801A6FD8`, ROM `0x85F68`,
   identical in both dumps and in the ROM):

   | idx | entry | who |
   |---|---|---|
   | 0 | (27, 8) | |
   | 1 | (18, 22) | |
   | 2 | (64, 13) | |
   | 3 | (25, 8) | |
   | 4 | (35, 8, 0, 8) | |
   | 5 | (6, 6) | |
   | 6 | (16, 16) | map cursor (`func_ovlM_801A103C` @`0x801A1C50`, `a2=6`) |
   | 7 | (24, 23) | |
   | 8 | (24, 23, 32, 0) | |
   | 9 | (1, 23, 63, 0) | |
   | 10 | **(32, 32, 0, 0)** | **the party** (`func_ovlM_801A2A7C` @`0x801A2D08`) |
   | 11 | **(16, 11, 0, 0)** | **the party's shadow** (@`0x801A2C34`) |
   | 12 | **(144, 23, 0, 0)** | **the date plate** (`func_ovlM_801A2410` @`0x801A2480`) |
   | 13 | (16, 24, 16, 0) | |
   | 14 | (16, 24, 0, 0) | |
   | 15 | (16, 24, 16, 0) | |
   | 16 | (16, 24, 16, 0) | |

   Fields are `(dstW, dstH, srcU, srcV)`: `func_ovlM_8019F83C` emits
   `G_TEXRECT` from `(x, y)` to `(x+F0, y+F1)` and `G_RDPHALF_1 = (F2<<5<<16)|(F3<<5)`
   with `dsdx = dtdy = 1.0`, so the first pair is the drawn size in pixels and the
   second the source texel offset.

### What that one shift ruined

Everything drawn through the builder or read from that data half, because every
table entry became its neighbour's. Verified by the emitted rects and the render:

* **the party** — `a2 = 10` drew 16x11 (the shadow's entry) from the 32x32 knight
  sheet: the "rect with random colors";
* **the shadow** — `a2 = 11` drew 144x23 (the date plate's entry) from the 16x11
  shadow, and with `masks = 3` wrap the tile repeated 18 times: the "multiple
  shadow sprites" / the row of ellipses;
* **the cursor** — entry 6 is `(16,16)`; the shifted table's entry 6 was
  `(24,23)`, so the map cursor drew the wrong sprite at the wrong size;
* **the date plate** — `a2 = 12` drew 16x24 instead of 144x23, and with
  `masks = 2` the 16-px sliver repeated every 4 texels: the "striped box";
* **the month/date strings** — `*(0x801A78BC + idx*4)` and the name table at
  `0x801A6F00` were also +8, so the panel printed `Flama` where retail prints
  `Sombra` (the fixed render prints `Sombra 1`, matching the retail screenshot).

Expected but **not yet verified** (the forced entry has no army data): the unit
markers and the route of dots, which `func_ovlM_801A2D84` builds from tables in
the same data half. Re-check these on the natural map (§7).

## 3. The fix

`config-bankM.yaml`'s data subsegment now starts at **`0x85840`**, not
`0x85838`:

```yaml
    subsegments:
      - [0x79750, asm]
      - [0x85840, data]
```

The 8 bytes at ROM `0x85838` are zeros and **no code references RAM
`0x801A68A8`** (the symbol count dropped 480 → 479 and the rebuild is clean), so
they are the module's own alignment padding: absorbing them into the `asm`
subsegment makes its size 0xC0F0, a multiple of 16, and the assembler emits no
pad. Verified:

* `mips-linux-gnu-objdump build/bankM.elf --start-address=0x8019F870` now shows
  `24426fd8` — identical to the ROM;
* `tools/elfcheck.py --syms`: 0 differing bytes for every ELF, 0 misplaced
  symbols;
* `BankMFuncs/*.c` regenerated with `ctx->r2 = ADD32(ctx->r2, 0X6FD8)`.

**Nothing outside `config-bankM.yaml` and the regenerated trees changed.** No
generated `Bank*Funcs/` or `RecompiledFuncs/` file was edited, so there was no
probe to revert (§6 of AGENTS: the generated trees are gitignored and were
regenerated by `make bank-recomp`).

## 4. New tool: `tools/elfcheck.py` / `make elf-rom-check`

The class of bug is invisible in review: the generated C looks plausible and only
the *immediate* is wrong, and only for labels that live after a misaligned
subsegment boundary. The tool compares each ELF's `CONTENTS` sections against the
ROM at the same LMA and (`--syms`) asserts that every symbol whose name ends in
eight hex digits is defined at that address — which is exactly the property that
broke, and the project's `ovl<U>_$VRAM` naming makes it checkable for free.

```
$ make elf-rom-check
bankM.elf: 0 differing bytes of 553504
bankM.elf: 479 address-named symbols all at their named address
ogrebattle64.elf: 0 differing bytes of 41943040
ogrebattle64.elf: 4224 address-named symbols all at their named address
```

`make bank-recomp` now runs it (quietly) before recompiling, so a misaligned
subsegment fails the build instead of producing a scene that draws the wrong
sprite.

## 5. Verification (what was run)

* `make bank` / `make bank-recomp` (all 13 units re-split, re-linked,
  re-recompiled; `cross_bank.py check-banks` passes).
* `cmake --build build-app -j8`.
* Forced scene `0x05`, `OGRE_PRESENT_ALWAYS=1 OGRE_CAPTURE_PRESENT=…`:
  before/after captures. After the fix the party is a 32x32 knight with a 16x11
  shadow, the cursor is one 24x23 arrow, and the panel is the full plate with
  `Sombra` and `1` — matching `docs/proofs/map-reference/retail-map-screen.png`.
* `OGRE_DL_DECODE=all` on the same scene: the party rects are now
  `ulx=304 uly=380 lrx=432 lry=508` (**32x32**, the knight sheet at
  `state[+0x34] + frame*0x1000`) and `ulx=336 uly=476 lrx=400 lry=520`
  (**16x11**, `state[+4]+0x1068`), and the plate is
  `ulx=600 uly=812 lrx=1176 lry=904` (**144x23**).
* Title regression run (`OGRE_SCENE=title`): unchanged.
* `make elf-rom-check` for every unit, before and after.

## 6. Files changed

* `config-bankM.yaml` — the data subsegment starts at `0x85840`; the comment
  records the whole mechanism and symptom chain.
* `tools/elfcheck.py` (**new**) — ELF-vs-ROM byte comparison + address-named
  symbol audit.
* `Makefile` — `elf-rom-check` target; `bank-recomp` guards with it.
* `docs/proofs/native-newgame-map-scene.png` (**replaced** — the old file was the
  broken render; it is kept as `docs/proofs/map-sprites-before-offby8.png`), new
  `docs/proofs/map-sprites-before-offby8.png`.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/guides/app-build.md` (the toolkit entry
  for `make elf-rom-check`), this handoff.

Generated/ignored, regenerated by the normal targets: `build/bankM.*`,
`Bank*Funcs/`, `app/src/bank_funcs.inc`. `git status --short` shows only
`config-bankM.yaml`, `Makefile`, `tools/elfcheck.py`, the two proof PNGs, the
docs, and the pre-existing `tools/RT64` submodule modification.

## 7. Next leads

1. **The natural map still needs a look with the fixed build** (developer drive,
   `OGRE_KEY_*` dump): the forced entry has no army data, so the *route dots*,
   unit markers, mission pins and the cursor's position were all drawn with the
   same shifted tables before. Session 64's "the route dots are in the wrong
   place, the lower left" and "the panel is absent" are now plausibly the same
   one-entry shift — re-check them. A fresh `dump`/`save` at the real map
   (`docs/guides/app-build.md` → "Checkpoints") is still the artifact to take.
   **Old checkpoints are invalid** (the build id changed).
2. **Re-check the other scenes that use the builder.** The builder is in unit M
   (scene `0x05`) only, but the same *linker* hazard applies to any unit whose
   code/data boundary is not 16-byte aligned — `make elf-rom-check` says all
   units are clean *now*; keep it in the loop when a boundary moves.
3. Session 64's remaining renderer leads are now lower priority, but still open
   and still cheap: RT64's handling of a `G_TEXRECT` whose tile window is much
   larger than the rect, and GLideN64's `enableTexCoordBounds`.
4. `docs/scenes.md`/`docs/symbols.md` should gain the corrected sprite table
   (`D_ovlM_801A6FD8`, 8-byte entries `(w, h, u, v)`) and the fact that
   `func_ovlM_801A2410` is the date panel (plate + month name + date digits) while
   `func_ovlM_801A2A7C` is the party + shadow.

## 8. Addendum (same session) — the developer's drive confirms the fix, and two new walls

**Confirmation (developer, in-game):** *"the scene looks perfect!"* — the
screenshot they sent shows the party knight, the 4-dot route to the next
location, the red pin, the crossed swords (next mission), the cursor and the full
`MONTH/DATE | Sombra | 1` panel, all correctly placed; the elements session 64 §8b
listed as "present on the real map and absent from forced captures" are all in
place now. So §7.1's "expected but not yet verified" marker for the marker/route
layer is resolved: those tables live in the same data half and the one-entry shift
was their cause too. They also report that moving the cursor onto the next mission
and selecting it **advances the game out of the map into the next scene**, and
that it crashes when the mission actually starts — that is the next wall.

**Artifacts taken (developer keys):** `/tmp/map-fixed.bin` (raw 8 MiB dump) and
`/tmp/map-fixed.ckpt` (checkpoint), both at the real map. The dump checks out:
`0x800E810E = 0x0005`, `0x800E8294 = 0x8018FD70`, `*(0x80197B18) = 0x801F1570`,
and the sprite table at `0x801A6FD8` reads `(27,8)` with entry 10 = `(32,32)` —
the ROM's table, as §2 says.

**Wall A — the mission-start crash.** Not investigated here (the developer
deferred it). Reaching it needs a playthrough to the map and the mission select;
the crash output was not captured. **Next session: ask for the `[crash]`/`[snap]`
block (or a run log) at the moment the mission starts**, then use the scene/step
it dies on (`c`) and the faulting guest address. The map's exit is a
`D_800C4C26` store like every other transition, so `tools/scenemap.py
transitions` should name the target scene first.

**Wall B — checkpoints are process-local, and the intended workflow needs them
not to be.** The checkpoint the developer saved cannot be loaded by another run:
`load` reports success and `c` shows scene `0x05`, then the process dies
deterministically in `do_send` (`SIGSEGV`, `do_send + 0x54C`, garbage host
address). A `save` + `load` pair **in the same run** at the same map works
(verified). Cause: the image contains **host pointers** — `OSThread::context` is
declared "an actual pointer regardless of platform" and written into guest RDRAM,
one per N64 thread (7 found in `/tmp/map-fixed.bin` at `OSThread::context` =
`base+0x20`, each with a plausible `id`/`sp`/`queue` around it), and the runtime's scheduler dereferences them after the
rewind. So `/tmp/map-real.ckpt`, `/tmp/map-fixed.ckpt` and any checkpoint handed
between sessions are **inert**; the recipe is `save` and `load` in one run.
Fixing it means rebuilding the runtime's host state from the restored guest state
(a per-thread `OSThread` → `UltraThreadContext*` registry installed by
`osCreateThread`, rebound after `read_checkpoint`, plus a reset of the scheduler's
blocked-thread host state) — a real milestone, worth doing because checkpoints
are the project's main iteration accelerator. Recorded in `docs/DECISIONS.md`
and in `docs/guides/app-build.md` → "Checkpoints".

**One operational note:** the live console's watched file is global
(`/tmp/ogre-console.txt`), so a *second* running instance silently eats the
commands meant for yours. Use `OGRE_CONSOLE_FILE=/tmp/<name>.txt` per instance
(this cost a few runs here while the developer's game was still open).

## 9. Addendum 2 — the **copy/backup** device (Controller Pak) is implemented; it is not the save path (§10)

**What landed (this session, after §8):** the runtime now has a real Controller
Pak **copy/backup** device, and the game is told a pak is inserted. (This is the
`Data loaded to Game Pak.` / `Data saved to Controller Pak.` feature — §10.)

* `tools/N64ModernRuntime/librecomp/src/pak.cpp` (in the gitignored runtime tree;
  captured in `n64modernruntime-ob64.patch`) implements a **flat 32 KiB image**
  accessed in 32-byte blocks, with block addresses `>= 1024` (byte `0x8000`, the
  bank/enable register) **ignored** — which is why libultra's own bank probe
  (`__osRepairPackId`) reports `banks == 1`, the value its inode/directory layout
  expects. The model and the fresh-pak format follow mupen64plus
  (`src/device/controllers/paks/mempak.c`) and cen64 (`si/pak.c`), so the image is
  interchangeable with emulator `.mpk` dumps. Blocks 1..6 (the ID block, its three
  backups, the label/reserved slots) are write-protected unless `force == 1` —
  `PFS_LABEL_AREA = 7` and `PFS_FORCE = 1`, **read out of this ROM's own**
  `__osContRamWrite` (`0x80097DC0`: `sltiu v1,a0,7`).
* The image is `<config>/saves/<game id>.mpk`
  (`~/Library/Application Support/ogrebattle64/saves/ogrebattle64-us-rev1.mpk`),
  loaded at first use and written back with the runtime's temp+`.bak` helpers; a
  missing or wrong-sized file is formatted like mupen's `format_mempak` (ID at
  page-0 block 1 + copies at 3/4/6, the page-1 index table with its checksum,
  the page-2 mirror).
* **Only three functions needed bridging** — the game's PFS is libultra the ROM
  already contains and `make recomp` already compiles, so there was no filesystem
  to port. The three that touch the SI/PIF hardware (which the port does not
  emulate: KSEG1 MMIO is aliased into a scratch page, `recomp.h`) are now
  reimplemented and bound: `__osContRamRead` `0x80097BD0`,
  `__osContRamWrite` `0x80097DC0`, `__osPfsGetStatus` `0x80096EC0`
  (`symbol_addrs.txt` + N64Recomp's `reimplemented_funcs`; the generated C calls
  `*_recomp` at all six sites and the old bodies are gone).
* `app/src/sdl_platform.cpp` reports `Pak::ControllerPak` for controller 0
  (a new enum value in `ultramodern/include/ultramodern/input.hpp`), so
  `osContInit` sets `CONT_CARD_ON` and `__osMotorAccess` still only rumbles for
  `RumblePak`.

**Where the game's save flow actually goes (new, instruction-level):**

* The Controller Pak menu **is not in the map module**. It is in **unit H** (the
  form/UI module, RAM `0x8019A7C0`): `func_ovlH_8019C69C` calls the pak API
  (`0x8008AC70`, `0x8008A6A0`, …) and the three pak-menu screens are
  `func_ovlH_8019DBA4`/`0x8019DC88`/`0x8019DEB4` (the "No Data"/"Game Data"
  strings exist **only** in unit H's ROM half; unit M has only "No Data").
* It is entered through the **scene descriptor's extra callbacks**. Scene `0x07`'s
  descriptor (`0x8018FDAC`) has `+0x14 = func_8017BA60`, **`+0x18 =
  func_8017BB28`**, `+0x1C = func_8017BB54`; `func_8017BB28` is a one-line
  wrapper: `jal 0x8019C69C` — i.e. **scene `0x07`'s `+0x18` callback is the
  Controller Pak menu**, and `+0x14` is the form (it loads unit H and calls
  `func_ovlH_8019A884`). Scene `0x05`'s (the map's) callbacks are the generic
  `0x8017B6D0`/`0x8017B858`/`0x8017B9C8` — no pak entry.
* **The map module (unit M) never calls the pak API**: of its 13 calls into the
  main segment only cache/queue/utility helpers (`0x80093380`, `0x80093060`,
  `0x80091AB0`, …) are involved, and no module except **unit H** calls the pak API
  or the pak-command sender (`func_80089CF8`). Unit H is the *only* caller.

**Therefore, in a *forced* map entry the save cannot reach the device**, and it
does not: driving R → right×3 → A (Save) → A (slot) → A (Yes) on `OGRE_SCENE=0x05`
draws the map's slot window and "WARNING … existing data will be overwritten …
Proceed? Yes/No", then returns to the map, having — verifiably —
* called none of the three bridged device functions (lldb breakpoints on all
  three were never hit; the `[pak]` load/format line never appears),
* issued **no DMA** (with `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1`: the only
  arena DMA in the whole run is the map module's own `rom=0x79750 →
  0x8019A7C0`; unit H's `rom=0x712A0` is never streamed),
* changed **no scene** (`c` during the prompt: `scene=0x0005 pending=0x0005`),
* and called the **pak manager not at all** (host-symbol breakpoints on
  `func_8008AD60`/`func_8008A910`/`func_8008AC30`/`func_8008AC70`/`func_8008ACA0`
  never hit; `func_8008AF60`, which the dispatcher `func_80075BC0` calls on every
  scene change, *is* hit, so the manager framework is alive).

The explanation is the same one this session used for the mission markers: a
**forced entry has no army/save state**, which the save/battery path needs, and the
Controller Pak menu itself lives in `scene 0x07` — so the map's save in the real
game must route through the form module, which a forced entry never does. A
**natural** run to the map is therefore required to exercise the device (and the
title's `Load Game` entry, which appears only once a save exists).

**What to run next (developer):** play to the map naturally with this build, open
R → right×3 → A (Save) → A (slot) → A (Yes), then report (a) the on-screen
message (`Saving data.` / `Data saved to Controller Pak.` vs `Saving data has
failed.` / `Insert Controller Pak.`), (b) whether the log prints
`[pak] formatted a new Controller Pak at …` or `[pak] loaded …`, and (c) whether
`~/Library/Application Support/ogrebattle64/saves/ogrebattle64-us-rev1.mpk`
appears (32768 bytes). A real 32 KiB `.mpk` from an emulator or console can simply
be dropped at that path for the Load half.

**Addendum to §9 (same round).** Two more facts, both instruction-level, and one
open puzzle:

* The pak-presence wiring is *verifiable*: the app now prints
  `[input] controller 1: Controller Pak inserted` once per process (from
  `get_connected_device_info`, `app/src/sdl_platform.cpp`), so a run's log says
  whether the game was told a pak is inserted even when the device is never
  touched. Verified on a title run: the line appears, and `osContInit` therefore
  sets `CONT_CARD_ON` for controller 1 (the game's own controller scan at
  `0x8008A2E8` tests `type & 0x1F07 == 5` and `errno == 0`; the pak bit is the
  `status` byte the save code reads).
* Scene `0x07`'s descriptor (`0x8018FDAC`) has three extra words:
  `+0x14 = func_8017BA60` (the form: it loads unit H and calls
  `func_ovlH_8019A884`), **`+0x18 = func_8017BB28`** (`jal 0x8019C69C` — the
  Controller Pak menu), `+0x1C = func_8017BB54`. Scene `0x05`'s are the generic
  `0x8017B6D0`/`0x8017B858`/`0x8017B9C8`.
* **The puzzle:** a scan of the *whole ROM* shows that the current-descriptor
  global `0x800E8294` is loaded in exactly **8** places, all in the main segment,
  and they read only `+0x00`, `+0x04`, `+0x08`, `+0x0C` and `+0x10` — i.e. the
  documented enter/update/hook/leave/mask. **Nothing in the ROM reads
  `+0x14`/`+0x18`/`+0x1C`**, and `func_8017BB28` is referenced *only* by that
  descriptor word (no `jal` to it anywhere, and the pointer value appears exactly
  once, in the descriptor table at ROM `0x65CC4`). So either the dispatcher for
  those three words lives in a module the port does not compile, or they are not
  callbacks at all. Worth resolving with the developer's knowledge of *where* the
  Controller Pak menu appears in the game (`docs/scenes.md` row 9 says "reached
  from a later scene", and the map's R→Save is a *map* dialog): if they can say
  which screen/flow shows `Controller Pak Menu` / `Save`/`Load`/`Erase`/`Exit`,
  that names the scene to attack next.

## 10. Correction (developer, end of session 65) — the game's **save is a battery**, not a Controller Pak

The developer: *"ob64 saved the game in a battery, not in a controller pak."*

**This reframes the save/load milestone.** Everything in §9 is still true and still
useful — the Controller Pak device, the three bridged SI functions and the
presence bit are the **copy/backup** path, not the save path. The string table
says so once you look for it: `Controller Pak Menu`, `1 note 25 pages to save.`,
`Insufficient pages to copy game.`, **`Data loaded to Game Pak.`** and
`Data saved to Controller Pak.` are a *copy between the cartridge ("Game Pak")
and a pak*, exactly the "copy/backup" feature most N64 games shipped. The
*primary* save (the map's R → Save → Yes, which session 58's recon assumed was
the pak) is **battery-backed cartridge save** = SRAM/FlashRAM/EEPROM.

**So the actual blocker for the map's save is `recomp::SaveType`.** The app still
sets `entry.save_type = recomp::SaveType::None` (`app/src/main.cpp`, with a
`TODO`), so the runtime tells the game there is no cartridge save device — which
is exactly consistent with everything §9 measured on the save flow: it draws its
dialog and prompt, then issues **no** device call, **no** PI DMA, **no** scene
change and no pak-manager call. The pak device was never the gate.

**Next step (concrete, small):**
1. Identify the chip from the game's own code: look for `osEepromProbe`
   (runtime `librecomp/src/eep.cpp`), `osFlashInit`/`osFlashReadId`/`osFlashReadStatus`
   (runtime `librecomp/src/pi.cpp` handles `SaveType::Flashram`), or raw PI DMAs
   to the save window `0x08000000` (SRAM has no probe) — and see which of those
   functions the ROM actually calls (`symbol_addrs.txt` names the bridged
   libultra set; `grep` the recompiled tree for `osEepromProbe_recomp`,
   `osFlashInit_recomp`, or a `pi.cpp` save-window DMA).
2. Set `entry.save_type` to that value (`recomp::SaveType::Sram` /
   `Flashram` / `Eep16k`) and rebuild — the runtime then owns the save image in
   `<config>/saves/<game id>.bin` through the existing `save_context`
   (`pi.cpp`: `save_buffer`, `update_save_file()`, temp+`.bak`).
3. Re-run the map's R → Save → Yes (the same tap schedule used in §9) and expect
   the file to appear and the game's own "saving…/saved" message; then re-enter
   the map / restart and confirm the game *loads* it.
4. The pak work stays as-is for the copy/backup path (`Load Game` on the title is
   documented as appearing "when a Controller Pak save exists", so the title
   entry belongs to that path, not to the battery).

Also recorded in `docs/DECISIONS.md` and corrected in `PLAN.md`.
