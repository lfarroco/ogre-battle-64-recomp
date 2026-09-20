Ogre Battle 64: Recomp
=====================

A native PC port of Ogre Battle 64: Person of Lordly Caliber (USA, Rev A),
built by statically recompiling the original N64 code.

This package contains no game data. You must supply your own ROM dump of the
USA Rev A cartridge — the executable is useless without it, and none of the
game's copyrighted content is included here.


HOW TO PLAY
-----------

1. Run the app: `ogrebattle64` (macOS/Linux) or `ogrebattle64.exe` (Windows).

2. The start screen appears:

       OGRE BATTLE 64: RECOMP
       == START GAME ==
       == ROM ==
       == MODS ==

   - Select the **ROM** row and press SPACE to pick your ROM, or
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
installed (so a shipped mod can always be turned off). It has three sections:

    == START GAME ==   [x] Start Game starts the game. It reads [ ] and cannot
                       be selected until a ROM is loaded.
    == ROM ==          [x] Loaded! and the ROM's file name, or [ ] No ROM.
                       Selecting it opens the file picker; the chosen ROM comes
                       back here as loaded, with START GAME ready.
    == MODS ==         one row per installed mod, with its short description

    UP / DOWN        select a row
    SPACE            activate the selected row: START GAME starts the game, the
                     ROM row opens the file picker, a mod row turns the mod on
                     or off, an option row steps its value
    LEFT / RIGHT     change the selected option's value
    ENTER            start the game, or open the ROM picker when there is no ROM
    mouse            click a row to activate it, click elsewhere to start

A mod's own options live in `mod_config/<mod id>.json`; the start screen is the
normal way to change them.


MODS
----

The `mods/` folder holds the mods the client loads. A fresh copy of this
package carries one example mod:

    mods/skip-boot-logos.nrm    Skip Boot Logos (boots straight to the title)

It ships **off**, so the game plays exactly as it did on the console until you
turn it on: select its row in the **MODS** section and press SPACE. The setting
is remembered.

Every `.nrm` in `mods/` is opened at startup and listed in the start screen's
**MODS** section. Dropping another `.nrm` into `mods/` installs it, and deleting
one uninstalls it.

Mods are recompiled into the client at startup, so a mod built for a different
build of the game may be refused; the start screen prints the reason.


REQUIREMENTS
------------

- A GPU with working Metal (macOS), Vulkan (Linux) or D3D12/Vulkan (Windows)
  drivers. On Linux, Mesa's radv/ANV drivers or the proprietary ones are fine;
  a machine with no Vulkan driver at all cannot run the RT64 renderer.
- On Linux the app does not need a desktop environment beyond a normal X11 or
  Wayland session. Some distributions split out SDL2's runtime packages; if
  the app reports a missing library, install your distribution's SDL2 package.

If the game does not start, run the executable from a terminal and read the
`[boot]` lines it prints; they name the resolved ROM, the renderer and the
folder where saves are written.

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
