#pragma once

#include "recomp.h"

namespace ogre {

// Registers the base code sections (entry + main) from recomp_overlays.inl.
// Must be called before recomp::start().
void register_base_overlays();

// Registers log-and-return stubs for streamed/overlay functions referenced by
// the main segment. Must be called after init_overlays() has run, i.e. from
// the GameEntry on_init_callback.
void register_streamed_stubs();

}  // namespace ogre
