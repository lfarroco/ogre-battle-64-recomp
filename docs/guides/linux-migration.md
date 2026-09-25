# Migrating the dev environment to Linux (Ubuntu)

Status: **optional setup guide**, revised 2026-09-22. The native port builds and
runs on macOS (RT64/Metal), Linux (RT64/Vulkan) and Windows (RT64/D3D12 or
Vulkan), so a Linux machine is a preference, not a requirement. The original
2026-08-24 plan assumed the macOS renderer path was blocked; that was wrong, and
the two blockers were in this app's own SDL glue (a `SDL_Window*` passed where
plume requires an `NSWindow*`, and `SDL_PumpEvents` called from the game thread).

## When to switch

- **For RT64's best-tested backend.** Vulkan is RT64's primary path.
- **For Linux release parity**, or to reproduce the Linux leg of
  `.github/workflows/release.yml`.
- **Not required for any feature.** The recompiler, the runtime and the app are
  platform-independent and build on all three platforms.

---

## Ubuntu prerequisites

Ubuntu 22.04 or 24.04 (x86_64). Packages:

```sh
sudo apt update
sudo apt install -y \
    build-essential cmake clang \
    gcc-mips-linux-gnu \        # provides mips-linux-gnu-as/ld/objcopy
    libsdl2-dev \
    libvulkan-dev vulkan-tools  # RT64 Vulkan backend
    zlib1g-dev \
    git python3 python3-venv

# GPU driver: NVIDIA (proprietary) or AMD (mesa-vulkan-drivers is usually
# installed already). Verify with:  vulkaninfo | head
```

Toolchain setup (same as the macOS flow, see `PLAN.md` "Reproduce"):

```sh
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git tools/N64Recomp
git -C tools/N64Recomp apply ../../patches/n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j
```

Note: `tools/N64Recomp`, `tools/N64ModernRuntime`, `tools/RecompFrontend` are
gitignored vendored clones, so they are NOT in the repo — clone them again (or
copy the tree over). `tools/RT64` **is** a git submodule and comes with the repo
(`git submodule update --init --recursive`).

After checking out RT64, apply the SDL compatibility patch to its `plume`
submodule (Ubuntu 22.04 ships SDL 2.0.20; `SDL_GetWindowSizeInPixels` needs
≥ 2.0.22):

```sh
git submodule update --init --recursive
git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch
```

`git submodule update` resets `tools/RT64` and its nested submodules, discarding
the patch — re-apply it whenever RT64 is re-checked-out.

## Toolchain mapping (macOS → Ubuntu)

| Tool | macOS (Homebrew) | Ubuntu (apt) |
|---|---|---|
| MIPS binutils | `mips-linux-gnu-binutils` | `gcc-mips-linux-gnu` |
| CMake | `cmake` | `cmake` |
| C/C++ compiler | Apple clang 17 | `clang` or `gcc` |
| SDL2 | `brew install sdl2` (sdl3 + sdl2-compat) | `libsdl2-dev` |
| zlib | `brew install zlib` | `zlib1g-dev` (system) |
| splat / spimdisasm | `tools/venv` + `pip install 'splat64[mips]'` | same |

---

## macOS-specific code in the app

The app still carries macOS-only branches. All are guarded, and none has to be
removed for a Linux build:

- `app/CMakeLists.txt` links the macOS frameworks only under `if (APPLE)`; Linux
  takes the `elseif (UNIX AND NOT APPLE)` branch (`Threads` and `${CMAKE_DL_LIBS}`).
- `app/src/sdl_platform.cpp`'s `create_window` builds the Metal window handle
  (`SDL_GetWindowWMInfo`, `SDL_Metal_CreateView`, `SDL_Metal_GetLayer`) under
  `#if defined(__APPLE__)`; on Linux it passes the `SDL_Window*` through.
- `poll_input` must stay a no-op on every platform: pumping SDL events off the
  main thread terminates the process on macOS, and the main thread already pumps
  in `pump_sdl_events`. Do not "restore" the `SDL_PumpEvents()` call.

---

## Build & run (Ubuntu)

```sh
# ROM (copy your dump over; big-endian .z64)
# assets/ogre64.z64

# regenerate the recompiled code (see docs/guides/app-build.md)
make regenerate

# build the app
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j$(nproc)

# run
./build-app/ogrebattle64 assets/ogre64.z64
```

---

## Gotchas

1. **RT64 submodule pinned commits** (imgui, xxHash) may not check out with a
   plain `git submodule update`. Fix:
   ```sh
   git -C tools/RT64/src/contrib/imgui fetch origin <pinned-sha>
   git -C tools/RT64/src/contrib/imgui checkout <pinned-sha>
   ```
2. **Vendored clones** (`tools/N64Recomp`, `tools/N64ModernRuntime`) are
   gitignored — re-clone + re-apply `patches/n64recomp-ob64.patch`.
3. **Sticky CMake cache**: after toolchain changes, delete `build-app` and
   reconfigure.
4. **SDL2**: use the distro `libsdl2-dev`; Homebrew's `sdl2` is now `sdl2-compat`
   (an SDL3 shim), which works but is not the combination RT64 is tested against.
5. **`RecompiledFuncs/` is generated** (gitignored) — regenerate with
   `make regenerate`; never hand-edit.
6. The `patches/n64recomp-ob64.patch` applies to a specific upstream N64Recomp commit;
   if upstream has moved, re-derive against the vendored clone's current state.
7. `git submodule update` (or a fresh `git submodule update --init --recursive`)
   resets `tools/RT64` and discards the plume SDL patch — re-apply with
   `git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch`.
