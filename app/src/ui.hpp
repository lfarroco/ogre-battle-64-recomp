// The app's ASCII UI: the launcher screen and the in-game overlay share it.
//
// The launcher (launcher.cpp) and the Esc overlay (overlay.cpp) show the same
// content in the same style, so the panel model, its rows, the tab bar and the
// text rendering live here. A Panel is either in Launcher mode (every tab
// active) or Overlay mode (only CONTROLS, SETTINGS and DEBUG are active; the
// rest are shown disabled, because starting the game, loading a ROM and
// toggling mods make no sense while the game is running).
//
// Rendering is the bitmap font (font.hpp) through an SDL_Renderer, into
// supersampled TextLayer textures, exactly as the launcher drew before.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL.h>

#include <librecomp/mods.hpp>

#include "font.hpp"
#include "input_map.hpp"

namespace ogre::ui {

// A text block rasterised once into a supersampled texture and scaled down by
// the GPU, so the bitmap glyphs come out anti-aliased at any integer scale.
class TextLayer {
public:
    TextLayer() = default;
    TextLayer(const TextLayer&) = delete;
    TextLayer& operator=(const TextLayer&) = delete;
    TextLayer(TextLayer&& other) noexcept;
    TextLayer& operator=(TextLayer&& other) noexcept;
    ~TextLayer();

    void destroy();

    // Safe to call repeatedly (every frame): each call replaces the texture.
    void build(SDL_Renderer* renderer, const Font& font, const std::string& text,
               int scale, SDL_Color color, int supersample = 4);

    void draw(SDL_Renderer* renderer, int center_x, int left, int top,
              bool centered) const;

    bool empty() const { return texture_ == nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

// Splits `text` so no line is wider than `max_chars` characters. Words are kept
// whole when they fit; a single over-long word (a file name) is cut.
std::vector<std::string> wrap_text(const std::string& text, size_t max_chars);

int chars_per_line(const Font& font, int scale, int max_width_px);

// `text` shortened with a trailing "..." so it fits `max_width` pixels at
// `scale`.
std::string truncate_to_width(const Font& font, const std::string& text, int scale,
                              int max_width);

// --- the tabbed panel ---------------------------------------------------------

enum class Tab { Start, Mods, Controls, Settings, Debug, Count };

constexpr int kTabCount = static_cast<int>(Tab::Count);

// The tab whose content sets the panel's on-screen position. CONTROLS is the
// tallest tab, so the header, the tab bar and the first row sit where they do
// on CONTROLS whatever tab is active.
constexpr Tab kLayoutReferenceTab = Tab::Controls;

// What activating a row asks the caller to do. Everything that touches the
// runtime (the file picker, the mod system) is handled outside the panel.
enum class RowAction {
    None,
    BrowseRom,
    Play,
    ModToggled,
    OptionChanged,
    BindingChanged,
    SettingChanged,
    CloseOverlay,
};

class Panel {
public:
    enum class Mode { Launcher, Overlay };

    enum class RowKind {
        Section,
        Start,
        Rom,
        Mod,
        Option,
        Binding,
        ResetBindings,
        GameSpeed,
        Widescreen,
        Sound,
        Volume,
        ChaosFrame,
    };

    struct Row {
        RowKind kind = RowKind::Section;
        std::string title;  // Section rows only
        size_t mod = 0;     // index into the mod list
        size_t option = 0;  // index into that mod's schema options
        int binding = -1;   // index into the input map (RowKind::Binding)
    };

    Panel(Mode mode, std::string game_id, std::filesystem::path rom);

    Mode mode() const { return mode_; }

    // Rebuilds the rows for the active tab. Mods can change underneath the panel
    // (enabling one can pull in a dependency), so this is called after a toggle.
    void reload();
    void set_rom(std::filesystem::path rom);

    // --- DEBUG tab readouts ---------------------------------------------------
    // The DEBUG tab shows live game state, so the panel does not read guest
    // memory itself: the caller samples the running game and pushes the value in
    // before draw_panel. The launcher has no running game and never calls the
    // setter, which is what makes its DEBUG tab say "GAME NOT RUNNING".
    void set_chaos_frame(int value) {
        chaos_frame_ = value;
        chaos_frame_known_ = true;
    }
    void clear_chaos_frame() { chaos_frame_known_ = false; }

    Tab tab() const { return tab_; }
    void set_tab(Tab tab);
    void switch_tab(int delta);
    bool tab_enabled(Tab tab) const;
    std::string tab_label(Tab tab) const;

    size_t row_count() const { return rows_.size(); }
    size_t selected() const { return selected_; }
    void set_selected(size_t index);
    // Puts the selection on the first selectable row of the active tab.
    void select_first();
    bool selectable(size_t index) const;
    bool selected_is_rom() const;
    RowKind row_kind(size_t index) const;
    bool move(int delta);

    // The text in the left column of a row (the name plus the keyboard or
    // selected value).
    std::string left_text(size_t index) const;
    // The text in the right column (a description, a file name, a hint).
    std::string right_text(size_t index) const;
    // The right column of an arbitrary row. measure_panel sizes the layout
    // reference tab with this, and that tab is not the active one.
    std::string right_text_for(const Row& row) const;

    // The active tab's rows, and the layout reference tab's rows. Both are
    // rebuilt by reload().
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<Row>& layout_rows() const { return layout_rows_; }

    // `direction` is +1 forward, -1 backward.
    RowAction activate(size_t index, int direction);
    // Picks option `option` of a radio row (GAME SPEED, WIDESCREEN, SOUNDS) or a
    // cell of the VOLUME slider directly (a click on it).
    RowAction choose_option(size_t index, size_t option);

    // --- rebinding capture ----------------------------------------------------
    // Activating a Binding row starts capture; the caller then feeds key and pad
    // events until one is accepted or the capture is cancelled.
    bool capturing() const { return capture_row_ >= 0; }
    void begin_capture(size_t index);
    void cancel_capture();
    // Returns true when the event was consumed by the capture.
    bool capture_key(SDL_Scancode code);
    bool capture_pad_button(int code);
    // Clears one slot of the capturing row (Backspace = keyboard, Delete = pad).
    bool capture_clear(bool keyboard);

    // --- hit testing ----------------------------------------------------------
    // Filled by draw_panel() each frame; clicks use the previous frame's
    // geometry, which is one frame of lag and not visible.
    struct Geometry {
        // One clickable radio marker of a radio row. `rect` is absolute, and
        // the entry is only present for markers draw_panel actually laid out.
        struct Option {
            int row = -1;
            int option = -1;
            SDL_Rect rect{};
        };
        std::vector<SDL_Rect> tabs;
        std::vector<SDL_Rect> rows;
        std::vector<Option> options;
    };
    struct Hit {
        enum class Kind { None, Tab, Row, RowOption } kind = Kind::None;
        int index = -1;   // tab or row index
        int option = -1;  // option index, for RowOption
    };
    void set_geometry(Geometry geometry) { geometry_ = std::move(geometry); }
    Hit hit_test(int x, int y) const;

private:
    void rebuild_rows();
    std::vector<Row> build_rows(Tab tab) const;
    size_t first_selectable() const;

    Mode mode_;
    Tab tab_ = Tab::Start;
    std::string game_id_;
    std::filesystem::path rom_;
    std::vector<recomp::mods::ModDetails> mods_;
    std::vector<Row> rows_;
    std::vector<Row> layout_rows_;
    Geometry geometry_;
    size_t selected_ = 0;
    int capture_row_ = -1;
    bool chaos_frame_known_ = false;
    int chaos_frame_ = 0;
};

// The panel's on-screen geometry from the last frame. `top_y` is where the panel
// background starts; the tab bar is drawn above the rows.
struct PanelDraw {
    int panel_left = 0;
    int panel_width = 0;
    int height = 0;   // tab bar + rows + hint
    int text_scale = 2;
};

// The panel's layout for one frame, computed without drawing so a caller can
// centre the whole screen before it commits to a top edge.
struct PanelMetrics {
    int panel_left = 0;
    int panel_width = 0;
    int scale = 2;
    int line_height = 0;
    int row_height = 0;
    int rows_top = 0;   // absolute y of the first row
    int hint_y = 0;     // absolute y of the hint line
    int height = 0;     // total panel height for the active tab, background included
    // The height the block reserves: the taller of the active tab's panel and
    // the layout reference tab's (kLayoutReferenceTab). A caller centres the
    // whole block and places its footer with this, so the header, the tab bar and
    // the footer stay put when the active tab changes. A tab that is taller than
    // the reference still grows the block, because it genuinely needs the room.
    int layout_height = 0;
    std::vector<int> row_heights;
    std::vector<std::vector<std::string>> right_lines;
};

PanelMetrics measure_panel(const Font& font, Panel& panel, int output_width,
                           int top_y, float ui_scale);

// Writes the renderer's current output as a binary PPM. Development aid for the
// screenshots in `docs/proofs`: it reads back exactly what the UI drew.
bool write_ppm(SDL_Renderer* renderer, const std::filesystem::path& path);

// Draws the tab bar and the active tab's rows, and stores the hit-test geometry
// in `panel`. `top_y` is the top of the tab bar.
PanelDraw draw_panel(SDL_Renderer* renderer, const Font& font, Panel& panel,
                     int output_width, int output_height, int top_y, float ui_scale);

}  // namespace ogre::ui
