# Handoff — 2026-09-15, session 45: the step-2 wall was a **missing streamed module** (rec14c), not the PI manager

## Goal and result

Session 44 left two things open: (1) the RT64-only `do_send` SIGBUS at the step-2
enter (guest `0xFE6E2C89`), which it suspected was a PI-manager/ROM-DMA bug, and
(2) the null build's second `0x0D` visit, which "runs the scene with no crash"
only because the new KUSEG mirror let it survive.

**Result: both are the same root cause, and it is a mis-binding, not the PI
manager.** The game streams a *second* module into record 14's arena for the
later New Game steps:

```
bankRec14c  ROM 0x2A8CF0 (0x56A0) -> RAM 0x802395E0   (one DMA, logged)
```

`0x802395E0` is the RAM `bankRec14b` (ROM `0x2AE390`, the module session 37
compiled into unit C) already occupies. The two modules are **banks of one
arena** — the game loads rec14b for scene `0x0D`'s first visit (the movie) and
rec14c over it for steps ≥ 2. The port had only rec14b, so N64Recomp bound the
95 calls from records 14/14a into `0x802395E0+` as **direct C calls to rec14b's
bodies**. rec14b's layout is different: at `0x80239C24` rec14b has a *body
interior* of the function whose prologue is `0x80239874` (no prologue, restores
`s0`-`s8`/`f20`-`f26` from the caller's frame, returns with `sp += 0x238`), while
rec14c has a real function there (`addiu sp,sp,-0x60`). The descriptor
interpreter's opcode-42 handler `jal 0x80239C24` (`0x80229004`) therefore ran
rec14b's interior with the interpreter's 0x78-byte frame:

* `sp` leaked `+0x238` (the tail's epilogue) and `s3`/`s6` were clobbered to 0,
* the interpreter then walked **guest address 0** as its step descriptor (the
  low window the session-44 KUSEG mirror had just made readable),
* opcode 6 at "pc 4" read arg `0x00010000` → `func_ovlC_8023BF50` → a table read
  at `malloc_base + 0x40000` (`0x801E4800`),
* null build: the word there is 0 → asset id 0 → no DMA (harmless by luck);
  RT64 build: `0x00010001` → asset id `0x00010001` → a **~2.5 GB ROM DMA** to
  guest `0` upwards, which overwrites `D_800AA400`/`D_800AA408` on its way past
  `0x800AA400`, and the next `func_8008BC40` send hits the garbage queue →
  `do_send` SIGBUS.

**Fix: compile rec14c (new unit G), move rec14b out of unit C into its own unit
F, and let unit C's arena calls compile as runtime lookups.** N64Recomp emits
`LOOKUP_FUNC(0xADDR)` for a `jal` whose target is not defined in the unit's ELF,
and the runtime's `load_function_bank` + `unload_overlapping_overlays` already
register/drop the right bank on the game's DMA. With that, step 2 runs: the
null build plays the 28.4 s movie visit, enters the step, and the RT64 build
renders the **cathedral dialogue with Archbishop Odiron** —
`docs/proofs/native-newgame-cathedral.png`. No crash, no stub call, on either
renderer.

## 1. How the missing module was found (instruction-level)

The chain from the session-44 RT64 crash report was:

```
func_80072398 → func_800765D8 → func_ovlC_80226110 → func_ovlC_8022D1CC
  → func_ovlC_80227030 → func_ovlC_802282D8 (descriptor interpreter)
  → func_ovlC_8023BF50 (opcode 6) → func_8009DBB8 (asset load) → … → do_send ✗
```

1. **The corrupting store, not the crash, is the fixable event.** An lldb
   hardware watchpoint on `rdram + 0xAA400` (set after `osCreatePiManager_recomp`
   returns; note `init_pi_manager` is inlined into it at `-O2`, so a breakpoint
   on the out-of-line symbol never fires) shows the PI globals being overwritten
   **byte by byte** from `do_rom_read` in the runtime's PI-request lambda —
   i.e. a ROM DMA whose destination is the RAM the globals live in. The DMA loop
   is `func_80089F80` chunking a huge size, and the crashing chunk
   (dram `0xAA400`, dev `0x64E655`, size `0x200`) is chunk 1362 of one call
   `func_80089F80(dev=0x005A4255, dram=0, size=0x9942DAD8)`.
2. **Where the bogus size comes from.** A probe on `func_80089F80`'s entry
   (`RecompiledFuncs/funcs_8.c`) over both builds shows the DMA sequences are
   **identical for 668 transfers** and then diverge: the null build reads asset
   id `0` at `base + 0x39*4`, the RT64 build reads `0x00010001`. The base is the
   `malloc`'d buffer `0x801A4800` for asset `0x016B3D18` (a 31-entry table), so
   both builds read **out of bounds** — the address that matters is
   `base + index*4`, and a probe on `func_ovlC_8023BF50` (generated
   `BankCFuncs/funcs_5.c`) shows `index = 0x00010000`, i.e. **`0x801E4800`**,
   from the step descriptor's opcode-6 argument.
3. **Why the interpreter reached an out-of-bounds table read at all.** Probes in
   the interpreter (`BankCFuncs/funcs_6.c`) show its frame start at
   `sp=0x800C2470` with `s3 = D_8022A978 = 0x801A2DE0`, `s6 = 2`, and then, after
   the opcode at `pc=82` (`desc[82] = 0x2A`), the *same invocation* at
   `sp=0x800C26A8` with **`s3 = 0`, `s6 = 0`** — a `+0x238` stack leak and a
   callee-saved clobber. `0x238` is the frame size of `func_ovlC_80239874`
   (`addiu sp,sp,-568` at `0x80239884`, `jr $ra` at `0x80239CC0`), and opcode
   42's handler is `jal 0x80239C24` at `0x80229004` — a mid-body entry.
4. **The decisive measurement: the live code is not the compiled code.** At the
   crash, RDRAM `0x80239C24` holds `27BDFFA0 AFBE0058 93BE007B …`, which is
   **ROM `0x2A9334`**, not rec14b's `0x2AE9D4` (`02002021 E7A001C8 …`). And the
   probe log has the DMA that put it there:
   `dev=0x002A8CF0 dram=0x802395E0 size=0x56A0` — a module the port had never
   compiled, loaded **after** rec14b (which is why the session-37 bring-up
   looked complete). `0x80239C24 - 0x802395E0 = 0x644`, and
   `0x2A8CF0 + 0x644 = 0x2A9334` exactly.

So the interpreter was calling a body interior of the wrong bank. Both builds
hit the same bad call; they differ only in what the out-of-bounds word holds.

## 2. The fix

* `config-bankG.yaml` / `config-bankG.toml` (**new unit G**): `bankRec14c`
  ROM `0x2A8CF0` size `0x56A0` -> RAM `0x802395E0`. It fills the ROM gap between
  `bankRec14a` (unit C) and `bankRec14b` exactly.
* `config-bankF.yaml` / `config-bankF.toml` (**new unit F**): `bankRec14b` moved
  out of unit C, with its three `function_sizes` overrides
  (`func_ovlF_8023C9F4` 0x28, `func_ovlF_80239D68` 0x470, `func_ovlF_8023E070`
  0x4B0 — sessions 37/39).
* `config-bankC.yaml`: rec14b's segment removed; the ROM gap is a `bin`.
  `config-bankC.toml`: the three rec14b overrides removed (moved to F).
* `Makefile`: `BANK_UNITS := A B C D E F G`; the bank ELF rule now tolerates a
  unit with **no** `asm/data` or `assets` objects (the `for f in build/bank$*/asm/data/*.s`
  loop passed the unmatched literal to `as`, which aborted the build).
* Nothing in `app/` or the runtime changed: `tools/gen_bank_funcs.py` globs
  `Bank*Funcs`, `bank_overlays.cpp` matches records by ROM range and calls
  `load_function_bank`, and the runtime already unloads overlapping banks.

**Static result:** unit C's arena calls now compile as lookups
(`BankCFuncs/funcs_5.c`: `LOOKUP_FUNC(0x80239C24)`, `0x80239AA4`, `0x80239B88`;
`funcs_11.c`: `LOOKUP_FUNC(0x802399AC)`) — 95 cross-module call sites in unit C
in total. `app/src/bank_funcs.inc` now has 7 units / **18 records / 1889
functions**, including `{ 0x002A8CF0, 0x802395E0, 0x56A0, 0x8023EC80, kG_… }`
alongside rec14b's `{ 0x002AE390, 0x802395E0, 0xA7E0, 0x80243DC0, kF_… }`.

**Dynamic result** (probe run, later reverted): the opcode-42 calls land in unit
G's real function — `[probe45] G_80239C24 enter a0=00000006 a1=00000020 a2=00000020`
and the same with `a0=00000007` (the two `0x2A` records in step 2's descriptor),
while the interpreter keeps `s3=801A2ED0 s6=00000002` throughout. The corrupted
`s3=0/s6=0` frame never appears.

## 3. What step 2 actually shows (proof)

`OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D` (taps stop the moment `0x0D`
begins, so the movie is not skipped), `OGRE_SPEED=4`, capture every 40th present:

* visit 1 (`0x0D` @ `t=11037 ms`) runs 28.4 s and ends at `t=39467 ms` — the
  sepia courtyard movie still renders (`docs/proofs/native-newgame-cutscene.png`
  from session 43; re-captured this session);
* visit 2 (`0x0D` @ `t=39588 ms`) is step 2 and renders the **cathedral**:
  Archbishop Odiron's dialogue box with the portrait, the winged statue, the
  candles, the characters, and the blinking "advance" quill —
  `docs/proofs/native-newgame-cathedral.png`. The line is the one the developer
  identified: *"He who has learned the way of the sword and god's teachings,"*.
  The step then waits for input (the quill), which is what an interactive
  dialogue should do.
* Before the dialogue the step also draws the narrative card *"Ischka Military
  Academy / Graduation Ceremony"* (`docs/proofs/native-newgame-academy-card.png`,
  from the tap-advancing run) — so the step is card → cathedral dialogue, not
  dialogue only.

`docs/scenes.md`'s step-2 row is updated from "crashes before drawing (RT64)" to
**renders** (null and RT64).

## 4. Correction to session 44 (AGENTS §2)

Session 44 concluded that the two low-address stores (guest `0` and `8`) are
"the game's own zeros" and that retail must therefore tolerate them, and fixed
`recomp_mem_addr` to mirror the N64's low window for `a < 0x80000000`. Both
stores came from **the mis-bound `jal`s into rec14b's interior**: with rec14c
resident and the calls dispatched, the interpreter's frame is intact, the
emitter is reached on its proper path, and the low RDRAM window
(`0x0..0x40`) is **all zero** after a full New Game run (checked with
`OGRE_DUMP_RDRAM`). So the KUSEG mirror is *not* what makes the opening work;
it was masking this mis-binding.

The mirror is left in place (it is plausible hardware behaviour and narrow — 4
MiB), but its justification is now much weaker and it may hide real null
dereferences. **Next session: A/B it** (revert the hunk in
`n64modernruntime-n64recomp.patch` + rebuild) and keep it only if a path still
needs it. `docs/DECISIONS.md` carries a `> Superseded` banner on session 44's
entry.

## 4b. Guardrails (added this session, after the post-mortem)

This wall took five sessions because the port silently ran a *different module's
code*, and nothing in the build or the logs said so. Three checks now do:

1. **`tools/cross_bank.py check`** (and `check-banks`, wired into
   `make bank-recomp`) — fails the build when a bank unit defines a RAM range
   another bank can own **and** calls into it from one of its *other* records.
   A call *within* one record is fine. Verified: the check passes now
   (`bank units: 1312 direct call(s) into swappable RAM, 0 outside their own
   record`) and **fails** on the pre-fix code (replaying the old unit-C
   `funcs_6.c` as a scratch unit: `2 outside their own record`, including
   `func_ovlC_802282D8 ... calls 0x80239C24 directly`). `make cross-bank-check`
   runs the full audit, which also reports the main unit's *known* backlog
   (576 sites / 76 targets, sessions 33-41) without failing; `--strict` fails
   on it too.
2. **The unknown-module detector** (`app/src/bank_overlays.cpp`) — any PI DMA
   that starts exactly on a RAM base the game streams modules to, with a ROM
   source the port does not know, prints
   `[bank] UNKNOWN module rom=… ram=… (that RAM is also where rom=… loads)` plus
   the scene and record mask. Verified two ways: silent on a normal New Game,
   Tutorial and attract run (no false positives), and — with unit G's record
   removed from the generated `bank_funcs.inc` (one variable) — it prints
   `[bank] UNKNOWN module rom=0x2A8CF0 ram=0x802395E0 (...)`, i.e. the line
   whose absence cost five sessions. Note it also knows the main ELF's
   streamed overlays (`kMainOverlayModules`), which is what keeps overlay C
   from being reported.
3. **`tools/rdram.py <dump> banks`** — the one-command discriminator: for every
   module in `bank_funcs.inc` (+ the uncompiled segment-table records) it
   compares the dump's bytes with the ROM and says which module is *actually*
   resident, naming the ROM offset when it is not a known one. Verified across
   two dumps of the same run: mid-movie it reports
   `0x802395E0 resident: rom=0x2AE390` (rec14b), at step 2
   `resident: rom=0x2A8CF0` (rec14c). That is the measurement that cracked this
   session, in one command.

The bring-up recipe for a new screen is therefore: run it, grep the log for
`UNKNOWN module`, add that module to a unit that is **not** the caller's, recomp,
rebuild; only if it still misbehaves, run `rdram.py … banks` on a dump and ask
which bank was live at the faulting address.

## 5. What's next

1. **A/B the KUSEG mirror** (§4): the New Game path no longer needs it; check the
   tutorial, attract and intro too.
2. **Continue the New Game sequence with input.** Step 2 renders and waits;
   steps 3-19 (name form, birthday form, personality questions, closing movie)
   are still unvisited. The tap schedule that works is
   `OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D` for the movie, then a
   button schedule that presses A/Start *inside* `0x0D` to advance dialogue.
   Each step is one `0x02`→`0x0D` visit, so `docs/scenes.md` can be filled in
   step by step.
3. **The other unknown arena modules.** This session's method (log every PI DMA
   destination and diff against `kBankRecords`) is cheap and should be repeated
   for the later scenes: `grep -o "dev=0x… dram=0x… size=0x…" | sort -u` over a
   long run, then check each module-shaped DMA against the record table. The
   candidates left in this session's log were all asset-sized loads into the
   heap, but the check is now part of the bring-up loop.
4. Unchanged and open: menu `0x18` natural entry, `OGRE_NO_AUDIO=1` early-boot
   crash, `osViFade`, scene `0x12` (Load Game), the movie-engine path
   (`0x80197794`), and the remaining uncompiled segment-table records
   (5, 7, 8, 9, 16).

## Verification (final binaries, all probes reverted by `make recomp` + `make bank-recomp`)

* `make recomp` and `make bank-recomp` clean; `gen_bank_funcs.py` reports
  **7 unit(s), 18 record(s), 1889 function(s)** (was 5/17/1867).
* `grep -rl probe45 RecompiledFuncs/ Bank*Funcs/ app/` → empty.
* `build-null` and `build-app` rebuild clean (exit 0) after reconfiguring (the
  new units are globbed by `app/CMakeLists.txt`).
* **Null, movie-preserving taps** (`OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D`,
  `OGRE_SPEED=4`): `0x02` @10922 → `0x0D` @11037 → `0x02` @39467 (28.4 s movie)
  → `0x0D` @39588, exit 0, **0** `streamed function stub`, **0** crashes.
* **RT64, same schedule**, 55 s wall: reaches `0x0D` @40738, exit 0, no
  `[crash]`, no stub. The session-44 `do_sendP + 0xC4` SIGBUS is gone.
* **Captures** (RT64, `OGRE_CAPTURE_PRESENT`, every 40th present): the sepia
  courtyard movie (visit 1), then the cathedral with Odiron's dialogue and the
  advance quill (visit 2) — `docs/proofs/native-newgame-cathedral.png`; the
  "Ischka Military Academy / Graduation Ceremony" card from the tap-advancing
  run — `docs/proofs/native-newgame-academy-card.png`.
* **Bank switching**: the run logs `[bank] loading overlay record rom=0x2AE390
  ram=0x802395E0 size=0xA7E0 (105 functions)` for visit 1 and
  `rom=0x2A8CF0 ram=0x802395E0 size=0x56A0 (57 functions)` for visit 2.
* **Low window**: `OGRE_DUMP_RDRAM` after the New Game run, and after the
  forced-Tutorial and attract runs, shows RDRAM `0x0..0x40` **all zero**
  (session 44's emitter stores no longer happen on any of the three) — only the
  exception-vector words at `0x300`/`0x308`/`0x318`, which the KSEG0 path writes
  in every run, are non-zero in the first 4 KiB.
* Regressions: forced scene `0x17` (Tutorial) 25 s → exit 0;
  attract run 45 s at `OGRE_SPEED=8` → `0x09 → 0x0A → 0x04 → 0x0B → 0x04`, exit 0.
* A/B before the fix (for the record): with rec14b in unit C, the null build's
  step 2 read out of bounds and survived on a zero; the RT64 build crashed in
  `do_send`. See §1.

No repo test harness exists; each check is a ROM-dependent 12-170 s game run, so
the repro commands below are the maintained verification.

## Files changed (vs session 44)

* `config-bankF.yaml`, `config-bankF.toml` (new) — unit F, `bankRec14b`.
* `config-bankG.yaml`, `config-bankG.toml` (new) — unit G, `bankRec14c`.
* `config-bankC.yaml` — rec14b's segment removed (comment updated).
* `config-bankC.toml` — the three rec14b `function_sizes` overrides removed.
* `Makefile` — `BANK_UNITS := A B C D E F G`; the bank-ELF rule tolerates
  missing `asm/data`/`assets` objects.
* `docs/proofs/native-newgame-cathedral.png`,
  `docs/proofs/native-newgame-academy-card.png` (new).
* `PLAN.md`, `docs/DECISIONS.md` (new entry + superseded banner on session 44),
  `docs/scenes.md` (step 2 = renders; the Start-skip note resolved),
  `docs/README.md`, `AGENTS.md` (walls list + the two first-response
  diagnostics), `docs/guides/app-build.md` (`rdram.py banks`,
  `make cross-bank-check`), this file.
* Guardrails: `tools/cross_bank.py` (`check` / `check-banks`), `Makefile`
  (`bank-recomp` runs `check-banks`; new `cross-bank-check` target),
  `app/src/bank_overlays.cpp` (unknown-module detector), `tools/rdram.py`
  (`banks`). All three verified above, including the negative tests (silent on
  a clean run) and the positive ones (the detector firing when rec14c is removed
  from the record table; the assertion failing on the pre-fix unit-C code).
* Probes, **all reverted by `make recomp` + `make bank-recomp`**:
  `RecompiledFuncs/funcs_8.c` (`func_80089F80` entry DMA log),
  `BankCFuncs/funcs_5.c` (`func_ovlC_8023BF50` entry + table read; interpreter
  entry + opcode-6 frame dump), `BankCFuncs/funcs_6.c` (interpreter dispatch,
  `func_ovlC_8022A218` entry), `BankCFuncs/funcs_9.c`, `BankGFuncs/funcs_0.c`
  (`func_ovlG_80239C24` entry). Each carried `#include <stdio.h>`/`<stdlib.h>`.
* No runtime/vendored change this session.

## Repro commands

```sh
# the New Game opening, movie preserved, no input inside 0x0D
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64

# the same on RT64, with a capture of the cathedral
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_SCENE_LOG=1 OGRE_CAPTURE_PRESENT=/tmp/cath OGRE_CAPTURE_EVERY=40 \
  OGRE_EXIT_AFTER_MS=75000 ./build-app/ogrebattle64

# every PI DMA destination in a run (how rec14c was found)
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_EXIT_AFTER_MS=30000 ./build-null/ogrebattle64 2>&1 \
  | grep -o "dev=0x[0-9A-F]* dram=0x[0-9A-F]* size=0x[0-9A-F]*" | sort -u

# regressions
OGRE_SCENE=0x17 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 ./build-null/ogrebattle64
OGRE_SPEED=8 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64

# the guardrails
make cross-bank-check                       # audit (bank units fail, main backlog reported)
python3 tools/rdram.py /tmp/rdram.bin banks # which module is resident where
grep "UNKNOWN module" /tmp/run.log          # modules the port has no functions for
```
