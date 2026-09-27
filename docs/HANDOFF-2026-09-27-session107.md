# Handoff — 2026-09-27, session 107: the v0.4.0 Windows access violation

**Goal (developer):** the pasted crash report, with no other text —

```
time: 2026-09-27T04:33:44Z
os: windows/x86_64
reason: unhandled exception 0xC0000005
rdram base: 0x2A018880000
fault instruction: 0x7FF644E101AE (...\ogrebattle64.exe+0x10001AE)
fault address: 0x6C
access: read
--- guest diagnostics ---
n64 thread: -1
```

and, after the answer, the note that **v0.3.0 worked on Windows**.

**Result:** the crashing build is v0.4.0, and the fault is
`plume::d3d12::D3D12CommandList::setSamplePositions(nullptr)`: `rdx` is the
`texture` argument and the read of `0x6C` is
`texture->desc.multisampling.sampleLocationsEnabled`. The null comes from
`copyTextureRegion` called with a `PlacedFootprint` (buffer) destination — a
location that carries no texture — and in this tree the only such call is the
port's own presented-frame readback in `rt64_present_queue.cpp`. That readback
ran on the first present on Windows because `init_capture_env` installs
`OGRE_CAPTURE_PRESENT` with `putenv` and then hides it by rewriting the first
byte of the name: POSIX `putenv` stores the caller's pointer, the UCRT's
`_putenv` copies the string and owns the copy, so the entry stayed visible
there. Fixed by giving the port an app-owned capture path
(`ogre_present_capture_path()`) that `snap` toggles without touching the
environment, and by skipping `setSamplePositions` for a destination that is not
a texture. A `tools/smoke-dist.sh` check now fails a package whose capture is on
without `OGRE_CAPTURE_PRESENT`.

---

## 1. The report names one build

The release asset `ogre-battle-64-recomp-windows-x86_64.zip` for **v0.4.0**
(`gh release download v0.4.0`, 24,655,872 bytes) has `.text` at RVA
`0x1000..0x12D3066`, so `ogrebattle64.exe+0x10001AE` is real code. Its `.pdata`
(17,312 entries) puts the containing function at RVA `0x10001A0..0x100032A`
(size `0x18A`), and the bytes at `0x10001AE` are `80 7A 6C 01` =
`cmpb $1, 0x6C(%rdx)`.

That instruction occurs **once** in the whole image, and in every candidate
release it is the `+0xE` instruction of a function:

| build | function RVA |
|---|---|
| v0.2.1-rc4, v0.2.1-rc5 | `0xFF8AC0` |
| v0.3.0 | `0xFF8ED0` |
| v0.4.0 | `0x10001A0` |

The report's `0x10001AE` is `0x10001A0 + 0xE`, so the crashing build is v0.4.0
and no other release. (Downloaded with `gh release download`; the comparison is
the byte pattern in each `.text`, not the offset alone.)

## 2. The function is `D3D12CommandList::setSamplePositions`

`tools/RT64/src/contrib/plume/plume_d3d12.cpp:2483`, called with `texture ==
nullptr`. The body matches instruction for instruction:

* `cmpb $1, 0x6C(%rdx)` is `interfaceTexture->desc.multisampling.sampleLocationsEnabled`.
  `D3D12Texture::desc` is at `+0x40` (`toD3D12`'s subresource arm reads
  `desc.mipLevels` at `+0x40`), so `multisampling` is at `+0x48`:
  `sampleCount` `+0x48`, `sampleLocations[16]` `+0x4C` (2-byte
  `RenderMultisamplingLocation`), `sampleLocationsEnabled` `+0x6C`.
* `movl 0x48(%rdx), %eax` is `sampleCount`; the resize and the copy loop have a
  2-byte element stride, which is `D3D12_SAMPLE_POSITION` = `{ INT8 X; INT8 Y; }`.
* The thread-local `std::vector<D3D12_SAMPLE_POSITION>` sits at TLS `+0x770`
  (begin/end/capacity) with its guard byte at `+0x788`.
* `callq *0x1F8(%rax)` on `this+0x10` (`d3dV1`) is `SetSamplePositions(size, 1, data)`;
  the `(0, 0, nullptr)` call and the clears of `this+0x64`
  (`activeSamplePositions`) and `this+0x38` (`targetFramebufferSamplePositionsSet`)
  are the inlined `resetSamplePositions`.

## 3. Which caller can pass null

Eight sites call it. Four are inlined `checkFramebufferSamplePositions`, and
each guards both `targetFramebuffer` and `targetFramebuffer->depthTarget` before
the call (`cmpb`/`testq`/`je` at `0x1000525`, `0x10005e5`, `0x1001380`,
`0x10015d5`). The unguarded four are `copyTexture` (`0x1001A90`, then
`CopyResource` at vtable slot `0x88`), `resolveTexture` (`0x1001AF0`, then
`ResolveSubresource` at `0x98`), `resolveTextureRegion` (`0x1001B70`) and
`copyTextureRegion` (`0x1001880`, then `CopyTextureRegion` at `0x80`). The
`assert(texture != nullptr)` in each is compiled out in Release.

For `copyTextureRegion` the null can only be a **buffer** destination. The
compiled order calls `toD3D12` for both locations first (a separate function at
`0x1001990`), whose `SUBRESOURCE` arm reads `location.texture->desc.mipLevels`
at `+0x40`; a null subresource texture would fault at `+0x40`. A
`RenderTextureCopyLocation::PlacedFootprint` carries `texture == nullptr` and
takes the other arm, so the fault lands later, in `setSamplePositions`, at
`+0x6C`.

`grep PlacedFootprint(` over `tools/RT64/src` finds two calls that use one as the
**destination**, both the port's own readbacks:
`rt64_present_queue.cpp:547` (`OGRE_CAPTURE_TARGET`) and `:592`
(`OGRE_CAPTURE_PRESENT`). The texture-cache calls pass it as the source.

## 4. Why the capture ran on Windows and not on macOS

`app/src/sdl_platform.cpp` installed one static `OGRE_CAPTURE_PRESENT=/tmp/ogre-shot`
buffer with `putenv` and toggled the capture by rewriting `g_capture_env[0]`
between `'O'` and `'X'`, which requires `putenv` to keep the caller's pointer.

The UCRT does not: `_putenv` (`env/putenv.cpp`) calls
`common_putenv(option, nullptr)` → `create_environment_string`, which allocates
with `_calloc_crt_t` and `tcscpy_s`es the string, and
`common_set_variable_in_environment_nolock` takes ownership of that copy (and
frees the old one on a replace). The `g_capture_env[0] = 'X'` write therefore
never reached `getenv` on Windows, and the entry stayed
`OGRE_CAPTURE_PRESENT=/tmp/ogre-shot`. RT64's `threadPresent` reads it on **every**
present, so `ogreCapture.thisFrame` was true from the first present, and the
first present issued the footprint-destination readback.

Sources: [`putenv.cpp`](https://raw.githubusercontent.com/huangqinjin/ucrt/master/env/putenv.cpp),
[`setenv.cpp`](https://raw.githubusercontent.com/huangqinjin/ucrt/master/env/setenv.cpp),
[`_putenv` docs](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/putenv-wputenv?view=msvc-170).

This also dates the regression: `init_capture_env` came in `bb01fc6`
(2026-09-25 18:25, "widescreen mod"), which is not an ancestor of v0.3.0, and
the buffer `OGRE_CAPTURE_PRESENT=/tmp/ogre-shot` is present in the v0.4.0 exe and
in none of rc4, rc5 or v0.3.0. That matches the developer's "v0.3.0 worked".

## 5. The fix

* `tools/RT64/src/contrib/plume/plume_d3d12.cpp`: `copyTextureRegion` calls
  `setSamplePositions` only when `dstLocation.texture != nullptr`. A buffer
  destination has no sample positions; this removes the fault and makes the
  capture path usable on D3D12 at all.
* `app/src/sdl_platform.cpp`: `init_capture_env` now only reads the user's
  `OGRE_CAPTURE_PRESENT` once, `capture_enable` flips `g_capture_enabled`, and
  the new `extern "C" const char* ogre_present_capture_path()` returns the
  user's path, else `/tmp/ogre-shot` while enabled, else `nullptr`. The process
  owns that buffer for its lifetime, so a toggle can never free a string a
  present thread is reading, and the path is the same on every platform.
  `snap` reports that the environment wins when the user set it.
* `tools/RT64/src/hle/rt64_present_queue.cpp`: declares the accessor and reads
  `presentPath` from it; `OGRE_CAPTURE_TARGET` and the rest still use `getenv`.
* `app/src/main.cpp`: calls `init_capture_env()` next to `crash_log::install`,
  before the `OGRE_SMOKE` early exit, and the smoke path prints
  `[smoke] capture path: <path|(unset)>`.
* `tools/smoke-dist.sh`: fails the package when that line says anything but
  `(unset)` and the harness did not set `OGRE_CAPTURE_PRESENT`. The Windows CI
  job now catches this class without a ROM.

## 6. What was run for verification

* `cmake --build build-app -j 8`, `build-dist`, `build-null`: all clean.
  (The plume change is in the Windows-only source list, so macOS compiles the
  `copyTextureRegion` guard only in the Windows job; the accessor is
  cross-platform.)
* `tools/smoke-dist.sh dist/ogre-battle-64-recomp`: PASS, including the new
  capture check and the crash-report check. The `OGRE_SMOKE` line reads
  `[smoke] capture path: (unset)` by default and `[smoke] capture path: /tmp/x`
  with `OGRE_CAPTURE_PRESENT=/tmp/x`.
* The maintained boot battery
  (`OGRE_PREF_DIR=/tmp/s107-bat OGRE_ROM=assets/ogre64.z64 OGRE_SPEED=4
  OGRE_EXIT_AFTER_MS=30000 ./build-dist/ogrebattle64`): exit 0, 7046 log lines,
  268 display lists, `tools/runlog.py --check` PASS.
* The capture, four runs on `build-dist/ogrebattle64` at `OGRE_SPEED=4`, a
  `snap` written into the console file at 8 s. The PPM path comes from the
  capture path itself, so each case is told apart by its extension:

  | run | expectation | result |
  |---|---|---|
  | default, `snap` | capture on, `/tmp/ogre-shot.<n>.ppm` | 35 files, `snap: capturing presented frames` |
  | default, no `snap` | capture off | 0 files |
  | `OGRE_CAPTURE_PRESENT=/tmp/s107-env`, `snap` | environment wins, `/tmp/s107-env.<n>.ppm` | 178 files, `snap: the environment sets OGRE_CAPTURE_PRESENT` |
  | `OGRE_CAPTURE_PRESENT=/tmp/s107-env`, no `snap` | capture on from the environment | 85 files |

  The second row is the property that was broken on Windows: with no `snap` and
  no environment entry, nothing captures.
* Not verified: a Windows run. No MSVC or D3D12 on this machine, and the
  packaged smoke run cannot boot a ROM on CI. The check that settles it is a
  v0.4.1 (or a build of this tree) on the machine that produced the report: the
  first present must not fault.

## 7. Still open

* **The report's captured `stdout`/`stderr` sections were empty.** By the first
  present the boot log holds about 20 lines, so the capture wrote nothing. Every
  Windows report so far has been empty there, including the rc2-era pair in
  `docs/HANDOFF-2026-09-20-session94.md` §22; the Windows CI check passes
  because the runner's shell gives the process valid std handles. That is now
  the only thing between a Windows report and the device, the driver version and
  the RT64 fallback line.
* **The developer's own Windows laptop still fails on every release, v0.3.0
  included.** v0.3.0 has no capture code, so that crash is a different cause and
  the session-94 fallback lead stands. v0.4.0's fault is not that one.
* The plume guard is a defence for one path. `copyTexture`, `resolveTexture` and
  `resolveTextureRegion` still dereference their texture argument after the
  compiled-out assert; with MSAA off and no ray tracing they are not reachable
  with a null, and they were left alone rather than guarded silently.

## 8. Probes

None. The analysis is offline, on the released binary and the sources; no
generated file or app file carried a probe, and nothing had to be reverted.
`tools/RT64` shows the pre-existing dirty state (`git status` there goes from 25
to 26 modified files because of `rt64_present_queue.cpp`), and its nested
`src/contrib/plume` from one modified file (`plume_metal.cpp`) to two
(`plume_d3d12.cpp`).
