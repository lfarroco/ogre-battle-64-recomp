# Next-session prompt — fix the New Game backdrop timing race

Paste the block below into the next session. It is written for a fresh agent
that has the repo but not the conversation. Companion record:
`docs/HANDOFF-2026-09-16-session56.md` (read §1 first).

---

Read `AGENTS.md`, then `docs/HANDOFF-2026-09-16-session56.md` §1 (the whole
section — it has the evidence and the previous session's dead ends), then
`PLAN.md`'s scene-`0x07`/opening bullet.

**Goal:** fix the intermittent **stale-backdrop artifact** in the New Game
opening, and if it is still reproducible, chase the **intermittent
end-of-sequence crash**. Both are open items from session 56.

**What is already known (do not re-derive):**

- The opening runs: title → `0x02` → `0x0D` steps (movie, cathedral) → scene
  `0x07` name form → `0x0D` date-of-birth and personality steps → scene `0x16`.
- The artifact: after a form step, the cathedral backdrop comes back with a
  rectangle showing the *previous screen* (the form). It is **in the game's own
  display framebuffers** (`0x80000400`/`0x80025C00`/`0x8004B400`), so it is not
  an RT64 scissor/presenter bug.
- Session 56's live dumps proved the assembly destination `0x80243E28` received
  a frame that was still showing the previous screen — a **render-vs-readback
  timing race**, not a wrong source selection. The game's own `state[0x64]`
  index and RT64's scratch handshake agree in every dump, and reverting
  `tools/njpeg_readback.py` changes nothing (verified A/B).
- The reference dumps are in `/tmp/ogre-rdram-*.bin`; ask the developer before
  assuming they still exist. Reading them:
  `tools/rdram.py <dump> image 0x80243E28 --width 320 -o /tmp/x.png`, and the
  scratch word is at guest `0x807FFC00` (`+0` colour image, `+4` YUV handshake,
  `+8` njpeg target, `+12` most recent game framebuffer).

**Approach:**

1. Read `app/src/renderer.cpp`'s `waitForGameFramebuffers` and the
   `OGRE_NJ_WAIT_MS` path, and `ogre_sync_framebuffers`; the stage-3 copy is
   `func_ovlE_8019976C` in `BankEFuncs/funcs_0.c` (regenerated —
   `tools/njpeg_readback.py` anchors into it). The defect is that the copy can
   run while RT64 is still rendering/committing the colour image.
2. Design the handshake so the stage-3 copy waits until the image RT64 drew
   into (scratch `+8`) is **committed to RDRAM**, and cannot be recycled
   mid-copy. `docs/HANDOFF-2026-09-16-session56.md` §1 has the half-written
   buffer (`A 0005`) that is the test case.
3. Reproduce with the live console and the developer's tap route; capture
   `dump` before/during/after the transition at `OGRE_SPEED=1`
   (`OGRE_KEY_3='dump' OGRE_SCENE_LOG=1`, see `docs/guides/app-build.md` →
   "The live console"). After the fix, no dump may catch a half-written
   `0x80243E28`, and the display buffers must never contain the form when a
   cathedral step is up.
4. If the crash reproduces, press the bound dump key at it and report the
   crash-handler lines (`SIGSEGV`/`SIGBUS`, faulting host PC, RDRAM base, guest
   address). It has been seen twice by the developer and never in the port's
   own runs, so treat it as a separate, timing-sensitive item.

**House rules that matters here:** probes in generated code must be tagged and
reverted with `make recomp && make bank-recomp`; do not fabricate state to force
a pass (AGENTS §7); verify at instruction level, not from generated C alone;
keep `PLAN.md`/`DECISIONS.md`/the handoff updated; `git status --short` must show
only intended changes. No repo test harness — every check is a ROM-dependent run.

**Deliverable:** a fix or a written, instruction-level explanation of why the
copy cannot be made to wait, plus an updated handoff and, if it is fixed, a
fresh clean-cathedral capture in `docs/proofs/`.

---

## Suggested invocation

If you only want the short version, open with:

> Read `AGENTS.md` and `docs/HANDOFF-2026-09-16-session56.md` §1, then fix the
> render-vs-readback timing race that makes the New Game cathedral backdrop show
> the previous screen. The evidence and the dead ends are in that handoff; use
> the live console (`docs/guides/app-build.md`) to verify before/after.
