# AGENTS.md — rules for AI agents working on this port

This repo is a long-running reverse-engineering / recompilation project: many
sessions, many agents, one ROM. Most wasted time in it came from an agent
**inferring intent from code** and then building on that inference for a whole
session. Read this before you start, and follow it even when it feels redundant.

Companion docs: `PLAN.md` (status + roadmap), `docs/README.md` (doc index),
`DECISIONS.md` (decision log), `docs/HANDOFF-*.md` (per-session record, newest
number wins), `docs/guides/app-build.md` (build/run + every `OGRE_*` knob).

---

## 1. Ask the developer what the scene should display — don't infer it

You can read the assembly; you cannot know what the *game* is supposed to show.
The developer has played it. Treat them as the oracle for **intent**, and use the
code only for **mechanism**.

> **If you find an unusual data structure, a table you cannot classify, or a
> branch you cannot name, stop and ask the developer what the current scene
> should display or do before you build a theory on it.**

This is not a formality. Real examples from this project:

- Sessions 40/41 concluded from branch structure that the `0x02`↔`0x0D` "loop"
  was the game's own scripted attract loop and that the crash was retail
  behaviour. The developer's knowledge of the opening (short intro movie → name
  entry → questions) is what exposed that this is the New Game **sequence** and
  that the loop is the sequence player.
- Session 41 described `func_80178568` as scene `0x0D`'s per-frame update; it is
  the *enter*. Session 42 found the descriptor lifecycle (`enter/update/leave`)
  and the hardcoded accessor table `D_800AF028`, which changed what the selector
  logic means (once per visit, not per frame).

Ask early, ask in batches, and always attach the evidence plus your candidate
answers so the developer only has to pick:

> "Scene `0x0D`'s enter takes two branches on `D_8018F1C0`: `0` loads records
> 10a/10b and installs a vtable at `&D_801E5AC0`; non-zero runs
> `func_80226FA8` and sets a selector. Which one is the intro movie, and should
> the first visit be step 1 or step 0?"

Useful questions: *What should this screen show? Is this a movie, a form, a menu
or a cutscene? Which button/flow gets here? Does the real game reach this? What
text/appearance should the player see? Is X supposed to happen automatically?*

## 2. Handoffs are hypotheses, not facts

A previous session's prose is a lead, not ground truth. Before repeating a
claim, verify it at instruction level:

- `mips-linux-gnu-objdump -d build/bankC.elf --start-address=… --stop-address=…`
  (or `build/ogrebattle64.elf`, `build/bank{A,C,D,E}.elf`), and the raw ROM bytes
  when endianness/mapping is in question (see the byte-order note in
  `docs/guides/app-build.md`).
- Never trust generated C alone for control flow: the recompiler expands jump
  tables, splits functions, and emits fall-through calls. `BankCFuncs/` and
  `RecompiledFuncs/` are **generated and gitignored** — they are regenerated, so
  a "fact" read there may reflect a probe or an older patch.
- When you correct a previous session, say so explicitly, give the instruction
  addresses, and update the record (`PLAN.md`, handoff, `DECISIONS.md`).

## 3. Verify the consumer before naming a struct, table or field

Do not infer a data structure's layout from the code that writes it. Find the
code that **reads** it and follow that.

- `D_800AF028` is an *accessor table* (scene id → function returning the scene
  descriptor), not a table of update functions. It is hardcoded in
  `func_80075BC0`; the descriptor's words are `+0x00` enter, `+0x04`/`+0x08`
  per-frame hooks, `+0x0C` leave, `+0x10` bank-record mask, called from
  `0x80075F58` / `0x8007265C` / `0x80072544` / `0x8007602C`.
- `D_8018F1C0`/`D_8018F1C2` are written by the scene-script VM `func_80170974`
  (register-relative stores), and read by the scene loader, the `0x0D` enter and
  the title transition. One word, several consumers — see rule 5.

## 4. Mis-binding is this project's most common bug class

Several walls were the recompiler binding a call to the wrong function or the
wrong half of a function. Check these before blaming game logic:

- **Size overrides** in `config.toml`/`config-bank*.yaml`: an override that runs
  past the next function's start makes the recompiler redirect every `jal` into
  it (session 41: `func_801AFC2C` `0x4A8` → `0x4A4`).
- **Fall-through / merged tails**: `func_ovlC_802399AC` has no prologue — it is
  the continuation of `func_ovlC_80239874`. A `jal` into it is real code with a
  frame contract (`sp+0x1EC`, `s0`, the `f` regs) that the caller may not satisfy.
- **Cross-bank fixed-address calls**: records overlap in RAM by design
  (`0x80197B90` holds records 0/1/2/15/17), so a fixed-address call can land in a
  *different* resident bank. `python3 tools/cross_bank.py report` lists sites;
  `dispatch --only …` is wired into `make recomp`. Bank code is compiled into
  `Bank{A,C,D,E}Funcs/` and registered at runtime in `app/src/bank_overlays.cpp`.
- **Register-relative stores/loads are invisible to symbol greps.** A symbol-name
  search for `D_8018F1C0` misses `lui $s5, %hi(…) / addiu $s5, … / sh $a0, 0($s5)`.
  Always grep both the symbol *and* the offset/call sites, and read the function.
- Chunk-DMA records may never be DMA'd at all (`0xD0` gap), and record BSS must be
  zeroed on load (`func_ovlE_…`; see `load_function_bank` and `RAM_END` in
  `tools/gen_bank_funcs.py`).

## 5. Isolate one variable per experiment

An A/B run must change exactly one thing, or the result cannot be interpreted.

- Poking `D_8018F1C0` at scene `0x02` "to test the `0x0D` enter's mode" also
  changed the `0x02` **loader**, which reads the same word — the movie-mode crash
  that followed was the loader's fault, not the enter's. The clean test forced
  the branch inside `func_80178568` instead.
- When you cannot isolate, say so in the handoff and name the confound.

## 6. Probes are temporary; revert them by regenerating, and prove it

- Instrument **generated** code (`RecompiledFuncs/*.c`, `Bank*Funcs/*.c`) or the
  app, never a submodule. Mark every probe with a unique tag (`// probe42`,
  `[probe42]`) so it can be grepped.
- Revert with `make recomp && make bank-recomp` (fast: ~5 s) and rebuild
  `build-null` **and** `build-app`.
- Prove it: `grep -rl probe42 RecompiledFuncs/ Bank*Funcs/ app/` must be empty,
  and re-run the battery. Record in the handoff which files carried probes.
- `OGRE_DUMP_RDRAM=<path>` gives the whole 8 MiB image for offline reads; read it
  with the byte-order rules in `docs/guides/app-build.md` (words are
  little-endian at `addr - 0x80000000`; logical bytes are XOR-3).

## 7. Do not fabricate state to get past a crash

Do not invent a buffer, a flag, a return value or a "safe" branch to make a wall
go away (session 40: *"Do NOT paper over it with a fabricated buffer"*). If a
path only works with state the game never sets, that is a finding — write it down
and say what it implies. A repair is only legitimate when it reproduces what the
game's own code or data does.

## 8. "Retail would / would not do X" needs hardware evidence

You cannot conclude retail behaviour from the port. Formulate both hypotheses and
name the cheapest discriminating experiment. Options, in order of cost:

- A runtime watcher or probe (cheap, in-port).
- A hardware watchpoint on the **correct host address**: print
  `ultramodern::get_rdram_base()` (the crash handler prints it) and watch
  `base + (guest_addr - 0x80000000)`, not an lldb expression evaluated at a bad
  stop.
- An RDRAM dump (`OGRE_DUMP_RDRAM`) to see what the game left where.

If nothing in the run ever writes a slot that a path needs, the path is dead in
the port *and* on hardware for the same reasons — that is a legitimate
conclusion, and it means stop chasing it as a port bug.

## 9. Timing and harness caveats

- `OGRE_TAP_MS` / `OGRE_EXIT_AFTER_MS` / `OGRE_SPEED` are **wall-clock**.
  `OGRE_TRACE_HOOKS=1` slows boot several-fold, so a tap schedule tuned at 1×
  misses the title; re-tune it or tap forever when you need a trace.
- `OGRE_SPEED=n` scales the emulated clock, so a fixed wall-clock tap point moves
  relative to the game.
- Scene forcing (`OGRE_SCENE=…`) only lands before the boot enters its first
  scene (~1.1 s). A forced scene enters without the pre-state a natural path
  builds, so "forced X crashes" does not mean "X is broken".
- No repo test harness exists. Each check is a ROM-dependent 12–170 s game run.
  Disclose that; do not invent a framework.

## 10. Leave the tree and the record clean

- Per session: update `PLAN.md`'s status list (it is per-session and goes stale
  fast), write `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, and add the decision to
  `DECISIONS.md` when a choice was made.
- The handoff must state: goal, result, the instruction-level evidence, what was
  run for verification, **files changed**, and which probes were used and
  reverted.
- Never commit ROM dumps, extracted assets, or changes inside `tools/`
  submodules. Vendored code is gitignored — list it explicitly in the handoff.
- `git status --short` before you finish must show only intended changes.

---

## Load-bearing facts (so you do not re-derive them)

These were wrong or missing in earlier sessions. Verify if in doubt, but start
from here.

- **Scene dispatch**: `func_80075BC0` looks up `D_800AF028[scene_id]()` (ids
  ≥ 0x1F map to index 0). The accessor returns the scene descriptor, whose words
  are `+0x00` enter (once), `+0x04`/`+0x08` per-frame hooks, `+0x0C` leave,
  `+0x10` bank-record mask. `D_800C4C26` is the current/pending scene word
  (`0x8000|id`, plus `0xFFFC`/`0xFFFE` control values).
- **`0x0D`** (`D_8018FC3C`): enter `func_80178568`, update `func_80178954`,
  hook `func_80178B40`, leave `func_80178B7C`, mask `0x40007C14`.
  Its enter has **two modes** selected by `D_8018F1C0`:
  `0`/bit15 → movie mode (DMAs records 10a/10b, installs the cutscene vtable
  `&D_801E5AC0`); non-zero → command mode (DMAs 14a/14b, calls
  `func_80226FA8` → `func_8022683C(-5, F1C0 & 0xFFF)`).
- **`0x02`** (`D_8018FC50`): enter `func_80178920` — a one-frame loader that sets
  next = `0x0D`.
- **New Game** is `title (0x04) → 0x02 → 0x0D → 0x02 → 0x0D → …`, a scripted
  sequence: the VM `func_80170974` writes the step word `D_8018F1C0` and the next
  scene `D_8018F1C2`, 16 script opcodes per visit. The attract loop is a
  different flow (`title ↔ story 0x0B / unit-info 0x0C`) and never enters
  `0x02`/`0x0D`.
- **Known open walls** (as of session 42): command-mode step 2
  (`func_ovlC_8022D1CC` → `jal 0x802399AC` with `s0 = 0` and an unprimed frame);
  movie mode (`func_801AFC2C(0)` → `func_8007A110` with a bogus size because
  `0x8019F794 == 0`); scene `0x17` (needs bank records 17/18 compiled); menu
  `0x18` natural entry. Current status and details: the newest
  `docs/HANDOFF-*.md` and `PLAN.md`.
