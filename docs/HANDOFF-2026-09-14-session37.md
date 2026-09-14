# Session 37 — New Game stops crashing: the first targeted cross-bank dispatch, and scene 0x0D's arena banks

Goal (user request): start actually playing — title screen → Start → New Game
crashes. See how far the game gets.

Result: the reported crash is fixed and the game reaches **scene `0x0D`**
with all of that scene's code resident and **zero stub calls** (was: SIGSEGV in
scene `0x02`'s first update). Scene `0x0D` then dies in list-unlink code
(`func_80071950`) on a wild pointer — the next wall, characterized below but
not fixed. Boot/attract/title are unchanged (exit 0).

## 0. TL;DR

| question | answer |
|---|---|
| why did New Game crash? | scene `0x02`'s update `func_80178920` does `jal 0x80198D28`; N64Recomp binds it to the *containing* overlay-C body `func_801989AC`, whose prologue reads `lbu $v1,3($s2)` with `$a0` unset (NULL) → read of N64 address 3 → SIGSEGV |
| fix? | `tools/cross_bank.py dispatch --only 0x80198D28`: that one call site becomes `LOOKUP_FUNC`, which resolves to bank unit E's real entry `func_ovlE_80198D28` (record 0 is resident in that scene); a miss would be a no-op stub, so other scenes are unaffected |
| second bug at the same site? | yes — N64Recomp emitted the size-overridden `jal` as a tail call (`call; return`), abandoning the caller's epilogue (incl. the `D_800C4C26 = 0x800D` store). The dispatch repairs it to call-and-continue |
| how far does it get? | `0x09` → `0x0A` → `0x04` (title) → Start → `0x02` → **`0x0D`**, loading records 0, 14, 2, 4, 10, 11, 12, 13, 14a, 14b — then SIGSEGV in `func_80071950` (list unlink) on thread 3/4 |
| what is still missing? | nothing the loader asks for: no UNCOMPILED records, no stub calls. The `0x0D` crash is a wild data pointer with all code resident |

Repro (all headless, null renderer):

```sh
# the user path: boot to title, Start presses carry menu -> new game -> 0x0D
OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=180000 ./build-null/ogrebattle64
# deterministic variant, no input: forced jump straight into new game
OGRE_SPEED=4 OGRE_SCENE=new-game OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=90000 ./build-null/ogrebattle64
```

## 1. Root cause of the New Game crash

Backtrace (lldb, scene `0x02` active, records 0+14 just loaded):

```
func_801989AC+1062  (movzbl (%rbx,%r9),%ecx, fault N64 addr 3)
func_80178920+228   (scene 0x02's update, dispatcher-called)
func_80075BC0       (the scene dispatcher)
```

`func_80178920` is 11 instructions: save ra, `jal 0x80198D28` (delay slot: `sb
$zero`), then its epilogue. `0x80198D28` is interior to `func_801989AC`
(size 0x520), so N64Recomp emits a call to the containing body — which starts
with `lbu $v1,3($s2)` (`$s2 = $a0`, never set by the caller: NULL). Register
proof: fault host = rdram + `0x80000003 ^ 3` region after `recomp_mem_addr(3)`
underflows. On hardware the `jal` lands directly on the interior entry and the
prologue never runs.

Bank unit E (record 0, resident in scene `0x02`) provides the real entry
`func_ovlE_80198D28` (own prologue, loader logic, returns via `jr $ra`), so
dispatching that address is correct there. (Unit D, which menu `0x18`'s
record 1 uses, merged the same address into `func_ovlD_80198A6C` — the menu
path still needs the session-34 seed work.)

## 2. `cross_bank.py`: targeted dispatch + two soundness fixes

`RecompiledFuncs/` is generated and gitignored, so the fix lives in the
committed tool, wired into the default build (`recomp` target runs
`dispatch --only 0x80198D28` after every regen):

* `dispatch --only ADDR[,ADDR...]` (repeatable): rewrites only those targets.
  An `--only` target with no bank entry or no call site is a hard error; with
  no bank data at all (fresh checkout that never ran `make bank-recomp`) it
  warns and leaves the tree alone. Re-runs are no-ops ("already dispatched").
* Tail-call repair: when a rewritten site is followed by the premature
  `recomp_trace_return; return;` plus the duplicated delay-slot emission, it
  becomes the standard `LOOKUP; goto after_N; <dup>; after_N:` shape. A true
  tail call at a function's end has no duplicated delay slot and keeps its
  `return`. Safe either way: a lookup miss is a no-op stub, so falling through
  can never be worse than returning early.
* `revert` fragment guard (found by accident: a probe run dispatched
  `0x801980A0`, and reverting "restored" our site to `func_80198D28` — a
  same-address *fragment* that was never the original call). `revert` now only
  restores targets whose definition opens like a function (same prologue set
  as `entry_looks_real`); fragment targets (`func_80198D28`, `func_801AB740`)
  stay dispatched with a message. `make recomp` regenerates pristine C.

Current landscape: 67 distinct cross-bank `jal` targets, 9 resolvable, 58
blocked; this session dispatches exactly one (`0x80198D28`, one site). The
other 8 resolvable targets stay on the recompiler's bindings.

## 3. Scene 0x0D's banks: record 4 + record 14's arena (unit C)

With the crash fixed, scene `0x0D` (mask `0x40007C14`) ran degraded: 56k
stub calls/167 s from four addresses, all holding real MIPS prologues —
unregistered arena code above record 14 (session-33 §9 pattern). A PI-DMA
trace collapsed them to two modules plus a table record:

* record 4: ROM `0x0FA010` (0x5E0, all code per the segment table) →
  RAM `0x8019EE70` — overlaps record 3's RAM, so it lives in unit C, not A
* `bankRec14a`: ROM `0x29A490` (0xDE60) → RAM `0x8022ACB0`
* `bankRec14b`: ROM `0x2AE390` (0xA7E0) → RAM `0x802395E0`

(`config-bankC.yaml`; `bank_overlays.cpp` marks record 4 compiled; the two
arena sections register through the existing generated `kBankRecords`.)
Unit C is now 15 records / ~1721 functions. Two build issues on the way:

* N64Recomp could not size a jump table (`jr` at `0x8022EB3C`, table
  `0x80239270`): splat capped `func_ovlC_8022EA08` as nonmatching at 0x24C.
  Fixed with `function_sizes` in `config-bankC.toml` (extend to `0x8022EED4`).
* Six more "undeclared label" failures from splat carving 14b's flow into
  pieces capped mid-emission. Same remedy, four overrides:
  `func_ovlC_8023BDA8` 0x54, `func_ovlC_8023C9F4` 0x28, `func_ovlC_80239D68`
  0x470 (stops at the real entry `func_ovlC_8023A1D8`, which the scene calls),
  `func_ovlC_8023E070` 0x4B0. N64Recomp redirects calls to swallowed symbols
  into the extended body (`resolve_jal`/`has_size_override`), so no map entry
  is lost. The two `static_35_*` fragments this also covered compile as
  duplicates.

After this, scene `0x0D` loads 9 bank records (0, 14, 2, 4, 10–13, 14a, 14b)
with **zero stub calls** — then crashes (next section).

## 4. The next wall: scene 0x0D dies in list unlink (not fixed)

```
func_80071950+742  (sw $v1,0($v0), v0 wild; varies run to run)
func_800712C4+450  (a0 = s1-0x20 node)
func_ovlC_8023BDA8+156 ...
```

`func_80071950` unlinks a node (`+0x8`/`+0x10` links); the node pointer it gets
is garbage (fault N64 varies: `0x8FE228B0` vs `0x8FE248B0` across runs), so
the corruption is data-dependent, not a wrong-region constant. Established
about it:

* needs no input: forced `OGRE_SCENE=new-game` with no taps crashes the same
  way, deterministically (2/2). Tap-mashing exonerated.
* not a speedup artifact: `OGRE_SPEED=1` crashes identically in `0x0D`.
* not missing code: no UNCOMPILED records, no stub calls, all 9 records load.
* thread varies (3 vs 4) — worker-pool scheduling, not necessarily a race.

Prime hypotheses for the next session, in order: (a) a silent splat
truncation in 14a/14b (a cap landing between a branch/`jal` and its delay
slot drops the delay effect with no build error); (b) still-missing *data*
(the 0xD0 gap `0x80239510..0x802395E0` is never DMA'd; mask bit 30 is
unexplained); (c) unimplemented I/O on this path (Controller Pak save
probing would plausibly feed garbage into game lists). The ` **static_35_*`
mutual-recursion frames (#4/#6 both `static_35_8023BCB0`) are worth a look
during forensics, but mutual calls are not evidence by themselves.

Also observed (parked, unrelated to play): `OGRE_NO_AUDIO=1` now crashes
during early boot (<1 s, before any scene) inside
`ultramodern::debug_dump_queue_snapshot` on the VI thread — a diagnostic-code
fault, not game code. Audio-on boots are green (exit 0 through story `0x0B`).
Needs a look separately; do not use `OGRE_NO_AUDIO=1` until then.

## 5. Files changed (this session)

* `tools/cross_bank.py` — `dispatch --only`, tail-call repair, idempotent
  re-runs, strict `--only` errors, `revert` fragment guard (`ENTRY_OPS`,
  `def_first_ops`), docstring updates.
* `Makefile` — `recomp` runs the single-target dispatch; comment block updated
  (no longer "not part of the default build" for this target).
* `config-bankC.yaml` — `bankRec4`, `bankRec14a`, `bankRec14b` (+ bins,
  header comment).
* `config-bankC.toml` — 5 `function_sizes` (14a switch + 4 truncations).
* `app/src/bank_overlays.cpp` — record 4 marked compiled.
* `RecompiledFuncs/funcs_1.c` (generated) — the dispatched site.
* `BankCFuncs/` (generated) — ~323 new functions.

Verification (no repo test harness exists; scripted runs are the check):
* `OGRE_SPEED=4 OGRE_TAP_MS=3000 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=180000
  ./build-null/ogrebattle64` → exit 0 pre-banks (0x0D degraded, 56k stubs);
  post-banks → all 9 records load, 0 stubs, SIGSEGV in `0x0D` list code.
* `OGRE_SCENE=new-game` forced runs (no taps): same `0x0D` crash 2/2.
* `OGRE_SPEED=1` tap run: same `0x0D` crash. Boot/attract/title on
  `build-app` (RT64): exit 0, title `0x04` reached.
* `cross_bank.py dispatch --only 0x80198D28` re-runs clean ("already
  dispatched"); bad addresses/flags exit 1 with usage text.

## 6. Next

1. The `0x0D` list corruption (§4): RDRAM forensics from `func_800712C4`'s
   `s1` upward, or audit 14b caps for branch/delay splits; check the 0xD0 gap
   and mask bit 30; check pak/SI stubs on this path.
2. `OGRE_NO_AUDIO=1` boot crash in the queue-snapshot diagnostic.
3. Menu `0x18` still needs unit-D seeds (session 34 §6); the other 8
   resolvable targets are still opt-in.
4. `osViFade` still unemulated (fades snap).
