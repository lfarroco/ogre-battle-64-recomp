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

// The id that mods target (`game_id` in a mod's `mod.toml`). This is separate
// from `GAME_ID`, which names the exact revision and is the ROM/save file name.
// A mod that works with the USA Rev A code keeps working when a revision-specific
// id changes, and `mods.json` stores mod toggles against this id. Changing this
// value after release orphans every installed mod's enabled state.
inline constexpr std::string_view MOD_GAME_ID = "ogrebattle64";

// Cart header entry point; the recompiled boot stub (`recomp_entrypoint`).
inline constexpr uint32_t ENTRYPOINT_ADDRESS = 0x80070C00;

// The player's Chaos Frame: one `u8` in the game's flags/state block at
// `0x801936C9`. It decides the good and the bad ending. The game prints it in
// only one place, the scene-`0x13` screen after the credits, and the app's DEBUG
// tab reads the same byte live.
//
// Evidence, instruction level (`build/ogrebattle64.elf`):
//   * The game-state initialiser `func_8016C900` bzeroes `0x80193698` for
//     `0x3A` bytes and then stores `50` at `0x801936C9` (`0x8016C998`), the
//     documented starting Chaos Frame. `0x801936A9` is never written.
//   * The scene-`0x13` update `func_801B5128` reads `0x801936C9` (`0x801B5360`)
//     and masks it with `0x7F`, the only flags-block byte that screen reads.
//   * The scene-script VM opcode `0xFF` stores a script operand there
//     (`0x80171480`, jump table `0x80190BC8`).
// The community figure `0x801936A9` (ogrebattle64.net, Project64 symbol) does
// not match this ROM: that byte has no read or write in the ROM and reads 0 at
// the title, where the Chaos Frame is 50.
inline constexpr uint32_t CHAOS_FRAME_ADDRESS = 0x801936C9;

}  // namespace ogre
