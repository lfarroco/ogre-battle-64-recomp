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

// The branding and the two prompt lines. The prompt text is the MVP spec
// verbatim ("Click to load your ROM (or drop it in this window)").
constexpr const char* kTitle = "OGRE BATTLE 64: RECOMP";
constexpr const char* kSubtitle = "CLICK TO LOAD YOUR ROM (OR DROP IT IN THIS WINDOW)";
constexpr const char* kReadyHint = "OR PLACE THE ROM IN THIS FOLDER AND LAUNCH AGAIN";
constexpr const char* kPlayPrompt = "PRESS ENTER TO PLAY";
constexpr const char* kErrorTitle = "THAT IS NOT A USABLE ROM";
constexpr const char* kErrorHint = "CLICK OR DROP A ROM TO TRY AGAIN";
constexpr const char* kModsTitle = "MODS";
constexpr const char* kModsHint = "UP/DOWN SELECT   SPACE/CLICK TOGGLE   LEFT/RIGHT CHANGE VALUE";

// A black window; the content sits slightly above centre.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kTitleScale = 5;
constexpr int kSubtitleScale = 2;
constexpr int kBodyScale = 2;
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
    TextLayer subtitle;
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

// --- mod panel ---------------------------------------------------------------
//
// The start screen owns the mod toggles. The runtime's mod system has no UI of
// its own (`app/src/renderer.cpp` was adapted from RecompFrontend "minus the
// RecompFrontend UI"), so this panel is the player's way to turn a shipped mod
// off.
//
// The list is flat: one row per mod, then one row per visible config option of
// that mod. Up/Down reach every row, Space activates the selected row (a mod
// row toggles the mod, an option row steps its value), and Left/Right step the
// value of an option row. Mouse clicks activate the row under the pointer.

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

struct ModRow {
    bool is_mod = true;
    size_t mod = 0;     // index into ModPanel::mods
    size_t option = 0;  // index into that mod's config schema options
};

class ModPanel {
public:
    explicit ModPanel(std::string game_id) : game_id_(std::move(game_id)) { reload(); }

    void reload() {
        mods_ = recomp::mods::get_all_mod_details(game_id_);
        rows_.clear();
        for (size_t mod = 0; mod < mods_.size(); mod++) {
            rows_.push_back(ModRow{true, mod, 0});
            const recomp::config::ConfigSchema& schema =
                recomp::mods::get_mod_config_schema(mods_[mod].mod_id);
            for (size_t option = 0; option < schema.options.size(); option++) {
                if (schema.options[option].hidden) {
                    continue;
                }
                rows_.push_back(ModRow{false, mod, option});
            }
        }
        if (rows_.empty()) {
            selected_ = 0;
        }
        else if (selected_ >= rows_.size()) {
            selected_ = rows_.size() - 1;
        }
        row_rects_.clear();
    }

    bool empty() const { return rows_.empty(); }
    size_t row_count() const { return rows_.size(); }
    size_t selected() const { return selected_; }
    bool is_mod_row(size_t index) const { return rows_[index].is_mod; }

    void move(int delta) {
        if (rows_.empty()) {
            return;
        }
        const int count = static_cast<int>(rows_.size());
        int index = static_cast<int>(selected_) + delta;
        index = ((index % count) + count) % count;
        selected_ = static_cast<size_t>(index);
    }

    std::string row_text(size_t index) const {
        const ModRow& row = rows_[index];
        const recomp::mods::ModDetails& mod = mods_[row.mod];
        if (row.is_mod) {
            const bool enabled = recomp::mods::is_mod_enabled(mod.mod_id);
            return std::string(enabled ? "[x] " : "[ ] ") + mod.display_name;
        }
        const recomp::config::ConfigSchema& schema =
            recomp::mods::get_mod_config_schema(mod.mod_id);
        const recomp::config::ConfigOption& option = schema.options[row.option];
        return "      " + option.name + ": " +
               config_value_text(option, recomp::mods::get_mod_config_value(mod.mod_id, option.id));
    }

    // The description of the selected mod, shown under its row. Empty when the
    // row is a config option or the mod has no description.
    std::string selected_description() const {
        if (rows_.empty() || !rows_[selected_].is_mod) {
            return {};
        }
        return mods_[rows_[selected_].mod].short_description;
    }

    // `direction` is +1 for a forward step and -1 for a backward one. A mod row
    // ignores it and toggles.
    void activate(size_t index, int direction) {
        if (index >= rows_.size()) {
            return;
        }
        const ModRow& row = rows_[index];
        const recomp::mods::ModDetails& mod = mods_[row.mod];
        if (row.is_mod) {
            const bool enabled = recomp::mods::is_mod_enabled(mod.mod_id);
            recomp::mods::enable_mod(mod.mod_id, !enabled);
            // Enabling a mod can enable a required dependency, so rebuild the
            // list instead of assuming only one row changed.
            reload();
            return;
        }

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
                    return;
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
                break;
            }
            case ConfigOptionType::Bool: {
                const bool current = std::holds_alternative<bool>(value)
                                         ? std::get<bool>(value)
                                         : std::get<recomp::config::ConfigOptionBool>(option.variant).default_value;
                recomp::mods::set_mod_config_value(mod.mod_id, option.id, !current);
                break;
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
                break;
            }
            default:
                break;
        }
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
    const std::vector<SDL_Rect>& row_rects() const { return row_rects_; }

    // The row a rectangle belongs to, or row_count() when it is not a row.
    size_t row_for_rect(const SDL_Rect* rect) const {
        if (rect == nullptr) {
            return row_count();
        }
        const size_t index = static_cast<size_t>(rect - row_rects_.data());
        return index < row_rects_.size() ? index : row_count();
    }

private:
    std::string game_id_;
    std::vector<recomp::mods::ModDetails> mods_;
    std::vector<ModRow> rows_;
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

    // A ROM found before this screen opened is ready to play: Enter, a click on
    // empty space, or a dropped ROM all leave with it.
    const std::filesystem::path ready_rom = context.ready_rom;
    ModPanel panel(context.mod_game_id);

    auto play = [&]() {
        if (!ready_rom.empty()) {
            accepted = ready_rom;
            running = false;
        }
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
            accepted = chosen;
            running = false;
        }
        else {
            error = std::move(reason);
        }
    };

    // What both the SDL_DROPFILE event and the test hook below run.
    auto handle_dropped = [&](const std::filesystem::path& dropped) {
        std::string reason = context.accept_rom(dropped);
        if (reason.empty()) {
            accepted = dropped;
            running = false;
        }
        else {
            error = std::move(reason);
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
                    panel.activate(panel.selected(), -1);
                }
                else if (key == SDLK_RIGHT) {
                    panel.activate(panel.selected(), 1);
                }
                else if (key == SDLK_SPACE) {
                    panel.activate(panel.selected(), 1);
                }
                else if (key == SDLK_RETURN || key == SDLK_RETURN2 || key == SDLK_KP_ENTER) {
                    play();
                    if (running) {
                        browse_and_accept();
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
                    panel.activate(panel.row_for_rect(row), 1);
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
        const char* subtitle = ready_rom.empty() ? kSubtitle : kPlayPrompt;
        frame.title.build(renderer, font, kTitle, px(kTitleScale), kTitleColor);
        frame.subtitle.build(renderer, font, subtitle, px(kSubtitleScale), kSubtitleColor);
        frame.footer.build(renderer, font, "SAVED TO: " + context.pref_dir.string(),
                           px(kFooterScale), kFooterColor);

        std::vector<std::string> error_lines;
        if (!error.empty()) {
            const int max_width = (output_width * 3) / 4;
            error_lines = wrap_text(error, chars_per_line(font, px(kErrorScale), max_width));
        }

        // Vertical layout, centred as a block.
        const int title_height = px(kTitleScale) * Font::kCellHeight;
        const int subtitle_height = px(kSubtitleScale) * Font::kCellHeight;
        const int line_height = px(kErrorScale) * Font::kCellHeight;
        const int mod_line_height = px(kModScale) * Font::kCellHeight;
        const int mod_row_height = mod_line_height + px(8);
        const int panel_width = std::min(output_width - px(64), px(880));
        const int panel_left = output_width / 2 - panel_width / 2;
        const std::string mod_description = panel.selected_description();
        const std::vector<std::string> mod_description_lines =
            mod_description.empty()
                ? std::vector<std::string>{}
                : wrap_text(mod_description,
                            chars_per_line(font, px(kModScale), panel_width));

        int panel_height = 0;
        if (!panel.empty()) {
            panel_height = px(16) + mod_line_height + px(14) +
                           static_cast<int>(panel.row_count()) * mod_row_height +
                           px(10) + mod_line_height;
            if (!mod_description_lines.empty()) {
                panel_height += px(8) + static_cast<int>(mod_description_lines.size()) *
                                            mod_line_height;
            }
        }

        int block_height = title_height + px(40) + px(2) + px(38) + subtitle_height;
        block_height += px(56);
        if (!error_lines.empty()) {
            block_height += px(6) + line_height + px(8);
            block_height += static_cast<int>(error_lines.size()) * (line_height + px(6));
            block_height += px(8) + line_height;
        }
        else if (ready_rom.empty()) {
            block_height += px(kBodyScale) * Font::kCellHeight;
        }
        if (panel_height > 0) {
            block_height += px(30) + panel_height;
        }

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

        frame.subtitle.draw(renderer, center_x, 0, cursor_y, true);
        cursor_y += subtitle_height + px(56);

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
        else if (ready_rom.empty()) {
            frame.body.build(renderer, font, kReadyHint, px(kBodyScale), kHintColor);
            frame.body.draw(renderer, center_x, 0, cursor_y, true);
        }

        if (!panel.empty()) {
            cursor_y += px(30);

            SDL_SetRenderDrawColor(renderer, 20, 22, 27, 240);
            const SDL_Rect background{panel_left - px(18), cursor_y - px(14),
                                      panel_width + px(36), panel_height};
            SDL_RenderFillRect(renderer, &background);

            frame.body.build(renderer, font, kModsTitle, px(kModScale), kHintColor);
            frame.body.draw(renderer, 0, panel_left, cursor_y, false);
            cursor_y += mod_line_height + px(14);

            std::vector<SDL_Rect> row_rects;
            row_rects.reserve(panel.row_count());
            for (size_t i = 0; i < panel.row_count(); i++) {
                const SDL_Rect row_rect{panel_left - px(12), cursor_y - px(4),
                                        panel_width + px(24), mod_row_height};
                if (i == panel.selected()) {
                    SDL_SetRenderDrawColor(renderer, 52, 58, 70, 255);
                    SDL_RenderFillRect(renderer, &row_rect);
                }
                const SDL_Color color =
                    i == panel.selected() ? kWarmColor
                                          : (panel.is_mod_row(i) ? kTitleColor : kSubtitleColor);
                frame.body.build(renderer, font, panel.row_text(i), px(kModScale), color);
                frame.body.draw(renderer, 0, panel_left, cursor_y, false);
                row_rects.push_back(row_rect);
                cursor_y += mod_row_height;
            }
            panel.set_row_rects(std::move(row_rects));

            if (!mod_description_lines.empty()) {
                cursor_y += px(8);
                for (const std::string& line : mod_description_lines) {
                    frame.body.build(renderer, font, line, px(kModScale), kHintColor);
                    frame.body.draw(renderer, 0, panel_left, cursor_y, false);
                    cursor_y += mod_line_height;
                }
            }

            cursor_y += px(10);
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
