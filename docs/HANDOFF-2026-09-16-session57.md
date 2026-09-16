# Handoff — 2026-09-16, session 57: the stale New Game backdrop is a stale readback *source*, not a render-vs-readback race

## Goal and result

**Goal (developer):** fix the intermittent **stale-backdrop artifact** in the New
Game opening (after a form step, the cathedral comes back with a rectangular
region showing the previous screen), and if it is still reproducible, chase the
**intermittent end-of-sequence crash**. Both were open items from session 56.

**Result.**

1. **The artifact is fixed, and its cause is not the one session 56 hypothesised.**
   It is not a render-vs-readback timing race. It is the **readback source
   *selection*** in `tools/njpeg_readback.py`, which preferred RT64's scratch word
   over the game's own choice. That scratch word (`0x807FFC08`) is only ever
   *re*set when a display list sets a YUV texture image followed by a colour
   image, and is **never cleared**, so at the first pass of an assembly it still
   names the *previous* step's target — the buffer that by then holds the previous
   screen. Measured over 10 runs / 120 stage-3 passes: the old rule read the wrong
   framebuffer in **13** passes (always pass 0, always the previous screen); the
   new rule read the right one in **120/120**.
2. **The ordering is already correct, and no handshake is needed.**
   `sp_complete()` does run *before* `send_dl()` (`events.cpp:422` vs `:429`), but
   the game does not proceed on that event: it waits for the **DP** completion
   (`dp_complete()`, `events.cpp:432`, posted *after* `send_dl` returns), on the
   queue it registers at `0x800E8BF4`. A game-thread handshake was implemented for
   this session (an in-flight-gfx-task flag the readback waits on); it **never
   blocked once**, even with the njpeg display list deliberately delayed 400 ms
   inside `send_dl`, and with it disabled the copy's source content was still
   correct in 36/36 passes. It was reverted (see §4). `ogre_sync_framebuffers()`
   (the session-48 SYNC patch) is what actually forces the RDP's pixels back to
   RDRAM before the copy, and it stays.
3. **The crash is still not reproduced** (200 s, `exit 0`), but the run now says
   *where* the sequence ends in the port: scene `0x16` immediately calls two
   functions in the streamed record `bankRec10a` (`0x801D40D0`, `0x801D410C`)
   that report **"not yet loaded"** and are called ~17 439 times each — a stub
   spin, not a fault. See §2.

## 1. The artifact: the readback source, not timing

### What the game actually does (instruction level)

`func_ovlE_8019976C` (`BankEFuncs/funcs_0.c`, bankE `0x8019976C`) is the njpeg
stage-3 copy. Its row loop at `0x80199884` is
`memcpy(state[0x70], state[0x64], 2*state[0x78])` per row with a fixed `0x280`
source stride, over `state[0x7A]` rows; the source is loaded at `0x80199878`
(`lw $s0, 0x64($a0)`), where `state = *(0x8019A680)`.

`state[0x64]` is written at **`0x80199B98`** (in `func_ovlE_80199A08`) from the
game's framebuffer table `D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`,
indexed by a stack byte computed at **`0x80199AF8`-`0x80199B68`**:

```
lbu  v1, 0x800E7A0C          ; a mode flag
sb   zero, 0x10(sp)          ; index = 0
bne  v1, 1, done             ; flag != 1 -> index stays 0
lw   v1, 0x800C4BB8          ; display word
lw   v0, 0x800A8204          ; table[0]
bne  v1, v0, next
sb   1, 0x10(sp)             ; display == table[0] -> index = 1
...
next:  table[1] -> index = 0 ; table[2] -> index = 0 ; no match -> index = 0
```

So `state[0x64]` is `0x80025C00` exactly when the display word is `0x80000400`,
and `0x80000400` otherwise — i.e. **the game reads back the framebuffer that is
not being displayed**. That is a well-defined, deliberate choice, not a
placeholder (session 48's "entry 0 is a ROM placeholder" is wrong; entry 0 is a
real framebuffer address).

The four passes of one assembly and their destinations, from the probe
(`width x height` as the copy reads them: `state[0x78]` is the row length,
`state[0x7A]` the row count, and the source stride is a fixed `0x280` = 320 px):

| pass | destination | width x height |
|---|---|---|
| 0 | `0x801AAE90` | 240x320 |
| 1 | `0x801D06B0` | 240x176 |
| 2 | `0x801E50D0` | 144x320 |
| 3 | `0x801FB8F0` | 144x176 |

Those sizes are a consistent 2x2 grid — columns 240 and 144 wide, rows 320 and
176 tall, i.e. a 384x496 composite — and the developer confirms the scene is
"4 background images in a 2x2 grid" whose **top-left** is the tile that goes
wrong. The only pass that ever reads the previous screen is **pass 0**, so pass 0
is almost certainly that tile — but it is *inferred*, not read out of the blit.

**The framebuffer's own chunk order is not the scene's order.** At the cathedral
step `0x80000400` holds the four chunks 2x2 with black seams
(`docs/proofs/native-newgame-backdrop-chunks.png`), but the developer confirms the
chunks are **out of order** there; reordering and rescaling them is what the
readback plus the blit are for. Do not derive an on-screen position from that
render.

Each pass has its own njpeg display list (ucode `0x800A5110`, the S2DEX2 draw)
and its own copy; the four sources are four deterministic frames
(FNV-1a `573FF47B8A5A3279`, `48892A76C362A4BA`, `3F79B6732153B2FD`,
`38BCBEBE5B555C3C`, in that order). An off-by-one read therefore shows up as a
*frame* mismatch, which is what the measurement below keys on — and the stale one
is always **pass 0**, because it is the first pass after the step change and the
scratch word still names the previous step's target. (Pass 0 is the 240x320
destination; the developer's broken on-screen tile is the top-left, so they are
almost certainly the same chunk — inferred, not verified.)

### Why the patch's rule was wrong

RT64's bridge (`tools/RT64/src/hle/rt64_rdp.cpp`, `OGRE_NJPEG_SCRATCH =
0x7FFC00`) records the colour image a YUV macroblock draw landed in:

* `setTextureImage(fmt == YUV)` sets `+4 = 1`;
* the next `setColorImage` copies the new colour image into **`+8`** and clears
  `+4`;
* `+12` tracks the most recent of the three game framebuffers.

`+8` is never invalidated. `tools/njpeg_readback.py` used it whenever it was
non-zero, so it overrode `state[0x64]` with a target left over from an earlier
display list. In the probe logs the two disagree at **pass 0 of every assembly**:
e.g.

```
[njread] pass=4 gameSrc=0x80025C00 game (scratch cimg=0x025C00 njpeg=0x000400 lastfb=0x025C00 disp=0x80000400) ...
[njread] pass=5 gameSrc=0x80025C00 game (scratch cimg=0x025C00 njpeg=0x025C00 lastfb=0x025C00 disp=0x80000400) ...
```

At pass 4 the scratch still says `0x400` (the previous step's target) while the
game's choice is `0x25C00`; by pass 5 the new draw has re-set it and the two
agree. The wrong buffer at those moments holds the previous screen — the
name-entry form or the date-of-birth form:

* `docs/proofs/native-newgame-readback-stale-source.png` — the buffer the old rule
  copied at pass 0 of the cathedral assembly: the name-entry form.
* `docs/proofs/native-newgame-readback-correct-source.png` — the buffer the game
  selects at the same instant: the backdrop sub-image.

**This also corrects session 56's "the game's own `state[0x64]` index and RT64's
scratch handshake agree in every dump".** They do not; the disagreement is the
artifact.

### The fix

`tools/njpeg_readback.py` now keeps the game's own `state[0x64]` whenever the
word the game derives its index from (`D_800C4BB8`) matches one of the three
framebuffer-table entries at `0x800A8204`, and only falls back to the scratch
word (then `+12`) when it does not. That keeps the fallback for the case the
patch was written for (session 48: the index defaults to entry 0 when the display
word matches nothing) while no longer replacing a correct selection with a stale
one.

### Live-console confirmation (the developer's own repro, A/B)

The measurement above is data-level. The end-to-end check uses the **live console**
(`OGRE_CONSOLE_FILE`, `docs/guides/app-build.md`): a driver polls `c` and issues
`dump` only while the dispatcher reports `desc=8018FC3C` and the cathedral step
`0x021D8002`, so the 8 MiB image is taken *while that step is live*. (A wall-clock
dump schedule does not work — the dumps themselves shift the tap route, which is
why the developer's live dumps are the right instrument.)

Five dumps per build, same state, rendering the assembly destination
`0x80243E28` at 320x240:

| build | `0x80243E28` at the cathedral step |
|---|---|
| scratch word first (the old rule) | the **name-entry form** (`Magnus`, `INS BS DEL END`, the A-Z/a-z grid) — 5/5 dumps, mean luminance 123.7 |
| game's `state[0x64]` first (the fix) | the **cathedral backdrop** — 5/5 dumps, mean luminance 58.8 |

`docs/proofs/native-newgame-backdrop-old-rule.png` and
`docs/proofs/native-newgame-backdrop-fixed.png` are that pair: the previous
screen, and the backdrop. This is the developer's artifact, reproduced and fixed
on the real path.

### Measurement

A temporary probe (`OGRE_NJREAD_LOG=1`, kept — see §4) logs, per pass, the game's
source, the framebuffer the patch chose, an FNV-1a of that buffer's 320x240
content (`sig`, non-zero bytes in the top 16 bits), and the hash the
pre-session-57 "scratch word first" rule would have read (`alt`, `0` when the two
rules agree). `sig` must be the four reference hashes
`573FF47B8A5A3279`/`48892A76C362A4BA`/`3F79B6732153B2FD`/`38BCBEBE5B555C3C`
repeating, and `alt` is non-zero exactly where the old rule read the previous
screen; e.g. from one run

```
pass=4 sig=573FF47B8A5A3279 alt=44C596C1DB18F550   <- 44C5... is the name-entry form
pass=8 sig=573FF47B8A5A3279 alt=405163DD80C3ABB0   <- 4051... is the date-of-birth form
```

so a log alone shows the defect — no reference images needed. Re-classifying each
run's log against the expected sub-image per pass:

| runs | passes | old rule wrong | new rule wrong |
|---|---|---|---|
| `s57-v2-{1,2,3}`, `s57-full-{1,2,3}`, `s57-newrule`, `s57-final-{1,2,3}` | 120 | 13 (always pass 0) | **0** |

The three `s57-final-*` runs are the post-revert build (no handshake) and each
also reaches scene `0x16` (`t≈44.7 s`, 4×). The old rule's failures were always
"the source was not any of the four sub-images", i.e. the previous screen.

### Why the "render-vs-readback race" hypothesis is wrong

* `events.cpp` posts `sp_complete()` **before** `send_dl()` (lines 422/429) but
  `dp_complete()` **after** it (line 432). The game registers `OS_EVENT_DP` on
  queue `0x800E8BF4` — `OGRE_DEBUG_TRACES=1` shows
  `[ev] dp_complete -> mq=0x800E8BF4` 342 times in a 6 s run — so its wait does
  cover the display list.
* **Experiment (one variable):** delaying the njpeg display list by 400 ms at the
  top of `send_dl` (i.e. after `sp_complete`, before `dp_complete`) never let the
  readback run inside the delay. The handshake that was implemented to catch that
  window never blocked, in any run, with or without the delay.
* `ogre_sync_framebuffers()` → `Application::syncFramebuffers` →
  `State::syncFramebuffers` waits for the submitted workload and calls
  `copyLastNativeToRAM` for every framebuffer with rendered content, so the copy
  cannot read RDRAM from before the draw; and the game submits no further display
  list until the copy returns, so the buffer cannot be recycled mid-copy.
* The existing `waitForGameFramebuffers` (`OGRE_NJ_WAIT_MS`) is a no-op once the
  game's framebuffers exist (`[njwait] spins=1 found=1 ms=0`); it costs a single
  500 ms stall on the first display list of the run (`spins=1948 found=0 ms=500`).
  Left as it is; noted because it is not doing what its comment claims.

## 2. The sequence end: scene `0x16` streams a module the port never compiled

Not reproduced as a crash: a 200 s tap-A run (`OGRE_SCENE=title OGRE_SPEED=4
OGRE_TAP_MS=1000
OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a"
OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=200000`) exits **0**, reaches
`scene 0x16` at `t=44672 ms`, grows to 30 943 log lines and prints no
`SIGSEGV`/`SIGBUS`/fault line.

What it *does* show is a wall immediately after `0x16` starts:

```
[overlays] streamed function stub called @ 0x801D40D0 (not yet loaded)   x17439
[overlays] streamed function stub called @ 0x801D410C (not yet loaded)   x17439
```

`tools/guestmap.py 0x801D410C` → `bankRec10a`, ROM `0x213AE0 + 0x38AC`, the record
the port compiles as unit C. But the run with `OGRE_DEBUG_TRACES=1` says which
module is actually resident there:

```
[bank] UNKNOWN module rom=0x244770 ram=0x801D0860 (0x801D0860 is also where rom=0x213AE0 loads)
        the port has no functions for this module: calls into 0x801D0860+ will run
        whichever bank is registered there instead (see docs/symbols.md)
[bank]   scene=0x0016 descriptor=0x8018FC00 record mask=0x00000400
```

Scene `0x16` chunk-DMAs **ROM `0x244770` (≈`0x7500`) → RAM `0x801D0860`** — the
*same* RAM `bankRec10a` owns. The port knows that ROM range but compiled it as a
gap, not code (`config-bankC.yaml`):

```yaml
  # Gap: records 18 (ROM 0x1BA020) and 15/overlay C (ROM 0x1CE040..0x1F0A00) are
  # not part of this unit.
  - type: bin
    start: 0x244770
    vram: 0x244770
```

(the record ends where `bankRec11` starts at `0x24BC70`, so `0x244770..0x24BC70`).
So the game calls into a module the port has no code for, at an address unit C
*does* have code for — the session-45/55 shape exactly. The port turns it into a
17 439-iteration stub spin; the developer's crash is plausibly the same wall with
different timing.

**Next step:** give ROM `0x244770` the session-55 treatment — compile it as a
record and put it in a bank unit *other than* the one that owns `bankRec10a`, so
N64Recomp emits `LOOKUP_FUNC` for the calls into `0x801D0860+` and the runtime's
DMA-driven bank map picks the resident module. `config-bankC.yaml`'s gap entry is
where the record goes; `make bank-recomp` + `cross_bank.py check-banks` is the
invariant.

## 3. What was run for verification

```sh
# the fix, three full-opening runs (each reaches scene 0x16, exit 0)
OGRE_NJREAD_LOG=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a" \
  OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 OGRE_EXIT_AFTER_MS=50000 \
  ./build-app/ogrebattle64 assets/ogre64.z64

# the source-selection A/B analysis (10 runs, 120 passes): old rule 13 wrong, new 0
#   (the old rule is reconstructed from the probe line: njpeg if non-zero, else lastfb)

# forced-old-rule control: one generated-file edit (`if (!displayKnown)` -> `if (1)`,
# tagged probe57c), rebuilt, captured, then reverted with `make bank-recomp`

# the ordering experiment (temporary, reverted): 400 ms sleep at the top of send_dl
# for ucode 0x800A5110 (OGRE_DL_DELAY_MS), with and without the game-thread handshake

# the crash chase: the 200 s run above; then a 52 s OGRE_DEBUG_TRACES=1 run
```

No repo test harness; every check is a ROM-dependent run. stdout must be
redirected for timed runs: the runtime's periodic `[snap]` queue dump (every ~90
VI retraces) goes to **stdout**, and letting it reach a file can stall the boot
for tens of seconds (observed: the title scene arriving at `t=51.9 s` instead of
`t≈3 s`). `> /dev/null 2> log` is the safe form.

## 4. Files changed

* `tools/njpeg_readback.py` — **the fix**: keep the game's own framebuffer
  selection when `D_800C4BB8` matches a table entry; scratch word only as the
  fallback. Docstring rewritten with the measurement and the corrected reading of
  session 48's case.
* `app/src/renderer.cpp` — `OGRE_NJREAD_LOG=1` diagnostic (`readback_probe`): one
  line per stage-3 pass with the game's source, the framebuffer the patch chose
  and an FNV-1a of its content (`src`/`sig`), the counterfactual the old rule
  would have read (`alt`), the scratch words, the display word, and the pass's
  destination/size, plus a `present` index that matches
  `OGRE_CAPTURE_PRESENT`'s file numbering. App code, env-gated, off by default.
* `docs/proofs/native-newgame-readback-stale-source.png`,
  `docs/proofs/native-newgame-readback-correct-source.png` — the same instant
  under the old and new rules (the previous screen vs the backdrop sub-image).
* `docs/proofs/native-newgame-backdrop-old-rule.png` /
  `docs/proofs/native-newgame-backdrop-fixed.png` — the **live-console A/B**: the
  assembly destination `0x80243E28` read while the cathedral step is live, under
  the old rule (the name-entry form) and with the fix (the cathedral backdrop).
* `docs/proofs/native-newgame-cathedral-post-dob.png` — a fresh clean cathedral
  capture, now *past* the date-of-birth form and further into the dialogue than
  any earlier proof ("Magnus Gallant, I ask thee…").
* `docs/guides/njpeg-backgrounds.md` — **new**: the njpeg background pipeline on
  every scene, the `'HU'`/`'HUFF'` asset format, all **75** assets enumerated by
  size (**42 are 320x240** — the same as this backdrop), the four-pass readback
  geometry, the contiguous-320x240-framebuffer trap, and the check recipe. The
  developer's point that "other large backgrounds might use the same size" is
  answered with the header-derived inventory, and the fix is documented as global
  to this path.
* `docs/proofs/native-newgame-backdrop-chunks.png` — the source framebuffer at
  the cathedral step: four background chunks, **out of order** (the developer's
  correction), with the black seams between them.
* `PLAN.md` (the opening bullet), `docs/DECISIONS.md` (two durable rows + a dated
  entry + the session-48 banner correction), `docs/README.md` (the index row),
  `docs/scenes.md` (the opening's status), `AGENTS.md` (the njpeg/cathedral
  load-bearing bullet), this file.

Generated/regenerated (gitignored): `BankEFuncs/` (via `make bank-recomp`, which
re-applies `njpeg_readback.py`).

**Probes: all reverted.** `probe57` (the raw per-pass buffer dumps in
`app/src/renderer.cpp`) was trimmed to the one-line log kept as
`OGRE_NJREAD_LOG`; `probe57b` (an `OGRE_RB_NO_OVERRIDE` switch in the generated
copy) and `probe57c` (forcing the old rule) lived only in the generated
`BankEFuncs/funcs_0.c` and are gone — `grep -rl probe57 Bank*Funcs/ RecompiledFuncs/ app/`
is empty. `OGRE_DL_DELAY_MS` (the 400 ms race-widening sleep) is removed.
The vendored `tools/N64ModernRuntime` change (the in-flight-gfx-task handshake)
was **reverted**, and `git -C tools/N64ModernRuntime diff` is byte-identical to
`n64modernruntime-ob64.patch` (verified). No RT64 file was touched.

## 5. Next leads

1. **Compile the scene-`0x16` module** (§2): ROM `0x244770` (≈`0x7500`) →
   RAM `0x801D0860`, currently a `bin` gap in `config-bankC.yaml` while
   `bankRec10a` owns the same RAM. This is the hard edge right after the New Game
   sequence, it is 100 % reproducible, and it is the same treatment scene `0x07`
   needed in session 55.
2. **`waitForGameFramebuffers` is dead weight** — it never blocks once the game's
   framebuffers exist and costs 500 ms once at boot. If a future session wants the
   500 ms back, delete the call in `send_dl` and the RT64 helper together (the
   readback no longer depends on it).
3. **The artifact is now demonstrated end to end** (§1, "Live-console
   confirmation"): the assembled backdrop `0x80243E28`, read live at the cathedral
   step, is the name-entry form under the old rule and the cathedral with the fix
   — 5/5 dumps each. The stale pass is always **pass 0**; the developer reports
   the broken on-screen tile as the top-left, which pass 0 almost certainly feeds
   (inferred — see the caveat in §1). `OGRE_NJREAD_LOG=1` prints the selection per
   pass if it ever comes back.
4. The intermittent crash: if it reproduces, the crash handler prints the
   faulting host PC, the RDRAM base and the guest address; bind a `dump` key
   (`OGRE_KEY_1='dump'`, `docs/guides/app-build.md` → "The live console").
