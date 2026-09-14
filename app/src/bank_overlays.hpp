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
// (`*(u16*)(*(u32*)D_800C4BBC + 4)` and the pending word `D_800E8214`) until the
// dispatcher reports that scene active (`D_800E810E`), which is how the game's
// own scene state machine is entered without waiting out the attract loop.
//
// The poke only takes effect *early*: after the boot enters its first real
// scene (~1 s) the running scene owns the state block and overwrites it. So
// `OGRE_SCENE_AFTER_MS` defaults to 0 — a delay makes the jump silently do
// nothing. `OGRE_FORCE_SCENE`/`OGRE_FORCE_SCENE_AFTER_MS` are the original
// hex-only spellings and still work. `OGRE_SCENE_LOG=1` logs every scene change
// with its descriptor and record mask; `OGRE_SCENE_TRACE=1` logs the forcing
// state.
void poll_scene();

}  // namespace ogre
