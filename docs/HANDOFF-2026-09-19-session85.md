# Handoff — 2026-09-19, session 85: the streamed-module hunt is exhausted

**Goal (developer):** *"let's locate code that wasn't recompiled yet, and compile
it"* — specifically a streamed module/bank the port has no record for (the
session-73/82 class), with the developer offering to place a finding in the game
from its callers or its content.

**Result: negative, and now well-evidenced — there is no un-recompiled streamed
module on any reachable path.** Two hand-played runs (~2 h of emulated time,
tutorial incl. mode 2, mission incl. battles, map, organize, Load Game, the
attract routes) plus a forced sweep of every scene found **0 `UNKNOWN module`,
0 `UNCOMPILED streamed record`, 0 `streamed function stub called`, 0
stub-microcode tasks**, and an offline diff of all census DMA pairs found no
module-shaped load the build does not know. Every registered module executed at
least one function. **Nothing was compiled because nothing was missing.**

Two real bugs were found and fixed on the way (both in the diagnostics used for
the hunt, not in the game): an `OGRE_SCENE_TRACE` wild-pointer SIGBUS, and a
missing DMA census on the window-close path. One bug is left open (a teardown
crash after the census dump).

## 1. What was run

| run | command (all `OGRE_DMA_TRACE=1 OGRE_DMA_TRACE_FULL=1 OGRE_SCENE_TRACE=1 OGRE_COVER=<file>`) | result |
|---|---|---|
| **interactive 1** | `OGRE_PREF_DIR=/tmp/ogre-hunt-prefs OGRE_SAVE=last_perfect`, `OGRE_SPEED=1`, played by hand: title → story → title → **tutorial `0x17`** → mission `0x03` → battle banks → Load Game `0x12` → map `0x05` → `0x08` → `0x0D` | 2098 s emulated, **36 records**, 179 177 distinct DMA pairs, **0 hits**, cover `2333/4963 = 47.01%`, **0 modules at 0%** |
| **interactive 2** | resumed the run-1 battery (`/tmp/ogre-hunt2-prefs`), `OGRE_SPEED=8`, played: title → tutorial → **Organize `0x06`** → Load Game → map → mission → battles → `0x11` | 31 records incl. **`U` (`0x1C9020`, tutorial mode 2)**, **`X` (`0x22A250`, combat)** and **`AH` (`0x23A370`, battle setup)**, 171 758 distinct DMA pairs, **0 hits**, cover 2403 executed, 1 module at 0% (`bankRec13`, covered by run 1) |
| **forced sweep** | `build-null`, `OGRE_SCENE=<id> OGRE_EXIT_AFTER_MS=18000`, ids `0x02,0x03,0x05..0x0D,0x0E..0x18,0x1A..0x1D` | every id that force-entered reported **0 hits**; scene `0x06`→`S`, `0x07`→`H`, `0x14`→`AG`, `0x16`→**`I`**, `0x17`→record 18 all loaded cleanly; **`0x0E` never force-entered** (stayed in the attract loop, AGENTS §9) |

The battle scene is a **mission sub-state, not scene `0x0E`**: run 2's log shows
unit `X` (`0x22A250`) and the setup fragment `AH` (`0x23A370`) loading repeatedly
while `active=0x0003` — so the battle path *was* exercised, with 0 hits.

**The detectors are the app's own**, already present before this session:
`[bank] UNKNOWN module rom=… ram=…` (`bank_overlays.cpp`, fires when a DMA lands
on a known module base with an unknown ROM), `[bank] UNCOMPILED streamed record`,
and `[streamed function stub called @ …]` (the runtime's `get_function` miss).
All three are silent across both interactive runs and the whole sweep.

## 2. The independent check: diff the DMA census against the build

`OGRE_DMA_TRACE_FULL=1` accumulates every streamed PI DMA as `(rom, ram)`. All
pairs from run 1 (179 177), run 2 (171 758) and the sweep (42 937) were compared
against `tools/stubmap.py`'s module model (both registration tables: the 44 bank
records and the main unit's section table):

* unknown ROM landing at a **known module base** — **0** (the `UNKNOWN module`
  shape);
* an **arena-shaped** load (`0x200`-aligned, `ram >= 0x80197B90`) whose ROM→RAM
  **delta matches a known arena** but whose base is unknown — **0** (the
  session-73/82 / record-16 shape);
* everything else that lands inside a known module's RAM span is an **asset or
  scratch copy** (the record-14 arena and scene `0x0F`/`0x16` buffers are used as
  DMA targets), not code.

Execution coverage is the complement: module residency across the two runs is
the union of 36 + 31 records, and the per-module executed-function counts hit
**0 modules at 0%** in run 1 (session 83's best was two: units T and U, both now
executed). RSP tasks in both runs are only type 2 (`ucode=0x8009E050`, audio) and
type 4 (`ucode=0x8009ED80`, njpeg) — both recompiled — with **0** stub-microcode
submissions.

## 3. Bugs found and fixed (both in the hunt's own diagnostics)

**3a. `OGRE_SCENE_TRACE` SIGBUS on a wild scene-object word — fixed.**
Run 1's first attempt died at t≈94 s with `signal 10`, `host pc
RT64Renderer::update_screen + 0xAE3`, faulting far outside RDRAM. Cause: the
trace block in `app/src/renderer.cpp` read `base = *(0x801B81D0)` and then walked
`base + 0x6C0 + i*0x10` (and `s10 = *(0x801B84AC)`, `s10 + 0x1114`) with no
bounds check; `wptr()` does no validation, so at the **title → story `0x0B`
handoff** — where that word is stale/wild for a frame — the diagnostic walked off
RDRAM. Exactly session 36's `poll_scene` bug, in the diagnostic. Fix: only walk
`base`/`s10` when they are KSEG0 pointers whose whole window fits in the 8 MiB
RDRAM. Evidence it is fixed: a 40 s `OGRE_SPEED=8` run now passes into `0x0B`
(previously the crash point) and exits 0; and run 1's *final* trace printed
`base=0x0806E086` (not KSEG0) and continued instead of dereferencing it.

**3b. The DMA census — and the whole RDRAM image — were lost on window close —
fixed.**
`dump_dma_trace()` ran only on the `OGRE_EXIT_AFTER_MS` path, so a hand-played
session (the intended way to hunt) produced no census. `app/src/main.cpp`'s
window-close path now calls it, exactly as session 83 did for `cover`. Run 1's
179 k-line census exists only because of this change. The same gap applied to
`OGRE_DUMP_RDRAM`: only the timed-exit path and the crash handler wrote the
8 MiB image, so when the credits freeze needed one there was none to read (the
developer asked). The close path now writes it too, and it was verified
end-to-end — launching with `OGRE_DUMP_RDRAM=/tmp/close-dump.bin` and closing
the window produced `[SDL] dumped 8388608 bytes of rdram to /tmp/close-dump.bin`
and an 8 388 608-byte file. **Next natural credits run should set
`OGRE_DUMP_RDRAM` and close the window at the freeze**, so the natural image can
be diffed against the forced one (`/tmp/credits-dump.bin`).

## 4. Open: teardown crash

Both interactive runs end with `[crash] host pc func_8007F8E4 + 0x2F8` (guest
PC) **after** the census dump — the last `dma-trace` line is 532 992 and the
crash is 532 994 — i.e. on window close/quit, not in play (both runs exited 0).
The handler prints only its first two lines and then dies, so there is no guest
call chain. Not investigated; needs its own session (`func_8007F8E4` is main-ELF).

## 5. Verification

* Every detector grep is empty over both `/tmp/ogre-hunt.log` (533 005 lines) and
  `/tmp/ogre-hunt2.log` (53 MiB): `UNKNOWN module`, `UNCOMPILED`, `stub called`.
* `stub microcode` count 0 in both; RSP ucode census printed above.
* Offline DMA diff and coverage scripts as described in §2 (throwaway, not added
  to `tools/`).
* Post-fix crash repro: `OGRE_SPEED=8 OGRE_SCENE_TRACE=1` reaches `0x0B` and
  exits 0.
* No probes were written, so nothing needed reverting; `RecompiledFuncs/` and
  `Bank*Funcs/` were not touched. `tools/RT64` shows the pre-existing modified
  submodule state, not a change from this session.

## 6. Files changed

* `app/src/main.cpp` — `ogre::dump_dma_trace()` and the `OGRE_DUMP_RDRAM` image
  on the window-close path (`<cstdlib>` added for `getenv`)
* `app/src/renderer.cpp` — validate `base`/`s10` before the `OGRE_SCENE_TRACE`
  base-relative reads
* `docs/HANDOFF-2026-09-19-session85.md` (this file), `PLAN.md`,
  `DECISIONS.md`, `docs/README.md`

No game code, no config, no generated C, no submodule change.

## 7. Developer report: the game freezes at the credits (scene `0x11`)

At the end of the second hunt run the developer finished the last mission and the
game **froze on the credits screen**. It is a real freeze, and it is *not* a
missing module — it is the frame/event path, and it reproduces in ~20 s without
the long playthrough.

**Scene `0x11` is the credits:** descriptor `0x8018FBAC`, mask `0x00008000`
(overlay C only), enter `func_80177F80` → overlay C's `func_801AC944` (allocates
3 x 88-byte credit entries, `malloc(264)`), update `func_80177F9C` →
`func_801ACC24`, a 6-state machine on `*(0x801C94D8)` with a jump table at
`0x801CA468` that `jalr`s callbacks and sets `D_800C4C26 = 0xFFFE` when
`func_801ACC24` returns `-1`.

**The freeze, measured.**

* In the natural run the last display list is `#125090 at t=1173789 ms`; the
  scene becomes `0x11` at `t≈1174273` and **no frame is submitted for the next
  13 s** while the VI keeps retracing. The credits window's log slice
  (`/tmp/ogre-hunt2.log` lines 481790..486862) has **0 `[bank] loading overlay
  record`, 0 `UNKNOWN module`, 0 `stub called`** — nothing is streamed and no
  dispatch misses.
* Session 60's signature checks out exactly: `D_800AEFA4` (frame counter)
  **frozen at 91/96** while `D_800C4BCC` (VI retrace) runs to 16 704.
* The frame pump is **`func_8008AFE0`**, an `osCreateMesgQueue(0x800C4C28, 8)` +
  `osRecvMesg` loop that registers three event types (`func_80089054`) and
  `jalr`s the callbacks at `0x800AA090`/`0x800AA094`. At the freeze **t4 is
  blocked in `osRecvMesg(0x800C4C28)`** (both runs; in the natural run its
  snapshot is literally `t4 BLOCKED on recv of queue 0x800C4C28`).
* It is **not** a missing retrace: with `OGRE_DEBUG_TRACES=1 OGRE_DEBUG_VI=1`
  the run shows **1160 `[vi-debug] retrace -> mq=0x800E8B84 msg=0x0000029A`**
  deliveries, and t19 (`func_80088F08`) is the thread parked on that queue — so
  the VI event reaches the game and stops *between* t19's retrace handler and
  the frame pump's queue `0x800C4C28`. `osViSetEvent` is called once and
  **`osViSwapBuffer` only twice in the whole run**.

**Fast repro.** `OGRE_SCENE=0x11` (forced entry, no ending pre-state) freezes
with the same signature — frame counter stuck (91) while retrace counts to
16 704+, 0 display lists after boot, 0 bank loads. Caveat (AGENTS §9): forced
entry has no ending pre-state, so it is the *same observed symptom* but not yet
proven to be the identical code path; the natural repro needs a checkpoint.
There is no checkpoint yet because the game's only battery write in the run was
at the map (`t≈320575 ms`), long before the credits.

**A/B, identical trace, 20 s, only the scene differs** (`OGRE_SCENE=0x04` vs
`0x11`, `OGRE_DEBUG_TRACES=1 OGRE_DEBUG_VI=1 OGRE_PROFILE=1`):

| scene | VI retraces | `osViSwapBuffer` | display lists | `D_800AEFA4` |
|---|---|---|---|---|
| title `0x04` (working) | 1159 | **562** | 64 | **1153** (tracks retrace) |
| credits `0x11` | 1160 | **2** | 3 (boot only) | **91**, then frozen |

So the retrace is delivered at the credits, but the game **stops calling
`osViSwapBuffer`** there; the frame counter cannot advance without a swap, and
the pump stays parked. That is the whole freeze, and it is upstream of the
renderer (0 stubs, 0 unknown modules).

**Control — forced entry itself is not the cause.** The developer asked whether
the freeze is really "the forced scene has no natural game data". Two controls
say no:

* **A second forced scene under the identical trace renders normally.**
  `OGRE_SCENE=0x14` (the Witch's Den), same `OGRE_DEBUG_TRACES=1
  OGRE_DEBUG_VI=1 OGRE_PROFILE=1`, same 20 s: **1158 retraces / 533
  `osViSwapBuffer` / 61 display lists / `D_800AEFA4`=1152**. Forced `0x11`, same
  conditions: 1160 retraces / 2 swaps / 3 display lists / 91 frozen. Forced
  entry does not stop frames by itself.
* **The credits' own data is set up in the forced run.** `OGRE_DUMP_RDRAM` at
  the freeze (`/tmp/credits-dump.bin`) reads the addresses with the correct
  wrap (`lui 0x801c` + a negative immediate lands at `0x801B….`, *not*
  `0x801C….`): state pointer `*(0x801B94D8) = 0x801BA8A0`, state
  `*(0x801BA8A0) = 4`, count `0x801BA5FA = 3`, entry array
  `*(0x801BA704) = 0x801BA770`, and the three 88-byte entries are initialised —
  entry 0 has index 1 and the callback `0x801ABCF4` at +8, entries 1/2 carry
  indexes 2 and `-1`. The state machine has already run 0 → 2 → 4, so the
  credits enter and several updates executed. Nothing here is missing.

The entry *contents* could still differ from a natural run (the forced entry
skips the ending), so a natural dump is still worth having — but the frozen
frame loop is **credits-specific**, not a forced-entry artifact.

**Developer's observation, and what it settles:** in the natural run the screen
was frozen showing the **"Ogre Battle 64" logo in the middle**; in the forced
runs it is **black**. Both are the same fact: the credits (scene `0x11`) submits
no display list, so the VI keeps presenting the *last* frame from the previous
scene — the ending's final logo beat in the natural run, the black pre-force
frame in the forced one. It is not a content difference in the credits itself.

**Not an input wait.** A button press at the freeze changes nothing: with the
live console, `press start 60` then `press a 60` (the console logged
`pressing start for 96 poll(s)`, `pressing a for 96 poll(s)`) produced no display
list, no swap and no frame. The console's `c` reports the frozen state directly:
`scene=0x0011 pending=0x0011 desc=0x8018FBAC mask=0x00008000 step=0 next=0x0000
spin=0xFFFF`.

**Next diagnostic for the credits** (cheap because the repro is 20 s): follow the
retrace from t19's handler (`func_80088F08` / the hot `func_800891A0`) into the
event registry list at `0x800F9178` (`func_80089054` links nodes there) and see
which registered queue the frame event stops reaching, then compare the same
walk in a working scene (the title) with `OGRE_DEBUG_TRACES=1`. Record a
checkpoint just before the credits for the natural case
(`OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_CMD='save /tmp/pre-credits.ckpt'`).

## 8. Next steps

1. **The streamed-module question is closed for reachable paths.** Do not re-run
   the hunt as a module hunt; a *new* module can only come from a route neither
   run nor the sweep took (none is known), or from a game version/region change.
2. **The only "not compiled" backlog left is static and latent**: `stubmap`'s
   **129 missing-function candidates** (a bank opens with a prologue at the
   address but has no entry — a split the disassembler could not see) and the
   **288 indirect `jalr` sites**. Neither fired a single stub in ~2 h of emulated
   play + a full forced sweep, so they are *not live*; a future session should
   treat them as a static-hardening pass (add `symbol_addrs`, recompile, require
   `elfcheck` byte identity) and never as a bug hunt.
3. **The teardown crash** (`func_8007F8E4 + 0x2F8`, §4) is reproducible in
   seconds (open, close) and is the cheapest remaining real crash.
4. **Hunt ergonomics:** use `OGRE_SPEED=8` (developer request) for hand-played
   hunts; the game's longest screens are otherwise painful. Reusing the previous
   run's `OGRE_PREF_DIR` battery resumes progress, so a hunt does not replay the
   tutorial. Note `OGRE_SPEED` scales the emulated clock, so wall-clock tap
   schedules shift.
