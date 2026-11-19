# Developing

Build instructions for the PC port from a fresh clone. The full reference, with
every `OGRE_*` knob and the diagnostics toolkit, is
[guides/app-build.md](guides/app-build.md).

## Prerequisites

- macOS: the Xcode command line tools, Homebrew, and the Metal toolchain on
  Xcode 26+.
- Linux: the packages in [the Linux setup guide](guides/linux-migration.md).
- CMake 3.20 or newer, Python 3, SDL2 and `mips-linux-gnu-binutils`.
- Your own Ogre Battle 64 (USA, Rev A) dump, 40 MB.

## Set up the toolchain

The commands below are for macOS. On Linux, replace `brew install` with the
distribution packages and skip the `xcodebuild` line.

```sh
brew install mips-linux-gnu-binutils cmake sdl2
# Xcode 26+: `metal` is a separate download; skip if `xcrun --find metal` works
xcodebuild -downloadComponent MetalToolchain
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'

# recompiler
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../patches/n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j4

# runtime
git clone https://github.com/N64Recomp/N64ModernRuntime.git tools/N64ModernRuntime
git -C tools/N64ModernRuntime checkout 589bbf0
git -C tools/N64ModernRuntime submodule update --init --recursive
git -C tools/N64ModernRuntime apply ../../patches/n64modernruntime-ob64.patch
git -C tools/N64ModernRuntime/N64Recomp apply ../../../patches/n64modernruntime-n64recomp.patch

# renderer
git submodule update --init --recursive
git -C tools/RT64 apply ../../patches/rt64-ob64.patch
git -C tools/RT64/src/contrib/plume apply ../../../../../patches/rt64-plume-ob64.patch
```

`tools/N64Recomp` and `tools/N64ModernRuntime` are gitignored clones.
`tools/RT64` is the only submodule.

## Generate the recompiled code

Put the dump at `assets/ogre64.z64`. `tools/convert_rom.py` converts a 16-bit
byte-swapped `.n64` to `.z64`.

```sh
make regenerate
```

`make regenerate` runs splat, the MIPS link, the 34 bank units, the main
recompilation and the RSP microcode. A fresh clone cannot build the app until
this has run once, because the generated C is not committed.

## Build and run the app

```sh
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
./build-app/ogrebattle64
```

The app opens on its start screen, with `START GAME`, `MODS`, `CONTROLS`,
`SETTINGS` and `DEBUG` tabs. Load a ROM with `SPACE`, by dropping it on the
window, or by placing it next to the executable. `ESC` opens the same panel over
the running game. The ROM is validated by hash and stored, so later launches go
straight into the game. Battery saves go in `saves/` beside the executable.

## SDL older than 2.0.22

`tools/rt64-plume-sdl.patch` adds a guard for SDL versions older than 2.0.22
(Ubuntu 22.04 ships 2.0.20). Apply it after the submodule checkout:

```sh
git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch
```

A `git submodule update` inside `tools/RT64` discards the patch, so re-apply it.
