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
#include "bank_overlays.hpp"

namespace ogre {
Platform g_platform;
}

static void update_gfx(void*) {
    // OGRE_SCENE=<name|hex>: boot straight into a screen (see bank_overlays.cpp).
    // Polled here because this callback runs once per frame.
    ogre::poll_scene();
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
    // OGRE_TAP_MS / OGRE_EXIT_AFTER_MS make a run self-driving and bounded (see
    // sdl_platform.hpp); both are off unless set.
    ogre::configure_automation(ogre::g_platform);

    // OGRE_PROFILE=1: sample every game thread's current recompiled function so
    // a "busy but not rendering" stall can be attributed to real work.
    if (getenv("OGRE_PROFILE") != nullptr) {
        ultramodern::debug_profile_start();
    }

    fprintf(stderr, "[boot] create_window...\n");
    auto window_handle = ogre::create_window(ogre::g_platform, "Ogre Battle 64: Person of Lordly Caliber");
    if (ogre::g_platform.window == nullptr) {
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[boot] window ok\n");

    // --- runtime config path ------------------------------------------------
    // OGRE_PREF_DIR overrides SDL_GetPrefPath so a run can keep its saves/mods
    // in a chosen directory (e.g. inside the repo, or a sandbox that cannot
    // write to the platform preference dir). Default is unchanged.
    std::filesystem::path pref_dir;
    if (const char* pref_override = getenv("OGRE_PREF_DIR"); pref_override != nullptr && pref_override[0] != '\0') {
        pref_dir = std::filesystem::path(pref_override);
        std::filesystem::create_directories(pref_dir);
        fprintf(stderr, "[boot] OGRE_PREF_DIR=%s\n", pref_dir.string().c_str());
    }
    else {
        char* pref_path = SDL_GetPrefPath("", "ogrebattle64");
        pref_dir = std::filesystem::path(pref_path);
        SDL_free(pref_path);
    }
    recomp::register_config_path(pref_dir);
    fprintf(stderr, "[boot] config path ok: %s\n", pref_dir.string().c_str());

    // --- game registration ----------------------------------------------------
    recomp::GameEntry entry;
    entry.rom_hash = ogre::ROM_HASH;
    entry.internal_name = std::string(ogre::INTERNAL_NAME);
    entry.display_name = std::string(ogre::DISPLAY_NAME);
    entry.game_id = std::u8string(ogre::GAME_ID);
    entry.entrypoint_address = ogre::ENTRYPOINT_ADDRESS;
    entry.entrypoint = recomp_entrypoint;
    // Register streamed overlays A/B/C after init_overlays() clears the map,
    // then arm the streamed-overlay bank swap (records the game DMA's into
    // overlay C's RAM from a different bank — see bank_overlays.cpp).
    entry.on_init_callback = [](uint8_t* rdram, recomp_context* ctx) {
        ogre::register_streamed_overlays();
        ogre::register_bank_overlays();
    };
    // OB64's save is a battery-backed cartridge save, and the chip is **SRAM**
    // (32 KiB). Evidence, session 66:
    //   * mupen64plus's game database gives SaveType=SRAM for this exact ROM
    //     (CRC 0ADAECA7 B17F9795, entry EBB4B4D2808DF427AAA3085A41B8A954),
    //     with Mempak=Yes for the unrelated Controller Pak copy/backup feature.
    //   * The game's own code: func_8008A040 builds the OSPiHandle with
    //     baseAddress 0xA8000000 (the SRAM window) and func_8008A0F0 (its DMA
    //     wrapper) uses it. The boot accessors func_80074CF0.. read the whole
    //     0x8000 bytes in 256-byte DMAs at device offsets 0..0x7F00, and the
    //     commit func_80074C58 writes the whole image back in 256-byte DMAs with
    //     direction 1. Save slots are 0x1850 (6224) bytes at offset 0x10.
    //   * No EEPROM/FlashRAM command protocol is present (osFlashInit /
    //     osEepromProbe are never called; there is no FlashRAM command DMA).
    // The DMA path is the game's own non-bridged func_8008BC40 -> D_800AA408
    // queue; librecomp/src/pi.cpp's inline PI handler resolves the device from
    // the OSPiHandle stored in the OSIoMesg (see init_pi_manager).
    entry.save_type = recomp::SaveType::Sram;
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
