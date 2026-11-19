# Distribution

```sh
make dist        # -> dist/ogre-battle-64-recomp/
make dist-zip    # -> dist/ogre-battle-64-recomp-<platform>.zip
```

The package is one executable with SDL2 linked statically. `make dist` fetches
and builds a pinned SDL2 once, because Homebrew's `sdl2` is the SDL3-based
compatibility shim and has no static library.

The package contains no game data. The player loads their own ROM on the start
screen, or places it next to the executable.

The release workflow, the mods bundle and the per-platform packaging details are
in [the build guide](guides/app-build.md) → "Distribution — `make dist`".
