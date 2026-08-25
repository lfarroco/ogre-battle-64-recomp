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

# regenerate the recompiled code if it has changed (uses the ELF + config.toml)
make recomp

# build the app
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
```

The executable is written to `build-app/ogrebattle64`.

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

