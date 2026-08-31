// Ogre Battle 64: Person of Lordly Caliber - WebAssembly (Emscripten) entry.
//
// The browser main thread runs the page UI; the runtime boots on a pthread so
// the page stays responsive (docs/WEB-PORT.md §11-12).
//
// Boot flow (mirrors app/src/main.cpp, minus SDL):
//   1. JS writes the user's ROM into the wasm filesystem at /rom.z64
//   2. JS calls ogre_start_boot()
//   3. a pthread registers the game + overlays, validates the ROM via
//      recomp::select_rom("/rom.z64"), then runs recomp::start(cfg) with the
//      null renderer and no-op web callbacks
//   4. milestones are mirrored to the page via ogre_poll_milestones()
//
// main() is a no-op: with -sEXIT_RUNTIME=0 the runtime stays alive after it
// returns, so the page can start the boot at any time.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/game.hpp"

#include "game.hpp"
#include "renderer.hpp"
#include "rsp.hpp"
#include "overlays.hpp"
#include "web_platform.hpp"
#include "milestones.hpp"

int main() {
    // Intentionally empty. The boot is started from JS via ogre_start_boot().
    return 0;
}

namespace {

void boot_runtime() {
    OGRE_MILESTONE("BOOT", "application started (web build)");

    // Config dir on the wasm virtual filesystem (MEMFS). The runtime stores a
    // validated copy of the ROM and save data here.
    recomp::register_config_path("/ogre");
    std::filesystem::create_directories("/ogre");  // ofstream won't create parents
    OGRE_MILESTONE("BOOT", "config path registered (/ogre)");

    // --- game registration ----------------------------------------------------
    recomp::GameEntry entry;
    entry.rom_hash = ogre::ROM_HASH;
    entry.internal_name = std::string(ogre::INTERNAL_NAME);
    entry.display_name = std::string(ogre::DISPLAY_NAME);
    entry.game_id = std::u8string(ogre::GAME_ID);
    entry.entrypoint_address = ogre::ENTRYPOINT_ADDRESS;
    entry.entrypoint = recomp_entrypoint;
    // Register streamed overlays A/B/C after init_overlays() clears the map.
    entry.on_init_callback = [](uint8_t* rdram, recomp_context* ctx) {
        ogre::register_streamed_overlays();
    };
    entry.save_type = recomp::SaveType::None;
    entry.is_enabled = true;
    entry.has_compressed_code = false;

    recomp::register_game(entry);
    OGRE_MILESTONE("BOOT", "game registered");

    // --- base overlays (entry + main sections) --------------------------------
    ogre::register_base_overlays();
    OGRE_MILESTONE("BOOT", "overlays registered");

    // --- ROM selection --------------------------------------------------------
    // web.js writes the user-selected ROM here (see app/web/web.js). The ROM
    // never leaves the user's machine; no ROM data is embedded in the build.
    // On a later page load the previously validated copy persisted under the
    // config dir (IDBFS-mounted /ogre) can be reused without re-picking a file.
    std::u8string game_id = entry.game_id;
    const char* rom_path = "/rom.z64";
    const std::filesystem::path stored_rom = recomp::get_config_path() / entry.stored_filename();
    if (std::filesystem::exists(rom_path)) {
        OGRE_MILESTONE("ROM", "selecting rom %s", rom_path);
        auto result = recomp::select_rom(rom_path, game_id);
        switch (result) {
            case recomp::RomValidationError::Good:
                break;
            case recomp::RomValidationError::IncorrectVersion:
                OGRE_MILESTONE("ROM", "ROM is a different version of Ogre Battle 64 than expected (need %s)",
                               ogre::INTERNAL_NAME.data());
                return;
            case recomp::RomValidationError::IncorrectRom:
                OGRE_MILESTONE("ROM", "ROM hash mismatch - this ROM is not supported.");
                return;
            case recomp::RomValidationError::NotARom:
                OGRE_MILESTONE("ROM", "the selected file does not look like an N64 ROM.");
                return;
            default:
                OGRE_MILESTONE("ROM", "failed to open ROM at %s", rom_path);
                return;
        }
        OGRE_MILESTONE("ROM", "rom ok (hash validated)");
    } else if (std::filesystem::exists(stored_rom)) {
        OGRE_MILESTONE("ROM", "no new ROM file; using previously validated stored rom");
    } else {
        OGRE_MILESTONE("ROM", "no ROM available - select a ROM file on the page to begin.");
        return;
    }

    // --- runtime configuration --------------------------------------------------
    recomp::Configuration cfg;
    cfg.project_version = recomp::Version(0, 1, 0);
    cfg.window_handle = {};  // the null renderer never touches the window
    cfg.rsp_callbacks = ogre::make_rsp_callbacks();
    cfg.renderer_callbacks = {.create_render_context = ogre::create_renderer};
    cfg.audio_callbacks = ogre::make_audio_callbacks();
    cfg.input_callbacks = ogre::make_input_callbacks();
    cfg.gfx_callbacks = {
        .create_gfx = nullptr,
        // recomp::start() asserts unless a create_window callback exists; the
        // null renderer ignores the (empty) handle.
        .create_window = [](void*) -> ultramodern::renderer::WindowHandle { return {}; },
        .update_gfx = [](void*) {},
    };
    cfg.events_callbacks = ogre::make_events_callbacks();
    cfg.error_handling_callbacks = ogre::make_error_handling_callbacks();
    cfg.threads_callbacks = ogre::make_threads_callbacks();

    // VI retraces and AI events must not be dropped when the destination queue
    // is momentarily full (see the comment in main.cpp): requeue them so the
    // next drain retries the delivery.
    ultramodern::MessageQueueControl mqc;
    mqc.requeue_vi = true;
    mqc.requeue_ai = true;
    ultramodern::set_message_queue_control(mqc);

    // --- boot -------------------------------------------------------------------
    // Same structure as the native app: start the runtime first (it spawns the
    // VI/audio/gfx/threads), then start the game from a separate thread after a
    // short delay so the VI thread has entered its dummy-mode phase.
    OGRE_MILESTONE("RUNTIME", "recomp::start...");

    std::thread game_starter{[game_id] {
        ultramodern::sleep_milliseconds(500);
        OGRE_MILESTONE("RUNTIME", "start_game...");
        recomp::start_game(game_id, "");
    }};

    recomp::start(cfg);  // blocks until ultramodern::quit()

    game_starter.join();
    OGRE_MILESTONE("RUNTIME", "runtime exited");
}

}  // namespace

extern "C" {

// Starts the runtime boot on a background thread. Returns immediately so the
// browser main thread keeps running the event loop.
void ogre_start_boot() {
    OGRE_MILESTONE("BOOT", "ogre_start_boot() called from JS");
    std::thread boot_thread{[] { boot_runtime(); }};
    boot_thread.detach();
}

// Pointer to the accumulated milestone text (see milestones.hpp).
const char* ogre_poll_milestones() {
    return ogre::milestones::poll_buffer();
}

// Enable/disable the runtime's chatty debug traces ([ev]/[mq]/[sch]/[drainer])
// at runtime (probes call this instead of relying on the OGRE_DEBUG_TRACES env
// var, which is awkward to set under Emscripten).
void ogre_set_trace_enabled(int enabled) {
    ultramodern::set_debug_traces_enabled(enabled != 0);
}

// Debug: inject a synthetic PRENMI (0x29D) to advance the game's boot phase.
void ogre_send_prnmi() {
    ultramodern::debug_send_prnmi();
}

}  // extern "C"
