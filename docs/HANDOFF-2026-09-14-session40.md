# Handoff — 2026-09-14, session 40: the 0x0D re-entry crash is harness-driven retail behavior, not a port bug (+ `OGRE_TAP_MAX`)

## Goal and result

Session 39 left one wall on the New Game path: natural title → `0x02` →
`0x0D` → `0x02` → `0x0D`, then SIGSEGV in `func_800988A0` (N64 `0x0`) via
`func_ovlC_80239C24` after the `static_35_80239A38`↔`static_35_80239BA0`
traversal, with the suspect `func_ovlC_80239D68 0x470` merge and missing
`bankRec10b` state as leads.

Both leads are eliminated, and the crash is resolved as **not a port bug**:
the "mutual recursion" is a bounded 28-iteration loop (`s5 < 0x1C`) compiled
correctly; the NULL comes from a shared-frame slot that is virgin-zero if and
only if scene `0x0D` is (re-)entered without the stack history that primes
it. Two independent triggers of that re-entry were proven in-session — a tap
pressing Start mid-`0x0D` (aborts to `0x02`, instant re-entry, crash ≤0.5 s
later) and a **tap-free** ~29 s idle timeout (same return, same crash). Visit
1 runs 11–29 s clean with zero stubs. The port executes the identical
instruction stream on identical zeroed memory, so hardware does the same;
there is no port layer to fix. Added `OGRE_TAP_MAX` so runs can go silent
after leaving the title. No recompiler/config/bank changes this session.

## 1. The NULL, precisely (probes, all reverted)

`800988A0` (float→fixed vertex converter: `lwc1` pairs from `a0`, packed
words to `a1`) entry probe, fires once per run:

```
a0=0x800C2438 (valid: sp+0x190 struct)  a1=0x00000000 (NULL dest)
s5=0x1C (all 28 loop iterations ran), sp=0x800C22A8
```

`a1` is `*(sp+0x1EC)` (struct+0x5C), loaded at `0x80239C1C`. Writer audit over
the whole reachable code: **nobody on the taken path writes it**.
`func_ovlC_80090940` (the struct filler) stores `+0x0…+0x3C` only; the sole
`+0x5C` writer in the game is `func_ovlC_80239874` (`sw $a2`, path A), which
the crashing path bypasses — the crash path is the direct `jal 802399AC`
from `0x8022D218` (path B, taken when `*(D_8018FC39)==2`, itself legitimately
produced from `*(D_8018F1C0)=0x8002` with bit 15 set). Path B's entry `a2`
(the buffer on the crash frame: `0x80196F80`) is clobbered as scratch
(`0x80239A00/0x80239A20`) and never stored. So path B can only work by
reading a leftover buffer pointer at `sp+0x1EC` — a same-sp path-A frame
having primed it. Visit 1: primed (`0x800C1EC0`, stable 200+ frames).
Re-entry visit: different stack depth (`sp 0x800C22A8` vs `0x800C1BF0`),
virgin zero → NULL → crash. Dump forensics agree (struct fully populated by
`80090940`, slots `+0x40…` zero; path-A signature slots `sp+0x170…` zero —
path A never ran at the crash sp).

## 2. The re-entry is real game flow, twice triggered (the key experiment)

Battery log correlation (wall ms vs scene-t ms, same clock):

- 3 s taps: tap4@12000 → `0x02`@12392 → `0x0D`@12521 (visit 1, healthy ~3 s);
  tap5@15000 → `0x02`@15506 (Start **aborts** `0x0D`) → `0x0D`@15647 →
  crash (visit 2, ≤0.5 s of init).
- `OGRE_TAP_MAX=4` (silent after tap4): visit 1 runs **29 s clean**, then
  `0x0D → 0x02 → 0x0D` **with no tap anywhere near it** → same N64-0 crash.
- `TAP_MS=11000` window: 11 s uninterrupted visit 1, tap → abort → re-entry →
  same crash.

So Start-during-`0x0D` is a back/cancel to `0x02`, and a ~29 s idle timeout
does the same; the crash follows EVERY re-entry and NO uninterrupted visit.
The table (`D_8022A994`, malloc'd fresh per frame in `8022D1CC`, all 28
`+0x18` NULL) is empty in all cases — the loop always skips to the tail call,
which is why the slot (not the table) is load-bearing. `80239D68+0x470`
is not on the crash path (chain goes `802399AC → statics → 80239C24`) and
`bankRec10b` is correctly absent (asset DMA covers that RAM by design,
session 38 §4) — both leads closed.

Retail-identity note: thread stacks live in game-cleared BSS and the table
block is allocator-fresh, so the crash words are zero on hardware for the
same structural reasons (the port's `calloc`-zeroed RDRAM vs power-on garbage
does not account for THESE words). A hw player mashing Start through loading
would execute the identical stream. Whether hw Start-during-load is even
deliverable is untestable here; either way no port fix exists at this layer.
Do NOT paper over it with a fabricated buffer.

## 3. `OGRE_TAP_MAX` (implemented, verified)

`app/src/sdl_platform.cpp`: stop tapping after tap number n
(`OGRE_TAP_MAX`, tap 0 = boot window; 0/unset = forever). Lets a run press
Start through the title then go silent. Verified: `TAP_MS=3000 TAP_MAX=4`
shows taps 1–4 then silence; visit 1 runs 29 s; the only crash is the
timeout re-entry (§2). This is the tool for next session's no-abort `0x0D`
exploration.

## Verification (final binaries, all probes reverted via regen)

- `make recomp` + `make bank-recomp` clean (RC 0); `build-null` + `build-app`
  rebuild clean; probe grep across touched generated files = 0.
- Battery, build-null: natural boot 20 s exit 0; forced
  title/intro/publishers/story/unit-info all activate, exit 0, **0 stubs**
  (unit-info and menu forces each missed once ~1.1 s poke race, retry stuck —
  known flakiness, note below); forced new-game = session-38 decompressor
  wall (`8007A7E0`, N64 0); forced menu = session-39 tail wall
  (`8017BB28 → 8019C69C`, N64 `0x14`); build-app forced title exit 0.
- Re-entry crash reproduced on demand twice (§2) with identical signature
  (N64 `0x0`, last func `0x800988A0`, 0 stubs).

No repo test harness exists; per the durable-test skill this is disclosed,
not fixed with a framework: each check is a ROM-dependent 12–120 s game run;
repro commands below are the maintained verification.

## Files changed (vs session 39)

- `app/src/sdl_platform.cpp` — `OGRE_TAP_MAX` (only tracked edit).
- `docs/HANDOFF-2026-09-14-session40.md` — this file.
- Regenerated (identical except probe removal): `RecompiledFuncs/`,
  `BankCFuncs/` (gitignored). Probes used, all reverted: `800988A0` entry
  (`a0/a1` + ring-buffered `802399AC{a2,sp,slot}`), `802399AC` entry
  (args/slot, shared-frame math), `80239874` entry (path-A args).
- `PLAN.md` — untouched (now three sessions stale; refresh overdue).

## What's next (session 41)

1. **Drive `0x0D` without aborting it**: `TAP_MS=3000 TAP_MAX=<through-title>`
   then keyboard (X = A per `keyboard_buttons`) or an `OGRE_TAP_BUTTON`
   extension to give it non-Start input. `0x0D` may be an interactive screen
   (name entry/menu) waiting for A, not more Start. Watch: does the table
   fill? does the selector leave 2? does init complete?
2. If `0x0D` needs inputs the harness can't give, map what it waits on
   (`D_8018F1C0`/`D_8018FC39` producers, record-10 list population).
3. Force-flakiness: 2 misses in ~10 forces today (poke race at boot); consider
   a retry/confirm loop in `poll_scene` if it worsens.
4. Still open, untouched: `OGRE_NO_AUDIO=1` boot crash, `osViFade`,
   `PLAN.md` refresh, menu-`0x18` natural entry (Tutorial path).

## Repro commands

```sh
# 29 s clean visit 1, timeout re-entry, same N64-0 crash (final binaries)
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=120000 ./build-null/ogrebattle64
# tap-abort variant: tap5 kills visit 1, instant re-entry crashes
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 OGRE_EXIT_AFTER_MS=180000 ./build-null/ogrebattle64
```
