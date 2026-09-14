#pragma once

namespace ogre {

// Registers the streamed-overlay *bank records* recompiled in the separate bank
// unit (BankFuncs/, built by `make bank-recomp`). Their VMAs overlap overlay C's
// — the game loads them into the same RAM — so they cannot live in the main
// ELF; app/src/bank_funcs.inc carries the address -> function table.
//
// Installs the runtime's PI-DMA hook: when the game copies a bank record into
// RAM, the record's functions are registered at their real addresses and any
// bank (or overlay C) that occupied that RAM is dropped.
//
// Must be called from the GameEntry on_init_callback, after init_overlays().
void register_bank_overlays();

// Boot straight into a screen. `OGRE_SCENE=<name|hex>` (see kScenes in
// bank_overlays.cpp for the names) pokes the game's attract scene id
// (`*(u16*)(D_800C4BBC + 4)`) until the dispatcher reports that scene active
// (`D_800E810E`), which is how the game's own scene state machine is entered
// without waiting out the attract loop. Called once per frame from the app's
// `update_gfx`; `OGRE_SCENE_AFTER_MS` (default 3000) delays the first poke so
// boot can settle. `OGRE_FORCE_SCENE`/`OGRE_FORCE_SCENE_AFTER_MS` are the
// original hex-only spellings and still work. `OGRE_SCENE_LOG=1` logs every
// scene change with its descriptor and record mask.
void poll_scene();

}  // namespace ogre
