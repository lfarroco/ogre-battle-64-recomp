# Ogre Battle 64: Person of Lordly Caliber (PC port project)

Static recompilation of the N64 game *Ogre Battle 64: Person of Lordly Caliber*
(USA, Rev A) to a native PC executable, using the [N64Recomp] toolchain.

**This repository contains no copyrighted game data.** You must supply your own
ROM dump (see below).

## Status

The main code segment (3659 functions) has been fully recompiled to C. The runtime
app (rendering, input, audio) is the next milestone. See [PLAN.md](PLAN.md) for the
full plan, current status, and technical findings.

## Directory layout

```
assets/                your ROM (gitignored; big-endian .z64 expected)
asm/                   splat-generated disassembly
config.yaml            splat config (segments, vram mapping)
config.toml            N64Recomp config
Makefile               assemble + link + recompile
n64recomp-ob64.patch   our N64Recomp modifications (apply to upstream clone)
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

# ROM: put your byte-swapped .n64 dump in assets/, then
tools/venv/bin/splat split config.yaml && make && make recomp
```

The ROM must be the USA Rev A dump (40 MB, `.n64` 16-bit byte-swapped or already
converted `.z64`). `tools/convert_rom.py` converts `.n64` → `.z64`.

## Legal

Ogre Battle 64 © Quest / Nintendo. This project is for preservation and
interoperability research. Never distribute the ROM or its extracted assets.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
