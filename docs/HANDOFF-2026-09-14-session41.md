# Handoff — 2026-09-14, session 41: `0x0D` is a fixed cutscene, `OGRE_TAP_BUTTON`, and what the re-entry crash actually is

## Goal and result

Continue session 40's New Game work: drive scene `0x0D` with non-Start input
without aborting it, and find out whether it is interactive.

Result:

1. **`0x0D` is a fixed ~28.7 s cutscene, not an input screen.** Every non-Start
   button (A, B, Z, D-pad, C-down) was tapped through the visit: the scene
   always ends and returns to `0x02` at 28.3–28.7 s, unchanged. The
   `0x0D` → `0x02` → `0x0D` cycle is the game's **own scripted loop**, not an
   abort and not an idle timeout (`0x02`'s enter hardcodes next = `0x0D`;
   `0x0D`'s leave — `func_80178B7C` — hardcodes next = `0x02`; the per-frame
   update picks `0x02` when `func_801C8884() == 2`).
2. **Added `OGRE_TAP_BUTTON`** — a per-press button schedule so a scripted run
   can press Start through the title and then A/B/Z/D-pad with no human. This is
   the tool session 40 asked for; it is what proved (1).
3. **Re-characterised the re-entry crash** (it is *not* what session 40
   concluded). `func_ovlC_802399AC` is not a standalone function: it is the
   **fall-through continuation of `func_ovlC_80239874`**, proven at instruction
   level from `build/bankC.elf`, and its prologue is what stores the buffer at
   `sp+0x1EC`. The `jal 0x802399AC` at `0x8022D218` enters mid-function with a
   24-byte frame, so the slot is never written and `func_800988A0` gets a NULL
   destination. That call is only reached when the mode byte `D_8018FC39 == 2`;
   forcing it to 0 moves the crash (N64 `-8` in `func_ovlC_8023C894`), so the
   re-entry is missing more than that one branch.
4. **Two genuine mis-binding bugs found and fixed**: the `func_801AFC2C` size
   override ran 4 bytes past the start of `func_801B00D0` (so the recompiler
   redirected every `jal 0x801B00D0` into the wrong function), and the two
   `jal 0x801B00D0` sites are now dispatched to bank unit C's real
   `func_ovlC_801B00D0`.

No fix for the re-entry wall yet. All probes reverted.

## 1. `0x0D` is a fixed cutscene (the button sweep)

`OGRE_TAP_MS=3000 OGRE_TAP_BUTTON="start,start,start,start,start,<btn>"`, 50 s,
`build-null`, scene log `0x0D` entered at ~12.5 s:

| btn  | `0x0D` in | `0x02` again | visit length |
|------|-----------|--------------|--------------|
| down | 12543 ms  | 40959 ms     | 28.42 s      |
| up   | 12535 ms  | 40989 ms     | 28.45 s      |
| b    | 12501 ms  | 41255 ms     | 28.75 s      |
| left | 12717 ms  | 41597 ms     | 28.88 s      |
| right| 12676 ms  | 41351 ms     | 28.68 s      |
| cd   | 12535 ms  | 41563 ms     | 29.03 s      |
| z    | (same)    | (same)       | ~28.7 s      |
| a    | 12526 ms  | 41063 ms     | 28.54 s      |

The first visit is healthy (zero stub calls, 200k-vertex frames); the return is
always the same. The selector byte `D_8018FC39` does **not** leave 2, the
object table `D_8022A994` does **not** fill, and no input changes any of it, so
there is no interactive state to drive. Session 40's "Start aborts `0x0D`" is
the same scripted return: Start during the visit is just the same leave path.

The loop's structure (from the disassembly):

- `func_80178920` (scene `0x02` enter): `jal func_80198D28` (record load),
  `D_8018FC38 = 0`, `D_800C4C26 = 0x800D`.
- `func_80178954` (scene `0x0D` per-frame update): if `func_8022770C()` (=
  `D_8022A9A0`) `!= 0xFF`, do nothing; else read `func_801C8884()` and set
  `D_800C4C26 = 0x8006` (state 6), `= 0x8002` (state 2), call `func_80178CB0`
  (state 5) or `func_80226FA8` (anything else). The ~28.7 s visit is state 2.
- `func_80178B7C` (scene `0x0D` leave): if the block's scene id is still
  `0x0D`, rewrite it to `0x02`, then the scene manager re-enters `0x0D`.

## 2. `OGRE_TAP_BUTTON` (implemented, verified)

`app/src/sdl_platform.cpp`: a comma-separated schedule, one entry per synthetic
press, advancing once per `OGRE_TAP_MS` interval and **holding the last entry**
for the rest of the run. An entry is a button name (`a`, `b`, `z`, `start`,
`l`, `r`, `up`, `down`, `left`, `right`, `cu`, `cd`, `cl`, `cr`) or several
joined by `+`; `none` is a silent slot. Default is a single `start`, so an
unset env behaves exactly as before, and `OGRE_TAP_MAX` still caps total taps.
The parsed schedule is logged once at startup and each press logs its buttons:

```
[SDL] tap schedule (6 slots, last sticks): start start start start start a
[SDL] automation tap 5 at 15001ms buttons=a
```

Verified: `start,start,start,start,start,a` gives Start for taps 1–4 (which is
what reaches `0x02`/`0x0D`) and A from tap 5 on.

## 3. What the re-entry crash actually is (revises session 40)

Session 40 concluded the crash was "harness-driven retail behavior" and that
`func_ovlC_802399AC` was a real function called with a different stack. The
instruction-level facts in `build/bankC.elf` say otherwise:

- `0x80239874 func_ovlC_80239874` prologue: `addiu sp,sp,-0x238`, saves
  `ra/fp/s0..s7/f20..f26`, then `sw $a2, 0x1EC($sp)` (`0x802398F0`) — this is
  the **only** writer of the buffer slot.
- Its body runs to `0x802399A8` (`swc1`) and **falls through** into `0x802399AC`
  with no `jr ra` in between. `0x802399AC` has no prologue: it is the shared
  continuation (loop over the 28-entry table at `D_8022A994`, then
  `0x80239BA0` → `func_ovlC_80239C24`, which reads `sp+0x1EC` at `0x80239C1C`
  and calls `func_800988A0`).
- `func_ovlC_8022D1CC` (record 14a) does `jal 0x802399AC` at `0x8022D218`
  (a real `R_MIPS_26` reloc to `func_ovlC_802399AC`) — but its own frame is
  only `addiu sp,sp,-0x18`. Entering the continuation with that frame means
  `sp+0x1EC` is `0x1EC` above `func_ovlC_8022D1CC`'s frame (inside its
  *caller's* frame) and the epilogue at `0x80239C88` restores from
  `sp+0x214`, `sp+0x210`, … — also the caller's frame. So this branch is only
  "survivable" by coincidence of the surrounding stack.

Probes (temporary, reverted) confirm the two visits differ only in **which
branch `func_ovlC_8022D1CC` takes**, and that in turn differs only in the
*input word*:

```
visit 1: func_80178568 enter, input word = 1  -> func_80226FA8 -> func_8022683C(-5,1)
         -> func_80227E64(1) = -7 -> selector = 0  -> 8022D1CC takes 8023A9AC (safe)
         then 80239874 runs 3210x, each falling into 802399AC with sp+0x1EC primed
visit 2: func_80178568 enter, input word = 2  -> func_8022683C(-5,2)
         -> func_80227E64(2) = -3 -> selector = 2  -> 8022D1CC jal 802399AC directly
         -> sp+0x1EC = 0 -> func_800988A0(a1 = 0) -> SIGSEGV
```

The input word is the game's own `D_8018F1C0` (`OSContPad.button`-shaped; the
scene enter reads `RELOC(12, 0x24240)` = `0x8018F1C0`). It is **1 on the first
`0x0D` visit and 2 on the second**, i.e. it tracks the transition count, and no
recompiled function writes those values:

- `func_80171BB4` is the only non-zero writer in the main unit
  (`sh 0x8000` when `D_80197B30 & 8`, a scripted A press).
- `BankCFuncs/funcs_14.c` and `RecompiledFuncs/funcs_10.c` only clear it to 0.
- `BankEFuncs/funcs_0.c` (`func_ovlE_80198D28`) only reads it.

So the writer is still unidentified (a DMA, the runtime relocator, or a writer
that reaches the address through a runtime pointer are the candidates; an
`OGRE_WATCH_F1C0` per-frame watcher and an lldb hardware watchpoint were tried
and were inconclusive — the watchpoint's evaluated address was wrong because
`ultramodern_get_rdram_base()` is not valid at that breakpoint).

Two diagnostics bound the problem:

- Forcing the selector read in `func_ovlC_8022D1CC` to 0 does **not** fix the
  re-entry: the crash moves to N64 `-8` in `func_ovlC_8023C894` (last func on
  thread 4). So the broken selector branch is *a* crash site, not the only one.
- Redirecting the `jal 0x802399AC` to the real entry `func_ovlC_80239874` also
  crashes (N64 `-8`), so 80239874's own register expectations (it takes
  `a0/a1/a2` and a full frame) are not satisfied when called from
  `func_ovlC_8022D1CC` either.

Both diagnostics say the re-entry reaches code whose input state is wrong. The
next step is to find the writer of the `0x8018F1C0` word and decide whether the
loop is even reachable on hardware with the values the port produces.

## 4. Two genuine mis-binding bugs (fixed, committed)

Both were found from the recompiler's own `[Info] Jal … redirected into …`
messages while regenerating with a trimmed override.

1. **`func_801AFC2C`'s override ran 4 bytes too long.** `config.toml` had
   `size = 0x4A8`; the ELF symbol is `0x4A4` (`0x801AFC2C..0x801B00D0`).
   `find_containing_size_override` uses strict containment, so
   `0x801B00D0` (the next function's prologue) fell *inside* the override and
   the recompiler redirected `jal 0x801B00D0` at `0x80178BB8` (scene `0x0D`'s
   leave, `func_80178B7C`) into `func_801AFC2C`. Trimmed to `0x4A4` with a
   comment. (Note `func_801AFAF4`'s `0x5E0` override ends at the same
   `0x801B00D4` and absorbs the redirect next; it is not trimmed here because
   it exists to keep that snapshot's flow compiling, and the dispatch below
   supersedes it.)
2. **`jal 0x801B00D0` was bound to the main unit's 4-byte placeholder symbol**
   (its `.streamedC` snapshot only has a stub there). Bank unit C's record 10
   provides the real entry `func_ovlC_801B00D0`, which *is* registered in
   `app/src/bank_funcs.inc`, so both call sites (`func_801779FC` @`0x801779FC`
   and `func_80178B7C` @`0x80178BB8`) are now dispatched:
   `cross_bank.py dispatch --only 0x80198D28,0x801AFC2C,0x801980A0,0x801B00D0`
   in the Makefile (both sites were the redirect/early-return shape and both got
   the tail repair). The crash is unchanged by this (the teardown does not
   reset the selector byte), but the teardown now runs the real code.

## Verification (final binaries, all probes reverted via regen)

- `make recomp` + `make bank-recomp` clean (RC 0); `build-null` + `build-app`
  rebuild clean; `grep probe-` across `RecompiledFuncs/` + `BankCFuncs/` = 0.
- Battery, build-null: natural boot 20 s exit 0; forced
  title/intro/publishers/story/unit-info activate, exit 0, 0 stubs; forced
  new-game = session-38 decompressor wall (`8007A7E0`, N64 0); forced menu =
  session-39 tail wall (`8017BB28 → 8019C69C`, N64 `0x14`); natural tap run =
  title → `0x02` → `0x0D` → `0x02` → `0x0D`, 0 stubs, stops at the
  `800988A0`/N64-0 wall; build-app forced title exit 0.
- `OGRE_TAP_BUTTON` schedule verified by log (schedule line + per-press
  `buttons=`).

No repo test harness exists; per the durable-test skill this is disclosed, not
fixed with a framework: each check is a ROM-dependent 12–120 s game run, so the
repro commands below are the maintained verification.

## Files changed (vs session 40)

- `app/src/sdl_platform.cpp` / `.hpp` — `OGRE_TAP_BUTTON` schedule.
- `config.toml` — `func_801AFC2C` size `0x4A8` → `0x4A4` + why-not comment.
- `Makefile` — added `0x801B00D0` to the `recomp` dispatch `--only` list, with a
  comment explaining the redirect and the bank-C entry.
- `PLAN.md` — status entries for sessions 39–41 (was stale at 38).
- `docs/HANDOFF-2026-09-14-session41.md` — this file.
- Regenerated (gitignored): `RecompiledFuncs/`, `BankCFuncs/`,
  `app/src/bank_funcs.inc` (1721 functions, 4 units), `build-null`,
  `build-app`. Probes used, all reverted: `func_80178568`/`func_800988A0`
  entries, `func_ovlC_80239874`/`802399AC`/`8022D1CC`/`80227700`/`8022683C`/
  `80226FA8` entries, `RecompiledFuncs/funcs_6.c` call sites, `input.cpp`
  `osContGetReadData`, and an `OGRE_WATCH_F1C0` watcher in `bank_overlays.cpp`.

## What's next (session 42)

1. **Find the writer of `0x8018F1C0`.** The two diagnostics in §3 both point at
   wrong input state on re-entry. It is 1 then 2 across the two `0x0D` visits;
   no recompiled function writes those (only `0x8000` and `0`). Next moves:
   watch it with a *correct* host address (print `ultramodern_get_rdram_base()`
   from the app, then set the lldb watchpoint on that value, not on an lldb
   expression evaluated at a bad stop), or add a temporary store probe to the
   runtime's `relocate_section`/`do_rom_read` to see whether the streamed
   overlay load or the relocator writes it.
2. **Decide what the correct `D_8018F1C0` value is** at scene entry and whether
   `func_80227E64(D_8018F1C0 & 0xFFF)` should ever produce the selector-2
   branch. `func_8022683C(-5, n)` maps `n=1 → selector 0`, `n=2 → selector 2`
   through `func_80227E64` (a table at asset `0x19A8804`).
3. **The re-entry wall generally.** Forcing selector 0 moves the crash to
   `func_ovlC_8023C894` (N64 `-8`), so the second `0x0D` entry is missing state
   beyond the selector. Candidate: the `0x02` loader's `func_80198D28` and the
   counter it advances; compare the record it loads on visit 1 vs visit 2.
4. Forced-path walls unchanged: forced new-game decompressor (`8007A7E0`),
   forced menu (`8019C69C`, N64 `0x14`); menu `0x18` natural entry (Tutorial
   path) still untouched.
5. Still open, untouched: `OGRE_NO_AUDIO=1` early-boot crash, `osViFade`.

## Repro commands

```sh
# 0x0D is a fixed ~28.7 s cutscene: any button, same visit length
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_BUTTON="start,start,start,start,start,down" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=50000 ./build-null/ogrebattle64

# re-entry wall: visit 1 clean, visit 2 dies in 800988A0, N64 0
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_TAP_MAX=4 OGRE_SCENE_LOG=1 \
  OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64

# show the mis-binding is gone: both 0x801B00D0 sites are LOOKUP_FUNC
grep -rn "LOOKUP_FUNC(0x801B00D0)" RecompiledFuncs/
```
