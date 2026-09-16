# Handoff — 2026-09-16, session 54: the New Game opening advances to scene `0x07`; its form module is not in RAM

## Goal and result

**Goal (developer, session 54):** continue from session 53 — the cathedral (New
Game scene `0x0D` step 2) still did not hand off to the name-entry form. The
developer's direction this session: *"start the game at title scene, press start
then a every 1 s"* — do not build machinery to drive the game.

**Results:**

1. **The opening advances.** With `OGRE_SCENE=title` and
   `OGRE_TAP_MS=1000 OGRE_TAP_BUTTON="start,a,start,a,…"`, New Game is entered,
   the step-1 movie plays, the step-2 cathedral dialogue is advanced with `A`,
   and the sequence **hands off to scene `0x07`** (`t≈17.9 s` in a 4× run).
   Session 53's report that the handoff *crashes* was an artefact of the
   `OGRE_STEP=2` seed: skipping the movie leaves the sequence with no step 1, and
   the exit then dispatches the movie-mode vtable and dies in a 1 GiB `memset`.
   Driving the opening normally, that wall is not reached. §1. **The step is
   *not* the problem: it advances 1 → 2 by itself and the exit resets it to 0
   with `next = 0x0007`.**
2. **Scene `0x07` runs but renders black.** It is *not* a crash and not a stall
   in the dispatcher: the scene's own enter (`func_80177F04`, descriptor
   `D_8018FB98`) either does not run its body or finds its state zeroed. It calls
   `func_801A578C`, which lives in **main overlay C's RAM window**
   (`config.yaml` `.streamedC`: ROM `0x1CE040` → VRAM `0x80197B90`), and in an
   `OGRE_DUMP_RDRAM` taken while `0x07` is active **that whole window is zero**
   (§3, §4). New tooling (`OGRE_DMA_TRACE=1`) and a watchpoint locate the
   destruction precisely. §5.

**The name-entry form is still not reached.**

## 1. The New Game route, with no machinery

```sh
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,start,a,start,a,start,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64 assets/ogre64.z64
```

    t=1.83s  title (0x04)      <- a Start summons the menu (developer, session 54)
    t=3.37s  new-game loader (0x02)
    t=3.50s  new-game step (0x0D) step=1   <- the movie
    t=4.51s  new-game loader (0x02) step=2
    t=4.73s  new-game step (0x0D) step=2   <- the cathedral, advanced by A
    t=17.88s new-game scene 0x07           <- the advance, with step reset to 0

`OGRE_PROBE57=1` (this session's probe, left in as a knob) shows the handoff:

    t=4522ms  step=1                                             (movie ends)
    t=4560ms  step=2                                             (cathedral)
    t=19905ms step=0  next=0x0007                                (advance)

The engine-exit path is session 53's, verified at instruction level:

* scene `0x0D`'s update `func_80178954` calls `func_8022770C` (`0x80178A98`);
  when it returns non-`0xFF` it falls through to `0x80178AAF` →
  `func_801C8884` (the `*(0x801D0830)[0]` dispatcher).
* the return value is compared against `6`, `2`, `5`; the `5` arm calls
  `func_80178CB0`, which calls `func_801723B4` — and that is the function that
  does `sh zero, -3648(at)` = **`D_8018F1C0 = 0`** (`0x801723D8`) — then writes
  `D_800C4C26 = D_8018F1C2` (`0x80178CD4`/`0x80178CDC`), i.e. the next scene
  `0x0007`.

So the sequence advance is real and the step word resetting to 0 is **game
behaviour on this path, not a bug**. Session 53's "why does the step become 0"
question is answered; its "the exit dispatches the movie vtable" observation is
true only for the seeded run.

### Why session 53's seeded run crashed

`OGRE_STEP=2` enters scene `0x0D` directly at step 2 with no step-1 visit, so the
sequence's own state is not built up. The exit then takes the `otherwise` arm
(`D_800C4C26 = 0x8002`), the loader re-enters `0x0D`, and with the step word 0
the enter takes the **movie-mode** branch (`0x80178740`, `D_8018F1C0 == 0`),
storing `D_801D0830 = 0x801E5AC0` — the movie vtable — after which the per-frame
hook dispatches `func_801D9300` and the engine memset-crashes. Reproduced this
session on the current build:

```sh
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_TAP_MS=2000 \
  OGRE_TAP_BUTTON="none,a,a,a,a,a,a,a,a" OGRE_EXIT_AFTER_MS=20000 \
  OGRE_PROBE57=1 ./build-app/ogrebattle64 assets/ogre64.z64   # SIGBUS, exit 138
```

`[probe57] t=14583ms scene=0x000D step=0 next=0x8002` then
`vt(801D0830)=0x801E5AC0` at the re-entry, then the crash — the seeded shortcut
is the only thing that produces step 0 *with* `next = 0x8002`.

## 2. Scene `0x07` is the name-entry form (developer, confirmed)

The developer supplied a retail (YouTube) screenshot of scene `0x07`: the **name
entry form** — a blue textured backdrop, a name box reading `Magnus` with `_`,
and the character grid `A–Z` / `a–z` with `◀ ▶ INS BS DEL END` and a scrollbar.
So the port's next target after the cathedral is confirmed: `0x07` is step 3 of
the opening, not a fade or a load screen. Its descriptor is

`func_80178CB0` writes `next = D_8018F1C2 = 0x0007`, and the dispatcher enters
scene `0x07`. `D_8018FB98` (main `.streamedB`, ROM `0x065A98`):

    +0x00 enter  func_80177F04   +0x04 update func_80177F20
    +0x08 hook   func_80177F3C   +0x0C leave  func_80177F58   +0x10 0x8000

`func_80177F04` is a one-liner: `addiu sp,-0x18` / `jal 0x801A578C` / `jr ra`.
The update `func_80177F20` calls `func_801A5934`, the hook `func_80177F3C` calls
`func_801A5A54` — all three in the same `0x801A57xx`/`0x801A59xx` block.

The **`0x0D` enter already loads the form's asset data** (as it does for every
step), through `func_80178E80`: `func_8009DAF4(0x019A8804)` →
`func_8009DBB8(buf, id)` → `func_8007A7E0` → `func_8007A110` (the LZ decoder), and
the step index indexes a table inside that asset
(`lw s0, 0(s0)` after `sll s0,2`). The step-2 asset is `0x1F3EAA2`
(`docs/HANDOFF-2026-09-15-session44.md` §1). So the *data* half of the form is
fetched; what is missing is the *code* at `0x801A578C`.

## 3. That block is overlay C's RAM, and it is zero at runtime

`config.yaml` maps `.streamedC` (ROM `0x1CE040`, size `0x229C0`) at VRAM
`0x80197B90`, so `0x801A578C` = file `0x1DBC3C` = `27BDFFC8 AFBF0030 …`
(verified: that byte pattern occurs **once** in the ROM). An `OGRE_DUMP_RDRAM`
taken at the `0x07` handoff:

    nonzero words in 0x801A3000..0x801A6000 : 0
    word at 0x801A578C                      : 0x00000000

The same in a second dump 4 s later. So the scene's enter calls into zeroed RAM,
and every other function it needs (`func_801A5934`, `func_801A5A54`, …) is in the
same destroyed window.

## 4. `OGRE_DMA_TRACE=1` (new): the arena is loaded, then overwritten

The `[bank]` line only fires for records the port knows, so a module the game
loads but the port has no record for is invisible. `OGRE_DMA_TRACE=1` now
accumulates **every** streamed DMA by `(rom, ram)` base with `first`/`last`
event indices, and dumps it from the bounded-run exit path (the process leaves
through `_exit`, so an `atexit` dump never runs). Restrict to overlay C's window
by default; `OGRE_DMA_TRACE_FULL=1` prints every base.

The full trace of a 26 s run is 25 056 events / 16 971 bases. Filtering the
`0x80197B90` arena:

    rom=0x1CE040 ram=0x80197B90 first=952   ...   <- streamedC itself, at boot
    rom=0x066E30 ram=0x80197B90 first=3149  last=5190   <- bankRec0 (unit E)
    rom=0x0E4910 ram=0x80197B90 first=3804  last=5445   <- bankRec2 (unit A)
    rom=0x06E680 ram=0x80197B90 first=16200 last=16200  <- bankRec1 (unit D)

`OGRE_PROBE57`-style polling of the word itself (and a temporary probe58) shows
the sequence in wall time:

    t=1.9s  word(0x801A578C) = 0x27BDFFC8   <- streamedC, as configured
    t=3.4s  word(0x801A578C) = 0x5B695A5A   <- asset data (New Game movie starts)
    t=4.8s  word(0x801A578C) = 0x18181818
    t=17.9s word(0x801A578C) = 0x00000000   <- scene 0x07 entry

**The zeroing write, from an lldb watchpoint on the guest word** (null build,
`--after func_80178920` is not enough — arm it on the word and condition on 0):

    stop  new value 0xafbf003027bdffc8   func_80080998 + 736
      <- func_ovlF_8023BAA4 + 6055 <- func_ovlF_8023BDFC <- func_ovlF_8023BF50
      <- func_ovlC_802282D8 (the step-descriptor interpreter) <- func_ovlC_80227030
      <- func_ovlC_8022D1CC (the step task body) <- func_ovlC_80225A3C ...
    stop  new value 0x0000000000000000   func_ovlC_8022B714 + 3230
      <- func_ovlC_802282D8 + 2117 <- func_ovlC_80227030 <- func_ovlC_8022D1CC

So the block is destroyed by the New Game sequence engine's own step interpreter
(`func_ovlC_802282D8`) while it runs step 2 — the cathedral's task body — and the
final zero is written by `func_ovlC_8022B714`. Both are in `bankRec14`
(unit C), the module scene `0x0D` streams at RAM `0x802258B0`.

**This is an active probe, not a theory:** the trace above is from
`build-app` with `OGRE_DMA_TRACE=1` and a temporary `OGRE_DMA_LOG_ARENA`
ordered log (both removed/kept as described in §7).

## 5. What this means (and what it does not)

* The dispatcher, the sequence advance and the scene-`0x07` entry all work.
  The wall is that **the code scene `0x07` calls is not resident when it runs**.
* Two readings are open and this session did not discriminate them:
  1. **The port never loads the module** the game loads for `0x07` (a module
     whose RAM span is `0x80197B90+`), and the main ELF's `.streamedC` binding of
     `0x801A578C` is a splat artifact of that same arena — the same
     cross-overlay class as session 45's step-2 wall.
  2. **The game does load it and the port's DMA hook sees a *different* record
     into the same RAM** (the four bases above), so the port's bank map and the
     game's own load order disagree.
* The DMA trace is the cheap discriminator: it shows every base the game writes
  into the arena, in order, with no reliance on `config.yaml`. Run it with
  `OGRE_DMA_TRACE_FULL=1` and look for the base whose `ram + (rom - base)` covers
  `0x801A578C` **after** the `func_ovlC_8022B714` zeroing — the game's own
  loader (`func_80076324`, main `0x800765E4`; record-list table
  `0x800B86FC`) is the code to read next.
* Do **not** paper over this by pre-loading streamedC: if the game never loads
  that module for `0x07`, forcing it in fabricates state the game does not set
  (AGENTS §7). The finding to write down is the missing/overwritten load.

## 6. What was run for verification

```sh
# the advance, end to end (RT64 build) — this is the maintained repro
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,start,a,start,a,start,a" \
  OGRE_SCENE_LOG=1 OGRE_PROBE57=1 OGRE_EXIT_AFTER_MS=200000 \
  ./build-app/ogrebattle64 assets/ogre64.z64

# the seeded-run crash (session 53's wall), still reproducible
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_TAP_MS=2000 \
  OGRE_TAP_BUTTON="none,a,a,a,a,a,a,a,a" OGRE_PROBE57=1 OGRE_EXIT_AFTER_MS=20000 \
  ./build-app/ogrebattle64 assets/ogre64.z64        # SIGBUS, exit 138

# the black screen: RDRAM at the handoff, then the DMA history
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,start,a,start,a,start,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=30000 OGRE_DUMP_RDRAM=/tmp/s54-07.rdram \
  ./build-app/ogrebattle64 assets/ogre64.z64
OGRE_DMA_TRACE=1 … (same run)

# the zeroing write
tools/watch.sh 0x801A578C --after func_80178920 --size 4 --hits 2 \
  --value 0 --exe build-null/ogrebattle64   # see §4 for the frame list
```

`cmake --build build-app -j` and `cmake --build build-null -j` are clean. There
is no repo test harness; every check above is a ROM-dependent game run (15–200 s).

## 7. Files changed

* `app/src/bank_overlays.cpp`
  * **`OGRE_DMA_TRACE=1` / `OGRE_DMA_TRACE_FULL=1`** — the all-DMA trace of §4
    (`dma_trace_chunks`/`dma_trace_events`, `dump_dma_trace()`), printed from the
    bounded-run exit path.
  * **`OGRE_PROBE57=1`** — the New Game sequence state (step, next, the
    `0x801D0830` vtable pointer, the engine words). Session 53's `probe55`
    fields are untouched.
  * Both tags are documented in `docs/guides/app-build.md`; no temporary
    `probe53`/`probe55`/`probe58` leftovers. `grep -rn "probe58\|DMA_LOG_ARENA"
    app/src` is empty.
* `app/src/sdl_platform.cpp` — `dump_dma_trace()` from the exit path. The
  session's `OGRE_TAP_AFTER_SCENE` / `OGRE_TAP_AUTO_NEW_GAME` experiments were
  **reverted**: a fixed `start,a` schedule is enough (developer, session 54).
* `app/src/bank_overlays.hpp` — the `dump_dma_trace()` declaration.
* `PLAN.md`, `docs/guides/app-build.md`, `docs/scenes.md`, `DECISIONS.md`, this
  file.
* No generated-code change; `git status --short` shows only `tools/RT64`
  (uncommitted work carried since before this session) plus the files above.

## 8. Next leads, in order

1. **Identify the module scene `0x07` should call.** Read the game's own record
   loader (`func_80076324`, main `0x800765E4`; list table `0x800B86FC`) and the
   `0x07` descriptor's record mask `0x8000`, then confirm against
   `OGRE_DMA_TRACE=1` which base the game loads into the `0x80197B90` arena for
   this scene. If the game issues no such DMA in the port, the port's scene
   loader is dropping the record selection (a port bug); if it does, the port's
   bank map has the wrong owner for that RAM (a session-45-class binding bug).
2. **`0x07` is the name-entry form** (developer screenshot, §2), so the module
   that draws it is the next bank unit to compile (`config-bank*.yaml` +
   `Makefile`'s `BANK_UNITS`), as session 45 did for `bankRec14c`. Add the record
   to a unit whose RAM range it really owns — do not add it to a unit that
   already defines that range (AGENTS §4).
4. **Keep the corrected step picture**: the step is *not* held, the exit resets
   it to 0 and sets `next`, and a seeded `OGRE_STEP` skips the sequence state
   that the exit needs. Any future New Game repro should drive the title instead
   of seeding a step (session 54).
