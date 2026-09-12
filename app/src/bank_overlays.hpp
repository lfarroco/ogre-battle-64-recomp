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

}  // namespace ogre
