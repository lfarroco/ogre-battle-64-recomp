// The app's start screen (see launcher.hpp).

#include "launcher.hpp"
#include "font.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>

#include <SDL.h>

#include <librecomp/mods.hpp>

#if defined(OGRE_HAVE_NFD)
#include <nfd.h>
#endif

namespace ogre {
namespace {

// The branding and the error lines. The old prompt line ("click to load your
// ROM") is gone: the ROM row states it, and the row's second column says how.
constexpr const char* kTitle = "OGRE BATTLE 64: RECOMP";
constexpr const char* kErrorTitle = "THAT IS NOT A USABLE ROM";
constexpr const char* kErrorHint = "PRESS SPACE ON THE ROM ROW TO TRY AGAIN";
constexpr const char* kModsHint = "UP/DOWN SELECT   SPACE ACTIVATE   LEFT/RIGHT CHANGE   ENTER PLAY";
constexpr const char* kRomHint = "PRESS SPACE TO CHOOSE A ROM, OR DROP IT IN THIS WINDOW";
constexpr const char* kChooseRomFirst = "CHOOSE A ROM FIRST";

// A black window; the content sits slightly above centre.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kTitleScale = 5;
constexpr int kErrorScale = 2;
constexpr int kFooterScale = 2;
constexpr int kModScale = 2;

const SDL_Color kTitleColor{238, 238, 232, 255};
const SDL_Color kSubtitleColor{196, 200, 208, 255};
const SDL_Color kHintColor{122, 128, 138, 255};
const SDL_Color kErrorColor{232, 116, 106, 255};
const SDL_Color kWarmColor{198, 160, 92, 220};
const SDL_Color kFooterColor{110, 116, 126, 255};

// Splits `text` so no line is wider than `max_chars` characters. Words are kept
// whole when they fit; a single over-long word (a file name) is cut.
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

// A text block rendered once into a supersampled texture and scaled down by the
// GPU, so the bitmap glyphs come out anti-aliased at any size on screen.
class TextLayer {
public:
    TextLayer() = default;
    TextLayer(const TextLayer&) = delete;
    TextLayer& operator=(const TextLayer&) = delete;
    TextLayer(TextLayer&& other) noexcept
        : texture_(other.texture_), width_(other.width_), height_(other.height_) {
        other.texture_ = nullptr;
    }
    TextLayer& operator=(TextLayer&& other) noexcept {
        if (this != &other) {
            destroy();
            texture_ = other.texture_;
            width_ = other.width_;
            height_ = other.height_;
            other.texture_ = nullptr;
        }
        return *this;
    }
    ~TextLayer() { destroy(); }

    void destroy() {
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
    }

    // `build` is safe to call repeatedly (every frame): each call replaces the
    // previous texture.
    void build(SDL_Renderer* renderer, const Font& font, const std::string& text,
               int scale, SDL_Color color, int supersample = 4) {
        destroy();
        if (text.empty() || scale <= 0) {
            return;
        }
        // The drawing resolution has nothing to do with the window size, so
        // rounding the supersample factor to a multiple of `scale` costs
        // nothing on screen.
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

    void draw(SDL_Renderer* renderer, int center_x, int left, int top,
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

    bool empty() const { return texture_ == nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

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

// Builds the layers for one frame. Kept separate so the layer lifetimes (and
// therefore the GPU textures) end at the bottom of each frame.
struct FrameText {
    TextLayer title;
    TextLayer body;
    TextLayer footer;
};

int chars_per_line(const Font& font, int scale, int max_width_px) {
    const int advance = font.width("M", scale);
    if (advance <= 0) {
        return 40;
    }
    return std::max(12, max_width_px / advance);
}

// `text` shortened with a trailing "..." so it fits `max_width` pixels at
// `scale`. Used for the second column, which holds file paths and descriptions.
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

// --- start panel -------------------------------------------------------------
//
// The start screen's list. It has two sections:
//
//   == ROM ==    one row: whether a usable ROM is loaded, and which file. It is
//                selectable so the whole screen can be driven from the keyboard;
//                activating it opens the file picker.
//   == MODS ==   one row per installed mod, then one row per visible config
//                option of that mod. A mod's short description sits in a second
//                column on its row.
//
// Up/Down move the selection (section headings are skipped), Space activates the
// selected row, Left/Right step an option, Enter plays. Mouse clicks activate
// the row under the pointer.

// The displayed value of one config option. Enum values show the option name,
// not the number, so the panel reads like the mod's own description.
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

// What activating a row asks the caller to do. `BrowseRom` and the two mod
// actions are handled outside the panel (the first opens the platform file
// picker, the others write through the runtime's mod system).
enum class PanelAction {
    None,
    BrowseRom,
    Play,
    ModToggled,
    OptionChanged,
};

class StartPanel {
public:
    enum class RowKind { Section, Start, Rom, Mod, Option };

    struct Row {
        RowKind kind = RowKind::Section;
        std::string title;  // Section rows only
        size_t mod = 0;     // index into `mods_`
        size_t option = 0;  // index into that mod's schema options
    };

    StartPanel(std::string game_id, std::filesystem::path rom)
        : game_id_(std::move(game_id)), rom_(std::move(rom)) {
        reload();
        // The first selectable row is START GAME when a ROM is loaded and the
        // ROM row otherwise, so the selection is always on the useful action.
        selected_ = first_selectable();
        if (selected_ >= rows_.size()) {
            selected_ = 0;
        }
    }

    void set_rom(std::filesystem::path rom) { rom_ = std::move(rom); }

    // Puts the selection on the first selectable row (START GAME once a ROM is
    // loaded, the ROM row otherwise).
    void select_first() {
        selected_ = first_selectable();
        if (selected_ >= rows_.size()) {
            selected_ = 0;
        }
    }

    void reload() {
        mods_ = recomp::mods::get_all_mod_details(game_id_);
        rows_.clear();
        rows_.push_back(Row{RowKind::Section, "== START GAME ==", 0, 0});
        rows_.push_back(Row{RowKind::Start, {}, 0, 0});
        rows_.push_back(Row{RowKind::Section, "== ROM ==", 0, 0});
        rows_.push_back(Row{RowKind::Rom, {}, 0, 0});
        rows_.push_back(Row{RowKind::Section, "== MODS ==", 0, 0});
        if (mods_.empty()) {
            rows_.push_back(Row{RowKind::Section, "(none installed)", 0, 0});
        }
        for (size_t mod = 0; mod < mods_.size(); mod++) {
            rows_.push_back(Row{RowKind::Mod, {}, mod, 0});
            const recomp::config::ConfigSchema& schema =
                recomp::mods::get_mod_config_schema(mods_[mod].mod_id);
            for (size_t option = 0; option < schema.options.size(); option++) {
                if (schema.options[option].hidden) {
                    continue;
                }
                rows_.push_back(Row{RowKind::Option, {}, mod, option});
            }
        }
        if (selected_ >= rows_.size() || !selectable(selected_)) {
            selected_ = first_selectable();
            if (selected_ >= rows_.size()) {
                selected_ = 0;
            }
        }
        row_rects_.clear();
    }

    size_t row_count() const { return rows_.size(); }
    size_t selected() const { return selected_; }
    void set_selected(size_t index) { selected_ = index; }
    RowKind row_kind(size_t index) const { return rows_[index].kind; }
    // START GAME is only reachable once a ROM is loaded; section headings are
    // never reachable.
    bool selectable(size_t index) const {
        const Row& row = rows_[index];
        if (row.kind == RowKind::Section) {
            return false;
        }
        if (row.kind == RowKind::Start) {
            return !rom_.empty();
        }
        return true;
    }
    bool selected_is_rom() const { return rows_[selected_].kind == RowKind::Rom; }

    void move(int delta) {
        const int count = static_cast<int>(rows_.size());
        if (count == 0) {
            return;
        }
        int index = static_cast<int>(selected_);
        for (int step = 0; step < count; step++) {
            index = ((index + delta) % count + count) % count;
            if (selectable(static_cast<size_t>(index))) {
                selected_ = static_cast<size_t>(index);
                return;
            }
        }
    }

    // The text in the left column of a row.
    std::string left_text(size_t index) const {
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
        }
        return {};
    }

    // The text in the right column of a row (empty for most rows).
    std::string right_text(size_t index) const {
        const Row& row = rows_[index];
        switch (row.kind) {
            case RowKind::Start:
                return rom_.empty() ? std::string(kChooseRomFirst) : std::string{};
            case RowKind::Rom:
                return rom_.empty() ? std::string(kRomHint) : rom_.filename().string();
            case RowKind::Mod:
                return mods_[row.mod].short_description;
            default:
                return {};
        }
    }

    // `direction` is +1 for a forward step and -1 for a backward one. A mod row
    // ignores it and toggles; the ROM row ignores it and asks to browse.
    PanelAction activate(size_t index, int direction) {
        if (index >= rows_.size()) {
            return PanelAction::None;
        }
        const Row& row = rows_[index];
        switch (row.kind) {
            case RowKind::Section:
                return PanelAction::None;

            case RowKind::Start:
                return PanelAction::Play;

            case RowKind::Rom:
                return PanelAction::BrowseRom;

            case RowKind::Mod: {
                const recomp::mods::ModDetails& mod = mods_[row.mod];
                const bool enabled = recomp::mods::is_mod_enabled(mod.mod_id);
                recomp::mods::enable_mod(mod.mod_id, !enabled);
                // Enabling a mod can enable a required dependency, so rebuild the
                // list instead of assuming only one row changed.
                reload();
                return PanelAction::ModToggled;
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
                            return PanelAction::None;
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
                        return PanelAction::OptionChanged;
                    }
                    case ConfigOptionType::Bool: {
                        const bool current = std::holds_alternative<bool>(value)
                                                 ? std::get<bool>(value)
                                                 : std::get<recomp::config::ConfigOptionBool>(option.variant).default_value;
                        recomp::mods::set_mod_config_value(mod.mod_id, option.id, !current);
                        return PanelAction::OptionChanged;
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
                        return PanelAction::OptionChanged;
                    }
                    default:
                        return PanelAction::None;
                }
            }
        }
        return PanelAction::None;
    }

    // Row rectangles from the last frame, for mouse hit testing. Clicks are
    // handled before that frame is drawn, so a click uses the previous frame's
    // geometry (one frame of lag, which is not visible).
    SDL_Rect* hit_test(int x, int y) {
        for (SDL_Rect& rect : row_rects_) {
            if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) {
                return &rect;
            }
        }
        return nullptr;
    }

    void set_row_rects(std::vector<SDL_Rect> rects) { row_rects_ = std::move(rects); }

    // The row a rectangle belongs to, or row_count() when it is not a row.
    size_t row_for_rect(const SDL_Rect* rect) const {
        if (rect == nullptr) {
            return row_count();
        }
        const size_t index = static_cast<size_t>(rect - row_rects_.data());
        return index < row_rects_.size() ? index : row_count();
    }

private:
    size_t first_selectable() const {
        for (size_t index = 0; index < rows_.size(); index++) {
            if (selectable(index)) {
                return index;
            }
        }
        return rows_.size();
    }

    std::string game_id_;
    std::filesystem::path rom_;
    std::vector<recomp::mods::ModDetails> mods_;
    std::vector<Row> rows_;
    std::vector<SDL_Rect> row_rects_;
    size_t selected_ = 0;
};

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
        return {};
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] renderer: %s\n", SDL_GetError());
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
    StartPanel panel(context.mod_game_id, context.ready_rom);

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
    // picker, a mod row toggles, an option row steps); Left/Right only step an
    // option. Enter plays when a ROM is ready, and opens the picker otherwise or
    // on the ROM row.
    auto run_action = [&](PanelAction action) {
        switch (action) {
            case PanelAction::BrowseRom:
                browse_and_accept();
                break;
            case PanelAction::Play:
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
            panel.row_kind(index) == StartPanel::RowKind::Option) {
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

    while (running) {
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
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE || key == SDLK_q) {
                    running = false;
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
                SDL_Rect* row = panel.hit_test(mouse_x, mouse_y);
                if (row != nullptr) {
                    const size_t index = panel.row_for_rect(row);
                    if (index < panel.row_count() && panel.selectable(index)) {
                        panel.set_selected(index);
                        run_action(panel.activate(index, 1));
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

        FrameText frame;
        frame.title.build(renderer, font, kTitle, px(kTitleScale), kTitleColor);
        frame.footer.build(renderer, font, "SAVED TO: " + context.pref_dir.string(),
                           px(kFooterScale), kFooterColor);

        std::vector<std::string> error_lines;
        if (!error.empty()) {
            const int max_width = (output_width * 3) / 4;
            error_lines = wrap_text(error, chars_per_line(font, px(kErrorScale), max_width));
        }

        // Vertical layout, centred as a block.
        const int title_height = px(kTitleScale) * Font::kCellHeight;
        const int line_height = px(kErrorScale) * Font::kCellHeight;
        const int mod_line_height = px(kModScale) * Font::kCellHeight;
        const int mod_row_height = mod_line_height + px(8);
        const int section_height = mod_line_height + px(16);
        const int panel_width = std::min(output_width - px(64), px(900));
        const int panel_left = output_width / 2 - panel_width / 2;
        // The second column starts two fifths across and wraps inside what is
        // left; the first column is truncated before it.
        const int description_x = panel_left + (panel_width * 2) / 5;
        const int description_width = panel_left + panel_width - description_x;
        const int left_width = description_x - panel_left - px(16);

        // A row grows to fit the second column when it wraps.
        std::vector<std::vector<std::string>> right_lines(panel.row_count());
        std::vector<int> row_heights(panel.row_count(), mod_row_height);
        int panel_height = px(14);
        for (size_t i = 0; i < panel.row_count(); i++) {
            if (panel.row_kind(i) == StartPanel::RowKind::Section) {
                row_heights[i] = section_height;
            }
            else {
                const std::string right = panel.right_text(i);
                if (!right.empty()) {
                    right_lines[i] = wrap_text(
                        right, chars_per_line(font, px(kModScale), description_width));
                    const int lines = static_cast<int>(right_lines[i].size());
                    row_heights[i] =
                        std::max(mod_row_height, lines * mod_line_height + px(10));
                }
            }
            panel_height += row_heights[i];
        }
        panel_height += px(14) + mod_line_height + px(14);

        int block_height = title_height + px(40) + px(2) + px(38);
        if (!error_lines.empty()) {
            block_height += px(6) + line_height + px(8);
            block_height += static_cast<int>(error_lines.size()) * (line_height + px(6));
            block_height += px(8) + line_height;
        }
        block_height += px(40) + panel_height;

        int cursor_y = std::max(px(24), (output_height - block_height) / 2);
        const int center_x = output_width / 2;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        frame.title.draw(renderer, center_x, 0, cursor_y, true);
        cursor_y += title_height + px(40);

        // A thin warm rule under the wordmark, sized to it.
        SDL_SetRenderDrawColor(renderer, kWarmColor.r, kWarmColor.g, kWarmColor.b,
                               kWarmColor.a);
        const int rule_width =
            frame.title.empty() ? output_width / 3 : (frame.title.width() * 3) / 4;
        const SDL_Rect rule{center_x - rule_width / 2, cursor_y, rule_width, px(2)};
        SDL_RenderFillRect(renderer, &rule);
        cursor_y += px(2) + px(38);

        if (!error_lines.empty()) {
            frame.body.build(renderer, font, kErrorTitle, px(kErrorScale), kErrorColor);
            frame.body.draw(renderer, center_x, 0, cursor_y, true);
            cursor_y += line_height + px(8);
            for (const std::string& line : error_lines) {
                frame.body.build(renderer, font, line, px(kErrorScale), kErrorColor);
                frame.body.draw(renderer, center_x, 0, cursor_y, true);
                cursor_y += line_height + px(6);
            }
            cursor_y += px(8);
            frame.body.build(renderer, font, kErrorHint, px(kErrorScale), kHintColor);
            frame.body.draw(renderer, center_x, 0, cursor_y, true);
        }

        // The list. It is always present: the ROM row is what makes this screen
        // usable from the keyboard alone.
        {
            cursor_y += px(40);

            SDL_SetRenderDrawColor(renderer, 20, 22, 27, 240);
            const SDL_Rect background{panel_left - px(18), cursor_y - px(14),
                                      panel_width + px(36), panel_height};
            SDL_RenderFillRect(renderer, &background);

            std::vector<SDL_Rect> row_rects;
            row_rects.reserve(panel.row_count());
            for (size_t i = 0; i < panel.row_count(); i++) {
                const StartPanel::RowKind kind = panel.row_kind(i);
                const int height = row_heights[i];
                const bool is_selected = i == panel.selected() && panel.selectable(i);
                const SDL_Rect row_rect{panel_left - px(12), cursor_y - px(4),
                                        panel_width + px(24), height};
                if (is_selected) {
                    SDL_SetRenderDrawColor(renderer, 52, 58, 70, 255);
                    SDL_RenderFillRect(renderer, &row_rect);
                }

                SDL_Color color = kTitleColor;
                if (kind == StartPanel::RowKind::Section || !panel.selectable(i)) {
                    // Section headings, and START GAME before a ROM is loaded.
                    color = kHintColor;
                }
                else if (is_selected) {
                    color = kWarmColor;
                }
                else if (kind == StartPanel::RowKind::Option) {
                    color = kSubtitleColor;
                }

                // Section headings sit in a taller row.
                const int text_y =
                    cursor_y + (kind == StartPanel::RowKind::Section ? px(6) : 0);
                const std::string left =
                    truncate_to_width(font, panel.left_text(i), px(kModScale), left_width);
                frame.body.build(renderer, font, left, px(kModScale), color);
                frame.body.draw(renderer, 0, panel_left, text_y, false);

                int right_y = text_y;
                for (const std::string& line : right_lines[i]) {
                    frame.body.build(renderer, font, line, px(kModScale),
                                     is_selected ? kWarmColor : kHintColor);
                    frame.body.draw(renderer, 0, description_x, right_y, false);
                    right_y += mod_line_height;
                }

                row_rects.push_back(row_rect);
                cursor_y += height;
            }
            panel.set_row_rects(std::move(row_rects));

            cursor_y += px(14);
            frame.body.build(renderer, font, kModsHint, px(kModScale), kHintColor);
            frame.body.draw(renderer, 0, panel_left, cursor_y, false);
        }

        frame.footer.draw(renderer, center_x, 0,
                          output_height - frame.footer.height() - px(24), true);

        SDL_RenderPresent(renderer);
    }

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
