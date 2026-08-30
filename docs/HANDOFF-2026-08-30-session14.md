# Handoff — 2026-08-30 (session 14): Web shell graphics status fix + boot-stall root cause (message-queue deadlock, not SI_STATUS)

## TL;DR

- **Fixed the web shell's graphics status reporting** (`app/web/web.js`): the
  `#gfx-status` line stayed on "graphics: waiting for the WebGL2 context
  (created on ROM pick)" forever even though the WebGL2 context **is** created
  and handed to the renderer — the success path never updated the text. It now
  reports the live state (context ready → renderer active → display-list count)
  and logs an explicit "known boot-stall" note after 15 s with no rendering
  work, so the black canvas no longer looks like a broken renderer.
- **Boot-stall root cause found — the session-13 SI_STATUS theory is a dead
  end.** With the page status filter disabled, the runtime's `[ev]` trace shows
  a **message-queue deadlock**: the VI-manager thread's retrace fan-out does
  *blocking* `osSendMesg`s to registered handler queues; their consumers stall,
  the queues fill (8/8), the fan-out blocks forever, the VI-manager never
  returns to drain its own retrace queue (`0x800E8B84`), every VI retrace is
  dropped, and the boot never advances. This is platform-independent (native
  shows the same "game threads freeze ~180 ms in").
- `func_8009A770`'s return value (the SI_STATUS poll the session-13 handoff
  suggested emulating) is **ignored** at its only call site
  (`func_80088F08` → `func_80098030` → `func_8009A770`), so emulating
  SI_STATUS would not unblock the boot. The real fix is in the queue
  consumers / retrace fan-out path.

## What was changed

- `app/web/web.js` — `initWebGL()` now updates `#gfx-status` on success
  ("WebGL2 context ready (320x240)"), on context-creation failure (distinct
  error text), and a live poller (`updateGfxStatus()`, piggybacked on the
  16 ms `ogre_gfx_flush` interval) reports renderer/DL state; `watchBootStall()`
  logs the known-stall note after 15 s with ≤1 task submitted. No wasm rebuild
  needed (pure JS, served directly).
- Verified in headless Chrome (`probe-gfxstatus.cjs`): context ready → renderer
  active → boot-stall note, no page errors.

## Boot-stall root cause (evidence from the unfiltered `[ev]` trace)

Run with `window.OGRE_STATUS_FILTER = /$a/` (probe-vi.cjs). Key lines:

```text
[ev] T=420 do_send OK queue=0x800E7988 msg=0x800E8B10 count=1/8
[ev] T=421 do_send OK queue=0x800E7988 msg=0x800E8B10 count=2/8
... (fills to 8/8 over ~85 ms) ...
[ev] do_send FAILED queue=0x800E7988 msg=0x800E8B10 (full, count=8/8)
[ev] do_send FAILED queue=0x800C4C28 msg=0x800E8B10 (full, count=8/8)
[ev] do_send FAILED queue=0x800E8B84 msg=0x0000029A (full, count=8/8)   <- VI retrace never delivered
```

Chain (all addresses are rdram offsets, KSEG0-formatted by the runtime):

1. `func_80088F08` — the game's VI-manager thread — receives VI retraces
   (msg 0x29A) on queue `0x800E8B84` (registered via the bridged
   `osViSetEvent`). On each retrace it calls `func_800891A0(0x800E8B10)`.
2. `func_800891A0` walks a linked list of registered retrace handlers and
   `osSendMesg(node->mq, 0x800E8B10, OS_MESG_BLOCK)` — **blocking** — to each.
   Registered queues include `0x800E7988` (created + consumed by
   `func_80089D9C`, a dispatcher thread) and `0x800C4C28`.
3. The consumers stop draining (see open question below), the queues fill to
   8/8, and the next fan-out send blocks forever → the VI-manager never returns
   to `osRecvMesg(0x800E8B84)`.
4. The runtime VI thread keeps posting retraces; `dequeue_external_messages`
   (`do_send(block=false)`) fails and the messages are dropped/requeued —
   `0x800E8B84` stays full, no retrace is ever processed, `D_800C4800` never
   advances, the frame producer (`func_8008949C`) never matches a framebuffer,
   `osViSwapBuffer` is never called, and the game submits no rendering tasks
   (the page's `ogre_gfx_stats()` stays at `tasks=1 dls=2 cmds=32` — the boot
   blanking DL, which contains no draw commands: `fillrect=0 texrect=0`).

### Why SI_STATUS emulation would not help (correcting session 13)

- `func_8009A770` (reads `0xA4040010` bit 0, writes `0xA4080000` on success) is
  only called from `func_80098030`, which is only called from `func_80088F08` at
  `0x80088FAC` — and **its return value is never checked** (the next
  instructions are `func_80095780(1.0)` and `func_80089BE4(1)`, unconditional).
- The phase byte `D_800C4800 |= 2` happens *before* the call, unconditionally
  (when `s0 == 0`; `s0` is never initialized in `func_80088F08` — thread entry
  registers are zeroed by the runtime, so it is 0).
- The recompiled game also does raw SI DMA register writes (`0xA4040000/4/8/C`)
  that wrap to valid-but-garbage heap offsets (`& 0x1FFFFFFF`); they corrupt
  memory but do not trap. Worth emulating eventually, but not the boot blocker.

### Open questions for the next session (where to look next)

- **Why do the queue consumers stall?** `func_80089D9C` (drains `0x800E7988`)
  loops `osRecvMesg` → dereference msg → `LOOKUP_FUNC` callbacks → repeat. If a
  callback blocks on a full queue (or the scheduler fails to wake it), it
  stalls. The "~180 ms game-thread freeze" from session 13 is the same
  deadlock. Candidates: (a) a callback issuing a blocking send to another full
  queue; (b) scheduler starvation of the consumer threads in the recomp
  cooperative scheduler (`run_next_thread_and_wait`); (c) a message *value*
  mismatch (the fan-out sends the flags-word *pointer* `0x800E8B10`; consumers
  dereference it — if the game expects a different shape, a callback may
  misbehave).
- Cheap next diagnostic: add `[ev]`-style tracing to `do_recv` for these queues
  (which thread blocks, for how long) and to the blocking-send path
  (`blocked_on_send`), then correlate with `trace_millis`. Also try
  `MessageQueueControl` requeue flags (`requeue_vi` etc., already plumbed in
  `mesgqueue.cpp`) so dropped retraces are retried instead of lost.

## Verification

- `probe-gfxstatus.cjs` (this session): gfx-status transitions
  "waiting" → "WebGL2 context ready" → "renderer active — 1 display list(s)
  processed (only the boot blanking DL...)" + boot-stall note; no page errors.
- `probe-webgl.cjs` (session 13, re-run): still 5/5 PASS — renderer pipeline
  unchanged and healthy.

## Files

- `app/web/web.js` — gfx-status reporting fix + boot-stall note (only change).
- `docs/WEB-PORT.md` — milestone 3/7 notes updated with the deadlock diagnosis
  (see below).
- Probe scripts in `/tmp/ogre-web-test/`: `probe-vi.cjs` (unfiltered `[ev]`
  trace), `probe-gfxstatus.cjs` (status-line regression check).
