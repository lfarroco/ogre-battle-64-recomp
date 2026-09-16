# Handoff — 2026-09-16, session 55: the New Game name-entry form renders (scene `0x07`'s streamed module is bank unit H)

## Goal and result

**Goal (developer):** continue from session 54 — scene `0x07` (the New Game
name-entry form) entered but rendered black.

**Result: the form renders.** Driving the opening from the title, the route runs
title → `0x02` → `0x0D` step 1 (movie) → `0x02` → `0x0D` step 2 (cathedral) →
`0x07` and the **name box (`Magnus`), the `A–Z` / `a–z` grid, `◀ ▶ INS BS DEL
END` and the `Is the name Magnus acceptable? / Yes No` prompt all draw** —
`docs/proofs/native-newgame-name-entry.png`. The form then advances: after `A`
it hands off to `0x02` → `0x0D` again at `t≈20.7 s` (4×), i.e. the opening
continues.

**Session 54's diagnosis was wrong in its two central claims**, and both are
corrected at instruction level here:

1. **The scene-`0x07` descriptor is `D_8018FDAC`, not `D_8018FB98`.** The
   dispatcher's accessor table is built at startup by `func_80075BC0`
   (`0x80075C44`: `D_800AF028[7] = func_8017B600`), and `func_8017B600` is
   `lui $v0,0x8019 / jr $ra / addiu $v0,$v0,0xFDAC` — it returns `0x8018FDAC`
   (streamedB ROM `0x65CAC`; the session-54 log shows exactly this). Its words
   are `enter 8017B794`, `update 8017B858`, `hook 8017B9C8`, `leave 0`,
   **`mask 0x00000002`**. `D_8018FB98` (ROM `0x65A98`, mask `0x8000`) is a
   *different entry in the same data table*; session 54 matched `0x801A578C`
   against that table's layout and drew the wrong conclusion. The real enter
   `func_8017B794` never calls `0x801A578C`.
2. **The scene-`0x07` module is not missing — the port compiled the wrong
   layout at its addresses.** The enter chunk-DMAs a **0x8600-byte code module,
   ROM `0x712A0` → RAM `0x8019A7C0`** (67 × `0x200`-byte chunks,
   `OGRE_DMA_TRACE`: `first=18006..18072`), then calls it. `0x801A578C` is a
   real function in overlay C's ROM (`0x1DBC3C`) but belongs to **record 3's**
   RAM; it plays no part in this scene.

## 1. The module, and why the net was wrong

`func_8017B794` (streamedB `0x8017B794`, descriptor `0x8018FDAC`):

* `0x8017B7AC` `func_800900C0(0x801A2C30, 0x801A18E0 - 0x801A2C30)` at
  `0x8017B79C`/`0x8017B7A0` (`lui $a0,0x801A` + `addiu $a0,$a0,-0x5840`) and
  `0x8017B7A4`/`0x8017B7A8` (`lui $a1,0x801A` + `addiu $a1,$a1,0x18E0`): a
  **backward bound-copy** of `0x8140` bytes from `0x801A2C30` down to
  `0x801A18E0`. (The second copy in the same listing, `0x8017B7C4`
  `func_80090010(0x801A18E0, 0x801A2C70 - 0x801A18E0)`, is a no-op pair: the
  `beql` at `0x8017B7FC` sees `a0 == a1` and branches past it.) Note this
  destination is the module's own **data** region in ROM (`0x801A18E0`), so the
  bytes there after the enter are the copy; the module's code remains at its
  load address, which is where every entry (`0x8019A884` etc.) lives.
* `0x8017B7E4` `func_8009DA50(0x712A0, 0x801A2C30, size)` (`a0 = 0x7<<16 +
  0x12A0`, `a1 = 0x801A2C30`, `a2 = 0x8<<16 - 0x68B0 = 0x79750 - 0x712A0 =
  0x84B0`): the module DMA. The chunked DMAs continue past that first call —
  the trace shows 67 × `0x200` from ROM `0x712A0` to `0x798A0`, i.e. a `0x8600`
  load — which is why the unit is sized `0x8600`, not `0x84B0`.
* `0x8017B838` **`jal 0x8019A884`**: the module's entry (`0x8019A884` is the
  first function after the module's 0xC4-byte header at `0x8019A7C0`).

The module's RAM `0x8019A7C0..0x801A2DC0` overlaps:

* **record 3** (bank unit A, `0x8019EE70..0x801AD5C0`), and
* **overlay C / record 15** — the main ELF's `.streamedC`
  (`0x80197B90..0x801BA550`, from `build/ogrebattle64.map`).

N64Recomp binds a `jal` to a function it knows as a **direct C call**, so the
main ELF's compiled bodies at those addresses won. Measurement: of the module's
**24 internal `jal` targets, 0 are function entries in the main ELF** at the
same addresses (the main ELF instead has 67 *different* entries in the range,
from its streamedC layout). The module ran overlay C's bodies for every call —
which is why the screen was black and no display lists were emitted, without a
crash. This is AGENTS §4's mis-binding class, session 45's shape.

The readback of the ROM also corrects the natural guess about the module's tail:
`0x801A18E0` is **data** — the pointer table at `0x801A18F0` (targets
`0x801A1B7C`, `0x801A1B70`, …), the character grid `ABCDEFGHIJ…` at
`0x801A1B7C`, the default name `Magnus` at `0x801A1B88`, and form text
(`Reset Control Deck`, …). splat flags the same boundary ("symbol at vram
801A1884 ends with extra nops"), so `config-bankH.yaml` splits code/data at
**ROM `0x783C0`**.

## 2. New bank unit H

| | |
|---|---|
| record | `bankRec07`, ROM `0x712A0` size `0x8600` → RAM `0x8019A7C0` |
| configs | `config-bankH.yaml` / `config-bankH.toml` (`ovlH_` prefix) |
| `Makefile` | `BANK_UNITS := A B C D E F G **H**` |
| compiled | 47 functions + data; registered by the existing chunk-DMA hook (`on_streamed_dma` matches the `(rom, ram)` delta), so the runtime arms it exactly when the game loads it |

The module's RAM is not a segment-table record, so `tools/gen_bank_funcs.py`
does not list it in `kAllStreamedRecords`; that is fine — the `[bank] loading
overlay record rom=0x0712A0 …(45 functions)` line in the fixed run confirms the
registration.

## 3. A bug in `tools/cross_bank.py`'s overlap model (found here)

`overloaded()` computed the ambiguous span as
`hi = min(r.hi, min(c.hi for c in competitors))` **and each region's
competitor list includes regions that start before it**, so the span collapsed
to the lowest competitor end. For unit H that reported only
`0x8019A7C0..0x8019F450` (because record 3's overlap begins at `0x8019EE70`),
and the module's own calls fell **outside** the swappable ranges — so
`dispatch --only 0x8019A884` answered "matched no call site". It is now
`hi = min(r.hi, max(c.hi for c in competitors))`. Effect on the report: the
swappable ranges go from 5 to 6 and now cover `0x8019A7C0..0x8019BA550`; the
main unit's function entries in swappable ranges go from 78 to 250 (the same
addresses were always at risk; the model just could not see them).

## 4. The dispatch, and persistence

`func_8017B794`'s `jal 0x8019A884` is bound by the recompiler to the containing
main-ELF body `func_8019A7C0` (streamedC's layout). With the fixed model,
`cross_bank.py dispatch --only 0x8019A884` rewrote it to
`LOOKUP_FUNC(0x8019A884)` (and repaired the tail-call shape to
call-and-continue). The `make recomp` `--only` list now also carries every call
the resident code makes into the module:

    --only 0x80198D28,0x801AFC2C,0x801980A0,0x801B00D0,
           0x8019A7C0,0x8019A884,0x8019B060,0x8019B340,0x8019C4A8,0x8019C69C

(the last six are the complete set of `LOOKUP_FUNC`s into `0x8019A7C0..` after
the full `dispatch`; `grep -n "LOOKUP_FUNC(0x8019" RecompiledFuncs/*.c`).

## 5. What was run for verification

```sh
# forced: the form renders in ~2 s (the fastest check)
OGRE_SCENE=0x07 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 \
  OGRE_CAPTURE_PRESENT=/tmp/f07 OGRE_CAPTURE_EVERY=20 OGRE_CAPTURE_AFTER=200 \
  OGRE_EXIT_AFTER_MS=9000 ./build-app/ogrebattle64 assets/ogre64.z64
#   -> 50 frames, nonblack 219302/128 distinct (before the fix: 0/1)
#   -> [bank] loading overlay record rom=0x0712A0 ram=0x8019A7C0 size=0x8600 (45 functions)

# natural, title-driven (the maintained repro) — the form, with the A prompt
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,start,a,start,a,start,a" \
  OGRE_SCENE_LOG=1 OGRE_PROBE57=1 OGRE_PRESENT_ALWAYS=1 \
  OGRE_CAPTURE_PRESENT=/tmp/nat OGRE_CAPTURE_EVERY=100 OGRE_CAPTURE_AFTER=400 \
  OGRE_EXIT_AFTER_MS=22000 ./build-app/ogrebattle64 assets/ogre64.z64
#   t=18.997s scene 0x07 (descriptor 0x8018FDAC mask 0x2)
#   t=20.655s scene 0x02   <- the form handed off
#   proof: docs/proofs/native-newgame-name-entry.png; cathedral re-verified unchanged

make bank-recomp            # cross_bank.py check-banks OK
python3 tools/cross_bank.py check   # OK (no bank unit calls into RAM it does not own)
cmake --build build-app -j8 # clean
```

The `make recomp` path is verified reproducible: after `make recomp`, the
`dispatch --only` line re-applies 13 call sites in 11 files (10 distinct
targets, 10 tail-call repairs), including `func_8017B794`'s
`LOOKUP_FUNC(0x8019A884)`; a rebuild and a forced-`0x07` run then load unit H
normally. `build-null` also builds clean.

No repo test harness; each check above is a ROM-dependent game run (9–22 s
wall-clock at 4×). Probe-free: no generated-code probe was added this session.

## 6. Files changed

* `config-bankH.yaml`, `config-bankH.toml` — **new** unit H (the form module);
  code/data split at ROM `0x783C0`.
* `Makefile` — `BANK_UNITS` gains `H`; the `recomp` dispatch `--only` list gains
  the six module targets; the cross-bank comment block documents them.
* `tools/cross_bank.py` — `overloaded()`'s `hi` fix (session-55 finding).
* `app/src/bank_overlays.cpp` — the `OGRE_DMA_TRACE` comment now names the real
  outcome (no code change).
* `docs/scenes.md`, `PLAN.md`, `docs/DECISIONS.md`, this file;
  `docs/proofs/native-newgame-name-entry.png` (new proof).
* Generated (gitignored): `BankHFuncs/`, `build/bankH*`, `app/src/bank_funcs.inc`,
  `RecompiledFuncs/*.c` (the dispatches).

## 7. Next leads

1. **The form's own input path.** The form is live and accepts `A` (the `Yes/No`
   prompt appeared in the natural run). Drive `Yes` → the sequence should reach
   session 44's step 3+ (birthday form, personality questions). Confirm with
   `OGRE_PROBE57` and a capture.
2. **The same module serves other scenes?** It is loaded by `func_8017B794`
   only; check whether the `0x8017B5D0`/`0x8017B5DC`/`0x8017B600` accessors
   (scenes `0x1E`/`0x1F`/`0x20`) load it too — if they do, unit H already covers
   them.
3. **The `0x801A18E0` copy.** The enter bound-copies `0x8140` bytes from
   `0x801A2C30` down to `0x801A18E0`, which overlaps the module's own data
   region in ROM. In the ROM that region *is* data; the copy is what the game
   does, so leave it — but it means the data at `0x801A18E0` in a dump after the
   enter is the copy, not the module's tables. Worth one dump check if a later
   form step misbehaves.
4. **`cross_bank.py`'s fixed model changes the reported backlog** (250 main-unit
   entries in swappable ranges, 77 targets with no bank entry). Re-run
   `make cross-bank-report` when choosing the next bank to compile.
