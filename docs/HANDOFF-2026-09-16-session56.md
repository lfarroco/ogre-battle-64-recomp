# Handoff — 2026-09-16, session 56: the opening runs past the name form; one background artifact and the sequence-end crash remain

## Goal and result

**Goal (developer):** after session 55 fixed the name-entry form (scene `0x07`),
continue the opening and chase the two things the developer reported:

1. some textures from the form are still visible in the background when the
   screen returns to the cathedral (intermittent), and
2. the sequence crashes when it ends.

**Result.** The opening is confirmed to run much further than any session has
seen:

```
t= 1.97s title (0x04)
t= 3.37s new-game loader (0x02)
t= 3.50s step 0x0D (movie, step 1)
t= 4.87s step 0x0D (cathedral, step 2)
t=19.17s scene 0x07 (name form)          <- session 55
t=21.94s step 0x0D (cathedral, date of birth)   <- the developer's report
t=24.35s scene 0x07 (date-of-birth form)
t=25.65s step 0x0D (personality questions, steps 3..8)
t=54.26s scene 0x16 (descriptor 0x8018FC00, mask 0x400)
```

The developer confirms the date-of-birth question and the personality questions
both render and are playable, and that the crash is at the sequence's end.
`0x16` is **new** (no prior session reached it) and its descriptor is
`801844E4 80184658 80184794 801848C8 00000400`.

The **artifact is not fixed**; §1 is what this session established about it. The
crash is **not reproduced in the port's own runs** (a 150 s tap-A run exits 0
through `0x16`); §2. Both are written up with the evidence and the next
discriminating experiment, because neither was pinned down here.

## 1. The stale-background artifact

**Symptom (developer, plus their screenshot).** After the name and the
date-of-birth forms, the cathedral backdrop comes back with a *rectangular
region* — the developer measured it at roughly 70% of the screen width and 30%
of the height — showing the **previous screen's content** (the form's character
grid) instead of the cathedral. It is intermittent: it happened after the name
question in one run and after the date-of-birth question in another.

**Repro (the maintained tap route; the artifact is a race, so capture densely):**

```sh
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a,…" \
  OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 \
  OGRE_CAPTURE_PRESENT=/tmp/s56 OGRE_CAPTURE_EVERY=40 OGRE_CAPTURE_AFTER=600 \
  OGRE_EXIT_AFTER_MS=150000 ./build-app/ogrebattle64 assets/ogre64.z64
```

The form→cathedral transitions land at presents ~3000-3200 (name) and
~3700-4100 (date of birth) with `OGRE_CAPTURE_EVERY=40`.

### What was established

* The backdrop is re-assembled from a **CPU readback** each time the sequence
  builds a step (sessions 47/48/50): `func_ovlE_8019976C`'s stage-3 row loop
  copies a framebuffer into an image, in **four passes sized 320x240, 176x240,
  320x144, 176x144** (one `'B5'` asset's four sub-images, ROM `0x7CADAC`).
* The port patches that copy's **source** to "the buffer the njpeg YUV draw
  landed in" (`tools/njpeg_readback.py`; RT64 keeps it in a scratch word at
  `0x807FFC00+8`, falling back to the most recent game framebuffer at `+12`,
  then to the game's own `state[0x64]`). The game's own index selects entry 0
  of the framebuffer table `0x800A8204 = {0x000400, 0x025C00, 0x04B400}`
  whenever the display word matches nothing — the session-48 finding.
* **A temporary probe (`probe59`, reverted — §3) logged the readback**: every
  pass copies from **`src=0x80000400`**, and the same source produces the *same
  content* across successive assemblies (`row0`/`row239` checksums repeat).
  That is consistent with a backdrop assembled from a buffer that is not
  re-rendered per step, but it does **not** by itself explain a rectangle.
* The artifact rectangle (~70% x 30%) does **not** match any of the four
  readback sub-image widths (176 and 320), so "a stale sub-image" is ruled out
  as the direct cause.
* A dump-and-render of the four readback destinations after the transition
  (`OGRE_DUMP_RDRAM`, render each at its own size) showed **static, not the
  cathedral**: by the time the dump is written those RAM ranges have been
  reused. Reading the backdrop out of a late dump is therefore not a valid
  diagnostic; it has to be captured at the moment of assembly.

### The next discriminating experiment (not run — the session ran out of budget)

The artifact is a race, so catch it in the act rather than inferring it:

1. Re-add a readback probe (source, destination, width, height, and a
   **checksum of the source's first and last rows**) plus
   `OGRE_DUMP_RDRAM` **at the moment of the copy** (the exit dump is too late).
2. Enter the cathedral with the form's grid still in the framebuffer, and check
   whether the readback's source contains the cathedral (then the artifact is
   in the *draw*: a missing/partial backdrop rectangle) or the form (then it is
   the *readback source*: the scratch-word handshake or the `+12` fallback
   selected a buffer the current step never drew).
3. A cheaper A/B first: `OGRE_NJPEG=0` (forces the njpeg stub) and
   `python3 tools/njpeg_readback.py --revert` change the readback's behaviour;
   if the artifact's character changes, the readback is implicated. Neither was
   run this session, so do not assume it.

## 2. The sequence-end crash

Not reproduced in the port's own runs: the 150 s tap-A run above reaches
`0x16` at `t=54.3 s` and exits 0, and no `SIGSEGV`/`SIGBUS`/host-fault line is
in its log. The last line of that run is a **streamed stub call** while
`D_800E810E`/`D_800C4BBC` are mid-frame, i.e. the run ended while `0x16` was
starting, not in a fault.

The developer's crash is at the end of the sequence and may need a different
input path (the personality answers, or the closing movie of step 10) or more
wall time than 150 s at 4x. Reproduce with `OGRE_SCENE_LOG=1 OGRE_PROBE57=1` and
the crash handler's output (it prints the faulting host PC, the RDRAM base and
the guest address) — the next session should start there, with the developer's
own save/flow if the port's tap route cannot reach it.

## 3. What was run for verification

```sh
# the full opening, tapped through every question (the run that reached 0x16)
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,…" \
  OGRE_SCENE_LOG=1 OGRE_PROBE57=1 OGRE_EXIT_AFTER_MS=150000 \
  ./build-app/ogrebattle64 assets/ogre64.z64          # exit 0, no fault

# the readback instrumentation (probe59, reverted)
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 OGRE_TAP_BUTTON="…" \
  OGRE_SCENE_LOG=1 OGRE_READBACK_TRACE=1 OGRE_EXIT_AFTER_MS=40000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
# -> [probe59] src=0x00000400 dst=0x801AAE90 w=320 h=240 row0=… row239=…
#            src=0x00000400 dst=0x801D06B0 w=176 h=240 …
#            src=0x00000400 dst=0x801E50D0 w=320 h=144 …
#            src=0x00000400 dst=0x801FB8F0 w=176 h=144 …   (x2 passes)

# the forced-scene form still renders after the revert
OGRE_SCENE=0x07 OGRE_SPEED=4 OGRE_EXIT_AFTER_MS=6000 ./build-app/ogrebattle64
```

**Probes:** one temporary probe, `probe59`, in the **generated**
`BankEFuncs/funcs_0.c` (a readback trace plus `<stdio.h>`/`<stdlib.h>`). It was
reverted with `make bank-recomp` (which also re-applies
`njpeg_readback.py`) and `grep -rl probe59 Bank*Funcs/ RecompiledFuncs/ app/`
is empty. `build-app` rebuilt clean and the forced-`0x07` check passes.

No repo test harness; every check is a ROM-dependent run.

## 4. Tooling built this session: the live console

The developer's suggestion — "let the agent extract data from the running game
on demand instead of guessing when to dump" — is implemented in
`app/src/sdl_platform.cpp` (namespace `ogre::console`) and documented in
`docs/guides/app-build.md` -> "The live console".

* **Trigger 1: a watched command file.** `OGRE_CONSOLE_FILE` (default
  `/tmp/ogre-console.txt`): when the file exists, its lines run and the file is
  removed, so an agent can drive a live run with its own tools.
* **Trigger 2: number keys `1`..`9`** → `OGRE_KEY_1`..`OGRE_KEY_9`, edge
  triggered.
* **Commands**: `r`/`rh`/`rb`/`rk` (word/half/logical-byte/raw-word reads),
  `d` (hex+ASCII), `f` (word search), `fb` (byte-pattern search), `s` (strings),
  `k` (fnv1a checksum — the cheap before/after for A/B runs), `w` (a real word
  store), `c` (scene/pending/descriptor/mask/step/next/spin), `dump [path]`
  (whole 8 MiB **at this instant**), `help`.
* It runs on the **main thread** from `pump_sdl_events` (called by `update_gfx`),
  not the game thread: a multi-megabyte dump must not race the game thread's own
  reads, and the main thread already owns the SDL event pump.
* Byte order matches `tools/rdram.py`: the logical word at guest `a` is the
  little-endian word at `a - 0x80000000`, the logical byte is `addr ^ 3`.

Verified against a live run:

```
[console] > c
[console] scene=0x0000 pending=0x0000 desc=0x8018FB40 mask=0x00008000 step=0 next=0x0000 spin=0x0000
[console] > d 0x80070C00 32
[console] 80070C00  3C 08 80 0B 25 08 ED B0 …  <...%...
[console] > k 0x80000000 4096
[console] fnv1a(0x80000000, 0x4096) = 308BC6DE
[console] > f 0x80000000 3
[console] found 80000000 at guest 0x800AC504 (file 0x0AC504)   (…3 hits, limit)
```

This is what the next session should use for §1: `dump` **at the moment the
backdrop is assembled** (a key bound to `dump`, or the file dropped in while the
form→cathedral transition is on screen), then read the image with
`tools/rdram.py image`.

## 5. Files changed

* `app/src/sdl_platform.cpp` — the live console (`ogre::console`: command
  parser/executor, the watched command file, the `1`..`9` hotkeys), called from
  `pump_sdl_events` on the main thread. This is app code, not a probe: it is
  gated behind `OGRE_CONSOLE_FILE` / `OGRE_KEY_<n>`, so an ordinary run behaves
  exactly as before.
* `docs/guides/app-build.md` — "The live console" in the diagnostics toolkit.
* `AGENTS.md` §4 — when to reach for it instead of an exit dump.
* `PLAN.md`, `docs/scenes.md`, this file — the confirmed flow through the
  personality questions and scene `0x16`, the two open items above, and the new
  tool.
* Generated (gitignored, regenerated): `BankEFuncs/`, `app/src/bank_funcs.inc`.
* `probe59` (the readback instrumentation) is **reverted** — §3.

## 6. Next leads

1. §1's step 2: instrument the readback at the moment of the copy and decide
   readback-source vs draw-rectangle for the stale region.
2. §2: reproduce the end-of-sequence crash with the crash handler and the
   developer's flow; scene `0x16` is the last scene reached, so start there.
3. `docs/scenes.md` still has steps 4-6 unknown; the developer's run confirms
   the birthday and personality screens, which should be filled in from their
   descriptions (screenshots welcome — AGENTS §1).
