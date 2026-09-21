Ogre Battle 64: Recomp
=====================

A native PC port of Ogre Battle 64: Person of Lordly Caliber (USA, Rev A),
built by statically recompiling the original N64 code.

This package contains no game data. You must supply your own ROM dump of the
USA Rev A cartridge — the executable is useless without it, and none of the
game's copyrighted content is included here.


REQUIREMENTS
------------

A 64-bit PC and a GPU the renderer supports:

    GPU    Direct3D 12.0 (Shader Model 6.0) or Vulkan 1.2 on Windows, Vulkan 1.2
           on Linux, Metal on macOS. The oldest GPUs those cover are roughly
           GeForce GT 630, Radeon HD 7750 (2012) and Intel HD 510 (Skylake).
    CPU    x86-64 with SSE4.1 (Intel Core 2 Penryn, AMD Bulldozer or newer).
           The macOS build needs Apple Silicon.
    OS     Windows 10 or 11 (64-bit); macOS 11 or newer on Apple Silicon; a
           glibc Linux with a Vulkan 1.2 driver.
    RAM    2 GB. The emulated N64 machine commits 512 MB.
    Disk   about 150 MB for the app, plus your ROM.
    ROM    your own Ogre Battle 64 (USA, Rev A) dump, 40 MB, `.z64`, `.n64` or
           `.v64`. No game data is included.

A keyboard is enough to play; a gamepad with XInput (Windows) or SDL controller
support is optional. An audio device is optional too: with none, the game runs
silently.

On Windows nothing else has to be installed. The package carries the Microsoft
C++ runtime that its shader compiler (`dxcompiler.dll`) needs.

**If the game crashes as it starts, update your graphics driver first.** On an
older GPU, set `OGRE_CONSOLE=1` before launching to open a console with the boot
log, then try `OGRE_GRAPHICS_API=vulkan` or `OGRE_GRAPHICS_API=d3d12` to pin the
renderer backend (`set OGRE_CONSOLE=1` in `cmd`, or the environment variables
under System Properties).


HOW TO PLAY
-----------

1. Run the app:

   - **macOS:** open `Ogre Battle 64.app` (double-click it). The ROM, the save
     and `error.log` live in this folder, next to the app bundle.
   - **Windows:** `ogrebattle64.exe`.
   - **Linux:** `ogrebattle64`.

   On macOS, open the bundle and not the executable inside `Contents/MacOS`:
   Finder opens a Terminal window for a bare executable, and the bundle is what
   avoids that.

2. The start screen appears:

       OGRE BATTLE 64: RECOMP
       [ START GAME ] [ ROM ] [ MODS ] [ CONTROLS ] [ SETTINGS ]

   - Select the **ROM** tab and press SPACE to pick your ROM, or
   - drag the ROM file onto the window, or
   - just put the ROM in this folder and launch the app again (it finds
     `ogre64.z64`, or any `.z64`/`.n64`/`.v64` next to the executable).

   The ROM is validated by hash: it must be the **USA Rev A** cartridge
   (internal name `OgreBattle64`). A 40 MB dump works in any of the three
   common byte orders (`.z64` big-endian, `.n64` byteswapped, `.v64`
   little-endian); the app converts them. A ROM of another region, another
   revision, or a decompressed/hacked one will be rejected by name, and the
   window says which case it is and keeps asking.

3. The **START GAME** row starts the game (ENTER does the same). The game stores
   the validated ROM in this folder, so later launches go straight into the game
   — unless a mod is installed, in which case the start screen appears first so
   the mods can be turned on or off. Hold on to this folder: the game, its save
   and its mods live in it.


SAVES
-----

Battery saves (the cartridge SRAM) are written to:

    saves/ogrebattle64-us-rev1.bin

in this folder, next to the executable. Back that file up to keep your
progress; delete it to start fresh. Setting the environment variable
`OGRE_PREF_DIR` to another directory moves both the save and the stored ROM
there instead.


START SCREEN
------------

The start screen appears when no ROM is loaded, and whenever at least one mod is
installed (so a shipped mod can always be turned off). It has five tabs:

    [ START GAME ]   [x] Start Game starts the game. It reads [ ] and cannot be
                     selected until a ROM is loaded.
    [ ROM ]          [x] Loaded! and the ROM's file name, or [ ] No ROM.
                     Selecting it opens the file picker; the chosen ROM comes
                     back here as loaded, with START GAME ready.
    [ MODS ]         one row per installed mod, with its short description
    [ CONTROLS ]     one row per N64 button, showing the keyboard key and the
                     gamepad button bound to it, and what the button does
    [ SETTINGS ]     GAME SPEED: [ ] 1  [ ] 2  [x] 4. The game runs that many
                     times faster than real time; 1 is normal speed.

    TAB / SHIFT+TAB  switch tab
    UP / DOWN        select a row
    SPACE            activate the selected row: START GAME starts the game, the
                     ROM row opens the file picker, a mod row turns the mod on
                     or off, an option row steps its value, the GAME SPEED row
                     steps the speed, a CONTROLS row starts a rebind (press the
                     key or gamepad button you want)
    LEFT / RIGHT     change the selected option's value
    ENTER            start the game, or open the ROM picker when there is no ROM
    mouse            click a tab to switch, a row to activate it, a GAME SPEED
                     number to select that speed, elsewhere to start

Bindings are saved in `controls.cfg` in this folder, and the game speed in
`settings.cfg`. **ESC** opens the same panel over the running game, with the ROM
and MODS tabs disabled and CONTROLS and SETTINGS active, so bindings and the
game speed can be changed mid-game; **ESC** closes it again.

A mod's own options live in `mod_config/<mod id>.json`; the start screen is the
normal way to change them.


MODS
----

The `mods/` folder holds the mods the client loads. A fresh copy of this
package carries one example mod:

    mods/skip-boot-logos.nrm    Skip Boot Logos (boots straight to the title)

It ships **off**, so the game plays exactly as it did on the console until you
turn it on: open the **MODS** tab, select its row and press SPACE. The setting
is remembered.

Every `.nrm` in `mods/` is opened at startup and listed on the start screen's
**MODS** tab. Dropping another `.nrm` into `mods/` installs it, and deleting
one uninstalls it.

Mods are recompiled into the client at startup, so a mod built for a different
build of the game may be refused; the start screen prints the reason.


IF THE GAME DOES NOT START
--------------------------

The reason is written to `error.log` in this folder and, when the failure
happens before the game window exists, shown in a dialog. Check the
REQUIREMENTS section above first: the most common cause on Windows is a GPU
driver older than the renderer needs.


IF THE GAME CRASHES
-------------------

The app opens no console window, so a crash is not visible in one. If the
process dies on a fault or an unhandled error it writes:

    error.log

into this folder, next to the save. The file holds the reason, the fault
address, the N64 thread call chain and the last lines of the app's own log (the
`[boot]`, `[bank]` and `[overlays]` lines). Attach the whole file to a bug report:

    https://github.com/lfarroco/ogre-battle-64-decomp/issues

`error.log` is overwritten by the next crash. Closing the window normally writes
nothing. A developer can also make the app crash on demand to check the file:
set `OGRE_CRASH_TEST=segv` in the environment before launching.

If the game does not start at all, the `[boot]` lines name the resolved ROM,
the renderer and the folder where saves are written. On macOS they are not
printed anywhere; run the executable inside the bundle from a Terminal
(`Ogre Battle 64.app/Contents/MacOS/ogrebattle64`) to see them. On Windows and
Linux, run the app from a terminal.

Troubleshooting, build instructions from source, and the full list of
environment options are in the repository:

    https://github.com/lfarroco/ogre-battle-64-decomp

See docs/guides/app-build.md there for the diagnostics toolkit.


LEGAL
-----

Ogre Battle 64 (c) Quest / Nintendo. This is a fan preservation and
interoperability project; it ships no ROM, no extracted assets and no game
data. Supply your own legally obtained dump.

The executable includes SDL2 (https://libsdl.org), Copyright (C) 1997-2025
Sam Lantinga, under the zlib license — see SDL2-LICENSE.txt in this folder.
