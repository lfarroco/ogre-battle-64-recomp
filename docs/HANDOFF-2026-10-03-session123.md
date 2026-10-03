# Handoff — 2026-10-03, session 123: the Item Randomizer's option loading, item counts and consumable table

## Goal

Continue session 122's Item Randomizer work: the mod at `mods/item-randomizer/`,
its five open items, and the developer's new battery save
`build-app/saves/before-pickup.bin` as a repeatable test.

## Result

Four defects fixed and verified from the mod's own log and the live console, one
open work item closed by research, and one route built for the developer to test
by playing. Nothing was committed.

| # | what | state |
|---|---|---|
| 1 | A stored mod option was ignored unless the file carried `mod_id` | fixed, A/B verified |
| 2 | A save load with few records was read as a burst of grants and rerolled | fixed, verified (the first fix, keyed on the scene word, was wrong and is withdrawn below) |
| 3 | The consumable definition table was wrong by four bytes, so no consumable was ever rewritten | fixed, verified |
| 4 | The session memo could put one item id into two records | fixed, verified |
| 5 | The `1/1` item that cannot be equipped | explained at instruction level; it is the game's own stale used count, not the mod |
| 8 | A battle reward was always the vanilla item | fixed: the pending reward queue at `0x801936D8` is rewritten; verified on two rewards, message and inventory both the replacement |
| 6 | The two mechanisms fought: a table-written replacement was flipped back by the list watch | fixed, verified |
| 7 | The per-map ground-item list was not rewritten, so a pickup's popup named the original | fixed, verified by the developer playing the pickup |

## 1. A stored mod option was ignored (session 122's item 4)

`parse_mod_config_storage` (`tools/N64ModernRuntime/librecomp/src/mod_manifest.cpp:815`)
rejected a mod config file with no `mod_id` key, and `Config::load_config` then
returns false, which leaves every option on its default with no error and no log.

A/B on one file, one variable (`.ogre-prefs-save-before-pickup/mod_config/`), the
mod's unconditional first two lines:

```
unpatched: [mod] start verb mode shops 0 0 0 0     [mod] start chance seed 100 191837307
patched:   [mod] start verb mode shops 2 1 1 0     [mod] start chance seed 55 12345
```

The file was stored with `log VERBOSE, mode "SAME KIND", chance 55, shops true,
seed 12345`. A `mod_id` that names a different mod is still rejected. The two
stores of the id (`Config::get_json_config` at `config.cpp:345` and
`save_mod_config_storage` at `mods.cpp:700`) both write it, so session 122's
"the runtime's own writer never emits one" is not what the current tree does; the
file on disk lacked the key and the loader was the part that had to tolerate it.

`tools/patchcheck.py` then failed on `mod_config_api.cpp` as well: session 122's
`recomp_log` export was in the tree and in no patch, so no release carried it and
every mod that imports `recomp_log` was refused by a shipped build.
`patchcheck.py --fix` was run; `patchcheck.py` now passes.

## 2. A save load was read as a burst of grants

The list watch treats a record that was empty and is now filled as an
acquisition. Loading a save fills the whole list at once, so the guard was a
count: more than `RELOAD_SLOTS` (12) records in one frame re-primes instead of
rerolling. The 40-slot consumable list holds four records in the pickup save, so
loading it was four acquisitions:

```
[mod] frame acq countup reroll 4176 4 0 0      # before the fix, at the save load
```

The four were not rerolled only because the consumable id check was also broken
(§3). With §3 fixed they would have been.

The first signal tried was the game's own used-count rebuild `func_8016B774`,
which the save loader calls at `0x8016CF30`. **A mod hook on it crashes the save
load**: `func_800749C0` calls it and the regenerated body fails `get_function`
(`#2 func_800749C0 + 781`, `#0 get_function + 826`). That is the same failure
class `mods/exp-overflow` records for `func_8016EBC4` and `func_80072398`. The
hook was removed.

The second signal tried was the scene word `0x800E810E`, which is a *wrong* fix
and is withdrawn. It re-primes on any scene change, and the game grants items on
scene changes: entering a mission runs the unit-init grants, and a battle's
reward arrives as the battle scene ends. Every one of those grants was swallowed
by the re-prime, so the player saw the original item. That is what the developer
reported twice — two items appearing at mission start and a battle reward that
was "the expected item from the loot table, not a random one" — and the log of a
mission entry with this rule shows no `acquire` line for a grant that the same
run makes visible with the rule removed.

**The signal that works is a hook on the save-field reader `func_800749C0`.** It
is `.main` code (the crash above is its *callee*), it walks the table at
`0x800A824C`, and entry 1 is the packed blob that carries both item lists. It
regenerates cleanly, which was not knowable without testing it. Verified on the
pickup save:

```
[mod] save-read
[mod] re-prime 21 3 0 1     # 21 equipment records, reload #3, list 0, forced by the hook
[mod] re-prime 4 4 1 1      # 4 consumable records, reload #4, list 1, forced
[mod] acquire 38 21 0 1     # a later grant, seen because nothing primes it away
[mod] table-grant 38 21 0 1
```

The record count stays as a fallback, because it catches a load the hook misses
(it did fire for both boot reads here): more than `RELOAD_SLOTS` (12) equipment
records in one frame re-primes both lists.

The list walk also has its own log budget now. On the frame the mission scene
loaded, the drop-table and pickup-table rewrites produced 88 events and pushed
the re-prime and acquisition lines past the 20-line cap, which is why the mission
entry looked silent in the earlier logs.

## 3. The consumable definition table was wrong

`func_8016F500` (`0x8016F500`) is `andi a0,0xffff; sll v0,a0,1; addu v0,v0,a0;
sll v0,v0,2` (id × `0x0C`) then `lw v0,-6420(at)` with `at = 0x80190000`, that is
`lw(0x8018E6EC + id*0x0C)`. The name pointer is the entry's `+0x00`. The mod used
base `0x8018E6F0` with the name at `+0x04`, which reads the **next** entry's
`+0x08` field, so every consumable failed the name check and none was rewritten.
Reading RDRAM (`/tmp/pre-pickup.bin`, the live dump) shows
`0x8018E6EC -> 0x801906DC "None"`, `0x8018E6F8 -> 0x801906D0 "Heal Leaf"`,
`0x8018E704 -> "Heal Seed"`, `0x8018E710 -> "Heal Pack"`.

Verified after the fix, live console writes into an empty consumable slot:

```
[mod] acquire-consumable 1 20 0 1
[mod] replace-consumable 1 19 20 1
```

**The same table holds the key items.** Entries 24..40 are "Medal of Vigor"
through "Pedra of Flame", including "Package for Gelda" and "Letter from Gelda".
Randomizing one would break a chapter, so the mod now keeps them: the entry's
byte at `+0x04` is `0x00`-`0x02` for the usable items and `0x03`/`0x04` for the
key items. Verified:

```
[mod] acquire-consumable 37 21 0 1
[mod] keep-quest 37 21 4 1
```

The byte at `+0x04` is the guest byte at that address, which is the **high** byte
of the word there: `lw(0x8018E6FC)` is `0x0000000A` for Heal Leaf, and the port's
`rd8(0x8018E6FC)` is `0x00`. The first attempt used `+0x07`, which is the word's
low byte, and read `10` for Heal Leaf. The console `rb` and the mod's `rd8` agree
byte for byte, which is how the offset was pinned (`rb 0x8018E6F0 1` reads `FF`
for entry 0's `0xFFFF0000`).

## 4. One item id in two records

The session memo maps `X` to `Y` and `Y` to `X`. The game grants an item by
searching the list for its id and merging, and the list holds no `X` after a
rewrite, so a **second** grant of `X` mapped to `Y` again and a second record
received an id the party already owned. Both `func_8016B6FC` and the used-count
rebuild stop at the first matching record, so the used count lands on the first
record and the availability test (`sltu(+0x02, +0x03)`) reads the wrong one.

Verified, live console writes of the same original id into two slots:

```
[mod] acquire 100 1 0 1
[mod] replace 100 95 1 0
[mod] acquire 100 0 0 1
[mod] dup-fix 100 95 138 0
[mod] replace 100 138 0 0
```

## 5. The `1/1` that cannot be equipped

`docs/notes-item-equipped.md` has the mechanism at instruction level. The item
screen draws the record's `+0x02` (a count of roster references) then `+0x03`
(the owned count), and the equip test is `sltu(+0x02, +0x03)`. The grant routine
writes the id and `+0x03` only (`0x801DC7FC`, `0x801DC808`) and never `+0x02`, so
it inherits whatever the record carried. A record can be empty with `+0x02 != 0`:
`func_8016BA6C` decrements `+0x02` and clears the id when `+0x03` reaches 0, and
`+0x02 > +0x03` is reachable because `func_ovlAF_80218E68` adds a user for class
equipment with no ownership test. The next grant into that record reads `1/1`
under the new item's name and refuses every equip. Nothing rebuilds the column
while the item screen is open (bank S and bank AB contain no call to
`func_8016B774`), so the value stays on screen.

The mod does not cause this and cannot see it. It rewrites only the id, and the
acquisition line now reports the byte: `acquire <id> <slot> <used +0x02>
<count +0x03>`. A run that reports `1/1` will show a non-zero third field on the
record it rewrote, which separates the stale-record case (mechanism A above) from
the duplicate-id case (§4, now fixed).

A fix from inside the mod needs the record's reference count for the replacement
id, which is the rebuild's whole roster scan: 99 character records' four stored
equipment ids and four class-derived ids plus ten unit ids. A blind clear of
`+0x02` is wrong for the ids class equipment references, which is a large
fraction of the table, so the mod reports the byte instead of writing it.

## The developer's save

`build-app/saves/before-pickup.bin` is a 32 KiB SRAM image, two slots plus the map
record, both checksums valid. `mods/item-randomizer/run-with-save.sh` installs the
built mod into that save's own config dir and starts the game there.

`--reset` matters. The port writes the battery while playing and on exit, so the
sandbox save diverged from the developer's file after the first run, and a
played-on image comes up on the world map (scene `0x05`, record mask `0x2`, no
record 7) while the pristine image resumes in the mission (scene `0x03`, record
mask `0x38C`, record 7 resident). `tools/run-save.sh --reset` restores it.

The load route is title → `START` → `LOAD GAME` (scene `0x12`) then a long `A`.
A short synthetic tap does not load; `press a 400` from the live console does.
A run is only usable once the log shows both the `[scene]` line for the scene the
check needs and `probe first last present 39 80 1` for anything about the tables.
Wait for both; a run that never reached `0x12` looks identical to a silent mod.

A scripted run and the developer's own window are the same binary, so a kill by
name ends their session. Start a run, record its PID, and kill only that PID.

State of the pristine save at the mission scene, from the live console:

```
[mod] probe first last present 39 80 1 0      # 0x27, 0x50, record 7 resident
rb 0x801F1002 = 07                            # seven per-map ground items
rh 0x801F1004 = 80CD 809C 8002 0007           # 205, 156, 2 equipment; 7 consumable
r 0x801F0E24 = 0                              # the next pickup is index 0
r 0x80196A8C = 0x3E8                          # gold 1000
```

The developer's own later save (`build-app/saves/ogrebattle64-us-rev1.bin`, a new
game with a suspend save) loads the same way, which is the path they asked about:
a suspend load resumes inside the mission rather than on the overworld, and it
does call the save-field reader. Verified:

```
[mod] save-read
[scene] id=0x0003 descriptor=0x8018F350 mask=0x0000038C
[mod] re-prime 22 3 0 1     # 22 equipment records, list 0, forced by the hook
[mod] re-prime 4 4 1 1      # 4 consumable records, list 1, forced
```

No `replace` or `acquire` line at the load, so the inventory was not rerolled.

So the next pickup is item `0x80CD` = 205 "Old Clothing", and the record-7
fingerprint reads `39 80 1`, so both source tables are in reach.

## 6. The two mechanisms fought over one id

The session memo is an involution: `X` becomes `Y` and `Y` becomes `X`. A source
table rewrite writes `Y`, the game grants `Y`, and the list watch then mapped `Y`
back to `X`. Verified before the guard existed, with `drop-table 49 140` in the
log:

```
[mod] acquire 140 30 0 1
[mod] replace 140 49 30 0
r 0x80196B98 = 00310001        # slot 30 holds 49, the original
```

The mod now records every id it writes into a source table
(`s_table_written[list][id]`) and the list watch leaves those alone with a
`table-grant` line.

## 7. The per-map ground-item list is rewritten

`func_ovlR_80215C38` (bankR record 9d) parses the map's item buffer at load: a
`u8` count to `0x801F1002`, then `u16` ids to `0x801F1004 + i*2`.
`func_ovlN_801B9694` (bankN record 7) serves a pickup with
`idx = lw(0x801F0E24)`, reads `lhu(0x801F1004 + idx*2)`, writes it to the popup
field `0x801F36C2` (`0x801BB8FC`) and passes the same value to the grant at
`0x801BB930`. One value feeds both, so rewriting this array makes the popup and
the inventory agree. Session 122's corrupting attempt wrote it as 32-bit words
(`0x801F1004[index]`, which is `i*4`), one id two bytes from the right one; the
array is `u16` at `i*2`. The write is gated on the same record-7 fingerprint as
the drop table and re-derived through the memo when the map loader restores it.

Verified on the developer's own save, in the run whose log carries both halves:

```
[mod] pickup-table 205 10 0 0        # entry 0: Old Clothing -> Glamdring
[mod] pickup-table 156 24 1 0
[mod] pickup-table 2 81 2 0
[mod] pickup-table 7 20 3 0
...
[mod] acquire 10 21 0 1              # the grant puts Glamdring in slot 21
[mod] table-grant 10 21 0 1          # the mod's own table value, left alone
[mod] frame acq countup reroll 8975 1 0 0
```

The developer's report of that pickup: "picked glamdring, and it was added to the
inventory". Item id 10 resolves to "Glamdring" and 205 to "Old Clothing" in the
item struct table at `0x8018C42C`.

The guard was also exercised the other way: after the pickup, the game moved to
scene `0x06`, the probe read `36932 10338 0 0` (record 7 gone) and the mod stopped
writing; `rh 0x801F1004 8` then read the other bank's code.

## 8. "One find produced four items" is not what happens

The developer picked up the same item twice and each time saw four or five names
in the inventory: notos, nephrite sword, yggdrasil, ice chain, plus bloody emblem.
The pickup grants exactly one item. Evidence, all from the pristine save and one
logged run:

* The pristine save's slots 17..20 already hold 270 Bloody Emblem, 68 Yggdrasil,
  163 Ice Chain and 14 Nephrite Sword, decoded from the live RDRAM image. Those
  four names are four of the five the developer listed.
* The same save with the same pickup in a logged run produced
  `frame acq countup reroll 3168 1 0 1`, one acquisition and one reroll.
* The five names the developer saw are exactly the four above plus the one the
  mod added (7 Notos in that run, 10 Glamdring in the next).

So the item screen was showing the items the party already owned. The earlier
"four grants in one frame" in session 122's log was the save-load misread of the
40-slot list, which §2 fixes.

## 9. The battle reward: the queue at 0x801936D8

The developer reported twice that a battle gave "the expected item from the loot
table from that encounter, not a random one". The first cause was the withdrawn
scene-change re-prime (§2). With that gone the reward was still vanilla, and the
log from their run says why:

```
[mod] acquire 157 22 0 1
[mod] table-grant 157 22 0 1        # a value the mod's drop table produces
[mod] count-up 155 16 7 8           # the reward: Leather Armor, stack 7 -> 8
```

The reward was id 155, which the party already owned seven of, so the grant merged
into that stack; the list watch only rewrites a slot the game newly filled, so it
left it. The reward id does **not** come from the drop table at that moment. With
the drop table's entry 10 column C rewritten 155 -> 17, the pending value was
still 155:

```
rh 0x801936D8 = 8030      # 48, the mod's value for entry 7 column A
rh 0x801936DA = 0000
rh 0x801936DC = 809B      # 155, the drop table's *vanilla* value for entry 10 C
rh 0x801936DE = 809D      # 157, the mod's value for entry 10 A
rh 0x801936E0 = 805E      # 94, the mod's value for entry 2 B
```

`0x801936D8` is the first of 20 `u16` the mission bank serves a reward from:
`func_ovlN_801E49D0` takes `lhu(0x801936D8)` at `0x801E5230` (fallback
`lhu(0x801936DA)` at `0x801E5254`), stores it to `0x8021C760`, grants it at
`0x801E53DC` and clears `0x801936D8` at `0x801E53F4`. The reward message is built
from the same stored word, and the queue is a save field (20 `u16` copied by
`func_80185128`), so the mod now rewrites it: each non-zero word keeps bit 15 and
gets the replacement for its id. That covers a reward the party already owns,
which the list watch never could.

Verified with the developer's save and the seed their run logged
(`seed 1410593729`), which reproduces their drop-table rewrites exactly:

```
[mod] reward-queue 155 17 2 0
[mod] reward-queue 94 235 4 0
rh 0x801936D8 = 8030   # left alone: this session's own replacement for 21
rh 0x801936DC = 8011   # 17, 155's replacement
rh 0x801936DE = 809D   # left alone: this session's own replacement for 38
rh 0x801936E0 = 80EB   # 235, 94's replacement
```

The mod flags every id it has produced (`s_memo_target`, set where the memo
records a pairing), and the table walks, the queue walk and the list watch all
leave such a value alone. That replaced the session-local "written into a table"
flag, which could not see a value an earlier run had left in the *saved* queue.
The list watch's line for that case is `already-replaced`.

The developer then reported "we got 'oracion' from a battle, a random item", and
then two rewards read back name for name. Their log for the second run:

```
[mod] reward-queue 48 164 0 0      # 48 Francisca   -> 164 Saint's Garb
[mod] reward-queue 155 193 2 0     # 155 Leather Armor -> 193 Magician's Robe
[mod] reward-queue 157 52 3 0      # 157 Ninja's Garb -> 52 Euros
[mod] reward-queue 94 3 4 0        # 94 Iron Claw   -> 3 Falchion
[mod] acquire 3 22 0 1             # reward 1: Falchion, a new record
[mod] already-replaced 3 22 0 1    # the mod's own value, left alone
[mod] count-up 193 4 1 2           # reward 2: Magician's Robe, stack 1 -> 2
```

The developer's report: "1st loot -> falchion, got falchion" and "2nd loot ->
magicians robe, got magicians robe". Resolved against the item struct table,
id 3 is "Falchion", 193 "Magician's Robe", 94 "Iron Claw" and 155 "Leather
Armor". So the reward id was the mod's replacement and not the vanilla value in
both cases, and the reward *message* read the same word. The second reward merged
into a stack the party already owned, which is the case the list watch alone
could never reach.

## Files changed

| path | what |
|---|---|
| `mods/item-randomizer/src/item_randomizer.c` | consumable table base and name offset; the key-item class guard; the scene-change re-prime; the per-map ground-item rewriter; the `table-grant` guard; `replacement_for_slot` and the `dup-fix` path; the count shadow and `count-up`; the `+0x02` field on the acquisition line |
| `mods/item-randomizer/run-with-save.sh` | new: run a battery save with the mod installed |
| `mods/item-randomizer/README.md` | corrected the consumable table, the new log lines, a testing section |
| `mods/item-randomizer/mod.toml` | description: the message agrees only where the mod rewrites a table |
| `patches/n64modernruntime-ob64.patch` | regenerated: the loader fix and session 122's `recomp_log` export, which was in no patch |
| `docs/notes-item-equipped.md` | new: the `x/y` pair, `+0x02`, the equip test (research, no code) |
| `docs/notes-bank-hooks.md` | new: what a hook on streamed bank code would take (research, no code) |

Vendored trees are gitignored and carried by `patches/`:
`tools/N64ModernRuntime/librecomp/src/mod_manifest.cpp`.

## Verification runs

All app runs used `OGRE_MOD_TEST=1` and `build-app/ogrebattle64` (rebuilt with the
loader fix). The mod's log lines are the evidence; the console writes are the
inputs. The count probes were driven with `OGRE_LIVE_CONSOLE=1` and a watched
command file written atomically.

End-to-end, on the developer's own save and the developer's own play:

```
[mod] pickup-table 205 10 0 0
[mod] acquire 10 21 0 1
[mod] table-grant 10 21 0 1
[mod] frame acq countup reroll 8975 1 0 0
```

The developer: "picked glamdring, and it was added to the inventory".

## Open work, in priority order

1. **The battle reward is closed.** The queue rewrite replaced the vanilla drop
   with the mod's value and the message named the same item on two rewards, one
   of which merged into an existing stack (§9). Nothing is open on this path.
2. **The `+0x02` fix**, if the developer wants it: reimplement the rebuild's roster
   scan in the mod, or add a base-function call API to the mod system. A mod
   cannot call game code today: `jal` fails `relocation truncated to fit:
   R_MIPS_26` from `0x81000000` to `0x80xxxxxx`, and the base game registers no
   function exports for `RECOMP_IMPORT` to bind.
3. **Bank-function hooks** (`docs/notes-bank-hooks.md`): a merged `mod-syms`
   dump, a hook-section registry in `librecomp` kept out of `sections_info`, and
   `rom_size` on `BankFunctionEntry`. `librecomp`'s own section map must not gain
   bank sections: `notify_rom_read` would call `load_overlay` for them and
   `section_addresses` is `calloc(23)` while bank C's indices reach 24.
4. **`func_8016B774` cannot be hooked.** Recorded so the next session does not
   retry it: the save load calls it and the regenerated body fails `get_function`.
   Any future re-prime signal should avoid a new hook in the save path.

## Lessons

1. A hook target can pass the recompile step and still break at run time. Test a
   new hook at the moment it fires; the `func_8016B774` hook only failed when the
   save load called it.
2. A data-derived rule needs its byte order fixed by reading the same byte two
   ways. The class guard used the word's low byte at `+0x07` and read 10 for Heal
   Leaf; the console `rb` at `+0x04` and the mod's `rd8` then agreed.
3. A console write to guest RAM is a raw host word, and the guest is byte-reversed
   inside it: writing item id 100 and count 1 to `0x80196B20` is
   `w 0x80196B20 0x00640001`, and `rh 0x80196B20 1` reads it back as 100.
4. Two writes to the same system need one decision procedure. The table rewrite
   and the list watch both decide an item's id, and neither knew about the other,
   so one undid the other. The `table-grant` guard is that decision made explicit.
5. A player's report of "several items from one pickup" needs the save decoded
   before it is believed. The four extra names were in the save's own list the
   whole time.
6. The battery save is written back by the port, so a test sandbox is not the
   file that was imported after one run. `tools/run-save.sh --reset` is what makes
   a route repeatable, and the difference was scene `0x03` against scene `0x05`.
7. `pkill -f build-app/ogrebattle64` kills the developer's window, not just the
   agent's run, and it cost a battle's loot. Record the PID of the run that was
   started and kill only that, and check `ps` for an instance before starting one.
8. A conclusion needs the run's state asserted first. Several runs here never
   reached the load screen and their logs looked "silent"; every claim above is
   tied to a `[scene]` line plus `probe first last present 39 80 1` (record 7
   resident), and the recipe now waits for both before reading anything.
