# Handoff — 2026-10-03, session 125: the Item Randomizer loses every option

## Goal

Developer request: the Item Randomizer is still experimental, so mark it in the
manifest and remove all of its options.

* The mod description carries a clear experimental note.
* SEED is removed; the sequence is always random (as random as an N64 code mod
  can be).
* CHANCE is removed; every eligible acquisition is rerolled.
* SHOPS is removed; it was never implemented.
* LOG is removed; the format is for development and not for a player.
* FLAVOR is removed; the replacement may always be anything in the item table.

## Result

`mods/item-randomizer/mod.toml` carries no `[[manifest.config_options]]` at all,
and `mods/item-randomizer/src/item_randomizer.c` reads no config value. The
behaviour the removed options used to select is now fixed:

| removed option | fixed behaviour |
|---|---|
| SEED | derived from the frame counter at `0x800AEFA4` on the first tick, so it is new every boot |
| CHANCE | 100: the roll is unconditional |
| SHOPS | off: a purchase frame, recognised by falling gold, is always skipped |
| LOG | `LOG_LEVEL`, a compile-time constant in the source, `0` (silent) in a shipped build |
| FLAVOR | `ANYTHING`: the roll covers the whole id space of the list |

The `mode` parameter and the `SAME KIND` branch are deleted from
`roll_replacement`, `replacement_for`, `replacement_for_slot`, `drop_slot` and
the three table walkers. `recomp_get_config_u32` and the `config_u32` wrapper are
deleted, and so is `item_category`, which only the `SAME KIND` branch used.

The diagnostic lines stay in the source, gated on `LOG_LEVEL`. Raising it and
rebuilding is now the only way to get them; the line set is otherwise unchanged,
except that the one-time announcement is `start verb` and `start seed` (the
`mode`/`shops`/`chance` fields it used to print no longer exist) and the
save-read line is gated the same way.

## Verification

`tools/build-example-mods.sh mods/item-randomizer` passes end to end (Docker
`gcc-mips-linux-gnu` compile, then `RecompModTool` recompile against the base
game's symbols) and writes `build/mods/item-randomizer.nrm`. This exercises the
whole changed source and the new manifest.

The built package's `mod.json` was read back out of the `.nrm`:

* it has no `config_options` key, so the MODS panel's row builder
  (`app/src/ui.cpp`, `Tab::Mods`) adds no `RowKind::Option` rows for this mod;
* `short_description` is `EXPERIMENTAL: randomize dropped and found items.`,
  which is the text `Panel::right_text_for` draws beside the mod row;
* `description` begins `EXPERIMENTAL.`.

A host `cc -fsyntax-only` reports no unresolved identifier; its only errors are
the macOS `section` attribute that `include/modding.h` uses for `RECOMP_IMPORT`
and `RECOMP_HOOK`, which are target-specific and unrelated to this change.

No game run was made. The launcher's MODS tab was not captured as a screenshot.

## Files changed

| file | change |
|---|---|
| `mods/item-randomizer/mod.toml` | experimental `description`/`short_description`; all five options removed |
| `mods/item-randomizer/src/item_randomizer.c` | no config read; SEED, CHANCE, SHOPS, LOG and FLAVOR fixed in code; `mode` plumbing and `item_category` removed; diagnostics behind `LOG_LEVEL` |
| `mods/item-randomizer/README.md` | experimental note; Options section rewritten as fixed behaviour plus the `LOG_LEVEL` recipe |
| `mods/item-randomizer/run-with-save.sh` | stops writing `mod_config/ogre_item_randomizer.json`, which no longer exists |
| `PLAN.md` | Status and open work 16 note the option removal |
| `docs/STATUS-LOG.md`, `docs/DECISIONS.md` | session entry and decision row |

## Open

* The mod has no user-facing way to turn the diagnostics on. A development run
  needs `LOG_LEVEL` raised in `src/item_randomizer.c` and a rebuild.
* Session 124's open item is unchanged: `fix_record_use` has still never written
  a corrected `+0x02` in game, because no acquisition in the runs so far
  inherited a non-zero byte.
