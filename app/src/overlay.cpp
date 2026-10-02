// The in-game Esc overlay (see overlay.hpp).

#include "overlay.hpp"

#include <ultramodern/ultramodern.hpp>

#include "font.hpp"
#include "game.hpp"
#include "input_map.hpp"
#include "ui.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace ogre {
namespace {

constexpr int kTitleScale = 3;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

// The game's frame counter (`D_800AEFA4`, written by `func_80072398` on the
// frame-pump thread). It is 0 before the game produces its first frame and
// climbs every frame after that, so it is the cheapest evidence that the boot
// has reached steady state. `guest_word` reads it below.
constexpr uint32_t kFrameCounterAddress = 0x800AEFA4;


const SDL_Color kTitleColor{238, 238, 232, 255};
const SDL_Color kWarmColor{198, 160, 92, 220};
const SDL_Color kHintColor{122, 128, 138, 255};

struct OverlayState {
    bool initialized = false;
    bool visible = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Font font;
    std::unique_ptr<ui::Panel> panel;
    SDL_Window* game_window = nullptr;
    SDL_GameController* const* game_pads = nullptr;
    int game_pad_count = 0;

    // Rebinding capture state, so a gamepad press can be polled rather than
    // waiting for an SDL_CONTROLLERBUTTONDOWN event (the launcher and the game
    // close and reopen controllers, and a button that is already held produces
    // no down event).
    bool capture_active = false;
    uint64_t capture_started_ms = 0;

    // Env-gated test hooks.
    uint32_t show_at_ms = 0;
    bool show_at_fired = false;
    const char* shot_path = nullptr;
    bool shot_written = false;
    // OGRE_OVERLAY_SHOT_MS: delay the capture until the scripted keys have been
    // delivered. Zero captures the first visible frame.
    uint32_t shot_at_ms = 0;
    uint64_t shown_ticks = 0;
    uint64_t boot_ticks = 0;
    float opacity = 0.90f;

    // OGRE_OVERLAY_KEYS: synthetic key presses delivered once per 150 ms while
    // the overlay is visible, so a scripted run can navigate and rebind.
    std::vector<SDL_Scancode> keys;
    size_t key_next = 0;
    uint64_t keys_started_ms = 0;

    // OGRE_OVERLAY_PAD: the same scripted-run aid for the gamepad, as SDL
    // controller button codes. Unlike the key script this is delivered whether or
    // not the overlay is visible, because `back` is what opens it.
    std::vector<int> pad_buttons;
    size_t pad_next = 0;
    uint64_t pad_started_ms = 0;
};

OverlayState g_overlay;
std::atomic<bool> g_visible{false};

uint32_t env_millis(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    const long parsed = std::strtol(value, nullptr, 0);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

// One logical byte of the running game's RDRAM, or -1 while the runtime has no
// image yet. The runtime byte-reverses RDRAM, so guest byte `a` is
// `rdram[(a & 0x1FFFFFFF) ^ 3]` (docs/guides/app-build.md -> "Reading a dump").
int guest_byte(uint32_t address) {
    const uint8_t* rdram = ultramodern::get_rdram_base();
    if (rdram == nullptr) {
        return -1;
    }
    return rdram[(address & 0x1FFFFFFFu) ^ 3u];
}

// One logical 32-bit word of the running game's RDRAM, or -1 while the runtime
// has no image yet. Little-endian guest order, so byte `i` of the word is
// `guest_byte(address + i)`.
int guest_word(uint32_t address) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) {
        const int byte = guest_byte(address + i);
        if (byte < 0) {
            return -1;
        }
        value |= static_cast<uint32_t>(byte) << (8u * i);
    }
    return static_cast<int>(value);
}

// "Tab,Down,Space,p": SDL scancode names, comma separated, trimmed.
std::vector<SDL_Scancode> parse_key_list(const char* spec) {
    std::vector<SDL_Scancode> keys;
    if (spec == nullptr) {
        return keys;
    }
    std::string text = spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string name = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        if (!name.empty()) {
            const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
            if (code != SDL_SCANCODE_UNKNOWN) {
                keys.push_back(code);
            }
            else {
                std::fprintf(stderr, "[overlay] OGRE_OVERLAY_KEYS: unknown key '%s'\n",
                             name.c_str());
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return keys;
}

// "back,down,right,a": gamepad tags from input_map's table, comma separated,
// trimmed. This is the `OGRE_OVERLAY_PAD` script.
std::vector<int> parse_pad_list(const char* spec) {
    std::vector<int> buttons;
    if (spec == nullptr) {
        return buttons;
    }
    const std::string text = spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string name = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                       : comma - pos);
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, last - first + 1);
        }
        if (!name.empty()) {
            const int code = pad_button_from_name(name);
            if (code >= 0) {
                buttons.push_back(code);
            }
            else {
                std::fprintf(stderr, "[overlay] OGRE_OVERLAY_PAD: unknown button '%s'\n",
                             name.c_str());
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return buttons;
}

bool create_window() {
    // Sit exactly over the game window. Both are SDL windows, so the position
    // and size SDL reports for the game are what the overlay must use; the
    // renderer's output size (pixels) may differ on a high-DPI display.
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    int width = kWindowWidth;
    int height = kWindowHeight;
    if (g_overlay.game_window != nullptr) {
        SDL_GetWindowPosition(g_overlay.game_window, &x, &y);
        SDL_GetWindowSize(g_overlay.game_window, &width, &height);
    }

    const uint32_t flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                           SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SKIP_TASKBAR;
    g_overlay.window = SDL_CreateWindow("Ogre Battle 64: Options", x, y, width, height, flags);
    if (g_overlay.window == nullptr) {
        std::fprintf(stderr, "[overlay] window: %s\n", SDL_GetError());
        return false;
    }
    // Uniform window opacity is what lets the game show through: SDL has no
    // per-pixel alpha for a normal window, and the overlay is a separate window
    // rather than a renderer pass.
    SDL_SetWindowOpacity(g_overlay.window, g_overlay.opacity);

    g_overlay.renderer = SDL_CreateRenderer(g_overlay.window, -1, SDL_RENDERER_ACCELERATED);
    if (g_overlay.renderer == nullptr) {
        g_overlay.renderer = SDL_CreateRenderer(g_overlay.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (g_overlay.renderer == nullptr) {
        std::fprintf(stderr, "[overlay] renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_overlay.window);
        g_overlay.window = nullptr;
        return false;
    }
    SDL_SetRenderDrawBlendMode(g_overlay.renderer, SDL_BLENDMODE_BLEND);
    if (!g_overlay.font.init(g_overlay.renderer)) {
        std::fprintf(stderr, "[overlay] font init failed\n");
        SDL_DestroyRenderer(g_overlay.renderer);
        SDL_DestroyWindow(g_overlay.window);
        g_overlay.renderer = nullptr;
        g_overlay.window = nullptr;
        return false;
    }
    std::fprintf(stderr, "[overlay] window ready\n");
    return true;
}

// Keeps the overlay over the game window as it moves and resizes.
void sync_geometry() {
    if (g_overlay.window == nullptr || g_overlay.game_window == nullptr) {
        return;
    }
    int game_x = 0;
    int game_y = 0;
    int game_w = 0;
    int game_h = 0;
    SDL_GetWindowPosition(g_overlay.game_window, &game_x, &game_y);
    SDL_GetWindowSize(g_overlay.game_window, &game_w, &game_h);
    if (game_w <= 0 || game_h <= 0) {
        return;
    }
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    SDL_GetWindowPosition(g_overlay.window, &x, &y);
    SDL_GetWindowSize(g_overlay.window, &w, &h);
    if (x != game_x || y != game_y) {
        SDL_SetWindowPosition(g_overlay.window, game_x, game_y);
    }
    if (w != game_w || h != game_h) {
        SDL_SetWindowSize(g_overlay.window, game_w, game_h);
    }
}

void show() {
    if (!g_overlay.initialized) {
        return;
    }
    if (g_overlay.window == nullptr && !create_window()) {
        return;
    }
    sync_geometry();
    SDL_ShowWindow(g_overlay.window);
    SDL_RaiseWindow(g_overlay.window);
    g_overlay.visible = true;
    g_visible.store(true, std::memory_order_relaxed);
    g_overlay.shown_ticks = SDL_GetTicks64();
    if (g_overlay.keys_started_ms == 0) {
        g_overlay.keys_started_ms = SDL_GetTicks64();
    }
    std::fprintf(stderr, "[overlay] shown\n");
    std::fflush(stderr);
}

void hide() {
    if (g_overlay.panel != nullptr) {
        g_overlay.panel->cancel_capture();
    }
    g_overlay.capture_active = false;
    g_overlay.visible = false;
    g_visible.store(false, std::memory_order_relaxed);
    if (g_overlay.window != nullptr) {
        SDL_HideWindow(g_overlay.window);
    }
    // Hand the keyboard back to the game.
    if (g_overlay.game_window != nullptr) {
        SDL_RaiseWindow(g_overlay.game_window);
    }
    std::fprintf(stderr, "[overlay] hidden\n");
    std::fflush(stderr);
}

// The MAIN tab's EXIT GAME row. The panel cannot end the process itself, so it
// reports `RowAction::QuitGame`, `overlay_handle_event` records it here, and
// `overlay_render` hands it to `main.cpp`'s frame callback. That callback is the
// one place that knows how a run ends (it prints the dumps and calls
// `ultramodern::quit`), so EXIT GAME leaves through the same path as the game
// window's close button instead of a second teardown.
std::atomic<bool> g_quit_requested{false};

void request_quit() {
    g_quit_requested.store(true, std::memory_order_relaxed);
}

void draw() {
    if (!g_overlay.visible || g_overlay.renderer == nullptr) {
        return;
    }
    sync_geometry();

    int output_width = 0;
    int output_height = 0;
    SDL_GetRendererOutputSize(g_overlay.renderer, &output_width, &output_height);
    if (output_width <= 0 || output_height <= 0) {
        return;
    }
    const float ui_scale =
        std::max(1.0f, static_cast<float>(output_height) / kWindowHeight);
    auto px = [ui_scale](int value) {
        return static_cast<int>(static_cast<float>(value) * ui_scale);
    };

    // The DEBUG tab reads the game's own state. Sample it here, before the panel
    // measures or draws, because both read the row text back.
    const int chaos_frame = guest_byte(CHAOS_FRAME_ADDRESS);
    if (chaos_frame >= 0) {
        g_overlay.panel->set_chaos_frame(chaos_frame);
    }
    else {
        g_overlay.panel->clear_chaos_frame();
    }

    SDL_SetRenderDrawColor(g_overlay.renderer, 8, 9, 12, 255);
    SDL_RenderClear(g_overlay.renderer);

    ui::TextLayer title;
    ui::TextLayer body;
    const int title_height = px(kTitleScale) * Font::kCellHeight;
    const int center_x = output_width / 2;

    const ui::PanelMetrics metrics =
        ui::measure_panel(g_overlay.font, *g_overlay.panel, output_width, 0, ui_scale);

    const int rule_height = px(2);
    // The block is laid out with the layout reference tab's height, so the
    // header, the tab bar and the footer do not move when the active tab
    // changes.
    const int gap = px(20);
    const int footer_height = px(2) * Font::kCellHeight;
    int block_height = title_height + px(30) + rule_height + px(26) +
                       metrics.layout_height + gap + footer_height;
    int cursor_y = std::max(px(24), (output_height - block_height) / 2);

    title.build(g_overlay.renderer, g_overlay.font, "OGRE BATTLE 64: RECOMP",
                px(kTitleScale), kTitleColor);
    title.draw(g_overlay.renderer, center_x, 0, cursor_y, true);
    cursor_y += title_height + px(30);

    SDL_SetRenderDrawColor(g_overlay.renderer, kWarmColor.r, kWarmColor.g, kWarmColor.b,
                           kWarmColor.a);
    const int rule_width = title.empty() ? output_width / 3 : (title.width() * 3) / 4;
    const SDL_Rect rule{center_x - rule_width / 2, cursor_y, rule_width, rule_height};
    SDL_RenderFillRect(g_overlay.renderer, &rule);
    cursor_y += rule_height + px(26);

    ui::draw_panel(g_overlay.renderer, g_overlay.font, *g_overlay.panel, output_width,
                   output_height, cursor_y, ui_scale);
    cursor_y += metrics.layout_height + gap;

    body.build(g_overlay.renderer, g_overlay.font, "ESC RESUMES THE GAME",
               px(2), kHintColor);
    body.draw(g_overlay.renderer, center_x, 0, cursor_y, true);

    if (g_overlay.shot_path != nullptr && !g_overlay.shot_written &&
        (g_overlay.shot_at_ms == 0 ||
         SDL_GetTicks64() - g_overlay.shown_ticks >= g_overlay.shot_at_ms)) {
        if (ui::write_ppm(g_overlay.renderer, g_overlay.shot_path)) {
            std::fprintf(stderr, "[overlay] wrote %s\n", g_overlay.shot_path);
        }
        else {
            std::fprintf(stderr, "[overlay] could not write %s: %s\n",
                         g_overlay.shot_path, SDL_GetError());
        }
        g_overlay.shot_written = true;
    }

    SDL_RenderPresent(g_overlay.renderer);
}

}  // namespace

void overlay_init(Platform& platform, const std::filesystem::path& pref_dir,
                  const std::string& mod_game_id) {
    (void)pref_dir;
    g_overlay.game_window = platform.window;
    g_overlay.game_pads = platform.controllers;
    g_overlay.game_pad_count = 4;
    g_overlay.panel = std::make_unique<ui::Panel>(ui::Panel::Mode::Overlay, mod_game_id,
                                                  std::filesystem::path{});
    // OGRE_OVERLAY_TAB=<main|controls|settings|debug>: open the panel on that
    // tab, so a scripted run or a screenshot reaches it without input.
    if (const char* tab = std::getenv("OGRE_OVERLAY_TAB")) {
        const std::string name = tab;
        if (name == "main" || name == "start") {
            g_overlay.panel->set_tab(ui::Tab::Main);
        }
        else if (name == "settings" || name == "setting") {
            g_overlay.panel->set_tab(ui::Tab::Settings);
        }
        else if (name == "controls" || name == "controller") {
            g_overlay.panel->set_tab(ui::Tab::Controls);
        }
        else if (name == "debug") {
            g_overlay.panel->set_tab(ui::Tab::Debug);
        }
    }
    g_overlay.boot_ticks = SDL_GetTicks64();
    g_overlay.show_at_ms = env_millis("OGRE_OVERLAY_AT_MS");
    g_overlay.shot_at_ms = env_millis("OGRE_OVERLAY_SHOT_MS");
    if (const char* path = std::getenv("OGRE_CAPTURE_OVERLAY");
        path != nullptr && path[0] != '\0') {
        g_overlay.shot_path = path;
    }
    if (const char* opacity = std::getenv("OGRE_OVERLAY_OPACITY"); opacity != nullptr) {
        const float value = static_cast<float>(std::atof(opacity));
        if (value >= 0.2f && value <= 1.0f) {
            g_overlay.opacity = value;
        }
    }
    g_overlay.keys = parse_key_list(std::getenv("OGRE_OVERLAY_KEYS"));
    // The pad script's clock starts here rather than at the first show, because
    // its first press is what a scripted run uses to open the overlay.
    g_overlay.pad_buttons = parse_pad_list(std::getenv("OGRE_OVERLAY_PAD"));
    g_overlay.pad_started_ms = SDL_GetTicks64();
    g_overlay.initialized = true;

    const char* start = std::getenv("OGRE_OVERLAY");
    if (start != nullptr && start[0] != '\0' && std::strcmp(start, "0") != 0) {
        show();
    }
    std::fprintf(stderr, "[overlay] ready\n");
}

void overlay_shutdown() {
    if (g_overlay.renderer != nullptr) {
        SDL_DestroyRenderer(g_overlay.renderer);
        g_overlay.renderer = nullptr;
    }
    if (g_overlay.window != nullptr) {
        // The panel's window is destroyed here and not merely hidden. It is an
        // SDL window like any other, and SDL synthesises SDL_QUIT from a
        // window's close event only while that window is the last one in its
        // window list, so leaving it alive keeps the game window from being the
        // last one for the rest of the run.
        SDL_DestroyWindow(g_overlay.window);
        g_overlay.window = nullptr;
    }
    g_overlay.panel.reset();
    g_overlay.initialized = false;
    g_overlay.visible = false;
    g_visible.store(false, std::memory_order_relaxed);
}

void overlay_toggle() {
    if (g_overlay.visible) {
        hide();
    }
    else {
        show();
    }
}

bool overlay_visible() {
    return g_visible.load(std::memory_order_relaxed);
}

bool overlay_quit_requested() {
    return g_quit_requested.load(std::memory_order_relaxed);
}

int overlay_frames_produced() {
    return guest_word(kFrameCounterAddress);
}

void overlay_handle_event(const SDL_Event& event) {
    if (!g_overlay.initialized || !g_overlay.visible || g_overlay.panel == nullptr) {
        // Escape opens the overlay from the game; everything else is the game's.
        // A held key's auto-repeat must not toggle it back and forth.
        if (g_overlay.initialized && event.type == SDL_KEYDOWN && !event.key.repeat &&
            event.key.keysym.sym == SDLK_ESCAPE) {
            show();
        }
        // A pad's Back button opens it too, but only while the input map has
        // nothing on that button: the overlay must not also press an N64 button
        // in the game every time it is opened.
        else if (g_overlay.initialized && event.type == SDL_CONTROLLERBUTTONDOWN &&
                 event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK &&
                 !pad_button_is_bound(SDL_CONTROLLER_BUTTON_BACK)) {
            show();
        }
        return;
    }

    switch (event.type) {
        case SDL_KEYDOWN: {
            const SDL_Keycode key = event.key.keysym.sym;
            if (g_overlay.panel->capturing()) {
                if (key == SDLK_ESCAPE) {
                    g_overlay.panel->cancel_capture();
                }
                else if (key == SDLK_BACKSPACE) {
                    g_overlay.panel->capture_clear(true);
                }
                else if (key == SDLK_DELETE) {
                    g_overlay.panel->capture_clear(false);
                }
                else {
                    g_overlay.panel->capture_key(event.key.keysym.scancode);
                }
                break;
            }
            if (key == SDLK_ESCAPE) {
                if (!event.key.repeat) {
                    hide();
                }
            }
            else if (key == SDLK_TAB) {
                const bool backward = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                g_overlay.panel->switch_tab(backward ? -1 : 1);
            }
            else if (key == SDLK_UP) {
                g_overlay.panel->move(-1);
            }
            else if (key == SDLK_DOWN) {
                g_overlay.panel->move(1);
            }
            else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                const size_t index = g_overlay.panel->selected();
                if (g_overlay.panel->row_is_steppable(index)) {
                    g_overlay.panel->activate(index, key == SDLK_RIGHT ? 1 : -1);
                }
            }
            else if (key == SDLK_SPACE || key == SDLK_RETURN || key == SDLK_RETURN2 ||
                     key == SDLK_KP_ENTER) {
                const size_t index = g_overlay.panel->selected();
                if (index < g_overlay.panel->row_count() &&
                    g_overlay.panel->selectable(index)) {
                    const ui::RowAction action = g_overlay.panel->activate(index, 1);
                    if (action == ui::RowAction::QuitGame) {
                        request_quit();
                    }
                    if (g_overlay.panel->capturing()) {
                        g_overlay.capture_active = true;
                        g_overlay.capture_started_ms = SDL_GetTicks64();
                    }
                }
            }
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN: {
            // A rebind capture owns the pad while it is armed; `overlay_render`
            // polls the held button into it, and `Back` is bindable then like any
            // other button.
            if (g_overlay.panel->capturing()) {
                break;
            }
            // Back closes the panel. It is the same button that opens it, and the
            // game does not see it: `get_input` reports an idle pad while the
            // overlay is visible.
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                hide();
                break;
            }
            const ui::Panel::PadCommand command =
                ui::Panel::pad_command_for_button(event.cbutton.button);
            if (command == ui::Panel::PadCommand::None) {
                break;
            }
            const ui::RowAction action = g_overlay.panel->pad_command(command);
            if (action == ui::RowAction::QuitGame) {
                request_quit();
            }
            if (g_overlay.panel->capturing()) {
                g_overlay.capture_active = true;
                g_overlay.capture_started_ms = SDL_GetTicks64();
            }
            break;
        }

        case SDL_MOUSEBUTTONDOWN: {
            if (event.button.button != SDL_BUTTON_LEFT) {
                break;
            }
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(g_overlay.renderer, &output_width, &output_height);
            int window_width = 0;
            int window_height = 0;
            SDL_GetWindowSize(g_overlay.window, &window_width, &window_height);
            const int mouse_x = window_width > 0
                                    ? (event.button.x * output_width) / window_width
                                    : event.button.x;
            const int mouse_y = window_height > 0
                                    ? (event.button.y * output_height) / window_height
                                    : event.button.y;
            const ui::Panel::Hit hit = g_overlay.panel->hit_test(mouse_x, mouse_y);
            if (hit.kind == ui::Panel::Hit::Kind::Tab) {
                const ui::Tab tab = static_cast<ui::Tab>(hit.index);
                if (g_overlay.panel->tab_enabled(tab)) {
                    g_overlay.panel->set_tab(tab);
                }
            }
            else if (hit.kind == ui::Panel::Hit::Kind::RowOption) {
                const size_t index = static_cast<size_t>(hit.index);
                if (index < g_overlay.panel->row_count() &&
                    g_overlay.panel->selectable(index)) {
                    g_overlay.panel->set_selected(index);
                    g_overlay.panel->choose_option(index, static_cast<size_t>(hit.option));
                }
            }
            else if (hit.kind == ui::Panel::Hit::Kind::Row) {
                const size_t index = static_cast<size_t>(hit.index);
                if (index < g_overlay.panel->row_count() &&
                    g_overlay.panel->selectable(index)) {
                    g_overlay.panel->set_selected(index);
                    const ui::RowAction action = g_overlay.panel->activate(index, 1);
                    if (action == ui::RowAction::QuitGame) {
                        request_quit();
                    }
                    if (g_overlay.panel->capturing()) {
                        g_overlay.capture_active = true;
                        g_overlay.capture_started_ms = SDL_GetTicks64();
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void overlay_render() {
    if (!g_overlay.initialized) {
        return;
    }
    // OGRE_OVERLAY_AT_MS=<n>: press Escape once, n ms after init, so a bounded
    // scripted run can exercise the overlay with no human at the keyboard. The
    // event goes through the same handler a real key press uses.
    if (g_overlay.show_at_ms != 0 && !g_overlay.show_at_fired &&
        SDL_GetTicks64() - g_overlay.boot_ticks >= g_overlay.show_at_ms) {
        g_overlay.show_at_fired = true;
        SDL_Event synthetic{};
        synthetic.type = SDL_KEYDOWN;
        synthetic.key.state = SDL_PRESSED;
        synthetic.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        synthetic.key.keysym.sym = SDLK_ESCAPE;
        if (g_overlay.game_window != nullptr) {
            synthetic.key.windowID = SDL_GetWindowID(g_overlay.game_window);
        }
        SDL_PushEvent(&synthetic);
    }

    // Deliver the next scripted pad press. This runs whether or not the overlay
    // is visible: the script's first `back` is what a headless run uses to open
    // it, exactly as a real controller's Back button does.
    if (g_overlay.pad_next < g_overlay.pad_buttons.size() &&
        SDL_GetTicks64() - g_overlay.pad_started_ms >= g_overlay.pad_next * 150) {
        SDL_Event synthetic{};
        synthetic.type = SDL_CONTROLLERBUTTONDOWN;
        synthetic.cbutton.state = SDL_PRESSED;
        synthetic.cbutton.button =
            static_cast<Uint8>(g_overlay.pad_buttons[g_overlay.pad_next++]);
        SDL_PushEvent(&synthetic);
    }

    if (!g_overlay.visible) {
        return;
    }

    // EXIT GAME asked to leave during this frame's events, and the caller acts
    // on it right after this call. Drawing the panel over the frame the run
    // ends on would be one wasted present.
    if (g_quit_requested.load(std::memory_order_relaxed)) {
        return;
    }

    // Deliver the next scripted key, exactly as a real key event would arrive.
    if (g_overlay.key_next < g_overlay.keys.size() &&
        SDL_GetTicks64() - g_overlay.keys_started_ms >= g_overlay.key_next * 150) {
        const SDL_Scancode code = g_overlay.keys[g_overlay.key_next++];
        SDL_Event synthetic{};
        synthetic.type = SDL_KEYDOWN;
        synthetic.key.state = SDL_PRESSED;
        synthetic.key.keysym.scancode = code;
        synthetic.key.keysym.sym = SDL_GetKeyFromScancode(code);
        synthetic.key.windowID = SDL_GetWindowID(g_overlay.window);
        SDL_PushEvent(&synthetic);
    }

    // A gamepad press rebinds the capturing row. Polling is what makes an
    // already-held button work; the 200 ms grace keeps the button that opened
    // the capture from binding itself.
    if (g_overlay.panel != nullptr && g_overlay.panel->capturing() &&
        g_overlay.capture_active && g_overlay.game_pads != nullptr &&
        SDL_GetTicks64() - g_overlay.capture_started_ms > 200) {
        const int pad = first_pressed_pad_button(g_overlay.game_pads, g_overlay.game_pad_count);
        if (pad >= 0) {
            g_overlay.panel->capture_pad_button(pad);
            g_overlay.capture_active = false;
        }
    }
    else if (g_overlay.panel != nullptr && !g_overlay.panel->capturing()) {
        g_overlay.capture_active = false;
    }

    draw();
}

}  // namespace ogre
