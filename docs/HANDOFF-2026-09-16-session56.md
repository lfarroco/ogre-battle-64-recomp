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

### Found (session 56, from the developer's live dumps)

The developer ran the game with `OGRE_KEY_1='dump /tmp/at-now.bin'` and pressed
it **while the artifact was on screen**. That dump is decisive:

* **The artifact is baked into the game's display framebuffers, not the
  renderer.** All three of the game's framebuffers
  (`0x80000400`/`0x80025C00`/`0x8004B400`, table at `0x800A8204`) contain it;
  two of them differ from each other only in rows 28-35. A display list drew it
  into each buffer, so no RT64 scissor or presenter fix can address it.
* **The game copied a snapshot of the *form* screen into its backdrop-assembly
  buffer.** At the readback destination guest `0x80243E28` (heap; the `'B5'`
  image's pixels — see `docs/HANDOFF-2026-09-15-session50.md`) the dump holds a
  **complete, clean picture of the name-entry form** (`Magnus`, the `A–Z`/`a–z`
  grid, `INS BS DEL END`). It is not in the later dump, so it is a live copy
  destination. Diagrammed:

  ```
  form screen  --[readback, func_ovlE_8019976C stage-3]-->  0x80243E28
  0x80243E28  --[blit, drawn into every display buffer]-->  artifact
  ```

  So the backdrop a later cathedral step draws is not the cathedral: it is a
  frame that was already showing the form. That is exactly what the screen
  shows, all three buffers included.
* Which framebuffer that readback *read* is the port's `njpeg_readback.py`
  patch's decision. In this dump the RT64 scratch word (`0x807FFC00`) says
  `+8` (the YUV handshake target) = `+12` (most recent game framebuffer) =
  `0x0004B400`, while the buffer whose content is the **form** is `0x00000400`.
  The copy therefore did **not** take `0x8004B400`, which makes the
  handshake/fallback selection the prime suspect (the patch is the only thing
  that overrides the game's own `state[0x64]`).

**Conclusion:** the artifact is a *readback-source* defect — the stage-3 copy
reads a framebuffer that still holds the previous screen — not a missing draw
and not a stale-destination problem. The earlier "70% x 30%" measurement was
this rectangle inside the copied frame, which is why it never matched a readback
sub-image width (176/320).

### The transition series (developer, third round of dumps) — the race, caught

At `OGRE_SPEED=1`, confirming the form input and pressing the key repeatedly
through the transition produced two series (A: 7 dumps, B: 8 dumps). Reading
`0x80243E28` (the assembly destination) and the three display framebuffers:

| dump | scene | step | `next` | scratch cimg/njpeg/lastfb | `0x80243E28` |
|---|---|---|---|---|---|
| A `0001` | `0x0000` | 7 | 0 | `04B400/04B400/04B400` | throne-room backdrop (correct) |
| A `0003` | `0x0000` | 7 | 0 | **`000400`**/04B400/000400 | mixed |
| A `0004` | `0x0100` | 7 | 0 | `04B400/04B400/04B400` | name-entry form |
| A `0005` | `0x0100` | 2 | `021D` | `000400`/04B400/`000400` | **half-written** throne room (correct rows in the middle, white bands top/bottom) |
| A `0007` | `0x0000` | 2 | `021D` | `04B400`/**`025C00`**/`04B400` | name-entry form (final) |
| B `0001`-`0004` | `0x0000`/`0x0100` | 7 | 0 | `04B400` | name-entry form |
| B `0005` | `0x0100` | 2 | `021D` | `04B400` | name-entry form |
| B `0006`-`0008` | `0x0100` | 2 | `021D` | **`000400`** | `0x00010001` fill (buffer recycled after the step) |

Three things follow.

1. **The assembly is caught mid-write** (`A0005`): part of the buffer holds the
   freshly copied backdrop rows and part is still unwritten. The copy for one
   step is not atomic with respect to the dumps, and it is *interleaved* with
   the renderer writing an older generation into the same RAM range.
2. **The same destination address is reused across the transition**, and once a
   step has settled `0x80243E28` is already cleared to the `0x00010001` fill
   (`B0006`-`B0008`). A backdrop is therefore only observable *during* the step
   - which is why the earlier late-dump readings looked contradictory.
3. **The scratch colour image changes between nearly simultaneous dumps**
   (`000400` -> `025C00` -> `04B400`), so the renderer and the game are cycling
   framebuffers while the readback is in flight.

**The on-screen proof.** Rendering the *display* framebuffer (`0x80000400`) from
series B `0008` shows the artifact exactly as the developer described: the
cathedral backdrop across the lower screen, and the name-entry form's grid plus
its dialogue-box region across the upper screen. So the failing draw is reading
a buffer that still holds the name form.

### Post-dump work (same session, after the developer's live dumps)

**The good/bad pair (developer, second round of dumps).** Two live dumps from
one run, both taken while scene `0x0D` was the active descriptor
(`0x8018FC3C`). Rendering the readback destination `0x80243E28` at 320x240:

| dump | when | `0x80243E28` holds | scratch `cimg`/`njpeg`/`lastfb` |
|---|---|---|---|
| `0001`/`0002` | cathedral at scene start, **no artifact** | the **throne-room backdrop** (the correct pre-rendered image) | `0x00025C00` |
| `0003` | right after the birthday input, **artifact** | the **date-of-birth form** (`BIRTHDAY`, `Jul. 25`, `Trueno 12`) — the previous screen | `0x00000400` |

That is the bug in one line: **the assembly copied a frame that still showed the
previous screen.** In the bad dump all three display framebuffers
(`0x80000400`/`0x80025C00`/`0x8004B400`) hold the correctly rendered current
frame (mean luminance ≈ 68, 140 distinct values, no near-black), so the backdrop
*was* drawn — just not into the buffer the readback copied.

**Why selection is not the whole story.** In both dumps the readback source
agrees with itself: the game's own `state[0x64]` index and RT64's scratch
handshake select the *same* buffer. In the good dump that buffer holds the
correct backdrop; in the bad dump it does not. So neither the session-48 patch
nor the game's index is "wrong" in the sense of disagreeing — the failing
condition is *which framebuffer the backdrop render had landed in by the time
the CPU copy ran*. The copy is racing the render.

* **A/B: reverting `tools/njpeg_readback.py` changes nothing.** Reverted,
  rebuilt, re-ran the same tap route, dumped at the same point:
  `control vs reverted backdrop: 2120/2160 pixels identical`, and the reverted
  destination is the same birth form. The patch was restored afterwards.
* **A `probe60` readback trace (temporary, reverted — §3) showed the four-pass
  shape and the source selection** for three assemblies in one run:

  ```
  PASS#0..3 src=0x00025C00 w=320/176/320/176 h=240/240/144/144   (movie step)
  PASS#4..7 src=0x00000400 ...                                   (cathedral step)
  PASS#8..11 src=0x00000400 ...                                  (birth step)
  scratch per pass: cimg=<src>  yuvsig=00000001  njpeg=<src>  lastfb=<src>
  ```

  The source is constant for all four passes of one assembly and equals the
  game's own `state[0x64]`; the scratch handshake agrees with it in the working
  run. In the developer's failing run the same machinery picked a buffer still
  showing the form — same code, different timing. That is why the earlier
  "readback source vs draw" question resolved to *timing of the source buffer*.

### The fix to try next (not implemented — the session ended here)

**The A/B was run and the patch is *not* the cause** (see *Post-dump work*
below). The defect is **render-vs-readback timing**:

1. The stage-3 copy must not run until the njpeg/backdrop render for *this*
   step has landed in the buffer it is about to read, and that buffer must not
   be recycled while it runs. Today the port completes the emulated RSP task as
   soon as the display list reaches RT64 and the game's CPU copy follows
   immediately; `ogre_sync_framebuffers()` /
   `Application::waitForGameFramebuffers` bound that wait but do not guarantee
   the copied buffer holds the current frame. Read
   `waitForGameFramebuffers` and the `OGRE_NJ_WAIT_MS` path in
   `app/src/renderer.cpp` against this series.
2. Candidate fix: a per-assembly handshake — when the njpeg gfx task is
   retired, record the colour image RT64 actually drew into (the scratch word
   already carries it) **and hold the game's stage-3 copy until that image is
   committed to RDRAM**; `dump 0005`'s half-written buffer is what "not yet
   committed" looks like. This is a narrower, timing-only successor to the
   session-48 patch, which is why that patch is a no-op here.
3. The same series reproduces deterministically at 1×: confirm a form input,
   press the key repeatedly through the transition, and `0005`-style partial
   assemblies should appear again.

### Repro (the maintained tap route; the artifact is a race, so capture densely)

```sh
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a,…" \
  OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 \
  OGRE_CAPTURE_PRESENT=/tmp/s56 OGRE_CAPTURE_EVERY=40 OGRE_CAPTURE_AFTER=600 \
  OGRE_EXIT_AFTER_MS=150000 ./build-app/ogrebattle64 assets/ogre64.z64
```

The form→cathedral transitions land at presents ~3000-3200 (name) and
~3700-4100 (date of birth) with `OGRE_CAPTURE_EVERY=40`.

### What the earlier probing established (before the live dump)

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
  That is the "a backdrop assembled from a buffer that is not re-rendered per
  step" shape, and the live dump confirms it is the actual defect.
* The artifact rectangle does **not** match any of the four readback sub-image
  widths (176 and 320); it is not a stale *sub-image*, it is content inside one
  copied frame.
* Reading the backdrop out of a *late* dump is not a valid diagnostic: those
  RAM ranges get reused. The developer's on-demand `dump` while the artifact was
  live is what resolved it (the destination was captured with the copy still in
  it, which an exit dump never manages).

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

1. **Fix the readback source for the backdrop assembly** (§1, *Found*): the
   destination `0x80243E28` received a frame that was still showing the form.
   Try the cheap A/B first (`python3 tools/njpeg_readback.py --revert`, rebuild)
   and see whether the artifact vanishes — if it does, the session-48 patch's
   fallback chain is the regression and needs to prefer the buffer the current
   njpeg decode actually produced. The live console's `dump` + `rdram.py image
   0x80243E28 --width 320` is the before/after check.
2. **The end-of-sequence crash** (§2): still unreproduced in the port (the
   developer's later run reached the end, faded to black and did not crash).
   Reproduce with the crash handler and `dump` bound to a key.
3. `docs/scenes.md` still has the birthday/personality rows as
   developer-reported; their screenshots/descriptions would close steps 4-6
   (AGENTS §1).
