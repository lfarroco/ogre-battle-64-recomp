# Handoff — 2026-09-17, session 72: **combat and items — eleven more arena banks, and a silent wrong-bank hang**

> **Read §1 if you are here for "the settings menu froze / the shop is blank".**
> Both were the same class as every swappable-arena wall in this project, in a
> shape that produces **no diagnostic at all**: the game streams a bank the port
> has no record for, so `on_streamed_dma` never calls `load_function_bank`, never
> evicts the *previous* bank from that RAM, and the caller's `jal` (correctly
> compiled as `LOOKUP_FUNC`) runs the old bank's body. The settings menu
> **hung**; the shop's entry was stubbed **3553×** (once per frame) and drew
> nothing. §2 fixes the settings menu (unit V), §3 combat (W, X), §4 the shop and
> the other six banks of record 9's arena (Y, Z, AA, AB, AC, AD, AE, AF), §5 two
> size corrections in existing units.

## Goal and result

**Goal (developer):** *"combat and items are very important parts of the game, we
can move into that."* Method agreed with the developer: the client writes its
logs to a file, the developer drives it into the situation, and this session
reads the log. That worked exactly as intended.

**Result: combat, the settings menu and the shop all run, developer-confirmed.**
Eleven new bank units, two size/BSS corrections in existing units, one new live
console command (`dmatrace`), and **0 `streamed function stub` / 0 `UNKNOWN
module`** on a full play session that reached the mission, combat, a town shop,
the Organize Screen and the dialogue engine.

## The instrumentation that made it fast

`app/src/sdl_platform.cpp`'s live console gained **`dmatrace`** (`dm`): it prints
the accumulated streamed-DMA map (`dump_dma_trace()`) *now*, instead of only on
the bounded-run exit path. `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1` accumulates
it. The developer runs the client with the diagnostics on and presses `2` when a
screen misbehaves; the log then names every module the game streamed. This is how
the 13-bank arena in §4 was enumerated.

## 1. The silent shape: an unknown record means no eviction

`on_streamed_dma` only calls `load_function_bank` for a record it knows. For an
unknown module it logs `[bank] UNKNOWN module` and returns — so the *bank map
still holds the previous bank's functions at those addresses*. Calls into the
new module then run the wrong code. Two observed faces:

* **settings menu** — `jal 0x80217E50` ran unit Q's body: no stub, no crash, the
  screen just stopped accepting input;
* **shop** — the entry address happened not to be a function in any registered
  bank, so `get_function` returned the logging stub (3553 calls) and the screen
  stayed empty.

The fix is always the same: compile the bank as its own unit and register the
record, so the runtime's DMA-driven bank map evicts the previous bank.

## 2. The settings menu — bank unit V (and my own size bug)

Developer: *"I opened the settings menu, and game stopped responding to my
input."* The log had exactly one line and no stub:

```
[bank] UNKNOWN module rom=0x1A4BE0 ram=0x80214FA0 (0x80214FA0 is also where rom=0x171EC0 loads)
[bank]   scene=0x0003 descriptor=0x8018F350 record mask=0x0000038C
```

The module's data half is the options screen (`Message speed`, `Cursor speed`,
`Game speed`, `Legion indicator`, `Sound settings`, `Restore defaults`,
`Stereo/Normal/Slower/Faster`, `Third Person/First Person`). The loader is unit
N's `func_ovlN_801B7530`:

```
func_800900C0(0x80214FA0, 0x802192C0)   ; code icache   (0x4320)
func_80090010(0x802192C0, 0x80219620)   ; data dcache   (0x360)
func_8009DA50(0x1A4BE0, 0x80214FA0, 0x4680)
func_80093380(0x80219620, 0x802196B0)   ; bss (0x90)
jal 0x80217E50                          ; entry
```

It is now **bank unit V** with the four real external entries forced
(`0x802170EC`, `0x80217BE4`, `0x80217E50`, `0x8021905C`).

**Correction to this session's own first attempt.** The first V config used
`size 0x14680` / `ram_end 0x80229620` / data `0x802292C0`, from two hex
subtractions that were simply wrong (`0x1B9260` for `0x1A9260`, and `0x80229620`
for `0x80219620`). That silently swallowed the next bank of the arena (ROM
`0x1A9260`, unit AE) into V's ELF. The ELF-vs-ROM check did **not** catch it: the
extra bytes are ROM bytes at their correct ROM offsets. What caught it was
re-deriving every arena loader from the disassembly when the shop appeared. Fixed
to `0x4680` / `0x80219620` / data `0x802192C0` / BSS `0x802196B0`; ROM 0x1A4BE0
stays byte-identical and the settings screen works.

## 3. Combat — bank units W and X

Developer: *"entered combat, pressed 2, 3, 4."* The log showed two unknown arena
banks and the first genuine stubs:

```
[bank] UNKNOWN module rom=0x17F9E0 ram=0x80214FA0
[overlays] streamed function stub called @ 0x80217690 / @ 0x80217660
[bank] UNKNOWN module rom=0x22A250 ram=0x801E6FD0
[overlays] streamed function stub called @ 0x801EFACC
```

* **W** — record 9's arena, ROM `0x17F9E0` (`0x91A0`) → RAM `0x80214FA0`; code to
  `0x802178D0`, data to `0x8021E140`, BSS to `0x8021E150`; loader unit N at
  `0x801BAD0C`. Forced entries `0x8021552C`, `0x80217660`, `0x80217690`.
* **X** — record 10's arena, ROM `0x22A250` (`0x10120`) → RAM `0x801E6FD0`; code to
  `0x801F68F0`, data to `0x801F70F0`, no BSS; loader the main unit's
  `func_80177754` at `0x8017791C` (scene `0x0E`'s enter, called as the battle
  setup). Forced entries `0x801EBCA0`, `0x801EFACC`, `0x801F0104`.

Developer: **"combat played beautifully."**

## 4. The shop, and record 9's arena has **thirteen** banks

Developer: *"later, I tried to open a shop in a town and nothing showed up."* The
log named `rom=0x1977B0 ram=0x80214FA0` with `stub 0x80219594` called **3553
times** — once per frame, because the shop UI was never built. Its data half is
the shop dialogue:

```
0x19bfbf  "I shouldn't expect much from a place like this."
0x19bff4  "This is all you got?"
0x19c064  "What do you have on sale?"
0x19c0d4  "Hello, how much is this?"
0x19c120  "Don't you have… those novelty things?"
```

Rather than add one unit and wait for the next wall, every `jal 8009da50` arena
load in unit N's record 7 was enumerated from the disassembly. **Record 9's arena
at RAM `0x80214FA0` has thirteen banks**, and the port had compiled five:

| ROM | size | unit | status |
|---|---|---|---|
| `0x165FE0` | `0xBEE0` | Q | already compiled (BSS fixed, §5) |
| `0x171EC0` | `0x6030` | P | already compiled (size fixed, §5) |
| `0x177EF0` | `0x7AF0` | **Y** | new |
| `0x17F9E0` | `0x91A0` | W | new (§3, combat) |
| `0x188B80` | `0x65A0` | **Z** | new |
| `0x18F120` | `0x6310` | **AA** | new |
| `0x195430` | `0x2380` | R | already compiled |
| `0x1977B0` | `0x4F80` | **AB** | new — **the shop** |
| `0x19C730` | `0x64C0` | **AC** | new |
| `0x1A2BF0` | `0x1FF0` | **AD** | new |
| `0x1A4BE0` | `0x4680` | V | new (§2, settings) |
| `0x1A9260` | `0x93E0` | **AE** | new |
| `0x1B2640` | `0x79E0` | **AF** | new |

Each got its own unit (they all share RAM `0x80214FA0`, so no two can share an
ELF), with the game's own code/data boundary from the loader's
`func_800900C0`/`func_80090010` calls and the external entries computed as "a
`LOOKUP_FUNC` target inside the bank's RAM range that is a real function start in
that bank's own layout" (preceded by `jr $ra`). Units `AA`–`AF` are the project's
first **two-character** unit names; the Makefile, `gen_bank_funcs.py` and
`symbol_name_format` all handle them unchanged.

Developer: **"everything runs beautifully."**

## 5. Two corrections in existing units

* **Unit P's size** was the chunk-rounded `0x6200` (49 × `0x200`), not the loader's
  `0x6030` (`0x177EF0 - 0x171EC0`). The old end marker `0x1780C0` swallowed the
  first `0x1D0` bytes of unit Y's bank. This is session 59's rule ("a bank's size
  is its DMA's, not the chunk-rounded figure") applied to the arena that rule was
  written for. Fixed: record size `0x6030`, end marker `0x177EF0`, BSS end
  `0x8021B010`.
* **P's and Q's BSS** were not zeroed (`RAM_END` had no entry), so the loader's
  `func_80093380` range was never reproduced. Added `0x171EC0 → 0x8021B010` and
  `0x165FE0 → 0x80220F50`.

## 6. Verification (what was run)

`make bank-recomp` → `cmake -S app -B build-app` → `cmake --build build-app -j8`:
**32 unit(s), 42 record(s), 3452 function(s)**, `cross_bank.py check-banks OK`.
`tools/elfcheck.py --syms`: every new ELF 0 differing bytes vs the ROM with all
address-named symbols at their address (`bankAB.elf` 322 symbols; `bankW.elf`
329; `bankX.elf` 560; …).

The final developer-driven run (`/tmp/ogre-live6.log`, `OGRE_SPEED=4`,
`OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1 OGRE_SCENE_LOG=1`):

| check | result |
|---|---|
| `streamed function stub` lines | **0** |
| `UNKNOWN module` / `UNCOMPILED` lines | **0** |
| scenes reached | `0x04 → 0x12 → 0x03` (mission), `0x06` (Organize), `0x02`/`0x0D` (dialogue engine/combat) |
| developer | *"everything runs beautifully"* (settings, combat, shop) |

### The slowdowns the developer saw are renderer-side, not missing code

They reported intermittent slowdowns; the log shows `processDisplayLists` timing
for 2940 display lists: **median 4.4 ms, p90 8.0 ms, one ~995 ms stall** (at
display list 4530, early in the mission) and a ~518 ms one. At `OGRE_SPEED=4` the
per-frame budget is ~4.15 ms, so the *median* is already at the 4× limit — the run
is render-bound, not missing-module-bound. The ~1 s stall is most consistent with
a first-use Vulkan pipeline compile (RT64), not game logic. The `dmatrace` and
`dump` the developer pressed cost main-thread I/O on top. Re-measuring at 1× is
the next step if the hitches matter.

## 7. Files changed

* **New units** (each `config-bank<U>.yaml`, `config-bank<U>.toml`,
  `symbol_addrs-bank<U>.txt`): **V** (settings, `0x1A4BE0`/`0x4680`), **W**
  (combat, `0x17F9E0`/`0x91A0`), **X** (battle, `0x22A250`/`0x10120`), **Y**
  (`0x177EF0`/`0x7AF0`), **Z** (`0x188B80`/`0x65A0`), **AA** (`0x18F120`/`0x6310`),
  **AB** (shop, `0x1977B0`/`0x4F80`), **AC** (`0x19C730`/`0x64C0`), **AD**
  (`0x1A2BF0`/`0x1FF0`), **AE** (`0x1A9260`/`0x93E0`), **AF**
  (`0x1B2640`/`0x79E0`) — all ROM → RAM `0x80214FA0` except X → `0x801E6FD0`.
* `Makefile` — `BANK_UNITS += V W X Y Z AA AB AC AD AE AF`.
* `app/src/bank_overlays.cpp` — records 22–32.
* `app/src/sdl_platform.cpp` — the `dmatrace`/`dm` console command.
* `tools/gen_bank_funcs.py` — `RAM_END` for all eleven new records plus the P/Q
  corrections.
* `config-bankP.yaml` — size `0x6200 → 0x6030`, end marker `0x1780C0 →
  0x177EF0`, header corrected.
* `config-bankV.yaml` — the §2 size/split correction.
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/README.md`, this file.

`git status --short` shows only these plus the pre-existing `tools/RT64` dirt.
**No probes**; `RecompiledFuncs/`/`Bank*Funcs/` were never hand-edited.

## 8. Next leads

1. **Other arenas.** The same enumeration (every `jal 8009da50` per unit) should
   be run for record 10's arena (`0x801E6FD0` — unit C's `bankRec10b`, unit X,
   and any others), record 12/13/14's (`0x8020A300`/`0x802210E0`/`0x802258B0`)
   and the scene-module arenas (`0x8019A7C0`, `0x801D0860`). Record 9 alone had
   eight uncompiled banks; the same is likely elsewhere.
2. **Identify units Y, Z, AA, AC, AD, AE, AF.** Their strings are mostly
   graphic/tile data; the developer can say which screen each corresponds to as
   they are exercised.
3. **Slowdowns at 1×.** Profile the 995 ms `processDisplayLists` stall (Vulkan
   pipeline compile?) and the steady-state render cost, without `OGRE_SPEED=4`.
4. **`dmatrace` at exit.** `dump_dma_trace()` still only runs from the bounded-run
   exit path; a `dmatrace` on the crash/signal path would capture the map when a
   run dies instead of being closed.
