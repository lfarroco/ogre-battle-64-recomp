// HD backgrounds: an external pack of PNGs that replaces the game's own
// dialogue/cutscene backdrops.
//
// The game builds every large backdrop on the njpeg path: four RSP-decoded
// sub-images are copied by `func_ovlE_8019976C` (bank unit E) into four fixed
// buffers, merged into a `'B5'` image, and blitted. That code is in a streamed
// bank unit, which a `.nrm` code mod cannot hook, so the port calls in from the
// generated unit instead: `tools/hd_backgrounds.py` places a call to
// `ogre_hd_background(rdram)` at the end of the readback copy.
//
// At that point the game's `state` object names the pass it just wrote:
//
//   state = *(u32*)0x8019A680
//   state[0x74]  destination buffer (the pass's pixels)
//   state[0x78]  destination width
//   state[0x7A]  destination height
//   state[0x80]  0 for the single-image path, which writes an 8-byte header at
//                `state[0x74]` and the pixels at +8; non-zero for `'B5'`, whose
//                pixels start at `state[0x74]`
//
// The current step and scene are read from the scripted-sequence words; the pack
// maps (scene, step) to a PNG. Only the 320x240 pass is replaced, which is the
// pass the cathedral backdrop uses (`docs/HANDOFF-2026-09-15-session50.md`,
// `docs/guides/njpeg-backgrounds.md`).
//
// The PNG is decoded once at startup, box-filtered to 320x240 and converted to
// big-endian RGBA5551, then written over the pass's pixels in the runtime byte
// order (`MEM_H`/`MEM_B` in `recomp.h`).

#pragma once

#include <cstdint>
#include <filesystem>

namespace ogre {

// Loads `mods/backgrounds/` from the first candidate directory that holds a
// `backgrounds.txt`: `$OGRE_BG_DIR`, `<pref_dir>/mods/backgrounds`,
// `<pref_dir>/../mods/backgrounds`, or `<cwd>/mods/backgrounds`. Called once at
// startup from `main.cpp`; a missing pack is not an error.
void hd_backgrounds_init(const std::filesystem::path& pref_dir);

}  // namespace ogre
