# Handoff — 2026-09-18, session 77: the boot's QUEST publisher-logo screen lagged because the RSP worker polled 500 ms per frame for framebuffers that scene never draws

## Goal and result

**Goal (developer):** *"when the game boots … one of the first sections is the
'quest' logo, which is composed of 3 3d 'q' characters. that screen lags badly.
can you check why?"*

**Result:** found and fixed in one line. The boot's **publisher stills** (scene
`0x0A`: "Licensed by Nintendo" → ATLUS → **QUEST**, the 3-colour 3D "q" logo)
were being drawn at **2 display lists/s instead of 30** — a ~16× frame-rate
drop, which is exactly the reported lag. Every frame was stalled 500 ms in
`Application::waitForGameFramebuffers`, the RSP-worker poll session 50 added for
the njpeg readback. It can never succeed on this screen (it looks for three
*fixed* framebuffer addresses the publisher stills never use), and session 57 had
already shown it is not what orders the njpeg copy. The call is deleted; the
scene now runs at the same 30 lists/s as every other screen, with its **duration
unchanged** (16.4 s) — the wait was costing frame rate, not game time.

**Developer follow-up (same day): the fix also removed perceptible slowdowns in
other scenes, not just the publisher stills.** That is the expected shape of this
bug, and it means scene `0x0A` was only its most visible face: the poll's
condition can be satisfied *only* by a scene whose draws create one of the three
njpeg framebuffer addresses, so every scene that does not — most of the game
outside the New Game njpeg backgrounds — paid 500 ms per display list. The
publisher stills were reported first because they are long (~16 s) and the motion
in them is obvious; anything else with a stall had the same cause. A future
session wanting the exact affected set can re-run with the old poll reinstated
(or just `OGRE_NJ_WAIT_MS=500`) and histogram the `OGRE_DL_TRACE` intervals per
scene — the signature is a uniform ~516 ms spacing.

## 1. The measurement: one `grep` names the bad scene

A natural boot with per-submission timestamps (`OGRE_SCENE_LOG=1
OGRE_DL_TRACE=1 OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64`) gives the
scene timeline and the display-list rate:

```
[scene] t=1619ms  id=0x0009 (intro)         30 display lists/s
[scene] t=11087ms id=0x000A (publishers)    ~2 display lists/s   <-- the lag
[scene] t=27553ms id=0x0004 (title)         30 display lists/s
```

Inside scene `0x0A` the submissions are metronomic, not erratic:

```
11316 11832 12350 12865 13383 13899 14408 14932 ... 25266 25782   (ms)
```

— one every **516 ms**, for the whole 16.4 s the scene runs. Every other scene in
the run is 30 lists/s (VI retrace is 60 Hz; the game submits a list per other
retrace). 516 ms is `500 ms + ~16 ms of work`, and the scene's *length* is the
same with and without the stall, so this is a frame-rate bug, not a timing one.

## 2. The cause: a per-frame 500 ms timeout that can never be satisfied

`OGRE_SYNC_TRACE=1` prints the blocking wait (`/tmp/quest-sync.log`):

```
intro:      [njwait] spins=1 found=1 ms=0 fbs=5      (returns immediately)
publishers: [njwait] spins=1941 found=0 ms=500 fbs=4 (full timeout, every list)
```

That is `State::waitForGameFramebuffers` (`tools/RT64/src/hle/rt64_state.cpp`,
called from `app/src/renderer.cpp::send_dl` on the RSP worker): it loops until one
of three **hardcoded** addresses (`0x400` / `0x25C00` / `0x4B400`) exists in
RT64's framebuffer manager with a nonzero size, up to a 500 ms timeout. Those are
the game's njpeg framebuffer table (`0x800A8204`), used by the map / New Game
path. The publisher stills render a **640x480 image** into a different buffer, so
`found` is never set and the poll always runs its full timeout. The game waits for
the RSP task's DP completion, so the whole game loop advances at 2 Hz.

The scene is otherwise healthy: `0x0A`'s descriptor is `0x8018FB84`, enter
`func_80177E78` → `func_801A3A10` (builds the logo objects and spawns the logo
thread), update `func_80177E94` → `func_801A3CA0` (the 17-state sequence at
`D_801BA70C`), hook `func_80177EC0` → `func_801A40EC` (per-object draw callbacks
+ gfx commands), in `streamedC`/overlay C. Its frame rate is the only defect.

## 3. Why it is safe to delete the wait

The wait was session 50's guard for the njpeg stage-3 CPU readback
(`func_ovlE_8019976C`). Sessions 51/52/57 then established it was never doing
that job:

* Session 57 measured it as a no-op in the njpeg path (`spins=1 found=1 ms=0`)
  and recommended deleting it as "dead weight" — it did not know about scene
  `0x0A`, where "once the game's framebuffers exist" is false for the whole
  scene.
* The copy is ordered by the game's own **DP** completion (`events.cpp:432`
  posts it *after* `send_dl` returns) plus `ogre_sync_framebuffers()` (inserted
  before the copy by `tools/njpeg_readback.py`, which waits for the workload and
  writes the rendered buffers back).
* Retail has no such wait, so removing it is the faithful direction.

## 4. Verification

| check | command | result |
|---|---|---|
| A/B, one variable | `OGRE_NJ_WAIT_MS=0` on the pre-fix binary | scene `0x0A` 2 → **30** lists/s; duration unchanged |
| fix | `OGRE_SCENE_LOG=1 OGRE_DL_TRACE=1` on the fixed binary | scenes `0x09` 1188 ms, `0x0A` 10636 ms, `0x04` 26954 ms; **30 lists/s throughout, including `0x0A`** |
| njpeg regression | `OGRE_NJREAD_LOG=1 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1` | 4 stage-3 passes read exactly the documented frames `573FF47B8A5A3279`, `48892A76C362A4BA`, `3F79B6732153B2FD`, `38BCBEBE5B555C3C`, `alt=0` 0/4 stale — unchanged |

The njpeg check is the assertion from `docs/guides/app-build.md` →
"`OGRE_NJREAD_LOG=1`": the four passes of one assembly must read those four
hashes in that order. Logs: `/tmp/quest-lag.log` (before), `/tmp/quest-sync.log`
(the `[njwait]` evidence), `/tmp/quest-nowait.log` (A/B), `/tmp/quest-fixed.log`
(after), `/tmp/njtry1.log` (regression).

## 5. Files changed

* `app/src/renderer.cpp` — deleted the `waitForGameFramebuffers` call (and its
  `OGRE_NJ_WAIT_MS` reader) from `send_dl`, replaced by a comment recording why.
* `AGENTS.md`, `docs/guides/app-build.md` (the `OGRE_NJ_WAIT_MS` row is marked
  **removed**), `docs/guides/njpeg-backgrounds.md`, `docs/DECISIONS.md`,
  `PLAN.md` — record.

**Not changed, deliberately:** `tools/RT64/src/hle/rt64_state.cpp` still defines
`State::waitForGameFramebuffers` / `Application::waitForGameFramebuffers`; it now
has no caller. Removing it means regenerating `rt64-ob64.patch` (a submodule
patch), which this fix does not need, so it is left for the next patch refresh.

## 6. Probes

None. This was diagnosed entirely from existing diagnostics (`OGRE_DL_TRACE`,
`OGRE_SYNC_TRACE`, `OGRE_NJREAD_LOG`, `OGRE_SCENE_LOG`); no generated code or
submodule file was instrumented, so there is nothing to revert.

## 7. Follow-ups / lessons

* **The cheap profile is `OGRE_DL_TRACE=1` plus a per-second histogram of the
  `at t=<ms>` timestamps** — it turned "this screen feels slow" into "this scene
  submits 2 lists/s and every other one submits 30" in one run. Worth reaching
  for before any frame-rate theory.
* **A bounded wait that *cannot* be satisfied is not bounded in practice**; it is
  a fixed per-frame cost. Any timeout-bounded poll added for one subsystem should
  say what makes the condition true, and be gated on the subsystem being active
  rather than run on every display list.
* The publisher stills use a 640x480 image buffer while the game's own
  framebuffer table is 320x240 — if a later session ever wants the wait back,
  gate it on the display list actually targeting `0x400`/`0x25C00`/`0x4B400`,
  not on a global per-frame poll.
