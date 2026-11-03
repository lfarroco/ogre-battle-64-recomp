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
// bank_overlays.cpp for the names) stores the game's own scene request
// (`0x8000 | id` into `D_800C4C26`) once a scene past the boot's `0x00` runs
// with no request pending. The dispatcher consumes it as in a natural
// transition: the running scene's leave hook runs and the next scene's enter
// loads its own banks. `poll_scene` retries until `D_800E810E` reports the
// target active.
//
// `OGRE_SCENE_AFTER_MS=n` lets the first scene run for n ms before the request
// is written (default 0). `OGRE_FORCE_SCENE`/`OGRE_FORCE_SCENE_AFTER_MS` are
// the original hex-only spellings and still work. `OGRE_SCENE_LOG=1` logs every
// scene change with its descriptor and record mask; `OGRE_SCENE_TRACE=1` logs
// the forcing state.
void poll_scene();

// The scene id the dispatcher is currently running (`D_800E810E`), or 0xFFFF
// before the game is up. Exposed so the synthetic-input automation can scope a
// scripted tap to a scene instead of to wall-clock time (see `OGRE_TAP_SCENE` /
// `OGRE_TAP_NOT_SCENE` in sdl_platform.cpp).
uint16_t active_scene_id();

// Resolve a scene name from `kScenes` (bank_overlays.cpp) or a hex id.
bool scene_lookup(const char* spec, uint16_t& id);

// `scene_list` is a `,`/`+`-separated list of scene names/ids; true when `id`
// matches one of them (false when the list is absent or empty).
bool scene_list_matches(const char* scene_list, uint16_t id);

// `OGRE_DMA_TRACE=1`: dump every streamed DMA the game issued, grouped by
// (ROM, RAM) base with a chunk count. Called from the bounded-run exit path
// (`OGRE_EXIT_AFTER_MS`), because the process leaves through `_exit` and an
// `atexit` dump never runs.
void dump_dma_trace();

}  // namespace ogre
