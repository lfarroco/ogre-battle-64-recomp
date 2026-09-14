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

# streamed-overlay bank unit (Phase 4): a second recompilation for the overlay
# records the game loads into overlay C's RAM from a different bank. Required
# before configuring the app — it writes BankFuncs/ and app/src/bank_funcs.inc,
# and the app links both.
make bank-recomp

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
| `OGRE_FOG=0` | disable the session-35 repair that makes OB64's fog/cloud "wrap" rectangles sample the layer's own image (found through the game's layer table at `D_801B80E0`). On by default; without it those rectangles inherit a zero-texel tile and are skipped |
| `OGRE_FOG_TRACE=1` | report each repaired fog rectangle group (layer, image address, dimensions and the rectangle's `s`/`t`) |
| `OGRE_FOG_SCALE=<percent>` | scale the fog image's intensity when it is staged for the fog group (default 100 = the raw asset). The copy lives in the last 64 KiB of RDRAM, which OB64 leaves untouched; the first use reports it if that area is not zero |
| `OGRE_FOG=alternate` | apply the repair on every other display list, so two consecutive presents differ by exactly what the fog draws. This is how the fog's contribution was measured: mean band difference 3.5 with max 17 out of 255, against 0.75 in a control region (`docs/proofs/native-title-fog-isolation.png`) |
| `OGRE_SCENE=<name\|hex>` | boot straight into a screen: `title` (0x0C), `menu` (0x18), `new-game` (0x02), or any hex scene id (`OGRE_FORCE_SCENE` still works). The poke runs on the streamed-DMA path, so the switch lands at the next scene load |
| `OGRE_SCENE_AFTER_MS=<n>` | delay before the first scene poke (default 3000; poking earlier crashes the boot) |
| `OGRE_SCENE_LOG=1` | log every scene change with its descriptor and record mask — how the scene ids above were identified |
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

The black canvas a stalled boot produces is the game's idle trajectory, not a
renderer failure - but do not conclude that from a screen capture; see
"Capturing the native output".

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

