// Tiny built-in text renderer for the app's own UI (the ROM launcher).
//
// The app has no font dependency, and the launcher has to draw before the game
// runtime (and therefore before RT64) exists, so the text is drawn with SDL's
// 2D renderer from a built-in ASCII bitmap font (font.cpp holds the glyph
// table). Each glyph is rasterised into a one-channel atlas texture at load
// time; text blocks are then drawn into small supersampled textures and scaled
// down by the GPU, which gives clean anti-aliased text at any integer scale
// without a TTF library.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <SDL.h>

namespace ogre {

class Font {
public:
    // `renderer` is borrowed and must outlive the Font.
    bool init(SDL_Renderer* renderer);

    // Prepares the atlas. Safe to call for every frame; only the first call
    // does work.
    bool ensure(SDL_Renderer* renderer);

    // Text extent, in *base* pixels, for `scale` (1 = the font's cell size).
    int width(std::string_view text, int scale) const;
    int height(int scale) const { return kCellHeight * scale; }

    // Draws `text` with its `anchor_x` at the start of the baseline row and its
    // top edge at `anchor_y_baseline`, in renderer coordinates.
    void draw(SDL_Renderer* renderer, std::string_view text, int anchor_x,
              int baseline_y, int scale, SDL_Color color) const;

    // Cell geometry in base pixels (5x7 glyph + spacing + descender room).
    static constexpr int kGlyphWidth = 5;
    static constexpr int kGlyphHeight = 7;
    static constexpr int kCellWidth = 6;
    static constexpr int kCellHeight = 9;
    static constexpr char kFirstChar = ' ';
    static constexpr char kLastChar = '~';

private:
    SDL_Texture* atlas_ = nullptr;
};

}  // namespace ogre
