# Session 117 — prod-build debug audit, and the stale-patch release gap

Date: 2026-10-01. Follows session 116 (the periodic `[snap]` snapshot stutter).

## Goal

Developer question: *"can you audit if we have other debug behaviors in the prod
build? this should never happen (apart from dumping the errors to the log file
when something goes wrong)"*. The developer added the context that a previous
session's slowdown fix did not take effect for players, *"or it was an issue
with the CI building"*.

## Result

Three results. The second answers the first.

### 1. The release builds from patch files that had drifted

A GitHub-hosted release runner has no vendored trees, so it clones each
third-party project at its pinned commit and applies the project's patch files
(`.github/workflows/release.yml`, step "Patch the RT64 submodules", guarded by
`vars.OGRE_DATA_REPO != ''`). `gh variable list` shows
`OGRE_DATA_REPO=lfarroco/ogre-recomp-private`, and the v0.5.1 run
(`36413921688`) executed that step on all three platforms. The developer's own
build uses the dirty trees instead, and those two sources had diverged:

* `patches/rt64-ob64.patch` still read
  `ogreCapture.presentPath = getenv("OGRE_CAPTURE_PRESENT")` while
  `tools/RT64/src/hle/rt64_present_queue.cpp:141` calls
  `ogre_present_capture_path()`. The patch was 29 lines behind the tree.
* `patches/rt64-plume-ob64.patch` contained only the `plume_metal.cpp` readback
  and no `plume_d3d12.cpp` hunk, so the guard that stops
  `setSamplePositions(dstLocation.texture)` dereferencing a placed-footprint
  (buffer) destination was in no release.

Both changes are session 107's fix for the v0.4.0 Windows first-present crash.
Consequences in a hosted build (v0.5.0, v0.5.1): `snap`'s presented-frame
capture silently does nothing, because RT64 reads an environment variable that
nothing sets; and a player who sets `OGRE_CAPTURE_PRESENT` still reaches the
D3D12 null-texture read. The `snap` RDRAM dump is app-side and was unaffected.

The unit-slowdown report has the same shape from the other direction. v0.5.1 was
built from `c7bd999` (session 112's commit), and `ogre-data/files.tar.gz` in the
private repository records `public commit: c7bd999` and contains 34 `yield_self`
call sites against the tree's 33. Session 115's fix (`8c5cbba`) is in **no**
release, and neither is session 116's `[snap]` gate (`da8676c`). A screen that
still stuttered after a release could therefore be three different things: the
screen's own remaining work loop, the un-shipped fix, or the periodic snapshot
hitch, which was in every release and lands on every screen once per 1.5 s.

Fixed: both patches were regenerated from their trees, and a check now fails
when any tracked patch stops describing its tree.

### 2. The audit

Ranked inventory of debug/diagnostic behaviour reachable in a release build with
no environment variable set. "Gated" means an `OGRE_*` variable that defaults to
off.

**Release parity (fixed in this session)**

| # | item | evidence |
|---|---|---|
| 1 | `patches/rt64-ob64.patch` 29 lines stale | `tools/patchcheck.py`, before the fix |
| 2 | `patches/rt64-plume-ob64.patch` missing the `plume_d3d12.cpp` hunk | same |
| 3 | v0.5.1 = `c7bd999`; session 115's fix in no release | `gh run view 36413921688 --json headSha` |
| 4 | the private bundle is `c7bd999` and has 34 `yield_self` against the tree's 33 | `gh api …/contents/files.tar.gz`, MD5 equal to `dist/ogre-data/files.tar.gz` |

**Unconditional behaviour that ships** (no gate at all)

| # | item | site | measured |
|---|---|---|---|
| 5 | live debug console channel: a fixed file is opened and executed, including guest-RAM writes (`w`), input injection (`press`), RDRAM dumps and checkpoint `save`/`load` — **fixed in this session, see result 3** | `app/src/sdl_platform.cpp:1705`, called from `pump_sdl_events` (`:659`) | a stock run with no env vars executed `c`, `r`, `w` from `/tmp/ogre-console.txt` and wrote guest RAM |
| 6 | `fopen` of that path per tick — **fixed in this session** | same | `update_gfx` runs in `while (!exited) { sleep_milliseconds(1); update_gfx(); }` (`librecomp/src/recomp.cpp:1017-1020`), so ~1000 opens/s, ~0.2-0.5 % of a core |
| 7 | `[rsp]` per-task logging: two printf per RSP task | `app/src/rsp.cpp:117`, `:131-147` | 4314 lines in a 37 s run = 116 lines/s; the crash report keeps only the last 100 stdout lines (`crash_log.cpp:417`), so its stdout section is under a second of run |
| 8 | `[renderer]` per-display-list lines (first 8, then 1 in 10) plus a per-list `high_resolution_clock::now()` and a 256-atomic `debug_total_entry_count()` sweep | `app/src/renderer.cpp:958-976`, `:1099-1105` | ~117 lines per 37 s at 30 display lists/s; the `entries=` field is always 0 because the counters only run under `OGRE_COVER` |
| 9 | `[dbg]` direct-send delivery print | `mesgqueue.cpp:126` | 9 in a 37 s run |
| 10 | audio-block auto-response: a synthetic `{0x800C49E8, 0, jam=false, retries=4}` is queued and printed whenever a thread blocks on that queue | `mesgqueue.cpp:684-696`, `:126` | 9 in a 37 s run. A documented bring-up shim (sessions 15, 75, 76); session 75's entry records the open follow-up, "fire the real `OS_EVENT_AI` when a buffer drains, and delete this" |
| 11 | `poll_scene()` does 9 uncached `getenv` per call | `app/src/bank_overlays.cpp:405-593` | ~9000/s, from the same 1 kHz loop |
| 12 | `recomp_trace_entry` on every recompiled function entry | `function_trace.cpp:237`, emitted at 5316 sites | 28 instructions in the callee plus 4 at the caller with every gate off, and it forces a stack frame in every recompiled function |
| 13 | `recomp_trace_return` before every recompiled `return` | `function_trace.cpp:293`, 5301 sites | 14 instructions with the gates off |
| 14 | `debug_sched_event_here` + `debug_note_resume` on every thread handoff; `resume_counts` is written in every run | `threads.cpp:190-195` | a `callq` + an inlined `lock incq` |
| 15 | `debug_printf` is no longer a no-op: upstream it expanded to nothing, here it calls `sched_traces_enabled()` | `ultramodern.hpp:309` | 23 call sites; `thread_queue_insert` calls it 4 times, one inside a per-node walk, so O(queue length) per insert |
| 16 | `do_send` diagnostics: an 8-way queue compare, 2 more, and `retrace_delivered.fetch_add` on every VI retrace regardless of `OGRE_VI_COUNT` | `mesgqueue.cpp:617-653` | one locked RMW per retrace, 60/s at speed 1 |
| 17 | `deliver_pending_direct_sends` takes a mutex and builds two vectors on every `osSendMesg`/`osJamMesg`/`osRecvMesg` | `mesgqueue.cpp:118-124` | one uncontended mutex per call |
| 18 | `is_watched_queue` membership test on every bridged message call | `ultra_translation.cpp:59-71` | a 14-entry binary search, twice per `osRecvMesg` |
| 19 | `get_function` miss path logs 2 lines, a `fflush` and a full call-chain dump, then returns a no-op stub | `librecomp/src/overlays.cpp:721-734` | unconditional; converts a hard failure into log-and-continue |
| 20 | uncached `getenv("OGRE_NO_DUMMY_VI")` per dummy VI workload | `events.cpp:485` | one libc lookup per VI frame before game start |

**Not debug behaviour, reported for completeness:** the crash logger (two
capture threads, 64 KiB pipes, `error.log` on a fault) is the accepted error
path; `main.cpp:531` starts the game thread after a fixed 500 ms sleep;
`app/src/synth_frame.cpp` and `app/src/gbi.cpp`'s analyzer are linked in but
inert without their variables; ~1.1 MiB of zero-init diagnostic BSS
(`call_chains`, `sched_ring`, `func_count_*`) and ~87 KB of function-name
literals are resident but untouched with the gates off; RT64's three-line device
banner is upstream.

**Verified clean:** `CMAKE_BUILD_TYPE=Release` with `-O3 -DNDEBUG`, so `assert()`
is out and RT64's own `LOG_*` macros compile away; every print the project added
inside RT64 is `getenv`-gated; the presented-frame capture is off by default
(static path from `ogre_present_capture_path()` plus a cleared-environment
`OGRE_SMOKE=1` run that reported `[smoke] capture path: (unset)`); the `OGRE_SNAP`
gate returns 0 when unset; every `debug_sched_event*` returns on one relaxed
load; `notify_rom_read` walks 5 code sections; the timer, heap, AI, SP, VI and
Pak changes are correctness work.

### 3. The live console is opt-in

`console::tick()` returned after reading the watched file, reading the keyboard
and executing whatever it found. It now decides once, from a cached static, which
triggers are on, and returns immediately when neither is:

* `OGRE_LIVE_CONSOLE=1` turns both on;
* naming a trigger's own variable turns that trigger on by itself, because naming
  one is an explicit request for it: `OGRE_CONSOLE_FILE` or `OGRE_CONSOLE_AT_MS`
  for the watched file, any non-empty `OGRE_KEY_<n>` for the number keys;
* `0`/`off`/`false`/`no` is an explicit off, the same spelling as `OGRE_BG`.

With no variable set a run opens no path, reads no keyboard and prints nothing.
The `[console] key N pressed; set OGRE_KEY_N=...` hint lived inside the key
trigger, so it is gone too. The `snap` capture window still expires even with the
console off, because `OGRE_CONSOLE_ON_CMD` can issue a `snap` through `exec()`
without `tick()`; `OGRE_CONSOLE_ON_CMD`/`_SCENE`/`_STEP` are unchanged, since
their own variables are the explicit opt-in.

Recipes written before this session write `/tmp/ogre-console.txt` with nothing
set, and now need `OGRE_LIVE_CONSOLE=1`. The current recipes in
`docs/guides/app-build.md`, `AGENTS.md` and `debug/menu-probe.sh` carry it.

## Files changed

| file | change |
|---|---|
| `patches/rt64-ob64.patch` | regenerated: 29 lines, the `ogre_present_capture_path()` accessor (session 107) |
| `patches/rt64-plume-ob64.patch` | regenerated: adds the `plume_d3d12.cpp` null-texture guard (session 107) |
| `tools/patchcheck.py` | new: fails when a tracked patch stops describing its tree |
| `tools/release-build.sh` | runs the check before packaging |
| `Makefile` | `make patch-check`, `make patch-fix` |
| `docs/guides/app-build.md` | new "Third-party patches" section; the live console is documented as off by default, with `OGRE_LIVE_CONSOLE` in the knob table and in every recipe in that section |
| `app/src/sdl_platform.cpp` | the console gate: `env_set`/`env_truthy`, `file_trigger_enabled()`/`key_trigger_enabled()`, and `tick()` returning early |
| `debug/menu-probe.sh` | sets `OGRE_CONSOLE_FILE` so its file channel survives the gate |
| `AGENTS.md` | the live-console bullet names `OGRE_LIVE_CONSOLE` |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | the record |

## Verification

* `python3 tools/patchcheck.py` before the fix names exactly the two defects
  and passes the other four patches; after the fix it prints `PASS`.
* The corrected content is exactly what the trees hold: `git diff patches/`
  shows 38 insertions and 11 deletions, all inside
  `src/hle/rt64_present_queue.cpp` and the new `plume_d3d12.cpp` hunk.
* Both patches apply to pristine checkouts: `git -C tools/RT64 worktree add
  --detach /tmp/rt64-pristine HEAD` (`4337374`) plus `git apply --check
  --ignore-whitespace patches/rt64-ob64.patch`, and the same for
  `tools/RT64/src/contrib/plume` (`d890ac8`). `git apply --check -R` succeeds in
  both trees, so the tree equals the patch's post-image.
* `bash -n tools/release-build.sh`; `make patch-check` exits 0.
* The console gate, five runs of `build-app/ogrebattle64` with a command file
  written before launch and, in every case but the first, the same `c` command:

  | run | `[console]` lines | the file afterwards |
  |---|---|---|
  | no variable set | 0 | still present, unread |
  | `OGRE_LIVE_CONSOLE=1` | 2 | consumed |
  | `OGRE_CONSOLE_FILE=/tmp/ogre-console.txt` | 2 | consumed |
  | `OGRE_CONSOLE_AT_MS=100` | 2 | consumed |
  | `OGRE_LIVE_CONSOLE=0` | 0 | still present |
  | `OGRE_KEY_1=c` only | 0 | still present (the key trigger alone does not open the file) |

* The direct path still works with the console off:
  `OGRE_CONSOLE_ON_SCENE=0x09 OGRE_CONSOLE_ON_CMD='c'` printed
  `[scene] console trigger: scene 0x0009 step 0 -> c` and the `[console]` reply.
* `snap` still captures end to end: `OGRE_LIVE_CONSOLE=1` plus a file containing
  `snap /tmp/stutter/snap 600` wrote an 8388608-byte RDRAM image, and RT64 wrote
  `/tmp/ogre-shot.<present>.ppm` frames.
* The audit's measurements are the two earlier 40 s runs (`/tmp/stutter/*.log`)
  for the line rates, the `/tmp/wsx.log` and `/tmp/w19.log` runs the
  sub-audits collected, `strings`/`nm`/`otool -tvV` on `build-app/ogrebattle64`,
  and the GitHub API for the run and the private bundle.
* No probe was added to generated code, so nothing needed reverting.

## Still open — needs a decision

The audit's items 7-20 are "debug behaviour in the prod build" by the
developer's stated rule, and they are not equally easy to remove. Only the
console (items 5 and 6) was changed in this session.

1. **The chatty logs (7, 8)** cost ~120 lines/s, and `tools/runlog.py --check`
   and `tools/dlgaps.py` parse exactly those lines. `make smoke`,
   `tools/smoke-dist.sh` and CI's `runlog.py --check` would all have to set the
   gate, or they would silently stop checking anything.
3. **The trace hooks (12-15)** are the largest steady cost, on the hottest path
   in the program. Making the disabled path a load-and-branch instead of an
   out-of-line call means changing what `patches/n64recomp-ob64.patch` emits, so
   it regenerates all the game code and re-runs the stutter measurement.
4. **The audio auto-response (10)** is a behaviour shim with a documented
   follow-up, not a log line: fixing it means firing the real `OS_EVENT_AI` when
   a buffer drains.

Audit items 11 and 16-20 are small and independent. None of the items above was
changed in this session; the console (items 5 and 6) was.
