# AGENTS.md — rules for AI agents working on this port

This repo reverse-engineers and recompiles one N64 ROM, across many sessions and
many agents. The most common failure in it is an agent inferring intent from code
and then building a whole session on that inference. Read this file first.

Companion docs:

- `PLAN.md` — status and roadmap.
- `docs/README.md` — doc index.
- `docs/DECISIONS.md` — decision log.
- `docs/HANDOFF-*.md` — per-session record; the newest number wins.
- `docs/guides/app-build.md` — build, run, and every `OGRE_*` knob.
- `docs/guides/emulator-first.md` — read this before interpreting a display list:
  what an emulator does differently, the five-check faithfulness list, how to hash
  a ucode into the GBI database, and how to borrow a reference emulator. See
  rule 8.

---

## Writing style — this applies to every word you produce

> **Write in simple, technical English. No mannerisms ("it's x, not y", "why x
> matters", etc).**

This covers generated docs (`PLAN.md`, `docs/DECISIONS.md`, handoffs, `docs/**`),
code comments, commit messages, and everything you say in session to the
developer.

- One claim per sentence. Name the mechanism, the address or the file, and the
  evidence.
- Do not use rhetorical questions, "not just x but y", "the real question is",
  or headings that promise significance. Say what the thing is.
- Do not tell the reader what they believe, want or should care about. State
  the fact.
- Drop emphasis words that carry no information: "crucially", "importantly",
  "the key insight", "what matters here".
- Write a plain statement instead of a contrast pair: "`func_80178568` is the
  enter; `func_80178954` is the update", not "`func_80178568` is the enter, not
  the update".

The same standard applies to your replies and to any text you write into this
file.

---

## 1. Ask the developer what a scene should display

The assembly gives mechanism. Only the developer knows intent; they have played
the game. Treat the developer as the oracle for what a scene shows and does.

> **If you find an unusual data structure, a table you cannot classify, or a
> branch you cannot name, stop and ask the developer what the current scene
> should display or do before building a theory on it.**

Two cases where an unasked question cost sessions:

- Sessions 40/41 read the `0x02`↔`0x0D` "loop" as the game's own scripted attract
  loop, and the crash as retail behaviour. The developer's description of the
  opening (short intro movie → name entry → questions) shows that it is the New
  Game sequence, and that the loop is the sequence player.
- Session 41 called `func_80178568` scene `0x0D`'s per-frame update. It is the
  enter. Session 42 found the descriptor lifecycle (`enter/update/leave`) and the
  hardcoded accessor table `D_800AF028`, which changed the selector logic from
  per-frame to once per visit.

Ask early, in batches, and attach the evidence plus your candidate answers so the
developer only has to choose:

> "Scene `0x0D`'s enter takes two branches on `D_8018F1C0`: `0` loads records
> 10a/10b and installs a vtable at `&D_801E5AC0`; non-zero runs
> `func_80226FA8` and sets a selector. Which one is the intro movie, and should
> the first visit be step 1 or step 0?"

Questions that resolve intent: what should this screen show; is this a movie, a
form, a menu or a cutscene; which button or flow gets here; does the real game
reach this; what text or appearance should the player see; is X supposed to
happen automatically.

## 2. Verify handoff claims at instruction level

A previous session's prose is a lead. Verify it before repeating it.

- Disassemble:
  `mips-linux-gnu-objdump -d build/bankC.elf --start-address=… --stop-address=…`
  (also `build/ogrebattle64.elf` and `build/bank{A,C,D,E}.elf`). Read the raw ROM
  bytes when endianness or mapping is in question; the byte-order note is in
  `docs/guides/app-build.md`.
- Never trust generated C alone for control flow. The recompiler expands jump
  tables, splits functions and emits fall-through calls. `BankCFuncs/` and
  `RecompiledFuncs/` are generated and gitignored, so a fact read there may come
  from a probe or an older patch.
- When you correct a previous session, say so, give the instruction addresses,
  and update `PLAN.md`, the handoff and `docs/DECISIONS.md`.

## 3. Verify the consumer before naming a struct, table or field

Do not infer a layout from the code that writes it. Find the code that reads it.

- `D_800AF028` is an accessor table (scene id → function returning the scene
  descriptor), and not a table of update functions. It is hardcoded in
  `func_80075BC0`. The descriptor words are `+0x00` enter, `+0x04`/`+0x08`
  per-frame hooks, `+0x0C` leave and `+0x10` bank-record mask. Callers:
  `0x80075F58`, `0x8007265C`, `0x80072544`, `0x8007602C`.
- `D_8018F1C0`/`D_8018F1C2` are written by the scene-script VM `func_80170974`
  with register-relative stores, and read by the scene loader, the `0x0D` enter
  and the title transition. One word, several consumers — see rule 5.

## 4. Check for mis-binding before blaming game logic

Several walls were the recompiler binding a call to the wrong function, or to the
wrong half of a function. Check these first.

- **Size overrides** in `config/config.toml` and
  `config/banks/config-bank*.yaml`. An override that runs
  past the next function's start makes the recompiler redirect every `jal` into
  it (session 41: `func_801AFC2C` `0x4A8` → `0x4A4`).
- **Fall-through and merged tails.** `func_ovlC_802399AC` has no prologue: in
  `bankRec14b` it is the continuation of `func_ovlC_80239874`, and a `jal` into
  it runs a body with a frame contract (`sp+0x1EC`, `s0`, the `f` regs) that the
  caller does not satisfy. Run `make midfunc` first; it lists every such tail
  (23 in the current build) with the frame slots and registers the tail reads
  before writing, and the `jal` sites that reach each one.
  Check which bank is resident first (session 45). In `bankRec14c`, the module
  scene `0x0D` streams to that RAM for steps ≥ 2, `0x802399AC` and `0x80239C24`
  are ordinary function entries. A prologue-less `jal` target in one bank is a
  hint that the call belongs to another bank, and not proof of a shared tail.
- **Cross-bank fixed-address calls.** Records overlap in RAM by design
  (`0x80197B90` holds records 0/1/2/15/17), so a fixed-address call can land in a
  different resident bank. `python3 tools/cross_bank.py report` lists the sites;
  `dispatch --only …` is wired into `make recomp`. Bank code compiles into
  `Bank{A..Z,AA..AH}Funcs/` (34 units) and is registered at runtime in
  `app/src/bank_overlays.cpp`. Unit H is the scene-`0x07` form module; unit I is
  scene `0x16`'s closing movie; unit J is the `bankRec10a` bank of the same RAM;
  K and L are the chapter-animation module and `bankRec14a`; M is scene `0x05`'s
  module. `cross_bank.py`'s `overloaded()` had a `min`/`max` bug that hid its
  whole range until session 55. `make bank-recomp` now runs
  `cross_bank.py check-banks`, which fails when a unit defines a RAM range another
  bank can own and calls into it from another of its records (a direct call bound
  to the wrong bank's layout; session 45). `make cross-bank-check` is the full
  audit, and it also counts the main unit's known backlog.
- **Register-relative stores and loads are invisible to symbol greps.** A search
  for `D_8018F1C0` misses
  `lui $s5, %hi(…) / addiu $s5, … / sh $a0, 0($s5)`. Grep the symbol and the
  offset/call sites, and read the function.
- **When a call runs the wrong code, ask the port first.** Run
  `grep "UNKNOWN module" run.log` (the app reports a module it has no functions
  for, with the RAM base and the ROM that normally loads there) and
  `tools/rdram.py <dump> banks` (which module is resident in each streamed RAM
  window, and which ROM offset the live bytes came from). These two answered in
  seconds what session 45 spent five sessions on.
- **Use the toolkit instead of writing another throwaway script** (session 46
  built it because those scripts cost hours). All of it is documented in
  `docs/guides/app-build.md` → "Diagnostics toolkit".
  - `tools/runlog.py <run.log>` — one screen: scene timeline, RSP tasks by type
    (including any non-gfx task the stub swallows), bank loads and problems.
    `--check` turns it into an assertion.
  - `tools/guestmap.py <addr>` — rom↔vram, owning record/unit, function-entry
    check, and which other records share this RAM.
  - `tools/rdram.py <dump> image` — render a region as an N64 texture, and say
    whether it is blank.
  - `tools/rdram.py diff A B` — what a run changed, grouped by module.
  - `tools/watch.sh <guest-addr>` — an lldb write watchpoint on the right host
    address, with backtraces.
  - `tools/scenemap.py` — the 25 scene types with their descriptor words, every
    writer of the pending-scene word, and the scripted step table. ROM-only, ~3 s,
    no run needed; see the scene fact below.
  - `tools/arenamap.py` — every streamed arena bank from the loader pattern
    itself: ROM start, RAM base, size, code/data split, BSS, the loader
    instruction, whether a bank unit links it; plus `--verify`, `--coverage` and
    `--entries`. Offline, ~10 s, no run needed.
- **For "what is in RAM at the moment X happens", use the live console, and not a
  bounded run's exit dump.** An exit dump is always too late (session 56). Start
  the game and write commands into the watched file
  (`printf 'c\nr <addr> <n>\ndump /tmp/at-now.bin\n' > /tmp/ogre-console.txt`),
  then read the `[console]` lines from the run's stdout. Keys `1`..`9` work with
  `OGRE_KEY_<n>` bound. Commands: `r`/`rh`/`rb`/`rk`, `d`, `f`, `fb`, `s`, `k`
  (checksum for A/B), `w` (a real write), `c` (scene/descriptor/mask/step),
  `dump`, `save`/`load`. See `docs/guides/app-build.md` → "The live console".
- **`save`/`load` are checkpoints** (session 58), and replaying a scripted path is
  the slowest part of a test. `save` writes the whole RDRAM image plus the
  runtime's overlay state: which recompiled body is mapped at each RAM address.
  RDRAM alone would restore a later bank's function map and run the wrong module's
  bodies. `load` rewinds the machine to that instant, and the game re-runs from
  there. Both wrap the file I/O in a runtime thread-park
  (`ultramodern::checkpoint_pause_begin`) because the game's N64 threads are 1:1
  native threads; without it the image tears (session 58's first attempts wrote a
  checkpoint whose own checksum did not match its bytes). A checkpoint is valid
  only for the process and binary that wrote it. One pair at the cathedral
  replaces a 45 s opening replay. See `docs/guides/app-build.md` →
  "Checkpoints".
- Chunk-DMA records may never be DMA'd at all (the `0xD0` gap), and record BSS
  must be zeroed on load (`func_ovlE_…`; see `load_function_bank` and `RAM_END` in
  `tools/gen_bank_funcs.py`).

## 5. Isolate one variable per experiment

An A/B run must change exactly one thing, or the result cannot be interpreted.

- Poking `D_8018F1C0` at scene `0x02` to test the `0x0D` enter's mode also changed
  the `0x02` loader, which reads the same word. The movie-mode crash that followed
  came from the loader. The clean test forced the branch inside `func_80178568`.
- When you cannot isolate, say so in the handoff and name the confound.

## 6. Revert probes by regenerating, and prove the revert

- Instrument generated code (`RecompiledFuncs/*.c`, `Bank*Funcs/*.c`) or the app.
  Never a submodule. Tag every probe (`// probe42`, `[probe42]`) so it can be
  grepped.
- Revert with `make recomp && make bank-recomp` (~5 s) and rebuild `build-null`
  and `build-app`.
- Prove it: `grep -rl probe42 RecompiledFuncs/ Bank*Funcs/ app/` must be empty,
  and re-run the battery. Record in the handoff which files carried probes.
- `OGRE_DUMP_RDRAM=<path>` writes the whole 8 MiB image for offline reads. Read it
  with `tools/rdram.py` (`word`/`half`/`byte`/`string`/`hexdump`/`find`/`ptr`),
  which implements the byte-order rules: words are little-endian at
  `addr - 0x80000000`, and logical bytes are XOR-3. See
  `docs/guides/app-build.md`.

## 7. Do not fabricate state to get past a crash

Do not invent a buffer, a flag, a return value or a "safe" branch to make a wall
go away (session 40: "Do NOT paper over it with a fabricated buffer"). A path
that only works with state the game never sets is a finding; write it down and say
what it implies. A repair is legitimate only when it reproduces what the game's
own code or data does.

## 8. Retail-behaviour claims need hardware evidence, and a port display list is not that evidence

The port cannot establish retail behaviour. A display list or decoded asset that
the port produced is evidence about the port until the port's faithfulness is
established. Sessions 60–63 treated them as statements about the game ("the
descriptor was never filled", "the constant is stale"), and then patched generated
C so a picture looked better. That is a rule 7 violation.

**Run the five faithfulness checks first.** They are cheap, and they partition
"the port differs from retail" into recompiler, ucode dispatch, renderer, RDRAM
contents and game state. Recipes are in `docs/guides/emulator-first.md`.

1. **Which GBI did this list get?** Hash the task's ucode with XXH3-64 and look it
   up in RT64's database (guide §3). Never infer a ucode from `OGRE_DL_DECODE` or
   `OGRE_DL_ANALYZE`; they decode every list as F3DEX2 and print plausible wrong
   geometry (session 50 §9).
2. **Did an RSP task go missing?** `tools/runlog.py <run.log>` flags non-gfx tasks
   that the stub swallowed. `app/src/rsp.cpp` runs the game's recompiled njpeg and
   audio microcodes and stubs every other task, so un-recompiled work produces no
   output at all.
3. **Is the code the code we compiled?** `tools/rdram.py <dump> banks` (which
   module is resident) and `make midfunc` (prologue-less tails).
4. **Are the bytes what hardware would have?** Decode an asset with an independent
   implementation and require the stream to end on its declared payload boundary
   (`tools/ogrelz.py`; 13/13 for the map's assets). Do this before blaming a
   buffer's contents.
5. **Is the recompiler faithful at this call?** Compare the generated C at the
   `jal`, the bank ELF and the raw ROM bytes. A `jal` delay slot runs exactly
   once, before the callee; the duplicate N64Recomp emits after `goto after_N` is
   dead code. Session 62 claimed the opposite and built a theory on it.

Then state both hypotheses and name the cheapest discriminating experiment:

- A runtime watcher or probe, which is cheap and stays in the port.
- A hardware watchpoint on the correct host address. Print
  `ultramodern::get_rdram_base()` (the crash handler prints it) and watch
  `base + (guest_addr - 0x80000000)`. Do not use an lldb expression evaluated at
  a bad stop.
- An RDRAM dump (`OGRE_DUMP_RDRAM`) shows what the game left where. It fires at
  exit, so it is always too late, and it can also be too early: session 64 read
  the map's `state` pointer as `0` from a dump taken at scene entry, after 856
  display lists had drawn. Use the live console (rule 4) for "at the moment X
  happens".
- A reference emulator, which is usually the strongest option and is already on
  this machine: RetroArch with `mupen64plus_next_libretro.dylib` or
  `parallel_n64_libretro.dylib`, and
  `~/Documents/RetroArch/system/Mupen64plus/mupen64plus.ini` is mupen64plus's game
  database. `docs/guides/emulator-first.md` §5 has the savestate → RDRAM recipe.

Read other emulators before writing a decoder for this game; they carry per-game
knowledge. GLideN64 ships
`[OGREBATTLE64] graphics2D\enableTexCoordBounds=1` ("prevents garbage due to
fetching out of texture bounds") and `hack_Ogre64`. mupen64plus-rsp-hle knows
OB64's JPEG ucode (`jpeg_decode_OB`, already ported).

If nothing in the run ever writes a slot that a path needs, the path is dead in
the port and on hardware for the same reasons. That conclusion is legitimate, and
it means stop chasing the path as a port bug.

## 9. Timing and harness caveats

- `OGRE_TAP_MS`, `OGRE_EXIT_AFTER_MS` and `OGRE_SPEED` are wall-clock.
  `OGRE_TRACE_HOOKS=1` slows boot several-fold, so a tap schedule tuned at 1×
  misses the title. Re-tune it, or tap forever, when you need a trace.
- `OGRE_SPEED=n` scales the emulated clock, so a fixed wall-clock tap point moves
  relative to the game.
- Scene forcing (`OGRE_SCENE=…`) lands only before the boot enters its first scene
  (~1.1 s). A forced scene enters without the pre-state a natural path builds, so
  "forced X crashes" does not mean "X is broken".
- No repo test harness exists. Each check is a ROM-dependent 12–170 s game run.
  Disclose that; do not invent a framework.

## 10. Leave the tree and the record clean

- Per session: update `PLAN.md`'s **Status** and **Open work** lists (the
  session-by-session log is `docs/STATUS-LOG.md`, and the per-session detail goes
  in the handoff), write `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, and add the
  decision to `docs/DECISIONS.md` when a choice was made.
- The handoff must state the goal, the result, the instruction-level evidence,
  what was run for verification, the files changed, and which probes were used and
  reverted.
- Never commit ROM dumps, extracted assets, or changes inside `tools/`
  submodules. Vendored code is gitignored; list it explicitly in the handoff.
- `git status --short` before you finish must show only intended changes.

---

## Verified facts

These were wrong or missing in earlier sessions. Re-verify if in doubt; otherwise
start from here.

- **Uninitialised guest state holds the game's own leftovers, not noise.** The
  recompiled code is the ROM's instructions, so a register or stack slot that
  nothing wrote holds the value it holds on retail. The one systemic difference is
  that the port zero-fills RDRAM at boot. When the port has `0` where retail
  clearly does not, check the address map (`recomp_mem_addr`: KSEG0, KSSEG/KSEG3,
  the KUSEG low window) and the runtime bridges (they translate guest pointers
  themselves and must agree with the macros) before suspecting a game-logic bug.
  Session 44 concluded from this that retail must tolerate the guest-`0` stores.
  Session 45 found that those stores came from a call bound to the wrong bank.
  Check the bank map before concluding that a zero is retail behaviour.
- **Vocabulary and intent live in two docs.** `docs/symbols.md` maps address →
  proposed name → evidence → confidence; read it before naming anything, and
  before calling a function "the X function". `docs/scenes.md` records what each
  screen should show, from the developer (the AGENTS §1 oracle data).
- **Scene dispatch.** `func_80075BC0` looks up `D_800AF028[scene_id]()`; ids
  ≥ 0x1F map to index 0. The accessor returns the scene descriptor, whose words
  are `+0x00` enter (once), `+0x04`/`+0x08` per-frame hooks, `+0x0C` leave and
  `+0x10` bank-record mask. `D_800C4C26` is the current/pending scene word
  (`0x8000|id`, plus the `0xFFFC`/`0xFFFE` control values).
- **There are exactly 25 scene types, and `tools/scenemap.py` extracts the whole
  scene graph from the ROM in ~3 s.** Do not hunt a scene's address by hand. The
  structure has three layers, and only the first two are code.
  1. **The registry.** `func_80075BC0` writes 25 accessor pointers into
     `D_800AF028[0..24]` from an immediate list. Each accessor is a 1-3
     instruction stub that returns a descriptor address. The descriptor table is
     static ROM data (`streamedB`, ROM `0x40E80` + (vram - `0x8016AF80`)). The
     tool prints id → accessor → descriptor → enter/update/hook/leave/mask for all
     25: 20 resolve statically, and ids 2, 3, 6, 8 and 23 branch and are listed as
     `(indirect accessor)`. It reproduces every address this project found by
     hand: id 5 (map) → `0x8018FD70`, enter `0x8017B60C`, mask `0x2`; id 7 (form)
     → `0x8018FDAC`, enter `0x8017B794`; id 13 (`0x0D`) → `0x8018FC3C`, mask
     `0x40007C14`.
  2. **The transitions.** Every store into `D_800C4C26` in the game is 39 sites
     in 30 functions. 21 carry a statically-known value; the rest are
     script-driven, because the scene-script VM writes `D_8018F1C2` and the
     handler copies it. These 39 sites are the whole scene-to-scene code.
  3. **The content.** The scripted step table is asset `0x19A8804` (ROM
     `0x1F3CA54`): a `u32` payload size `0x1A74`, then 1693 `u32` per-step asset
     ids (1499 distinct), indexed by `D_8018F1C0 & 0xFFF`. The 200+ dialogues are
     entries in this table, and not scene types, so decoding the step-descriptor
     opcode format once covers all of them. There is no per-dialogue linking to
     author. The 75 njpeg assets have the same shape: decode the format once, and
     everything works.
  `tools/scenemap.py` also has `scenes`, `transitions` and `steps` subcommands and
  `--dump <rdram>` to read descriptors from a live image instead of the ROM.
- **`0x0D`** (`D_8018FC3C`): enter `func_80178568`, update `func_80178954`, hook
  `func_80178B40`, leave `func_80178B7C`, mask `0x40007C14`. Its enter has two
  modes selected by `D_8018F1C0`. `0` or bit15 selects movie mode: it DMAs records
  10a/10b and installs the cutscene vtable `&D_801E5AC0`. Non-zero selects the
  cutscene engine that the New Game opening uses: it DMAs 14a/14b and calls
  `func_80226FA8` → `func_8022683C(-5, F1C0 & 0xFFF)`. Session 42 called the
  second mode "command mode". Session 43's render shows it drawing the New Game
  movie, so that label is a misnomer.
- **`0x02`** (`D_8018FC50`): enter `func_80178920`, a one-frame loader that sets
  next = `0x0D`.
- **New Game** is `title (0x04) → 0x02 → 0x0D → 0x02 → 0x0D → …`, a scripted
  sequence. The VM `func_80170974` writes the step word `D_8018F1C0` and the next
  scene `D_8018F1C2`, 16 script opcodes per visit. The attract loop is a different
  flow (`title ↔ story 0x0B / unit-info 0x0C`) and never enters `0x02` or `0x0D`.
- **Known open walls**, as of session 45, with the later corrections below. The
  New Game opening runs. Step 1 (the movie, sepia courtyard) renders. Step 2
  renders the cathedral dialogue (`Archbishop Odiron` / "He who has learned the
  way of the sword and god's teachings,") on both renderers
  (`docs/proofs/native-newgame-cathedral.png`), and then waits for input. The wall
  that looked like step 2 was a missing streamed module: scene `0x0D` streams
  `bankRec14c` (ROM `0x2A8CF0`, 0x56A0) over `bankRec14b` at RAM `0x802395E0` for
  steps ≥ 2, and the port had only rec14b, so unit C's 95 calls into that arena ran
  rec14b's prologue-less body interiors (session 45; the records are now in units
  F and G, and the calls compile as `LOOKUP_FUNC`). Steps 1..19 are decoded
  (`docs/HANDOFF-2026-09-15-session44.md` §1), and every step ≥ 2 sets
  `D_8018FC39 = 2`. Session 44's readings of the same wall (`jal 0x802399AC` as a
  "shared tail", the guest-`0`/`8` stores, the KUSEG mirror fix in
  `recomp_mem_addr`) were symptoms of that mis-binding and are corrected. The
  low-window stores no longer happen, so the mirror is unexercised and should be
  A/B'd.
  Session 54 corrects the rest of this wall: the cathedral does hand off. `A`
  advances its dialogue; the engine exit `func_80178CB0` resets `D_8018F1C0` to 0
  and sets the next scene to `0x07`, which the dispatcher runs at `t≈17.9 s` (4×)
  of a title-driven New Game. Session 53's "never advances / 1 GiB `memset`" is a
  property of the `OGRE_STEP` shortcut: seeding step 2 skips step 1, so the exit
  takes its `otherwise` arm, re-enters `0x0D` at step 0 and selects the movie-mode
  branch. Drive the opening from the title
  (`OGRE_TAP_BUTTON="start,a,start,a,…"`) instead of seeding a step.
  Session 55 corrects session 54's reading of scene `0x07`: its descriptor is
  `D_8018FDAC` (the accessor table maps id 7 → `func_8017B600`; enter
  `func_8017B794`, mask `0x00000002`), and not `D_8018FB98`. The module it streams
  is real: ROM `0x712A0`, size `0x84B0` (the enter's own `subu`; session 55
  recorded `0x8600`, the 67-chunk rounded figure) → RAM `0x8019A7C0`, chunk-DMA'd
  by the enter. That RAM overlaps record 3 (unit A) and overlay C (the main ELF),
  so the port's build-time bindings ran overlay C's bodies at the module's
  addresses; none of the module's 24 internal `jal` targets were main-ELF entries.
  It is now bank unit H, and the six calls into it are dispatched. The name-entry
  form renders (`docs/proofs/native-newgame-name-entry.png`). The general rule: a
  streamed module that is not in the segment table still owns a swappable RAM
  range, and needs the session-45 treatment. `tools/cross_bank.py`'s overlap model
  had to be fixed to see it.
  Also open: the movie-engine branch (`F1C0 == 0`, gated by the word `0x80197794`,
  and not `0x8019F794`; see session 43); menu `0x18` natural entry; scene `0x12` =
  Load Game (needs save pre-state); the `OGRE_NO_AUDIO=1` early-boot crash;
  `osViFade`. Scene `0x17` = the Tutorial and runs (bank unit B, records 17/18).
  Dialogue text is LZ-compressed (`func_8007A110`, `tools/ogrelz.py --asset`).
  Current status and details are in the newest `docs/HANDOFF-*.md` and `PLAN.md`.
- **Do not compile a swappable RAM range into the unit whose code calls into it.**
  If two records occupy the same RAM (banks of one arena), put both in other
  units. N64Recomp then emits `LOOKUP_FUNC(addr)` for the call sites, and the
  runtime's DMA-driven bank map picks the resident module. If a caller's unit also
  defines the range, the call binds at build time to one bank's bodies, and the
  other bank's callers run them with the wrong frame and register contract
  (session 45's step-2 wall). Find unknown modules by logging every PI DMA
  destination in a run and diffing against `bank_funcs.inc`'s record table.
- **The njpeg display list is an S2DEX2 list, and its `0xDA` command is the draw**
  (session 51; session 52 fixed the colours). The ucode at `0x800A5110` is
  `S2DEX 2.08` (`GBIUCode::S2DEX2`). XXH3-64 of the game's own ucode text
  (0x18C0) and data (0x390) reproduces RT64's DB entries `S2DEX2_FIFO_2_08`
  (`0x9300F34F3B438634` / `0x50EF0DFBD3A8CD0F`) exactly. In an S2DEX2 list the
  per-macroblock `0xDC`/`0xDA` are `G_OBJ_MOVEMEM` (`gSPObjSubMatrix`; the matrix
  selector is `cmd0 & 0xFFFF`, `2` = sub matrix) and `G_OBJ_RECTANGLE_R`
  (`gSPObjRectangleR`, the 24-byte `uObjSprite`). They are not F3DEX2
  `G_MOVEMEM`/`G_MTX`. RT64's `GBI_S2DEX2` mapped neither, so the geometry was
  silently skipped and only the texture loads ran. Session 50 §9's "the list
  carries no geometry" is therefore wrong; the app's own `OGRE_DL_ANALYZE` and
  `OGRE_DL_DECODE` still report that, because they decode every list as F3DEX2.
  Both commands are now in the tree, and a live `OGRE_S2D_TRACE=1` shows 300
  rectangles tiling the 320x240 `G_SETCIMG` target exactly, one per macroblock.
  A YUV tile is the RDP's two-plane format: one luma byte per texel at
  `offset + stride*t + s` in the upper TMEM half, and one U/V pair per two texels
  in the lower, both at `stride = line << 3`. A YUV `LOADTILE` therefore
  de-interleaves (`RDP::loadYUVTileToTMEM`); the sampler follows parallel-rdp's
  `sample_texel_yuv16`; YUV16 tiles need raw-TMEM sampling; and the decoder's
  source word is `[U, Y(even), V, Y(odd)]` (mupen64plus-rsp-hle `jpeg.c`
  `GetUYVY` / `jpeg_decode_OB`, verified against a live macroblock dump), which
  corrects session 50 §2's `[Y, V, U, Y]`.
  The textures are converted with the matrix that the game itself programs via
  `G_SETCONVERT`. `OGRE_CONVERT_TRACE=1` shows the raw 9-bit fields
  `k0=175 k1=469 k2=423 k3=222`. The conversion must sign-extend each 9-bit field
  and scale it `2*K+1` (GLideN64 `gDPSetConvert`: `SIGN(k,9)<<1 + 1`;
  parallel-rdp `set_convert`: `2*sext<9>(k)+1`), which gives the coefficients
  351/−85/−177/445.
  The rows are `R = Y + K0*V'`, `G = Y + K1*U' + K2*V'` and `B = Y + K3*U'` (with
  `U'=U-128`, `V'=V-128`); `K1` pairs with U, and `K2` with V. Session 51 fed the
  raw unsigned fields in and paired `K1` with V, so the green row exploded positive
  and the backdrop drew as blue banding. Session 52 fixed both, and the backdrop is
  now the cathedral. `docs/proofs/native-newgame-cathedral.png` and
  `-cathedral-background.png` were replaced; the old files were the black/blue-band
  broken renders, mistaken for correct because the top of the real scene is dark.
  The geometry, the de-interleaved TMEM planes and the UYVY source are all verified
  correct, so session 51 §7's "the defect is in sampling/upload" is wrong. `K4` and
  `K5` as combiner inputs stay the raw fields over 255 (GLideN64
  `_FIXED2FLOATCOLOR(k,8)`), which is what RT64 already did.
  The port completes the emulated RSP task as soon as the display list reaches
  RT64, but the game then waits for the DP completion, which the runtime posts only
  after `send_dl` (`events.cpp:432`). The CPU copy therefore does not race the
  render, and it calls `ogre_sync_framebuffers()` first, which forces the RDP's
  pixels back to RDRAM. Session 50 added
  `Application::waitForGameFramebuffers` (RSP worker, `OGRE_NJ_WAIT_MS`) for that
  race. It never did anything useful: session 57 measured it as a no-op in the
  njpeg path (`spins=1 found=1 ms=0`), and session 77 found its real cost. It polls
  three fixed addresses (`0x400`/`0x25C00`/`0x4B400`), so every scene that rendered
  elsewhere burned the whole 500 ms timeout on every display list. The boot's
  publisher stills (scene `0x0A`, the Nintendo/ATLUS/QUEST logos) ran at 2 display
  lists/s instead of 30. The call was deleted; a per-frame wait for a condition
  that cannot become true is not a slow frame, it is a 16× slow scene.
  The njpeg readback's source is the game's own `state[0x64]`, and not RT64's
  scratch word. `0x807FFC08` is never cleared, so it is stale at the first pass of
  every assembly, and preferring it unconditionally produced the intermittent
  stale-backdrop rectangle (`tools/njpeg_readback.py`,
  `docs/HANDOFF-2026-09-16-session57.md` §1).
  The readback that matters runs in scene `0x02`: the four stage-3 copies in
  `func_ovlE_8019976C`, whose sole caller is bankE `0x80199D80` inside
  `func_ovlE_80199D30`'s drive loop. The four passes copy `state[0x78]` x
  `state[0x7A]` = 240x320 / 240x176 / 144x320 / 144x176 into `0x801AAE90` /
  `0x801D06B0` / `0x801E50D0` / `0x801FB8F0`, the four chunks of the scene's 2x2
  background. The framebuffer's own chunk order is not the scene order, so do not
  infer an on-screen position from a render of it. See
  `docs/guides/njpeg-backgrounds.md`.
  The assembled image at `0x80243E28` renders as the cathedral. The opening then
  runs to scene `0x16` (the closing movie), which chunk-DMAs ROM `0x244770`
  (0x7500) → RAM `0x801D0860`, record 10's arena, which `bankRec10a` (unit J) also
  owns. That module is bank unit I (session 58; 43 functions), so the movie plays
  and the sequence advances.
  The step after it is step 1073, a valid step and not an overrun. The step table
  at ROM `0x1F3CA54` (asset `0x19A8804`) is a raw `u32` array of 1693 entries (its
  header word is `0x1A74`), and 1073 is the "Prologue" chapter animation the
  developer describes. Session 59 fixed the crash it used to produce. The step's
  command is `-8`, and not `6`: `func_ovlC_80227E64` maps the descriptor's
  last-word low byte 6 through the table at `0x8022ABE0`, and session 58's "command
  6" was the raw byte. `func_ovlC_8022683C`'s `-8` arm at `0x802269DC` streams a
  third bank of the record-14 arena: ROM `0x286BA0` (0x138F0) → RAM `0x8022ACB0`,
  code to `0x8023DE30`, i.e. RAM `0x8022ACB0..0x8023E5A0`, bss to `0x8023E630`.
  The port had no code for it, while `bankRec14a` (the other bank of that RAM, ROM
  `0x29A490`) was compiled into unit C, so unit C's calls into `0x8022ACB0+` ran
  rec14a's bodies with the chapter module resident. Those calls are the interpreter
  `func_ovlC_802282D8`'s `jal 0x8022C270`/`0x8022C6E4` and the callback
  `func_ovlC_80225F60`'s `jal 0x8022E3F0`. They are now units K and L, and compile
  as `LOOKUP_FUNC`.
  The engine/interpreter state that the interpreter reads is at `0x8022A970` (pc),
  `0x8022A978` (descriptor base) and `0x8022A994` (engine struct), and not at
  `0x8023A9xx`. The code addresses them with `lui at,0x8023` plus a negative
  immediate, and the addition wraps (`0x80230000 + 0xFFFFA994 = 0x8022A994`), so
  reading `0x8023A9xx` reads the DMA'd arena image. Session 58's probe58 conclusion
  ("the engine init does not run on the faulting visit") and its `0x8023A994`
  readings describe the same crash from the wrong address.
  Result: the Prologue card renders
  (`docs/proofs/native-newgame-prologue-card.png`); the opening plays on into the
  post-movie story (General Godeslas `-received-for-duty.png`; Magnus
  `-magnus-old-man.png`) and reaches scene `0x05` (descriptor `0x8018FD70`, mask
  `0x2`), whose module is unit M (ROM `0x79750`, 0xDAD0 → RAM `0x8019A7C0`). The
  other bank of that RAM is unit H, whose real size is `0x84B0`, and not the
  `0x8600` that session 55 recorded. A bank's size is its DMA's, and "do we know
  this module?" must compare the chunk's rom→ram delta rather than the ROM range
  alone; that range test is what hid unit M behind unit H's over-claimed size.
  Scene `0x05` is the map scene (developer, session 60), and it renders
  (`docs/proofs/native-newgame-map-scene.png`). It stayed on the prologue's black
  fade because no frames were produced. The scene update's `jal 0x8019AF0C`
  (`func_8017B858` @0x8017B8A0) and the scene hook's `jal 0x801A103C`
  (`func_8017B9C8` @0x8017BA10) are calls into the RAM the module occupies, and
  N64Recomp compiled each as a call to the containing body followed by an early
  `return` (its emission for a `jal` into a size-overridden body interior). The
  early return abandons the caller's frame, so the frame-pump thread (t4,
  `func_8008AFE0`) read its saved `$s0`/`$s1` from the wrong stack slots. `$s1` is
  the message-type comparator `1`, so the type-1 dispatch stopped matching and the
  pump died after two frames. The signature is `D_800AEFA4` (frame counter) frozen
  while `D_800C4BCC` (VI retrace) keeps counting, with only 2 display lists
  submitted. Both targets are now in `make recomp`'s
  `cross_bank.py dispatch --only` list, which also runs the tool's tail-call
  repair (call-and-continue).
  `0x8019AF0C` resolves to unit M's state-1 handler; scene `0x05`'s enter sets
  `0x801977E8 = 1`, and scene `0x07`'s sets `3` and takes the already-dispatched
  `0x8019B340`. To find a leaking call, log the guest `$sp` (`ctx->r29`) before and
  after each call in the chain; a leak shows as a lower `sp` after return. The
  no-`--only` full dispatch also fixes it (89 sites, 7 extra targets) and is the
  way to clear the remaining backlog. See
  `docs/HANDOFF-2026-09-16-session60.md` §1-§3, then
  `docs/HANDOFF-2026-09-16-session59.md` §1-§4, then `-session58.md`,
  `-session57.md`, `-session52.md`, `-session51.md` §1-§7 and `-session50.md`
  §5/§9.
  Other large backgrounds use the same machinery, and the dominant one is the same
  size. All 75 njpeg assets are enumerated (42 are 320x240), with the asset format,
  the tile geometry and the check recipe in `docs/guides/njpeg-backgrounds.md`. The
  stale-source fix is global to that path, and not cathedral-specific.
- **The map screen's sprites (scene `0x05`): session 64 corrects sessions 60–63,
  and landed no code.** Four independent checks say the port is faithful here. The
  LZ decoder is exact (13/13 map assets end on their declared payload boundary).
  The live RDRAM at `state[+0x04]` is 21128/21128 bytes identical to an offline
  decode of asset `0x01DD210A`. The map's gfx ucode `0x8009F540` hashes (XXH3-64
  over the raw bytes, length `0x1390`) to RT64's `F3DEX2.fifo 2.08` database entry,
  so the GBI choice is right. Every constant was re-read from the raw ROM, and not
  only from the ELF.
  Two session 61/62 mechanisms are therefore withdrawn. N64Recomp does not run a
  `jal` delay slot twice; the duplicate after `goto after_N` is dead code. The
  builder `func_ovlM_8019F83C` emits no `G_SETTILESIZE` at all: the word at
  `0x8019F990` that sessions 61/62 read as "the builder's own window" is the
  TEXRECT's s,t half (`u<<21 | v<<5`), so there was never a second window writer to
  find.
  The knight is not a decoded asset. The map's enter `func_ovlM_8019A7C0`
  `malloc(0x18000)`s `state[+0x34]` (`0x8019A9E4`) and composites an RGB555 LUT over
  an 8-bit index image into it: a 32-texel-wide RGBA32 sheet of 24 frames (`0x1000`
  bytes each, that is 8 directions × 3 animation frames, selected by
  `3*state[0x1DC] + f`). `state[+0x04]` is asset `0x01DD210A`. The shadow draw's
  `+0x1068` really is all zero, and the only translucent-black shadow art in any
  map asset is at `+0x10AC` of that same asset. `state` is a heap pointer; read it
  from `*(0x80197B18)` in each image. The `0x801F1570` of sessions 60/63 is
  run-specific.
  The two party draws are `func_ovlM_801A2A7C`'s calls at ROM `0x81B50` (`a2=0xB` =
  entry 11 `(144,23)`, texture `state[+0x04]+0x1068`, static, `line=2`,
  `cms=WRAP masks=3`) and `0x81BFC` (`a2=0xA` = entry 10 `(16,11)`, texture
  `state[+0x34] + frame*0x1000`, `line=8`, `SETTILESIZE` 31×31).
  The open lead is renderer-side. The sheet's row stride is 128 B (`line` 16) while
  the draw's render tile declares `line=8`. That is the game's own command, so it
  is a question about `G_LOADBLOCK`/tile-line semantics, and not about game data.
  Call 1's `line=2` with `masks=3` and a 7-texel window is only self-consistent read
  as 16-bit, which is the same question.
  Do not "fix" the entry indices or the `0x1068` offset by editing generated C;
  that is a rule 7 violation. See `docs/guides/emulator-first.md` and
  `docs/HANDOFF-2026-09-17-session64.md`.
- **The njpeg decoder is correct** (session 47). CPU Huffman decode
  (`func_8008B250`) → four `M_NJPEGTASK` (type 4) RSP tasks that decode in place to
  16-bit YUV (`ucode=0x8009ED80`, boot `0x8009ECB0`, tables `0x800AC050`;
  `data_size` = macroblocks, `yield_data_size` = quantization scale; asset
  `0x00183352` = ROM `0x7175A2`, magic `'HU'` + `'HUFF'`) → a second gfx ucode
  `0x800A5110`, which session 51 identified as S2DEX2 and whose per-macroblock
  `0xDA` command is the draw (see the cathedral fact above; session 50's "loads and
  emits no geometry" is wrong) → a CPU framebuffer readback → assembly of a
  `'B5'`-headed image at `0x80243E10` whose pixels at `0x80243E28` the blit reads.
  The microcode is recompiled (`make rsp-recomp` → `RspFuncs/njpeg_ucode.cpp`) and
  runs by default (`OGRE_NJPEG=0` forces the stub). All `mbs` blocks decode in 0–1
  ms. See `docs/HANDOFF-2026-09-15-session47.md` and `-session46.md` (superseded).
  The renderer half is implemented in RT64 (sessions 50/51): the YUV16 tile is
  loaded as the RDP's two-plane format and converted with the game's own
  `G_SETCONVERT` matrix (see the cathedral fact above).
  [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
  ("Missing backgrounds in Ogre Battle 64 battles and also some cutscenes") was
  fixed upstream by the RSP-side `jpeg_decode_OB`, which this port already has. See
  `docs/HANDOFF-2026-09-15-session50.md`.
- **RSPRecomp's `text_address` is a label base, not an address.** It must equal the
  RSP IMEM DMA address that the microcode was assembled for, masked `0x1FFF`. For a
  ucode that the game's boot loader loads at IMEM `0x080`, that value is `0x1080`
  (`addi $7,$0,0x1080` / `mtc0 $7,SP_MEM_ADDR` at ROM `0x2F0C0`), and not the
  text's RDRAM address. The bytes come from `text_offset`. A wrong value rotates
  every `j` target by the difference, and the recompiled microcode silently walks
  the wrong blocks (session 47: `0x8009ED80` → one `0x300`-byte block per task
  instead of `mbs`; the same applies to any future ucode recompilation).
