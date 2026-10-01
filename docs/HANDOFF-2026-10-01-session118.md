# Handoff — 2026-10-01 (session 118): the gamepad was never wired to N64 controller 1

## Goal

GitHub issue #13, "0.5.1 — Windows — Xbox Controller is not recognized in game":
the pad works in the launcher/overlay CONTROLS tab and `controls.cfg` updates,
but the game sees nothing while the keyboard works.

## Result

Root cause found and fixed. A connected pad was opened into
`Platform::controllers[1..3]`, while every consumer of pad input in this game
reads N64 controller 1, which the app serves from `controllers[0]`. Keyboard
input is OR'd into `controllers[0]` (`get_input`, `controller_num == 0`), which
is why the keyboard worked in game and the pad did not.

The fix is in `app/src/sdl_platform.cpp`:

* both slot-assignment sites (the one-time enumeration in `pump_sdl_events` and
  the `SDL_CONTROLLERDEVICEADDED` branch) now fill the first free slot starting
  at 0, so the first pad takes slot 0 and the keyboard keeps working beside it;
* the `SDL_CONTROLLERDEVICEADDED` branch skips a device whose instance id is
  already in a slot. SDL queues an ADDED event for every pad connected at init,
  which the enumeration has already opened; the old code opened the same pad a
  second time and reported it as a second N64 controller;
* the one-time enumeration logs one `[input] controller <n>: <name>` line per
  filled slot. The old code logged only slot 1, so a pad in slot 0 was invisible
  in the log.

## Evidence

### The game reads N64 controller 1 and nothing else

* `tools/N64ModernRuntime/ultramodern/src/input.cpp:164` `osContGetReadData`
  calls `input_callbacks.get_input(controller, ...)` and writes the result into
  `data[controller]`. `ultramodern/include/ultramodern/input.hpp:37` documents
  `controller_num` as zero-indexed, "0 corresponds to the first controller".
* The game's own pad read uses that index 0 only:
  * `func_8008A3A4` (`asm/1060.s:30119`, guest `0x8008A3A4`) calls
    `osContStartReadData(D_800E9B88)` then `osContGetReadData($s2)`, where `$s2`
    is its first argument.
  * `func_8008A47C` (`asm/1060.s:30185`) passes `D_800C4BF0` as that argument
    (`jal func_8008A3A4` at `asm/1060.s:30202`).
  * `func_8008A600` (`asm/1060.s:30310`) copies `0x18` bytes (one `OSContPad`)
    from `D_800C4BF0` into its argument. `func_8007297C` (`asm/1060.s:2163`) is
    the per-frame poll that calls it with `D_800AEE78` (`jal` at `:2180`).
  * `func_8007284C` (`asm/1060.s:2064`) reads `lhu $v0, 0(D_800C4BF0)`
    (`asm/1060.s:2075`), i.e.
    `OSContPad[0].button`, and nothing at `+4`, `+8` or `+0xC`.

No live path reads `OSContPad[1]`, `[2]` or `[3]`: nothing loads
`D_800C4BF0+4`, `+8` or `+0xC`, the per-frame poll copies the one pad at offset
0, and the only other caller of the pad read (`func_8008A508`, `asm/1060.s:30227`,
which takes its destination from a struct field) has no caller in the ROM.

### The app put the pad elsewhere

`app/src/sdl_platform.cpp` `get_input` reads
`g_platform.controllers[controller_num]`, and the keyboard is OR'd in only for
`controller_num == 0`. Both assignment sites started at slot 1
(`for (int slot = 1; slot < 4; slot++)`), so the pad could never be the
controller the game reads. This predates the CONTROLS tab: the first app
skeleton (`0390658`) already assigned pads to slots 1..3 with the comment
"slot 0 is keyboard-first".

### Zelda64Recomp does it the other way

`Zelda64Recomp/src/game/controls.cpp:78` `recomp::get_n64_input` returns `false`
for every `controller_num != 0`, and for controller 0 ORs
`keyboard_input_mappings` and `controller_input_mappings` into one N64 button
word and one stick pair. `src/game/input.cpp:565` `controller_button_state` ORs
the button across every open SDL controller, so any connected pad is player 1.
`src/game/input.cpp:522` `get_connected_device_info` returns Controller only for
case 0. There is no per-pad N64 port assignment upstream.

This matches the fix: N64 controller 1 = keyboard ∪ pad 0.

### A/B, on this machine, with no physical pad

`SDL_JoystickAttachVirtualEx` (`SDL_JOYSTICK_TYPE_GAMECONTROLLER`) creates a pad
that `SDL_IsGameController` accepts, and `SDL_JoystickSetVirtualButton(joy, 0, 1)`
holds N64 A on it. `build-dist/ogrebattle64` (static SDL2 2.32.10, the same
configuration the Windows release uses) then ran with the live console:

```sh
printf 'c\nrh 0x800C4BF0 8\n' > /tmp/cmd.txt
SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 OGRE_PROBE_VIRTUAL_PAD=1 \
  OGRE_LIVE_CONSOLE=1 OGRE_CONSOLE_FILE=/tmp/cmd.txt OGRE_CONSOLE_AT_MS=5000 \
  OGRE_EXIT_AFTER_MS=6500 OGRE_NO_AUDIO=1 \
  ./build-dist/ogrebattle64 assets/ogre64.z64
```

| code | app log | game `OSContPad[0].button` (`0x800C4BF0`) |
|---|---|---|
| slots 1..3 (the bug) | `[input] controller 2: probe118 virtual pad` | `0000` |
| slots 0..3 (the fix) | `[input] controller 1: probe118 virtual pad` | `8000` (A held) |

With the fix, `0x800C4BF4`/`+8`/`+C` (pads 2-4) read `0000`, so one pad is one
N64 controller. Without the dedupe the same pad also appeared in a later slot
(the old run shows `800C4BFC = 8000`).

`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` is required only for this scripted run:
without keyboard focus SDL drops button *presses* before they reach the joystick
state (`SDL_PrivateJoystickShouldIgnoreEvent`, SDL2 2.32.10
`src/joystick/SDL_joystick.c`), so an unfocused run reads `0` from every pad.
This is the reason the first probe runs reported `readback=0`; it is not an app
defect and it does not affect a player at the keyboard.

## What was run for verification

* `cmake --build build-dist --target ogrebattle64 -j` and
  `cmake --build build-app --target ogrebattle64 -j`, both from the final tree.
* The A/B above, one variable changed (the slot start index) on one binary.
* A no-probe run of the final `build-dist/ogrebattle64` with `assets/ogre64.z64`:
  boots, boots the ROM, no pad lines, exits clean.
* `grep -rn probe118 app/src` is empty.

## Files changed

* `app/src/sdl_platform.cpp` — the slot assignment, the add-event dedupe, the
  per-slot controller log.

`tools/RT64` shows as `-dirty` in `git status`; that is the pinned checkout with
`patches/rt64-ob64.patch` applied, and it was dirty before this session.

## Probes used and reverted

`probe118` (an `OGRE_PROBE_VIRTUAL_PAD`-gated virtual game controller in
`pump_sdl_events`, plus per-second pad-state lines in `get_input`) was compiled
in for the A/B and removed. Nothing tagged `probe118` remains in `app/`,
`RecompiledFuncs/` or `Bank*Funcs/`, and no generated code was touched, so no
`make recomp`/`make bank-recomp` revert was needed.

## Still open

* **The browser build has the same defect.** `app/web/web.js` assigns pads to
  slots 1..3 ("stable per-physical-pad slot in 1..3 (slot 0 is keyboard-first)",
  line ~242) and `app/src/web_platform.cpp:21` documents the same model, so a
  browser gamepad cannot reach N64 controller 1 either. The fix is the same one:
  OR the first pad's state into slot 0 with the keyboard. It needs a wasm build
  and a browser run to verify, which this machine did not do.
* Slots 1..3 still exist and still map to N64 controllers 2-4, which this game
  never reads. Zelda64Recomp drops the per-port model and ORs every pad into
  controller 1. Either model fixes issue #13; this session kept the slot model
  and changed only which slot the first pad takes.
