// See hd_backgrounds.hpp for the pipeline and the call site.

#include "hd_backgrounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__EMSCRIPTEN__)
// `widescreen_mode()` selects the `-wide` image. The browser build has no
// settings screen and no widescreen mode, so it always uses the plain image.
#include "settings.hpp"
#endif

// A single-header decoder. RT64 compiles its own copy of stb_image into the
// renderer, so this one is `static` to keep the two from colliding at link
// time. It is the only reason the app needs the RT64 contrib include directory;
// the file is present for every build flavor, renderer or not.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb/stb_image.h"

// The reference extractor's PNG writer (`OGRE_BG_DUMP`). Its HDR path calls
// `sprintf`, which Clang marks deprecated; the header is vendored and that path
// is unused here, so silence the one warning rather than carry it in every
// build's log.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {

// --- RDRAM access ----------------------------------------------------------
// `recomp.h` defines guest memory as: MEM_W a native u32 at `offset`, MEM_H a
// native u16 at `offset ^ 2`, MEM_B a native u8 at `offset ^ 3`, where
// `offset = guest - 0x80000000` for KSEG0.
constexpr uint32_t kKseg0 = 0x80000000u;
constexpr uint32_t kRamEnd = 0x80800000u;

// The backdrop canvas the four njpeg sub-images tile. The 'B5' asset places its
// chunks at (-229,-235), (90,-235), (-229,4) and (90,4) with sizes 320x240,
// 176x240, 320x144 and 176x144 (`assets/ogre64.z64` ROM 0x7CADB4 and following),
// which is a 496x384 canvas: a 320-wide left column, a 176-wide right column, a
// 240-tall top row and a 144-tall bottom row.
constexpr int kCanvasW = 496;
constexpr int kCanvasH = 384;

inline uint32_t offset_of(uint32_t guest) { return guest & 0x1FFFFFFFu; }

inline uint8_t load_u8(const uint8_t* rdram, uint32_t guest) {
    return rdram[offset_of(guest) ^ 3];
}

inline uint16_t load_u16(const uint8_t* rdram, uint32_t guest) {
    uint16_t v = 0;
    std::memcpy(&v, rdram + (offset_of(guest) ^ 2), sizeof(v));
    return v;
}

inline uint32_t load_u32(const uint8_t* rdram, uint32_t guest) {
    uint32_t v = 0;
    std::memcpy(&v, rdram + offset_of(guest), sizeof(v));
    return v;
}

inline void store_u16(uint8_t* rdram, uint32_t guest, uint16_t v) {
    std::memcpy(rdram + (offset_of(guest) ^ 2), &v, sizeof(v));
}

// --- The pack --------------------------------------------------------------

struct Entry {
    std::string id;
    std::string name;
    uint16_t scene = 0;
    uint16_t step = 0;
    std::vector<uint16_t> pixels;       // kCanvasW x kCanvasH, guest-order RGBA5551
    std::vector<uint16_t> pixels_wide;  // the `-wide` sibling, if the pack has one
};

struct Pack {
    bool loaded = false;
    std::filesystem::path dir;
    std::vector<Entry> entries;
};

Pack g_pack;

int to_5bit(uint32_t c) { return int((c * 31u + 127u) / 255u); }

// Box filter the source to `dw` x `dh` and convert to big-endian RGBA5551.
std::vector<uint16_t> resample(const uint8_t* rgba, int sw, int sh, int dw, int dh) {
    std::vector<uint16_t> out(size_t(dw) * dh);
    for (int dy = 0; dy < dh; ++dy) {
        const int y0 = dy * sh / dh;
        const int y1 = std::max(y0 + 1, (dy + 1) * sh / dh);
        for (int dx = 0; dx < dw; ++dx) {
            const int x0 = dx * sw / dw;
            const int x1 = std::max(x0 + 1, (dx + 1) * sw / dw);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = rgba + size_t(sy) * sw * 4;
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + size_t(sx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            }
            const uint32_t rr = to_5bit(r / n);
            const uint32_t gg = to_5bit(g / n);
            const uint32_t bb = to_5bit(b / n);
            out[size_t(dy) * dw + dx] =
                uint16_t((rr << 11) | (gg << 6) | (bb << 1) | 1u);  // opaque
        }
    }
    return out;
}

// Box filter the source to the backdrop canvas and convert to big-endian
// RGBA5551. This stretches the source to the canvas shape, which is what the
// plain image wants: the game's own canvas is the shape the draw expects.
std::vector<uint16_t> to_canvas(const uint8_t* rgba, int sw, int sh) {
    return resample(rgba, sw, sh, kCanvasW, kCanvasH);
}

// Fit the source into the canvas **preserving its aspect ratio**, for the
// widescreen variant. A 16:9 image is wider than the canvas, so it fills the
// canvas width and its height is 496*9/16 = 279 rows, anchored to the canvas
// bottom (the lower part of the canvas is what the draw shows); the rows above
// repeat its first row. Stretching a 16:9 source to the 496x384 canvas instead
// displays it 29 % narrow (measured: a circle drawn 1:1 in a 1920x1080 source
// showed at 0.71 aspect, against 0.96 for a canvas-aspect source).
std::vector<uint16_t> to_canvas_fit(const uint8_t* rgba, int sw, int sh) {
    std::vector<uint16_t> canvas(size_t(kCanvasW) * kCanvasH);
    if (sw <= 0 || sh <= 0) {
        return canvas;
    }
    int dw = kCanvasW;
    int dh = std::max(1, int(std::lround(double(kCanvasW) * sh / sw)));
    if (dh > kCanvasH) {
        dh = kCanvasH;
        dw = std::max(1, int(std::lround(double(kCanvasH) * sw / sh)));
    }
    const std::vector<uint16_t> img = resample(rgba, sw, sh, dw, dh);
    const int x0 = (kCanvasW - dw) / 2;
    const int y0 = kCanvasH - dh;  // bottom-anchored
    for (int y = 0; y < kCanvasH; ++y) {
        const int iy = std::min(std::max(y - y0, 0), dh - 1);
        for (int x = 0; x < kCanvasW; ++x) {
            const int ix = std::min(std::max(x - x0, 0), dw - 1);
            canvas[size_t(y) * kCanvasW + x] = img[size_t(iy) * dw + ix];
        }
    }
    return canvas;
}

// Load one image file into the canvas form, or leave `out` empty. `fit` picks
// the aspect-preserving mapping used for the widescreen variant.
bool load_canvas(const std::filesystem::path& png, const char* what, bool fit,
                 std::vector<uint16_t>& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(png, ec)) {
        return false;
    }
    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(png.string().c_str(), &w, &h, &comp, 4);
    if (data == nullptr) {
        std::fprintf(stderr, "[hdbg] %s: stb_image failed: %s\n", png.string().c_str(),
                     stbi_failure_reason());
        return false;
    }
    out = fit ? to_canvas_fit(data, w, h) : to_canvas(data, w, h);
    stbi_image_free(data);
    std::fprintf(stderr, "[hdbg]   %-13s <- %s (%dx%d -> %s)\n", what,
                 png.filename().string().c_str(), w, h,
                 fit ? "canvas, aspect kept" : "canvas");
    return true;
}

// An entry's image is `<id>.png` (or `<name>.png`), stretched to the canvas. A
// `<id>-wide.png` (or `<name>-wide.png`) beside it is the widescreen variant,
// used when the WIDESCREEN toggle is on. The wide image keeps its own aspect
// ratio (a 16:9 source fills the canvas width and is anchored to the bottom), so
// a widescreen-aspect source is not squeezed to the canvas's 496x384 shape. See
// `mods/backgrounds/make-example.py` for the placeholder pair.
bool load_entry_image(Entry& e) {
    const std::vector<std::string> stems{e.id, e.name};
    bool any = false;
    for (const std::string& stem : stems) {
        if (e.pixels.empty() &&
            load_canvas(g_pack.dir / (stem + ".png"), stem.c_str(), false, e.pixels)) {
            any = true;
        }
        if (e.pixels_wide.empty() &&
            load_canvas(g_pack.dir / (stem + "-wide.png"), (stem + "-wide").c_str(), true,
                        e.pixels_wide)) {
            any = true;
        }
    }
    if (!any) {
        std::fprintf(stderr, "[hdbg] %s: no %s.png or %s.png\n", e.id.c_str(),
                     e.id.c_str(), e.name.c_str());
        return false;
    }
    std::fprintf(stderr, "[hdbg] %-10s %-12s scene=0x%02X step=%-3u%s\n", e.id.c_str(),
                 e.name.c_str(), e.scene, e.step,
                 e.pixels_wide.empty() ? "" : " (+ -wide)");
    return true;
}

// `backgrounds.txt`: one `<id> <name> <scene> <step>` per line, '#' comments.
bool load_pack(const std::filesystem::path& dir) {
    std::FILE* f = std::fopen((dir / "backgrounds.txt").string().c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    g_pack.dir = dir;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (char* hash = std::strchr(line, '#'); hash != nullptr) {
            *hash = '\0';
        }
        char id[128] = {0};
        char name[128] = {0};
        char scene[64] = {0};
        char step[64] = {0};
        if (std::sscanf(line, "%127s %127s %63s %63s", id, name, scene, step) != 4) {
            continue;
        }
        Entry e;
        e.id = id;
        e.name = name;
        e.scene = uint16_t(std::strtoul(scene, nullptr, 0) & 0x7FFFu);
        e.step = uint16_t(std::strtoul(step, nullptr, 0) & 0x7FFFu);
        if (load_entry_image(e)) {
            g_pack.entries.push_back(std::move(e));
        }
    }
    std::fclose(f);
    g_pack.loaded = !g_pack.entries.empty();
    std::fprintf(stderr, "[hdbg] pack %s: %zu background(s) loaded\n", dir.string().c_str(),
                 g_pack.entries.size());
    return true;
}

const Entry* find_entry(uint16_t scene, uint16_t next_scene, uint16_t step) {
    // Prefer the entry whose scene is current or pending. The 0x0D dialogue
    // backdrop is assembled one scene early, by the 0x02 loader's enter
    // (`func_80178920` -> `func_80198D28`), so the mapped scene is often not
    // current yet; the scripted step still identifies the image, so fall back to
    // the step alone.
    const Entry* by_step = nullptr;
    for (const Entry& e : g_pack.entries) {
        if (e.step != step) {
            continue;
        }
        if (e.scene == scene || e.scene == next_scene) {
            return &e;
        }
        if (by_step == nullptr) {
            by_step = &e;
        }
    }
    return by_step;
}

// Which part of the canvas a readback pass wrote, from the pass's dimensions.
// The four passes are the four chunks of the 'B5' asset in order, and the sizes
// name the column and the row of the tiling above. The x offsets are 1 less than
// the column width in the asset's own header (a 1px overlap), and the y offsets
// are 1 less than the row height; the rounded values below are what the draw
// uses.
bool chunk_rect(uint32_t w, uint32_t h, int& x, int& y) {
    if (w == 320 && h == 240) {
        x = 0;
        y = 0;
    }
    else if (w == 176 && h == 240) {
        x = 320;
        y = 0;
    }
    else if (w == 320 && h == 144) {
        x = 0;
        y = 240;
    }
    else if (w == 176 && h == 144) {
        x = 320;
        y = 240;
    }
    else {
        return false;
    }
    return true;
}

// The pass's index in the four-chunk tiling, for the dump's completion mask.
int chunk_index(uint32_t w, uint32_t h) {
    if (w == 320 && h == 240) {
        return 0;
    }
    if (w == 176 && h == 240) {
        return 1;
    }
    if (w == 320 && h == 144) {
        return 2;
    }
    if (w == 176 && h == 144) {
        return 3;
    }
    return -1;
}

// --- Original-backdrop dump (`OGRE_BG_DUMP=<dir>`) ---------------------------
// The reference an artist draws a replacement from: the game's own four
// sub-images assembled into the 496x384 canvas, written as one PNG per
// assembly. It runs on every pass the readback copies, before the pack touches
// the pixels, so one run dumps every backdrop it assembles and the same switch
// covers later backgrounds. See `tools/backgrounds.py`.
struct Dump {
    bool enabled = false;
    std::filesystem::path dir;
    uint16_t step = 0xFFFF;
    unsigned have = 0;
    std::vector<uint16_t> canvas;
};

Dump g_dump;

void dump_write_png() {
    std::vector<uint8_t> rgba(size_t(kCanvasW) * kCanvasH * 4);
    for (size_t i = 0; i < g_dump.canvas.size(); ++i) {
        const uint32_t v = g_dump.canvas[i];
        rgba[i * 4 + 0] = uint8_t(((v >> 11) & 31) * 255 / 31);
        rgba[i * 4 + 1] = uint8_t(((v >> 6) & 31) * 255 / 31);
        rgba[i * 4 + 2] = uint8_t(((v >> 1) & 31) * 255 / 31);
        rgba[i * 4 + 3] = 255;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "original-step%04u.png", (unsigned)g_dump.step);
    const std::filesystem::path out = g_dump.dir / name;
    const int ok = stbi_write_png(out.string().c_str(), kCanvasW, kCanvasH, 4, rgba.data(),
                                  kCanvasW * 4);
    std::fprintf(stderr, "[hdbg] dump %s%s\n", out.string().c_str(), ok ? "" : " (failed)");
}

void dump_pass(const uint8_t* rdram, uint32_t base, uint32_t w, uint32_t h, int cx, int cy,
               uint16_t step) {
    if (!g_dump.enabled) {
        return;
    }
    if (step != g_dump.step || g_dump.canvas.empty()) {
        g_dump.step = step;
        g_dump.have = 0;
        g_dump.canvas.assign(size_t(kCanvasW) * kCanvasH, 0);
    }
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            g_dump.canvas[size_t(cy + row) * kCanvasW + cx + col] =
                load_u16(rdram, base + (row * w + col) * 2);
        }
    }
    g_dump.have |= 1u << chunk_index(w, h);
    if (g_dump.have == 0xF) {
        dump_write_png();
    }
}

// Debug: paint each readback pass a distinct colour, so `OGRE_BG_DEBUG=1` plus a
// screen capture reads the four-pass composition off the screen.
bool debug_fill(uint8_t* rdram, uint32_t base, uint32_t w, uint32_t h) {
    uint16_t colour = 0;
    if (w == 320 && h == 240) {
        colour = 0xF801;  // red
    }
    else if (w == 176 && h == 240) {
        colour = 0x07E1;  // green
    }
    else if (w == 320 && h == 144) {
        colour = 0x003F;  // blue
    }
    else if (w == 176 && h == 144) {
        colour = 0xFFC1;  // yellow
    }
    else {
        return false;
    }
    for (uint32_t i = 0; i < w * h; ++i) {
        store_u16(rdram, base + i * 2, colour);
    }
    return true;
}

}  // namespace

namespace ogre {
namespace {

// `OGRE_BG=0` (or `off` / `false`) turns the pack off for one run. `OGRE_BG_DIR`
// names the pack folder and, when set, is the only folder searched, so pointing
// it at an empty directory is the other way to run without a pack.
bool pack_disabled() {
    const char* value = std::getenv("OGRE_BG");
    if (value == nullptr) {
        return false;
    }
    return std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
           std::strcmp(value, "false") == 0 || std::strcmp(value, "no") == 0;
}

}  // namespace

void hd_backgrounds_init(const std::filesystem::path& pref_dir) {
    // The dump is independent of the pack: it writes the game's own backdrop, so
    // it works with no `mods/backgrounds/` at all (`tools/backgrounds.py`).
    if (const char* dump = std::getenv("OGRE_BG_DUMP"); dump != nullptr && dump[0] != '\0') {
        std::error_code ec;
        g_dump.dir = dump;
        std::filesystem::create_directories(g_dump.dir, ec);
        g_dump.enabled = !ec;
        std::fprintf(stderr, "[hdbg] dump %s\n",
                     g_dump.enabled ? g_dump.dir.string().c_str() : "(directory failed)");
    }

    if (pack_disabled()) {
        std::fprintf(stderr, "[hdbg] pack disabled (OGRE_BG=%s)\n", std::getenv("OGRE_BG"));
        return;
    }

    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("OGRE_BG_DIR"); env != nullptr && env[0] != '\0') {
        // An explicit folder is the only candidate, so `OGRE_BG_DIR` can both
        // choose a pack and (pointed at an empty folder) disable the default one.
        candidates.emplace_back(env);
    }
    else {
        candidates.push_back(pref_dir / "mods" / "backgrounds");
        candidates.push_back(pref_dir.parent_path() / "mods" / "backgrounds");
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec) {
            candidates.push_back(cwd / "mods" / "backgrounds");
        }
    }
    for (const std::filesystem::path& dir : candidates) {
        if (load_pack(dir)) {
            return;
        }
    }
    std::fprintf(stderr, "[hdbg] no backgrounds.txt found (looked in %zu place(s))\n",
                 candidates.size());
}

}  // namespace ogre

bool ogre::hd_backgrounds_covers_scene(uint16_t scene) {
    for (const Entry& e : g_pack.entries) {
        if (e.scene == scene) {
            return true;
        }
    }
    return false;
}

// Called by the generated njpeg readback (tools/hd_backgrounds.py) at the end of
// each pass's row copy.
extern "C" void ogre_hd_background(uint8_t* rdram) {
    if (!g_pack.loaded && !g_dump.enabled) {
        return;
    }

    // The pipeline's state object. The pointer lives at a fixed guest word in
    // the main segment; the fields are the ones func_ovlE_8019976C reads.
    const uint32_t state = load_u32(rdram, 0x8019A680u);
    if (state < kKseg0 || state >= kRamEnd) {
        return;
    }
    const uint32_t dest = load_u32(rdram, state + 0x74u);
    if (dest < kKseg0 || dest >= kRamEnd) {
        return;
    }
    const uint32_t w = load_u16(rdram, state + 0x78u);
    const uint32_t h = load_u16(rdram, state + 0x7Au);
    const bool single_image = load_u8(rdram, state + 0x80u) == 0;

    // The scripted-sequence words. The readable scene id is the low 15 bits
    // (`0x8000 | id`, plus the control values).
    const uint16_t step = load_u16(rdram, 0x8018F1C0u) & 0x7FFFu;
    const uint16_t scene = load_u16(rdram, 0x800C4C26u) & 0x7FFFu;
    const uint16_t next_scene = load_u16(rdram, 0x8018F1C2u) & 0x7FFFu;

    // The single-image path writes an 8-byte header at `dest`; the 'B5' path's
    // pixels start there.
    const uint32_t base = dest + (single_image ? 8u : 0u);

    // The reference dump reads the game's own pixels here, before the pack
    // replaces them.
    int cx = 0, cy = 0;
    const bool known_chunk = chunk_rect(w, h, cx, cy);
    if (known_chunk) {
        dump_pass(rdram, base, w, h, cx, cy, step);
    }

    if (!g_pack.loaded) {
        return;
    }

    if (std::getenv("OGRE_BG_DEBUG") != nullptr) {
        if (debug_fill(rdram, base, w, h)) {
            std::fprintf(stderr, "[hdbg] debug pass dest=0x%08X %ux%u step=%u\n", dest, w, h,
                         step);
        }
        return;
    }

    // The widescreen mode selects the `-wide` image when the pack has one. The
    // readback runs one scene early (the `0x02` loader), so the live aspect is
    // still 4:3 there and the setting, not the current `ar_option`, is the
    // signal.
    bool wide = false;
#if !defined(__EMSCRIPTEN__)
    wide = ogre::widescreen_mode() != ogre::WidescreenMode::Off;
#endif

    const Entry* match = find_entry(scene, next_scene, step);
    if (std::getenv("OGRE_BG_LOG") != nullptr) {
        std::fprintf(stderr,
                     "[hdbg] pass dest=0x%08X %ux%u step=%u scene=0x%02X next=0x%02X -> %s\n",
                     dest, w, h, step, scene, next_scene,
                     match != nullptr ? match->name.c_str() : "-");
    }
    if (match == nullptr) {
        return;
    }

    if (!known_chunk) {
        std::fprintf(stderr, "[hdbg] %s: no canvas rect for a %ux%u pass\n",
                     match->name.c_str(), w, h);
        return;
    }
    const size_t bytes = size_t(w) * h * sizeof(uint16_t);
    if (base < kKseg0 || uint64_t(base) + bytes > kRamEnd) {
        std::fprintf(stderr, "[hdbg] %s: destination 0x%08X out of range\n",
                     match->name.c_str(), base);
        return;
    }
    const bool use_wide = wide && !match->pixels_wide.empty();
    const std::vector<uint16_t>& art = use_wide ? match->pixels_wide : match->pixels;
    // Copy this pass's sub-rectangle out of the canvas.
    for (uint32_t row = 0; row < h; ++row) {
        const uint16_t* src = &art[size_t(cy + row) * kCanvasW + cx];
        uint32_t addr = base + row * w * 2;
        for (uint32_t col = 0; col < w; ++col, addr += 2) {
            store_u16(rdram, addr, src[col]);
        }
    }
    std::fprintf(stderr,
                 "[hdbg] applied '%s' (%s%s) step=%u chunk (%d,%d) %ux%u to 0x%08X\n",
                 match->name.c_str(), match->id.c_str(), use_wide ? " wide" : "", step, cx, cy,
                 w, h, base);
}
