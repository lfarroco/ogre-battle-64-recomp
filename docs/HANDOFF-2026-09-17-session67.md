# Handoff — 2026-09-17, session 67: **the mission scene renders**

> **Read §1 if you are here for "rendering a mission".** The developer's suspend
> save resumes at **scene `0x03`**, which is the mission (3D terrain with a 2D
> party sprite and UI panels). It now enters, runs and draws:
> `docs/proofs/native-mission-scene.png` (terrain + knight + `Stronghold`
> tooltip) and `docs/proofs/native-mission-unit-panel.png` (`No. / FRIENDLY /
> STATUS`, `1. Magnus`, `STRONGHOLD / Zemio`, `START ^ FATIGUE`).

## Goal and result

**Goal (developer):** continue from the save/load milestone (session 66) and
attack "the next big wall: actually rendering a mission". The developer supplied
two saves in `assets/` and then a **suspend save** taken *inside* a mission
(`assets/save-mission-1.srm`, third SRAM slot at `0x30B0`; loading it from the
title erases it, as the game does).

**Result: the mission scene renders, with no crash and no stub calls.**

It took four things:

1. **Compile the three records the mission streams** (segment table records 7/8/9)
   as bank **unit N** — the same "missing streamed module" wall as sessions
   45/55/59. Before that the game's first call into the mission module hit the
   runtime's streamed stub and the run died on a wild pointer (§2).
2. **Fix a cross-bank mis-binding the new records exposed**: unit A's record 3
   called `0x801AD6BC` — record 6's RAM — and N64Recomp bound it directly to
   unit A's own record-6 body. Scene `0x03` loads record 3 **without** record 6
   (record 7 is resident there), so the wrong bank ran. Record 6 moved to **unit
   O** and the call compiles as `LOOKUP_FUNC` (§3).
3. **Declare the function entries a per-record disassembler cannot see**: a `jal`
   that crosses from one record of a unit into another (or an address only
   another bank's caller targets) leaves no `jal` in the target record, so
   spimdisasm emits it as a body interior. `get_function` has **no interior
   fallback** — such a `LOOKUP_FUNC` becomes the logging no-op streamed stub —
   so those entries are forced with a `symbol_addrs_path` file (§4).
4. **Compile two more arena banks the mission streams** into record 9's arena
   (units **P** and **Q**), found from the port's own stub log and a light
   `do_rom_read` probe (§5).

Also fixed: **`tools/cross_bank.py check-banks` was a silent no-op** — its YAML
parser dropped the `name:` on `- name: bankRecX` lines, so every record was named
`"?"` and the same-record test was vacuously true. It now parses names, uses the
scene descriptors' record masks to distinguish "two records always loaded
together" from a real split, and carries an explicit allowlist of the 23
pre-existing unit-C hazards it exposes (§6). This is the bug that let the session
67 wall reach a run.

## 1. What scene `0x03` is, and how it is reached

The developer's suspend save resumes at **scene `0x03`** — not `0x06`, the
briefing that loads the big `0x87220` module. `[scene] t=31081ms id=0x0003
descriptor=0x8018F350 mask=0x0000038C` (forced `title` → `Load Game` → suspend,
the developer's drive). Descriptor `0x8018F350` (branch selected by
`*(0x80193700)`, which also selects scene `0x06`'s two descriptors):

| word | value | meaning |
|---|---|---|
| enter | `0x80173724` | `func_801792DC`/`func_80179080` (record loaders), `func_80073164` (VI mode), `func_80076F5C` (a 320x240 `0xC000`-byte buffer at `0x8019EE70`), then sets `D_801977E8 = 2` |
| update | `0x801737CC` | `jr ra` (no-op) |
| hook | `0x801737D4` | `func_8016C814` (the frame pump) |
| leave | `0x801737F0` | |
| mask | `0x0000038C` | **records 2, 3, 7, 8, 9** (bit N = record N) |

Scene `0x0B` (attract story) loads records 2, 3 **and 6** (mask `0x4C`); scene
`0x03` loads 2 and 3 but **not** 6. That difference is what §3 turns on.

The developer's reference for this screen: *"a scene with lots of 3d elements,
and some 2d characters over it"* — 3D terrain/rivers/cliffs, the party sprite
with its selection brackets, and the tooltip/unit panels. It renders.

## 2. Unit N — records 7, 8, 9

`config-bankN.yaml` / `config-bankN.toml`, `BANK_UNITS += N`. The segment table
(ROM `0x387C0`) describes them, and their ROM and RAM ranges tile contiguously:

| record | ROM | size | RAM |
|---|---|---|---|
| 7 | `0x101D00` | `0x43530` | `0x801AD5C0 .. 0x801F4050` |
| 8 | `0x145230` | `0x099D0` | `0x801F4050 .. 0x801FDA90` |
| 9 | `0x14EC00` | `0x173E0` | `0x801FDA90 .. 0x80220F60` |

They are RAM-disjoint from each other, so one unit can hold all three; each is
RAM-*overloaded* by other units' records (7 shares its base with unit A's record
6, unit C's bankRec10 and the scene-`0x05`/`0x06` modules; 9's tail is unit B's
record 18's base), which is why they are their own unit.

Code/data boundaries are each record's own last `jr ra`, all landing on a 16-byte
boundary (the session-65 assembler-padding trap): record 7 code `0x3A7C0`
(ROM `0x13C4C0`), record 8 `0x8C80` (ROM `0x14DEB0`), record 9 `0x11010` (ROM
`0x15FC10`). `make bank-recomp`'s ELF check reports `0 differing bytes` and all
address-named symbols at their named address.

**BSS:** the segment table's `ram_end` word is larger than `ram start + rom size`
for all three, so their bss ranges were added to `tools/gen_bank_funcs.py`'s
`RAM_END` — `0x101D00 -> 0x801F4050`, `0x145230 -> 0x801FDA90`,
`0x14EC00 -> 0x80220F60` — and the runtime now zeroes them on load, as the game's
own loader does.

Before this unit the run reported
`[bank] UNKNOWN module rom=0x087220 ram=0x8019A7C0` for a **forced** scene
`0x06`, and for scene `0x03` the first call into record 8
(`[overlays] streamed function stub called @ 0x801F8530`) was followed by
`SIGBUS` in `do_send + 0xC4` with a wild queue pointer (`mq_ = 0x00010001`).

## 3. The wall under the wall: unit A bound record 3's call to record 6

With unit N in, the records load (`300 functions`, `85`, `97`) and the stub
warning is gone — **but the run still died, with wild pointers that moved between
runs** (`do_send` one run, `func_8007A110` the next). That is the signature of
frame/memory corruption, and the crash handler's shadow chain named it:

```
func_8008AFE0 -> func_80072398 -> func_800765D8 -> func_ovlA_8019EE70
             -> func_ovlA_801AD6BC -> func_ovlA_801AEC60 -> func_8007A110
```

`func_ovlA_8019EE70` is **unit A's record 3**. `BankAFuncs/funcs_0.c:20637` had

```c
// 0x8019F4AC: jal 0x801AD6BC
func_ovlA_801AD6BC(rdram, ctx);       // a direct, build-time-bound call
```

`0x801AD6BC` is inside **record 6** (`0x801AD5C0..0x801B4CC0`). Both records were
in unit A, so N64Recomp resolved the call locally — but scene `0x03` loads record
3 *without* record 6, and record 7 (unit N) occupies that RAM, so the port ran
record 6's body while the mission module was resident.

**Fix:** move `bankRec6` (ROM `0xFA600`, `0x7700` -> RAM `0x801AD5C0`) out of unit
A into **unit O** (`config-bankO.yaml`/`.toml`). Unit A's call now compiles as
`LOOKUP_FUNC(0x801AD6BC)` and the runtime's DMA-driven bank map picks whichever
module is actually resident (record 6/unit O in the attract story, record 7/unit
N in the mission). This is the session-45 rule, applied to *non-overlapping*
records of one unit that a scene can nonetheless split.

`make cross-bank-check`'s main-unit backlog is unrelated: the corrupting call was
in a **bank unit**, which the (broken) `check-banks` guard was supposed to catch
— see §6.

## 4. `lookup misses are silent no-ops`: forced function entries

`get_function` (`librecomp/src/overlays.cpp:698`) has **no fallback to the
containing function**: a `LOOKUP_FUNC` target that is not a registered entry in
the streamed range returns `streamed_stub_generic`, which prints and returns. A
`jal` that crosses *records* inside one unit is invisible while the target record
is disassembled (each record is its own splat segment), so spimdisasm emits the
target as a label inside the preceding body and N64Recomp has no entry there.

Forcing a **subsegment split** at such an address does not work: the assembler
pads each `.text` subsegment to 16 bytes, and these addresses are not 16-aligned,
so the ELF stops matching the ROM (`bankO.elf: 24265 differing bytes`, session
65's check). The working mechanism is `symbol_addrs_path` — the same
`name = 0xADDR; // type:func` form as the main `symbol_addrs.txt`, which makes it
a function start without touching `.text`:

| file | entries | why |
|---|---|---|
| `symbol_addrs-bankO.txt` | 4 (`0x801AD6BC`, `0x801AE880`, `0x801B0654`, `0x801B0714`) | record 3 calls them; record 6 runs into them as tails |
| `symbol_addrs-bankN.txt` | 51 (`0x80214FA0 … 0x8021EA70`) | record 7 calls them in record 9's RAM |
| `symbol_addrs-bankP.txt` | 2 (`0x80216D1C`, `0x80216E50`) | record 7's calls into module P |
| `symbol_addrs-bankQ.txt` | 3 (`0x8021954C`, `0x80216DC0`, `0x8021DC80`) | record 7's calls into module Q |

The bank-N list was derived mechanically: every `LOOKUP_FUNC` in `BankNFuncs/`
whose target lies inside unit N's own RAM ranges and is not already an entry. The
others came from the port's `[overlays] streamed function stub called @ …` log.

## 5. Units P and Q — two more banks of record 9's arena

With N/O in place the mission ran and drew, but the port logged three per-frame
stub addresses. They were **past record 9's declared size**
(`0x14EC00 + 0x173E0 = 0x80214E70`), i.e. in its bss/arena, with real MIPS at
them. `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1` named module **P**
(`rom=0x171EC0 -> ram=0x80214FA0`, 49 chunks).

That trace was **incomplete for the other run**: it perturbs the run enough to
take a different path. A light probe in `recomp::do_rom_read` (destination in
`0x80214000..0x80221000` or source in `0x160000..0x179000`) printed the second
bank directly: `rom=0x165FE0 -> ram=0x80214FA0`, 0x200-byte chunks up to
`rom=0x171EC0` — module **Q**. The ROM is tiled:

```
rec9  ROM 0x14EC00 (0x173E0) -> RAM 0x801FDA90   (unit N)
rec9c ROM 0x165FE0 (0x0BEE0) -> RAM 0x80214FA0   (unit Q)
rec9b ROM 0x171EC0 (0x06200) -> RAM 0x80214FA0   (unit P)
```

All three of `rec9c`/`rec9b` are banks of RAM `0x80214FA0` (unit C's bankRec12
also lives at `0x8020A300..0x80220CC0` in the same arena), so each got its own
unit. Both are disassembled whole as `asm` (they mix code with small data/
pointer tables); `bankQ` has a `data` subsegment after its last `jr ra`.

Result: **no stub calls, no crash**, and the mission draws its terrain, party
sprite, tooltip and unit panel.

## 6. `tools/cross_bank.py`: the guard was not checking

`check-banks` prints "0 outside their own record" for every unit and exits 0.
`parse_yaml_segments` matched `- name: bankRec3` with its *item* branch
(`line.startswith("  - ")`), created an empty segment and `continue`d, so the
`name` was never stored: every record's name was `"?"`, `record_of()` returned
`"?"` for both caller and target, and `record_of(target) == record_of(caller)`
was always true. The session-45 invariant has been unchecked for as long as the
names have been on the item line.

Three changes:

1. **Parse the item line.** `- name: X` / `- type: bin` now fill the segment.
   The check immediately found the session-67 wall (`unit A rec3 -> rec6`) plus a
   backlog.
2. **Use the scene masks.** A call from record R into another record R' of the
   same unit is only wrong when a scene can load R *without* R'; 1728 such calls
   exist and most are safe because their records are always loaded together. The
   check now reads every descriptor's `+0x10` mask word (bit N = record N,
   `SCENE_DESCRIPTOR_MASKS`, from `tools/scenemap.py scenes`) and only flags a
   pair no scene keeps together. 1728 -> 28, then 23 after unit O.
3. **Allowlist the pre-existing unit-C backlog.**
   `tools/cross_bank_known_hazards.txt` lists the 23 accepted `<caller> <target>`
   pairs with the reason; `check` skips them and still **hard-fails on anything
   new**, so `make bank-recomp` passes and a future violation stops the build.
   The listed pairs are unit C's `rec14/rec10/rec10b -> rec10/rec12` calls, which
   predate this session and have not been observed to misbehave; removing entries
   as those records are split into units that do not call them is the cleanup.

## 7. Verification (what was run)

All on `build-app/ogrebattle64` rebuilt from this tree, with
`OGRE_PREF_DIR=/Users/momo/dev/ogre/.ogre-prefs-suspend` and
`assets/save-mission-1.srm` imported with `tools/sramsave.py`.

| run | result |
|---|---|
| `make recomp` + `make bank-recomp` + `cmake --build build-app -j8` | clean; `check-banks` OK (23 accepted hazards); `elfcheck` 0 differing bytes for every unit |
| title → `Load Game` → suspend, 60 s | **no crash, no stub calls**; scene timeline `0x04 -> 0x12 -> 0x03`; records 2/3/7/8/9 + P + Q load |
| the same, `OGRE_CAPTURE_PRESENT` | `docs/proofs/native-mission-scene.png` (terrain + knight + `Stronghold` tooltip), `docs/proofs/native-mission-unit-panel.png` (`1. Magnus`, `STRONGHOLD / Zemio`, `START ^ FATIGUE`) |
| `OGRE_SCENE=story` (0x0B, loads record 6 -> unit O) and `OGRE_SCENE=title`, 30 s each | exit 0, 0 stub calls, 0 crashes |
| `tools/runlog.py` | no crash, no non-gfx stub task |

**Probes used and reverted** (AGENTS §6): `probe67` in
`librecomp/src/pi.cpp` (`do_rom_read` destination filter — the one that found
unit Q), in `ultramodern/src/mesgqueue.cpp` (`do_send` wild-queue check), and in
`RecompiledFuncs/funcs_9.c` / `funcs_10.c` (the VI-callback list walk and
registration). All are gone — `grep -rl probe67 RecompiledFuncs/ Bank*Funcs/
app/ tools/N64ModernRuntime/` is empty — the generated trees were regenerated by
`make recomp` / `make bank-recomp`, and `git -C tools/N64ModernRuntime diff`
is byte-identical to `n64modernruntime-ob64.patch`.

## 8. Files changed

* `config-bankN.yaml` / `.toml`, `config-bankO.yaml` / `.toml`,
  `config-bankP.yaml` / `.toml`, `config-bankQ.yaml` / `.toml` (**new**).
* `symbol_addrs-bankN.txt`, `symbol_addrs-bankO.txt`, `symbol_addrs-bankP.txt`,
  `symbol_addrs-bankQ.txt` (**new**) — forced function entries (§4).
* `config-bankA.yaml` — record 6 removed (moved to unit O), with the reason.
* `Makefile` — `BANK_UNITS += N O P Q`.
* `tools/gen_bank_funcs.py` — `RAM_END` entries for records 7/8/9's bss.
* `app/src/bank_overlays.cpp` — records 7/8/9 marked compiled in the diagnostic
  table.
* `tools/cross_bank.py` — the parser fix, the scene-mask refinement and the
  allowlist (§6).
* `tools/cross_bank_known_hazards.txt` (**new**) — the accepted backlog.
* `docs/proofs/native-mission-scene.png`,
  `docs/proofs/native-mission-unit-panel.png` (**new**).
* `.gitignore` — `.ogre-prefs*/`.
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/README.md`,
  `docs/guides/app-build.md`, this file.

Generated/ignored, regenerated by the normal targets: `build/bank*.elf`,
`Bank*Funcs/`, `app/src/bank_funcs.inc`.

`git status --short` shows the files above plus the pre-existing ` m tools/RT64`.

## 9. Next leads

1. **Drive the mission from the suspend save with the developer** and compare
   against `docs/proofs/map-reference/retail-map-screen.png`-style references:
   the tooltip, the unit panel and the `R` menu (`Organize / Hugo Report /
   Settings / Save`) are the surface to check next. The save/load work of session
   66 now has a mission to save *from*.
2. **The briefing scene `0x06` is still uncompiled.** It loads a `0x56D60` module
   from ROM `0x87220` to RAM `0x8019A7C0` (entry `0x801C19B0`, update
   `0x801C214C`), which the port reports as
   `[bank] UNKNOWN module rom=0x087220 ram=0x8019A7C0`. That module is the path
   the *natural* map → mission flow takes; the suspend save skips it. Same recipe:
   new unit, `symbol_addrs` for the entries the map's update calls
   (`0x801C19B0`/`0x801C214C` are already dispatched as `LOOKUP_FUNC`).
3. **The unit-C backlog in `cross_bank_known_hazards.txt`** (23 pairs) is real but
   latent; splitting `bankRec10`/`bankRec12`/`bankRec14` into units that do not
   call each other is the cleanup, and the allowlist makes it measurable.
4. **`get_function`'s missing interior fallback** is worth a decision: either
   keep the loud stub (current) or fall back to the containing function. The loud
   stub is what found units P and Q, so keep it, but a *build-time* report of
   every `LOOKUP_FUNC` whose target is inside the calling unit's own RAM and is
   not an entry would catch §4 before a run.
