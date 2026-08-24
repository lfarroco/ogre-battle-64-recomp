#pragma once

#include <cstdint>
#include <string_view>

namespace ogre {

// Game identity constants for the app.
//
// ROM hash: XXH3-64 of the full big-endian (.z64) USA Rev A dump. Computed with
// the librecomp hashing logic (post-byteswap contents). See docs/DECISIONS.md.
inline constexpr uint64_t ROM_HASH = 0xbe6adaa5c3f8f7a9ULL;

inline constexpr std::string_view INTERNAL_NAME = "OgreBattle64";
inline constexpr std::string_view DISPLAY_NAME = "Ogre Battle 64: Person of Lordly Caliber (USA, Rev A)";
inline constexpr char8_t GAME_ID[] = u8"ogrebattle64-us-rev1";

// Cart header entry point; the recompiled boot stub (`recomp_entrypoint`).
inline constexpr uint32_t ENTRYPOINT_ADDRESS = 0x80070C00;

}  // namespace ogre
