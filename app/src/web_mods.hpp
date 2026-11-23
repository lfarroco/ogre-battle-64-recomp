// Browser build: the bundled code mods and the call sites the recompiled game
// code uses for them.
//
// WebAssembly has no runtime code generation, so the browser cannot load an
// arbitrary `.nrm` code mod the way the native build does. The mods this port
// ships are recompiled ahead of time (tools/gen_web_mods.py) and linked into the
// wasm app; their manifests are embedded and installed through the normal mod
// system, so the MODS tab and `mods.json` behave as on native. The two helpers
// below are what the generated game code calls instead of the native runtime's
// prologue patch:
//
//   ogre_web_mod_entry_hook   at the entry of a function a bundled mod hooks
//   ogre_web_mod_replace      at the entry of a function a bundled mod replaces
//
// tools/web_mod_hooks.py inserts both calls, guarded by `__EMSCRIPTEN__`, and
// `make recomp` re-applies the insertion after regenerating RecompiledFuncs/.
#pragma once

#include <cstdint>

namespace ogre {

// Registers the embedded `.nrm` archives and scans the mod folder. Safe to call
// more than once; `recomp::start` repeats the scan itself. It requires the game
// entry to be registered first, because a manifest names the game it targets;
// `main_web.cpp`'s `ogre_web_early_init()` does that before calling this.
void web_mods_scan();

}  // namespace ogre
