// Ogre Battle 64: Person of Lordly Caliber - PC port app entry point.
//
// Boot flow:
//   1. SDL + window
//   2. register_game() with the recompiled entrypoint
//   3. register base overlays (section tables from recomp_overlays.inl)
//   4. select_rom() to validate/store the user's ROM
//   5. start_game() + recomp::start() (the runtime spawns the game thread,
//      which boots the recompiled code via `recomp_entrypoint`)
//
// The main thread stays in recomp::start's loop, pumping SDL events via the
// update_gfx callback until ultramodern::quit() is requested.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <SDL.h>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/game.hpp"

#include "game.hpp"
#include "sdl_platform.hpp"
#include "renderer.hpp"
#include "rsp.hpp"
#include "overlays.hpp"

namespace ogre {
Platform g_platform;
}

static void update_gfx(void*) {
    bool quit = false;
    ogre::pump_sdl_events(ogre::g_platform, &quit);
    if (quit) {
        ultramodern::quit();
    }
}

int main(int argc, char** argv) {
    // --- SDL + window --------------------------------------------------------
    fprintf(stderr, "[boot] init_sdl...\n");
    if (!ogre::init_sdl()) {
        return EXIT_FAILURE;
    }

    fprintf(stderr, "[boot] create_window...\n");
    auto window_handle = ogre::create_window(ogre::g_platform, "Ogre Battle 64: Person of Lordly Caliber");
    if (ogre::g_platform.window == nullptr) {
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[boot] window ok\n");

    // --- runtime config path ------------------------------------------------
    char* pref_path = SDL_GetPrefPath("", "ogrebattle64");
    recomp::register_config_path(std::filesystem::path(pref_path));
    SDL_free(pref_path);
    fprintf(stderr, "[boot] config path ok\n");

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
    // TODO: determine OB64's save hardware (Controller Pak / EEPROM / Flashram)
    // from the ROM's osPfs/osEeprom/osFlash call sites.
    entry.save_type = recomp::SaveType::None;
    entry.is_enabled = true;
    entry.has_compressed_code = false;

    recomp::register_game(entry);
    fprintf(stderr, "[boot] game registered\n");

    // --- base overlays (entry + main sections) --------------------------------
    ogre::register_base_overlays();
    fprintf(stderr, "[boot] overlays registered\n");

    // --- ROM selection --------------------------------------------------------
    std::filesystem::path rom_path;
    if (argc > 1) {
        rom_path = argv[1];
    } else if (std::filesystem::exists("assets/ogre64.z64")) {
        rom_path = "assets/ogre64.z64";
    }

    std::u8string game_id = entry.game_id;
    if (!rom_path.empty()) {
        fprintf(stderr, "[boot] selecting rom %s\n", rom_path.string().c_str());
        auto result = recomp::select_rom(rom_path, game_id);
        switch (result) {
            case recomp::RomValidationError::Good:
                break;
            case recomp::RomValidationError::IncorrectVersion:
                fprintf(stderr, "ROM is a different version of Ogre Battle 64 than expected (need %s).\n",
                        ogre::INTERNAL_NAME.data());
                return EXIT_FAILURE;
            case recomp::RomValidationError::IncorrectRom:
                fprintf(stderr, "ROM hash mismatch - this ROM is not supported.\n");
                return EXIT_FAILURE;
            default:
                fprintf(stderr, "Failed to open ROM at %s\n", rom_path.string().c_str());
                return EXIT_FAILURE;
        }
        fprintf(stderr, "[boot] rom ok\n");
    } else {
        fprintf(stderr, "No ROM path provided and no stored ROM found; relying on stored ROM.\n");
    }

    // --- runtime configuration --------------------------------------------------
    recomp::Configuration cfg;
    cfg.project_version = recomp::Version(0, 1, 0);
    cfg.window_handle = window_handle;
    cfg.rsp_callbacks = ogre::make_rsp_callbacks();
    cfg.renderer_callbacks = {.create_render_context = ogre::create_renderer};
    cfg.audio_callbacks = ogre::make_audio_callbacks();
    cfg.input_callbacks = ogre::make_input_callbacks();
    cfg.gfx_callbacks = {.create_gfx = nullptr, .create_window = nullptr, .update_gfx = update_gfx};
    cfg.events_callbacks = ogre::make_events_callbacks();
    cfg.error_handling_callbacks = ogre::make_error_handling_callbacks();
    cfg.threads_callbacks = ogre::make_threads_callbacks();

    // Hardware events that the game polls at a fixed cadence (VI retrace, AI)
    // must not be dropped when the destination queue is momentarily full: the
    // game's VI manager processes one retrace per wake, and a dropped retrace
    // stalls the boot's frame state machine. Requeue them so the next drain
    // retries the delivery instead of losing it.
    ultramodern::MessageQueueControl mqc;
    // VI retraces are NOT requeued: requeuing a dropped retrace keeps the
    // external-message backlog alive, which floods the VI-manager's queue so
    // it never blocks on recv — starving every lower-priority thread parked in
    // the running queue (the cooperative scheduler only preempts to a strictly
    // higher priority). On real hardware retraces are paced by the VI
    // interrupt; dropping a retrace when the queue is momentarily full just
    // makes the game wait for the next one.
    mqc.requeue_vi = false;
    mqc.requeue_ai = false;
    ultramodern::set_message_queue_control(mqc);

    // --- boot -------------------------------------------------------------------
    // Start the runtime first: it spawns the VI/audio/gfx/threads and the game
    // thread (which waits for the game status). Then start the game from a
    // separate thread so the VI thread has already entered its dummy-mode phase
    // (start_game before recomp::start skips set_dummy_vi and the VI thread
    // crashes on a null mode).
    fprintf(stderr, "[boot] recomp::start...\n");

    std::thread game_starter{[game_id] {
        ultramodern::sleep_milliseconds(500);
        fprintf(stderr, "[boot] start_game...\n");
        recomp::start_game(game_id, "");
    }};

    recomp::start(cfg);  // blocks until ultramodern::quit()

    game_starter.join();
    ogre::shutdown_sdl(ogre::g_platform);
    return EXIT_SUCCESS;
}
