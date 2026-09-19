// Built-in 5x7 ASCII bitmap font for the app's own UI (see font.hpp).
//
// Each glyph is seven rows of five bits, MSB (bit 4) leftmost. Rows are laid
// out to match CELL_HEIGHT (9): glyph rows 0..6 are the body, rows 7..8 are the
// descender area lower-case letters with tails (g j p q y) draw into. The
// glyphs are deliberately drawn in a geometric all-caps-plus-lowercase style so
// the title reads as a wordmark rather than terminal output.

#include "font.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ogre {
namespace {

constexpr int kGlyphCount = Font::kLastChar - Font::kFirstChar + 1;

// Rows 0..8 per glyph; unused rows (and the gap columns) are 0.
constexpr uint8_t kGlyphs[kGlyphCount][Font::kCellHeight] = {
    // ' '  0x20
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // '!'
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100, 0b00000, 0b00000},
    // '"'
    {0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // '#'
    {0b01010, 0b11111, 0b01010, 0b01010, 0b11111, 0b01010, 0b00000, 0b00000, 0b00000},
    // '$'
    {0b00100, 0b01111, 0b10100, 0b01110, 0b00101, 0b11110, 0b00100, 0b00000, 0b00000},
    // '%'
    {0b11001, 0b11010, 0b00010, 0b00100, 0b01011, 0b10011, 0b00000, 0b00000, 0b00000},
    // '&'
    {0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101, 0b00000, 0b00000},
    // '\''
    {0b00100, 0b00100, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // '('
    {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00000, 0b00000},
    // ')'
    {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000, 0b00000, 0b00000},
    // '*'
    {0b00000, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00000, 0b00000, 0b00000},
    // '+'
    {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000, 0b00000, 0b00000},
    // ','
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000, 0b00000},
    // '-'
    {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // '.'
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00000, 0b00000},
    // '/'
    {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000, 0b00000, 0b00000},
    // '0'
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110, 0b00000, 0b00000},
    // '1'
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110, 0b00000, 0b00000},
    // '2'
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111, 0b00000, 0b00000},
    // '3'
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110, 0b00000, 0b00000},
    // '4'
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010, 0b00000, 0b00000},
    // '5'
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110, 0b00000, 0b00000},
    // '6'
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000},
    // '7'
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00000, 0b00000},
    // '8'
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000},
    // '9'
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100, 0b00000, 0b00000},
    // ':'
    {0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00110, 0b00000, 0b00000, 0b00000},
    // ';'
    {0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00100, 0b01000, 0b00000, 0b00000},
    // '<'
    {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010, 0b00000, 0b00000},
    // '='
    {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000, 0b00000},
    // '>'
    {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000, 0b00000, 0b00000},
    // '?'
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100, 0b00000, 0b00000},
    // '@'
    {0b01110, 0b10001, 0b10111, 0b10101, 0b10111, 0b10000, 0b01110, 0b00000, 0b00000},
    // 'A'
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'B'
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110, 0b00000, 0b00000},
    // 'C'
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110, 0b00000, 0b00000},
    // 'D'
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110, 0b00000, 0b00000},
    // 'E'
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111, 0b00000, 0b00000},
    // 'F'
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000, 0b00000, 0b00000},
    // 'G'
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111, 0b00000, 0b00000},
    // 'H'
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'I'
    {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110, 0b00000, 0b00000},
    // 'J'
    {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100, 0b00000, 0b00000},
    // 'K'
    {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001, 0b00000, 0b00000},
    // 'L'
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111, 0b00000, 0b00000},
    // 'M'
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'N'
    {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'O'
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000},
    // 'P'
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000, 0b00000, 0b00000},
    // 'Q'
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101, 0b00000, 0b00000},
    // 'R'
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001, 0b00000, 0b00000},
    // 'S'
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110, 0b00000, 0b00000},
    // 'T'
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00000},
    // 'U'
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000},
    // 'V'
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00000, 0b00000},
    // 'W'
    {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001, 0b00000, 0b00000},
    // 'X'
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'Y'
    {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00000},
    // 'Z'
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111, 0b00000, 0b00000},
    // '['
    {0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110, 0b00000, 0b00000},
    // '\\'
    {0b10000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00010, 0b00001, 0b00000, 0b00000},
    // ']'
    {0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110, 0b00000, 0b00000},
    // '^'
    {0b00100, 0b01010, 0b10001, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // '_'
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111, 0b00000},
    // '`'
    {0b01000, 0b00100, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000},
    // 'a'
    {0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111, 0b00000, 0b00000},
    // 'b'
    {0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b11110, 0b00000, 0b00000},
    // 'c'
    {0b00000, 0b00000, 0b01111, 0b10000, 0b10000, 0b10000, 0b01111, 0b00000, 0b00000},
    // 'd'
    {0b00001, 0b00001, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111, 0b00000, 0b00000},
    // 'e'
    {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110, 0b00000, 0b00000},
    // 'f'
    {0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000, 0b00000, 0b00000},
    // 'g'
    {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110},
    // 'h'
    {0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'i'
    {0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110, 0b00000, 0b00000},
    // 'j'
    {0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100},
    // 'k'
    {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b00000, 0b00000},
    // 'l'
    {0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110, 0b00000, 0b00000},
    // 'm'
    {0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10101, 0b10101, 0b00000, 0b00000},
    // 'n'
    {0b00000, 0b00000, 0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b00000, 0b00000},
    // 'o'
    {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000},
    // 'p'
    {0b00000, 0b00000, 0b11110, 0b10001, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000},
    // 'q'
    {0b00000, 0b00000, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001},
    // 'r'
    {0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000, 0b00000, 0b00000},
    // 's'
    {0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110, 0b00000, 0b00000},
    // 't'
    {0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110, 0b00000, 0b00000},
    // 'u'
    {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101, 0b00000, 0b00000},
    // 'v'
    {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00000, 0b00000},
    // 'w'
    {0b00000, 0b00000, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010, 0b00000, 0b00000},
    // 'x'
    {0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b00000, 0b00000},
    // 'y'
    {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110},
    // 'z'
    {0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111, 0b00000, 0b00000},
    // '{'
    {0b00110, 0b00100, 0b00100, 0b01000, 0b00100, 0b00100, 0b00110, 0b00000, 0b00000},
    // '|'
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00000},
    // '}'
    {0b01100, 0b00100, 0b00100, 0b00010, 0b00100, 0b00100, 0b01100, 0b00000, 0b00000},
    // '~'
    {0b00000, 0b00000, 0b00000, 0b01000, 0b10101, 0b00010, 0b00000, 0b00000, 0b00000},
};

// Builds a one-channel (alpha) mask of every glyph, one glyph per cell.
SDL_Surface* build_glyph_atlas() {
    const int width = kGlyphCount * Font::kCellWidth;
    const int height = Font::kCellHeight;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 8, SDL_PIXELFORMAT_INDEX8);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_Color palette[256] = {};
    for (int i = 0; i < 256; ++i) {
        palette[i] = SDL_Color{255, 255, 255, static_cast<uint8_t>(i)};
    }
    SDL_SetPaletteColors(surface->format->palette, palette, 0, 256);
    SDL_SetColorKey(surface, SDL_FALSE, 0);

    if (SDL_MUSTLOCK(surface)) {
        SDL_LockSurface(surface);
    }
    std::memset(surface->pixels, 0, static_cast<size_t>(surface->pitch) * height);
    for (int g = 0; g < kGlyphCount; ++g) {
        for (int row = 0; row < Font::kCellHeight; ++row) {
            const uint8_t bits = kGlyphs[g][row];
            uint8_t* dst = static_cast<uint8_t*>(surface->pixels) +
                           static_cast<size_t>(row) * surface->pitch +
                           g * Font::kCellWidth;
            for (int col = 0; col < Font::kGlyphWidth; ++col) {
                if ((bits >> (Font::kGlyphWidth - 1 - col)) & 1u) {
                    dst[col] = 255;
                }
            }
        }
    }
    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }
    return surface;
}

}  // namespace

bool Font::init(SDL_Renderer* renderer) {
    return ensure(renderer);
}

bool Font::ensure(SDL_Renderer* renderer) {
    if (atlas_ != nullptr) {
        return true;
    }
    if (renderer == nullptr) {
        return false;
    }
    SDL_Surface* surface = build_glyph_atlas();
    if (surface == nullptr) {
        std::fprintf(stderr, "[launcher] font atlas: %s\n", SDL_GetError());
        return false;
    }
    atlas_ = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (atlas_ == nullptr) {
        std::fprintf(stderr, "[launcher] font texture: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas_, SDL_ScaleModeNearest);
    return true;
}

int Font::width(std::string_view text, int scale) const {
    return static_cast<int>(text.size()) * kCellWidth * scale;
}

void Font::draw(SDL_Renderer* renderer, std::string_view text, int x, int y,
                int scale, SDL_Color color) const {
    if (atlas_ == nullptr || renderer == nullptr || text.empty() || scale <= 0) {
        return;
    }
    SDL_SetTextureColorMod(atlas_, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(atlas_, color.a);
    for (char raw : text) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch < static_cast<unsigned char>(kFirstChar) ||
            ch > static_cast<unsigned char>(kLastChar)) {
            x += kCellWidth * scale;
            continue;
        }
        const int index = ch - kFirstChar;
        SDL_Rect src{index * kCellWidth, 0, kCellWidth, kCellHeight};
        SDL_Rect dst{x, y, kCellWidth * scale, kCellHeight * scale};
        SDL_RenderCopy(renderer, atlas_, &src, &dst);
        x += kCellWidth * scale;
    }
    SDL_SetTextureColorMod(atlas_, 255, 255, 255);
    SDL_SetTextureAlphaMod(atlas_, 255);
}

}  // namespace ogre
