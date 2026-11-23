// Browser build: install the bundled `.nrm` mods and expose them to the page.
//
// The wasm build cannot generate code at runtime, so the code mods this port
// ships are recompiled ahead of time (tools/gen_web_mods.py) and linked in. The
// embedded archives are still normal mods: `register_embedded_mod` opens each
// one, `mods.json` carries the on/off state, and the runtime's hook and
// replacement machinery is what a loaded mod uses. Only the code-handle creation
// differs (librecomp/src/mods.cpp, StaticModCodeHandle).
//
// The recompiled game code calls the two helpers at the bottom of this file at
// the entry of a function a bundled mod hooks or replaces; tools/web_mod_hooks.py
// inserts those calls.

#include "web_mods.hpp"

#include <cstdio>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

#include "web_mods/bundled_mods.h"
#include "game.hpp"

namespace {

// The installed mods, refreshed by the scan. `c_str()` pointers handed to the
// page stay valid until the next scan.
std::vector<recomp::mods::ModDetails> g_mods;
bool g_scanned = false;

void refresh_mods() {
    g_mods = recomp::mods::get_all_mod_details(std::string(ogre::MOD_GAME_ID));
    if (g_mods.empty()) {
        // The listing runs before the game entry is registered, so the filtered
        // lookup has nothing to match yet. Showing every opened mod is better
        // than an empty MODS tab.
        g_mods = recomp::mods::get_all_mod_details(std::string{});
    }
}

bool valid_index(int index) {
    return index >= 0 && static_cast<size_t>(index) < g_mods.size();
}

}  // namespace

namespace ogre {

void web_mods_scan() {
    if (g_scanned) {
        // The page can open the MODS tab while the game is running. A second
        // scan would close every opened mod, so only refresh the listing.
        refresh_mods();
        return;
    }
    // `initialize_mods` creates <config>/mods and <config>/mod_config and sets
    // the mods.json path. The native entry calls it once at startup so its start
    // screen can list the mods; the browser needs the same before it can draw
    // the MODS tab.
    recomp::mods::initialize_mods();

    size_t count = 0;
    const OgreWebModEntry* entries = ogre_web_mod_entries(&count);
    for (size_t i = 0; i < count; i++) {
        recomp::mods::register_embedded_mod(
            entries[i].id,
            std::span<const uint8_t>(entries[i].bytes, entries[i].size));
    }

    recomp::mods::scan_mods();
    refresh_mods();
    g_scanned = true;
    std::fprintf(stderr, "[boot] %zu bundled mod(s)\n", g_mods.size());
}

}  // namespace ogre

#ifdef __EMSCRIPTEN__

// The browser entry registers the game before the first scan. Declared here so
// the MODS tab can run its scan before the user starts a ROM.
extern "C" void ogre_web_early_init();

extern "C" int ogre_web_mods_scan() {
    ogre_web_early_init();
    ogre::web_mods_scan();
    return static_cast<int>(g_mods.size());
}

extern "C" int ogre_web_mods_count() {
    return static_cast<int>(g_mods.size());
}

extern "C" const char* ogre_web_mods_id(int index) {
    return valid_index(index) ? g_mods[index].mod_id.c_str() : "";
}

extern "C" const char* ogre_web_mods_name(int index) {
    return valid_index(index) ? g_mods[index].display_name.c_str() : "";
}

extern "C" const char* ogre_web_mods_description(int index) {
    if (!valid_index(index)) {
        return "";
    }
    const std::string& text = g_mods[index].short_description.empty()
                                  ? g_mods[index].description
                                  : g_mods[index].short_description;
    return text.c_str();
}

extern "C" int ogre_web_mods_enabled(int index) {
    if (!valid_index(index)) {
        return 0;
    }
    return recomp::mods::is_mod_enabled(g_mods[index].mod_id) ? 1 : 0;
}

extern "C" void ogre_web_mod_set_enabled(const char* mod_id, int enabled) {
    if (mod_id == nullptr || mod_id[0] == '\0') {
        return;
    }
    // The game has already loaded its mods once it is running, and a code mod is
    // not runtime-toggleable, so the runtime ignores this then. The page
    // disables the rows in that state; `trigger_save` is false because the file
    // is written synchronously below, before the page syncs IDBFS.
    recomp::mods::enable_mod(mod_id, enabled != 0);
    recomp::mods::flush_mods_config();
    refresh_mods();
}

// The generated game code (tools/web_mod_hooks.py) calls these instead of the
// runtime patching a function prologue.
extern "C" void ogre_web_mod_entry_hook(uint8_t* rdram, recomp_context* ctx, uint32_t function_vram) {
    recomp::mods::run_entry_hook(function_vram, rdram, ctx);
}

extern "C" int ogre_web_mod_replace(uint8_t* rdram, recomp_context* ctx, uint32_t function_vram) {
    return recomp::mods::run_static_replacement(function_vram, rdram, ctx) ? 1 : 0;
}

#endif  // __EMSCRIPTEN__
