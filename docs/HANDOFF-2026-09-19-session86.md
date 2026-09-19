# Handoff — 2026-09-19, session 86: the credits screen (scene `0x11`) plays

**Goal (developer):** *"let's try fixing the credits screen"* — session 85 left it
as the open wall: at the end of a playthrough the credits froze on the
"Ogre Battle 64" logo (`docs/HANDOFF-2026-09-19-session85.md` §7).

**Result: fixed.** On the natural route the credits now play through — the fade
from black to "Ogre Battle 64", the scrolling staff roll over the game's
backgrounds, the **Chaos Frame** total screen (scene `0x13`), then back to the
attract loop. Developer-confirmed live: *"it worked!! there were some artifacts in
the rendered backgrounds, but you've done it!!"* (the background artifacts are a
separate, untouched issue).

Four fixes, all in `config.toml`, all the **session-41 class** (a
`function_sizes` override that runs past the next function's start, or a `jr ra`
whose delay slot splat cut into a separate symbol), and each one produced a
**frame leak on the frame-pump thread (t4)** — the freeze signature session 60
identified (`D_800AEFA4` frozen while `D_800C4BCC` counts).

## 1. Correction to session 85, first: the forced-scene repro was bogus

Session 85 measured the freeze with `OGRE_SCENE=0x11` and the live console's
`c`. **That run does not execute the credits.** With `OGRE_SCENE_TRACE=1` the
trace reports `active=0x0009 pending=0x0011` (or, at `OGRE_SPEED=8`,
`active=0x0011 done=1`) while the *window* plays the opening sequence: 50 of 56
captured frames hash-match a plain natural boot (soldiers, ATLUS, QUEST,
Nintendo 64 logo). The developer corrected this twice ("these are not the
credits, this is the opening sequence"; "that last run was the intro scene").
**Do not use a forced scene to validate the credits.** The only valid
reproduction is the natural ending, driven from a suspend save
(`assets/saves/suspend_final_boss.n64`, gitignored) at `OGRE_SPEED=8`.

The freeze measurement itself was real — the credits functions do execute in the
forced run (coverage shows `func_801AC944`/`func_801ACC24`/`func_801ACE00`), and
the frame counter freezes there too — but the *content* is the opening, so no
visual conclusion may be drawn from it.

## 2. The four mis-bindings

All four were found statically (see §3) and verified at instruction level against
the ELF symbol sizes, the raw ROM, and the generated C.

| # | override | was | now | ELF / real | symptom |
|---|---|---|---|---|---|
| 1 | `func_801AB76C` | `0x22C` | `0x4` | 4 (a 4-byte splat artifact) | swallowed the real `func_801AB770` (`0x228`), so `func_801AB998`'s `jal 0x801AB770` — the credits text drawer — compiled as *call the containing body + early `return`* |
| 2 | `func_801AB568` | `0x1E0` | `0x208` | real end `0x801AB770` (ELF `0x1D8` stops mid-epilogue) | the continuation `static_16_801AB748` did the s-register restores and `jr ra` but its `addiu sp,sp,56` delay slot came from `func_801AB76C`'s range and was **dropped** — every call returned with `sp` `0x38` too low |
| 3 | `func_801B00D0` | `0x544` | `0x4` | 4 (a 4-byte splat artifact) | swallowed the real `func_801B00D4` (`0x540`), so `func_801B1BBC`'s `jal 0x801B00D4` (on the credits call tree) compiled as *call + early `return`* |
| 4 | `func_801AFC2C` | `0x4A4` | `0x4A8` | ELF `0x4A4` stops one instruction short | its own `jr ra` delay slot `addiu sp,sp,0x80` at `0x801B00D0` was dropped — every call returned with `sp` `0x80` too low |

The instruction-level evidence:

* `func_801AB76C`/`func_801AB740` are the loop-branch and `jr ra` delay slots of
  `func_801AB568` (`0x801AB73C bnez` / `0x801AB768 jr ra`); `0x801AB770` is a
  real function (`addiu sp,sp,-56`, ELF size `0x228`).
* `func_801AB568`'s real body runs to `0x801AB770` — the epilogue
  (`lw ra/s7..s0`, `jr ra`, `addiu sp,sp,56`) is at `0x801AB740`–`0x801AB76C`.
  This is exactly the `func_8019ABC4 = 0x368` pattern the file already documents,
  so it is fixed the same way: extend *through* the epilogue.
* `func_801B00D0` is `func_801AFC2C`'s `jr ra` delay slot (`addiu sp,sp,0x80`);
  the real next function starts at `0x801B00D4` (`addiu sp,sp,-0x78`, ELF
  `0x540`). `func_801AFC2C`'s ELF size covers through `0x801B00CC` only, which is
  why its delay slot needs the `0x4A8` override. Session 41 had trimmed it to
  `0x4A4` to stop `jal 0x801B00D0` (scene `0x0D`'s leave) being redirected into
  it; that call is now **dispatched at run time** (Makefile `--only
  0x801B00D0`), which rewrites the binding to `LOOKUP_FUNC`, so extending it
  again is safe. `make recomp` still reports `dispatched 0x801B00D0 (2 site(s))`.

The two calls that actually leaked on the credits path are **`func_801AB998`'s
`jal 0x801AB770`** and **`func_801B1BBC`'s `jal 0x801B00D4`**; both are now
emitted as call-and-continue (`func_801AB770(rdram, ctx); goto after_2;` /
`func_801B00D4(rdram, ctx); goto after_1;`), and `static_16_801AB748` no longer
exists.

## 3. How they were found (the two shapes to grep for)

Neither needs a run — both are pure generated-C/ELF checks. They are worth
keeping for the next wall:

1. **Leak-shaped `jal`** — a `jal 0xT` whose emitted call is followed by
   `recomp_trace_return(...); return;` *and* a duplicated delay slot, inside a
   function that has more code after it. That is N64Recomp's emission for a
   `jal` into a size-overridden body interior (session 60 §2). Before this
   session: 5 sites; after: 3 remain (all latent, none executed on the credits
   route — see §5).
2. **Dropped `jr ra` delay slot** — `// 0xADDR: jr $ra` immediately followed by
   `recomp_trace_return(...)` with no delay-slot instruction emitted in between
   (the delay slot landed in the next symbol). That function returns with `sp`
   never restored. Before: 2 sites (`static_16_801AB748`, `func_801AFC2C`);
   after: **0**.
3. **Call-tree audit** — for each function reachable from the scene's
   enter/update/hook, compare every `// 0xSRC: jal 0xTARGET` comment with the
   emitted callee's own address (from its `recomp_trace_entry`). A mismatch is a
   redirect. On the scene-`0x11` tree (roots `func_80177F80`, `func_80177F9C`,
   `func_80177FD4`, `func_801AC944`, `func_801ACC24`, `func_801ACE00`,
   `func_801ABCF4`, `func_801AB998`, `func_801AB770`) this found exactly the
   `func_801B1BBC → func_801B00D4` redirect.

## 4. Verification

The only valid test is the natural route. The developer drove the last mission
from a **suspend save in front of the final boss** (imported as
`OGRE_SAVE=suspend_final_boss`, copied to `assets/saves/suspend_final_boss.n64`)
on the fixed build:

```
OGRE_PREF_DIR=/tmp/ogre-credits-try2 OGRE_SAVE_RESET=1 OGRE_SAVE=suspend_final_boss \
  OGRE_SPEED=8 OGRE_SCENE_LOG=1 OGRE_PROFILE=1 OGRE_COVER=/tmp/credits-try2-cover.txt \
  OGRE_DUMP_RDRAM=/tmp/credits-try2.bin ./build-app/ogrebattle64
```

Result (`/tmp/credits-try2.log`):

* scene `0x11` (descriptor `0x8018FBAC`, mask `0x8000`) is entered at
  `t=273613 ms`; **display lists keep coming** (`30250 … 34400+`, both the
  F3DEX2 list `0x8009F540` and the S2DEX2/njpeg list `0x800A5110`), and
  `D_800AEFA4` **tracks** `D_800C4BCC` (`last=131988` vs `retrace=131988`) — the
  session-60 freeze signature is gone;
* the credits functions really run: coverage has `func_801ABCF4`,
  `func_801AB998`, `func_801AB770`, `func_801AB568`, `func_801ACE00`,
  **`func_801B1BBC`** and **`func_801B00D4`** (the two whose binding was fixed);
* the credits complete into **scene `0x13`** (descriptor `0x8018FBD4`, the Chaos
  Frame total) at `t=328073 ms`, then the attract loop (`0x09`/`0x0A`);
* **0** `UNKNOWN module` / `streamed function stub called` / `UNCOMPILED
  streamed record`, and 0 crashes during play; 1763/8192 functions entered.

Build invariants after the change: `make bank-recomp` → `check-banks OK` (no bank
unit calls into a swappable range it does not own); `make cross-bank-check` → OK;
`build-app` **and** `build-null` both rebuilt.

**Probes:** none. No generated C, runtime or app code was instrumented this
session (the two shape greps and the call-tree audit are throwaway Python over
the generated tree; nothing was added to `tools/`). `git status --short` shows
only `config.toml` + the record files.

## 5. What is left open (not this wall)

* **Credits background artifacts** (developer): the scrolling credits draw, but
  some of the njpeg/S2DEX2 credit backgrounds have artifacts. Untouched here.
* **Three latent leak-shaped `jal`s remain**, none of which executed on this
  route: `func_8017BA60 → 0x8019D67C`, and `func_80079BD8 → 0x801AB740` /
  `0x801AB76C` (coverage 0 for all three in the natural run). The
  `func_80079BD8` pair is the interesting one: on hardware it calls **unit A**'s
  `func_ovlA_801AB740` (a 4-byte big-endian reader, ELF entry), but
  `tools/cross_bank.py`'s `entry_looks_real` refuses 0x801AB740 as "not a
  function prologue" because its first instruction is `lbu` — a false negative in
  the leaf-function heuristic. With `func_801AB568 = 0x208` those calls now
  resolve to `func_801AB568`, i.e. the epilogue, so that path still leaks. Fix it
  by seeding unit A's `0x801AB740`/`0x801AB76C` (or widening `entry_looks_real`),
  **not** by editing generated C.
* The **teardown crash** `func_8007F8E4 + 0x2F8` on window close (session 85 §4)
  is still there and still reproducible in seconds.

## 6. Files changed

* `config.toml` — the four `function_sizes` entries above, each with a comment
  giving the instruction-level reason (this is the whole code change).
* `docs/HANDOFF-2026-09-19-session86.md` (this file), `PLAN.md`,
  `DECISIONS.md`, `docs/README.md`, `docs/scenes.md`.

`assets/saves/suspend_final_boss.n64` (the developer's in-game suspend save in
front of the final boss) was copied out of the run's battery dir; it is under
`.gitignore`d `assets/`.
