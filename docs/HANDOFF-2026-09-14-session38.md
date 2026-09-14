# Handoff — 2026-09-14, session 38: new-game scene (0x0D), three dispatches, BSS zeroing, and the residue wall

## Goal and result

Continue session 37's work: the scene shown on New Game (`OGRE_SCENE=new-game`
→ scene `0x02` → scene `0x0D`). The forced run crashed in `0x0D` init. This
session fixed two stale-code crashes (same bug class as session 37), added
record-BSS zeroing the hardware loader does and the port didn't, and proved
the remaining `0x0D` crash is **missing pre-state from skipped scenes, not a
port bug**: `0x0D`'s init reads a flag (`0x8019F794`) that is zero because
scene `0x02`'s record-0 writer never runs in the single-frame forced `0x02`.
The natural path (title → menu → `0x02` → `0x0D`) builds that residue but is
blocked at menu `0x18`'s pre-existing unit-D crash. Next session: menu `0x18`
(unit-D seeds), then re-test `0x0D` via the natural path.

## 1. Stale `func_801AFAF4` → dispatch `0x801AFC2C` (fixed, verified)

Repro (`OGRE_SPEED=4 OGRE_SCENE=new-game`, build-null) crashed deterministically
right after record 10a loaded, in main-unit `func_801AFAF4+818` (`lhu`, wild
pointer). `OGRE_TRACE_HOOKS=1` gave the shadow chain:

```
func_80071EB0 → func_800E9CEC → func_80080DC0 → func_80075BC0 (dispatcher)
  → func_80178568 (scene 0x0D's update, resident)
  → func_801AFAF4 (stale overlay-C body; RAM owned by resident record 10)
```

Root cause: resident `jal 0x801AFC2C` (two sites: `func_80177754` @`0x80177964`,
`func_80178568` @`0x80178834`) bound by N64Recomp to the containing overlay-C
body `func_801AFAF4`, while bank unit C's record 10
(RAM `0x801AD5C0`, resident) owns the address and provides the real entry
`func_ovlC_801AFC2C` (real prologue, in `kC_bankRec10Functions`).
`python3 tools/cross_bank.py dispatch --only 0x80198D28,0x801AFC2C` rewrote
both sites to `LOOKUP_FUNC` with the standard tail-call repair
(call-and-continue); the post-fix chain goes through `func_ovlC_801AFC2C`.
Wired into `make recomp` + Makefile comment.

## 2. Decompressor NULL-dst → missing residue (characterized, not a port bug)

With the dispatch, execution reaches bank code and dies one asset later:
`… → func_ovlC_801AFC2C → func_ovlC_801B7EBC → func_8007A110` (decompress),
storing to N64 `0x0`. Temporary probes (all reverted) established the full
chain, each link verified:

- `func_8007A110(dst=0, src=0x800CF9B0)` — dst NULL, so the first word-store
  faults at exactly N64 `0x0` (new `sigaction` handler prints
  `fault/rdram/offset`; offset `-0x80000000` = N64 0).
- dst=0 because `func_80070F30` (game heap allocator) legitimately refused
  size `0x4855FE00` (~1.2 GB).
- The size is the raw big-endian header word at src (`func_8007A7E0` returns
  it directly; its `func_8007A80C` tail call is dead code after `return`).
- The src bytes are byte-correct in RDRAM (compared against ROM `0x59439C`;
  note the host dump is word-swapped by design — guest word = reversed host
  bytes — which initially looked like a DMA endianness bug but isn't; all
  DMAs go through swizzled `MEM_B`).
- The ROM slice is the correct table lookup for asset id `0x148`
  (neighbouring table entry holds the matching length `0x6290`), so the id
  itself is wrong for this use.
- The id comes from `0x0D` init (`func_ovlC_801B7EBC`) taking the
  flag-zero path: entry reads `*(0x801D0790)`, the `0x801B80BC` branch reads
  flag `*(0x8019F794) == 0` and falls through to the init sequence that
  computes id `0x148` and decompresses the wrong asset. The skip target
  `L_801B8218` is downstream, so reaching site 1 proves flag==0.
- Flag lifecycle (probes): `0x02`'s loader entry `func_ovlE_80198D28` runs
  **once** but never reaches its writer call (`func_ovlE_801988C8` at
  `0x80198E44`); no write to `0x8019F794` ever happens; `0x0D` reads 0.
- A natural-boot RDRAM dump (20 s wall, reached scene `0x09`) shows the flag
  region and the record-3 state the `0x02` entry branches on are nonzero —
  i.e. earlier scenes build the residue the forced jump skips. Forcing `0x02`
  at t≈0 leaves zeros, the entry takes the non-writer branch, and `0x0D`
  walks the wrong init path with correct code and correct bytes.

A blind alley worth not repeating: mid-session the evidence briefly suggested
a DMA endianness bug (RAM bytes looked word-swapped vs ROM). They aren't —
host RDRAM stores words little-endian and `MEM_*` compensates; always compare
guest words (byte-reversed host words), never raw host bytes.

## 3. Dispatch `0x801980A0` (kept; same class, no effect on this path yet)

Two more resident sites (`func_801776EC` @`0x801776F4`,
`func_80177720` @`0x80177728`, both scene-transition helpers writing
`D_800C4C26`) call `jal 0x801980A0`, bound to the overlay-C **fragment**
`func_801980A0` (no prologue; reads `$s0`/`$t9` the callers never set), while
bank unit E (record 0, resident in scene `0x02`) owns the real entry
`func_ovlE_801980A0` (real prologue, contract on callers' `$a0` of `-1`/`0x29`).
Dispatched (both sites already call-and-continue, no tail repair). It does not
fire on the forced path (those helpers never run here) so it changed nothing
in `0x0D`, but it is the same verified bug class for when `0x02` runs
naturally. Battery shows no regression.

## 4. Record-BSS zeroing (fixed, verified mechanism)

`load_function_bank` only registered map entries while `do_rom_read` copies
just ROM bytes: `[ram_start + rom_size, ram_end)` (BSS) kept stale bytes.
`0x0D`'s list anchors live in record 10's BSS (`0x801D06A0..0x801F7100`);
record 14's BSS (`0x8022AC20..0x80243DD0`) holds the 14a/14b arena slots and
session 37's never-DMA'd `0xD0` gap. `ram_end` per record comes from the
game's segment table (ROM `0x387C0`, parsed offline) via a static map in
`tools/gen_bank_funcs.py` (chunk-DMA records 10a/10b/14a/14b default to no
BSS); the streamed-DMA hook zeroes the span and evicts overlapping map
entries. Verified: record-14 BSS reads back zero after load. This did not move
the forced-`0x0D` crash (that wall is residue, §2), but it is required
hardware fidelity for the arena/list code the natural path is about to run
(session 37's list-unlink wall lives exactly there).

Related ground truth, parsed from the segment table while here: record 10 =
RAM `0x801AD5C0..0x801F7100` (ROM `0x230E0` + BSS); record 14 =
RAM `0x802258B0..0x80243DD0`; record 1 (menu) =
RAM `0x80197B90..0x801F1530` (huge BSS — relevant when menu work starts).
There is **no** segment-table record for ROM `0x22A250`→RAM `0x801E6FD0`
(the game DMAs `0x10120` bytes there mid-`0x0D`-init; it is an asset/cart DMA
like the fills, not a record) nor for `0x23B1F0` (our `bankRec10b` never
loads in these runs — flag it when arena work resumes).

## 5. Crash diagnostics (kept)

`app/src/bank_overlays.cpp`: `on_fatal_signal` is now `sigaction` with
`SA_SIGINFO` — prints `fault=<host> rdram=<base> offset=<diff>` (the N64 fault
for word accesses) plus per-thread last functions; `OGRE_DUMP_RDRAM=<path>`
works on the crash path. These located both walls this session.

## Verification (final binaries, probes reverted)

- `make recomp` regenerates pristine `RecompiledFuncs` and re-applies all
  three dispatches (rerun is a no-op: "already dispatched" ×3).
- build-null + build-app both rebuild clean (only pre-existing SDL dylib
  warnings on app link).
- Battery, build-null: natural boot 20 s exit 0; forced
  title/intro/publishers/story/unit-info all activate and exit 0, **zero
  stub calls everywhere**; build-app forced title exit 0.
- Forced new-game / direct-`0x0D`: same deterministic wall (thread 3, N64
  fault `0x0`, 8-deep chain through bank entries) — documented §2, not a
  regression.
- Forced menu: same pre-existing unit-D crash
  (`8008AFE0 → 80072398 → 8017BB28 → 8019C5D4`, N64 `0xFC`) — unchanged.

## Files changed (vs session 37)

- `Makefile`: `recomp` dispatches three targets; comment block documents all
  three + BSS pointer.
- `tools/cross_bank.py`: docstring only (three-target usage).
- `tools/gen_bank_funcs.py`: `RAM_END` map + 6th `kBankRecords` field.
- `app/src/bank_funcs.inc`: regenerated (record `ram_end`s).
- `app/src/bank_overlays.cpp`: `BankRecord.ram_end`; BSS zero+evict in the
  streamed-DMA hook; `sigaction` crash handler (fault/rdram/offset,
  last-func dump, RDRAM dump on crash).
- `RecompiledFuncs/`: regenerated (`make recomp`); 5 dispatched sites total.
- `docs/DECISIONS.md`: session-38 entry. `PLAN.md`: untouched (see below).

## What's next (for session 39)

1. **Menu `0x18`** (unit-D seeds, session 34 §6): the gate to the natural
   path. Forced menu crashes in `func_8019C5D4` right after record 1 loads;
   chain `8008AFE0 → 80072398 → 8017BB28 → 8019C5D4`, fault N64 `0xFC`.
2. Re-run the **natural** path (title → Start → menu → `0x02` → `0x0D`) once
   menu works; expect the `0x8019F794` flag nonzero and `0x0D` past §2.
3. Then session 37's list-unlink wall (`func_80071950` via 14b) — retest with
   BSS zeroing in place; it may already be fixed.
4. Open question: `bankRec10b` (ROM `0x23B1F0`) never loads in any run so
   far; the game instead DMAs ROM `0x22A250` (`0x10120` B) to the same RAM.
   Revisit when arena-code calls land in `0x801E6FD0..0x801F70F0`.
5. `OGRE_NO_AUDIO=1` early-boot crash (session 37) still open, untouched.

## Repro commands

```sh
# current wall (deterministic): thread 3, N64 fault 0x0, 8-frame bank chain
OGRE_SPEED=4 OGRE_SCENE=new-game OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 \
  OGRE_EXIT_AFTER_MS=90000 ./build-null/ogrebattle64
# menu wall (unchanged): thread 4, func_8019C5D4, N64 0xFC
OGRE_SPEED=4 OGRE_SCENE=menu OGRE_SCENE_LOG=1 OGRE_TRACE_HOOKS=1 \
  OGRE_EXIT_AFTER_MS=60000 ./build-null/ogrebattle64
# RDRAM forensics at any crash:
OGRE_DUMP_RDRAM=./dump.rdram ...  # guest word at N64 A = reverse of host bytes at A-0x80000000
```
