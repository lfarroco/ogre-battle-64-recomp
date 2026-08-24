# Migrating the dev environment to Linux (Ubuntu)

Status: **planned** (2026-08-24). The dev machine is currently an Intel Mac
(Xcode 26.2, Apple clang 17, Homebrew SDL2/sdl2-compat). The next session moves
to Ubuntu. This guide lists what to install, what macOS-specific code to remove,
and the gotchas.

---

## When to switch

- **Not needed for the libultra-bridging milestone** (`docs/LIBULTRA-BRIDGING.md`):
  that work is CPU-side and platform-independent. The SIGBUS crash reproduces
  identically on Linux.
- **Recommended for the RT64/renderer phase** (RSP microcode + RT64 integration):
  RT64's Vulkan backend is the primary, most-tested path, and the whole
  N64Recomp/N64ModernRuntime/RT64 ecosystem is Linux/Windows-first.

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
git -C tools/N64Recomp apply ../../n64recomp-ob64.patch
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build --target N64RecompCLI -j
```

Note: `tools/N64Recomp`, `tools/N64ModernRuntime`, `tools/RecompFrontend` are
gitignored vendored clones, so they are NOT in the repo — clone them again (or
copy the tree over). `tools/RT64` **is** a git submodule and comes with the repo
(`git submodule update --init --recursive`).

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

## macOS-specific code to remove

These are the only macOS-specific pieces in our own `app/` source. They are
guarded by `APPLE`/`__APPLE__`, so they do **not** break the Linux build on
their own — removing them is cleanup (and avoids the Metal/`SDL_Metal_GetLayer`
workaround path entirely).

### 1. `app/CMakeLists.txt`

- **Delete** the `APPLE` deployment-target block (lines 12-14):
  ```cmake
  if (APPLE)
      set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
  endif()
  ```
- **Update** the comment on line 36 (RT64 backend note) — on Linux
  `RT64_SDL_WINDOW_VULKAN` is meaningful (creates a Vulkan surface from the SDL
  window), not the Metal auto-select.
- **Fix** the stale comment on line 52: `3659 functions` → `807 functions`.
- **Restructure** the platform link block (lines 101-110). On Linux the
  `elseif (UNIX AND NOT APPLE)` branch is the active one; simplify to:
  ```cmake
  if (UNIX AND NOT APPLE)
      find_package(Threads REQUIRED)
      target_link_libraries(ogrebattle64 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  ```
  (the `if (APPLE) ... -framework Cocoa -framework Metal -framework QuartzCore`
  branch becomes dead and can be deleted).

### 2. `app/src/sdl_platform.cpp`

- **Simplify `create_window`** (lines 44-58): the `#if defined(__APPLE__)`
  block constructs a `WindowHandle{window, nullptr}` and is a workaround for
  `SDL_Metal_GetLayer` segfaulting under Homebrew sdl2-compat. On Linux the
  Vulkan path just passes the `SDL_Window*`:
  ```cpp
  platform.window = window;
  return window;
  ```
  Delete the `#if defined(__APPLE__) ... #else ... #endif` entirely.

### 3. `tools/` (macOS-only build artifacts, not committed)

- **Delete** `tools/RT64/src/contrib/dxc/lib/x64/libz.dylib` — an untracked
  symlink to Homebrew zlib created to fix the macOS `dxc` shader-compiler abort.
  On Linux RT64 uses `dxc-linux` (or glslang/SPIRV-Cross), so this is
  unnecessary.
- **Delete** the `.DS_Store` files (cosmetic).

### 4. System-level (not in the repo)

- The Metal toolchain component (`xcodebuild -downloadComponent MetalToolchain`)
  is not needed — RT64 uses **Vulkan** on Linux.
- Homebrew sdl2-compat / the `SDL_Metal_GetLayer` segfault are gone with
  distro SDL2.
- If the RT64 shader pipeline hits issues, install the relevant drivers:
  NVIDIA proprietary driver (recommended) or `mesa-vulkan-drivers` for AMD/Intel.

---

## Build & run (Ubuntu)

```sh
# ROM (copy your dump over; big-endian .z64)
# assets/ogre64.z64

# regenerate recompiled code (after any symbol_addrs.txt changes)
tools/venv/bin/splat split config.yaml
make
make recomp

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
   gitignored — re-clone + re-apply `n64recomp-ob64.patch`.
3. **Sticky CMake cache**: after toolchain changes, delete `build-app` and
   reconfigure.
4. **SDL2**: use the distro `libsdl2-dev`, not SDL3/sdl2-compat, so the
   `SDL_Metal_GetLayer`-style issues can't recur.
5. **`RecompiledFuncs/` is generated** (gitignored) — regenerate with
   `make recomp`; never hand-edit.
6. The `n64recomp-ob64.patch` applies to a specific upstream N64Recomp commit;
   if upstream has moved, re-derive against the vendored clone's current state.

