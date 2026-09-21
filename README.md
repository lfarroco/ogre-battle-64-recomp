# Ogre Battle 64: Person of Lordly Caliber (PC port project)

Static recompilation of the N64 game *Ogre Battle 64: Person of Lordly Caliber*
(USA, Rev A) to a native PC executable, using the [N64Recomp] toolchain.

**This repository contains no copyrighted game data.** You must supply your own
ROM dump (see below).

## System Requirements

A 64-bit PC and a GPU the renderer supports:

* **GPU** — Direct3D 12.0 (Shader Model 6.0) or Vulkan 1.2 on Windows, Vulkan
  1.2 on Linux, Metal on macOS. The oldest GPUs those cover are roughly GeForce
  GT 630, Radeon HD 7750 (2012) and Intel HD 510 (Skylake).
* **CPU** — x86-64 with SSE4.1 (Intel Core 2 Penryn, AMD Bulldozer or newer).
  The macOS build needs Apple Silicon.
* **OS** — Windows 10 or 11 (64-bit); macOS 11 or newer on Apple Silicon; a
  glibc Linux with a Vulkan 1.2 driver.
* **RAM** — 2 GB. The emulated N64 machine commits 512 MB.
* **Disk** — about 150 MB for the app, plus the ROM.
* **Game data** — your own Ogre Battle 64 (USA, Rev A) cartridge dump, 40 MB in
  `.z64`, `.n64` or `.v64`. No game data is included.

A keyboard is enough to play; a gamepad with XInput (Windows) or SDL controller
support is optional. An audio device is optional too: with none, the game runs
silently. On Windows no Visual C++ redistributable is required — the package
carries the runtime its shader compiler needs.

If the game crashes as it starts, update the graphics driver first. On an older
GPU, `OGRE_CONSOLE=1` opens a console with the boot log and `OGRE_GRAPHICS_API`
pins the backend (`vulkan` or `d3d12`).

## AI Disclaimer

This work in this project was mostly performed by the DeepSeek v4/v4.1 Flash model.

## Status

See [PLAN.md](PLAN.md) for the full plan, current status, and technical findings.

## Directory layout

```
assets/                your ROM (gitignored; big-endian .z64 expected)
asm/                   splat-generated disassembly
debug/                 headless-browser probes for the wasm build (see debug/README.md)
config.yaml            splat config (segments, vram mapping)
config.toml            N64Recomp config
Makefile               assemble + link + recompile
n64recomp-ob64.patch   our N64Recomp modifications (apply to upstream clone)
rt64-plume-sdl.patch   our RT64 plume patch — SDL >= 2.0.22 guard (apply to the
                       tools/RT64 submodule on systems with older SDL2, e.g. Ubuntu 22.04)
PLAN.md                the project plan
```

## Getting started

See **Reproduce** in [PLAN.md](PLAN.md). Summary:

```sh
# tools (macOS)
brew install mips-linux-gnu-binutils cmake
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4

# ROM: put your .z64 dump in assets/, then regenerate the recompiled code
# (see docs/guides/app-build.md -> "Regenerating the recompiled code")
make regenerate
```

The ROM must be the USA Rev A dump (40 MB, `.n64` 16-bit byte-swapped or already
converted `.z64`). `tools/convert_rom.py` converts `.n64` → `.z64`.

**A fresh clone cannot build the app until `make regenerate` has run once**: the
recompiled C (`RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`,
`app/src/bank_funcs.inc`) is generated from your own ROM and is deliberately not
committed. `make regenerate` runs splat, the MIPS link, the 34 bank units, the
main recompilation and the RSP microcode in the one order that works.

### Build and run the app

```sh
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
./build-app/ogrebattle64
```

On launch the app shows a black start screen — `OGRE BATTLE 64: RECOMP` /
`CLICK TO LOAD YOUR ROM (OR DROP IT IN THIS WINDOW)`. Click it to pick your ROM,
drag the ROM onto the window, or just put the ROM next to the executable. The
ROM is validated by hash and stored, so later launches go straight into the
game. The battery save lands in `saves/` **beside the executable**.

### A playable build

```sh
make dist        # -> dist/ogre-battle-64-recomp/   (one self-contained executable)
make dist-zip    # -> dist/ogre-battle-64-recomp-<platform>.zip
```

The package is a single file: SDL2 is linked statically (`make dist` fetches and
builds a pinned real SDL2 once, because Homebrew's `sdl2` is the SDL3-based
compat shim and has no static library). No game data is included, so the player
supplies their own ROM on the start screen. See `docs/guides/app-build.md` →
"Distribution".

The renderer (`tools/RT64`) is a git submodule pinned to an upstream commit and
needs its own one-time patch on systems with SDL < 2.0.22 (e.g. Ubuntu 22.04
ships SDL 2.0.20, but `SDL_GetWindowSizeInPixels` requires 2.0.22+):

```sh
git submodule update --init --recursive
git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch
```

Re-apply after any `git submodule update` inside `tools/RT64`, which resets the
submodule and discards the patch.

## Legal

Ogre Battle 64 © Quest / Nintendo. This project is for preservation and
interoperability research. Never distribute the ROM or its extracted assets.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
