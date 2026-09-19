# Handoff — 2026-09-18, session 83: a stub-finder tool, and what the port's dispatch actually resolves

**Read §1–§4.** The developer's ask was *"now it's mostly a game of whack-a-mole
to find the missing stubs — can you look around, or maybe build a tool, to locate
those places, and see what points to them?"* This session built the tool, and its
first answer is a negative that is worth as much as a list: **every static `jal`
target in the build is registered by at least one compiled module.** No run state
can be shown, from the build alone, to stub on a static dispatch. The remaining
mole is `jalr` (288 sites), a bank the port has no record for, or a load that
missed `on_streamed_dma` — and the tool's `log` mode names which, from a run log,
with no instrumentation.

No game code, config or generated output changed: this is `tools/stubmap.py`,
`make stubmap` / `make stub-check`, and the docs. The tree's only modified path is
`tools/RT64` (pre-existing submodule state — **not touched by this session**).

## 1. The failure class, and where the truth lives

A call stubs when `recomp::overlays::get_function(addr)` misses `func_map`. Three
questions decide whether it can:

1. **Is the RAM owned?** A streamed module owns a RAM range only while the game
   has DMA'd it there. The registration tables are
   * `app/src/bank_funcs.inc` — the `kBankRecords` rows `on_streamed_dma`
   registers (rom, ram, size, ram end, function array), **and**
   * `RecompiledFuncs/recomp_overlays.inl` — the `section_table` that
   `init()`'s `load_overlays(0x1000, entrypoint, 0x3E1B0)` installs (entry +
   main), plus the streamed overlays the app registers at boot.
2. **Does the module register the address?** Its function array.
3. **Was the module registered at all?** Only a run shows that.

Two corrections to the obvious implementation, both load-bearing:

* **Parse the section table, not the generated C.** `osSetIntMask` at
  `0x8008B820` is a *reimplemented* function: `elf.cpp` renames it,
  `N64Recomp::reimplemented_funcs` marks it, and the body lives in
  `librecomp/src/ultra_translation.cpp` (`osSetIntMask_recomp`). The generated C
  therefore has **no definition** at that address, but
  `recomp_overlays.inl:449` has the entry
  (`{ .func = osSetIntMask_recomp, .offset = 0x0001ABC0, ... }`), and the main
  unit's own calls compile to a direct `osSetIntMask_recomp(...)`. A first
  version scanned `RecompiledFuncs/*.c` for definitions and reported 18 call
  sites into `0x8008B820` from unit C as gaps — all false; the table is what
  `get_function` reads.
* **Ownership for dispatch is the code half only.** A record's `ram_end`
  (BSS included) is not code: it is exactly where the *other* banks of the arena
  load (record 14's chapter modules at `0x8022ACB0`/`0x802395E0`). Counting it as
  an owner (a) hides real gaps — session 82's `0x801D1508` sits inside record
  10's BSS span — and (b) invents false "stubs in the caller's own bank" ones
  (130 of them, all in the record-14 arena, before the fix).

## 2. The tool

`tools/stubmap.py` (new, ~1230 lines, stdlib only, writes nothing):

```
tools/stubmap.py report [--strict] [--json] [--verbose] [--max-sites N]
tools/stubmap.py target 0xADDR [0xADDR...]
tools/stubmap.py log <run.log> [--max-sites N]
tools/stubmap.py pointers [--limit N]
```

Call sites come from the generated C's own annotations (`// 0x801AFE88: jal
0x80219594` precedes `LOOKUP_FUNC(0x80219594)(rdram, ctx);`), so every finding
names the caller function, its unit and `file:line`. Buckets, strongest first:

| bucket | meaning |
|---|---|
| **DEFINITE** | no module registers the address (no run state can resolve it), or the caller's own resident bank has no entry inside its own DMA'd code |
| **MISSING FUNCTION** | a bank that can be resident has a real prologue in its own ROM bytes there but no entry in its table |
| **UNRESOLVABLE** | no compiled module's *code* covers the address at all |
| **BANK SELECTION** | the missing bank's bytes there are a body interior (the caller disposed of another bank's address) |
| **INERT** | the scene-descriptor record masks say that bank's record is never loaded with the caller's |
| **RSP IMEM** | `0x84000000..0x84200000` — microcode text the CPU never runs |

Current build (`make stubmap`): **985 static dispatch targets — 0 definite, 0
unresolvable, 129 missing-function candidates, 152 bank-selection candidates,
369 inert, 332 resolvable; 25 028 call sites (10 899 statically dispatched,
13 841 bound direct, 288 indirect `jalr`).**

`log` mode is the arbiter for a real hit, and needs no new instrumentation: the
runtime already prints the live words at a stub address
(`[overlays] @0xADDR: 27BDFFE8 …`), so the tool matches them against each
candidate module's ROM image at the same address and names the module that *was*
resident. It then uses the run's own `loading overlay record` lines (which the
app prints only when it really registered a record) to say which of the bugs it
is:

* the resident module **does** register the address, and the log has no
  `loading overlay record` line for it → the DMA never matched `kBankRecords`
  (the `UNKNOWN module` half);
* it registered, but the lookup still missed → a later overlapping load evicted
  its entries (the load/unload order in the log says which);
* it **does not** register the address → the caller dispatched an address that is
  a body interior in that layout (the tool names the containing function), or the
  port has no record for that module at all (the tool then finds the live bytes
  in the ROM and names the streamed record they belong to).

`pointers` scans the ROM for 4-byte-aligned words that are a *registered entry*
in some module and are not registered everywhere — the `jalr`/callback tables
that no `jal` comment can name (662 words; 487 risky).

## 3. Verification (all offline except the log/replay evidence)

* `make stubmap` (the report above); `make stub-check` exits 1 because
  missing-function candidates remain — that is the intended strict behaviour and
  is **not** wired into the build (a candidate is not a proof).
* **Replayed against the pre-session-82 module set** (unit AH filtered out in a
  throwaway harness): `0x801D1508` and `0x801D0AAC` classify as **DEFINITE**
  ("no module registers this address") with two callers each
  (`func_80177754` @ `RecompiledFuncs/funcs_3.c:14895`, `func_80178568`
  @ `funcs_6.c:2722`) — i.e. the tool would have found the neutral-encounter bug
  statically. That is the validation that matters.
* `stubmap.py log /tmp/enc2.log` (session 82's *pre-fix* log): stub at
  `0x801D1508`, live `27BDFFD0 AFB30024 3C138019 92737708`, resident =
  `bankRec10s` (unit AH, rom `0x23A370`) which registers the address; the log's
  own `UNKNOWN module rom=0x23A370` line is the missing-record half.
* `stubmap.py log /tmp/ogre-s73-r5.log` (session 73's settings-menu stub): all
  three hits (`0x80226B7C`, `0x80226CE0`, `0x8022859C`) are resident
  `bankRec16b` (unit AG, rom `0x279FF0`) and registered; the log's
  `UNCOMPILED streamed record 16` line is the missing-record half.
* `stubmap.py log /tmp/ogre-live4.log` (the 38 051-hit burst): 1 UNKNOWN module
  (`rom 0x1977B0` → `0x80214FA0`, streamed record 28) and both addresses
  (`0x80219594` ×38 050, `0x80219600`) match `bankRec9ab` (unit AB, rom
  `0x1977B0`), which registers them — the shop screen was uncompiled that day.
* A synthetic log with random live words exercises the unknown-module fallback
  (byte search → record attribution) and the callchain printer.
* `stubmap.py target 0x80219594` prints all thirteen banks of that arena, each
  one's entry/body status and ROM provenance, the scenes that load record 9, and
  the call sites — the "what points at them" view.

## 4. What the numbers say (the actionable part)

* **Nothing static is provably broken.** All 985 static targets are registered by
  some compiled module; `0x8008B820`-style false gaps are gone once the section
  table is the source. So a future session should not expect the report to hand
  it a crash — it hands it *leads*.
* The largest single cluster is **0x8019FAF8 (14 sites)** — 13 callers are unit N
  (mission, record 7) and one is unit T (the record-18 tutorial-practice bank);
  the registering module is **bankRec3 (unit A)** and the flagged bank is
  **bankRec07 (unit H)**, the scene-`0x07` form module whose bytes at that
  address open with a prologue. H is not loaded by record 7's mask (it is
  chunk-DMA'd by scene 0x07's enter), so this is very likely inert; the tool's
  record number for the scene modules is the RAM's record, not the mask bit, and
  that is the model's known weak spot (below).
* The **288 indirect `jalr` sites** are the true blind spot. `pointers` is the
  first pass at them: `0x801B7A88` has 36 pointer words in record 7's and record
  10's data and is registered only by unit N's record 7 — a callback table worth
  a run.

## 5. Known limits (say so in the next handoff rather than re-deriving them)

* **Which bank of one record is resident is scene data, not build data.** A
  bank's name encodes the record whose RAM it occupies (`bankRec9b` → 9), but the
  scene modules (`bankRec07` = scene `0x07`, `bankRec05` = scene `0x05`,
  `bankRec06` = scene `0x06`) are chunk-DMA'd by their scene's enter, so their
  name's record number does not correspond to a descriptor mask bit. The mask
  co-residency model is therefore only exact for the segment-table records; for
  the rest the tool falls back to "candidate". **The next refinement is
  mechanical**: `tools/arenamap.py` already computes each bank's loader function
  and `tools/scenemap.py` knows each scene's enter function, so loader → scene →
  mask would decide the scene modules too (both tools are offline; ~13 s
  combined).
* `looks_like_entry` (the prologue test) flags an address that is a function
  start in a *missing* bank's layout, but it cannot say whether that bank is ever
  resident with the caller — that is what makes 129 "missing function" entries
  leads rather than bugs.
* `pointers` filters to 4-byte-aligned words that are registered entries, but a
  word inside a module's code half can still be a coincidence (`arenamap.py` has
  the game's real code/data split from the loader's icache/dcache calls; this
  tool does not read it).
* `stubmap.py` is an analysis tool. It does not edit `symbol_addrs`, and it adds
  no build failure — a new dispatch target that lands in a bank with a prologue
  there but no entry does not stop `make recomp`.

## 6. Second deliverable: "what is the recomp %?", and measuring it

The developer asked this after §2 landed. There is no single number, because the
question decomposes into four with different certainty; `tools/recompcov.py`
(new) reports all four and says which is which. `make recompcov`.

| measure | value (this build) | source |
|---|---|---|
| **code bytes** | **99.05%** — `0x2B11D0` covered of the code span `0x001000..0x2B8B70` | the ELFs' `CODE`/`CONTENTS` sections vs the ROM (arenamap's model); the one `0x6990`-byte gap (`0x0DDF80..0x0E4910`) classifies **data**, with no `jal 0x8009DA50` in it |
| **modules** | **28/28** loader-pattern arenas have a unit; 44 bank records over 34 units + 10 base sections | `tools/arenamap.py`'s scan + `app/src/bank_funcs.inc` |
| **functions** | **5037** registered entries, **4963** distinct addresses | the two registration tables |
| **dispatch** | **985** static targets, **0 unresolvable**; 288 `jalr` sites unmeasurable statically | `tools/stubmap.py` |
| **execution** | **2205/4963 = 44.4%** for one mission played by hand (2213 functions entered); 1593/4963 = 32.1% on the scripted route; 408 on a bare 20 s boot | `OGRE_COVER=<path>` + `tools/recompcov.py --log` |

The execution number is the only one that needs the game, and it is a *route*
measurement, not a property of the build. The runtime already kept a per-function
entry counter for `OGRE_PROFILE`'s hot list but only ever printed the per-second
top 8; the counter's keys are never cleared, so its occupied slots **are** the
executed set. Added: `OGRE_COVER=<path>` (a path writes the census to its own
file — the form a hand-played session needs, since the run's stdout is tens of
thousands of lines; `=1` keeps it on stdout for scripts) — `app/src/main.cpp`
starts it, the bounded-run exit path, the window-close path and the live
console's `cover` dump it, `ultramodern::debug_cover_start()/debug_cover_dump()`
implement it (`function_trace.cpp`, declared in `ultramodern.hpp`), and a
`heavy_traces_enabled()` split makes a coverage run skip the shadow call chain and
the per-thread counters — without that split the hooks made the game crawl (62 s
of emulated time in minutes of wall clock; with it, ~134 s of game time in 180 s
of wall clock at `OGRE_SPEED=8`).

**Route measured** (`/tmp/cover-run2.log`): `OGRE_SAVE=assets/save-mission-1.srm`,
`OGRE_TAP_SCENE_BUTTON="title:start:4,0x12:a:3,0x03:a:14,0x0d:a:14"`,
`OGRE_SPEED=8`, 180 s wall — boot → title → Load Game → map → mission, plus the
developer playing in the window. Result: **0 stub hits, 0 UNKNOWN modules**, 20
records registered, and per-module execution fractions (main 392/803, streamedB
193/459, unit C's bankRec10 183/301, unit N's bankRec7 181/300, ... unit C's
bankRec11 28/188).

**The useful run is the one the developer played by hand**
(`OGRE_COVER=/tmp/cover.txt OGRE_PREF_DIR=/tmp/ogre-cover-prefs`, one full
mission): **2213 distinct functions entered, 2205/4963 = 44.43% of registered
addresses executed**, and — the number that answers "what is left?" — **exactly
two modules were never entered at all**:

```
  NEVER EXECUTED by this route: 2 module(s) / 61 registered function(s)
       54 function(s)      T bankRec18b   (record 18's tutorial-practice bank)
        7 function(s)      U bankRec18c   (record 18's mode-2 bank)
```

Everything else the mission route reaches, at least by address. Two caveats the
tool now prints itself:

* **A module that shares its RAM with a sibling bank has an upper-bounded count**
  (`[s]` in the output): siblings register the *same* addresses, so an executed
  address credits every module that lists it, including banks that were never
  resident. The 13 banks of arena `0x80214FA0` and record 10's banks are all
  marked; **only the 0% entries are definitive**. Which bank was actually loaded
  is what the run's `[bank] loading overlay record` lines say, which is why
  capturing stdout alongside the census is worth doing.
* A cover-only dump carries no run-log lines, so stub/`UNKNOWN module` status is
  *unobservable* from it — the tool says so rather than claiming "0 stubs".

**A bonus signal fell out of it:** 8 entered addresses are registered by *no*
module (`0x80198268`, `0x801983B8`, `0x80198448`, `0x801984BC`, `0x801985D8`,
`0x8019C6C8`, ...). They are body interiors in every layout that covers them, and
`tools/cross_bank.py check` already lists them as main-unit **direct calls into
swappable RAM** — i.e. the coverage census observed the wrong-binding backlog
being *executed*, from a run, with no probe. That is the clearest demonstration
that the two tools are measuring two halves of the same thing.

**A test-harness mistake, recorded because it cost the developer two runs:** the
first attempt was scripted with `OGRE_EXIT_AFTER_MS=240000` and scene-keyed taps,
so the window sat idle in the mission once the tap schedule ran out (14 `a`
presses on scene `0x03`), and the exit timer then killed the window while the
developer was playing in it. Worse, the **window-close path did not dump at all**
(the census was only on the `OGRE_EXIT_AFTER_MS` path), so that run produced
nothing. Fixed: `update_gfx`'s `quit` branch now calls `debug_profile_dump()` and
`debug_cover_dump()` before `ultramodern::quit()`. The right way to run a
coverage session is **`OGRE_COVER=1` with no taps and no exit timer**, played by
hand, closed when done.

## 7. Files changed

* `tools/stubmap.py` (new)
* `tools/recompcov.py` (new)
* `Makefile` — `stubmap`, `stub-check` and `recompcov` targets, added to the
  trailing `.PHONY`
* `docs/guides/app-build.md` — "Diagnostics toolkit" → `tools/stubmap.py` and
  `tools/recompcov.py`; the live console's `cover` command; `OGRE_COVER=1` row in
  the env table
* `docs/README.md` — the toolkit line
* `PLAN.md` — a status entry for this session
* `docs/DECISIONS.md` — a durable-decision row (session 83)
* `docs/HANDOFF-2026-09-18-session83.md` (this file)
* `app/src/main.cpp` — `OGRE_COVER=1` starts the census; the window-close path
  dumps it (and the profile) before `ultramodern::quit()`
* `app/src/sdl_platform.cpp` — `debug_cover_dump()` on the bounded-run exit path;
  the live console's `cover` command and its `help` line
* **submodule** `tools/N64ModernRuntime`:
  `ultramodern/src/function_trace.cpp` (`debug_cover_start`/`debug_cover_dump`,
  `heavy_traces_enabled()` split, `OGRE_COVER` in `trace_hooks_enabled`),
  `ultramodern/include/ultramodern/ultramodern.hpp` (declarations) — captured by
  regenerating `n64modernruntime-ob64.patch` with the nested `N64Recomp` excluded
  (`git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`), as
  session 76 requires. `build-app` and `build-null` were both rebuilt.

No probes were written; nothing needed reverting. No ROM, assets or generated C
was touched.

## 8. Next steps

1. **Get a real execution-coverage number**: `OGRE_COVER=1` with *no* taps and
   *no* `OGRE_EXIT_AFTER_MS`, played by hand, then closed — or `cover` from the
   live console mid-session. `tools/recompcov.py --log <log>` prints the per-unit
   breakdown, and the modules at 0% are the untested surface of the port.
2. Wire `arenamap`'s loader → `scenemap`'s enter → scene id into the module
   model, so the scene modules get real record/residency answers (§5).
3. Pipe a normal play log through `tools/stubmap.py log`; every hit attributes
   itself. That is the intended loop: play → `stubmap.py log` → either add a bank
   unit (the session-67/82 recipe) or add the missing entry to a
   `symbol_addrs-bank*.txt`.
4. Triage `pointers` (`0x801B7A88`, 36 words) against the callback tables the
   game actually calls, to close the `jalr` blind spot.
