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
# before configuring the app — it writes Bank{A..M}Funcs/ and
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
| `OGRE_TAP_BUTTON=<list>` | which pad buttons the synthetic taps press, cycled one per tap (`start`, `a`, `b`, `down`, …; `none` skips a tap). **The list shape matters, and so does the form (developer, session 64): a tap that lands while the cursor is in a text field opens a *tooltip*, and the tooltip blocks the sequence from advancing.** The name-entry form (scene `0x07`) is where this bites. The recorded working shape is **a few `start,a` pairs then a long run of plain `a`** — e.g. `"start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a"` (see the checkpoint and `OGRE_NJREAD_LOG` recipes below). An *endless* `start,a` alternation stalls there forever: a scripted run that shows no crash, no stub call, and a final scene of `0x07` is almost always this. If a schedule keeps stalling, drive the run by hand and take a checkpoint instead |
| `OGRE_TAP_SCENE_BUTTON=<scene>:<button>[:<count>],…` | the **scene-keyed** sibling of `OGRE_TAP_BUTTON` (session 68). `OGRE_TAP_BUTTON`'s slot index advances with **wall time**, so a route across several scenes has to be hand-tuned to how long each takes — the same "title → Load Game → map" schedule landed on the map in one run and in the attract loop in the next, because the boot reaches the title anywhere between 1.6 s and 15 s (the forced-scene poke races the publisher stills). This one keys the button on the dispatcher's active scene (`D_800E810E`; names from `kScenes` or hex) and presses it every `OGRE_TAP_MS` while that scene runs, at most `<count>` times (default unlimited). Entries in one scene are consumed in order, so `OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3,0x05:right:3,0x05:a:1"` is "Start up to five times on the title (early presses are sometimes swallowed; the scene change stops it), A three times on the Load Game screen, then walk the map cursor right three times and press A". Requires `OGRE_TAP_MS`; it replaces the slot schedule entirely |
| `OGRE_EXIT_AFTER_MS=<n>` | after `n` ms the app prints the last recompiled function *and the live call chain* of every game thread, then exits 0 (a scripted run does not unwind on purpose: the graceful path tears threads down mid-call and segfaults intermittently) |
| `OGRE_PREF_DIR=<dir>` | use `<dir>` instead of `SDL_GetPrefPath` as the runtime config dir, so a run keeps its `saves/` and `mods/` where you choose (e.g. inside the repo, or on a sandbox that cannot write `~/Library/Application Support`). Default is unchanged; the boot log prints the resolved path |
| `OGRE_SAVE=<name\|path>` | start the run from that save as the cartridge battery (session 78). A bare name is looked up as the path, then `<rom dir>/saves/<name>[.n64\|.bin]`, then `assets/saves/<name>[.n64\|.bin]`, so `OGRE_SAVE=prologue` finds `assets/saves/prologue.n64`. Accepts the port's 32 KiB image, an emulator wrapper of it (in either byte order, at any offset), a **DexDrive `.N64` Controller Pak dump** — converted through its notes' battery slots, checksums reseeded — or a bare pak. The result is written to `<config>/saves/<game id>.bin`, never back into the source |
| `OGRE_SAVE_RESET=1` | re-import even when the battery in the config dir is newer than the source. Without it a run **keeps** that battery, so progress the game wrote back survives a re-run, and swapping a save file in re-imports it (its mtime is newer) |
| `OGRE_SAVE_ALL=1` | with `OGRE_SAVE`, also take the stale records a deleted Controller Pak note leaves behind (still capped at the battery's two save slots) |
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
| `OGRE_NJPEG_TRACE=1` | `[njpeg]` lines: the colour image the YUV macroblock draw landed in. RT64 records it in the scratch word, but it is only *re*set by a YUV-texture-then-colour-image pair and never cleared, so it can be stale — the readback trusts the game's own `state[0x64]` instead (`tools/njpeg_readback.py`, `docs/HANDOFF-2026-09-16-session57.md` §1) |
| `OGRE_NJREAD_LOG=1` | `[njread]` one line per njpeg stage-3 pass (`func_ovlE_8019976C`): the pass number, scene/step, the framebuffer the game's `state[0x64]` selects, which rule the `njpeg_readback.py` patch applied (`game` or `scratch`), the scratch word (`cimg`/`njpeg`/`lastfb`), the display word `D_800C4BB8`, and the pass's destination/size. This is how the stale-source artifact was found and is the before/after check for it |
| `OGRE_YUV_TRACE=1` | `[yuv]` one line per `G_SETTIMG` with `fmt=YUV` (the njpeg macroblock texture image): confirms the `0x800A5110` draws reach RT64 at all |
| ~~`OGRE_NJ_WAIT_MS=<n>`~~ **removed (session 77)** | this used to make the **RSP worker** wait after *every* display list for the game's njpeg framebuffers (`0x400`/`0x25C00`/`0x4B400`) to appear in RT64's framebuffer manager, 500 ms timeout. Session 57 measured it as a no-op in the njpeg path (`[njwait] spins=1 found=1 ms=0`) but session 77 found its real cost: any scene that renders into neither of those three **fixed** addresses can never satisfy the condition, so the poll burned the whole 500 ms on every submitted list. The boot's **publisher stills (scene `0x0A`, the Nintendo/ATLUS/QUEST logos, which use a 640x480 image buffer) ran at 2 display lists/s instead of 30** — a visible ~16× slowdown. The readback never needed the wait (the game's own DP-completion wait plus `ogre_sync_framebuffers()` order the copy), so the call was deleted. `OGRE_SYNC_TRACE=1` no longer prints `[njwait]` lines, only `[syncfb]`. See `docs/HANDOFF-2026-09-18-session77.md` |
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
| `OGRE_PROBE57=1` | `[probe57]` the New Game **handoff state** on change: the scene, the step word `D_8018F1C0`, the next-scene word `D_8018F1C2`, the cutscene vtable pointer the scene-`0x0D` exit dispatches through (`0x801D0830`), the engine words `0x8023A994`/`0x8023A960` and `0x801DFC90`. Session 54 used it to show the natural opening advance `step 1 → 2` and the exit setting `D_8018F1C0 = 0` with `next = 0x0007` before scene `0x07` runs |
| `OGRE_DMA_TRACE=1` | at `OGRE_EXIT_AFTER_MS`, dump **every** streamed PI DMA the game issued, grouped by `(rom, ram)` base with a `first`/`last` event index and a chunk count. The `[bank]` line only covers records the port compiled, so this is how a module the game loads but the port has no record for is found (session 54: the scene-`0x07` black screen). Prints only bases in overlay C's window `0x80190000..0x801C0000`; `OGRE_DMA_TRACE_FULL=1` prints every base (≈17k in a 26 s run) |
| `OGRE_STEP` (corrected) | the `OGRE_STEP` seed is a **shortcut that changes the sequence's own state**, not just an entry point: entering `0x0D` at step 2 with no step-1 visit makes the exit take its `otherwise` arm and re-enter at step 0, which selects the movie-mode branch and crashes (session 53's wall). For a real New Game repro, drive the title instead: `OGRE_SCENE=title OGRE_TAP_MS=1000 OGRE_TAP_BUTTON="start,a,start,a,…"` (session 54) |
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
hand. None of them needs a CUDA/GPU/game run except where noted. The **live
console** below is the fifth, and the only one that queries a *running* game.

### `make elf-rom-check` — is the linked code the ROM's code? (session 65)

`tools/elfcheck.py` compares every `CONTENTS` section of every linked ELF
(`build/bank*.elf`, `build/ogrebattle64.elf`) with the ROM at the same ROM offset,
and `--syms` (what the target passes) also asserts that every symbol whose *name*
ends in eight hex digits is defined at that address — free, because the project
names labels `D_ovlM_801A6FD8` / `func_ovlM_801A2A7C`.

```
$ make elf-rom-check
bankM.elf: 0 differing bytes of 553504
bankM.elf: 479 address-named symbols all at their named address
ogrebattle64.elf: 0 differing bytes of 41943040
ogrebattle64.elf: 4224 address-named symbols all at their named address
```

Run it whenever a scene draws the wrong *size*, the wrong *slice* or the wrong
*string* from a table — before blaming the renderer or the game's data. Session 65
found unit M's whole `.data` subsegment 8 bytes high this way: `mips-linux-gnu-as`
pads `.text` to 16 bytes, unit M's code/data boundary is only 8-byte aligned, so
the data subsegment moved up 8 and **every** `lui/%lo` data reference in the
recompiled module read 8 bytes too high — which put the map's sprite table one
entry late and made the party, the shadow, the cursor and the date panel all draw
their neighbours' sizes for four sessions. A misalignment is fixed in the unit's
config (`align`/`subalign`, or by ending the `asm` subsegment on the 16-byte
boundary as unit M's now does), never in the generated `.s` or the generated C.
`make bank-recomp` runs the check before recompiling, so a misaligned link fails
the build instead of producing a scene that looks wrong.

### The live console — query a running game (session 56)

The offline tools can only look at a dump, and a bounded run can only dump at its
exit; by then the buffer that mattered has usually been overwritten (session 56
lost an afternoon to a framebuffer readback that had to be caught mid-frame).
The console executes commands inside the running game, on the **main thread**,
where RDRAM is available and a multi-megabyte write cannot race the game thread.

Two triggers, both landing in the same command set:

* **a watched command file** (default `/tmp/ogre-console.txt`, override with
  `OGRE_CONSOLE_FILE`): when the file exists its lines are executed and the file
  is **removed**, so an external tool can drive a live run:

  ```sh
  OGRE_EXIT_AFTER_MS=600000 ./build-app/ogrebattle64 assets/ogre64.z64 | tee /tmp/live.log
  # in another shell (or from an agent):
  printf 'c\nr 0x8018FDAC 8\nk 0x80243E00 8192\ndump /tmp/at-now.bin\n' > /tmp/ogre-console.txt
  ```

* **number keys `1`..`9`** run `OGRE_KEY_1`..`OGRE_KEY_9` (edge-triggered, one
  command per press), e.g.
  `OGRE_KEY_1='dump /tmp/here.bin' OGRE_KEY_2='c' ./build-app/ogrebattle64 …`.

Commands (all addresses are guest `0x80xxxxxx`; output goes to stdout prefixed
`[console]` and is flushed):

| command | what it does |
|---|---|
| `r <addr> [n]` | `n` words in the runtime's dump order (what `tools/rdram.py word` prints) |
| `rh` / `rb` / `rk` | halfwords / logical bytes (`addr ^ 3`) / **raw** host-order words |
| `d <addr> [len]` | hex + ASCII dump of a byte range |
| `f <value> [max]` | find a 32-bit word anywhere in RDRAM (guest + file offset) |
| `fb <hexbytes> [max]` | find a byte pattern |
| `s <addr> [len] [max]` | strings in a range |
| `k <addr> [len]` | fnv1a checksum of a range — the cheap before/after for A/B runs |
| `w <addr> <value>` | store a word (A/B experiments; it is a real write) |
| `c` | scene, pending scene, current descriptor + its record mask, step, next, spin |
| `dump [path]` | write the whole 8 MiB RDRAM image **at this instant**. A bare `dump` never overwrites: it writes `/tmp/ogre-rdram-NNNN.bin`, one new file per press, so the bound key can be hit as often as you like and every snapshot is kept |
| `save [path]` | write a **checkpoint** (whole RDRAM + the runtime's overlay state). A bare `save` writes `/tmp/ogre-checkpoint-NNNN.ckpt` and remembers it |
| `load [path]` | **restore** a checkpoint this build wrote — the machine rewinds to the instant of the save and the game re-runs from there. A bare `load` uses the most recent bare `save` |
| `press <buttons> [polls] [x] [y]` | **hold a synthetic pad press** for `polls` input polls and then release it, so an external tool can walk a menu interactively ("press right", look at the capture, "press a") instead of guessing a wall-clock tap schedule (session 68). `<buttons>` is the `+`-joined `OGRE_TAP_BUTTON` vocabulary (`start`, `a`, `up`, `left`, …), `polls` and the stick values are parsed as **hex** for the poll count and `atof` for `x`/`y`, so `press a 400` is 1024 polls and `press none 60 -1 0` is a full-left analog stick. **A short press is much shorter than it looks: the game polls input about once per VI retrace (measured, session 70: `min=1 max=2 mean=1.01` calls per retrace), so `polls` ≈ emulated frames — `press right 8` is invisible, `press right 2000` walks the map cursor across the world; budget a few hundred polls per cursor step** |
| `help` | the list |

`dump` is the one that fixes the "wrong moment" problem: run the game until the
screen is in the state you want, then drop the command file (or press the bound
key) and the image is written while that state is live. Pair it with
`tools/rdram.py <dump> image …` / `diff …` as usual.

`OGRE_CONSOLE_AT_MS=<n>` delays every watched-file read until `n` ms of wall
clock have elapsed, so a scripted run can leave the command file in place at
launch instead of racing it from a background writer.

**Write the watched file atomically** (`printf … > f.tmp && mv f.tmp f`). The app
polls the path, opens, reads and **removes** it; a plain `> f` truncates before
the write, so the reader can catch an empty file, delete it and the bytes land in
an unlinked inode — the command silently never runs (session 68 lost two rounds
to this).

**`OGRE_CONSOLE_ON_SCENE` / `OGRE_CONSOLE_ON_STEP` / `OGRE_CONSOLE_ON_CMD`** run
one console command on the first frame a chosen scene (and optionally that exact
sequence step) is active — the deterministic alternative to timing a command
against wall-clock taps:

```sh
OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_STEP=974 \
  OGRE_CONSOLE_ON_CMD='save /tmp/ck.ckpt' ./build-app/ogrebattle64 assets/ogre64.z64
# [scene] console trigger: scene 0x000D step 974 -> save /tmp/ck.ckpt
```

`OGRE_CONSOLE_ON_SCENE` takes a name from the scene table (`new-game`, `tutorial`,
`title`, …) or a hex id; `OGRE_CONSOLE_ON_STEP` is the value of the sequence step
word `D_8018F1C0` (`docs/scenes.md` lists the opening's steps). It fires once per
process. This exists because every tap route in this project is wall-clock while
the game's own progress is not: a schedule tuned for a 2 s title lands in the
attract loop when the title takes 9 s, and the trigger removes that failure mode
(session 58).

### Checkpoints — `save` / `load` (session 58)

A checkpoint is a **full machine rewind**, so a long scripted path only has to be
played once: reach the scene you are working on, `save`, then `load` it instead
of replaying the New Game opening every run (the opening is ~45 s at 4x before
the first form and ~60 s to the closing movie).

```sh
# leave `c\nsave /tmp/ck.ckpt\nc\n` in the watched file before launch:
printf 'c\nsave /tmp/ck.ckpt\nc\n' > /tmp/ogre-console.txt
OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1500 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a,a" \
  OGRE_CONSOLE_AT_MS=30000 OGRE_EXIT_AFTER_MS=600000 \
  ./build-app/ogrebattle64 assets/ogre64.z64 | tee /tmp/live.log
# later, from another shell:
printf 'load /tmp/ck.ckpt\nc\n' > /tmp/ogre-console.txt
```

The file is `[8-byte magic][u32 version][u32 rdram_size][u32 host_size][u32
flags][u64 fnv1a][overlay-state blob][whole RDRAM image]` (8 MiB + a few KiB).
`load` verifies magic/version/sizes/checksum and refuses a file from another
build; a bare `dump` is still the plain image `tools/rdram.py` reads.

Why it is not just "write RDRAM to a file":

* **The function map is host state.** Which recompiled body runs at each RAM
  address is the runtime's `func_map`, not RDRAM. Restoring RDRAM alone would
  rewind the game's data but leave the map on a later bank, so the restored
  scene would run the wrong module's bodies at those addresses (the
  session-45/55 mis-binding class). The blob carries the loaded section and
  function-bank records and rebuilds the map from them
  (`recomp::overlays::get_overlay_state_blob` / `restore_overlay_state_blob`).
* **The game runs on its own host threads**, so reading or writing 8 MiB from
  the console's main thread raced the game thread that was mid-frame. Session
  58's first attempts produced **torn images**: the visible symptoms were a
  checkpoint whose own checksum did not match its bytes and whose first `0x300`
  bytes were zeros while the game kept submitting RSP tasks and building frames
  during the write. `save`/`load` now wrap the file I/O in
  `ultramodern::checkpoint_pause_begin()` / `checkpoint_pause_end()`, which
  parks every thread executing recompiled code at a function-entry boundary
  (`recomp_trace_entry`, the hook the recompiler emits at every function start)
  for the duration, then releases it. The console reports how many threads
  parked.

Verified (session 58): a checkpoint's stored checksum matches its own bytes; a
`load` 20 s later puts scene/step back to the saved values and the game
**continues from there** (a checkpoint taken at the personality questions
replays forward into scene `0x16`, the closing movie); a save/load pair in the
*same* run reproduces the rewind.

Verified again, and bounded, at the map (session 65): a `save` + `load` pair in
the **same run**, taken on the live map (scene `0x05`, a busy screen with every
N64 thread active), restores and continues with no crash. Loading the *same
bytes* written by an **earlier process** (the developer's run, same binary)
crashes deterministically and immediately in `do_send` (`SIGSEGV` at
`do_send + 0x54C`, on an N64 thread, with a garbage host address) even though the
load reports success and `c` shows the restored scene. Reason: the image holds
**host pointers** — `OSThread::context` is "an actual pointer regardless of
platform" and is written into guest RDRAM, one per N64 thread (~10 in a map
image, values in the `0x00007F…` range in the file). Same process ⇒ still valid;
another process ⇒ a stale address the scheduler dereferences. So a checkpoint
is **not transferable between runs** yet — a session cannot hand one to the next
one, and a `load` recipe must `save` and `load` in the same run. Making it
transferable means rebuilding the runtime's host state from the restored guest
state after the rewind (rebind a per-thread context registry, and reset the
scheduler/blocked-thread host state), not just the RDRAM image.

Limits: valid only in the process that wrote it (function pointers are
process-local) and for the same binary — the file carries an FNV-1a hash of the
running executable (`version 2`), so a **rebuild invalidates every checkpoint**
and `load` says so. The app's own knobs (`OGRE_TAP_BUTTON` position,
`OGRE_SCENE`, `OGRE_STEP`) are not part of the snapshot, and RT64's own render
state is not rewound (the next frame redraws from the restored RDRAM).
**Where** you save matters: a checkpoint taken while a *stable step* is live
resumes and replays (verified at the cathedral and at the personality
questions), but one taken at a scene/step **transition** (e.g. during the brief
`0x02` loader visit, when the engine has already reset the step word to 0)
restores into the "no step" movie-mode branch and the sequence bounces between
`0x00` and `0x0D` instead of continuing — so save *inside* the step you want to
replay, and check `c` reports the step you expect before `save` (session 58).

### Saves — the cartridge battery (SRAM), and emulator interchange (session 66)

**OB64 saves to the cartridge battery, and the chip is SRAM: 32 KiB, no
Controller Pak and no EEPROM/FlashRAM.** The Controller Pak strings
(`Controller Pak Menu`, `Data saved to Controller Pak.`) are the game's
*copy/backup* feature, not the save. `app/src/main.cpp` sets
`entry.save_type = recomp::SaveType::Sram`; the evidence is in the comment there.

The runtime keeps the image at

```
<config>/saves/<game id>.bin          # ogrebattle64-us-rev1.bin, exactly 32768 bytes
# macOS default: ~/Library/Application Support/ogrebattle64/saves/
# or set OGRE_PREF_DIR=<dir> to relocate it
```

and the game's own DMA path reaches it: `func_8008BC40` queues an `OSIoMesg` to
`D_800AA408`, and the runtime completes it inline in `librecomp/src/pi.cpp`
(`pi_perform_dma`) — **the device is chosen from the OSPiHandle the message
carries**, not from the address: the save handle `func_8008A040` builds has
`baseAddress 0xA8000000` (physical `0x08000000` = the SRAM window), the cart one
from `osCartRomInit` is `0xA0000000`. The game reads the whole image at boot in
256-byte DMAs at device offsets `0..0x7F00` (`func_80074CF0` and friends) and
writes it back the same way when its dirty flag is set (`func_80074BF0` →
`func_80074C58`). Save slots are `0x1850` (6224) bytes at `0x10 + n*0x1850`; the
device signature and slot-0 magic are the ASCII `QuestOG3`.

**A slot's first 4 bytes are two `u16` checksums, and their seed is the slot's own
device offset** (session 78). `func_8007541C` rejects a slot unless
`memcmp(D_800B8240, slot+4, 8)` matches `QuestOG3` **and**

```
u16[+0x00] == (sum of the bytes of slot+0x0C .. slot+0x1850) + (0x10 + n*0x1850)   // func_80075A84
u16[+0x02] == (count of their set bits)                     + (0x10 + n*0x1850)   // func_80075B00
```

So a slot is valid **only in the slot index it was checksummed for**. The game's
own copy-to-pak path uses the *un-seeded* twins (`func_80075AC4`/`func_80075B60`),
so a slot lifted out of a Controller Pak is seeded 0 and must be reseeded for the
battery slot it lands in. Any tool that moves save data between slots, or from a
pak, has to rewrite those two halfwords; `tools/sramsave.py` does, and `check`
reports each slot as `ok` or `MISMATCH (the game will reject this slot)`.

Diagnostics: the runtime prints one `[save]` line when it loads/creates the file,
one on the game's first read and first write of the window, and one when the
saving thread writes the file (`[save] wrote …`); a failed write also prints
`[save] FAILED to write …` to stderr before its message box.

**Bringing an emulator save in.** Ports/emulators all hold the same 32 KiB of
logical bytes but wrap them differently, so use `tools/sramsave.py` rather than
guessing:

```sh
tools/sramsave.py check  ~/Downloads/save.srm        # what is in the file
tools/sramsave.py import ~/Downloads/save.srm \
  "$HOME/Library/Application Support/ogrebattle64/saves/ogrebattle64-us-rev1.bin"
tools/sramsave.py export <that .bin> out.sra         # 32-bit byteswapped, .sra-style
```

`import` finds the SRAM by its magic at any offset and byte order. It was
written for a **parallel-n64** dump: 296960 bytes, SRAM at **0x20800**, every
32-bit word byte-reversed (so the magic reads `seuQ3GOt`). mupen64plus/RetroArch
`.srm` dumps are the bare logical 32 KiB (offset 0, "logical") and import
unchanged. It never invents data: a file without the magic is refused.

**DexDrive `.N64` files are Controller Pak dumps, not battery images** (session
78). A DexDrive dump is a 0x1040-byte header (`"123-456-STD"` at 0x00) followed
by a 32 KiB Controller Pak, so it is 36928 bytes — which is exactly what
`assets/saves/*.n64` are. They hold **no** battery image at any offset (scanning
every offset for the device magic finds none), but they do hold the game's
*copy/backup* notes: one 25-page (6400-byte) note per save in the pak's own PFS
filesystem, game code `NOBE`, publisher `EB`, named `OgreBATTLE64 <n>`, and each
note carries a verbatim copy of a battery slot at `note+0x20`. `import` follows
the pak's FAT to the **live** notes (a deleted note leaves its 25 pages behind,
still readable), extracts each slot and reseeds it into battery slot 0 (then 1);
the battery has **only two save slots** — slot index 2's offset `0x30B0` is where
the game's 19176-byte map record begins, and it covers the rest of the image, so
extra notes are dropped with a warning rather than written into it;
`--all` also takes the stale records, and `pak --all` lists them:

```sh
tools/sramsave.py pak --all assets/saves/prologue.n64
```

### Running with one of these saves — `OGRE_SAVE`, or `tools/run-save.sh`

```sh
# one knob: import (if needed) and boot
OGRE_SAVE=prologue ./build-app/ogrebattle64 [rom.z64]
OGRE_SAVE=~/Downloads/save.srm OGRE_SAVE_RESET=1 ./build-app/ogrebattle64

# or the wrapper, which gives each save its own config dir
tools/run-save.sh --list             # the saves in assets/saves
tools/run-save.sh prologue           # import it, then boot with it as the battery
tools/run-save.sh prologue --reset   # re-import, discarding progress saved since
```

`OGRE_SAVE` writes the converted image to `<config>/saves/<game id>.bin` — the
active config dir, i.e. the default one or whatever `OGRE_PREF_DIR` says — and
never back into the source, so a run left on a battery keeps its progress and
`OGRE_SAVE_RESET=1` starts over. `run-save.sh` does the same thing with a
per-save config dir, `.ogre-prefs-save-<name>/` (gitignored like the other
`.ogre-prefs-*` sandboxes), and its `--import-only` prints the path without
booting. Both accept the same file kinds and pass every `OGRE_*` knob through:

```sh
OGRE_SAVE=prologue OGRE_TAP_MS=1500 \
  OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:3,0x05:a:1" \
  OGRE_EXIT_AFTER_MS=60000 ./build-app/ogrebattle64
```

Verified (session 78): with `assets/saves/prologue.n64` imported, the tap route
takes the game **title → `0x12` (the Load Game book) → `0x05` (the map)**, and
battery slot 0 stays **byte-identical** to the converted note. The *discriminator*
is what the game's own data screen reads back, not the route — a **blank** battery
also reaches `0x12`/`0x05` (pressing A on the data screen starts a new game), so
the route alone proves nothing:

| battery image in the config dir | the book's GAME DATA 1 |
|---|---|
| `assets/saves/prologue.n64` converted | `Magnus / Prologue / Alba / 0:20:42` |
| `assets/save-mission-1.srm` converted (the older import) | `Magnus / Prologue / Alba / 0:07:02` |

Same screen, same route, two config dirs, different playtime — the game is reading
the file this script installed. `OGRE_SAVE=prologue` rebuilds that battery
**byte-identically** to `tools/sramsave.py import` (device header and slot 0
compared directly), so the two entry points are one conversion. Proofs:
`docs/proofs/native-load-game-prologue.png`,
`docs/proofs/native-load-game-battery-ab.png`. The same file imported *without*
reseeding the header checksums makes the game repair the whole image to its blank
format (sha256 `ed38cfd7…`), which is what the seed rule above predicts.

Verified (session 66, all on this build): boot with no file → the game formats a
blank battery and the port writes a real 32768-byte image with the `QuestOG3`
signature; restart → the game reads it back and does **not** re-write it (it is
accepted); overwrite the file with `0xA5`-garbled bytes → the game reads,
rejects and repairs it, and the file is valid again; a converted parallel-n64
save with real progress → the game reads it and does not re-write it (accepted).
A `[save]` write is followed by `[save] wrote <path>` unless the directory is
unwritable, which raises the runtime's "Failed to write to the save file" box —
**from the saving thread**, so a run that cannot write its `saves/` directory
stalls (that is what the sandboxed first attempt of session 66 hit; use
`OGRE_PREF_DIR`).

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

### `OGRE_NJREAD_LOG=1` — which framebuffer the njpeg readback copies

```sh
OGRE_NJREAD_LOG=1 OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000 \
  OGRE_TAP_BUTTON="start,a,start,a,start,a,start,a,start,a,a,a,a" \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64 assets/ogre64.z64 \
  > /dev/null 2> /tmp/njread.log
grep njread /tmp/njread.log
```

One `[njread]` line per stage-3 pass. Each New Game njpeg assembly runs four
passes — the four tiles of the backdrop's 2x2 grid, `width x height` 240x320
(top-left, destination `0x801AAE90`), 240x176 (`0x801D06B0`, bottom-left),
144x320 (`0x801E50D0`, top-right), 144x176 (`0x801FB8F0`, bottom-right) — each
with its own YUV draw. The four correct sources are the deterministic frames
`573FF47B8A5A3279`, `48892A76C362A4BA`, `3F79B6732153B2FD`, `38BCBEBE5B555C3C`
in that order, so an off-by-one read shows as a frame mismatch. **Pass 0 (the
top-left tile) is where a stale selection shows**: the scratch word still names
the previous step's target while `gameSrc` is the current one. See
`docs/HANDOFF-2026-09-16-session57.md` §1.

To check the *result* rather than the selection — the assembled backdrop the game
blits — drive the live console from game state, not wall time (the 8 MiB dumps
shift the tap schedule, so a fixed-time schedule does not reproduce):

```sh
# poll `c` and dump only while the dispatcher is on a chosen scene+step
printf 'c\n' > /tmp/ogre-console.txt            # read state...
printf 'dump /tmp/at-cathedral.bin\n' > /tmp/ogre-console.txt   # ...then dump
tools/rdram.py /tmp/at-cathedral.bin image 0x80243E28 --width 320 -o /tmp/dst.png
```

With the console reading `desc=0x8018FC3C` and `step=0x021D8002` (the cathedral
step after the name form), `0x80243E28` is the backdrop: the fix shows the
cathedral, the old scratch-first rule showed the name-entry form — that is the
intermittent artifact (`docs/proofs/native-newgame-backdrop-old-rule.png` vs
`-backdrop-fixed.png`).

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

### `tools/scenemap.py` — the scene registry, the transitions, the script (session 64)

```sh
tools/scenemap.py                 # all three tables (~3 s, ROM-only, no run)
tools/scenemap.py scenes          # 25 scene types -> accessor -> descriptor words
tools/scenemap.py transitions     # every writer of the pending-scene word
tools/scenemap.py steps           # the scripted step table summary
tools/scenemap.py scenes --dump /tmp/map3.bin   # read descriptors from a live image
```

Answers *"do we have to link the scenes by hand?"* with numbers, and replaces
hunting a scene's address one at a time. Three layers, only the first two code:

1. **The registry** — `func_80075BC0` writes **25** accessor pointers into
   `D_800AF028[0..24]`; each accessor is a 1-3 instruction stub returning a
   descriptor, and the descriptor table is **static ROM data** (`streamedB`).
   `scenes` prints `id → accessor → descriptor → enter/update/hook/leave/mask`
   (20 resolve statically; ids 2, 3, 6, 8, 23 branch and say
   `(indirect accessor)`). It reproduces every address earlier sessions found by
   hand — id 5 → `0x8018FD70` (map), id 7 → `0x8018FDAC` (form), id 13 →
   `0x8018FC3C` (the `0x0D` movie/dialogue engine).
2. **The transitions** — every store into `D_800C4C26` in the game is
   **39 sites in 30 functions**; 21 carry a statically-known value, the rest are
   script-driven. That is the whole scene-to-scene *code*, so there is no graph
   to author.
3. **The content** — the scripted step table (asset `0x19A8804`, ROM
   `0x1F3CA54`) holds **1693** per-step asset ids (1499 distinct), so the 200+
   dialogues are *entries in one table* driven by the same few scene types, not
   200 scene links.

Pair it with `tools/guestmap.py <descriptor>` for the ROM offset and owning
segment of a descriptor, and `tools/runlog.py` for which scenes a run actually
entered.

### `tools/arenamap.py` — every streamed arena, and whether we have it (session 73)

```sh
tools/arenamap.py                      # every arena, every bank (~10 s, offline)
tools/arenamap.py --missing            # banks no unit links
tools/arenamap.py --arena 0x80214FA0   # one RAM window
tools/arenamap.py --verify             # every unit vs the loader that DMA's it
tools/arenamap.py --coverage           # the ROM the scanned ELFs disassemble
tools/arenamap.py --entries            # cross-record entry candidates
tools/arenamap.py --md                 # markdown table (docs/scenes.md)
```

Answers *"what is this bank's size, and is it compiled?"* without a run, from the
loader's own instructions. Every bank DMA is

```
lui/addiu x3  ->  jal func_8009DA50  ->  subu a2,a2,a0   (the size)
func_800900C0(ram_base, code_size)   icache   (the brackets)
func_80090010(code_end, data_size)   dcache
func_80093380(data_end, bss_size)    bss
```

so the block yields ROM start, RAM base, size and the code/data boundary. **The
size is the loader's `subu`, never the chunk count**; the tool reads the `subu`'s
own operands because the assembler sometimes hoists it above the `jal`, and it
evaluates each block in ascending address order because `addiu a1,a1,%lo` must run
*after* the `lui` that gives it its base.

`--verify` cross-checks every compiled record against the loader that DMA's it.
A **split** difference is not automatically a bug: `mips-linux-gnu-as` pads
`.text` to 16 bytes, so a record whose data starts on an 8-byte boundary links its
data subsegment up to 8 bytes high (session 65's unit M), and several units'
linker scripts declare their record as one subsegment; `make elf-rom-check` is
what proves the bytes. `--coverage` is the negative check that matters: a loader
can only be found in code some ELF disassembles, so it lists the uncovered ROM
gaps and any `jal 0x8009DA50` inside them.

Current answer: **27 bank loads across 5 arena RAM windows, 26 compiled**, the one
gap being a `0xE80` scene-`0x14` setup fragment whose RAM another unit's record
already owns (see `docs/scenes.md`).

### Adding a streamed bank unit (the session-67 recipe)

When a scene reaches RAM the port has no code for, the run says so — take it in
this order, because each step is cheaper than the next and the later ones
mislead if the earlier one is skipped.

1. **The port names the missing module by itself.**
   `[bank] UNKNOWN module rom=0x… ram=0x…` (a *segment-table* record the port
   has no unit for) or `[overlays] streamed function stub called @ 0x…` (a
   `LOOKUP_FUNC` onto an address nothing registered). Run it, read the address,
   and only then look for the DMA.
2. **Add the unit.** `config-bank<U>.yaml` (splat: `type: code`, `start`/`vram`,
   `[start, asm]` + `[<code end>, data]`, `symbol_name_format: "ovl<U>_$VRAM"`)
   and `config-bank<U>.toml` (N64Recomp), then `BANK_UNITS += <U>` in the
   `Makefile`. A **bank's size is its DMA size**, and the code/data boundary is
   its last `jr ra` **rounded up to a 16-byte boundary** — otherwise the
   assembler pads `.text` and `make bank-recomp`'s ELF check fails loudly (the
   session-65 trap; that is exactly what it is for).
3. **Give records their BSS.** If the segment table's `ram_end` word is past
   `ram start + rom size`, add the record to `RAM_END` in
   `tools/gen_bank_funcs.py`; the runtime then zeroes it on load as the game's
   loader does.
4. **Declare cross-record function entries.** `symbol_addrs-bank<U>.txt`
   (`name = 0xADDR; // type:func`, wired with `symbol_addrs_path` in the splat
   config) for every address the game *calls* that the target record never `jal`s
   from inside itself. Each record is disassembled as its own segment, so such an
   address is emitted as a label inside the preceding body, N64Recomp has no
   entry there, and `LOOKUP_FUNC` becomes the **no-op** streamed stub
   (`get_function` has no interior fallback). Derive the list mechanically —
   every `LOOKUP_FUNC(0x…)` in `Bank<U>Funcs/` whose target is inside the unit's
   own RAM ranges and is not already a `recomp_trace_entry`. Do **not** try a
   subsegment split at the same address: the assembler's 16-byte `.text` padding
   breaks the ELF-vs-ROM identity.
5. **Watch for a bank the trace cannot see.** `OGRE_DMA_TRACE=1
   OGRE_DMA_TRACE_FULL=1` names the transfers, but its own overhead changes which
   path the run takes — in session 67 it showed one arena bank and the clean runs
   loaded a different one. A one-line probe in `recomp::do_rom_read` (log reads
   whose destination is the arena, or whose source is the ROM region) is
   lighter and found the second bank immediately.
6. **Fix the binding, not the symptom.** If a call runs the wrong bank's body,
   the unit that *contains* the target record is the problem: move the target
   record to a unit that does not call it, so the caller compiles as
   `LOOKUP_FUNC`. `make cross-bank-check` is the audit, and
   `tools/cross_bank_known_hazards.txt` is the explicit allowlist of accepted
   pre-existing hazards (a *new* one still fails `make bank-recomp`).

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

