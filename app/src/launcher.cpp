// The app's start screen (see launcher.hpp).
//
// The screen's list is the shared tabbed Panel (ui.hpp): a START GAME tab, a ROM
// tab, a MODS tab, a CONTROLS tab whose rows rebind the pad, and a SETTINGS tab.
// The same Panel is what the in-game overlay shows, so the two screens cannot
// drift apart.

#include "launcher.hpp"

#include "font.hpp"
#include "input_map.hpp"
#include "sdl_platform.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>

#include <SDL.h>

#if defined(OGRE_HAVE_NFD)
#include <nfd.h>
#endif

namespace ogre {
namespace {

// The branding and the error lines. The old prompt line ("click to load your
// ROM") is gone: the ROM tab states it, and the row's second column says how.
constexpr const char* kTitle = "OGRE BATTLE 64: RECOMP";
constexpr const char* kErrorTitle = "THAT IS NOT A USABLE ROM";
constexpr const char* kErrorHint = "PRESS SPACE ON THE ROM TAB TO TRY AGAIN";

// A black window; the content sits slightly above centre.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kTitleScale = 5;
constexpr int kErrorScale = 2;
constexpr int kFooterScale = 2;

const SDL_Color kTitleColor{238, 238, 232, 255};
const SDL_Color kHintColor{122, 128, 138, 255};
const SDL_Color kErrorColor{232, 116, 106, 255};
const SDL_Color kWarmColor{198, 160, 92, 220};
const SDL_Color kFooterColor{110, 116, 126, 255};

// Opens the platform's file picker. Empty when the build has no dialog (the
// null-renderer variant links no file-dialog library): dragging a file in, or
// placing one beside the executable, still works there.
std::filesystem::path browse_for_rom() {
#if defined(OGRE_HAVE_NFD)
    // The UTF-8 entry points, not the native-char ones: `nfdnchar_t` is
    // `wchar_t` on Windows (nfd.h), so `NFD_OpenDialogN` cannot take the narrow
    // literals below. Plain (not u8) literals: nfdu8char_t is `char`, and in
    // C++20 u8"" is char8_t, which does not convert. Standard execution encoding
    // is UTF-8 on every platform this builds for.
    const nfdu8filteritem_t filters[] = {
        {"N64 ROM (*.z64; *.n64; *.v64)", "z64,n64,v64"},
        {"All files", "*"},
    };
    nfdu8char_t* chosen = nullptr;
    const nfdresult_t result =
        NFD_OpenDialogU8(&chosen, filters, SDL_arraysize(filters), nullptr);
    if (result != NFD_OKAY) {
        return {};
    }
    // Build the path from a char8_t string so the UTF-8 bytes are decoded to
    // the native encoding. On Windows this avoids depending on the ANSI code
    // page; `std::filesystem::u8path` would do the same but is deprecated in
    // C++20.
    std::filesystem::path path{std::u8string(reinterpret_cast<const char8_t*>(chosen))};
    NFD_FreePathU8(chosen);
    return path;
#else
    return {};
#endif
}

// Lightweight "is this worth offering to the runtime?" test for a scan: N64
// dumps are a few MiB and always have a PI BSD DOM1 header. The runtime is the
// authority (accept_rom re-validates); this only avoids showing the launcher
// when there is provably nothing here.
bool looks_like_n64_rom(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 1024 * 1024 || size > 128ull * 1024 * 1024) {
        return false;
    }
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    unsigned char header[4] = {};
    const size_t got = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    if (got != sizeof(header)) {
        return false;
    }
    const uint32_t be = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
                        (uint32_t(header[2]) << 8) | uint32_t(header[3]);
    return be == 0x80371240u ||  // big-endian .z64
           be == 0x40123780u ||  // byteswapped .n64
           be == 0x12408037u;    // little-endian .v64
}

bool is_writable_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path probe = dir / ".ogre-write-probe";
    FILE* file = std::fopen(probe.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    std::filesystem::remove(probe, ec);
    return true;
}

bool has_rom_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

std::string lowercase(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// OGRE_LAUNCHER_KEYS=<name>[,<name>...]: one synthetic key down per 150 ms, fed
// through the same handler a real key press uses. Verification aid for scripted
// runs and screenshots; no effect unless set. Names are SDL scancode names.
struct ScriptedKeys {
    std::vector<SDL_Scancode> keys;
    size_t next = 0;
};

ScriptedKeys parse_scripted_keys(const char* spec) {
    ScriptedKeys script;
    if (spec == nullptr) {
        return script;
    }
    std::string text = spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string name = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        // Trim surrounding spaces so "tab, down" works.
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        if (!name.empty()) {
            const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
            if (code != SDL_SCANCODE_UNKNOWN) {
                script.keys.push_back(code);
            }
            else {
                std::fprintf(stderr, "[launcher] OGRE_LAUNCHER_KEYS: unknown key '%s'\n",
                             name.c_str());
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return script;
}

}  // namespace

std::filesystem::path executable_directory() {
    char* base = SDL_GetBasePath();
    if (base == nullptr) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    std::filesystem::path dir{base};
    SDL_free(base);
    if (dir.empty()) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    return dir;
}

std::filesystem::path resolve_pref_dir() {
    if (const char* override_dir = std::getenv("OGRE_PREF_DIR");
        override_dir != nullptr && override_dir[0] != '\0') {
        std::filesystem::path dir{override_dir};
        std::filesystem::create_directories(dir);
        return dir;
    }
    const std::filesystem::path exe_dir = executable_directory();
    if (is_writable_directory(exe_dir)) {
        return exe_dir;
    }
    // Read-only install location: fall back to the platform preference dir so
    // saves and mods still work.
    char* pref = SDL_GetPrefPath("", "ogrebattle64");
    if (pref == nullptr) {
        return exe_dir;
    }
    std::filesystem::path dir{pref};
    SDL_free(pref);
    return dir;
}

std::filesystem::path find_exe_rom(const std::filesystem::path& exe_dir) {
    // Distribution convention first (a ROM dropped next to the app), then the
    // same names in a roms/ subfolder.
    const std::filesystem::path roms_dir = exe_dir / "roms";
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    for (const std::filesystem::path& dir : {exe_dir, roms_dir}) {
        for (const char* name : {"ogre64.z64", "ogre64.n64", "ogrebattle64.z64",
                                 "Ogre Battle 64 - Person of Lordly Caliber (USA) (Rev A).n64"}) {
            const std::filesystem::path path = dir / name;
            if (std::filesystem::is_regular_file(path, ec)) {
                candidates.push_back(path);
            }
        }
    }
    if (candidates.empty()) {
        for (const std::filesystem::path& dir : {exe_dir, roms_dir}) {
            std::error_code iter_ec;
            std::filesystem::directory_iterator it{dir, iter_ec};
            if (iter_ec) {
                continue;
            }
            std::vector<std::filesystem::path> found;
            for (const auto& entry : it) {
                std::error_code type_ec;
                if (!entry.is_regular_file(type_ec)) {
                    continue;
                }
                const std::filesystem::path path = entry.path();
                if (has_rom_extension(path)) {
                    found.push_back(path);
                }
            }
            std::sort(found.begin(), found.end());
            candidates.insert(candidates.end(), found.begin(), found.end());
        }
    }
    for (const std::filesystem::path& path : candidates) {
        if (looks_like_n64_rom(path)) {
            return path;
        }
    }
    return {};
}

std::filesystem::path run_launcher(const LauncherContext& context) {
    SDL_Window* window = SDL_CreateWindow(
        "Ogre Battle 64: Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        std::fprintf(stderr, "[launcher] window: %s\n", SDL_GetError());
        report_boot_failure(std::string("The start screen window could not be created: ") +
                            SDL_GetError());
        return {};
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] renderer: %s\n", SDL_GetError());
        report_boot_failure(std::string("No SDL renderer is available for the start screen: ") +
                            SDL_GetError());
        SDL_DestroyWindow(window);
        return {};
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    Font font;
    font.init(renderer);
    std::fprintf(stderr, "[launcher] waiting for a ROM (save folder: %s)\n",
                 context.pref_dir.string().c_str());

    std::string error = context.initial_error;
    std::filesystem::path accepted;
    bool running = true;

    // A ROM found before this screen opened is ready to start. Choosing another
    // one replaces it; the game starts from the START GAME row.
    std::filesystem::path ready_rom = context.ready_rom;
    ui::Panel panel(ui::Panel::Mode::Launcher, context.mod_game_id, context.ready_rom);
    PadList pads;

    // OGRE_LAUNCHER_SHOT=<path>: draw one frame, write it as a PPM, and quit.
    // Verification aid for the screen's own drawing; no effect unless set.
    const char* shot_path = std::getenv("OGRE_LAUNCHER_SHOT");
    const bool shot_pending = shot_path != nullptr && shot_path[0] != '\0';
    bool shot_written = false;

    auto play = [&]() {
        if (!ready_rom.empty()) {
            accepted = ready_rom;
            running = false;
        }
    };

    // A validated ROM becomes the one START GAME will boot. The screen stays up
    // so the player sees it loaded and starts the game themselves.
    auto accept_rom_path = [&](const std::filesystem::path& path) {
        ready_rom = path;
        error.clear();
        panel.set_rom(path);
        panel.reload();
        panel.select_first();
    };

    // Opens the platform picker and validates the choice. Shared by a click on
    // empty space and the Enter key.
    auto browse_and_accept = [&]() {
        const std::filesystem::path chosen = browse_for_rom();
        if (chosen.empty()) {
#if !defined(OGRE_HAVE_NFD)
            error = "This build has no file browser. Drop the ROM onto this "
                    "window, or set OGRE_ROM to its path.";
#endif
            return;
        }
        std::string reason = context.accept_rom(chosen);
        if (reason.empty()) {
            accept_rom_path(chosen);
        }
        else {
            error = std::move(reason);
        }
    };

    // What both the SDL_DROPFILE event and the test hook below run.
    auto handle_dropped = [&](const std::filesystem::path& dropped) {
        std::string reason = context.accept_rom(dropped);
        if (reason.empty()) {
            accept_rom_path(dropped);
        }
        else {
            error = std::move(reason);
        }
    };

    // Space activates the selected row (START GAME plays, the ROM row opens the
    // picker, a mod row toggles, an option row steps, a binding row arms the
    // rebind capture); Left/Right only step an option.
    auto run_action = [&](ui::RowAction action) {
        switch (action) {
            case ui::RowAction::BrowseRom:
                browse_and_accept();
                break;
            case ui::RowAction::Play:
                play();
                break;
            default:
                break;
        }
    };
    auto activate_selected = [&]() {
        const size_t index = panel.selected();
        if (index >= panel.row_count() || !panel.selectable(index)) {
            return;
        }
        run_action(panel.activate(index, 1));
    };
    auto step_option = [&](int direction) {
        const size_t index = panel.selected();
        if (index < panel.row_count() &&
            (panel.row_kind(index) == ui::Panel::RowKind::Option ||
             panel.row_kind(index) == ui::Panel::RowKind::GameSpeed)) {
            panel.activate(index, direction);
        }
    };

    // OGRE_TEST_DROP=<path>: feed one synthetic drop for that path so the drop
    // handler can be exercised without a human drag (SDL cannot synthesize a
    // real Finder drag, and pushing a fabricated SDL_DROPFILE through
    // sdl2-compat's event translation dereferences a null string). Development
    // aid, no effect unless set.
    const char* test_drop = std::getenv("OGRE_TEST_DROP");
    bool test_drop_pending = test_drop != nullptr && test_drop[0] != '\0';
    const uint64_t test_drop_at = SDL_GetTicks64() + 300;

    // OGRE_LAUNCHER_TAB=<start|rom|mods|controls|settings>: open the screen on
    // that tab, so a screenshot or a scripted run reaches it without input.
    if (const char* tab = std::getenv("OGRE_LAUNCHER_TAB")) {
        const std::string name = lowercase(tab);
        if (name == "rom") {
            panel.set_tab(ui::Tab::Rom);
        }
        else if (name == "mods") {
            panel.set_tab(ui::Tab::Mods);
        }
        else if (name == "controls" || name == "controller") {
            panel.set_tab(ui::Tab::Controls);
        }
        else if (name == "settings" || name == "setting") {
            panel.set_tab(ui::Tab::Settings);
        }
    }

    // OGRE_LAUNCHER_KEYS drives the screen from synthetic key presses.
    ScriptedKeys script = parse_scripted_keys(std::getenv("OGRE_LAUNCHER_KEYS"));
    const uint64_t script_started_ms = SDL_GetTicks64();
    const uint64_t script_step_ms = 150;
    // OGRE_LAUNCHER_SHOT_MS delays the screenshot; it defaults to a point after
    // the scripted keys have all been delivered.
    uint64_t shot_at_ms = 0;
    if (shot_pending) {
        if (const char* value = std::getenv("OGRE_LAUNCHER_SHOT_MS")) {
            shot_at_ms = static_cast<uint64_t>(std::strtoul(value, nullptr, 10));
        }
        else {
            shot_at_ms = script.keys.empty() ? 0
                                             : (script.keys.size() + 2) * script_step_ms;
        }
    }

    uint64_t capture_started_ms = 0;

    while (running) {
        // Feed the next scripted key, exactly as a real key event would arrive.
        if (script.next < script.keys.size() &&
            SDL_GetTicks64() - script_started_ms >= script.next * script_step_ms) {
            const SDL_Scancode code = script.keys[script.next++];
            SDL_Event synthetic{};
            synthetic.type = SDL_KEYDOWN;
            synthetic.key.state = SDL_PRESSED;
            synthetic.key.keysym.scancode = code;
            synthetic.key.keysym.sym = SDL_GetKeyFromScancode(code);
            synthetic.key.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&synthetic);
        }

        if (test_drop_pending && SDL_GetTicks64() >= test_drop_at) {
            test_drop_pending = false;
            std::fprintf(stderr, "[launcher] test drop: %s\n", test_drop);
            handle_dropped(std::filesystem::path(test_drop));
            // The test hook also starts the game, so a scripted run needs no
            // input of its own. A real drop or picker stops at START GAME.
            play();
            continue;
        }

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer, &output_width, &output_height);
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        const float ui_scale =
            std::max(1.0f, static_cast<float>(output_height) / kWindowHeight);
        auto px = [ui_scale](int value) {
            return static_cast<int>(static_cast<float>(value) * ui_scale);
        };

        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (pads.handle_event(event)) {
                continue;
            }
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode key = event.key.keysym.sym;
                // A rebind capture consumes every key. Escape and the two clear
                // keys are the only ways out, so the overlay's own Escape stays
                // reachable.
                if (panel.capturing()) {
                    if (key == SDLK_ESCAPE) {
                        panel.cancel_capture();
                    }
                    else if (key == SDLK_BACKSPACE) {
                        panel.capture_clear(true);
                    }
                    else if (key == SDLK_DELETE) {
                        panel.capture_clear(false);
                    }
                    else {
                        panel.capture_key(event.key.keysym.scancode);
                    }
                    continue;
                }
                if (key == SDLK_ESCAPE || key == SDLK_q) {
                    running = false;
                }
                else if (key == SDLK_TAB) {
                    const bool backward = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                    panel.switch_tab(backward ? -1 : 1);
                }
                else if (key == SDLK_UP) {
                    panel.move(-1);
                }
                else if (key == SDLK_DOWN) {
                    panel.move(1);
                }
                else if (key == SDLK_LEFT) {
                    step_option(-1);
                }
                else if (key == SDLK_RIGHT) {
                    step_option(1);
                }
                else if (key == SDLK_SPACE) {
                    activate_selected();
                    if (panel.capturing()) {
                        capture_started_ms = SDL_GetTicks64();
                    }
                }
                else if (key == SDLK_RETURN || key == SDLK_RETURN2 || key == SDLK_KP_ENTER) {
                    if (ready_rom.empty() || panel.selected_is_rom()) {
                        browse_and_accept();
                    }
                    else {
                        play();
                        if (running) {
                            browse_and_accept();
                        }
                    }
                }
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN &&
                     event.button.button == SDL_BUTTON_LEFT) {
                // The renderer output is in pixels while SDL mouse coordinates
                // are in window points; on a high-DPI display they differ.
                const int mouse_x = window_width > 0
                                        ? (event.button.x * output_width) / window_width
                                        : event.button.x;
                const int mouse_y = window_height > 0
                                        ? (event.button.y * output_height) / window_height
                                        : event.button.y;
                const ui::Panel::Hit hit = panel.hit_test(mouse_x, mouse_y);
                if (hit.kind == ui::Panel::Hit::Kind::Tab) {
                    const ui::Tab tab = static_cast<ui::Tab>(hit.index);
                    if (panel.tab_enabled(tab)) {
                        panel.set_tab(tab);
                    }
                }
                else if (hit.kind == ui::Panel::Hit::Kind::RowOption) {
                    const size_t index = static_cast<size_t>(hit.index);
                    if (index < panel.row_count() && panel.selectable(index)) {
                        panel.set_selected(index);
                        run_action(panel.choose_option(index, static_cast<size_t>(hit.option)));
                    }
                }
                else if (hit.kind == ui::Panel::Hit::Kind::Row) {
                    const size_t index = static_cast<size_t>(hit.index);
                    if (index < panel.row_count() && panel.selectable(index)) {
                        panel.set_selected(index);
                        run_action(panel.activate(index, 1));
                        if (panel.capturing()) {
                            capture_started_ms = SDL_GetTicks64();
                        }
                    }
                }
                else {
                    play();
                    if (running) {
                        browse_and_accept();
                    }
                }
            }
            else if (event.type == SDL_DROPFILE) {
                const std::filesystem::path dropped{event.drop.file};
                SDL_free(event.drop.file);
                handle_dropped(dropped);
            }
        }

        // A gamepad press rebinds the capturing row. The 200 ms grace keeps the
        // button that opened the capture from binding itself.
        if (panel.capturing() && SDL_GetTicks64() - capture_started_ms > 200) {
            const int pad = first_pressed_pad_button(pads.pads, PadList::capacity());
            if (pad >= 0) {
                panel.capture_pad_button(pad);
            }
        }

        char footer_text[512];
        std::snprintf(footer_text, sizeof(footer_text), "SAVED TO: %s",
                      context.pref_dir.string().c_str());

        ui::TextLayer title;
        ui::TextLayer footer;
        ui::TextLayer body;
        title.build(renderer, font, kTitle, px(kTitleScale), kTitleColor);
        footer.build(renderer, font, footer_text, px(kFooterScale), kFooterColor);

        std::vector<std::string> error_lines;
        if (!error.empty()) {
            const int max_width = (output_width * 3) / 4;
            error_lines = ui::wrap_text(
                error, ui::chars_per_line(font, px(kErrorScale), max_width));
        }

        // The panel measures itself so the whole block can be centred before the
        // top edge is chosen.
        const ui::PanelMetrics metrics =
            ui::measure_panel(font, panel, output_width, 0, ui_scale);

        const int title_height = px(kTitleScale) * Font::kCellHeight;
        const int line_height = px(kErrorScale) * Font::kCellHeight;
        int block_height = title_height + px(32) + px(2) + px(30);
        if (!error_lines.empty()) {
            block_height += px(6) + line_height + px(8);
            block_height += static_cast<int>(error_lines.size()) * (line_height + px(6));
            block_height += px(8) + line_height;
        }
        // The footer follows the panel instead of being pinned to the window
        // bottom: the CONTROLS tab is tall enough that the two would collide.
        block_height += px(30) + metrics.height + px(14) + footer.height();

        int cursor_y = std::max(px(24), (output_height - block_height) / 2);
        const int center_x = output_width / 2;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        title.draw(renderer, center_x, 0, cursor_y, true);
        cursor_y += title_height + px(32);

        // A thin warm rule under the wordmark, sized to it.
        SDL_SetRenderDrawColor(renderer, kWarmColor.r, kWarmColor.g, kWarmColor.b,
                               kWarmColor.a);
        const int rule_width =
            title.empty() ? output_width / 3 : (title.width() * 3) / 4;
        const SDL_Rect rule{center_x - rule_width / 2, cursor_y, rule_width, px(2)};
        SDL_RenderFillRect(renderer, &rule);
        cursor_y += px(2) + px(30);

        if (!error_lines.empty()) {
            body.build(renderer, font, kErrorTitle, px(kErrorScale), kErrorColor);
            body.draw(renderer, center_x, 0, cursor_y, true);
            cursor_y += line_height + px(8);
            for (const std::string& line : error_lines) {
                body.build(renderer, font, line, px(kErrorScale), kErrorColor);
                body.draw(renderer, center_x, 0, cursor_y, true);
                cursor_y += line_height + px(6);
            }
            cursor_y += px(8);
            body.build(renderer, font, kErrorHint, px(kErrorScale), kHintColor);
            body.draw(renderer, center_x, 0, cursor_y, true);
        }

        cursor_y += px(30);
        ui::draw_panel(renderer, font, panel, output_width, output_height, cursor_y, ui_scale);
        cursor_y += metrics.height + px(14);

        footer.draw(renderer, center_x, 0, cursor_y, true);

        if (shot_pending && !shot_written &&
            SDL_GetTicks64() - script_started_ms >= shot_at_ms) {
            shot_written = true;
            if (ui::write_ppm(renderer, shot_path)) {
                std::fprintf(stderr, "[launcher] wrote %s\n", shot_path);
            }
            else {
                std::fprintf(stderr, "[launcher] could not write %s: %s\n", shot_path,
                             SDL_GetError());
            }
            running = false;
        }

        SDL_RenderPresent(renderer);
    }

    pads.close_all();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    if (accepted.empty()) {
        std::fprintf(stderr, "[launcher] closed without a ROM\n");
    }
    else {
        std::fprintf(stderr, "[launcher] accepted %s\n", accepted.string().c_str());
    }
    return accepted;
}

}  // namespace ogre
