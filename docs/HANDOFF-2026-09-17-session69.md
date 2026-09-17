# Handoff — 2026-09-17, session 69: **the Organize Screen (scene `0x06`) is bank unit S**

> **Read §1 if you are here for "selecting Organize Screen just reloads the
> mission".** Scene `0x06` streams **ROM `0x87220` (`0x56D60`) → RAM
> `0x8019A7C0`** — 355 KB, and the port had no code for any of it. The enter
> `jal`s the module's entry `0x801C19B0` and the generic scene update/hook `jal`
> `0x801C214C` / `0x801B7FC0`; all three landed on the runtime's logging stub, so
> the screen was never built and the scene's state machine took its "done" arm and
> returned to `0x03` **68 ms later** — which is the "reload". It is now **bank
> unit S**, and the developer drove the screen up: `docs/proofs/native-organize-screen.png`.

## Goal and result

**Goal (developer):** *"help me implement the 'organize screen' … currently, the
screen just reloads, but the expected is going to another screen where you can
organize your army."*

**Result: it works, developer-confirmed from the live build.** One new bank unit
(S) and three dispatched call sites; the mission → `R` menu → Organize Screen now
enters scene `0x06` and draws the army-organizing screen instead of bouncing back.

**The route (developer, and it is the *tutorial* one — no save needed):**

1. title → `start` (menu) → `down` → `A`/`start` = **Tutorial** (scene `0x17`);
2. `A` through Deneb's dialogue (*"Beginners need not worry! …"* → *"Are you
   ready? Then, let's start!"*), then `down` `down` `A` = **practice stage**;
3. the **mission** (scene `0x03`) starts; `A` through the winning/losing-condition
   `NOTE` phases;
4. **hold `R`** — a horizontal icon menu opens (entry 1 = `Dispatch`, 5 =
   `Mission Objective`, 7 = `End`); `→` and `A` on **Organize Screen**.

**Two driving facts this cost real time to learn (both worth keeping):**

* **`R` on the keyboard is not mapped.** The keyboard map is `X`=A, `Z`=B, `C`=Z,
  `Return`=START, **`Q`=L, `E`=R**, arrows = D-pad, `I/J/K/L` = C-buttons
  (`keyboard_buttons()`, `app/src/sdl_platform.cpp`). The *letter* `R` does
  nothing; the shoulder button is `E`.
* **The menu is a *hold* menu.** It is drawn while `R` is held and closes on
  release, so any screenshot taken after a synthetic tap misses it (my first four
  attempts concluded "R does nothing"). To drive it: hold `E` with
  `osascript … key down "e"`, and send the D-pad/`A` from the live console —
  `console_input_take()` is OR'd with the keyboard state, so the two compose.
  A console `press` on its own **replaces** the held mask and closes the menu.

## 1. The bug: scene `0x06`'s module was never compiled

Measured with `OGRE_SCENE_LOG=1` (`/tmp/run-organize3.log`, t≈334.8 s), pressing
`A` on the Organize Screen entry:

```
[scene] t=334853ms id=0x0006 descriptor=0x8018FD84 mask=0x00000002
[bank] loading overlay record rom=0x06E680 ram=0x80197B90 size=0x2C20 (18 functions)   <- record 1, the mask
[bank] UNKNOWN module rom=0x087220 ram=0x8019A7C0 (0x8019A7C0 is also where rom=0x0712A0 loads)
[bank] UNKNOWN module rom=0x09A020 ram=0x801AD5C0 (0x801AD5C0 is also where rom=0x1F0A00 loads)
[overlays] streamed function stub called @ 0x801C19B0 (not yet loaded)
[overlays] streamed function stub called @ 0x801C214C (not yet loaded)
[scene] t=334921ms id=0x0003 descriptor=0x8018F350 mask=0x0000038C     <- the 68 ms bounce
```

The scene ids and descriptors line up with the scene graph the project already
has (`tools/scenemap.py`; id 6 is one of the three *indirect* accessors):

* accessor `func_8017B5DC` reads `*(0x80193700)` and returns descriptor
  **`0x8018FD84`** (mask `0x2`) or **`0x8018FD98`** (mask `0x40002`);
* both descriptors share **enter `0x8017B6D0`**, update `0x8017B858`, hook
  `0x8017B9C8`, leave `0`.

### The enter, at instruction level (`func_8017B6D0`, `.streamedB`)

```
8017b6d8  a0 = 0x8019A7C0, a1 = 0x801FE0E0        -> func_800900C0  icache invalidate
8017b6f0  func_80090010(0x801FE0E0, 0x801F1520-0x801FE0E0)
8017b708  a0 = 0x00087220
8017b710  a1 = 0x8019A7C0
8017b718  a2 = 0x000DDF80
8017b724  subu a2, a2, a0   -> size = 0x56D60
8017b720  jal 0x8009DA50                          -> DMA ROM 0x87220 (0x56D60) -> RAM 0x8019A7C0
8017b728  a0 = a1 = 0x801F1520, beql -> skip      -> bss_start == bss_end: NO BSS
8017b76c  jal 0x80073164 (0, 1, 1, 64, {1,0x600,0x2200})
8017b774  jal 0x801C19B0                          -> the module's ENTRY  (stub above)
8017b77c  *(0x801977E8) = 2                       -> the scene-state word
```

So the module owns RAM `0x8019A7C0 .. 0x801F1520` = `0x56D60`, no BSS.

### The three entry points resident code calls

`func_8017B858` (the **generic scene update** shared by scenes `0x05`/`0x06`/`0x07`)
switches on `*(0x801977E8)`: state 1 → `jal 0x8019AF0C` (unit M), state 3 →
`jal 0x8019B340`, and **state 2 → `jal 0x801C214C`** (`0x8017B8F4`) — which is
scene `0x06`'s own update, because its enter is what sets state 2. The
**generic hook** `func_8017B9C8` does the same and calls **`0x801B7FC0`**
(`0x8017BA24`, with `a0 = *(s0+0x10) + 0x200`).

So: `0x801C19B0` (enter), `0x801C214C` (update), `0x801B7FC0` (hook). All three
are `jal` targets **from another segment** while the module is disassembled on its
own, so none of them is a function entry in the module's own disassembly — the
session-67 `get_function` trap. They are forced in `symbol_addrs-bankS.txt`.

### The second `UNKNOWN module` line is a false positive — do not chase it

`0x9A020 - 0x87220 == 0x12E00 == 0x801AD5C0 - 0x8019A7C0`. The game copies a
record in **`0x200`-byte chunks**, and chunk **151** of *this* DMA lands exactly
on another module's base (`0x801AD5C0`, record `0x1F0A00`), so
`on_streamed_dma`'s "a DMA that starts on a known module base from an unknown ROM"
heuristic fires. There is **one** uncompiled module here, not two.

## 2. The fix: bank unit S

`config-bankS.yaml` / `config-bankS.toml` / `symbol_addrs-bankS.txt` (**new**),
`BANK_UNITS += S` in the `Makefile`, and the record added to
`kAllStreamedRecords` in `app/src/bank_overlays.cpp`. The whole module is
disassembled as `asm` (the unit-P/R treatment — spimdisasm splits the embedded
data itself), and `0x56D60` is a multiple of 16, so the assembler adds no `.text`
pad (session 65's +8 trap). Splat's own file-split suggestions agree with the
layout: its last suggestion is `[0xDAB40, asm]`, and the module's last `jr ra` is
at RAM `0x801EE0D0` (ROM `0xDAB30`).

The three targets are also added to `make recomp`'s
`cross_bank.py dispatch --only` list, so the main unit's `jal`s compile as
`LOOKUP_FUNC` and the runtime's DMA-driven bank map picks unit S when it is
resident:

```
RecompiledFuncs/funcs_10.c:24658:    LOOKUP_FUNC(0x801B7FC0)(rdram, ctx);   // hook   @0x8017BA24
RecompiledFuncs/funcs_14.c:6835:     LOOKUP_FUNC(0x801C19B0)(rdram, ctx);   // enter  @0x8017B774
RecompiledFuncs/funcs_5.c:7352:      LOOKUP_FUNC(0x801C214C)(rdram, ctx);   // update @0x8017B8F4
```

**The RAM is a swappable arena, and that is why unit S must be its own unit.**
`0x8019A7C0..0x801F1520` is where record 7 (`0x101D00` → `0x801AD5C0`,
`0x43530`, unit N), record 3 (`0x0EBBD0` → `0x8019EE70`), overlay C (`0x1CE040` →
`0x80197B90`) and units H (`0x712A0`) and M (`0x79750`) also live. In particular
the main unit has its **own** real function at `0x801B7FC0` (overlay C), which is
exactly why that call had to be dispatched rather than left bound.

The swap works in both directions because the game re-streams the mission's banks
on the way out: in the measured run the `0x06 → 0x03` transition is followed
within the same frame by

```
[bank] loading overlay record rom=0x0E4910 ram=0x80197B90 size=0x72C0 (40 functions)
[bank] loading overlay record rom=0x0EBBD0 ram=0x8019EE70 size=0xE440 (96 functions)
[bank] loading overlay record rom=0x101D00 ram=0x801AD5C0 size=0x43530 (300 functions)
```

so `recomp::overlays::load_function_bank`'s `unload_overlapping_overlays` is
undone by the game's own DMA and the mission keeps running. **Session-45 rule, for
the fourth arena in a row:** a streamed module the segment table does not describe
still owns a swappable RAM range.

### What the screen is (developer oracle, session 69)

The **Organize Screen** — the player's army-management screen, reached from the
mission's `R` menu and on the natural `map → mission` route before the battle
(session 67 met the same scene `0x06` there and called it "the briefing"; it is
this screen). The capture shows, over a dark tiled backdrop:

| what | source |
|---|---|
| three counters across the top: **`SOLDIER 030`**, **`CHARACTER 25`**, **`UNIT 03`** | dev capture |
| the **formation grid**: characters standing on a chequered floor of stepped tiles, a **blue** block of tiles and a lone gold tile among the cream ones | dev capture |
| the selected entry's name in the bottom-right plate: **`Scarlet Magi`** | dev capture |
| **`WAR FUNDS`** / **`0001000 Goth`** in the bottom-left plate | dev capture |

Recorded in `docs/scenes.md` as scene `0x06`, with the R menu's observed entries
(`Dispatch`, `Mission Objective`, `End` — the full ordered list is **not** yet
transcribed; only those three were read off while finding the route).

## 3. Verification (what was run)

| run | result |
|---|---|
| the developer's live route (tutorial → practice stage → mission → hold `R` → `→` → `A`) | **the Organize Screen draws** — developer: *"I checked it myself. it works!"*; `docs/proofs/native-organize-screen.png` (their capture) |
| the pre-fix route, `OGRE_SCENE_LOG=1` | scene `0x03 → 0x06 → 0x03` in 68 ms with two stub calls (the "reload"); §1 |
| `tools/elfcheck.py --syms` | `bankS.elf: 0 differing bytes of 909184`, **1382 address-named symbols all at their named address**; all 15 bank ELFs + the main ELF still byte-identical to the ROM |
| `make bank-recomp` | `19 unit(s), 29 record(s), 3094 function(s)`; `cross_bank.py check-banks` → **OK** (no new hazard; unit S is a *single* record, so its calls are same-record and the check is silent by construction) |
| forced `OGRE_SCENE=0x06` | **does not work and is not evidence** (developer): the scene's enter builds the screen from the party/character state the mission/route sets up, so a boot-time forced entry has nothing to display. The **natural route is the only test** for this screen |

**No probes were used.** Nothing in `RecompiledFuncs/` or `Bank*Funcs/` was
hand-edited (`grep -rl probe6* RecompiledFuncs/ Bank*Funcs/ app/` is empty) and
`git -C tools/N64ModernRuntime diff` is unchanged from
`n64modernruntime-ob64.patch`.

**Correction to session 67 §9.2:** it listed scene `0x06` as *"the briefing scene
… still uncompiled"* and as the module the **natural map → mission** flow needs.
The module and the address (`ROM 0x87220` → RAM `0x8019A7C0`, entry `0x801C19B0`,
update `0x801C214C`) are right, and that is the lead this session used — but the
screen is the **Organize Screen**, not a briefing, and it is reached from the
mission's `R` menu as well as from the map's pre-battle flow.

## 4. Files changed

* `config-bankS.yaml`, `config-bankS.toml`, `symbol_addrs-bankS.txt` (**new**) —
  the module at ROM `0x87220` (`0x56D60`) → RAM `0x8019A7C0`, entries
  `0x801C19B0` / `0x801C214C` / `0x801B7FC0`.
* `Makefile` — `BANK_UNITS += S`, and the three targets appended to `recomp`'s
  `cross_bank.py dispatch --only` list.
* `app/src/bank_overlays.cpp` — record 19 in `kAllStreamedRecords`
  (`0x087220` → `0x8019A7C0`, `0x056D60`, `compiled = true`).
* `docs/proofs/native-organize-screen.png` (**new**) — the developer's capture.
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/README.md`, this file.

Generated/ignored, regenerated by the normal targets: `build/bankS.*`,
`BankSFuncs/`, `app/src/bank_funcs.inc`.

`git status --short`: `M Makefile`, `M app/src/bank_overlays.cpp`,
`M tools/RT64` (pre-existing), plus the new files above.

## 5. Next leads

1. **Play the mission on** (still open from session 68): units moving, combat, the
   losing condition, saving from inside. The batch of screens the developer named
   in session 67 — the tooltip, the unit panel and the mission `R` menu — can now
   be compared against the port one by one.
2. **Transcribe the mission `R` menu's entries in order** (developer oracle), and
   add the Organize Screen's own controls (what the cursor does, what buttons
   assign/remove/move a character) to `docs/scenes.md`. Only `Dispatch` /
   `Mission Objective` / `End` were read off interactively this session.
3. **The chunk-DMA false positive in `on_streamed_dma`** now has a reproducible
   shape: `rom - record.rom_start == ram - record.ram_start` with the chunk landing
   exactly on another base. It cost a wrong "two missing modules" lead for a few
   minutes; a cheap fix is to report an unknown module only when the DMA's *first*
   chunk lands on the base (track the first `ram_addr` of a `rom_addr` run), not
   any chunk.
4. **Driving the game from a script is still the slow part.** The tutorial's
   dialogue advances *unpredictably* to the live console's synthetic holds: some
   presses are swallowed and a dialogue line can need five or six attempts, so a
   fixed `sleep`-based route script desynchronises. `OGRE_TAP_SCENE_BUTTON` keys
   the press on the active scene but cannot gate on "this line is on screen". A
   console predicate ("press A until scene/step changes") would pay for itself
   immediately — the developer ended up driving by hand, which is the right
   division of labour but does not scale.
