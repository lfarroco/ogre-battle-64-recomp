# Handoff — 2026-09-17, session 70: **the port's timeline is wall-clock, but its state is not** — input-replay determinism measured

> **Read §1 if you are here for "can we replay an emulator input recording (or a
> savestate) in the port?".** Short answer: an emulator **savestate** is the wrong
> artifact to *load* (guest addresses are identical — that was never the problem —
> but a savestate is mid-frame host state the port has no resume point for); an
> emulator **input recording** is the right artifact, but it must be re-anchored
> **on game state, not on frame or poll index**, because the port's VI retrace
> clock is driven by wall time (`events.cpp:237`). The good news, measured this
> session: **the guest-visible state trajectory is reproducible to the byte** —
> across two runs *and* across `OGRE_SPEED` — so a state-anchored recording will
> replay exactly.

## Goal and result

**Goal (developer), in two steps.** First: *"one option I'm considering is using
an emulator to create a series of save states at specific moments, and have the
game load this save state. I'm not sure if this is possible in this recomp
project, as the memory addresses might be completely different, right?"* Chosen
moments: **world map / menus after the game's own save points**. Then: *"what
about this: emulator saves + input recording? I can provide the game a .srm file
and then run the game with an input recorder flag … then I tell an agent to create
a test: run the game with the save and play the inputs, then assert something."*
Agreed next step: **measure determinism before designing any fixture format.**

**Result: the measurement is done, it is favourable, and it changes the design.**

* **The addresses are the same, not different.** The port runs the ROM's own
  instructions, so guest virtual addresses are identical to an emulator's
  (§2). What does not transfer is *host* state: `OSThread::context` is a real host
  pointer written into guest RDRAM (`ultra64.h:106`, "An actual pointer regardless
  of platform"), and the checkpoint's own `save`/`load` already documents that a
  foreign image crashes in `do_send` (session 65). So an emulator savestate is a
  fine **data oracle** (read RDRAM out of it offline, `emulator-first.md` §5) and a
  bad **execution checkpoint**.
* **For the chosen moments the durable fixture already exists and is
  build-independent: the game's own SRAM save.** `tools/sramsave.py import` takes
  the emulator's `.srm` (session 66), and with a real save the title's `Load Game`
  cursor defaults to it: `0x04 → 0x12 → 0x05` (the map) — re-verified this session
  (§3). Unlike a checkpoint it never needs regenerating: the game rebuilds all of
  its own state.
* **The determinism measurement (§3-§4)** used a temporary probe: a scene-armed,
  poll-counted input script (title → Load Game → map → three cursor steps left)
  plus a state sample of the map's `state` struct at a retrace-anchored point.
  Three runs: A and B identical env at `OGRE_SPEED=4`, C at `OGRE_SPEED=2`.
  - **Input polls are ~1:1 with VI retraces** (`min=1 max=2 mean=1.01`
    calls/retrace). This **corrects session 68 and the console help's "the game
    polls input far faster than 60 Hz"** — the game polls once per retrace; the
    reason a cursor step needs a long hold is that the cursor integrates a small
    delta *per retrace*, not that the poll rate is high.
  - **A and B differ only in the absolute clock.** Boot reached the title 16-19
    retraces earlier in B; every later event kept that constant offset
    (A: title 1843, `0x12` 3078, map 3688; B: 1825, 3059, 3669). Normalised for
    `retrace`/`frame`/`polls`/`t=`, the two runs are **identical down to the byte**:
    map-entry `state` hexdump, `dir=0`, `anim=0`, `statehash80=0x24BD4B741FDB3AB8`,
    and the post-input sample hexdump, `dir=7`, `statehash80=0x1074EC06FA2F0569`.
  - **C (half the clock rate) is byte-identical to A** at map entry and at the
    sample — same hashes, same `dir` — **except `state[+0x108]`, the free-running
    animation tick: 373 vs 374.** So the clock speed shifts the timeline and
    changes nothing substantive; only a free-running counter is phase-sensitive.

**Conclusion for the recording plan.** Because the timeline is wall-clock-coupled
(the VI thread schedules retraces off `ultramodern::get_start()`, `events.cpp:237`)
but the state is stable, **replay must be anchored on game state, not on absolute
frame/poll index**. A recording should be a list of `(state signature, input
segment)` pairs: hold the segment's input until the port's signature matches the
recorded one, then advance; on mismatch, stop and report the first divergence.
That is also self-asserting, so an agent never invents assertions or reads an
image. `§5` is the design; `§6` is what to avoid (asserting on free-running
counters).

## 1. Why an emulator savestate is a data oracle, not a checkpoint

Three independent reasons, in increasing severity:

1. **The function map is host state.** `func_map` (which recompiled body runs at
   each RAM address) is serialised separately by the checkpoint's overlay blob —
   and that blob is already pointer-free (`librecomp/src/overlays.cpp:452-549`
   stores only `loaded_ram_addr`/`section_table_index` and
   `rom_start`/`ram_addr`/`size`/`count`, and rebuilds the map from those records).
2. **Guest RDRAM holds host pointers.** Session 65 measured them: `OSThread::context`
   is "an actual pointer regardless of platform" (`ultra64.h:106`) and the runtime
   allocates it with `new UltraThreadContext{}` (`threads.cpp:371`), storing it in
   guest RDRAM. A checkpoint from another process loads, reports success, and then
   SIGSEGVs deterministically in `do_send` on an N64 thread.
3. **A savestate is the wrong shape to resume from.** The port has no PC. It can
   only re-enter recompiled code at a function-entry boundary where the threads are
   parked (`checkpoint_pause_begin`, `function_trace.cpp:134`). An emulator
   savestate captured at "the cursor is here" is mid-frame; there may be no such
   boundary. Note also that even same-process `load` works partly because the
   parked threads keep their live `recomp_context` on their host C stacks — the
   guest frames are in RDRAM (`ctx->r29 = ADD32(...)`, `MEM_W(0x18, ctx->r29)` in
   the generated code), but the in-flight register set is not in the file.

So: **read** the savestate (extract RDRAM with the vendored
`tools/RT64/src/contrib/mupen64plus-core/src/main/savestates.c`, at guest addresses
via `tools/rdram.py`), **never load** it.

## 2. The address question, precisely

Guest addresses are identical because the recompiled code *is* the ROM's
instructions and the runtime implements the same address map
(`recomp_mem_addr`: KSEG0/KSSEG/KSEG3 and the KUSEG low window). `tools/rdram.py`
and the live console's `console_word()`/`console_half()` both already read dumps by
*guest* address (`addr & 0x1FFFFFFF`, with the runtime's within-word byte reversal
— a logical byte is `addr ^ 3`, a logical halfword is at `(addr & 0x1FFFFFFE) ^ 2`).
The one systemic difference is that the port zero-fills RDRAM at boot, which
matters when *diffing uninitialised regions*, not when loading an image.

## 3. What was run (verification)

The save fixture: `assets/__save-mission-1.srm` (296960 bytes, parallel-n64
wrapping, SRAM at `0x20800`, `byteswapped32`, 22167 non-zero bytes) imported with

```sh
python3 tools/sramsave.py import assets/__save-mission-1.srm \
  "$HOME/Library/Application Support/ogrebattle64/saves/ogrebattle64-us-rev1.bin"
```

The developer's pre-existing save was backed up to `/tmp/ogre-save-before-probe70.bin`
and **restored afterwards** (the fixture is not left installed; re-import with the
command above to reproduce).

Three runs, `./build-app/ogrebattle64 assets/ogre64.z64`, logs in
`/tmp/p70-{a,b,c}.log`:

| run | env | map entry | sample |
|---|---|---|---|
| A | `OGRE_P70=1 OGRE_SPEED=4 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=150000` | retrace 3690 | retrace 4438, `anim=373` |
| B | identical to A | retrace 3671 | retrace 4419, `anim=373` |
| C | A with `OGRE_SPEED=2` | retrace 3500 | retrace 4248, `anim=374` |

A vs B, normalised for the clock, diff **empty** except the total poll count
(4289 vs 4272 = the timeline offset). A vs C: identical except `anim` (373 vs 374).
The probe: a temporary `probe70` namespace in `app/src/sdl_platform.cpp` —
`get_input` for controller 0 was driven by a scene-armed script
(`title: START ×6`, `0x12: A ×4`, `map: LEFT ×3`, each press held/separated in
**polls**, not wall time), the sample fired at `map_state_live_retrace + 120` and
the process then exited through the bounded-run `_Exit` path. **Reverted** with
`git checkout -- app/src/sdl_platform.cpp`, rebuilt, and verified:
`grep -rn probe70 app/src/` is empty and `strings build-app/ogrebattle64 | grep -c probe70`
is 0. No files changed net (`git status --short` shows only the pre-existing
`tools/RT64` submodule dirt: `rt64_gbi_s2dex.*`, `rt64_tmem_hasher.h`, `plume`).

**Scenes/ids in the probe** are from the dispatcher's `D_800E810E`; the route
matches session 68's verified one. `D_800C4BCC` is the VI retrace counter
(incremented by the retrace handler — `RecompiledFuncs/funcs_7.c:17343-17350`,
`lw/sw 0x4BCC`; session 15's `0x29A (retrace)`); `D_800AEFA4` is the game's frame
counter (written by `func_80072398` at `0x800726C4`, session 60), and it tracked the
retrace counter (`frame ≈ retrace - 2`), so the frame pump was alive in every run.

## 4. What the measurement says about an emulator recording

If the port's input polls are ~1:1 with VI retraces, an emulator movie's per-frame
controller state maps onto **one poll per frame** with no resampling — a real
simplification, and the opposite of what the "far faster than 60 Hz" note implied.
What cannot be reused is the movie's **absolute frame index**: the port reaches the
same state at a different absolute frame every run (a 16-19 retrace boot offset
here), so a frame-indexed replay desynchronises. Convert a movie into
**state-anchored, poll-counted segments** instead:

* *state anchor*: a signature over named guest globals (scene `D_800E810E`, step
  `D_8018F1C0`/`F1C2`, pending `D_800C4C26`, the map's `state[+0x00..0x7F]`) —
  computable on both sides because the addresses are the same;
* *segment*: buttons + stick + a **poll-counted** duration, armed when the
  signature matches.

That is exactly the probe's model with the anchors made explicit, and it is the
same change that fixes the flakiness the developer started from: wall-clock tap
schedules (`OGRE_TAP_*`) become state-anchored input.

## 5. The recorder design this implies (not built)

* `OGRE_INPUT_RECORD=<file>`: from `app/src/sdl_platform.cpp:get_input` (the app's
  input callback — **not** the `tools/N64ModernRuntime` submodule, AGENTS §6),
  append `(signature, buttons, stick_x, stick_y, polls)`. Record at the N64 level
  so any driver (keyboard, pad, console `press`, or an emulator import) can feed
  the same file.
* `OGRE_INPUT_REPLAY=<file>`: hold the current segment until the live signature
  equals the recorded one, then advance; on mismatch, print the first divergent
  word and fail. Version the format like `kCheckpointVersion`.
* Pair every recording with its start state (record from "boot with this `.srm`"
  so the fixture is self-contained). The `.srm` + the recording is the fixture; a
  checkpoint, if wanted for speed, stays a same-process cache.
* Assertions come from the recorded signatures plus explicit `k` checks — no image
  analysis.
* An emulator recording can be imported by emitting the same format from a
  memory-logging script / periodic savestates, since the signature function and
  the addresses are shared. Do that only for moments the port cannot be driven
  because it is visually broken — the one real argument for emulator-side capture.

## 6. Assertion hazards found

* **`state[+0x108]` is a free-running animation tick** (session 64 read it as
  `f = ((state[+0x108]*0xAAAAAAAB)>>34)&3`). It differed by one between `OGRE_SPEED`
  2 and 4 while everything else matched, so a test must not assert on it (or must
  allow ±1). Assert on scene/step/`dir`/the state header instead.
* **Absolute indices of any kind are not stable** (§3): retrace, frame, poll index
  and wall time all carry a variable boot offset.
* A checkpoint is invalidated by *every* rebuild today
  (`executable_fingerprint`, `app/src/sdl_platform.cpp`), which is coarser than
  necessary — session 64 already recorded the fix as a lead (fingerprint the bank
  record table + recompiled function address list instead). Note the direction:
  for regression testing you *want* a state captured before a fix to load under
  the fixed build.

## 7. Files changed, probes used and reverted

* **Files changed: none net.** `app/src/sdl_platform.cpp` carried the `probe70`
  namespace and its three call sites (init in `configure_automation`, the
  controller-0 branch in `get_input`, the exit hook in `pump_sdl_events`); all
  reverted with `git checkout`, rebuilt, and verified absent from source and
  binary.
* **New: this handoff.** `PLAN.md` (a session-70 entry), `docs/README.md` (the
  index row, and the session-68 blurb's "far faster than 60 Hz" clause corrected
  in place), `docs/guides/app-build.md` (the console `press` row), and
  `docs/DECISIONS.md` (a durable-decision row plus a session-70 section — note it
  lives at `docs/DECISIONS.md`, which is what AGENTS rule 10 means).
* No ROM, asset or save is committed; the imported SRAM fixture
  (`assets/__save-mission-1.srm`) was already present and untracked, and the
  developer's save was restored from
  `/tmp/ogre-save-before-probe70.bin`.

## 8. Open questions

1. The 16-19 retrace boot offset: is it the boot's own variable progress before the
   game installs its VI handler, or the VI thread's phase at `osViSetEvent` time?
   If the latter, a fixed phase would make even absolute indices reproducible.
2. Does a *long* journey (>1 min of input, many scene changes) stay byte-stable, or
   does the offset accumulate a divergence? The measurement covers ~20 s and two
   scene transitions.
3. The animation tick at `state[+0x108]`: which clock drives it? If it is the
   rendered-frame count, it is inherently phase-sensitive and should be excluded
   from signatures.
4. Should the port offer a genuinely clock-independent mode (VI retrace driven by
   the guest's own frame completion rather than wall time)? That would make raw
   frame-indexed emulator movies replay directly, at the cost of frame pacing.
