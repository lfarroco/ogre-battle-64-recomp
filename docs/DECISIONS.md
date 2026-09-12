# Technical Decisions

This is the running log of technical decisions for the Ogre Battle 64 PC port.
Each entry records what was decided, why, and when. New entries go on top.

---

## 2026-09-12 (session 26) — a real call stack for game threads, and the renderer half proven

### Context

Session 25 concluded that the wall was `func_8008AFE0` (thread 4) blocking on
`0x800C4C28`, a queue "nothing ever sends to". That came from a snapshot that
knows only *block kind + resume count + last function entered* — and "last
function entered" is the callee, not the caller, so it cannot say where a thread
actually is. This session replaced the diagnostic and re-answered the question.

### Decision: measure the stack, not the last entry (N64Recomp codegen + runtime)

The code generator now emits

```c
recomp_trace_entry(rdram, 0x80089804, "func_80089804", (uint32_t)ctx->r31);
```

in every function prologue and `recomp_trace_return(rdram, <func_index>);` before
every return. At prologue time `ctx->r31` still holds the caller's return
address (the function has not saved it yet), so each frame knows both its own
name and the call site that created it. The runtime keeps a per-thread shadow
chain of live frames (`function_trace.cpp`), so entry/return pairs make a *stack*
rather than an ever-growing ring of entries.

Consequences that made the session:

* the boot thread's live chain is stable across snapshots and readable;
* `debug_dump_call_chain()` also runs for every parked thread in the VI
  snapshot, and for all threads in the `OGRE_EXIT_AFTER_MS` dump;
* `OGRE_CHAIN_HISTORY=<tid>` records the ordered sequence of live-frame sets, so
  a wedge reads as a path (the frames it passed through), not a single state.

The alternative — logging `[func] enter/exit` lines — was rejected: the VI
snapshot and the game threads write the same stream and the interleaving destroys
the ordering that is the whole point.

### Finding: the earlier localisation was wrong

The send trace (`do_send`) shows `func_800891A0 → mq=0x800C4C28 msg=0x800E8B10`
dozens of times, and t4's chain shows it parked in `func_8008AFE0 →
func_80089054`. `func_80089054` is the *retrace-subscriber registration*
(`asm/1060.s` 0x80089054), so t4 is a per-frame service thread, not the wall.

The boot thread (`func_8007F8E4` → `osCreateThread(t3, func_80071EB0)`), by
contrast, goes `func_80071EB0 → func_8008A1B0 → {func_80089AB0 → func_80089A30}
→ func_80089660 → func_80089804` and never returns from `func_80089804`, the
frame-submit function. That is the handshake the boot is waiting on.

### Finding: the runtime's `func_80089A10` override is dead code

`librecomp/src/recomp.cpp`'s `func_80089A10_recomp` (a yielding spin) is never
called. The recompiled `_func_80089A10` is a strong symbol defined in
`funcs_5.c.o`; direct calls link to it, and only *function-pointer* lookups go
through the overlay map that the runtime can patch (`get_function`). Session 15
recorded the override as "the fix that unblocked the boot"; it was inert, which
is why the stall kept surviving every "fix". This defines the actual boot gate:
a high-priority thread executing `while (D_800E79A4 != 0);` with no yield, in a
cooperative scheduler whose only preemption is voluntary.

### Decision: a synthetic display list as a renderer-path probe

`app/src/synth_frame.cpp` (`OGRE_SYNTH_FRAME=1`) builds a small F3DEX2 list in
RDRAM and submits it through the same path the game uses
(`ultramodern::submit_rsp_task` from the VI callback). It exists because "the
canvas is black" cannot distinguish "the renderer never got a list" from "the
renderer dropped it", and because a stalled boot only ever submits its blanking
list. Three traps are documented in the file: `TO_PTR` sign-extends `int32_t`
pointers (so KSEG0 only), `OSTask::t.data_ptr` must be a *physical* offset
(`send_dl` masks with `0x3FFFFFF`), and RT64's fill path needs a scissor set or
the rect never reaches `drawColorRect`.

Result: the probe reaches `send_dl` and RT64 executes every command (verified by
instrumenting `setFillColor`/`fillRect`). The captured window is still black, so
the open gap is presentation, not display-list handling — a much narrower claim
than "the renderer is not proven".

### Finding: use a memory dump, not snapshot words

`OGRE_DUMP_RDRAM=<path>` writes the whole 8 MiB image at the scripted exit.
Reading a handful of words through the snapshot repeatedly produced
contradictory readings during this session; the dump settles questions offline in
one shot. Note the byte rules (`recomp.h`): the word at game address `a` is a
little-endian word at file offset `a - 0x80000000`, and the logical byte at `a`
is at `(a ^ 3) - 0x80000000`.

---

## 2026-09-12 (session 25) — the native app drives itself, and the idle trajectory is reproduced + localised on it

### Context

Session 24 left two open threads: the title sprites now render correctly in the
browser, and the native RT64 build runs but paints a black canvas because it
never receives input. The web probes feed input (`tap()` in
`debug/lib/harness.cjs`, one Enter press every 5 s), so no native measurement
could be reproduced without a human at the keyboard, and none of the web
probes' tooling (screenshots, pixel counts, `tasks=`) had a native counterpart.

### Decision: `OGRE_TAP_MS` / `OGRE_EXIT_AFTER_MS` instead of an external driver

A scripted native run is now self-driving, with two env vars read once in
`configure_automation()` (`app/src/sdl_platform.cpp`):

* `OGRE_TAP_MS=<n>` — controller 0 presses Start for the first 150 ms of every
  `n` ms window, contributing **button state only**. This mirrors the web
  harness's Enter tap so the two platforms' input is the same in kind.
* `OGRE_EXIT_AFTER_MS=<n>` — the main thread prints the per-thread last-function
  table after `n` ms and exits 0 immediately (`_Exit`, after a 100 ms grace so
  the VI thread can finish a snapshot it is printing). The graceful path
  (`ultramodern::quit()` → `recomp::start` unwinding) instead tears down the
  renderer and the runtime's workers while the game's threads are mid-call,
  which segfaults intermittently (seen with and without
  `OGRE_DEBUG_TRACES`); a scripted run wants the log and an exit code, not the
  unwind.

Two alternatives were rejected:

* **Synthesising SDL key events** (`SDL_PushEvent` from the main thread). It
  would work, but it puts the button state through SDL's keyboard layer, which
  fights the main-thread-only `SDL_PumpEvents` constraint documented in session
  24; button state cannot violate it.
* **Driving the app externally** (AppleScript/`cliclick` keystrokes). It needs
  Accessibility permissions and a focused window, i.e. it is not headless, and
  it would be the only part of the project's verification that is not
  self-contained.

Both default to 0, so an interactive run behaves exactly as before.

### Finding: the idle trajectory is not input-gated, and not platform-specific

| run | input | outcome |
|---|---|---|
| `ogrebattle64`, 40 s, no env | none | 1 display list (the boot blanking DL), no RSP task at all |
| `ogrebattle64`, 60 s, `OGRE_TAP_MS=5000` | 11 Start taps | 1 display list, **1085** type-2 RSP tasks, `bootstate=0x00000060` |
| `ogrebattle64`, 45 s, `OGRE_TAP_MS=3000` | 14 taps | 1 display list, no RSP task, `bootstate=0xBF880415` |
| `progress.cjs`, 4 x 70 s (web, taps Enter) | 4 boots | `maxTasks=1` on all four |

So the taps *do* reach the game — they moved the boot state from the
`0xBF880415` stall to `0x00000060` and started the audio path — but they are not
what gates frames. The web build, with its own input pump, failed to reach a
real display list on 4/4 attempts in this session, where session 21's six
attempts reached 1, 9, 19, 19, 35, 39. The distinct stall points
(`0xBF880415`, `0x00000060`) and the run-to-run spread say this is a **race in
the game's boot**, not a renderer or input problem, and it is shared by both
platforms.

### Finding: the stall is localised — the frame/RSP worker never gets a message

With `OGRE_DEBUG_TRACES=1` the whole 25 s message-queue trace is only five
successful sends:

```
T=147  do_send OK queue=0x800C6490 msg=0x800E8B10 count=1/1   (PI-manager handshake)
T=177  do_send OK queue=0x800E8B4C msg=0x800E7D90 count=1/8   (boot frame -> t17)
T=178  do_send OK queue=0x800E8BBC msg=0x0000029B             (SP done)
T=182  do_send OK queue=0x800E8BF4 msg=0x0000029C             (SI done)
T=193  do_send OK queue=0x800E9BA8 msg=0x800E7D90 count=1/8   (boot frame -> t5)
```

and the per-thread exit table:

```
t1  last func 0x80099740   (PI/DMA busy-wait)
t3  last func 0x800901C0   (osCont family)
t4  last func 0x80089054   (osSetEventMesg wrapper, func_8008AFE0's queue loop)
t5  last func 0x80089540   (queue worker: osCreateMesgQueue + osRecvMesg)
t16 last func 0x80089358   (RSP task thread: osRecvMesg + osSpGetStatus)
t17 last func 0x800901C0   (osCont family)
t18 last func 0x80089200   (RSP task thread)
t19 last func 0x800891A0   (VI-retrace worker, receives 0x29A ~60/s)
```

What this establishes:

* The VI path is healthy: `[vi-debug] retrace -> mq=0x800E8B84 msg=0x0000029A`
  fires at ~60 Hz and t19 consumes it, so "the VI thread is stalled" is not the
  story (session 21's measurement said the same).
* `func_8008AFE0` (thread 4, priority 50) creates its own queue
  `0x800C4C28` (buf `0x800BE1A0`, count 8), registers an event (`func_80089054`
  with type 3), and then blocks on `osRecvMesg` forever. **Nothing ever sends to
  it** — zero sends in 25 s of trace, and it re-blocks ~45 times/s.
* The frame message `0x800E7D90` is delivered exactly once, at T=193, to
  `0x800E9BA8`; t16's RSP thread (`func_80089358`) is blocked on a *different*
  queue (`0x800B9C40`) and never runs its `osSpGetStatus` → submit path again.
  Since gfx tasks are submitted by these RSP threads, that is why exactly one
  display list exists.
* `func_8008AFE0`'s thread is created from `func_80071EB0` (the boot/init
  function, via `func_8008A1B0` → `func_8008B0B0`), so the worker exists from
  boot init and is simply never fed.
* t16's resume counter is stuck at 3 across every snapshot (`t17=3`,
  `t16=3`), i.e. it has not been scheduled since boot.

### Correction: `[snap]`'s `bootstate(D_800AEF98)` is read as a word, but the game stores a byte

`func_80071EB0` (the boot/init function) writes `D_800AEF98` with `sb`:

```
/* 23EC 80071FEC */  lui  $at, %hi(D_800AEF98)
/* 23F0 80071FF0 */  sb   $zero, %lo(D_800AEF98)($at)
```

and every other reference in `asm/1060.s` is `lbu`/`sb`. The snapshot prints it
with `snap_w` (32-bit), so `bootstate=0xBF880415` is one byte of game state plus
three bytes of adjacent memory — the low byte (`0x15`) is the state, the rest is
noise. It is still useful as a coarse "which stall point" fingerprint (it is
reproducibly `0xBF880415` or `0x00000060`), but it must not be read as a word
value. The other fields on that line come from globals whose access width has
not been checked either; treat the line as a fingerprint, not as decoded state.

### Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean (only the pre-existing sdl2-compat deployment-target link warning) |
| interactive run (no env) | unchanged: window opens, `api=3`, boot DL 1, stalls at ~3 display lists |
| `screencapture -l <window id>` on a live run | works (2560x1496 PNG); the canvas area is black on a stalled boot (`colorful=0`), which matches "one blanking DL" |
| bounded run | `OGRE_EXIT_AFTER_MS=60000` exits on time and prints the per-thread table |

### What this does not establish

The **taps' effect on the boot trajectory** is not proven causal. They changed
`bootstate` and started the audio path in one run, but the next run with
*different* tap timing stalled earlier, so the input tap and the boot race are
still confounded; a controlled A/B (same boot count, taps on/off, several runs
each) is the way to separate them.

---

## 2026-09-11 (session 24, native bring-up) — the native RT64 build works on macOS: two SDL-glue bugs

### Context

The web build had become the project's only renderer: `app/src/web_renderer.cpp`
is 3 355 lines against `app/src/renderer.cpp`'s 357, and sessions 19-24 were all
web-renderer work. The plan was to move the renderer phase to Linux/Vulkan
(`docs/guides/linux-migration.md`), partly because the macOS window path was
believed to be blocked. Before committing to that, the native app was brought up
on the current machine (macOS 15.7.9, x86_64, Metal).

It runs. RT64 initialises its Metal backend (`api=3`, `AMD Radeon Pro 560X`), the
window opens, the game boots and stays up. Two bugs in the app's own SDL glue
were in the way, both macOS-only.

### Bug 1: `create_window` handed RT64 the SDL_Window*, not the NSWindow*

`renderer.cpp` fills `RT64::Application::Core::window` from
`ultramodern::renderer::WindowHandle`. On Apple that struct is
`{void* window; void* view;}`, and plume casts `window` straight to an
`NSWindow*` - `plume_apple.mm`'s `CocoaWindow::updateWindowAttributesInternal`
does `[[nsWindow contentView] frame]` and `[nsWindow screen]`. `create_window`
passed the `SDL_Window*` and left `view` null, so the first main-thread event
pump died in `objc_msgSend` with `EXC_BAD_ACCESS (code=1, address=0x18)`.

The null `view` was a deliberate stub whose comment said `SDL_Metal_GetLayer`
"segfaults with Homebrew's sdl2-compat". That does not reproduce: a standalone
probe (`SDL_Init` -> `SDL_CreateWindow(SDL_WINDOW_METAL)` -> `SDL_GetWindowWMInfo`
/ `SDL_Metal_CreateView` / `SDL_Metal_GetLayer`) returns valid pointers for all
three under sdl2-compat 2.32.70.

`create_window` now takes the Cocoa window from `SDL_GetWindowWMInfo` and the
CAMetalLayer from `SDL_Metal_CreateView` + `SDL_Metal_GetLayer`, and owns the
`SDL_MetalView` in `Platform` so it is released before the window.

This is *not* what RT64's own `ApplicationWindow::create` does - it sets
`windowHandle.window = sdlWindow` as well. The frontend is expected to supply the
handles, which is what this app does.

### Bug 2: `poll_input` pumped SDL events from the game thread

```cpp
static void poll_input() {
    SDL_PumpEvents();   // "Safe to call from any thread" - it is not
}
```

ultramodern calls `poll_input` from `osContStartReadData`, i.e. the game thread.
On macOS `SDL_PumpEvents` -> `Cocoa_PumpEvents` -> `[NSApp nextEventMatchingMask:]`
raises

```
NSInternalInconsistencyException: 'nextEventMatchingMask should only be called from the Main Thread!'
```

which terminates the process: 2 of 3 runs died there, immediately after the boot
display list. The main thread already pumps every frame through
`pump_sdl_events` (from `update_gfx`), and reading SDL's keyboard/controller
*state* off-thread is fine - only pumping is not. The call is gone; 3 of 3 runs
then survived 45 s.

### Verified

| check | result |
|---|---|
| `cmake --build build-app` | clean (one pre-existing sdl2-compat deployment-target link warning) |
| RT64 setup | `[renderer] RT64 renderer initialized (api=3)`, `Device Name: AMD Radeon Pro 560X` |
| window | opens (1280x748); captured with `screencapture -l <window id>` |
| stability | 3/3 runs alive after 45 s (1/3 before bug 2) |
| display lists | 1 per run: `type=1 ucode=0x8009F540 data=0x800C6500` - the boot blanking DL |
| canvas | black |

### What this does and does not establish

The **environment is not a blocker**: macOS + Metal + sdl2-compat is a workable
native platform, so `linux-migration.md`'s premise (switch for the renderer phase
because macOS is blocked) no longer stands on its own. Linux is now a preference
- RT64's most-tested backend is Vulkan - not a requirement.

The **game-side stall is unchanged and platform-independent**: every native run
submits only its boot blanking display list and then idles, exactly like the web
build. The web probes get past that wall by pressing Enter every 5 s
(`debug/lib/harness.cjs`, `tapMs: 5000`), and session 17 found that input advances
the title. These native runs sent no input, so the black canvas is expected and
is not evidence of a renderer failure.

Two diagnostics were added to `renderer.cpp`: the RT64-init line now goes to
stderr (it was `printf`, and stdout is block-buffered when piped, so a successful
setup looked like a silent failure), and `send_dl` logs the first few display
lists and then every 50th - nothing on the native path reported whether the game
submits gfx tasks at all.

---

## 2026-09-11 (session 24) — the "green smudges": texture bytes read at the wrong offset, and a blender cycle folded when it should cancel

### Symptom

The twelve title-screen soldiers rendered with green and purple speckle
"smudges" over the helmet area, and a dim colour cast everywhere. The sprites
were recognisable and their palette was broadly right, so this read like a
combiner, filtering or blending problem. It was two separate bugs: a texture
decode that read the wrong byte, and a blender cycle whose overflow simulation
should not have applied.

### Finding: `decode_texture_rect` read texture bytes at their *logical* offset

The runtime's rdram stores every N64 32-bit word byte-reversed, so the logical
byte at address `a` lives at physical `a ^ 3` (`recomp.h`'s `MEM_B`). RT64's TMEM
load states the rule exactly (`hle/rt64_rdp.cpp`, `loadWord`):

```
TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + i) ^ 3];
```

`decode_texture_rect` used a raw `rd16()`/`p[0]` at the logical offset.
`read_matrix` has always dodged this with its `c ^ 1` column index and session 23
added `n64h`/`n64b` for `Vtx`; the texture decoder was the one place left. Two
distortions followed, and only the second is obvious:

- **16-bit texels** (TEXEL1, the RGBA16 colour): `rd16()` at logical offset `o`
  returns the halfword at `o ^ 2`, i.e. the *adjacent* texel, so every horizontal
  pair came out swapped. Harmless inside flat regions, wrong wherever the art has
  detail.
- **I4/CI4 bytes** (TEXEL0, the 32x34 mask): a byte read at logical offset `b`
  returns logical `b ^ 3`, so the four bytes of every word came back reversed -
  eight texels per group, in 2-texel units. The mask's alpha therefore landed in
  the wrong places and uncovered colour texels the mask exists to hide. OB64's
  colour data really does carry green under the mask, which is where the green
  smudges came from.

The mask's alpha *histogram* is identical before and after (the bug is a
permutation, so the multiset of nibbles is preserved) - only the spatial
arrangement changes. Comparing histograms would have missed this entirely.

### Decision: apply RT64's byte rule in the texture decoder

`n64be16(rdram, addr)` joins `n64h`/`n64b` next to `rd16`. `decode_texture_rect`
reads I4/I8 via `n64b` and 16/32-bit texels via `n64be16`, and `OP_LOADTLUT`
reads its entries the same way. Nothing about the tile model changes - sizes,
strides, formats and rects are untouched; only the byte index is compensated.

### Finding: the viewport was correct only by accident, and its comment said the opposite

`set_viewport` read `rd16(rdram_ + addr + i * 2)` and then took x from index 1, y
from index 0 and z from index 3. Because the raw reads return the other halfword,
the local array held `[y, x, 0, z]` and those index choices undid the swap: the
code logged `vscale=[480 640 0 511]` and was right. The comment, though, asserted
that `Vp_t` is laid out `(y, x, pad, z)`. That is not true - it is `(x, y, z, 0)`
and OB64 submits `[640 480 511 0]` - and a reader who believed it would have
"fixed" the index juggling into a transposed viewport.

It now reads through `n64h()` with the canonical indices. Equivalent, and the
`[GFX-VP]` log shows the same transform with the swap gone:

```
before: vscale=[480 640 0 511] -> scale=[160.00 120.00 127.75]
after:  vscale=[640 480 511 0] -> scale=[160.00 120.00 127.75]
```

### Finding: the blender's overflow simulation folded a cycle that cancels exactly

With the decode fixed the decoded pair was clean but the rendered sprite was
still a dim, speckled mess, so the fault had to be downstream of the sampler.
`[GFX-UV]` ruled the sampler out (`k1` found, a 20x34 colour texture bound to
unit 1, `scl1=(0.0500 0.0294)`, vertex `uv1=(0.000 0.000)`, `glGetError` clean),
and `[GFX-CMD]` gave the sprite's exact state:

```
cyc=2 omh=0x182CF0 oml=0x00184240 cmb=0xFCFFFFFFFFFD7238
mux0=[rgb=(ZERO-ZERO)*ZERO+TEXEL1 a=(ZERO-ZERO)*ZERO+TEXEL0]
mux1=[rgb=(ZERO-ZERO)*ZERO+COMBINED a=(ZERO-ZERO)*ZERO+COMBINED]
prim=[1.00,1.00,1.00,0.00] ablend=1 force=1 approx=0 ccyc=2
b0=[P=CC M=CC A=CC_A B=ONE]   b1=[P=CC M=FB A=CC_A B=1MA]
```

So the combiner is `rgb = TEXEL1, a = TEXEL0` - correct - and the colour goes
through the blender. That mode has `forceBlend` set with a 2-cycle combiner, so
`blendCycleCount` is 2 and cycle 0 (`P=CC, M=CC, A=CC_A, B=ONE`) is evaluated
before cycle 1 does the framebuffer blend. Cycle 1 feeds cycle 0's colour
through as its `CC` input, so cycle 0 must be an identity.

It is: `(P*a + M*b) / (a + b)` with `P == M` is exactly `P`. But our cycle is a
field-for-field port of RT64 `Blender::runCycle`, which wraps the numerator with
`fmod(numerator, 1 + 8/255)` to simulate the hardware's accumulator overflow -
and that fold is applied unconditionally. With `P=M=CC` and `B=ONE` the
numerator is `CC*(a+1)`, which exceeds `1.031` for ordinary colours, so the fold
fires on almost every pixel. Because the fold is per channel, it changes hue:

```
mask alpha=0.0: orange(255,166,33) -> (255,166, 33)
mask alpha=0.1:                    -> ( 16,166, 33)   green
mask alpha=0.5:                    -> ( 80,166, 33)   green
mask alpha=1.0:                    -> (124, 34, 33)   dark red
```

Red is the first channel to exceed the limit, so mid-alpha pixels lose red and
come out green, and fully opaque pixels come out about half brightness. The mask
histogram shows most of the helmet/detail texels sit at intermediate alpha, which
is exactly where the green appeared.

Note this is not a porting slip: RT64's `runCycle` computes a `numeratorOverflow`
flag (`!passthrough && (B != B_ONE_MINUS_A)`) and then never uses it. RT64
already special-cases the analogous identity - `(P == M) && (B == B_ONE_MINUS_A)`
- as a passthrough, so treating `P == M` as an identity is consistent with its
own intent.

### Decision: skip the fold when `P == M`

`blender_run_cycle` now computes the numerator and applies `mod(..., Overflow)`
only when `P != M`. When `P == M` the division cancels the numerator exactly, so
the fold can only ever corrupt the result and skipping it cannot change any
correct output. The fold is left in place for `P != M`, where the pinned RT64's
behaviour is a separate question (the same reasoning suggests it should not fold
there either, since a weighted average of two in-range inputs is in range - the
example above with `P=CC, M=0.5, a=b=1` folds `1.5` to `0.234` instead of
`0.75` - but nothing in this scene exercises it, so it is left alone and flagged
here rather than changed blind).

### Verified

| check | before | after |
|---|---|---|
| `textures.cjs` mask silhouette | ragged, 8-texel block steps, detached fragment | smooth plume + figure |
| `spritecheck.cjs` rendered side | dim green/purple speckle, no resemblance to the composite | the same character as the composite (orange body, white plume, green helmet band, magenta sash) |
| full settled canvas | tangle of green/purple/blue slivers | twelve readable soldiers in two mirrored clusters of three |
| `shots.cjs` settled `nonBlack` | `239425` | `239425` (unchanged - only the colours changed, so this is not a brightness effect) |
| `testdraw.cjs` | `nonBlack=12312 colorful=10237` | unchanged |
| `logmode.cjs` | PASS | PASS |
| `[GFX-VP]` scale | `[160.00 120.00 127.75]` | unchanged |

This closes session 23's open item 0: a single isolated sprite now matches its own
`colour x mask` composite.

Still open from session 23: raw TEXEL1 (`--flags 9`) was reported to sample black
while its decoded image is bright. That was not re-measured here, and with the
blender fixed the composite comparison no longer shows a discrepancy, so it may
have been an artifact of the isolation probe (which compares the *first* sprite
against the *last* textured draw's texture pair - two different sprites). It
needs a measurement before it is either fixed or dismissed.

---

## 2026-09-11 (session 23, follow-up) — the rectangle commands were decoded word-swapped, so nothing ever cleared the canvas

### Finding: `G_FILLRECT`/`G_TEXRECT` take `lrx/lry` from w0 and `ulx/uly` from w1

`gDPFillRectangle`/`gSPTextureRectangle` emit the lower-right corner in **w0**
and the upper-left in **w1**; RT64 `GBI_RDP::fillRect`/`texrect` read `p0` for
the lower-right and `p1` for the upper-left. This renderer read the upper-left
from `w0`, so a rectangle from `(0,0)` to `(w-1,h-1)` - a full-screen clear, the
most common rectangle there is - decoded as *inverted* and was dropped by the
empty-rect guard session 20 added.

Session 20's note ("OB64's title DL contains a TEXRECT with xl=319, yl=239,
xh=0, yh=0 - decoding that span as a quad produced a full-screen black cover")
is that same mis-decode seen from the other side: the inverted rect *was* the
real full-screen rect.

Consequences: the game's per-frame `G_FILLRECT` clear and its full-screen fade
rect were both discarded, so **frames accumulated on the canvas** - the title
sprites looked like thin streaked columns because many frames of a slowly
changing scene were stacked on top of each other.

### Decision: decode rectangles the way the SDK and RT64 do, and carry the extra rules

`G_FILLRECT` and `G_TEXRECT`/`G_TEXRECTFLIP` now read `ulx/uly` from `w1` and
`lrx/lry` from `w0`. Two related rules came with them:

- a rectangle samples **its own tile** (`G_TEXRECT` w1 bits 24-26), not
  `G_TEXTURE`'s - RT64 `RDP::drawTexRect` takes the tile as a parameter, so the
  draw scopes `active_tile` to the rectangle's;
- fill/copy mode rounds the origin down and the far corner up to the 4-pixel
  group (RT64 `RDP::fillRect`'s `lrx |= 3`/`lry |= 3` and `RDP::drawRect`'s
  `ulx &= ~3`).

`test_draw`'s synthetic list had been built with the swapped layout (it put both
corners in w1), which is why it kept "working" until this fix and then drew
nothing; it now emits the SDK layout.

### Note: the reference screenshot is a zoomed capture

The reference frame from a real session that this work was checked against is
**zoomed**, so it is only valid for zoom-invariant comparisons: layout, palette,
and whether a sprite is recognisable. Its pixel counts and absolute scale say
nothing about our render. The scene's layout does match it (two mirrored groups
per side, three characters above and three below in a diagonal).

### Finding: the intro fades in from black, so the first frame is not a fidelity measure

The full-screen rectangle drawn *after* the sprites each frame is a fade whose
alpha is `G_SETPRIMCOLOR`'s (`rgb = PRIM`, `a = PRIM`, `FORCE_BL`). Every probe
that captures "the first rendered frame" therefore captures a black frame.

Measured over 48 display lists the sprites never change: the first quad stays
0.1463 x 0.3253 NDC (23.4 x 39.4 px), the vertex UV range stays 0..19 x 0..33
texels, and the tiles stay a 32x34 I4 mask plus a 20x34 RGBA16 colour. The
bounding box that appeared to "grow" was the fade revealing a static picture.

### Decision: probes must wait for a fresh display list, not a fixed delay

`debug/probes/spritecheck.cjs` now (a) waits for the `exec: task=` counter to
advance after it changes the debug flags - the game stalls for seconds and a
stale canvas reads a faded (black) frame - and (b) compares the rendered sprite
against `ogre_gfx_debug_last_tex`, the exact pair the last textured draw
sampled, because `ogre_gfx_debug_tex`'s ring of decodes spans frames. It also
takes `--settle MS`.

---

## 2026-09-11 (session 23) — the title sprites are decoded the way the RDP samples them

### Finding: every vertex field was read with the wrong byte order

The runtime stores each N64 32-bit word byte-reversed and compensates with
`MEM_W` (native load), `MEM_H` (`^ 2`) and `MEM_B` (`^ 3`) in
`N64Recomp/include/recomp.h`. So the 16-bit field at logical offset `o` lives at
`o ^ 2`, and a direct `rd16(p + o)` returns the *other* half of the word.
`read_matrix()` has always applied that compensation through its `c ^ 1` column
index; `OP_VTX` did not, so every `Vtx` was decoded as
`(y, x) (flag, z) (t, s) (a, b, g, r)` instead of `(x, y) (z, flag) (s, t)
(r, g, b, a)`.

That single mistake transposed the sprite quads (39x23 where the sheet is
20x34) *and* their texture coordinates, which is why the title sprites looked
like scrambled slivers while the ring they form still looked like a ring. It had
survived five sessions of renderer work because a near-symmetric composition
hides a transpose.

### Decision: one `n64h()`/`n64b()` helper pair, used with named N64 offsets

`gbi.hpp`'s `rd16()` reads a halfword out of the byte-reversed buffer; the new
`n64h(p, off)`/`n64b(p, off)` in `web_renderer.cpp` apply the `^ 2`/`^ 3` rule.
`OP_VTX` now reads `n64h(p, 0/2/4/6/8/10)` and `n64b(p, 12..15)` — the N64 `Vtx`
layout as written — so a future field addition cannot reintroduce the transpose
silently.

### Decision: apply `G_TEXTURE`'s scale, and only to vertices

RT64 `RSP::setVertexCommon` computes `tc = (s * sc) / (65536 * 32)` with `sc`
zero-extended from the 16-bit field; OB64's title sprites use `0x8000` (0.5), so
the renderer was sampling twice the texture it should have. `RenderState` now
carries `tex_scale_s`/`tex_scale_t` and `transform_vertex` applies them.
Rectangles deliberately do **not** apply the scale: RT64's `RDP::drawRect` uses
`uls/32` and `lrs/32` straight. The field must also be read *unsigned* (the
session-21 code cast it to `int16_t`, turning `0x8000` into `-32768`).

### Finding: I8/I4 texels must carry their intensity into alpha

RT64's `I8ToFloat4`/`I4ToFloat4` return `i` for all four channels. The decoder
set alpha to 255 for I formats, so a mask sampled as `TEXEL0_ALPHA` was
constant `alpha = 1` and did nothing. Fixed, and `RGBA16` now uses RT64's
`(c << 3) | (c >> 2)` replication rather than `c * 255 / 31` so the two
renderers agree bit for bit.

### Decision: port `rt64_blender.h`, do not guess the blend from `oml`

The blend was approximated from `othermode_l & 0xFFF` with a two-entry
heuristic that read the wrong bits, so OB64's title sprites
(`oml = 0x00184240`, `FORCE_BL` set, `M1 = FRAMEBUFFER_COLOR`, `B1 =
ONE_MINUS_A`) were drawn with blending **disabled** and the alpha mask thrown
away.

`Blender` now mirrors RT64: `OtherMode::blenderInputs` (L bits 16–31, packed
`P1 P0 A1 A0 M1 M0 B1 B0`), `checkEmulationRequirements` (including both
approximations), `usesAlphaBlend`, and a field-for-field port of
`run`/`runCycle` into the fragment shader. GL blending is enabled exactly when
`usesAlphaBlend` is true, with `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` — RT64 uses
dual-source blending with the factor in the secondary output, which is the same
thing here because the primary output's alpha carries that value and the canvas
has no alpha channel.

The title render mode decodes to the classic two-cycle XLU:
`b0=[P=CC M=CC A=CC_A B=ONE]`, `b1=[P=CC M=FB A=CC_A B=1MA]`.

### Decision: decode a tile's image from the *render tile*, not from the load

The RDP samples TMEM through the render tile, whose `fmt`/`siz`/`line` need not
match the load's. OB64's mask is the proof: it loads 16 bytes/row with 8-bit
addressing (the standard `gDPLoadTextureBlock` idiom) and then declares the
render tile as **I4**, making the sampled image 32x34, not 16x34 (each byte is
two 4-bit texels). Decoding the load's rect with the load's format produced a
thin silhouette that did not match the character at all.

`RenderState` now keeps a *pending load* (set by `G_LOADTILE`/`G_LOADBLOCK`,
claimed by the next `G_SETTILE` that names a different tile) and each
`TileState` carries the source address and a lazily decoded image.
`ensure_tile_image()` decodes the tile's rect at the tile's format with
`line * 8` as the row stride — the same three inputs RT64 uses (`RDPTile` +
`GPUTile`) — and queues one upload per key per display list. This replaces the
`recent_loads[2]` "the last two loads are TEXEL0/TEXEL1" model, which paired
the images correctly but decoded them wrongly.

`decode_texture_rect()` now takes an explicit `row_bytes`; the old
`width * bytes_per_texel` was only accidentally right when the load's format
matched the tile's. The stride itself comes from the **SETTIMG** state at load
time (`bytesPerRow = width << siz >> 1`, RT64 `loadTileOperation`), not from the
tile's `line`: `line` is the TMEM stride the load writes with, which is the same
number here (`line = 2` words = 16 bytes vs `16 << 1 >> 1` = 16) but is a
different quantity.

### Finding: the synthetic `test_draw` probe had been drawing black

`test_draw` wrote its scratch display list and texture at `0x1FE00000`, and
`resolve_address()` (RT64 `fromSegmented`) reads the top nibble of an address as
a *segment index* — so the texture address resolved through segment 15 (base 0)
to `0x00E01000`, zeroed memory. Its combiner word also set the cycle-1 C
selector to ZERO, which evaluates to 0. The probe reported "rendered" while
drawing a black rectangle, so it proved nothing about the texture path.

Scratch memory moved to `0x00C00000` (segment 0, above the N64's 8 MiB, below
the 32 MiB walk-escape threshold) and the combiner is now the SDK's
`G_CC_TEXEL0` (`rgb = (TEXEL0 - 0) * SHADE + 0`). `testdraw.cjs` now shows the
four quadrants of the synthetic 2x2 RGBA16 texture, so it is a real isolation
test again. Note that color selector C has no `ONE` (`15` is `K5`, which the
shader returns as 0); "TEXEL0 only" has to be `A=TEXEL0, C=SHADE` with a white
shade.

### Decision: verify a sprite by rendering one and comparing it with its textures

Screenshots were actively misleading here (a 20x37 character at 1:1, through a
2x canvas and a JPEG re-encode, reads as noise). Two probes were added instead:

- `debug/probes/textures.cjs` dumps the last 24 decoded images (a frame's
  `mask, colour` pairs) as RGB and alpha views, plus the `colour × mask`
  composite of each pair.
- `debug/probes/spritecheck.cjs` sets `ogre_gfx_debug_flags(2)` so the renderer
  draws only the first sprite, reads the canvas back, and writes the rendered
  crop beside that composite. Matching means the whole path
  (vertex → decode → combiner → blender → GL) is right.

Two debug entry points exist for this and only this: `ogre_gfx_debug_tex(i,
&w, &h, &tile, &fmt, &siz)` and `ogre_gfx_debug_flags(bits)` (`1` = ignore
alpha blending, `2` = draw only the first sprite).

---

## 2026-09-11 (session 22) — game audio is muted by default

### Decision: silence the output, keep the ring draining

The audio microcode is not emulated (the RSP audio task is only auto-completed),
so the samples the game hands to the AI are not real audio: playing them is a
wall of screeching. The page therefore **does not play game audio unless it is
asked to**.

Two things had to stay exactly as they were, because the audio path is
load-bearing for the game's flow (session 15 had to unblock it to get the boot
moving):

- the **AudioWorklet is still created, connected and draining** the ring buffer,
  so the game's `get_frames_remaining()` sees identical backpressure;
- the ring bookkeeping in the worklet (`tail`, `readPos`, the underrun snap) runs
  unchanged - a muted worklet only zeroes the samples it writes to the output.

Silencing at the *page* layer rather than in the app/runtime means no wasm
rebuild, no change to the game's view of the host, and no risk of re-breaking
the boot path that the audio plumbing feeds.

### How to enable it

`?audio` on the page URL (also `?audio=1`), or at runtime:

```js
window.ogreAudio.enabled()        // false by default
window.ogreAudio.setEnabled(true) // unmute (screeches until the audio ucode runs)
```

The page logs the state on connect (`game audio MUTED - add ?audio to the URL to
hear it`) and still logs when the game starts queueing samples
(`game is queueing audio (N frames buffered, muted)`), so a silent page is
distinguishable from a stalled audio path.

### Why this is a deferral, not a fix

Making audio real is Phase 6 work with two independent halves: run the game's
audio microcode (RSPRecomp/`rsp_callbacks` instead of the auto-response that
satisfies the task), and only then is `queue_audio_samples()` fed actual PCM.
The switch above is exactly what that work needs: unmute and listen.

---

## 2026-09-11 (session 21) — `G_SETOTHERMODE_H/L` never applied anything; the combiner is now evaluated the way RT64 does

### Finding: the othermode decode double-shifted its data, so OTHERMODE_H stayed 0

`G_SETOTHERMODE_H`/`_L` carry the field's **already shifted** data word (the SDK
macro emits `val << sft`), and the RSP only clears the target field:
`H = (H & ~(((1 << len) - 1) << sft)) | w1` (RT64 `RSP::setOtherModeH`). The
renderer instead computed `((w1 & mask) << off)`, i.e. it masked the data down
to `len` bits *and* shifted it again, which zeroed every bit of every field. The
observable consequence: `OTHERMODE_H` was always 0, so **every draw was
`G_CYC_1CYCLE`** with no dithering, no texture filtering and no perspective
correction, whatever the game asked for.

With the decode fixed, OB64's title sprites report
`omh=0x182CF0` = `G_CYC_2CYCLE` + `G_TF_BILERP` + `G_TP_PERSP` +
`G_AD_DISABLE` + `G_CD_DISABLE`, `oml=0x00184240`. The same fix is what made the
combiner's mux question answerable (below): the sprites *are* a two-cycle
combiner, so the second-cycle path is exercised on every title frame.

### Decision: evaluate the combiner exactly as `rt64_color_combiner.h` does

The session-19 selector decode was position-independent and therefore wrong for
the top of every mux field (color selector 6 is `ONE` for A/D, `KEY_CENTER` for
B, `KEY_SCALE` for C; `8..15` are C-only alpha-ish sources; alpha D reads `H` in
both cycles, not `L` in the first). The renderer now mirrors RT64's
`parseColorInput*`/`parseAlphaInput*` bit fields, `colorInputA/B/C/D`,
`alphaInputABD/C` and `runCycle` field for field, including:

- **which mux a cycle type evaluates** — 1-cycle evaluates the *second* mux
  (`run()` calls `runCycle(inputs, twoCycle ? 0 : 1, twoCycle, ...)`), 2-cycle
  evaluates mux 0 then mux 1, and `G_CYC_COPY` bypasses the combiner entirely
  (`texel0` passthrough);
- the **TEXEL0/TEXEL1 value swap** in the second cycle of a two-cycle combiner;
- the **wrap** rules for the second cycle's `COMBINED` inputs
  (`wrapInputC`/`wrapInputABD`, then `wrapClamp`) rather than a plain clamp.

Evidence that this matters for OB64: the sprite combiner is the single ROM word
`0xFCFFFFFF 0xFFFD7238` (file offset `0x1EE828`), which decodes to
`cycle0 = rgb TEXEL1, alpha TEXEL0` and `cycle1 = COMBINED` — i.e. a real
two-cycle combiner whose colour comes from the second texture tile and whose
alpha mask comes from the first. All of it is now visible in the `[GFX-CMD]` log
as an N64 expression per cycle.

### Finding: OB64 loads every image into tile 7 while `G_TEXTURE` selects tile 0

`[GFX-TEX]` (tile index added this session) shows every `G_LOADTILE`/`G_LOADBLOCK`
targeting **tile 7**, while the game's `G_TEXTURE` names **tile 0**
(`D7000002 80008000`). That is not a decode bug (RT64 decodes both the same way);
all eight tiles in OB64's state block are programmed identically with `tmem = 0`,
so the tile index does not select the image — the *load order* does. The RDP
samples TMEM, and the pair of loads per object (an I8 alpha mask, then the
RGBA16 colour image) overwrite the same TMEM region.

### Decision: model the two texel units as the last two loads

A draw needs two images (the combiner takes colour from TEXEL1 and alpha from
TEXEL0), so `RenderState` keeps `recent_loads[2]`: `[1]` = most recent load,
`[0]` = the one before it, with its own UV transform (loaded size + `uls/ult`
origin). The shader gets `v_uv0`/`v_uv1` and two sampler units, and an unloaded
unit binds a 1x1 white fallback instead of texture 0. This replaces "the last
loaded texture is the only texture", which silently sampled the colour image for
TEXEL0 as well. (A real TMEM model — per-tile tmem address, line and the tile's
`uls/lrt` rect as the UV basis — is the principled replacement; see the handoff.)

### Decision: the walker and the executor share one address resolver, and the walker tracks segments

`gbi::walk_dl` picked the next command while `WebGLRenderer::exec_command`
decoded it, but each had its own copy of `resolve_address` and only the executor
applied `G_MOVEWORD G_MW_SEGMENT`. The walker's segment registers therefore
stayed zero, so a `G_DL` to a segmented address was followed as if it were
physical — the walker and the executor could disagree about where the walk *is*.
Both now call `gbi::resolve_address` and `gbi::set_segment_from_moveword`.

---

## 2026-09-11 — the `/tmp` browser probes are consolidated into `debug/`

### Decision: keep the probe harness in the repo, not in `/tmp`

Every browser session so far drove the wasm build with one-off Playwright
scripts under `/tmp/ogre-probe` (18 files, ~1 200 lines), each hard-coding the
Playwright install at `/Users/momo/dev/fatos/node_modules/playwright`, the ROM
path, and the `:8931` URL. They were never committed, and loss had already
started: session 16's `server.py` was gone from disk while its process was still
serving `:8931`, and the base `probe.cjs` that `probe-delay.cjs` documents itself
as extending no longer existed.

The reusable part is the harness, not the individual scripts: the boot-retry
rule, the page contract (`#rom-input`, `#gfxstats`, `#gfx-status`,
`#game-canvas`, `#status`, `window.ogreLog`), and the measurement methods. So
`debug/` now holds a shared `lib/harness.cjs`, eight curated probes (`boot`,
`shots`, `fps`, `vi`, `stats`, `diag`, `logmode`, `testdraw`), the reconstructed
COOP/COEP `server.py`, and a Playwright `package.json`. Paths and budgets come
from flags (`--url`, `--rom`, `--out`, `--out-dir`, `--attempts`, `--secs`) or
`OGRE_URL`/`OGRE_ROM`/`OGRE_OUT`; artifacts go to the gitignored `debug/out/`.
See `docs/guides/web-probes.md`.

### Decision: the "idle trajectory" retry lives in the harness, not in each probe

Roughly one in two boots stalls at ~3 display lists (`tasks=1` in `#gfxstats`)
instead of submitting the game's display list (`tasks>=2`). `attemptLoop()`
re-runs the whole boot until a real list arrives; a probe without it reports a
false negative about half the time. That rule, plus reading `window.ogreLog`
instead of the DOM, is the main reason the probes belong in the repo rather than
in a session's shell history.

The one-off investigation scripts (the freeze/locate/tap variants) are not
carried over; their findings are in the session handoffs. Rebuild them from the
harness if those bugs recur.

---

## 2026-09-10 (session 20) — The browser renderer now draws the title scene; seven WebGL2-prototype bugs found (viewport, matrix decode, matrix stack, empty rects)

### Finding: the black screen was seven independent renderer bugs, not a game/runtime problem

`app/src/web_renderer.cpp` had never applied the RSP viewport, was decoding
every `Mtx` with its column pairs swapped, ignored the projection `MUL`, composed
modelview matrices in the wrong order, read F3DEX2's inverted `G_MTX` PUSH bit
literally (so the title DL's per-object matrices accumulated into one another),
hit `push_back(vector.back())` UB once the PUSH path became live, and drew a
`TEXRECT` that decodes as an inverted/empty rectangle as a full-screen cover.
Fixing those takes the title DL from 18/24 triangles rejected and a black canvas
to 0 rejected and a stable ring of twelve character sprites. See
`docs/HANDOFF-2026-09-10-session20.md` for the evidence and the bisection.

### Decision: match RT64's HLE semantics exactly where they are observable

Each fix was validated against `tools/RT64` rather than guessed:
`RSP::setViewport` (viewport field indices), `FixedMatrix::toFloat` (the `j ^ 1`
column swap forced by the runtime's byte-reversed rdram), `RSP::matrixCommon`
(compose `MUL` as `m * old`, keep one composite view-projection matrix),
`GBI_F3DEX2::matrix` (`p0(0,8) ^ pushMask`, i.e. the PUSH bit is stored
inverted - the game's own `0xDA380000`/`0xDA380001` words confirm it), and
`RDP::fillRect` / `FixedRect::isEmpty` (empty and inverted rectangles draw
nothing).

### Deliberately not applied: the `G_TEXTURE` `sc`/`tc` scale

RT64 converts vertex texcoords as `(s * sc) / (65536 * 32)` and OB64 submits
`sc = tc = 0x8000`, so the hardware UVs are half of this renderer's `s/32`.
Applying it visibly truncates the sprites (the vertical span drops below the
loaded texture height), so it is left unapplied and recorded as the top open
question with the per-draw values logged in `[GFX-V]`.

### Decision: no per-line DOM logging; the runtime log lives on `window.ogreLog`

The page used to append every `printf` line to `#status` with
`textContent += line`. Emscripten proxies wasm-thread output onto the browser
main thread, so that O(n) write per line blocked the printing threads and cost
roughly 2x of the frame rate once the log passed 100 KB. Lines now go into a
bounded ring buffer exposed as `window.ogreLog`
(`tail`/`text`/`find`/`contains`/`save`/`clear`/`show`), `#status` renders one
line on a 200 ms timer, and `?log` restores the full console for debugging. Keep
the page free of per-line DOM work whenever wasm threads can print.

---

## 2026-08-29 (session 10) — The VI-thread segfault was a runtime pointer-translation bug; osViSetMode now validates its argument; the GBI question is resolved (auto-detected F3DEX2 is correct)

### Finding: OB64's `osViSetMode` is really the swap-context routine and ignores its argument; the runtime translated a garbage value into an out-of-bounds pointer

The game's `osViSetMode` at `0x80095820` (bridged to the runtime's native
`osViSetMode_recomp`) is actually `__osViSwapContext` — it reads
`__osViNext->mode`/`framep` from its own context and writes the VI hardware
registers, never using its incoming `$a0`. Its callers pass leftover registers:
`func_8009AB50` (VI init, called by `osCreateViManager`) passes the MMIO address
`0xA4400010` (VI_STATUS), and `viMgrMain` passes a leftover message-queue
pointer. On hardware this is harmless.

The runtime's `osViSetMode` translated that argument with `TO_PTR`, producing a
host pointer ~4.58 GB past the start of the 512 MiB rdram buffer
(`rdram[0x124400010]`), stored it as the next VI mode, and the VI thread's
`update_vi()` dereferenced it (`next_mode->comRegs`) → the long-standing flaky
SIGSEGV. It reproduced under both the Intel hasvk driver and Lavapipe, so it is
a general port bug.

### Decision: validate the mode pointer in `osViSetMode`

`ultramodern/src/events.cpp` now translates the mode argument only when it is a
KSEG0 address (`0x80000000..0xA0000000`), which maps 1:1 into the rdram buffer;
otherwise it keeps the current mode (matching real hardware, where the argument
is unused). Also added null-guards in `update_vi()` (fall back to `dummy_mode`)
and `osViSetSpecialFeatures`. Result: the VI-thread crash is gone; under Lavapipe
the port boots, submits and completes 1864 RSP gfx tasks, and renders without a
single crash.

### Finding/Decision: RT64's auto-detected F3DEX2 GBI is correct — remove the session-9 "force F3DEX" experiment

Dumping the game's real display lists settled the open GBI question:
- First gfx task DL: `DE000000 / 000A9EF0` = G_DL branch, `E9000000` =
  G_RDPFULLSYNC, `DF000000` = G_ENDDL — exactly RT64's F3DEX2 map
  (`0xDE`=G_DL, `0xDF`=ENDDL).
- Branch target `0xA9EF0` is standard RDP color/rect setup (`0xFB..0xF7`,
  `0xFC`, `0xF5`); later boot DLs branch to real KSEG0 targets (`0x801869E8`).

So `GBIManager::getGBIForUCode` (which matches OB64's ucode hash to
`GBIUCode::F3DEX2`) is right, and the session-9 "force `gbiCache[F3DEX]`"
override misparsed the DLs (plain F3DEX does not map `0xDE/0xDF`). The override
was removed; `renderer.cpp` documents this.

### Outcome

Boot is stable under Lavapipe (software rasterizer). The remaining crash
(`submitRasterScene` → `libvulkan_intel_hasvk.so`) is specific to this dev
machine's Intel Haswell GPU (the driver itself warns its Vulkan support is
incomplete) and is not a general issue; the next session should run on a
supported GPU. The vendored runtime changes are snapshotted in
`n64modernruntime-ob64.patch` (new).



## 2026-08-25 (session 9) — The `func_8019FC68` DL-build crash was a recompilation bug; N64Recomp now emits fall-through tail calls; boot reaches real rendering

### Finding: KMC shared-epilogue functions lose their register restores

OB64's compiler (KMC) merges identical function epilogues into a separate symbol
that the preceding function **falls through into**. `func_8019ABC4`'s epilogue
ends with `lw $ra/$fp/$s7/$s6` at `0x8019AEFC..0x8019AF08` and then falls into
`func_8019AF0C`, a separately-symbolized shared epilogue that restores
`$s5..$s0` and `$sp`. N64Recomp treated the fall-through as the end of
`func_8019ABC4` and never ran the shared epilogue, so the callee-saved
registers and `$sp` were left clobbered. In the title path,
`func_8019FC68` (session-8 crash site) captured a heap pointer in `s0`, called
`func_8019ABC4`, then `free(s0)` — freeing a garbage value (`0x801bc666`) and
SIGSEGV'ing in `func_800712C4`.

### Decision: emit a fall-through tail call for shared-epilogue chains

`N64Recomp::recompile_function_impl` now, after processing a function's
instructions, emits `next_func(rdram, ctx); return;` when the function's code can
fall off the end of its range and the next function starts exactly at
`func.vram + words*4`. "Can fall off the end" means the last instruction is not
`jr`/`j`/`syscall`/`break`/`eret` (nor the delay slot of one of those, which
already emitted a return). Detected 23 genuine cases across all segments.

### Decision: process N64Recomp static functions to a fixpoint and register them by address

Static functions (created for cross-boundary branches) were neither registered in
`context.functions_by_vram` nor created in an order that guaranteed a static's
fall-through target (another static) existed before it was recompiled. `main.cpp`
now registers statics in `functions_by_vram` and processes them in a two-phase
fixpoint loop (create all known statics, then recompile the batch, repeat until no
new statics). This fixed `static_16_8019AB84` → `static_16_8019AB94` (a shared
epilogue chunk), which was otherwise lost. Total fall-through tail calls: 35.

### Outcome

Boot now passes the title-screen DL build (the `func_8019FC68` crash is gone),
runs ~1528 PI DMAs and 64 RSP tasks (including new `type=1` tasks), and reaches
real RT64 frame rendering. The next crash is in the **RT64 render thread** calling
`libvulkan_intel_hasvk.so` (the Intel Haswell Vulkan driver) inside
`FramebufferRenderer::submitRasterScene` — see
`docs/HANDOFF-2026-08-25-session9.md` for leads (driver vs. RT64 GBI vs. the
pre-existing flaky renderer-init race).



## 2026-08-25 (session 8) — The post-boot spin was a cooperative-scheduler deadlock; N64Recomp now emits `yield_self` for poll loops


### Finding: Thread 3's `.L80075FB8` spin on `D_800C4C26` starves the drainer (deadlock)

The session-7 "N64 threads 1+3 spin" is Thread 3 (`func_80075BC0`, the system
loop) waiting in a **pure spin** for the 16-bit word `D_800C4C26` to change
from `0xFFFF`/`0xFFFD`. The word is set to `0xFFFC` by the boot state-machine
callback `func_80072398` after 13 invocations. The callback is driven by the
game's own event chain — Thread 19 (`func_80088F08`) wakes on VI-retrace/AI
messages (`0x29A`/`0x29D` on `D_800E8B84`), dispatches via `func_800891A0` to
Thread 4 (`func_8008AFE0`, queue `D_800C4C28`), which calls the callback. The
VI thread enqueues the messages, but delivery requires the cooperative
**drainer** (pri 5) to run, and Thread 3's pure spin never yields → the whole
chain stalls forever. Same class as session 6's `func_80089A10` spin.

### Decision: make N64Recomp emit `yield_self` for poll loops (general fix)

Instead of reimplementing the spinning function natively (session 6's approach,
impractical for the 1390-byte `func_80075BC0`), N64Recomp's `print_branch` now
detects **poll loops** — backward conditional branches whose body has no
function calls and no stores, and contains a load whose base register is
loop-constant at the load site (cyclic last-write is `lui`, or never written in
the body) — and emits `yield_self(rdram);` before the loop-back goto.
`yield_self` (already in the runtime) waits for one external message and checks
the running queue, so the drainer can deliver the very event the poll awaits.

The heuristic was tuned against the whole codebase: it flags ~17 loops (the
`D_800C4C26` spin, the PI/DP status polls, overlay game-logic polls) while
correctly excluding data loops (strlen/list-walk/memcpy) whose load bases are
written by non-constant ops.

### Outcome

Boot now passes the old spin, runs through the overlay-D data loads, the
controller path, and the RSP pipeline, and reaches **title-screen display-list
building** (`func_801A1FCC`/`func_8019FC68`, overlay C). The next crash is
`func_8019FC68` at `0x8019FFF4`: it stores through `0x803ffa7b +
entry->[0x34]` where a graphics-object entry in the heap array at `0x803fefa0`
has a garbage `[0x34]` (varies per run), wrapping the address to unmapped
RDRAM. The array is allocated + zeroed by `func_801A1A2C`, so the garbage is
written later — root cause open (see session-8 handoff leads: likely an
audio-init / record-population step or a wrong splat function boundary for the
`nonmatching func_8019FC68, 0xF58` region).


## 2026-08-25 (session 7) — Phase 4: streamed overlays are plain linked code (no relocation); A+B+C recompiled and registered

### Finding: the streamed overlays need no runtime relocation

The session-6 handoff framed the next wall as "overlay relocation". That was
wrong. OB64's streamed overlays are **plain linked MIPS code+data** DMA'd to a
**fixed** RAM address (per the streamed-segment table at ROM `0x387C0`). All
pointers inside them (function-pointer tables at e.g. ROM `0x65200`, RDP
display lists, data tables) are absolute addresses already correct for that
load address. The real problem was that overlay *functions* were log-and-return
stubs, so the data structures they build at boot (e.g. the descriptor that
`func_80075BC0` dispatches through) were never created, and the game read raw
code bytes as function pointers (`0x3C028019` = `lui $v0, 0x8019`).

### Decision: disassemble + recompile the overlays into the same ELF, register them eagerly

- Added `streamedA` (ROM `0x3F1B0` → RAM `0x800E9C20`), `streamedB` (ROM
  `0x40E80` → RAM `0x8016AF80`), and `streamedC` (ROM `0x1CE040` → RAM
  `0x80197B90`) as splat `code` segments; bin gaps (`0x66E30`, `0x1F0A00`) are
  pinned to their ROM address as VMA so they don't collide in RDRAM VMA space.
- Recompiled the whole ELF together (807 → ~1520 functions). Cross-overlay
  absolute references (overlay B takes the address of overlay C's
  `.L8019EE70`) need the label exported globally; `tools/fix_cross_overlay_labels.sh`
  re-applies that after every `splat split` (wired into the Makefile).
- The app registers A+B+C via `load_overlays()` from the GameEntry
  `on_init_callback`; the runtime's `recomp::init()` now loads only the base
  sections (`entry`+`main`) so overlays aren't registered at the wrong
  linear-mapped addresses (which would corrupt `section_addresses`, read by the
  recompiled overlay code through `RELOC_HI16`/`LO16`).

### Decision: a `load_overlays` DMA hook was tried and REVERTED

Hooking `func_8008BC40_recomp` (the game's DMA request, reimplemented as a sync
ROM read) to call `load_overlays(dev_off, dramAddr, size)` is the "obvious" way
to register overlays as the game streams them. It fails for OB64 because the
game streams in **0x200-byte chunks**: `load_overlays` computes a bound range
from `rom`+`size`, and a chunk-sized range makes `lower_bound > upper_bound`
(inverted), so the load loop walks off the end of the section table and
segfaults. Registration therefore happens eagerly at boot instead.

### Outcome

The game now boots through the old `func_80075BC0` function-pointer-table crash
and runs ~1520 real functions through the whole streamed-overlay load sequence
before **spinning on N64 threads 1 and 3** after loading the next overlay's data
blocks into RAM `0x801BD930`. No stub calls, no `get_function` hard-fail.
Remaining: find that spin (overlay D at ROM `0x1F0A00` → RAM `0x801F7100` is the
likely next recompilation target), fix the flaky VI-thread segfault, then the
RT64 GBI fix.


## 2026-08-25 (session 6) — The boot stall was a scheduler busy-spin deadlock; three fixes unblock boot into the main loop

### Finding: "waits for a second RSP task" was wrong — the game thread busy-spins and starves the drainer

The session-5 conclusion (callback slots at 0x800B9E84 all zero → the sync task's
completion "does nothing" → the game waits for a second task) was **incorrect** on two
counts:

- The real callback-slot address is **0x800A9E84** (0x800B9E84 was a dump misrecord, off
  by 0x10000). The type-4 slot is registered at boot (`func_8008A1B0` →
  `func_800899D0(func_8008B110)`), and the sync task is **type 0**, which never dispatches
  to any callback anyway — so "dispatch does nothing" is expected, not a bug.
- The actual deadlock: after the sync task, the game thread enters `func_80089A10`, a tight
  `bnez` spin on the task-done counter `D_800E79A4`. The runtime's cooperative scheduler
  (higher priority numbers first) cannot preempt a spinning thread, so the spin (pri 10)
  starves the external-message **drainer** (pri 5) that must deliver the SP/DP completion
  events which eventually decrement the counter.

### Decision: reimplement the spin (`func_80089A10`) as a yielding wait

N64Recomp only emits its `pause_self` yield for *exact self-loops* (`j`/`b` to the
instruction's own address); this spin branches to the *function start*, so it was compiled
with no yield. Rather than special-casing the generator, we added `func_80089A10` to
N64Recomp's `reimplemented_funcs` and implemented `func_80089A10_recomp` in the runtime:
loop while `MEM_W(0, 0x800E79A4) != 0`, calling `wait_for_external_message` +
`check_running_queue` each iteration. This lets the game thread itself drain the SP/DP
completion events and hand off to the higher-priority RSP threads.

### Finding: the game's PI manager is dead, so every DMA deadlocks

`osCreatePiManager_recomp` is an **empty stub**. The game's verbatim `osCreatePiManager`
(0x8008B8C0) — the only writer of `D_800AA400`/`D_800AA408` — is dead code. The game's
DMA-request function `func_8008BC40` checks `D_800AA400` and bails when 0, so the blocking
DMA helpers (`func_80089F80`, `func_8008A0F0`) wait forever on their completion queue; the
first streamed-code load (`func_8009DA50`) deadlocks.

### Decision: reimplement `func_8008BC40` (DMA request) as a synchronous ROM read

`func_8008BC40_recomp` reads the game's `OSIoMesg` (mq@+4, dram@+8, dev@+0xC, size@+0x10),
calls `recomp::do_rom_read`, and completes the request via `osSendMesg` so the caller's
`osRecvMesg` returns immediately. One function fixes all game DMA paths without the PI
thread. **Important**: the ROM copy must go through `recomp::do_rom_read` (which
byte-swaps big-endian ROM bytes into host-order RDRAM words via `MEM_B`); a plain `memcpy`
produces byte-swapped garbage pointers.

### Decision: generic stub for not-yet-loaded streamed functions

`get_function` now returns a logging no-op stub for unknown addresses in the streamed
ranges (`0x8016A000..0x80200000`, `0x84000000..0x84200000`) instead of hard-failing. This
lets boot reach the main loop. It is a bring-up measure; Phase 4 will replace the stubs
with recompiled overlay functions.

### Outcome

The game now boots through the RSP pipeline, controllers, 703 streamed-overlay DMA loads,
and into its main loop (18 distinct streamed functions called, all stubbed). The next
crash is an **indirect call through a streamed-overlay function-pointer table that needs
relocation** (`func_80075BC0` reads `*(u32*)(*(u32*)(0x800E8294))` = `0x3C028019`, a MIPS
instruction, not a pointer). That is the start of **Phase 4 (overlay loading +
recompilation)**. See `docs/HANDOFF-2026-08-25-session6.md`.


## 2026-08-25 (session 5) — RT64 renderer integrated; the boot stalls waiting for a second RSP task

### Decision: replace the null renderer with RT64 (Vulkan on Linux)

The game has been submitting real gfx tasks since session 4, so the null
renderer has served its purpose. `app/src/renderer.cpp` now wraps
`RT64::Application` (the current RT64 HLE architecture with `Interpreter` /
`GBIManager` / `State`) behind ultramodern's `RendererContext`. Wiring copied
from N64Recomp/RecompFrontend's `rt64_render_context.cpp`: `Application::Core`
gets RDRAM, DMEM/IMEM slots, DPC registers, and the VI registers straight from
`ultramodern::renderer::get_vi_regs()`. RT64's render hooks are optional
(null-checked) and not wired.

Verified on the Intel HD 4400 (Mesa): RT64 sets up Vulkan, presents frames at
~60 Hz, and the game boots through the renderer unchanged. This also fixed the
SDL window: it must be created with `SDL_WINDOW_VULKAN` on Linux (and
`SDL_WINDOW_METAL` on macOS) or `SDL_Vulkan_CreateSurface` fails and RT64
segfaults during setup.

### Decision: gfx display lists are interpreted by RT64, not recompiled microcode

The runtime routes gfx tasks (`M_GFXTASK`) to the renderer's `send_dl`, never
through `get_rsp_microcode` — so `RSPRecomp` is **not** needed for the gfx
ucode. RT64 ships its own GBI interpreters (`src/gbi/`) and parses display
lists itself via `loadUCodeGBI` + `processDisplayLists`. RSPRecomp remains for
the **audio** ucode (task type 5) once audio tasks flow.

### Finding: OB64's ucode is "F3DEX fifo 2.08" (short-format opcodes) and RT64 misidentifies it as F3DEX2

Runtime OSTask dump: `ucode=0x8009F540 ucode_data=0x800AC140` →
ROM `0x2F940`/`0x3C540`; the data block contains
`"RSP Gfx ucode F3DEX fifo 2.08 ... Yoshitaka Yasumoto 1999 Nintendo"`.
The game's display lists use the F3DEX **short-format** opcode set
(`G_SETOTHERMODE_H=0xDE`, `G_SETOTHERMODE_L=0xDF`, `G_RDPFULLSYNC=0xE9`, ...).

RT64's `GBIManager::getGBIForUCode` (XXH3 hash database) matches this ucode to
`GBIUCode::F3DEX2` (verified in-app: `GBI matched: 7`). F3DEX2 semantics differ
(`0xDE` = `G_DL`, `0xDF` = `G_ENDDL`), so RT64 misparses OB64's DLs — the first
(sync) DL "branches" into MIPS code at RDRAM `0xA9EF0`. RT64 has no GBI map for
the F3DEX-2.08 short set. **Action item**: add/force the correct GBI before
real DLs flow (see handoff).

### Finding: the boot stalls after the first gfx task — the task-done dispatch has zero callbacks

The whole RSP task pipeline works end-to-end: `osSpTaskStartGo` → `sp_complete`
(→ `0x800E8BBC`) + `send_dl` + `dp_complete` (→ `0x800E8BF4`) →
`func_80089200` sends the task-done message to the task's `done_mq`
(`0x800E9BA8`) → the done pump `func_80089540` consumes and dispatches on the
message's command type. But the dispatch callback slots (`0x800B9E84/88/8C`)
are all **zero** at boot, so the first (sync) task's completion does nothing
and the game waits for a second task that never comes. The game's next boot
steps (`func_8009DA50` streamed DMA, then streamed funcs `0x800E9CEC`/
`0x800E9C20`) are never reached. See the handoff for the thread map and the
investigation path.

### Decision: keep the boot-time diagnostics in the vendored runtime for now

The gitignored runtime carries temporary `[sp]`/`[ev]`/`[mq]` debug prints
(task submission, event registration, SP/DP completion, targeted queue
activity). They are cheap, and removing them needs a full librecomp/ultramodern
rebuild; they are documented in the handoff and should be stripped once the
stall is fixed.

---

## 2026-08-25 (session 4) — First RSP task + real VI mode: boot is past the libultra bridge

### Decision: correct osSendMesg/osJamMesg identification

Session 3 named `0x800935A0 = osSendMesg`, but that function does a **front
insert** (`mq->first = (first+msgCount-1) % msgCount`), i.e. `osJamMesg`. The
game's real back-insert `osSendMesg` is `func_80093810` (36 call sites; the
audio/event request system uses it). Renaming `func_80093810 = osSendMesg`
makes audio-request responses go through the runtime's `osSendMesg`, which
**wakes blocked threads** — the missing piece that let the game thread proceed
past its first audio request.

### Decision: add an external-message drainer thread to the runtime

With VI events being delivered every frame, the boot still deadlocked: the
runtime only drains its external-message queue when a game thread calls
`osSendMesg`/`osRecvMesg`, and OB64's boot blocks every game thread on a queue
fed by the VI retrace event (its main thread busy-spins). Added a hidden
"drainer" game thread in `librecomp/src/recomp.cpp` (spawned after
`on_init_callback`) that loops `wait_for_external_message` +
`check_running_queue`. It must be **lower** priority (5) than the threads it
wakes, because this runtime schedules **higher** priority numbers first —
priority 30 starved the game thread (pri 10) in the running queue.

### Decision: name the osCont family, timers, events, and osSpGetStatus

- The PFS (Controller-Pak) cluster at `0x80096B90+` is *not* the osCont family
  (`__osSumcalc`/`__osIdCheckSum` etc.); the osCont cluster is the SI-touching
  one at `0x800900C0..0x800906C0`. Named `osContInit`, `osContStartQuery`,
  `osContGetQuery`, `osContStartReadData`, `osContGetReadData`.
- Timers: `osGetTime` 0x80094C90, `osSetTime` 0x80094D20, `osSetTimer`
  0x80094D40 (the handoff's `0x80094E34` is the timer *interrupt handler*, not
  osSetTimer).
- Events: `osViSetEvent` 0x80095560 and `osSetEventMesg` 0x80093940 — the game
  registered its VI-retrace→audio-queue message (0x29A) through its verbatim
  osViSetEvent, so the runtime never delivered it.
- `osSpGetStatus` 0x800939F0: the RSP task threads busy-wait on SP_STATUS; not
  dead code as session 3 assumed. Added to N64Recomp's `reimplemented_funcs`
  (N64RecompCLI rebuild required) + runtime `osSpGetStatus_recomp` (returns
  SP_STATUS_HALTED).

### Outcome

The game boots without crashing for the full observed window (~25 s), submits
its **first RSP gfx task** (`send_dl frame=1 type=1`), and sets a **real VI
mode** (dummy workloads stop; the null renderer swaps the game's framebuffers
at ~50-60 Hz). Next watch point: the streamed-code ROM DMA (`func_8009DA50`)
before Phase 4 overlay loading.

---

## 2026-08-24 (session 3) — First boot achieved: libultra bridging is working

### Decision: name-based libultra replacement confirmed and applied (3 batches)

Applied 24 names to `symbol_addrs.txt` (see `LIBULTRA-BRIDGING.md` "Progress").
The boot path now runs the runtime's native `osXxx_recomp` services:
`osInitialize`, `osCreateThread`/`osStartThread`/`osSetThreadPri`/
`osGetThreadPri`, `osCreateMesgQueue`/`osSendMesg`/`osRecvMesg`,
`osCreatePiManager`/`osCartRomInit`, `osSetIntMask`,
`osAiGetLength`/`osAiGetStatus`/`osAiSetFrequency`/`osAiSetNextBuffer`,
`osSpTaskLoad`/`osSpTaskStartGo`/`osSpTaskYield`/`osSpTaskYielded`,
`osCreateViManager`/`osViSetMode`/`osViSetSpecialFeatures`/`osViSwapBuffer`/
`osViGetCurrentFramebuffer`/`osViGetNextFramebuffer`, `osDpSetNextBuffer`,
`osGetCount`, `__osSetFpcCsr`, `osVirtualToPhysical`.

**Outcome:** the game **boots without crashing** on Linux — 7 N64 threads start,
the null renderer swaps VI buffers at ~50-60 Hz, and the game runs its init.
No MMIO shim (handoff option 3) was needed; naming the RSP-task family made the
remaining verbatim MMIO readers (`osSpGetStatus`, `osSpSetStatus`,
`__osAiDeviceBusy`, `osSiGetStatus`, `osDpGetStatus`) unreachable dead code.

### Decision: fix the runtime's initial-1MB DMA sign-extension bug

`recomp::init()` passed the entrypoint VRAM address to `do_rom_read` as a
zero-extended `uint64_t`, but recompiled memory accesses expect sign-extended
32-bit addresses (the `MEM_B`/`MEM_W` macros subtract `0xFFFFFFFF80000000`).
The DMA wrote ~4 GiB past rdram, so the game's data section never loaded and a
later verbatim VI helper null-deref'd. Fixed by sign-extending:

```cpp
recomp::do_rom_read(rdram, (gpr)(int32_t)entrypoint, 0x10001000, 0x100000);
```

This is a genuine N64ModernRuntime bug that other projects would hit with a
high `0x800xxxxx` entrypoint; our fix stays local to the vendored runtime.

### Decision: new analysis tool

Added `tools/libultra_scan.py` — parses `asm/1060.s` and reports, per function:
MMIO registers, cop0 registers, and direct callees. This replaces the one-off
manual scans with a reproducible inventory.

### Next session (see `docs/HANDOFF-2026-08-24.md` / `LIBULTRA-BRIDGING.md`)

- Name the `osCont*` family (`osContInit` etc.) — the game may be waiting on
  controller init; the game thread has not yet hit streamed-code stubs or
  submitted RSP tasks in the observed window.
- Watch for the first RSP task and the game's first real `osViSetMode`
  (the null renderer currently shows the runtime's dummy framebuffers).

---


## 2026-08-24 (session 2) — Libultra bridging: name-based replacement + platform decision

### Decision: adopt handoff Option 1 — name OB64's libultra functions in the ELF

The next milestone (see `LIBULTRA-BRIDGING.md`) is to make the runtime's native
`osXxx_recomp` services replace OB64's verbatim libultra. Mechanism is purely
name-based in N64Recomp (`reimplemented_funcs`), so the work is building the
OB64 libultra **address→name table** and adding it to `symbol_addrs.txt`,
then re-splat → rebuild ELF → re-run `make recomp`.

Rejected for now:
- **MMIO shim first (handoff option 3)**: only to be used surgically for
  registers the game still reads after reimplementation.
- **Address-based N64Recomp config**: viable fallback, but the canonical
  `symbol_addrs.txt` route also names the disassembly for future debugging.
- **Skipping the game's libultra entirely (option 4)**: too risky.

### Decision: this milestone is macOS- and Linux-equivalent; switch for the renderer phase

The libultra-bridging work is CPU-side and platform-independent — no reason to
switch machines for it. The RT64/renderer phase should move to Ubuntu (Vulkan
primary backend). Documented in `guides/linux-migration.md` with the exact
macOS-specific code to remove (`app/CMakeLists.txt` APPLE blocks, the
`__APPLE__` branch in `sdl_platform.cpp::create_window`, the RT64 DXC `libz.dylib`
symlink, Metal toolchain download).

### Decision: correct the function count

PLAN.md's "3659 functions" is stale; the recompiled output has **807 functions**
(802 `func_800xxxxx` + `main_recomp`/`recomp_entrypoint`/others).

---

## 2026-08-24 — Phase 3: first-boot app

### Decision: static recompilation pipeline (unchanged, restated)

Confirmed the project continues on the `N64Recomp` + `N64ModernRuntime` stack
(Zelda 64: Recompiled approach), not a matching decomp.

### Decision: app project layout

- New `app/` CMake project produces the `ogrebattle64` executable.
- `RecompiledFuncs/*.c` (recompiler output) is compiled into a static lib
  (`ogrebattle64_recomp`) and linked into the app.
- `tools/N64ModernRuntime` (ultramodern + librecomp) and `tools/RT64` are pulled
  in via `add_subdirectory`.
- `tools/RecompFrontend` (RmlUi-based menu UI) is intentionally **not** used yet;
  it adds heavy dependencies (RmlUi, lunasvg, GamepadMotion) and is only needed
  for the config/mod menu (Phase 7). Input is wired directly via SDL2 for now.

Rationale: keep the first-boot milestone minimal and debuggable. Adding the
frontend later is purely additive.

### Decision: RT64 as the renderer

RT64 (MIT) is the recommended renderer for N64ModernRuntime projects and the
same one used by Zelda64Recomp. It interprets RDP commands and provides the
`RendererContext` used by ultramodern. Added as submodule at `tools/RT64`.

Build options (same as Zelda64Recomp): `RT64_STATIC=ON`, `RT64_SDL_WINDOW_VULKAN=ON`,
`HLSL_CPU` compile definition. On macOS RT64 uses its **Metal** backend (the
`RT64_SDL_WINDOW_VULKAN` option is only meaningful on Linux).

### Decision: phased bring-up (null renderer first)

The app is structured so the renderer is swappable. For the first boot
validation we use a **null renderer** that:
- acknowledges VI register updates and display-list submissions without drawing,
- logs boot progress (VI swaps, RSP tasks, thread activity).

Rationale: Ogre Battle 64 renders everything through RDP, and RDP commands only
exist after the game's RSP microcode is recompiled. A null renderer lets us
validate the whole boot path (thread scheduler, libultra shims, PI DMA) before
RT64/RSP work lands; it also avoids coupling renderer bring-up with microcode
bring-up.

### Decision: ROM hash constant

`GameEntry.rom_hash` uses XXH3-64 of the full big-endian ROM
(`librecomp` hashes the post-byteswap contents):

```
XXH3_64(assets/ogre64.z64) = 0xbe6adaa5c3f8f7a9
```

### Decision: entrypoint

`GameEntry.entrypoint_address = 0x80070C00` (cart header entry), entrypoint
function = `recomp_entrypoint` (recompiled boot stub). `recomp::init()` already
handles IPL3 variable setup and the initial 1MB ROM DMA, and the boot stub
clears BSS and jumps to the game's `main_recomp`.

### Decision: streamed/overlay code stubs during bring-up

Functions outside the ELF (`0x8016C900+`, `0x84001120+`, etc.) are referenced
via runtime `get_function` lookups, which hard-fail if a function is unknown.
Until Phase 4 implements real overlay loading, the app registers **log-and-return
stubs** for all unresolved addresses via `recomp::overlays::add_loaded_function`.
This lets the boot path survive early references to streamed code.

---

## 2026-08-24 — First boot findings (critical)

### Decision: record the MMIO / libultra-verbatim problem

The app now builds and boots the runtime, but the game crashes on its first
hardware access:

- `recomp::mem_size` is **512 MiB** (not GB); the recompiled `MEM_W` macro maps
  MMIO reads like `0xA4800018` (SI status) to `rdram + ~0x24800000`
  (~580 MiB), which is outside the RW region → SIGBUS.
- More fundamentally, OB64's libultra is recompiled **verbatim** (generic
  `func_800xxxxx` symbols), so N64Recomp's name-based libultra reimplementation
  never fired and the runtime's `osXxx_recomp` services are not called by the
  game. Bridging this (see `HANDOFF-2026-08-24.md` options 1–4) is the next
  milestone.

### Decision: `start_game` must be delayed

Calling `recomp::start_game` before `recomp::start` makes the VI thread skip
its dummy-mode phase and crash on a null VI mode. The app now starts the game
from a thread delayed 500 ms.


### Decision: SDL2 dependency

The app uses SDL2 for window/input/audio. On macOS it is provided by Homebrew
(`brew install sdl2`, which today installs `sdl3` + `sdl2-compat`).

### Decision: licensing

- N64Recomp: MIT
- N64ModernRuntime: GPL-3.0
- RT64: MIT
- RecompFrontend: (see repo)

Because N64ModernRuntime is GPL-3.0 and is statically linked, the final
`ogrebattle64` app is **GPL-3.0**.
