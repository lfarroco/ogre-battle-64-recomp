# Session 212 handoff — issue #22 items 1 and 2

## Goal

Developer request: address items 1 and 2 of
`https://github.com/lfarroco/ogre-battle-64-recomp/issues/22`, and leave items 3
(save states), 4 (the green bars) and 5 (widescreen) alone.

1. Add a restart option next to quit and Start ROM.
2. Make the suspend (temporary) save permanent, as a mod.

The developer chose the shapes before any code was written: RESTART GAME is the
overlay's MAIN tab row next to EXIT GAME, and it relaunches the app and boots the
ROM again while skipping the start screen. The suspend save is a mod, shipped off
by default, that keeps the record when it is loaded.

## Result

Both landed. `app/src/ui.cpp` builds a `RestartGame` row only in Overlay mode,
between ROM and EXIT GAME. The row requests a restart the same way EXIT GAME
requests a quit, and `app/src/main.cpp` ends the run through that same teardown
and then starts a new copy of the executable. `mods/permanent-suspend/` is a new
code mod whose single `RECOMP_PATCH` keeps SRAM record 15 when the game loads it.

## Item 1 — RESTART GAME

### Why it is a relaunch

The runtime boots the image once. `recomp::start` returns when
`ultramodern::quit()` is called, and the game's own threads are not joined after
that: `main` leaves with `_Exit` for that reason (`app/src/main.cpp`, the block
above the final `_Exit`). A second `recomp::start` in the same process is
therefore not supported, and the game has no soft-reset path of its own.

So the row does what EXIT GAME does — set a request, let `update_gfx` call
`ultramodern::quit` — and `main` starts a new process after
`recomp::start` has returned, `overlay_shutdown()`, `speed_badge_shutdown()` and
`shutdown_sdl()` have run. The new process starts with both windows gone and the
renderer released.

### The relaunch is a spawn, not `execv`

The first implementation used `execv` and hung: in the headless test the log
stopped at `[RENDERER] null renderer shut down` and the process was still alive
45 s later, with no second boot. `execv` replaces the image while the runtime's
unjoined game threads are still running recompiled code, and the C library takes
locks during exec that those threads can hold.

`relaunch_process()` now:

* calls `ogre::crash_log::flush()`. `crash_log::install` replaces stdout and
  stderr with the write end of a pipe read by a thread in this process
  (`redirect_start` in `app/src/crash_log.cpp`). A child that inherited that pipe
  would write to it with no reader left, and every line would be lost; `flush`
  dup2s the original descriptors back (`redirect_stop`) before the spawn.
* resolves its own executable: `_NSGetExecutablePath` + `realpath` on macOS,
  `/proc/self/exe` on Linux, `GetModuleFileNameW` on Windows.
* sets `OGRE_RESTARTED=1` and spawns the same arguments with `posix_spawn`
  (`_wspawnv(_P_NOWAIT, ...)` on Windows), then leaves with `_Exit`.

`main` reads `OGRE_RESTARTED` before the start-screen condition and skips the
screen when it is set. An installed mod normally forces the screen so the player
can turn it off, and that reason does not apply to a restart.

### The row

`ui::RowAction::RestartGame` and `RowKind::RestartGame` are new. `build_rows`
adds the row only when `mode_ == Mode::Overlay`: the launcher has no running game
to restart. `activate` returns `RowAction::RestartGame` for it, `left_text` reads
`[ ] Restart Game`, and the second column reads "Boot the ROM again from the
beginning". The overlay's three input paths (keyboard, pad, mouse) now share
`handle_row_action`, so the row works from every one of them.

`overlay_restart_requested()` mirrors `overlay_quit_requested()`, and
`overlay_render` skips its draw once either request is set.

## Item 2 — Permanent Suspend Save

### What the game does

The suspend save is SRAM record 15: 19176 bytes at device offset `0x30B0`
(`func_80074AD4`'s slot-15 arm allocates `0x4AE8` and reads `0x30B0`). It is
written by `func_8007485C(15)`, which bank unit A's suspend action calls at
`0x8019F9F8` (`func_ovlA_8019F994`: `jal 0x8007485C` with `a0=14`, then
`a0=15`). It is loaded by `func_800749C0(15)`.

The load menu's confirm handler `func_801A9B3C` (`streamedC`, `0x801A9B3C`)
tests that the loaded slot was 15 and calls `func_80074834(15)` at `0x801A9BCC`.
`func_80074834` (`.main`, `0x80074834`, `0x28` bytes) is:

| call | effect |
|---|---|
| `func_80074F9C(15)` | blank the 12-byte header at `image + 0x30B0` in the cached 32 KiB image: `memset` 12 bytes, copy `QuestOG3` to `+4`, copy the 12 bytes back, set `g_save_dirty` |
| `func_80074BF0(0x37081383)` | write the cached image to SRAM when dirty, clear the flag, free the cache |

`func_80075578` accepts a record whose first word is zero as "empty"
(`beqz` at `0x80075620`), which is why clearing those 12 bytes deletes it. The
19164-byte payload after the header is not touched.

The erase is gated at `0x801A9BA4` by `func_801707B4(0x80) == 0`. That function
reads 4 bytes from ROM `0x275D98C` into `0x80190F30`; the bytes are the tail of a
build-date string (`3a 34 32 00`, ":42\0"), so bit `0x80` is clear and the gate
passes on this ROM.

Only one other caller passes 15 to `func_80074834`: bank unit H's "Reset Control
Deck" debug path (`func_ovlH_8019B340+0x10ac`), which erases slots 0, 1, 15 and
14. No other bank calls `func_80074834`, `func_80074F9C` or `func_80075360` with
15.

### The mod

`mods/permanent-suspend/src/permanent_suspend.c`:

```c
RECOMP_PATCH void func_80074834(unsigned int slot) {
    if (slot != SUSPEND_SLOT) {
        SAVE_BLANK_SLOT(slot);
    }
    SAVE_COMMIT(SAVE_COMMIT_MAGIC);
}
```

Both game functions are reached through a `volatile` address rather than a
direct call. A direct `jal` to `func_80074BF0` fails to link with
`relocation truncated to fit: R_MIPS_26`: the symbol is undefined in the mod ELF,
and a 26-bit call from the mod's `0x81000000` segment to the game's `0x800xxxxx`
segment does not fit. A `volatile` function-pointer value makes the compiler emit
`lui`/`addiu`/`jalr` (and a `jr t9` tail call for the commit), which N64Recomp
turns into `LOOKUP_FUNC` calls that the runtime resolves against the loaded game.

The mod manifest sets `enabled_by_default = false`, so the package plays the
vanilla game until the player turns it on in the MODS tab.

Caveat recorded in the mod's README: the debug "Reset Control Deck" path also
keeps the record now, because it goes through the same patched function.

## Verification

### The suspend record (real renderer, `build-dist/ogrebattle64`)

Both runs used the developer's `assets/saves/suspend_final_boss.n64` as the
battery, the title with a save present (its cursor starts on Load Game), and
`A`/`Start` taps to confirm. Route, identical in both runs:

```sh
OGRE_PREF_DIR=/tmp/ogre-suspend-a OGRE_NO_AUDIO=1 \
OGRE_SAVE=suspend_final_boss OGRE_SAVE_RESET=1 OGRE_SCENE=title OGRE_SPEED=1 \
OGRE_TAP_MS=1000 \
OGRE_TAP_BUTTON="none,none,none,none,start,none,start,none,a,none,a" \
OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=45000 ./build-dist/ogrebattle64
```

Both runs reached `[scene] t=6838ms id=0x0012` (Load Game) and then
`[scene] t=15355ms id=0x0003` (the mission the suspend save was taken in), so the
save was loaded. The bytes at `0x30B0` afterwards:

| run | `0x30B0..0x30BB` | image against the source |
|---|---|---|
| no mod | `00000000 51756573 744f4733` | differs |
| `ogre_permanent_suspend` enabled | `dc419ec6 51756573 744f4733` | byte-identical over all 32 KiB |

A second process with the same pref dir, no `OGRE_SAVE`, loaded the same record
again and reached scene `0x0003` at `t=14321ms`; the image was still
byte-identical afterwards. That is the behaviour item 2 asked for: one suspend
point, reloadable after a relaunch.

The write the log reports at the same instant in both runs (`[save] game writes
the battery (SRAM) save`) is the commit inside the patched function, which both
paths call. The modded run's file is unchanged because the only dirty source was
the header write that the patch skips.

### RESTART GAME (headless, null renderer)

The row was exercised with `build-null` under `SDL_VIDEODRIVER=dummy`, so no
window appeared:

```sh
SDL_VIDEODRIVER=dummy OGRE_PREF_DIR=/tmp/ogre-restart8 \
  OGRE_NO_AUDIO=1 OGRE_ROM=assets/ogre64.z64 OGRE_OVERLAY_AT_MS=9000 \
  OGRE_OVERLAY_TAB=main OGRE_OVERLAY_KEYS="down,return" \
  OGRE_SCENE_LOG=1 ./build-null/ogrebattle64
```

The log holds `[overlay] RESTART GAME`, the full `recomp::start` teardown
(`[RENDERER] null renderer shut down`, the `[GFX-WORKLOAD]` dump) and then a
second `[boot] init_sdl...` from the spawned process. The scripted keys repeat in
each generation, so the test restarts again; the process was stopped after the
second boot.

`OGRE_RESTARTED=1` was also checked on its own with
`mods/permanent-suspend.nrm` installed and no `OGRE_MOD_TEST`: the boot goes
`[boot] rom ok` -> `[boot] create_window...` with no `[launcher]` line, which is
the start-screen bypass.

The overlay capture with `OGRE_OVERLAY_TAB=main` shows the row:
`[ ] Restart Game   Boot the ROM again from the beginning`, between `ROM` and
`Exit Game` (`docs/proofs/native-overlay-restart-row.png`, RT64, the window
captured with `OGRE_CAPTURE_OVERLAY`).

### What was not measured

RESTART GAME has not been pressed in the RT64 window on this machine, and the
Windows `_wspawnv` path is compiled only by the Release workflow. This session
stopped running the game window after the developer reported several instances
overlaying the desktop. The row, the request path and the spawn were measured
headlessly; the first press on the real window is the remaining check.

### Builds

* `make app` (`build-dist`, RT64) clean.
* `cmake --build build-null --target ogrebattle64` clean.
* `tools/build-example-mods.sh mods/permanent-suspend` via Docker writes
  `build/mods/permanent-suspend.nrm` (1121 bytes).
* `make web` was not run: `main.cpp`, `overlay.cpp`, `overlay.hpp`, `ui.cpp` and
  `ui.hpp` are all in the native source list in `app/CMakeLists.txt`, and no
  shared or browser file changed.

## Probes

One temporary probe was added while diagnosing the restart test: `probeRS`
prints in `app/src/overlay.cpp` (`handle_row_action` and the SDL_KEYDOWN branch).
It is reverted; `grep -rn "probeRS" app/` is empty, and both builds above were
rebuilt from the reverted tree.

The probe also showed why the first scripted test missed the row: the overlay's
`Panel` opens on **CONTROLS**, not MAIN (`Panel`'s Overlay constructor sets
`tab_ = Tab::Controls`), so `down,return` armed a rebind capture instead. The
headless recipe passes `OGRE_OVERLAY_TAB=main`.

## Files changed

* `app/src/ui.hpp` (`RowAction::RestartGame`, `RowKind::RestartGame`,
  `RestartGame` row comment)
* `app/src/ui.cpp` (`build_rows` in Overlay mode, `left_text`, `right_text_for`,
  `activate`)
* `app/src/overlay.cpp` (`g_restart_requested`, `request_restart`,
  `handle_row_action`, `overlay_restart_requested`, the draw guard)
* `app/src/overlay.hpp` (`overlay_restart_requested`)
* `app/src/main.cpp` (the relaunch helper, includes, `OGRE_RESTARTED` bypass,
  `relaunch_process()` on both the early-exit and normal-exit paths)
* `mods/permanent-suspend/{mod.toml,mod.ld,include/modding.h,README.md,src/permanent_suspend.c}`
  (new mod)
* `docs/guides/app-build.md` (`OGRE_RESTARTED`, the overlay's CONTROLS default,
  the RESTART GAME mechanism, the headless recipe, the shipped-mod list and the
  base-call rule)
* `PLAN.md` (Status bullet, open item 25)
* `docs/STATUS-LOG.md` (session 212 entry)
* `docs/DECISIONS.md` (session 212 row)
* `docs/README.md` (handoff index row)
* `docs/HANDOFF-2026-11-02-session212.md` (this file)

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
that was modified before this session.
