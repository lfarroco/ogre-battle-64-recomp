// Ogre Battle 64: Person of Lordly Caliber - RT64-based renderer.
//
// Replaces the bring-up null renderer. Wraps RT64's Application (Vulkan on
// Linux) behind ultramodern's RendererContext interface. Display lists are
// parsed by RT64's built-in GBI interpreters; OB64's ucode is F3DEX 2.08,
// which RT64 auto-detects from the OSTask's ucode data via loadUCodeGBI.
//
// Adapted from N64Recomp/RecompFrontend's rt64_render_context.cpp (minus the
// RecompFrontend UI / texture-pack / mod wiring).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "hle/rt64_application.h"
#include "shared/rt64_f3d_defines.h"

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "renderer.hpp"
#include "gbi.hpp"
#include "ram_key.hpp"
#include "sdl_platform.hpp"
#include "widescreen.hpp"

namespace ogre {

extern "C" uint8_t* ultramodern_get_rdram_base();

namespace {

// RSP DMEM/IMEM and the RDP FIFO registers RT64's Application::Core expects.
// The game's RSP task code is reimplemented by the runtime / RT64, so these
// are just static slots RT64 can read/write during display list processing.
uint8_t DMEM[0x1000];
uint8_t IMEM[0x1000];

uint32_t MI_INTR_REG = 0;
uint32_t DPC_START_REG = 0;
uint32_t DPC_END_REG = 0;
uint32_t DPC_CURRENT_REG = 0;
uint32_t DPC_STATUS_REG = 0;
uint32_t DPC_CLOCK_REG = 0;
uint32_t DPC_BUFBUSY_REG = 0;
uint32_t DPC_PIPEBUSY_REG = 0;
uint32_t DPC_TMEM_REG = 0;

uint8_t dummy_rom_header[0x40];

void dummy_check_interrupts() {}

// Create `path` and its parents, or do nothing if it already exists. POSIX
// `mkdir(path, mode)` has no Windows declaration (`<direct.h>` declares the
// one-argument `_mkdir`), and clang-cl rejects the call, so use the C++17 call
// the rest of the app already uses. The error_code overload does not throw on
// the EEXIST case that every dump-directory call hits.
void ensure_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

// --- ultramodern GraphicsConfig -> RT64 config mappers -------------------

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:   return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        default: return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original: return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:   return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:   return RT64::UserConfiguration::AspectRatio::Manual;
        default: return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:  return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:   return RT64::UserConfiguration::RefreshRate::Manual;
        default: return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:  return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:   return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        default: return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

RT64::EnhancementConfiguration::Presentation::Mode to_rt64(ultramodern::renderer::PresentationMode mode) {
    switch (mode) {
        case ultramodern::renderer::PresentationMode::Console:       return RT64::EnhancementConfiguration::Presentation::Mode::Console;
        case ultramodern::renderer::PresentationMode::SkipBuffering: return RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
        case ultramodern::renderer::PresentationMode::PresentEarly:  return RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    }
    return RT64::EnhancementConfiguration::Presentation::Mode::Console;
}



void set_application_user_config(RT64::Application* app, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            app->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Original;
            app->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original2x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    app->userConfig.aspectRatio = to_rt64(config.ar_option);
    app->userConfig.antialiasing = to_rt64(config.msaa_option);
    app->userConfig.refreshRate = to_rt64(config.rr_option);
    app->userConfig.refreshRateTarget = config.rr_manual_value;
    app->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

    // 2D rectangles sample at the console's pixel positions (RT64 `Original`).
    // RT64's default `ScaledOnly` upscales every `G_TEXRECT` whose dsdx/dtdy is
    // not exactly 1024 (`DrawCall::identityRectScale`). OB64 draws its dialogue
    // box as three rectangles stretched by 1026..1169/1024 over a 16x61 texel
    // window, and at a scaled resolution (3x here) the last row and column
    // sample one texel past the window: a broken row under the box's shadow and
    // a sliver beside the portrait. With `Original` the shader snaps each rectangle's UV to the
    // native pixel (`RasterPS.hlsl`, `lowResUV`). Triangles (3D, the cutscene
    // backdrop strips) are always upscaled, and HD replacement textures sample
    // at full resolution in either mode. `OGRE_UPSCALE_2D=scaled|all` restores
    // RT64's modes for an A/B.
    {
        auto mode = RT64::UserConfiguration::Upscale2D::Original;
        const char* name = "original";
        if (const char* env = getenv("OGRE_UPSCALE_2D")) {
            if (strcmp(env, "scaled") == 0) {
                mode = RT64::UserConfiguration::Upscale2D::ScaledOnly;
                name = "scaled";
            } else if (strcmp(env, "all") == 0) {
                mode = RT64::UserConfiguration::Upscale2D::All;
                name = "all";
            }
        }
        app->userConfig.upscale2D = mode;
        static const char* logged = nullptr;
        if (logged != name) {
            fprintf(stderr, "[renderer] 2D rectangles: upscale2D %s\n", name);
            logged = name;
        }
    }

    // One line per applied configuration: the SETTINGS tab's DISPLAY rows change
    // any of these at runtime, and this is what makes the value a run actually
    // used readable from its log (the rows themselves only show the panel's
    // copy).
    // The names follow `ultramodern::renderer::Resolution` (Original, Original2x,
    // Auto), not RT64's own enum order.
    static const char* const kResolutionNames[] = {"native", "2x", "auto"};
    static const char* const kAntialiasNames[] = {"off", "2x", "4x", "8x"};
    static const char* const kWindowNames[] = {"windowed", "fullscreen"};
    const int resolution_index = static_cast<int>(config.res_option);
    const int antialias_index = static_cast<int>(config.msaa_option);
    const int window_index = static_cast<int>(config.wm_option);
    fprintf(stderr, "[renderer] config: resolution %s (mult %.2f), msaa %s, window %s\n",
            resolution_index >= 0 && resolution_index < 3 ? kResolutionNames[resolution_index]
                                                          : "?",
            static_cast<double>(app->userConfig.resolutionMultiplier),
            antialias_index >= 0 && antialias_index < 4 ? kAntialiasNames[antialias_index] : "?",
            window_index >= 0 && window_index < 2 ? kWindowNames[window_index] : "?");
    fflush(stderr);
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult result) {
    switch (result) {
        case RT64::Application::SetupResult::Success:                   return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:       return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:      return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    fprintf(stderr, "Unhandled RT64::Application::SetupResult\n");
    return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:     return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:    return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:     return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic: return ultramodern::renderer::GraphicsApi::Auto;
        default: break;
    }
    fprintf(stderr, "Unhandled RT64::UserConfiguration::GraphicsAPI\n");
    return ultramodern::renderer::GraphicsApi::Auto;
}

// --- OGRE_NOP_RECT: neutralise selected G_TEXRECT commands ----------------
//
// Bisect aid for "which draw produces this rectangle on screen". The submitted
// display list is patched in RDRAM before RT64 parses it: every G_TEXRECT that
// matches one of the selectors is replaced (together with its RDPHALF_1 /
// RDPHALF_2 pair) by G_SPNOOP, so the draw disappears and the layers under it
// show through. Selectors (comma separated):
//
//   any           every texture rectangle
//   tex:<hex>     the SETTIMG address in effect (e.g. tex:801E2918)
//   flip          dsdx < 0 (a right-to-left rectangle)
//   y:<a>-<b>     the rectangle's screen rows overlap [a,b)
//   x:<a>-<b>     the rectangle's screen columns overlap [a,b)
//   n:<index>     the nth texture rectangle of the list
//
// Every skipped rectangle is logged with its decoded parameters so a run says
// exactly what was removed.
// Defined after nop_rect's walker (which drives it): repair one stale-tile
// fog group. See the OGRE_FOG comment below.
namespace fog {
uint32_t repair(uint8_t* rdram, uint32_t layer, uint32_t settimg, uint32_t settile7,
                uint32_t loadtile, uint32_t settile0, uint32_t tilesize, uint32_t uls, uint32_t ult,
                uint32_t lrs, uint32_t lrt, int rect_s, int rect_t, bool verbose);
}

namespace nop_rect {

struct Selector {
    enum Kind { Any, Tex, Flip, Rows, Cols, Index } kind;
    uint32_t value = 0;
    int lo = 0, hi = 0;
};

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1u);
}

inline void write_op(uint8_t* rdram, uint32_t off, uint8_t op) {
    uint32_t w = rd32(rdram + off);
    w = (w & 0x00FFFFFFu) | (uint32_t(op) << 24);
    std::memcpy(rdram + off, &w, sizeof(w));
}

struct Walker {
    uint8_t* rdram;
    const std::vector<Selector>* selectors;
    bool verbose = false;
    uint32_t segments[16] = {};
    uint32_t index = 0;
    uint32_t removed = 0;
    uint32_t budget = 200000;
    // OGRE_FOG: when set, the walker also repairs OB64's stale-tile fog pieces
    // (see the fog namespace): the fog layer's own image replaces the texture
    // setup the rectangle inherits.
    bool fog_fix = false;
    uint32_t fog_fixed = 0;
    uint32_t fog_layer = 0;
    uint32_t fog_settimg = 0, fog_settile7 = 0, fog_loadtile = 0, fog_settile0 = 0,
             fog_tilesize = 0;
    uint32_t fog_uls = 0, fog_ult = 0, fog_lrs = 0, fog_lrt = 0;
    bool fog_white = false;
    uint32_t fog_last_image = 0;
    uint32_t fog_last_layer = 0;
    uint32_t fog_dl = 0;             // display-list number, for traces
    uint32_t fog_group_index = 0;    // white-group ordinal within this list
    uint32_t fog_white_groups = 0;   // white-combiner groups seen
    uint32_t fog_zero_groups = 0;    // ... whose inherited tile covers no texels

    uint32_t resolve(uint32_t addr) const {
        return (segments[(addr >> 24) & 0xFu] + (addr & 0x00FFFFFFu)) & 0x1FFFFFFFu;
    }

    bool matches(const Selector& s, uint32_t timg, int16_t dsdx, int x0, int y0, int x1,
                 int y1) const {
        switch (s.kind) {
            case Selector::Any:  return true;
            case Selector::Tex:  return timg == s.value;
            case Selector::Flip: return dsdx < 0;
            case Selector::Rows: return (y0 < s.hi) && (y1 > s.lo);
            case Selector::Cols: return (x0 < s.hi) && (x1 > s.lo);
            case Selector::Index: return index == s.value;
        }
        return false;
    }

    void walk(uint32_t off, int depth) {
        if (depth > 32) {
            return;
        }
        uint32_t timg = 0;
        for (;;) {
            if (budget == 0 || off + 8 > 0x20000000u) {
                return;
            }
            --budget;
            const uint8_t* p = rdram + off;
            const uint32_t w0 = rd32(p);
            const uint32_t w1 = rd32(p + 4);
            const uint8_t op = uint8_t(w0 >> 24);

            if (op == 0xFD) {  // G_SETTIMG
                timg = w1;
                if (fog_fix) {
                    fog_settimg = off;
                }
                off += 8;
                continue;
            }
            if (fog_fix) {
                if (op == 0xF5) {  // G_SETTILE
                    if (p0(w1, 24, 3) == 7) {
                        fog_settile7 = off;
                    } else if (p0(w1, 24, 3) == 0) {
                        fog_settile0 = off;
                    }
                } else if (op == 0xF4 || op == 0xF3) {  // G_LOADTILE / G_LOADBLOCK
                    fog_loadtile = off;
                } else if (op == 0xF2) {  // G_SETTILESIZE
                    if (p0(w1, 24, 3) == 0) {
                        fog_tilesize = off;
                        fog_uls = p0(w0, 12, 12);
                        fog_ult = p0(w0, 0, 12);
                        fog_lrs = p0(w1, 12, 12);
                        fog_lrt = p0(w1, 0, 12);
                    }
                } else if (op == 0xFC) {  // G_SETCOMBINE
                    fog_white = (w1 == 0xFFFF73B9u);
                } else if (op == 0xFD) {  // G_SETTIMG (handled just below too)
                    fog_settimg = off;
                }
            }
            if (op == 0xDB) {  // G_MOVEWORD: segment registers
                if (((w0 >> 16) & 0xFF) == 0x06) {
                    segments[(w0 >> 2) & 0xF] = w1;
                }
                off += 8;
                continue;
            }
            if (op == 0xDE) {  // G_DL
                const uint32_t target = resolve(w1);
                const bool push = ((w0 >> 16) & 1) == 0;
                if (push) {
                    walk(target, depth + 1);
                    off += 8;
                } else {
                    off = target;
                }
                continue;
            }
            if (op == 0xDF) {  // G_ENDDL
                return;
            }
            if (op == 0xE4 || op == 0xE5) {  // G_TEXRECT / G_TEXRECTFLIP
                const int16_t uls = int16_t(p0(rd32(rdram + off + 8 + 4), 16, 16));
                const int16_t ult = int16_t(p0(rd32(rdram + off + 8 + 4), 0, 16));
                const int16_t dsdx = int16_t(p0(rd32(rdram + off + 16 + 4), 16, 16));
                (void)uls;
                (void)ult;
                const int x0 = int(p0(w1, 12, 12)) / 4, y0 = int(p0(w1, 0, 12)) / 4;
                const int x1 = int(p0(w0, 12, 12)) / 4, y1 = int(p0(w0, 0, 12)) / 4;
                bool skip = false;
                for (const Selector& s : *selectors) {
                    if (matches(s, timg, dsdx, x0, y0, x1, y1)) {
                        skip = true;
                        break;
                    }
                }
                if (fog_fix && fog_white) {
                    ++fog_white_groups;
                    if (fog_lrs == fog_uls || fog_lrt == fog_ult) {
                        ++fog_zero_groups;
                    }
                    const int rect_s = int16_t(p0(rd32(rdram + off + 12), 16, 16)) / 32;
                    const int rect_t = int16_t(p0(rd32(rdram + off + 12), 0, 16)) / 32;
                    const uint32_t used_image =
                        fog::repair(rdram, fog_layer, fog_settimg, fog_settile7, fog_loadtile,
                                    fog_settile0, fog_tilesize, fog_uls, fog_ult, fog_lrs, fog_lrt,
                                    rect_s, rect_t, verbose);
                    if (getenv("OGRE_FOG_SUMMARY") != nullptr) {
                        fprintf(stderr,
                                "[fog-group] dl=%u n=%u s=%d t=%d uls=%u ult=%u lrs=%u lrt=%u "
                                "layer=%u used=0x%08X\n",
                                fog_dl, fog_group_index, rect_s, rect_t, fog_uls, fog_ult,
                                fog_lrs, fog_lrt, fog_layer, used_image);
                        ++fog_group_index;
                    }
                    if (used_image != 0) {
                        ++fog_fixed;
                        fog_last_image = used_image;
                        fog_last_layer = fog_layer;
                        fog_layer = (fog_layer == 0) ? 2 : fog_layer + 1;
                    }
                    fog_white = false;  // the combiner applies to one group
                }
                if (skip && off + 24 <= 0x20000000u) {
                    if (verbose) {
                        fprintf(stderr,
                                "[nop-rect] removed #%u @0x%05X x %d..%d y %d..%d dsdx=%d tex=0x%08X\n",
                                index, off, x0, x1, y0, y1, (int)dsdx, timg);
                    }
                    write_op(rdram, off, 0xE0);
                    write_op(rdram, off + 8, 0xE0);
                    write_op(rdram, off + 16, 0xE0);
                    ++removed;
                }
                ++index;
                off += 24;
                continue;
            }
            // G_BRANCH_Z is the only 16-byte command in F3DEX2.
            off += (op == 0x04) ? 16 : 8;
        }
    }
};

inline std::vector<Selector> parse(const char* spec) {
    std::vector<Selector> out;
    std::string s(spec);
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        std::string tok = s.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? s.size() + 1 : comma + 1;
        if (tok.empty()) {
            continue;
        }
        Selector sel;
        if (tok == "any") {
            sel.kind = Selector::Any;
        } else if (tok == "flip") {
            sel.kind = Selector::Flip;
        } else if (tok.rfind("tex:", 0) == 0) {
            sel.kind = Selector::Tex;
            sel.value = (uint32_t)strtoul(tok.c_str() + 4, nullptr, 16);
        } else if (tok.rfind("n:", 0) == 0) {
            sel.kind = Selector::Index;
            sel.value = (uint32_t)strtoul(tok.c_str() + 2, nullptr, 10);
        } else if ((tok.rfind("y:", 0) == 0) || (tok.rfind("x:", 0) == 0)) {
            sel.kind = (tok[0] == 'y') ? Selector::Rows : Selector::Cols;
            const char* rest = tok.c_str() + 2;
            sel.lo = (int)strtol(rest, (char**)&rest, 10);
            if (*rest == '-') {
                sel.hi = (int)strtol(rest + 1, nullptr, 10);
            }
        } else {
            fprintf(stderr, "[nop-rect] unknown selector '%s' (ignored)\n", tok.c_str());
            continue;
        }
        out.push_back(sel);
    }
    return out;
}

}  // namespace nop_rect

// --- OGRE_TEX_TRACE: report what every textured rectangle samples ----------
//
// Diagnostic for "one part of a sprite draws as a black rectangle while the
// rest of the sprite is correct". The submitted display list is walked before
// RT64 parses it, and every G_TEXRECT is reported with the rectangle it covers
// and the texel window it reads. RDRAM is then sampled at the address the
// window resolves to and the bytes are summarised, so the run says whether the
// source data is black, whether the sample landed outside the intended image,
// or whether the palette is wrong.
//
// The source address of a rectangle depends on the format in effect, so each
// line carries the whole state (SETTIMG format/size/width/address, the tile's
// line and palette, the SETTILESIZE window, and the rectangle's s/t). The
// bytes are written beside the line and `tools/ogre_tex_uv.py` decodes both
// offline. Nothing here decides what is correct.
//
// Knobs:
//   OGRE_TEX_TRACE=1           one line per textured rectangle
//   OGRE_TEX_TRACE_DUMP=<dir>  also write the sampled RDRAM to <dir>/tex/
//   OGRE_TEX_TRACE_DRAW=<n>    report only the nth rectangle of the run
//   OGRE_TEX_TRACE_MAX=<n>     stop after n reported rectangles (default 20000)
namespace tex_trace {

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1u);
}

struct State {
    uint8_t* rdram = nullptr;
    std::string dump_dir;
    uint32_t only_draw = 0;    // 0 = every rectangle
    uint32_t max_draws = 20000;
    uint32_t min_px = 0;       // skip rectangles narrower and shorter than this
    uint32_t dl = 0;
    uint32_t segments[16] = {};
    // SETTIMG
    uint32_t timg = 0, timg_fmt = 0, timg_siz = 0, timg_width = 0;
    // SETTILE, tile 0
    uint32_t tile_line = 0, tile_palette = 0;
    // SETTILESIZE, tile 0
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;
    // LOADTLUT, tile 0
    uint32_t tlut_addr = 0, tlut_count = 0;
    bool tlut_seen = false;
    bool trace_all_images = false;   // report every SETTIMG, not only TEXRECT sources
    // OGRE_RAM_KEY_ON_BLACK: dump the whole RDRAM image on the first black draw,
    // so a per-pose fault is captured without a key press.
    bool dump_on_black = false;
    uint32_t index = 0;        // every rectangle of the run
    uint32_t reported = 0;
    bool truncated = false;
    // OGRE_TEX_TRACE_COLOUR: decode the sampled window through its palette and
    // report the resulting colour, so a draw that samples a correct cutout and
    // paints it black is visible in the log without a screenshot (session 131).
    bool colour = false;
    uint32_t colour_blackness = 0;   // report a draw only at or below this mean
    uint32_t black_draws = 0;
    bool reported_black = false;
    uint32_t dumps_written = 0;      // file pairs written by dump()
    bool dumps_capped = false;

    // The RDP's own byte order for a sampled image: the trace samples RDRAM the
    // way the renderer does, so the logical byte at `a` is `rdram[(a ^ 3)]`.
    uint8_t image_byte(uint32_t addr) const {
        return rdram[(addr & 0x1FFFFFFFu) ^ 3u];
    }
    uint16_t image_half(uint32_t addr) const {
        const uint32_t o = addr & 0x1FFFFFFEu;
        return uint16_t(rdram[o ^ 2u] | (rdram[(o + 1) ^ 2u] << 8));
    }

    uint32_t resolve(uint32_t addr) const {
        return (segments[(addr >> 24) & 0xFu] + (addr & 0x00FFFFFFu)) & 0x1FFFFFFFu;
    }

    // Bytes per image row, from the format and the image width.
    uint32_t bytes_per_row() const {
        const uint32_t w = timg_width + 1;
        switch (timg_siz) {
            case 0: return (w + 1) / 2;   // 4b
            case 1: return w;             // 8b
            case 2: return w * 2;         // 16b
            default: return w * 4;        // 32b
        }
    }

    // A cheap fingerprint of an image's data: sample count, non-zero count and
    // a sum, so the log alone says which of the bound images is all zero
    // (the black-part case) without a per-frame dump of every image.
    void fingerprint(uint32_t addr, uint32_t count, const char* tag) {
        const uint32_t off = addr & 0x1FFFFFFFu;
        if (count == 0 || off + count > 0x800000u) {
            std::fprintf(stderr, "[tex]   %s: 0x%08X+%u outside RDRAM\n", tag, addr, count);
            return;
        }
        const uint8_t* p = rdram + off;
        uint32_t nonzero = 0, sum = 0, samples = 0;
        const uint32_t step = (count > 1024) ? (count / 1024) : 1;
        for (uint32_t i = 0; i < count; i += step) {
            sum += p[i];
            if (p[i] != 0) {
                ++nonzero;
            }
            ++samples;
        }
        std::fprintf(stderr, "[tex]   %s: %u bytes, nonZero %u/%u, mean %.1f%s\n", tag, count,
                     nonzero, samples, samples ? double(sum) / double(samples) : 0.0,
                     (nonzero == 0) ? "   <-- ALL ZERO" : "");
    }

    void dump(const std::string& tag, uint32_t addr, uint32_t count, uint32_t fmt, uint32_t siz,
              uint32_t width, uint32_t y, const char* what, const char* note) {
        if (dump_dir.empty() || count == 0 || count > 0x200000u) {
            return;
        }
        // A capture that runs to the window close reports tens of thousands of
        // draws, and one file pair per draw is tens of thousands of files: the
        // capture then stalls on file creation, which looks like a frozen game
        // (session 131: `OGRE_TEX_TRACE_MS=0` plus a dump directory). Cap it and
        // say so, once; the log lines are unaffected.
        constexpr uint32_t kMaxDumpFiles = 4000;
        if (dumps_written >= kMaxDumpFiles) {
            if (!dumps_capped) {
                dumps_capped = true;
                std::fprintf(stderr,
                             "[tex] dump limit %u file(s) reached; the [tex] lines continue, "
                             "the per-draw .bin/.json files stop\n",
                             kMaxDumpFiles);
                std::fflush(stderr);
            }
            return;
        }
        const uint32_t off = addr & 0x1FFFFFFFu;
        if (off + count > 0x800000u) {
            return;
        }
        char base[64];
        std::snprintf(base, sizeof(base), "%06u_%s", index, tag.c_str());
        const std::string dir = dump_dir + "/tex";
        ensure_dir(dump_dir);
        ensure_dir(dir);
        if (FILE* f = std::fopen((dir + "/" + base + ".bin").c_str(), "wb")) {
            std::fwrite(rdram + off, 1, count, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((dir + "/" + base + ".json").c_str(), "w")) {
            std::fprintf(f,
                         "{\"what\":\"%s\",\"note\":\"%s\",\"rdram\":%u,\"count\":%u,\"fmt\":%u,"
                         "\"siz\":%u,\"width\":%u,\"line\":%u,\"palette\":%u,\"tlut\":%u,"
                         "\"tlut_rdram\":%u,\"dl\":%u,\"draw\":%u,\"y\":%u}\n",
                         what, note, off, count, fmt, siz, width, tile_line, tile_palette,
                         tlut_seen ? 1u : 0u, tlut_seen ? (tlut_addr & 0x1FFFFFFFu) : 0u, dl, index,
                         y);
            std::fclose(f);
        }
        ++dumps_written;
    }

    // The colour a draw paints, from the bytes it samples. This is the half the
    // byte summary cannot show: a CI draw whose indices are a correct cutout and
    // whose palette is black samples non-zero bytes and still paints black.
    //
    // Returns a mean 0..255 over the sampled texels, and sets `opaque` to the
    // count of texels whose palette entry has alpha 1. Only the formats the game
    // uses are decoded (CI8 through the loaded TLUT, RGBA16 directly); any
    // other format reports `known = false` and no mean.
    //
    // CI8 is `siz = 1`, one index byte per texel, and it is the format of every
    // publisher still, the title's cloud layers and the njpeg output, so the
    // guard below admits it. CI4 (`siz = 0`, two indices per byte) is not
    // decoded: no screen has been found where the colour of a CI4 draw had to be
    // known, and the nibble order is one more thing to get wrong silently.
    bool colour_mean(uint32_t img, uint32_t t_a, uint32_t s_a, uint32_t tex_w, uint32_t tex_h,
                     double& mean, uint32_t& opaque) {
        mean = 0.0;
        opaque = 0;
        const bool indexed = (timg_fmt == 2 || timg_fmt == 3);
        if (!indexed && timg_fmt != 0) {
            return false;   // only RGBA16 is decoded for the direct formats
        }
        if (indexed ? (timg_siz != 1) : (timg_siz != 2)) {
            return false;   // CI8 indices, or 16-bit texels
        }
        if (indexed && !tlut_seen) {
            return false;   // no palette has been loaded yet
        }
        const uint32_t bpr = bytes_per_row();
        if (bpr == 0 || tex_w == 0 || tex_h == 0) {
            return false;
        }
        // Bound the work and the reads. A rectangle's window is 12 bits of texel
        // per axis, so a wild s/t can ask for millions of texels; every address
        // is also checked against RDRAM because a bad `timg`/`tlut` would read
        // past the image (the walker trusts the list, so this sampler cannot).
        constexpr uint32_t kMaxTexels = 0x10000;   // 65536 texels per draw
        const uint32_t texels = tex_w * tex_h;
        if (texels > kMaxTexels) {
            return false;
        }
        const uint32_t pal = indexed ? resolve(tlut_addr) : 0;
        uint64_t sum = 0;
        uint32_t n = 0;
        for (uint32_t ty = 0; ty < tex_h; ++ty) {
            for (uint32_t tx = 0; tx < tex_w; ++tx) {
                const uint32_t row = (t_a + ty) * bpr;
                uint16_t rgb;
                if (indexed) {
                    const uint32_t idx_addr = img + row + (s_a + tx);
                    if ((idx_addr & 0x1FFFFFFFu) >= 0x800000u) {
                        return false;
                    }
                    const uint32_t idx = image_byte(idx_addr);
                    const uint32_t rgb_addr = pal + idx * 2u;
                    if ((rgb_addr & 0x1FFFFFFFu) + 1u >= 0x800000u) {
                        return false;
                    }
                    rgb = image_half(rgb_addr);
                } else {
                    const uint32_t rgb_addr = img + row + (s_a + tx) * 2u;
                    if ((rgb_addr & 0x1FFFFFFFu) + 1u >= 0x800000u) {
                        return false;
                    }
                    rgb = image_half(rgb_addr);
                }
                if (rgb & 1u) {
                    ++opaque;
                }
                sum += uint32_t((rgb >> 11) & 31u) + uint32_t((rgb >> 6) & 31u) +
                       uint32_t((rgb >> 1) & 31u);
                ++n;
            }
        }
        if (n == 0) {
            return false;
        }
        // Three 5-bit channels per texel, so the 0..255 scale is 255/(3*31).
        mean = double(sum) / double(n) * (255.0 / (3.0 * 31.0));
        return true;
    }

    // A summary of the bytes at `addr`, to separate "the source is black" from
    // "the source is fine and the rectangle reads the wrong place".
    void summarise(uint32_t addr, uint32_t count, const char* tag) {
        if (count == 0 || count > 0x200000u) {
            std::fprintf(stderr, "[tex]   %s: out-of-range count %u\n", tag, count);
            return;
        }
        const uint32_t off = addr & 0x1FFFFFFFu;
        if (off + count > 0x800000u) {
            std::fprintf(stderr, "[tex]   %s: 0x%08X+%u leaves RDRAM\n", tag, addr, count);
            return;
        }
        const uint8_t* p = rdram + off;
        uint32_t nonzero = 0, sum = 0, distinct = 0, samples = 0;
        uint8_t seen[16] = {};
        const uint32_t step = (count > 512) ? (count / 512) : 1;
        for (uint32_t i = 0; i < count; i += step) {
            const uint8_t b = p[i];
            sum += b;
            if (b != 0) {
                ++nonzero;
            }
            bool known = false;
            for (uint32_t s = 0; s < distinct; ++s) {
                if (seen[s] == b) {
                    known = true;
                    break;
                }
            }
            if (!known && distinct < 16) {
                seen[distinct++] = b;
            }
            ++samples;
        }
        std::fprintf(stderr, "[tex]   %s: raw=%u nonZero=%u/%u mean=%.1f distinct=%u first16=", tag,
                     count, nonzero, samples, samples ? double(sum) / double(samples) : 0.0,
                     distinct);
        for (uint32_t i = 0; i < 16 && i < count; ++i) {
            std::fprintf(stderr, "%02x", p[i]);
        }
        std::fprintf(stderr, "\n");
    }

    // The 24-byte G_TEXRECT, as `gbi.cpp`'s own decoder reads it:
    //   +0  w0  = lower-right  (x in bits 12..23, y in bits 0..11, quarter px)
    //   +4  w1  = upper-left   (same packing) and the tile in bits 24..26
    //   +8  RDPHALF_1 (its second word = s in bits 16..31, t in bits 0..15)
    //   +16 RDPHALF_2 (its second word = dsdx in bits 16..31, dtdy in bits 0..15)
    void rect(uint32_t off, const uint8_t* q, uint32_t w0, uint32_t w1, bool flip) {
        const uint32_t st_w = rd32(q + 12);
        const uint32_t ds_w = rd32(q + 20);
        const int32_t s = int16_t(p0(st_w, 16, 16));
        const int32_t t = int16_t(p0(st_w, 0, 16));
        const int32_t dsdx = int16_t(p0(ds_w, 16, 16));
        const int32_t dtdy = int16_t(p0(ds_w, 0, 16));
        const int x0 = int(p0(w1, 12, 12)) / 4, y0 = int(p0(w1, 0, 12)) / 4;
        const int x1 = int(p0(w0, 12, 12)) / 4, y1 = int(p0(w0, 0, 12)) / 4;
        const int px_w = (x1 > x0) ? (x1 - x0) : (x0 - x1);
        const int px_h = (y1 > y0) ? (y1 - y0) : (y0 - y1);

        if (only_draw != 0 && only_draw != index) {
            ++index;
            return;
        }
        // OGRE_TEX_TRACE_MIN_PX: skip small rectangles (the font's 11x14 glyphs
        // are the bulk of a frame and never the suspect part).
        if (min_px != 0 && uint32_t(px_w) < min_px && uint32_t(px_h) < min_px) {
            ++index;
            return;
        }
        if (reported >= max_draws) {
            truncated = true;
            return;
        }

        // The texel window the rectangle walks, in whole texels. s and t are in
        // 1/32 texel, dsdx and dtdy are the change per pixel in 1/1024 texel.
        const int32_t s_end = s + (dsdx * px_w) / 32;
        const int32_t t_end = t + (dtdy * px_h) / 32;
        const uint32_t s_a = uint32_t(s) / 32, s_b = uint32_t(s_end) / 32;
        const uint32_t t_a = uint32_t(t) / 32, t_b = uint32_t(t_end) / 32;
        const uint32_t tex_w = (s_b > s_a ? s_b - s_a : s_a - s_b) + 1;
        const uint32_t tex_h = (t_b > t_a ? t_b - t_a : t_a - t_b) + 1;

        std::fprintf(stderr,
                     "[tex] dl=%u n=%u %s rect=%d,%d..%d,%d px=%dx%d s=%d t=%d dsdx=%d dtdy=%d "
                     "window=uls%u ult%u lrs%u lrt%u tile=line%u pal%u timg=0x%08X "
                     "fmt=%u siz=%u imgw=%u texels=%ux%u\n",
                     dl, index, flip ? "TEXRECTFLIP" : "TEXRECT", x0, y0, x1, y1, px_w, px_h, s, t,
                     dsdx, dtdy, uls, ult, lrs, lrt, tile_line, tile_palette, timg, timg_fmt,
                     timg_siz, timg_width + 1, tex_w, tex_h);

        // The bytes the draw reads, addressed the same way for every format.
        if (!dump_dir.empty() && timg != 0) {
            const uint32_t bpr = bytes_per_row();
            const uint32_t indexed = (timg_fmt == 2 || timg_fmt == 3) ? 1u : 0u;
            const uint32_t img = resolve(timg);
            // A generous window: the image rows the window covers plus margin,
            // capped so a wild tile does not dump the whole RDRAM.
            uint32_t rows = tex_h;
            if (((lrt - ult) >> 4) + 1 > rows) {
                rows = ((lrt - ult) >> 4) + 1;
            }
            uint32_t count = bpr * (rows + 2);
            if (count > 0x40000u) {
                count = 0x40000u;
            }
            char note[256];
            std::snprintf(note, sizeof(note),
                          "window=%u,%u,%u,%u start=%d,%d end=%d,%d rect=%d,%d..%d,%d",
                          uls, ult, lrs, lrt, s_a, t_a, s_b, t_b, x0, y0, x1, y1);
            dump("src", img, count, timg_fmt, timg_siz, timg_width, t_a, "src", note);
            summarise(img, count, "src");
            if (indexed && tlut_seen) {
                dump("pal", resolve(tlut_addr), tlut_count, 2, 2, tlut_count / 2, 0, "pal",
                     "the palette the LOADTLUT before this draw loaded");
                summarise(resolve(tlut_addr), tlut_count, "pal");
            }
        }

        // OGRE_TEX_TRACE_COLOUR: the colour this draw paints. A draw that samples
        // a correct cutout and paints it black has non-zero source bytes and a
        // black mean, which is the report both earlier captures could not make.
        if (colour && timg != 0) {
            double mean = 0.0;
            uint32_t opaque = 0;
            if (colour_mean(resolve(timg), t_a, s_a, tex_w, tex_h, mean, opaque)) {
                // Only a draw with a real opaque area counts: a thin rule, or a
                // window of transparent texels, is not a black sprite part. The
                // report carries the opaque count so the two are distinguishable.
                const bool black = (opaque >= 32) && (mean <= double(colour_blackness));
                if (black) {
                    ++black_draws;
                }
                std::fprintf(stderr,
                             "[tex]   colour: mean %.1f/255 over %ux%u texels, opaque %u%s\n",
                             mean, tex_w, tex_h, opaque, black ? "   <-- BLACK" : "");
                if (black && !reported_black) {
                    reported_black = true;
                    std::fprintf(stderr,
                                 "[tex] BLACK DRAW: dl=%u n=%u rect=%d,%d..%d,%d timg=0x%08X "
                                 "fmt=%u line=%u tlut=%s texels=%ux%u opaque=%u mean=%.1f\n",
                                 dl, index, x0, y0, x1, y1, timg, timg_fmt, tile_line,
                                 tlut_seen ? "yes" : "no", tex_w, tex_h, opaque, mean);
                    std::fflush(stderr);
                }
                // OGRE_RAM_KEY_ON_BLACK=1: take a whole-RDRAM image on the
                // first black draw, with no key press. A per-pose fault cannot
                // be bracketed by hand -- three captures taken that way missed
                // the frame -- and this fires on the renderer thread while the
                // game thread is parked on the RSP task, so the image is
                // coherent. `session 132` numbers the files.
                if (black && dump_on_black) {
                    static int auto_serial = 0;
                    ++auto_serial;
                    ram_key_dump_full(rdram, auto_serial);
                }
            }
        }
        ++reported;
        ++index;
    }

    void walk(uint32_t off, int depth) {
        if (depth > 32 || truncated) {
            return;
        }
        for (;;) {
            if (truncated || off + 8 > 0x20000000u) {
                return;
            }
            const uint8_t* q = rdram + off;
            const uint32_t w0 = rd32(q);
            const uint32_t w1 = rd32(q + 4);
            const uint8_t op = uint8_t(w0 >> 24);

            switch (op) {
                case gbi::OP_SETTIMG:  // 0xFD
                    timg_fmt = p0(w0, 21, 3);
                    timg_siz = p0(w0, 19, 2);
                    timg_width = p0(w0, 0, 12);
                    timg = w1;
                    // Every image the frame binds, whether or not it ends up in a
                    // G_TEXRECT. A 3D character's texture is bound here and drawn
                    // by triangles, so this is the only place a TEXRECT-only
                    // report would miss it.
                    if (trace_all_images) {
                        // One line per image per capture, not per bind: the game
                        // re-binds the same image every frame and the data is
                        // static for the frame. Tracked by address and size.
                        static std::vector<uint64_t> seen_images;
                        const uint64_t key = (uint64_t(resolve(timg)) << 16) | (timg_width & 0xFFFF);
                        bool first = true;
                        for (uint64_t k : seen_images) {
                            if (k == key) { first = false; break; }
                        }
                        if (first && seen_images.size() < 4096) {
                            seen_images.push_back(key);
                        }
                        if (first) {
                        std::fprintf(stderr,
                                     "[tex] dl=%u SETTIMG img=0x%08X raw=0x%08X fmt=%u siz=%u "
                                     "width=%u\n",
                                     dl, resolve(timg), timg, timg_fmt, timg_siz, timg_width + 1);
                        // 32 rows covers the largest character part; the count is
                        // capped so a wild bind cannot read the whole RDRAM.
                        const uint32_t fp_bpr = bytes_per_row();
                        uint32_t fp = fp_bpr * 32;
                        if (fp == 0 || fp > 0x4000u) {
                            fp = 0x4000u;
                        }
                        if (fp_bpr != 0) {
                            fingerprint(resolve(timg), fp, "img");
                        }
                        }
                    }
                    off += 8;
                    continue;
                case gbi::OP_SETTILE:  // 0xF5
                    if (p0(w1, 24, 3) == 0) {
                        tile_line = p0(w0, 9, 9);
                        tile_palette = p0(w1, 20, 4);
                    }
                    off += 8;
                    continue;
                case gbi::OP_SETTILESIZE:  // 0xF2
                    if (p0(w1, 24, 3) == 0) {
                        uls = p0(w0, 12, 12);
                        ult = p0(w0, 0, 12);
                        lrs = p0(w1, 12, 12);
                        lrt = p0(w1, 0, 12);
                    }
                    off += 8;
                    continue;
                case gbi::OP_LOADTLUT: {  // 0xF0, every tile: the palette may be
                                          // loaded for a tile the draw does not name
                    const uint32_t tile = p0(w1, 24, 3);
                    const uint32_t addr = timg + 2 * p0(w0, 14, 10);
                    const uint32_t count = (p0(w1, 14, 10) + 1) * 2;
                    if (!tlut_seen) {
                        tlut_addr = addr;
                        tlut_count = count;
                        tlut_seen = true;
                    }
                    if (dump_dir.empty() == false) {
                        std::fprintf(stderr, "[tex]   LOADTLUT t%u addr=0x%08X count=%u\n",
                                     tile, addr, count);
                    }
                    off += 8;
                    continue;
                }
                case gbi::OP_MOVEWORD:  // 0xDB, segment registers
                    if (((w0 >> 16) & 0xFF) == 0x06) {
                        segments[(w0 >> 2) & 0xF] = w1;
                    }
                    off += 8;
                    continue;
                case gbi::OP_DL: {  // 0xDE
                    const uint32_t target = resolve(w1);
                    if (((w0 >> 16) & 1) == 0) {
                        walk(target, depth + 1);
                        off += 8;
                    } else {
                        off = target;
                    }
                    continue;
                }
                case gbi::OP_ENDDL:  // 0xDF
                    return;
                case 0xE4:      // G_TEXRECT
                case 0xE5:      // G_TEXRECTFLIP
                    rect(off, q, w0, w1, op == 0xE5);
                    if (truncated) {
                        return;
                    }
                    off += 24;
                    continue;
                default:
                    break;
            }
            off += (op == 0x04) ? 16 : 8;  // G_BRANCH_Z is the only 16-byte command
        }
    }
};

}  // namespace tex_trace

// --- OGRE_FOG: repair the stale-tile fog pieces ---------------------------
//
// OB64 draws each scrolling fog/cloud layer as a wrapped "layer image": a set
// of `G_TEXRECT`s that mirror-sample a 64x64 image with the *white* combiner
// (`FCFFFFFF FFFF73B9` = RGB ONE, ALPHA TEXEL0), so the image's own intensity
// becomes a faint white overlay. The draw assumes the layer's image has been
// uploaded into TMEM by the layer's loop, but that loop leaves a tile behind
// that covers **no texels** (`SETTILESIZE t0 uls=0 ult=420 lrs=1276 lrt=420`
// at the end of the 105-row cloud layer), so the sample collapses onto one
// stale row - and RT64 paints an opaque white band instead of the fog (the
// band this port showed on the title screen until session 35).
//
// The intended image is reachable: the game keeps its layer table at
// `D_801B80E0`; `+0xFC + i*0x24` is layer `i`'s record, whose first word
// points at an image record (`+8` = the image data; height above it, then
// width, then the siz/fmt code bytes). This pass rewrites the texture setup
// the rectangle inherits (the last `SETTIMG` / `SETTILE t7` / `LOADTILE` /
// `SETTILE t0` / `SETTILESIZE t0` before it) so the pieces sample the layer's
// own image. `OGRE_FOG=0` disables it; `OGRE_FOG_TRACE=1` reports repairs.
namespace fog {

constexpr uint32_t kLayerTable = 0x801B80E0;  // D_801B80E0: layer table pointer

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1u);
}

inline void put32(uint8_t* rdram, uint32_t off, uint32_t w) {
    std::memcpy(rdram + off, &w, sizeof(w));
}

struct Image {
    uint32_t address = 0;
    uint32_t size = 0;
    uint32_t width = 0, height = 0;
    uint32_t siz = 0, fmt = 0;
};

// The image record the game's layer `index` draws, or 0 when it is not usable.
inline bool layer_image(uint8_t* rdram, uint32_t index, uint32_t& rec) {
    const uint32_t table = rd32(rdram + (kLayerTable - 0x80000000u));
    if (table < 0x80000000u || table >= 0x80800000u) {
        return false;
    }
    const uint32_t entry = (table - 0x80000000u) + 0xFCu + index * 0x24u;
    if (entry + 8 > 0x20000000u) {
        return false;
    }
    rec = rd32(rdram + entry);
    return rec >= 0x80000000u && rec < 0x80800000u;
}

// The record's fields, read the way the game reads them (the runtime stores
// each word byte-reversed, so a byte field at logical offset `o` lives at
// `o ^ 3` and a 16-bit field at `o` lives at the two bytes starting at `o ^ 2`).
inline bool read_image(uint8_t* rdram, uint32_t rec, Image& img) {
    const uint32_t o = rec - 0x80000000u;
    if (o + 8 > 0x20000000u) {
        return false;
    }
    const uint8_t* p = rdram + o;
    img.siz = p[3 ^ 3];              // logical byte 3
    img.fmt = p[2 ^ 3];              // logical byte 2
    img.height = p[(4 ^ 2)] | (p[(5 ^ 2)] << 8);
    img.width = p[(6 ^ 2)] | (p[(7 ^ 2)] << 8);
    img.address = rec + 8;
    if (img.siz > 3 || img.fmt > 4 || img.width == 0 || img.height == 0 || img.width > 256 ||
        img.height > 256 || img.address >= 0x80800000u) {
        return false;
    }
    // Bits per texel for the G_IM_SIZ_* codes (0 = 4b, 1 = 8b, 2 = 16b, 3 = 32b).
    const uint32_t bits = 4u << img.siz;
    const uint32_t bytes_per_row = (img.width * bits) / 8u;
    img.size = bytes_per_row * img.height;
    return img.size != 0 && img.address + img.size <= 0x80800000u;
}

// OGRE_FOG_SCALE=<percent>: stage a copy of the layer's image in scratch RDRAM
// with its intensity scaled, so the fog can be made more or less prominent than
// the raw asset (whose mean intensity is only 0.05). The copy lives in the last
// 64 KiB of RDRAM, which OB64 leaves untouched (its data ends at 4 MiB and this
// port runs with the 8 MiB expansion); the first use reports it if that area is
// not zero, since a game that grew into 8 MiB would collide here.
inline uint32_t stage_scaled_image(uint8_t* rdram, const Image& img, uint32_t scale) {
    constexpr uint32_t kScratch = 0x007F0000u;
    constexpr uint32_t kScratchBytes = 0x4000u;  // 16 KiB: room for a 256x256 8b image
    if (img.size == 0 || img.size > kScratchBytes) {
        return img.address;
    }
    static bool checked = false;
    if (!checked) {
        checked = true;
        bool dirty = false;
        for (uint32_t i = 0; i < kScratchBytes; ++i) {
            if (rdram[kScratch + i] != 0) {
                dirty = true;
                break;
            }
        }
        if (dirty) {
            fprintf(stderr,
                    "[fog] scratch area 0x%06X is not zero; OGRE_FOG_SCALE cannot be used "
                    "safely in this scene (using the unscaled image)\n",
                    kScratch);
            fflush(stderr);
            return img.address;
        }
    }
    const uint32_t src = img.address - 0x80000000u;
    for (uint32_t i = 0; i < img.size; ++i) {
        const uint32_t v = (uint32_t(rdram[src + i]) * scale + 50u) / 100u;
        rdram[kScratch + i] = uint8_t(v > 255u ? 255u : v);
    }
    return kScratch + 0x80000000u;
}

// Rewrite the texture setup a zero-area rectangle inherits so it samples the
// fog layer's own image. Returns the image address the repair installed, or 0
// when no repair was applied (the caller reports it).
inline uint32_t repair(uint8_t* rdram, uint32_t layer, uint32_t settimg, uint32_t settile7,
                       uint32_t loadtile, uint32_t settile0, uint32_t tilesize, uint32_t uls,
                       uint32_t ult, uint32_t lrs, uint32_t lrt, int rect_s, int rect_t,
                       bool verbose) {
    // A zero-area tile is the marker: a one-texel tile is `lrs == uls + 4`.
    if (!(lrs == uls || lrt == ult)) {
        return 0;
    }
    if (settimg == 0 || settile7 == 0 || loadtile == 0 || settile0 == 0 || tilesize == 0) {
        return 0;
    }
    uint32_t rec = 0;
    Image img;
    if (!layer_image(rdram, layer, rec) || !read_image(rdram, rec, img)) {
        return 0;
    }
    // Every white group in the frame is a fog piece. The title screen has three:
    // the layer wrap behind the logo (the first group, `s = 63..0`) and the
    // bottom-of-screen sweep (later groups of one-screen-row-tall rectangles
    // whose `t` walks one texture row and whose `s` walks the width). They all
    // inherit the cloud loop's zero-area tail tile, which names a row of the
    // *cloud* texture rather than the layer's image.
    //
    // The sweep's `s` runs past 64 (it walks a 320-wide cloud texture: `s` up to
    // ~118 texels), so the repaired tile wraps (see `masks`/`maskt` below)
    // instead of the group being dropped — dropping it is what made the fog
    // blink on and off. Only a group with a negative coordinate cannot be
    // repaired at all; it keeps a valid one-row tile so RT64 reads the row the
    // loop left behind instead of a fabricated one.
    if ((rect_s < 0) || (rect_t < 0)) {
        // Keep the row the loop left behind and give it a one-texel height, so
        // RT64 decodes exactly that row out of TMEM.
        const uint32_t oneRow = 0xF2000000u | (uls << 12) | ult;
        const uint32_t oneRowLr = ((lrs > uls ? lrs : uls) << 12) | (ult + 4);
        put32(rdram, tilesize, oneRow);
        put32(rdram, tilesize + 4, oneRowLr);
        return 0;
    }
    const uint32_t bytes_per_row = (img.width * (4u << img.siz)) / 8u;
    const uint32_t line = std::max(bytes_per_row / 8u, 1u);  // SETTILE `line` in 64-bit words
    // G_SETTILESIZE / G_LOADTILE carry uls/ult/lrs/lrt in **quarter-texel**
    // units, not texels: the game's own tiles use `lrs = (width - 1) * 4` (e.g.
    // 320 texels -> lrs = 1276). Encoding the texel extents directly declared a
    // 64x64 image as a ~16x16 tile, so the repaired rectangles sampled a
    // clamped corner of the fog image and drew nothing.
    const uint32_t last = (((img.width - 1) * 4u) << 12) | ((img.height - 1) * 4u);

    // OGRE_FOG_SCALE=<percent> (default 100 = the asset as the game authored it).
    //
    // The fog's opacity is not a number we choose: the game's combiner is
    // `RGB = ONE, ALPHA = TEXEL0` with alpha compare off and blend colour alpha 0
    // (`othermode_l = 0x00504240`), so the overlay is white modulated by the
    // sampled texel's alpha, which for the layer table's 64x64 `G_IM_FMT_I`
    // images is simply their intensity (mean 12.9/255 = 5.1%, peak 52/255 =
    // 20.4%). This knob exists only to stage a scaled copy in scratch RDRAM for
    // A/B runs; 100 means "use the game's own image", and a retail emulator
    // capture measures the port's `©1999 QUEST` fog at 112-116% of a 45% build
    // (i.e. the raw asset) in the clean cloud region below the copyright.
    static const uint32_t scale = [] {
        const char* v = getenv("OGRE_FOG_SCALE");
        return (v != nullptr) ? uint32_t(strtoul(v, nullptr, 10)) : 100u;
    }();
    const uint32_t image_address =
        (scale == 100u) ? img.address : stage_scaled_image(rdram, img, scale);
    if (getenv("OGRE_FOG_TRACE") != nullptr) {
        fprintf(stderr, "[fog] scale=%u src=0x%08X staged=0x%08X size=%u\n", scale, img.address,
                image_address, img.size);
        fflush(stderr);
    }
    // G_SETTIMG: fmt/siz/width + the image address.
    put32(rdram, settimg, 0xFD000000u | (img.fmt << 21) | (img.siz << 19) | (img.width - 1));
    put32(rdram, settimg + 4, image_address);
    // G_SETTILE for the load tile (7) and the draw tile (0): same image at
    // TMEM offset 0. The draw tile wraps: the sweep's `s` walks to ~118 texels
    // (it is written against a 320-wide cloud texture), so without a mask the
    // samples past the edge would be clamped away. `masks`/`maskt` are the log2
    // of the wrap size, so this only applies to the power-of-two layer images;
    // anything else keeps the game's clamp.
    const uint32_t tile_w0 =
        0xF5000000u | (img.fmt << 21) | (img.siz << 19) | (line << 9);
    const auto mask_bits = [](uint32_t n) -> uint32_t {
        uint32_t bits = 0;
        while ((1u << bits) < n) {
            ++bits;
        }
        return ((1u << bits) == n) ? bits : 0;
    };
    const uint32_t masks = mask_bits(img.width);
    const uint32_t maskt = mask_bits(img.height);
    put32(rdram, settile7, tile_w0);
    put32(rdram, settile7 + 4, 7u << 24);
    put32(rdram, settile0, tile_w0);
    put32(rdram, settile0 + 4, (maskt << 14) | (masks << 4));
    // G_LOADTILE: the whole image into the load tile (uls/ult = 0,0).
    put32(rdram, loadtile, 0xF4000000u);
    put32(rdram, loadtile + 4, (7u << 24) | last);
    // G_SETTILESIZE for the draw tile: the rectangle's texture coordinates are
    // tile-relative, so the tile spans the whole image.
    put32(rdram, tilesize, 0xF2000000u);
    put32(rdram, tilesize + 4, last);
    if (verbose) {
        fprintf(stderr,
                "[fog] layer %u image=0x%08X %ux%u siz=%u fmt=%u line=%u rect s=%d t=%d -> "
                "repaired a zero-area group\n",
                layer, img.address, img.width, img.height, img.siz, img.fmt, line, rect_s, rect_t);
        fflush(stderr);
    }
    return image_address;
}

}  // namespace fog

// --- RT64Renderer ---------------------------------------------------------

class RT64Renderer final : public ultramodern::renderer::RendererContext {
  public:
    // OGRE: bring RDRAM up to date with the RDP's render targets. Called by the
    // recompiled game code before it copies a framebuffer with the CPU.
    void sync_framebuffers() {
        if (app_ != nullptr) {
            app_->syncFramebuffers();
            readback_probe();
        }
    }

    // OGRE_NJREAD_LOG=1: one line per njpeg stage-3 pass (func_ovlE_8019976C's
    // row loop), saying which framebuffer the copy is about to read. This is how
    // the stale-source defect was found (session 57): the patch used to override
    // the game's own `state[0x64]` with RT64's never-cleared scratch word, which
    // at the first pass of an assembly still named the *previous* screen's
    // framebuffer. `gameSrc` is the game's choice, the `game`/`scratch` token says
    // which one the patch applied, and the parenthesised words are RT64's record
    // (`scratch only when the game's index had no framebuffer-table match`).
    void readback_probe() {
        if (getenv("OGRE_NJREAD_LOG") == nullptr) {
            return;
        }
        if (app_ == nullptr) {
            return;
        }
        uint8_t* r = app_->core.RDRAM;
        if (r == nullptr) {
            return;
        }
        auto word = [&r](uint32_t guest) -> uint32_t {
            const uint8_t* b = r + (guest - 0x80000000u);
            return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        };
        const uint32_t dispWord = word(0x800C4BB8);
        const bool displayKnown = (dispWord == word(0x800A8204)) || (dispWord == word(0x800A8208)) ||
                                  (dispWord == word(0x800A820C));
        // The njpeg pipeline state object (bankE loads it from 0x8019A680): the
        // stage-3 copy reads +0x64 (source), writes +0x70 (destination), with
        // +0x78 columns and +0x7A rows.
        const uint32_t st = word(0x8019A680);
        static uint32_t pass = 0;
        if (st == 0) {
            return;
        }
        // FNV-1a over the 320x240 RGB555 source, in the runtime's byte order. The
        // four passes of one assembly read four fixed frames in order, so a
        // 4-periodic sequence of `src` hashes is a self-contained "the copy read
        // the right sub-image" check; `alt` is what the scratch-first rule would
        // have read instead, and is 0 when it agrees.
        auto sourceSig = [&r](uint32_t guest) -> uint64_t {
            const uint8_t* b = r + ((guest & 0x1FFFFFFFu));
            uint64_t h = 1469598103934665603ull;
            uint32_t nz = 0;
            for (uint32_t i = 0; i < 320u * 240u * 2u; i++) {
                h = (h ^ b[i]) * 1099511628211ull;
                nz += (b[i] != 0);
            }
            return (h & 0xFFFFFFFFFFFFull) | ((uint64_t)nz << 48);
        };
        const uint32_t gameSrc = word(st + 0x64);
        const uint32_t njpegWord = word(0x807FFC08);
        const uint32_t lastfbWord = word(0x807FFC0C);
        const uint32_t scratchFirst = njpegWord ? njpegWord : lastfbWord;
        const uint32_t used = displayKnown ? gameSrc : (scratchFirst ? scratchFirst : gameSrc);
        const uint64_t srcSig = sourceSig(used);
        // What the pre-session-57 rule (scratch word first) would have read; 0 when
        // it agrees with the game, in which case the two rules are indistinguishable
        // for this pass.
        const uint64_t altSig = (displayKnown && (scratchFirst != 0) &&
                                 ((scratchFirst & 0x1FFFFFFFu) != (gameSrc & 0x1FFFFFFFu)))
                                    ? sourceSig(scratchFirst)
                                    : 0;
        fprintf(stderr,
                "[njread] pass=%u present=%u scene=0x%04X step=0x%08X src=0x%08X %s sig=%016llX alt=%016llX "
                "(gameSrc=0x%08X scratch njpeg=0x%06X lastfb=0x%06X disp=0x%08X) dst=0x%08X w=%u h=%u\n",
                pass++, present_count_, word(0x800C4C26) & 0xFFFFu, word(0x8018F1C0), used,
                displayKnown ? "game" : "scratch", (unsigned long long)srcSig, (unsigned long long)altSig,
                gameSrc, njpegWord, lastfbWord, dispWord, word(st + 0x70),
                word(st + 0x78) & 0xFFFFu, word(st + 0x7A) & 0xFFFFu);
        fflush(stderr);
    }

    RT64Renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;

        // The synthetic-frame probe only makes sense if RT64 is willing to
        // present when nothing changes (a stalled boot never swaps VI buffers)
        // and to show a render target the framebuffer manager has not seen.
        // Both are env-gated diagnostics on the RT64 side; turn them on for the
        // probe unless the user set them explicitly (overwrite=0).
#if !defined(_WIN32)
        if (getenv("OGRE_SYNTH_FRAME") != nullptr) {
            setenv("OGRE_PRESENT_ALWAYS", "1", 0);
            // The render-target fallback is only wanted for the display-list
            // probe. With OGRE_SYNTH_NO_DL the probe is testing the raw RDRAM
            // upload path, which the fallback would bypass in favour of a stale
            // (empty) render target.
            if (getenv("OGRE_SYNTH_NO_DL") == nullptr) {
                setenv("OGRE_PRESENT_FBTARGET", "1", 0);
            }
        }
#endif

        // Set up the RT64 application core fields.
        // Keep the RDRAM base for the env-gated scene/record diagnostics below.
        rdram_ = rdram;
        RT64::Application::Core app_core{};
#if defined(_WIN32)
        app_core.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
        app_core.window = window_handle;
#elif defined(__APPLE__)
        app_core.window.window = window_handle.window;
        app_core.window.view = window_handle.view;
#endif
        app_core.checkInterrupts = dummy_check_interrupts;

        app_core.HEADER = dummy_rom_header;
        app_core.RDRAM = rdram;
        app_core.DMEM = DMEM;
        app_core.IMEM = IMEM;

        app_core.MI_INTR_REG = &MI_INTR_REG;

        app_core.DPC_START_REG = &DPC_START_REG;
        app_core.DPC_END_REG = &DPC_END_REG;
        app_core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        app_core.DPC_STATUS_REG = &DPC_STATUS_REG;
        app_core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
        app_core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        app_core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
        app_core.DPC_TMEM_REG = &DPC_TMEM_REG;

        // The VI registers are owned by ultramodern (written by the recompiled
        // osVi* calls); point RT64 at them so it decodes the real VI mode.
        ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
        app_core.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
        app_core.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
        app_core.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
        app_core.VI_INTR_REG = &vi_regs->VI_INTR_REG;
        app_core.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
        app_core.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
        app_core.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
        app_core.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
        app_core.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
        app_core.VI_H_START_REG = &vi_regs->VI_H_START_REG;
        app_core.VI_V_START_REG = &vi_regs->VI_V_START_REG;
        app_core.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
        app_core.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
        app_core.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

        // RT64 writes its own configuration to the config path otherwise.
        RT64::ApplicationConfiguration app_config;
        app_config.appId = "ogrebattle64";
        app_config.useConfigurationFile = false;

        // Create the RT64 application.
        app_ = std::make_unique<RT64::Application>(app_core, app_config);

        // Initial user config from the current ultramodern settings.
        const auto& cur_config = ultramodern::renderer::get_graphics_config();
        set_application_user_config(app_.get(), cur_config);
        app_->userConfig.developerMode = developer_mode;
        // OB64's ucode is F3DEX 2.08; RT64's getGBIForUCode picks the right GBI
        // from the OSTask's ucode data on every send_dl, so no override needed.
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;

        // OGRE_GRAPHICS_API=auto|d3d12|vulkan|metal pins RT64's backend.
        //
        // RT64 switches D3D12 to Vulkan on some old drivers ("Falling back to
        // Vulkan due to device workaround", rt64_application.cpp): an NVIDIA
        // driver at or below 475.14, an AMD driver at or below Jan 2019, and
        // Intel 6th-gen graphics at or below 31.0.101.2115. The fallback
        // destroys the D3D12 device and creates a Vulkan interface on the same
        // window, so starting in Vulkan directly skips that transition; `d3d12`
        // is the other side of the same A/B. A player whose old GPU crashes
        // during renderer setup can pin either one and report which works.
        if (const char* api_env = getenv("OGRE_GRAPHICS_API")) {
            if (strcmp(api_env, "d3d12") == 0) {
                app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            } else if (strcmp(api_env, "vulkan") == 0) {
                app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            } else if (strcmp(api_env, "metal") == 0) {
                app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            } else if (strcmp(api_env, "auto") != 0) {
                fprintf(stderr, "[renderer] OGRE_GRAPHICS_API=%s is not one of auto/d3d12/vulkan/metal; using auto\n",
                        api_env);
            }
            fprintf(stderr, "[renderer] OGRE_GRAPHICS_API=%s\n", api_env);
        }

        // Set up the RT64 application (window surface, device, shaders, workers).
        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app_->setup(thread_id));
        chosen_api = map_graphics_api(app_->chosenGraphicsAPI);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[renderer] RT64 setup failed (rt64 api=%d)\n", static_cast<int>(app_->chosenGraphicsAPI));
            app_ = nullptr;
            return;
        }

        // Fix the Organize Screen's DEF/MDEF "extra digit": RT64's "Fix LR with
        // Scissor" enhancement (`enhancementConfig.rect.fixRectLR`, default on)
        // snaps a rectangle's right edge to the scissor's right edge whenever the
        // two are within one pixel (rt64_rdp.cpp, `RDP::drawRect`). OB64's font
        // rectangles are correct as authored, and the unit profile's rightmost
        // digit sits exactly 1 px inside the panel scissor — scissor lrx `1180`,
        // digit rect lrx `1176` — so the snap widens that 6-px glyph rectangle to
        // 7 px, and the seventh pixel samples the *next* font-sheet cell's first
        // column (`28` reads as a cropped `9`, `11` as a cropped `2`). The
        // enhancement exists for games that under-draw a full-screen background;
        // OB64 does not, and mupen64plus + parallel-rdp, which has no such
        // enhancement, renders the same display list without the column.
        // `OGRE_RECT_LR_FIX=1` restores RT64's default for an A/B. This runs after
        // `setup`, because `updateEnhancementConfig` writes through the shared
        // queue resources that `setup` creates.
        {
            const char* lr_fix = getenv("OGRE_RECT_LR_FIX");
            app_->enhancementConfig.rect.fixRectLR = (lr_fix != nullptr) && (lr_fix[0] != '0');
            app_->updateEnhancementConfig();
            fprintf(stderr, "[renderer] rect/scissor snap (fixRectLR) = %d%s\n",
                    app_->enhancementConfig.rect.fixRectLR ? 1 : 0,
                    app_->enhancementConfig.rect.fixRectLR ? " (OGRE_RECT_LR_FIX=1)" : "");
        }

        app_->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        // OGRE_DUMP_TEX=<dir>: make RT64 dump every texture it decodes (a 4 KiB
        // .tmem plus its .tile.json) into <dir>. Used to inspect what the
        // renderer actually samples for a suspect tile.
        if (const char* texdir = getenv("OGRE_DUMP_TEX")) {
            app_->state->dumpingTexturesDirectory = texdir;
        }
        // OGRE_FB_GPU_COPY=0: turn off RT64's GPU framebuffer copies. A texture
        // load whose RDRAM source is covered by a framebuffer RT64 has on record
        // can be served from that framebuffer's GPU copy instead of the texture's
        // own bytes. OB64 reuses heap that a depth framebuffer still covers for
        // the map unit's CPU-built sprite bitmaps, so that copy is stale (the
        // black head sprite: `[fbcopy] ... fbFmt=5` in
        // docs/HANDOFF-2026-10-11-session133.md's follow-up run). The RT64-side
        // fix refuses a copy whose framebuffer is out of sync with RDRAM; this
        // knob is the fallback that removes the path entirely, for an A/B.
        if (const char* v = getenv("OGRE_FB_GPU_COPY")) {
            app_->emulatorConfig.framebuffer.copyWithGPU = (v[0] != '0');
            fprintf(stderr, "[renderer] framebuffer.copyWithGPU = %d%s\n",
                    app_->emulatorConfig.framebuffer.copyWithGPU ? 1 : 0,
                    app_->emulatorConfig.framebuffer.copyWithGPU ? "" : " (OGRE_FB_GPU_COPY=0)");
        }
        // stderr, not stdout: stdout is block-buffered when the app is piped and
        // this line was being lost, which made a successful RT64 setup look like
        // a silent failure.
        fprintf(stderr, "[renderer] RT64 renderer initialized (api=%d)\n", static_cast<int>(chosen_api));
        fflush(stderr);

        // RT64 only pushes a present when the VI changes or the RDRAM copy of the
        // VI framebuffer changes (rt64_state.cpp State::updateScreen). A stalled
        // boot does neither (the game is wedged before its frame loop), so the
        // presenter stops after the first few VIs and the window keeps an old
        // (black) frame forever. OGRE_INSTANT_PRESENT=1 switches RT64 to
        // PresentEarly, where a submitted display list presents the framebuffer
        // it drew instead of waiting for a VI change.
        if (getenv("OGRE_INSTANT_PRESENT") != nullptr) {
            enable_instant_present();
            fprintf(stderr, "[renderer] instant present enabled (PresentEarly)\n");
            fflush(stderr);
        }
    }

    ~RT64Renderer() override = default;


    bool valid() override { return app_ != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (app_ == nullptr || old_config == new_config) {
            return false;
        }

        if (new_config.wm_option != old_config.wm_option) {
            app_->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
            // Leaving fullscreen hands the window back with the rect RT64 saved
            // before it went fullscreen; ask for one re-fit so the shape matches
            // the WIDESCREEN mode even if the player changed it while
            // fullscreen. Doing it from the renderer thread would be an SDL call
            // off the main thread, so this only flags it.
            if (new_config.wm_option == ultramodern::renderer::WindowMode::Windowed) {
                ogre::widescreen_request_refit();
            }
        }

        set_application_user_config(app_.get(), new_config);

        // Only discard framebuffers when a resolution-affecting option changed.
        const bool resolution_changed = new_config.res_option != old_config.res_option;
        const bool aspect_ratio_changed = new_config.ar_option != old_config.ar_option;
        const bool downsampling_changed = new_config.ds_option != old_config.ds_option;
        const bool msaa_changed = new_config.msaa_option != old_config.msaa_option;
        app_->updateUserConfig(resolution_changed || aspect_ratio_changed || downsampling_changed || msaa_changed);

        if (msaa_changed) {
            app_->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        if (app_ == nullptr) {
            return;
        }
        app_->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        if (app_ == nullptr) {
            return;
        }
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);

        ++dl_count_;
        const auto dl_begin = std::chrono::high_resolution_clock::now();
        // Wall time between submissions plus the recompiled-function entries the
        // game spent on the frame: a frame that takes 2 s because it executes
        // 50x the work is a different bug from one that simply slept.
        static uint64_t last_entries = 0;
        {
            uint64_t entries = ultramodern::debug_total_entry_count();
            const uint64_t delta = entries - last_entries;
            last_entries = entries;
            // OGRE_DL_TRACE=1 prints every submission with its time; the default
            // samples the first few and every tenth.
            if (getenv("OGRE_DL_TRACE") != nullptr || dl_count_ <= 8 || (dl_count_ % 10) == 0) {
                // trace_millis, not time_since_start: the profiler and scheduler
                // ring report on the same clock, so stalls can be lined up with
                // the work the game was doing.
                fprintf(stderr, "[renderer] display list %u at t=%llums entries=%llu (type=%u ucode=0x%08X data=0x%08X)\n",
                        dl_count_, (unsigned long long)ultramodern::trace_millis(),
                        (unsigned long long)delta, task->t.type, task->t.ucode, task->t.data_ptr);
            }
        }

        // OGRE_NOP_RECT=<selectors>: bisect aid, see the helper above.
        if (const char* nop = getenv("OGRE_NOP_RECT")) {
            static const std::vector<nop_rect::Selector> selectors = nop_rect::parse(nop);
            static uint32_t patched = 0;
            const bool verbose = (patched < 2);
            ++patched;
            nop_rect::Walker walker{app_->core.RDRAM, &selectors, verbose, {}, 0, 0, 200000};
            walker.walk(task->t.data_ptr & 0x1FFFFFFF, 1);
            fprintf(stderr, "[nop-rect] dl=%u walked=%u removed=%u\n", dl_count_, walker.index,
                    walker.removed);
            fflush(stderr);
        }

        // OGRE_TEX_TRACE=<1>: report every textured rectangle and the RDRAM it
        // samples (see the helper above). Read-only; nothing is patched.
        //
        // The capture is armed by the first of:
        //   OGRE_TEX_TRACE_KEY=<1..9>  the digit key is pressed (edge-triggered)
        //   OGRE_TEX_TRACE_AFTER_MS=<n>  the run's trace clock passes n ms
        // and it stops after OGRE_TEX_TRACE_MS=<n> ms (default 2000), so pressing
        // a key at the bad frame captures that frame and the next second and
        // nothing else. OGRE_TEX_TRACE_KEY=0 (or no key and no AFTER_MS) traces
        // from the first display list.
        if (getenv("OGRE_TEX_TRACE") != nullptr) {
            static const std::string dumpDir = [] {
                const char* d = getenv("OGRE_TEX_TRACE_DUMP");
                if (d == nullptr || d[0] == '\0') {
                    return std::string();
                }
                std::string dir(d);
                if (!dir.empty() && dir.back() == '/') {
                    dir.pop_back();
                }
                ensure_dir(dir);
                ensure_dir(dir + "/tex");
                fprintf(stderr, "[tex] dumping sampled RDRAM to %s/tex\n", dir.c_str());
                return dir;
            }();
            static const uint32_t onlyDraw = [] {
                const char* v = getenv("OGRE_TEX_TRACE_DRAW");
                return (uint32_t)((v != nullptr) ? strtoul(v, nullptr, 0) : 0);
            }();
            static const uint32_t maxDraws = [] {
                const char* v = getenv("OGRE_TEX_TRACE_MAX");
                return (uint32_t)((v != nullptr) ? strtoul(v, nullptr, 0) : 20000);
            }();
            static const uint32_t minPx = [] {
                const char* v = getenv("OGRE_TEX_TRACE_MIN_PX");
                return (uint32_t)((v != nullptr) ? strtoul(v, nullptr, 0) : 0);
            }();
            // OGRE_TEX_TRACE_BLACK=<0..255>: a draw whose sampled colour mean is
            // at or below this is reported as a black draw (default 8). Used with
            // OGRE_TEX_TRACE_COLOUR.
            static const uint32_t colourBlackness = [] {
                const char* v = getenv("OGRE_TEX_TRACE_BLACK");
                return (uint32_t)((v != nullptr) ? strtoul(v, nullptr, 0) : 4);
            }();
            static uint32_t blackDraws = 0;
            static const uint64_t afterMs = [] {
                const char* v = getenv("OGRE_TEX_TRACE_AFTER_MS");
                return (uint64_t)((v != nullptr) ? strtoull(v, nullptr, 0) : 0);
            }();
            static const uint64_t holdMs = [] {
                const char* v = getenv("OGRE_TEX_TRACE_MS");
                return (uint64_t)((v != nullptr) ? strtoull(v, nullptr, 0) : 2000);
            }();
            static const int hotkey = [] {
                const char* v = getenv("OGRE_TEX_TRACE_KEY");
                return (v != nullptr) ? (int)strtol(v, nullptr, 0) : 0;
            }();
            // OGRE_TEX_TRACE_KEY2: a second arming key with its own edge state,
            // so one run can take an RDRAM dump on one key (OGRE_RAM_KEY) and
            // arm the trace on another without the two consuming each other's
            // press. Session 131.
            static const int hotkey2 = [] {
                const char* v = getenv("OGRE_TEX_TRACE_KEY2");
                return (v != nullptr) ? (int)strtol(v, nullptr, 0) : 0;
            }();
            static bool armed = false;
            static bool hotkey_mode = false;   // a key capture re-arms; a timer does not
            static bool finished = false;      // a timer capture has already run
            static uint64_t armed_at = 0;
            if (!armed) {
                // A timer capture runs once; a key capture waits for the next press.
                bool go = false;
                if (hotkey != 0) {
                    go = console::key_pressed(hotkey, 0);
                } else if (!finished) {
                    go = (afterMs == 0) || (ultramodern::trace_millis() >= afterMs);
                }
                if (hotkey2 != 0) {
                    go = go || console::key_pressed(hotkey2, 1);
                }
                if (go) {
                    armed = true;
                    hotkey_mode = (hotkey != 0) || (hotkey2 != 0);
                    armed_at = ultramodern::trace_millis();
                    fprintf(stderr, "[tex] armed at t=%llums (%s), holding %llums\n",
                            (unsigned long long)armed_at,
                            hotkey_mode ? "key pressed" : "timer", (unsigned long long)holdMs);
                    fflush(stderr);
                }
            } else if (holdMs != 0 && (ultramodern::trace_millis() - armed_at) >= holdMs) {
                fprintf(stderr, "[tex] capture window closed at t=%llums\n",
                        (unsigned long long)ultramodern::trace_millis());
                fflush(stderr);
                armed = false;
                if (!hotkey_mode) {
                    finished = true;          // a timer capture does not repeat
                }
            }
            if (armed) {
                tex_trace::State st;
                st.rdram = app_->core.RDRAM;
                st.dump_dir = dumpDir;
                st.only_draw = onlyDraw;
                st.max_draws = maxDraws;
                st.min_px = minPx;
                st.trace_all_images = (getenv("OGRE_TEX_TRACE_IMAGES") != nullptr);
                st.colour = (getenv("OGRE_TEX_TRACE_COLOUR") != nullptr);
                st.dump_on_black = (getenv("OGRE_RAM_KEY_ON_BLACK") != nullptr);
                st.colour_blackness = colourBlackness;
                st.dl = dl_count_;
                st.walk(task->t.data_ptr & 0x1FFFFFFF, 1);
                if (st.truncated) {
                    fprintf(stderr, "[tex] dl=%u truncated after %u rectangles\n", dl_count_,
                            st.reported);
                }
                blackDraws += st.black_draws;
                if (st.black_draws != 0) {
                    fprintf(stderr, "[tex] dl=%u %u black draw(s) this list (run total %u)\n",
                            dl_count_, st.black_draws, blackDraws);
                }
                fflush(stderr);
            }
        }

        // OGRE_FOG: repair OB64's stale-tile fog/cloud "wrap" pieces (see the
        // helper above). Runs on the game's own display list, before RT64
        // parses it; OGRE_FOG=0 turns it off.
        {
            // OGRE_FOG=alternate applies the repair on even display lists only,
            // so two consecutive presents differ by exactly what the repair
            // draws (a phase-matched A/B for a single run).
            static const int fogMode = [] {
                const char* v = getenv("OGRE_FOG");
                if (v == nullptr) return 1;
                if (strcmp(v, "0") == 0) return 0;
                if (strcmp(v, "alternate") == 0) return 2;
                return 1;
            }();
            const bool fogEnabled =
                (fogMode == 1) || ((fogMode == 2) && (((dl_count_ / 2) % 2) == 0));
            if (fogEnabled) {
                static const bool fogVerbose = (getenv("OGRE_FOG_TRACE") != nullptr);
                static const std::vector<nop_rect::Selector> noSelectors;
                static uint32_t fogFrames = 0;
                ++fogFrames;
                nop_rect::Walker walker{app_->core.RDRAM, &noSelectors, fogVerbose, {}, 0, 0, 200000};
                walker.fog_fix = true;
                walker.fog_dl = dl_count_;
                walker.walk(task->t.data_ptr & 0x1FFFFFFF, 1);
                if ((walker.fog_fixed != 0) && (fogFrames <= 2)) {
                    fprintf(stderr, "[fog] dl=%u repaired %u group(s)\n", dl_count_,
                            walker.fog_fixed);
                    fflush(stderr);
                }
                // OGRE_FOG_SUMMARY=1: one line per display list, so a fog group
                // that is repaired on some frames and not others (a blink) is
                // visible next to the captured frames.
                if (getenv("OGRE_FOG_SUMMARY") != nullptr) {
                    fprintf(stderr,
                            "[fog-summary] dl=%u ptr=0x%08X repaired=%u last_layer=%u "
                            "last_image=0x%08X white=%u zero=%u\n",
                            dl_count_, (uint32_t)task->t.data_ptr, walker.fog_fixed,
                            walker.fog_last_layer, walker.fog_last_image, walker.fog_white_groups,
                            walker.fog_zero_groups);
                }
            }
        }

        // GBI selection note (2026-08-29): OB64's gfx ucode is "RSP Gfx ucode
        // F3DEX fifo 2.08". RT64's GBI database matches it (by hash) to
        // GBIUCode::F3DEX2, and that map is correct for the game's display
        // lists (first boot DL: 0xDE=G_DL -> 0xA9EF0, 0xE9=G_RDPFULLSYNC,
        // 0xDF=G_ENDDL; later DLs branch to real KSEG0 targets like 0x801869E8).
        // Do NOT force the plain F3DEX GBI here — it does not map those opcodes
        // and misparses the DLs.
        app_->processDisplayLists(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);

        // OGRE (session 77): the per-display-list wait for the njpeg framebuffers
        // (`Application::waitForGameFramebuffers`, the old OGRE_NJ_WAIT_MS) used
        // to run here. It was added in session 50 to stop the stage-3 CPU copy
        // (`func_ovlE_8019976C`) reading RDRAM before the YUV macroblock draw had
        // rendered, but it is not what orders that copy: the game's own
        // DP-completion wait plus `ogre_sync_framebuffers()` (inserted by
        // tools/njpeg_readback.py, which waits for the workload and writes the
        // rendered buffers back) do it. Session 57 showed the wait never once
        // blocked in the njpeg path.
        //
        // Worse, it polls for three *fixed* addresses (0x400/0x25C00/0x4B400).
        // Every scene that does not render into them -- most visibly the boot's
        // publisher stills (scene 0x0A, the Nintendo/ATLUS/QUEST logos), which
        // use a 640x480 image buffer instead -- can never satisfy the condition,
        // so the poll burned its whole 500 ms timeout on every submitted list:
        // 2 display lists/s instead of 30. Removing it restores the retail frame
        // rate (there is no such wait on hardware) and also drops the one-off
        // 500 ms stall on the run's first list. The RT64-side helper is left in
        // place (unused) so the submodule patch does not have to be regenerated;
        // it can go with the next patches/rt64-ob64.patch refresh.

        // OGRE_DL_ANALYZE=1: walk the submitted display list with the app's own
        // F3DEX2 analyzer and report its geometry. "The cube is missing" is
        // either "the DL has no triangles" (game-side) or the triangles are
        // there and the renderer drops them.
        if (getenv("OGRE_DL_ANALYZE") != nullptr) {
            ogre::gbi::WorkloadStats stats;
            ogre::gbi::analyze_dl(app_->core.RDRAM, task->t.data_ptr & 0x1FFFFFFF,
                                  (uint32_t)task->t.data_size, stats);
            fprintf(stderr,
                    "[dl-analyze] dl=%u ptr=0x%08X cmds=%llu dls=%llu tri1=%llu tri2=%llu quad=%llu "
                    "texrect=%llu fill=%llu vtx=%llu verts=%llu settimg=%llu unknown=%llu\n",
                    dl_count_, (uint32_t)task->t.data_ptr, (unsigned long long)stats.commands,
                    (unsigned long long)stats.dls_walked, (unsigned long long)stats.tri1,
                    (unsigned long long)stats.tri2, (unsigned long long)stats.quad,
                    (unsigned long long)stats.texrect, (unsigned long long)stats.fillrect,
                    (unsigned long long)stats.vtx_calls, (unsigned long long)stats.vertices,
                    (unsigned long long)stats.settimg, (unsigned long long)stats.unknown_cmds);
            fflush(stderr);
        }
        // OGRE_DL_DECODE=<dl>|all: dump the decoded command stream of one
        // submission. This is the ground truth for "which RDP command draws
        // this" questions (source size, texrect destination, combiner).
        if (const char* dec = getenv("OGRE_DL_DECODE")) {
            const bool all = (strcmp(dec, "all") == 0);
            const unsigned want = (unsigned)strtoul(dec, nullptr, 0);
            if (all || want == dl_count_) {
                std::string dump = ogre::gbi::decode_dl(app_->core.RDRAM,
                                                        task->t.data_ptr & 0x1FFFFFFF);
                fprintf(stderr, "[dl-decode] dl=%u ptr=0x%08X (%zu bytes)\n%s", dl_count_,
                        (uint32_t)task->t.data_ptr, dump.size(), dump.c_str());
                fflush(stderr);
            }
        }
        const auto dl_end = std::chrono::high_resolution_clock::now();
        const auto dl_ms = std::chrono::duration_cast<std::chrono::microseconds>(dl_end - dl_begin).count();
        if (dl_count_ <= 8 || (dl_count_ % 10) == 0 || dl_ms > 100000) {
            fprintf(stderr, "[renderer]   processDisplayLists %lld us (dl %u)\n",
                    (long long)dl_ms, dl_count_);
            fflush(stderr);
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        if (app_ == nullptr) {
            return;
        }
        app_->state->listProcessBegin();
        app_->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
        // G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_FILL | G_PM_NPRIMITIVE
        // G_AC_NONE | G_ZS_PIXEL | G_RM_NOOP | G_RM_NOOP2
        app_->state->rdp->setOtherMode(0x382C30, 0);
        app_->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
        app_->state->fullSync();
        app_->state->listProcessEnd();
    }

    void update_screen() override {
        if (app_ == nullptr) {
            return;
        }
        ++present_count_;
        // OGRE_VI_TRACE=1: report the VI state RT64 is about to present. The
        // presenter only draws when the decoded VI is visible and has a nonzero
        // size (rt64_present_queue.cpp: viVisible/fbSize), so a black canvas is
        // either "not visible" or "not this framebuffer".
        if (getenv("OGRE_VI_TRACE") != nullptr) {
            static uint32_t vi_trace_count = 0;
            ++vi_trace_count;
            if (vi_trace_count <= 5 || (vi_trace_count % 120) == 0) {
                RT64::VI vi = app_->core.decodeVI();
                hlslpp::uint2 size = vi.fbSize();
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "[vi] n=%u status=0x%08X origin=0x%06X width=%u visible=%u fbAddr=0x%06X "
                         "fbSizeX=%u fbSizeY=%u fbSiz=%u hRegionW=0x%08X vRegionW=0x%08X "
                         "xformW=0x%08X yformW=0x%08X xScale=%.4f yScale=%.4f swapChainH=%d",
                         vi_trace_count, (unsigned)vi.status.word, (unsigned)vi.origin, (unsigned)vi.width,
                         (unsigned)(vi.visible() ? 1 : 0), (unsigned)vi.fbAddress(), (unsigned)size.x,
                         (unsigned)size.y, (unsigned)vi.fbSiz(), (unsigned)vi.hRegion.word,
                         (unsigned)vi.vRegion.word, (unsigned)vi.xTransform.word, (unsigned)vi.yTransform.word,
                         vi.xScaleFloat(), vi.yScaleFloat(),
                         (app_->sharedQueueResources != nullptr) ? (int)app_->sharedQueueResources->swapChainHeight : -1);
                fprintf(stderr, "%s\n", buf);
                fflush(stderr);
            }
        }
        // OGRE_SCENE_TRACE=1: dump the intro scene state once per second. The
        // intro ("soldiers, falling cube, N64 logo") is driven by overlay C's
        // state machines; every value below is plain RDRAM, so no recompiled code
        // needs to be touched to observe it:
        //   0x801B81D0  scene object base (13 x 0x10 texture records at base+0x6C0)
        //   0x801BA70C  9/10 phase counters, plus the 700/701/72C draw flags
        //   0x801977E8  which streamed bank the loader staged (1/2/3)
        //   0x80190F30  boot scene bitmask (DMA'd from ROM 0x275D98C)
        //   0x801B84AC  the state-10 object (0x1114/0x1116 fades, 0x1118 timer)
        if (getenv("OGRE_SCENE_TRACE") != nullptr) {
            static uint32_t last_ms = 0;
            static uint32_t last_base = 0;
            uint32_t now_ms = (uint32_t)ultramodern::trace_millis();
            // Dump immediately when the scene object first appears: the intro
            // currently dies a frame or two after state 9 init, so an
            // interval-only trace would never show the records.
            const bool base_appeared = (last_base == 0) && (u32(0x801B81D0) != 0);
            if (now_ms - last_ms >= 200 || base_appeared) {
                last_ms = now_ms;
                last_base = u32(0x801B81D0);
                if (uint8_t* live = ultramodern_get_rdram_base()) {
                    rdram_ = live;
                }
                // RDRAM is native-endian 32-bit words indexed by
                // (addr - 0x80000000); 16/8-bit accesses are byte-swapped
                // inside the word (see MEM_W/MEM_HU/MEM_BU in recomp.h).
                auto wptr = [this](uint32_t addr) -> uint8_t* { return rdram_ + (addr - 0x80000000u); };
                auto u32 = [&wptr](uint32_t addr) -> uint32_t {
                    uint32_t v;
                    std::memcpy(&v, wptr(addr), sizeof(v));
                    return v;
                };
                auto s16 = [&wptr](uint32_t addr) -> int {
                    int16_t v;
                    std::memcpy(&v, wptr(addr ^ 2u), sizeof(v));
                    return (int)v;
                };
                auto u8 = [&wptr](uint32_t addr) -> unsigned { return wptr(addr ^ 3u)[0]; };
                auto f32 = [&u32](uint32_t addr) -> float {
                    uint32_t w = u32(addr);
                    float f;
                    std::memcpy(&f, &w, sizeof(f));
                    return f;
                };
                {
                    static bool scanned = false;
                    if (!scanned) {
                        scanned = true;
                        size_t nz = 0;
                        for (size_t i = 0; i < 0x800000; i++) {
                            if (rdram_[i] != 0) nz++;
                        }
                        uint32_t first_nz = 0;
                        for (size_t i = 0; i < 0x800000; i++) {
                            if (rdram_[i] != 0) { first_nz = (uint32_t)i; break; }
                        }
                        fprintf(stderr, "[scene]   rdram=%p core_rdram=%p nonzero=%zu first_nz=0x%X\n", (void*)rdram_,
                                (void*)app_->core.RDRAM, nz, first_nz);
                        fflush(stderr);
                    }
                }
                const uint32_t base = u32(0x801B81D0);
                // `base` is game state, not a constant: at a scene transition the
                // word holds a stale/wild value before the new scene installs its
                // object (session 85 -- the title -> story `0x0B` handoff). Every
                // read below is base-relative, and `wptr` does no bounds check, so
                // an unvalidated base walks off RDRAM and dies with SIGBUS inside
                // this diagnostic. Only walk it when it is a KSEG0 pointer whose
                // whole window (the 13 records at +0x6C0 plus the +0x830 fields)
                // fits in the 8 MiB RDRAM.
                const bool base_ok = base >= 0x80000000u && base <= 0x80800000u - 0x8A0u;
                fprintf(stderr,
                        "[scene] t=%ums state=%u req=0x%04X base=0x%08X phase=%u p710=%u flags=%u/%u/%u bank=%u "
                        "mask=0x%08X\n",
                        now_ms, (unsigned)(u32(0x800E8214) & 0xFFFF), (unsigned)(u32(0x800C4C26) & 0xFFFF), base,
                        u32(0x801BA70C), u32(0x801BA710), u8(0x801BA700), u8(0x801BA701), u8(0x801BA72C),
                        u32(0x801977E8), u32(0x80190F30));
                if (base_ok) {
                    for (int i = 0; i < 13; i++) {
                        uint32_t rec = base + 0x6C0 + (uint32_t)i * 0x10;
                        fprintf(stderr, "[scene]   rec[%2d] @0x%08X ptr=0x%08X flag=%d x=%d y=%d\n", i, rec,
                                u32(rec), s16(rec + 4), s16(rec + 0xA), s16(rec + 0xC));
                    }
                    uint32_t neff = u32(base + 0x810);
                    fprintf(stderr, "[scene]   idx790=%u idx794=%u effects=%u script=0x%08X at=%u\n",
                            u32(base + 0x790), u32(base + 0x794), neff, u32(base + 0x82C),
                            u32(base + 0x830));
                    for (uint32_t i = 0; i < neff && i < 8; i++) {
                        uint32_t e = u32(base + 0x798 + i * 4);
                        fprintf(stderr, "[scene]     eff[%u]=0x%08X\n", i, e);
                    }
                }
                const uint32_t s10 = u32(0x801B84AC);
                // Same hazard as `base`: only touch the object when the word is a
                // plausible KSEG0 pointer (`s10 + 0x1118` must be inside RDRAM).
                const bool s10_ok = s10 >= 0x80000000u && s10 <= 0x80800000u - 0x1120u;
                fprintf(stderr, "[scene]   s10=0x%08X f14=%d f16=%d f18=%.3f\n", s10,
                        s10_ok ? s16(s10 + 0x1114) : 0, s10_ok ? s16(s10 + 0x1116) : 0,
                        s10_ok ? f32(s10 + 0x1118) : 0.0f);
                fflush(stderr);
            }
        }
        app_->updateScreen();
    }

    void shutdown() override {
        if (app_ != nullptr) {
            app_->end();
            app_ = nullptr;
        }
    }

    uint32_t get_display_framerate() const override {
        if (app_ == nullptr || app_->presentQueue == nullptr || app_->presentQueue->ext.sharedResources == nullptr) {
            return 60;
        }
        return app_->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        if (app_ == nullptr || app_->presentQueue == nullptr || app_->presentQueue->ext.sharedResources == nullptr) {
            return 1.0f;
        }
        constexpr int kReferenceHeight = 240;
        switch (app_->userConfig.resolution) {
            case RT64::UserConfiguration::Resolution::WindowIntegerScale:
                if (app_->presentQueue->ext.sharedResources->swapChainHeight > 0) {
                    return std::max(float((app_->presentQueue->ext.sharedResources->swapChainHeight + kReferenceHeight - 1) / kReferenceHeight), 1.0f);
                }
                return 1.0f;
            case RT64::UserConfiguration::Resolution::Manual:
                return float(app_->userConfig.resolutionMultiplier);
            case RT64::UserConfiguration::Resolution::Original:
            default:
                return 1.0f;
        }
    }

  private:
    std::unique_ptr<RT64::Application> app_;
    // RDRAM base, kept so the env-gated scene diagnostics can peek at game state.
    uint8_t* rdram_ = nullptr;
    // Display lists actually submitted by the game. The title scene's stall is
    // "the game never sends a gfx task", and nothing else on this path reports
    // that, so log the first few and then every 50th.
    uint32_t dl_count_ = 0;
    // Presents seen by update_screen(). RT64's `OGRE_CAPTURE_PRESENT` names its
    // files by the same count, so the `[njread]` line's present index says which
    // capture to look at for a given njpeg pass.
    uint32_t present_count_ = 0;
};

}  // namespace

// OGRE: set right after the renderer is created so the recompiled game code can
// ask RT64 to bring RDRAM up to date before it reads a framebuffer with the CPU
// (see RT64::State::syncFramebuffers and tools/njpeg_readback.py).
static RT64Renderer* g_renderer = nullptr;

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    auto renderer = std::make_unique<RT64Renderer>(rdram, window_handle, developer_mode);
    g_renderer = renderer.get();
    return renderer;
}

// OGRE: C entry point for the recompiled game code (declared in the generated
// funcs file by tools/njpeg_readback.py). On renderers that do not maintain RDP
// render targets (the null renderer, the web renderer) the symbol is a no-op.
// Only one renderer source is compiled per build, so there is never a second
// definition to override; the weak attribute is kept for GCC/Clang on ELF/Mach-O
// and dropped for the MSVC-ABI toolchains, which cannot emit a weak function
// (cl rejects the attribute, and COFF has no weak symbols). `_MSC_VER` covers
// both cl.exe and clang-cl; MinGW defines `_WIN32` but not `_MSC_VER` and does
// support the attribute.
#if defined(_MSC_VER)
#define OGRE_WEAK_SYMBOL
#else
#define OGRE_WEAK_SYMBOL __attribute__((weak))
#endif
extern "C" OGRE_WEAK_SYMBOL void ogre_sync_framebuffers() {
    if (g_renderer != nullptr) {
        g_renderer->sync_framebuffers();
    }
}
#undef OGRE_WEAK_SYMBOL

}  // namespace ogre
