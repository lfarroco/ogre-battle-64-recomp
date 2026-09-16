# Handoff — 2026-09-15, session 53: the cathedral name-entry crash is fixed; the sequence now hands off with step 0

## Goal and result

**Goal (developer, session 53):** continue the cathedral scene (New Game scene
`0x0D` step 2) to the point where the game asks for the player's name. The
reported state was: the scene starts, Archbishop Odiron asks the player's name,
and the game crashes; booting with
`OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 ./build-app/ogrebattle64`, waiting
~10 s and pressing `A` four times reaches the name-entry form.

**Results, both verified on `build-app`:**

1. **The name-entry crash is found and fixed — it was in the port, not the
   game.** `RT64`'s `NativeTarget::copyToRAM` dereferenced a null readback
   buffer: `State::syncFramebuffers` (the OGRE hook that writes RDP render
   targets back to RDRAM before the game's CPU readback) called
   `Framebuffer::copyNativeToRAM` on framebuffers whose write-buffer slot the
   game had already consumed when it retired the workload. §1.
2. **The "cathedral restarts" loop is the `OGRE_STEP` hold itself.** Session 48
   re-applied `D_8018F1C0 = <step>` every frame; the moment the game's own VM
   wrote the next step the hold put the seeded one back, so `0x0D` re-entered
   step 2 forever. The hold is now a one-shot **seed**. §2.

With both fixed, the cathedral runs the full dialogue and hands the sequence
back. **The developer confirmed (session 53) that retail advances to the name
form automatically after the last line — no button.** In the port the sequence
never advances: `D_8018F1C0` (the step) reads 2 through the whole cathedral and
then 0, the scene's per-frame hook re-enters the cutscene engine every frame, and
it dies issuing a **1 GiB `memset`** (`memset(dst, 0, 0x40000000)`) inside the
engine update. The crash reproduces on the null renderer and with the §1 RT64
fix reverted, so it is a **pre-existing** wall, not a consequence of the two
fixes. §3 and §5. **The name-entry form is still not reached.**

## 1. The `copyToRAM` crash (fixed)

Reproduced exactly as the developer described (the crashes are wall-clock
sensitive; see §5):

```sh
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_TAP_MS=2000 \
  OGRE_TAP_BUTTON="none,a,a,a,a,a,a,a,a" OGRE_EXIT_AFTER_MS=20000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
# -> signal 11 (SIGSEGV/SIGBUS) in RT64::NativeTarget::copyToRAM
```

A temporary probe (`probe53`, removed — §6) printed the state at the fault:

```
[probe53] A2086 this=0x7fdf5062c218 index=1 hist=1 count=1
```

`writeBufferHistoryIndex == 1` with a one-entry `writeBufferHistory`: the map in
`copyToRAM` (`tools/RT64/src/render/rt64_native_target.cpp`) walks off the vector.

The write-buffer slots are produced by `NativeTarget::copyToNative` (called per
framebuffer pair from the first loop of `State::renderAndSynchronize`,
`tools/RT64/src/hle/rt64_state.cpp`) and consumed by `copyToRAM` (the second loop,
through `Framebuffer::copyNativeToRAM`). `State::syncFramebuffers` then walks
**every** framebuffer in `framebufferManager.framebuffers` and called
`copyNativeToRAM` on each again. For a framebuffer the current workload did not
touch, `writeBufferHistoryIndex` is whatever the previous frame left (already
past the end), and its `modifiedBytes` is still non-zero, so the guards at
`rt64_state.cpp` `(fb.width == 0) || (fb.height == 0) || (fb.modifiedBytes == 0)`
do not skip it. That is the null dereference.

**Fix (intent-preserving):** `NativeTarget` now records the slot the most recent
`copyToNative` filled (`lastWrittenBufferSlot`, reset by `resetBufferHistory`),
and `Framebuffer::copyLastNativeToRAM` / `NativeTarget::copyLastToRAM` read *that*
slot without advancing the consumer index, returning false when the framebuffer
has not been written since its reset. `State::syncFramebuffers` uses it, so each
framebuffer is still written back, from the pixels the last render produced, and
a framebuffer with no render this frame is skipped instead of crashing.

An `assert(writeBufferHistoryIndex < writeBufferHistoryCount)` was added to
`copyToRAM` so the invariant the two loops rely on is explicit.

## 2. `OGRE_STEP` must seed, not hold (fixed)

`app/src/bank_overlays.cpp`'s `hold_step()` wrote `D_8018F1C0 = step` and
`D_8018F1C2 = 0x8002` on *every* frame while the target scene was active. The
scene-script VM (`func_80170974`, `0x80170acc`/`0x80170adc`: `s5` is set to
`0x8018F1C0` at `0x80170998`) writes the same word when the sequence advances,
so the per-frame hold overwrote every advance and the opening looped on the
seeded step:

```
[scene] seeded step 2 while scene 0x02 runs
[scene] t=2203ms id=0x000D            <- visit 1: cathedral (held at 2)
[scene] t=14598ms id=0x0002           <- leaves, D_8018F1C0 = 0
[scene] t=14674ms id=0x000D           <- visit 2: step 0 -> crash
```

`hold_step()` now seeds once on the first frame the target scene is active and
releases the moment the live step differs from the seed (or after a 1500 ms
window, so a step the game never rewrites cannot pin the sequence). The
per-frame `D_8018F1C2 = 0x8002` write is gone with it. `docs/guides/app-build.md`
and `PLAN.md` were corrected.

## 3. The handoff state (step word + vtable) — measured

With §1 and §2 fixed the cathedral dialogue runs to its end. The last line is
`I shall now complete thy training with an oath to our Mother Berthe.` (the
capture `docs/proofs/native-newgame-cathedral.png` is an earlier line), and
then:

* A per-frame probe on the step word shows `D_8018F1C0` at **2** for the whole
  cathedral, then **0**. It never becomes 3.
* The scene's update `func_80178954` (scene `0x0D` descriptor `+0x04`) ends by
  calling `func_8022770C()` (0xFF = the sequence has ended) and then
  `func_801C8884()`, whose result selects the next scene — `2 → D_800C4C26 =
  0x8002`, `5 → func_80178CB0`, `6 → 0x8006`, otherwise `func_80226FA8`:

```
80178a98: jal 8022770c
...
80178acc: bne v1,6 -> 80178ae4
80178af0: li  v0,0x8002 ; store to D_800C4C26
80178b00: jal 80226fa8 ; the "otherwise" arm
```

* `func_801C8884` loads the vtable pointer at `0x801D0830` and calls its first
  word. In the `OGRE_DUMP_RDRAM` image `*(0x801D0830) == 0x801E5AC0`, whose
  first word is `0x801D85E4` = `func_ovlC_801D85E4`. **`0x801E5AC0` is the
  movie-mode cutscene vtable that scene `0x0D`'s enter installs for
  `D_8018F1C0 == 0`** (AGENTS.md fact list, session 42), while this visit ran in
  command mode (`D_8018F1C0 = 2`). The leading hypothesis is a **stale/incorrect
  vtable pointer** at `0x801D0830` on the command-mode exit path — the exit runs
  the movie engine's update, which is also what §5's crash frame #4 shows.
* The scene's **hook** `func_80178B40` (descriptor `+0x08`) independently
  re-enters the engine every frame while `func_8022770C() & 0xFF == 0xFF`, which
  is where the 1 GiB `memset` of §5 is reached from.

## 4. What was run for verification

```sh
# the developer's shortcut, with the tap schedule a human uses
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_TAP_MS=2000 \
  OGRE_TAP_BUTTON="none,a,a,a,a,a,a,a,a" OGRE_EXIT_AFTER_MS=20000 \
  ./build-app/ogrebattle64 assets/ogre64.z64        # exit 0 (was SIGSEGV)

# the dialogue is progressing (capture per line)
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_TAP_MS=2000 \
  OGRE_TAP_BUTTON="none,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a" \
  OGRE_CAPTURE_PRESENT=/tmp/s53 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=15000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
# present 990 = "I shall now complete thy training with an oath to our Mother Berthe."

# the step word over time (temporary probe53 in poll_scene, removed)
OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_PROBE53=1 ... # step 2 -> 0 at the handoff
```

`cmake --build build-app -j` and `cmake --build build-null -j` are clean. There
is no repo test harness; every check above is a ROM-dependent game run.

## 5. The crash is pre-existing: a 1 GiB `memset` in the cutscene engine's update

The crash is **not** renderer-specific and **not** caused by §1: it reproduces on
the freshly rebuilt **null-renderer** build (`build-null`, same env), and it
reproduces with the §1 RT64 fix **reverted** (A/B: old `rt64_state.cpp` + the new
app seed → same `func_80093380 + 0x45B`). It is game logic.

**Precise signature** (lldb, stopped at the fault):

```
frame #0  func_80093380 + 1115        movl $0x0, (%rbx,%rax)
frame #1  func_ovlC_801B5D78 + 2753
frame #2  func_ovlC_801BB5AC + 1626
frame #3  func_ovlC_801D9300 + 1570
frame #4  func_ovlC_801C88BC + 272
frame #5  func_80178B40 + 300        <- scene 0x0D descriptor +0x08 (the per-frame hook)
frame #6  func_80072398 + 1347
rdi = 0x110063000   rsi = 0x40000000   rdx = 0xa0000000
```

So the call is **`memset(dst, 0, 0x40000000)`** — a *1 GiB* size — inside the
cutscene engine's update, reached from the scene's per-frame hook.

**Correction to this session's earlier reading:** the `func_8007A110` /
`func_8007ACB0` symbolisations were wrong. The crash handler names the function
containing the *return address*, and that return address decodes into
`func_8007ACB0` territory while the faulting instruction is this `memset`. Treat
the lldb frame list as authoritative, not `[crash] host pc`. The wild-`s0` story
in the earlier draft of this section is retracted.

The chain:

* `func_80178B40` (scene `0x0D`'s **hook**, descriptor `+0x08`) calls
  `func_8022770C`; when that returns `0xFF` it calls `func_801C88BC`
  (`0x80178B40`: `jal 8022770c` / `andi v0,0xff` / `li v1,255` / `bne` /
  `jal 801c88bc`).
* `func_801C88BC` goes through the vtable pointer at `0x801D0830`; in the
  `OGRE_DUMP_RDRAM` image `*(0x801D0830) == 0x801E5AC0`, whose first word is
  `0x801D85E4` = the **movie-mode** cutscene update the `0x0D` enter installs
  for `D_8018F1C0 == 0` — although this visit ran in command mode
  (`D_8018F1C0 = 2`).
* That update walks into `func_801D9300` → `func_801BB5AC` → `func_801B5D78` →
  `memset(_, 0, 0x40000000)`.

**The step word does not advance, and the probe says why.** A per-frame probe on
`D_8018F1C0`/`D_8018F1C2` (temporary `probe55`, removed) shows exactly three
values across the run:

```
t=731ms   scene=0x0000 step=0 next=0x0000
t=1877ms  scene=0x0002 step=2 next=0x8002   <- the seed, held through visit 1
t=14612ms scene=0x000D step=0 next=0x8002   <- 0 at the handoff
```

`D_80197B23` / `0x80197B38` / `0x80197B3C` (the "script buffer" triple from
session 43) read **0 at every sample**, so those addresses are not the live
script state in this run — session 43's buffer addresses are not where to look.
A scan of every ELF for the `lui 0x8019` / `addiu -3648` pair finds only two
writers of `D_8018F1C0`: the VM store `0x80170ADC`, and `func_801723B4`
(`0x801723D8: sh zero,-3648(at)`), a teardown that also frees
`D_80197B38`/`D_80197B3C`. The last one to run before the handoff sets 0.

So the engine's update runs on the same visit with the same state and never
advances, and something inside it eventually issues a 1 GiB `memset`. What is
missing is the **advance**: retail moves to the name form automatically after the
last line (developer, session 53), which never happens in the port.

## 6. Timing and tooling caveats

* The handoff crash is **wall-clock sensitive**: runs at 2000 ms taps reach it,
  and the same binary under lldb (which slows the process) does not crash in
  40 s. When reproducing §3, keep the tap period and `OGRE_EXIT_AFTER_MS` the
  same; a `memset`-shaped SIGBUS and an LZ-pointer SIGBUS are both symptoms of
  the same handoff, and which one appears depends on timing.
* **`tools/rdram.py half <addr>` prints the *byte-swapped* view** (use
  `word`/`byte`, or read the raw file, when the question is what the N64
  halfword is). The probe in §3 read the step with the recompiled `MEM_HU`
  macro and is the trustworthy measurement: **step 2 during the first `0x0D`
  visit, step 0 at the start of the second**. The `OGRE_DUMP_RDRAM` image's
  `word` at `0x8018F1C0` was `0x00028002` — step 2, next-scene `0x8002` — which
  is the *first* visit's state, so the dump was written before the handoff
  completed in that run. Do not mix the two.

## 7. Files changed

* `tools/RT64/src/render/rt64_native_target.{h,cpp}` — `lastWrittenBufferSlot`,
  `copyLastToRAM`, the `copyToRAM` invariant assert.
* `tools/RT64/src/hle/rt64_framebuffer.{h,cpp}` — `copyLastNativeToRAM`.
* `tools/RT64/src/hle/rt64_state.cpp` — `syncFramebuffers` uses
  `copyLastNativeToRAM` (with a comment naming the session-53 crash).
* `app/src/bank_overlays.cpp` — `hold_step()` seeds once and releases (the
  `kStepHoldWindowMs` window, the release log).
* `PLAN.md`, `docs/guides/app-build.md`, `docs/HANDOFF-2026-09-15-session53.md`,
  `docs/DECISIONS.md`.
* **No generated-code change and no probe left**: `grep -rn probe53 app/src
  tools/RT64/src Bank*Funcs RecompiledFuncs` is empty; the `execinfo.h`
  backtrace probe was removed with the rest.

## 8. Next leads, in order

1. **Why `func_8022770C` never stops returning `0xFF`, and why the command-mode
   exit dispatches the movie vtable.** This is the advance that is missing.
   `func_80178B40` (scene `0x0D`'s hook) re-enters the engine every frame while
   `func_8022770C() & 0xFF == 0xFF`, and `func_801C88BC` dispatches
   `*(0x801D0830)` — measured as `0x801E5AC0`, the movie-mode vtable. Watch
   `0x801D0830` across the visit (`tools/watch.sh 0x801D0830`) and check what the
   command-mode enter (`func_80226FA8` → `func_8022683C(-5, F1C0 & 0xFFF)`) is
   supposed to install there.
2. **Find the 1 GiB `memset`.** Break on the guest `memset` at the site in
   `func_801B5D78` (frame #1) and walk back to the local that supplies the size;
   1 GiB is not a game-authored constant, so the size is being computed from
   corrupted state. `func_801B5D78`'s other `memset` calls are all
   `memset(ptr, ?, 16)` against the bump pointer `0x80197198`, so this one is
   off its normal path.
3. **Reproduce under lldb.** The crash does **not** happen under lldb (25 s run
   exits 0) even though it is deterministic at 4x/8x/16x without it. Break on
   the guest `memset` **with a condition** (`$rdx == 0x40000000`) rather than
   single-stepping, or attach to a running process, to keep the timing.
4. **Do not trust `[crash] host pc`.** It names the function containing the
   return address; the lldb frame list is authoritative (§5).
