# Handoff — 2026-09-17, session 71: **the tutorial's practice stage is bank unit T (and its mode-2 sibling is unit U)**

> **Read §1 if you are here for "the tutorial puts you on a map with no
> instructions and no enemies".** The tutorial's practice stage is scene `0x03`
> entered with `D_80193700 == 1` (descriptor **`0x8018F364`**, mask
> `0x0004038C`). Its own module is the **second bank of record 18's arena** —
> ROM `0x1C32D0` (`0x5D50`) → RAM `0x8022A860` — and the port had no code for
> it, so four call sites hit the runtime's logging stub and the tutorial's
> instruction/enemy-setup module never ran. It is now **bank unit T**; the same
> arena's mode-2 bank (ROM `0x1C9020`, `0x5020`) is **bank unit U**. The run
> reports **0 stub calls** and the tutorial instruction box (`TUTORIAL / These
> are called the Field`, `docs/proofs/native-tutorial-practice-field.png`) and
> the stronghold/`Use Item` lessons play.

## Goal and result

**Goal (developer):** *"let's fix the tutorials. they have multiple situations
(combat, items, dialogue), so I guess that they should be a good place to
implement missing game parts. if we open the tutorial scene (either normally, or
going directly to the scene, then ending the deneb dialog … and if we open any
of the options …, we are placed in a map and nothing happens, without enemies.
in the regular game, there would be tutorial instructions and enemies in the
map."*

**Result: the practice stage runs its own module, developer-confirmed live.** One
new bank unit (T) and a second for the arena's other bank (U); the tutorial's
instruction text renders, the lessons advance, the run is stub-free, and the
developer confirms the **enemy units now appear on the practice field** as well.
The enemy constructor (**unit R**, session 68) is a separate module that loads on
this route; the tutorial module's stub was what stopped the practice's setup from
completing, which is why the same map previously had neither instructions nor
enemies.

## 1. The route, and the two scene-`0x03` descriptors

The tutorial is scene `0x17`. Its accessor `func_801862F0` (asm/40E80.s
@0x801862F0) picks between two descriptors on bit 3 of `D_80196B0C`:

| descriptor | enter | update | hook | leave | mask |
|---|---|---|---|---|---|
| `0x8018FE50` (bit 3 clear) | `0x8019B2C0` | `0x8019AD04` | `0x8019B028` | `0x8019B094` | `0x00060000` = records **17, 18** |
| `0x8018FE64` (bit 3 set) | `0x8019B540` | `0x8019B588` | 0 | 0 | `0x00020000` = record **17** only |

Driven this session with `OGRE_SCENE=tutorial` (the forced poke works for this
scene; the developer's title route works too) plus scene-keyed taps:

```sh
OGRE_SCENE=tutorial OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_SCENE_BUTTON="0x17:a:40,0x02:a:10,0x0d:a:80,0x03:a:60" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=80000 ./build-app/ogrebattle64
```

The A presses end Deneb's dialogue and pick a lesson; the sequence then runs
`0x17 → 0x02 → 0x0D → 0x03` (measured `[scene] t=9756 ms id=0x0003
descriptor=0x8018F364 mask=0x0004038C`). The two scene-`0x03` descriptors differ
only in the mask, selected by the **mode word `D_80193700`** in the accessor
`func_80173700` (asm/40E80.s @0x80173700):

* `D_80193700 == 0` → `0x8018F350`, mask `0x0000038C` (records 2,3,7,8,9) — the
  normal mission;
* `D_80193700 != 0` → `0x8018F364`, mask `0x0004038C` — **+ record 18**, the
  tutorial practice.

A second route (`0x17:a:4,0x17:down:2,0x17:a:20,…`) selects the other lesson and
lands on `0x8018F350` (mode 0, the normal mission) — **also 0 stubs**. So the
"any option" the developer tried is the mode-1 practice, and mode 0 already
worked.

## 2. The bug: four stubs into record 18's arena

The pre-fix run logged, at the practice entry:

```
[overlays] streamed function stub called @ 0x8022A860
[overlays] streamed function stub called @ 0x8022A88C
[overlays] streamed function stub called @ 0x8022AA94
[overlays] streamed function stub called @ 0x8022B36C
```

4608 calls in a 40 s run, dominated by `0x8022A860`/`0x8022A88C`. The scene
loaded record 18 (`rom=0x1BA020 ram=0x80220F60`, unit B) and the mission records,
but **not** the module those four addresses belong to.

`tools/guestmap.py` put all four inside RAM that other units also own
(`0x8022A860` in unit C's record 14, `0x8022B36C` in unit L's `bankRec14a`), and
the bytes at those addresses in unit C's layout are **strings** (`0x8022A860` is
inside `"the Valiant"`/`"of the Wind"`), so the calls were not into the resident
module at all.

### The loader, at instruction level

`func_ovlB_80221D50` (record 18 = **unit B**; ROM `0x1BA020 + 0xDF0` = `0x1BAE10`,
RAM `0x80221D50`) is the arena's loader. It indexes **two** tables by
`D_80193700`:

```asm
80221de0  lbu   v1, D_80193700
80221de8  sll   v0,v1,2 ; addu v0,v0,v1 ; sll v0,v0,3   ; v1 * 0x28
80221df4  addiu v1,v1,-0x6224    ; 0x80230000-0x6224 = 0x80229DDC   (segment table)
80221dfc  addu  s0,v0,v1
80221e1c  lw a0,0x18(s0) ; lw a1,0x1C(s0) -> func_800900C0(code)   ; icache
80221e2c  lw a0,0x20(s0) ; lw a1,0x24(s0) -> func_80090010(data)   ; dcache
80221e3c  lw a0,0x08(s0) (rom) ; lw a2,0x0C(s0) (rom end)
80221e44  lw a1,0x00(s0) (ram) ; subu a2,a2,a0 -> func_8009DA50     ; the DMA
80221e50  lw a0,0x10(s0) ; lw a1,0x14(s0) -> func_80093380          ; zero BSS
80221e70  ... *0x1C index into 0x80229E88, read +0x08 -> jalr        ; entry
```

For `D_80193700 == 1`, the segment entry is at `0x80229DDC + 0x28 =
0x80229E04`:

| field | value |
|---|---|
| ram start / ram end | `0x8022A860` / `0x802305F0` |
| rom start / rom end | `0x001C32D0` / `0x001C9020` (`size 0x5D50`) |
| code / data | `0x8022A860..0x8022CBF0` / `0x8022CBF0..0x802305B0` |
| BSS | `0x802305B0..0x802305F0` |

and the mode record at `0x80229E88 + 0x1C = 0x80229EA4` calls `+0x08 =
0x8022AA94`. So the DMA is
`func_8009DA50(0x1C32D0, 0x8022A860, 0x1C9020-0x1C32D0 = 0x5D50)`, and that this
is real is confirmed by the port's own `OGRE_DMA_TRACE_FULL` dump: a 44-chunk
`0x200`-byte series `rom=0x1C32D0 ram=0x8022A860 … rom=0x1C8ED0 ram=0x80230460`.

**What the module is:** its data half carries the tutorial text the developer
described — `Stationing unit will gather information.`, `Displays the report on
the stronghold`, `Purchases items at a shop in the stronghold.`, `This will end
the instruction on Stronghold Command. Proceed?`, `This concludes the tutorial
on Use Item command. Proceed?` — i.e. the **tutorial instruction/lesson module**
(combat/stronghold/items), which is why the practice map was inert without it.

`0x8022A860` is the module entry (unit A's record 3 `jal 0x8022A860` @0x8019F7AC,
in the `D_80193700 != 0` arm at 0x8019F5B4); `0x8022A88C` is called by unit N's
record 7 @0x801B054C; `0x8022AA94` is the mode-table callback; `0x8022B36C` is
unit A's other practice call @0x8019F5CC.

## 3. The fix: bank unit T (and unit U)

`config-bankT.yaml` / `config-bankT.toml` / `symbol_addrs-bankT.txt` (**new**),
`BANK_UNITS += T` in the `Makefile`, and record **20** in
`kAllStreamedRecords` (`app/src/bank_overlays.cpp`):

```
0x1C32D0 (0x5D50) -> RAM 0x8022A860   // bankRec18b, the tutorial-practice bank
```

* Code/data split is the loader table's own `code_end` `0x8022CBF0` =
  ROM `0x1C5660` (last `jr ra` at `0x1C5658` plus its delay slot; the first
  instruction string is at `0x1C5660`). Code size `0x2390` is 16-aligned, so the
  assembler adds no `.text` pad (session 65's +8 trap).
* Forced entries (`symbol_addrs-bankT.txt`): `0x8022A88C`, `0x8022AA94`,
  `0x8022B36C`. `0x8022A860` is the segment start and spimdisasm emits it.
  All four are real entries in the module (each preceded by a `jr $ra`) — they
  are invisible to a per-record disassembly because only *other* records `jal`
  them (session 67's `get_function` no-interior-fallback wall).
* BSS `0x802305B0..0x802305F0` is recorded in `tools/gen_bank_funcs.py`'s
  `RAM_END` (`0x1C32D0: 0x802305F0`), so the runtime zeroes it on load.

**Unit U is the arena's other bank** (mode `D_80193700 == 2`): segment entry
`0x80229E2C`, `{ ram 0x8022A860..0x8022F880, rom 0x001C9020..0x001CE040,
code 0x8022A860..0x8022AA00, data 0x8022AA00..0x8022F880 }`, mode record
`0x80229EC0` calls `0x8022A87C` (a bare `jr ra` — the bank is mostly its data
half). It is `config-bankU.yaml`/`.toml`/`symbol_addrs-bankU.txt`, record
**21**. No observed route loads it (both driven lessons selected modes 0 and 1),
but it is a bank of the **same RAM** as unit T and unit L, so compiling it is
what makes the runtime's DMA-driven bank map evict the previous bank when the
game does stream it — the session-45/59/67 mis-binding class.

`make bank-recomp` → `21 unit(s), 31 record(s), 3155 function(s)`, `check-banks
OK`; `tools/elfcheck.py --syms`: `bankT.elf: 0 differing bytes of 1871904`, 380
address-named symbols at their address; `bankU.elf: 0 differing bytes of
1892416`, 148 symbols.

## 4. Verification (what was run)

All on `build-app/ogrebattle64` rebuilt from this tree (`make bank-recomp` →
`cmake -S app -B build-app` → `cmake --build build-app -j8`).

| run | env | result |
|---|---|---|
| practice (mode 1) | `OGRE_SCENE=tutorial OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="0x17:a:40,0x02:a:10,0x0d:a:80,0x03:a:60"` `OGRE_SCENE_LOG=1` 70–80 s | `0x17 → 0x02 → 0x0D → 0x03 (0x8018F364)`; **0 `streamed function stub`**, 0 `UNKNOWN module`; `[bank] loading overlay record rom=0x1C32D0 ram=0x8022A860 size=0x5D50 (54 functions)`; the tutorial instruction box renders and advances (`docs/proofs/native-tutorial-practice-field.png`, `-command.png`) |
| normal-mission lesson (mode 0) | same but `0x17:a:4,0x17:down:2,0x17:a:20` and `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1` | `0x17 → 0x02 → 0x0D → 0x03 (0x8018F350)`; **0 stubs**; no `0x1C90xx` DMA (unit U not exercised) |
| unit U build | `make build/bankU.elf` + `elfcheck` | byte-identical; entry `0x8022A87C` present |

The captures were taken from the presented swap chain
(`OGRE_CAPTURE_PRESENT`, every 25th present): the field map with the tutorial
box, the command-icon lesson, and the stronghold/town scenes after it.

## 5. Driving facts worth keeping

* **`OGRE_SCENE=tutorial` really does reproduce the developer's bug** — no save
  and no title route needed; the A presses in `0x17` both end Deneb's dialogue
  and confirm a lesson.
* The tutorial's practice is **not a separate scene**: it is scene `0x03` (the
  mission) entered with `D_80193700 != 0`, so the mission modules (units N, P, Q,
  R, O) and record 18 all load alongside unit T. That is why the fix is a *bank
  of record 18's arena*, not a new scene.
* The console `c` command during the sequence shows the practice step word:
  `step=1082` (session 58's decoded opening ended at 1073 = the Prologue card;
  the tutorial lessons follow it in the same step table).
* `D_80193700` is set by `func_80173694(a0)` (asm/40E80.s @0x80173694), whose
  callers are record 17 (`0x8019B624` calls it with 1) and `0x8019ABDC` (calls
  it with a variable from `0x8019C943`) — the lesson selector.

## 6. Files changed

* `config-bankT.yaml`, `config-bankT.toml`, `symbol_addrs-bankT.txt` (**new**) —
  ROM `0x1C32D0` (`0x5D50`) → RAM `0x8022A860`; entries `0x8022A88C`,
  `0x8022AA94`, `0x8022B36C`.
* `config-bankU.yaml`, `config-bankU.toml`, `symbol_addrs-bankU.txt` (**new**) —
  ROM `0x1C9020` (`0x5020`) → RAM `0x8022A860`; entry `0x8022A87C`.
* `Makefile` — `BANK_UNITS += T U`. (No `dispatch --only` addition is needed:
  the callers into both banks are bank units, which already emit `LOOKUP_FUNC`
  for any target outside their own records, and the main unit does not call
  these addresses.)
* `app/src/bank_overlays.cpp` — records 20 and 21 in `kAllStreamedRecords`.
* `tools/gen_bank_funcs.py` — `RAM_END` entries for `0x1C32D0` (unit T) and
  `0x1C9020` (unit U).
* `docs/proofs/native-tutorial-practice-field.png`,
  `docs/proofs/native-tutorial-practice-command.png` (**new**) — the tutorial
  instruction box on the practice field.
* `PLAN.md`, `docs/scenes.md`, `docs/DECISIONS.md`, `docs/README.md`, this file.

Generated/ignored, regenerated by the normal targets: `build/bankT.*`,
`build/bankU.*`, `BankTFuncs/`, `BankUFuncs/`, `app/src/bank_funcs.inc`.

`git status --short`: `M Makefile`, `M app/src/bank_overlays.cpp`,
`M tools/gen_bank_funcs.py`, `M tools/RT64` (pre-existing), plus the new files
above.

**No probes were used.** Nothing in `RecompiledFuncs/` or `Bank*Funcs/` was
hand-edited and `tools/N64ModernRuntime` is untouched.

## 7. Next leads

1. **The tutorial's remaining lessons.** The module's strings list lessons beyond
   the ones captured (`Use Item command`, `Stronghold Command`); play them
   through and compare against `docs/scenes.md`.
2. **Exercise mode `D_80193700 == 2`** (unit U). No driven lesson selected it;
   if a later lesson does, the run should show `[bank] loading overlay record
   rom=0x1C9020 ram=0x8022A860` and 0 stubs.
3. **The chunk-DMA false positive in `on_streamed_dma`** is still open
   (session 69 §5): report an unknown module only when the DMA's *first* chunk
   lands on the base.
