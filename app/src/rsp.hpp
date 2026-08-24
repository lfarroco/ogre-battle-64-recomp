#pragma once

#include "librecomp/rsp.hpp"

namespace ogre {

// RSP microcode callbacks. Currently returns a stub that completes tasks without
// executing the game's ucode; to be replaced by RSPRecomp output.
recomp::rsp::callbacks_t make_rsp_callbacks();

}  // namespace ogre
