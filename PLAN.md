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

In addition to the native port, the project has a **WebAssembly / browser build**
(`docs/WEB-PORT.md`): the recompiled game plus `N64ModernRuntime` under
Emscripten, with a null renderer first and a WebGL2 renderer prototype. The
native port is the primary target, and RT64 remains its renderer. See
`docs/WEB-PORT.md`, `docs/WEB-PORT-REPORT.md` and
`docs/WEB-PORT-DEPLOYMENT.md`. Those three documents were written 2026-08-29 and
have not been updated since; the current state of the port is this file.

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
- **Audio** — the game's own audio microcode is recompiled and runs by default.
- **Saves** — the cartridge battery is SRAM and works; the Controller Pak menus
  render.
- **Player features** — tabbed launcher, in-game `ESC` overlay, rebindable
  controls, GAME SPEED, mods, `make dist` packages, GitHub releases.

What each screen should show is `docs/scenes.md`. Build instructions and every
`OGRE_*` knob are `docs/guides/app-build.md`. A picture or a display list that a
run produces is evidence about the port until the five checks in
`docs/guides/emulator-first.md` pass.

## Open work

Ordered by how much each item blocks a player. Each item names the handoff that
holds the evidence.

1. **Windows access violation when the game starts (`0xC0000005`).** The
   launcher runs and loading the ROM dies on an access violation. The first
   release on a player's machine worked and the developer's Windows laptop fails
   on every release. The console build printed RT64's `Falling back to Vulkan
   due to device workaround.` just before the crash; the fallback destroys the
   D3D12 device and builds a Vulkan interface on the same window.
   `OGRE_GRAPHICS_API=<auto|d3d12|vulkan|metal>` lets a player pin the backend.
   The next report names the device, the driver version, whether the fallback
   ran, and the faulting module. See `docs/HANDOFF-2026-09-20-session94.md`
   part 5.
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
8. **Web build.** Its audio is still silenced by default and still points at the
   stub, and its renderer is a WebGL2 prototype. Re-pointing it at the working
   audio microcode and choosing a browser renderer are deferred. See
   `docs/WEB-PORT.md` and `docs/WEB-PORT-REPORT.md`.

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
