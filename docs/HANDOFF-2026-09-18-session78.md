# Handoff — 2026-09-18, session 78: **the `assets/saves/*.n64` saves run as the battery**

> **Read §1-§3 if you are here for save/load.** The chip is still 32 KiB of
> battery SRAM (session 66). These four files are **DexDrive Controller Pak
> dumps**, not battery dumps, and they hold **no battery image at any offset** —
> but each *live* 25-page note carries a verbatim copy of a battery slot. The
> whole wall was that a slot's two `u16` header checksums are **seeded with the
> slot's own device offset**, while the game's copy-to-pak path seeds them with
> `0`. `tools/sramsave.py` now extracts + **reseeds**, and
> **`tools/run-save.sh <name>`** boots the game with the result.

## 0. Goal and result

**Goal (developer):** *"I placed some saves for this game at `assets/saves`. can we
create a way to run the game with them, as the 'battery' state for the game?"*

**Result: yes, and the developer watched it load.**

```sh
OGRE_SAVE=prologue ./build-app/ogrebattle64     # one knob, in-process
tools/run-save.sh --list                        # last_low_chaos / last_perfect / penultimate_mission / prologue
tools/run-save.sh prologue                      # same, with a per-save config dir
tools/run-save.sh prologue --reset              # re-import, discarding progress saved since
```

`OGRE_SAVE=<name|path>` (`app/src/save_import.cpp`, built into the app — **no
runtime patch**) converts the file and writes the battery to
`<config>/saves/<game id>.bin` before the runtime first reads it; a bare name is
looked up as the path, then `<rom dir>/saves/<name>[.n64|.bin]`, then
`assets/saves/<name>[.n64|.bin]`. It accepts everything `tools/sramsave.py`
does — the port's 32 KiB image, an emulator wrapper (either byte order, any
offset), a **DexDrive `.N64` pak** or a bare pak — and never writes back into the
source. A battery newer than the source is **kept** (so play progress survives a
re-run); `OGRE_SAVE_RESET=1` re-imports and `OGRE_SAVE_ALL=1` also takes a pak's
stale note records. `tools/run-save.sh` is the same conversion through the Python
tool plus a per-save config dir; its output is byte-identical.

## 1. What the files are — mechanically, not by inference

| observation | evidence |
|---|---|
| **DexDrive `.N64`**, not a battery and not 64DD | 36928 bytes = `0x1040` + `0x8000`; `"123-456-STD"` at 0x00. The DexDrive format is *literally* "a normal 32,768-byte `.MPK` file with a 4,160-byte header" ([mpkedit wiki](https://github.com/bryc/mpkedit/wiki/DexDrive-.N64-format)); `cpaktool` calls `.n64` "DexDrive controller pak dumps" ([libdragon wiki](https://github.com/DragonMinded/libdragon/wiki/Cpaktool)) |
| **no battery image is embedded** | scanning every offset for the SRAM device magic (`QuestOG3` at +0x04) finds nothing; the only magics are *slot* headers at 0x1564/0x2e64/0x4764/0x6064 |
| the payload **is** a real PFS pak | page 1 (`0x100`) inode table == page 2 (`0x200`) mirror; page 3 (`0x300`) directory with game code `NOBE` + publisher `EB` and note-name bytes that decode to `OGREBATTLE64 1`/`2` (pak codepage: `0`-`9` at 0x10-0x19, `A`-`Z` at 0x1A-0x33, space at 0x0F) |
| the pak's **FAT says which notes are live** | u16 BE per page at `0x100`; `0x0003` = free, `0x0001` = end-of-chain. Every live note is **25 pages** (`0x1900` = 6400 bytes — the game's own *"1 note 25 pages to save"*). Deleting a note leaves its pages readable, so stale copies stay: `prologue`/`last_perfect` have 1 live + 3 stale, `penultimate_mission` 1 live (`…2`) + 1 stale, `last_low_chaos` 1 + 1 |
| a note **carries a battery slot** | the note payload is `[0x20 bytes of save context][slot]`: the slot's `u32` header word sits at `note+0x20` and its `QuestOG3` magic at `note+0x24` — exactly where the battery slot has them (+0x00/+0x04). Byte-for-byte the same after that, e.g. `… 14 2a "Magnus"` then the whole `0x40` row identical to a battery slot |

**Consequence, found while testing the tool: the battery has only TWO save slots.**
Slot index 2's offset is exactly `0x30B0`, which is where the game's 19176-byte
(`0x4AE8`) map record begins; that record runs to `0x7B98`, i.e. over the rest of
the image. That is why session 66 read "slot 2's magic with header word 0" and
"slots 3/4 zeroed", and why the game validates it as slot **15**
(`func_80075578`). `import` therefore fills slots 0 and 1 and drops any further
note with a warning instead of writing into the record. (Slot 15's record is
accepted with a **zero first word** without its checksums being verified — the
`beqz` at `0x80075620` — which is the state a blank or freshly-written battery is
in.)

## 2. The wall: the checksum seed

`func_8007541C(slot, buf)` is the battery validator (`build/ogrebattle64.elf`,
`0x8007541C`):

```c
read 6224 bytes from *(0x800B83B8) + 0x10 + slot*0x1850;   // the cached image
if (memcmp(D_800B8240, buf+4, 8)) return 0;                // the 8-byte "QuestOG3"
if (u16[+0x00] != func_80075A84(buf+0x0C, 6212, 0x10+slot*0x1850)) return 0;
if (u16[+0x02] != func_80075B00(buf+0x0C, 6212, 0x10+slot*0x1850)) return 0;
return 1;
```

* `func_80075A84(data, len, seed)` = **sum of the bytes + seed**, `& 0xFFFF`.
* `func_80075B00(data, len, seed)` = **count of set bits + seed**, `& 0xFFFF`.
* **The seed is the slot's device offset.** `func_80075AC4`/`func_80075B60` are
  the same two loops *without* the seed — and that pair is what the game's
  copy-to-pak path uses.

Confirmed numerically, both ways:

* the game-written **blank** battery fits the seeded rule exactly: slot 0 `sum =
  0x4036, popcount = 0x11FE`, stored `0x4046/0x120E` = both `+ 0x10`; slot 1 the
  same `+ 0x1860` → `0x5896/0x2A5E`.
* **every** note slot in **all four** files fits the *un-seeded* rule: stored ==
  sum and stored == popcount, i.e. seed `0`.

So a note lifted straight out of a pak is valid at offset 0 and **invalid in every
battery slot**. First attempt (copy without reseeding): `[save] loaded …`, then
`[save] wrote …` and the file became the **blank format** (sha256 `ed38cfd7…`,
session 66's deterministic blank), and the title offered only New Game. After
reseeding the two halfwords, the game accepts the slot.

## 3. Verification — and a correction to my own first check

**My first "it works" check was not discriminating and I withdraw it.** A fresh
config dir with **no save at all** also reaches `title → 0x12 → 0x05` — pressing
`A` on the data screen starts a new game — so "reached the map" proves nothing.
The discriminator is the game's own **data screen** (the book, scene `0x12`):

| battery image in that run's config dir | the book's GAME DATA 1 |
|---|---|
| `assets/saves/prologue.n64` converted | `Magnus / Prologue / Alba / **0:20:42**` |
| `assets/save-mission-1.srm` converted (the older import) | `Magnus / Prologue / Alba / **0:07:02**` |
| nothing (blank battery) | (empty — the screen is reachable, the entry is not) |

Same screen, same tap route, three config dirs, different playtime: the game is
reading the file that was installed. Supporting evidence: after the run the
battery's **slot 0 is byte-identical** to the converted note (the first differing
byte is 0x1860, i.e. the game's own writes to slot 1 and the `0x30B0` record).

Proofs: `docs/proofs/native-load-game-prologue.png` (the whole screen) and
`docs/proofs/native-load-game-battery-ab.png` (the two GAME DATA 1 blocks).

Runs (all 1x, `build-app/ogrebattle64` from this tree, isolated `OGRE_PREF_DIR`,
taps `OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:3,0x05:a:1"`
unless noted):

| run | log | result |
|---|---|---|
| pre-fix, no reseed | `/tmp/ogre-prologue.log` | game repaired the image to the blank format; no save on the title |
| post-fix | `/tmp/ogre-prologue2.log`, `/tmp/ogre-prologue3.log` | `scene title -> start`, `scene 0x12 -> a`, `scene 0x05 -> a`; slot 0 untouched |
| capture, no taps | `/tmp/ogre-title.log`, `/tmp/titleproof.*.ppm` | boot stills + opening movie |
| capture, no taps | `/tmp/ogre-title2.log`, `/tmp/tp2.*.ppm` | the Load Game book: `Prologue / Magnus / Alba / 0:20:42` |
| blank control | `/tmp/ogre-control.log` | **also** reaches `0x12`/`0x05` — this is the confound that made the correction necessary |
| A/B/C | `/tmp/abc-{blank,prologue,mission1}.log`, `/tmp/abc-*.ppm` | the table above; the blank run aborted (exit 134) after the title, the documented intermittent scripted-run teardown |
| `OGRE_SAVE=prologue` | `/tmp/ogre-ogresave.log` | the app converted the pak itself and the runtime loaded what it wrote; its battery is **byte-identical** to the `sramsave.py` import (device header and slot 0) |
| `OGRE_SAVE=assets/save-mission-1.srm` | `/tmp/…` | the byteswapped, `0x20800`-offset emulator wrapper is detected and matches the Python import byte for byte |
| `OGRE_SAVE=prologue` + captures | `/tmp/ogre-os7.log`, `/tmp/os7.*.ppm` | the data screen reads `Magnus / Prologue / Alba / 0:20:42` again — proof `docs/proofs/native-load-game-ogre-save.png` |

## 4. Files changed

| file | change |
|---|---|
| `tools/sramsave.py` | DexDrive/pak detection (`find_pak`, with a positive PFS signature so a 32-bit-byteswapped `.sra` is still read as the battery, not a pak) + PFS parsing (`live_notes` from the FAT, `all_records`, `note_name` in the pak codepage, `note_slot`); **`reseed_slot`** and the checksum helpers built from `func_80075A84`/`func_80075B00`; `slot_checksum_ok` and `record15_valid` so `check` reports every slot; `import` now handles paks (capped at the battery's **two** slots, `--all` for stale records); new `pak` subcommand |
| `tools/run-save.sh` | **new** — import one save into `.ogre-prefs-save-<name>/` and boot the app with it, passing `OGRE_*` through; `--list`, `--reset`, `--import-only` |
| `app/src/save_import.cpp`, `app/src/save_import.hpp` | **new** — `OGRE_SAVE` / `OGRE_SAVE_RESET` / `OGRE_SAVE_ALL`: the same conversion in C++ inside the app (DexDrive pak → live or all notes → reseeded battery slots; emulator wrappers in either byte order at any offset; bare pak), writing `<config>/saves/<game id>.bin` and keeping a newer battery |
| `app/src/main.cpp` | calls `ogre::apply_ogre_save(pref_dir, game_id, rom_path)` after ROM selection, before `recomp::start` |
| `app/CMakeLists.txt` | `src/save_import.cpp` in both the native and the Emscripten source lists |
| `docs/symbols.md` | new "Save (battery SRAM) — the whole access path" section: the 11 functions, the 4 globals, the slot geometry and the seed rule |
| `docs/guides/app-build.md` | the checksum/seed rule, the DexDrive case, the `run-save.sh` recipe and the discriminating verification table |
| `docs/DECISIONS.md` | new durable-table row (session 78) |
| `PLAN.md` | new status bullet |
| `docs/proofs/native-load-game-prologue.png`, `-battery-ab.png` | **new** proof captures |
| `docs/HANDOFF-2026-09-18-session78.md` | this file |

**No probes, no generated code, no runtime patch.** Nothing under
`RecompiledFuncs/`, `Bank*Funcs/` or `tools/N64ModernRuntime/` changed, so there is
nothing to revert and `n64modernruntime-ob64.patch` is untouched. `build-app` was
reconfigured and rebuilt (`cmake -S app -B build-app && cmake --build build-app -j`)
and verified with the runs above.

## 5. The saves themselves: keep them out of git

The developer asked whether the `.n64` saves could be committed as repo test data.
**Recommendation: no — use a generated fixture instead**, and note that
`assets/` is already in `.gitignore` under *"ROM and derived assets (never commit
copyrighted material)"*, so committing them would mean deliberately un-ignoring
that tree, against AGENTS §10 (*"never commit ROM dumps, extracted assets"*).

Reasons beyond the repo's own rule (not legal advice, and not a lawyer's opinion):
a save file is mostly the player's own data (names, party, progress), and sharing
saves is common practice — but these files are *Controller Pak dumps whose bytes
are the game's own structures*, including game strings (location/chapter names
such as `Alba`) and the game's identifiers, so they are a derivative of the
game's data rather than purely user-authored content. There is also no test
harness to attach them to (AGENTS §9), so they would be inert files.

If regression data is wanted, generate it: a `tools/gen-save-fixture.py` writing a
DexDrive header + a pak with one synthetic 25-page note (our own bytes, the
`QuestOG3` structure and a seed-0 checksum) tests the container parse, the FAT
walk, the two-slot cap and `reseed_slot` without distributing anyone's game data.
What it cannot test is "the game accepts it" — that needs a real save, which stays
local (`assets/saves/`, gitignored).

## 6. Next leads

1. **The game's own pak restore path** (the Controller Pak menu, still not reached
   — session 65). When it is: does it reseed a note for the battery slot it lands
   in, or does it copy the seed-0 checksums verbatim and produce a slot its own
   validator rejects? That is now a cheap, precise question.
2. **Two-note paks.** No file here has two live notes, but a pak with two would
   fill battery slots 0 and 1 — the title's book should then show GAME DATA 1
   *and* 2. Not driven yet. (Slots stop at 1: see the two-slot consequence in §1.)
3. **The stale records.** `import --all` gives up to 4 slots from one file
   (`prologue` and `last_perfect` each carry 3 deleted notes). They are free extra
   test states; `pak --all` lists them. The pak codepage decodes their *names* only
   when the directory entry survives, so most show `(name lost)` — the slot data
   itself is intact.
4. **The `0x30B0` record** (19176 bytes, `func_80075578`): the game writes it at
   the map. A checkpoint + image diff would name its fields; `tools/sramsave.py
   check` already validates it.
