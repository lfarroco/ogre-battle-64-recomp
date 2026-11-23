# Handoff — 2026-09-30, session 113: the v0.4.0 Windows crash is confirmed fixed by a release

> **Summary.** Session 113 handoff — **the v0.4.0 Windows crash is confirmed fixed by a release, and the D3D12 backend that faulted starts cleanly on the reporting laptop.** The developer's `error.log` (`unhandled exception 0xC0000005`, `ogrebattle64.exe+0x10001AE`, read of `0x6C`) is the **v0.4.0** release: that asset is 24,655,872 bytes, and RVA `0x10001AE` is `80 7A 6C 01` = `cmpb $1, 0x6C(%rdx)`, session 107's `plume::d3d12::D3D12CommandList::setSamplePositions` null-`texture` fault reached from the presented-frame readback, so no new cause is involved. The replacement package (`ogrebattle64.exe` 24,657,408 bytes, PE timestamp 2026-09-28) omits the pre-fix installed name `OGRE_CAPTURE_PRESENT=`, carries session 107's `capture path:` strings, and its archive carries both example mods, so it is newer than v0.4.0's empty `mods/`. A bounded run (`OGRE_EXIT_AFTER_MS=22000 OGRE_SPEED=4`, battery save copied to `%TEMP%` first) opens a responding `Ogre Battle 64: Recomp` window and loads `dxgi.dll`, `d3d12.dll` and `D3D12Core.dll` with no `vulkan-1.dll`, exits 0 and leaves `error.log` untouched — the Windows run session 107 §6 recorded as not verified, which closes `PLAN.md` item 1(a); item 1(b)'s fallback condition is absent (Intel `31.0.101.2140`, NVIDIA `32.0.15.8266` against session 94e's thresholds). Captured `stdout`/`stderr` is still 0 bytes on Windows (session 107 §7).

**Goal (developer):** the Windows package crashes as soon as a ROM is selected and
the game starts, on a laptop with a GeForce 940MX and a freshly updated driver.
`error.log`:

```
time: 2026-09-30T21:50:20Z
reason: unhandled exception 0xC0000005
rdram base: 0x20CC69F0000
fault instruction: 0x7FF6A10101AE (...\ogrebattle64.exe+0x10001AE)
fault address: 0x6C
access: read
--- guest diagnostics ---
n64 thread: -1
```

**Result:** the crashing program is the **v0.4.0** release binary, and the fault
is session 107's defect. The developer replaced the package with the newest
release, and this session verified with a Windows run that the replacement uses
the D3D12 backend that faulted in v0.4.0 and starts cleanly.

---

## 1. The crashing binary is the v0.4.0 release

The packaged `ogrebattle64.exe` in the folder the developer launched from is
24,655,872 bytes. Session 107 records that exact size for the v0.4.0 Windows
release asset.

RVA `0x10001AE` maps through `.text` (`vaddr 0x1000`, `rawptr 0x400`) to file
offset `0xFFF5AE`, whose bytes are `80 7A 6C 01` = `cmpb $1, 0x6C(%rdx)`. That is
the instruction session 107 identified as
`plume::d3d12::D3D12CommandList::setSamplePositions` with a null `texture`
argument, reached from `copyTextureRegion` with a `PlacedFootprint` (buffer)
destination. The only such call in the tree is the port's own presented-frame
readback, and in v0.4.0 it ran on the first present because `init_capture_env`
hid `OGRE_CAPTURE_PRESENT` by rewriting the first byte of a `putenv`'d name, which
the UCRT's copying `_putenv` ignores. No new cause is involved.

The report's empty `stdout`/`stderr` sections match every earlier Windows report
(session 107 §7).

## 2. The replacement package contains the fix and not the defect

Downloaded archive `%USERPROFILE%\Downloads\ogre-battle-64-recomp-windows-x86_64(1).zip`
(17,125,048 bytes, 2026-09-30 21:20). Its `ogrebattle64.exe` is 24,657,408 bytes,
PE timestamp 2026-09-28 08:17.

* The string `OGRE_CAPTURE_PRESENT=` is **absent**. That is the pre-fix marker:
  the installed environment name the mangling left visible.
* The strings `the environment sets OGRE_CAPTURE_PRESENT` and `capture path:` are
  **present**. Both were added by session 107's fix.
* The archive carries `mods/exp-overflow.nrm` and `mods/skip-boot-logos.nrm`, so it
  is built from a data bundle newer than session 108, and not v0.4.0, whose
  `mods/` is empty (session 108).

## 3. The D3D12 path runs on this machine

Run against the developer's package folder with the stored ROM,
`OGRE_EXIT_AFTER_MS=22000 OGRE_SPEED=4`:

* At 13 s the process holds a responding window titled `Ogre Battle 64: Recomp`.
* Loaded modules include `dxgi.dll`, `d3d12.dll` and `D3D12Core.dll`. `vulkan-1.dll`
  is **not** loaded, so RT64 initialized D3D12 — the backend whose
  `copyTextureRegion` faulted in v0.4.0 — and did not take a workaround fallback.
* Exit code 0 after 19.3 s. `error.log` kept its `2026-09-30 18:50:20` timestamp
  from the v0.4.0 crash, so the run wrote no report.

This is the Windows run that session 107 §6 recorded as not verified, and it closes
`PLAN.md` open-work item 1(a).

## 4. Item 1(b) no longer reproduces

`PLAN.md` item 1(b) is "the developer's Windows laptop fails on every release,
v0.3.0 included", whose lead was RT64's
`Falling back to Vulkan due to device workaround.` The fallback did not run in
§3's run. The machine's adapters are:

| adapter | driver | session-94e threshold |
|---|---|---|
| Intel HD Graphics 620 | `31.0.101.2140` | Intel 6th-gen `<= 31.0.101.2115` |
| NVIDIA GeForce 940MX | `32.0.15.8266` (582.66) | NVIDIA `<= 475.14` |

Neither arm of the workaround matches, and the developer reports updating the
driver before this test. No claim is made that the driver update is the cause;
the observation is that the condition session 94e named is not present now.

## 5. What was run for verification

Two bounded runs of the replacement package on this laptop, in the package folder
next to the ROM and `saves/`:

1. `OGRE_EXIT_AFTER_MS=18000 OGRE_SPEED=4`, output redirected to a file: exit 0 in
   19.3 s, 0 bytes captured, `error.log` untouched.
2. `OGRE_EXIT_AFTER_MS=22000 OGRE_SPEED=4` with the window and module list sampled
   at 13 s: the results in §3.

The battery save (`saves/ogrebattle64-us-rev1.bin`, 32768 bytes) was copied to
`%TEMP%\ogrebattle64-save-backup.bin` before the first run. An offline check on the
two executables supplies §1 and §2; no game or generated file was modified.

## 6. Still open

* **Windows stdout/stderr capture is still empty.** Both runs produced 0 bytes
  through `Start-Process -RedirectStandardOutput`, so the device name, the driver
  version and the fallback line remain unavailable from a report or a scripted run.
  This is session 107 §7's open item, confirmed again, and it is why §3 reads the
  loaded module list instead of the boot log.
* The fix was not re-tested against its own failure: the pre-fix binary was not run
  here with the capture forced on. Section 1 identifies it by bytes.

## 7. Probes

None. The analysis is offline (PE headers, a byte search in two executables) plus
two bounded runs; no generated file or app file carried a probe.
