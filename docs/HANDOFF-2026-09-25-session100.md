# Handoff — 2026-09-25, session 100: the Linux release had no SDL audio backend

**Goal (developer):** GitHub issue #8, *"No audio on linux"*. The reporter runs
the 0.3.0 Linux build on CachyOS KDE (PipeWire, a Focusrite 18i20 interface) and
on a fresh Fedora mini PC. Both print

```
[SDL] audio unavailable, running silently: dsp: No such audio device
```

at boot and then repeat `[SDL] Failed to open audio device: Audio subsystem is
not initialized` for the whole run, with no sound.

**Result:** the shipped binary, and not either machine. The v0.3.0 Linux package
contains a statically linked SDL2 2.32.10 whose only real audio driver is OSS
(`/dev/dsp`); ALSA, PulseAudio and PipeWire are absent from it. Neither machine
has `/dev/dsp`, so `SDL_InitSubSystem(SDL_INIT_AUDIO)` fails and every later
`SDL_OpenAudioDevice` returns "Audio subsystem is not initialized". The cause is
the build host, not the code: SDL2's CMake drops an audio backend whose
development headers are missing **without failing**, and the OSS header is the
one that is always present. `make sdl2-static` now states the three Linux
backends, verifies the built archive with the new `tools/check-sdl2-audio.sh`,
and rebuilds from scratch when a cached copy fails; `tools/smoke-dist.sh` now
fails a Linux package whose binary carries none of the three. No game or runtime
code changed.

---

## 1. The evidence, in the order it was obtained

1. **The released binary.** `strings` on
   `ogre-battle-64-recomp-linux-x86_64.tar.gz` from release `v0.3.0`
   (`ogrebattle64`, ELF x86-64, `SDL-release-2.32.10-0-g5d2495703`):

   | string | present |
   |---|---|
   | `OSS /dev/dsp standard audio` | yes |
   | `SDL dummy audio driver` | yes |
   | `direct-to-disk audio` | yes |
   | `dsp: No such audio device` | yes |
   | `ALSA PCM audio` | **no** |
   | `PulseAudio` | **no** |
   | `Pipewire` | **no** |
   | `libasound` / `libpulse` / `libpipewire` | **no** |

   `DSP_bootstrap` is the last non-`demand_only` entry in SDL2's bootstrap array
   (`SDL2-2.32.10/src/audio/SDL_audio.c` line 44; `disk` and `dummy` carry
   `demand_only = SDL_TRUE`), so an OSS-only build reports the OSS driver's error
   and nothing else. `SDL2-2.32.10/src/audio/dsp/SDL_dspaudio.c` line 293 is the
   exact `SDL_SetError("dsp: No such audio device")` from the report, reached
   when `SDL_EnumUnixAudioDevices` finds no `/dev/dsp*`.

2. **The build condition, reproduced.** `tools/SDL2-static/SDL2-2.32.10` was
   configured and built twice in a bare `ubuntu:24.04` container, once before and
   once after installing the audio headers:
   - no audio `-dev` packages: `libSDL2.a` defines `DSP_bootstrap`,
     `DUMMYAUDIO_bootstrap`, `DISKAUDIO_bootstrap` and no other audio bootstrap;
   - with `libasound2-dev libpulse-dev libpipewire-0.3-dev`: it also defines
     `ALSA_bootstrap`, `PULSEAUDIO_bootstrap` and `PIPEWIRE_bootstrap`.

   That symbol set is exactly the string set of the shipped binary, so the
   package was built on a host without the headers.

3. **The failure is silent, and the cache misreports it.** On the bare host
   SDL2's configure prints

   ```
   -- Could NOT find ALSA (missing: ALSA_LIBRARY ALSA_INCLUDE_DIR)
   --   SDL_ALSA                    (Wanted: ON): OFF
   --   SDL_PIPEWIRE                (Wanted: ON): OFF
   --   SDL_PULSEAUDIO              (Wanted: ON): OFF
   --   SDL_SNDIO                   (Wanted: ON): OFF
   ```

   and exits 0, while `CMakeCache.txt` still contains `SDL_ALSA:BOOL=ON`,
   `SDL_PIPEWIRE:BOOL=ON` and `SDL_PULSEAUDIO:BOOL=ON`. `SDL_ALSA`,
   `SDL_PULSEAUDIO` and `SDL_PIPEWIRE` are `set_option`/`dep_option` defaults
   (`CMakeLists.txt` lines 443/449/451), and the "not found" path writes a
   *normal* variable that shadows the cache entry. **A check on the configure
   summary or the cache is therefore not a check**; the built archive is the only
   place the answer is recorded. This is why `tools/check-sdl2-audio.sh` reads
   symbols out of `libSDL2.a`.

4. **The reporter's machines were never the problem.** Both fail with the same
   OSS error, and neither is consulted by SDL before that error is set: the
   bootstrap array is a compile-time list, and a driver that is not compiled in
   cannot be tried. `SDL_AUDIODRIVER=alsa` and `SDL_AUDIODRIVER=pulseaudio`
   cannot help an affected user either, for the same reason.

5. **The reference.** Zelda64Recomp links the **system** SDL2 on Linux
   (`Zelda64Recomp/Zelda64Recomp` `CMakeLists.txt`: `find_package(SDL2 REQUIRED)`
   and `SDL2::SDL2`; the fetched prebuilt SDL2 is the Windows branch only), and
   its Linux CI installs `libsdl2-dev` before building SDL2 from source for the
   ARM64 job (`validate.yml`, "Install Linux Dependencies"). A distribution's
   SDL2 is always built with the audio backends, which is why that project never
   met this failure. Its `libsdl2-dev` also depends on `libasound2-dev` and
   `libpulse-dev` on Ubuntu 24.04, so installing the SDL2 development package is
   enough to give a from-source static build its audio backends.

## 2. What changed

- `tools/check-sdl2-audio.sh` (new). Takes `libSDL2.a` and exits non-zero unless
  it defines `ALSA_bootstrap`, `PULSEAUDIO_bootstrap`, `PIPEWIRE_bootstrap`,
  `COREAUDIO_bootstrap`, `WASAPI_bootstrap`, `DSOUND_bootstrap`,
  `AAUDIO_bootstrap` or `openslES_bootstrap`. `dsp` is deliberately absent. An
  *undefined* (`U`) reference to one of those symbols does not count, because the
  object that fills SDL2's bootstrap array also references each enabled backend;
  the type field of the `nm` line is what distinguishes "compiled in" from
  "referenced". The failure message lists the compiled-in drivers and the
  per-distribution package names.
- `Makefile`. `sdl2-static` now (a) asks for `-DSDL_ALSA=ON
  -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON` on `DIST_OS=linux`, (b) treats a cached
  archive that fails the audio check as not built, removes `prefix/` and
  `build/`, and rebuilds, and (c) fails the target when the fresh build still has
  no backend. The header comment and the per-platform release table name the
  packages.
- `tools/smoke-dist.sh`. The Linux branch greps the packaged binary for
  `ALSA PCM audio`, `PulseAudio` or `Pipewire` and fails the release without one.
  The check is separate from the `ldd` check, so it runs where `ldd` is absent.
- `app/src/sdl_platform.cpp`. The audio-failure line now appends the compiled-in
  driver list: `... (3 drivers compiled in: dsp, disk, dummy)`. This is the line
  that would have turned issue #8 into a one-line answer.
- `.github/workflows/release.yml`. The hosted Linux dependency step installs
  `libasound2-dev libpulse-dev libpipewire-0.3-dev` beside the existing packages.
- `docs/guides/app-build.md`, `PLAN.md`, `docs/DECISIONS.md`,
  `docs/STATUS-LOG.md`, `docs/README.md` and this file.

No generated file, no recompiler input, no submodule and no ROM-derived file was
touched. No probe was added, so nothing needed reverting. `tools/RT64` and its
`plume` submodule were already modified in the working tree at session start
(session 99's handoff records the same) and are not part of this change.

## 3. Verification

- **Released artifact, negative.** `tools/smoke-dist.sh` against a directory
  holding the real `v0.3.0` `ogrebattle64` and its `README.txt`:
  `FAIL: the binary carries no ALSA, PulseAudio or PipeWire SDL audio backend
  (issue #8)`, exit 1. The check finds the actual shipped defect.
- **Smoke check, positive.** The same directory with `ALSA PCM audio` appended to
  the binary: `smoke-dist: the binary carries a Linux SDL audio backend`, then
  `PASS (static checks only)` on this macOS host, exit 0.
- **`check-sdl2-audio.sh`, positive.** Against `tools/SDL2-static/prefix/lib/
  libSDL2.a` (the macOS build): exit 0, via `_COREAUDIO_bootstrap`.
- **`check-sdl2-audio.sh`, negative.** Against an archive defining only
  `DSP_bootstrap`, `DUMMYAUDIO_bootstrap` and `DISKAUDIO_bootstrap` — the
  v0.3.0 driver set: exit 1, with the driver list and the package names on
  stderr.
- **`check-sdl2-audio.sh`, undefined reference is not a backend.** An archive
  that defines the three OSS-era symbols and additionally carries `U
  ALSA_bootstrap` (a reference with no definition): also exit 1, and the message
  does not list ALSA. Without the symbol-type test this case would have passed.
- **`check-sdl2-audio.sh` under GNU `nm` and `awk`.** The same three archives
  built with `gcc`/`ar` inside `ubuntu:24.04` give the same three results
  (fail / fail / pass with `ALSA_bootstrap` defined), so the script is not
  relying on Mach-O's leading underscore or on BSD `awk`.
- **`make sdl2-static` fast path.** With the macOS prefix present:
  `==> static SDL2 already built`, exit 0 (the audio check runs and passes
  inside it).
- **`make sdl2-static` stale-cache path.** An isolated copy of the `Makefile`
  with `SDL2_PREFIX`/`SDL2_STAMP`/`SDL2_TARBALL` overridden onto a fake
  OSS-only prefix: the check fails, the target prints `==> cached static SDL2 is
  unusable; rebuilding from scratch`, removes the prefix, fails the fetch, and
  exits non-zero. The bad cache is not silently reused.
- **`app/src/sdl_platform.cpp` compiles.** Compiled with its own recorded flags
  from `build-dist/compile_commands.json` (`-fsyntax-only`): exit 0, no
  diagnostics. `cmake --build build-dist --target ogrebattle64` relinked the app.
- **The build variables.** `libsdl2-dev` on Ubuntu 24.04 `Depends:
  libasound2-dev`, `libpulse-dev`, `libsndio-dev` (checked with `apt-cache
  depends` in the same container).

## 4. What was not verified, and what is left

- **No Linux host and no Linux run.** Every check above is static, or runs
  against the released binary. A rebuilt Linux package was not produced here, and
  no run has played the game with sound. The mechanism is established from the
  release's own strings and from a reproduction of its build condition, but the
  end-to-end "audio is audible" step needs a Linux machine.
- **The 0.3.0 Linux archive on the releases page is still the broken one.** The
  fix repairs the build; a new tag has to be built and published for affected
  users. Until then the workaround is a local build with the system SDL2
  (`make app DIST_STATIC_SDL=0`, needs `libsdl2-dev`) or with the audio headers
  installed.
- **A self-hosted Linux runner needs the three packages installed before
  `make dist`.** `docs/guides/app-build.md`'s per-platform table now says so, and
  `make sdl2-static` fails with the package names when they are missing. The
  release workflow installs them on the GitHub-hosted path only, because the
  self-hosted path skips that step by design.
- **`SDL_ALSA_SHARED`/`SDL_PULSEAUDIO_SHARED`/`SDL_PIPEWIRE_SHARED` default to
  ON**, so the static SDL2 `dlopen`s the runtime libraries and the package stays
  one file. This was read from SDL2 2.32.10's `CMakeLists.txt` and was not
  measured on a Linux package: the next Linux release should confirm with
  `ldd`.
- **The web build's audio** is a separate open item (`PLAN.md` open work 8) and
  is not touched here.

## 5. Files changed

- `tools/check-sdl2-audio.sh` (new, executable).
- `tools/smoke-dist.sh` — the Linux audio-backend check.
- `Makefile` — `sdl2-static` verification, rebuild-on-failure, header comment.
- `app/src/sdl_platform.cpp` — the driver list in the audio-failure line.
- `.github/workflows/release.yml` — Linux audio development packages.
- `docs/guides/app-build.md`, `PLAN.md`, `docs/DECISIONS.md`,
  `docs/STATUS-LOG.md`, `docs/README.md`,
  `docs/HANDOFF-2026-09-25-session100.md`.

`docs/README.md` also gained the missing session-99 row, which the index had
skipped while still marking session 98 as "Latest."
