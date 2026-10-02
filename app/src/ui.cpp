// The app's ASCII UI (see ui.hpp).

#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include "settings.hpp"

namespace ogre::ui {
namespace {

// The launcher's scale and colours. The overlay reuses them so the two screens
// look like one program.
constexpr int kTextScale = 2;
constexpr int kTabScale = 2;

const SDL_Color kTitleColor{238, 238, 232, 255};
const SDL_Color kSubtitleColor{196, 200, 208, 255};
const SDL_Color kHintColor{122, 128, 138, 255};
const SDL_Color kWarmColor{198, 160, 92, 220};
const SDL_Color kPanelColor{20, 22, 27, 240};
const SDL_Color kSelectColor{52, 58, 70, 255};

// Pads `text` with spaces on the right so a column of rows lines up. The font is
// fixed-cell, so space padding is exact.
std::string pad_to(const std::string& text, size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

// The displayed value of one mod config option. Enum values show the option
// name, not the number, so the panel reads like the mod's own description.
std::string config_value_text(const recomp::config::ConfigOption& option,
                              const recomp::config::ConfigValueVariant& value) {
    using recomp::config::ConfigOptionType;
    switch (option.type) {
        case ConfigOptionType::Enum: {
            const auto& enumeration =
                std::get<recomp::config::ConfigOptionEnum>(option.variant);
            const uint32_t current = std::holds_alternative<uint32_t>(value)
                                         ? std::get<uint32_t>(value)
                                         : enumeration.default_value;
            const auto found = enumeration.find_option_from_value(current);
            return found != enumeration.options.end() ? found->name
                                                      : std::to_string(current);
        }
        case ConfigOptionType::Bool: {
            const bool current = std::holds_alternative<bool>(value)
                                     ? std::get<bool>(value)
                                     : std::get<recomp::config::ConfigOptionBool>(option.variant).default_value;
            return current ? "on" : "off";
        }
        case ConfigOptionType::Number: {
            const auto& number =
                std::get<recomp::config::ConfigOptionNumber>(option.variant);
            const double current = std::holds_alternative<double>(value)
                                       ? std::get<double>(value)
                                       : number.default_value;
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "%.*f", number.precision, current);
            return buffer;
        }
        case ConfigOptionType::String: {
            return std::holds_alternative<std::string>(value) ? std::get<std::string>(value)
                                                              : std::string{};
        }
        default:
            return {};
    }
}

const char* kLauncherHint =
    "TAB SWITCHES TAB   UP/DOWN SELECT   SPACE ACTIVATE   LEFT/RIGHT CHANGE";
const char* kOverlayHint =
    "TAB SWITCHES TAB   UP/DOWN SELECT   LEFT/RIGHT CHANGE   ESC RESUMES THE GAME";
const char* kCaptureHint =
    "PRESS A KEY OR GAMEPAD BUTTON   BACKSPACE CLEARS KEY   DELETE CLEARS PAD   ESC CANCELS";

// --- radio rows (GAME SPEED, WIDESCREEN) --------------------------------------
// A radio row draws its options as markers in the second column instead of one
// text run, so measure_panel (the row height) and draw_panel (the markers and
// their click rects) lay the same group out with this helper. The result is a
// pure function of its arguments, so the measured and the drawn row agree.

struct RadioToken {
    std::string text;
    int x = 0;
    int y = 0;
    int option = -1;
};

struct RadioLayout {
    std::vector<RadioToken> tokens;
    std::vector<SDL_Rect> option_rects;  // relative to the row's top-left
    int height = 0;
};

// `labels` in display order; `current` is the index of the live option, or -1
// when the live value is not one the row offers.
RadioLayout layout_radio(const Font& font, int scale, int max_width,
                         const std::vector<std::string>& labels, int current) {
    RadioLayout layout;
    const int line_height = scale * Font::kCellHeight;
    const int space = font.width(" ", scale);
    int x = 0;
    int y = 0;
    for (size_t i = 0; i < labels.size(); i++) {
        const std::string marker = static_cast<int>(i) == current ? "[x]" : "[ ]";
        const int marker_width = font.width(marker, scale);
        const int option_width = marker_width + space + font.width(labels[i], scale);
        // A narrow window wraps the group onto another line; the option itself
        // never splits.
        if (x > 0 && x + option_width > max_width) {
            x = 0;
            y += line_height;
        }
        layout.tokens.push_back(
            RadioToken{marker, x, y, static_cast<int>(i)});
        layout.tokens.push_back(
            RadioToken{labels[i], x + marker_width + space, y, static_cast<int>(i)});
        layout.option_rects.push_back(SDL_Rect{x, y, option_width, line_height});
        x += option_width + space;
    }
    layout.height = y + line_height;
    return layout;
}

std::vector<std::string> game_speed_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kGameSpeedCount; i++) {
        labels.push_back(std::to_string(game_speed_value(i)));
    }
    return labels;
}

RadioLayout layout_game_speed(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, game_speed_labels(),
                        game_speed_index(current));
}

std::vector<std::string> widescreen_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kWidescreenModeCount; i++) {
        labels.push_back(widescreen_mode_label(i));
    }
    return labels;
}

RadioLayout layout_widescreen(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, widescreen_labels(), current);
}

std::vector<std::string> resolution_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kResolutionModeCount; i++) {
        labels.push_back(resolution_mode_label(i));
    }
    return labels;
}

RadioLayout layout_resolution(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, resolution_labels(), current);
}

std::vector<std::string> antialias_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kAntialiasModeCount; i++) {
        labels.push_back(antialias_mode_label(i));
    }
    return labels;
}

RadioLayout layout_antialias(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, antialias_labels(), current);
}

std::vector<std::string> display_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kDisplayModeCount; i++) {
        labels.push_back(display_mode_label(i));
    }
    return labels;
}

RadioLayout layout_display(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, display_labels(), current);
}

std::vector<std::string> sound_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < kSoundModeCount; i++) {
        labels.push_back(sound_mode_label(i));
    }
    return labels;
}

RadioLayout layout_sound(const Font& font, int scale, int max_width, int current) {
    return layout_radio(font, scale, max_width, sound_labels(), current);
}

// --- radio rows ---------------------------------------------------------------
// Six row kinds draw as a radio group (GAME SPEED, WIDESCREEN, RESOLUTION, MSAA,
// WINDOW, SOUNDS). The measure, draw, step, click and pad paths each need the
// layout and the live option, so both are looked up in one place: a kind added
// to the table is handled by all of them.

bool is_radio_row(Panel::RowKind kind) {
    switch (kind) {
        case Panel::RowKind::GameSpeed:
        case Panel::RowKind::Widescreen:
        case Panel::RowKind::Resolution:
        case Panel::RowKind::Antialias:
        case Panel::RowKind::Display:
        case Panel::RowKind::Sound:
            return true;
        default:
            return false;
    }
}

// The index of the row's live option, or -1 when the live value is not one the
// row offers.
int radio_row_current(Panel::RowKind kind) {
    switch (kind) {
        case Panel::RowKind::GameSpeed:
            return game_speed_index(game_speed());
        case Panel::RowKind::Widescreen:
            return widescreen_mode_index();
        case Panel::RowKind::Resolution:
            return resolution_mode_index();
        case Panel::RowKind::Antialias:
            return antialias_mode_index();
        case Panel::RowKind::Display:
            return display_mode_index();
        case Panel::RowKind::Sound:
            return sound_mode_index();
        default:
            return -1;
    }
}

int radio_row_count(Panel::RowKind kind) {
    switch (kind) {
        case Panel::RowKind::GameSpeed:   return kGameSpeedCount;
        case Panel::RowKind::Widescreen:  return kWidescreenModeCount;
        case Panel::RowKind::Resolution:  return kResolutionModeCount;
        case Panel::RowKind::Antialias:   return kAntialiasModeCount;
        case Panel::RowKind::Display:     return kDisplayModeCount;
        case Panel::RowKind::Sound:       return kSoundModeCount;
        default:                          return 0;
    }
}

RadioLayout radio_row_layout(Panel::RowKind kind, const Font& font, int scale, int max_width,
                             int current) {
    switch (kind) {
        case Panel::RowKind::GameSpeed:
            return layout_game_speed(font, scale, max_width, game_speed());
        case Panel::RowKind::Widescreen:
            return layout_widescreen(font, scale, max_width, current);
        case Panel::RowKind::Resolution:
            return layout_resolution(font, scale, max_width, current);
        case Panel::RowKind::Antialias:
            return layout_antialias(font, scale, max_width, current);
        case Panel::RowKind::Display:
            return layout_display(font, scale, max_width, current);
        default:
            return layout_sound(font, scale, max_width, sound_mode_index());
    }
}

// Stores option `option` of a radio row. Returns the action the caller's
// `activate` or `choose_option` reports; an option that is already live is a
// no-op.
RowAction store_radio_row(Panel::RowKind kind, int option) {
    if (option < 0 || option >= radio_row_count(kind) || option == radio_row_current(kind)) {
        return RowAction::None;
    }
    switch (kind) {
        case Panel::RowKind::GameSpeed:
            set_game_speed(game_speed_value(option));
            break;
        case Panel::RowKind::Widescreen:
            set_widescreen_mode(static_cast<WidescreenMode>(option));
            break;
        case Panel::RowKind::Resolution:
            set_resolution_mode(static_cast<ResolutionMode>(option));
            break;
        case Panel::RowKind::Antialias:
            set_antialias_mode(static_cast<AntialiasMode>(option));
            break;
        case Panel::RowKind::Display:
            set_display_mode(static_cast<DisplayMode>(option));
            break;
        case Panel::RowKind::Sound:
            set_sound_mode(static_cast<SoundMode>(option));
            break;
        default:
            return RowAction::None;
    }
    return RowAction::SettingChanged;
}

// Steps a radio row by `direction`, wrapping at the ends, and stores the new
// option.
RowAction step_radio_row(Panel::RowKind kind, int direction) {
    const int count = radio_row_count(kind);
    if (count <= 0) {
        return RowAction::None;
    }
    int current = radio_row_current(kind);
    if (current < 0) {
        // A value that came from an environment override and is not one of the
        // offered steps starts the walk at the first step.
        current = 0;
    }
    current = ((current + direction) % count + count) % count;
    return store_radio_row(kind, current);
}

// --- slider rows (VOLUME) -----------------------------------------------------
// The volume row draws one text run, the bar with its handle and the percent,
// and then lays a click rect over each bar cell. The layout is a pure function
// of its arguments, so the measured and the drawn row agree, as with the radio
// rows above.

struct SliderLayout {
    std::string text;
    std::vector<SDL_Rect> cell_rects;  // relative to the row's top-left
    int height = 0;
};

SliderLayout layout_volume(const Font& font, int scale, int percent) {
    SliderLayout layout;
    layout.text = volume_text(percent);
    layout.height = scale * Font::kCellHeight;
    const int cell_width = font.width("-", scale);
    for (int cell = 0; cell < kVolumeCells; cell++) {
        layout.cell_rects.push_back(SDL_Rect{cell * cell_width, 0, cell_width, layout.height});
    }
    return layout;
}

// The GAME SPEED group as one string, for right_text (the drawn row uses the
// layout above; this is what a caller outside the panel reads).
std::string game_speed_text(int current) {
    std::string text;
    for (int i = 0; i < kGameSpeedCount; i++) {
        if (!text.empty()) {
            text += "  ";
        }
        text += std::string(game_speed_value(i) == current ? "[x] " : "[ ] ") +
                std::to_string(game_speed_value(i));
    }
    return text;
}

// The band a Chaos Frame value falls in, which is what it means: the endings
// are chosen by these ranges (developer, this session): 0-35 low, 36-64
// neutral, 65-100 high.
const char* chaos_frame_band(int value) {
    if (value <= 35) {
        return "LOW";
    }
    if (value <= 64) {
        return "NEUTRAL";
    }
    return "HIGH";
}

// --- row heights --------------------------------------------------------------
// One tab's row heights at the measured scale. `panel` supplies the right-column
// text, which depends on the panel's ROM, mods and input map. `right_lines` is
// filled only when `keep_lines` is set: only the active tab is drawn, so the
// layout reference tab needs its total height alone.

struct RowsLayout {
    std::vector<int> heights;
    // The wrapped lines of a section row's title (the left column of a section
    // spans the whole panel, because a section has no description) and of a
    // row's right column.
    std::vector<std::vector<std::string>> left_lines;
    std::vector<std::vector<std::string>> right_lines;
    int total = 0;
};

RowsLayout layout_rows(const Font& font, const Panel& panel, const std::vector<Panel::Row>& rows,
                       int scale, int row_height, int section_height, int description_width,
                       int full_width, int wrap_pad, bool keep_lines) {
    const int line_height = scale * Font::kCellHeight;
    RowsLayout layout;
    layout.heights.assign(rows.size(), row_height);
    if (keep_lines) {
        layout.left_lines.assign(rows.size(), {});
        layout.right_lines.assign(rows.size(), {});
    }
    for (size_t i = 0; i < rows.size(); i++) {
        const Panel::Row& row = rows[i];
        if (row.kind == Panel::RowKind::Section) {
            // A section row's title is drawn in the left column and has no
            // description, so it may use the whole panel and wrap there. A
            // one-line section keeps the height it had before this could wrap.
            std::vector<std::string> lines =
                wrap_text(row.title, chars_per_line(font, scale, full_width));
            const int count = static_cast<int>(lines.size());
            layout.heights[i] = std::max(section_height, count * line_height + (section_height - line_height));
            if (keep_lines) {
                layout.left_lines[i] = std::move(lines);
            }
        }
        else if (is_radio_row(row.kind)) {
            // The radio group is laid out by option, not wrapped as one string;
            // its height is what the row must reserve.
            const RadioLayout radio = radio_row_layout(
                row.kind, font, scale, description_width, radio_row_current(row.kind));
            layout.heights[i] = std::max(row_height, radio.height);
        }
        else if (row.kind == Panel::RowKind::Volume) {
            const SliderLayout slider = layout_volume(font, scale, volume_percent());
            layout.heights[i] = std::max(row_height, slider.height);
        }
        else {
            const std::string right = panel.right_text_for(row);
            if (!right.empty()) {
                std::vector<std::string> lines =
                    wrap_text(right, chars_per_line(font, scale, description_width));
                const int count = static_cast<int>(lines.size());
                layout.heights[i] = std::max(row_height, count * line_height + wrap_pad);
                if (keep_lines) {
                    layout.right_lines[i] = std::move(lines);
                }
            }
        }
        layout.total += layout.heights[i];
    }
    return layout;
}

}  // namespace

// --- TextLayer ----------------------------------------------------------------

TextLayer::TextLayer(TextLayer&& other) noexcept
    : texture_(other.texture_), width_(other.width_), height_(other.height_) {
    other.texture_ = nullptr;
}

TextLayer& TextLayer::operator=(TextLayer&& other) noexcept {
    if (this != &other) {
        destroy();
        texture_ = other.texture_;
        width_ = other.width_;
        height_ = other.height_;
        other.texture_ = nullptr;
    }
    return *this;
}

TextLayer::~TextLayer() {
    destroy();
}

void TextLayer::destroy() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

void TextLayer::build(SDL_Renderer* renderer, const Font& font, const std::string& text,
                      int scale, SDL_Color color, int supersample) {
    destroy();
    if (text.empty() || scale <= 0) {
        return;
    }
    // The drawing resolution has nothing to do with the window size, so
    // rounding the supersample factor to a multiple of `scale` costs nothing on
    // screen.
    if (supersample < scale) {
        supersample = scale;
    }
    supersample = (supersample / scale) * scale;
    const int render_scale = scale + supersample;

    const int base_width = font.width(text, render_scale);
    const int base_height = font.height(render_scale);
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, base_width,
                                           base_height);
    if (target == nullptr) {
        return;
    }
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);

    SDL_Texture* previous = SDL_GetRenderTarget(renderer);
    if (SDL_SetRenderTarget(renderer, target) != 0) {
        SDL_DestroyTexture(target);
        SDL_SetRenderTarget(renderer, previous);
        return;
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    font.draw(renderer, text, 0, 0, render_scale, color);
    SDL_SetRenderTarget(renderer, previous);

    width_ = (base_width * scale) / render_scale;
    height_ = (base_height * scale) / render_scale;
    SDL_SetTextureScaleMode(target, SDL_ScaleModeLinear);
    texture_ = target;
}

void TextLayer::draw(SDL_Renderer* renderer, int center_x, int left, int top,
                     bool centered) const {
    if (texture_ == nullptr || width_ <= 0) {
        return;
    }
    const float x = centered ? static_cast<float>(center_x - width_ / 2)
                             : static_cast<float>(left);
    const SDL_FRect dst{x, static_cast<float>(top), static_cast<float>(width_),
                        static_cast<float>(height_)};
    SDL_RenderCopyF(renderer, texture_, nullptr, &dst);
}

// --- text helpers -------------------------------------------------------------

std::vector<std::string> wrap_text(const std::string& text, size_t max_chars) {
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ') {
            ++pos;
        }
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string word = text.substr(pos, end - pos);
        pos = end;
        while (word.size() > max_chars) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, max_chars));
            word.erase(0, max_chars);
        }
        if (word.empty()) {
            continue;
        }
        if (line.empty()) {
            line = word;
        }
        else if (line.size() + 1 + word.size() <= max_chars) {
            line += ' ';
            line += word;
        }
        else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

int chars_per_line(const Font& font, int scale, int max_width_px) {
    const int advance = font.width("M", scale);
    if (advance <= 0) {
        return 40;
    }
    return std::max(12, max_width_px / advance);
}

std::string truncate_to_width(const Font& font, const std::string& text, int scale,
                              int max_width) {
    if (text.empty() || max_width <= 0 || font.width(text, scale) <= max_width) {
        return text;
    }
    const int advance = font.width("M", scale);
    if (advance <= 0) {
        return text;
    }
    const size_t max_chars = static_cast<size_t>(max_width / advance);
    if (max_chars == 0) {
        return {};
    }
    if (max_chars <= 3) {
        return text.substr(0, max_chars);
    }
    return text.substr(0, max_chars - 3) + "...";
}

// --- Panel --------------------------------------------------------------------

Panel::Panel(Mode mode, std::string game_id, std::filesystem::path rom)
    : mode_(mode), game_id_(std::move(game_id)), rom_(std::move(rom)) {
    if (mode_ == Mode::Overlay) {
        // The overlay opens on CONTROLS; the other tabs are not selectable there.
        tab_ = Tab::Controls;
    }
    reload();
    selected_ = first_selectable();
    if (selected_ >= rows_.size()) {
        selected_ = 0;
    }
}

void Panel::set_rom(std::filesystem::path rom) {
    rom_ = std::move(rom);
}

bool Panel::tab_enabled(Tab tab) const {
    // The overlay shows the same tab bar. MODS is the only one that cannot be
    // used while the game is running: its rows toggle mods, and the runtime
    // loads mods before it boots. MAIN stays active because EXIT GAME lives
    // there, so closing the panel is not the only way out of the game.
    if (mode_ == Mode::Overlay) {
        return tab != Tab::Mods;
    }
    return true;
}

std::string Panel::tab_label(Tab tab) const {
    switch (tab) {
        case Tab::Main:
            return "MAIN";
        case Tab::Mods:
            return "MODS";
        case Tab::Controls:
            return "CONTROLS";
        case Tab::Settings:
            return "SETTINGS";
        case Tab::Debug:
            return "DEBUG";
        default:
            return {};
    }
}

void Panel::set_tab(Tab tab) {
    if (!tab_enabled(tab)) {
        return;
    }
    tab_ = tab;
    reload();
    selected_ = first_selectable();
    if (selected_ >= rows_.size()) {
        selected_ = 0;
    }
}

void Panel::switch_tab(int delta) {
    if (delta == 0) {
        return;
    }
    int index = static_cast<int>(tab_);
    for (int step = 0; step < kTabCount; step++) {
        index = ((index + delta) % kTabCount + kTabCount) % kTabCount;
        if (tab_enabled(static_cast<Tab>(index))) {
            set_tab(static_cast<Tab>(index));
            return;
        }
    }
}

void Panel::reload() {
    mods_ = recomp::mods::get_all_mod_details(game_id_);
    // A reload can change the row list under an armed capture; drop it rather
    // than leave it pointing at a row index that may no longer exist.
    capture_row_ = -1;
    rebuild_rows();
    if (selected_ >= rows_.size() || !selectable(selected_)) {
        selected_ = first_selectable();
        if (selected_ >= rows_.size()) {
            selected_ = 0;
        }
    }
    geometry_ = Geometry{};
}

void Panel::rebuild_rows() {
    rows_ = build_rows(tab_);
    // The layout reference tab's rows are built alongside the active tab's, so
    // the fixed top edge cannot drift from what that tab would draw.
    layout_rows_ = build_rows(kLayoutReferenceTab);
}

std::vector<Panel::Row> Panel::build_rows(Tab tab) const {
    std::vector<Row> rows;
    switch (tab) {
        case Tab::Main:
            // The ROM row lives here: with no ROM loaded it is the first
            // selectable row, so the start screen can still pick one. EXIT GAME
            // is last, so the row a stray SPACE or ENTER lands on is Start Game
            // in the launcher and Exit Game in the overlay, whose Start Game row
            // is not selectable.
            rows.push_back(Row{RowKind::StartGame, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::Rom, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::ExitGame, {}, 0, 0, -1});
            break;

        case Tab::Mods:
            if (mods_.empty()) {
                rows.push_back(Row{RowKind::Section, "(none installed)", 0, 0, -1});
            }
            for (size_t mod = 0; mod < mods_.size(); mod++) {
                rows.push_back(Row{RowKind::Mod, {}, mod, 0, -1});
                const recomp::config::ConfigSchema& schema =
                    recomp::mods::get_mod_config_schema(mods_[mod].mod_id);
                for (size_t option = 0; option < schema.options.size(); option++) {
                    if (schema.options[option].hidden) {
                        continue;
                    }
                    rows.push_back(Row{RowKind::Option, {}, mod, option, -1});
                }
            }
            break;

        case Tab::Controls:
            rows.push_back(Row{RowKind::Section, "KEYBOARD + GAMEPAD TO N64", 0, 0, -1});
            for (int i = 0; i < kN64ButtonCount; i++) {
                rows.push_back(Row{RowKind::Binding, {}, 0, 0, i});
            }
            rows.push_back(Row{RowKind::ResetBindings, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::Section, "L-STICK: N64 ANALOG STICK", 0, 0, -1});
            rows.push_back(Row{RowKind::Section, "R-STICK: ALSO PRESSES C", 0, 0, -1});
            // The panel's own pad controls. They are not bindings: they act on
            // the panel, and the game does not see the pad while the overlay is
            // open. The SELECT button opens and closes the overlay only while no
            // row above binds it (docs/guides/app-build.md -> "The CONTROLS
            // tab"). One section row: a section title wraps and may use the whole
            // panel, and a section is skipped by the selection.
            rows.push_back(Row{RowKind::Section,
                               "MENU PAD NAVIGATION: D-PAD MOVE, A SELECT, SELECT MENU, "
                               "LB/RB SWITCH TAB",
                               0, 0, -1});
            break;

        case Tab::Settings:
            rows.push_back(Row{RowKind::GameSpeed, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::Section, "DISPLAY", 0, 0, -1});
            rows.push_back(Row{RowKind::Widescreen, {}, 0, 0, -1});
            // RESOLUTION and MSAA are experimental and off unless the run asks
            // for them, so their rows are absent by default rather than present
            // and inert (settings.hpp -> display_experiments_enabled).
            if (display_experiments_enabled()) {
                rows.push_back(Row{RowKind::Resolution, {}, 0, 0, -1});
                rows.push_back(Row{RowKind::Antialias, {}, 0, 0, -1});
            }
            rows.push_back(Row{RowKind::Display, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::Section, "AUDIO", 0, 0, -1});
            rows.push_back(Row{RowKind::Sound, {}, 0, 0, -1});
            rows.push_back(Row{RowKind::Volume, {}, 0, 0, -1});
            break;

        case Tab::Debug:
            rows.push_back(Row{RowKind::Section, "GAME STATE", 0, 0, -1});
            rows.push_back(Row{RowKind::ChaosFrame, {}, 0, 0, -1});
            break;

        default:
            break;
    }
    return rows;
}

size_t Panel::first_selectable() const {
    for (size_t index = 0; index < rows_.size(); index++) {
        if (selectable(index)) {
            return index;
        }
    }
    return rows_.size();
}

bool Panel::selectable(size_t index) const {
    if (index >= rows_.size()) {
        return false;
    }
    const Row& row = rows_[index];
    if (row.kind == RowKind::Section) {
        return false;
    }
    if (row.kind == RowKind::StartGame) {
        // The overlay's MAIN tab cannot start a game that is already running, so
        // its Start Game row is inert there and the selection skips it.
        return mode_ != Mode::Overlay && !rom_.empty();
    }
    return true;
}

bool Panel::selected_is_rom() const {
    return selected_ < rows_.size() && rows_[selected_].kind == RowKind::Rom;
}

bool Panel::selected_is_exit() const {
    return selected_ < rows_.size() && rows_[selected_].kind == RowKind::ExitGame;
}

Panel::RowKind Panel::row_kind(size_t index) const {
    return index < rows_.size() ? rows_[index].kind : RowKind::Section;
}

void Panel::set_selected(size_t index) {
    if (index < rows_.size()) {
        selected_ = index;
    }
}

void Panel::select_first() {
    selected_ = first_selectable();
    if (selected_ >= rows_.size()) {
        selected_ = 0;
    }
}

bool Panel::move(int delta) {
    const int count = static_cast<int>(rows_.size());
    if (count == 0) {
        return false;
    }
    int index = static_cast<int>(selected_);
    for (int step = 0; step < count; step++) {
        index = ((index + delta) % count + count) % count;
        if (selectable(static_cast<size_t>(index))) {
            selected_ = static_cast<size_t>(index);
            return true;
        }
    }
    return false;
}

bool Panel::row_is_steppable(size_t index) const {
    if (index >= rows_.size()) {
        return false;
    }
    const RowKind kind = rows_[index].kind;
    return is_radio_row(kind) || kind == RowKind::Volume || kind == RowKind::Option;
}

Panel::PadCommand Panel::pad_command_for_button(int button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return PadCommand::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return PadCommand::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return PadCommand::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return PadCommand::Right;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_START:
            return PadCommand::Accept;
        case SDL_CONTROLLER_BUTTON_B:
            return PadCommand::Back;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return PadCommand::TabPrev;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return PadCommand::TabNext;
        default:
            return PadCommand::None;
    }
}

RowAction Panel::pad_command(PadCommand command) {
    const size_t index = selected_;
    switch (command) {
        case PadCommand::Up:
            move(-1);
            return RowAction::None;
        case PadCommand::Down:
            move(1);
            return RowAction::None;
        case PadCommand::Left:
        case PadCommand::Right:
            if (!row_is_steppable(index)) {
                return RowAction::None;
            }
            return activate(index, command == PadCommand::Right ? 1 : -1);
        case PadCommand::Accept:
            if (index >= rows_.size() || !selectable(index)) {
                return RowAction::None;
            }
            return activate(index, 1);
        case PadCommand::Back:
            // The overlay closes on Back and the launcher ignores it; neither is
            // the panel's decision.
            return RowAction::None;
        case PadCommand::TabPrev:
            switch_tab(-1);
            return RowAction::None;
        case PadCommand::TabNext:
            switch_tab(1);
            return RowAction::None;
        case PadCommand::None:
        default:
            return RowAction::None;
    }
}

std::string Panel::left_text(size_t index) const {
    if (index >= rows_.size()) {
        return {};
    }
    const Row& row = rows_[index];
    switch (row.kind) {
        case RowKind::Section:
            return row.title;
        case RowKind::StartGame:
            return rom_.empty() ? "[ ] Start Game" : "[x] Start Game";
        case RowKind::ExitGame:
            return "[ ] Exit Game";
        case RowKind::Rom:
            return rom_.empty() ? "[ ] No ROM" : "[x] ROM loaded";
        case RowKind::Mod: {
            const recomp::mods::ModDetails& mod = mods_[row.mod];
            const bool enabled = recomp::mods::is_mod_enabled(mod.mod_id);
            return std::string(enabled ? "[x] " : "[ ] ") + mod.display_name;
        }
        case RowKind::Option: {
            const recomp::config::ConfigSchema& schema =
                recomp::mods::get_mod_config_schema(mods_[row.mod].mod_id);
            const recomp::config::ConfigOption& option = schema.options[row.option];
            return "      " + option.name + ": " +
                   config_value_text(option, recomp::mods::get_mod_config_value(
                                                 mods_[row.mod].mod_id, option.id));
        }
        case RowKind::Binding: {
            const InputMap map = read_input_map();
            const N64ButtonInfo& info = n64_button_info(row.binding);
            if (capture_row_ == index) {
                return pad_to(info.name, 10) + "PRESS A KEY OR PAD BUTTON...";
            }
            return pad_to(info.name, 10) + "KEY " + map.key_text(row.binding);
        }
        case RowKind::ResetBindings:
            return "Reset bindings to defaults";
        case RowKind::GameSpeed:
            return "GAME SPEED";
        case RowKind::Widescreen:
            return "WIDESCREEN";
        case RowKind::Resolution:
            return "RESOLUTION";
        case RowKind::Antialias:
            return "MSAA";
        case RowKind::Display:
            return "WINDOW";
        case RowKind::Sound:
            return "SOUNDS";
        case RowKind::Volume:
            return "VOLUME";
        case RowKind::ChaosFrame:
            return "Chaos Frame";
    }
    return {};
}

std::string Panel::right_text(size_t index) const {
    if (index >= rows_.size()) {
        return {};
    }
    return right_text_for(rows_[index]);
}

std::string Panel::right_text_for(const Row& row) const {
    switch (row.kind) {
        case RowKind::StartGame:
            return mode_ == Mode::Overlay
                       ? std::string("ALREADY RUNNING")
                       : (rom_.empty() ? std::string("CHOOSE A ROM FIRST")
                                       : std::string("Press ENTER or SPACE to start"));
        case RowKind::ExitGame:
            return mode_ == Mode::Overlay
                       ? std::string("Quit the game and close the window")
                       : std::string("Quit without starting a game");
        case RowKind::Rom:
            return rom_.empty() ? std::string("PRESS SPACE TO CHOOSE A ROM, OR DROP IT IN A WINDOW")
                                : rom_.filename().string();
        case RowKind::Mod:
            return mods_[row.mod].short_description;
        case RowKind::Binding: {
            const InputMap map = read_input_map();
            const N64ButtonInfo& info = n64_button_info(row.binding);
            return "PAD " + pad_to(map.pad_text(row.binding), 14) + "- " + info.field;
        }
        case RowKind::ResetBindings:
            return "Restore the default keyboard and gamepad map.";
        case RowKind::GameSpeed:
            return game_speed_text(game_speed());
        case RowKind::Widescreen:
            return widescreen_mode_text();
        case RowKind::Resolution:
            return resolution_mode_text();
        case RowKind::Antialias:
            return antialias_mode_text();
        case RowKind::Display:
            return display_mode_text();
        case RowKind::Sound:
            return sound_mode_text();
        case RowKind::Volume:
            return volume_text(volume_percent());
        case RowKind::ChaosFrame: {
            if (!chaos_frame_known_) {
                return "GAME NOT RUNNING";
            }
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "%d  %s", chaos_frame_,
                          chaos_frame_band(chaos_frame_));
            return buffer;
        }
        default:
            return {};
    }
}

RowAction Panel::activate(size_t index, int direction) {
    if (index >= rows_.size()) {
        return RowAction::None;
    }
    const Row& row = rows_[index];
    switch (row.kind) {
        case RowKind::Section:
            return RowAction::None;

        case RowKind::StartGame:
            return mode_ == Mode::Overlay ? RowAction::None : RowAction::Play;

        case RowKind::ExitGame:
            return RowAction::QuitGame;

        case RowKind::Rom:
            return mode_ == Mode::Overlay ? RowAction::None : RowAction::BrowseRom;

        case RowKind::Mod: {
            const recomp::mods::ModDetails& mod = mods_[row.mod];
            const bool enabled = recomp::mods::is_mod_enabled(mod.mod_id);
            recomp::mods::enable_mod(mod.mod_id, !enabled);
            // Enabling a mod can enable a required dependency, so rebuild the
            // list instead of assuming only one row changed.
            reload();
            return RowAction::ModToggled;
        }

        case RowKind::Option: {
            const recomp::mods::ModDetails& mod = mods_[row.mod];
            const recomp::config::ConfigSchema& schema =
                recomp::mods::get_mod_config_schema(mod.mod_id);
            const recomp::config::ConfigOption& option = schema.options[row.option];
            const recomp::config::ConfigValueVariant value =
                recomp::mods::get_mod_config_value(mod.mod_id, option.id);
            using recomp::config::ConfigOptionType;
            switch (option.type) {
                case ConfigOptionType::Enum: {
                    const auto& enumeration =
                        std::get<recomp::config::ConfigOptionEnum>(option.variant);
                    if (enumeration.options.empty()) {
                        return RowAction::None;
                    }
                    const uint32_t current = std::holds_alternative<uint32_t>(value)
                                                 ? std::get<uint32_t>(value)
                                                 : enumeration.default_value;
                    const auto found = enumeration.find_option_from_value(current);
                    size_t position = found != enumeration.options.end()
                                          ? static_cast<size_t>(found - enumeration.options.begin())
                                          : 0;
                    position = direction >= 0
                                   ? (position + 1) % enumeration.options.size()
                                   : (position + enumeration.options.size() - 1) % enumeration.options.size();
                    recomp::mods::set_mod_config_value(mod.mod_id, option.id,
                                                       enumeration.options[position].value);
                    return RowAction::OptionChanged;
                }
                case ConfigOptionType::Bool: {
                    const bool current = std::holds_alternative<bool>(value)
                                             ? std::get<bool>(value)
                                             : std::get<recomp::config::ConfigOptionBool>(option.variant).default_value;
                    recomp::mods::set_mod_config_value(mod.mod_id, option.id, !current);
                    return RowAction::OptionChanged;
                }
                case ConfigOptionType::Number: {
                    const auto& number =
                        std::get<recomp::config::ConfigOptionNumber>(option.variant);
                    double current = std::holds_alternative<double>(value)
                                         ? std::get<double>(value)
                                         : number.default_value;
                    const double step = number.step != 0.0 ? number.step : 1.0;
                    current += direction >= 0 ? step : -step;
                    if (number.max > number.min) {
                        current = std::clamp(current, number.min, number.max);
                    }
                    recomp::mods::set_mod_config_value(mod.mod_id, option.id, current);
                    return RowAction::OptionChanged;
                }
                default:
                    return RowAction::None;
            }
        }

        case RowKind::Binding:
            begin_capture(index);
            return RowAction::None;

        case RowKind::ResetBindings: {
            InputMap map;
            map.reset_defaults();
            update_input_map(map);
            return RowAction::BindingChanged;
        }

        case RowKind::GameSpeed:
        case RowKind::Widescreen:
        case RowKind::Resolution:
        case RowKind::Antialias:
        case RowKind::Display:
        case RowKind::Sound:
            // Left/Right and Space step the radio group, wrapping at the ends
            // like a mod option row. Each setting applies itself (the runtime for
            // GAME SPEED, `widescreen.cpp` or the renderer for the DISPLAY rows),
            // so a change under the overlay lands on the next frame.
            return step_radio_row(row.kind, direction);

        case RowKind::Volume:
            // Left/Right and Space move the handle one step; the value is
            // clamped at the ends rather than wrapped.
            set_volume_percent(volume_percent() + direction * kVolumeStepPercent);
            return RowAction::SettingChanged;

        case RowKind::ChaosFrame:
            // A read-only readout: selectable so the value is highlighted, but
            // activating it does nothing.
            return RowAction::None;
    }
    return RowAction::None;
}

RowAction Panel::choose_option(size_t index, size_t option) {
    if (index >= rows_.size()) {
        return RowAction::None;
    }
    const RowKind kind = rows_[index].kind;
    if (is_radio_row(kind)) {
        // A click on one of the group's markers sets that option directly, which
        // is not the same as stepping to it.
        if (option >= static_cast<size_t>(radio_row_count(kind))) {
            return RowAction::None;
        }
        return store_radio_row(kind, static_cast<int>(option));
    }
    switch (kind) {
        case RowKind::Volume: {
            // A click lands on one bar cell; the cell sets the percent, at the
            // 10% the 11 cells divide the range into.
            if (option >= static_cast<size_t>(kVolumeCells)) {
                return RowAction::None;
            }
            const int percent = volume_percent_for_cell(static_cast<int>(option));
            if (percent == volume_percent()) {
                return RowAction::None;
            }
            set_volume_percent(percent);
            return RowAction::SettingChanged;
        }
        default:
            return RowAction::None;
    }
}

void Panel::begin_capture(size_t index) {
    if (index < rows_.size() && rows_[index].kind == RowKind::Binding) {
        capture_row_ = static_cast<int>(index);
    }
}

void Panel::cancel_capture() {
    capture_row_ = -1;
}

bool Panel::capture_key(SDL_Scancode code) {
    if (!capturing() || is_reserved_key(code)) {
        return false;
    }
    InputMap map = read_input_map();
    map.set_key(rows_[capture_row_].binding, code);
    update_input_map(map);
    cancel_capture();
    return true;
}

bool Panel::capture_pad_button(int code) {
    if (!capturing()) {
        return false;
    }
    InputMap map = read_input_map();
    map.set_pad_button(rows_[capture_row_].binding, code);
    update_input_map(map);
    cancel_capture();
    return true;
}

bool Panel::capture_clear(bool keyboard) {
    if (!capturing()) {
        return false;
    }
    InputMap map = read_input_map();
    const int binding = rows_[capture_row_].binding;
    if (keyboard) {
        map.clear_key(binding);
    }
    else {
        map.clear_pad(binding);
    }
    update_input_map(map);
    return true;
}

Panel::Hit Panel::hit_test(int x, int y) const {
    auto inside = [&](const SDL_Rect& rect) {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    };
    for (size_t i = 0; i < geometry_.tabs.size(); i++) {
        if (inside(geometry_.tabs[i])) {
            return Hit{Hit::Kind::Tab, static_cast<int>(i)};
        }
    }
    // A radio marker sits inside its row, so it is tested first.
    for (const Geometry::Option& option : geometry_.options) {
        if (inside(option.rect)) {
            return Hit{Hit::Kind::RowOption, option.row, option.option};
        }
    }
    for (size_t i = 0; i < geometry_.rows.size(); i++) {
        if (inside(geometry_.rows[i])) {
            return Hit{Hit::Kind::Row, static_cast<int>(i)};
        }
    }
    return Hit{};
}

// --- drawing ------------------------------------------------------------------

bool write_ppm(SDL_Renderer* renderer, const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0 || width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGB24, pixels.data(),
                             width * 3) != 0) {
        return false;
    }
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
    return true;
}

PanelMetrics measure_panel(const Font& font, Panel& panel, int output_width,
                           int top_y, float ui_scale) {
    auto px = [ui_scale](int value) {
        return static_cast<int>(static_cast<float>(value) * ui_scale);
    };

    PanelMetrics metrics;
    metrics.scale = px(kTextScale);
    metrics.line_height = metrics.scale * Font::kCellHeight;
    metrics.row_height = metrics.line_height + px(5);
    const int section_height = metrics.line_height + px(6);
    metrics.panel_width = std::min(output_width - px(64), px(900));
    metrics.panel_left = output_width / 2 - metrics.panel_width / 2;
    // The second column starts two fifths across and wraps inside what is left;
    // the first column is truncated before it.
    const int description_x = metrics.panel_left + (metrics.panel_width * 2) / 5;
    const int description_width = metrics.panel_left + metrics.panel_width - description_x;

    // A section row's title may use the whole panel, because a section has no
    // description to its right.
    const int full_width = metrics.panel_width - px(16);
    RowsLayout rows = layout_rows(font, panel, panel.rows(), metrics.scale, metrics.row_height,
                                  section_height, description_width, full_width, px(4), true);
    metrics.row_heights = std::move(rows.heights);
    metrics.left_lines = std::move(rows.left_lines);
    metrics.right_lines = std::move(rows.right_lines);

    const int tab_bar_height = metrics.line_height + px(10);
    const int chrome = tab_bar_height + px(10) + px(12) + metrics.line_height + px(12);
    metrics.rows_top = top_y + tab_bar_height + px(10);
    metrics.hint_y = metrics.rows_top + rows.total + px(12);
    metrics.height = chrome + rows.total;

    // The same layout for the tab that fixes the panel's position. Only its
    // height is read, so the wrapped right-column lines are not kept.
    const RowsLayout reference =
        layout_rows(font, panel, panel.layout_rows(), metrics.scale, metrics.row_height,
                    section_height, description_width, full_width, px(4), false);
    metrics.layout_height = std::max(metrics.height, chrome + reference.total);
    return metrics;
}

PanelDraw draw_panel(SDL_Renderer* renderer, const Font& font, Panel& panel,
                     int output_width, int output_height, int top_y, float ui_scale) {
    (void)output_height;
    auto px = [ui_scale](int value) {
        return static_cast<int>(static_cast<float>(value) * ui_scale);
    };

    const PanelMetrics metrics = measure_panel(font, panel, output_width, top_y, ui_scale);
    const int scale = metrics.scale;
    const int line_height = metrics.line_height;
    const int panel_width = metrics.panel_width;
    const int panel_left = metrics.panel_left;
    const int description_x = panel_left + (panel_width * 2) / 5;
    const int description_width = panel_left + panel_width - description_x;
    const int left_width = description_x - panel_left - px(16);

    TextLayer layer;
    Panel::Geometry geometry;

    // The panel background is drawn first: the tab bar and the rows both sit on
    // top of it. It reserves the layout height, so the frame does not change
    // size when the active tab changes.
    SDL_SetRenderDrawColor(renderer, kPanelColor.r, kPanelColor.g, kPanelColor.b, kPanelColor.a);
    const SDL_Rect background{panel_left - px(18), top_y - px(12),
                              panel_width + px(36), metrics.layout_height};
    SDL_RenderFillRect(renderer, &background);

    // --- tab bar ---
    int tab_x = panel_left;
    const int tab_text_y = top_y + px(3);
    for (int i = 0; i < kTabCount; i++) {
        const Tab tab = static_cast<Tab>(i);
        const bool enabled = panel.tab_enabled(tab);
        const bool active = tab == panel.tab();
        const std::string label = "[ " + panel.tab_label(tab) + " ]";
        const int text_width = font.width(label, scale);
        const SDL_Rect rect{tab_x, top_y, text_width + px(10), line_height + px(6)};
        geometry.tabs.push_back(rect);

        SDL_Color color = kHintColor;
        if (active) {
            color = kWarmColor;
        }
        else if (enabled) {
            color = kSubtitleColor;
        }
        layer.build(renderer, font, label, scale, color);
        layer.draw(renderer, 0, tab_x + px(5), tab_text_y, false);
        tab_x += rect.w + px(6);
    }

    int cursor_y = metrics.rows_top;
    for (size_t i = 0; i < panel.row_count(); i++) {
        const Panel::RowKind kind = panel.row_kind(i);
        const int height = metrics.row_heights[i];
        const bool is_selected = i == panel.selected() && panel.selectable(i);
        const SDL_Rect row_rect{panel_left - px(12), cursor_y - px(4),
                                panel_width + px(24), height};
        geometry.rows.push_back(row_rect);
        if (is_selected) {
            SDL_SetRenderDrawColor(renderer, kSelectColor.r, kSelectColor.g, kSelectColor.b,
                                   kSelectColor.a);
            SDL_RenderFillRect(renderer, &row_rect);
        }

        SDL_Color color = kTitleColor;
        if (kind == Panel::RowKind::Section || !panel.selectable(i)) {
            color = kHintColor;
        }
        else if (is_selected) {
            color = kWarmColor;
        }
        else if (kind == Panel::RowKind::Option || kind == Panel::RowKind::Binding ||
                 kind == Panel::RowKind::ResetBindings) {
            color = kSubtitleColor;
        }

        const int text_y =
            cursor_y + (kind == Panel::RowKind::Section ? px(6) : 0);
        if (kind == Panel::RowKind::Section) {
            // A section title is laid out wrapped by layout_rows; the left
            // column of a section spans the panel.
            int line_offset = 0;
            for (const std::string& line : metrics.left_lines[i]) {
                layer.build(renderer, font, line, scale, color);
                layer.draw(renderer, 0, panel_left, text_y + line_offset, false);
                line_offset += line_height;
            }
        }
        else {
            const std::string left =
                truncate_to_width(font, panel.left_text(i), scale, left_width);
            layer.build(renderer, font, left, scale, color);
            layer.draw(renderer, 0, panel_left, text_y, false);
        }

        int right_y = text_y;
        if (is_radio_row(kind)) {
            // The marker of the live option stays warm so the current value is
            // readable even when the row is not selected.
            const int current = radio_row_current(kind);
            const RadioLayout layout =
                radio_row_layout(kind, font, scale, description_width, current);
            for (const RadioToken& token : layout.tokens) {
                const bool live = token.option == current;
                layer.build(renderer, font, token.text, scale,
                            (is_selected || live) ? kWarmColor : kSubtitleColor);
                layer.draw(renderer, 0, description_x + token.x, text_y + token.y, false);
            }
            for (size_t option = 0; option < layout.option_rects.size(); option++) {
                SDL_Rect rect = layout.option_rects[option];
                rect.x += description_x;
                rect.y += text_y;
                geometry.options.push_back(Panel::Geometry::Option{
                    static_cast<int>(i), static_cast<int>(option), rect});
            }
        }
        else if (kind == Panel::RowKind::Volume) {
            // The bar and its handle are one text run, so the handle is always
            // visible; the cells under it are click targets.
            const SliderLayout slider = layout_volume(font, scale, volume_percent());
            layer.build(renderer, font, slider.text, scale,
                        is_selected ? kWarmColor : kSubtitleColor);
            layer.draw(renderer, 0, description_x, text_y, false);
            for (size_t cell = 0; cell < slider.cell_rects.size(); cell++) {
                SDL_Rect rect = slider.cell_rects[cell];
                rect.x += description_x;
                rect.y += text_y;
                geometry.options.push_back(Panel::Geometry::Option{
                    static_cast<int>(i), static_cast<int>(cell), rect});
            }
        }
        else {
            for (const std::string& line : metrics.right_lines[i]) {
                layer.build(renderer, font, line, scale,
                            is_selected ? kWarmColor : kHintColor);
                layer.draw(renderer, 0, description_x, right_y, false);
                right_y += line_height;
            }
        }

        cursor_y += height;
    }

    const char* hint = kLauncherHint;
    if (panel.capturing()) {
        hint = kCaptureHint;
    }
    else if (panel.mode() == Panel::Mode::Overlay) {
        hint = kOverlayHint;
    }
    layer.build(renderer, font, hint, scale, kHintColor);
    layer.draw(renderer, 0, panel_left, metrics.hint_y, false);

    panel.set_geometry(std::move(geometry));

    PanelDraw result;
    result.panel_left = panel_left;
    result.panel_width = panel_width;
    result.height = metrics.layout_height;
    result.text_scale = scale;
    return result;
}

}  // namespace ogre::ui
