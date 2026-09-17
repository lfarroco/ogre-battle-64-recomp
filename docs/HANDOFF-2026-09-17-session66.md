# Handoff — 2026-09-17, session 66: **OB64's save is 32 KiB of battery SRAM, and it now works end to end**

> **Read §3 if you are here for save/load.** The chip is **SRAM** (32 KiB), the
> runtime owns `<config>/saves/ogrebattle64-us-rev1.bin`, the game formats /
> repairs / reads / writes it through its own PI DMA path, and the title's
> **`Load Game`** entry reads the battery (not the Controller Pak) — verified by
> loading a converted emulator save and watching the game come up at the map.

## Goal and result

**Goal (developer):** continue session 65's save/load work with the correction
that **OB64 saves to an internal battery, not a Controller Pak**; identify the
chip and make the game's own save flow work.

**Result: the save/load feature works end to end.**

* **The chip is 32 KiB SRAM** (§1). Two independent sources: mupen64plus's per-game
  database (`SaveType=SRAM` for this ROM's CRC) and the game's own code —
  `func_8008A040` builds the save `OSPiHandle` with `baseAddress 0xA8000000`
  (physical `0x08000000`), `func_80074CF0..` read the whole image in 256-byte
  DMAs, `func_80074BF0` → `func_80074C58` write it back. No EEPROM/FlashRAM
  protocol exists in the ROM.
* **`entry.save_type = recomp::SaveType::Sram`** (`app/src/main.cpp`), and the
  game's own DMA path is routed to the runtime's save buffer (§2). The rule that
  makes it work: **the device of a game-issued PI DMA is the `OSPiHandle` the
  `OSIoMesg` carries** (`baseAddress 0xA8000000` = SRAM), not the address. The
  old handler read ROM for every transfer, so the boot save read pulled ROM
  bytes `0..0x7FFF` and the game "repaired" that garbage.
* **Verified** (§3): blank battery → formatted and written (32768 bytes, real
  `QuestOG3` structure); restart → accepted, no rewrite; garbled image → rejected
  and repaired; **a converted parallel-n64 save with progress → accepted, and the
  title's `Load Game` appears with the cursor on it**: same taps with no save run
  New Game (`0x04 → 0x02 → 0x0D`), with the save run `0x04 → 0x12` (Load Game) →
  `0x05` (the map). The developer watched it load.
* **New: `tools/sramsave.py`** imports/exports emulator wrappings of the same
  32 KiB (§4) — the parallel-n64 dump the developer supplied is 296960 bytes with
  the SRAM at `0x20800`, 32-bit byteswapped.
* **New knob `OGRE_PREF_DIR`** relocates the config/save dir, added because the
  first (sandboxed) attempt could not write `~/Library/Application Support` and a
  failed save write raises a modal **from the saving thread** (§5, open).

## 1. The chip: SRAM, established without inference

`mupen64plus.ini` (`~/Documents/RetroArch/system/Mupen64plus/`) is the emulator
database, and it exists because the save chip is not in the ROM header:

```
[EBB4B4D2808DF427AAA3085A41B8A954]
GoodName=Ogre Battle 64 - Person of Lord Caliber (U) (V1.1) [!]
CRC=0ADAECA7 B17F9795
SaveType=SRAM
Mempak=Yes
```

`assets/ogre64.z64`'s header CRC is `0ADAECA7 B17F9795`, so this is the entry.
`Mempak=Yes` is the Controller Pak **copy/backup** device of session 65, not the
save.

The game's own code agrees, at instruction level:

| what | where | evidence |
|---|---|---|
| save `OSPiHandle` | `func_8008A040`, called once from `func_80071EB0` (`0x80071F4C`) | writes `type=3` at handle+4 and **`baseAddress 0xA8000000` at handle+0xC** (`0x800BE110`), publishes it at `0x800E79AC` |
| DMA wrapper | `func_8008A0F0(devOffset, dramAddr, size, dir)` | builds the `OSIoMesg` on the stack and calls `func_8008BC40` with the handle from `0x800E79AC` |
| whole-image read | `func_80074CF0`.. (13 accessors) | read `0x8000` bytes in **256-byte DMAs at device offsets `0..0x7F00`**, cached at `*(0x800B83B8)` |
| whole-image write | `func_80074BF0` → `func_80074C58` | same loop with **direction `1`** through the function pointer stored at the malloc'd struct (`0x8008A0F0`) |
| slot geometry | `func_8007541C` / `func_80075578` (called from `func_80074AD4`) | slot `n` = `0x10 + n*0x1850` (`0x1850` = 6224) |
| magic | SRAM `0x04` and each slot's `0x10+4` | ASCII `QuestOG3` |

No `osEepromProbe`, no `osFlashInit`/`osFlashReadId`, no FlashRAM command DMA
(`0xD2000000`/`0xE1000000`-style writes) — the only PI-register code in the ROM is
libultra's own cart/raw-DMA routines and the game's PI-manager thread.

## 2. The mechanism, and why it read ROM before

The port does **not** bridge `osEPiStartDma`; the game's own DMA path runs. It
funnels through `func_8008BC40`, which sends the `OSIoMesg` to the queue at
`D_800AA408`; the runtime's `do_send` intercepts that queue and completes the
transfer inline (`librecomp/src/pi.cpp`, `init_pi_manager`).

That handler treated **every** message as a ROM read at `devAddr`:

```cpp
uint32_t dev_off = (devAddr >= recomp::rom_base) ? (devAddr - recomp::rom_base) : devAddr;
recomp::do_rom_read(rdram, dramAddr, recomp::rom_base + dev_off, size);
```

so the boot save read (`dev = 0x0, 0x100, … 0x7F00`) copied ROM bytes, and the
commit would have read ROM into the save buffer instead of writing it. The fix is
one rule, in `pi_perform_dma`:

```cpp
uint32_t handle   = MEM_W(0x14, mesg);                       // OSIoMesg.piHandle
uint32_t base_phys = handle ? MEM_W(0x0C, handle) & 0x1FFFFFFF : 0;  // OSPiHandle.baseAddress
if (base_phys == recomp::sram_base) { /* save_read / save_write */ }
else { /* ROM at devAddr, exactly as before */ }
```

Direction is `OSIoMesg.hdr.type` (`0xF` = EDMAREAD, `0x10` = EDMAWRITE) when the
handler only has the message, or `func_8008BC40`'s `a2` when it has the context.

Why this is safe for the other callers:

* The **cart** handle comes from the bridged `osCartRomInit`, whose
  `baseAddress` is `phys_to_k1(0x10000000)` = `0xB0000000` → `base_phys`
  `0x10000000`, so `phys >= rom_base` and the ROM offset is `phys - rom_base` =
  `devAddr` (the raw offset the game passes). Behaviour identical to before.
* `func_8008C360` (the AI helper at `0x8008C360`) sends with **`piHandle = 0`**
  (`sw zero, 0x14(s0)`), so it keeps the old ROM path.
* A run confirms it: **1290 ROM DMAs, 384 SRAM-window DMAs** (0x8000/0x100, the
  whole image) and **128 SRAM writes** (also the whole image), all with
  `handle=0x800BE110`.

`[save]` diagnostics were added so a run says what happened:
`[save] no save file at …; starting with a blank image`,
`[save] loaded … (32768 bytes)`,
`[save] game reads/writes the battery (SRAM) save: dev=… size=…` (first of each),
`[save] wrote …` / `[save] FAILED to write …`.

## 3. Verification (what was run)

All on `build-app/ogrebattle64` rebuilt from this tree
(`cmake --build build-app -j`), with `OGRE_PREF_DIR=/Users/momo/dev/ogre/.ogre-prefs*`
so the sandboxed runs could write their saves.

| run | result |
|---|---|
| pre-change baseline, 14 s | 12 direct `jal` sites; the boot save read went to ROM (`[pi] inline DMA dev=0x00000000` …); no save file touched |
| post-change, no file | `384` SRAM reads + `128` writes; `[save] wrote …/ogrebattle64-us-rev1.bin (32768 bytes)` |
| the written image | 32768 bytes, 1518 non-zero; `0x04` device and `0x14` slot-0 magics `QuestOG3`; slot header words `0x4046120E` (0), `0x58962A5E` (1), `0x00000000` (2), slots 3/4 zeroed. **Deterministic**: two separate blank runs produced the identical image (sha256 `ed38cfd7…`) |
| restart with it | `[save] loaded …`; the game reads it and does **not** write (accepted); file hash unchanged |
| `0xA5`-garbled whole image | the game reads, rejects, repairs and rewrites; magics restored |
| developer's parallel-n64 save | `tools/sramsave.py import` → 32768 bytes; the game accepts it, no rewrite |
| title A/B, same taps (`none×15,start`, `OGRE_TAP_SCENE=title`, `OGRE_SPEED=4`) | **no save**: `0x04 → 0x0B → 0x04 → 0x02 → 0x0D` (New Game). **with the save**: `0x04 → 0x12` (Load Game) → `0x05` (the map). Dev-confirmed: *"it worked! … the game was loaded"* |

`tools/runlog.py` on both A/B logs shows no crash and no non-gfx stub task
(`problems: none`) for the no-save run; the save run's `--check` reports **2
crash lines, and they are the known bounded-exit teardown** — the app died in
`func_8007F8E4 + 0x2F8` (the game's `main`) as `OGRE_EXIT_AFTER_MS` tore the
threads down, *after* 16 s of the loaded map (`docs/guides/app-build.md`:
"a scripted run does not unwind on purpose"). Nothing in the save path is in the
block; `[save] wrote …` had already completed. The no-save control hit the same
path without the crash, i.e. it is the documented intermittent one.

**This corrects `docs/scenes.md`** ("when a Controller Pak save exists"): the
title's `Load Game` and its default cursor read the **battery** save. The
Controller Pak remains the copy/backup device of session 65.

**Open (not investigated): a failed save write should not be able to take the
process down.** The first attempt (before `OGRE_PREF_DIR`) could not write
`~/Library/Application Support`, and `update_save_file` raised
`error_handling::message_box` **on the saving thread**; the game stalled and the
run died inside the periodic `[snap]` dump (`debug_dump_queue_snapshot`). Session
65's checkpoint crash is a different one (host pointers in the image).

**Also observed:** a load-then-play run printed `[save] wrote …` and the file
hash was **identical** afterwards — the game re-commits an unchanged image. So
"the game wrote the save" is not by itself evidence of new progress; compare the
bytes.

## 4. New tool: `tools/sramsave.py`

Every port/emulator holds the same 32 KiB of logical bytes but wraps them
differently, so a save cannot be dropped in blind (the developer supplied a
296960-byte parallel-n64 dump; the port expects exactly 32768):

```sh
tools/sramsave.py check  <file>...        # find/validate: offset, byte order, magics
tools/sramsave.py import <in> <out.bin>   # -> the port's logical 32 KiB
tools/sramsave.py export <in.bin> <out>   # -> 32-bit byteswapped (.sra-style)
```

It finds the SRAM by the `QuestOG3` magic at **any offset and either byte order**
(literal, or every 4-byte group reversed), so it handles:

* the port / mupen64plus-RetroArch `.srm`: bare 32 KiB, offset 0, "logical";
* **parallel-n64**: 296960 bytes, SRAM at **0x20800**, **byteswapped32** (the
  magic reads `seuQ3GOt`) — FlashRAM `0x20000` + EEPROM `0x800` precede it in the
  container.

It refuses a file with no magic rather than inventing an image. `import` of the
developer's save is byte-identical to the image the port then loaded.

## 5. Files changed

| file | change |
|---|---|
| `app/src/main.cpp` | `entry.save_type = recomp::SaveType::Sram` with the evidence in the comment; `OGRE_PREF_DIR` override for the runtime config dir |
| `tools/N64ModernRuntime/librecomp/src/pi.cpp` | `pi_perform_dma` (device from the `OSPiHandle`; SRAM read/write; ROM unchanged), `func_8008BC40_recomp` uses it, `[save]` diagnostics in `read_save_file`/`update_save_file`. **Gitignored tree — captured in `n64modernruntime-ob64.patch`** |
| `n64modernruntime-ob64.patch` | regenerated with `git -C tools/N64ModernRuntime diff --ignore-submodules=all` (the only changed file vs the previous patch is `librecomp/src/pi.cpp`) |
| `tools/sramsave.py` | **new** — emulator save import/export |
| `docs/guides/app-build.md` | new `OGRE_PREF_DIR` row; new "Saves — the cartridge battery (SRAM), and emulator interchange" section |
| `docs/DECISIONS.md` | durable-table row + a session-66 section |
| `docs/scenes.md` | the title's `Load Game` entry now says **battery**, with the A/B |
| `PLAN.md` | session-66 status entry; the session-65 "next: identify the chip" item resolved |
| `docs/HANDOFF-2026-09-17-session66.md` | this file |

**No probes, and no generated code changed**: nothing in `RecompiledFuncs/` or
`Bank*Funcs/` was touched, so `make recomp`/`make bank-recomp` were not needed and
there is nothing to revert. The recompile inputs (`config*.yaml`,
`symbol_addrs.txt`) are unchanged.

`git status --short` at the end of the session: `M app/src/main.cpp`,
`M n64modernruntime-ob64.patch`, `M docs/...`, `?? tools/sramsave.py`, plus the
pre-existing ` m tools/RT64`.

## 6. Next leads

1. **Play the port from the imported save** (the developer started this): the map
   loads; confirm the map's `R → Save → Yes` writes new progress (`tools/sramsave.py
   check` the file before/after and compare the byte diff), then `Load Game` it
   back.
2. **The map's Save on a forced entry still does nothing** (session 65 §9). The
   battery commit is always-resident main-segment code, so the interesting
   question is whether the *slot window* needs army state the forced entry lacks,
   or whether the pak path is what the map dialog actually calls. The game now
   has a loadable save, so a natural-map run is cheap to drive.
3. **Robustness: a failed save write takes the process down** (§3). Replacing the
   saving-thread modal with a logged failure, and/or making the periodic `[snap]`
   dump safe under a stalled game, is worth one session.
4. **`Load Game`'s slot screen** (scene `0x12`): with two populated slots, does it
   list both, and does selecting the second load it? The imported save has slot 0
   populated and slot 1 empty (`0x58962A5E`, the same header word as a blank slot
   in the port's own format); a two-slot save would exercise the list.
