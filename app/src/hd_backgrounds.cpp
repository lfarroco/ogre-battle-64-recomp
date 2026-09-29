// See hd_backgrounds.hpp for the pipeline and the call site.

#include "hd_backgrounds.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// A single-header decoder. RT64 compiles its own copy of stb_image into the
// renderer, so this one is `static` to keep the two from colliding at link
// time. It is the only reason the app needs the RT64 contrib include directory;
// the file is present for every build flavor, renderer or not.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb/stb_image.h"

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
    std::vector<uint16_t> pixels;  // kCanvasW x kCanvasH, guest-order RGBA5551
};

struct Pack {
    bool loaded = false;
    std::filesystem::path dir;
    std::vector<Entry> entries;
};

Pack g_pack;

int to_5bit(uint32_t c) { return int((c * 31u + 127u) / 255u); }

// Box filter the source to the backdrop canvas and convert to big-endian
// RGBA5551.
std::vector<uint16_t> to_canvas(const uint8_t* rgba, int sw, int sh) {
    constexpr int kW = kCanvasW;
    constexpr int kH = kCanvasH;
    std::vector<uint16_t> out(size_t(kW) * kH);
    for (int dy = 0; dy < kH; ++dy) {
        const int y0 = dy * sh / kH;
        const int y1 = std::max(y0 + 1, (dy + 1) * sh / kH);
        for (int dx = 0; dx < kW; ++dx) {
            const int x0 = dx * sw / kW;
            const int x1 = std::max(x0 + 1, (dx + 1) * sw / kW);
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = rgba + size_t(sy) * sw * 4;
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + size_t(sx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += p[3];
                    ++n;
                }
            }
            const uint32_t rr = to_5bit(r / n);
            const uint32_t gg = to_5bit(g / n);
            const uint32_t bb = to_5bit(b / n);
            out[size_t(dy) * kW + dx] =
                uint16_t((rr << 11) | (gg << 6) | (bb << 1) | 1u);  // opaque
        }
    }
    return out;
}

bool load_entry_image(Entry& e) {
    std::filesystem::path png = g_pack.dir / (e.id + ".png");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(png, ec)) {
        png = g_pack.dir / (e.name + ".png");
    }
    if (!std::filesystem::is_regular_file(png, ec)) {
        std::fprintf(stderr, "[hdbg] %s: no %s.png or %s.png\n", e.id.c_str(),
                     e.id.c_str(), e.name.c_str());
        return false;
    }
    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(png.string().c_str(), &w, &h, &comp, 4);
    if (data == nullptr) {
        std::fprintf(stderr, "[hdbg] %s: stb_image failed: %s\n", png.string().c_str(),
                     stbi_failure_reason());
        return false;
    }
    e.pixels = to_canvas(data, w, h);
    stbi_image_free(data);
    std::fprintf(stderr, "[hdbg] %-10s %-12s scene=0x%02X step=%-3u <- %s (%dx%d -> %dx%d)\n",
                 e.id.c_str(), e.name.c_str(), e.scene, e.step,
                 png.filename().string().c_str(), w, h, kCanvasW, kCanvasH);
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

void hd_backgrounds_init(const std::filesystem::path& pref_dir) {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("OGRE_BG_DIR"); env != nullptr && env[0] != '\0') {
        candidates.emplace_back(env);
    }
    candidates.push_back(pref_dir / "mods" / "backgrounds");
    candidates.push_back(pref_dir.parent_path() / "mods" / "backgrounds");
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "mods" / "backgrounds");
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

// Called by the generated njpeg readback (tools/hd_backgrounds.py) at the end of
// each pass's row copy.
extern "C" void ogre_hd_background(uint8_t* rdram) {
    if (!g_pack.loaded) {
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

    if (std::getenv("OGRE_BG_DEBUG") != nullptr) {
        if (debug_fill(rdram, base, w, h)) {
            std::fprintf(stderr, "[hdbg] debug pass dest=0x%08X %ux%u step=%u\n", dest, w, h,
                         step);
        }
        return;
    }

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

    int cx = 0, cy = 0;
    if (!chunk_rect(w, h, cx, cy)) {
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
    // Copy this pass's sub-rectangle out of the canvas.
    for (uint32_t row = 0; row < h; ++row) {
        const uint16_t* src = &match->pixels[size_t(cy + row) * kCanvasW + cx];
        uint32_t addr = base + row * w * 2;
        for (uint32_t col = 0; col < w; ++col, addr += 2) {
            store_u16(rdram, addr, src[col]);
        }
    }
    std::fprintf(stderr, "[hdbg] applied '%s' (%s) step=%u chunk (%d,%d) %ux%u to 0x%08X\n",
                 match->name.c_str(), match->id.c_str(), step, cx, cy, w, h, base);
}
