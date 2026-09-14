# Handoff — 2026-09-14, session 39: New Game plays into scene 0x0D (two false merges fixed, one recompiler bug)

## Goal and result

Continue session 38's work: the scene shown on New Game (`OGRE_SCENE=new-game`
→ scene `0x02` → scene `0x0D`). Session 38 left two walls: menu `0x18`'s
unit-D crash (gate to the natural path) and the forced-`0x0D` residue crash.

This session fixed the menu crash, then ran the **natural** path
(title → Start → `0x02` → `0x0D`) and fixed what it hit next. The game now
plays **title → `0x02` → `0x0D` → `0x02` → `0x0D`** (scene transitions =
live game logic, 200k-vertex frames, **zero stub calls**) before stopping at
a NEW wall: a NULL pointer into the float-convert helper `func_800988A0`,
via `func_ovlC_80239C24` after a deep `static_35_80239A38`↔`static_35_80239BA0`
traversal. Three real bugs were fixed along the way (two false function
merges, one latent recompiler bug); the forced-jump residue walls from
session 38 stand unchanged in kind.

## 1. Menu `0x18` crash: a false merge, not unit-D seeds (fixed, verified)

Session 38 pointed at "unit-D seeds" for the forced-menu crash
(`8008AFE0 → 80072398 → 8017BB28 → 8019C5D4`, fault N64 `0xFC`). It isn't a
bank problem at all — every frame is resident overlay A/B/C code, and record
1's load just happens to precede it.

Root cause: `func_8017BB28` (overlay B) does `jal 0x8019C69C`, but
`config.toml` extended `func_8019C5D4` to `0x5D8`, swallowing `0x8019C69C`.
N64Recomp's size-override redirect (`find_containing_size_override`) sent the
call into the head, which derives `s0` from `a0` via `D_801B80E0` and faults
at `0xFC`. But `0x8019C69C` is a **genuine alternate entry** with its own
caller and a caller-registers contract (`sw $v0,0x14($s0)` first instruction)
— the July batch override (session 30's 19-symbol sweep) never checked for
callers. The only external reference into the whole merged range is that one
`jal`, so splitting is safe.

Fix: override removed (comment in `config.toml` explains why). The head now
reaches the shared tail through three static tail-calls and the direct `jal`
lands on the real entry (recomp log confirms the shape; call site verified in
`RecompiledFuncs/funcs_4.c`).

Effect: forced menu now runs the REAL tail (`8019C69C` in the crash chain)
and faults at N64 `0x14` (`s0=0`, probe-verified once: `a0=0 s0=0 s2=0`) —
right code, missing state. Scene `0x18`'s descriptor names `0x8017BB28` as its
update (`0x8018FDC0: [8017BA60, 8017BB28, 8017BB54, 0]`), called via
`80072398`'s descriptor+4 `jalr` with stale `a0`/`s0` on the forced path.
Forced-menu is a residue artifact (session-38 theme), not a port bug; the
natural path never touches it (taps go title → `0x02` directly).

## 2. Scene `0x0D` list-unlink crash: the same bug class in 14b (fixed, verified)

The natural path (taps) reached `0x0D` and died deterministically in
`func_80071950+742` (`sw`, guest `0x800028B0` — i.e. MIPS store to N64
`0x28B0`; the `0x800028B0` offset is `recomp_mem_addr` wraparound of a
near-NULL address, same class as session 37's N64-addr-3 read). A filtered
entry probe caught exactly one bad call in the run:

```
[probe-71950] BADLINK n=3186 a0=801a4f5c +8=000028b0 +10=d61d8cf9 +C=0
```

Host backtrace (via `backtrace_symbols_fd` in the probe — the shadow chain
needs `OGRE_TRACE_HOOKS` and `$ra` isn't emulated as a value, so host stacks
are the reliable caller ID):

```
80071950 ← 80070F30 (malloc) ← ovlC_8023BB68 ← ovlC_8023BAA4 ← ovlC_8023BDFC
  ← ovlC_8023BF50 ← 802282D8 ← 80227030 ← 8022D1CC ← 80225A3C ← 800765D8 ← 80072398 ← 8008AFE0
```

— session 37's exact chain (`800712C4+450, a0=s1-0x20` frames included),
now deterministic (same guest address every run, not varying).

Root cause: `func_ovlC_8023BDA8`'s `0x54` override (session 37, to silence an
"undeclared label" build error) merged the malloc-tail head `BDA8` with the
shared-epilogue shim `BDC8` (`addu a0,s1; addu v0,s0; restores; jr ra`). Two
genuine entries were redirected into the head:

- `func_ovlC_8023BC3C`'s `j .LovlC_8023BDD0` (0x8023BD80) — an epilogue jump —
  re-ran malloc/memcpy/`800712C4` with stale `s3/s1/s0`, manufacturing the
  garbage node (`+8=0x28B0, +10=0xD61D8CF9`). This is the crash.
- `jal func_ovlC_8023BDC8` from `0x80229138` (14a code) — a genuine shim call
  that ran the malloc head instead of the two-move epilogue (silent
  misbehavior on every call, crash or not).

Fix: override removed (comment in `config-bankC.toml`). Verified in asm that
`BDA8` (0x20, ends in `jal`) + `BDC8` (0x34, epilogue) is the true layout,
with `BDA8`'s own `jal` callers (3 sites) and `BDC8`'s own `jal` caller
preserved. Recomp shape is exactly right: `Fall-through in BDA8 into BDC8`,
`Tail call ... to static 0x8023BDD0` for the epilogue jump.

## 3. Latent N64Recomp bug: trailing `jal` + fall-through drops its label (fixed)

Splitting `BDA8` left it ending in `jal 0x800712C4` with fall-through into
`BDC8`. The recompiler emitted the call-and-continue `goto after_2` but no
`after_2:` (no following instruction anchors it) → `use of undeclared label`
build break in `BankCFuncs/funcs_4.c`. This combination (link-branch as a
body's last instruction + fall-through tail call) could never occur while
every such site sat inside a size override.

Fix (`tools/N64Recomp/src/recompilation.cpp`, fall-through emitter): plant the
pending `after_N` label before the fall-through tail call when
`needs_link_branch` is set. Result:

```c
LOOKUP_FUNC(0x800712C4)(rdram, ctx);
    goto after_2;
after_2:
func_ovlC_8023BDC8(rdram, ctx);
return;
```

(The delay slot at the fall-through address is benignly single-executed by
the callee; on hardware it runs once as delay slot and the callee re-runs it
— here an idempotent `addu`.)

Patch hygiene: `tools/N64Recomp` is a gitignored upstream checkout;
`n64recomp-ob64.patch` is the committed record. Regenerated it wholesale via
`git diff HEAD`, then proved fidelity: fresh local clone at `ffb39cd` +
regenerated patch reproduces the working tree byte-for-byte (`src/`,
`include/`, `LiveRecomp/`), and old-patch vs new-patch trees differ by only
the 9-line hunk. Recompiler binary rebuilt (`tools/N64Recomp/build/N64Recomp`).

## 4. Where the natural path stops now (next wall, characterized)

```
0x02 → 0x0D → 0x02 → 0x0D → SIGSEGV, N64 0x0, t4 last func 0x800988A0
```

62-deep shadow chain (hooks on):
`80072398 → 800765D8 → ovlC_80226110 → ovlC_8022D1CC → ovlC_802399AC →
static_35_80239BA0 ⇄ static_35_80239A38` (mutual recursion, terminates) `→
func_ovlC_80239C24 → func_800988A0` (fault).

`func_800988A0` is a float→fixed converter (`lwc1` pairs from `a0`, packed
words to `a1`); `a0` or `a1` is NULL. The traversal above it
(`802399AC` region, with the `80239D68 0x470` override nearby) is the next
audit target — same false-merge pattern is the prime suspect (the override
comment itself notes a fragment + real entry `8023A1D8` adjacency), followed
by missing data (session 38 §4: `bankRec10b` never loads; the game DMAs ROM
`0x22A250` to that RAM instead) and uninitialized arena state. RDRAM dump of
the crash is the starting point (`a0`/`a1` at the `800988A0` call name the
NULL).

Note: session 38's forced-`0x0D` decompressor wall (flag `0x8019F794 == 0`)
still stands for forced jumps (battery: same `8007A7E0`/N64-0 chain), and the
flag is *still 0* on the natural path too — the natural run sails past the
decompressor anyway (different branches taken with natural residue:
`0x800E9BE0 = 0x800A81C0` vs forced `0`). The flag theory needs revisiting
when the 800988A0 wall falls.

## Verification (final binaries, probes reverted via `make recomp`)

- `make recomp` (pristine regen, 3 dispatches re-applied) + `make bank-recomp`
  + both `build-null`/`build-app` rebuild clean.
- Battery, build-null: natural boot 20 s exit 0; forced
  title/intro/publishers/story/unit-info all activate, exit 0, **zero stub
  calls everywhere**; build-app forced title exit 0.
- Forced new-game: same deterministic session-38 §2 wall
  (`8007A7E0`, N64 `0x0`) — unchanged, still residue.
- Forced menu: `8019C69C` reached (was `8019C5D4`), fault N64 `0x14`
  (`s0=0`) — right code, forced-path-only missing state.
- Natural tap run: `0x02 → 0x0D → 0x02 → 0x0D`, **0 stubs**, stops at the §4
  wall (`800988A0`, N64 `0x0`). Deterministic across runs.

No repo test harness exists (sessions 33/37); per the durable-test skill this
is disclosed rather than fixed with a new framework: each "test" is a
ROM-dependent 12–180 s game run, so the repro commands below are the
maintained verification. No existing tests to regress (none in repo).

## Files changed (vs session 38)

- `config.toml` — removed `func_8019C5D4 0x5D8` merge + why-not comment.
- `config-bankC.toml` — removed `func_ovlC_8023BDA8 0x54` merge + why-not comment.
- `tools/N64Recomp/src/recompilation.cpp` — fall-through trailing-link label fix.
- `n64recomp-ob64.patch` — regenerated (old + the 9-line hunk, proven equivalent).
- `tools/N64Recomp/build/N64Recomp` — rebuilt binary (gitignored).
- `RecompiledFuncs/`, `BankCFuncs/`, `app/src/bank_funcs.inc` — regenerated
  (all gitignored/generated except `bank_funcs.inc`, which regenerated
  identically).
- `PLAN.md` — untouched (same convention as session 38; status section is now
  two sessions stale — worth a refresh pass).

Probes used (all reverted, none committed): entry-register probe on
`func_8019C69C`, wild-node/link probe with host backtrace on `func_80071950`.
Technique worth reusing: `$ra` is NOT emulated (direct C calls), so
`backtrace_symbols_fd` in a generated-code probe is the reliable caller ID;
`recomp_mem_addr` wraparound means a host fault `rdram+0x8000XXXX` reads back
as MIPS near-NULL (`offset − 0x80000000`).

## What's next (for session 40)

1. **The §4 wall**: `func_ovlC_80239C24 → func_800988A0` NULL (`a0`/`a1`?).
   Audit the `80239D68 0x470` region splits first (same false-merge smell),
   then missing-data (`bankRec10b`, arena init).
2. Revisit session 38's flag theory (`0x8019F794` still 0 naturally).
3. Menu `0x18` natural entry (taps skip it: title → `0x02` directly) — needed
   for the Tutorial path and the `0x8019C69C`-with-valid-`s0` case.
4. Still open, untouched: `OGRE_NO_AUDIO=1` early-boot crash, `osViFade`.
5. `PLAN.md` status refresh (sessions 38–39 unrecorded).

## Repro commands

```sh
# the money run: title -> 0x02 -> 0x0D -> 0x02 -> 0x0D, 0 stubs, stops at 800988A0/N64-0
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 OGRE_EXIT_AFTER_MS=180000 ./build-null/ogrebattle64
# forced walls (residue, unchanged in kind): decompressor via 8007A7E0 ...
OGRE_SPEED=4 OGRE_SCENE=new-game OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 OGRE_EXIT_AFTER_MS=90000 ./build-null/ogrebattle64
# ... and menu tail via 8019C69C (was 8019C5D4), N64 0x14
OGRE_SPEED=4 OGRE_SCENE=menu OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64
```
