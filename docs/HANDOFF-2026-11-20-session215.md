# Session 215 handoff — the wasm job's emsdk 7.0.0 failure

> **Summary.** Session 215 handoff — **the wasm job's emsdk 7.0.0 failure.** `emsdk install latest` moved from `eb13275…` to Emscripten 7.0.0 (`6afbd83…`), whose libc++ stopped declaring `malloc`/`free` through `<stdexcept>`; rabbitizer's `InstructionBase.cpp` calls them with only that header, so the workflow failed there. `app/CMakeLists.txt` now pre-includes `<cstdlib>` for the `rabbitizer` target's C++ sources only, because the same target compiles C sources and the whole-target form failed on them. Verified against Emscripten 7.0.0 installed locally: the file reproduces the CI's two errors without the flag and compiles with it, and the full browser target reaches `[100%] Built target ogrebattle64`; `make app` and `make web` are unaffected on the local 6.0.8-git.

## Goal

The push that closed issue #27 (session 214) passes every check except the
WebAssembly workflow: job `135819634389` of run `44597497797` failed in
"Configure and build". Fix the browser target's CI build.

## Result

The failure is not in this port's sources. The workflow installs the emsdk
release that `latest` names, and between the last green run (2026-11-03) and
this one (2026-11-19) that moved to Emscripten 7.0.0:

- green run `41634572421`: `emsdk install latest` resolved to
  `eb132757325583b787fe9414b09afea9442b72ad`.
- failing run `44597497797`: it resolved to
  `6afbd83493d647051772d491124187d4e9a0cda9`, which emsdk names `7.0.0`.

Emscripten 7.0.0's libc++ no longer declares `malloc`/`free` transitively
through `<stdexcept>`, and rabbitizer's
`N64Recomp/lib/rabbitizer/cplusplus/src/instructions/InstructionBase.cpp`
calls both with only that header included. The job stopped there, so nothing
after rabbitizer was compiled.

Fix: `app/CMakeLists.txt` pre-includes `<cstdlib>` for the `rabbitizer`
target's C++ sources, beside the existing `APPLE` force-include that libc++ 21
made necessary for RT64's `labs()` (commit `17afd71`).

## Evidence

The failing job's error, from `gh run view 44597497797 --job 135819634389
--log-failed`:

```
InstructionBase.cpp:978:22: error: use of undeclared identifier 'malloc'
InstructionBase.cpp:986:5: error: use of undeclared identifier 'free'
...
gmake[3]: *** [.../rabbitizer.dir/.../InstructionBase.cpp.o] Error 1
```

The emsdk resolution, from the same log:

```
Resolving SDK version '7.0.0' to 'sdk-releases-6afbd83493d647051772d491124187d4e9a0cda9-64bit'
```

Only one file in rabbitizer uses `malloc`/`free` without `<cstdlib>` or
`<stdlib.h>`: a loop over `cplusplus/src` and `cplusplus/include` reports
`no cstdlib: cplusplus/src/instructions/InstructionBase.cpp` and nothing else.

The fix was verified against the CI's toolchain, not the local one. The local
emsdk is Homebrew Emscripten 6.0.8-git, whose headers still declare
`malloc`/`free` through `<stdexcept>`, so it cannot reproduce the failure.
Emscripten 7.0.0 was installed under `/private/tmp/emsdk700` with
`emsdk install 7.0.0`; what it showed:

- The single translation unit, compiled with the target's include paths:
  without the flag, `use of undeclared identifier 'malloc'` and `'free'` at
  lines 978/986, the CI error; with `-include cstdlib`, 0 errors and an object
  file.
- The whole browser target: `emcmake cmake -S app -B /private/tmp/ws27/bw700b`
  then `cmake --build ... -j8` finishes `[100%] Built target ogrebattle64`
  with 0 `error:` lines and writes a 21.3 MB `ogrebattle64.wasm`. `rabbitizer`
  builds, and so does everything after it.

Two details found on the way:

- The first attempt passed `SHELL:-include cstdlib` to the whole target. That
  target also compiles C sources (`lib/rabbitizer/src/...`), and `-include
  cstdlib` has no meaning in C: the C files failed with `'cstdlib' file not
  found`. The option is now `"$<$<COMPILE_LANGUAGE:CXX>:-include;cstdlib>"`, so
  `C_FLAGS` is unchanged and `CXX_FLAGS` carries `-include cstdlib`.
- On macOS, a scratch emsdk at `/tmp/emsdk700` (resolving to
  `/private/tmp/emsdk700`) with `EM_CACHE` under `/tmp` fails at the link with
  `clang: error: no such file or directory: '../../../../../../private/tmp/.../emscripten_tls_init.c'`.
  Emscripten 7.0.0 generates its cached system libraries with a relative path,
  so both paths must resolve under the same `/tmp` form. Re-running with
  `/private/tmp/...` for both builds cleanly.

## What changed

- `app/CMakeLists.txt`: the `rabbitizer` pre-include and its comment, after
  `add_subdirectory(N64ModernRuntime)` and before the app's own targets. It is
  limited to `Clang|GNU` because cl.exe rejects `-include` and declares
  `malloc()` through `<stdexcept>` on every supported version.
- `docs/guides/app-build.md`: the emsdk-`latest` caveat and the macOS
  `/tmp`/`/private/tmp` rule, beside the `EM_CACHE` paragraph.
- `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` and this handoff.

## Verification

- Emscripten 7.0.0: the translation-unit test and the full browser build above.
- Local toolchains: `make app` and `make web` (Emscripten 6.0.8-git) both
  build; the pre-include is inert there.
- `rabbitizer`'s `C_FLAGS` line in `build-wasm` has no `-include`, and its
  `CXX_FLAGS` line ends in `-include cstdlib`.
- The CI run after the push is the final check; this handoff was written before
  it completed.

## Open

- The workflow pins nothing but the N64ModernRuntime commit, so the next
  `latest` move can do this again elsewhere in the third-party tree. Pinning
  `version:` in `.github/workflows/wasm.yml` to a release would trade that
  early warning for reproducibility; the repository currently chooses `latest`.
