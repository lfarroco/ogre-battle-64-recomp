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
constexpr const char* kErrorTitle = "THAT IS NOT A USABLE ROM";
constexpr const char* kErrorHint = "CLICK OR DROP A ROM TO TRY AGAIN";

// A black window; the content sits slightly above centre.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kTitleScale = 5;
constexpr int kSubtitleScale = 2;
constexpr int kBodyScale = 2;
constexpr int kErrorScale = 2;
constexpr int kFooterScale = 2;

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
    // Plain (not u8) literals: nfdu8char_t is `char`, and in C++20 u8"" is
    // char8_t, which does not convert. Standard execution encoding is UTF-8 on
    // every platform this builds for.
    const nfdnfilteritem_t filters[] = {
        {"N64 ROM (*.z64; *.n64; *.v64)", "z64,n64,v64"},
        {"All files", "*"},
    };
    nfdnchar_t* chosen = nullptr;
    const nfdresult_t result =
        NFD_OpenDialogN(&chosen, filters, SDL_arraysize(filters), nullptr);
    if (result != NFD_OKAY) {
        return {};
    }
    std::filesystem::path path{chosen};
    NFD_FreePathN(chosen);
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
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN &&
                     (event.key.keysym.sym == SDLK_ESCAPE ||
                      event.key.keysym.sym == SDLK_q)) {
                running = false;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN &&
                     event.button.button == SDL_BUTTON_LEFT) {
                const std::filesystem::path chosen = browse_for_rom();
                if (chosen.empty()) {
#if !defined(OGRE_HAVE_NFD)
                    error = "This build has no file browser. Drop the ROM onto this "
                            "window, or set OGRE_ROM to its path.";
#endif
                    continue;
                }
                std::string reason = context.accept_rom(chosen);
                if (reason.empty()) {
                    accepted = chosen;
                    running = false;
                }
                else {
                    error = std::move(reason);
                }
            }
            else if (event.type == SDL_DROPFILE) {
                const std::filesystem::path dropped{event.drop.file};
                SDL_free(event.drop.file);
                handle_dropped(dropped);
            }
        }

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer, &output_width, &output_height);
        const float ui_scale =
            std::max(1.0f, static_cast<float>(output_height) / kWindowHeight);
        auto px = [ui_scale](int value) {
            return static_cast<int>(static_cast<float>(value) * ui_scale);
        };

        FrameText frame;
        frame.title.build(renderer, font, kTitle, px(kTitleScale), kTitleColor);
        frame.subtitle.build(renderer, font, kSubtitle, px(kSubtitleScale), kSubtitleColor);
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
        int block_height = title_height + px(40) + px(2) + px(38) + subtitle_height;
        block_height += px(56);
        if (error_lines.empty()) {
            block_height += px(kBodyScale) * Font::kCellHeight;
        }
        else {
            block_height += px(6) + line_height + px(8);
            block_height += static_cast<int>(error_lines.size()) * (line_height + px(6));
            block_height += px(8) + line_height;
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

        if (error_lines.empty()) {
            frame.body.build(renderer, font, kReadyHint, px(kBodyScale), kHintColor);
            frame.body.draw(renderer, center_x, 0, cursor_y, true);
        }
        else {
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
