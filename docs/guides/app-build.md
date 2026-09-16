# Building and running the PC port app

The app (`app/`) compiles the recompiled game code against `N64ModernRuntime`
and links the renderer (RT64), then boots the game on your ROM.

## Prerequisites

- CMake ≥ 3.20 and a C++20 compiler (clang/gcc/msvc).
- SDL2. On macOS: `brew install sdl2` (installs `sdl3` + `sdl2-compat`).
- The toolchain from the main [PLAN.md](../../PLAN.md) reproduce section
  (splat, mips binutils, N64Recomp, RT64, N64ModernRuntime submodules).
- Your own big-endian `.z64` dump at `assets/ogre64.z64`.

## Configure & build

```sh
# one-time: initialize all third-party submodules
git submodule update --init --recursive

# apply the SDL >= 2.0.22 compatibility patch to RT64's plume submodule
# (needed on systems with older SDL2, e.g. Ubuntu 22.04's 2.0.20)
git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch

# apply the OGRE diagnostics (present traces, GPU readback capture, presenter
# knobs). rt64-plume-ob64.patch is the texture -> buffer readback support in
# plume's Metal backend.
git -C tools/RT64 apply ../../rt64-ob64.patch
git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-ob64.patch

# regenerate the recompiled code if it has changed (uses the ELF + config.toml)
make recomp

# streamed-overlay bank units (Phase 4): a second recompilation for the overlay
# records the game loads into RAM another overlay/bank also occupies. Required
# before configuring the app — it writes Bank{A..G}Funcs/ and
# app/src/bank_funcs.inc, and the app links all of them. Units are listed in
# `BANK_UNITS` in the Makefile; add a unit there when a new bank record is
# compiled (a range can hold several records, but two records that share RAM
# must be in different units — see config-bankF.yaml).
# bank-recomp also re-applies the two generated-code patches that must survive
# regeneration: `cross_bank.py check-banks` (an invariant) and
# `tools/njpeg_readback.py` (the njpeg stage-3 copy must read the buffer its own
# YUV draw landed in — see docs/HANDOFF-2026-09-15-session48.md).
make bank-recomp

# optional: the full cross-bank audit (bank units fail, main-unit backlog is
# reported; `make bank-recomp` already runs the bank-unit half and fails on it)
make cross-bank-check

# build the app
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
```

The executable is written to `build-app/ogrebattle64`.

### Build variants

| Variant | Configure command | Renderer |
|---|---|---|
| Native + RT64 (default) | `cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release` | RT64 (Vulkan/Metal/D3D12) |
| Native + null renderer | `cmake -S app -B build-null -DCMAKE_BUILD_TYPE=Release -DOGRE_USE_RT64=OFF` | `null_renderer.cpp` (no GPU, no RT64) |
| WebAssembly (Emscripten) | `emcmake cmake -S app -B build-wasm -DCMAKE_BUILD_TYPE=Release` | null renderer (no RT64) |

The null-renderer variant is useful for bring-up/CI on machines without a
working GPU driver; the Emscripten variant is the browser port
(`docs/WEB-PORT.md`) — it needs the Emscripten SDK (`brew install emscripten`),
is built with `cmake --build build-wasm -j`, and is served per
`docs/WEB-PORT-DEPLOYMENT.md` (cross-origin isolation headers required for
pthreads).

## Running

```sh
# from the repo root (the app resolves the ROM path from the config path)
./build-app/ogrebattle64 [path-to-rom.z64]
```

On first run the app:
1. creates the config directory (per-platform user config dir, subfolder
   `ogrebattle64`),
2. validates and stores your ROM by XXH3 hash,
3. boots the recompiled game via `recomp_entrypoint`.

If no ROM path is given, the app looks for the stored ROM in the config dir.

## Scripted runs and diagnostics

Environment variables make a run self-driving and bounded, for measurements and
captures without a human at the keyboard (see `docs/DECISIONS.md`, sessions 25,
26 and 27). All default to off.

| Variable | Effect |
|---|---|
| `OGRE_TAP_MS=<n>` | controller 0 presses Start for 150 ms out of every `n` ms (the native equivalent of the web probes' Enter tap) |
| `OGRE_TAP_SCENE=<list>` | press `OGRE_TAP_MS`/`OGRE_TAP_BUTTON` only while the dispatcher's scene (`D_800E810E`) is in the list — names from `kScenes` (`title`, `new-game`, …) or hex, `,`/`+`-separated. Scopes a scripted tap to **game state instead of wall time**, so `OGRE_SPEED` and capture readback no longer break the schedule |
| `OGRE_TAP_NOT_SCENE=<list>` | the complement of the above. `OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game` presses Start through the title and goes silent the moment New Game is confirmed — no `OGRE_TAP_MAX` tuning |
| `OGRE_EXIT_AFTER_MS=<n>` | after `n` ms the app prints the last recompiled function *and the live call chain* of every game thread, then exits 0 (a scripted run does not unwind on purpose: the graceful path tears threads down mid-call and segfaults intermittently) |
| `OGRE_DEBUG_TRACES=1` | the runtime's `[ev]`/`[mq]`/`[sch]`/`[vi-debug]` traces, including `[pi] inline DMA` for every streamed-overlay load (very chatty) |
| `OGRE_DEBUG_VI=1` | with `OGRE_DEBUG_TRACES`, log every `osViSetMode` (mode pointer + decoded geometry) and every VI geometry change (`[vi-debug]`) |
| `OGRE_DL_ANALYZE=1` | per-submission F3DEX2 workload counts (commands, triangles, texrects, textures) |
| `OGRE_DL_DECODE=<n>\|all` | dump the decoded command stream of display list `<n>` (or all): `SETTIMG`/`SETTILE`/`SETTILESIZE`/`LOAD*`/`TEXRECT` with its `RDPHALF` halves/`FILLRECT`/`SETSCISSOR`/combiner/othermode. The ground truth for "which RDP command draws this" |
| `OGRE_DUMP_RDRAM=<path>` | with `OGRE_EXIT_AFTER_MS`, write the whole 8 MiB RDRAM image for offline analysis |
| `OGRE_CHAIN_HISTORY=<tid>` | record and print the ordered sequence of live call chains thread `<tid>` goes through |
| `OGRE_SYNTH_FRAME=1` | submit a synthetic F3DEX2 display list (seven colour bars) through the normal task path, as a renderer-path probe. Also turns on the two presenter diagnostics below |
| `OGRE_SYNTH_AT_MS=<n>` / `OGRE_SYNTH_PERIOD=<n>` | when the probe first submits, and how many VIs between re-submits (default 30; 0 = every VI). Keep `AT_MS` above ~600 ms: before that the game's ucode is not in RDRAM yet and RT64 cannot identify the GBI |
| `OGRE_NO_DUMMY_VI=1` | skip the pre-start dummy VI workload (it clears the screen black every VI and would hide a diagnostic draw) |
| `OGRE_SYNTH_RAW=1` | also write the bars straight into the VI framebuffer in RDRAM, bypassing the RDP (isolates the presenter from the display list) |
| `OGRE_SYNTH_ANIMATE=1` | rotate the raw palette every VI so RT64's change-driven presenter keeps presenting |
| `OGRE_SYNTH_NO_DL=1` | with `OGRE_SYNTH_RAW`, do not submit the display list (RAM path only) |
| `OGRE_SYNTH_FORCE_VI=0` | do not pin the VI to the probe's framebuffer (the pin is on by default; without it the probe is timing-dependent, because the game may have repointed the VI at one of its own framebuffers) |
| `OGRE_CAPTURE_PRESENT=<path>` | at present time, GPU-read back the exact swap-chain texture and write `<path>.<n>.ppm` (see "Capturing the native output") |
| `OGRE_CAPTURE_TARGET=<path>` | same, for the render target the VI renderer sampled (`<path>.<n>.bin`, `R16G16B16A16_UNORM`, 8 bytes/texel) |
| `OGRE_CAPTURE_AFTER=<n>` | skip the first `n` presents before capturing |
| `OGRE_CAPTURE_EVERY=<n>` | with `OGRE_CAPTURE_PRESENT`, capture only every `n`th present — a multi-minute run becomes a slideshow instead of one 3 MB PPM per frame (files stay numbered by present index) |
| `OGRE_SPEED=<n>` | scale the emulated clock (CPU counter **and** VI retrace schedule) by `n` (1..64), so timed sequences — the attract loop, songs — complete in `1/n` of the wall time. Semantics are unchanged: every timer scales together (audio is off in these runs). `OGRE_SPEED=8` reaches the attract loop's second variant in ~42 s instead of ~360 s |
| `OGRE_FORCE_SCENE=<hex>` | switch a run to attract scene `<hex>` by poking the scene id (`*(u16*)(D_800C4BBC+4)`) until `D_800E810E` reports it active — no need to wait out the attract loop. `OGRE_FORCE_SCENE_AFTER_MS` (default 3000) delays the first poke so boot can settle |
| `OGRE_PRESENT_ALWAYS=1` | push a present on every VI even when nothing changed (a stalled boot changes nothing, so the window otherwise freezes on an old frame) |
| `OGRE_PRESENT_FBTARGET=1` | if the framebuffer manager has no framebuffer at the VI address, present a non-empty render target there instead of the RDRAM copy |
| `OGRE_INSTANT_PRESENT=1` | switch RT64 to `PresentEarly` (a display list presents the framebuffer it drew) |
| `OGRE_PRESENT_TRACE=1` | `[present]`/`[state]` traces: what each present carries and which path it took |
| `OGRE_RDP_TRACE=1` | `[rdp]` traces: `setColorImage`, `fillRect`, `drawRect` (with the scissor/empty checks) |
| `OGRE_WORKLOAD_TRACE=1` | `[workload]` traces: the framebuffer pair RT64 built (colour image, scissor, call count) |
| `OGRE_FBRENDER_TRACE=1` | `[fbrender]` traces: the renderer's per-call view (cycle type, fill colour, rect) |
| `OGRE_VI_TRACE=1` | `[vi]` trace of the decoded VI RT64 is about to present |
| `OGRE_SCHED_TRACE=1` | enable the scheduler traces and, at exit, dump the race-free scheduler event ring (`[sched-ring]`): every insert/pop/remove/resume/park/wake/swap with a global sequence number and the running queue after it. This is how a thread that yielded and was never resumed is read off a run |
| `OGRE_SCHED_TRACE_TID=<tid>` | with `OGRE_SCHED_TRACE`, filter the ring dump to events involving thread `<tid>` |
| `OGRE_NOP_RECT=<selectors>` | app-side bisect aid: walk the display list before RT64 parses it and replace matching `G_TEXRECT`s (and their `RDPHALF_1`/`RDPHALF_2` pair) with `G_SPNOOP`, so a suspected draw disappears and the layers under it show. Selectors: `any`, `tex:<hex>` (the current `SETTIMG` address), `flip` (`dsdx < 0`), `x:<a>-<b>`, `y:<a>-<b>`, `n:<index>` |
| `OGRE_FOG=0` | disable the session-35 repair that rewrites OB64's fog/cloud "wrap" rectangles to sample the layer's own image (found through the game's layer table at `D_801B80E0`). On by default. The repair was a no-op until session 36 (its `G_SETTILESIZE`/`G_LOADTILE` extents were in texels, not the GBI's quarter-texels); it now draws the title's logo-band fog and the `©1999 QUEST` sweep (`docs/HANDOFF-2026-09-14-session36.md` §3) |
| `OGRE_FOG_TRACE=1` | report each repaired fog rectangle group (layer, image address, dimensions and the rectangle's `s`/`t`) |
| `OGRE_FOG_SCALE=<percent>` | scale the fog image's intensity when it is staged for the fog group (**default 100 = the game's own asset**, which is the right level: the combiner is `RGB=ONE, ALPHA=TEXEL0`, so the overlay is white modulated by the layer image's intensity, mean 5.1% / peak 20.4%). Lower values stage a scaled copy in the last 64 KiB of RDRAM, which OB64 leaves untouched (the first use reports it if that area is not zero); `45` gives a softer, non-retail look |
| `OGRE_FOG_SUMMARY=1` | one `[fog-summary]` line per display list (groups repaired, layer/image used) plus one `[fog-group]` line per white group (`s`, `t`, the inherited tile, layer, image installed). This is how the `©1999 QUEST` blink was traced |
| `OGRE_NJPEG=0` | force the **stub** for the Nintendo-JPEG decoder (`M_NJPEGTASK`, type 4: the New Game step ≥ 2 background decode). On by default since session 47, when the recompiled microcode was fixed (`text_address` is a label base — see `docs/guides/rsp-microcode.md`); it costs 0–1 ms per image |
| `OGRE_NJPEG_TRACE=1` | `[njpeg]` lines: the colour image the YUV macroblock draw landed in (the buffer the readback should copy — see `tools/njpeg_readback.py` and `docs/HANDOFF-2026-09-15-session48.md`) |
| `OGRE_YUV_TRACE=1` | `[yuv]` one line per `G_SETTIMG` with `fmt=YUV` (the njpeg macroblock texture image): confirms the `0x800A5110` draws reach RT64 at all |
| `OGRE_NJ_WAIT_MS=<n>` | how long the **RSP worker** waits after a display list for the game's njpeg framebuffers (`0x400`/`0x25C00`/`0x4B400`) to appear in RT64's framebuffer manager (default `500`; `0` disables). The game reads one of them back with the CPU right after its own RSP wait, and the port completes the emulated task as soon as RT64 has the list — without this the copy runs before the draw renders. Progress: `OGRE_SYNC_TRACE=1` prints `[njwait]` and `[syncfb]` |
| `OGRE_SYNC_TRACE=1` | `[njwait]` (the RSP-worker render wait: spins, found, ms, queue/workload ids), `[syncfb]` (the framebuffer manager at the game's CPU readback) and `[njpair]` (the framebuffer pairs RT64 builds for `0x400`/`0x25C00`/`0x4B400`) |
| `OGRE_S2D_TRACE=1` | `[s2d]` the first 40 S2DEX 2D-object commands (`G_OBJ_RECTANGLE`/`_R`/`G_OBJ_SPRITE`), with the decoded `uObjSprite` (fmt/siz/image size/stride/TMEM address), the matrix-derived destination rectangle, the target colour image and the current texture image address. This is the diagnostic that showed Ogre Battle 64's 300 macroblock rectangles tiling its 320x240 njpeg framebuffer (session 51) — without the S2DEX2 object commands implemented, the same list looks like "texture loads only" |
| `OGRE_CONVERT_TRACE=1` | `[convert]` the raw 9-bit `G_SETCONVERT` fields the game programs (`k0..k5`). The YUV16 decode sign-extends and scales them `2*K+1` and converts with `R = Y+K0*V'`, `G = Y+K1*U'+K2*V'`, `B = Y+K3*U'` (`U'=U-128`, `V'=V-128`) — the game sets `k0=175 k1=469 k2=423 k3=222 k4=114 k5=42`, i.e. coefficients `351/−85/−177/445` (session 52; `k4`/`k5` are the combiner's `K4`/`K5`, used raw over 255) |
| `OGRE_FB_WRITEBACK=1` | `[fbpair]` (which framebuffer pairs RT64 renders, with their rows) and `[fbwrite]` (the CPU-visible RDP→RDRAM writeback, with the first pixels). This is how "does a rendered framebuffer reach RDRAM for a CPU read" is answered |
| `OGRE_RDP_TRACE=2` | instead of the first-20 `[rdp]` lines, a `[cimg]` histogram of every colour-image address plus a `[cimgseq]` ordered list of the game's framebuffer targets — the ground truth for "which buffers does the game render into" |
| `OGRE_FOG=alternate` | apply the repair on every other display list. On screens the game redraws every list this A/Bs the fog; it cannot say *which* screen a pair is on (session 35's isolation image was the story screen), and on the static title it shows nothing because no display list changes |
| `OGRE_SCENE=<name\|hex>` | boot straight into a screen: `title` (0x04: the attract title — logo plus the New Game / Tutorial / Stereo menu over the clouds), `intro` (0x09), `publishers` (0x0A), `story` (0x0B), `unit-info` (0x0C), `tutorial` (0x17: Deneb's "Is this your first time here?" dialogue, the title's Tutorial entry), `menu` (0x18), `new-game` (0x02), or any hex id (`OGRE_FORCE_SCENE` still works). The poke is only seen **before the boot enters its first scene (~1.1 s)**, so it runs from the streamed-DMA hook and a per-frame retry and releases once `D_800E810E` reports the target. `menu`/`new-game` still SIGSEGV (unfinished cross-bank work) |
| `OGRE_STEP=<n>` | **seed** the New Game opening's **step** at `n` for the selected scene — the scene-script step lives in `D_8018F1C0` and scene `0x0D` branches on it, so the movie (step 1, ~28.7 s) can be skipped. `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 ./build-app/ogrebattle64` lands in the cathedral ~2.2 s after boot (step table: `docs/HANDOFF-2026-09-15-session44.md` §1, what each step shows: `docs/scenes.md`). The seed is written **once** on the first frame the target scene is active, then released as soon as the live step moves off it (or after a 1500 ms window). It is *not* held every frame: the VM writes the step itself when the sequence advances, and a per-frame hold overwrites that advance, so the opening loops on the seeded step forever (session 53) |
| `OGRE_SCENE_AFTER_MS=<n>` | delay before the first scene poke (**default 0**). A delay past ~1500 ms makes the jump a silent no-op: the running scene re-establishes the state block every frame |
| `OGRE_PROBE55=1` | `[probe55]` the New Game **sequence state** on every change: the scene, the scene-script step word `D_8018F1C0`, the next-scene word `D_8018F1C2`, and session 43's script-buffer words `0x80197B23`/`0x80197B38`/`0x80197B3C`. Session 53 used it to show the step stuck at 2 through the cathedral and then 0 (never 3), with the buffer words reading 0 the whole run |
| `OGRE_SCENE_LOG=1` | log every scene change with its descriptor and record mask — how the scene ids above were identified |
| `OGRE_SCENE_TRACE=1` | the forcing state every 500 ms (`active`, `pending(D_800E8214)`, the state block and its id, and whether the target is active) — use this when a jump "does nothing" |
| `OGRE_EMPTY_TILE=draw` | restore the pre-session-35 behaviour of drawing rectangles whose tile covers zero texels (`lrs == uls` or `lrt == ult`) instead of skipping them |
| `OGRE_EMPTY_TILE_TRACE=1` | name every rectangle skipped because its tile covers no texels |
| `OGRE_RECT_STATE=<y0>-<y1>` | per-rectangle render state for rectangles contained in rows `[y0,y1)`: cycle type, both combiner cycles (decoded by RT64's own `ColorCombiner::cycleColorText`/`cycleAlphaText`), the blender inputs, the primitive colour and the tile descriptor it samples |
| `OGRE_TILE_TRACE=1` | every tile whose sampling rectangle is degenerate (`sampleHeight <= 1`), with the texture the cache returned (`hash`, index, dimensions, `tcScale`, `rawTMEM`) |
| `OGRE_DUMP_TEX=<dir>` | write RT64's decoded-texture dump (a 4 KiB `.tmem` plus `.tile.json` per texture) into `<dir>` instead of opening its file dialog |

```sh
# a bounded run with taps, and the per-thread call chains
OGRE_TAP_MS=5000 OGRE_EXIT_AFTER_MS=60000 ./build-app/ogrebattle64 > /tmp/ogre.out 2>&1
grep -A 12 "per-thread last" /tmp/ogre.out

# where the boot thread actually goes (the session-26 measurement)
OGRE_CHAIN_HISTORY=3 OGRE_EXIT_AFTER_MS=3000 ./build-app/ogrebattle64 2>&1 | \
  sed -n '/chainhistory/,/callchain t1/p'

# renderer-path probe: build and submit a real display list while the boot is stuck
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=1000 OGRE_SYNTH_PERIOD=30 \
  OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64
```

### Jumping straight into a screen

The attract loop takes minutes at 1×. `OGRE_SCENE` enters a screen at the next
scene load instead, and `OGRE_SPEED` compresses whatever waits inside it:

```sh
# boot into the attract title (logo + PRESS START over the clouds)
OGRE_SCENE=title ./build-app/ogrebattle64

# a bounded title-screen capture: the title arrives ~17 s in at 4× and lasts
# ~10 s, so capture a window across it
OGRE_SCENE=title OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/title \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=26000 \
  ./build-app/ogrebattle64

# if a jump "does nothing", this says whether the poke landed
OGRE_SCENE=title OGRE_SCENE_TRACE=1 OGRE_EXIT_AFTER_MS=6000 ./build-app/ogrebattle64
```

The poke only counts before the boot enters its first scene (~1.1 s), so leave
`OGRE_SCENE_AFTER_MS` at its 0 default. See
`docs/HANDOFF-2026-09-14-session36.md` for the dispatcher path and the verified
ids (`menu` 0x18 and `new-game` 0x02 still crash: unfinished cross-bank work).

### Capturing the native output

Do **not** use `screencapture` to decide whether the renderer works. On a locked
or asleep display it returns the last frame the window server committed, which
can be seconds old (a cycling clear colour stays one colour across many
captures), and the session-26 "the window is black" conclusion came from exactly
that. Measure the presented texture instead:

```sh
# one command: submit the probe every 30 VIs and read back what is presented
OGRE_SYNTH_FRAME=1 OGRE_SYNTH_AT_MS=900 OGRE_SYNTH_PERIOD=30 OGRE_NO_DUMMY_VI=1 \
  OGRE_CAPTURE_PRESENT=/tmp/frame OGRE_CAPTURE_AFTER=200 \
  OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64
# -> /tmp/frame.201.ppm, /tmp/frame.202.ppm, ... (P6; the swap chain is BGRA8)
```

`OGRE_CAPTURE_PRESENT` is the ground truth for "what does RT64 present": it is
the swap-chain texture itself, with no window server in the path. Add
`OGRE_CAPTURE_TARGET=/tmp/target` to also dump the render target the VI renderer
sampled, which separates "the RDP did not draw" from "the presenter dropped it".

`OGRE_SYNTH_FRAME` turns on `OGRE_PRESENT_ALWAYS` and `OGRE_PRESENT_FBTARGET` for
you (with `setenv(..., overwrite=0)`, so an explicit value still wins); with
`OGRE_SYNTH_NO_DL` it turns on only `OGRE_PRESENT_ALWAYS`, because the
render-target fallback would bypass the RAM upload path the raw probe is testing.
The probe also pins the VI to its own framebuffer every VI (`osViSwapBuffer`), so
the presented frame is the probe's and not whichever of the game's framebuffers
the VI happened to point at; `OGRE_SYNTH_FORCE_VI=0` turns that off.

Without `OGRE_PRESENT_ALWAYS` a stalled boot never re-presents and the window
keeps an old black frame even though the render path works, and without
`OGRE_PRESENT_FBTARGET` an RDP-only fill has no framebuffer in the manager so the
presenter uploads empty RDRAM instead of the drawn target.

### The game's own frames

The game drives its own double-buffered VI, so none of the probe knobs are
needed to see it - only the capture. This is the end-to-end check that the port
renders the game (it should show the title scene's ring of soldiers):

```sh
# title scene, captured straight off the presented swap chain
OGRE_TAP_MS=4000 OGRE_CAPTURE_PRESENT=/tmp/game OGRE_CAPTURE_AFTER=30 \
  OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64
# -> /tmp/game.31.ppm ... : the twelve soldiers of the title screen
```

`docs/proofs/native-game-title.png` and `native-game-title-still.png` are captures
of that run. A healthy run submits the game's real display lists
(`data=0x801C1520` / `0x801C80A0` alternating, ~2.5/s) rather than only the boot
blanking list at `0x800C6500`.

### Reading a dump

The runtime byte-reverses RDRAM (see `recomp.h`): the 32-bit word at game address
`a` is a **little-endian** word at file offset `a - 0x80000000`, and the logical
byte at `a` is at `(a ^ 3) - 0x80000000`. So a Python read is
`struct.unpack_from('<I', data, a & 0x1FFFFFFF)[0]` for words and
`data[(a & 0x1FFFFFFF) ^ 3]` for logical bytes.

`tools/rdram.py` does all of that for you (`word`/`half`/`byte`/`string`/
`hexdump`/`find`/`ptr`), and one mode answers the question mis-binding walls turn
on:

```sh
tools/rdram.py /tmp/rdram.bin banks        # which module is resident in each window
```

It compares the dump against every record in `app/src/bank_funcs.inc` (+ the
uncompiled segment-table records) and says which ROM offset is *actually* live at
each streamed RAM base. Streamed RAM holds different modules at different times
(`0x802395E0` is `bankRec14b` for the movie visit and `bankRec14c` for the steps
after it), so "the port called the wrong function" usually shows up here as
"the bytes at the faulting address are a different module's".

The black canvas a stalled boot produces is the game's idle trajectory, not a
renderer failure - but do not conclude that from a screen capture; see
"Capturing the native output".

## Diagnostics toolkit

Four offline tools answer the questions every session otherwise re-derives by
hand. None of them needs a CUDA/GPU/game run except where noted.

### `tools/runlog.py <run.log>` — one screen per run

```sh
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_SCENE_LOG=1 \
  OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64 > /tmp/run.log 2>&1
tools/runlog.py /tmp/run.log              # scene timeline, RSP tasks, bank loads, problems
tools/runlog.py /tmp/run.log --check      # exit 1 on crash / stub call / unknown module
tools/runlog.py /tmp/run.log --json       # machine-readable
```

The **non-gfx RSP task table** is the part that matters: graphics tasks go to
RT64 regardless, but a non-gfx task served by `stub microcode` is work the port
silently drops. Session 46's four `M_NJPEGTASK` background-decode tasks were
invisible inside 23k lines of gfx chatter and this prints them with their ucode,
data and `data_ptr` on one screen. `--check` is the assertion a future
`make smoke` should use.

### `tools/rdram.py ... image` — is the buffer blank or an image?

```sh
tools/rdram.py /tmp/rdram.bin image 0x80243E28 --width 320 -o /tmp/bg.png
tools/rdram.py /tmp/rdram.bin image 0x80243E28 --fmt ia8 --width 320
tools/rdram.py /tmp/rdram.bin image 0x800D0000 --offset 0x18 --scale 3
```

Renders a region as an N64 texture (`rgba16`, `rgba32`, `ia16`, `ia8`, `ia4`,
`i8`, `i4`, `ci8` + `--palette`) to an RGBA PNG (zlib only, no Pillow) and
prints a luminance summary with a **`(near-)uniform` note** — the difference
between "nobody drew this" and "an asset failed to decode".

### `tools/rdram.py diff A.bin B.bin` — what did a run change, by module?

```sh
tools/rdram.py diff /tmp/stub.bin /tmp/with-fix.bin --min-run 16 --limit 20
```

Clusters changed bytes into runs, merges runs within `--gap`, and prints the
running total **grouped by owning record/overlay** (from `tools/n64map.py`),
flagging RAM that several records share. This replaces "add a probe to `rsp.cpp`
that dumps a region before and after a call".

### `tools/guestmap.py <addr>` — which module is this?

```sh
tools/guestmap.py 0x801B7EBC 0x802395E0 0x2F180    # vram or rom, auto-detected
tools/guestmap.py 0x801B7EBC --context 4           # +-symbols
tools/guestmap.py --list                           # the whole segment/record table
```

Prints the ROM offset (`vram - 0x80070C60 + 0x1060` and back), the owning
segment/record and unit, whether the address is a **function entry in the ELF
that owns that layout**, and — the mis-binding warning — the other records that
map the same RAM. Example: `0x801B7EBC` is `func_801B7B9C + 0x320` (not an
entry) in `ogrebattle64.elf`, but `func_ovlC_801B7EBC` (an entry) in `bankC.elf`
and `func_ovlG_801B7EBC` in `bankG.elf`; which one is live is decided by the
game's DMA.

### `tools/watch.sh <guest-addr>` — who writes this address?

```sh
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  tools/watch.sh 0x80243E28 --after func_80178920 --ignore 1 --hits 3
```

Arms an lldb watchpoint on `rdram + (guest - 0x80000000)` after a given guest
symbol (skip its first `--ignore` hits to pick a later visit), and prints a
backtrace per write.

`--value <n>` makes it conditional: it keeps running until the watched word
holds `<n>`, which is how a *specific* write is caught on an address that is
written many times with different values (session 48 used
`tools/watch.sh 0x800C4BB8 --value 0x80000400 --hits 2 --size 4` to catch the VI
manager writing a placeholder into the display word, with the caller chain
`func_8007307C ← func_80089540`).

The app, the recompiled code and `librecomp`/`ultramodern` are built with
`-fno-omit-frame-pointer` (RT64 is not), because the runtime's shadow *guest*
call chain is empty when a fault happens inside runtime code reached through a
bridge. With frame pointers a breakpoint in the runtime unwinds across the
bridge to the guest caller:

```
frame #0  do_recv            frame #2  osRecvMesg_recomp
frame #1  osRecvMesg         frame #3  func_80088F08 + 717   <- the guest caller
```

(`bt` at a *signal* stop can still print only the faulting frame — if the crash
is a `SIGFPE`/`SIGBUS` inside the runtime, break on that function instead:
`lldb -b -o "breakpoint set -n do_recv" -o run -o "bt 14" -- ./build-null/ogrebattle64`.)

## Config directory

- macOS: `~/Library/Application Support/ogrebattle64/`
- Linux: `~/.config/ogrebattle64/`
- Windows: `%APPDATA%\ogrebattle64\`

## Troubleshooting

### RT64 submodules won't check out ("Unable to find current revision")

RT64 pins contrib submodules (imgui, xxHash) to commits that aren't always
fetchable by plain `git submodule update`. Fix by fetching the pinned commit
explicitly:

```sh
git -C tools/RT64/src/contrib/imgui fetch origin <pinned-sha>
git -C tools/RT64/src/contrib/imgui checkout <pinned-sha>
```

### DXC shader compiler aborts ("Library not loaded: libz.dylib")

The bundled `dxc-macos` needs `libz.dylib` next to it (its `DYLD_LIBRARY_PATH`
includes `tools/RT64/src/contrib/dxc/lib/x64`). Install zlib and symlink it in:

```sh
brew install zlib
ln -sf "$(brew --prefix zlib)/lib/libz.dylib" tools/RT64/src/contrib/dxc/lib/x64/libz.dylib
```

### `metal` tool missing ("missing Metal Toolchain")

RT64's macOS shader build calls `xcrun -sdk macosx metal`. Newer Xcode releases
require the separate Metal toolchain component:

```sh
xcodebuild -downloadComponent MetalToolchain
```

### RT64 build still fails after changes

The RT64 CMake cache is sticky; after touching `tools/RT64/CMakeLists.txt` or
fixing toolchain issues, delete the `build-app` directory and reconfigure.

