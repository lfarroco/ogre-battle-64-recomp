# Handoff — Session 17 (2026-09-05): input advances title into a new deterministic crash; cb88 proven unsettable; title-dispatch fully decoded

## TL;DR

- **cb88 has no setter. Period.** Validated ROM-wide scan (`/tmp/cb88scan2.py`):
  `lui r,0x800B/0x800A → sw *,0x9E88(r)` = 0 hits across the whole 40 MB ROM,
  while the same scanner finds both known cb84 setters + the cb8c setter.
  No literal `0x800A9E88` anywhere either. The frame-2 wall is therefore NOT
  "find the missing store" — cb88 is set by code that hasn't run yet, i.e. the
  boot→steady-state transition is stalled elsewhere.
- **Input advances the game past the session-16 idle wall into NEW title code
  that crashes deterministically** (3/4 input runs, identical wasm stack):
  `run_thread → func_80071EB0 → func_80075BC0 → func_8017BA60 → func_8019D67C`
  OOB. No-input control reproduces the old idle wall cleanly (zero errors).
- **The crash chain is 100% legitimate game logic** (new instrumentation
  proves it): dispatch index `0x18` → table[24]=`8017BA54` (returns
  `&D_8018FDC0`) → `*0x8018FDC0 = 0x8017BA60` (overlay-B .data vector,
  confirmed in ROM at `0x65CC0`: `[BA60, BB28, BB54, …]`) → `BA60` →
  `func_80073164` (returns constant 2 on ALL paths, asm + recompile verified)
  → `func_8019D67C` stores through `v0=2` → OOB. No wild pointers anywhere.
- Paradox + resolution: a shipped game cannot store to `0x2` (no TLB writes
  exist in any segment — checked; TLB hypothesis dead). So **title state
  `0x18` in `hu(*D_800C4BBC+4)` must be unreachable on HW / lacking HW
  context** — we are executing dead-path code after an earlier state
  divergence. Do NOT "fix" BA60/D67C; find where the title state went wrong.
- Runs bifurcate early with identical input scripts: idle-wall trajectory
  (t3 retires, title dispatch never runs, table all zeros) vs advanced
  trajectory (t3 steady-audio `func_80080F78` ×261, t5 in `func_8007284C` on
  `0x800E7988`, dispatch runs, crash). Timing/race decides which.
- Added `[snap] titledisp` + `titleTab` lines (table contents, dispatch index,
  `D_800E8294` chain). Patch regenerated, verified `apply --check` clean
  against pristine `589bbf0` (via temp worktree, removed after), wasm rebuilt.

## Starting state (from session-16 handoff)

Clean tree at `50936e6 session 16`; leads ordered: (1) cb88 setter hunt,
(2) latent unyielded polls, (3) wild-queue-writer watchpoints, (4) round-5
leftovers (`0x800E79C8`, SI, audio RSP, `D_800A9890`), (5) native-vs-browser
divergence. Probe harness at `/tmp/ogre-probe` (`server.py` :8931,
`probe.cjs [secs] [trace] [out]`, `OGRE_PRESS=0` disables input).

## What this session did

### 1. cb88 setter hunt — closed with a negative result (ROM-wide, validated)

- `/tmp/cb88scan2.py`: scans all 10M ROM words for `lui r,HI; …; sw *,LO(r)`
  pairs. Controls hit (cb84: 2, cb8c: 1); `LO=0x9E88` with `HI=0x800B`
  (sign-extended form the known setters use) AND `HI=0x800A` (alt codegen):
  **0 hits**. Literal-word scan for `0x800A9E88/84/8C`: 0 hits (code uses
  lui/sw splits, as expected). A setter via `gp`-relative store or
  `&cb84+4` array indexing can't be fully excluded, but no evidence supports
  them; the productive reframing is "setter code hasn't run yet".
- Incidental: KMC emits `hi=0x800B` for these globals (lo sign-extends);
  no `hi=0x800A` variants exist for any of the three callbacks.

### 2. Fresh probes: input/no-input split + variance (build-wasm, session-16 build)

- `s17` (45 s, input): crash `8019D67C←8017BA60←80075BC0←80071EB0`, t3=261
  steady-audio, t5 in `8007284C`/`0x800E7988`, corrupt audio queues again
  (`0x800C6C98` 40/2, `0x800C6CA8` huge-negative — wild writer still active).
- `s17b` (45 s, `OGRE_PRESS=0`): **zero errors**, exact session-16 idle wall
  (t5 on `0x800E9BA8` in `80089540`, key=8/cb88=0, t3=3 retired).
- `s17c` (45 s, input): same crash, identical stack → looked deterministic.
- `s17d` (45 s, input, NEW build): **clean** — but titledisp shows table all
  zeros, `statep=0`: dispatch never ran; old trajectory (t3=5, t5=1).
- `s17e` (60 s, input, NEW build): crash again, identical stack; titledisp
  captured the full chain (see TL;DR). Score: input 3/4 crash, no-input 1/1
  clean. Bifurcation is timing, crash is deterministic once advanced.

### 3. Crash-chain decode (all static, verified against recompile output)

- `func_80075BC0`: fills table `0x800AF028`+25 slots (all overlay-B addrs;
  runtime snapshot matches disassembly exactly), then dispatches on
  `v1 = hu(*D_800C4BBC+4)` clamped `<0x1F` (`sltiu/negu/and`), records index
  at `D_800E810E`, `jalr table[v1]`. Index `0x18`=24 → `8017BA54` =
  12-byte stub returning `&D_8018FDC0`.
- Caller then stores that to `D_800E8294`, and a later `jalr` goes through
  `*(D_800E8294)` = `0x8017BA60` (vector slot 0; vector lives in overlay-B
  `.data`, ROM-verified). So the wasm frame `80075BC0→8017BA60` is the
  *second* jalr, not the table dispatch — both legitimate.
- `func_8017BA60`: cache flushes, DMA-loads overlay C (`func_8009DA50`
  ROM `0x712A0` → `0x8019A7C0`), `beql` memset (compares two *addresses*,
  always memsets `0x801A2C70+0x40`), `jal func_80073164` (delay: stash
  `0x3000`), `jal func_8019D67C`, `beqz`→`D_800C4C26=9` pattern shared with
  sibling stubs `8017BB28/BB54` (which call `8019C5D4`-family instead).
- `func_80073164` returns **2 on every path** (both asm and
  `RecompiledFuncs/funcs_8.c` line-by-line verified; `bnez`/`bltz`/`j`
  delay slots all faithful). `func_8019D67C` (overlay-C DL builder bumping
  cursor `D_800E9BA0`) takes cursor in `v0` → first insn `sw $s1,0($v0)`
  faults with `v0=2`. Recompilation of all three functions is faithful;
  this is NOT a recompile bug.
- `func_80071EB0` is a *thread entry* (TCB `D_800AF5F0`, pri 3, created by
  `func_8007F8E4`-thread which main creates). Crashed worker identity
  (t3 frozen at 261 vs t18 frozen at 104) not pinned down — needs trace.

### 4. Runtime change (vendored, in patch)

- `ultramodern/src/mesgqueue.cpp`: `[snap] titledisp` line (dispatch index
  `D_800E810E`, `statep`+`srcidx`, `D_800E8294`+`*ind`, all RDRAM-guarded)
  plus `titleTab` (25 table words). Wasm rebuilt; `s17d/s17e` verified the
  lines in practice (zeros pre-dispatch, full chain mid-dispatch).

### 5. Verification this session (all observed)

- Patch `apply --check` clean vs pristine `589bbf0` (temp worktree).
- `cmake --build build-wasm` clean; 5 browser probes (headless Chromium):
  crash stack identical 3/3 times on advanced trajectory; control + survivor
  clean. GFX still only the boot blanking DL in all runs.

## What is still open (leads, ordered)

1. **Who writes `0x18` to `hu(*D_800C4BBC+4)`** (state struct at
   `0x800AEFE0`)? Writers: `80075BC0`'s own tail (`sh $v0,4($v1)` at several
   sites) + unknown others. A traced (`trace=1`) input probe that reaches
   the advanced trajectory shows the state evolution; alternatively add a
   store-watch log for that halfword in the runtime (victim-range `MEM_H`
   hook, debug-only — hot path, do NOT ship enabled).
2. **Why do runs bifurcate?** Same input script → t3 retires (idle wall) vs
   t3 steady-audio (advanced). Suspect input-vs-retrace timing at boot
   (Enter held from boot-start; SI `0x800E9B88` behavior) or VI/audio race.
   Try: delay input start (hold Enter only after N seconds), single-tap
   variants, `OGRE_PRESS=0` + mid-run input injection.
3. **Which thread dies?** Traced probe or `recomp_trace_func` log around
   `func_80071EB0` entry per thread id; resolves t3-vs-t18 question.
4. Untouched from session 16: latent unyielded polls (`func_80089AB0`
   `.L80089B70`, t16 `.L80089374`); wild-writer watchpoints (still active
   per corrupt audio-queue counts — now secondary, since the crash chain is
   legitimate logic, but the writer can still poison other state);
   `0x800E79C8`/SI/audio-RSP/`D_800A9890` leftovers; native-vs-browser
   divergence.
5. Note for the state-0x18 hunt: `0x800E8214` (compared against the state
   halfword in the `L80075DB8` loop) and controller/SI input values are the
   prime divergence suspects — our environment may feed the state machine
   values HW would never produce at that point.

## Repro / commands

- `EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8`
- Probe: `python3 /tmp/ogre-probe/server.py` (foreground bash session; macOS
  has no `setsid`, `&` rejected) + `node /tmp/ogre-probe/probe.cjs [secs]
  [trace] [out]` (`OGRE_PRESS=0` for no input). Key logs: `/tmp/ogre-probe/
  s17{,b,c,d,e}-status.txt` (+`-gfx.txt`, `-errors.txt`).
- Scanners: `/tmp/cb88scan.py`, `/tmp/cb88scan2.py` (validated; rerun after
  any ROM change — none expected).
- Patch regen: `git -C tools/N64ModernRuntime diff HEAD >
  n64modernruntime-ob64.patch`; pristine check: worktree-add HEAD at
  `589bbf0` elsewhere + `apply --check` (see §4; remove worktree after).
- Server running at session end (bash session 18) — dies with the session;
  restart per above.

## Files changed (tracked)

- `n64modernruntime-ob64.patch` (regenerated: +titledisp instrumentation).
- `docs/WEB-PORT.md` (milestone-8 cell: session-17 note).
- `docs/HANDOFF-2026-09-05-session17.md` (this file).
- Vendored (gitignored, captured via patch): `mesgqueue.cpp` (snapshot only).
- Deliberately NOT changed: N64Recomp fork, app code, web page, overlays,
  scheduler/audio guards (all armed, no `[sch-err]` fired this session).
