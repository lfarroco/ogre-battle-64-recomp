# Session 34 — the missing smoke/logo effects: a stale linker script, fixed

## Addendum — the reported regressions (no intro smoke, no 3D "que" in the logo)

Both are the **same bug**, and it was found and fixed after the cross-bank work
below.

### Root cause: the checked-in linker script was stale, so overlay C was never loaded

`build/ogrebattle64.elf`'s section table had overlay C at ROM `0x0E4910` instead
of `0x1CE040` (the config's `start`). `0xE4910` is **bank record 2's** ROM start,
i.e. the linker script still described the abandoned "bank records inside the
main unit" layout from sessions 31/32. Consequences:

* `ogre::register_streamed_overlays()` calls `load_overlays(0x1CE040, 0x80197B90,
  0x22A00)`, which matched **0 sections** — overlay C's 67 function-map entries
  never existed.
* The intro then DMA'd record 2 over the same RAM, and everything the game drew
  through function lookup into overlay C's tail hit the generic stub:
  `func_8019E588` (the smoke puffs, session 29) 2024 times and `func_801A34FC`
  3116 times — 5140 stub calls out of a 45 s run.

`make` never re-runs `splat split` for the main unit, so nothing regenerated it:
`ogrebattle64.ld` and `config.yaml` carried the same mtime.

**Fix:** re-split the main unit (`splat split config.yaml` after removing the
generated `ogrebattle64.ld` and the stale `assets/*.bin`). The map now has
`streamedC` at `0x1CE040`.

### Second bug: a partial bank swap dropped the evicted overlay's whole map

`recomp::overlays::unload_overlapping_overlays` erased **every** function-map
entry of a section it evicted, even though a bank record is much narrower than
the overlay it replaces (record 2 is `0x72C0` of overlay C's `0x229C0`). The RAM
above the record is not overwritten, so that overlay's code still runs.

**Fix:** only erase the entries the incoming load actually covers (both for
sections and for function banks). This keeps the tail reachable and is what makes
the boot intro's effects survive the first bank swap. The runtime patch
(`n64modernruntime-ob64.patch`) was regenerated.

### Where these stand

| check | before | after |
|---|---|---|
| intro stubs (45 s) | 5140 | **0** |
| `load_overlays` matches for overlay C | 0 | 1 |
| boot intro / lore / attract | effects missing | renders (`/tmp/attract_run.log`, exit 0, 6500+ display lists) |

### Still to check

A full attract loop (title screen with the soldier intro and the "Ogre Battle 64"
logo) should be re-captured into `docs/proofs/` as the visual proof for both
effects.

---

Goal (from session 33 §14 and §12): finish the New Game / Tutorial path by
routing the main unit's cross-bank `jal`s through `get_function` and seeding each
bank unit with the cross-bank entry points.

Done: the routing mechanism is built (`tools/cross_bank.py`, an opt-in Makefile
target), **two real bank-build defects are fixed** (a stale-object link that made
every seed experiment a no-op, and a `jal`-source-vs-callee bug), and the reason
the routing cannot ship yet is measured: **62 of the 67 function entries in
overlay C have no bank registration at all, and only 9 of the 67 cross-bank `jal`
targets are entry points a bank unit's own disassembly found.** The default build
keeps session 33's bindings; the attract loop is exit 0 at 130 s.

## 0. State at the start and at the end

```sh
# session 33 state, still the default behaviour after this session:
OGRE_NO_AUDIO=1 OGRE_SPEED=4 OGRE_EXIT_AFTER_MS=130000 ./build-app/ogrebattle64
#   exit 0 after 130 s (~520 s game), no crash, 66 bank/arena loads,
#   5140 streamed-stub calls from two addresses (see §3), 0 "Failed to find".
```

Build order (unchanged for the default build):

```sh
make recomp        # main unit -> RecompiledFuncs/
make bank-recomp   # bank units -> build/bank<U>.elf, Bank<U>Funcs/, bank_funcs.inc
cmake --build build-app -j
```

## 1. Fixed: the bank ELF silently linked stale objects

`build/bank<U>.elf` used to depend only on the generated `build/bank<U>.ld`, and
its recipe found the `.o` files by **globbing**. `splat` rewrites an `.s`
whenever it re-splits, but only rewrites the `.ld` when the *layout* changes —
so a re-split that changed code left `make` thinking the ELF was current. The
first seed experiment reported "186 seeds written" while the ELF contained none
of them (`nm` showed the old symbol table).

**Fix:** `build/bank%.elf: config-bank%.yaml | build/bank%.ld` (config as a real
prerequisite, the `.ld` order-only), with `bank-force` keeping the recipe
unconditional. Any config change now re-assembles and re-links every unit.

## 2. Fixed: the dispatch address is the `jal` source, not the callee

N64Recomp emits a **size-overridden** target as a call to the *containing body*:
`jal 0x80198D28` (inside `func_801989AC`, size 0x520) becomes
`func_801989AC(rdram, ctx)`, deliberately. Rewriting that call as
`LOOKUP_FUNC(0x801989AC)` broke overlay B's menu path. The generated C's
`// 0xADDR: jal 0xTARGET` comment is the authoritative source address, so
`cross_bank.py dispatch` now keys on it.

## 3. Measured: what the cross-bank path actually needs

`tools/cross_bank.py` models every RAM region that can be occupied by more than
one section (overlay A/B/C from the linker map, every bank record from the
`config-bank<U>.yaml` extents) and reports:

| quantity | value |
|---|---|
| swappable RAM (merged) | `0x80197B90..0x8019A680`, `0x8019EE70..0x801AD2B0`, `0x801AD5C0..0x801B4CC0` |
| cross-bank `jal` call sites in the main unit | 95 |
| distinct cross-bank `jal` targets | 67 |
| targets that are an entry point in some bank's own disassembly | **9** |
| targets that are body interiors in every bank that covers them | **58** |
| overlay C function entries with no bank registration | **62 of 67** |
| bank-registered addresses (all units) | 1395 |

The 58/62 gap is the wall. Record 1's `0x80198D28` is the canonical case: the
main unit compiled it as a function (`func_80198D28`), but `splat` merged it into
`func_ovlD_80198A6C` in unit D, so `get_function(0x80198D28)` has no entry and the
lookup hits the generic stub.

**Forcing the split does not work.** `splat` has to be told the address is a
function; putting a `func_<vram>` symbol at a body interior made it split an
existing function, and N64Recomp then compiled the fragment with a `goto` whose
label lives in the sibling fragment:

```
BankDFuncs/funcs_0.c:3843:14: error: use of undeclared label 'after_4'
```

Rewriting only the 9 resolvable targets and leaving the other 58 bound still
crashes the boot (SIGBUS before the first present, 0 bank loads), so the dispatch
path is **not correct yet** and is off by default.

### What the stub calls are

With the default (session-33) bindings, the 130 s run makes 5140 stub calls, from
two addresses only:

```
3116  streamed function stub called @ 0x801A34FC
2024  streamed function stub called @ 0x8019E588
```

Both are inside unit A's record-3 range but are absent from *every* bank table
and from the main unit's overlay C entries — i.e. they are only reachable from
code that the resident bank provides. They are the same class of failure as the
menu crash, and they are the reason the "0 stubs" line in session 33's 280 s run
needs re-measuring under the same conditions (that run used a seed file this
session's rebuild destroyed — see §4).

## 4. A caveat on reproducing session 33

The bank units' `build/bank<U>/symbol_addrs.txt` files — **generated and
gitignored**, so not in the repository — were consumed by session 33's splat run
and by this session's experiments. Session 33's build reported 40 functions for
record 2 and registered 1398 bank functions; the current config, built from
scratch, splits record 2 into 96 functions and registers 1395. The two splits
disagree on function boundaries in record 2/3, and that is why the 5140 stubs are
visible now: the current split has `func_ovlA_8019DFA4` (size 0x1F8) covering a
region where the session-33 split evidently had entries.

This is a reproducibility defect in the build, not a game or runtime defect: **a
bank unit's function set must not depend on a gitignored intermediate.** Fixing
it is the first task of the next session (see §6).

## 5. Files changed (this session)

* `tools/cross_bank.py` (new) — region model, `report`, `write-seeds`,
  `dispatch`, `revert`; reports the dispatched/blocked targets by name. A
  prologue check (`entry_looks_real`) refuses any address that would split an
  existing body, which is what makes a bad seed a warning instead of a broken
  build.
* `Makefile` — `build/bank%.elf` prerequisite fix, `bank-force`;
  `cross-bank-report` / `cross-bank-dispatch` (opt-in experiment).
* `config-bank{A,C,D,E}.yaml` — cleaned back up (an experimental
  `symbol_addrs_path` was added and removed again).
* `docs/DECISIONS.md` — session 34 entry, including the negative results.

## 6. Next

1. **Make the bank function sets deterministic.** Either commit a per-unit
   `symbol_addrs`/`functions` list (the addresses really are a property of the
   game's overlay layouts) or derive it from the ROM, and make `make bank` fail
   loudly if a unit's function count changes unexpectedly. Re-measure session
   33's 280 s `OGRE_SPEED=4` run afterwards and compare the stub count — the
   5140 stubs above are the number to beat.
2. **Then finish the dispatch:** with a deterministic, complete per-unit entry
   set, the 58 blocked `jal` targets stop being body interiors, the seeds become
   pure renames, and `make cross-bank-dispatch` can be turned on and tested
   against scene `0x18`/`0x02` (`OGRE_TAP_MS=700`).
3. Still open from sessions 29–33: `osViFade` unemulated (fades snap),
   `SDL_OpenAudioDevice` blocking the start thread on some hosts
   (`OGRE_NO_AUDIO=1`), the session-31 idle-stall oddity, and the bank swap being
   "latest load wins" rather than descriptor-driven (session 33 §7.2).
