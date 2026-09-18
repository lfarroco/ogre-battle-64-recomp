# Handoff — 2026-09-18, session 82: neutral encounters now spawn their monster

**Read §1–§3.** The bug was the project's standard shape — a streamed module the
port had no record for — in the one place nobody had run: the battle scene's
setup fragment, which only the neutral-encounter path streams. `tools/arenamap.py`
had listed it since session 73 as *"no observed run streams"*; this session is the
observed run. Fixed by **bank unit AH**; developer-confirmed ("it worked").

## 1. The report and the reproduction

Developer: a **neutral encounter** (a rare map event, documented by the GameFAQs
guide the developer pasted) displayed the wild-monster message, entered the
battle, and **ended in an instant victory with no enemy**. Because the event is
random, the request was to find it without relying on a lucky test run.

The mechanic is fully decodable from the ROM, and that is what made a
deterministic reproduction possible:

* The neutral encounter is **`func_ovlN_801E7940`** (unit N, the mission module
  record 7; called every frame from mission **state 0**, whose handler is
  `0x801af34c`, table `0x801EDC60`).
* It requires a field unit whose flag word satisfies
  `(unit[0] & 0x60011) == 0x40011`, rolls `rand() % 72000 <= 50` (or `10` when
  `func_80186230(17)` is false — the "revisiting the area" case), then reads the
  terrain byte under that unit and indexes two tables in record 7's data:
  * **`0x801ED780`** terrain → encounter group (1 = highway, 2 = Plains,
    3 = Barrens, 4 = Forest, 6 = Highlands, 7 = Marsh, 8/9/10 = the snow types);
  * **`0x801ED79E`** `group*2 + rand()%2` beside a per-mission row from
    `0x801E7E38 + mission*12` (byte − 3), stride `20`: the **class index**.
  * The class is stored as **`class + 0x100`** at `0x801F0E24`, the unit index at
    `0x801F0E20`, and `func_ovlN_801B8D64` sets `*(0x801F0CC4) = 46` and the
    camera/state globals (message id 50 comes from the state-46 handler inside
    `func_ovlN_801B9694` at `0x801BAB90`; class names are in streamedB at
    `0x80190000`). The guide's whole table set reproduces from these two tables.
* A neutral encounter can therefore be forced by patching the two probability
  branches (see §5), and that is how it was reproduced on demand — no luck
  required. The message displayed correctly right up to the bug, confirming the
  class lookup worked and localising the failure downstream.

**The reproduction** (automated, no human): `OGRE_SAVE=assets/save-mission-1.srm`
with the scene-keyed schedule
`"title:start:4,0x12:a:3,0x03:a:14,0x0d:a:14"` reaches the mission; with the force
probe the encounter fires as a unit moves. The run log then showed the whole
failure in two lines:

```
[bank] UNKNOWN module rom=0x23A370 ram=0x801D0860 (0x801D0860 is also where rom=0x244770 loads)
[overlays] streamed function stub called @ 0x801D1508 (not yet loaded)
[bank] loading overlay record rom=0x213AE0 ram=0x801D0860 size=0x16770   <- unit J
[bank] loading overlay record rom=0x22A250 ram=0x801E6FD0 size=0x10120   <- unit X
```

and the capture `docs/proofs/native-neutral-encounter-message.png` shows
`Magnus: "A wild Young Dragon? Here!?"` — then the battle with an empty enemy
side. (The developer independently hit the same event in the live window and
reported "no one spawned (it should have been a young dragon)".)

## 2. Root cause

Scene `0x0E`'s enter (`func_80177754`, main ELF; the same loader/`jal` arm exists
in scene `0x0D`'s enter `func_80178568`) DMAs a small **setup fragment** before the
battle banks:

```
icache func_800900C0(0x801D0860, 0x801D16D0)      ; code  0xE70
dcache func_80090010(0x801D16D0, 0x801D16E0)      ; data  0x10
DMA    func_8009DA50(0x23A370, 0x801D0860, 0xE80) ; ROM 0x23A370..0x23B1F0
bss    none
jal    0x801D0AAC   (or 0x801D1508 on the other flag branch)
```

The port had **no record** for ROM `0x23A370` → RAM `0x801D0860`: the main ELF's
calls are `LOOKUP_FUNC(0x801D0AAC/0x801D1508)` (`RecompiledFuncs/funcs_3.c:14795`,
`funcs_6.c:2622/2722`) and there was nothing to resolve them to, so they ran the
runtime's streamed stub. `func_801D1508` is the battle participant setup (it scans
the 30 unit slots at `0x801F0CD0` for a free one, `memset`s 25 bytes and tags it),
and `func_801D0AAC` is the larger setup that reads the 16-byte table at
`0x801D16D0`. **Normal battles still worked** because their units already exist on
the field; a neutral encounter's wild unit is *created* here, so the battle began
with an empty enemy side and the victory check fired immediately.

## 3. Fix — bank unit AH

New streamed-overlay unit (`bankRec10s`), RAM `0x801D0860` is a sub-range of unit
J's `bankRec10a`, i.e. another bank of the same arena, so it cannot live in J (or
in unit I, which shares the same base) — the session-45 rule.

* `config-bankAH.yaml` / `config-bankAH.toml` — code `0x23A370..0x23B1E0`
  (`asm`), data `0x23B1E0..0x23B1F0` (16 zero bytes, `data`); the gap
  `0x213AE0..0x23A370` (unit J and the `0x120` pointer blob at `0x23A250`) is an
  opaque `bin`. Code size `0xE70` is a multiple of 16, so no assembler pad.
* `symbol_addrs-bankAH.txt` — the three real entries
  **`0x801D0860`, `0x801D0AAC`, `0x801D1508`** (the external `jal` targets).
* `Makefile` — `BANK_UNITS += AH`.
* `app/src/bank_funcs.inc` — regenerated (`gen_bank_funcs.py`):
  `{ 0x0023A370, 0x801D0860, 0x00000E80, 0x801D16E0, kAH_bankRec10sFunctions, 3 }`.

## 4. Verification

* `make bank` / `tools/elfcheck.py --syms`: **bankAH.elf 0 differing bytes of
  2339312**, 73 address-named symbols at their named address; every other ELF
  unchanged (main ELF still 0/41943040).
* `make bank-recomp`: 34 units / 44 records / 3512 functions;
  `tools/cross_bank.py check-banks` **OK** (24 accepted pre-existing unit-C
  hazards, 0 new; "no bank unit calls into a swappable range it does not own").
* **Fixed-build run** (same automated route, `OGRE_DMA_TRACE=1`,
  `OGRE_CAPTURE_PRESENT`, ~260 s): the log has **no `UNKNOWN module` and no
  `streamed function stub`**, and instead
  `[bank] loading overlay record rom=0x23A370 ram=0x801D0860 size=0xE80 (3 functions)`;
  the encounter message renders and the battle shows the enemy unit with HP
  bars and damage (`docs/proofs/native-neutral-encounter-battle.png`).
* Developer ran the window and confirmed: **"it worked"**.

## 5. Probe (reverted)

A single temporary probe, tag **`probe82`**, in generated
`BankNFuncs/funcs_4.c` (unit N, `func_ovlN_801E7940`), three sites:
`0x801E7B08` (`if (ctx->r3 != 0)` → `if (0)`), `0x801E7C70`
(`if (ctx->r2 == 0)` → `if (0)`), and `0x801E7BBC` (the unit flag test → `if (0)`,
so a stationary unit also qualifies). **Reverted by `make bank-recomp` and
proven**: `grep -rl probe82 RecompiledFuncs/ Bank*Funcs/ app/` is empty and
`BankNFuncs/funcs_4.c` is back to `if (ctx->r3 != 0)`. `build-app` and
`build-null` rebuilt from the clean tree. (The probe links the class-index table
to a forced roll, so a future session that wants a neutral encounter on demand can
re-apply exactly these three edits — they are the shortest path to this scene.)

## 6. Files changed

* `config-bankAH.yaml`, `config-bankAH.toml`, `symbol_addrs-bankAH.txt` (new)
* `Makefile` (one line: `BANK_UNITS += AH`)
* `app/src/bank_funcs.inc` (generated; 34 units / 44 records / 3512 functions)
* `PLAN.md`, `docs/DECISIONS.md`, `docs/scenes.md`
* `docs/proofs/native-neutral-encounter-message.png`,
  `docs/proofs/native-neutral-encounter-battle.png` (new)
* `BankAHFuncs/` (generated, gitignored)

## 7. Notes for the next session

* `tools/arenamap.py` (session 73) already printed the fragment
  (`0x0023A370 → 0x801D0860`) and marked it "1 uncompiled … no observed run
  streams". **The tool was right; the run coverage was not.** Any remaining
  "uncompiled, no observed run" entry is only as good as the routes tried — the
  battle setup is reached by scene `0x0E`'s enter, which a mission that never
  fights never runs.
* The neutral-encounter tables (`0x801ED780`, `0x801ED79E`, `0x801E7E38`) are
  decoded and match the GameFAQs guide exactly; the class-name table is at
  `0x80190000` in streamedB, stride `72`, name field at `+4` of
  `0x80187A30 + class*72`.
* The battle scene's fragment runs at `0x801D0860`, but *unit J* and *unit I*
  also own that RAM for other scenes; the runtime's DMA-driven bank map picks the
  resident module, which is why the record had to be registered, not the calls
  patched.
