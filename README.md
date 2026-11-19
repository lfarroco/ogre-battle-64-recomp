# Ogre Battle 64: Person of Lordly Caliber (PC port)

Static recompilation of the N64 game *Ogre Battle 64: Person of Lordly Caliber*
(USA, Rev A) to a native PC executable, using the [N64Recomp] toolchain.

This repository contains no game data. You must supply your own ROM dump.

## Web version

An experimental browser build runs at
https://lfarroco.github.io/ogre-battle-64-recomp/. It imports and exports saves
from the native version. Mods are not supported yet. The renderer is documented
in [docs/guides/web-renderer.md](docs/guides/web-renderer.md).

## Requirements

The port needs an x86-64 CPU, or Apple Silicon on macOS. Its graphics
requirements come from RT64, which renders through Direct3D 12 (Shader Model
6.0), Vulkan 1.2 or Metal. The OS must be Windows 10/11, macOS 11 or newer, or a
glibc Linux with Vulkan 1.2. 2 GB of RAM and about 150 MB of disk are enough. A
keyboard is enough to play. A gamepad and an audio device are optional.

In practice, players have needed only an up-to-date graphics driver. If the game
crashes at startup, update the driver first.

Game data is a 40 MB Ogre Battle 64 (USA, Rev A) dump in `.z64`, `.n64` or
`.v64`.

## AI disclosure

The port is largely written by DeepSeek models, as an experiment in agent-driven
recompilation.

## Status

See [PLAN.md](PLAN.md) for the full plan, current status and technical findings.

## Directory layout

```
app/                   the port's CMake project (the executable)
assets/                your ROM (gitignored; big-endian .z64 expected)
asm/                   splat-generated disassembly
config/                every config the build reads
config/config.yaml     splat config (segments, vram mapping)
config/config.toml     N64Recomp config
config/banks/          per-unit splat and N64Recomp configs
config/symbols/        symbol addresses and relocation lists
config/rsp-*.toml      RSPRecomp configs (njpeg, audio)
debug/                 headless-browser probes for the wasm build
docs/                  documentation, guides and session handoffs
patches/               patches for N64Recomp, N64ModernRuntime and RT64
tools/rt64-plume-sdl.patch
                       SDL 2.0.22 guard for older SDL2 releases
Makefile               assemble, link and recompile
PLAN.md                the project plan
```

## Developing

Build from source: [docs/DEVELOPING.md](docs/DEVELOPING.md). Package a release:
[docs/DISTRIBUTION.md](docs/DISTRIBUTION.md).

## Legal

Ogre Battle 64 © Quest / Nintendo. This project is for preservation and
interoperability research. Do not distribute the ROM or its extracted assets.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
