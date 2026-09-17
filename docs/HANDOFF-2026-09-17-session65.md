# Handoff — 2026-09-17, session 65: **the map's sprites were a linker artifact** — unit M's data labels were 8 bytes high

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
