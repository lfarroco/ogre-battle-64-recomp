# Item Randomizer

**EXPERIMENTAL.** This mod is still in development: its behaviour can change
between builds, and it has no options to tune. It rewrites items the game is
about to grant, so keep a backup of any save you use it on.

An Ogre Battle 64: Recomp code mod. An item the party is given after defeating an
enemy unit, and an item found on the map, is replaced by a different item from
the game's own item table. The replacement may be anything in the table.

Shops, cutscene gifts and quest items are left alone.

Two mechanisms are needed, because the game grants items by more than one route.
Where the granted id is read out of a table in RAM, the mod rewrites the table,
so the message the player sees and the inventory agree. Where the id arrives in a
register from streamed bank code the mod cannot hook, the mod watches the
inventory list instead: the message names what the game rolled, and the inventory
and the item screen show the replacement.

## How it works

The game gives an item with one routine, `func_ovlN_801DC740` (bank unit N
record 7, streamed RAM `0x801AD5C0`). It searches a list for a matching id and
increments that entry's count, or claims the first empty slot and writes a count
of 1.

**There are two lists, and bit 15 of the item word selects one:**

| bit 15 | list | slots | definition table | id space |
|---|---|---|---|---|
| set | `0x80196B20` | 278 | `0x8018C42C`, stride `0x20`, name pointer `+0x00`, category `+0x04` | equipment, ids 1..277, 278 = "None" |
| clear | `0x80193AE0` | 40 | `0x8018E6EC`, stride `0x0C`, name pointer `+0x00`, class `+0x04` | consumables, ids 1..40, 0 = "None" |

Both use the record layout `{u16 id at +0x00, u8 used at +0x02, u8 count at
+0x03}`. "Heal Leaf" cannot be produced from the equipment table at all, so a mod
that watches only `0x80196B20` is blind to every consumable grant.

The consumable table base is `0x8018E6EC`, read by `func_8016F500`
(`0x8016F500`: `id * 0x0C` then `lw -6420(at)` with `at = 0x80190000`). An earlier
build used `0x8018E6F0` with the name at `+0x04`, which reads the next entry's
`+0x08`: every consumable failed the name check, so no consumable was ever
rewritten.

The consumable entry's byte at `+0x04` separates the usable items (`0x00` to
`0x02`) from the key items (`0x03` and `0x04`, "Medal of Vigor" through "Pedra of
Flame", including "Package for Gelda"). The mod keeps a key item as the game
granted it, because a chapter cannot be completed without it.

`func_ovlN_801DC740` is streamed bank code, so a code mod cannot hook it: a hook
resolves through `get_vrom_to_section_map()`, which holds the base ELF's sections
(`.entry`, `.main`, `.streamedA/B/C`) and not the bank units registered in
`app/src/bank_overlays.cpp`. See `docs/guides/app-build.md` → "What a hook can
reach".

The mod therefore runs from a base-section frame hook, `func_80072944` (a
`.main` function that runs once per frame; `mods/exp-overflow` measured 3072
entries over a 3203-frame run), and does two things:

1. **Rewrites the three sources of a granted id.** The popup and the grant both
   read one value out of each, so a rewrite here is what makes the message and
   the inventory agree.
   * **Table O**, the map-object table at `0x801EDB38`: 35 entries of 8 bytes,
     `{u16 type, u16 item_a, u16 item_b, u16 item_c}`. The game scans it for the
     object's type (bankN `0x801ADD3C`), picks a column with `rand() % 3` (bankN
     `0x801ADD68`) and reads it (bankN `0x801ADDB0`). One value then becomes the
     popup id and the granted item.
   * **Table P**, the per-map ground-item list: a `u8` count at `0x801F1002` and
     `u16` ids at `0x801F1004 + i*2`, parsed at map load by `func_ovlR_80215C38`
     (bankR record 9d). A pickup reads `idx = lw(0x801F0E24)`, takes
     `lhu(0x801F1004 + idx*2)`, writes it to the popup field `0x801F36C2`
     (`0x801BB8FC`) and passes the same value to the grant (`0x801BB930`).
   * **The pending battle-reward queue** at `0x801936D8`, 20 `u16`. The mission
     bank serves a battle reward from it: `func_ovlN_801E49D0` takes
     `lhu(0x801936D8)` at `0x801E5230` with `lhu(0x801936DA)` as the fallback
     (`0x801E5254`), stores it to `0x8021C760`, grants it at `0x801E53DC` and
     clears `0x801936D8` at `0x801E53F4`. The reward message is built from the
     same stored word, so a rewrite here makes the message and the inventory
     agree. **This is the only place a battle reward can be replaced**: the id is
     in RAM before the grant, and the drop table is not read at that moment.
     Verified on the developer's save: with the drop table's entry 10 column C
     rewritten 155 → 17, the queue still held `0x809B` (155) and the reward
     granted 155, which the party already owned seven of, so it merged into that
     stack and the list watch had nothing to rewrite. With the queue rewritten,
     two rewards came back name for name — vanilla Iron Claw (94) became
     `acquire 3` (Falchion) and vanilla Leather Armor (155) became
     `count-up 193` (Magician's Robe, stack 1 → 2) — and the reward message named
     the same item in both cases.

   The two tables are in record 7's RAM window, which unit X also owns, so every
   write to them is gated on a record-7 fingerprint (`lhu(0x801EDB38) == 0x27`
   and `lhu(0x801EDC48) == 0x50`). On the pickup save the probe reads `39 80 1`,
   which is record 7 resident and the drop table present. The queue is plain RAM
   and needs no fingerprint. Each item word's bit 15 is preserved, because it
   selects the list below.
2. **Watches both lists.** A slot that was **empty and has just been filled** is
   an acquisition: the mod writes a replacement id into that slot. This is what
   makes one find add one item. A slot that merely changed its id is the game
   re-sorting the list and is left alone. A frame that fills **more than one
   record** is a bulk grant by the game rather than a set of acquisitions, so the
   shadows are re-primed and nothing is rerolled. Two things produce a bulk
   grant. A save load rewrites both lists wholesale; its precise signal is a hook
   on `func_800749C0`, the save-field reader: it walks the table at `0x800A824C`,
   and entry 1 is the packed blob that carries both item lists. It is `.main`
   code and regenerates cleanly; the used-count rebuild it calls,
   `func_8016B774`, does **not** (a hook on it fails `get_function` and crashes
   the load). The record count is the fallback for a load the hook misses, and
   the only signal for the map loader, which no hook can reach: a map load grants
   the party's starting items and the created units' class equipment in one
   frame. One interaction grants one item, so a single filled record is still an
   acquisition.

   A scene change is deliberately **not** the signal. The game grants items on
   scene changes — a mission's unit-init grants, and a battle's reward as the
   battle scene ends — so a re-prime keyed on the scene word swallowed exactly
   the grants the mod exists to replace. That was a real defect in this mod, and
   the developer saw it as a reward that was "the expected item from the loot
   table, not a random one".

3. **Recomputes the used count (`+0x02`) of a record the game just filled.** The
   item screen draws `+0x02` then `+0x03` (`used / owned`) and the equip test is
   `sltu(+0x02, +0x03)`. `func_ovlN_801DC740` writes the id and `+0x03` only, so
   the byte it inherits is whatever the record carried. A record can be empty
   with `+0x02 != 0`, and the next grant into it then reads `1/1` under the new
   item's name and refuses every equip.

   The game recomputes that byte from the roster in `func_8016B774`
   (`0x8016B774`, `.streamedB`, size 0x29C). A mod cannot call it: `jal` from the
   mod's own `0x81000000` to game code fails `relocation truncated to fit:
   R_MIPS_26`, and a hook on it crashes the save load. The mod reimplements it
   instead. One increment per roster reference:

   | source | structure | ids | list |
   |---|---|---|---|
   | units | `0x80197210`, stride `0x19`, 30 records, gate `+0x01 & 1` | 10 bytes at `+0x0D` | 40-slot |
   | characters | `0x80193BE0`, stride `0x38`, 100 records, gate `+0x11` | 4 `lhu` at `+0x2A/2C/2E/30` | 278-slot |
   | class | `0x80187C62`, stride `0x48`, key `+0x79`, ids `+0x00 + i*2` | 4 per character, from the class byte at `+0x12` | 278-slot |

   The correction is written after the list walk, for every record the game
   newly filled, and only while the record still holds the id the mod wrote. The
   value is the count for the replacement, so it is also right when a unit
   genuinely uses that item — the case a blind clear of `+0x02` would corrupt.
   Nothing is written while the roster is not in RAM, because a scan of an
   unloaded roster counts 0 for everything.

The mechanisms would otherwise fight. The memo is an involution (`X` becomes
`Y` and `Y` becomes `X`), so a grant of a value the mod itself produced maps back
to the original. The mod flags every id it has produced (`s_memo_target`) and the
table walks, the queue walk and the list watch all leave those alone, the list
watch with an `already-replaced` line. Verified before the flag existed: with
`drop-table 49 140` in the log, writing 140 into an empty slot produced
`acquire 140 30 0 1` then `replace 140 49 30 0`.

The mapping is also an involution for a second reason: the game grants an item by
searching the list for its id and merging, and the list holds no `X` after a
rewrite, so a second grant of `X` maps to `Y` again and a second record receives
an id the party already owns. `replacement_for_slot` catches that case, rolls a
different item, and reports `dup-fix`. Two records with one id split the game's
own used count across them: `func_8016B6FC` and the used-count rebuild both stop
at the first matching record, so the second record keeps `+0x02 = 0` and the
equip test reads the first record's numbers.

## Where it does not reach

**The party's starting items are left as the game granted them.** A new game
builds the party's item lists during the opening, and a map load grants a
starting set in one frame: `func_ovlR_80215C38` (bankR record 9d), called once
per map from bankN `func_ovlN_801AE880+0x764` (`jal 80215c38` at `0x801AEFE4`),
grants `1,1,1,1,1,8,8,22,22` at `0x80217068` when `lbu(0x8018F4A1)` is 63/64,
else `2..5/8/21..23` at `0x802170CC`, and equips each unit it creates from that
unit's class row through `func_ovlR_801DD430` (`0x80216F10`-`0x80217028`). Those
records appear several per frame, so the multi-record rule re-primes both lists
instead of rerolling. Measured on a New Game from the title: frames 13907, 14753,
14754 and 14755 filled 9, 9, 8 and 14 records; before the rule the mod replaced
all 39 and reported `frame acq countup reroll 13907 9 0 9`, after it each of the
four frames reports a `re-prime` and no `replace`.

**The drop table covers the map-object path, and not the battle reward.** Table O
`0x801EDB38` is read by `func_ovlN_801AD6BC`, whose one call site is
`0x8019F4AC` in the map scene's image (scene `0x03`), and the entry it selects is
keyed on `lbu(0x80197708)`, a map object's type. It writes map-object action
fields (`0x801F0E20`, `0x801F0E00`) and the popup id `0x801F36C2`. The battle
reward is a different path: `func_ovlN_801E49D0` reads the saved halfword
`0x801936D8` (`0x801E5230`, fallback `0x801936DA`), grants it at `0x801E53DC`,
and clears it at `0x801E53F4`. A constant-base scan of every unit finds no writer
of `0x801936D8` except the save-field loader `func_80185128`, so the id arrives
from a loaded save. The mod cannot rewrite it, and the list watch below is what
randomizes that grant. See `docs/notes-drop-table.md` §0-§1.

**One find adds one item.** Verified on the developer's pickup save: the game
granted 205 Old Clothing, the mod had already rewritten that table entry to 10
Glamdring, the grant put Glamdring in slot 21, and the frame line read
`frame acq countup reroll 8975 1 0 0` — one acquisition, no reroll, because the
list watch recognised the id as the mod's own table value. An earlier report of
"one find produced four items" was the save-load misread (four consumable records
read as four acquisitions, fixed by the scene-change re-prime) plus an item
screen that already held the other names.

**A merge into an existing stack leaves the slot's id unchanged**, so the id
shadow cannot see it. At log level 2 the count shadow reports it as
`count-up <id> <slot> <old> <new>`, and the per-frame line reports how many slots
one interaction filled (`frame acq countup reroll <frame> <a> <c> <r>`).

## Checking the used count against the game

`tools/refscan.py` rebuilds the same `+0x02` column offline from an
`OGRE_DUMP_RDRAM` image and diffs it against the byte the game has in RAM:

```sh
OGRE_DUMP_RDRAM=/tmp/run.bin OGRE_EXIT_AFTER_MS=180000 \
  mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin
make refscan DUMP=/tmp/run.bin          # mismatches 0
python3 tools/refscan.py /tmp/run.bin --list      # every occupied record
python3 tools/refscan.py /tmp/run.bin --item 117  # which records credit one item
```

`mismatches 0` means the reimplementation reproduced the game's own column on
that image. Three of the developer's dumps give 0 over 21, 22 and 25 occupied
equipment records and all of the consumable records. `--item` names the records,
which is how the count decomposes: in one dump item 117's 2 comes from character
records 6 and 17, both class byte 17, each crediting 117 from its class row
(`class[0] = 117`).

The dump has to be taken with the party's items loaded. `Dump.half` in
`tools/rdram.py` applies the image's byte order, including the XOR-2 that a
hand-rolled halfword read misses (a local one read 516 for 117).

**The write itself is not exercised by any run so far.** `fix_record_use` only
writes when the game's byte disagrees with the rebuild, and nine runs produced no
acquisition that inherited a non-zero `+0x02`. What is proven is the value it
writes, which is the reconstruction above.

## Options

The mod has none. The start screen's MODS panel draws no rows under it, and it
neither reads nor writes `mod_config/ogre_item_randomizer.json`. The behaviour is
fixed:

| behaviour | value |
|---|---|
| what a replacement may be | anything in the item table |
| how often | every eligible acquisition is rerolled |
| a frame that fills more than one record | a bulk grant by the game: the shadows re-prime and nothing is rerolled |
| shop purchases | left alone, recognised by the gold they cost |
| the seed | new every boot, derived from the frame counter |
| the log | silent in a shipped build |

The diagnostics are the compile-time `LOG_LEVEL` in `src/item_randomizer.c`:
0 is silent, 1 reports each replacement and each save-load re-prime, and 2 also
reports every acquisition. Raise it and rebuild to diagnose. The lines, in the
order they appear in this file:

| line | fields |
|---|---|
| `start verb`, `start seed` | the run's log level and derived seed, once |
| `acquire` / `acquire-consumable` | `<id> <slot> <used +0x02> <count +0x03>` |
| `replace` / `replace-consumable` | `<from> <to> <slot> <list>` |
| `dup-fix` | `<original> <memoised> <fresh> <list>` |
| `keep-quest` | `<id> <slot> <class byte> <count>` |
| `count-up` | `<id> <slot> <old count> <new count>` |
| `drop-table` | `<from> <to> <entry*3+column>` |
| `pickup-table` | `<from> <to> <index>` |
| `reward-queue` | `<from> <to> <queue slot>` |
| `already-replaced` | `<id> <slot> <list> <count>`, the grant of a value this mod has already produced |
| `re-prime` | `<changed> <total> <list> <forced>`; `forced` is 1 when the save-field reader ran, and a bulk grant shows `changed` > 1 with `forced` 0 |
| `acquire ref game` | `<id> <scan count> <game +0x02> <characters loaded>`, with `DIFF` in place of `game` when the two disagree. The scan is recomputed for that frame, so a `DIFF` is a real disagreement and not a stale read |
| `frame acq countup reroll` | `<frame> <acquisitions> <count-ups> <rerolls>` |
| `frame uses fixed stale skipped` | `<frame> <corrections> <stale records> <skipped, roster not in RAM>` |
| `use` | `<id> <slot> <old +0x02> <new +0x02>`, one per corrected record |
| `probe first last present` | `<lhu 0x801EDB38> <lhu 0x801EDC48> <record 7 resident>` |
| `save-read` | the save-field reader ran, so the next tick re-primes instead of rerolling |

At most 20 event lines per frame, then one summary, because an unbounded
per-event `fprintf` makes combat unplayable.

An acquisition the mod cannot find a replacement for is left as the original
item rather than dropped.


The log needs the port's `recomp_log` export (`librecomp/src/mod_config_api.cpp`,
a gitignored vendored tree). A build without it refuses this mod at load with
`Imported function not found:*:recomp_log`.

## Building

```sh
make mod-syms        # writes mods/reference/dump.toml (needs the linked ELF)
make example-mods    # writes build/mods/item-randomizer.nrm
```

`make example-mods` uses a `mips-linux-gnu-gcc` on `PATH` and falls back to
`gcc-mips-linux-gnu` in a `debian:bookworm-slim` Docker container. The build sets
`-mno-check-zero-division`: gcc's MIPS default emits a `teq` after each division
to trap a zero divisor, and `RecompModTool` has no case for `teq`, so the mod
fails with `Unhandled instruction: teq` → `Failed to recompile mod`.

## Installing

Copy `item-randomizer.nrm` into the client's `mods/` folder — the folder next to
the executable, alongside `saves/`. The client opens every `.nrm` there on
startup and lists it in the start screen's **MODS** panel. The manifest ships it
off by default, so the package plays the vanilla game until the player opts in.

An installed mod makes the start screen appear even when a ROM is ready, because
the screen owns the mod toggles. `OGRE_MOD_TEST=1` starts the game with
`mods.json` as it stands and no screen, for a scripted run.

## Testing with a save

`run-with-save.sh` installs the built mod into a battery save's own config dir and
starts the game there. `tools/run-save.sh` does the save half; this adds the mod
and `mods.json`. The mod has no options, so nothing is written to the config dir.

```sh
make example-mods
# --reset re-imports the save and discards the progress the game wrote back
mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin --reset
```

`--reset` matters: the port writes the battery while playing and on exit, so a
second run without it resumes from wherever the first stopped. With the pickup
save used to develop this, the pristine image resumes in the mission (scene
`0x03`, record mask `0x38C`, so record 7 is resident and the drop table is
present) and a played-on image comes up on the world map (scene `0x05`, no
record 7, nothing to pick up).

The load route is title → `START` → `LOAD GAME` (scene `0x12`), then a long press
of `A`. A short synthetic tap does nothing on that screen; the live console
command is `press a 400` (`OGRE_LIVE_CONSOLE=1`, written to the watched command
file atomically: `printf 'press a 400\n' > /tmp/c.tmp && mv /tmp/c.tmp
/tmp/ogre-console.txt`).

The per-map ground-item list gives a deterministic test of the fallback path:
with the pristine pickup save, `rb 0x801F1002 1` reads `07` (seven items), `rh
0x801F1004 4` reads `80CD 809C 8002 0007`, and `r 0x801F0E24 1` reads `0`, so the
next pickup is item `0x80CD` = 205 "Old Clothing".

## Reading the code

* `src/item_randomizer.c` — the frame hook, the drop-table rewriter, the list
  diff and count diff, and the shop guard, with the addresses read at
  instruction level.
* `include/modding.h` — the section macros (`RECOMP_HOOK`, `RECOMP_IMPORT`, …).
* `mod.toml` — the manifest.
* `mod.ld` — the link script.

The research behind the addresses is in `docs/notes-save-items.md` (the item
list and its save encoding) and `docs/notes-item-re.md` (the grant routine, the
drop and pickup call sites, and the message path).
