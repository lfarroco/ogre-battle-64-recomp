# Ogre Battle 64: Person of Lordly Caliber — PC Port Project

Goal: a native Windows/Linux/macOS **port** of the N64 game **Ogre Battle 64:
Person of Lordly Caliber** (USA, Rev A) that runs the full game with modern
enhancements and modding support.

**Scope note:** this is a *PC port* project — NOT a byte-exact ("matching")
decompilation. We do not reconstruct the original C source or aim for a
byte-identical ROM. We use **static recompilation** (`N64Recomp`): the ROM's MIPS
code is automatically translated to C, then compiled natively against the
`N64ModernRuntime` runtime library. This is the same approach as Zelda 64:
Recompiled and Majora's Mask / Ocarina of Time PC ports.

**Legal note:** this is a "bring your own ROM" project. No copyrighted ROM data is
committed to this repository. You must provide a dump of your own cartridge.

**Working rules for AI agents: [`AGENTS.md`](AGENTS.md).** Read it before you
start. Every rule in it is a session that lost time — especially "ask the
developer what the current scene should display instead of inferring intent from
the code" and "a previous handoff is a hypothesis, verify it at instruction
level".

**Per-session record:** `docs/HANDOFF-YYYY-MM-DD-sessionN.md`, newest number
wins. The session-by-session status log that this file used to carry (sessions 3
to 97) is `docs/STATUS-LOG.md`.

---

## Goal

A native Windows/Linux/macOS port that runs the full game (menus, world map,
battles, cutscenes, audio, Controller Pak saves) with a modding framework.

## Browser port (WebAssembly)

The project also has a **WebAssembly / browser build**: the recompiled game plus
`N64ModernRuntime` under Emscripten. It compiles and runs the game again
(session 109). Its renderer is the WebGL2 prototype, which draws the title's
sprite layer and mangles the title's 3D content and the menu screens, because it
has no S2DEX2, no YUV16 and no RDRAM framebuffer indirection. The route for a
browser renderer is recorded in `docs/DECISIONS.md`; the native port is the
primary target and RT64 remains its renderer. `docs/WEB-PORT.md`,
`docs/WEB-PORT-REPORT.md` and `docs/WEB-PORT-DEPLOYMENT.md` describe the
2026-08-29 feasibility work and not the current state; the current state is this
file and `docs/HANDOFF-2026-09-27-session109.md`.

## Status

The game runs end to end. A build boots the publisher stills, the title, the New
Game opening, the map, missions and battles, the tutorial, the shop, the
Organize Screen, the credits, the ending and the attract loop.

- **Recompilation** — 99.05% of the ROM code span `0x001000..0x2B8B70` is
  compiled; the one gap (`0x0DDF80..0x0E4910`) is data. 44 records in 34 bank
  units (`BankA`..`BankZ`, `BankAA`..`BankAH`), 5037 registered function entries
  at 4963 addresses, 986 static dispatch targets with 0 unresolvable.
  `tools/recompcov.py` measures all of it.
- **Streamed code** — no un-recompiled module remains on any reachable path
  (session 85). A hand-played session executes 44.4% of the entries; the rest
  are paths nobody has played.
- **Renderer** — RT64 (Metal, Vulkan, D3D12) with a null renderer for bring-up.
  RT64's **"Fix LR with Scissor"** rect enhancement is off by default here
  (`OGRE_RECT_LR_FIX=1` restores it): it rewrites a rectangle's right edge to the
  scissor's edge whenever the two are within one pixel, which widened the unit
  profile's rightmost DEF/MDEF digit rectangle and made it sample the next font
  cell (session 101).
- **Audio** — the game's own audio microcode is recompiled and runs by default.
  The Linux package needs a static SDL2 with an ALSA, PulseAudio or PipeWire
  backend. The 0.3.0 build host had no audio development headers, SDL2 dropped
  all three without failing, and the shipped binary ran silently on every
  PipeWire and PulseAudio machine (`dsp: No such audio device`). `make
  sdl2-static` and `tools/smoke-dist.sh` now fail instead of shipping that
  (session 100, issue #8).
- **Saves** — the cartridge battery is SRAM and works; the Controller Pak menus
  render.
- **Player features** — tabbed launcher, in-game `ESC` overlay, rebindable
  controls, GAME SPEED, WIDESCREEN, mods, `make dist` packages, GitHub releases.
  The launcher and the overlay draw one `ui::Panel`: the tabs are MAIN (which
  carries the ROM row and EXIT GAME), MODS, CONTROLS, SETTINGS and DEBUG, and the
  panel reserves the CONTROLS tab's height so the header, the tab bar and the
  footer keep one position on every tab (session 104; MAIN replaced START GAME in
  session 110). `mods/skip-boot-logos/` is the
  mod reference; `mods/exp-overflow/` carries a level-up's leftover EXP over
  instead of letting the game zero it (session 103). The package carries them:
  `make dist` and `tools/smoke-dist.sh` require at least one `.nrm`, and the
  hosted release runners get them from the private data bundle (session 108).
  WIDESCREEN is a toggle (`off` / `on`): `on` turns RT64's Expand aspect ratio
  on only while scene `0x03` runs, so the mission field is hor+ 16:9 and the 4:3
  2D screens are untouched (sessions 102, 105). The window shape follows the
  toggle and never the scene: `off` opens 4:3 with no pillarbox, and `on` opens
  16:9 with the 4:3 scenes pillarboxed inside. SOUNDS is a radio group
  (`off` / `on`) and VOLUME a 0..100 slider; the audio callback scales every
  queued buffer by the gain and queues silence rather than nothing at `off`, so
  the runtime's queue-depth pacing is unchanged (session 106). A connected pad
  fills `Platform::controllers[0]`, the N64 controller the game reads, with the
  keyboard OR'd into the same slot; pads used to fill slots 1..3, so a pad was
  visible in the launcher and the overlay and invisible in game (session 118,
  issue #13).
- **HD backgrounds (experiment)** — `mods/backgrounds/` is a pack of PNGs that
  replaces the dialogue/cutscene backdrops. The app writes the pack's image over
  the pixels the njpeg readback copies, split to match the game's four-chunk
  496x384 backdrop canvas, so one PNG fills the whole backdrop. The first
  mapping is scene `0x0D` step 2 (the New Game cathedral), with a plain
  `01.png` and a widescreen `01-wide.png`. With WIDESCREEN on, scene `0x0D`
  expands whether or not the pack is installed (its own 496x384 backdrop has
  content the 4:3 view crops), and a scene the pack covers is added to the RT64
  Expand set, so the backdrop fills the 16:9 window and reveals more of the
  canvas at the sides while the dialogue box and text keep their size.
  `OGRE_BG=0` runs without the pack. `tools/backgrounds.py` (and `make
  bg-extract`) extracts the game's own backdrop as the reference an artist
  draws over; the output lives in the gitignored `mods/backgrounds/reference/`.
  It is a reskin at the game's resolution, and not an HD texture replacement.
  See `docs/guides/hd-backgrounds.md` and
  `docs/HANDOFF-2026-09-29-session114.md`.
- **Performance** — five misclassified work loops in bank unit N carried a
  recompiler-injected blocking `yield_self`; their backward branches are listed
  in `config/banks/config-bankN.toml`'s `yield_work_loop_branches`
  (`0x801B3008`, `0x801DA2C0`, `0x801D0C08`, `0x8020A910`, `0x80204C48`;
  sessions 80, 90, 98, 112, 115). A slow screen is measured with `OGRE_PROFILE=1
  OGRE_DL_TRACE=1 OGRE_SCENE_LOG=1` for its display-list series and its `t4`
  `[prof]` line, but the profiler reports the most recently entered function and
  can name an entry-heavy function rather than the one holding the thread. Take a
  host stack sample (`sample <pid> 4 -file …`) during the slow state as well; that
  named `func_ovlN_80208E84` in session 112 and `func_ovlN_80204C08` in session
  115 after the profiler had named other addresses. `debug/menu-probe.sh` runs
  the app, the sampler, the console `snap` output and the coverage census
  together, so no capture time has to be agreed in advance.
- **Periodic hitch** — the runtime's message-queue snapshot
  (`ultramodern::debug_dump_queue_snapshot`) ran unconditionally every 90 VI
  frames on the Critical-priority VI thread, which measured as a 33 -> 59 ms
  frame every 1.5 s on macOS and is larger on Windows, where its
  `sleep_for(200us)` sampler resolves to SDL's 1 ms timer. It is now behind
  `OGRE_SNAP` and off by default (session 116). `OGRE_SNAP=1` restores the old
  ~1.5 s period for hang diagnosis; a number sets the period in VI frames.
  `tools/dlgaps.py <run.log>` measures the next such report: it prints the gap
  distribution and the phase histogram of the spikes, and `--check` fails when
  they are periodic. See `docs/HANDOFF-2026-10-01-session116.md`.
- **Debug console** — the live console's `snap [prefix] [ms]` writes the whole
  8 MiB RDRAM image with the game threads parked and, for `ms`, RT64's
  presented-frame capture for the same screen, so a player-reported defect can be
  read offline without a bounded run (`docs/guides/app-build.md`, session 101).
- **Windows** — v0.4.0's access violation on the first present is fixed, released
  and confirmed by a Windows run. The presented-frame capture was left enabled by
  `init_capture_env`'s `putenv` name-mangling (the UCRT copies the string), and
  RT64's readback handed the D3D12 backend a null texture. The capture path is
  now app-owned and the D3D12 copy skips sample positions for a buffer
  destination (session 107). The released Windows package of 2026-09-28 carries
  the fix: session 113 measured it opening its window on the developer's GeForce
  940MX / Intel HD 620 laptop with `d3d12.dll` and `D3D12Core.dll` loaded and no
  `vulkan-1.dll`, exit 0 and no new `error.log`. Players on v0.4.0 need the newer
  package. **Those two changes were not in any release**: the hosted runner
  builds the third-party trees from `patches/`, and both RT64 patches had drifted
  from them (session 117). The patches are regenerated and `make patch-check` now
  fails when a patch stops describing its tree. The captured `stdout`/`stderr`
  sections are empty in an Explorer launch because the process starts with NULL
  standard handles, and in that state the capture records nothing at all
  (session 119). A player's report therefore carries no log, and
  `ogrebattle64.exe > run.log 2>&1` from a shell discards the redirect as well.
- **Release parity** — a GitHub-hosted release builds three different sources: the
  public repository, the private generated-code bundle (`ogre-data/files.tar.gz`,
  which has a commit-drift check), and the `patches/` applied to pristine
  third-party checkouts, which had none. `tools/patchcheck.py` and
  `make patch-check` add it, and `tools/release-build.sh` runs it before
  packaging. Measured: v0.5.1 was built from `c7bd999` (session 112), so session
  115's slowdown fix and session 116's stutter fix are in no release, and the
  bundle holds 34 `yield_self` sites against the tree's 33 (session 117).
- **Debug behaviour in a release** — an audit with no environment variables set
  found the live console channel (a fixed path whose commands write guest RAM and
  load checkpoints), 116 `[rsp]` lines/s, per-display-list `[renderer]` lines, and
  trace hooks on every recompiled function entry and return. The console is now
  off unless `OGRE_LIVE_CONSOLE=1` (or the trigger's own variable) is set, so a
  stock run opens no path, reads no keyboard and prints nothing (session 117). The
  rest is open work below.

What each screen should show is `docs/scenes.md`. Build instructions and every
`OGRE_*` knob are `docs/guides/app-build.md`. A picture or a display list that a
run produces is evidence about the port until the five checks in
`docs/guides/emulator-first.md` pass.

## Open work

Ordered by how much each item blocks a player. Each item names the handoff that
holds the evidence.

1. **Windows access violation when the game starts (`0xC0000005`). Closed, and
   verified on Windows (session 113).** Session 107 root-caused v0.4.0's fault at
   `ogrebattle64.exe+0x10001AE`: `plume::d3d12::D3D12CommandList::setSamplePositions`
   with a null `texture`, called from `copyTextureRegion` with a `PlacedFootprint`
   (buffer) destination, which is the port's own presented-frame readback. The
   readback ran because `init_capture_env` hid `OGRE_CAPTURE_PRESENT` by rewriting
   the first byte of a `putenv`'d name, which the UCRT's copying `_putenv` ignores.
   `snap` now toggles an app-owned path through `ogre_present_capture_path()`,
   the D3D12 path skips sample positions for a non-texture destination, and
   `tools/smoke-dist.sh` fails a package whose capture is on. The released Windows
   package of 2026-09-28 contains the fix and not the defect (the pre-fix
   `OGRE_CAPTURE_PRESENT=` string is absent, session 107's `capture path:` strings
   are present), and a bounded run on the laptop that produced the report
   initializes D3D12 (`d3d12.dll`, `D3D12Core.dll`, no `vulkan-1.dll`), opens its
   window, exits 0 and writes no `error.log`. Part (b), the failure on releases
   older than the capture code, no longer reproduces: RT64's
   `Falling back to Vulkan due to device workaround.` did not run, because the
   adapters report Intel `31.0.101.2140` and NVIDIA `32.0.15.8266` (582.66),
   both above session 94e's thresholds. The captured `stdout`/`stderr` sections
   are empty in an Explorer launch, and that is now root-caused: the process
   starts with NULL standard handles, and in that state the capture records
   nothing — a six-second boot puts 101 `stdout` and 53 `stderr` lines into the
   report with valid handles and 0 lines with NULL handles, so the device, the
   driver version and the fallback are still not visible in a player's report.
   The proposed fix is to give fds 1/2 a real destination before
   `crash_log::install`, and to stop the `AttachConsole` branch from freopening
   `CONOUT$` over an inherited redirection. See
   `docs/HANDOFF-2026-09-30-session119.md`,
   `docs/HANDOFF-2026-09-30-session113.md`,
   `docs/HANDOFF-2026-09-27-session107.md` and
   `docs/HANDOFF-2026-09-20-session94.md` part 5.
2. **SIGBUS after a lost battle, `func_800862C0+0x59A` at guest `0x80086328`.**
   The faulting instruction is identified and the writer is not. The instruction
   is `lw $s6, 4($a1)`, where `$a1` is `+0x7C` of an effect object; the value
   looks like a misread record field, not a pointer, and the crash lands on a
   scene/mode transition. The effect subsystem itself has no cross-bank call
   site, so the write that corrupts the record list comes from another resident
   function. See `docs/HANDOFF-2026-09-21-session96.md` §1.
3. **Title sprite green border, a regression.** A transparent sprite composites
   opaquely for 1-3 presents on a type's first appearance, and the sprite's colour
   texture border shows as exactly `(0,132,0)`. The mask and the combiner are
   byte-exact; the failing draw reaches the `M == PM_FRAMEBUFFER_COLOR` arm with
   `forceBlend` set and returns `finalAlpha = 1` instead of the combiner alpha
   (`rt64_blender.h`). The defect is present in every source state buildable on
   this machine, including RT64 at the 2026-09-17 commit `52f7160`, so no RT64
   change since then causes it and none fixes it. The next instrument is in the
   shader at the tagged draws: log `otherMode.forceBlend()`, `cycleType()`,
   `combinerCycles` and `blenderInputs()` and compare them with the CPU's values
   for the same `omL`. See `docs/HANDOFF-2026-09-21-session96.md` §2-§3.
4. **Credits screen (scene `0x11`) artifacts, and three latent leak-shaped
   `jal`s.** The screen plays; its backgrounds carry artifacts, and three `jal`s
   are emitted in the call-plus-early-`return` shape that leaks the frame-pump
   stack if their scene is reached. See
   `docs/HANDOFF-2026-09-19-session86.md` §5 and `-session85.md` §8.
5. **Teardown crash `func_8007F8E4+0x2F8` after the DMA census dump.** The
   window-close path dumps the DMA census and then faults. See
   `docs/HANDOFF-2026-09-19-session85.md` §4.
6. **Latent static backlog.** `tools/stubmap.py report` classifies the 986 static
   dispatch targets as 130 missing-function candidates, 152 bank-selection
   candidates, 369 inert, 332 resolvable and 3 IMEM. The main unit also makes 951
   direct calls into swappable RAM across 236 targets (`make cross-bank-check`),
   a known backlog that `cross_bank.py dispatch --only` fixes one target at a
   time. The 288 indirect `jalr` sites are outside any static measurement; a
   run's stub log is the only check for them.
7. **Controller Pak copy/backup (cartridge battery ↔ Controller Pak
   `Save`/`Load`/`Erase`).** The screen renders; the copy flow is deferred. The
   developer's decision is that it is low priority for a PC port. The device half
   (`osPfs*` and a flat 32 KiB `.mpk`) is implemented. See `docs/DECISIONS.md`
   (88b).
8. **Web build.** The wasm target compiles and runs again (session 109): two
   defects had blocked it since 2026-09-16. `app/src/bank_overlays.cpp`
   included the native-only `sdl_platform.hpp` for the live console, which is
   now `#ifndef __EMSCRIPTEN__`; and `app/src/main_web.cpp` registered
   `recomp::SaveType::None`, so the game's first SRAM DMA exited the runtime at
   about 350 ms (`librecomp/src/pi.cpp`), which read as a boot freeze. It now
   registers `Sram` like `app/src/main.cpp`. The title renders
   (`docs/proofs/web-title.png`), 40 display lists are submitted with no bad
   walk, and the audio microcode runs with the layer muted until `?audio`. The
   renderer is still the WebGL2 prototype: no S2DEX2, no YUV16, no RDRAM
   framebuffer indirection, so the opening and menu screens are wrong
   (`docs/proofs/web-opening-broken.png`). The renderer route is decided in
   `docs/DECISIONS.md`; the build is not in CI, which is why it stayed broken.
   See `docs/HANDOFF-2026-09-27-session109.md`.
9. **The published v0.4.0 archives ship an empty `mods/`, and the bundle that
   feeds the release did not carry one.** Fixed on main (session 108):
   `make dist`, `tools/release-build.sh`, the workflow's unpack step and
   `tools/smoke-dist.sh` all require an `.nrm`, and `tools/data-bundle.sh` packs
   `build/mods/*.nrm`. Session 111 regenerated the private data repository's
   `files.tar.gz` — it still predated the rule, so every hosted job failed at the
   unpack step with `build/mods/*.nrm is missing from files.tar.gz` — and made
   the unpack step fail when the bundle's recorded commit differs from the commit
   being built, where it used to warn. A clean `make recomp` and `make bank-recomp`
   reproduced the tree's generated code byte for byte, so the new bundle changes
   only the mods and the recorded commit. The re-run (`36349726660`) completed
   success on all three platforms and its Release job produced **`v0.5.0` as a
   draft**, whose Linux and macOS archives both contain
   `mods/exp-overflow.nrm` and `mods/skip-boot-logos.nrm`. That release is
   published: session 113's download carries both mods and the session-107 fix.
   The published `v0.4.0` assets still have the empty `mods/`.
   See `docs/HANDOFF-2026-09-27-session111.md`.
10. **HD backgrounds: true HD needs the renderer.** `mods/backgrounds/` reskins
    the backdrop at the game's own 320x240 (box-filtered to the 496x384 canvas),
    so its detail is capped by the original. Sampling a larger texture needs
    RT64 texture replacement, keyed by the drawn texture's hash. RT64 already has
    the machinery (`TextureCache::loadReplacementDirectory`, keyed by a TMEM
    hash) and `upscale2D = ScaledOnly`, but the port does not load a replacement
    directory and the hash for one backdrop has to be recorded from a run. The
    four-chunk layout the current feature uses is hardcoded to the cathedral
    asset's chunk sizes; a pack image for a backdrop with a different layout is
    left alone (`no canvas rect`). See `docs/guides/hd-backgrounds.md`.
11. **Confirm the periodic-hitch fix on Windows, and keep the hang dump
    available.** Session 116 measured the snapshot's 1.5 s hitch on macOS and
    gated it; the Windows size is inferred from SDL's `timeBeginPeriod(1)` and
    the 200 us sampler, so one run there decides it (`OGRE_DL_TRACE=1` with
    `OGRE_SNAP` unset against `OGRE_SNAP=1`). With the gate off by default a hang
    produces no `[snap]` output unless the run asked for it; a
    frame-counter-triggered dump would keep the diagnosis at zero periodic cost.
    See `docs/HANDOFF-2026-10-01-session116.md`.
12. **Gate the remaining debug behaviour that ships (session 117).** The live
    console is done: it is off unless `OGRE_LIVE_CONSOLE=1` or one of its own
    trigger variables is set. What is left, ranked by the difficulty of removing
    it rather than by the cost:
    (a) `[rsp]` (116 lines/s) and the per-display-list `[renderer]` lines — gate
    them and set the variable in `make smoke`, `tools/smoke-dist.sh` and the
    CI `runlog.py --check`, or those checks silently stop checking anything;
    (b) the trace hooks on every recompiled function entry and return (28 + 4
    instructions at 5316 sites with the gates off) — making the disabled path a
    load-and-branch needs a change to `patches/n64recomp-ob64.patch`, so it
    regenerates the game code and re-runs the stutter measurement;
    (c) the audio auto-response on `0x800C49E8` is a behaviour shim, not a log:
    firing the real `OS_EVENT_AI` when a buffer drains lets it be deleted.
    See `docs/HANDOFF-2026-10-01-session117.md` for the full list.
13. **The browser build assigns gamepads to N64 controllers 2-4.** The native
    fix for issue #13 landed in `app/src/sdl_platform.cpp` (the first pad takes
    slot 0), but `app/web/web.js` still places gamepads in slots 1..3 with the
    keyboard alone in slot 0, and `app/src/web_platform.cpp` documents the same
    model. A browser gamepad therefore cannot reach N64 controller 1 either
    (`docs/HANDOFF-2026-10-01-session118.md`). The change is to OR the first
    pad's state into slot 0; it needs a wasm build and a browser run to verify.
Parked, not defects:

- Audio above 1× speed is untested; `OGRE_SPEED` runs always had audio off
  (`docs/HANDOFF-2026-09-20-session93.md` §8).
- The `ESC` overlay does not pause the game (the developer's choice), and gamepad
  rebinding is code-verified only because this machine has no gamepad
  (`docs/HANDOFF-2026-09-20-session92.md` §9).
- `OGRE_NO_AUDIO=1` is the workaround on hosts where `SDL_OpenAudioDevice`
  blocks.
- `0x801936A9`, the community's Chaos Frame address, is in the same saved flags
  block but has no static read or write in this ROM
  (`docs/HANDOFF-2026-09-21-session95.md` §5).

## Roadmap

1. **Phase 0 — toolchain & ROM** ✅
2. **Phase 1 — disassembly & ELF** ✅
3. **Phase 2 — recompile the main segment** ✅
4. **Phase 3 — first boot** ✅ (sessions 3-6)
5. **Phase 4 — overlays & streamed code** ✅ — 44 records in 34 units (A..AH);
   no un-recompiled module on a reachable path (session 85)
6. **Phase 5 — assets** ✅ — LZ, njpeg backgrounds, textures; 99.05% of the code
   span recompiled; `tools/recompcov.py` measures coverage
7. **Phase 6 — saves, audio, QoL** — the SRAM battery and the Controller Pak
   device work, the audio microcode runs, and settings, controls and the overlay
   are done. Open: the Controller Pak copy flow, audio above 1×
8. **Phase 7 — modding framework & packaging** ✅ — mod system with a shipped
   example, `make dist` packages, GitHub releases

## Toolchain

| Tool | Purpose | Location |
|---|---|---|
| splat 0.50 (`splat64[mips]`) | ROM splitting / disassembly | `tools/venv` |
| spimdisasm | MIPS disassembler (used by splat) | via pip |
| mips-linux-gnu-binutils | assemble `.s` → `.o`, link ELF | Homebrew |
| N64Recomp (forked) | MIPS → C recompilation | `tools/N64Recomp` (vendored clone) |
| N64ModernRuntime | recompiled game runtime (libultra shim, renderer) | `tools/N64ModernRuntime` (vendored clone) |
| RT64 | renderer | `tools/RT64` (git submodule) |

`tools/N64Recomp`, `tools/N64ModernRuntime` and `tools/RecompFrontend` are
gitignored clones, not submodules. RT64 is the only submodule.

Our modifications to the upstream tools are the five patches under `patches/`:
`n64recomp-ob64.patch`, `n64modernruntime-ob64.patch`,
`n64modernruntime-n64recomp.patch`, `rt64-ob64.patch` and
`rt64-plume-ob64.patch`. `tools/rt64-plume-sdl.patch` is separate: it is the
SDL ≥ 2.0.22 guard for RT64's plume, for systems with an older SDL2. Apply order
and commands are in `docs/guides/app-build.md` → "Configure & build".

## Reproduce (macOS)

> Linux (Ubuntu) setup: see `docs/guides/linux-migration.md`. The commands below
> are the same except `brew install` → `apt install` equivalents (listed there).

```sh
# 1. Tools
brew install mips-linux-gnu-binutils cmake
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../patches/n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4

# 2. ROM: place your big-endian dump at assets/ogre64.z64
#    (tools/convert_rom.py converts a 16-bit byte-swapped .n64)

# 3. Regenerate the recompiled code (splat -> ELF -> 34 bank units ->
#    main recompilation -> RSP microcode), then build the app
make regenerate
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
./build-app/ogrebattle64
```

`make regenerate` is the one supported order. `make recomp` refuses to run when
`app/src/bank_funcs.inc` is missing, because its cross-bank dispatch would then
be skipped and calls into swappable RAM would bind to the wrong bank. See
`docs/guides/app-build.md` → "Regenerating the recompiled code".

## Key technical findings

- **Byte order.** The conventional dump is a 16-bit byte-swapped `.n64`.
  `tools/convert_rom.py` converts it to big-endian `.z64`. The app accepts
  `.z64`, `.n64` and `.v64`.
- **Header layout.** Official Nintendo layout: entry point at offset `0x08`
  (`0x80070C00`), internal name at `0x20` (`OgreBattle64`), country at `0x3E`.
- **Memory map.** ROM `0x1000` ↔ vram `0x80070C00` (entry segment, `0x60` bytes);
  main segment ROM `0x1060` ↔ `0x80070C60`, ending at BSS start `0x800AEDB0`;
  BSS `0x800AEDB0..0x800E9C20`; stack `0x800C6D60`; `main` at `0x8007F880`.
- **libultra** is bridged by name: `config/symbols/symbol_addrs.txt` names the
  libultra functions so the runtime's native `osXxx_recomp` services replace
  OB64's verbatim copy. `docs/LIBULTRA-BRIDGING.md` records the identification
  method and the address→name table.
- **Streamed code is mapped and compiled.** Each streamed overlay is plain
  linked MIPS code and data DMA'd to a fixed RAM address. Records that share a
  RAM range are separate bank units, so a call into the range compiles as
  `LOOKUP_FUNC` and the runtime's DMA-driven bank map picks the resident module.
  Compiling a swappable range into the unit that calls into it binds the call to
  one bank's bodies at build time and runs them with the wrong frame contract
  (sessions 41/45/55). AGENTS.md rule 4 holds the details.
- **Scene graph.** `func_80075BC0` looks up `D_800AF028[scene_id]()`; there are
  exactly 25 scene types, 39 transitions and a 1693-entry scripted step table.
  `tools/scenemap.py` extracts all of it from the ROM in about 3 seconds.
  AGENTS.md's "Verified facts" holds the addresses.
- **Display lists are not run through recompiled microcode.** The runtime routes
  gfx tasks to RT64, which parses them with its own GBI interpreters. RSPRecomp
  compiles the game's njpeg and audio microcodes only.

## Working documents

| Document | Holds |
|---|---|
| `AGENTS.md` | working rules and the durable verified facts |
| `docs/README.md` | documentation index |
| `docs/HANDOFF-YYYY-MM-DD-sessionN.md` | per-session record, newest number wins |
| `docs/STATUS-LOG.md` | the session status log this file used to carry |
| `docs/DECISIONS.md` | decision log; the "Durable decisions" table at the top is the current set |
| `docs/scenes.md` | what each screen should show, from the developer |
| `docs/symbols.md` | proposed symbol names with evidence and confidence |
| `docs/guides/app-build.md` | build, run, every `OGRE_*` knob, the diagnostics toolkit |
| `docs/guides/emulator-first.md` | read before interpreting a display list |
| `docs/guides/njpeg-backgrounds.md` | the njpeg background pipeline |
| `docs/guides/rsp-microcode.md` | RSP microcode research and recompilation |
| `docs/guides/app-architecture.md` | app structure, runtime flow, callbacks |
| `docs/guides/linux-migration.md` | Ubuntu setup |
| `docs/WEB-PORT*.md` | the browser port (dated 2026-08-29) |
