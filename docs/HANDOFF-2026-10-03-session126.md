# Handoff — 2026-10-03, session 126: the Item Randomizer leaves the party's starting items alone

Developer request: *"we created a mod for item randomization, but there's an
issue. the initial items that the player start the game with are all randomized
as well. those should be left alone."* Sightings confirmed by the developer:
a New Game from the title, driven to the map screen, with the randomized items
visible in the inventory.

**Result: the mod re-primes its two list shadows on any frame that fills more
than one record, because that is a bulk grant by the game and not an
acquisition. The party's starting items stay as the game granted them.**
Verified by an A/B run of the same New Game route at log level 2 (39 `replace`
lines before, 5 `re-prime` lines and 0 `replace` after), and developer-confirmed
on the fixed build ("I saw the expected items in the inventory").

## 1. The mechanism, at instruction level

The mod's list watch rerolls any owned-item record that was empty and has just
been filled (`mods/item-randomizer/src/item_randomizer.c`, the pass-2 loop
`if (id != 0u && list->shadow_id[slot] == 0u) acquired = 1;`). The game's own
starting-item grant is a straight-line run of literal grants in the map loader,
so every record it fills looked like an acquisition.

The loader is `func_ovlR_80215C38` (bankR record 9d, ROM `0x195430` size
`0x2380` → RAM `0x80214FA0`), called from bankN `func_ovlN_801AE880+0x764`:

```
801aefa4: lui  a0,0x19 / addiu a0,a0,0x5430     ; ROM 0x195430
801aefb0: lui  a1,0x8021 / addiu a1,a1,0x4fa0   ; RAM 0x80214FA0
801aefbc: jal  0x8009da50                        ; DMA the bank in
801aefe4: jal  0x80215c38                        ; func_ovlR_80215C38
```

The block that reaches it is gated on the map index: `0x801aee54
lbu v0,0x801f1054` compared with `lbu a0,0x8018f4a1`, and it stores the new
index at `0x801aee6c`. It runs once per map entry, in one frame.

Inside `func_ovlR_80215C38`:

* `0x80216f10`-`0x80217028`: for each unit it creates, four class-derived
  equipment ids from `0x80187c62 + class*0x48` (`+0x62`, `+0x64`, `+0x66`,
  `+0x68`) through `jal 0x801dd430` = `func_ovlR_801DD430`, the 278-slot list add
  with count maintenance.
* `0x80217050`: `lbu(0x8018f4a1) - 63`, `sltiu v0,v0,2`, so the map index being
  63 or 64 selects the small set.
* `0x80217068`-`0x802170C4` (that set): `li a0,1` five times, `li a0,8` twice,
  `li a0,22` once, each `jal 0x801dc740` = the grant routine, `a1 = 0`.
* `0x802170CC`-`0x80217244` (otherwise): `2,3,4,5,8` five times each, `21` twice,
  `22` three times, `23` twice.

Every literal `a0` is below `0x8000`, so bit 15 is clear and the grants fill the
40-slot consumable list at `0x80193AE0`; the class equipment fills the 278-slot
list at `0x80196B20`. Both lists are the ones the mod watches.

## 2. The change

`mods/item-randomizer/src/item_randomizer.c`:

* `RELOAD_SLOTS 12u` becomes `BULK_SLOTS 1u`, with the instruction addresses
  above in its comment. A frame that fills **more than one** record re-primes the
  shadows instead of rerolling; a single filled record is still an acquisition.
* The walk's re-prime condition and the `equipment_reload` assignment use the new
  name. The coupling is unchanged and now matters more: the map loader touches
  both lists in the same frame, so a bulk equipment change re-primes the
  consumable list too even when its own records only merged into stacks.
* The save-field reader hook on `func_800749C0` stays as the precise save-load
  signal; the record count is now also sufficient for the 40-slot list, whose
  four records in an early save were the reason the count alone used to be
  rejected.
* `README.md`: the list-watch section, the Options table, the `re-prime` log row,
  and a new paragraph in "Where it does not reach" with the addresses above.
* The file-header note about the list watch.

## 3. What was run for verification

Both runs are a New Game from the title, driven by the synthetic pads, in a
fresh config dir with only this mod installed:

```sh
cp build/mods/item-randomizer.nrm /tmp/ir-newgame/mods/
OGRE_PREF_DIR=/tmp/ir-newgame OGRE_MOD_TEST=1 OGRE_SCENE=title OGRE_SPEED=4 \
  OGRE_TAP_MS=1000 OGRE_TAP_BUTTON="start,a,start,a,start,a,a,…(58 more a)" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=200000 ./build-app/ogrebattle64 assets/ogre64.z64
```

Baseline (`/tmp/ir-newgame-baseline.log`, `LOG_LEVEL 2u`), four bulk frames in
the opening, every record rerolled:

```
[mod] frame acq countup reroll 13907 9 0 9
[mod] frame acq countup reroll 14753 9 0 9
[mod] frame acq countup reroll 14754 8 0 8
[mod] frame acq countup reroll 14755 14 0 13
39 x [mod] replace <from> <to> <slot> 0
```

Fixed (`/tmp/ir-newgame-fix.log`, `BULK_SLOTS 1u`, `LOG_LEVEL 2u`), the same
route, the same frames re-prime and nothing is replaced:

```
[mod] re-prime 9 1 0 0
[mod] re-prime 0 2 1 0
[mod] re-prime 8 3 0 0
[mod] re-prime 0 4 1 0
[mod] re-prime 4 5 1 0
0 x [mod] replace
```

Both runs reach the map: scene `0x05` at `t=159576ms` (baseline) and
`t=146859ms` (fixed), then `0x06`. The developer watched the fixed run's window
and reports the expected items in the inventory.

The two later `frame acq countup reroll 36070 0 1 0` / `36310 0 1 0` lines are
merges into stacks the party already owned (`count-up`), which the id shadow
cannot see and which were never rerolled.

A third run checks the other side of the rule, on the shipped build
(`LOG_LEVEL 0`) with the live console on the same New Game route
(`/tmp/ir-single2.log`):

```
d 80196E40 8                      -> 00 00 00 00 00 00 00 00     (slot 200 empty)
w 80196E40 01003200               -> d 80196E40 8 = 01 00 32 00  (id 0x0100, planted)
d 80196E40 8   (5 s later)        -> 00 D8 32 00                 (id 0x00D8 = 216)
d 80196E44 8                      -> 00 00 00 00                 (slot 201, untouched)
```

The console's `w` stores a guest word, so `01003200` is the guest bytes
`01 00 32 00`: one record, one frame, and the mod rerolled it (`00 D8`). The
record's `+0x02`/`+0x03` bytes are the ones the write planted and were not
changed, which is the `s_roster_ok` gate skipping the count correction while the
opening's roster is not yet populated. The untouched neighbour stayed zero.

## 4. Files changed

| file | change |
|---|---|
| `mods/item-randomizer/src/item_randomizer.c` | `BULK_SLOTS 1u` replaces `RELOAD_SLOTS 12u`; the re-prime comments carry the map loader's addresses |
| `mods/item-randomizer/README.md` | the multi-record rule, the starting-items paragraph, the Options row, the `re-prime` log row |
| `PLAN.md` | the player-features paragraph and open work 16 |
| `docs/STATUS-LOG.md` | the session entry |
| `docs/DECISIONS.md` | the session entry |
| `docs/HANDOFF-2026-10-03-session126.md` | this file |

Generated and gitignored: `build/mods/item-randomizer.nrm` (the shipped build,
`tools/build-example-mods.sh mods/item-randomizer`).

## 5. Probes used and reverted

One probe: `#define LOG_LEVEL 2u  // probe126` in
`mods/item-randomizer/src/item_randomizer.c`, for the run logs above. Reverted to
`#define LOG_LEVEL 0u`; `grep -rn probe126 mods/ app/ RecompiledFuncs/
Bank*Funcs/` is empty, and the mod was rebuilt after the revert. No generated
code, app or runtime probe was used.

## 6. Open

* The rule leaves alone **any** multi-record frame, so a grant the mod would
  otherwise reroll is missed if it arrives with a second record in the same
  frame. One interaction grants one record, so no observed path does this.
* A map load whose starting set is almost entirely merged into stacks the party
  already owns can still present a single new record (for example one item the
  party does not own yet). The coupled `equipment_reload` covers the usual case,
  because the class-equipment burst is in the same frame; a map where no unit is
  created would not.
* Bank-function hooks (`docs/notes-bank-hooks.md`) would let the grant routine be
  hooked directly and remove the need for the count heuristic.
