# Handoff — 2026-09-17, session 73: **the arena map is a tool now, and every reachable bank is compiled**

> **Read §1 if you are here for "which streamed banks exist and which are
> missing".** `tools/arenamap.py` answers it from the ELFs in ~10 s, and the
> answer today is **27 bank loads, 26 compiled, 1 uncompiled and not on any
> observed route**. §2 is the loader pattern in full (including the two ways it
> fooled this session's first cut). §3 is the one gap and why it is deliberate.
> §4 is the verification. §5 is the table's home in `docs/scenes.md`.

## Goal and result

**Goal (developer):** map the streamed arena code banks, compile the missing
ones, add a tool that scans every ELF for the arena-loader pattern
(`func_8009DA50` with the `func_800900C0`/`0x10` cache brackets), work arenas in
order of what the developer can reach — one bank per unit, size from the loader's
`subu`, entries forced only where they are real function starts in that bank's
layout — and record the table in `docs/scenes.md`.

**Result.** The map is complete and mechanical, and **no bank on a reachable
path is missing**:

* `tools/arenamap.py` scans all 33 ELFs, finds **27 bank loads across 5 arena
  RAM windows**, and prints per bank: ROM start, RAM base, size, code/data split,
  BSS, the loader instruction, whether a unit links it, and the cross-record
  entry candidates with a real-function-start check.
* **26 of 27 are compiled.** `--verify` checks each compiled record against the
  loader that DMA's it: **ROM range and size match for all of them.**
* The 27th is a `0xE80` scene-`0x14` setup fragment whose RAM unit C's record 10b
  already owns, and no driven run has ever streamed it (§3).
* The negative is proven as well as the positive: `--coverage` shows every
  `jal 0x8009DA50` in the ROM (76 of them) lies inside code the scanned ELFs
  disassemble, so no loader can be hiding in an uncompiled segment.
* Two driven runs, `tools/runlog.py --check` **PASS** on both: 0
  `streamed function stub`, 0 `UNKNOWN module`, 0 crash.
* No new bank units were needed — the compiled set already covers the map.
  `make bank-recomp` is unchanged and green: **32 units, 42 records, 3452
  functions**, `check-banks OK`.

## 1. The tool

```
tools/arenamap.py                      # every arena, every bank
tools/arenamap.py --missing            # banks no unit links
tools/arenamap.py --arena 0x80214FA0   # one RAM window
tools/arenamap.py --unit Y             # what one unit covers
tools/arenamap.py --entries            # cross-record entry candidates
tools/arenamap.py --verify             # every unit vs the loader that DMA's it
tools/arenamap.py --coverage           # ROM the scanned ELFs disassemble
tools/arenamap.py --md                 # markdown table for docs/scenes.md
tools/arenamap.py --unmapped           # DMA sites with no cache bracket
```

It re-reads `build/*.elf` with `mips-linux-gnu-objdump`/`-nm`; no ROM-only step,
no run needed, no state to seed. A `--arena` query is ~10 s cold, <1 s warm.

## 2. The loader pattern, and the two ways it fooled the first cut

Every streamed bank in OB64 is DMA'd by one generated loader block. The game's
own boundaries are *in the instructions*:

```
lui   a0,%hi(rom_start) ; addiu a0,a0,%lo(rom_start)
lui   a1,%hi(ram_base)  ; addiu a1,a1,%lo(ram_base)
lui   a2,%hi(rom_end)   ; addiu a2,a2,%lo(rom_end)
jal   func_8009DA50
subu  a2,a2,a0                      ; size = rom_end - rom_start
```

bracketed by:

```
func_800900C0(ram_base, code_size)  ; icache invalidate, code half
func_80090010(code_end, data_size)  ; dcache invalidate, data half
func_80093380(data_end, bss_size)   ; bss zero (guarded by beq a0,a1)
```

So **one** block yields ROM start, RAM base, size, the code/data boundary and
the BSS end. The size is the loader's own `subu`, never the chunk count
(session 59's rule; session 72's unit V is the cautionary tale).

**Trap 1 — the size `subu` is not always in the delay slot.** The generator emits
it as the last argument, so it usually sits at `jal+4`; but the assembler
sometimes *hoists* it above the `jal`, and then the register already holds the
size at the call site. Reading the register at the call therefore gives the
right answer for one shape and the wrong one for the other. The tool evaluates
the `subu`'s **own operands** instead (`size_arg`), which is correct for both.
This is what made Z/AA/R/AB/AC and unit I report a plausible but wrong code size
in the first cut.

**Trap 2 — a backward walk must *execute* the block forward.** Naively applying
instructions from the call site up to the function start runs `addiu a1,a1,%lo`
*before* the `lui a1,%hi` that gives it its base, and every address comes out one
page off (`0x80190000` for `0x8019A7C0`). `walk_back` collects the addresses
backwards and then evaluates them in ascending address order.

Two more correctness notes, both found by the tool disagreeing with a config:

* **A function contains several loader blocks**, so the walk must stop at the
  function start *and* (for argument reads) at the call before this one —
  otherwise the bss call inherits the previous block's `lui`/`addiu` pair and
  reports the bank's own RAM base as its BSS start. The streamedA entry in the
  first table claimed a **3 MiB BSS** that way. Argument reads now try the whole
  block first (several loaders materialise their arguments once, into `s4`/`s5`)
  and fall back to the call-bounded pass.
* **A `jal 0x8009DA50` is not always a bank.** `func_8017909C` calls it for a
  plain asset DMA, where the "arguments" are memory loads; the first cut turned
  those into 27 phantom banks. `plausible()` requires the icache bracket and an
  in-ROM, in-RDRAM, 16-byte-aligned range. A record-18 loop loads its bank from a
  **`0x28`-byte descriptor table** (RAM `0x80229DDC`, indexed by `D_80193700`), so
  its addresses are data, not immediates; the tool correctly lists it in
  `--unmapped` rather than inventing a bank.

## 3. The one uncompiled bank, and why

`func_80177754` (scene `0x14`'s enter) has two arms. The first loads record 10's
arena (units J/X). Its **second** arm — reached when `*(0x801977F8) & 8` is clear
**and** `*(0x80197B80) == 0` — DMAs

```
rom 0x0023A370 (0xE80) -> RAM 0x801D0860
code 0x801D0860..0x801D16D0 (0xE70), data ..0x801D16E0 (0x10), no bss
```

which is exactly the first `0xE80` bytes of unit C's record 10b. It is **not
compiled**, deliberately: a new record on that RAM would collide with unit C's
record 10b in `cross_bank.py check-banks`, and nothing observed streams it — the
runs in sessions 67/69/71/72 and this session's two runs all take the first arm.
If a scene-`0x14` screen ever draws from stale bytes, this is the first thing to
check; compiling it means moving record 10b out of unit C.

## 4. Verification (what was run)

| check | result |
|---|---|
| `make bank-recomp` | **32 unit(s), 42 record(s), 3452 function(s)**; `check-banks OK` (24 pre-existing hazards allowlisted, 13402 direct calls into swappable RAM, 0 outside their own record) |
| `tools/elfcheck.py --syms` (all 33 ELFs) | **0 differing bytes** vs the ROM; every address-named symbol at its named address |
| `tools/arenamap.py --verify` | ROM range and size match for every compiled record; 9 split-only divergences, all explained by the assembler's 16-byte `.text` alignment or a one-subsegment linker script |
| `tools/arenamap.py --coverage` | 47 code sections, 0x2A8B10 bytes; 2 uncovered gaps, **0** `jal 0x8009DA50` inside |
| `runlog --check` — tutorial drive | **PASS**: 0 stub, 0 UNKNOWN, 0 crash. `OGRE_SCENE=tutorial … OGRE_TAP_SCENE_BUTTON="0x17:a:40,0x02:a:10,0x0d:a:80,0x03:a:60"`, scenes `0x17 → 0x02 → 0x0D → 0x03 ×2`, **37 bank loads** |
| `runlog --check` — map drive | **PASS**: 0 stub, 0 UNKNOWN, 0 crash. `OGRE_TAP_SCENE_BUTTON="title:start:3,0x12:a:3,0x05:a:2,0x05:a:2,0x03:a:6"`, scenes `0x09 → 0x0A → 0x04 → 0x12 → 0x03` |

16 of the 37 tutorial bank loads are listed in the `runlog.py` table (the rest
are the boot scripts and the remaining distinct records); the two driven runs
cover scenes `0x17`, `0x12`, `0x05`, `0x02`, `0x0D`, `0x03` and record 9's arena
plus units **T**, **G/L** and **K**. Combat and the settings/shop banks were
verified in session 72 and are unchanged (no bank unit was edited this session).

**No code was changed this session** — the tool and the record are the output.
`RecompiledFuncs/`/`Bank*Funcs/` were regenerated but never hand-edited, and no
probes were used.

## 5. Where the record lives

* **`docs/scenes.md`** has the full table under *"Every streamed arena the game
  can load (session 73)"*, with the loader pattern, the per-bank columns and the
  two caveats (the scene-`0x14` fragment and the table-driven record-18 loader).
  The older "Record 9's arena: thirteen banks" table is folded into it.
* **`docs/DECISIONS.md`** has the durable entry: the map is mechanical, the
  loader is the source of truth, and the two scheduling traps.
* Regenerate the table with `tools/arenamap.py --md`.

## 6. Files changed

* **`tools/arenamap.py`** — new (the whole deliverable).
* `docs/scenes.md` — the arena table and the two caveats.
* `docs/DECISIONS.md` — the session-73 durable entry.
* `PLAN.md` — session-73 status.
* this file.

`git status --short` shows only these plus the pre-existing `tools/RT64` dirt.

## 7. Next leads

1. **Identify units Y, Z, AA, AC, AD, AE, AF.** Their strings are graphic/tile
   data; the developer can say which screen each is as they play (unchanged from
   session 72).
2. **Scene `0x14` still has no mapped screen.** The tool found its enter and the
   two-armed loader; the second arm's `0xE80` fragment is the one uncompiled
   bank. Ask the developer what scene `0x14` shows and whether they have seen it.
3. **`--entries` is the next safety pass.** It now attributes each call to the
   record it sits in and reports only calls that cross into another bank's RAM.
   Running it per arena and forcing the real-start entries that are missing is
   the mechanical version of the session-45/67/72 fixes; the tool names them, so
   a future session can work down the list instead of waiting for a wall.
4. **The map drive only reached the mission head** before `OGRE_EXIT_AFTER_MS`.
   A longer driven run (or a checkpoint at the map) is what exercises units Q and
   W again; use the session-72 developer-driven recipe for combat/shop.

## 8. Follow-up (same session): scene `0x14` is the shop, and it exposed a missing bank — **unit AG**

The developer asked to boot the game straight into `0x14` and identify it from a
screenshot. What came back was better than a screenshot.

**Scene `0x14` is the Witch's Den** (developer, session 73): *"on every mission,
there's one city that has a 'witch den'. visiting it allows resurrecting dead
party soldiers."* It is reached in normal play by walking into that city's den —
the developer confirmed after this session's fix: *"I know how to reach the witch
scene, and I just did that — it works!"*. So this is **not** a test-only scene;
the missing bank it exposed was on a real route.

### What `OGRE_SCENE=0x14` did

The scene can be forced, but the poke **races the boot** and lands about **1 run
in 5** (the scene's own transition to the intro at ~1.6 s wins otherwise) — loop
the command until `[scene] forced scene 0x14 is active` appears. The first
successful run logged:

```
[bank] loading overlay record rom=0x275820 ram=0x802210E0 size=0x47D0 (49 functions)
[bank] UNCOMPILED streamed record 16: rom=0x279FF0 ram=0x802258B0 size=0x7840
[overlays] streamed function stub called @ 0x80226B7C (not yet loaded)
[overlays] streamed function stub called @ 0x8022859C (not yet loaded)
[overlays] streamed function stub called @ 0x80226CE0 (not yet loaded)
```

and the present was **black** (the render target had content, the presenter did
not), with an occasional SIGBUS in RT64's `update_screen` at the present — the
game handing the presenter a pointer into an unloaded module.

### The missing bank: record 16, not a segment-table record

Segment-table **entry 13** is `rom 0x275820..0x279FF0 -> ram 0x802210E0`, and
unit C links exactly that. The game then DMAs a **second** module over the same
arena from `0x279FF0`. It has **no loader `subu`**: the load is
descriptor-table-driven, so `tools/arenamap.py`'s pattern scan cannot see it —
its size is the table's `ram_end` for entry 13 (`0x8022D0F0`) minus `0x802258B0`
= **`0x7840`**, which is exactly what the port's own `UNCOMPILED` line printed.

The three stubbed addresses are in that module (`ROM 0x27B2BC`, `0x27CCDC`,
`0x27B420`), each preceded by `jr $ra`. And the window is **heavily used**: a
scan of every ELF finds **125 distinct call targets** in
`0x802258B0..0x8022D0F0` from banks B, C, F, G, K, L, N, T and the main ELF,
because three other arenas overlap it — record 18's tutorial module
(`0x8022A860`, unit T), record 14d's chapter animation (`0x8022ACB0`, unit K)
and record 14a (unit L). Unit C's record 13 has compiled bodies at the low half
of the same RAM, so every call from unit C's code into the high half was bound
at build time to record 13's layout.

### The fix

**Bank unit AG** — `bankRec16b`, ROM `0x279FF0`, size `0x7840`, RAM
`0x802258B0` — with the three addresses declared in `symbol_addrs-bankAG.txt`
(real function starts in *this* layout). New files `config-bankAG.yaml` and
`config-bankAG.toml`; `BANK_UNITS += AG` in the `Makefile`;
`app/src/bank_overlays.cpp` needs nothing (the record table is generated).
Record 13 stays in unit C: the two are separate units, which is what makes the
runtime's DMA-driven bank map evict one for the other.

Verified: `bankAG.elf` is **0 differing bytes** from the ROM with 426
address-named symbols at their addresses; `make bank-recomp` is
**33 units / 43 records / 3509 functions**, `check-banks OK`; `elfcheck --syms`
0 bytes on all 34 ELFs; `--coverage` leaves only **one** uncovered gap (record
16's closed).

### Result: scene `0x14` is the shop

`runlog --check` **PASS** (0 stub, 0 UNKNOWN, 0 crash):

```
[scene] t=1.66s id=0x0014 mask=0x00013C14
```

and the frame (`docs/proofs/native-scene-14-shop.png`) is the Witch's Den —
cauldron, bottle shelves, a counter reading **`WAR FUNDS 0001000 Goth`**, and
the dialogue box **`Old Witch` / "Heh heh heh... Can I help you?"**. The
developer reached it in normal play the same day and confirmed it works.

Getting the picture needed one technique worth keeping: with the forced entry
the **present** is black while the **render target is not**, so use
`OGRE_CAPTURE_TARGET` (320x239, `RenderFormat` 11 = `R16G16B16A16_UNORM`, 8
bytes per texel) and convert it, **bottom-up**:

```python
d = open("t.NNN.bin","rb").read()[:320*239*8]
v = struct.unpack_from("<%dH" % (320*239*4), d)
for i in range(320*239):
    r,g,b = v[4*i], v[4*i+1], v[4*i+2]
    row = 239-1-(i//320); col = i%320          # target is bottom-up
```

### Files changed by §8

* `config-bankAG.yaml`, `config-bankAG.toml`, `symbol_addrs-bankAG.txt` (new)
* `Makefile` — `BANK_UNITS += AG`
* `tools/arenamap.py` — `KNOWN_LOADERLESS` (record 16), the derived-bank row, and
  the docstring caveat that the loader pattern is not the whole map
* `docs/scenes.md` — scene `0x14` row, the arena table's record 13/16 rows, §
  *"Record 16 — the bank that hid behind a loader-less load"*
* `docs/proofs/native-scene-14-shop.png` (new)
* `PLAN.md`, `docs/DECISIONS.md`, this file

No probes; `RecompiledFuncs/`/`Bank*Funcs/` regenerated, never hand-edited.

### Lesson for the next session

`tools/arenamap.py`'s negative — all 76 ROM-wide `jal 0x8009DA50` inside
compiled code — is **necessary but not sufficient**: a descriptor-table load has
no `jal` to find, and record 16 hid behind exactly that. The arbiter is the run
log's `[bank] UNCOMPILED streamed record N: rom=… ram=… size=…`, which names
both the module and its size. Boot the scene, read the line, add the unit.
