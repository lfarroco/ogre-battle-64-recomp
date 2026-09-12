#pragma once

#include <cstdint>

namespace ogre {

// Synthetic frame probe (see synth_frame.cpp). Reads OGRE_SYNTH_FRAME /
// OGRE_SYNTH_AT_MS; returns true when the probe is enabled.
bool synth_frame_enabled();

// Submit the synthetic display list on this VI (once, after the configured
// delay). Must run on the VI thread, which owns the runtime's action queue.
void synth_frame_vi_tick(uint8_t* rdram);

}  // namespace ogre
