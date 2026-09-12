# Handoff — 2026-09-12 — session 26: the boot thread's real stack, and a display list that reaches RT64

## Outcome

Session 25 closed with a scripted-run harness and the idle trajectory localised to
"`func_8008AFE0` (t4) blocks forever on its own queue". That was a **misreading of
the diagnostic**, not the wall. This session rebuilt the diagnostic so it reports
an actual call stack, and used it to answer the "where is the boot thread?"
question that three sessions of snapshot-squinting could not:

1. **Per-thread live call chains.** The recompiled code now calls
   `recomp_trace_entry(rdram, vram, name, ctx->r31)` at every function entry and
   `recomp_trace_return(rdram, index)` before every return (N64Recomp change), and
   the runtime keeps a per-thread shadow stack. `debug_dump_call_chain(tid)`
   prints the live frames, outermost first, and `OGRE_CHAIN_HISTORY=<tid>` records
   the ordered sequence of stack states a boot goes through.
2. **The boot thread's path is now readable, and it is a frame handshake, not
   `func_8008AFE0`.** With `OGRE_CHAIN_HISTORY=3` the boot thread goes
   `func_80071EB0 → func_8008A1B0 → {func_80089AB0 → func_80089A30} →
   func_80089660 → func_80089804` and then never returns from
   `func_80089804` (the frame-submit function). `func_8008AFE0` (t4) is one of
   the post-boot service threads, as is t5/t16/t17/t18/t19.
3. **`func_80089A10`'s runtime override is dead code — it always has been.**
   `librecomp/src/recomp.cpp` defines `func_80089A10_recomp` (yield instead of
   spin), and session 15 recorded it as "the fix that unblocked the boot". It is
   never called: the recompiled `_func_80089A10` is a strong symbol in
   `funcs_5.c.o`, direct calls link to it, and only *function-pointer* lookups go
   through the overlay map. Setting `OGRE_WATCH_FUNC=0x80089A10` and
   instrumenting the override both stayed silent while the boot thread sat in the
   real function. The boot thread therefore executes a
   `while (D_800E79A4 != 0);` loop — no `osRecvMesg`, no yield — in a
   cooperative scheduler: every lower-priority thread on that core only runs
   while the loop happens to yield to the external-message wait.
4. **The native renderer path works up to RT64.** A hand-built F3DEX2 display
   list (`app/src/synth_frame.cpp`, `OGRE_SYNTH_FRAME=1`) is submitted through
   `ultramodern::submit_rsp_task`, reaches `RT64Renderer::send_dl`, and RT64
   executes every command: `setFillColor` fires with the seven colours, and
   `fillRect` reports the seven bar rectangles with the right colour-image
   (320x240 at 0x00700000). The captured window is still black, so what is
   missing is presentation (see "Still open").

## What changed

### N64Recomp codegen — entry/return hooks (`tools/N64Recomp/src/cgenerator.cpp`)

`emit_function_start` now emits

```c
recomp_trace_entry(rdram, 0x80089804, "func_80089804", (uint32_t)ctx->r31);
```

and `emit_return` emits `recomp_trace_return(rdram, <func_index>);` before the
`return;`. `ctx->r31` at entry still holds the caller's return address (the
function has not saved it yet), so every frame knows its call site.

### Runtime — shadow call chain (`ultramodern/src/function_trace.cpp`)

* `ThreadCallChain` per thread: 64 frames of `{ra, func, name, seq, end}`.
* `recomp_trace_entry` pushes (and keeps feeding the session-18 alloc ring);
  `recomp_trace_return` closes the innermost live frame. Frames are printed only
  while live, so the tail of the dump is where the thread is *now*.
* `ultramodern::debug_dump_call_chain(tid, label)`; the VI snapshot also dumps
  every parked thread's chain, and the `OGRE_EXIT_AFTER_MS` path dumps all of
  them at exit.
* `OGRE_CHAIN_HISTORY=<tid>` starts a sampler thread that records each distinct
  live-frame sequence (capped 256 entries) and prints it grouped.
* `get_rdram_base()` / `set_rdram_base()`: the RDRAM base, set by
  `recomp::start`, so runtime-side and app-side diagnostics can reach game memory
  (previously each site used its own copy).

### Diagnostics added

| control | effect |
|---|---|
| `OGRE_DUMP_RDRAM=<path>` | at `OGRE_EXIT_AFTER_MS`, write the whole 8 MiB RDRAM image; analyse offline with the byte rules in `recomp.h` (word at `a` is little-endian at `rdram[a]`; logical byte `a` is at `rdram[a^3]`) |
| `OGRE_CHAIN_HISTORY=<tid>` | record + print the sequence of live call chains for a thread |
| `OGRE_SYNTH_FRAME=1` | build and submit a synthetic F3DEX2 display list (see below) |
| `OGRE_SYNTH_AT_MS`, `OGRE_SYNTH_PERIOD` | when to first submit, and how many VIs between re-submits (default 30) |
| `OGRE_NO_DUMMY_VI=1` | skip the pre-start dummy VI workload that clears the screen every VI (it would hide a diagnostic draw) |

### `app/src/synth_frame.cpp` — synthetic display list probe

Builds an F3DEX2 list in RDRAM at 0x80500000 (below the 64 MiB `& 0x3FFFFFF`
RT64 mask, above the game's first-megabyte data), with a task header 0x8000 bytes
above it, and submits it from the VI thread's per-vblank callback. The list is
`G_SETSCISSOR` (full screen) + seven `G_SETCIMG`/`G_RDPSETOTHERMODE`/
`G_SETFILLCOLOR`/`G_FILLRECT` colour bars + `G_RDPFULLSYNC` + `G_ENDDL`.

Two traps this session paid for, both now commented in the file:

* **Pointers are `int32_t` and `TO_PTR` sign-extends.** A task pointer must be a
  KSEG0 address (`0x80000000+`); a bare physical offset (`0x0D000000`) is
  positive, sign-extends to `0xFFFF_FFFF_0D00_0000`, and bus-errors.
* **`OSTask::t.data_ptr` must hold a physical offset**, because `send_dl` passes
  `data_ptr & 0x3FFFFFF` to RT64. With the list above 64 MiB the mask aliases it
  to a different, zero-filled area, and the renderer sees an empty list.
* RT64's `RDP::fillRect` only merges a rect into `drawColorRect` when a scissor
  is set, so a fill-only list silently does nothing without `G_SETSCISSOR`.
* Opcode packing: `G_FILLRECT` puts `ulx@12|uly@0` in `w0` and `lrx@12|lry@0` in
  `w1`; `G_SETSCISSOR` puts `ulx@12|uly@0` in `w0` and `mode@24|lrx@12|lry@0` in
  `w1` (see `rt64_gbi_rdp.cpp`).

## Evidence

### The boot thread's stack (exit dump, `OGRE_CHAIN_HISTORY=3`)

```
[snap] chainhistory: last 13 entries (grouped):
[snap]    80071EB0 80071EB0
[snap]    8008A1B0
[snap]    80071EB0
[snap]    8008A1B0
[snap]    80089AB0
[snap]    80089A30
[snap]    80071EB0
[snap]    8008A1B0
[snap]    80089660
[snap]    80071EB0
[snap]    8008A1B0
[snap]    80089804
```

The last state never changes: `func_80071EB0` (thread entry) → `func_8008A1B0`
(graphics bring-up) → `func_80089804` (frame submit) and stuck. All `ogre_exit`
snapshots agree (`callchain t3 at exit (3 deep …)`).

### `func_8008AFE0` is not the wall

`OGRE_CHAIN_HISTORY=4` shows t4 entering `func_8008AFE0` and parking in
`func_80089054`'s `osRecvMesg`; `func_80089054` is the retrace-subscriber
registration, and the queue the snapshot watches (`0x800C4C28`) *is* fed — the
send trace shows `t19 → mq=0x800C4C28 msg=0x800E8B10` dozens of times. Session
25's "nothing ever sends to that queue" was an artifact of the old
resume-count/queue-snapshot view.

### The renderer half is verified

```
[synth] submitting synthetic display list #1 (dl=0x80500000 size=312) at T=…
[renderer] display list 2 (type=1 ucode=0x8009F540 data=0x80500000)
[rt64] setFillColor FF0000FF          (…FF00FF00, FFFF0000, FFFFFFFF, FF00FFFF, FFFF00FF, FFC0C0C0)
[rt64] fillRect ulx=0 uly=0 lrx=45  lry=0 fill=FF00FF00 cyc=0 scissor=1 cimg=00700000 320x2
[rt64] fillRect ulx=0 uly=0 lrx=90  lry=0 fill=FFFF0000 cyc=0 scissor=1 cimg=00700000 320x2
… (seven bars)
```

`screencapture -l <window id>` still returns a black canvas (`colorful=0`), and
so does a raw write of bars straight into the framebuffer at 0x80700000 (the
bytes are confirmed present in the `OGRE_DUMP_RDRAM` image). So RT64 receives,
parses and executes a real display list, but the window never shows the result —
either the presenter samples a different surface, or this window-capture method
does not see the Metal layer.

## Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean |
| `make recomp` after the codegen change | regenerates with `recomp_trace_entry`/`recomp_trace_return` |
| shell chain dump, t3 | stable, correct `func_80071EB0 → func_8008A1B0 → func_80089804` |
| parked-thread chains (t4/t5/t16/t17/t18/t19) | `func_8008AFE0`, `func_80089540`, `func_80089358`, `func_800893C0`, `func_80089200`, `func_80088F08` |
| `OGRE_DUMP_RDRAM` | 8 388 608 bytes, values match the queues/threads the traces describe |
| synthetic frame | submitted, parsed, executed by RT64 (see above) |
| window capture | still black, before and after the synthetic frame |

## Still open

0. **Get one real pixel on the native screen.** Two candidates, in order:
   (a) the presenter never composites the framebuffer the game draws into —
   check `RT64::State::updateScreen` / `WorkloadQueue` versus the framebuffer
   the `G_SETCIMG` targets, and whether `osViSwapBuffer` has to name it;
   (b) the capture path (`screencapture -l`) misses RT64's Metal layer — try a
   full-screen `screencapture -x` with the window positioned, or RT64's
   `presentEarly`/screenshot path. Everything below the presenter is now proven.
1. **`func_80089A10` must not spin.** The game's `while (D_800E79A4 != 0);` is
   real and high-priority; the runtime cannot preempt it. Options: register the
   runtime's yielding implementation in the overlay function map *and* make
   direct calls resolve through the map, or patch the generated call site, or
   give the scheduler a preemption/yield tick. This is the actual boot gate and
   it is a one-function problem now — but note that the counter is 0 in the
   RDRAM dump while the thread is inside the loop, so log the value at entry
   before assuming it is stale state.
2. **`func_80089804` completion handshake.** Even with `A10` fixed, the boot
   thread is waiting on the frame's SP/DP completion path
   (`func_80089540`'s `D_800E79A4` decrement). One RSP display list is submitted
   in a whole run; a successful boot needs that to become a per-frame loop.
3. **The title scene still needs its assets.** The web build reached them; the
   native boot never builds the game's real display list, and the title textures
   (web log: `G_SETTIMG 0x801C41E0`, `TEXEL1 0x001C3C88`) are not present in the
   native RDRAM dump at stall time. A browser-side RDRAM dump (the web app needs
   an export like this session's `OGRE_DUMP_RDRAM`) would give both the reference
   frame and the assets to replay natively.
4. **Untouched from session 24:** the raw-TEXEL1 black report, the remaining
   combiner inputs (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys), coverage/alpha
   compare, framebuffer/VI indirection, the TMEM model, `G_LOADBLOCK`'s `dxt`.
5. The `[snap]` retrace-handler / dispatcher-callback walkers still print an
   **empty list while the fan-out demonstrably runs** — the send trace shows
   `func_800891A0` (the list walker) `osSendMesg`-ing to `0x800C4C28` dozens of
   times, i.e. the list it walks is not empty at run time. So either the
   snapshot's read is wrong or the list is rebuilt per call. Do not use those two
   `[snap]` lines as evidence until this is settled.

## Repro

```sh
cmake --build build-app -j 8

# boot thread's path (the session's key measurement)
OGRE_CHAIN_HISTORY=3 OGRE_EXIT_AFTER_MS=3000 ./build-app/ogrebattle64 2>&1 | \
  sed -n '/chainhistory/,/callchain t1/p'

# full memory image at the stall
OGRE_DUMP_RDRAM=/tmp/ogre.rdram OGRE_EXIT_AFTER_MS=6000 ./build-app/ogrebattle64

# renderer-path proof: a real display list into RT64
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=1000 OGRE_SYNTH_PERIOD=30 \
  OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64
#   then capture the window (see docs/guides/app-build.md)
```

## Files changed (tracked)

- `tools/N64Recomp/src/cgenerator.cpp` — entry/return trace hooks (with name and
  return address).
- `tools/N64ModernRuntime/N64Recomp/include/recomp.h` — declarations.
- `tools/N64ModernRuntime/ultramodern/src/function_trace.cpp` — shadow call
  chain, chain-history sampler, RDRAM base accessors.
- `tools/N64ModernRuntime/ultramodern/include/ultramodern/ultramodern.hpp` —
  `debug_dump_call_chain`, `debug_chain_history_*`, `get_rdram_base`.
- `tools/N64ModernRuntime/ultramodern/src/mesgqueue.cpp` — parked-thread call
  chains in the snapshot; first-send logging for the two never-fed queues.
- `tools/N64ModernRuntime/ultramodern/src/threads.cpp` — `osStartThread` logs the
  starter's chain.
- `tools/N64ModernRuntime/ultramodern/src/events.cpp` — `OGRE_NO_DUMMY_VI`.
- `tools/N64ModernRuntime/librecomp/src/recomp.cpp` — RDRAM base for diagnostics.
- `app/src/synth_frame.{hpp,cpp}` (new) — synthetic display list probe.
- `app/src/sdl_platform.cpp` — per-thread call-chain dump and `OGRE_DUMP_RDRAM`
  at the scripted exit; VI callback wiring for the probe.
- `app/src/renderer.cpp` — probe task plumbing (no behaviour change).
- `docs/DECISIONS.md`, `PLAN.md`, `docs/guides/app-build.md` — session-26 entries.
- `RecompiledFuncs/*` — regenerated.
