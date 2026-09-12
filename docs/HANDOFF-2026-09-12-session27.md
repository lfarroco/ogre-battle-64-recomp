# Handoff — 2026-09-12 — session 27: the native window renders

## Outcome

Session 26 ended on "RT64 receives, parses and executes a real display list, but
the window never shows the result", with two candidate explanations (the
presenter never composites; `screencapture` cannot see the Metal layer). Both
were wrong, and so was the evidence behind the session's headline: **every
capture-based conclusion in session 26 was measured through a locked display.**

This session:

1. **Replaced the measurement.** `OGRE_CAPTURE_PRESENT=<path>` GPU-reads back the
   exact swap-chain texture RT64 is presenting and writes a PPM; there is no
   window server in the path. `OGRE_CAPTURE_TARGET=<path>` does the same for the
   render target the VI renderer sampled. This needed texture → buffer readback
   in plume's Metal backend (`rt64-plume-ob64.patch`).
2. **Found the real display-list bugs.** The hand-built F3DEX2 list in
   `app/src/synth_frame.cpp` had four independent encoding bugs; "RT64 executed
   every command" was true but no command ever rasterized anything (details
   below).
3. **Found two presenter behaviours that a stalled boot exposes.** RT64 is
   change-driven, so once the VI and its RDRAM copy stop changing the presenter
   stops (the traced present count freezes at ~10); and the presenter reads its
   framebuffer from the framebuffer manager, which does not know a colour image
   that was only set and filled, so it uploaded the empty RDRAM copy instead of
   the drawn render target.
4. **A native frame is on screen.** The seven-colour-bar probe now renders
   through the real RDP display-list path and is presented:
   `docs/proofs/native-synth-frame-rdp.png` (1280x715, the presented swap-chain
   texture).

## The capture problem (read this first)

`CGWindowListCopyWindowInfo` (a 20-line Swift helper, see "Repro") shows a
`loginwindow` window at layer 2004 covering the full screen: **the display is
locked**. On a locked display:

* `screencapture -x` (whole screen) returns only the wallpaper;
* `screencapture -l<windowid>` (one window) *does* return window contents — a
  Chrome window captures perfectly — but it returns the **last frame the window
  server committed**, which can be seconds old. With a clear colour that cycles
  every presented frame, ten captures over three seconds all showed the same
  colour.

So a window capture can confirm a still frame, but it cannot decide whether the
renderer is producing new frames. Session 26's "black canvas" was a real
capture of a stale black frame, not the renderer's current output.

## What changed

### RT64 — present capture (`tools/RT64/src/hle/rt64_present_queue.cpp`)

* `OGRE_CAPTURE_PRESENT=<path>` / `OGRE_CAPTURE_TARGET=<path>` /
  `OGRE_CAPTURE_AFTER=<n>`: copy the swap-chain texture (or the VI's render
  target) into a `RenderBufferDesc::ReadbackBuffer`, map it after the present
  worker's fence, write `<path>.<n>.ppm` (BGRA8) or `.bin`.
* `OGRE_PRESENT_ALWAYS=1`: `State::updateScreen` forces `viDifferent = true`, so
  a present is pushed on every VI even when nothing changed.
* `OGRE_PRESENT_FBTARGET=1`: when `framebufferManager.find(VI address)` is null,
  ask the render target manager directly and present a non-empty target at that
  address instead of uploading RDRAM.
* `OGRE_PRESENT_MAGENTA=1`, `OGRE_PRESENT_SIGNAL=1` (+`_CYCLE`): flat-colour
  presentation diagnostics.
* `OGRE_PRESENT_TRACE=1`: `[present]`/`[state]`/`[signal]` traces (which path,
  what the RAM copy holds, whether the target is empty).

### plume (Metal) — texture → buffer readback (`tools/RT64/src/contrib/plume/plume_metal.cpp`)

`MetalCommandList::copyTextureRegion` only implemented buffer → texture. The
mirror case is added, and it has to fetch the source through
`ExtendedRenderTexture::getTexture()`: a swap-chain texture is a `MetalDrawable`,
not a `MetalTexture`, so reading `MetalTexture::mtl` off it is undefined
behaviour (it aborted inside `copyFromTexture:toBuffer:` with
`-[__NSCFType textureType]: unrecognized selector`).

### Probe fixes (`app/src/synth_frame.cpp`)

The list "executed" for four independent reasons without drawing. All four are
fixed and commented in the file:

| # | bug | effect |
|---|---|---|
| 1 | every two-word RDP command emitted as **two** `emit()` calls (4 words) | `G_SETSCISSOR` got `w1 = 0` (a **null scissor**, followed by a garbage opcode) and `G_FILLRECT` got `lrx = lry = 0` (an **empty rectangle**); `RDP::drawRect` returns early for an empty rect and only merges `drawColorRect` when a scissor is set, so nothing was recorded |
| 2 | `G_FILLRECT` operand halves swapped | RT64's `GBI_RDP::fillRect` reads `ulx/uly` from **w1** and `lrx/lry` from **w0** (as libultra's `gDPFillRectangle` emits); the session-26 note in this repo had it backwards |
| 3 | coordinates not 10.2 fixed point | the RDP wants `pixel << 2`; raw pixels made the "full screen" scissor 80x60 and clipped everything to the top 60 rows |
| 4 | `G_RDPSETOTHERMODE` pre-shifted (`mode0 >> 8` in w0) | RT64 tests fields at their full-word positions (`OtherMode::cycleType() == H & (3 << G_MDSFT_CYCLETYPE)`, bits 20-21 of w0), so the cycle type was `G_CYC_1CYCLE` and `FramebufferRenderer`'s `G_CYC_FILL` path never ran. The fill colour also has to be RGBA16 (`0xF801`), not RGBA8888, for a 16-bit colour image |

### Diagnostics added to RT64 (all env-gated)

* `rt64_rdp.cpp`: `OGRE_RDP_TRACE=1` → `[rdp] setColorImage/fillRect/drawRect`
  (with the scissor/empty checks, which is how bug 1 and bug 3 were found).
* `rt64_workload_queue.cpp`: `OGRE_WORKLOAD_TRACE=1` → the framebuffer pair RT64
  built (colour image, scissor, call count).
* `rt64_framebuffer_renderer.cpp`: `OGRE_FBRENDER_TRACE=1` → per-call cycle type,
  fill colour, rect, and the projection scissor (how bug 4 was found).
* `renderer.cpp`: `OGRE_VI_TRACE=1` → the decoded VI RT64 is about to present.

### App wiring (`app/src/renderer.cpp`)

`OGRE_SYNTH_FRAME` now implies `OGRE_PRESENT_ALWAYS` and
`OGRE_PRESENT_FBTARGET` via `setenv(..., overwrite=0)`, so the probe is one
command. With `OGRE_SYNTH_NO_DL` (the raw RDRAM path) it implies only
`OGRE_PRESENT_ALWAYS`, because the render-target fallback bypasses the RAM upload
path the raw probe is testing (auto-enabling both made the raw probe black).
Without `OGRE_SYNTH_FRAME` the default run is unchanged.

### The probe pins the VI (`app/src/synth_frame.cpp`)

The probe was black in about one run in three. The VI is the game's: on some
boots the game has repointed it at one of its own (black) framebuffers by the
time the capture runs. `synth_frame_vi_tick` now calls
`osViSwapBuffer(rdram, 0x80700000)` every VI (the VI thread is outside the
runtime's `message_mutex` there), which made 4/4 RDP runs and 2/2 raw runs
produce frames. `OGRE_SYNTH_FORCE_VI=0` disables the pin.

## Evidence

### The RDP display list rasterizes

```
[rdp] setColorImage fmt=0 siz=2 width=320 address=0x00700000
[rdp] fillRect ulx=0 uly=0 lrx=180 lry=960
[rdp] drawRect ulx=0 uly=0 lrx=180 lry=960 scissorNull=0 drawRectEmpty=0
...
[fbrender] call d=0 cycleType=3145728 fillColor=0x0000F801 rect=(0,0)-(183,960) scissorNull=0
```

`cycleType=3145728` is `3 << 20` = `G_CYC_FILL`; before the fix it was `0`.

### The render target has the bars

`OGRE_CAPTURE_TARGET` (320x240, `R16G16B16A16_UNORM`, 8 bytes/texel), sampled at
row 120: `(65535,0,0) (0,65535,0) (0,0,65535) (65535,65535,65535)
(65535,65535,0) (65535,0,65535) (50886,50886,25443)` — red, green, blue, white,
yellow, magenta, grey.

### The presented swap chain has the bars

`docs/proofs/native-synth-frame-rdp.png` is the readback of the presented
swap-chain texture. The same run's `OGRE_CAPTURE_TARGET` dump is the source of
the row above, so "the RDP drew it" and "the window presents it" are both
measured, not inferred.

## Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean |
| RDRAM upload path (`OGRE_SYNTH_RAW=1 OGRE_SYNTH_NO_DL=1 OGRE_SYNTH_ANIMATE=1`) | 2/2 runs, every captured frame has the bars |
| RDP display-list path (`OGRE_SYNTH_FRAME=1`) | 4/4 runs, every captured frame has the bars; target dump shows the seven bars |
| one-command probe (see Repro) | works; the presenter knobs and the VI pin are implied |
| default run (no env) | unchanged (all diagnostics are env-gated) |

## Still open

0. **The game still submits only its boot blanking display list.** The title
   scene needs the boot to get past `func_80089804` (session 26's item 2) and
   `func_80089A10` (item 1). That is unchanged by this session — what changed is
   that the renderer is no longer a suspect: with `OGRE_PRESENT_ALWAYS=1` and
   `OGRE_PRESENT_FBTARGET=1` a real display list from the game would now be
   visible.
1. **Should the presenter knobs become defaults?** `OGRE_PRESENT_ALWAYS` costs a
   present per VI and `OGRE_PRESENT_FBTARGET` can show a stale render target;
   both are diagnostics today. If the port wants "the game's frame is always on
   screen", make `FBTARGET` default on (it only changes behaviour when the
   framebuffer manager has nothing, where today the result is an empty RDRAM
   upload) and keep `PRESENT_ALWAYS` opt-in. Note the probe now relies on both
   plus the VI pin, which is fine for a probe but would have to be re-thought if
   the game itself is driving the VI.
2. **`OGRE_INSTANT_PRESENT` (PresentEarly) is a dead end for the probe**: in
   early-present mode the presenter only presents a workload whose colour image
   matches an entry in VI history, and the probe's 0x700000 matches the *live*
   VI (the dummy framebuffer), not history, so present count went to 0. Left in
   as a knob but do not use it for the probe.
3. **Untouched from sessions 24/26**: the remaining combiner inputs
   (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys), coverage/alpha compare,
   framebuffer/VI indirection, the TMEM model, `G_LOADBLOCK`'s `dxt`, and the
   `func_80089A10` spin.
4. The `[snap]` retrace-handler/dispatcher walkers still print an empty list
   while the fan-out runs; do not use those two lines as evidence.

## Repro

```sh
# build (RT64 + plume patches are already applied in this checkout; fresh
# checkouts need the two `git apply`s from docs/guides/app-build.md)
cmake --build build-app -j 8

# one command: submit the probe every 30 VIs and read back what is presented
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=900 OGRE_SYNTH_PERIOD=30 OGRE_NO_DUMMY_VI=1 \
  OGRE_CAPTURE_PRESENT=/tmp/frame OGRE_CAPTURE_AFTER=200 \
  OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
# /tmp/frame.201.ppm ... -> the seven bars

# isolate the renderer from the display list (RAM upload path)
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_NO_DL=1 OGRE_SYNTH_RAW=1 OGRE_SYNTH_ANIMATE=1 \
  OGRE_SYNTH_AT_MS=900 OGRE_SYNTH_PERIOD=0 OGRE_NO_DUMMY_VI=1 \
  OGRE_CAPTURE_PRESENT=/tmp/raw OGRE_CAPTURE_AFTER=400 \
  OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64

# why a capture is black: list on-screen windows and find loginwindow
#   (compile once: swiftc -O /tmp/winlist.swift -o /tmp/winlist)
/tmp/winlist | grep loginwindow
```

Reading a PPM: P6, 24-bit, top-left origin; the swap chain is BGRA8 and
`rt64_present_queue.cpp` writes it out as RGB. Reading a `.bin` target: 8 bytes
per texel, four little-endian `uint16` in RGBA order.

## Files changed (this session)

- `app/src/synth_frame.cpp` — four display-list encoding fixes + RGBA16 fill
  colours + `OGRE_SYNTH_ANIMATE` / `OGRE_SYNTH_NO_DL` / raw readback check + the
  `osViSwapBuffer` VI pin (`OGRE_SYNTH_FORCE_VI`).
- `app/src/renderer.cpp` — `OGRE_VI_TRACE`; `OGRE_SYNTH_FRAME` implies
  `OGRE_PRESENT_ALWAYS` (and `OGRE_PRESENT_FBTARGET`, except with
  `OGRE_SYNTH_NO_DL`); `OGRE_INSTANT_PRESENT`.
- `tools/RT64/src/hle/rt64_present_queue.cpp` — present/target capture,
  `OGRE_PRESENT_ALWAYS` support hook, `FBTARGET` fallback, traces.
- `tools/RT64/src/hle/rt64_state.cpp` — `OGRE_PRESENT_ALWAYS`, `[state]` trace.
- `tools/RT64/src/hle/rt64_rdp.cpp` — `[rdp]` traces.
- `tools/RT64/src/hle/rt64_workload_queue.cpp` — `[workload]` trace.
- `tools/RT64/src/render/rt64_framebuffer_renderer.cpp` — `[fbrender]` traces.
- `tools/RT64/src/contrib/plume/plume_metal.cpp` — texture → buffer
  `copyTextureRegion`.
- `rt64-ob64.patch`, `rt64-plume-ob64.patch` (new) — the two RT64-side changes.
- `docs/proofs/native-synth-frame-rdp.png` (new) — the presented frame.
- `docs/DECISIONS.md`, `PLAN.md`, `docs/guides/app-build.md` — session-27 entries.
