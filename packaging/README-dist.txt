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

2. A black window appears:

       OGRE BATTLE 64: RECOMP
       CLICK TO LOAD YOUR ROM (OR DROP IT IN THIS WINDOW)

   - Click the window and pick your ROM, or
   - drag the ROM file onto the window, or
   - just put the ROM in this folder and launch the app again (it finds
     `ogre64.z64`, or any `.z64`/`.n64`/`.v64` next to the executable).

   The ROM is validated by hash: it must be the **USA Rev A** cartridge
   (internal name `OgreBattle64`). A 40 MB dump works in any of the three
   common byte orders (`.z64` big-endian, `.n64` byteswapped, `.v64`
   little-endian); the app converts them. A ROM of another region, another
   revision, or a decompressed/hacked one will be rejected by name, and the
   window says which case it is and keeps asking.

3. The game stores the validated ROM in this folder, so later launches go
   straight into the game. Hold on to this folder: the game and its save live
   in it.


SAVES
-----

Battery saves (the cartridge SRAM) are written to:

    saves/ogrebattle64-us-rev1.bin

in this folder, next to the executable. Back that file up to keep your
progress; delete it to start fresh. Setting the environment variable
`OGRE_PREF_DIR` to another directory moves both the save and the stored ROM
there instead.


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
