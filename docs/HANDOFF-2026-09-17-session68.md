# Handoff — 2026-09-17, session 68: **the mission intro works on the natural route** — unit R

> **Read §1 if you are here for "the mission is broken when you play to it
> normally".** The natural route (map → story → mission) streams a **fourth bank**
> of record 9's arena — ROM `0x195430` (`0x2380`) → RAM `0x80214FA0` — that the
> port had no code for, so the mission's own entry call landed on the runtime's
> logging stub: the target fort drew garbled, the winning-condition `NOTE` never
> advanced, and the camera never panned. It is now **bank unit R**, and the whole
> intro runs (`docs/proofs/native-mission-intro-natural.png`).

## Goal and result

**Goal (developer):** fix the mission scene when it is reached the normal way
(`map → story → mission`): the unit-condition `NOTE` appeared over a **garbled
target-fort draw**, the losing-condition message never came, `A`/`START` did not
dismiss it, and the camera never panned to the target fort — while the same
mission entered from a **suspend save** was fine.

**Result: it works.** Root cause: **the natural route loads a module the suspend
route does not**, and it was uncompiled. One new bank unit (R) fixes it; the run
now reports **0 `UNKNOWN module` and 0 `streamed function stub`** on the route,
and the intro plays (camera pan, winning condition, panic back, losing condition,
`MISSION START`). The developer, driving it live: *"it works now!!"*.

Two other things landed, both for driving scripted runs (§6): a **scene-keyed tap
schedule** (`OGRE_TAP_SCENE_BUTTON`) and a **synthetic pad press in the live
console** (`press <buttons> [polls] [x] [y]`) — the map is not reachable with the
existing wall-clock `OGRE_TAP_BUTTON` schedule, and the console `press` is what
made it drivable (a "short press" is far shorter than it looks: the game polls
input much faster than 60 Hz).

## 1. The bug: scene 0x03 on the natural route streams a fourth arena bank

The developer's route, measured with `OGRE_SCENE_LOG=1` (`OGRE_SPEED=4`):

```
0x04 (title) -> 0x12 (Load Game) -> 0x05 (map)
   -- A on the mission location --
-> 0x02 -> 0x0D   (the scripted "story" dialogue, step word D_8018F1C0 = 10)
-> 0x02 -> 0x0D -> 0x16  (the same closing movie the New Game opening plays)
-> 0x02 -> 0x0D
-> 0x03 (the mission, descriptor 0x8018F350, mask 0x38C = records 2,3,7,8,9)
```

At the mission the old build logged exactly two lines, and they are the whole
bug:

```
[bank] UNKNOWN module rom=0x195430 ram=0x80214FA0 (0x80214FA0 is also where rom=0x171EC0 loads)
[overlays] streamed function stub called @ 0x80215C38 (not yet loaded)
```

`0x80214FA0` is record 9's arena — the RAM units **P** (record 9's `rec9b`, ROM
`0x171EC0`) and **Q** (`rec9c`, ROM `0x165FE0`) already own, and the **suspend**
save's mission loads Q instead. The natural route loads a *third* bank there.

The loader is record 7's own code (**unit N**, `func_ovlN_801AE880+0x764`, ROM
`0x1036E4`):

```asm
801aefa4: lui   a0,0x19 ; addiu a0,a0,0x5430      ; ROM 0x195430
801aefac: lui   a1,0x8021 ; addiu a1,a1,0x4FA0    ; RAM 0x80214FA0
801aefb4: lui   a2,0x19 ; addiu a2,a2,0x77b0      ; end 0x1977B0
801aefbc: jal   8009da50  <func_8009DA50>          ; DMA size = 0x1977B0-0x195430 = 0x2380
801aefe4: jal   80215c38                           ; <- the module's entry (the stub above)
```

`0x1977B0 - 0x195430 = 0x2380`, and the cache-op bracketing the DMA
(`func_800900C0(0x80214FA0, …)` / `func_80090010(0x80217290, 0x90)`) confirms the
span RAM `0x80214FA0..0x80217320`.

**What the module is** (from its own data half): the strings
`No free space on TCharacterEnemyData.`, `No free space on EnemySolderData.` and
a name table starting `Mitsuiye`. It is the mission's **enemy/unit constructor**
— it builds the enemy units and the target fort. Without it the fort is built
from stale bytes (the garbled draw) and the intro's script never gets the state
it waits on (the stalled `NOTE`).

## 2. The fix: bank unit R

`config-bankR.yaml` / `config-bankR.toml` / `symbol_addrs-bankR.txt` (**new**),
`BANK_UNITS += R` in the `Makefile`. The module is disassembled whole as `asm`
(the unit-P treatment — spimdisasm splits the embedded data itself), and `0x2380`
is already a multiple of 16, so the assembler adds no `.text` pad (session 65's
+8 trap).

Only **one** forced function entry was needed:

| file | entry | why |
|---|---|---|
| `symbol_addrs-bankR.txt` | `0x80215C38` | unit N's record-7 `jal` target, invisible to a per-record disassembly of the module (the `get_function` no-interior-fallback wall of session 67) |

The module's other four entries (`0x80214FA0`, `0x802150DC`, `0x80215360`,
`0x8021592C`) are reachable from `jal`s **inside** the module, so spimdisasm
finds them; its five `jr ra` are at `0x802150D4`, `0x80215358`, `0x80215924`,
`0x80215C30`, `0x80217280` and the data half starts at ROM `0x197720`
(splat itself suggested `- [0x197720, asm]`).

`tools/elfcheck.py --syms`: `bankR.elf: 0 differing bytes of 1669040`, 180
address-named symbols all at their named address. `make bank-recomp` reports
**18 unit(s), 28 record(s), 2688 function(s)** and `cross_bank.py check-banks`
passes (no bank unit calls into a swappable range it does not own).

**The session-45 rule applies again, in the *same* arena for the third time:** a
streamed module the segment table does not describe still owns a swappable RAM
range, and `[bank] UNKNOWN module` + `streamed function stub called @ …` in a run
log *is* the diagnosis. The natural/suspend difference is not port state — it is
**which bank the game chooses to stream**, so a scene that works from a save can
still be missing its code when played to.

## 3. Verification (what was run)

All on `build-app/ogrebattle64` rebuilt from this tree
(`make bank` → `make bank-recomp` → `cmake --build build-app -j8`), with
`OGRE_PREF_DIR=$PWD/.ogre-prefs-mission` and its battery save (slot 0 = the
post-prologue map save).

| run | result |
|---|---|
| natural route, `OGRE_SPEED=4`, developer driving, `OGRE_SCENE_LOG=1` | scene path `0x04 → 0x12 → 0x05 → 0x02/0x0D → 0x16 → 0x02/0x0D → 0x03`; `[bank] loading overlay record rom=0x195430 ram=0x80214FA0 size=0x2380 (5 functions)`; **0 `UNKNOWN module`, 0 stub calls** |
| the same run, `OGRE_CAPTURE_PRESENT` | `docs/proofs/native-mission-intro-natural.png` (winning-condition `NOTE` + the `Theodricus Mine` fort illustration and label, first phase), `docs/proofs/native-mission-intro-natural-pan.png` (the same NOTE with the camera mid-pan — the `MISSION` banners slide) |
| `make bank-recomp` / `tools/elfcheck.py --syms` | all 14 bank ELFs + the main ELF byte-identical to the ROM; every address-named symbol at its address |
| `tools/cross_bank.py check-banks` | OK (23 allowlisted pre-existing unit-C pairs, 0 new) |

**No probes were used.** Nothing in `RecompiledFuncs/` or `Bank*Funcs/` was
hand-edited (the `njpeg_readback.py` patch inside `bank-recomp` is pre-existing),
so there is nothing to revert; `git -C tools/N64ModernRuntime diff` is unchanged
from `n64modernruntime-ob64.patch`.

## 4. The route, for the next session

To reach the mission without the suspend save (which the game deletes when it is
resumed — re-import it with `tools/sramsave.py import assets/save-mission-1.srm
"$HOME/Library/Application Support/ogrebattle64/saves/ogrebattle64-us-rev1.bin"`):

1. title → `Load Game` (the cursor starts there when a save exists) → slot 1;
2. on the map the cursor starts **next to**, not on, the mission location; press
   `A` with the cursor on the crossed-swords location (the `Tenne Plains` label
   shows when it is on a location);
3. the story is scene `0x0D` dialogue advanced with `A`, then scene `0x16` (the
   closing movie), then the mission.

**A tooltip blocks input.** A `START` press on the map opens the location tooltip
and nothing else works until it is dismissed — the same trap the name-entry form
has (session 64).

## 5. The intro's phases are advanced with `A` (developer-confirmed)

The camera pans on its own, and each `NOTE` / the `MISSION START` banner waits for
an `A` press (the developer: *"you just need to press A to advance to the next
part of the mission start animation"*). In the broken build those same frames sat
still for minutes and `A`/`START` did nothing at all — that is what a stub
returning into a state machine whose data was never built looks like. If a future
session sees a *frozen* cutscene, check the log for a stub call before theorising
about input.

## 6. New driving knobs (they are what made this session's repro possible)

| knob | what it does |
|---|---|
| `OGRE_TAP_SCENE_BUTTON="<scene>:<buttons>[:<count>],…"` | the **scene-keyed** sibling of `OGRE_TAP_BUTTON`. `OGRE_TAP_BUTTON`'s slot index advances with **wall time**, so a multi-scene route is hand-tuned to how long each scene took: the same "title → Load Game → map" schedule landed on the map in one run and in the attract loop in the next, because the boot reaches the title anywhere between 1.6 s and 15 s (the forced-scene poke races the publisher stills). This one keys the button on the dispatcher's active scene: `OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3"`. An entry's optional `count` is how many presses it may spend; entries in one scene are consumed in order (`0x05:right:3,0x05:a:1`). |
| console `press <buttons> [polls] [x] [y]` | a synthetic pad press **and analog stick** that lasts `polls` input polls (numbers are parsed as **hex**, so `press a 400` is 1024 polls), released afterwards. This is the "run, look at the capture, act" loop the developer suggested, driven from the watched console file. `press none 60 -1 0` is a full-left stick. |

**A short press is much shorter than it looks.** The game polls input far faster
than 60 Hz: `press right 8` (8 polls) is invisible, `press right 2000` (8192
polls) walks the map cursor to the other side of the world. Budget a few hundred
polls per cursor step.

**Write the watched console file atomically** (`printf … > f.tmp && mv f.tmp f`):
the app polls the path, and a plain `> f` truncates before writing, so the reader
can consume an empty file and delete the command.

## 7. Files changed

* `config-bankR.yaml`, `config-bankR.toml`, `symbol_addrs-bankR.txt` (**new**) —
  the module at ROM `0x195430` → RAM `0x80214FA0`.
* `Makefile` — `BANK_UNITS += R`.
* `app/src/sdl_platform.cpp` — `OGRE_TAP_SCENE_BUTTON` (scene-keyed tap
  schedule) and the console's `press` command (buttons + analog stick).
* `docs/proofs/native-mission-intro-natural.png`,
  `docs/proofs/native-mission-intro-natural-pan.png` (**new**).
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/guides/app-build.md`,
  this file.

Generated/ignored, regenerated by the normal targets: `build/bankR.*`,
`BankRFuncs/`, `app/src/bank_funcs.inc`. `git status --short` shows the files
above plus the pre-existing ` m tools/RT64`.

## 8. Next leads

1. **Play the mission on.** The intro now runs; what has not been checked is the
   mission proper (units moving, combat, the `R` menu inside a mission, the
   *losing* condition, saving from inside). The natural route is now cheap: the
   map save + a short drive.
2. **The natural/suspend divergence is a general lesson**: a scene that a suspend
   save resumes into may stream a *different* arena bank from the same scene
   reached by playing. When a screen works from a save and not from the front,
   compare the `[bank] loading overlay record` lines of the two runs before
   anything else.
3. **`docs/scenes.md`'s mission row** should gain the intro sequence's phases
   (developer-confirmed) and the enemy-constructor module's identity.
4. The two new knobs deserve a mention in the "Scripted runs" guide's
   `OGRE_TAP_*` family (§6 landed it) and the web port's equivalent, if it grows
   one.
