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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <SDL.h>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

#include "game.hpp"
#include "launcher.hpp"
#include "sdl_platform.hpp"
#include "renderer.hpp"
#include "rsp.hpp"
#include "overlays.hpp"
#include "bank_overlays.hpp"
#include "save_import.hpp"

namespace ogre {
Platform g_platform;
}

namespace {

// A one-line explanation of a ROM rejection, for both the launcher screen and
// the console.
std::string rom_error_text(recomp::RomValidationError error, const std::string& path) {
    switch (error) {
        case recomp::RomValidationError::IncorrectVersion:
            return "This is a different version of Ogre Battle 64. The port needs the "
                   "USA Rev A dump (" +
                   std::string(ogre::INTERNAL_NAME) + ").";
        case recomp::RomValidationError::IncorrectRom:
            return "That file is not Ogre Battle 64 (" + path + ").";
        case recomp::RomValidationError::NotARom:
            return "That file is not an N64 ROM (" + path + ").";
        case recomp::RomValidationError::NotYet:
            return "That version of the game is not supported yet (" + path + ").";
        case recomp::RomValidationError::FailedToOpen:
            return "Could not open " + path + ".";
        default:
            return "Could not load " + path + ".";
    }
}

// A launch that names its ROM explicitly (the argument, OGRE_ROM, a ROM sitting
// next to the executable, or the copy the runtime stored on a previous run)
// boots straight away; `OGRE_LAUNCHER=1` asks for the start screen anyway.
bool launcher_forced() {
    const char* value = getenv("OGRE_LAUNCHER");
    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

}  // namespace

static void update_gfx(void*) {
    // OGRE_SCENE=<name|hex>: boot straight into a screen (see bank_overlays.cpp).
    // Polled here because this callback runs once per frame.
    ogre::poll_scene();
    bool quit = false;
    ogre::pump_sdl_events(ogre::g_platform, &quit);
    if (quit) {
        // Closing the window is how an *interactive* run ends, so the dumps a
        // bounded run prints on its exit path have to print here too --
        // otherwise a session played by hand (`OGRE_COVER=1` for a coverage
        // census, `OGRE_PROFILE=1` for the hot list) produces nothing at all.
        // All are no-ops unless their env var is set. OGRE_DMA_TRACE=1 in
        // particular is how a hand-played hunt names a streamed module the port
        // has no record for (session 82/85), so it must survive a window close
        // the same way the bounded-run exit path lets it.
        ultramodern::debug_profile_dump();
        ultramodern::debug_cover_dump();
        ogre::dump_dma_trace();
        // OGRE_DUMP_RDRAM=<path>: the whole 8 MiB image, the same dump the
        // bounded-run exit path writes. Without this a hand-played session had
        // no way to produce one at all -- only `OGRE_EXIT_AFTER_MS` (which kills
        // the window mid-play) and the crash handler did (session 85: the credits
        // freeze wanted exactly this image and there was none).
        if (const char* dump_path = getenv("OGRE_DUMP_RDRAM")) {
            if (uint8_t* rdram = ultramodern::get_rdram_base()) {
                if (FILE* f = fopen(dump_path, "wb")) {
                    const size_t written = fwrite(rdram, 1, 0x800000, f);
                    fclose(f);
                    fprintf(stderr, "[SDL] dumped %zu bytes of rdram to %s\n", written, dump_path);
                } else {
                    fprintf(stderr, "[SDL] could not open %s for rdram dump\n", dump_path);
                }
            }
        }
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

    // OGRE_COVER=1: census which recompiled functions a run enters (the entry
    // counters only, no sampling thread). tools/recompcov.py joins the dump with
    // the port's registered entries, so "how much of the game did this run
    // execute?" is a number.
    if (getenv("OGRE_COVER") != nullptr) {
        ultramodern::debug_cover_start();
    }

    fprintf(stderr, "[boot] window ok\n");

    // --- runtime config path ------------------------------------------------
    // The config dir is where the runtime keeps the ROM it stored, the battery
    // save and mods. A distributable build keeps all of it in the executable's
    // own folder, so `saves/<game id>.bin` lands right beside the app;
    // OGRE_PREF_DIR still overrides (the test harnesses use it), and a
    // read-only install location falls back to the platform preference dir.
    const std::filesystem::path pref_dir = ogre::resolve_pref_dir();
    recomp::register_config_path(pref_dir);
    fprintf(stderr, "[boot] config path ok: %s\n", pref_dir.string().c_str());

    // --- game registration ----------------------------------------------------
    recomp::GameEntry entry;
    entry.rom_hash = ogre::ROM_HASH;
    entry.internal_name = std::string(ogre::INTERNAL_NAME);
    entry.display_name = std::string(ogre::DISPLAY_NAME);
    entry.game_id = std::u8string(ogre::GAME_ID);
    // The id mods target. `recomp::register_game` registers it with the runtime's
    // mod system, and `wait_for_game_started` loads the enabled mods only when
    // this is non-empty (librecomp/src/recomp.cpp). It stayed empty until the mod
    // system was wired up, so every mod in `mods/` was opened, parsed and thrown
    // away.
    entry.mod_game_id = std::string(ogre::MOD_GAME_ID);
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

    // --- mods -----------------------------------------------------------------
    // Create `<config>/mods` and `<config>/mod_config`, then open every mod in
    // `mods/` and apply `mods.json`. `recomp::start` repeats both calls before it
    // boots (librecomp/src/recomp.cpp), which is safe: `scan_mod_folder` starts
    // with `close_mods()`, and the second pass re-reads `mods.json`. Scanning
    // here is what lets the start screen list the installed mods and write a
    // toggle before the game starts.
    recomp::mods::initialize_mods();
    recomp::mods::scan_mods();
    const std::vector<recomp::mods::ModDetails> installed_mods =
        recomp::mods::get_all_mod_details(entry.mod_game_id);
    fprintf(stderr, "[boot] %zu mod(s) installed in %s\n", installed_mods.size(),
            recomp::mods::get_mods_directory().string().c_str());

    // --- base overlays (entry + main sections) --------------------------------
    ogre::register_base_overlays();
    fprintf(stderr, "[boot] overlays registered\n");

    // --- ROM selection --------------------------------------------------------
    // Order: an explicit argument, OGRE_ROM, a ROM sitting next to the
    // executable, the copy the runtime stored on a previous run, the repo's
    // assets/ (development), then the start screen.
    std::filesystem::path rom_path;
    if (argc > 1) {
        rom_path = argv[1];
    }
    else if (const char* env_rom = getenv("OGRE_ROM"); env_rom != nullptr && env_rom[0] != '\0') {
        rom_path = env_rom;
    }
    else if (!launcher_forced()) {
        rom_path = ogre::find_exe_rom(ogre::executable_directory());
    }

    std::u8string game_id = entry.game_id;
    bool explicit_rom = !rom_path.empty();

    if (!explicit_rom) {
        const std::filesystem::path stored = pref_dir / entry.stored_filename();
        std::error_code exists_ec;
        if (std::filesystem::is_regular_file(stored, exists_ec)) {
            // The runtime stored (and hash-checked) this ROM on an earlier run,
            // so booting it directly is what makes the second launch skip the
            // start screen. If it no longer validates, select_rom deletes it.
            rom_path = stored;
        }
        else if (std::filesystem::exists("assets/ogre64.z64")) {
            rom_path = "assets/ogre64.z64";
        }
    }

    // Validates and stores a candidate ROM; empty string means success. The
    // start screen calls this for every click/drop, so a wrong file is a
    // message on the screen rather than a process exit.
    auto accept_rom = [&game_id](const std::filesystem::path& candidate) -> std::string {
        auto chosen = recomp::select_rom(candidate, game_id);
        if (chosen == recomp::RomValidationError::Good) {
            std::fprintf(stderr, "[boot] rom ok: %s\n", candidate.string().c_str());
            return {};
        }
        return rom_error_text(chosen, candidate.string());
    };

    std::string launcher_error;
    if (!rom_path.empty()) {
        fprintf(stderr, "[boot] selecting rom %s\n", rom_path.string().c_str());
        const auto result = recomp::select_rom(rom_path, game_id);
        if (result != recomp::RomValidationError::Good) {
            const std::string message = rom_error_text(result, rom_path.string());
            if (explicit_rom) {
                fprintf(stderr, "%s\n", message.c_str());
                return EXIT_FAILURE;
            }
            // The remembered ROM is unusable now: show the start screen with
            // the reason instead of guessing.
            fprintf(stderr, "[boot] stored rom unusable (%s); showing the launcher\n",
                    message.c_str());
            launcher_error = message;
            rom_path.clear();
        }
        else {
            fprintf(stderr, "[boot] rom ok\n");
        }
    }

    if (rom_path.empty() || !installed_mods.empty() || launcher_forced()) {
        // The start screen is the whole interface: it returns the path of a ROM
        // it has already validated, or empty if the user closed the window.
        //
        // It also owns the mod toggles, and for that reason it is shown even when
        // a ROM is ready. A vanilla install (no mods in `mods/`) still boots
        // straight away, so the "put the ROM next to the app and launch" flow is
        // unchanged; an install with mods gets one screen where the player can
        // turn them off. `OGRE_LAUNCHER=1` forces the screen, and a command-line
        // ROM still boots directly when nothing is installed.
        ogre::LauncherContext context;
        context.pref_dir = pref_dir;
        context.initial_error = launcher_error;
        context.ready_rom = rom_path;
        context.mod_game_id = std::string(ogre::MOD_GAME_ID);
        context.accept_rom = accept_rom;
        rom_path = ogre::run_launcher(context);
        if (rom_path.empty()) {
            fprintf(stderr, "[boot] no ROM selected; exiting\n");
            return EXIT_SUCCESS;
        }
    }

    // --- window for the game ---------------------------------------------------
    // The start screen owned a window of its own; the game gets a fresh one so
    // the window flags (Metal/Vulkan) and the renderer attach to a clean
    // surface.
    fprintf(stderr, "[boot] create_window...\n");
    auto window_handle = ogre::create_window(ogre::g_platform, "Ogre Battle 64: Recomp");
    if (ogre::g_platform.window == nullptr) {
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[boot] window ok\n");

    // --- OGRE_SAVE: start from a save file as the battery ----------------------
    // The runtime loads `<config>/saves/<game id>.bin` on the game's first SRAM
    // access, which happens after recomp::start, so install it here. Accepts the
    // port's own 32 KiB image, an emulator wrapper of it, a DexDrive .N64
    // Controller Pak dump or a bare pak; see save_import.cpp.
    ogre::apply_ogre_save(pref_dir, game_id, rom_path);

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
