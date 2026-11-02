# Permanent Suspend Save

An Ogre Battle 64: Recomp code mod. It keeps the game's suspend (temporary) save
instead of letting the game erase it when it is loaded.

## What it does

The Suspend command writes one SRAM record: 19176 bytes at device `0x30B0`,
slot 15. Loading it resumes exactly where it was taken, and then the load menu
erases the record, so the point can only be resumed once.

With this mod the record stays after a load. Quit the game, relaunch, open
`Load Game`, and the same suspend point is still there. The two normal save
slots are unchanged, and a new Suspend overwrites the previous record as usual.

The record is part of the 32 KiB battery image (`saves/ogrebattle64-us-rev1.bin`
beside the executable), so it stays with the rest of the save data.

## How it works

The suspend record is written by `func_8007485C(15)`, called from bank unit A's
suspend action (`func_ovlA_8019F994`, `0x8019F9F8`). The load path is
`func_800749C0(15)`; on success the load menu's confirm handler
`func_801A9B3C` (streamedC) tests that the loaded slot was 15 and calls
`func_80074834(15)` at `0x801A9BCC`.

`func_80074834` is `.main` code, 0x28 bytes, and it does two calls:

| call | effect |
|---|---|
| `func_80074F9C(15)` | blank the 12-byte header at `image + 0x30B0` in the cached 32 KiB image: the two `u16` checksums to zero, `QuestOG3` copied to `+4`. The payload after the header is untouched. |
| `func_80074BF0(0x37081383)` | write the cached image to SRAM when dirty, then free the cache. |

`func_80075578` accepts a record whose first word is zero as "empty", which is
why clearing those 12 bytes is enough to delete it.

The mod is a `RECOMP_PATCH` of `func_80074834`. For slot 15 it skips
`func_80074F9C` and still calls the commit, so the image is flushed exactly as
before and the record survives. Any other slot runs the original two calls.

## Building

```sh
make mod-syms        # writes mods/reference/dump.toml (needs the linked ELF)
make example-mods    # writes build/mods/permanent-suspend.nrm
```

`make example-mods` uses a `mips-linux-gnu-gcc` on `PATH` when there is one and
falls back to `gcc-mips-linux-gnu` in a `debian:bookworm-slim` Docker
container. `tools/build-example-mods.sh mods/permanent-suspend` builds just this
mod.

## Installing

Copy `permanent-suspend.nrm` into the client's `mods/` folder (next to the
executable, alongside `saves/`). The client lists it in the start screen's
**MODS** panel, where it can be turned on. The manifest ships it off by default,
so the package plays the vanilla game until the player opts in.

## Verification

Take a suspend save in a mission, note the file
`saves/ogrebattle64-us-rev1.bin`, then load the suspend save.

* Without the mod, the load erases it: the 12 bytes at `0x30B0` become
  `00 00 00 00 51 75 65 73 74 4F 47 33` (`00 00`, `00 00`, `QuestOG3`).
* With the mod, the same 12 bytes keep the record's two checksums and the image
  still passes `func_80075578`, so `Load Game` keeps offering the suspend entry
  after a relaunch.

`tools/sramsave.py check <file>` reports the slots an image holds and validates
them, which is the offline half of that check.

## Limit

The same erase routine is reachable from a debug action in bank unit H ("Reset
Control Deck", `func_ovlH_8019B340+0x10ac`), which erases slots 0, 1, 15 and 14
in turn. That action therefore also leaves the suspend record behind. Nothing a
normal player path does calls `func_80074834` for slot 15 except the load.
