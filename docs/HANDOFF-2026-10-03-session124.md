# Handoff — 2026-10-03, session 124: the Item Randomizer's `+0x02` used count

## Goal

Continue sessions 122 and 123's Item Randomizer work. The developer selected the open
item from session 123's list: *"The `+0x02` fix: reimplement `func_8016B774`'s
roster scan in the mod"*, because the `1/1` item that cannot be equipped is the
one player-visible defect left in the mod.

## Result

`func_8016B774`'s roster scan is reimplemented in
`mods/item-randomizer/src/item_randomizer.c`, and the `+0x02` byte of a record
the game newly fills is corrected with the value that rebuild would have written.

The reconstruction is proven against the game's own byte on three live dumps
with **0 mismatches**. The write itself is **not** exercised by any run, because
no acquisition in nine runs inherited a non-zero `+0x02`. That is the one open
line, and it is stated as open in `PLAN.md`.

| # | what | state |
|---|---|---|
| 1 | The roster scan (`func_8016B774` reimplemented) | implemented, 0 mismatches on three dumps |
| 2 | The `+0x02` correction for a newly filled record | implemented, never triggered in game |
| 3 | `tools/refscan.py` + `make refscan` | new, reproduces the game's column |
| 4 | The run-time sweep that reported `mismatch 15` | **removed**; its gate could not tell "loaded" from "half-loaded" |
| 5 | The `spoilslot` self-test that forced the stale byte | **removed**; `drop_table_present()` is false after the mod rewrites its own fingerprint |

## 1. The scan, from the disassembly

`func_8016B774` (`0x8016B774`, `.streamedB`, size `0x29C`) clears `+0x02` for
both owned-item lists and then adds one per roster reference. The mod cannot
call it: `jal` from the mod's own `0x81000000` to game code fails
`relocation truncated to fit: R_MIPS_26`, and a session-123 hook on it crashes
the save load (`func_800749C0` calls it; the regenerated body fails
`get_function`). The reimplementation is therefore the only route.

Instruction-level source, with the register-relative stores resolved:

| source | loop | gate | ids | list |
|---|---|---|---|---|
| units | `0x8016B7CC`-`0x8016B870` | `0x8016B7DC` `lbu v0,1(a3)`; `andi v0,v0,1` | `0x8016B7F4` `lbu a2,13(v0)`, 10 bytes | 40-slot, `slti v1,40` |
| characters | `0x8016B874`-`0x8016B9E8` | `0x8016B890` `lbu v0,17(s1)` | `0x8016B8DC/8E8/8F4/900` `lhu a2,42/44/46/48(s1)` | 278-slot, `slti v1,278` |
| class | `0x8016B958`-`0x8016B9CC` | `0x8016B964` `lw v0,28660(at)`, `at=0x80180000+i*4` -> table `0x80186FF4`; `jalr` with `a0 = lbu 17(s1)`, `a1 = lbu 18(s1)` | the four accessors return `lhu(0x80187C62 + idx*0x48 + 0x62 + i*2)` when `lbu(+0x79) == a0`, and the `a0` entry otherwise | 278-slot |

Structures: units `0x80197210` stride `0x19`, 30 records; characters
`0x80193BE0` stride `0x38`, 100 records; class table `0x80187C62` stride `0x48`.
The character loop starts at index 1 (`li s2,1`), which the mod reproduces by
skipping record 0 on its zero gate.

The halfword at `+0x2A` and the byte at `+0x2B` carry the same value, because
each id's high byte is 0. Both readings reproduce the game's column, which
`tools/refscan.py --item` confirms by naming the contributing records.

## 2. The correction

`fix_record_use` is called after each list walk, for every record the game newly
filled (the same `acquired` test that gates the list watch). It writes
`ref_count(list, slot)` into `+0x02`, and only while the record still holds the
id the mod wrote, so a record the game has since changed is left alone.

The value is the replacement item's own roster count, so it is correct in the
case that forbids a blind clear: an item a unit genuinely references. Nothing is
written while the roster is not in RAM, because a scan of an unloaded roster
counts 0 for everything; that case is counted as `s_uses_skipped`.

## 3. `tools/refscan.py`

`make refscan DUMP=<OGRE_DUMP_RDRAM image>` rebuilds the column from an image
and diffs it against the byte the game has. `mismatches 0` is the pass.

```
$ make refscan DUMP=/tmp/ir-final.bin
characters loaded 20, units loaded 6
mismatches 0
```

Three of the developer's dumps give 0: `/tmp/pre-pickup.bin` (21 occupied
equipment records), `/tmp/ir-chars.bin` (22) and `/tmp/ir-final.bin` (25), plus
4/4/6 consumable records.

`--item <id>` names the records that credit an item, which is how a count is
decomposed. In `/tmp/ir-final.bin`, item 117's 2 is character records 6 and 17,
both class byte 17, each crediting 117 from its class row:

```
$ python3 tools/refscan.py /tmp/ir-final.bin --item 117
list0 item 117 (Scipplay Staff): 2 credit(s)
   char6 class0
   char17 class0
```

The tool must use `Dump.half` from `tools/rdram.py`. A local reimplementation
that applied only the byte-reversal and not the `^2` read **516** for 117 -- the
same defect session 58 fixed in the in-app console.

## 4. The run-time sweep, and why it was removed

Four attempts were made to validate the scan inside the running game with a
sweep that compares both columns once per session. Every one fired while the
character records held their gate byte at `+0x11` but not their ids, so the scan
correctly returned 0 for every item and the sweep reported **15 mismatches out of
25**. The fourth attempt printed the evidence:

```
[mod] roster sweep chars units gate id0 20 6 1 0
```

20 gate bytes set, `id0` (record 0's `+0x2A`) still 0. The gates and the id
fields are written at different times, so none of `equipment_occupied()`,
`roster_chars()`, `drop_table_present()`, a frame counter, or `+0x2A` on record 0
distinguishes "loaded" from "half-loaded" in this save. A diagnostic that cannot
make that distinction is worse than none, so the sweep was deleted. The
per-acquisition line `acquire ref game <id> <scan> <game> <chars>` remains and
needs no gate, because it is computed at the moment of a grant.

## 5. The self-test, and why it was removed

`fix_record_use` only writes when the game's byte differs from the rebuild, and
nine runs produced no acquisition that inherited a non-zero `+0x02`. A
`spoilslot` option (an id the manifest does not define, so it reads 0 in a normal
run) wrote `+0x02 = 3` into the first empty equipment record, which
`func_ovlN_801DC740` claims next, to force that case. It never fired:
`drop_table_present()` tests the drop table's own fingerprint words, and the mod
rewrites exactly those words, so the gate is false in the window the test needs.
It was removed rather than given a sixth gate.

## What was run for verification

Every app run used `OGRE_MOD_TEST=1` through
`mods/item-randomizer/run-with-save.sh` and `build-app/saves/before-pickup.bin`
with `--reset`.

| run | log | what it gave |
|---|---|---|
| 1 | `/tmp/ir-use.log` | 8 acquisitions, all `+0x02 = 0`; `pickup-table 205 13` -> `acquire 13 21 0 1` |
| 2 | `/tmp/ir-fix.log` | `roster check ... 25 15`, the first half-loaded sweep |
| 3 | `/tmp/ir-check.log`, `/tmp/ir-fix.log` | dumps `occupied 0` at frame 600, before the save is read |
| 4 | `/tmp/ir-diag.log` | no sweep in a 35 s boot |
| 5 | `/tmp/ir-final.bin` + `.log` | `acquire ref game <id> 0 0 20` x4; the dump that gives 0 mismatches |
| 6 | `/tmp/ir-fix.log` (17:14) | acquisitions only; no `spoil` line |
| 7 | `/tmp/ir-fix.log` (17:21) | `pickup-table 205 44 0 0` -> `acquire ref game 44 0 0 20`; the sweep removal is safe |

`/tmp/ir-final.bin` and `/tmp/ir-check.bin` are the offline evidence. The
mod's `[mod]` lines and the developer's own reports are the in-game evidence:
"got a phoenix robe this time, no 1/1 issue", "got 'peridot sword', no 1/1
issue", "found some items on the floor and got some loot from fights, seems to
be working fine".

## Files changed

| path | what |
|---|---|
| `mods/item-randomizer/src/item_randomizer.c` | the roster scan, the `+0x02` correction, the `acquire ref game` line, `roster_populated`, and the removal of the sweep and the self-test |
| `mods/item-randomizer/README.md` | the `+0x02` mechanism, the new log lines, and the verification recipe |
| `tools/refscan.py` | new: the offline check of the scan against the game's own byte |
| `Makefile` | `make refscan DUMP=<image>` |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md` | the session record |

## Open work

1. **The correction write is not exercised.** No run has produced a record that
   was empty with a non-zero `+0x02` at the moment the game filled it, so
   `fix_record_use` has never written in game. What is proven is the value it
   writes. Evidence for it needs an acquisition that inherits a stale byte --
   the developer's own equip-then-unequip sequence did not produce one in this
   save -- or a self-test that does not depend on `drop_table_present()`.
2. **`docs/notes-item-equipped.md` §4 mechanism A is now handled by the mod**;
   mechanism B (a duplicate id) is handled by `replacement_for_slot`'s
   `dup-fix`; mechanism D (a unit's equipment pointing at an id the list no
   longer holds, which writes one byte of a unit record through the 511
   sentinel) is untouched.
3. **Bank-function hooks** would let the grant routine be hooked directly, which
   would remove the need to watch the lists at all. Scope in
   `docs/notes-bank-hooks.md`.

## Lessons

1. **A gate on "the data is loaded" needs the field being read, not a proxy.**
   Five gates were tried for this sweep and every one was a proxy: an occupied
   equipment list, a frame number, a character gate count, the record-7
   fingerprint, and record 0's first id. The equipment list seats ~400 log lines
   before the character ids do. The sweep should have asserted the value it was
   comparing, not a load signal.
2. **A diagnostic that reports a false failure is worse than no diagnostic.** The
   `mismatch 15` line cost five developer runs and read as a scan defect. The
   per-acquisition `acquire ref game` line replaced it because it is computed
   where the answer is needed and needs no gate.
3. **A test option must not be gated on state the mod itself rewrites.**
   `drop_table_present()` tests the drop table's fingerprint words and the mod
   rewrites those words, so the self-test's gate was false in the only window it
   could fire.
4. **Read the dump's byte order from the tool that owns it.** A hand-written
   halfword read returned 516 for 117; `Dump.half` applies the XOR-2 that the
   `docs/guides/app-build.md` rule describes.
5. **An offline check on a captured image can settle a question that run-time
   gating cannot.** The image is a fixed input, so the comparison has no timing
   component at all.
