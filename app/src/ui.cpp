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

// --- the GAME SPEED radio group -----------------------------------------------
// A GameSpeed row draws its options as radio markers in the second column
// instead of one text run, so measure_panel (the row height) and draw_panel (the
// markers and their click rects) lay the same group out with this helper. The
// result is a pure function of its arguments, so the measured and the drawn row
// agree.

struct GameSpeedToken {
    std::string text;
    int x = 0;
    int y = 0;
    int option = -1;
};

struct GameSpeedLayout {
    std::vector<GameSpeedToken> tokens;
    std::vector<SDL_Rect> option_rects;  // relative to the row's top-left
    int height = 0;
};

std::string game_speed_marker(int option_value, int current) {
    return option_value == current ? "[x]" : "[ ]";
}

GameSpeedLayout layout_game_speed(const Font& font, int scale, int max_width, int current) {
    GameSpeedLayout layout;
    const int line_height = scale * Font::kCellHeight;
    const int space = font.width(" ", scale);
    int x = 0;
    int y = 0;
    for (int i = 0; i < kGameSpeedCount; i++) {
        const int value = game_speed_value(i);
        const std::string marker = game_speed_marker(value, current);
        const std::string number = std::to_string(value);
        const int marker_width = font.width(marker, scale);
        const int option_width = marker_width + space + font.width(number, scale);
        // A narrow window wraps the group onto another line; the option itself
        // never splits.
        if (x > 0 && x + option_width > max_width) {
            x = 0;
            y += line_height;
        }
        layout.tokens.push_back(GameSpeedToken{marker, x, y, i});
        layout.tokens.push_back(GameSpeedToken{number, x + marker_width + space, y, i});
        layout.option_rects.push_back(SDL_Rect{x, y, option_width, line_height});
        x += option_width + space;
    }
    layout.height = y + line_height;
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
        text += game_speed_marker(game_speed_value(i), current) + " " +
                std::to_string(game_speed_value(i));
    }
    return text;
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
    // The overlay shows the same tab bar, but only CONTROLS and SETTINGS can be
    // used while the game is running.
    if (mode_ == Mode::Overlay) {
        return tab == Tab::Controls || tab == Tab::Settings;
    }
    return true;
}

std::string Panel::tab_label(Tab tab) const {
    switch (tab) {
        case Tab::Start:
            return "START GAME";
        case Tab::Rom:
            return "ROM";
        case Tab::Mods:
            return "MODS";
        case Tab::Controls:
            return "CONTROLS";
        case Tab::Settings:
            return "SETTINGS";
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
    rows_.clear();
    switch (tab_) {
        case Tab::Start:
            rows_.push_back(Row{RowKind::Start, {}, 0, 0, -1});
            break;

        case Tab::Rom:
            rows_.push_back(Row{RowKind::Rom, {}, 0, 0, -1});
            break;

        case Tab::Mods:
            if (mods_.empty()) {
                rows_.push_back(Row{RowKind::Section, "(none installed)", 0, 0, -1});
            }
            for (size_t mod = 0; mod < mods_.size(); mod++) {
                rows_.push_back(Row{RowKind::Mod, {}, mod, 0, -1});
                const recomp::config::ConfigSchema& schema =
                    recomp::mods::get_mod_config_schema(mods_[mod].mod_id);
                for (size_t option = 0; option < schema.options.size(); option++) {
                    if (schema.options[option].hidden) {
                        continue;
                    }
                    rows_.push_back(Row{RowKind::Option, {}, mod, option, -1});
                }
            }
            break;

        case Tab::Controls:
            rows_.push_back(Row{RowKind::Section, "KEYBOARD + GAMEPAD TO N64", 0, 0, -1});
            for (int i = 0; i < kN64ButtonCount; i++) {
                rows_.push_back(Row{RowKind::Binding, {}, 0, 0, i});
            }
            rows_.push_back(Row{RowKind::ResetBindings, {}, 0, 0, -1});
            rows_.push_back(Row{RowKind::Section, "L-STICK: N64 ANALOG STICK", 0, 0, -1});
            rows_.push_back(Row{RowKind::Section, "R-STICK: ALSO PRESSES C", 0, 0, -1});
            break;

        case Tab::Settings:
            rows_.push_back(Row{RowKind::GameSpeed, {}, 0, 0, -1});
            break;

        default:
            break;
    }
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
    if (row.kind == RowKind::Start) {
        return !rom_.empty();
    }
    return true;
}

bool Panel::selected_is_rom() const {
    return selected_ < rows_.size() && rows_[selected_].kind == RowKind::Rom;
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

std::string Panel::left_text(size_t index) const {
    if (index >= rows_.size()) {
        return {};
    }
    const Row& row = rows_[index];
    switch (row.kind) {
        case RowKind::Section:
            return row.title;
        case RowKind::Start:
            return rom_.empty() ? "[ ] Start Game" : "[x] Start Game";
        case RowKind::Rom:
            return rom_.empty() ? "[ ] No ROM" : "[x] Loaded!";
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
    }
    return {};
}

std::string Panel::right_text(size_t index) const {
    if (index >= rows_.size()) {
        return {};
    }
    const Row& row = rows_[index];
    switch (row.kind) {
        case RowKind::Start:
            return rom_.empty() ? std::string("CHOOSE A ROM FIRST")
                                : std::string("Press ENTER or SPACE to start");
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

        case RowKind::Start:
            return mode_ == Mode::Overlay ? RowAction::None : RowAction::Play;

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

        case RowKind::GameSpeed: {
            // Left/Right and Space step the radio group, wrapping at the ends
            // like a mod option row. A value that came from `OGRE_SPEED` and is
            // not one of the offered steps starts the walk at the first step.
            int current = game_speed_index(game_speed());
            if (current < 0) {
                current = 0;
            }
            current = ((current + direction) % kGameSpeedCount + kGameSpeedCount) % kGameSpeedCount;
            set_game_speed(game_speed_value(current));
            return RowAction::SettingChanged;
        }
    }
    return RowAction::None;
}

RowAction Panel::choose_option(size_t index, size_t option) {
    if (index >= rows_.size() || rows_[index].kind != RowKind::GameSpeed ||
        option >= static_cast<size_t>(kGameSpeedCount)) {
        return RowAction::None;
    }
    const int value = game_speed_value(static_cast<int>(option));
    if (value == game_speed()) {
        return RowAction::None;
    }
    set_game_speed(value);
    return RowAction::SettingChanged;
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

    metrics.row_heights.assign(panel.row_count(), metrics.row_height);
    metrics.right_lines.assign(panel.row_count(), {});
    int rows_height = 0;
    for (size_t i = 0; i < panel.row_count(); i++) {
        if (panel.row_kind(i) == Panel::RowKind::Section) {
            metrics.row_heights[i] = section_height;
        }
        else if (panel.row_kind(i) == Panel::RowKind::GameSpeed) {
            // The radio group is laid out by option, not wrapped as one string;
            // its height is what the row must reserve.
            const GameSpeedLayout layout =
                layout_game_speed(font, metrics.scale, description_width, game_speed());
            metrics.row_heights[i] = std::max(metrics.row_height, layout.height);
        }
        else {
            const std::string right = panel.right_text(i);
            if (!right.empty()) {
                metrics.right_lines[i] = wrap_text(
                    right, chars_per_line(font, metrics.scale, description_width));
                const int lines = static_cast<int>(metrics.right_lines[i].size());
                metrics.row_heights[i] =
                    std::max(metrics.row_height, lines * metrics.line_height + px(4));
            }
        }
        rows_height += metrics.row_heights[i];
    }

    const int tab_bar_height = metrics.line_height + px(10);
    metrics.rows_top = top_y + tab_bar_height + px(10);
    metrics.hint_y = metrics.rows_top + rows_height + px(12);
    metrics.height = tab_bar_height + px(10) + rows_height + px(12) + metrics.line_height + px(12);
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
    // top of it.
    SDL_SetRenderDrawColor(renderer, kPanelColor.r, kPanelColor.g, kPanelColor.b, kPanelColor.a);
    const SDL_Rect background{panel_left - px(18), top_y - px(12),
                              panel_width + px(36), metrics.height};
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
        const std::string left =
            truncate_to_width(font, panel.left_text(i), scale, left_width);
        layer.build(renderer, font, left, scale, color);
        layer.draw(renderer, 0, panel_left, text_y, false);

        int right_y = text_y;
        if (kind == Panel::RowKind::GameSpeed) {
            // The marker of the live speed stays warm so the current value is
            // readable even when the row is not selected.
            const int current_speed = game_speed();
            const GameSpeedLayout layout =
                layout_game_speed(font, scale, description_width, current_speed);
            for (const GameSpeedToken& token : layout.tokens) {
                const bool live = game_speed_value(token.option) == current_speed;
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
    result.height = metrics.height;
    result.text_scale = scale;
    return result;
}

}  // namespace ogre::ui
