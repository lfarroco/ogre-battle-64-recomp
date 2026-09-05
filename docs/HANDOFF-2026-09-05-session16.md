# Handoff — Session 16 (2026-09-05): rendering — frame-2 wall decoded, worker deaths fixed

## TL;DR

- Session 15's wall description is **superseded**: the frame thread *does* run.
  Frame 1 completes end-to-end (t3 kick → t17 `func_800893C0` → t16 RSP submit →
  SP/DP-done → kick_next → t5 dispatch), then t3 **retires** (parks idle).
- Precise frame-2 wall: t5's completion dispatch reads dispatch-key **8** →
  callback `*(0x800A9E88)` is **NULL** (no setter exists in any executed code),
  so t5 loops silently and frame 2 is never enqueued. Only cb84 (swap,
  `0x8008B110`) is ever set.
- Racy worker deaths (fast-run OOBs in browser, SIGBUS natively) are **root-caused
  and fixed**: `check_running_queue` dereferenced a **corrupt running-queue entry**
  (intrusive list lives in game memory; wild boot-time writes poison it). The
  scheduler is now **self-healing** (validate + amputate + `[sch-err]` log).
- Verified: 4/4 fast browser probes + 2/2 native runs survive with **zero worker
  deaths**, all reaching the same healthy idle wall. Patch regenerated and
  verified to apply clean.

## Starting state (from session-15 handoff)

Session 15 round 5: scheduler bugs fixed, frame-1 RSP DL submitted with
sp_complete + dp_complete, but frame 2 never submitted; listed blockers were
t17 waiting on DP-done/0x800E8B4C, t16 parked on 0x800B9C40 (producer unknown),
0x800E79C8 filling, t3 stuck in osSendMesg, no SI input. Working tree was clean
at `24e8f77 session 15`; `build-wasm` was current (wasm newer than all sources).

## What this session did

### 1. Probe harness rebuilt (scratch, NOT committed)

`/tmp/ogre-probe/` (recreate if lost):
- `server.py` — static server on 127.0.0.1:8931 with COOP/COEP + no-store.
- `probe.cjs` — playwright (from `/Users/momo/dev/fatos/node_modules/playwright`,
  browsers at `~/Library/Caches/ms-playwright`, rev 1234) harness:
  `node probe.cjs [secs] [trace=0|1] [outprefix]`; `OGRE_PRESS=0` disables input.
  With input (default post-edit): holds Enter (Start) + taps X (A) via real key
  events through the page's keydown/keyup listeners. Later edit: `pageerror`
  handler captures `e.stack` (this is what cracked the OOBs — see §4).
- Server runs via `muse.bash` foreground session (macOS has no `setsid`; `&` is
  rejected — start it with a short yield and run probes in parallel calls).

### 2. Full decode of the frame/task/retrace path (all `asm/1060.s`, main segment)

- `func_80089054(node, mq, flags)` — prepend retrace handler to `0x800E9178`
  (node.next=node+0, mq=node+4, flags=node+8); if (flags&2) && phase: NOBLOCK
  kick. Never blocks itself (last-func attribution to it is stale-entry noise).
- `func_80089124(node)` — unregister (3 call sites). Handler list legitimately
  shrinks 4→1 during boot.
- `func_800891A0(mask)` — VI fan-out, NOBLOCK sends of the mask-word pointer.
  t19 = VI manager; `0x29A` = retrace msg, `0x29B` = SP-done.
- `func_80089A30(n)` — "wait n retraces on a stack queue, unregister, return".
  Stack-resident nodes (do NOT treat as leaks).
- `func_80089AB0(fblist, n)` — wait-1-retrace + `func_80089528` + framebuffer
  match poll (`.L80089B70` calls `osViGetCurrentFramebuffer` in a loop with a
  `jal` inside → **no yield_self emitted → latent scheduler starvation** if the
  framebuffer never matches; never observed taken, but flag it).
- `func_800893C0` (t17 frame loop): recv `0x800E8B4C` → phase&2 check →
  `func_8008949C` (register retrace handler, wait framebuffer, unregister) →
  `func_800901C0` (cache flush — explains t17/t18/t3 "in func 0x800901C0") →
  send `0x800B9C40` → recv SP-done (`0x800E8BBC`) → send `0x800B9C40` NULL →
  recv DP-done (`0x800E8BF4`) → kick_next: send(`*(msg+0x50)`, msg) → loop.
- `func_80089358` (t16 RSP-submit thread): recv `0x800B9C40` → **SP-status spin**
  (`.L80089374`, `osSpGetStatus()&1`, call inside → no yield → second latent
  starvation site) → if msg: `D_800E917C = msg`, `osSpTaskLoad/StartGo(msg+0x10)`.
- `func_80089200` (t18 audio/RSP thread): recv `0x800E8B14` (**never fed** — only
  producer is t3's `func_80080F78`, gated on `D_800A9890==1`) → tail forwards
  `send(*(msg+0x50), *(msg+0x54))`.
- `func_80089540` (t5 task-dispatch thread): creates `0x800E9BA8`, loops recv →
  dispatch on halfword at `*(msg+0x54)`: **4→cb84 `*(0x800A9E84)`,
  8→cb88 `*(0x800A9E88)`, 0x10→cb8c `*(0x800A9E8C)`** → `D_800E79A4--` → loop.
  NULL callbacks are silently skipped.
- `func_80089660` (task-table init, called from late boot `func_8008A1B0`):
  10 descriptors × 0x58 at `0x800E7DE0`, pipes `0x800B9C84` (key 8) /
  `0x800B9C86` (key 4), `taskptr D_800B9C80 = 0x800E7D90`, creates t5.
- `func_80089804` (task enqueue, 3 boot call sites): fills OSTask-ish struct,
  sets `*(task+0x54)` = pipe ptr (a3&1 selects), `D_800E79A4++`, cache flush,
  **kick `0x800E8B4C` with the task msg itself**, pop head.
- `func_8008B110` (cb84, the only set callback) = `osViSwapBuffer(*(task+0xC))`.
- `func_800899D0(v)` sets cb84; `func_8008AFB8(v)` would set cb8c (**zero
  callers**); **no setter for cb88 exists** in main segment or A/B/C overlays
  (only loads in t5's dispatch).
- t17's kick_next target = `*(0x800E7D90+0x50)` = `*(0x800E7DE0)` = `0x800E9BA8`
  (init value) → frame completions go **directly t17→t5**.
- t3's audio side: `func_80080DC0` (init, sets `D_800A9890=1`),
  `func_80080EE8` (loop on `0x800E79C8`; key==1 exits), `func_80080F78`
  (steady: copy + send `0x800E8B14` + wait `0x800C49E8`). t3 exits the audio
  loop on first retrace (mask1 key=1), enqueues the frame task, and retires.

### 3. Frame-1 traced end-to-end; frame-2 wall isolated (browser, traced probe)

Traced 60 s run (no crash — tracing slows timing into the stable regime):
`T=100 t3→0x800E8B4C(0x800E7D90)` → t17 wakes → `func_8008949C` (unwatched stack
recvs) → sends `0x800B9C40` (newly watched this session) → t16 submits RSP task
→ SP-done → DP-done → kick_next `t17→0x800E9BA8(0x800E7D90)` → t5 consumes,
dispatches, loops. **Zero game sends after that.** New `framedisp` snapshot line
reads: `kick=0x800E9BA8 dptr=0x800B9C84 key=hu(...)=0x0008 cb84=0x8008B110
cb88=0 cb8c=0` — i.e. key 8 with a NULL callback. t17 parks on `0x800E8B4C`,
t16 on `0x800B9C40`, t18 on `0x800E8B14`, t5 on `0x800E9BA8`, t3 retired
(last-func `0x800901C0` = enqueue tail flush; "not blocked in mesg" = scheduler
idle-park, consistent with `osStopThread(self)` after boot).
Multi-enqueue/re-init ordering explains `taskptr` readings: late boot
`func_8008A1B0` runs cb84-set → task-init (resets head to `0x800E7D90`) after the
first enqueue, so `taskptr=0x800E7D90` (1 kick) vs `0x800E7DE8` (2 kicks) varies
by run — both healthy.

### 4. Worker deaths root-caused (the rendering blocker for fast runs)

Fast runs died ~50%: 2× early OOB (quiet browser) + native SIGBUS + variants.
Decisive oracle: `pageerror` **`e.stack`** in the probe — both browser OOBs land
in **`check_running_queue`** via `osRecvMesg_recomp ← func_80080EE8 ←
func_80085908` (t3 audio) and via `osSendMesg_recomp ← func_800891A0 ←
func_80088F08` (t19 fan-out). Native ASan confirmed the identical fan-out site
(`BUS ... check_running_queue ... func_800891A0 ... func_80088F08`); lldb showed
a t1-boot-code variant. Mechanism: the intrusive running/blocked lists live in
**game memory** (game-allocated OSThread structs); wild boot-time writes poison
a link; the next scheduler list walk dereferences it OOB. Ultimate wild writer
**not** identified (prime suspects, unproven: `func_8019FC68`'s
heap-record `[0x34]` store from session 8/9; audio-mixer writes during the
type-2 task burst that also clobber t3-stack queues `0x800C6C98/CA8` —
quiet-freeze shows garbage counts there plus t3/t18 wedged on `0x800C49E8` after
the auto-response gives up).

### 5. Fixes (runtime; `n64modernruntime-ob64.patch` regenerated 1783→1937 lines)

- `ultramodern/src/threadqueue.cpp`: `thread_ptr_valid()` (KSEG0 range);
  validation in insert (refuse wild `toadd`, truncate corrupt walk),
  unlink_locked (truncate + log), pop (drop corrupt head/next + log), remove
  (guarded log line). Loop restructured so validation precedes deref.
- `ultramodern/src/scheduling.cpp`: `check_running_queue` drop-and-retry
  (bounded 8) on wild peek + `[sch-err]` log; `schedule_running_thread`
  pre-validates (trace/state writes faulted before insert-validation).
- `ultramodern/src/audio.cpp`: `queue_audio_buffer` validates addr/len
  (KSEG0, ≤256 KB, aligned) — defense for game-driven `osAiSetNextBuffer`
  args; never fired in probes, kept as cheap insurance.
- `ultramodern/src/mesgqueue.cpp`: `[snap] framedisp` line (kick, dptr,
  dispatch key, cb84/88/8c, `D_800E917C`, taskptr).
- `librecomp/src/ultra_translation.cpp`: `0x800B9C40` added to watched queues
  (t17→t16 path was previously invisible).
- Patch verified: `git apply --check` clean against a pristine clone of the
  vendored repo HEAD (`589bbf0`).

### 6. Verification (this session, all observed)

- Browser (headless Chromium, rev 1234): 4/4 fast 40–45 s probes post-fix
  (`fix2`, `verify1`, `v2`, `v3`) — **zero pageerrors**, retrace ≈2400+,
  all reach the healthy idle wall (`framedisp` key=8/cb88=0). One run with Start
  held + A taps: no state change (title does not advance on input alone).
- Native (`build-null`, current): 2× 100 s runs survive, zero crashes, t4/t19
  cycling (2600+ resumes). 30 s post-revert sanity: boots, frame 1 submits.
- No `[sch-err]` fired post-fix (corruption didn't manifest — guards are
  armed but inert; correctness by construction + review).
- `build-wasm` and `build-null` rebuilt from final sources (wasm includes all
  fixes). TEMP `OGRE_NO_PUMP` SDL edit used for headless ASan/lldb work was
  **reverted** (tracked tree clean). Scratch `build-asan/` deleted.
- Pre-existing environment quirk (not a game bug): native `SDL_PumpEvents()`
  from a game thread intermittently throws Cocoa `NSException`
  (`nextEventMatchingEventMask` off-main-thread) headless — use
  `OGRE_NO_PUMP`-style guard locally if doing native input-thread-adjacent
  debugging again (do NOT commit).

## What is still open (leads, ordered)

1. **cb88 setter hunt (the frame-2 key).** No static setter; dynamic-store or
   overlay-D+ (not disassembled) or downstream-of-stall. Watch `0x800A9E88`
   with lldb in a run that progresses past this wall (none yet). Alternative:
   find a steady-state caller of `func_80089804` (all 3 known sites are boot).
2. **Latent unyielded poll loops**: `func_80089AB0` `.L80089B70`
   (`osViGetCurrentFramebuffer` match) and t16 `.L80089374` (SP-status) — both
   contain calls so no `yield_self`; if ever taken with no-match they starve
   the scheduler exactly like session 8. Consider belt-and-braces yields if the
   game ever parks there.
3. **Wild queue writer**: catch with lldb watchpoints (`$rdi+0xE9178` head,
   `+0xC6C98` audio queues) in a *crashing* run — scripting that prints `bt`
   on hit needs one-line `-o` commands (`watchpoint command add -o "bt 12" -o
   "continue" 1`); heredoc-style multi-line command bodies silently eat the
   `bt`. Crash is racy; several runs may be needed.
4. Round-5 leftovers still open: `0x800E79C8` fills/owner-drain, SI `0x800E9B88`
   never fires (input reaches game but title doesn't advance), audio RSP
   repetition, `D_800A9890`-gate steady audio (t3→`0x800E8B14` never happens).
5. Native-vs-browser boot divergence: native `framedisp` stays zero (task table
   seemingly never built) yet reaches identical thread blocks; browser builds
   it. Same code — timing/path divergence unexplained. Not blocking browser work.

## Repro / commands

- `EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8`
  (canonical cache; `/tmp` caches are stale/forbidden).
- Native: `cmake --build build-null -j 8 && ./build-null/ogrebattle64
  assets/ogre64.z64` (headless-has-GUI-quirk: needs a window server for input
  pump; alarm-kill for bounded runs).
- Probe: `python3 /tmp/ogre-probe/server.py` (foreground session) +
  `node /tmp/ogre-probe/probe.cjs [secs] [trace] [out]` (`OGRE_PRESS=0` for no
  input). Status/gfx dumps land next to `out`.
- Patch regen: `git -C tools/N64ModernRuntime diff HEAD >
  n64modernruntime-ob64.patch` (covers staged `function_trace.cpp` too).

## Files changed (tracked)

- `n64modernruntime-ob64.patch` (+182/−28: all runtime fixes above).
- `docs/WEB-PORT.md` (milestone-8 cell rewritten for session 16).
- `docs/HANDOFF-2026-09-05-session16.md` (this file).
- Vendored (gitignored, captured via patch): `threadqueue.cpp`,
  `scheduling.cpp`, `audio.cpp`, `mesgqueue.cpp`, `ultra_translation.cpp`.
- Deliberately NOT changed: N64Recomp fork, app code, web page, overlays.
