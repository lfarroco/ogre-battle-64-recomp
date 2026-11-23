# AGENTS.md — rules for AI agents working on this port

This repo reverse-engineers and recompiles one N64 ROM, across many sessions and
many agents. The most common failure in it is an agent inferring intent from code
and then building a whole session on that inference. Read this file first.

Companion docs:

- `PLAN.md` — status and roadmap.
- `docs/README.md` — doc index.
- `docs/reference/README.md` — canonical facts and the address registry: memory
  map, streamed banks, GBI microcodes, the scene script and dispatch, save,
  assets, runtime and the glossary. `make docs-index` renders it; `make
  docs-check` verifies the tree; `make docs-find Q=0x…` searches it.
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
  - `debug/probes/scene.cjs --route tutorial-field` — reach a named browser
    screen (title, mission briefing, tutorial field, forced tutorial) and capture
    it, one PNG per `ogre_gfx_debug_flags` value, with sampled pixels, the page
    log and the scene timeline. Routes are data in the probe; add one instead of
    writing another throwaway Playwright script. See
    `docs/guides/web-probes.md` → "Capturing a screen".
  - `debug/probes/audio.cjs` — measure the browser's audio in two places: the
    game's PCM in the wasm ring (a 25 ms watcher, because one 552-frame buffer
    drains in 17 ms) and the worklet's own output levels (`ogreAudio.levels()`,
    accumulated on the audio thread after the gain and mute). It also requires
    mute to be silent while the ring keeps draining. Audio is frame-paced, and
    headless SwiftShader slows the frames to 2-3 buffers/s; use `--headed` for
    the real rate. See `docs/guides/web-probes.md` → "The audio check".
  - In the browser, `Module._ogre_guest_rdram(0)` reads guest RDRAM from the page
    and `Module._ogre_debug_threads()` dumps the call chains, running queue and
    scheduler ring. Use them when the page still responds but the game stops
    drawing: the canvas keeps the last frame and `tasks=` stops advancing. See
    `docs/guides/web-probes.md` → "When the page keeps responding but the game
    stops drawing". Session 202 read the scene-0x02 njpeg fault this way.
- **For "what is in RAM at the moment X happens", use the live console, and not a
  bounded run's exit dump.** An exit dump is always too late (session 56). The
  console is off unless the run asks for it, because a command writes guest RAM:
  set `OGRE_LIVE_CONSOLE=1` (or name the trigger's own variable —
  `OGRE_CONSOLE_FILE`, `OGRE_CONSOLE_AT_MS`, `OGRE_KEY_<n>`), start the game and
  write commands into the watched file
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
- Scene forcing (`OGRE_SCENE=…`) stores its request at a scene change, so it lands
  when the boot's first scene hands over (~2.7 s at 8x, ~6.4 s at 1x), not at
  1.1 s. A forced scene enters without the pre-state a natural path builds, so
  "forced X crashes" does not mean "X is broken".
- No repo test harness exists. Each check is a ROM-dependent 12–170 s game run.
  Disclose that; do not invent a framework.

## 10. Leave the tree and the record clean

- Per session: update `PLAN.md`'s **Status** and **Open work** lists (the
  session-by-session log is `docs/STATUS-LOG.md`, and the per-session detail goes
  in the handoff), write `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, and add the
  decision to `docs/DECISIONS.md` when a choice was made.
- A durable fact gets a home in the reference layer, not only a paragraph in the
  handoff. Add the address to `docs/reference/addresses.tsv`, put the mechanism
  on the page that owns it (`docs/reference/`, `docs/scenes.md`,
  `docs/symbols.md`), then run `make docs-index && make docs-check`. A fact that
  lives only in a handoff is invisible to the next session.
- The handoff must state the goal, the result, the instruction-level evidence,
  what was run for verification, the files changed, and which probes were used and
  reverted.
- Never commit ROM dumps, extracted assets, or changes inside `tools/`
  submodules. Vendored code is gitignored; list it explicitly in the handoff.
- `git status --short` before you finish must show only intended changes.

## 11. Do not drive the game yourself; ask the developer

The developer plays the game and drives it to the state a bug needs. Ask them for
that state instead of scripting a route to it.

A scripted route costs more than it looks. Session 130 spent two runs and a long
stretch of wall clock trying to tap the opening into a mission, and the app on
this machine reached 5.9 s of game time in 70 s of wall clock with tracing on, so
the taps landed in the wrong scene and both runs were discarded. The developer
reached the same state in one attempt.

- When a bug needs a played state, say which state and let them reach it. Ask for
  the state in their terms ("the cutscene where the knight's head goes black")
  and attach whatever identification helps (a screenshot, the scene, the save).
- A save is their instrument too. `OGRE_SAVE=<path>` loads a battery image, and
  session 130's investigation ran on their `pre_ogre.bin` and `scene_7.bin`
  copies rather than on a tap schedule.
- Scripted taps (`OGRE_TAP_*`) stay for routes that must be exact and short, such
  as a known boot sequence in CI. They are not the tool for reaching a scene the
  developer can load from a save or walk to in seconds.
- Ask for the artefact you need in a form you can read yourself (see rule 12),
  and read it from disk instead of asking them to copy terminal output.

## 12. Instrument the game with debugging shortcuts, and have the developer press a key

A bug that appears in a played state is investigated by instrumenting the run and
**pressing a key at the moment of the bug**, not by parsing a log of the whole
session afterwards.

`OGRE_TEX_TRACE` (session 130) is the worked example of the shape to build:

- **A key arms the capture.** `OGRE_TEX_TRACE_KEY=<1..9>` takes the capture on
  the press, and `OGRE_TEX_TRACE_MS=<n>` closes it by itself, so one key press
  captures the bad frame and a second of context and nothing else. A capture that
  cannot be stopped is a capture of the whole run: session 130's first version
  re-armed after every window and produced a 7.6 MB log from a 2 s capture.
- **A key writes a file, so nothing is copied out of a terminal.**
  `OGRE_RAM_KEY=<1..9>` (`_ADDR`, `_LEN`, `_OUT`) writes guest RDRAM to
  `<OGRE_PREF_DIR>/ram-<n>.bin` and `.txt`. Read those files from disk; do not
  ask the developer to paste console output. Asking them to drive and then paste
  cost this project several rounds in session 130.
- **Answer offline what the log can answer.** A fingerprint in the log beats a
  dump per frame: `OGRE_TEX_TRACE_IMAGES` prints each bound image once with a
  nonzero-byte count, and the `OGRE_RAM_KEY` text file ends in a per-slot
  summary that marks an empty slot. Session 130 found the fault from a `mean 0.0`
  and a row of zero bytes, with no further run.
- **Filter at the source.** `OGRE_TEX_TRACE_MIN_PX` and the per-image dedup cut a
  frame from 28 rectangles and 90 image binds to a handful of lines.
- **Bracket the bug.** Take a dump on each side of the change (before and after a
  pose, before and after a transition) so a single run answers "empty from the
  start" against "emptied later".
- The rule for probes still holds (rule 6): tag them, revert them with
  `make recomp && make bank-recomp`, and prove the revert. A diagnostic that is
  part of the port's toolkit (`OGRE_TEX_TRACE`, `OGRE_RAM_KEY`) is not a probe
  and stays.

## 13. Start debug runs at the title screen

`OGRE_SCENE=title` boots straight to the title scene `0x04` instead of waiting
out the boot intro and the publisher stills. Verified in session 213:
`forcing scene 0x04: requested 0x8004 while 0x09 runs`, then
`forced scene 0x04 is active` at `t=6353ms` (1x).

- `OGRE_SCENE=<name|hex>` uses the scene table in `app/src/bank_overlays.cpp`
  (`title`, `intro`, `publishers`, `story`, `unit-info`, `tutorial`, `menu`,
  `new-game`), and a hex id works for a scene with no name.
- Pair it with the state the bug needs: `OGRE_SAVE=<path> OGRE_SCENE=title` lands
  on the title with the battery already loaded, so the run reaches Load Game and
  the save's scene without replaying the boot.
- Keep the diagnostics that make the run readable: `OGRE_SCENE_LOG=1` (one
  `[scene]` line per change) or `OGRE_SCENE_TRACE=1`.
- A forced scene enters **without** the pre-state a natural path builds (rule 9),
  so a forced scene that misbehaves is not by itself a finding. Use it to reach a
  state faster, and say in the handoff when a result came from a forced entry.

## 14. Launch every game run with no audio

Set `OGRE_NO_AUDIO=1` on every run of the game, in scripts and in the commands
this file records. It skips opening an output device entirely
(`app/src/sdl_platform.cpp`), so a test run does not take the machine's sound
device and does not make noise while a session works.

This applies to debug runs, capture runs and scripted runs. Put it in the script
that launches the run rather than retyping it, and write it in any example
command line.

The audio microcode still runs; only the output device is skipped, so a result
that depends on the game's audio task is unaffected.

---

## 15. Build the browser target before finishing a change it touches

`make web` builds `build-wasm` with Emscripten. The browser target is a separate
build directory that no other command touches, so a source file both builds
compile and that includes a native-only header fails only here, and the failure
is invisible until someone tries it. That has happened twice: `bank_overlays.cpp`
(session 109) and `ram_key.cpp` (session 138), both including `sdl_platform.hpp`
for `console::`.

- Run `make web` before finishing any change to a file in `app/CMakeLists.txt`'s
  Emscripten source list, and to `app/src/web_renderer.cpp`,
  `app/src/web_platform.cpp`, `app/web/*` or `app/src/gbi.cpp`.
- A shared source must not include a native-only header. Guard the include and
  its call sites with `#ifndef __EMSCRIPTEN__`, as `bank_overlays.cpp` and
  `ram_key.cpp` do.
- `.github/workflows/wasm.yml` compiles the target in CI, guarded by
  `vars.OGRE_DATA_REPO` (the private bundle supplies the generated code).
- The browser build takes the environment knobs from the page's query string
  (`OGRE_SCENE`, `OGRE_TAP_MS`, `OGRE_TAP_BUTTON`, `OGRE_S2D_TRACE`, ...), so a
  scene is reached with a URL instead of a throwaway Playwright script. See
  `docs/guides/app-build.md` -> "The browser build reads the same environment
  knobs".

---

## Verified facts

The detail is in `docs/reference/`; read [`docs/reference/README.md`](docs/reference/README.md)
before relying on memory. This list is the subset that changes how a session
works.

- **The port zero-fills RDRAM at boot; retail does not.** A register or stack
  slot that nothing wrote still holds the game's own leftover value. A zero where
  retail has a value is a lead to the address map and the runtime bridges, and to
  the bank map before it is a game-logic bug. See
  [`docs/reference/memory.md`](docs/reference/memory.md).
- **Vocabulary is `docs/symbols.md`; intent is `docs/scenes.md`; mechanism is
  `docs/reference/`.** Read the first before naming a function, the second before
  deciding what a screen should show, and the third before deriving how a
  subsystem works.
- **A swappable RAM range is never compiled into the unit whose code calls into
  it.** Both banks of an arena go to other units, so the call compiles as
  `LOOKUP_FUNC` and the runtime's DMA-driven bank map picks the resident module.
  A streamed module that is not in the segment table still owns such a range and
  needs the same treatment. See [`docs/reference/banks.md`](docs/reference/banks.md).
- **A display list's GBI comes from the ucode hash, not from
  `OGRE_DL_DECODE` or `OGRE_DL_ANALYZE`.** Those decode every list as F3DEX2. See
  [`docs/reference/gbi.md`](docs/reference/gbi.md).
- **A `jal` delay slot runs exactly once, before the callee.** The duplicate
  N64Recomp emits after `goto after_N` is dead code.
- **A call that abandons the caller's frame stops the frame pump.** The signature
  is `D_800AEFA4` frozen while `D_800C4BCC` keeps counting, with two display
  lists submitted. See [`docs/reference/runtime.md`](docs/reference/runtime.md).
- **RSPRecomp's `text_address` is a label base, not an address.** For the boot
  loader's IMEM `0x080` it is `0x1080`. See
  [`docs/reference/runtime.md`](docs/reference/runtime.md).
- **The VI scans out from `framebuffer + 0x280`, not from the RDP's colour
  address.** Resolve a scanout buffer by containment. See
  [`docs/reference/memory.md`](docs/reference/memory.md).
- **Scene forcing enters without the pre-state a natural path builds.**
  `OGRE_SCENE` stores the game's own request at the next scene change. See
  [`docs/reference/scene-dispatch.md`](docs/reference/scene-dispatch.md).
- **The scene descriptor table, the 39 transition sites and the step table are
  enumerable from the ROM.** `tools/scenemap.py` prints them in about 3 s; do not
  hunt a scene address by hand.
- **Current walls** are `PLAN.md`'s Status and Open work lists. Each screen's
  status is `docs/scenes.md`. A claim that a wall is retail behaviour needs the
  rule 8 evidence.

Three corrections that cost sessions and are easy to repeat:

- `0x802399AC` is an ordinary function entry in `bankRec14c` and a body interior
  of `func_ovlC_80239874` in `bankRec14b`. Check the resident bank first.
- Scene `0x07`'s descriptor is `0x8018FDAC`. `0x8018FB98` is scene `0x10`.
- Step 1073 is a valid table entry, and its command is `-8`, not `6`.
