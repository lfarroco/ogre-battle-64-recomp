# Handoff — 2026-10-03, session 122: the Item Randomizer mod, and what it cannot reach

## Goal

Developer request: *"let's create a crazy mod for this game: an item randomizer"*,
scoped by the developer to **items dropped after defeating an enemy unit** and
**items found on the map**. Shops, cutscene gifts and quest items stay vanilla.
The developer chose **"anything in the item table"** as the replacement flavour.

Session 121 ended with the modding framework usable and two example mods
(`mods/exp-overflow`, `mods/skip-boot-logos`); this session built the first mod
that touches game data a player cares about.

## Result

**A working mod at `mods/item-randomizer/`, with one verified behaviour and two
open defects.** The evidence for every claim below is a `[mod]` line the mod
itself wrote during a developer-driven run, not inference from disassembly.

Working: a map pickup is rerolled — the item the game grants is replaced by a
different item from the same list, once per acquisition, with no duplicate
added. Developer-confirmed three times ("Flag of Unit", "Decoy Cap", and a clean
pickup after an earlier corruption was removed).

Open: (1) whether the game or the mod is responsible for a find that adds more
than one item; (2) an item reported as `1/1` ("I have one, and one is already
equipped") that can no longer be equipped.

## What was built

`mods/item-randomizer/` — source, manifest with four options, README, link
script, `modding.h`. It runs from one base-section frame hook,
`RECOMP_HOOK("func_80072944")`, and does three things:

1. **Rewrites the drop table** (`0x801EDB38`, 35 entries × 8 B, entry
   `{u16 type, u16 item_a, u16 item_b, u16 item_c}`), gated on a record-7
   fingerprint. The game reads one of the three columns with `rand() % 3`
   (bankN `0x801ADD68`, read `0x801ADDB0`) and uses that one value for the popup
   id and for the grant, so a source rewrite is the only way the message and the
   inventory can agree.
2. **Watches both owned-item lists** and replaces the id in a slot that was
   empty and has just been filled. This is the mechanism that actually
   randomizes a pickup today.
3. **Logs** through a new port export, `recomp_log`, because a mod cannot print
   (it is compiled `-nostdinc -fno-builtin` and the base game exports no
   `printf`).

### The two owned-item lists

The grant routine `func_ovlN_801DC740` (bank unit N record 7, `0x801DC740`)
chooses a list with **bit 15 of the item word**, and the two lists have separate
id spaces and separate definition tables:

| bit 15 | list | slots | stride | definition table | id space |
|---|---|---|---|---|---|
| set | `0x80196B20` | 278 | 4 | `0x8018C42C`, stride `0x20`, name ptr `+0x00`, category `+0x04` | equipment, 1..277, 278 = "None" |
| clear | `0x80193AE0` | 40 | 4 | `0x8018E6F0`, stride `0x0C`, name ptr `+0x04`, id in the low byte at `+0x00` | consumables, id 1 = **"Heal Leaf"** |

Record layout for both: `{u16 id +0x00, u8 in-use/cache +0x02, u8 count +0x03}`.

**"Heal Leaf" cannot be produced from the equipment table at all.** Watching only
`0x80196B20` is why the tutorial's consumable grants were invisible for most of
this session. The mod now walks both lists, validates each id against that
list's own table, and keeps a **memo per list** (a shared memo would map
equipment id 1 and "Heal Leaf" onto each other).

## Verification

Developer-driven runs, `OGRE_MOD_TEST=1 OGRE_ROM=... ./build-app/ogrebattle64`,
log to `/tmp/ir.log`. The mod's own lines:

```
[mod] start verb mode shops 2 0 0 0        # options reached the mod
[mod] start chance seed 100 191837307 0 0
[mod] start droptable first last 0 0 0 0
[mod] probe first last present 39 80 1 0   # record 7 resident, table found
[mod] acquire 205 21 0 1                   # Old Clothing filled empty slot 21
[mod] replace 205 43 21 0                  # rerolled to 43, same slot
[mod] drop-table 190 185 10 0              # table entry rewritten
[mod] drop-table absent 36932 10338 0 0    # another bank owns that window
```

A full session: 9 `acquire`, 5 `replace`, 62 `drop-table`. Every `replace` is a
distinct slot going empty → one item. **No `acquire` was ever followed by two
`replace` lines for the same slot**, so the mod never adds a second item.

The corrupting write is gone: an earlier build wrote `0x801F1004[index]` for the
per-map pickup list, which is **off by two bytes** — the map loader
(`func_ovlR_80215C38`, bankR, `0x80215d10` `addiu v1,v1,4098` then `a3 = v1+2`)
writes the count at `0x801F1002` and the ids at `0x801F1004` onward. The
developer saw the result: a pickup with no name and garbage icon pixels. That
whole code path was **removed**, not fixed, because the array's identity could
not be established to my satisfaction.

## Open work, in priority order

### 1. `1/1` — an item that reports as equipped and cannot be equipped

Developer, verbatim: *"durandel shows up as 1/1 (which means, I have one, and one
is already equipped by someone). which means that I can't equip it on anyone."*

Not investigated at all. The likely mechanism, and the first thing to check: the
mod rewrites the id inside a slot of the list, but the game keeps its own
per-item bookkeeping (equipped counts, the `+0x02` in-use/cache byte, the
`0x80196A58` flag bits) elsewhere. A rewrite that changes the id without
adjusting that bookkeeping can leave a "one equipped" claim pointing at an item
the player does not own. The **source-table rewrite** (mechanism 1 above) does
not have this shape, because the game's own bookkeeping then sees the replacement
item from the start; the list rewrite (mechanism 2) does.

Cheapest checks: `func_8016B6FC` (`0x8016B6FC`) is the 278-list search and
`func_8016B738` (`0x8016B738`) the 40-list equivalent; `func_80170800`-family
accessors and the `+0x02` byte's writer (`RecompiledFuncs/funcs_12.c`,
`0x80172CC4` onward) are where a count/equip flag is updated. Read those before
changing anything.

### 2. One find, several items

The developer's last run: "old clothing shows once", and the items acquired were
**Oracion, Durandel, Bloody Emblem**. In the run before, one find produced four
grants in the same frame (slots 17–20) plus one later (slot 21).

The mod's log shows **one `acquire` per slot and one `replace` per `acquire`**,
so the mod is not duplicating. Either the game grants a set of items for one
object interaction, or an item merged into an existing stack (which the mod
deliberately ignores, since only an empty slot counts). The two cases are
distinguishable from the log: several `acquire` lines means the game granted
several; one `acquire` with a growing `count` means a merge.

**A future session should add a count-change probe** — log every slot whose count
increases, with the old and new count — because that is the one acquisition the
mod currently cannot see and the developer's complaint is about item count.

### 3. The drop table may not be the battle-drop source

The rewriter fires (`drop-table` lines, `probe ... 39 80 1`) and the game also
reverts it mid-session (`drop-table 23 22`, `22 23`, `1 75` — values returning,
which is the game restoring ROM data). Whether a battle reward changed as a
result is **not established**. The reader `func_ovlN_801AD6BC` is keyed on
`lbu(0x80197708)`, which looks like a map object's type, and it writes map-object
action fields (`0x801F0E00`, `0x801F0E20`); it is reached from the scene-0x03
**enter** (`func_ovlA_8019EE70`, `0x8019F4AC`). `docs/notes-drop-table.md` and
`docs/notes-tutorial-drops.md` disagree about which bank holds some of these call
sites and about the tutorial; both should be re-read with the disassembly open.

### 4. A port bug worth fixing on its own

**A mod config file is rejected unless it carries a `mod_id` key, and the
runtime's own writer never writes one.** `parse_mod_config_storage`
(`tools/N64ModernRuntime/librecomp/src/mod_manifest.cpp:815`) returns false when
`config_json.find("mod_id") == end()`, so `load_config` bails and every option
silently reads its default. The file the runtime writes
(`build-app/mod_config/ogre_item_randomizer.json`) has no such key, so **every
mod option in this port is permanently its default**, whatever the player sets.
Adding `"mod_id": "ogre_item_randomizer"` to the file makes stored values take
effect (verified: `chance 55`, `shops 1` arriving in the mod). This cost this
session most of a day of confusion. The fix is a line in the writer
(`mods.cpp:700`-ish, `save_mod_config_storage`) and/or making the validator
tolerant.

### 5. Hooking streamed banks, the real fix for battle drops

The deciding code for a battle drop is in streamed bank code, and a mod hook
resolves through `get_vrom_to_section_map()`, which `init_overlays` fills from
the base ELF's sections only (`librecomp/src/overlays.cpp:614`). The bank units
in `app/src/bank_overlays.cpp` are not in it. Registering them there (or adding a
fallback lookup) would let a mod hook `func_ovlN_801DC740` directly, which
removes every timing problem in this handoff and makes one code path serve drops,
pickups and gifts. **This is the change to make if the feature is to be finished
properly.**

## Files changed

| path | what |
|---|---|
| `mods/item-randomizer/` | the mod: `src/item_randomizer.c` (672 lines), `mod.toml` (4 options), `README.md`, `mod.ld`, `include/modding.h` |
| `app/src/main.cpp` | `OGRE_MOD_TEST=1`: an installed mod otherwise forces the start screen, which owns the mod toggles, so a scripted run could not get past it |
| `tools/build-example-mods.sh` | `-mno-check-zero-division`: gcc's MIPS default emits a `teq` after each division, and `RecompModTool` has no case for `teq` (`Unhandled instruction: teq` → `Failed to recompile mod`) |
| `tools/N64ModernRuntime/librecomp/src/mod_config_api.cpp` | the `recomp_log` export (**gitignored vendored tree**, so it must be listed here explicitly; a build without it refuses this mod with `Imported function not found:*:recomp_log`) |
| `docs/notes-save-items.md`, `docs/notes-item-re.md`, `docs/notes-drop-table.md`, `docs/notes-tutorial-drops.md` | research notes from four agents; each states what is proven and what is not |

Nothing is committed. `tools/RT64` was already modified before this session.

## Probes used and their status

The mod's `LOG` option and the `probe` line are the instrumentation; there are no
patched generated files to revert. `grep -rl probe122 RecompiledFuncs/ Bank*Funcs/
app/` is not applicable — nothing in the recompiled output was touched.

## Lessons this session paid for

1. **A mod option that reads its default looks exactly like a mod that does
   nothing.** The `mod_id` bug hid every result for hours. A mod should log one
   unconditional line at startup saying which options it resolved; this mod now
   does (`[mod] start verb mode shops 2 0 0 0`).
2. **Never write guest RAM on a half-derived address.** The `0x801F1004` write
   was off by two bytes and corrupted items in a way that looked like a mod
   feature. The handoff's rule: a write to an array needs its writer
   disassembled, not its reader.
3. **I corrected myself three times in this session** — `0x801936D8` is the
   experience award, not the drop; the tutorial does load record 7; and the
   `39 80` probe reading that I first called proof of residency came from a run
   whose other samples were not record 7 either. Inference from disassembly was
   wrong each time, and a single measured log line was right each time.
4. **Bound the diagnostics.** An unbounded per-event `fprintf` made combat
   unplayable; the first diagnosis was "the mod is too slow". The mod now prints
   at most 20 event lines per frame plus a summary.
5. **Ask the developer, do not infer, when a mechanism has two candidate
   addresses.** The dev's "it shows garbage, no name" identified the map-table
   bug immediately; I had been treating that array as verified for hours.
