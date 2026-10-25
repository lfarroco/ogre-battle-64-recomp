// Ogre Battle 64: Person of Lordly Caliber - WebGL2 browser renderer
// (milestone 7 prototype, docs/WEB-PORT.md §16 Option A).
//
// Implements ultramodern::renderer::RendererContext for the Emscripten build.
// It parses the game's F3DEX2 display lists with the shared walker from
// gbi.hpp and draws them with WebGL2:
//   - RDP state: scissor, cycle type, blend, combiner (general 2-cycle
//     formula evaluated in the fragment shader), prim/env/fill colors.
//   - Geometry: G_VTX + G_TRI1/G_TRI2/G_QUAD through the F3DEX2 matrix
//     pipeline (modelview x projection, perspective divide, viewport).
//   - Sprites: G_TEXRECT / G_TEXRECTFLIP / G_FILLRECT.
//   - Textures: N64 formats (RGBA16/32, IA16/8/4, I8/I4, CI8/CI4 + TLUT)
//     decoded on G_LOADTILE/G_LOADBLOCK/G_LOADTLUT into a GL texture cache.
//
// The WebGL2 context is created on the browser main thread by app/web/web.js
// and handed to the renderer via ogre_gfx_set_canvas(); GL calls from the
// runtime's gfx pthread are proxied to the main thread by Emscripten.
//
// Known prototype limitations (documented in WEB-PORT-REPORT.md §10):
//   - A `G_LOADTILE` whose source lies inside a colour image the RDP has drawn
//     reads RDRAM. A framebuffer-source copy exists behind `OGRE_FBTEX=1`, but
//     it is not enabled: the GPU copy is only authoritative once RDRAM changes
//     are re-uploaded into it and the framebuffer is written back at the end of
//     every workload (RT64's `checkRAM`/`uploadRAM` plus its per-workload
//     writeback), which this renderer does not do yet. The scene-0x03 briefing
//     is the A/B that shows it (session 193).
//   - TEXEL1/second-texture combiners fall back to TEXEL0.
//   - LOADBLOCK dxt (arbitrary texel spacing) is approximated.
//   - No lighting/texgen (OB64's title content is unlit 2D).
//   - Presentation blits the whole scanout target; the VI's own view rectangle
//     (`fldRegs[].origin` and the x/y scale) is not applied.

#include "renderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <utility>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <emscripten/threading.h>
#include <GLES3/gl3.h>

// XXH3-64, header-only in this one translation unit. The GBI a display list
// belongs to is decided by hashing the task's ucode text, the way RT64's
// GBIManager::getGBIForUCode does it; see the constants below.
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "gbi.hpp"
#include "milestones.hpp"
#include "bank_overlays.hpp"
#include "web_settings.hpp"
#include "ram_key.hpp"

namespace ogre {

namespace {

// ============================================================================
// Workload analysis export (milestone 6) - shared with the null renderer.
// ============================================================================

gbi::WorkloadStats g_workload;
std::mutex g_stats_mutex;
char g_stats_snapshot[4096];

// Session-19: non-perturbing execution telemetry. The gfx pthread updates these
// atomics once per display-list command (a few relaxed stores; no I/O), and the
// browser main thread surfaces them in ogre_gfx_stats() at 1 Hz. This is how a
// DL walk that stalls is located without the per-command proxied-printf
// throttling that tracing causes.
// Debug switches for the probes (ogre_gfx_debug_flags):
//   bit 0 = ignore the render mode's alpha blending (writes the combiner colour
//           opaquely) - separates "is the colour pipeline right" from "is the
//           mask right";
//   bit 1 = draw only the first sprite of a display list, so one rendered sprite
//           can be compared with the texture pair that produced it;
//   bits 3-4 = bypass the combiner and output TEXEL1 (bit 3 set) or TEXEL0
//           (bit 4 set) raw, so the sampled texture can be compared with
//           ogre_gfx_debug_last_tex's decoded image;
//   bit 2 = ignore full-screen *texrects* (>= 300x220). OB64's intro draws a
//           full-screen PRIM-alpha rect *after* its sprites and animates the
//           alpha from opaque to transparent, i.e. a fade-in from black;
//           skipping it shows the frame at full brightness immediately. The
//           fill rect (the clear) is deliberately NOT skipped - dropping both
//           just makes frames accumulate again, which is the bug this
//           diagnostic was used to find.
//   bit 13 = disable the framebuffer-as-texture source copy, so a texture load
//           whose source is a drawn colour image reads RDRAM (the pre-session-193
//           path). The live A/B for that copy.
//   bit 5 = `ogre_gfx_test_draw` also renders into a colour image and loads a
//           tile back out of that same address, the positive control for the
//           `[GFX-FBTEX]` diagnostic (see `fb_written_`);
//   bit 6 = two negative controls on top of bit 5's image: a load in a drawn
//           row and outside every drawn column, and a load in a row the fill
//           never drew (`test_draw`, `testdraw.cjs --flags 96`).
std::atomic<uint32_t> g_debug_flags{0};

std::atomic<uint64_t> g_exec_cmd{0};      // command index within the current DL
std::atomic<uint32_t> g_exec_off{0};      // rdram offset of that command
std::atomic<uint32_t> g_exec_op{0};       // its opcode byte
std::atomic<uint32_t> g_exec_task{0};     // task number being executed
std::atomic<uint32_t> g_exec_draws{0};    // DrawCmds queued so far
std::atomic<uint32_t> g_flush_ok{0};      // flushes that acquired the lock
std::atomic<uint32_t> g_flush_skip{0};    // flushes skipped because the gfx thread was busy
std::atomic<uint32_t> g_flush_cmds{0};    // DrawCmds presented to GL
std::atomic<uint32_t> g_load_seq{0};      // load_tile_texture() calls started
std::atomic<uint32_t> g_load_done{0};     // ... finished
std::atomic<uint32_t> g_load_w{0}, g_load_h{0};   // requested texel rect
std::atomic<uint32_t> g_load_siz{0}, g_load_fmt{0}, g_load_timgw{0}, g_load_addr{0};

void refresh_stats_snapshot() {
    std::string summary = gbi::format_summary(g_workload);
    snprintf(g_stats_snapshot, sizeof(g_stats_snapshot), "%s", summary.c_str());
}

// ============================================================================
// Small row-vector math (matches the RSP's M·v convention with row-major
// storage, exactly like the N64 FixedMatrix format).
// ============================================================================

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[16];  // row-major

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};

// Row-vector transform: v' = v · M.
inline Vec4 mul(const Vec4& v, const Mat4& M) {
    Vec4 r;
    r.x = v.x * M.m[0] + v.y * M.m[4] + v.z * M.m[8] + v.w * M.m[12];
    r.y = v.x * M.m[1] + v.y * M.m[5] + v.z * M.m[9] + v.w * M.m[13];
    r.z = v.x * M.m[2] + v.y * M.m[6] + v.z * M.m[10] + v.w * M.m[14];
    r.w = v.x * M.m[3] + v.y * M.m[7] + v.z * M.m[11] + v.w * M.m[15];
    return r;
}

// ============================================================================
// F3DEX2/RDP state + command execution
// ============================================================================

constexpr int kMaxVertices = 32;
constexpr int kMaxTiles = 8;

// A vertex as stored in rdram (16 bytes, game endianness via rd32 pairs).
struct RawVertex {
    int16_t x, y, z;
    uint16_t flag;
    int16_t s, t;
    uint8_t r, g, b, a;
};

struct LoadedImage {
    uint64_t key = 0;                 // GL texture-cache key; 0 = nothing loaded
    uint64_t decode_key = 0;          // source key: TMEM bytes + tile addressing
    int width = 0, height = 0;        // loaded rect size in texels
    int origin_s = 0, origin_t = 0;   // tile uls/ult in texels (UV origin)
    // OGRE_TEXWINCLAMP: the tile rect in texels the sampler bounds a wrapped
    // coordinate to (0 = no bound, the default). See ensure_tile_image.
    int win_w = 0, win_h = 0;
    // The decoded colours of the first decodes of a run, packed 0xRRGGBB with
    // their texel counts. Filled only while `OGRE_TEX_TRACE` is on and for the
    // first few loads, so a draw's decode is answerable from the log.
    std::vector<std::pair<uint32_t, uint32_t>> hist;
};

struct TileState {
    uint8_t fmt, siz;
    uint16_t line, tmem;
    uint8_t palette;
    uint8_t cmt, cms;    // clamp/mirror t/s
    uint8_t maskt, masks;
    uint8_t shiftt, shifts;
    uint16_t uls, ult, lrs, lrt;
    // The image this tile *samples*: decoded from the source the last tile load
    // read, at THIS tile's fmt/siz/line over THIS tile's rect. This is what the
    // RDP actually samples (it reads TMEM with the render tile's format), and it
    // is why the load's own format/rect is not the right thing to decode with:
    // OB64 loads its 32x34 I4 mask as 16 bytes/row of 8-bit "texels" (the
    // standard gDPLoadTextureBlock idiom), so the load rect is 16 wide and the
    // sampled image is 32 wide.
    uint32_t image_address = 0;       // source image address from the last load
    int image_stride = 0;             // source row stride in bytes (SETTIMG)
    bool image_source_valid = false;
    LoadedImage image;                // decoded image (filled lazily at draw time)
    bool image_valid = false;         // `image` matches the current rect/format
    // Mean of the last decoded image, filled only under OGRE_TEX_TRACE. It is
    // the number `OGRE_TEX_TRACE_COLOUR` prints on the native renderer, so a
    // browser frame and an RT64 frame of the same screen can be compared line
    // by line.
    float trace_mean = 0.0f;
    unsigned trace_opaque = 0;
};

// RDP cycle types (OTHERMODE_H bits 20-21).
constexpr uint32_t kCyc1 = 0, kCyc2 = 1, kCycCopy = 2, kCycFill = 3;

// Combiner input selectors. The numeric values deliberately equal RT64's
// ColorCombiner::ColorInput / AlphaInput enum values
// (tools/RT64/src/shared/rt64_color_combiner.h) so the GLSL switch in
// kFragmentShaderSrc and the log names below cannot drift from the reference.
enum ColorSel {
    CSEL_COMBINED = 0, CSEL_TEXEL0, CSEL_TEXEL1, CSEL_PRIMITIVE, CSEL_SHADE,
    CSEL_ENVIRONMENT, CSEL_KEY_CENTER, CSEL_KEY_SCALE, CSEL_COMBINED_ALPHA,
    CSEL_TEXEL0_ALPHA, CSEL_TEXEL1_ALPHA, CSEL_PRIMITIVE_ALPHA, CSEL_SHADE_ALPHA,
    CSEL_ENV_ALPHA, CSEL_LOD_FRACTION, CSEL_PRIM_LOD_FRAC, CSEL_NOISE, CSEL_K4,
    CSEL_K5, CSEL_ONE, CSEL_ZERO
};
enum AlphaSel {
    ASEL_COMBINED = 0, ASEL_TEXEL0, ASEL_TEXEL1, ASEL_PRIMITIVE, ASEL_SHADE,
    ASEL_ENVIRONMENT, ASEL_LOD_FRACTION, ASEL_PRIM_LOD_FRAC, ASEL_ONE, ASEL_ZERO
};

const char* const kColorSelNames[] = {
    "COMBINED", "TEXEL0", "TEXEL1", "PRIM", "SHADE", "ENV", "KEY_CENTER",
    "KEY_SCALE", "COMBINED_A", "TEXEL0_A", "TEXEL1_A", "PRIM_A", "SHADE_A",
    "ENV_A", "LOD_FRAC", "PRIM_LOD", "NOISE", "K4", "K5", "ONE", "ZERO"
};
const char* const kAlphaSelNames[] = {
    "COMBINED", "TEXEL0", "TEXEL1", "PRIM", "SHADE", "ENV", "LOD_FRAC",
    "PRIM_LOD", "ONE", "ZERO"
};
inline const char* color_sel_name(int s) {
    return (s >= 0 && s <= CSEL_ZERO) ? kColorSelNames[s] : "?";
}
inline const char* alpha_sel_name(int s) {
    return (s >= 0 && s <= ASEL_ZERO) ? kAlphaSelNames[s] : "?";
}

// RT64 ColorCombiner::colorInputA/B/C/D and alphaInputABD/C: what a selector
// means depends on WHICH of A/B/C/D consumes it. Decoding selectors
// position-independently (the session-19 approximation) is wrong for the top
// of every field: color selector 6 is ONE for A and D, KEY_CENTER for B and
// KEY_SCALE for C, and selectors >= 8 name alpha-ish sources that only C
// understands.
constexpr int cs_a(int i) {
    return i <= 5 ? i : (i == 6 ? CSEL_ONE : (i == 7 ? CSEL_NOISE : CSEL_ZERO));
}
constexpr int cs_b(int i) {
    return i <= 5 ? i : (i == 6 ? CSEL_KEY_CENTER : (i == 7 ? CSEL_K4 : CSEL_ZERO));
}
constexpr int cs_c(int i) {
    return i <= 5 ? i
         : i == 6 ? CSEL_KEY_SCALE
         : i == 7 ? CSEL_COMBINED_ALPHA
         : i == 8 ? CSEL_TEXEL0_ALPHA
         : i == 9 ? CSEL_TEXEL1_ALPHA
         : i == 10 ? CSEL_PRIMITIVE_ALPHA
         : i == 11 ? CSEL_SHADE_ALPHA
         : i == 12 ? CSEL_ENV_ALPHA
         : i == 13 ? CSEL_LOD_FRACTION
         : i == 14 ? CSEL_PRIM_LOD_FRAC
         : i == 15 ? CSEL_K5
         : CSEL_ZERO;   // RT64 colorInputC default: indexes 16-31 are unused
}
constexpr int cs_d(int i) { return i <= 5 ? i : (i == 6 ? CSEL_ONE : CSEL_ZERO); }
constexpr int as_abd(int i) { return i <= 5 ? i : (i == 6 ? ASEL_ONE : ASEL_ZERO); }
constexpr int as_c(int i) { return i <= 5 ? i : (i == 6 ? ASEL_PRIM_LOD_FRAC : ASEL_ZERO); }

// Decoded 2-cycle combiner mux (RT64 ColorCombiner). `ra`-`rd` keep the raw
// bitfields for diagnostics; `ca`-`ad` hold the decoded CSEL_/ASEL_ selectors.
struct Combiner {
    int ra[2] = {}, rb[2] = {}, rc[2] = {}, rd[2] = {};
    int ca[2] = {}, cb[2] = {}, cc[2] = {}, cd[2] = {};   // CSEL_*
    int aa[2] = {}, ab[2] = {}, ac[2] = {}, ad[2] = {};   // ASEL_*
    uint32_t L = 0, H = 0;   // raw G_SETCOMBINE words (diagnostics)
    bool valid = false;

    // Bit layout per RT64 ColorCombiner::parseColorInputA..D and
    // parseAlphaInputA..D (tools/RT64/src/shared/rt64_color_combiner.h).
    void decode(uint32_t L, uint32_t H) {
        for (int cyc = 0; cyc < 2; cyc++) {
            const bool sc = cyc == 1;   // second cycle
            ra[cyc] = sc ? (L >> 5) & 0xF : (L >> 20) & 0xF;
            rb[cyc] = sc ? (H >> 24) & 0xF : (H >> 28) & 0xF;
            rc[cyc] = sc ? (L >> 0) & 0x1F : (L >> 15) & 0x1F;
            rd[cyc] = sc ? (H >> 6) & 0x7 : (H >> 15) & 0x7;
            ca[cyc] = cs_a(ra[cyc]);
            cb[cyc] = cs_b(rb[cyc]);
            cc[cyc] = cs_c(rc[cyc]);
            cd[cyc] = cs_d(rd[cyc]);
            const int a_a = sc ? (H >> 21) & 0x7 : (L >> 12) & 0x7;
            const int a_b = sc ? (H >> 3) & 0x7 : (H >> 12) & 0x7;
            const int a_c = sc ? (H >> 18) & 0x7 : (L >> 9) & 0x7;
            // Alpha D reads H in both cycles (RT64 parseAlphaInputD); reading
            // L for the first cycle made alpha D mirror alpha C.
            const int a_d = sc ? (H >> 0) & 0x7 : (H >> 9) & 0x7;
            aa[cyc] = as_abd(a_a);
            ab[cyc] = as_abd(a_b);
            ac[cyc] = as_c(a_c);
            ad[cyc] = as_abd(a_d);
        }
        this->L = L;
        this->H = H;
        valid = true;
    }
};

// Renders one combiner cycle as its N64 expression, so a [GFX-CMD] line can be
// compared directly against the RDP reference or the game's own DL words.
inline void format_combiner(const Combiner& cb, int cyc, char* out, size_t n) {
    snprintf(out, n, "rgb=(%s-%s)*%s+%s a=(%s-%s)*%s+%s raw=[%d %d %d %d]",
             color_sel_name(cb.ca[cyc]), color_sel_name(cb.cb[cyc]),
             color_sel_name(cb.cc[cyc]), color_sel_name(cb.cd[cyc]),
             alpha_sel_name(cb.aa[cyc]), alpha_sel_name(cb.ab[cyc]),
             alpha_sel_name(cb.ac[cyc]), alpha_sel_name(cb.ad[cyc]),
             cb.ra[cyc], cb.rb[cyc], cb.rc[cyc], cb.rd[cyc]);
}

// RT64 ColorCombiner::usesTexture(otherMode, index, hardwareBugCheck = false).
// The texture unit a draw actually samples is a property of the mux the
// hardware evaluates, not of the tile state: a rectangle whose combiner never
// names TEXEL0/TEXEL1 (a G_CYC_FILL-style solid colour, or the publisher
// stills' PRIM-only background bands) draws without any texture at all. A tile
// that was never configured (`line == 0`) still has to be drawn in that case,
// and dropping it removes whatever the primitive colour paints (session 179:
// the QUEST still's white background). Copy mode reads TEXEL0; one-cycle mode
// evaluates the second mux and treats a TEXEL1 reference as TEXEL0 (RT64's
// "TEXEL1 is not supported in one cycle mode" note); two-cycle mode checks both
// cycles, with the TEXEL0/TEXEL1 roles swapped on the second.
inline bool combiner_uses_texture(const Combiner& cb, uint32_t othermode_h, int index) {
    if (!cb.valid) {
        return true;   // no combiner yet: keep the old "needs a texture" behaviour
    }
    const uint32_t cyc = (othermode_h >> 20) & 3u;
    if (cyc == kCycCopy) {
        return index == 0;
    }
    auto colour = [&cb](int cycle, int i) -> int {
        switch (i) {
            case 0: return cb.ca[cycle];
            case 1: return cb.cb[cycle];
            case 2: return cb.cc[cycle];
            default: return cb.cd[cycle];
        }
    };
    auto alpha = [&cb](int cycle, int i) -> int {
        switch (i) {
            case 0: return cb.aa[cycle];
            case 1: return cb.ab[cycle];
            case 2: return cb.ac[cycle];
            default: return cb.ad[cycle];
        }
    };
    if (cyc == kCyc1) {
        if (index != 0) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            const int s = colour(1, i);
            if (s == CSEL_TEXEL0 || s == CSEL_TEXEL1 || s == CSEL_TEXEL0_ALPHA ||
                s == CSEL_TEXEL1_ALPHA) {
                return true;
            }
        }
        for (int i = 0; i < 4; ++i) {
            const int s = alpha(1, i);
            if (s == ASEL_TEXEL0 || s == ASEL_TEXEL1) {
                return true;
            }
        }
        return false;
    }
    for (int c = 0; c < 2; ++c) {
        const bool second = (c == 1);
        const int tex = second ? (index == 0 ? CSEL_TEXEL1 : CSEL_TEXEL0)
                               : (index == 0 ? CSEL_TEXEL0 : CSEL_TEXEL1);
        const int tex_alpha = second ? (index == 0 ? CSEL_TEXEL1_ALPHA : CSEL_TEXEL0_ALPHA)
                                     : (index == 0 ? CSEL_TEXEL0_ALPHA : CSEL_TEXEL1_ALPHA);
        const int atex = second ? (index == 0 ? ASEL_TEXEL1 : ASEL_TEXEL0)
                                : (index == 0 ? ASEL_TEXEL0 : ASEL_TEXEL1);
        for (int i = 0; i < 4; ++i) {
            const int s = colour(c, i);
            if (s == tex || s == tex_alpha) {
                return true;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (alpha(c, i) == atex) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// RT64 Blender (tools/RT64/src/shared/rt64_blender.h)
// ============================================================================
//
// OTHERMODE_L bits 16-31 hold the blender's four 2-bit inputs for each cycle,
// packed as `P1 P0 A1 A0 M1 M0 B1 B0` (RT64 `OtherMode::blenderInputs`). The
// blender turns the colour combiner's output into the pixel that is written,
// and - when it reads the framebuffer - produces the alpha factor that mixes
// that pixel with what is already there.
//
// Until session 23 this renderer guessed `blend` from `othermode_l & 0xFFF`
// with a two-entry heuristic, which read completely the wrong bits: OB64's
// title sprites (oml=0x00184240, FORCE_BL set, M1=FRAMEBUFFER_COLOR,
// B1=ONE_MINUS_A, A1=CC_ALPHA) were drawn with blending disabled, so the I8
// alpha mask the combiner puts in TEXEL0's alpha was thrown away and every
// sprite's transparent texels were written as opaque black - which is exactly
// the "fragmented soldiers" the title scene showed.
enum BlendPM { BPM_CC = 0, BPM_FRAMEBUFFER_COLOR, BPM_BLEND_COLOR, BPM_FOG_COLOR };
enum BlendA { BA_CC_ALPHA = 0, BA_FOG_ALPHA, BA_SHADE_ALPHA, BA_ZERO };
enum BlendB { BB_ONE_MINUS_A = 0, BB_FRAMEBUFFER_ALPHA, BB_ONE, BB_ZERO };
enum BlendApprox { BAPPROX_NONE = 0, BAPPROX_SQUARE_MIX, BAPPROX_MULTIPLY_MIX };

struct Blender {
    uint16_t inputs = 0;
    int p[2] = {}, m[2] = {}, a[2] = {}, b[2] = {};   // P/M/A/B per cycle
    int approx = BAPPROX_NONE;      // Blender::Approximation
    bool force_blend = false;       // OTHERMODE_L FORCE_BL
    int cycles = 0;                 // combineCycleCount (0 = FILL/COPY)
    int blend_cycles = 0;           // blendCycleCount
    bool alpha_blend = false;       // Blender::usesAlphaBlend
    uint32_t L = 0;

    static int dec_p(uint16_t bi, int c) { return c ? (bi >> 12) & 3 : (bi >> 14) & 3; }
    static int dec_m(uint16_t bi, int c) { return c ? (bi >> 4) & 3 : (bi >> 6) & 3; }
    static int dec_a(uint16_t bi, int c) { return c ? (bi >> 8) & 3 : (bi >> 10) & 3; }
    static int dec_b(uint16_t bi, int c) { return c ? (bi >> 0) & 3 : (bi >> 2) & 3; }

    void decode(uint32_t othermode_l, uint32_t othermode_h) {
        L = othermode_l;
        inputs = static_cast<uint16_t>((othermode_l >> 16) & 0xFFFF);
        for (int c = 0; c < 2; ++c) {
            p[c] = dec_p(inputs, c);
            m[c] = dec_m(inputs, c);
            a[c] = dec_a(inputs, c);
            b[c] = dec_b(inputs, c);
        }
        force_blend = (othermode_l & 0x4000u) != 0;   // FORCE_BL
        const uint32_t cyc = (othermode_h >> 20) & 3;
        cycles = (cyc == kCyc2) ? 2 : (cyc == kCyc1 ? 1 : 0);
        blend_cycles = force_blend ? cycles : (cycles > 0 ? cycles - 1 : 0);
        approx = check_approximation();
        alpha_blend = compute_uses_alpha_blend();
    }

    // Blender::checkEmulationRequirements(): can the blender be evaluated in a
    // shader without ever reading the framebuffer colour?
    int check_approximation() const {
        struct CycReq { bool passthrough = false, numerator_overflow = false, fb = false; };
        CycReq req[2];
        for (int c = 0; c < blend_cycles && c < 2; ++c) {
            const bool any_zero = (a[c] == BA_ZERO) || (b[c] == BB_ZERO);
            const bool dup_1ma = (p[c] == m[c]) && (b[c] == BB_ONE_MINUS_A);
            if (any_zero || dup_1ma) req[c].passthrough = true;
            else if (b[c] != BB_ONE_MINUS_A) req[c].numerator_overflow = true;
            if (p[c] == BPM_FRAMEBUFFER_COLOR || m[c] == BPM_FRAMEBUFFER_COLOR) req[c].fb = true;
        }
        bool simple = true;
        if (req[0].numerator_overflow && req[0].fb) {
            simple = false;
        } else if (blend_cycles == 2) {
            if (req[0].fb && !req[0].passthrough) simple = false;
            else if (req[1].numerator_overflow && req[1].fb) simple = false;
        }
        if (simple || blend_cycles != 2) {
            return BAPPROX_NONE;
        }
        if (p[0] == BPM_CC && m[0] == BPM_FRAMEBUFFER_COLOR && a[0] == BA_CC_ALPHA &&
            b[0] == BB_ONE_MINUS_A && p[1] == BPM_CC && m[1] == BPM_FRAMEBUFFER_COLOR &&
            a[1] == BA_CC_ALPHA && b[1] == BB_ONE_MINUS_A) {
            return BAPPROX_SQUARE_MIX;   // CombinerFramebuffer1MA_SquareMix
        }
        if (p[0] != BPM_FRAMEBUFFER_COLOR && m[0] == BPM_FRAMEBUFFER_COLOR &&
            b[0] == BB_ONE_MINUS_A && p[1] == BPM_CC && m[1] == BPM_FRAMEBUFFER_COLOR &&
            b[1] == BB_ONE_MINUS_A) {
            return BAPPROX_MULTIPLY_MIX;  // AnyFramebuffer1MA_MultiplyMix
        }
        return BAPPROX_NONE;
    }

    // Blender::usesAlphaBlend(): does any *evaluated* cycle read the framebuffer?
    bool compute_uses_alpha_blend() const {
        auto uses_cycle = [&](int c, bool all_inputs) {
            if (all_inputs) {
                if (p[c] == BPM_FRAMEBUFFER_COLOR && a[c] != BA_ZERO) return true;
                if (m[c] == BPM_FRAMEBUFFER_COLOR && b[c] != BB_ZERO) return true;
                return false;
            }
            return p[c] == BPM_FRAMEBUFFER_COLOR;
        };
        if (cycles >= 2 && uses_cycle(1, force_blend)) return true;
        if (cycles >= 1 && uses_cycle(0, (cycles >= 2) || force_blend)) return true;
        return false;
    }
};

// Names for the blender enums so a [GFX-CMD] line reads like the RDP reference.
const char* const kBlendPMNames[] = {"CC", "FB", "BLEND", "FOG"};
const char* const kBlendANames[] = {"CC_A", "FOG_A", "SHADE_A", "ZERO"};
const char* const kBlendBNames[] = {"1MA", "FB_A", "ONE", "ZERO"};
inline const char* blend_pm_name(int v) { return (v >= 0 && v <= 3) ? kBlendPMNames[v] : "?"; }
inline const char* blend_a_name(int v) { return (v >= 0 && v <= 3) ? kBlendANames[v] : "?"; }
inline const char* blend_b_name(int v) { return (v >= 0 && v <= 3) ? kBlendBNames[v] : "?"; }

// RT64's `ShiftScaleMap` (tools/RT64/src/hle/rt64_state.cpp:33-51): the scale a
// tile's G_TX shifts/shiftt field applies to the sampled coordinate,
// `uvCoord = inputUV * perspCorrectionMod * float2(shifts, shiftt)`
// (shaders/TextureSampler.hlsli:226). `State::setupDrawCall` stores the table
// value in `RDPTile::shifts`/`shiftt` (`hle/rt64_state.cpp:306-307`).
inline float tex_shift_scale(uint32_t shift) {
    static const float kShiftScaleMap[16] = {
        1.0f,       1.0f / 2.0f, 1.0f / 4.0f,  1.0f / 8.0f,
        1.0f / 16.0f, 1.0f / 32.0f, 1.0f / 64.0f, 1.0f / 128.0f,
        1.0f / 256.0f, 1.0f / 512.0f, 1.0f / 1024.0f, 32.0f,
        16.0f,      8.0f,       4.0f,         2.0f
    };
    return kShiftScaleMap[shift & 0xF];
}

// Rendering state carried across a display list.
enum class GbiKind {
    F3DEX2,
    // F3DEX2.NoN.fifo 2.08: the mission's 3D ucode. It is opcode-compatible
    // with F3DEX2 and RT64 maps it to GBIUCode::F3DEX2 with flags.NoN, which
    // only disables the F3D microcodes' near-depth clip (`depthClipEnabled =
    // !NoN`, render/rt64_raster_shader.cpp:315). The browser applied that clip
    // to the mission's terrain and frame geometry because the hash was not in
    // this table and the fallback chose plain F3DEX2.
    F3DEX2_NON_FIFO,
    S2DEX2
};

// The S2DEX2 opcodes that collide with F3DEX2's, from
// tools/RT64/src/gbi/rt64_gbi_s2dex2.h. `0xDC`/`0xDA` are F3DEX2's G_MOVEMEM and
// G_MTX; `0x01`/`0x02` are its G_VTX and G_MODIFYVTX; `0xE4` is its G_TEXRECT
// and S2DEX2's G_RDPHALF_0.
constexpr uint8_t S2D_OBJ_RECTANGLE = 0x01;
constexpr uint8_t S2D_OBJ_SPRITE = 0x02;
constexpr uint8_t S2D_OBJ_RECTANGLE_R = 0xDA;
constexpr uint8_t S2D_OBJ_MOVEMEM = 0xDC;
constexpr uint8_t S2D_RDPHALF_0 = 0xE4;

// F3DEX2 geometry-mode bits this renderer evaluates. G_LIGHTING, G_FOG and
// G_ZBUFFER are RT64 `shared/rt64_f3d_defines.h:100-103`. G_SHADE (0x00000004)
// has no arm: shading is always smooth.
constexpr uint32_t kGeomCullFront = 0x00000200;  // G_CULL_FRONT
constexpr uint32_t kGeomCullBack = 0x00000400;   // G_CULL_BACK
constexpr uint32_t kGeomCullBoth = kGeomCullFront | kGeomCullBack;
constexpr uint32_t kGeomFog = 0x00010000;        // G_FOG
constexpr uint32_t kGeomLighting = 0x00020000;   // G_LIGHTING
constexpr uint32_t kGeomTextureGen = 0x00040000;        // G_TEXTURE_GEN
constexpr uint32_t kGeomTextureGenLinear = 0x00080000;  // G_TEXTURE_GEN_LINEAR

struct RenderState {
    // Matrices (RSP row-vector convention).
    Mat4 projection = Mat4::identity();
    std::vector<Mat4> modelview_stack{Mat4::identity()};

    // Geometry mode (G_GEOMETRYMODE/G_CLEARGEOMETRYMODE + OTHERMODE_H bits).
    uint32_t geometry_mode = 0;
    uint32_t othermode_h = 0;  // 24-bit; cycle type at bits 20-21
    uint32_t othermode_l = 0;

    // RSP lights. F3DEX2 has no light command of its own: G_MOVEMEM index 10
    // (MV_LIGHT) writes one entry, and G_MOVEWORD sets the count and a colour.
    // RT64 keeps `std::array<Light, RSP_MAX_LIGHTS + 1> lights`
    // (tools/RT64/src/hle/rt64_rsp.h:163) and treats entry `num_lights` as the
    // ambient term (RSPProcessCS.hlsl:92 `ambientIndex = lightIndex +
    // lightCount - 1`), so 7 directional/point lights plus ambient.
    static constexpr int kMaxLights = 8;       // RT64 RSP_MAX_LIGHTS (7) + ambient
    static constexpr int kLightStride = 24;    // G_MOVEMEM slot = offset/24
    // The 24 raw bytes of each slot, already in N64 byte order (copied with
    // n64b at G_MOVEMEM time). RT64 memcpy's sizeof(Light) = 16 bytes
    // (RSP::setLight); the trailing 8 bytes are kept so the slot stride and the
    // stored image agree, and are never read.
    uint8_t lights[kMaxLights][kLightStride]{};
    int num_lights = 0;
    // G_MOVEMEM MV_LIGHT slots 0/1: the texture-generation lookat vectors
    // (RT64 `RSP::setLookAt`, hle/rt64_rsp.cpp:969-982). RT64 normalizes the
    // 3-vector at logical bytes 8/9/10 of the slot and keeps it per RSP state;
    // `computeTextureGen` projects the vertex normal onto it. Slot 0 is X, slot
    // 1 is Y, and a zero vector stays zero.
    float lookat[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    // G_MOVEWORD G_MW_FOG: the fog vector RT64 keeps per RSP state. Stored so
    // the command is decoded where RT64 decodes it; no per-vertex fog alpha is
    // applied (see transform_vertex).
    int32_t fog_mul = 0, fog_offset = 0;

    // Vertices.
    RawVertex vertices[kMaxVertices]{};

    // Colors (ABGR bytes -> RGBA floats).
    float prim_color[4] = {1, 1, 1, 1};
    float env_color[4] = {1, 1, 1, 1};
    float fill_color[4] = {0, 0, 0, 1};
    // The G_SETFILLCOLOR word itself. The fill colour is the one RDP colour
    // register whose value depends on the *colour image's* format: a 16-bit
    // image takes the low 16 bits as RGBA5551, a 32-bit image the whole word as
    // RGBA8888 (see draw_fill_rect). `fill_color` keeps the 32-bit decode for
    // the canvas path and for the trace.
    uint32_t fill_color_word = 0;
    // Blender inputs (G_SETFOGCOLOR / G_SETBLENDCOLOR).
    float fog_color[4] = {0, 0, 0, 1};
    float blend_color[4] = {0, 0, 0, 0};
    // G_SETKEYR / G_SETKEYGB: the chroma-key centre and scale, 0..1. RT64
    // `RDP::setKeyR`/`setKeyGB` divide each 8-bit field by 255
    // (hle/rt64_rdp.cpp:1223-1236); the width fields are ignored. They are the
    // combiner's KEY_CENTER / KEY_SCALE inputs (RasterPS.hlsl:185-186,
    // shared/rt64_color_combiner.h:482-485).
    float key_center[3] = {0, 0, 0};
    float key_scale[3] = {0, 0, 0};

    // Scissor (integer pixels, inclusive bounds).
    bool scissor_enabled = false;
    int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 0, scissor_y1 = 0;

    // RSP viewport (Vp_t, G_MOVEMEM MV_VIEWPORT). vscale/vtrans are 14.2
    // fixed-point screen pixels; these hold the already-divided values.
    // Until the game sends one, transform_vertex() falls back to NDC
    // passthrough so display lists that never set a viewport still draw.
    bool viewport_set = false;
    float view_scale[3] = {1.0f, 1.0f, 1.0f};
    float view_trans[3] = {0.0f, 0.0f, 0.0f};

    // Combiner.
    Combiner combiner;

    // Which GBI this display list belongs to (see identify_gbi). In an S2DEX2
    // list 0xDA/0xDC are the object-rectangle and object-matrix commands.
    GbiKind gbi = GbiKind::F3DEX2;

    // G_SETCONVERT: the YUV -> RGB matrix the RDP uses for YUV16 texels. Raw
    // 9-bit fields, sign-extended and scaled `2*K+1` at use time.
    int32_t convert_k[6] = {0, 0, 0, 0, 0, 0};

    // S2DEX2 object transform (G_OBJ_MOVEMEM). RT64 resets it to the identity
    // at the start of every task, so it lives in the per-DL state.
    struct ObjMtx {
        int32_t a = 0x10000, b = 0, c = 0, d = 0x10000;
        int16_t x = 0, y = 0;
        uint16_t base_scale_x = 1024, base_scale_y = 1024;
        bool sub_mtx = false;
    } obj_mtx;

    // Blender / render mode (OTHERMODE_L bits 16-31).
    Blender blender;

    // Texture state.
    uint32_t timg_address = 0;
    uint8_t timg_fmt = 0, timg_siz = 0;
    uint16_t timg_width = 0;
    TileState tiles[kMaxTiles]{};
    int active_tile = 0;          // G_TEXTURE tile
    int32_t tex_sc = 0, tex_tc = 0;  // G_TEXTURE scale (unsigned 16-bit fields)
    // G_TEXTURE scale as a multiplier: RT64 computes
    //   tc = (s * sc) / (65536 * 32)
    // (RSP::setVertexCommon, `Divisor = 65536.0f * 32.0f`), i.e. the raw 16-bit
    // scale over 65536 multiplies the s10.5 texel coordinate. OB64's title
    // sprites use sc = tc = 0x8000 (0.5), so ignoring it doubled every texture
    // coordinate and smeared the sprites.
    float tex_scale_s = 1.0f, tex_scale_t = 1.0f;
    bool texture_on = false;      // G_TEXTURE "on" field

    // The tile load awaiting its render-tile configuration. G_LOADTILE /
    // G_LOADBLOCK always name tile 7 (G_TX_LOADTILE) whatever the game does; the
    // F3DEX2 texture-load idiom then re-configures the *render* tile (0, 1, ...)
    // with G_SETTILE + G_SETTILESIZE, and that tile is what samples the loaded
    // image. So a load is claimed by the next SETTILE that names a different
    // tile; the claimed address becomes that tile's image source.
    struct PendingLoad {
        uint32_t address = 0;
        int load_tile = -1;
        // The source image's row stride is the SETTIMG width converted with the
        // *image's* format: RT64 `loadTileOperation` uses
        // `bytesPerRow = width << siz >> 1`. It is not the tile's `line`, which
        // is the TMEM stride the RDP writes with (OB64's mask: 16 bytes/row of
        // 8-bit texels, sampled as 32 4-bit texels - `line` is 2 words = 16
        // bytes there, so the two agree, but they need not).
        int stride = 0;
        bool valid = false;
    } pending_load;

    // G_SETCIMG: the RDP colour image, i.e. the render target for every draw
    // that follows it. It is not a texture: the RDP writes pixels into this
    // guest buffer, and the game reads them back with the CPU (the njpeg
    // backdrop assembly copies the framebuffer into its own image). The main
    // thread keeps a GL framebuffer object per guest address, and
    // ogre_sync_framebuffers() writes the rendered pixels back into RDRAM.
    //
    // A zero address means no colour image has been set yet; those draws go to
    // the canvas, which is how this prototype drew everything before.
    uint32_t cimg_address = 0;
    uint32_t cimg_row_bytes = 0;   // the image's row pitch: width << siz >> 1
    uint16_t cimg_width = 0;       // the image's width in texels (320 or 640)
    uint8_t cimg_siz = 0;          // 2 = 16b RGBA5551, 3 = 32b RGBA8888
    uint8_t cimg_fmt = 0;          // G_IM_FMT_RGBA for every image OB64 sets

    // G_SETZIMG: the RDP depth image. RT64 keeps it in `RDP::depthImage` and
    // allocates the depth buffer from it (hle/rt64_rdp.cpp `setDepthImage`,
    // `maskAddress(address)` = `address & 0xFFFFFF`; the fmt/siz/width fields
    // are the same shape as G_SETCIMG's). This renderer has no RDRAM depth
    // image: the depth buffer is a GL renderbuffer owned by the target's
    // GlFramebuffer, and these fields are recorded on it for the diagnostics.
    uint32_t zimg_address = 0;
    uint16_t zimg_width = 0;
    uint8_t zimg_siz = 0;
    uint8_t zimg_fmt = 0;

    // G_SETPRIMDEPTH: RT64 `RDP::setPrimDepth` (hle/rt64_rdp.cpp:1160):
    // `primDepth.x = (z & 0x7FFF) / 32767` (0..1 screen depth) and
    // `primDepth.y = (dz & 0xFFFF) / 65535` (the decal tolerance, unused here
    // because ZMODE_DEC is not implemented).
    float prim_depth = 0.0f;
    float prim_depth_dz = 0.0f;
};

// Reads a 32-bit word from rdram with the runtime's byte-reversed storage.
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 0) | (p[1] << 8));
}

// The runtime's rdram stores each N64 32-bit word byte-reversed (recomp.h:
// MEM_W is a native little-endian load at the logical address, MEM_H adds
// `^ 2`, MEM_B `^ 3`). So the 16-bit field at logical offset `off` inside a
// 4-byte word is read at `off ^ 2`, not at `off`.
//
// Reading a field at its logical offset returns the OTHER half of the word.
// For an F3DEX2 Vtx - (x,y)(z,flag)(s,t)(r,g,b,a) - that meant the renderer
// was decoding (y,x)(flag,z)(t,s)(a,b,g,r) and the title sprites were drawn
// with x/y swapped and their texture coordinates transposed. read_matrix() has
// always compensated with its `c ^ 1` column index (equivalent to `^ 2` on the
// offset); vertex reads did not, and neither did the texture decoder - which
// needs the rule at byte granularity, not at halfword offsets (see n64be16).
inline uint16_t n64h(const uint8_t* p, int off) { return rd16(p + (off ^ 2)); }
inline uint8_t n64b(const uint8_t* p, int off) { return p[off ^ 3]; }

// Where a TMEM load reads its source bytes. Normally RDRAM. A load whose source
// is a colour image the RDP has drawn reads a private copy of that image's
// rendered pixels instead (`WebGLRenderer::fetch_fb_source`), because those
// pixels are still in a GL framebuffer object. The copy is in the same physical
// byte order as RDRAM (see n64b), so the same accessor reads both.
struct LoadSource {
    const uint8_t* rdram = nullptr;
    const uint8_t* shadow = nullptr;
    uint32_t shadow_origin = 0;
    uint32_t shadow_len = 0;
    uint8_t byte(uint32_t addr) const {
        if (shadow != nullptr && addr >= shadow_origin &&
            (addr - shadow_origin) < shadow_len) {
            return n64b(shadow, static_cast<int>(addr - shadow_origin));
        }
        return n64b(rdram, static_cast<int>(addr));
    }
};

// Texture/TLUT reads need the same compensation, at *byte* granularity, and
// `n64h` is not it: a texture's rows are not 4-byte aligned fields, so the byte
// rule has to be applied to every byte index. RT64's TMEM load is the reference
// (hle/rt64_rdp.cpp `loadWord`): `TMEM[..] = RDRAM[(textureAddress + i) ^ 3]`.
// Reading a texture byte at its logical offset without the `^ 3` returns a
// different byte of the same word, which scrambles the image in two ways:
// a 16-bit texel comes back as its horizontal neighbour (a pair swap), and an
// I4/CI4 byte comes back from a different position in its 4-byte group (four
// bytes reversed = 8 texels per group).
inline uint16_t n64be16(const uint8_t* p, uint32_t addr) {
    return static_cast<uint16_t>((n64b(p, static_cast<int>(addr)) << 8) |
                                 n64b(p, static_cast<int>(addr) + 1));
}

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1);
}

// The executor and the DL walker share one resolver (gbi.hpp) so a G_DL
// target cannot mean two different addresses.
inline uint32_t resolve_address(const uint32_t* segments, uint32_t addr) {
    return gbi::resolve_address(segments, addr);
}

// ============================================================================
// The VI's scanout size
// ============================================================================
//
// `ultramodern::renderer::get_vi_regs()` holds the register values the runtime
// computed for the frame the VI is currently scanning out (events.cpp
// `update_vi`). The framebuffer the VI reads is not the RDP colour image's
// address: `fldRegs[].origin` puts it a row or two into the image, and its
// *height* is not a register either; it is derived from the vertical region and
// the y scale.
//
// This is a port of RT64's `VI::fbSize` (tools/RT64/src/hle/rt64_vi.cpp), which
// is what the native renderer sizes its framebuffers and its window with:
//
//   size.x = width; if interlaced and the horizontal region is narrow, width/2
//   size.y = (vEnd - vStart) / (2 * yScale * (size.x / width))
//   size.y += 2, then rounded down to a multiple of 4
//
// The +2/round-to-4 exists because the VI region is usually a few rows short of
// the image the RDP wrote. For OB64 the standard NTSC mode yields 320x240 and
// the world map's hi-res mode 640x480, which is what the game's own
// `G_SETSCISSOR`/`G_SETCIMG` pairs use (docs/HANDOFF-2026-10-14-session139.md
// §1). The bit layouts are RT64's `HRegion`/`VRegion`/`XTransform`/`YTransform`.
struct ViSize {
    int width = 0;
    int height = 0;
};

// RT64 `VI::visible`: a blanked mode (type 0) or one whose horizontal region
// starts at 0 does not update the VI history and so does not resize anything.
inline bool vi_visible(const ultramodern::renderer::ViRegs& r) {
    return ((r.VI_STATUS_REG & 3u) != 0u) && (((r.VI_H_START_REG >> 16) & 0x3FFu) > 0u);
}

inline ViSize vi_framebuffer_size(const ultramodern::renderer::ViRegs& r) {
    ViSize out;
    const uint32_t width = r.VI_WIDTH_REG;
    if (width == 0) {
        return out;   // no mode set yet
    }
    const uint32_t h_word = r.VI_H_START_REG;
    const uint32_t v_word = r.VI_V_START_REG;
    const uint32_t x_scale = r.VI_X_SCALE_REG & 0xFFFu;
    const uint32_t y_scale = r.VI_Y_SCALE_REG & 0xFFFu;
    if (x_scale == 0 || y_scale == 0) {
        return out;
    }
    const float x_scale_f = 1024.0f / static_cast<float>(x_scale);
    const float y_scale_f = 1024.0f / static_cast<float>(y_scale);
    const uint32_t h_start = (h_word >> 16) & 0x3FFu;
    const uint32_t h_end = h_word & 0x3FFu;
    const uint32_t v_start = (v_word >> 16) & 0x3FFu;
    const uint32_t v_end = v_word & 0x3FFu;

    int size_x = static_cast<int>(width);
    // Interlaced without deflickering doubles the framebuffer's row stride, so a
    // narrow horizontal region means the real image is half the declared width.
    if ((r.VI_STATUS_REG & 0x40u) != 0) {
        const float estimated = static_cast<float>(h_end - h_start) / x_scale_f;
        if (estimated < (static_cast<float>(width) / 1.875f)) {
            size_x = static_cast<int>(width) / 2;
        }
    }

    const float scale_ratio = static_cast<float>(size_x) / static_cast<float>(width);
    const float rows = static_cast<float>(v_end - v_start) / (2.0f * y_scale_f * scale_ratio);
    int size_y = static_cast<int>(std::lround(rows));
    if (size_x <= 0 || size_y <= 0) {
        return out;
    }
    size_y += 2;
    size_y = static_cast<int>(std::lround(static_cast<float>(size_y) / 4.0f)) * 4;
    out.width = size_x;
    out.height = size_y;
    return out;
}

// RT64 `VIRenderer::visibleFramebufferSize` (`src/render/rt64_vi_renderer.cpp`):
// the area of the framebuffer the presenter samples is the VI's `fbSize` minus
// an overscan crop (the game's scissor and clear stop one pixel short on the
// right and the bottom, so those pixels hold leftovers from an earlier scene).
// `OGRE_OVERSCAN=<0..8>` overrides it, the same knob the native renderer reads.
inline int overscan_crop() {
    static const int crop = [] {
        const char* v = getenv("OGRE_OVERSCAN");
        const int value = (v != nullptr) ? atoi(v) : 1;
        return std::clamp(value, 0, 8);
    }();
    return crop;
}

inline ViSize visible_framebuffer_size(const ViSize& fb_size) {
    const int crop = overscan_crop();
    ViSize out;
    out.width = (fb_size.width > crop) ? (fb_size.width - crop) : fb_size.width;
    out.height = (fb_size.height > crop) ? (fb_size.height - crop) : fb_size.height;
    return out;
}

// ============================================================================
// Which GBI does this task use?
// ============================================================================
//
// The game's tasks carry one of two ucodes, and two of their opcodes mean
// different things in each: 0xDA is G_MTX under F3DEX2 and G_OBJ_RECTANGLE_R
// under S2DEX2, and 0xDC is G_MOVEMEM under F3DEX2 and G_OBJ_MOVEMEM under
// S2DEX2 (docs/HANDOFF-2026-09-15-session51.md §3). Walking an S2DEX2 list as
// F3DEX2 is what made the njpeg backgrounds decode as one ignored `G_MTX` per
// macroblock and draw nothing.
//
// The choice is made by hashing the ucode text with XXH3-64, not by address:
// the runtime stores each guest 32-bit word byte-reversed in RDRAM, and RT64's
// database is keyed on exactly those bytes. Session 51 §2 reproduces these two
// constants from the ROM (`tools/RT64/src/gbi/rt64_gbi.cpp`'s textSegments):
//
//   ROM 0x35510, 0x18C0 -> 0x9300F34F3B438634  S2DEX2_FIFO_2_08
//   ROM 0x2F940, 0x1390 -> 0xCF55FAE288BFE48D  F3DEX2.FIFO 2.08
//
// The digest is over the *whole* text, so the length is part of the entry; the
// OSTask's own `ucode_size` is 0x1000 (one RSP IMEM load), which is why RT64's
// `GBIManager::getGBIForUCode` grows the hash segment by segment and compares
// the running digest at each database length instead of trusting that field.
struct GbiTextSegment {
    uint32_t length;
    uint64_t hash;
    GbiKind kind;
};
constexpr GbiTextSegment kGbiTextSegments[] = {
    {0x1390, 0xCF55FAE288BFE48Dull, GbiKind::F3DEX2},
    // RT64 rt64_gbi.cpp:229, `F3DEX2.NON_FIFO_2_08` (hash and length verified
    // against the mission's ucode at 0x800A08D0).
    {0x1390, 0x4C12DAE0534D7135ull, GbiKind::F3DEX2_NON_FIFO},
    {0x18C0, 0x9300F34F3B438634ull, GbiKind::S2DEX2},
};

// Identifies the GBI of `task` from its ucode text. Falls back to the ucode's
// guest address when the text hashes to no known value, so an unrecognised
// ucode still renders as something and the fallback is visible in the log.
GbiKind identify_gbi(const uint8_t* rdram, const OSTask* task) {
    const uint32_t base = static_cast<uint32_t>(task->t.ucode) & 0x1FFFFFFFu;
    if (rdram != nullptr) {
        for (const GbiTextSegment& seg : kGbiTextSegments) {
            if (base + seg.length > gbi::kRdramSize) {
                continue;
            }
            if (XXH3_64bits(rdram + base, seg.length) == seg.hash) {
                return seg.kind;
            }
        }
        static uint32_t logged = 0;
        if (logged < 4) {
            ++logged;
            // Print the two running digests, the way RT64's database reports an
            // unrecognised ucode, so the value can be looked up or added.
            char hashes[128];
            int o = 0;
            for (const GbiTextSegment& seg : kGbiTextSegments) {
                if (base + seg.length > gbi::kRdramSize || o >= static_cast<int>(sizeof(hashes)) - 40) {
                    continue;
                }
                o += snprintf(hashes + o, sizeof(hashes) - o, "0x%llX/len 0x%X ",
                              static_cast<unsigned long long>(
                                  XXH3_64bits(rdram + base, seg.length)),
                              seg.length);
            }
            OGRE_MILESTONE("WEB-GBI",
                           "unknown ucode 0x%08X (task ucode_size 0x%X) hashes %s; "
                           "using the address (0x800A5110 = S2DEX2, else F3DEX2)",
                           static_cast<unsigned>(task->t.ucode),
                           static_cast<unsigned>(task->t.ucode_size), hashes);
        }
    }
    return (static_cast<uint32_t>(task->t.ucode) == 0x800A5110u) ? GbiKind::S2DEX2
                                                                : GbiKind::F3DEX2;
}

// ============================================================================
// N64 texture decoding (rdram -> RGBA8)
// ============================================================================

int bytes_per_texel(uint8_t siz) {
    switch (siz) {
        case gbi::IM_SIZ_4b: return 0;  // handled specially
        case gbi::IM_SIZ_8b: return 1;
        case gbi::IM_SIZ_16b: return 2;
        case gbi::IM_SIZ_32b: return 4;
        default: return 1;
    }
}

// Decodes one RGBA16 texel (5/5/5/1). RT64's RGBA16ToFloat4 replicates the
// 5-bit fields with `(c << 3) | (c >> 2)` rather than scaling by 255/31; do the
// same so the two renderers agree bit for bit.
inline void rgba16_to_rgba8(uint16_t v, uint8_t out[4]) {
    const uint32_t r = (v >> 11) & 0x1F, g = (v >> 6) & 0x1F, b = (v >> 1) & 0x1F;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    out[3] = (v & 1) ? 255 : 0;
}

// Decodes one IA16 texel (8-bit intensity, 8-bit alpha).
inline void ia16_to_rgba8(uint16_t v, uint8_t out[4]) {
    const uint8_t i = static_cast<uint8_t>(v >> 8);
    const uint8_t a = static_cast<uint8_t>(v & 0xFF);
    out[0] = out[1] = out[2] = i;
    out[3] = a;
}

// One YUV16 texel -> RGBA8, using the game's own G_SETCONVERT matrix.
//
// The RDP converts a YUV tile to RGB with `G_SETCONVERT`'s six 9-bit fields,
// sign-extended and scaled `2*K+1` (GLideN64 `gDPSetConvert`, parallel-rdp
// `set_convert`), then
//
//     R = Y + K0*V'   G = Y + K1*U' + K2*V'   B = Y + K3*U'      (U'=U-128, V'=V-128)
//
// K1 pairs with U and K2 with V. `tools/RT64/src/shaders/TextureDecoder.hlsli`
// `sampleTMEMYUV16` is the reference this mirrors, including the fallback to
// the standard NTSC JPEG matrix (K0=175 K1=469 K2=423 K3=222) when the game has
// programmed none.
inline void yuv16_to_rgba8(uint8_t y, uint8_t u, uint8_t v, const int32_t* convert_k,
                           uint8_t out[4]) {
    auto sext9 = [](int32_t k) -> float {
        const int32_t m = k & 0x1FF;
        return static_cast<float>((m >= 256) ? (m - 512) : m);
    };
    // With no matrix programmed the hardware's standard NTSC JPEG coefficients
    // apply. RT64 tests the raw fields' sum, so match that test and not a
    // bitwise one.
    if ((convert_k[0] + convert_k[1] + convert_k[2] + convert_k[3]) == 0) {
        static const int32_t kJpeg[4] = {175, 469, 423, 222};
        convert_k = kJpeg;
    }
    const float kk0 = 2.0f * sext9(convert_k[0]) + 1.0f;
    const float kk1 = 2.0f * sext9(convert_k[1]) + 1.0f;
    const float kk2 = 2.0f * sext9(convert_k[2]) + 1.0f;
    const float kk3 = 2.0f * sext9(convert_k[3]) + 1.0f;
    const float yf = static_cast<float>(y);
    const float uf = static_cast<float>(u) - 128.0f;
    const float vf = static_cast<float>(v) - 128.0f;
    auto clamp255 = [](float c) {
        const float s = (c < 0.0f) ? 0.0f : ((c > 255.0f) ? 255.0f : c);
        return static_cast<uint8_t>(s + 0.5f);
    };
    out[0] = clamp255(yf + (kk0 * vf) / 256.0f);
    out[1] = clamp255(yf + (kk1 * uf + kk2 * vf) / 256.0f);
    out[2] = clamp255(yf + (kk3 * uf) / 256.0f);
    out[3] = 255;
}

// Decoding a tile's image. The RDP samples TMEM, not RDRAM: a load command
// copies its source into TMEM with one row stride and the render tile samples
// it with another (`line << 3`), so `decode_tmem_tile` reads TMEM at
// `tile.tmem << 3` with the tile's own `line`. RT64's `RDP::loadTile`/
// `loadBlock` (the writes) and `TextureDecoder.hlsli` (the reads) are the
// reference. Which load wrote a given TMEM base is not recorded, and does not
// need to be: a later load overwrites it in command order.
//
// `lut_mode` is the RDP's 2-bit texture LUT mode (`OTHERMODE_H` bits 14-15,
// RT64's `OtherMode::textLUT()`). It selects the decode for every entry of an
// indexed tile: 2 = `G_TT_RGBA16` (5/5/5/1), 3 = `G_TT_IA16` (8-bit intensity,
// 8-bit alpha). RT64's `TextureDecoder.hlsli` switches on this mode
// (`case G_TT_RGBA16: RGBA16ToFloat4(...)`, `case G_TT_IA16: IA16ToFloat16(...)`)
// and never on the entry's own bit 15. Using bit 15 instead decoded the
// publisher still's ATLUS strokes - palette entry `0x3AE8`, bit 15 clear, in a
// `G_TT_RGBA16` draw - as `ia16(0x3AE8)` = (58,58,58) where RT64 paints
// `rgba16(0x3AE8)` = (57,90,165); the red T's entry has bit 15 set and matched
// by accident.
//
// A CI tile with LUT mode 0 does not read a palette at all: RT64 reinterprets
// the loaded bytes as I8 (`rt64_framebuffer_manager.cpp`'s
// `(dstFmt == G_IM_FMT_CI) && (dstSiz == G_IM_SIZ_8b) && (tlutFormat == 0)`
// case, which runs `ANY8toI8` in `FbReinterpretCS.hlsl`), i.e. the byte is an
// intensity replicated into RGB and into alpha. Decoding such a tile as CI
// against an empty palette paints it black, which is what the title screen's
// 72x28 and 72x20 unit sprites did.

// One byte of a tile's TMEM image, following RT64's sampler
// (`TextureDecoder.hlsli` `implLoadTMEM` and `sampleTMEMWithConvert`): the tile
// base is `tmem << 3`, rows are `line << 3` bytes apart, a texel's byte offset
// inside its row is `(s << shift) >> 1`, and an odd row holds the two 4-byte
// halves of each 8-byte word swapped. The load wrote them swapped
// (`loadRowStep`'s `tmemXorMask ^= 4`) and the sampler reads them back, so a
// decode that reads TMEM directly applies the same swap.
inline uint8_t tmem_sample_byte(const uint8_t* tmem, uint32_t address, uint32_t stride,
                                uint32_t address_mask, int shift, int x, int y, int extra,
                                uint32_t or_address = 0) {
    const uint32_t rel = static_cast<uint32_t>(y) * stride +
                         ((static_cast<uint32_t>(x) << shift) >> 1) +
                         static_cast<uint32_t>(extra);
    uint32_t a = rel;
    if ((y & 1) != 0) {
        const uint32_t row_start = (rel / stride) * stride;
        const uint32_t word = (rel - row_start) / 4;
        a = row_start + (word ^ 1u) * 4u + (rel & 3u);
    }
    return tmem[((address + a) & address_mask) | or_address];
}

// The 16-bit palette entry a CI texel selects, at RT64's fixed palette base
// (`RDP_TMEM_PALETTE` 0x800): CI4 uses `0x800 + (palette << 7) + (index << 3)`
// and CI8 `0x800 + (index << 3)`, one entry per 8 bytes.
inline uint16_t tmem_palette_entry(const uint8_t* tmem, uint8_t palette, uint8_t siz,
                                   uint32_t index) {
    const uint32_t at = 0x800u + ((siz == gbi::IM_SIZ_4b)
                                      ? ((static_cast<uint32_t>(palette) << 7) | (index << 3))
                                      : (index << 3));
    return static_cast<uint16_t>((tmem[at & 0xFFF] << 8) | tmem[(at + 1) & 0xFFF]);
}

// Decodes a tile's image out of TMEM the way RT64's `sampleTMEMWithConvert`
// reads it. `w`/`h` are RT64's `sampleWidth`/`sampleHeight` (the tile window
// clamped to its wrap mask), and the image starts at `tile.tmem << 3`; the
// tile's `uls`/`ult` are the UV origin, not a source offset.
void decode_tmem_tile(const uint8_t* tmem, const TileState& tile, int w, int h,
                      int lut_mode, const int32_t* convert_k, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    const uint32_t stride = static_cast<uint32_t>(tile.line) << 3;
    if (stride == 0 || w <= 0 || h <= 0) {
        return;
    }
    const uint32_t address = static_cast<uint32_t>(tile.tmem) << 3;
    const bool indexed = (tile.fmt == gbi::IM_FMT_CI) && (lut_mode != 0);
    const bool rgba32 = (tile.fmt == gbi::IM_FMT_RGBA) && (tile.siz == gbi::IM_SIZ_32b);
    // RT64 masks a sample to the lower half of TMEM for CI and RGBA32 (whose
    // upper half holds the other two colour channels), and to all 4 KiB for
    // every other format.
    const uint32_t address_mask = (rgba32 || indexed) ? 0x7FFu : 0xFFFu;
    const int shift = rgba32 ? 2 : static_cast<int>(tile.siz);
    const bool yuv16 = (tile.fmt == gbi::IM_FMT_YUV) && (tile.siz == gbi::IM_SIZ_16b);

    auto load = [&](int x, int y, int extra = 0, uint32_t or_address = 0) {
        return tmem_sample_byte(tmem, address, stride, address_mask, shift, x, y, extra,
                                or_address);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t rgba[4] = {0, 0, 0, 255};

            if (yuv16) {
                // A YUV tile is one luma byte per texel in TMEM's upper half
                // and one U/V pair per two texels in the lower half, both at
                // the tile's row stride (RT64 `sampleTMEMYUV16`).
                const uint32_t row_base = address + stride * static_cast<uint32_t>(y);
                const uint32_t row_xor = ((y & 1) != 0) ? 4u : 0u;
                const uint8_t yy = tmem[((row_base + (static_cast<uint32_t>(x) ^ row_xor)) | 0x800u) &
                                        0xFFFu];
                const uint32_t chroma = row_base + (((static_cast<uint32_t>(x) >> 1) << 1) ^ row_xor);
                const uint8_t uu = tmem[chroma & 0xFFFu];
                const uint8_t vv = tmem[(chroma + 1u) & 0xFFFu];
                yuv16_to_rgba8(yy, uu, vv, convert_k, rgba);
            } else if (tile.siz == gbi::IM_SIZ_4b) {
                const uint8_t byte = load(x, y);
                const uint8_t nib = (x & 1) ? (byte & 0xF) : (byte >> 4);
                switch (tile.fmt) {
                    case gbi::IM_FMT_IA: {
                        const uint8_t i = static_cast<uint8_t>((nib >> 1) * 255 / 7);
                        rgba[0] = rgba[1] = rgba[2] = i;
                        rgba[3] = (nib & 1) ? 255 : 0;
                        break;
                    }
                    case gbi::IM_FMT_I: {
                        const uint8_t i = static_cast<uint8_t>(nib * 255 / 15);
                        rgba[0] = rgba[1] = rgba[2] = rgba[3] = i;
                        break;
                    }
                    case gbi::IM_FMT_CI:
                        if (lut_mode == 2) {
                            rgba16_to_rgba8(
                                tmem_palette_entry(tmem, tile.palette, tile.siz, nib), rgba);
                        } else if (lut_mode == 3) {
                            ia16_to_rgba8(
                                tmem_palette_entry(tmem, tile.palette, tile.siz, nib), rgba);
                        } else {
                            // A CI tile with no TLUT behaves like I (RT64
                            // `sampleTMEM4b`'s CI arm: the palette index is the
                            // intensity). Painting it black was the title
                            // prologue's two unit sprites, session 144.
                            const uint8_t i = nib;
                            rgba[0] = rgba[1] = rgba[2] = rgba[3] = i;
                        }
                        break;
                    default:
                        break;
                }
            } else if (tile.siz == gbi::IM_SIZ_8b) {
                const uint8_t v = load(x, y);
                switch (tile.fmt) {
                    case gbi::IM_FMT_IA:
                        rgba[0] = rgba[1] = rgba[2] = static_cast<uint8_t>((v >> 4) * 255 / 15);
                        rgba[3] = static_cast<uint8_t>((v & 0xF) * 255 / 15);
                        break;
                    case gbi::IM_FMT_I:
                        rgba[0] = rgba[1] = rgba[2] = rgba[3] = v;
                        break;
                    case gbi::IM_FMT_CI:
                        if (lut_mode == 2) {
                            rgba16_to_rgba8(
                                tmem_palette_entry(tmem, tile.palette, tile.siz, v), rgba);
                        } else if (lut_mode == 3) {
                            ia16_to_rgba8(tmem_palette_entry(tmem, tile.palette, tile.siz, v),
                                          rgba);
                        } else {
                            // No TLUT: CI behaves like I (RT64 `sampleTMEM8b`).
                            rgba[0] = rgba[1] = rgba[2] = rgba[3] = v;
                        }
                        break;
                    default:
                        break;
                }
            } else if (tile.siz == gbi::IM_SIZ_16b) {
                const uint16_t value = static_cast<uint16_t>((load(x, y) << 8) | load(x, y, 1));
                switch (tile.fmt) {
                    case gbi::IM_FMT_IA:
                        ia16_to_rgba8(value, rgba);
                        break;
                    case gbi::IM_FMT_RGBA:
                        rgba16_to_rgba8(value, rgba);
                        break;
                    case gbi::IM_FMT_CI:
                    case gbi::IM_FMT_I:
                        // RT64 `sampleTMEMI16`: not a real format. Intensity
                        // comes from the high byte and alpha from the low one
                        // (the observed hardware behaviour).
                        rgba[0] = rgba[2] = static_cast<uint8_t>(value >> 8);
                        rgba[1] = rgba[3] = static_cast<uint8_t>(value & 0xFF);
                        break;
                    default:
                        break;
                }
            } else if (tile.siz == gbi::IM_SIZ_32b) {
                if (rgba32) {
                    // R,G live in TMEM's lower half and B,A in its upper half
                    // (RT64 `loadWord<RGBA32>` / `sampleTMEMRGBA32`).
                    rgba[0] = load(x, y);
                    rgba[1] = load(x, y, 1);
                    rgba[2] = load(x, y, 0, 0x800);
                    rgba[3] = load(x, y, 1, 0x800);
                } else {
                    // RT64 `sampleTMEMI32`: odd columns decode R,G from the
                    // lower half and even columns B,A from the upper half.
                    if ((x & 1) != 0) {
                        rgba[0] = rgba[2] = load(x, y);
                        rgba[1] = rgba[3] = load(x, y, 1);
                    } else {
                        rgba[0] = rgba[2] = load(x, y, 0, 0x800);
                        rgba[1] = rgba[3] = load(x, y, 1, 0x800);
                    }
                }
            }

            const size_t o = (static_cast<size_t>(y) * w + x) * 4;
            out[o + 0] = rgba[0];
            out[o + 1] = rgba[1];
            out[o + 2] = rgba[2];
            out[o + 3] = rgba[3];
        }
    }
}

// ============================================================================
// WebGL2 plumbing
// ============================================================================

const char* kVertexShaderSrc = R"GLSL(#version 300 es
layout(location = 0) in vec2 a_pos;    // NDC x, y
layout(location = 1) in vec2 a_uv;   // texel coordinates (s10.5 x G_TEXTURE scale)
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_z;     // NDC z (-1..1); see transform_vertex
layout(location = 4) in float a_w;     // clip-space w (1 for a rectangle)
out vec2 v_tex;          // the same texel coordinates, for the sampler
out vec4 v_shade;
void main() {
    // RT64's RasterVS multiplies the whole position by w before the rasterizer
    // (`ndcPos.xyz *= ndcPos.w`), which makes the varying interpolation
    // perspective-correct. Writing w here is measurably wrong for this port's
    // sampler, though: with w in gl_Position the mission frame's top cliff goes
    // from 0.93 to 10.27 mean abs against native `/tmp/w168/nat2/t.800` and the
    // whole frame from 5.28 to 7.74 (session 172 measured both). The w is still
    // uploaded (attribute 4) because it is RDP state, but the clip position
    // keeps w = 1 and interpolates in screen space, which is what the stored
    // native frame agrees with.
    gl_Position = vec4(a_pos, a_z, 1.0);
    // The tile's origin and size moved into the fragment shader: RT64's
    // sampler works in texel space, and it quantizes the interpolated
    // coordinate to the hardware's 1/128 texel before sampling.
    v_tex = a_uv;
    v_shade = a_color;
}
)GLSL";

// Evaluates the RDP color combiner the way RT64 does (ColorCombiner::run /
// runCycle in shared/rt64_color_combiner.h):
//   rgb   = wrap_clamp((A - B) * C + D)   per channel
//   alpha = wrap_clamp((Aa - Ba) * Ca + Da)
// The selector uniforms hold the CSEL_/ASEL_ enums decoded on the gfx thread.
// Which uniform pair is evaluated depends on the cycle type: a 1-cycle draw
// evaluates the SECOND mux (RT64 run() -> runCycle(..., 1, false, ...) - the
// hardware ignores the first mux then), a 2-cycle draw evaluates mux 0 then
// mux 1, and in the second cycle the TEXEL0/TEXEL1 values are swapped and the
// COMBINED inputs are wrapped instead of clamped.
const char* kFragmentShaderSrc = R"GLSL(#version 300 es
// RT64's RasterPS works in fp32 (`float`, `hlslpp::float4`). `mediump` is
// fp16 on parts of the mobile GLSL spec and would round the combiner and
// blender a second time, so the shader asks for fp32 explicitly.
precision highp float;
in vec2 v_tex;
in vec4 v_shade;
out vec4 fragColor;
uniform sampler2D u_tex0;
uniform sampler2D u_tex1;
uniform vec4 u_prim;
uniform vec4 u_env;
uniform int u_ca0, u_cb0, u_cc0, u_cd0;
uniform int u_aa0, u_ab0, u_ac0, u_ad0;
uniform int u_ca1, u_cb1, u_cc1, u_cd1;
uniform int u_aa1, u_ab1, u_ac1, u_ad1;
uniform int u_cycle;   // 0 = COPY (texel0 passthrough), 1 or 2 = combiner cycles
uniform int u_use_tex;
// ---- RT64 texture sampler (tools/RT64/src/shaders/TextureSampler.hlsli) ----
// Per texture unit: the decoded image's size in texels, the tile's uls/ult in
// texels, the clamp/mirror mode per axis (G_TX_WRAP 0, G_TX_MIRROR 1,
// G_TX_CLAMP 2) and the wrap size per axis in texels (0 = no mask). These are
// RT64's RDPTile fields after `State::setupDrawCall`: `masks` is the wrap size
// `1 << masks`, and a tile with no mask is forced to clamp.
uniform vec2 u_tsize0, u_tsize1;
uniform vec2 u_torig0, u_torig1;
uniform ivec2 u_tcm0, u_tcm1;
uniform ivec2 u_tmask0, u_tmask1;
// OGRE_TEXWINCLAMP: the tile's own rect in texels, which a wrapped sample is
// bounded to (0 = no bound, the default). A 0.5 threshold keeps a 1-texel rect
// a bound.
uniform vec2 u_twin0, u_twin1;
uniform int u_tex_filter;   // OTHERMODE_H G_TF_*: 0 POINT, 2 BILERP, 3 AVERAGE
uniform int u_texf;         // 0 GL sampler, 1 shader bilinear, 2 RT64 3-point
uniform int u_tex_persp;    // 0 halves the coordinate (G_TP_NONE, non-rect)
// The tile's G_TX shift scale (RT64 `ShiftScaleMap[shifts]`), one vec2 per unit.
uniform vec2 u_tshift0, u_tshift1;
// Blender / render mode (RT64 Blender::run in shared/rt64_blender.h). The P/M/A/B
// selector enums and the approximation codes are the CPU-side BlendPM/BlendA/
// BlendB/BlendApprox values.
uniform vec4 u_fog_color;
uniform vec4 u_blend_color;
// G_SETCONVERT's K4/K5, as combiner colour inputs (RT64 RasterPS.hlsl). RT64
// divides the raw 9-bit field by 255; K4/K5 carry no sign extension.
uniform float u_k4;
uniform float u_k5;
// G_SETKEYR/G_SETKEYGB centre and scale (RT64 `ccInputs.keyCenter/keyScale`,
// RasterPS.hlsl:185-186). Only the RGB channels are combiner inputs.
uniform vec3 u_key_center;
uniform vec3 u_key_scale;
uniform int u_bp0, u_bm0, u_ba0, u_bb0;
uniform int u_bp1, u_bm1, u_ba1, u_bb1;
uniform int u_force_blend;
uniform int u_blend_approx;
uniform int u_combiner_cycles;
// The RDP's alpha compare and coverage path (RT64 RasterPS.hlsl). A texel
// whose combiner alpha is zero is discarded when CVG_X_ALPHA is set, which is
// what makes a transparent CI4 texel transparent instead of painting its
// colour: `resultCvg = (8/cvgRange) * combinerAlpha` and anything below one
// coverage step is cut. The alpha written to the target is the coverage value
// (`cvgDst`), not the blender's alpha.
uniform int u_alpha_blend;    // the draw's GL blend is enabled (alpha is its factor)
uniform int u_ac;             // OTHERMODE_L bits 0-1: G_AC_NONE 0, G_AC_THRESHOLD 1, G_AC_DITHER 3
// The two seeds of RT64's alpha dither (`Random.hlsli` `initRand`): the frame
// count and the draw's instance index. `u_dither_frame` is this renderer's
// present counter and `u_dither_instance` the draw's index, which are RT64's
// `FrParams.frameCount` and `instanceIndex` in kind, not in value.
uniform uint u_dither_frame;
uniform uint u_dither_instance;
// RT64's F3D depth clip (RasterPS.hlsl:63-104). The F3D microcodes clip polygons
// earlier than the hardware's own depth range: a non-rect draw whose depth
// source is G_ZS_PIXEL is dropped outside [0, 1022/1024]. `u_zclip` is 0 for a
// rectangle and for a G_ZS_PRIM draw (RT64 sets MaxDepth = 1.0 for both, and the
// `z < 0` half cannot fire for a prim depth, which is already 0..1).
uniform int u_zclip;
uniform int u_cvg_x_alpha;    // CVG_X_ALPHA
uniform int u_cvg_dst;        // 0 clamp, 1 wrap, 2 full, 3 save
uniform float u_cvg_range;    // 255 for the 8-bit attachment, 65535 for RGBA16F
// Debug: 1 = output TEXEL1's RGB raw, 2 = output TEXEL0's RGB raw. Set from
// ogre_gfx_debug_flags bits 3-4 by the probes; bypasses the combiner and the
// blender so the sampled texture can be compared with the decoded image.
uniform int u_debug_mode;

// ---------------------------------------------------------------------------
// RT64 sampler: TextureSampler.hlsli `clampWrapMirrorSample` and
// `sampleTextureLevel`, restricted to the two fields this renderer has - a
// decoded RGBA8 image per tile, no TMEM, no mip levels, no texture
// replacement. The texel arithmetic is RT64's, including its `modulo` and the
// `SIMULATE_LOW_PRECISION` quantization of the interpolated coordinate.
// ---------------------------------------------------------------------------
int tex_modulo(int x, int y) {
    // RT64 Math.hlsli `modulo`: a mathematical modulus, defined at y == 0 (it
    // returns x) so a tile with no mask cannot divide by zero.
    return (y != 0) ? x - y * int(floor(float(x) / float(y))) : x;
}

vec4 sampler_texel(int unit, ivec2 t, vec2 size, ivec2 cm, ivec2 mask, vec2 win) {
    ivec2 hi = ivec2(size) - ivec2(1);
    // clampWrapMirrorSample: clamp to the tile window first, then wrap.
    if (cm.x == 2) t.x = clamp(t.x, 0, hi.x);
    if (cm.y == 2) t.y = clamp(t.y, 0, hi.y);
    if (mask.x > 0) {
        if (cm.x == 1 && tex_modulo(t.x, mask.x * 2) >= mask.x) {
            t.x = (mask.x - 1) - tex_modulo(t.x, mask.x);
        } else {
            t.x = tex_modulo(t.x, mask.x);
        }
    }
    if (mask.y > 0) {
        if (cm.y == 1 && tex_modulo(t.y, mask.y * 2) >= mask.y) {
            t.y = (mask.y - 1) - tex_modulo(t.y, mask.y);
        } else {
            t.y = tex_modulo(t.y, mask.y);
        }
    }
    // OGRE_TEXWINCLAMP: bound the sample to the tile's own rect. A band drawn
    // over its own rect samples one row/column past it at the far edge, and the
    // wrap above then reads the texels TMEM happens to hold there.
    if (win.x > 0.5) t.x = clamp(t.x, 0, int(win.x) - 1);
    if (win.y > 0.5) t.y = clamp(t.y, 0, int(win.y) - 1);
    // The GL texture holds the one tile window the game loaded, where RT64
    // holds the whole decoded TMEM area. A coordinate the wrap above sends past
    // this window has no texel here, so it clamps instead of reading out of
    // bounds (which GL defines as zero).
    t = clamp(t, ivec2(0), hi);
    return unit == 1 ? texelFetch(u_tex1, t, 0) : texelFetch(u_tex0, t, 0);
}

// sampleTextureLevel's filter selection and weight arithmetic. `uv` is the
// interpolated texel coordinate, `origin` the tile's uls/ult in texels.
vec4 sample_unit(int unit, vec2 uv, vec2 size, vec2 origin, ivec2 cm, ivec2 mask,
                 vec2 win, bool filtering) {
    vec2 local = round(uv * 128.0) / 128.0 - origin;   // SIMULATE_LOW_PRECISION
    ivec2 base = ivec2(floor(local));
    vec4 s00 = sampler_texel(unit, base, size, cm, mask, win);
    if (!filtering) {
        return s00;
    }
    vec4 s01 = sampler_texel(unit, base + ivec2(0, 1), size, cm, mask, win);
    vec4 s10 = sampler_texel(unit, base + ivec2(1, 0), size, cm, mask, win);
    vec4 s11 = sampler_texel(unit, base + ivec2(1, 1), size, cm, mask, win);
    vec2 frac_part = local - floor(local);
    if (u_texf < 2) {
        // True bilinear (`linearFiltering` in RT64, the GL sampler's rule).
        return mix(mix(s00, s10, frac_part.x), mix(s01, s11, frac_part.x), frac_part.y);
    }
    bool use_average =
        (u_tex_filter == 3) && all(lessThanEqual(abs(frac_part - vec2(0.5)), vec2(1.0 / 128.0)));
    if (use_average) {
        return (s00 + s01 + s10 + s11) / 4.0;
    }
    // Three-point bilinear, written by ArthurCarvalho and ported to RT64; the
    // RT64 default (`threePointFiltering = true`) uses this, not plain bilinear.
    vec4 tri0 = mix(s00, s10, frac_part.x) + (s01 - s00) * frac_part.y;
    vec4 tri1 = mix(s11, s01, 1.0 - frac_part.x) + (s10 - s11) * (1.0 - frac_part.y);
    return mix(tri0, tri1, step(1.0, frac_part.x + frac_part.y));
}

// The GL fixed-function path (`OGRE_TEXF=gl`), which is what this renderer did
// before the sampler port: texture units normalized by the tile's origin/size.
vec4 fixed_texel(int unit, vec2 uv, vec2 size, vec2 origin) {
    vec2 st = (uv - origin) / size;
    return unit == 1 ? texture(u_tex1, st) : texture(u_tex0, st);
}

vec4 tex_val(int sel, bool second) {
    bool use1 = (sel == 2) != second;
    // COPY mode has no combiner cycle: the RDP samples TEXEL0 point-sampled
    // (RT64 `filterBilerp` requires cycleType != G_CYC_COPY).
    bool filtering = (u_tex_filter != 0) && (u_cycle != 0);
    // RT64's `perspCorrectionMod`: a non-perspective triangle halves the
    // interpolated coordinate. The tile's shift scale multiplies it next, in the
    // same expression as RT64 (`TextureSampler.hlsli:226`), before the 1/128
    // rounding inside sample_unit.
    vec2 uv = v_tex * (u_tex_persp != 0 ? 1.0 : 0.5) *
              (use1 ? u_tshift1 : u_tshift0);
    if (u_texf == 0) {
        return use1 ? fixed_texel(1, uv, u_tsize1, u_torig1)
                    : fixed_texel(0, uv, u_tsize0, u_torig0);
    }
    return use1 ? sample_unit(1, uv, u_tsize1, u_torig1, u_tcm1, u_tmask1, u_twin1,
                              filtering)
                : sample_unit(0, uv, u_tsize0, u_torig0, u_tcm0, u_tmask0, u_twin0,
                              filtering);
}

// RT64 ColorCombiner::fromColorInput, sel = CSEL_*.
vec4 color_input(int sel, bool second, vec4 combined) {
    if (sel == 0) return combined;                              // COMBINED
    if (sel == 1 || sel == 2) return tex_val(sel, second);      // TEXEL0 / TEXEL1
    if (sel == 3) return u_prim;                                // PRIMITIVE
    if (sel == 4) return v_shade;                               // SHADE
    if (sel == 5) return u_env;                                 // ENVIRONMENT
    if (sel == 6) return vec4(u_key_center, 0.0);               // KEY_CENTER
    if (sel == 7) return vec4(u_key_scale, 0.0);                // KEY_SCALE
    if (sel == 8) return vec4(combined.a);                      // COMBINED_ALPHA
    if (sel == 9) return vec4(tex_val(1, second).a);            // TEXEL0_ALPHA
    if (sel == 10) return vec4(tex_val(2, second).a);           // TEXEL1_ALPHA
    if (sel == 11) return vec4(u_prim.a);                       // PRIMITIVE_ALPHA
    if (sel == 12) return vec4(v_shade.a);                      // SHADE_ALPHA
    if (sel == 13) return vec4(u_env.a);                        // ENV_ALPHA
    if (sel == 14 || sel == 15) return vec4(0.0);               // LOD_FRACTION / PRIM_LOD_FRAC
    if (sel == 16) return vec4(0.0);                            // NOISE (not emulated)
    if (sel == 17) return vec4(u_k4);                           // K4
    if (sel == 18) return vec4(u_k5);                           // K5
    if (sel == 19) return vec4(1.0);                            // ONE
    return vec4(0.0);                                           // ZERO
}

// RT64 ColorCombiner::fromAlphaInput, sel = ASEL_*.
float alpha_input(int sel, bool second, float combined) {
    if (sel == 0) return combined;                              // COMBINED
    if (sel == 1) return tex_val(1, second).a;                  // TEXEL0
    if (sel == 2) return tex_val(2, second).a;                  // TEXEL1
    if (sel == 3) return u_prim.a;                              // PRIMITIVE
    if (sel == 4) return v_shade.a;                             // SHADE
    if (sel == 5) return u_env.a;                               // ENVIRONMENT
    if (sel == 6 || sel == 7) return 0.0;                       // LOD_FRACTION / PRIM_LOD_FRAC
    if (sel == 8) return 1.0;                                   // ONE
    return 0.0;                                                 // ZERO
}

// RT64 wrap / wrapInputC / wrapInputABD / wrapClamp: the second cycle wraps
// its COMBINED inputs into range before clamping them.
void wrap_input(inout float i, float lo, float hi) {
    float range = hi - lo;
    if (lo >= i) i += range;
    if (i >= hi) i -= range;
}
void wrap_c(inout float i) { wrap_input(i, -1.0 - 1.0 / 255.0, 1.0 + 1.0 / 255.0); }
void wrap_abd(inout float i) { wrap_input(i, -0.5 - 1.0 / 255.0, 1.5 + 1.0 / 255.0); }
void wrap_clamp(inout float i) { wrap_abd(i); i = clamp(i, 0.0, 1.0); }

// ---------------------------------------------------------------------------
// Blender (RT64 Blender::run / runCycle, shared/rt64_blender.h)
// ---------------------------------------------------------------------------
// The framebuffer-colour input returns white: RT64 only reaches a branch that
// needs the *actual* framebuffer colour when checkEmulationRequirements()
// flagged the blender as non-simple, and in that case it uses one of the two
// approximations below instead of the cycle walk. In every other branch the
// framebuffer is only ever the input that is *replaced* (the destination is
// supplied by the GL blend, see alpha_blend on the CPU side).
vec3 from_input_pm(int pm, vec3 cc) {
    if (pm == 0) return cc;               // PM_CC_OR_BLENDER
    if (pm == 2) return u_blend_color.rgb;
    if (pm == 3) return u_fog_color.rgb;
    return vec3(1.0);                     // PM_FRAMEBUFFER_COLOR (see above)
}

float from_input_a(int a, float combiner_alpha) {
    if (a == 0) return combiner_alpha;    // A_CC_ALPHA
    if (a == 1) return u_fog_color.a;     // A_FOG_ALPHA
    if (a == 2) return v_shade.a;         // A_SHADE_ALPHA
    return 0.0;                           // A_ZERO
}

float from_input_b(int b, float a_multiplier) {
    if (b == 0) return 1.0 - a_multiplier;   // B_ONE_MINUS_A
    if (b == 3) return 0.0;                  // B_ZERO
    return 1.0;                              // B_FRAMEBUFFER_ALPHA / B_ONE
}

void blender_run_cycle(bool force_blend, bool last_cycle, bool not_first_cycle,
                       vec3 combiner_rgb, float combiner_alpha,
                       int P, int M, int A, int B,
                       inout bool passthrough_enabled, inout int passthrough_input,
                       inout vec3 blender_color, inout float final_alpha) {
    bool replace_cc = not_first_cycle && !passthrough_enabled;

    // Output colour is just the P input in this case.
    if (last_cycle && !force_blend) {
        if (P == 1) {                     // PM_FRAMEBUFFER_COLOR
            final_alpha = 0.0;
        } else {
            vec3 input_color = replace_cc ? blender_color : combiner_rgb;
            blender_color = from_input_pm(P, input_color);
            final_alpha = 1.0;
        }
        return;
    }

    bool any_input_is_zero = (A == 3) || (B == 3);
    bool duplicate_1ma = (P == M) && (B == 0);
    bool passthrough = any_input_is_zero || duplicate_1ma;
    bool framebuffer_color = (P == 1) || (M == 1);
    if (passthrough_enabled) {
        if (P == 0) {
            P = passthrough_input;
            framebuffer_color = framebuffer_color || (passthrough_input == 1);
        } else if (M == 0) {
            M = passthrough_input;
            framebuffer_color = framebuffer_color || (passthrough_input == 1);
        }
    }

    if (passthrough) {
        if (!last_cycle) {
            passthrough_input = (A == 3) ? M : P;
            passthrough_enabled = true;
        } else if (framebuffer_color) {
            final_alpha = 0.0;
        } else {
            vec3 input_color = replace_cc ? blender_color : combiner_rgb;
            blender_color = from_input_pm((A == 3) ? M : P, input_color);
            final_alpha = 1.0;
        }
    } else if (framebuffer_color) {
        vec3 input_color = replace_cc ? blender_color : combiner_rgb;
        if (P == 1) {
            blender_color = from_input_pm(M, input_color);
            final_alpha = 1.0 - from_input_a(A, combiner_alpha);
        } else if (M == 1) {
            blender_color = from_input_pm(P, input_color);
            final_alpha = from_input_a(A, combiner_alpha);
        }
    } else {
        vec3 input_color = replace_cc ? blender_color : combiner_rgb;
        float a_multiplier = from_input_a(A, combiner_alpha);
        float b_multiplier = from_input_b(B, a_multiplier);
        vec3 numerator = from_input_pm(P, input_color) * a_multiplier +
                         from_input_pm(M, input_color) * b_multiplier;
        // Simulate the hardware's numerator overflow with fmod (RT64
        // rt64_blender.h) - but not when P and M are the same input. The
        // numerator is then P*(a+b) and the division below cancels it exactly,
        // so the output is P whatever the multipliers are; folding it is an
        // artifact of the simulation, not of the hardware.
        //
        // OB64's title sprites are exactly that case: b0 is
        // [P=CC M=CC A=CC_A B=ONE] with b1 = [P=CC M=FB A=CC_A B=1MA], and
        // cycle 1 feeds cycle 0's colour through as its "CC". Folding there
        // damaged every pixel whose mask alpha was above ~0.05: a bright orange
        // body pixel (255,166,33) came out green for alpha 0.1-0.6 (red is the
        // only channel that exceeds 1 + 8/255, so only red folded) and about
        // half brightness at alpha 1. That is the "green smudges" over the
        // soldiers' helmets, and the reason the rendered sprite looked dim
        // next to its own colour x mask composite.
        const float Overflow = 1.0 + 8.0 / 255.0;
        if (P != M) {
            numerator = mod(numerator, Overflow);
        }
        blender_color = numerator / max(a_multiplier + b_multiplier, 1.0 / 255.0);
        final_alpha = 1.0;
    }
}

// Blender::run: returns the pixel the RDP writes (rgb) and the alpha the GL
// blend uses (a). `overrideFog` is false, as in RasterPS.hlsl.
vec4 blender_run(vec4 cc) {
    if (u_blend_approx == 1) {            // CombinerFramebuffer1MA_SquareMix
        return vec4(cc.rgb, cc.a * cc.a);
    }
    if (u_blend_approx == 2) {            // AnyFramebuffer1MA_MultiplyMix
        return vec4(from_input_pm(u_bp0, cc.rgb),
                    from_input_a(u_ba0, cc.a) * from_input_a(u_ba1, cc.a));
    }
    if (u_combiner_cycles == 0) {
        return vec4(cc.rgb, 1.0);
    }

    vec3 blender_color = vec3(0.0);
    float final_alpha = 0.0;
    bool passthrough_enabled = false;
    int passthrough_input = 0;
    blender_run_cycle(u_force_blend != 0, u_combiner_cycles == 1, false, cc.rgb, cc.a,
                      u_bp0, u_bm0, u_ba0, u_bb0,
                      passthrough_enabled, passthrough_input, blender_color, final_alpha);
    if (u_combiner_cycles > 1) {
        blender_run_cycle(u_force_blend != 0, true, true, cc.rgb, cc.a,
                          u_bp1, u_bm1, u_ba1, u_bb1,
                          passthrough_enabled, passthrough_input, blender_color, final_alpha);
    }
    return vec4(blender_color, final_alpha);
}

// RT64 `src/shaders/Random.hlsli` `initRand` / `nextRand`, ported verbatim.
// `initRand` is the DXR-path seed generator (16 rounds), and `nextRand` is a
// 32-bit LCG whose top 24 bits are the returned fraction.
uint dither_init_rand(uint val0, uint val1) {
    uint v0 = val0, v1 = val1, s0 = 0u;
    for (int n = 0; n < 16; ++n) {
        s0 += 0x9e3779b9u;
        v0 += ((v1 << 4) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4u);
        v1 += ((v0 << 4) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761eu);
    }
    return v0;
}

uint dither_next_rand(inout uint s) {
    s = 1664525u * s + 1013904223u;
    return s;
}

void main() {
    // The F3D depth clip. gl_FragCoord.z is the RDP's screen depth (0..1): the
    // vertex shader writes `2 * screen_z - 1` as NDC z and GL maps that back.
    if (u_zclip != 0) {
        const float MaxDepth = 1022.0 / 1024.0;
        if (gl_FragCoord.z < 0.0 || gl_FragCoord.z > MaxDepth) {
            discard;
        }
    }
    if (u_debug_mode == 1) {
        // Point-sampled so the probe compares the decoded texel itself.
        fragColor = vec4(sample_unit(1, v_tex, u_tsize1, u_torig1, u_tcm1, u_tmask1,
                                     vec2(0.0), false).rgb,
                         1.0);
        return;
    }
    if (u_debug_mode == 2) {
        fragColor = vec4(sample_unit(0, v_tex, u_tsize0, u_torig0, u_tcm0, u_tmask0,
                                     vec2(0.0), false).rgb,
                         1.0);
        return;
    }
    if (u_cycle == 0) {
        // G_CYC_COPY bypasses the combiner and writes TEXEL0 straight through
        // (RT64 ColorCombiner::run).
        fragColor = tex_val(1, false);
        return;
    }

    // Slot 0 is the first mux the hardware evaluates: mux 0 for a 2-cycle
    // draw, mux 1 for a 1-cycle draw (see record_state).
    vec4 c = vec4(0.0, 0.0, 0.0, 0.0);
    c.rgb = (color_input(u_ca0, false, c).rgb - color_input(u_cb0, false, c).rgb) *
                color_input(u_cc0, false, c).rgb + color_input(u_cd0, false, c).rgb;
    c.a = (alpha_input(u_aa0, false, c.a) - alpha_input(u_ab0, false, c.a)) *
              alpha_input(u_ac0, false, c.a) + alpha_input(u_ad0, false, c.a);

    // RT64 ColorCombiner::run: the alpha compared against the threshold is the
    // one the first evaluated cycle produced, before the second cycle runs.
    float alpha_compare_value = c.a;

    if (u_cycle == 2) {
        // RT64: wrapInputC when the cycle's color C input is COMBINED (CSEL 0),
        // wrapInputABD otherwise; alpha uses A_COMBINED (ASEL 0).
        if (u_cc1 == 0) { wrap_c(c.r); wrap_c(c.g); wrap_c(c.b); }
        else { wrap_abd(c.r); wrap_abd(c.g); wrap_abd(c.b); }
        if (u_ac1 == 0) wrap_c(c.a); else wrap_abd(c.a);
        c.rgb = (color_input(u_ca1, true, c).rgb - color_input(u_cb1, true, c).rgb) *
                    color_input(u_cc1, true, c).rgb + color_input(u_cd1, true, c).rgb;
        c.a = (alpha_input(u_aa1, true, c.a) - alpha_input(u_ab1, true, c.a)) *
                  alpha_input(u_ac1, true, c.a) + alpha_input(u_ad1, true, c.a);
    }

    wrap_clamp(c.r); wrap_clamp(c.g); wrap_clamp(c.b); wrap_clamp(c.a);
    wrap_clamp(alpha_compare_value);

    // Alpha compare (RT64 RasterPS.hlsl:205-217). G_AC_THRESHOLD compares
    // against the blend colour's alpha; G_AC_DITHER against a per-pixel
    // pseudo-random value. `u_ac` is OTHERMODE_L bits 0-1 as RT64 reads them
    // (`OtherMode::alphaCompare()` returns `L & 3`), so the values are
    // G_AC_NONE 0, G_AC_THRESHOLD 1 and G_AC_DITHER 3
    // (tools/RT64/src/shared/rt64_f3d_defines.h:8-10).
    if (u_ac == 1) {
        if (alpha_compare_value < u_blend_color.a) {
            discard;
        }
    } else if (u_ac == 3) {
        // RT64's dither, `nextRand` seeded from the frame count and the pixel
        // (`Random.hlsli`, RasterPS.hlsl:65 `randomSeed = initRand(FrParams.
        // frameCount, instanceIndex * pixelPosSeed.y * pixelPosSeed.x, 16)`).
        // The combiner's NOISE input consumes one `nextRand` before the compare
        // does (RasterPS.hlsl:189 vs :213), so the compare reads the second.
        ivec2 pixel_pos = ivec2(floor(gl_FragCoord.xy));
        uint seed = dither_init_rand(u_dither_frame,
                                     u_dither_instance * uint(pixel_pos.y) * uint(pixel_pos.x));
        dither_next_rand(seed);
        float dither_value =
            float(dither_next_rand(seed) & 0x00FFFFFFu) / float(0x01000000u);
        if (alpha_compare_value < dither_value) {
            discard;
        }
    }

    // Coverage. With CVG_X_ALPHA the combiner's alpha is the coverage; a value
    // below one eight-of-a-step carries no coverage and the pixel is dropped,
    // so a transparent texel contributes no colour. The alpha written to the
    // target is the coverage value selected by CVG_DST, and the GL blend uses
    // the blender's alpha instead while it is enabled.
    float cvg = 8.0 * ((u_cvg_x_alpha != 0) ? c.a : 1.0);
    if (cvg < 1.0) {
        discard;
    }
    float coverage;
    if (u_cvg_dst == 3) {
        coverage = 0.0;                                     // CVG_DST_SAVE
    } else if (u_cvg_dst == 2) {
        coverage = 7.0 / u_cvg_range;                       // CVG_DST_FULL
    } else if (u_cvg_dst == 0) {
        coverage = min(cvg, 7.0) / u_cvg_range;             // CVG_DST_CLAMP
    } else {
        coverage = cvg / u_cvg_range;                       // CVG_DST_WRAP
    }

    // The blender turns the combiner output into the written pixel and, when it
    // reads the framebuffer, into the alpha the GL blend mixes with.
    vec4 outc = blender_run(c);
    fragColor = vec4(outc.rgb, (u_alpha_blend != 0) ? outc.a : coverage);
}
)GLSL";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "[web-renderer] shader(%s) compile failed: %s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        fprintf(stderr, "[web-renderer]   source head: %.120s\n", src);
        // Print the whole source so the error can be matched to a line.
        for (int line = 1; *src; ++line) {
            const char* nl = strchr(src, '\n');
            size_t len = nl ? static_cast<size_t>(nl - src) : strlen(src);
            fprintf(stderr, "[web-renderer]   %3d | %.*s\n", line, (int)len, src);
            if (!nl) break;
            src = nl + 1;
        }
    }
    return sh;
}

// ============================================================================
// OGRE_FOG: repair OB64's stale-tile fog pieces (browser port)
// ============================================================================
//
// OB64 draws each scrolling fog/cloud layer as a set of `G_TEXRECT`s with the
// white combiner (`FCFFFFFF FFFF73B9` = RGB ONE, ALPHA TEXEL0). The draws
// inherit the cloud loop's last `G_SETTILESIZE t0`, which covers no texels
// (`uls == lrs` or `ult == lrt`), so the sample collapses onto one stale row
// and RT64 paints an opaque white band instead of the fog. The game's layer
// table at `D_801B80E0` still names the image each layer is meant to draw, so
// the texture setup the rectangle inherits is rewritten to sample that image.
//
// This is a port of `app/src/renderer.cpp` `namespace nop_rect`
// (`Walker::fog_fix`) + `namespace fog` (`repair`); the native code is the
// spec, and the rewrite below is the same bytes for the same reads:
//
//   - `D_801B80E0` holds the layer table pointer. Layer `i`'s record is at
//     `table + 0xFC + i*0x24`; its first word is an image record, whose
//     `+8` is the image data, logical byte 2 is `fmt`, byte 3 is `siz`, and
//     the 16-bit fields at logical 6 and 4 are width and height.
//   - The walker tracks the last `SETTIMG`, `SETTILE t7`, `LOADTILE`/`LOADBLOCK`,
//     `SETTILE t0` and `SETTILESIZE t0` before the draw, plus the white-combiner
//     flag (`G_SETCOMBINE` w1 == `0xFFFF73B9`) and the tile window.
//   - At a white-combiner `G_TEXRECT` whose inherited window is zero-area,
//     layer `fog_layer` is repaired and `fog_layer` advances 0 -> 2 -> 3 ...
//     A group with a negative `s`/`t` keeps the loop's own one-row tile instead
//     (see the native comment).
//   - The rewrite is `SETTIMG` (fmt/siz/width + image address), `SETTILE t7`
//     and `SETTILE t0` (same image at TMEM 0; tile 0 wraps by `masks`/`maskt`),
//     `LOADTILE` of the whole image, and `SETTILESIZE t0` over the whole image
//     in quarter-texel units.
//
// The native `OGRE_FOG_SCALE` scratch staging is not ported: the browser always
// samples the game's own image (`scale = 100`), which is the default.
//
// Like the native pass, this runs on the submitted display list in RDRAM before
// the renderer walks it. `OGRE_FOG=0` disables it (default on, like the native).

constexpr uint32_t kFogLayerTable = 0x801B80E0u;  // D_801B80E0: layer table

inline void fog_put32(uint8_t* rdram, uint32_t off, uint32_t w) {
    rdram[off + 0] = static_cast<uint8_t>(w);
    rdram[off + 1] = static_cast<uint8_t>(w >> 8);
    rdram[off + 2] = static_cast<uint8_t>(w >> 16);
    rdram[off + 3] = static_cast<uint8_t>(w >> 24);
}

struct FogImage {
    uint32_t address = 0;   // guest address of the image data (record + 8)
    uint32_t size = 0;
    uint32_t width = 0, height = 0;
    uint32_t siz = 0, fmt = 0;
};

// The image record the game's layer `index` draws, or false when it is not
// usable. Same reads and same byte order as the native `fog::layer_image`.
inline bool fog_layer_image(uint8_t* rdram, uint32_t index, uint32_t& rec) {
    const uint32_t table = rd32(rdram + (kFogLayerTable - 0x80000000u));
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

// The record's fields, read the way the game reads them: the runtime stores
// each word byte-reversed, so a byte field at logical offset `o` lives at
// `o ^ 3` and a 16-bit field at `o` lives at the two bytes starting at `o ^ 2`.
inline bool fog_read_image(uint8_t* rdram, uint32_t rec, FogImage& img) {
    const uint32_t o = rec - 0x80000000u;
    if (o + 8 > 0x20000000u) {
        return false;
    }
    const uint8_t* p = rdram + o;
    img.siz = p[0];                  // logical byte 3 lives at 3 ^ 3 = 0
    img.fmt = p[1];                  // logical byte 2 lives at 2 ^ 3 = 1
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

// Rewrite the texture setup a zero-area rectangle inherits so it samples the fog
// layer's own image. Returns the image address the repair installed, or 0 when
// no repair was applied.
inline uint32_t fog_repair_group(uint8_t* rdram, uint32_t layer, uint32_t settimg,
                                 uint32_t settile7, uint32_t loadtile, uint32_t settile0,
                                 uint32_t tilesize, uint32_t uls, uint32_t ult, uint32_t lrs,
                                 uint32_t lrt, int rect_s, int rect_t, bool verbose) {
    // A zero-area tile is the marker: a one-texel tile is `lrs == uls + 4`.
    if (!(lrs == uls || lrt == ult)) {
        return 0;
    }
    if (settimg == 0 || settile7 == 0 || loadtile == 0 || settile0 == 0 || tilesize == 0) {
        return 0;
    }
    uint32_t rec = 0;
    FogImage img;
    if (!fog_layer_image(rdram, layer, rec) || !fog_read_image(rdram, rec, img)) {
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
    // instead of the group being dropped. Only a group with a negative
    // coordinate cannot be repaired at all; it keeps a valid one-row tile so the
    // renderer reads the row the loop left behind instead of a fabricated one.
    if ((rect_s < 0) || (rect_t < 0)) {
        const uint32_t oneRow = 0xF2000000u | (uls << 12) | ult;
        const uint32_t oneRowLr = ((lrs > uls ? lrs : uls) << 12) | (ult + 4);
        fog_put32(rdram, tilesize, oneRow);
        fog_put32(rdram, tilesize + 4, oneRowLr);
        return 0;
    }
    const uint32_t bytes_per_row = (img.width * (4u << img.siz)) / 8u;
    const uint32_t line = std::max(bytes_per_row / 8u, 1u);  // SETTILE `line` in 64-bit words
    // G_SETTILESIZE / G_LOADTILE carry uls/ult/lrs/lrt in quarter-texel units,
    // not texels; `last` is the whole image in those units.
    const uint32_t last = (((img.width - 1) * 4u) << 12) | ((img.height - 1) * 4u);
    // G_SETTIMG: fmt/siz/width + the image address.
    fog_put32(rdram, settimg, 0xFD000000u | (img.fmt << 21) | (img.siz << 19) | (img.width - 1));
    fog_put32(rdram, settimg + 4, img.address);
    // G_SETTILE for the load tile (7) and the draw tile (0): same image at TMEM
    // offset 0. The draw tile wraps: the sweep's `s` walks to ~118 texels, so
    // without a mask the samples past the edge would be clamped away.
    // `masks`/`maskt` are the log2 of the wrap size, so this only applies to the
    // power-of-two layer images; anything else keeps the game's clamp.
    const uint32_t tile_w0 = 0xF5000000u | (img.fmt << 21) | (img.siz << 19) | (line << 9);
    const auto mask_bits = [](uint32_t n) -> uint32_t {
        uint32_t bits = 0;
        while ((1u << bits) < n) {
            ++bits;
        }
        return ((1u << bits) == n) ? bits : 0;
    };
    const uint32_t masks = mask_bits(img.width);
    const uint32_t maskt = mask_bits(img.height);
    fog_put32(rdram, settile7, tile_w0);
    fog_put32(rdram, settile7 + 4, 7u << 24);
    fog_put32(rdram, settile0, tile_w0);
    fog_put32(rdram, settile0 + 4, (maskt << 14) | (masks << 4));
    // G_LOADTILE: the whole image into the load tile (uls/ult = 0,0).
    fog_put32(rdram, loadtile, 0xF4000000u);
    fog_put32(rdram, loadtile + 4, (7u << 24) | last);
    // G_SETTILESIZE for the draw tile: the rectangle's texture coordinates are
    // tile-relative, so the tile spans the whole image.
    fog_put32(rdram, tilesize, 0xF2000000u);
    fog_put32(rdram, tilesize + 4, last);
    if (verbose) {
        fprintf(stderr,
                "[fog] layer %u image=0x%08X %ux%u siz=%u fmt=%u line=%u rect s=%d t=%d -> "
                "repaired a zero-area group\n",
                layer, img.address, img.width, img.height, img.siz, img.fmt, line, rect_s, rect_t);
        fflush(stderr);
    }
    return img.address;
}

struct FogWalker {
    uint8_t* rdram;
    bool verbose = false;
    uint32_t segments[16] = {};
    uint32_t budget = 200000;
    uint32_t layer = 0;
    uint32_t settimg = 0, settile7 = 0, loadtile = 0, settile0 = 0, tilesize = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;
    bool white = false;
    uint32_t fixed = 0;
    uint32_t last_image = 0;
    uint32_t last_layer = 0;
    uint32_t dl = 0;
    uint32_t group_index = 0;
    uint32_t white_groups = 0;
    uint32_t zero_groups = 0;

    void walk(uint32_t off, int depth) {
        if (depth > 32) {
            return;
        }
        for (;;) {
            if (budget == 0 || off + 8 > 0x20000000u) {
                return;
            }
            --budget;
            const uint8_t* p = rdram + off;
            const uint32_t w0 = rd32(p);
            const uint32_t w1 = rd32(p + 4);
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);

            if (op == 0xFD) {  // G_SETTIMG
                settimg = off;
                off += 8;
                continue;
            }
            if (op == 0xF5) {  // G_SETTILE
                if (p0(w1, 24, 3) == 7) {
                    settile7 = off;
                } else if (p0(w1, 24, 3) == 0) {
                    settile0 = off;
                }
            } else if (op == 0xF4 || op == 0xF3) {  // G_LOADTILE / G_LOADBLOCK
                loadtile = off;
            } else if (op == 0xF2) {  // G_SETTILESIZE
                if (p0(w1, 24, 3) == 0) {
                    tilesize = off;
                    uls = p0(w0, 12, 12);
                    ult = p0(w0, 0, 12);
                    lrs = p0(w1, 12, 12);
                    lrt = p0(w1, 0, 12);
                }
            } else if (op == 0xFC) {  // G_SETCOMBINE
                white = (w1 == 0xFFFF73B9u);
            }
            if (op == 0xDB) {  // G_MOVEWORD: segment registers
                gbi::set_segment_from_moveword(segments, w0, w1);
                off += 8;
                continue;
            }
            if (op == 0xDE) {  // G_DL
                const uint32_t target = gbi::resolve_address(segments, w1);
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
                if (white) {
                    ++white_groups;
                    if (lrs == uls || lrt == ult) {
                        ++zero_groups;
                    }
                    const int rect_s =
                        static_cast<int16_t>(p0(rd32(rdram + off + 12), 16, 16)) / 32;
                    const int rect_t =
                        static_cast<int16_t>(p0(rd32(rdram + off + 12), 0, 16)) / 32;
                    const uint32_t used_image =
                        fog_repair_group(rdram, layer, settimg, settile7, loadtile, settile0,
                                         tilesize, uls, ult, lrs, lrt, rect_s, rect_t, verbose);
                    if (summary_enabled()) {
                        fprintf(stderr,
                                "[fog-group] dl=%u n=%u s=%d t=%d uls=%u ult=%u lrs=%u lrt=%u "
                                "layer=%u used=0x%08X\n",
                                dl, group_index, rect_s, rect_t, uls, ult, lrs, lrt, layer,
                                used_image);
                        ++group_index;
                    }
                    if (used_image != 0) {
                        ++fixed;
                        last_image = used_image;
                        last_layer = layer;
                        layer = (layer == 0) ? 2 : layer + 1;
                    }
                    white = false;  // the combiner applies to one group
                }
                off += 24;
                continue;
            }
            // G_BRANCH_Z is the only 16-byte command in F3DEX2.
            off += (op == 0x04) ? 16 : 8;
        }
    }

    static bool summary_enabled() {
        static const bool on = getenv("OGRE_FOG_SUMMARY") != nullptr;
        return on;
    }
};

// Repairs one submitted display list in RDRAM. `dl` is the browser's task
// number, used only by the summary log. Returns the number of repaired groups.
inline uint32_t fog_repair_display_list(uint8_t* rdram, uint32_t dl_offset, uint32_t dl) {
    static const bool enabled = [] {
        const char* v = getenv("OGRE_FOG");
        return v == nullptr || strcmp(v, "0") != 0;
    }();
    if (!enabled || rdram == nullptr) {
        return 0;
    }
    static const bool verbose = getenv("OGRE_FOG_TRACE") != nullptr;
    static const bool summary = getenv("OGRE_FOG_SUMMARY") != nullptr;
    static uint32_t lists = 0;
    ++lists;
    FogWalker walker;
    walker.rdram = rdram;
    walker.verbose = verbose;
    walker.dl = dl;
    walker.walk(dl_offset, 1);
    if (walker.fixed != 0 && lists <= 2) {
        OGRE_MILESTONE("GFX-FOG", "task %u: repaired %u fog group(s)", dl, walker.fixed);
    }
    if (summary) {
        // The last repaired layer's image record, so a run says what the
        // repaired draws sample (format, size, pitch). The fog is the one part
        // of the title/story frame whose composite still differs from RT64 by
        // 1-3 levels, so the format is the first thing to read.
        uint32_t rec = 0;
        FogImage img;
        const bool have = fog_layer_image(rdram, walker.last_layer, rec) &&
                          fog_read_image(rdram, rec, img);
        OGRE_MILESTONE("GFX-FOG-SUMMARY",
                       "task %u: repaired=%u last_layer=%u last_image=0x%08X white=%u zero=%u "
                       "image siz=%u fmt=%u %ux%u",
                       dl, walker.fixed, walker.last_layer, walker.last_image,
                       walker.white_groups, walker.zero_groups,
                       have ? img.siz : 0u, have ? img.fmt : 0u,
                       have ? img.width : 0u, have ? img.height : 0u);
    }
    return walker.fixed;
}

// ============================================================================
// The renderer
// ============================================================================

// One queued texture upload: RGBA8 pixels decoded on the gfx pthread, uploaded
// to a GL texture by the main thread in ogre_gfx_flush().
struct TexUpload {
    uint64_t key;
    int width = 0, height = 0;
    int tile = 0;   // RDP tile the image was loaded into (diagnostics)
    bool clamp_s = true, clamp_t = true;
    bool bilerp = false;
    // Test-only: set by `test_draw` under debug flag bit 11, so the upload
    // loop can report whether the same content key was already resident.
    bool probe = false;
    // Test-only: this upload reused a decoded-tile cache entry instead of
    // decoding TMEM again. Read by `[GFX-TEXPROBE]`.
    bool decode_hit = false;
    // Shared with the decoded-tile cache so a cache hit does not copy the
    // pixels. A texture evicted from the GL cache is re-uploaded from here.
    std::shared_ptr<const std::vector<uint8_t>> pixels;
};

// One decoded tile image, keyed by a hash of the TMEM bytes and tile fields
// that produce it. The cache lives on the gfx thread and lets a display list
// that reloads the same source pixels reuse the previous decode; the content
// key is still computed from the decoded bytes, so the GL texture is unchanged.
struct DecodedTile {
    std::shared_ptr<const std::vector<uint8_t>> pixels;
    int width = 0, height = 0;
    uint64_t last_used = 0;
    size_t bytes = 0;
};

// A GL tile texture and the flush at which a draw last named it. The age
// window keeps a texture an alternating-frame screen still needs.
struct GlTextureEntry {
    GLuint tex = 0;
    size_t bytes = 0;
    uint64_t last_used = 0;
};

// One queued draw: a triangle list plus the full RDP state needed to render it
// (combiner mux, colors, blend, scissor, texture key). The gfx pthread records
// these; the browser main thread executes them in ogre_gfx_flush().
struct DrawCmd {
    int kind = 0;                         // 0 = triangle, 1 = fill rect, 2 = texrect
    int vertex_count = 0;                 // 3 or 6
    float pos[12] = {};                   // NDC (x,y) per vertex
    float uv[12] = {};                    // texture (u,v) per vertex
    float shade[24] = {};                 // rgba per vertex
    // The vertex's clip-space w, for perspective-correct interpolation. The
    // RDP interpolates texture coordinates and colours by dividing by the
    // interpolated 1/w (`RSPProcessCS.hlsl` stores `tfPos.w` in the screen
    // position; `RasterVS` multiplies it back into gl_Position), so a triangle
    // whose corners have different w does not interpolate them linearly in
    // screen space. A 2D rectangle uses w = 1.
    float w[6] = {};
    // Combiner mux (decoded).
    int ca[2] = {}, cb[2] = {}, cc[2] = {}, cd[2] = {};
    int aa[2] = {}, ab[2] = {}, ac[2] = {}, ad[2] = {};
    int cycle = 1;
    // Blender / render mode, decoded exactly as RT64 does (see Blender above).
    // Slot 0 is the first cycle the hardware evaluates, slot 1 the second.
    int bp[2] = {}, bm[2] = {}, ba[2] = {}, bb[2] = {};
    int blend_approx = 0;
    bool force_blend = false;
    bool alpha_blend = false;      // GL blend: src_alpha / one-minus-src-alpha
    int combiner_cycles = 1;
    // The RDP alpha-compare and coverage path (RT64 RasterPS.hlsl): the
    // combiner's alpha is compared against a threshold or a dither, and with
    // CVG_X_ALPHA it is the pixel's coverage, so a zero-alpha texel is dropped
    // instead of painting its colour. The alpha written to the target is the
    // coverage selected by CVG_DST.
    int acmp = 0;                  // OTHERMODE_L bits 0-1: 0 none, 1 threshold, 3 dither
    bool cvg_x_alpha = false;
    int cvg_dst = 0;               // 0 clamp, 1 wrap, 2 full, 3 save
    float fog[4] = {0, 0, 0, 1};
    float blend_color[4] = {0, 0, 0, 0};
    // Raw RDP state for the diagnostics (the numeric mux fields alone cannot
    // be checked against the game's own DL words without them).
    uint32_t othermode_h = 0, othermode_l = 0;
    uint32_t comb_L = 0, comb_H = 0;
    float prim[4] = {1, 1, 1, 1};
    float env[4] = {1, 1, 1, 1};
    // G_SETCONVERT's last two fields as combiner inputs (RT64 RasterPS.hlsl
    // `ccInputs.K4 = convertK[4] / 255.0f`). Raw 9-bit values; no sign extension
    // or `2*K+1` scaling, unlike k0..k3 of the YUV matrix.
    float k4 = 0.0f, k5 = 0.0f;
    // G_SETKEYR/G_SETKEYGB's centre and scale, RT64's combiner KEY_CENTER /
    // KEY_SCALE inputs (hle/rt64_rdp.cpp:1223-1236).
    float key_center[3] = {0, 0, 0};
    float key_scale[3] = {0, 0, 0};
    bool textured = false;
    uint64_t tex_key = 0;    // TEXEL0's tile image
    uint64_t tex_key1 = 0;   // TEXEL1's tile image
    // The GL fixed-function UV transform per unit (the `OGRE_TEXF=gl` path and
    // the `[GFX-UV]` diagnostic): 1/size and the tile origin in texels.
    float tex_scale[2] = {1, 1};
    float tex_origin[2] = {0, 0};
    float tex_scale1[2] = {1, 1};
    float tex_origin1[2] = {0, 0};
    // RT64 sampler state per unit (RDPTile): the decoded image's texel size,
    // the tile origin, the per-axis clamp/mirror mode (G_TX_WRAP 0,
    // G_TX_MIRROR 1, G_TX_CLAMP 2) and the wrap size per axis (0 = no mask).
    // The image's size is the tile window, so a mask past it is clamped in the
    // shader.
    float tex_size[2][2] = {{1, 1}, {1, 1}};
    int tex_cms[2] = {2, 2};
    int tex_cmt[2] = {2, 2};
    int tex_masks[2] = {0, 0};
    int tex_maskt[2] = {0, 0};
    // OGRE_TEXWINCLAMP: the tile rect in texels a wrapped sample is bounded to
    // (0 = no bound). The tile's own rect, not the wrap mask.
    int tex_win[2][2] = {{0, 0}, {0, 0}};
    // The tile's G_TX shift fields. RT64 scales the interpolated coordinate by
    // `ShiftScaleMap[shifts]`/`[shiftt]` (`hle/rt64_state.cpp:33-51`,
    // `shaders/TextureSampler.hlsli:226`) before the tile origin is
    // subtracted; `tex_shift` holds the table value the shader multiplies.
    int tex_shifts[2] = {0, 0};
    int tex_shiftt[2] = {0, 0};
    float tex_shift[2][2] = {{1.0f, 1.0f}, {1.0f, 1.0f}};
    int tex_filter = 0;   // OTHERMODE_H G_TF_*: 0 POINT, 2 BILERP, 3 AVERAGE
    // OTHERMODE_H G_TP_PERSP, bit 19 (`shared/rt64_f3d_defines.h:51`
    // `G_MDSFT_TEXTPERSP`). RT64 halves a non-perspective, non-rect draw's
    // texture coordinate (`perspCorrectionMod` in TextureSampler.hlsli:221-226)
    // to compensate for the missing perspective divide.
    bool tex_persp = true;
    int tex_sc = 0, tex_tc = 0;   // raw G_TEXTURE scale fields (diagnostics)
    bool blend = false;
    bool scissor_on = false;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    // The RDP colour image this draw renders into (RenderState::cimg_*). The
    // main thread renders it into the GL framebuffer object for that guest
    // address, so the pixels exist in RDRAM when the game reads them back.
    uint32_t fb_address = 0;
    uint32_t fb_row_bytes = 0;
    // The render target's size in pixels: the colour image's width and the
    // screen height the viewport defines (they are 320x240 for most of the
    // game, 640x480 for the world map).
    uint16_t fb_width_px = 0, fb_height_px = 0;
    uint8_t fb_siz = 0, fb_fmt = 0;
    // G_SETZIMG's configuration at draw time (RenderState::zimg_*), stored on
    // the target for the `[GFX-DEPTH]` census.
    uint32_t zimg_address = 0;
    uint16_t zimg_width = 0;
    uint8_t zimg_siz = 0, zimg_fmt = 0;
    // Depth pipeline, decoded from OTHERMODE_L exactly as RT64 decodes its
    // pipeline flags (render/rt64_framebuffer_renderer.cpp:1729-1730):
    //   depth_test  = zCmp && !copy && zMode != ZMODE_DEC
    //   depth_write = zUpd && !copy
    // `z_mode` keeps the raw G_MDSFT_ZMODE field so a decal draw states itself
    // (the decal test is not implemented; see note_depth_census).
    bool depth_test = false;
    bool depth_write = false;
    bool z_source_prim = false;   // OTHERMODE_L G_ZS_PRIM (bit 2)
    // This draw's display list uses F3DEX2.NoN: RT64's `flags.NoN` disables the
    // F3D near-depth clip, so `u_zclip` must stay 0 for it.
    bool no_near_clip = false;
    float prim_depth = 0.0f;      // G_SETPRIMDEPTH's 0..1 screen depth
    int z_mode = 0;               // 0 OPA, 0x400 INTER, 0x800 XLU, 0xC00 DEC
    // NDC depth per vertex, alongside pos[]. The clip-space z the vertex shader
    // writes is `z_ndc * w` (see transform_vertex).
    float z[6] = {};
    // Test-only: the synthetic depth control's last triangle asks the flush to
    // read the target's centre pixel back and log `[GFX-DEPTH-TEST]`.
    bool depth_probe = false;
    // Test-only: the synthetic alpha-compare control (`test_draw`, debug flag
    // bit 9) marks its three rectangles 1..3, and the flush counts the target
    // pixels each one drew and logs `[GFX-AC-TEST]`.
    int ac_probe = 0;
    // Test-only: the writeback-dither control (`test_draw`, debug flag bit 10).
    // 1 = G_CD_MAGICSQ target, 2 = G_CD_DISABLE target; the flush reads each
    // target back and logs `[GFX-DITHER-TEST]`.
    int dither_probe = 0;
};

// RT64's Float4ToRGBA16 (`src/shaders/Formats.hlsli`) and its writeback dither.
// RGB rounds through 8 bits, the colour-dither value is added (0..7) and the
// result keeps 5 bits; the alpha coverage bit is bit 2 of the 16-bit alpha. When
// the colour image is a 16-bit float target this is the only quantization
// between the fragment shader and RDRAM; with the 8-bit fallback the caller has
// the same value already rounded, and the two agree except at a rounding tie.
//
// `dither` is `DitherPatternValue`'s 0..7 for this pixel. RT64 adds it before
// the shift (`FbWriteColorCS.hlsl`), so a value whose low three bits cross a
// 5-bit boundary moves by one 5-bit step and a value that does not is
// unchanged. A 32-bit image has no dither (`Float4ToRGBA32`).
inline uint16_t float4_to_rgba16(float r, float g, float b, float a,
                                 uint32_t dither = 0) {
    auto c5 = [dither](float v) {
        const long n = std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        return static_cast<uint16_t>(
            std::min(n + static_cast<long>(dither), 255L) >> 3);
    };
    const long cvg = std::lround(std::clamp(a, 0.0f, 1.0f) * 65535.0f) % 8;
    return static_cast<uint16_t>((c5(r) << 11) | (c5(g) << 6) | (c5(b) << 1) |
                                 ((cvg & 0x4) ? 1 : 0));
}

// RT64's `DitherPatternBayer` / `DitherPatternMagicSquare` and
// `DitherPatternValue` (`src/shaders/Formats.hlsli`). The coordinate is the
// framebuffer pixel, so the same guest texel always gets the same value.
inline uint32_t dither_pattern_value(uint32_t pattern, int x, int y,
                                     uint32_t random_seed) {
    static constexpr uint32_t kBayer[16] = {
        0, 4, 1, 5,
        4, 0, 5, 1,
        3, 7, 2, 6,
        7, 3, 6, 2,
    };
    static constexpr uint32_t kMagicSquare[16] = {
        0, 6, 1, 7,
        4, 2, 5, 3,
        3, 5, 2, 4,
        7, 1, 6, 0,
    };
    const uint32_t index = (static_cast<uint32_t>(y) & 3u) * 4u +
                           (static_cast<uint32_t>(x) & 3u);
    switch (pattern) {
        case 0: return kMagicSquare[index];   // G_CD_MAGICSQ
        case 1: return kBayer[index];         // G_CD_BAYER
        case 2: return random_seed & 7u;      // G_CD_NOISE
        default: return 0;                    // G_CD_DISABLE
    }
}

// RT64's `initRand` (`src/shaders/Random.hlsli`). Only `G_CD_NOISE` reads it,
// with the writeback's seed and the guest texel index, as `FbWriteColorCS.hlsl`
// does; the two patterns OB64 uses are position-only.
inline uint32_t dither_init_rand(uint32_t v0, uint32_t v1, uint32_t backoff = 16) {
    uint32_t s0 = 0;
    for (uint32_t n = 0; n < backoff; ++n) {
        s0 += 0x9e3779b9u;
        v0 += ((v1 << 4) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4u);
        v1 += ((v0 << 4) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761eu);
    }
    return v0;
}

// RT64 `Framebuffer::bestDitherPattern`: the most-used G_CD index, and the first
// one on a tie. `G_CD_DISABLE` (3) is index 3, so a framebuffer whose draws all
// disable dithering gets dither value 0.
inline uint32_t best_dither_pattern(const std::array<uint32_t, 4>& counts) {
    return static_cast<uint32_t>(
        std::max_element(counts.begin(), counts.end()) - counts.begin());
}

// A GL framebuffer object standing in for one RDP colour image. The guest
// address is the key: everything the game draws into that address renders here,
// and sync_framebuffers() writes the pixels back into RDRAM in the RDP's own
// pixel layout. Only touched on the browser main thread.
struct GlFramebuffer {
    GLuint fbo = 0;
    GLuint tex = 0;
    // The depth attachment currently attached to `fbo`. The renderbuffers
    // belong to `depth_targets_`, keyed by the RDP depth image (G_SETZIMG) and
    // its size, not by this colour image: RT64 keys its depth RenderTarget by
    // the depth image's own framebuffer address, width and siz
    // (`RenderTargetKey(depthFb->addressStart, depthFb->width, depthFb->siz,
    // Framebuffer::Type::Depth)`), so one depth buffer serves every colour image
    // that names it at the same size. This field only records what is attached
    // right now, so a colour-image switch does not re-attach an unchanged depth
    // buffer. The depth values live on the GPU only: they are not written back
    // to RDRAM and not uploaded from it.
    GLuint depth_rb = 0;
    // The last G_SETZIMG configuration this target was drawn with, for the
    // `[GFX-DEPTH]` census.
    uint32_t z_address = 0;
    uint16_t z_width = 0;
    uint8_t z_siz = 0, z_fmt = 0;
    int width = 0, height = 0;
    uint32_t row_bytes = 0;   // the guest row pitch (width << siz >> 1)
    uint8_t siz = 0, fmt = 0;
    bool fp = false;          // the attachment is RGBA16F, not RGBA8
    bool inited = false;      // the RDRAM content has been uploaded
    // Rows (window coordinates, 0 = bottom) that have been drawn into since the
    // last writeback. The writeback copies only this band: rows the RDP never
    // touched still hold what RDRAM held when the target was created.
    int min_row = 1 << 30;
    int max_row = -1;
    struct FbRect {
        int x0, y0, x1, y1;
    };
    // Exact pixel coverage of this target's rect and fill draws since the last
    // writeback. A triangle's equivalent rectangle is a bounding box, so it is
    // not recorded. The framebuffer-as-texture copy takes the GPU's pixels inside
    // these rects and RDRAM's bytes outside; the writeback still copies the row
    // band the draws touched.
    std::vector<FbRect> exact_rects;
    // Monotonic stamp of the last draw into this target. Presentation picks the
    // most recently written framebuffer when the VI origin is contained in more
    // than one (RT64 uses its own lastWriteTimestamp the same way).
    uint32_t last_write_seq = 0;
    // Non-black pixel count from the most recent writeback, so presentation can
    // report how much colour the target it is about to show actually holds.
    uint32_t last_nonblack = 0;
    // How many of this target's draws used each G_CD mode (OTHERMODE_H bits
    // 6-7), for RT64 `Framebuffer::bestDitherPattern`. The writeback applies
    // the most-used pattern, the way RT64 chooses the pattern for its
    // `FbWriteColorCS`; the counts are cleared after a writeback (RT64 clears
    // them at the end of a workload).
    std::array<uint32_t, 4> dither_patterns{};
};

// One RDP depth image (`G_SETZIMG`) as a GL renderbuffer. RT64's depth
// RenderTarget is keyed by the depth image's address, width and siz and a colour
// RenderTarget by the colour image's, so the two are independent objects
// (hle/rt64_state.cpp `getTargetsFromPair`: `fbKey.colorTargetKey` and
// `fbKey.depthTargetKey` are separate keys). A pass that renders into colour
// image A with depth image Z, then into colour image B with the same Z, keeps
// Z's contents in RT64; a depth buffer attached to the colour image instead
// would start B at the clear value and lose them.
//
// The depth image's own `G_SETZIMG` word carries no usable size in this game
// (width 1, siz 0, fmt 0: the RDP only keeps the address), so the buffer is
// sized from the colour target that draws into it.
//
// The size is part of the key. RT64 keys a depth target by the image's address,
// width and siz, so a 320-wide scene and a 640-wide scene that name the same
// depth image get two targets there as well. GL requires it regardless: a
// framebuffer whose colour and depth attachments differ in size is incomplete
// (`GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS`), and every draw into it is rejected.
// Scene `0x0A` (the publisher still) creates depth image `0x2E6840` at 640x481;
// a title frame then names the same image for its 320x476 colour target
// (session 186), and sharing one grown renderbuffer left the title's whole
// display list rejected and the canvas showing uninitialised texture bytes.
struct GlDepthTarget {
    GLuint rb = 0;
    int width = 0;
    int height = 0;
    uint32_t z_address = 0;    // the address the key was built from (depth image, else colour)
    uint32_t created_at = 0;   // draw sequence at creation, for the census
};

class WebGLRenderer final : public ultramodern::renderer::RendererContext {
  public:
    WebGLRenderer() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        // Depth test/write switch. On by default; `OGRE_DEPTH=0` turns the test
        // and the write off, which is the A/B for a frame whose depth ordering is
        // in question.
        const char* depth_env = getenv("OGRE_DEPTH");
        depth_enabled_ = !(depth_env != nullptr && depth_env[0] == '0');
        const char* tex_trace = getenv("OGRE_TEX_TRACE");
        trace_ = tex_trace != nullptr && tex_trace[0] != '\0' && tex_trace[0] != '0';
        // OGRE_TEX_TRACE_AFTER_MS / OGRE_TEX_TRACE_MS mirror the native
        // renderer's window (app/src/renderer.cpp:1583): trace only the frames
        // inside [after, after+ms). Without them a natural boot spends the
        // trace on the publisher stills and the title arrives thousands of
        // draws later, past kTraceMaxDraws. Both are wall-clock milliseconds
        // since the runtime started, the clock ram_key uses.
        const char* trace_after = getenv("OGRE_TEX_TRACE_AFTER_MS");
        trace_after_ms_ = (trace_after != nullptr)
                              ? std::strtoull(trace_after, nullptr, 0)
                              : 0ull;
        const char* trace_span = getenv("OGRE_TEX_TRACE_MS");
        trace_span_ms_ = (trace_span != nullptr) ? std::strtoull(trace_span, nullptr, 0) : 0ull;
        const char* tlut_trace = getenv("OGRE_TLUT_TRACE");
        tlut_trace_ = tlut_trace != nullptr && tlut_trace[0] != '\0' && tlut_trace[0] != '0';
        // Framebuffer-as-texture source copy and the RDRAM writeback are each
        // separately switchable for an A/B: `OGRE_FBTEX=0` makes a texture load
        // read RDRAM as the renderer did before session 193, and
        // `OGRE_FBWRITE=0` makes `ogre_sync_framebuffers()` a no-op.
        // The framebuffer-as-texture copy is opt-in. A same-screen A/B on the
        // scene-0x03 briefing shows that the GPU copy of colour image 0x228080
        // is a white fade frame while RDRAM holds the CPU-composed NOTE text:
        // enabling the copy turns readable text into white bars. RT64's tile
        // copy is only authoritative because RT64 re-uploads RDRAM into the
        // framebuffer when the CPU changed it (`checkRAM` + `uploadRAM`) and
        // writes the framebuffer back at the end of every workload; this
        // renderer does neither, so RDRAM is the source a load must read.
        // `OGRE_FBTEX=1` enables the copy for that future work; `=2` publishes
        // the draws mid-list without the copy; `=3` serves the copy from RDRAM.
        const char* fbtex_env = getenv("OGRE_FBTEX");
        fbtex_publish_only_ = (fbtex_env != nullptr && fbtex_env[0] == '2');
        fbtex_rdram_only_ = (fbtex_env != nullptr && fbtex_env[0] == '3');
        fbtex_enabled_ = (fbtex_env != nullptr &&
                          (fbtex_env[0] == '1' || fbtex_env[0] == '2' ||
                           fbtex_env[0] == '3' ||
                           (fbtex_env[0] != '0' && fbtex_env[0] != '\0')));
        const char* fbwrite_env = getenv("OGRE_FBWRITE");
        fb_writeback_enabled_ = !(fbwrite_env != nullptr && fbwrite_env[0] == '0');
        // OGRE_FRAME_DUMP=1 writes the mission intro picture strips' source
        // image and TMEM at the draw that samples them (see dump_frame_source).
        const char* frame_dump_env = getenv("OGRE_FRAME_DUMP");
        frame_dump_enabled_ =
            frame_dump_env != nullptr && frame_dump_env[0] != '\0' && frame_dump_env[0] != '0';
        // OGRE_TEXWINCLAMP bounds a wrapped texture sample to the tile's own
        // rect (see ensure_tile_image). Off by default: it removes the mission
        // intro's band-seam hairlines, and the wrapped-tile screens (the fog
        // layers, the map's water) are not measured with it on yet.
        const char* winclamp_env = getenv("OGRE_TEXWINCLAMP");
        tex_win_clamp_ =
            winclamp_env != nullptr && winclamp_env[0] != '\0' && winclamp_env[0] != '0';
        OGRE_MILESTONE("RENDERER", "webgl2 renderer pending canvas handoff");
    }

    ~WebGLRenderer() override = default;

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
        return false;
    }

    void enable_instant_present() override {}

    // Texture loads whose source overlapped a colour image that was still in a
    // GL framebuffer (the framebuffer-as-texture gap). Read by ogre_gfx_stats.
    unsigned fbtex_hits() const { return fbtex_hits_.load(std::memory_order_relaxed); }
    unsigned fbtex_syncs() const { return fbtex_syncs_.load(std::memory_order_relaxed); }

    // Live GL textures in the tile-image cache, and the total number of uploads.
    // Both are main-thread state, read by ogre_gfx_stats. Entries survive a
    // bounded age window and byte cap (see flush_commands), so an
    // alternating-frame screen keeps its textures between batches.
    size_t texture_cache_size() const { return gl_textures_.size(); }
    size_t texture_cache_bytes() const { return gl_texture_bytes_; }
    size_t texture_uploads() const { return tex_upload_count_; }
    size_t texture_reused() const { return tex_reused_; }
    size_t texture_released() const { return tex_released_; }
    // Gfx-thread counters: tile decodes and decoded-tile cache hits.
    size_t texture_decodes() const { return tex_decodes_.load(std::memory_order_relaxed); }
    size_t texture_decode_hits() const {
        return tex_decode_hits_.load(std::memory_order_relaxed);
    }

    void set_canvas(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle, int width, int height) {
        std::lock_guard<std::mutex> lock(mutex_);
        canvas_handle_ = handle;
        canvas_width_ = width;
        canvas_height_ = height;
        gl_ready_ = false;
        OGRE_MILESTONE("RENDERER", "webgl2 canvas handed over (%dx%d)", width, height);
        // GL setup is deferred to ogre_gfx_flush() on the browser main thread
        // (the gfx pthread only records commands; no proxied GL).
    }

    // Builds a synthetic F3DEX2 display list in rdram and runs it through the
    // normal DL executor: a fill rect, a textured rect (2x2 RGBA16 test
    // pattern) and a textured quad. Used to validate the render pipeline
    // while the game is still at the title screen (which submits only the
    // boot blanking DL). Test-only.
    void test_draw() {
        if (rdram_ == nullptr) {
            return;
        }
        // Scratch memory must stay inside the low 32 MiB *and* below the point
        // where resolve_address() starts reading the top nibble as a segment
        // index (RT64 fromSegmented). At 0x1FE00000 the texture address resolved
        // through segment 15 (base 0) to 0x00E01000 - zeroed memory - so the
        // synthetic rect drew black. 12 MiB is above the N64's 8 MiB expansion
        // and inside segment 0.
        constexpr uint32_t kDlOff = 0x00C00000;   // scratch DL area (unused)
        constexpr uint32_t kTexOff = 0x00C01000;  // 2x2 RGBA16 texture
        // Scratch colour image for the framebuffer-as-texture control (debug
        // flag bit 5): 320x240 RGBA16 is 0x25800 bytes, so it runs to
        // 0x00C27800. The row control (debug flag bit 6) uses a second image
        // after it, at 0x00C28000, because the two controls need separate drawn
        // row ranges: one image's drawn rows are the union of every draw into
        // it.
        constexpr uint32_t kFbOff = 0x00C02000;
        constexpr uint32_t kFb2Off = 0x00C28000;
        // The depth control (debug flag bit 7) needs its own target: 320x240
        // RGBA16 is 0x25800 bytes, so it sits after kFb2Off's image
        // (0x00C28000..0x00C4D800) at 0x00C4E000. Its depth image address is a
        // scratch guest address with no GL meaning; it exists so the
        // G_SETZIMG path and the `[GFX-DEPTH]` census are exercised.
        constexpr uint32_t kFbDepthOff = 0x00C4E000;
        constexpr uint32_t kFbDepthZOff = 0x00C74000;
        constexpr uint32_t kMtxOff = 0x00C76000;    // the control's scale matrix
        constexpr uint32_t kVtxNearOff = 0x00C76040;  // 3 vertices, z = -16384
        constexpr uint32_t kVtxFarOff = 0x00C76080;   // 3 vertices, z = +16384
        // The alpha-compare control (debug flag bit 9) needs its own target
        // (320x240 RGBA16 = 0x25800 bytes, so 0x00C78000..0x00C9D800) and a 2x2
        // IA16 texture after it.
        constexpr uint32_t kFbAcOff = 0x00C78000;
        constexpr uint32_t kAcTexOff = 0x00CA0000;
        // The writeback-dither control (debug flag bit 10) needs two 320x240
        // RGBA16 targets, one drawn with G_CD_MAGICSQ and one with
        // G_CD_DISABLE. They sit after the alpha control's target and texture:
        // 0x00CC0000..0x00CE5800 and 0x00CF0000..0x00D15800.
        constexpr uint32_t kFbDitherOnOff = 0x00CC0000;
        constexpr uint32_t kFbDitherOffOff = 0x00CF0000;

        auto put32 = [this](uint32_t off, uint32_t v) {
            // rdram stores words byte-reversed (runtime MEM_W convention).
            rdram_[off + 0] = static_cast<uint8_t>(v & 0xFF);
            rdram_[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            rdram_[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            rdram_[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        };

        // 2x2 RGBA16 texture: red, green, blue, white.
        put32(kTexOff + 0, 0xF800);
        put32(kTexOff + 2, 0x07C0);
        put32(kTexOff + 4, 0x003E);
        put32(kTexOff + 6, 0xFFFF);
        // Debug flag bit 12's negative control: every call writes a different
        // red texel, so a cache keyed by content must miss and produce a new
        // texture. A false hit shows as N-1 decode hits and one key. The red
        // word is inside the load's single word per row; the blue/white pair
        // is not, because the synthetic G_LOADTILE copies one word.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 4096u) != 0) {
            put32(kTexOff + 0, 0xF800 ^ (++test_draw_variant_ & 0x1F));
        }

        uint32_t o = kDlOff;
        // G_SETFILLCOLOR dark grey
        put32(o, 0xF7000000); put32(o + 4, 0x20202020); o += 8;
        // G_FILLRECT (0,0)-(63,47)  [10.2 fixed: 63<<2=252, 47<<2=188].
        // Rectangles carry `lrx/lry` in w0 and `ulx/uly` in w1 (the SDK splits
        // them that way and RT64 decodes them that way); the synthetic list used
        // to put both corners in w1, which only worked while the renderer had
        // the two words swapped.
        put32(o, 0xF6000000 | (63 << 2) << 12 | (47 << 2)); put32(o + 4, 0); o += 8;
        // G_SETTIMG RGBA16 width=2 -> texture at kTexOff
        put32(o, 0xFD100001); put32(o + 4, kTexOff); o += 8;
        // G_SETTILE RGBA16, line=1, tmem=0, clamp
        put32(o, 0xF5100200); put32(o + 4, 0x00060000); o += 8;
        // G_SETTILESIZE tile 0: texels (0,0)-(1,1)  [10.2: 1<<2 = 4]
        put32(o, 0xF2000000); put32(o + 4, 0x4004); o += 8;
        // G_LOADTILE tile 0
        put32(o, 0xF4000000); put32(o + 4, 0x4004); o += 8;
        // G_SETCOMBINE, 1-cycle: rgb = (TEXEL0 - 0) * SHADE + 0, the SDK's
        // G_CC_TEXEL0. A 1-cycle draw evaluates the SECOND mux (RT64
        // ColorCombiner::run), so the fields that matter are ca1 (L bits 5-8) =
        // 1 (TEXEL0), cb1 (H bits 24-27) = 8 (ZERO), cc1 (L bits 0-4) = 4
        // (SHADE), cd1 (H bits 6-8) = 0 (COMBINED). Note color selector C has no
        // ONE - 15 is K5, which the shader returns as 0, and the word used
        // before (0xFC121000) selected C = ZERO. Either one draws the rect
        // black, so the probe "passed" while proving nothing.
        put32(o, 0xFC000024); put32(o + 4, 0x08000000); o += 8;
        // G_TEXRECT (0,0)-(63,47), tile 0; RDPHALF_1 s=0,t=0; RDPHALF_2
        // dsdx=32, dtdy=43.
        //
        // The step spreads the whole 2x2 tile over the rectangle, which is what
        // makes this a usable self-check: `u2 - u1 = dsdx * rectWidth / 1024`
        // (see draw_texrect), so 2 texels over the rectangle's 63 pixels is
        // `2 * 1024 / 63 = 32` and 2 texels over its 47 rows is
        // `2 * 1024 / 47 = 43`. The pair was `1, 1` before session 141, when the
        // S/T step was only `dsdx / 32` of the rectangle's width and the pair
        // therefore sampled a 1/16-texel sliver of the tile.
        put32(o, 0xE4000000 | (63 << 2) << 12 | (47 << 2)); put32(o + 4, 0); o += 8;
        put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
        put32(o, 0xF1000000); put32(o + 4, 0x0020002B); o += 8;
        // Positive control for the framebuffer-as-texture diagnostic (debug flag
        // bit 5): render into a colour image and then load a tile out of that
        // same address in the same list, which is the one case the renderer
        // decodes from RDRAM bytes that the draw never wrote there. The
        // `[GFX-FBTEX]` line it must produce is what makes a zero-hit scene
        // sweep meaningful. Nothing else in the list depends on it: it runs
        // after the canvas draw, and the game never sets a colour image at
        // kFbOff, so the GL framebuffer it creates is used only by this list.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 32u) != 0) {
            // G_SETCIMG: fmt=0 (RGBA), siz=2 (16b), width=320, at kFbOff.
            put32(o, 0xFF000000 | (2u << 19) | (320 - 1)); put32(o + 4, kFbOff); o += 8;
            // G_SETFILLCOLOR red, then G_FILLRECT (0,0)-(63,239). The fill
            // covers the left fifth of the image: the positive control's load
            // (texels 0..31) lands inside it, and the negative control's load
            // (texels 160..191, debug flag bit 6) is in the same drawn rows but
            // outside the drawn columns.
            put32(o, 0xF7000000); put32(o + 4, 0x0000F801); o += 8;
            put32(o, 0xF6000000 | ((63 << 2) << 12) | (239 << 2)); put32(o + 4, 0); o += 8;
            // G_SETTIMG the same image, RGBA16, width 320.
            put32(o, 0xFD100000 | (2u << 19) | (320 - 1)); put32(o + 4, kFbOff); o += 8;
            // G_SETTILE tile 0: RGBA16, line 160 (320 texels of 16b), tmem 0,
            // clamp both axes (w1 = 0x00060000, as above).
            put32(o, 0xF5100000 | (2u << 19) | (160u << 9)); put32(o + 4, 0x00060000); o += 8;
            // G_SETTILESIZE tile 0: texels (0,0)-(31,31) [10.2 -> 124]
            put32(o, 0xF2000000); put32(o + 4, (124u << 12) | 124u); o += 8;
            // G_LOADTILE tile 0 (the load tile is also the sampling tile here).
            // The load's own rect is what it copies, so it matches the tile:
            // texels (0,0)-(31,31). A load with a zero rect copies one word and
            // leaves the tile degenerate, which `draw_texrect` drops.
            put32(o, 0xF4000000); put32(o + 4, (124u << 12) | 124u); o += 8;
            // G_SETCOMBINE, the same 1-cycle TEXEL0 pass-through
            put32(o, 0xFC000024); put32(o + 4, 0x08000000); o += 8;
            // G_TEXRECT (0,0)-(63,47), tile 0, sampling the image just rendered
            put32(o, 0xE4000000 | (63 << 2) << 12 | (47 << 2)); put32(o + 4, 0); o += 8;
            put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
            // dsdx/dtdy spread the 32x32 tile over 63x47 pixels.
            put32(o, 0xF1000000); put32(o + 4, 0x02000200); o += 8;
            if ((g_debug_flags.load(std::memory_order_relaxed) & 64u) != 0) {
                // Negative control for the column half of the test (debug flag
                // bit 6, requires bit 5's image): the same colour image, a load
                // whose destination rows the fill drew and whose columns it did
                // not. A test that compares rows only reports it; the load's
                // bytes are outside every column a draw wrote, so it must not.
                // G_SETTILESIZE tile 0: texels (160,0)-(191,31) [10.2 fixed].
                // The upper-left corner is in the command word and the
                // lower-right in the next one, as OP_TEXRECT decodes them.
                put32(o, 0xF2000000 | ((160 << 2) << 12) | 0);
                put32(o + 4, ((191 << 2) << 12) | (31 << 2)); o += 8;
                // G_LOADTILE tile 0 (the load tile is also the sampling tile):
                // texels (160,0)-(191,31), the tile's own rect.
                put32(o, 0xF4000000 | ((160 << 2) << 12) | 0);
                put32(o + 4, ((191 << 2) << 12) | (31 << 2)); o += 8;
                // The same 1-cycle TEXEL0 pass-through.
                put32(o, 0xFC000024); put32(o + 4, 0x08000000); o += 8;
                // G_TEXRECT (160,0)-(223,47), tile 0, sampling texels 160..191.
                put32(o, 0xE4000000 | (223 << 2) << 12 | (47 << 2));
                put32(o + 4, (160 << 2) << 12 | 0); o += 8;
                put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
                put32(o, 0xF1000000); put32(o + 4, 0x02000200); o += 8;

                // Negative control for the row half of the test (debug flag
                // bit 6): a second colour image whose fill covers rows 100..163
                // only, and a load of rows 98..99, whose columns are inside the
                // fill's. Those rows are outside every row the fill drew and
                // inside the band the pre-session-155 inversion recorded: the
                // guest row was `height - 1 - wy`, which is `y - 1` for row 100,
                // so a fill of 100..163 was recorded as 99..163 and the row test
                // reported the load at row 99. The inversion is exact now, so
                // the load must not be reported. The fill does not start at row
                // 0: its top row is clamped to 0 (`std::max(gy, 0)`), which
                // hides a one-row over-report there. The load spans two rows
                // because `draw_texrect` drops a tile with `lrt == ult` (RT64's
                // degenerate-tile guard), so a one-row load never reaches the
                // decode.
                put32(o, 0xFF000000 | (2u << 19) | (320 - 1)); put32(o + 4, kFb2Off); o += 8;
                put32(o, 0xF7000000); put32(o + 4, 0x0000F801); o += 8;
                // G_FILLRECT (0,100)-(63,163): rows 100..163, columns 0..63.
                put32(o, 0xF6000000 | ((63 << 2) << 12) | (163 << 2));
                put32(o + 4, (0 << 2) << 12 | (100 << 2)); o += 8;
                // G_SETTIMG the second image, RGBA16, width 320.
                put32(o, 0xFD100000 | (2u << 19) | (320 - 1)); put32(o + 4, kFb2Off); o += 8;
                // G_SETTILE tile 0: RGBA16, line 160 (320 texels of 16b), clamp.
                put32(o, 0xF5100000 | (2u << 19) | (160u << 9));
                put32(o + 4, 0x00060000); o += 8;
                // G_SETTILESIZE tile 0: texels (0,98)-(31,99) - rows 98 and 99.
                put32(o, 0xF2000000 | ((0 << 2) << 12) | (98 << 2));
                put32(o + 4, ((31 << 2) << 12) | (99 << 2)); o += 8;
                // G_LOADTILE tile 0 (the load tile is also the sampling tile):
                // texels (0,98)-(31,99), the tile's own rect.
                put32(o, 0xF4000000 | (98 << 2));
                put32(o + 4, ((31 << 2) << 12) | (99 << 2)); o += 8;
                // The same 1-cycle TEXEL0 pass-through.
                put32(o, 0xFC000024); put32(o + 4, 0x08000000); o += 8;
                // G_TEXRECT (0,100)-(63,163), tile 0, sampling texels 0..31 of
                // rows 98..99. Its own draw lands inside the fill's rows, so it
                // does not add a row to the image's drawn range.
                put32(o, 0xE4000000 | (63 << 2) << 12 | (163 << 2));
                put32(o + 4, (0 << 2) << 12 | (98 << 2)); o += 8;
                put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
                put32(o, 0xF1000000); put32(o + 4, 0x02000200); o += 8;
            }
        }
        // The depth pipeline's synthetic control (debug flag bit 7): two
        // identical triangles at different depths, drawn in the adversarial
        // order - the NEAR one first, the FAR one second - so a renderer
        // without a depth test shows the far colour and one with a test shows
        // the near colour. Bit 8 (256) is the negative control: it clears
        // Z_CMP and keeps Z_UPD, so the far triangle must overwrite the near
        // one. The last triangle is marked for the renderer's pixel readback
        // (`[GFX-DEPTH-TEST]`).
        //
        // Geometry/state: a modelview matrix with 1/32768 on the diagonal (0
        // integer part, fraction 2), so a vertex of +-32767 lands at NDC
        // +-0.99997; the two triangles carry z = -16384 (screen depth 0.25)
        // and z = +16384 (0.75), both inside RT64's F3D depth clip
        // (1022/1024). The combiner is `(0 - 0) * COMBINED + PRIM`, i.e. the
        // primitive colour, which is what makes the readback a single value.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 128u) != 0) {
            auto put16 = [this](uint32_t base, int off, uint16_t v) {
                // The logical halfword at `off` lives at byte `off ^ 2` (see
                // n64h): store it little-endian there.
                const uint32_t a = base + static_cast<uint32_t>(off ^ 2);
                rdram_[a] = static_cast<uint8_t>(v & 0xFF);
                rdram_[a + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            };
            auto put8 = [this](uint32_t base, int off, uint8_t v) {
                rdram_[base + static_cast<uint32_t>(off ^ 3)] = v;
            };
            // read_matrix reads the matrix's halves with a plain rd16 at
            // `addr + i*2` and `addr + 32 + i*2`, not through n64h's `^ 2`
            // addressing, so the matrix needs its own little-endian writer.
            auto put16raw = [this](uint32_t base, int off, uint16_t v) {
                rdram_[base + static_cast<uint32_t>(off)] = static_cast<uint8_t>(v & 0xFF);
                rdram_[base + static_cast<uint32_t>(off) + 1] =
                    static_cast<uint8_t>((v >> 8) & 0xFF);
            };
            // Scale matrix: int parts all zero except m[3][3] = 1, fraction 2
            // on the three diagonal entries (2/65536 = 1/32768). read_matrix
            // swaps the column pairs (`c ^ 1`), so store through the same swap.
            for (int i = 0; i < 16; ++i) {
                put16raw(kMtxOff, i * 2, 0);
                put16raw(kMtxOff, 32 + i * 2, 0);
            }
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    const int i = r * 4 + (c ^ 1);
                    if (r == c && r < 3) {
                        put16raw(kMtxOff, 32 + i * 2, 2);     // fraction 1/32768
                    } else if (r == 3 && c == 3) {
                        put16raw(kMtxOff, i * 2, 1);          // integer 1
                    }
                }
            }
            // 3 vertices at (x, y) = (-32767,-32767), (32767,-32767),
            // (-32767,32767): after the y flip they are NDC (-1,1), (1,1),
            // (-1,-1), the region y >= x. NDC (-0.6, -0.2) - the pixel the
            // readback samples - is well inside it.
            const int16_t vx[3] = {-32767, 32767, -32767};
            const int16_t vy[3] = {-32767, -32767, 32767};
            for (int set = 0; set < 2; ++set) {
                const uint32_t base = set == 0 ? kVtxNearOff : kVtxFarOff;
                const int16_t vz = set == 0 ? -16384 : 16384;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t vbase = base + static_cast<uint32_t>(i * 16);
                    put16(vbase, 0, static_cast<uint16_t>(vx[i]));
                    put16(vbase, 2, static_cast<uint16_t>(vy[i]));
                    put16(vbase, 4, static_cast<uint16_t>(vz));
                    put16(vbase, 6, 0);
                    put16(vbase, 8, 0);
                    put16(vbase, 10, 0);
                    put8(vbase, 12, 255);
                    put8(vbase, 13, 255);
                    put8(vbase, 14, 255);
                    put8(vbase, 15, 255);
                }
            }
            // G_SETCIMG the control's target, G_SETZIMG a scratch depth image.
            put32(o, 0xFF000000 | (2u << 19) | (320 - 1)); put32(o + 4, kFbDepthOff); o += 8;
            put32(o, 0xFE000000 | (2u << 19) | (320 - 1)); put32(o + 4, kFbDepthZOff); o += 8;
            // G_RDPSETOTHERMODE: OTHERMODE_H = 0 (1-cycle), OTHERMODE_L =
            // Z_CMP|Z_UPD|AA_EN|IM_RD (0x78). Bit 8 clears Z_CMP only.
            const uint32_t oml = (g_debug_flags.load(std::memory_order_relaxed) & 256u) != 0
                                     ? 0x68u : 0x78u;
            put32(o, 0xEF000000); put32(o + 4, oml); o += 8;
            // G_SETCOMBINE: rgb = (ZERO - ZERO) * COMBINED + PRIM, alpha = ONE
            // (cycle-1 fields; see Combiner::decode).
            put32(o, 0xFC000100); put32(o + 4, 0x08E000FE); o += 8;
            // G_MTX MODELVIEW|LOAD|NOPUSH: the F3DEX2 stored byte is
            // `flags ^ G_MTX_PUSH`, so 0x02 ^ 0x01 = 0x03.
            put32(o, 0xDA000003); put32(o + 4, kMtxOff); o += 8;
            // The near set into slots 0..2, the far set into slots 3..5.
            put32(o, 0x01003006); put32(o + 4, kVtxNearOff); o += 8;
            put32(o, 0x0100300C); put32(o + 4, kVtxFarOff); o += 8;
            // Near first, red, then far, blue. G_TRI1 packs each index doubled:
            // v1 at bits 16-23, v2 at bits 8-15, v0 at bits 0-7, which is what
            // the executor's p0(17,7)/p0(9,7)/p0(1,7) reads back.
            put32(o, 0xFA000000); put32(o + 4, 0xFF0000FF); o += 8;
            put32(o, 0x05000000 | (1 * 2 << 16) | (2 * 2 << 8) | (0 * 2)); put32(o + 4, 0); o += 8;
            put32(o, 0xFA000000); put32(o + 4, 0x0000FFFF); o += 8;
            put32(o, 0x05000000 | (4 * 2 << 16) | (5 * 2 << 8) | (3 * 2)); put32(o + 4, 0); o += 8;
        }
        // The alpha-compare control (debug flag bit 9): three identical
        // rectangles on a scratch target, each drawn with a combiner alpha of
        // 0.2 (G_SETPRIMCOLOR's alpha), differing only in OTHERMODE_L's
        // alpha-compare field and in the blend colour's alpha:
        //
        //   1. G_AC_THRESHOLD (L bits 0-1 = 1) with blend alpha 0: every
        //      fragment passes.
        //   2. G_AC_THRESHOLD with blend alpha 1: every fragment is discarded.
        //   3. G_AC_DITHER (L bits 0-1 = 3): a fraction of the fragments pass.
        //
        // A combiner alpha of 0.2 is the mission band's own situation: a glyph
        // texel whose alpha the compare rejects.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 512u) != 0) {
            // The proven 2x2 RGBA16 texture the canvas rectangle above uses,
            // and a combiner alpha taken from PRIMITIVE (0.2), so the control
            // measures the compare and not the texture decode.
            put32(o, 0xFF000000 | (2u << 19) | (320 - 1)); put32(o + 4, kFbAcOff); o += 8;
            put32(o, 0xF7000000); put32(o + 4, 0x00000000); o += 8;
            put32(o, 0xF6000000 | ((319 << 2) << 12) | (239 << 2)); put32(o + 4, 0); o += 8;
            put32(o, 0xFD100001); put32(o + 4, kTexOff); o += 8;
            put32(o, 0xF5100200); put32(o + 4, 0x00060000); o += 8;
            put32(o, 0xF2000000); put32(o + 4, 0x4004); o += 8;
            put32(o, 0xF4000000); put32(o + 4, 0x4004); o += 8;
            // G_SETPRIMCOLOR: white with alpha 0x33 (0.2).
            put32(o, 0xFA000000); put32(o + 4, 0xFFFFFF33); o += 8;
            // G_SETCOMBINE, cycle-1 fields: rgb = (TEXEL0 - ZERO) * SHADE + 0,
            // alpha = (PRIM - PRIM) * COMBINED + PRIM = 0.2.
            put32(o, 0xFC000024); put32(o + 4, 0x0860001B); o += 8;
            for (int i = 0; i < 3; ++i) {
                put32(o, 0xEF000000); put32(o + 4, (i == 2) ? 3u : 1u); o += 8;
                put32(o, 0xF9000000); put32(o + 4, (i == 1) ? 0x000000FFu : 0x00000000u);
                o += 8;
                const int y0 = 10 + i * 50, y1 = 49 + i * 50;
                put32(o, 0xE4000000 | ((49 << 2) << 12) | (y1 << 2));
                put32(o + 4, ((10 << 2) << 12) | (y0 << 2)); o += 8;
                put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
                put32(o, 0xF1000000); put32(o + 4, 0x00340034); o += 8;
            }
        }
        // The writeback-dither control (debug flag bit 10). Two identical
        // full-image fills, one with G_CD_MAGICSQ (OTHERMODE_H bits 6-7 = 0) and
        // one with G_CD_DISABLE (3). The fill colour 0x214D decodes to
        // (33, 41, 49, 255), and each channel's low three bits are 1, so a
        // dither of 7 moves every channel one 5-bit step (0x214D -> 0x298F)
        // while every smaller value leaves the word unchanged. The flush reads
        // both targets back from RDRAM and counts the two words.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 1024u) != 0) {
            const uint32_t targets[2] = {kFbDitherOnOff, kFbDitherOffOff};
            const uint32_t othermode_h[2] = {0u, 3u << 6};   // MAGICSQ, DISABLE
            for (int i = 0; i < 2; ++i) {
                put32(o, 0xFF000000 | (2u << 19) | (320 - 1));
                put32(o + 4, targets[i]); o += 8;
                put32(o, 0xEF000000 | (othermode_h[i] & 0xFFFFFFu));
                put32(o + 4, 0); o += 8;
                put32(o, 0xF7000000); put32(o + 4, 0x214D214Du); o += 8;
                put32(o, 0xF6000000 | ((319 << 2) << 12) | (239 << 2));
                put32(o + 4, 0); o += 8;
            }
        }
        // G_ENDDL
        put32(o, 0xDF000000); put32(o + 4, 0x00000000); o += 8;

        RenderState st;
        st.othermode_l = 0;
        st.othermode_h = 0;
        st.combiner.decode(0, 0);
        // The synthetic control carries no game scene, so it is never a wide
        // pass.
        widescreen_expand_ = false;
        // execute_dl() records DrawCmds into the execution-local buffer; publish
        // them under a short try_lock. This hook runs on the browser main
        // thread, which must never block on the renderer mutex.
        ExecCtx ctx;
        execute_dl(rdram_, kDlOff, st, ctx);
        // The depth control's last triangle carries the pixel readback marker,
        // and the next flush reports it (`note_depth_probe`). Both run on the
        // browser main thread, so no lock or atomic is needed for the flag.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 128u) != 0) {
            for (auto it = ctx.draws.rbegin(); it != ctx.draws.rend(); ++it) {
                if (it->fb_address == kFbDepthOff) {
                    it->depth_probe = true;
                    depth_probe_pending_ = true;
                    break;
                }
            }
        }
        // The alpha-compare control's three rectangles carry markers 1..3, and
        // the flush counts each one's surviving pixels (`note_ac_probe`).
        if ((g_debug_flags.load(std::memory_order_relaxed) & 512u) != 0) {
            int next = 3;
            for (auto it = ctx.draws.rbegin(); it != ctx.draws.rend() && next > 0; ++it) {
                if (it->fb_address == kFbAcOff && it->kind == 2 && it->textured) {
                    it->ac_probe = next--;
                }
            }
        }
        // The dither control's two fills carry markers 1 (MAGICSQ) and 2
        // (DISABLE), and the flush reads each target back (`[GFX-DITHER-TEST]`).
        if ((g_debug_flags.load(std::memory_order_relaxed) & 1024u) != 0) {
            for (DrawCmd& d : ctx.draws) {
                if (d.fb_address == kFbDitherOnOff) {
                    d.dither_probe = 1;
                } else if (d.fb_address == kFbDitherOffOff) {
                    d.dither_probe = 2;
                }
            }
        }
        // The texture-cache control marks this list's uploads so the flush can
        // report whether each content key was uploaded or reused.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 2048u) != 0) {
            for (TexUpload& t : ctx.tex) {
                t.probe = true;
            }
        }
        {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                fprintf(stderr, "[web-renderer] test_draw skipped publish: renderer busy\n");
                return;
            }
        }
        publish_exec(ctx);
        fprintf(stderr, "[web-renderer] test_draw: %u bytes of DL recorded\n", o - kDlOff);
    }

    // ----- guest framebuffers (browser main thread only) -------------------------
    //
    // The RDP renders into a guest colour image rather than into the canvas. The
    // game reads that image back out of RDRAM with the CPU (the njpeg backdrop
    // assembly), so the pixels have to reach RDRAM in the RDP's own layout:
    // 16-bit RGBA5551 words stored the way the runtime's MEM_H/MEM_W expect.

    // Reads one colour-image texel from RDRAM as RGBA8. Mirrors RT64's
    // RGBA16ToFloat4 / RGBA32ToFloat4.
    static void fb_texel_to_rgba8(const uint8_t* p, uint32_t off, uint8_t siz, uint8_t out[4]) {
        if (siz == gbi::IM_SIZ_32b) {
            out[0] = n64b(p, static_cast<int>(off) + 0);
            out[1] = n64b(p, static_cast<int>(off) + 1);
            out[2] = n64b(p, static_cast<int>(off) + 2);
            out[3] = n64b(p, static_cast<int>(off) + 3);
        } else {
            rgba16_to_rgba8(n64h(p, static_cast<int>(off)), out);
        }
    }

    // GL texture row 0 is window row 0, which is the *bottom* of the image, so
    // the upload flips: guest row gy lands in texture row height-1-gy. All the
    // draws and the present blit use the same convention (NDC y = +1 is the
    // image's row 0), and the readback flips back.
    void upload_framebuffer(GlFramebuffer& fb, uint32_t address) {
        if (rdram_ == nullptr || address == 0) {
            return;
        }
        const int w = fb.width, h = fb.height;
        const uint32_t pitch = fb.row_bytes;
        if (pitch == 0 ||
            static_cast<uint64_t>(address) + static_cast<uint64_t>(pitch) * h > gbi::kRdramSize) {
            return;   // not a buffer this renderer can address
        }
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
        const uint8_t bpt = static_cast<uint8_t>(bytes_per_texel(fb.siz));
        for (int gy = 0; gy < h; ++gy) {
            const uint8_t* src = rdram_ + address + static_cast<uint32_t>(gy) * pitch;
            uint8_t* dst = px.data() + static_cast<size_t>(h - 1 - gy) * w * 4;
            for (int x = 0; x < w; ++x) {
                fb_texel_to_rgba8(src, static_cast<uint32_t>(x) * bpt, fb.siz, dst + x * 4);
            }
        }
        glBindTexture(GL_TEXTURE_2D, fb.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (fb.fp) {
            // A sized float texture takes float client data only: WebGL2 rejects
            // `format=RGBA, type=UNSIGNED_BYTE` for an RGBA16F internal format
            // (INVALID_OPERATION, and a texSubImage that writes nothing). Scale
            // the decoder's 8-bit values instead; the decode itself is the same
            // one RT64 feeds its render target (`RGBA16ToFloat4` etc.).
            std::vector<float> fpx(px.size());
            for (size_t i = 0; i < px.size(); ++i) {
                fpx[i] = px[i] / 255.0f;
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, fpx.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        // Do not leave the colour target's own texture bound: the flush binds
        // this texture to a sampler unit, and WebGL rejects a draw into a
        // framebuffer whose texture is still attached to a unit the program
        // samples (a feedback loop, `INVALID_OPERATION`). Session 181 saw fills
        // dropped this way; the same target's first draw is the one that hits it.
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // The key of the depth buffer a draw uses: the depth image address when the
    // draw set `G_SETZIMG`, else the colour image (the boot blanking lists), plus
    // the buffer's width and height. The high bit keeps the two address
    // namespaces apart (the mission's depth image `0x3350C0` is also one of its
    // colour image addresses); the size keeps a 320-wide pass and a 640-wide pass
    // of one depth image separate, the same dimensions RT64 puts in its depth
    // `RenderTargetKey`. Widths and heights are capped at 1024 by
    // kMaxFramebufferSide, so 11 bits each.
    static uint64_t depth_target_key(uint32_t address, int width, int height, bool depth) {
        return (depth ? (uint64_t{1} << 63) : 0) |
               (static_cast<uint64_t>(address) << 22) |
               (static_cast<uint64_t>(width & 0x7FF) << 11) |
               static_cast<uint64_t>(height & 0x7FF);
    }

    // The depth renderbuffer for a draw, created (and registered for its
    // one-time clear to the far value) on first use. Sized from the colour
    // target because this game's G_SETZIMG word carries no usable size. The
    // size is part of the key, so the target never spans two sizes; the grow
    // branch below is a guard.
    GLuint depth_target_for(const DrawCmd& cmd, int width, int height) {
        if (cmd.zimg_address != 0) {
            depth_image_addrs_.insert(cmd.zimg_address);
        }
        const uint32_t address = cmd.zimg_address != 0 ? cmd.zimg_address : cmd.fb_address;
        GlDepthTarget& d = depth_targets_[
            depth_target_key(address, width, height, cmd.zimg_address != 0)];
        d.z_address = address;
        const int w = std::max(width, 1);
        const int h = std::max(height, 1);
        if (d.rb != 0 && (d.width < w || d.height < h)) {
            glDeleteRenderbuffers(1, &d.rb);
            d.rb = 0;
        }
        if (d.rb == 0) {
            const int nw = std::max(w, d.width);
            const int nh = std::max(h, d.height);
            glGenRenderbuffers(1, &d.rb);
            glBindRenderbuffer(GL_RENDERBUFFER, d.rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, nw, nh);
            d.width = nw;
            d.height = nh;
            d.created_at = ++depth_target_seq_;
            depth_needs_clear_.insert(d.rb);
            if (depth_targets_logged_ < 8) {
                ++depth_targets_logged_;
                OGRE_MILESTONE("GFX-DEPTH",
                               "depth target #%u for SETZIMG addr=0x%06X %dx%d "
                               "(colour 0x%06X %dx%d, GL_DEPTH_COMPONENT24)",
                               d.created_at, cmd.zimg_address, nw, nh, cmd.fb_address, w, h);
            }
        }
        return d.rb;
    }

    // Creates the GL framebuffer for a draw's colour image if it does not exist
    // yet, and uploads what RDRAM already holds so a partial frame keeps the
    // bytes the game wrote there with the CPU.
    GlFramebuffer& ensure_framebuffer(const DrawCmd& cmd) {
        GlFramebuffer& fb = gl_framebuffers_[cmd.fb_address];
        // The colour image's own width is the guest truth (320 or 640). The
        // height comes from the draw (see target_height_px): the RDP command
        // carries no height, so it is the VI's scanout height, the viewport's
        // screen height and the scissor's bottom row together.
        const int width = (cmd.fb_width_px >= 1 && cmd.fb_width_px <= kMaxFramebufferSide)
                              ? cmd.fb_width_px
                              : canvas_width_;
        int height = (cmd.fb_height_px >= 1 && cmd.fb_height_px <= kMaxFramebufferSide)
                         ? cmd.fb_height_px
                         : canvas_height_;
        // RT64 FramebufferManager::get keeps the tallest height a framebuffer has
        // been asked for while its width and siz do not change, and resets it when
        // they do. Without that, a workload whose draws do not cover the target
        // (a partial scissor, or the one frame lag on a VI mode switch) would
        // shrink the target, recreate it, and lose the pixels already in it.
        if (fb.fbo != 0 && fb.width == width && fb.siz == cmd.fb_siz) {
            height = std::max(height, fb.height);
        }
        const uint32_t row_bytes = cmd.fb_row_bytes != 0
                                       ? cmd.fb_row_bytes
                                       : static_cast<uint32_t>(width << cmd.fb_siz >> 1);
        if (fb.fbo != 0 &&
            (fb.width != width || fb.height != height || fb.siz != cmd.fb_siz ||
             fb.row_bytes != row_bytes)) {
            glDeleteFramebuffers(1, &fb.fbo);
            glDeleteTextures(1, &fb.tex);
            // The depth renderbuffer is owned by `depth_targets_` (keyed by the
            // depth image) and survives this colour target's recreation, the way
            // RT64's depth RenderTarget survives a colour target resize.
            fb = GlFramebuffer{};
        }
        if (fb.fbo == 0) {
            fb.width = width;
            fb.height = height;
            fb.siz = cmd.fb_siz;
            fb.fmt = cmd.fb_fmt;
            fb.row_bytes = row_bytes;
            // The RDP's blender writes each draw's result into its internal
            // colour buffer, and RT64's high-precision mode keeps that buffer
            // at 16 bits (`RenderTarget::colorBufferFormat`, an R16G16B16A16
            // target) so a stack of translucent draws quantizes once, when the
            // framebuffer is written back to RDRAM, not at every draw. An
            // RGBA8 attachment quantizes at every blend instead, which is the
            // 1-3 level difference the title's fog rows showed (session 162).
            // `EXT_color_buffer_float` makes RGBA16F renderable; without it the
            // 8-bit attachment stays and only the precision of blended pixels
            // differs from RT64.
            fb.fp = hp_fb_;
            glGenTextures(1, &fb.tex);
            glBindTexture(GL_TEXTURE_2D, fb.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, fb.fp ? GL_RGBA16F : GL_RGBA8, width, height, 0,
                         GL_RGBA, fb.fp ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenFramebuffers(1, &fb.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   fb.tex, 0);
            // The depth attachment is not created here: it belongs to the draw's
            // G_SETZIMG depth image (see depth_target_for), and the flush
            // attaches it before the draw. A colour image with no depth image is
            // a complete framebuffer on its own.
            fb.depth_rb = 0;
            glDepthMask(GL_TRUE);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE && fb.fp) {
                // A context can advertise EXT_color_buffer_float and still refuse
                // the attachment (no float memory left, a software path). Keep the
                // 8-bit attachment: it costs blend precision, not the frame.
                OGRE_MILESTONE("GFX-FB",
                               "RGBA16F attachment 0x%06X incomplete (status 0x%04X); "
                               "falling back to RGBA8",
                               cmd.fb_address, static_cast<unsigned>(status));
                fb.fp = false;
                glBindTexture(GL_TEXTURE_2D, fb.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, nullptr);
                hp_fb_ = false;   // every later colour image takes the 8-bit path
            }
            const GLenum final_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (final_status != GL_FRAMEBUFFER_COMPLETE) {
                OGRE_MILESTONE("GFX-FB", "framebuffer 0x%06X incomplete (status 0x%04X)",
                               cmd.fb_address, static_cast<unsigned>(final_status));
                glDeleteFramebuffers(1, &fb.fbo);
                glDeleteTextures(1, &fb.tex);
                fb.fbo = 0;
                fb.tex = 0;
                fb.depth_rb = 0;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return fb;
            }
            if (framebuffers_logged_ < 16) {
                ++framebuffers_logged_;
                OGRE_MILESTONE("GFX-FB",
                               "framebuffer 0x%06X: %dx%d siz=%u fmt=%u pitch=%u %s depth=24",
                               cmd.fb_address, width, height, (unsigned)cmd.fb_siz,
                               (unsigned)cmd.fb_fmt, row_bytes,
                               fb.fp ? "rgba16f" : "rgba8");
            }
            // The first colour target, once: the `[GFX-DEPTH]` census (gfx
            // thread) states what the display list asked for, and this states
            // what the pipeline has to work with.
            if (!depth_target_logged_) {
                depth_target_logged_ = true;
                OGRE_MILESTONE("GFX-DEPTH",
                               "colour target 0x%06X %dx%d; depth comes from SETZIMG "
                               "addr=0x%06X fmt=%u siz=%u width=%u (keyed by the depth image)",
                               cmd.fb_address, width, height, cmd.zimg_address,
                               (unsigned)cmd.zimg_fmt, (unsigned)cmd.zimg_siz,
                               (unsigned)cmd.zimg_width);
            }
            fb.inited = false;
            fb.exact_rects.clear();
        }
        // The depth image the display list last named, kept on the target for
        // the census. A zero address means the game never sent G_SETZIMG.
        if (cmd.zimg_address != 0) {
            fb.z_address = cmd.zimg_address;
            fb.z_width = cmd.zimg_width;
            fb.z_siz = cmd.zimg_siz;
            fb.z_fmt = cmd.zimg_fmt;
        }
        if (!fb.inited) {
            upload_framebuffer(fb, cmd.fb_address);
            fb.inited = true;
        }
        return fb;
    }

    // Test-only readback for the synthetic alpha-compare control (`test_draw`,
    // debug flag bit 9): counts the fragments of one of the control's three
    // rectangles that the alpha compare let through. The target is a scratch
    // colour image filled with black, so a discarded fragment reads black and a
    // surviving one reads the IA16 texture's white.
    void note_ac_probe(const DrawCmd& cmd, GlFramebuffer* fb) {
        if (fb == nullptr || fb->fbo == 0) {
            OGRE_MILESTONE("GFX-AC-TEST", "rect=%d no target bound", cmd.ac_probe);
            return;
        }
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (int i = 0; i < cmd.vertex_count; ++i) {
            minx = std::min(minx, cmd.pos[i * 2 + 0]);
            maxx = std::max(maxx, cmd.pos[i * 2 + 0]);
            miny = std::min(miny, cmd.pos[i * 2 + 1]);
            maxy = std::max(maxy, cmd.pos[i * 2 + 1]);
        }
        const int gx0 = std::clamp(static_cast<int>((minx + 1.0f) * 0.5f * fb->width) + 1, 0,
                                   fb->width - 1);
        const int gx1 = std::clamp(static_cast<int>((maxx + 1.0f) * 0.5f * fb->width) - 1, gx0,
                                   fb->width - 1);
        const int gy0 = std::clamp(static_cast<int>((miny + 1.0f) * 0.5f * fb->height) + 1, 0,
                                   fb->height - 1);
        const int gy1 = std::clamp(static_cast<int>((maxy + 1.0f) * 0.5f * fb->height) - 1, gy0,
                                   fb->height - 1);
        const int w = gx1 - gx0 + 1, h = gy1 - gy0 + 1;
        static std::vector<uint8_t> block;
        block.resize(static_cast<size_t>(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (fb->fp) {
            // The control target takes the renderer's high-precision attachment
            // (RGBA16F), and WebGL2 rejects GL_UNSIGNED_BYTE for it with
            // INVALID_OPERATION, which read as an all-zero block. Read floats
            // and scale them, as `note_depth_probe` does.
            static std::vector<float> fblock;
            fblock.resize(static_cast<size_t>(w) * h * 4);
            glReadPixels(gx0, gy0, w, h, GL_RGBA, GL_FLOAT, fblock.data());
            for (size_t i = 0; i + 3 < fblock.size(); i += 4) {
                for (int c = 0; c < 4; ++c) {
                    block[i + c] = static_cast<uint8_t>(std::lround(
                        std::clamp(fblock[i + c], 0.0f, 1.0f) * 255.0f));
                }
            }
        } else {
            glReadPixels(gx0, gy0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, block.data());
        }
        int lit = 0, peak = 0;
        for (size_t i = 0; i + 3 < block.size(); i += 4) {
            const int m = std::max({block[i], block[i + 1], block[i + 2]});
            if (m > 20) {
                ++lit;
            }
            peak = std::max(peak, m);
        }
        OGRE_MILESTONE("GFX-AC-TEST",
                       "rect=%d acmp=%d blend_a=%.3f om_l=0x%06X window=%d,%d..%d,%d "
                       "lit=%d/%d peak=%d",
                       cmd.ac_probe, cmd.acmp, cmd.blend_color[3], cmd.othermode_l, gx0, gy0,
                       gx1, gy1, lit, w * h, peak);
    }

    // Test-only readback for the synthetic depth control (`test_draw`, debug
    // flag bit 7): reads one pixel of the target the control drew into, right
    // after the draw, and logs it as `[GFX-DEPTH-TEST]`. The target is a scratch
    // colour image whose depth buffer was cleared to 1.0 when it was created, so
    // the result does not depend on any other draw or on the canvas.
    //
    // The pixel is the one the control's triangles cover at NDC (-0.6, -0.2):
    // window x = 0.2 * width, y = 0.4 * height (window row 0 is NDC y = -1).
    void note_depth_probe(const DrawCmd& cmd, GlFramebuffer* fb) {
        if (!depth_probe_pending_) {
            return;
        }
        depth_probe_pending_ = false;
        if (fb == nullptr || fb->fbo == 0) {
            OGRE_MILESTONE("GFX-DEPTH-TEST", "no depth target bound for the control draw");
            return;
        }
        const int px_x = static_cast<int>(fb->width / 5);
        const int px_y = static_cast<int>(fb->height * 2 / 5);
        unsigned out[4] = {0, 0, 0, 0};
        if (fb->fp) {
            float f[4] = {0, 0, 0, 0};
            glReadPixels(px_x, px_y, 1, 1, GL_RGBA, GL_FLOAT, f);
            for (int i = 0; i < 4; ++i) {
                out[i] = static_cast<unsigned>(
                    std::lround(std::clamp(f[i], 0.0f, 1.0f) * 255.0f));
            }
        } else {
            uint8_t b[4] = {0, 0, 0, 0};
            glReadPixels(px_x, px_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
            for (int i = 0; i < 4; ++i) {
                out[i] = b[i];
            }
        }
        OGRE_MILESTONE("GFX-DEPTH-TEST",
                       "target=0x%06X %dx%d pixel=(%d,%d) rgba=(%u,%u,%u,%u) "
                       "zcmp=%d zupd=%d zmode=0x%03X",
                       cmd.fb_address, fb->width, fb->height, px_x, px_y,
                       out[0], out[1], out[2], out[3],
                       cmd.depth_test ? 1 : 0, cmd.depth_write ? 1 : 0,
                       (unsigned)cmd.z_mode);
    }

    // Writes a framebuffer's rendered rows back into RDRAM in the RDP's layout:
    // a 16-bit RGBA5551 value V at logical byte offset `off` is read by the
    // runtime as `rd16(p + (off ^ 2))`, so V's low byte goes to `off ^ 2` and its
    // high byte to `off ^ 2 + 1` (RT64 does the same with a word swap of the
    // whole native buffer).
    // Returns true when rows were actually written, with the guest row range in
    // `first_row`/`last_row` (for the diagnostic).
    bool readback_framebuffer(GlFramebuffer& fb, uint32_t address, int* first_row,
                              int* last_row) {
        if (rdram_ == nullptr || address == 0 || fb.fbo == 0) {
            return false;
        }
        if (fb.max_row < fb.min_row) {
            return false;   // nothing drawn since the last writeback
        }
        const int w = fb.width, h = fb.height;
        const uint32_t pitch = fb.row_bytes;
        const int wy0 = std::max(0, fb.min_row);
        const int wy1 = std::min(h - 1, fb.max_row);
        fb.min_row = 1 << 30;
        fb.max_row = -1;
        if (wy1 < wy0 || pitch == 0 ||
            static_cast<uint64_t>(address) + static_cast<uint64_t>(pitch) * h > gbi::kRdramSize) {
            return false;
        }
        const int rows = wy1 - wy0 + 1;
        static std::vector<uint8_t> px;
        static std::vector<float> fpx;
        px.resize(static_cast<size_t>(w) * rows * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (fb.fp) {
            // The attachment holds the blend result at 16-bit float precision.
            // Read that, and let `float4_to_rgba16` do RT64's single 8-bit
            // rounding step; `px` keeps the rounded bytes for the counters.
            fpx.resize(static_cast<size_t>(w) * rows * 4);
            glReadPixels(0, wy0, w, rows, GL_RGBA, GL_FLOAT, fpx.data());
            for (size_t i = 0; i < fpx.size(); i += 4) {
                for (int c = 0; c < 4; ++c) {
                    px[i + c] = static_cast<uint8_t>(std::lround(
                        std::clamp(fpx[i + c], 0.0f, 1.0f) * 255.0f));
                }
            }
        } else {
            glReadPixels(0, wy0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }

        const uint8_t bpt = static_cast<uint8_t>(bytes_per_texel(fb.siz));
        // RT64 picks the most-used G_CD mode for the whole writeback
        // (`Framebuffer::copyRenderTargetToNative` -> `NativeTarget::copyToNative`
        // -> `FbWriteColorCS.hlsl`). The seed advances once per copy, including
        // the depth copy RT64 makes in the same pass; this renderer has no depth
        // writeback, and only `G_CD_NOISE` reads the seed.
        const uint32_t dither_pattern = best_dither_pattern(fb.dither_patterns);
        const uint32_t dither_seed = dither_random_seed_;
        for (int i = 0; i < rows; ++i) {
            // glReadPixels returns row wy0 first; window row wy is guest row
            // height-1-wy (see upload_framebuffer).
            const int gy = h - 1 - (wy0 + i);
            uint8_t* dst = rdram_ + address + static_cast<uint32_t>(gy) * pitch;
            const uint8_t* src = px.data() + static_cast<size_t>(i) * w * 4;
            const float* fsrc = fb.fp ? fpx.data() + static_cast<size_t>(i) * w * 4
                                      : nullptr;
            for (int x = 0; x < w; ++x) {
                const uint8_t* t = src + x * 4;
                const uint32_t off = static_cast<uint32_t>(x) * bpt;
                uint32_t dither = 0;
                // RT64 `Float4ToUINT` passes the dither only to
                // `Float4ToUINT16` (`FbCommon.hlsli:144-154`); the 8-bit and
                // 32-bit arms ignore it. OB64's colour images are 16-bit.
                if (fb.siz == gbi::IM_SIZ_16b) {
                    // `FbWriteColorCS.hlsl`: the seed is
                    // `initRand(ditherRandomSeed, dstIndex, 16)` with
                    // `dstIndex = guest_y * write_width + x`, and the pattern
                    // value is read at the same guest pixel. Only the NOISE
                    // pattern consumes the seed, so the two table patterns skip
                    // the 16-round hash.
                    const uint32_t dst_index =
                        static_cast<uint32_t>(gy) * static_cast<uint32_t>(w) +
                        static_cast<uint32_t>(x);
                    const uint32_t random_seed =
                        (dither_pattern == 2)
                            ? dither_init_rand(dither_seed, dst_index, 16)
                            : 0u;
                    dither = dither_pattern_value(dither_pattern, x, gy, random_seed);
                }
                if (fb.siz == gbi::IM_SIZ_32b) {
                    dst[(off + 0) ^ 3] = t[0];
                    dst[(off + 1) ^ 3] = t[1];
                    dst[(off + 2) ^ 3] = t[2];
                    dst[(off + 3) ^ 3] = t[3];
                } else {
                    // RT64 Float4ToRGBA16 with this pixel's dither value; the
                    // alpha bit is the coverage test `(round(a*255) % 8) & 4`
                    // in the standard format and `(round(a*65535) % 8) & 4` in
                    // the high-precision one (`usesHDR`). The float source is
                    // the latter's exact input; the byte source is the former's.
                    uint16_t v;
                    if (fsrc != nullptr) {
                        const float* f = fsrc + x * 4;
                        v = float4_to_rgba16(f[0], f[1], f[2], f[3], dither);
                    } else {
                        auto c5 = [dither](uint8_t c) {
                            return static_cast<uint32_t>(
                                       std::min<unsigned>(c + dither, 255u)) >>
                                   3;
                        };
                        v = static_cast<uint16_t>(
                            (c5(t[0]) << 11) | (c5(t[1]) << 6) | (c5(t[2]) << 1) |
                            (((t[3] % 8) & 4) ? 1 : 0));
                    }
                    dst[off ^ 2] = static_cast<uint8_t>(v & 0xFF);
                    dst[(off ^ 2) + 1] = static_cast<uint8_t>(v >> 8);
                }
            }
        }
        // RT64 clears the counts when the workload that drew these pixels ends
        // (`FramebufferManager::resetTracking`); the next writeback starts from
        // the draws that follow. Keep this writeback's counts for the log.
        const std::array<uint32_t, 4> dither_counts = fb.dither_patterns;
        fb.dither_patterns.fill(0);
        // The drawn pixels are in RDRAM now, so the GPU copy of these exact
        // rectangles is no longer needed for a texture source.
        fb.exact_rects.clear();
        ++dither_random_seed_;
        ++framebuffer_readbacks_;
        if (first_row != nullptr) *first_row = h - 1 - wy1;
        if (last_row != nullptr) *last_row = h - 1 - wy0;
        // How much colour the target actually holds. A framebuffer the game
        // draws into but that reads back empty is the "nothing reached RDRAM"
        // case, and it is invisible without this count.
        size_t nz = 0;
        uint64_t sum = 0;
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            const unsigned v = px[i] + px[i + 1] + px[i + 2];
            if (v) { ++nz; sum += v; }
        }
        fb.last_nonblack = static_cast<uint32_t>(nz);
        if (framebuffer_readbacks_ <= 8 || (framebuffer_readbacks_ % 500) == 0) {
            OGRE_MILESTONE("GFX-FBREAD",
                           "fb 0x%06X rows=[%d,%d] nonBlack=%zu/%d meanRGB=%llu "
                           "dither=%d counts=%u/%u/%u/%u seed=%u",
                           address, h - 1 - wy1, h - 1 - wy0, nz, w * rows,
                           (unsigned long long)(nz ? sum / nz : 0),
                           (int)dither_pattern, dither_counts[0], dither_counts[1],
                           dither_counts[2], dither_counts[3], dither_seed);
        }
        return true;
    }

    void readback_all_framebuffers() {
        if (!fb_writeback_enabled_) {
            fb_writeback_pending_ = 0;
            return;
        }
        for (auto& [addr, fb] : gl_framebuffers_) {
            int first_row = 0, last_row = 0;
            if (readback_framebuffer(fb, addr, &first_row, &last_row) && sync_logged_ < 6) {
                ++sync_logged_;
                OGRE_MILESTONE("GFX-FBSYNC",
                               "wrote back 0x%06X %dx%d siz=%u pitch=%u guestRows=[%d,%d]",
                               addr, fb.width, fb.height, (unsigned)fb.siz, fb.row_bytes,
                               first_row, last_row);
            }
        }
        fb_writeback_pending_ = 0;
    }

    // ----- framebuffer-as-texture diagnostic (gfx thread) ----------------------
    //
    // Deciding this needs no GL state: a recorded draw's DrawCmd already carries
    // the colour image it renders into (fb_address/fb_row_bytes/fb_height_px), so
    // the gfx thread can track which guest byte ranges are only on the GPU and
    // test every texture decode against them. The two functions below are the
    // whole check; see `fb_written_`.
    //
    // `fbtex_mutex_` exists because the synthetic control display list
    // (`test_draw`, debug flag bit 5) runs on the browser main thread while the
    // gfx thread may be recording; both paths touch `fb_written_`. It is held for
    // a handful of instructions, so it cannot stall the main thread the way
    // `mutex_` could (that one is held across a display list walk).

    // The pixels of a recorded write are in RDRAM again once the main thread has
    // written them back, which it publishes with an epoch bump. Caller holds
    // `fbtex_mutex_`.
    void drop_written_after_writeback() {
        const uint32_t epoch = fb_writeback_epoch_.load(std::memory_order_acquire);
        if (epoch != fb_written_epoch_seen_) {
            fb_written_epoch_seen_ = epoch;
            fb_written_.clear();
        }
    }

    void note_fb_write(const DrawCmd& cmd) {
        if (cmd.fb_address == 0 || cmd.fb_row_bytes == 0 || cmd.fb_height_px == 0) {
            return;
        }
        // The guest rows this draw covers. Window row 0 is the image's *bottom*
        // row (`upload_framebuffer` flips), and `ndc_yf` maps guest row y to
        // `1 - 2y/H`, so the guest edge a vertex sits on is
        // `(1 - pos) / 2 * height`: the y mirror of the column inverse below,
        // and not `height - 1 - wy`. That extra `- 1` subtracted one from the
        // recorded rows (session 155): a fill of rows 100..163 was recorded as
        // 99..163, so a load in row 99 was reported. A vertex marks an edge and
        // the lower-right corner is exclusive, so the first row covered is the
        // top edge and the last is the bottom edge minus one. `std::lround`
        // recovers a rect's authored integer exactly; a triangle's fractional
        // edge is rounded to the nearest row. Every draw that writes a colour
        // image here is a rect or a fill.
        int row_min = 1 << 30, row_max = -1;
        for (int i = 0; i < cmd.vertex_count; ++i) {
            const double edge = (1.0 - static_cast<double>(cmd.pos[i * 2 + 1])) * 0.5 *
                                static_cast<double>(cmd.fb_height_px);
            const int e = static_cast<int>(std::lround(edge));
            const int top = std::max(std::min(e, static_cast<int>(cmd.fb_height_px) - 1), 0);
            const int bottom = std::min(e - 1, static_cast<int>(cmd.fb_height_px) - 1);
            row_min = std::min(row_min, top);
            if (bottom >= 0) {
                row_max = std::max(row_max, bottom);
            }
        }
        if (row_max < row_min) {
            return;
        }
        // The byte columns those rows were drawn across. NDC x is
        // `2*x/width - 1` (`ndc_xf`), so the inverse is `(pos + 1) / 2 * width`
        // and it is not flipped the way y is. A draw's x extent is the min/max
        // over its vertices: exact for a rect, a bounding box for a triangle
        // list. `col_max` is one past the last byte. The column range is what
        // tells a load that reads a drawn row from a load that reads a drawn
        // pixel; a test with only the rows reports the first as the second.
        const uint8_t bpt = static_cast<uint8_t>(bytes_per_texel(cmd.fb_siz));
        int col_min = 1 << 30, col_max = -1;
        if (cmd.fb_width_px >= 1 && bpt > 0) {
            for (int i = 0; i < cmd.vertex_count; ++i) {
                const int wx = static_cast<int>((cmd.pos[i * 2] + 1.0f) * 0.5f *
                                                static_cast<float>(cmd.fb_width_px));
                const int cx = std::min(std::max(wx, 0),
                                        static_cast<int>(cmd.fb_width_px) - 1) * bpt;
                col_min = std::min(col_min, cx);
                col_max = std::max(col_max, cx + bpt);
            }
            col_max = std::min(col_max, static_cast<int>(cmd.fb_row_bytes));
        }
        if (col_max < col_min) {
            col_min = 0;
            col_max = -1;   // unknown: the column test below is skipped
        }
        std::lock_guard<std::mutex> lock(fbtex_mutex_);
        drop_written_after_writeback();
        if (fb_written_.size() >= 64) {
            return;
        }
        for (FbWrittenRange& r : fb_written_) {
            if (r.address == cmd.fb_address) {
                r.row_min = std::min(r.row_min, row_min);
                r.row_max = std::max(r.row_max, row_max);
                if (col_max < 0 || r.col_max < 0) {
                    r.col_min = 0;
                    r.col_max = -1;   // one draw's columns unknown: keep no bound
                } else {
                    r.col_min = std::min(r.col_min, col_min);
                    r.col_max = std::max(r.col_max, col_max);
                }
                return;   // one entry per image is enough
            }
        }
        const uint64_t bytes = static_cast<uint64_t>(cmd.fb_row_bytes) * cmd.fb_height_px;
        if (static_cast<uint64_t>(cmd.fb_address) + bytes > gbi::kRdramSize) {
            return;   // not a buffer this renderer can address
        }
        FbWrittenRange r;
        r.begin = cmd.fb_address;
        r.end = static_cast<uint32_t>(cmd.fb_address + bytes);
        r.address = cmd.fb_address;
        r.pitch = cmd.fb_row_bytes;
        r.height = cmd.fb_height_px;
        r.siz = cmd.fb_siz;
        r.row_min = row_min;
        r.row_max = row_max;
        r.col_min = col_max < 0 ? 0 : col_min;
        r.col_max = col_max;
        fb_written_.push_back(r);
    }

    // The image a tile decodes covers rows y0..y0+height at `stride` bytes each,
    // starting at `address`. True when that range overlaps rows and columns a
    // draw actually wrote in a colour image whose pixels are not in RDRAM yet:
    // the decode read stale bytes. `fb_written_` is capped at 64 images, which is
    // far above the handful a display list batch writes (a game writing more
    // would drop the ones past the cap, and that direction loses a report rather
    // than inventing one).
    //
    // The rect's own columns are `x0 .. x0+w` texels, not the whole `stride`:
    // the TMEM load reads byte `address + ty*stride + tx*bpp`, so a 32-texel
    // RGBA16 load out of a 640-byte-pitch image reads 64 bytes of each row,
    // not 640. A test that used the stride for the rect's width claimed columns
    // the load never read (session 154).
    // `out_image` receives the overlapping colour image's guest address.
    // `report` counts and logs the overlap; the load path calls this once before
    // the writeback attempt (report=false) and once after a failed one
    // (report=true), so only loads that actually read stale RDRAM are reported.
    bool note_fb_texture(uint32_t address, int stride, int y0, int height, int w, int h,
                         int x0, int tex_fmt, int tex_siz, uint32_t* out_image = nullptr,
                         bool report = true) {
        if (address == 0 || stride <= 0 || height <= 0) {
            return false;
        }
        // The rect's byte columns: `bytes_per_texel` is 0 for a 4-bit image,
        // whose two texels share a byte (the case RT64's `siz == 4b` handles).
        const int bpp = bytes_per_texel(static_cast<uint8_t>(tex_siz));
        int col_lo = (bpp > 0) ? x0 * bpp : x0 / 2;
        int col_hi = (bpp > 0) ? (x0 + w) * bpp : (x0 + w + 1) / 2;
        if (col_hi > stride) {
            col_hi = stride;   // a rect cannot read past its own row
        }
        if (col_lo < 0) {
            col_lo = 0;
        }
        std::lock_guard<std::mutex> lock(fbtex_mutex_);
        drop_written_after_writeback();
        if (fb_written_.empty()) {
            return false;
        }
        const uint64_t first = address + static_cast<uint64_t>(y0) * stride + col_lo;
        const uint64_t last = address + static_cast<uint64_t>(y0 + height - 1) * stride + col_hi;
        for (const FbWrittenRange& r : fb_written_) {
            if (first >= r.end || last <= r.begin) {
                continue;   // bytes are outside this image
            }
            // Which of the image's rows those bytes fall in. A load whose bytes
            // land only in rows the game never drew reads RDRAM correctly: the
            // image is up to one row taller than what was rendered and the heap
            // past it holds the game's own textures.
            const uint64_t lo = std::max<uint64_t>(first, r.begin) - r.begin;
            const uint64_t hi = std::min<uint64_t>(last, r.end) - 1 - r.begin;
            const int row_lo = static_cast<int>(lo / r.pitch);
            const int row_hi = static_cast<int>(hi / r.pitch);
            if (row_hi < r.row_min || r.row_max < row_lo) {
                continue;   // no row the draw wrote is in this range
            }
            // The columns. The rect's column offset is relative to its own row
            // start, which is `(address - r.begin) mod pitch` into the image's
            // row. The test is only meaningful when every rect row has that
            // same offset, i.e. the image's pitch divides the stride; otherwise
            // it keeps the row-only answer (over-report, never under-report).
            int load_col_lo = col_lo, load_col_hi = col_hi;
            bool col_tested = false;
            if (r.col_max >= 0 && r.pitch > 0 && (stride % static_cast<int>(r.pitch)) == 0) {
                col_tested = true;
                const int64_t delta = static_cast<int64_t>(address) - static_cast<int64_t>(r.begin);
                const int base_col = static_cast<int>(
                    (delta % static_cast<int64_t>(r.pitch) + r.pitch) % r.pitch);
                load_col_lo += base_col;
                load_col_hi += base_col;
                if (load_col_hi <= static_cast<int>(r.pitch) &&
                    (load_col_hi <= r.col_min || r.col_max <= load_col_lo)) {
                    continue;   // drawn rows, outside every drawn column
                }
            }
            if (out_image != nullptr) {
                *out_image = r.address;
            }
            if (!report) {
                return true;
            }
            ++fbtex_hits_;
            // One line per distinct (texture address, image, rect origin): the
            // same image loaded at two rects is two findings, and the first 24
            // are logged.
            uint64_t key = static_cast<uint64_t>(address);
            key = key * 31 + r.address;
            key = key * 31 + static_cast<uint32_t>(x0);
            key = key * 31 + static_cast<uint32_t>(y0);
            char drawn_cols[32];
            if (col_tested) {
                snprintf(drawn_cols, sizeof(drawn_cols), "%d,%d)", r.col_min, r.col_max);
            } else {
                snprintf(drawn_cols, sizeof(drawn_cols), "?)");
            }
            if (fbtex_logged_.size() < 24 && fbtex_logged_.insert(key).second) {
                OGRE_MILESTONE("GFX-FBTEX",
                               "load tex=0x%06X rect=%dx%d stride=%d fmt=%d siz=%d reads "
                               "colour image 0x%06X pitch=%u h=%d siz=%u rows=[%d,%d] drawn "
                               "rows=[%d,%d] cols=[%d,%d) drawn cols=[%s by an earlier draw; "
                               "the load read RDRAM (the GPU copy is off or was refused)",
                               address, w, h, stride, tex_fmt, tex_siz, r.address, r.pitch,
                               r.height, (unsigned)r.siz, row_lo, row_hi, r.row_min, r.row_max,
                               load_col_lo, load_col_hi, drawn_cols);
            }
            return true;
        }
        return false;
    }

    // OGRE_FRAME_DUMP: writes the bytes behind one draw's texture, so a wrong
    // pixel is attributed to the game's data or to this renderer's load/decode
    // instead of inferred. Three files in `OGRE_PREF_DIR` (default /tmp):
    //
    //   texsrc-<n>.bin  the 0x2E00-byte image the strip loads read, in the
    //                   guest's logical byte order (the order `tools/rdram.py`
    //                   reads), so an offline decode is the same code path
    //   tmem-<n>.bin    the 4 KiB TMEM the load left, which is what the
    //                   decoder samples
    // texdec-<n>.bin    the decoded RGBA8 image this draw samples, from the
    //                   debug ring entry with the draw's content key
    //
    // Called from the draw, in the same display-list walk as the load, so the
    // source bytes are the ones the load saw. A timed RDRAM dump cannot bracket
    // this draw: the heap buffer is reused by the screens that follow.
    void dump_frame_source(const TileState& ft) {
        if (rdram_ == nullptr) {
            return;
        }
        const char* env_dir = getenv("OGRE_PREF_DIR");
        const std::string dir =
            (env_dir != nullptr && env_dir[0] != '\0') ? std::string(env_dir) : std::string("/tmp");
        const unsigned n = frame_dump_count_++;
        // The 15 strip loads cover one 92x60 RGBA16 image: 15 * 0x2E0 = 0x2A60
        // bytes, rounded up to 0x2E00 so the last strip's row stride fits.
        const uint32_t len = 0x2E00u;
        if (static_cast<uint64_t>(ft.image_address) + len > gbi::kRdramSize) {
            return;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/texsrc-%u.bin", dir.c_str(), n);
        if (FILE* f = std::fopen(path, "wb")) {
            for (uint32_t i = 0; i < len; ++i) {
                std::fputc(n64b(rdram_, static_cast<int>(ft.image_address + i)), f);
            }
            std::fclose(f);
        }
        std::snprintf(path, sizeof(path), "%s/tmem-%u.bin", dir.c_str(), n);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(tmem_, 1, sizeof(tmem_), f);
            std::fclose(f);
        }
        bool decoded = false;
        std::snprintf(path, sizeof(path), "%s/texdec-%u.bin", dir.c_str(), n);
        {
            std::lock_guard<std::mutex> lock(debug_mutex_);
            for (const DebugTex& d : debug_tex_) {
                if (d.key != ft.image.key || d.pixels.empty()) {
                    continue;
                }
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(d.pixels.data(), 1, d.pixels.size(), f);
                    std::fclose(f);
                }
                decoded = true;
                break;
            }
        }
        OGRE_MILESTONE("GFX-FRAME-DUMP",
                       "n=%u src=0x%06X len=%u fmt=%u siz=%u line=%u tmem=%u key=0x%llX "
                       "decoded=%d dir=%s",
                       n, ft.image_address, len, (unsigned)ft.fmt, (unsigned)ft.siz,
                       (unsigned)ft.line, (unsigned)ft.tmem,
                       (unsigned long long)ft.image.key, decoded ? 1 : 0, dir.c_str());
    }

    // Resizes the canvas drawing buffer to the size the VI is scanning out, and
    // the element's CSS box to an integer multiple of that buffer. Only the
    // browser main thread calls this (from flush_commands' present path), so the
    // DOM calls are legal.
    //
    // The buffer is the VI's visible area (`present_framebuffer`), so the box is
    // that area at 1x or 2x. The CSS box has to be a whole multiple of the
    // drawing buffer. A canvas whose CSS box does not match its buffer is scaled
    // to the box by the browser, and `image-rendering: pixelated` makes that a
    // nearest-neighbour resample, not a display-only stretch: the publisher
    // still's VI mode is 640x476 and index.html had pinned a 640x480 box, so the
    // frame on screen was the 476-row buffer resampled 476 -> 480. Session 148
    // measured the captured canvas as exactly that resample (`docs/proofs/
    // web-publishers-atlus-css-box-fix.png`: canvas row `Y` = target row
    // `floor((Y + 0.5) * 476 / 480)`, mean abs 0.87/0.84/0.75 per channel, 99.6%
    // of channel samples exact), which is what the session-147 "about one row
    // low" difference was.
    static int css_scale_for(int height) {
        return std::max(1, std::min(2, kCanvasCssHeight / std::max(1, height)));
    }

    void resize_canvas(int width, int height) {
        if (width < 1 || height < 1 || width > kMaxFramebufferSide ||
            height > kMaxFramebufferSide) {
            return;
        }
        if (width == canvas_width_ && height == canvas_height_) {
            return;
        }
        emscripten_set_canvas_element_size("#game-canvas", width, height);
        const int scale = css_scale_for(height);
        emscripten_set_element_css_size("#game-canvas", width * scale, height * scale);
        canvas_width_ = width;
        canvas_height_ = height;
        // The RDRAM fallback's staging texture is canvas-sized.
        if (present_tex_ != 0) {
            glDeleteTextures(1, &present_tex_);
            present_tex_ = 0;
        }
        OGRE_MILESTONE("GFX-PRESENT",
                       "canvas resized to %dx%d (css %dx%d, x%d) for the VI's visible area",
                       width, height, width * scale, height * scale, scale);
    }

    // Draws `tex` over `frac_w` x `frac_h` of the canvas with the COPY combiner
    // (TEXEL0 straight through), which is the present path. `u_max`/`v_min` bound
    // the texture region the VI scans out: RT64's present shader maps
    // `videoResolution / textureResolution` over the window
    // (`src/shaders/VideoInterfacePS.hlsl`), so a framebuffer taller than the VI's
    // `fbSize` is cropped rather than squashed with the rows past the colour
    // image. Texture v = 1 is guest row 0 (`upload_framebuffer` flips), so the
    // visible rows are the *high* end of v.
    //
    // `frac_w`/`frac_h` are the destination rectangle as a fraction of the
    // canvas. `present_framebuffer` sizes the canvas to the visible area, so they
    // are 1.0 for every present and the blit writes every canvas pixel. RT64's
    // `VIRenderer::getViewportAndScissor` draws the visible area into a viewport
    // of `visibleFramebufferSize` SD pixels anchored at the window's top-left and
    // leaves the rest of the window alone; the canvas here *is* that area, which
    // is RT64's `removeBlackBorders` case. Stretching the visible area over a
    // larger canvas instead was measured in session 146: it moved the sampled
    // texel by up to one column and row over the right and bottom halves of every
    // frame.
    void blit_to_canvas(GLuint tex, float u_max = 1.0f, float v_min = 0.0f,
                        float frac_w = 1.0f, float frac_h = 1.0f) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, canvas_width_, canvas_height_);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        // The canvas has no depth attachment; the last RDP draw may have left
        // GL_DEPTH_TEST enabled for its colour image.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(program_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        auto seti = [this](const char* name, int v) {
            glUniform1i(glGetUniformLocation(program_, name), v);
        };
        seti("u_cycle", 0);
        seti("u_debug_mode", 0);
        seti("u_zclip", 0);
        seti("u_tex0", 0);
        seti("u_use_tex", 1);
        // The present path samples with normalized UVs, so it uses the GL
        // sampler at the framebuffer texture's own (point) filter; the shader
        // sampler's texel-space uniforms do not apply.
        seti("u_texf", 0);
        glUniform2f(glGetUniformLocation(program_, "u_tsize0"), 1.0f, 1.0f);
        glUniform2f(glGetUniformLocation(program_, "u_torig0"), 0.0f, 0.0f);
        glUniform2i(glGetUniformLocation(program_, "u_tcm0"), 2, 2);
        glUniform2i(glGetUniformLocation(program_, "u_tmask0"), 0, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        // The destination rectangle: NDC spans the visible fraction of the canvas
        // from its top-left corner (NDC x = -1, y = +1).
        const float fw = std::clamp(frac_w, 0.0f, 1.0f);
        const float fh = std::clamp(frac_h, 0.0f, 1.0f);
        const float x0 = -1.0f, x1 = -1.0f + 2.0f * fw;
        const float yt = 1.0f, yb = 1.0f - 2.0f * fh;
        // (x, y, u, v, rgba, w, ndc z) per vertex; two triangles covering the
        // rectangle. w = 1 and z = 0 (the middle of the depth range) as a
        // rectangle would carry, though this draw never tests depth.
        const float quad[6 * 10] = {
            x0, yb, 0,     v_min, 1, 1, 1, 1, 1, 0,
            x1, yb, u_max, v_min, 1, 1, 1, 1, 1, 0,
            x1, yt, u_max, 1,     1, 1, 1, 1, 1, 0,
            x0, yb, 0,     v_min, 1, 1, 1, 1, 1, 0,
            x1, yt, u_max, 1,     1, 1, 1, 1, 1, 0,
            x0, yt, 0,     1,     1, 1, 1, 1, 1, 0,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // The key of the framebuffer whose guest range contains `address`, or 0 when
    // none does. The VI origin is not the RDP colour-image address: the standard
    // NTSC mode's `fldRegs[].origin` is 0x280 (one 320-pixel 16-bit row), so the
    // VI scans out from `framebuffer + 0x280` while the RDP renders at
    // `framebuffer`. RT64 resolves it by containment the same way
    // (FramebufferManager::findMostRecentContaining).
    uint32_t framebuffer_containing(uint32_t address) const {
        uint32_t best = 0;
        uint32_t best_seq = 0;
        for (const auto& [addr, fb] : gl_framebuffers_) {
            if (fb.tex == 0 || fb.row_bytes == 0) {
                continue;
            }
            const uint64_t end = static_cast<uint64_t>(addr) +
                                 static_cast<uint64_t>(fb.row_bytes) * fb.height;
            if (address >= addr && address < end && fb.last_write_seq >= best_seq) {
                best = addr;
                best_seq = fb.last_write_seq;
            }
        }
        return best;
    }

    // Presentation. The VI scans out a guest buffer: show the GL framebuffer the
    // RDP rendered into when there is one, otherwise the bytes RDRAM holds. When
    // the draws went straight to the canvas (no colour image was ever set), the
    // canvas already holds the frame and nothing is blitted over it.
    void present_framebuffer(uint32_t address) {
        // The canvas is the VI's *visible* area, which is the area RT64's
        // presenter writes (`visibleFramebufferSize`, the framebuffer minus the
        // overscan crop). RT64 anchors that area at the window's top-left and
        // leaves the window's last column and row untouched; the drawing buffer
        // here is sized to the area itself, so the 1:1 blit below covers every
        // pixel of it and no column or row is left holding an older frame's
        // contents or the page background. Session 148 measured the alternative
        // (a VI-sized buffer with an unwritten last column and row): the
        // publisher still's whole-frame difference was -0.87/-0.84/-0.75 per
        // channel and its last column and last row differed from RT64's target
        // by 222 and 223 levels on average.
        //
        // A 640x475 mode (the publisher stills) presents 1:1; a 319x239 mode (the
        // title) at the 2x pixelated CSS upscale. `css_scale_for` keeps the box a
        // whole multiple of the buffer.
        const ViSize vi = vi_size();
        const ViSize visible = visible_framebuffer_size(vi);
        const bool have_visible = (visible.width > 0 && visible.height > 0);
        resize_canvas(have_visible ? visible.width : vi.width,
                      have_visible ? visible.height : vi.height);

        // The destination is the whole canvas: buffer and visible area are the
        // same size, so the quad covers it. A present that carries no visible
        // size (the first one, before the VI mode is known) still writes all of
        // the canvas it was given.
        const float present_frac_w = 1.0f;
        const float present_frac_h = 1.0f;
        uint32_t key = address;
        auto it = gl_framebuffers_.find(key);
        if (it == gl_framebuffers_.end() || it->second.tex == 0) {
            key = framebuffer_containing(address);
            it = (key != 0) ? gl_framebuffers_.find(key) : gl_framebuffers_.end();
        }
        const bool have_fbo = (it != gl_framebuffers_.end() && it->second.tex != 0);
        if (present_requests_ <= 12 || (present_requests_ % 300) == 0) {
            ++presents_logged_;
            OGRE_MILESTONE("GFX-PRESENT",
                           "VI scanout 0x%06X -> fb 0x%06X: fbo=%d size=%dx%d siz=%u "
                           "visible=%dx%d canvas=%dx%d nonBlack=%u canvasDirty=%d fbs=%zu status=0x%08X "
                           "(present %u)",
                           address, have_fbo ? key : 0u, have_fbo ? 1 : 0,
                           have_fbo ? it->second.width : 0, have_fbo ? it->second.height : 0,
                           have_fbo ? (unsigned)it->second.siz : 0u,
                           have_fbo ? visible.width : 0, have_fbo ? visible.height : 0,
                           canvas_width_, canvas_height_,
                           have_fbo ? it->second.last_nonblack : 0u,
                           canvas_dirty_ ? 1 : 0, gl_framebuffers_.size(), vi_status_,
                           present_requests_);
        }
        if (have_fbo) {
            // The GL framebuffer is the RDP colour image at the tallest height
            // `ensure_framebuffer` has ever been asked for, so it can be taller
            // than the VI's scanout (a 320x240 image that grew to 320x476 across
            // the publishers' hi-res mode). Present only the visible area, the way
            // RT64 does; the rows past the colour image hold the next one.
            const float u_max = (have_visible && it->second.width > 0)
                                    ? std::min(1.0f, static_cast<float>(visible.width) /
                                                         static_cast<float>(it->second.width))
                                    : 1.0f;
            // Texture v = 1 is guest row 0, so the visible rows are the top of the
            // image and the crop takes the high end of v.
            const float v_min = (have_visible && it->second.height > 0)
                                    ? std::max(0.0f, 1.0f - static_cast<float>(visible.height) /
                                                               static_cast<float>(it->second.height))
                                    : 0.0f;
            blit_to_canvas(it->second.tex, u_max, v_min, present_frac_w, present_frac_h);
            canvas_dirty_ = false;
            return;
        }
        if (canvas_dirty_ || rdram_ == nullptr) {
            canvas_dirty_ = false;
            return;
        }
        const uint8_t siz = ((vi_status_ & 3) == 3) ? gbi::IM_SIZ_32b : gbi::IM_SIZ_16b;
        if (present_tex_ == 0) {
            glGenTextures(1, &present_tex_);
            glBindTexture(GL_TEXTURE_2D, present_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas_width_, canvas_height_, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        GlFramebuffer tmp;
        tmp.tex = present_tex_;
        tmp.width = canvas_width_;
        tmp.height = canvas_height_;
        tmp.siz = siz;
        // The guest framebuffer's row pitch is the VI's scanout width, not the
        // canvas's: the canvas is the visible area, one column narrower than the
        // buffer the VI scans out. Reading the canvas's width as the pitch would
        // take the last texel of every row from the next row's first byte.
        const int pitch_width = (vi.width > 0) ? vi.width : canvas_width_;
        tmp.row_bytes = static_cast<uint32_t>(pitch_width) << siz >> 1;
        upload_framebuffer(tmp, address);
        blit_to_canvas(present_tex_, 1.0f, 0.0f, present_frac_w, present_frac_h);
    }

    // ----- main-thread flush (ogre_gfx_flush) ------------------------------------

    // Runs on the browser main thread: uploads queued textures, then executes
    // queued draws. This is the only place GL is called.
    void flush_commands() {
        if (!ensure_gl()) {
            return;
        }

        // Never block the browser main thread on the gfx pthread's mutex: a
        // contended pthread lock on a browser main thread cannot wait legally
        // (Atomics.wait is disallowed there) and was observed to freeze the
        // whole page while the gfx pthread sat inside send_dl(). Skip this
        // tick instead; the next flush drains the queued work. Skips are
        // counted (flush_skipped_) and surfaced as a one-off stderr note.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            g_flush_skip.fetch_add(1, std::memory_order_relaxed);
            if (flush_skipped_.fetch_add(1) == 0) {
                fprintf(stderr, "[web-renderer] ogre_gfx_flush: renderer busy (gfx pthread "
                                "inside send_dl); skipping flush instead of blocking the main thread\n");
            }
            return;
        }

        // Texture uploads.
        {
            ++texture_epoch_;
            for (TexUpload& up : tex_upload_queue_) {
                // The key is a hash of the decoded bytes, so a resident texture
                // with this key already holds exactly these pixels. Skip the
                // upload instead of re-issuing `glTexImage2D` on every display
                // list that reloads the same image.
                auto resident = gl_textures_.find(up.key);
                if (resident != gl_textures_.end()) {
                    resident->second.last_used = texture_epoch_;
                    ++tex_reused_;
                    if (up.probe) {
                        OGRE_MILESTONE("GFX-TEXPROBE",
                                       "key=0x%llX %dx%d result=reuse decode=%s",
                                       (unsigned long long)up.key, up.width, up.height,
                                       up.decode_hit ? "hit" : "miss");
                    }
                    continue;
                }
                // A decode-cache hit still carries the decoded bytes (shared
                // with the cache), so an evicted GL texture can be re-created
                // from them. An empty pixel list would be a bug in that
                // contract, not a texture to skip.
                if (!up.pixels || up.pixels->empty()) {
                    if (tex_missing_pixels_ < 8) {
                        ++tex_missing_pixels_;
                        OGRE_MILESTONE("GFX-TEXCACHE",
                                       "dropped upload key=0x%llX %dx%d: no pixels",
                                       (unsigned long long)up.key, up.width, up.height);
                    }
                    continue;
                }
                GLuint tex = 0;
                glGenTextures(1, &tex);
                GlTextureEntry& entry = gl_textures_[up.key];
                entry.tex = tex;
                entry.bytes = up.pixels->size();
                entry.last_used = texture_epoch_;
                gl_texture_bytes_ += entry.bytes;
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, up.width, up.height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, up.pixels->data());
                ++tex_upload_count_;
                if (up.probe) {
                    OGRE_MILESTONE("GFX-TEXPROBE",
                                   "key=0x%llX %dx%d result=upload decode=%s",
                                   (unsigned long long)up.key, up.width, up.height,
                                   up.decode_hit ? "hit" : "miss");
                }
                // The shader sampler reads this texture with texelFetch and
                // filters in fp32 itself, so the GL sampler stays point. The
                // wrap modes are only the `OGRE_TEXF=gl` path's; that path sets
                // them again per draw, because the filter mode is draw state
                // (OTHERMODE_H) and a texture can be used by both a bilerp and a
                // point draw.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                up.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                up.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (tex_logged_ < 6) {
                    ++tex_logged_;
                    size_t nz = 0; uint64_t sum = 0;
                    for (size_t i = 0; i + 3 < up.pixels->size(); i += 4) {
                        const unsigned v = (*up.pixels)[i] + (*up.pixels)[i + 1] +
                                           (*up.pixels)[i + 2];
                        if (v) { ++nz; sum += v; }
                    }
                    OGRE_MILESTONE("GFX-TEX",
                                   "tex %u: %dx%d tile=%d key=0x%llX nonzero=%zu/%zu avgRGB=%llu",
                                   tex_logged_, up.width, up.height, up.tile,
                                   (unsigned long long)up.key, nz, up.pixels->size() / 4,
                                   (unsigned long long)(nz ? sum / nz : 0));
                }
            }
            tex_upload_queue_.clear();
        }

        // Draws.
        std::vector<DrawCmd> cmds;
        {
            cmds.swap(draw_queue_);
        }
        // Presentation and framebuffer-sync requests travel with the queue, so
        // the main thread renders everything submitted before them.
        const bool present = present_pending_;
        const uint32_t present_address = present_address_;
        present_pending_ = false;
        const uint32_t sync_request = sync_request_.load(std::memory_order_acquire);
        const bool sync_pending = sync_request > sync_done_.load(std::memory_order_relaxed);

        glUseProgram(program_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        if (!logged_first_flush_ && !cmds.empty()) {
            logged_first_flush_ = true;
            OGRE_MILESTONE("GFX-FLUSH", "first non-empty main-thread flush: %u draw cmds, %u textures uploaded",
                           static_cast<unsigned>(cmds.size()),
                           static_cast<unsigned>(tex_upload_count_));
        }
        bool had_rect = false;
        for (const DrawCmd& c : cmds) {
            if (c.kind != 0) had_rect = true;
        }
        if (blit_logged_ < 6 && !cmds.empty()) {
            ++blit_logged_;
            std::string seq;
            for (const DrawCmd& c : cmds) {
                if (seq.size() >= 120) { seq += "..."; break; }
                seq += c.kind == 1 ? 'F' : (c.kind == 2 ? 'T' : (c.textured ? 't' : 'u'));
            }
            OGRE_MILESTONE("GFX-BLIT", "flush %u: %zu cmds -> %s",
                           flush_count_, cmds.size(), seq.c_str());
        }
        flushed_cmds_ += static_cast<unsigned>(cmds.size());
        ++flush_count_;
        g_flush_ok.store(flush_count_, std::memory_order_relaxed);
        g_flush_cmds.store(flushed_cmds_, std::memory_order_relaxed);

        // Which GL framebuffer the current draw targets (null = the canvas).
        uint32_t bound_fb_address = 0;
        GlFramebuffer* bound_fb = nullptr;

        for (const DrawCmd& cmd : cmds) {
            // Bind the RDP colour image this draw renders into. A zero address
            // means the game never set one, and the draw goes to the canvas -
            // the prototype's original path.
            if (cmd.fb_address != bound_fb_address || bound_fb == nullptr) {
                if (cmd.fb_address != 0) {
                    GlFramebuffer& fb = ensure_framebuffer(cmd);
                    bound_fb = (fb.fbo != 0) ? &fb : nullptr;
                    bound_fb_address = cmd.fb_address;
                } else {
                    bound_fb = nullptr;
                    bound_fb_address = 0;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, bound_fb != nullptr ? bound_fb->fbo : 0);
            }
            // The draw's NDC was built against `cmd.fb_height_px` (the
            // screen height `target_height_px` returned when the draw was
            // recorded), not against the colour image's tallest height. The
            // colour image can be taller than the draw's screen: the title's
            // 320x240 rects go into a 320x476 image that an earlier draw grew,
            // and a viewport of the image's full height stretched the rects
            // over all 476 rows, so the presenter's 239-row visible area showed
            // the top half of the scene at 2x (session 186). Map the NDC into
            // `cmd.fb_height_px` rows at the top of the image, which is where
            // the RDP's screen row 0 is and where the scissor already points.
            if (bound_fb != nullptr) {
                const int vw = std::clamp(static_cast<int>(cmd.fb_width_px), 1,
                                          bound_fb->width);
                const int vh = std::clamp(static_cast<int>(cmd.fb_height_px), 1,
                                          bound_fb->height);
                glViewport(0, bound_fb->height - vh, vw, vh);
            } else {
                glViewport(0, 0, canvas_width_, canvas_height_);
            }
            const int target_height = bound_fb != nullptr ? bound_fb->height : canvas_height_;
            if (bound_fb == nullptr) {
                canvas_dirty_ = true;
            }
            // The RDP's depth image is framebuffer memory: a colour write into
            // the same guest address overwrites the depth contents. RT64 keeps
            // one Framebuffer per address with a last-write type and rebuilds
            // the depth target when that address was last written as colour
            // (hle/rt64_state.cpp:1581 `depthFbTypeChanged`, :1630). Scene 0x0A
            // shares `0x2E6840` between its plate colour image and its depth
            // image, so without this the previous still frame's depth survived
            // and every depth-tested q triangle failed GL_LESS: the QUEST logo
            // lost its three 3D q meshes (session 183).
            if (cmd.fb_address != 0 && depth_image_addrs_.count(cmd.fb_address) != 0) {
                // One depth image can now own several size-keyed renderbuffers
                // (a 320-wide and a 640-wide target for the same address), so
                // invalidate every one of them.
                for (auto& [dk, depth] : depth_targets_) {
                    (void)dk;
                    if (depth.z_address == cmd.fb_address && depth.rb != 0) {
                        depth_needs_clear_.insert(depth.rb);
                    }
                }
            }
            // Attach this draw's depth image. The buffer belongs to
            // G_SETZIMG's address, not to the colour image, so two colour images
            // that name the same depth image share one depth buffer, as they do
            // in RT64. The attach is skipped when the right buffer is already
            // there, so a frame that stays in one colour image pays one compare.
            if (bound_fb != nullptr) {
                const GLuint want_depth = depth_target_for(cmd, bound_fb->width,
                                                           bound_fb->height);
                if (bound_fb->depth_rb != want_depth) {
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_RENDERBUFFER, want_depth);
                    bound_fb->depth_rb = want_depth;
                    // The size is part of the depth key, so a mismatch cannot
                    // happen by construction. Check once per pair anyway: an
                    // incomplete framebuffer rejects every draw silently, and
                    // this is the check that named the title's 640x481 depth /
                    // 320x476 colour mismatch (session 186).
                    const GLenum pair_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                    if (pair_status != GL_FRAMEBUFFER_COMPLETE &&
                        depth_pair_logged_.insert(
                            (static_cast<uint64_t>(bound_fb->fbo) << 32) | want_depth).second) {
                        OGRE_MILESTONE("GFX-DEPTH",
                                       "colour 0x%06X %dx%d with depth 0x%06X: status=0x%04X",
                                       cmd.fb_address, bound_fb->width, bound_fb->height,
                                       cmd.zimg_address, static_cast<unsigned>(pair_status));
                    }
                }
                if (depth_needs_clear_.erase(want_depth) != 0) {
                    // The clear to the far value, on the first draw that uses a
                    // depth image after it was created or invalidated, so
                    // GL_LESS passes everywhere there. RT64 clears its depth
                    // target when it creates it as well and otherwise keeps the
                    // contents.
                    //
                    // The scissor must be off for the clear. This runs inside
                    // the draw loop before the draw's own scissor is applied, so
                    // the clear used to inherit whatever rect the previous draw
                    // left; a depth image shared by a 320x240 screen and a
                    // 640x480 one (scene 0x0A's stills share `0x2E6840`) was
                    // only cleared inside that stale rect and the rest kept the
                    // renderbuffer's zero fill.
                    const GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
                    glDisable(GL_SCISSOR_TEST);
                    glDepthMask(GL_TRUE);
                    glClearDepthf(1.0f);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    if (scissor_was) {
                        glEnable(GL_SCISSOR_TEST);
                    }
                }
                // One-shot: the depth attachment is on the framebuffer this draw
                // uses. A colour-only framebuffer makes GL_DEPTH_TEST a no-op,
                // and every depth measurement would then be meaningless.
                if (cmd.depth_test && !depth_verified_) {
                    depth_verified_ = true;
                    GLint obj = 0;
                    glGetFramebufferAttachmentParameteriv(
                        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
                    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                    OGRE_MILESTONE("GFX-DEPTH",
                                   "first depth-tested draw: depthRenderbuffer=%d status=0x%04X "
                                   "colour=0x%06X depth image=0x%06X",
                                   static_cast<int>(obj), static_cast<unsigned>(st),
                                   cmd.fb_address, cmd.zimg_address);
                }
            }
            if (vert_logged_ < 6) {
                ++vert_logged_;
                char vbuf2[640];
                int o = 0;
                for (int i = 0; i < cmd.vertex_count && o < 560; ++i) {
                    o += snprintf(vbuf2 + o, sizeof(vbuf2) - o, "(%.3f,%.3f u%.2f,%.2f) ",
                                  cmd.pos[i * 2], cmd.pos[i * 2 + 1],
                                  cmd.uv[i * 2], cmd.uv[i * 2 + 1]);
                }
                OGRE_MILESTONE("GFX-V", "draw %u verts=%d tex=%d scale=(%.4f,%.4f) org=(%.1f,%.1f) sc=%d tc=%d: %s",
                               vert_logged_, cmd.vertex_count, cmd.textured ? 1 : 0,
                               cmd.tex_scale[0], cmd.tex_scale[1],
                               cmd.tex_origin[0], cmd.tex_origin[1],
                               cmd.tex_sc, cmd.tex_tc, vbuf2);
            }
            if (draw_logged_ < 6) {
                ++draw_logged_;
                const bool found = cmd.textured && gl_textures_.find(cmd.tex_key) != gl_textures_.end();
                // The mux the hardware actually evaluates (see record_state),
                // as an N64 expression so it can be checked at a glance.
                char mux0[256], mux1[256];
                char blend0[48], blend1[48];
                snprintf(blend0, sizeof(blend0), "[P=%s M=%s A=%s B=%s]",
                         blend_pm_name(cmd.bp[0]), blend_pm_name(cmd.bm[0]),
                         blend_a_name(cmd.ba[0]), blend_b_name(cmd.bb[0]));
                snprintf(blend1, sizeof(blend1), "[P=%s M=%s A=%s B=%s]",
                         blend_pm_name(cmd.bp[1]), blend_pm_name(cmd.bm[1]),
                         blend_a_name(cmd.ba[1]), blend_b_name(cmd.bb[1]));
                snprintf(mux0, sizeof(mux0), "rgb=(%s-%s)*%s+%s a=(%s-%s)*%s+%s",                         color_sel_name(cmd.ca[0]), color_sel_name(cmd.cb[0]),
                         color_sel_name(cmd.cc[0]), color_sel_name(cmd.cd[0]),
                         alpha_sel_name(cmd.aa[0]), alpha_sel_name(cmd.ab[0]),
                         alpha_sel_name(cmd.ac[0]), alpha_sel_name(cmd.ad[0]));
                if (cmd.cycle == 2) {
                    snprintf(mux1, sizeof(mux1), "rgb=(%s-%s)*%s+%s a=(%s-%s)*%s+%s",
                             color_sel_name(cmd.ca[1]), color_sel_name(cmd.cb[1]),
                             color_sel_name(cmd.cc[1]), color_sel_name(cmd.cd[1]),
                             alpha_sel_name(cmd.aa[1]), alpha_sel_name(cmd.ab[1]),
                             alpha_sel_name(cmd.ac[1]), alpha_sel_name(cmd.ad[1]));
                } else {
                    snprintf(mux1, sizeof(mux1), "-");
                }
                OGRE_MILESTONE("GFX-CMD",
                               "draw %u: verts=%d textured=%d tex_found=%d cyc=%d omh=0x%06X oml=0x%08X "
                               "cmb=0x%08X%08X mux0=[%s] mux1=[%s] prim=[%.2f,%.2f,%.2f,%.2f] env=[%.2f,%.2f,%.2f,%.2f] "
                               "ablend=%d force=%d approx=%d ccyc=%d b0=%s b1=%s "
                               "scissor=%d(%d,%d,%d,%d) p0=(%.2f,%.2f) uv0=(%.3f,%.3f)",
                               draw_logged_, cmd.vertex_count, cmd.textured ? 1 : 0, found ? 1 : 0, cmd.cycle,
                               cmd.othermode_h, cmd.othermode_l,
                               cmd.comb_L, cmd.comb_H,
                               mux0, mux1,
                               cmd.prim[0], cmd.prim[1], cmd.prim[2], cmd.prim[3],
                               cmd.env[0], cmd.env[1], cmd.env[2], cmd.env[3],
                               cmd.alpha_blend ? 1 : 0, cmd.force_blend ? 1 : 0, cmd.blend_approx,
                               cmd.combiner_cycles, blend0, blend1,
                               cmd.scissor_on ? 1 : 0,
                               cmd.sx0, cmd.sy0, cmd.sx1, cmd.sy1,
                               cmd.pos[0], cmd.pos[1], cmd.uv[0], cmd.uv[1]);
            }
            // Combiner uniforms.
            auto seti = [this](const char* name, int v) {
                glUniform1i(glGetUniformLocation(program_, name), v);
            };
            seti("u_ca0", cmd.ca[0]); seti("u_cb0", cmd.cb[0]); seti("u_cc0", cmd.cc[0]); seti("u_cd0", cmd.cd[0]);
            seti("u_aa0", cmd.aa[0]); seti("u_ab0", cmd.ab[0]); seti("u_ac0", cmd.ac[0]); seti("u_ad0", cmd.ad[0]);
            seti("u_ca1", cmd.ca[1]); seti("u_cb1", cmd.cb[1]); seti("u_cc1", cmd.cc[1]); seti("u_cd1", cmd.cd[1]);
            seti("u_aa1", cmd.aa[1]); seti("u_ab1", cmd.ab[1]); seti("u_ac1", cmd.ac[1]); seti("u_ad1", cmd.ad[1]);
            seti("u_cycle", cmd.cycle);
            seti("u_debug_mode",
                 static_cast<int>((g_debug_flags.load(std::memory_order_relaxed) >> 3) & 3));
            // Blender (render mode) uniforms.
            seti("u_force_blend", cmd.force_blend ? 1 : 0);
            seti("u_blend_approx", cmd.blend_approx);
            seti("u_combiner_cycles", cmd.combiner_cycles);
            // Alpha compare and coverage. `u_cvg_range` is the attachment's
            // step count: RT64 uses 65535 when the render target is the
            // high-precision (RGBA16F) colour buffer and 255 otherwise.
            seti("u_ac", cmd.acmp);
            // RT64's alpha-dither seeds: the frame count and the draw's index
            // (see the `dither_init_rand` port). Only G_AC_DITHER reads them.
            glUniform1ui(glGetUniformLocation(program_, "u_dither_frame"), present_requests_);
            glUniform1ui(glGetUniformLocation(program_, "u_dither_instance"), flushed_cmds_);
            seti("u_cvg_x_alpha", cmd.cvg_x_alpha ? 1 : 0);
            seti("u_cvg_dst", cmd.cvg_dst);
            seti("u_alpha_blend", cmd.alpha_blend ? 1 : 0);
            glUniform1f(glGetUniformLocation(program_, "u_cvg_range"),
                        hp_fb_ ? 65535.0f : 255.0f);
            seti("u_bp0", cmd.bp[0]); seti("u_bm0", cmd.bm[0]);
            seti("u_ba0", cmd.ba[0]); seti("u_bb0", cmd.bb[0]);
            seti("u_bp1", cmd.bp[1]); seti("u_bm1", cmd.bm[1]);
            seti("u_ba1", cmd.ba[1]); seti("u_bb1", cmd.bb[1]);

            glUniform4fv(glGetUniformLocation(program_, "u_prim"), 1, cmd.prim);
            glUniform4fv(glGetUniformLocation(program_, "u_env"), 1, cmd.env);
            glUniform4fv(glGetUniformLocation(program_, "u_fog_color"), 1, cmd.fog);
            glUniform4fv(glGetUniformLocation(program_, "u_blend_color"), 1, cmd.blend_color);
            glUniform1f(glGetUniformLocation(program_, "u_k4"), cmd.k4);
            glUniform1f(glGetUniformLocation(program_, "u_k5"), cmd.k5);
            glUniform3fv(glGetUniformLocation(program_, "u_key_center"), 1, cmd.key_center);
            glUniform3fv(glGetUniformLocation(program_, "u_key_scale"), 1, cmd.key_scale);

            // One-shot: a draw whose evaluated combiner mux reads KEY_CENTER (B)
            // or KEY_SCALE (C) states itself and the key registers it reads, so
            // a screen that exercises the chroma key reports it instead of
            // needing a guess. `ck_key` is `G_CK_KEY`, OTHERMODE_H bit 8
            // (`shared/rt64_f3d_defines.h:28-30`, `G_MDSFT_COMBKEY`).
            if (!key_logged_) {
                const int mux_count = (cmd.cycle == 2) ? 2 : 1;
                bool uses_key = false;
                for (int m = 0; m < mux_count; ++m) {
                    if (cmd.cb[m] == CSEL_KEY_CENTER || cmd.cc[m] == CSEL_KEY_SCALE) {
                        uses_key = true;
                    }
                }
                if (uses_key) {
                    key_logged_ = true;
                    OGRE_MILESTONE("GFX-KEY",
                                   "at cmd %u a draw reads a key input: "
                                   "center=(%.4f %.4f %.4f) scale=(%.4f %.4f %.4f) "
                                   "ck_key=%d omh=0x%06X cycle=%d",
                                   flushed_cmds_, cmd.key_center[0], cmd.key_center[1],
                                   cmd.key_center[2], cmd.key_scale[0], cmd.key_scale[1],
                                   cmd.key_scale[2],
                                   ((cmd.othermode_h >> 8) & 1u) != 0 ? 1 : 0,
                                   cmd.othermode_h, cmd.cycle);
                }
            }

            // A draw into a framebuffer must not sample that framebuffer's own
            // texture. Bind the 1x1 white fallback to both units before every
            // draw; a textured draw replaces them below. This is the other half
            // of the feedback-loop fix in upload_framebuffer: GL rejects a draw
            // whose program samples a texture attached to the bound framebuffer,
            // and the target's own texture is the one upload_framebuffer left
            // bound.
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, white_tex_);
            glUniform1i(glGetUniformLocation(program_, "u_tex0"), 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, white_tex_);
            glUniform1i(glGetUniformLocation(program_, "u_tex1"), 1);
            glActiveTexture(GL_TEXTURE0);

            if (cmd.textured) {
                // TEXEL0's and TEXEL1's tile images are separate GL textures,
                // each with its own UV transform (the title sprites take their
                // colour from tile 1 and their alpha mask from tile 0).
                auto lookup = [this](uint64_t key) -> GLuint {
                    auto it = gl_textures_.find(key);
                    return (key != 0 && it != gl_textures_.end()) ? it->second.tex : white_tex_;
                };
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, lookup(cmd.tex_key));
                glUniform1i(glGetUniformLocation(program_, "u_tex0"), 0);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, lookup(cmd.tex_key1));
                glUniform1i(glGetUniformLocation(program_, "u_tex1"), 1);
                if (texf_ == 0) {
                    // The GL sampler decides filtering per texture object, not
                    // per draw, so it is set here from this draw's G_TF_* while
                    // each unit is bound.
                    const GLint filt = (cmd.tex_filter != 0) ? GL_LINEAR : GL_NEAREST;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
                    glActiveTexture(GL_TEXTURE0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
                }
                glActiveTexture(GL_TEXTURE0);
                // The RDP sampler state (RT64 RDPTile per unit). The shader
                // samples with texelFetch and filters in fp32; `u_texf` selects
                // the GL fixed-function sampler instead, for the A/B.
                glUniform2f(glGetUniformLocation(program_, "u_tsize0"),
                            cmd.tex_size[0][0], cmd.tex_size[0][1]);
                glUniform2f(glGetUniformLocation(program_, "u_tsize1"),
                            cmd.tex_size[1][0], cmd.tex_size[1][1]);
                glUniform2f(glGetUniformLocation(program_, "u_torig0"),
                            cmd.tex_origin[0], cmd.tex_origin[1]);
                glUniform2f(glGetUniformLocation(program_, "u_torig1"),
                            cmd.tex_origin1[0], cmd.tex_origin1[1]);
                glUniform2i(glGetUniformLocation(program_, "u_tcm0"),
                            cmd.tex_cms[0], cmd.tex_cmt[0]);
                glUniform2i(glGetUniformLocation(program_, "u_tcm1"),
                            cmd.tex_cms[1], cmd.tex_cmt[1]);
                glUniform2i(glGetUniformLocation(program_, "u_tmask0"),
                            cmd.tex_masks[0], cmd.tex_maskt[0]);
                glUniform2i(glGetUniformLocation(program_, "u_tmask1"),
                            cmd.tex_masks[1], cmd.tex_maskt[1]);
                glUniform2f(glGetUniformLocation(program_, "u_twin0"),
                            static_cast<float>(cmd.tex_win[0][0]),
                            static_cast<float>(cmd.tex_win[0][1]));
                glUniform2f(glGetUniformLocation(program_, "u_twin1"),
                            static_cast<float>(cmd.tex_win[1][0]),
                            static_cast<float>(cmd.tex_win[1][1]));
                glUniform2f(glGetUniformLocation(program_, "u_tshift0"),
                            cmd.tex_shift[0][0], cmd.tex_shift[0][1]);
                glUniform2f(glGetUniformLocation(program_, "u_tshift1"),
                            cmd.tex_shift[1][0], cmd.tex_shift[1][1]);
                glUniform1i(glGetUniformLocation(program_, "u_tex_filter"), cmd.tex_filter);
                glUniform1i(glGetUniformLocation(program_, "u_texf"), texf_);
                glUniform1i(glGetUniformLocation(program_, "u_tex_persp"),
                            cmd.tex_persp ? 1 : 0);
                ++samp_draws_;
                if (cmd.tex_filter == 0) {
                    ++samp_point_;
                } else if (cmd.tex_filter == 3) {
                    ++samp_average_;
                } else {
                    ++samp_bilerp_;
                }
                if (cmd.tex_masks[0] > 0 || cmd.tex_maskt[0] > 0 || cmd.tex_masks[1] > 0 ||
                    cmd.tex_maskt[1] > 0) {
                    ++samp_masked_;
                }
                if (cmd.tex_shifts[0] != 0 || cmd.tex_shiftt[0] != 0 ||
                    cmd.tex_shifts[1] != 0 || cmd.tex_shiftt[1] != 0) {
                    ++samp_shifted_;
                }
                if (cmd.kind == 0 &&
                    (std::fabs(cmd.w[0] - cmd.w[1]) > 1e-4f ||
                     std::fabs(cmd.w[1] - cmd.w[2]) > 1e-4f)) {
                    ++samp_persp_;
                }
                if (!cmd.tex_persp) {
                    ++samp_nonpersp_;
                }
                // G_TP family census: one line per distinct (kind, OTHERMODE_H)
                // a textured draw carries, with the G_TP_PERSP reading. It names
                // the families a screen uses, so a claim about the halving rule
                // is measured per family (`changes=` is the two readings of bit
                // 10, the pre-session-174 field, against bit 19).
                {
                    const uint32_t key = (static_cast<uint32_t>(cmd.kind) << 24) |
                                         (cmd.othermode_h & 0xFFFFFFu);
                    if (tp_seen_.count(key) == 0 && tp_seen_.size() < 24) {
                        tp_seen_.insert(key);
                        const bool old_tp =
                            (cmd.kind != 0) || ((cmd.othermode_h >> 10) & 1u) != 0;
                        const bool new_tp = cmd.tex_persp;
                        OGRE_MILESTONE("GFX-TP",
                                       "family kind=%d omh=0x%06X old_persp=%d "
                                       "new_persp=%d changes=%d",
                                       cmd.kind, cmd.othermode_h, old_tp ? 1 : 0,
                                       new_tp ? 1 : 0, (old_tp != new_tp) ? 1 : 0);
                    }
                }

                // Which image each sampler actually got, and where the first
                // vertex lands in it. A sprite whose decoded pair is clean but
                // whose render is speckled is a question about *this* - a key
                // that never reached gl_textures_, a unit that fell back to the
                // 1x1 white texture, or a uv1 outside [0,1] (which clamps to an
                // edge texel, or repeats the image across the quad, depending on
                // the tile's wrap mode).
                if (tex_uv_logged_ < 4) {
                    ++tex_uv_logged_;
                    const bool f0 = cmd.tex_key != 0 && gl_textures_.count(cmd.tex_key) != 0;
                    const bool f1 = cmd.tex_key1 != 0 && gl_textures_.count(cmd.tex_key1) != 0;
                    const float su = cmd.uv[0], sv = cmd.uv[1];
                    OGRE_MILESTONE("GFX-UV",
                                   "k0=0x%llX/%d/id%u k1=0x%llX/%d/id%u "
                                   "scl0=(%.4f %.4f) scl1=(%.4f %.4f) org1=(%.1f %.1f) "
                                   "v0=(%.1f %.1f) -> uv0=(%.3f %.3f) uv1=(%.3f %.3f) e=%X "
                                   "filt=%d size0=%.0fx%.0f cm0=(%d,%d) mask0=(%d,%d) "
                                   "cm1=(%d,%d) mask1=(%d,%d)",
                                   (unsigned long long)cmd.tex_key, f0 ? 1 : 0, lookup(cmd.tex_key),
                                   (unsigned long long)cmd.tex_key1, f1 ? 1 : 0, lookup(cmd.tex_key1),
                                   cmd.tex_scale[0], cmd.tex_scale[1],
                                   cmd.tex_scale1[0], cmd.tex_scale1[1],
                                   cmd.tex_origin1[0], cmd.tex_origin1[1], su, sv,
                                   (su - cmd.tex_origin[0]) * cmd.tex_scale[0],
                                   (sv - cmd.tex_origin[1]) * cmd.tex_scale[1],
                                   (su - cmd.tex_origin1[0]) * cmd.tex_scale1[0],
                                   (sv - cmd.tex_origin1[1]) * cmd.tex_scale1[1],
                                   glGetError(),
                                   cmd.tex_filter, cmd.tex_size[0][0], cmd.tex_size[0][1],
                                   cmd.tex_cms[0], cmd.tex_cmt[0],
                                   cmd.tex_masks[0], cmd.tex_maskt[0],
                                   cmd.tex_cms[1], cmd.tex_cmt[1],
                                   cmd.tex_masks[1], cmd.tex_maskt[1]);
                }
            }

            // Scissor. The RDP's scissor rectangle is half-open like the rest of
            // the RDP's rectangles (`FixedRect`, RT64 `rt64_common.cpp`): RT64
            // rounds a scissor's lower-right corner up into a render rect and the
            // render target's scissor is exclusive, so a scissor of
            // `(0,0)-(319,239)` - what this game sets - covers pixels 0..318 by
            // 0..238 and leaves the last column and row unwritten. GL's scissor is
            // also `[x, x+w)`, so the stored pixel bounds are used as they are
            // instead of adding one back.
            if (cmd.scissor_on) {
                const int x = cmd.sx0;
                const int y = target_height - cmd.sy1;
                const int w = cmd.sx1 - cmd.sx0;
                const int h = cmd.sy1 - cmd.sy0;
                glEnable(GL_SCISSOR_TEST);
                glScissor(x, y, std::max(w, 0), std::max(h, 0));
            } else {
                glDisable(GL_SCISSOR_TEST);
            }

            // Blend. RT64 uses dual-source blending with the factor in the
            // secondary output (SRC1_ALPHA / INV_SRC1_ALPHA); the primary
            // output's alpha carries the same value here, so plain
            // SRC_ALPHA / ONE_MINUS_SRC_ALPHA is equivalent.
            if (cmd.alpha_blend) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            // Depth. RT64's pipeline flags (render/rt64_framebuffer_renderer.cpp
            // :1729-1730) are `zCmp && !copy && zMode != ZMODE_DEC` for the test
            // and `zUpd && !copy` for the write, with `LESS` as the function
            // (render/rt64_raster_shader.cpp:317) and depth enabled whenever
            // either flag is set - a write-only draw gets `ALWAYS` there
            // (`depthFunction = c.zCmp ? LESS : ALWAYS`), which is what GL needs
            // too, because GL writes no depth while GL_DEPTH_TEST is disabled.
            // The canvas has no depth attachment (attrs.depth is off for the
            // default framebuffer), so a draw that targets it never tests depth.
            if (bound_fb != nullptr && bound_fb->depth_rb != 0) {
                if (cmd.depth_test || cmd.depth_write) {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(cmd.depth_test ? GL_LESS : GL_ALWAYS);
                    glDepthMask(cmd.depth_write ? GL_TRUE : GL_FALSE);
                } else {
                    glDisable(GL_DEPTH_TEST);
                    glDepthMask(GL_FALSE);
                }
            } else {
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
            }
            seti("u_zclip", (cmd.kind == 0 && !cmd.z_source_prim && !cmd.no_near_clip) ? 1 : 0);

            // Interleave pos/uv/shade/w/z into the vertex buffer.
            float vbuf[6 * 10];
            const int count = cmd.vertex_count;
            for (int i = 0; i < count; ++i) {
                vbuf[i * 10 + 0] = cmd.pos[i * 2 + 0];
                vbuf[i * 10 + 1] = cmd.pos[i * 2 + 1];
                vbuf[i * 10 + 2] = cmd.uv[i * 2 + 0];
                vbuf[i * 10 + 3] = cmd.uv[i * 2 + 1];
                vbuf[i * 10 + 4] = cmd.shade[i * 4 + 0];
                vbuf[i * 10 + 5] = cmd.shade[i * 4 + 1];
                vbuf[i * 10 + 6] = cmd.shade[i * 4 + 2];
                vbuf[i * 10 + 7] = cmd.shade[i * 4 + 3];
                vbuf[i * 10 + 8] = (cmd.w[i] > 0.0f) ? cmd.w[i] : 1.0f;
                vbuf[i * 10 + 9] = cmd.z[i];
            }
            glBufferData(GL_ARRAY_BUFFER, count * 10 * sizeof(float), vbuf, GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, count);
            if (cmd.depth_probe) {
                note_depth_probe(cmd, bound_fb);
            }
            if (cmd.ac_probe != 0) {
                note_ac_probe(cmd, bound_fb);
            }

            // Remember which rows this draw touched: the readback copies only
            // the band that was drawn into. Window row 0 is NDC y = -1, and it
            // is the *bottom* of the image (see upload_framebuffer).
            if (bound_fb != nullptr) {
                // Exact pixel coverage, for the framebuffer-as-texture copy. A
                // rect or fill draw's window rectangle is exact; a triangle's is
                // a bounding box and is not recorded (see exact_rects).
                if (cmd.kind != 0) {
                    const int vw = std::clamp(static_cast<int>(cmd.fb_width_px), 1,
                                              bound_fb->width);
                    const int vh = std::clamp(static_cast<int>(cmd.fb_height_px), 1,
                                              bound_fb->height);
                    const int y_off = bound_fb->height - vh;
                    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
                    for (int i = 0; i < count; ++i) {
                        minx = std::min(minx, cmd.pos[i * 2 + 0]);
                        maxx = std::max(maxx, cmd.pos[i * 2 + 0]);
                        miny = std::min(miny, cmd.pos[i * 2 + 1]);
                        maxy = std::max(maxy, cmd.pos[i * 2 + 1]);
                    }
                    auto to_x = [vw](float v) {
                        return (static_cast<double>(v) + 1.0) * 0.5 * static_cast<double>(vw);
                    };
                    auto to_y = [vh, y_off](float v) {
                        return (static_cast<double>(v) + 1.0) * 0.5 * static_cast<double>(vh) +
                               static_cast<double>(y_off);
                    };
                    GlFramebuffer::FbRect r;
                    r.x0 = static_cast<int>(std::lround(to_x(minx)));
                    r.x1 = static_cast<int>(std::lround(to_x(maxx)));
                    r.y0 = static_cast<int>(std::lround(to_y(miny)));
                    r.y1 = static_cast<int>(std::lround(to_y(maxy)));
                    if (cmd.scissor_on) {
                        r.x0 = std::max(r.x0, cmd.sx0);
                        r.x1 = std::min(r.x1, cmd.sx1);
                        r.y0 = std::max(r.y0, target_height - cmd.sy1);
                        r.y1 = std::min(r.y1, target_height - cmd.sy0);
                    }
                    r.x0 = std::clamp(r.x0, 0, bound_fb->width);
                    r.x1 = std::clamp(r.x1, 0, bound_fb->width);
                    r.y0 = std::clamp(r.y0, 0, bound_fb->height);
                    r.y1 = std::clamp(r.y1, 0, bound_fb->height);
                    if (r.x1 > r.x0 && r.y1 > r.y0) {
                        bound_fb->exact_rects.push_back(r);
                    }
                }
                for (int i = 0; i < count; ++i) {
                    const int wy = static_cast<int>((cmd.pos[i * 2 + 1] + 1.0f) * 0.5f *
                                                    static_cast<float>(target_height));
                    // A vertex on the target's outermost row is a draw one pixel
                    // taller than the row count the target was sized for. The
                    // writeback copies every row the draw touched, so a draw that
                    // reaches past the guest's colour image writes into whatever
                    // follows it in RDRAM. That is how the 640x480 publisher
                    // stills corrupted the next heap block (session 142), and it
                    // is invisible without this line. Bounded: only targets
                    // taller than 300 rows, 24 lines.
                    if (target_height > 300 && (wy <= 0 || wy >= target_height - 1)) {
                        static int edge_logged = 0;
                        if (edge_logged < 24) {
                            ++edge_logged;
                            OGRE_MILESTONE("GFX-EDGE",
                                           "draw kind=%d row=%d target=%dx%d fb=0x%06X siz=%u "
                                           "vertices=%d first=(%.3f,%.3f)",
                                           cmd.kind, wy, bound_fb->width, target_height,
                                           cmd.fb_address, (unsigned)cmd.fb_siz, count,
                                           cmd.pos[0], cmd.pos[1]);
                        }
                    }
                    bound_fb->min_row = std::min(bound_fb->min_row, std::max(wy, 0));
                    bound_fb->max_row = std::max(bound_fb->max_row, std::min(wy, target_height - 1));
                }
                bound_fb->last_write_seq = ++framebuffer_write_seq_;
                // RT64 `FramebufferPair::addGameCall` counts the G_CD mode of
                // every call that contributes to the pair, and the pair's counts
                // are added to the colour framebuffer. This renderer has no
                // separate pair, so each draw's OTHERMODE_H bits 6-7 count
                // straight into the target it drew into.
                bound_fb->dither_patterns[(cmd.othermode_h >> 6) & 3u]++;
            }
        }

        // A WebGL error here means a draw was rejected (the session-181
        // feedback loop did this silently: the target's own texture was bound,
        // and every draw into that framebuffer raised INVALID_OPERATION). Report
        // it once per flush so a future binding mistake is visible.
        {
            const GLenum gl_err = glGetError();
            if (gl_err != GL_NO_ERROR && gl_errors_logged_ < 16) {
                ++gl_errors_logged_;
                OGRE_MILESTONE("GFX-GLERR",
                               "flush %u: draws=%zu first GL error 0x%04X",
                               flushed_cmds_, cmds.size(), static_cast<unsigned>(gl_err));
            }
        }

        // Mark every texture key the batch named, so the eviction below never
        // drops one a draw just used even if its upload was skipped.
        for (const DrawCmd& cmd : cmds) {
            if (!cmd.textured) {
                continue;
            }
            for (uint64_t key : {cmd.tex_key, cmd.tex_key1}) {
                auto it = gl_textures_.find(key);
                if (it != gl_textures_.end()) {
                    it->second.last_used = texture_epoch_;
                }
            }
        }

        // Age out tile images instead of keeping only this batch's. A screen
        // that alternates two texture sets uses each key every other frame, and
        // the old per-flush sweep deleted it in the gap, so it was re-uploaded
        // every time. An entry now survives kTextureKeepFlushes after the flush
        // that last named it, and the cache is additionally bounded by
        // kTextureCacheBytes. Eviction is least-recently-used, but an entry
        // named by the batch just drawn is never a victim: GL defers the
        // deletion until the draws issued here stop referencing it, and the
        // byte cap can therefore be exceeded for one batch. The decoded-tile
        // cache still holds the pixels, so a texture evicted here is re-created
        // from them rather than from a new decode.
        {
            for (auto it = gl_textures_.begin(); it != gl_textures_.end();) {
                if (texture_epoch_ - it->second.last_used > kTextureKeepFlushes) {
                    gl_texture_bytes_ -= it->second.bytes;
                    glDeleteTextures(1, &it->second.tex);
                    it = gl_textures_.erase(it);
                    ++tex_released_;
                } else {
                    ++it;
                }
            }
            // The cap can need many victims at once (an animated screen produces
            // one new image per tile per frame). Sort the candidates once rather
            // than rescanning the map for every victim.
            if (gl_texture_bytes_ > kTextureCacheBytes ||
                gl_textures_.size() > kTextureMaxEntries) {
                std::vector<std::pair<uint64_t, uint64_t>> lru;   // last_used, key
                lru.reserve(gl_textures_.size());
                for (const auto& [key, e] : gl_textures_) {
                    if (e.last_used != texture_epoch_) {
                        lru.emplace_back(e.last_used, key);
                    }
                }
                std::sort(lru.begin(), lru.end());
                for (const auto& entry : lru) {
                    if (gl_texture_bytes_ <= kTextureCacheBytes &&
                        gl_textures_.size() <= kTextureMaxEntries) {
                        break;
                    }
                    auto it = gl_textures_.find(entry.second);
                    if (it == gl_textures_.end()) {
                        continue;
                    }
                    gl_texture_bytes_ -= it->second.bytes;
                    glDeleteTextures(1, &it->second.tex);
                    gl_textures_.erase(it);
                    ++tex_released_;
                }
            }
        }
        // One cache census per minute of flushes, so a run's log answers
        // whether the caches plateau without a separate probe.
        if (flush_count_ > 0 && (flush_count_ % 60) == 0) {
            OGRE_MILESTONE("GFX-TEXCACHE",
                           "flush %u: live=%zu bytes=%zu/%zu uploads=%zu reused=%zu "
                           "released=%zu decodes=%zu hits=%zu mismatches=%u",
                           flush_count_, gl_textures_.size(), gl_texture_bytes_,
                           kTextureCacheBytes, tex_upload_count_, tex_reused_, tex_released_,
                           tex_decodes_.load(std::memory_order_relaxed),
                           tex_decode_hits_.load(std::memory_order_relaxed),
                           texcache_mismatches_.load(std::memory_order_relaxed));
        }
        // Writeback-dither control (`test_draw`, debug flag bit 10). Each marked
        // fill's target is read back here, with the pattern the batch counted,
        // and the two expected 16-bit words are counted. `0x214D` is the
        // no-dither result of the fill colour; `0x298F` is the same pixel after
        // a dither of 7 (every channel +1 in its 5-bit field). The MAGICSQ
        // target must show both, the DISABLE target only the first.
        for (const DrawCmd& cmd : cmds) {
            if (cmd.dither_probe == 0) {
                continue;
            }
            auto fit = gl_framebuffers_.find(cmd.fb_address);
            if (fit == gl_framebuffers_.end()) {
                OGRE_MILESTONE("GFX-DITHER-TEST",
                               "target=0x%06X pattern=%d: no framebuffer",
                               cmd.fb_address,
                               (int)((cmd.othermode_h >> 6) & 3u));
                continue;
            }
            GlFramebuffer& fb = fit->second;
            const uint32_t pattern = best_dither_pattern(fb.dither_patterns);
            int first_row = 0, last_row = 0;
            const bool wrote =
                readback_framebuffer(fb, cmd.fb_address, &first_row, &last_row);
            uint32_t no_dither = 0, with_dither = 0, other = 0;
            uint16_t sample[4] = {};
            if (wrote) {
                for (int gy = first_row; gy <= last_row; ++gy) {
                    const uint8_t* row =
                        rdram_ + cmd.fb_address + static_cast<uint32_t>(gy) * fb.row_bytes;
                    for (int x = 0; x < fb.width; ++x) {
                        const uint16_t v =
                            n64h(row, static_cast<int>(static_cast<uint32_t>(x) * 2u));
                        if (gy == first_row && x < 4) {
                            sample[x] = v;
                        }
                        if (v == 0x214D) {
                            ++no_dither;
                        } else if (v == 0x298F) {
                            ++with_dither;
                        } else {
                            ++other;
                        }
                    }
                }
            }
            OGRE_MILESTONE("GFX-DITHER-TEST",
                           "target=0x%06X probe=%d pattern=%d wrote=%d "
                           "no_dither=%u dithered=%u other=%u rows=[%d,%d] "
                           "sample=%04X %04X %04X %04X",
                           cmd.fb_address, cmd.dither_probe, (int)pattern, wrote ? 1 : 0,
                           no_dither, with_dither, other, first_row, last_row,
                           sample[0], sample[1], sample[2], sample[3]);
        }

        // A display-list load that reads a colour image the RDP has drawn is
        // waiting for a private copy of it (see fetch_fb_source). The batch's
        // draws have executed, so the copy shows their pixels.
        service_fb_source();

        // Presentation: show the VI's scanout buffer on the canvas.
        if (present) {
            present_framebuffer(present_address);
        }

        // Sampler census: which G_TF_* rules and masks the screens exercised.
        // Printed on the first batch that carries a textured draw and every 120
        // after it, so a run says whether any draw is bilinear at all.
        if (samp_draws_ > 0 && (samp_logged_at_ == 0 || samp_draws_ - samp_logged_at_ >= 400)) {
            samp_logged_at_ = samp_draws_;
            OGRE_MILESTONE("GFX-SAMPLER",
                           "textured draws=%u point=%u bilerp=%u average=%u masked=%u shifted=%u "
                           "persp=%u nonpersp=%u",
                           samp_draws_, samp_point_, samp_bilerp_, samp_average_, samp_masked_,
                           samp_shifted_, samp_persp_, samp_nonpersp_);
        }

        // Framebuffer writeback. The game's CPU reads a colour image out of
        // RDRAM after ogre_sync_framebuffers(), which waits on sync_done_, so
        // that request is the one point at which the guest is blocked on this
        // thread and the pixels it is about to read must be in RDRAM.
        //
        // The present path used to write every framebuffer back as well ("so
        // RDRAM tracks the last frame"). That is not what the reference
        // renderer does, and it wrote a colour image over live heap memory:
        // scene 0x12 renders a 320x240 image at guest 0x80228080
        // (0x228080..0x24D880), and the sprite part cache's block at
        // 0x80243E10 lies inside it, so the writeback put 16-bit pixels over
        // the block's allocator header at 0x80243DF0. The next cache release
        // (the scene-enter `func_801792DC`) read 0xFFFDFFFD as a tree link and
        // stored through it (`func_80071950`, memory access out of bounds).
        // RT64 writes a framebuffer back when the workload that drew it retires
        // and at the game's own readback request; it never writes every
        // framebuffer on every present (session 170).
        if (sync_pending) {
            readback_all_framebuffers();
            // The pixels a recorded draw put into a colour image are in RDRAM
            // again, so the gfx thread's framebuffer-as-texture ranges are stale.
            fb_writeback_epoch_.fetch_add(1, std::memory_order_release);
        }
        if (sync_pending) {
            sync_done_.store(sync_request, std::memory_order_release);
        }

        // Read the canvas back right after the batch, so "what the frame looks
        // like once every draw has landed" is measured rather than inferred
        // from a screenshot that may catch a partially flushed frame. The draws
        // now land in the colour image's framebuffer object, so bind the canvas
        // first: this reads what the VI presentation put there.
        static unsigned after_logged = 0;
        if (after_logged < 24 && had_rect) {
            static std::vector<uint8_t> px;
            px.resize(static_cast<size_t>(canvas_width_) * canvas_height_ * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, canvas_width_, canvas_height_);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, canvas_width_, canvas_height_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            size_t nz = 0; uint64_t sum = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                const unsigned v = px[i] + px[i + 1] + px[i + 2];
                if (v) { ++nz; sum += v; }
            }
            ++after_logged;
            OGRE_MILESTONE("GFX-AFTER",
                           "flush %u: canvas after batch nonBlack=%zu/%d meanRGB=%llu",
                           flush_count_, nz, canvas_width_ * canvas_height_,
                           (unsigned long long)(nz ? sum / nz : 0));
        }
    }

    void send_dl(const OSTask* task) override {
        ++task_count_;

        // Workload analysis (plan §15) continues to run in the browser even
        // with the WebGL renderer active, feeding ogre_gfx_stats(). It walks
        // the whole display list, so it runs OUTSIDE mutex_: holding the lock
        // across it would stall the browser main thread's ogre_gfx_flush()
        // for the entire analysis (blocking a browser main thread on a
        // pthread mutex is illegal and freezes the page).
        const auto analyze_t0 = std::chrono::steady_clock::now();
        if (rdram_ != nullptr) {
            gbi::analyze_dl(rdram_, task->t.data_ptr & 0x3FFFFFF, task->t.data_size, g_workload);
            refresh_stats_snapshot();
            if (task_count_ == 1 || (task_count_ % 50) == 0) {
                OGRE_MILESTONE("GFX-WORKLOAD", "%s", g_stats_snapshot);
            }
        }
        const auto analyze_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - analyze_t0).count();

        const auto exec_t0 = std::chrono::steady_clock::now();
        RenderState st;
        st.othermode_l = 0;
        st.othermode_h = 0;
        st.combiner.decode(0, 0);
        // The RDP keeps its colour image across display lists, so a task that
        // never sends G_SETCIMG renders into the target the previous task left
        // (RT64's RDP state is persistent the same way).
        st.cimg_address = last_cimg_address_;
        st.cimg_row_bytes = last_cimg_row_bytes_;
        st.cimg_width = last_cimg_width_;
        st.cimg_siz = last_cimg_siz_;
        st.cimg_fmt = last_cimg_fmt_;
        st.zimg_address = last_zimg_address_;
        st.zimg_width = last_zimg_width_;
        st.zimg_siz = last_zimg_siz_;
        st.zimg_fmt = last_zimg_fmt_;
        // Which GBI owns this display list. Hashing the ucode text is what
        // decides whether 0xDA/0xDC are G_MTX/G_MOVEMEM (F3DEX2) or the njpeg
        // macroblock draw and its matrix (S2DEX2).
        st.gbi = identify_gbi(rdram_, task);
        // The page sets the 16:9 canvas box while WIDESCREEN is on and the
        // dispatcher is on an expanding scene; the draw commands recorded below
        // carry the counter-scaled projection for that pass.
        widescreen_expand_ = ogre::web_widescreen_expand();

        // The DL executes on the gfx pthread and only records draw commands;
        // the browser main thread issues the GL in ogre_gfx_flush(). Crucially
        // this runs WITHOUT holding mutex_: a slow (or stalled) DL walk must
        // never block the browser main thread's flush, and locking mutex_ here
        // is also what used to self-deadlock via queue_triangles.
        ExecCtx ctx;
        last_task_data_ptr_ = task->t.data_ptr;
        execute_dl(rdram_, task->t.data_ptr & 0x3FFFFFF, st, ctx);

        const auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - exec_t0).count();

        // Publish the recorded commands under a short lock. A load that needed
        // a framebuffer already published the draws before it (see
        // tmem_load_command), so this is usually what remains after it.
        publish_exec(ctx);

        // Every 8th frame carries a gfx-thread timestamp: the inter-frame delta
        // shows directly whether the game is pacing itself or blocked.
        if (task_count_ <= 16 || (task_count_ % 8) == 0) {
            const unsigned tc = task_count_.load();
            OGRE_MILESTONE("RSP", "display list submitted (frame %u, t=%.0fms, type %u, ucode 0x%08X, draws=%u)",
                           tc, emscripten_get_now(), static_cast<unsigned>(task->t.type),
                           static_cast<unsigned>(task->t.ucode),
                           static_cast<unsigned>(ctx.draws.size()));
        }
        // Session-21: task-stream telemetry. A corrupt entry pointer is only
        // visible if the raw task fields are logged next to the walk results,
        // so every task in the suspicious window gets a line.
        const unsigned tc_now = task_count_.load();
        const bool task_logged = tc_now <= 8 || (tc_now >= 30 && tc_now <= 48) ||
                                 (tc_now % 50) == 0;
        if (task_logged) {
            char words[128];
            int o = 0;
            const uint32_t walked = task->t.data_ptr & 0x3FFFFFF;  // what execute_dl got
            for (int i = 0; i < 4 && o < 100; ++i) {
                if (walked + (i + 1) * 8 > gbi::kRdramSize) {
                    break;
                }
                const uint8_t* p = rdram_ + walked + i * 8;
                o += snprintf(words + o, sizeof(words) - o, "%08X %08X  ", rd32(p), rd32(p + 4));
            }
            OGRE_MILESTONE("GFX-TASK",
                           "task %u: type=%u ucode=0x%08X data_ptr=0x%08X (&0x3FFFFFF=0x%07X, "
                           "&0x1FFFFFFF=0x%07X) size=%u cmds=%u draws=%u words[@0x%07X]=%s",
                           tc_now, (unsigned)task->t.type, (unsigned)task->t.ucode,
                           (unsigned)task->t.data_ptr, (unsigned)walked,
                           (unsigned)(task->t.data_ptr & 0x1FFFFFFF),
                           (unsigned)task->t.data_size, last_dl_cmds_,
                           (unsigned)ctx.draws.size(), walked, words);
        }

        // Session-19 diagnostics: what the WebGL path did with this DL.
        if (task_count_ <= 8 || (task_count_ % 120) == 0) {
            OGRE_MILESTONE("GFX-DRAW",
                           "task %u: cmds=%u queued=%u rejected=%u ndc_x=[%.2f,%.2f] ndc_y=[%.2f,%.2f] gl_ready=%d skipped=%u flushed=%u/%u analyze=%lldms exec=%lldms",
                           task_count_.load(), last_dl_cmds_, draws_recorded_.load(), tris_rejected_,
                           ndc_min_[0], ndc_max_[0], ndc_min_[1], ndc_max_[1],
                           gl_ready_ ? 1 : 0, flush_skipped_.load(), flushed_cmds_, flush_count_,
                           (long long)analyze_ms, (long long)exec_ms);
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        // The runtime clears a scratch framebuffer with the RDP and points the
        // VI at it while the game is still booting (events.cpp: the VI is black
        // and the workload only exists so the presenter has something to show).
        // The pixels are all zero, so presenting that buffer from RDRAM is the
        // same as the RDP fill the native renderer issues.
        request_present(fb_address, 2);
    }

    void update_screen() override {
        // Present the buffer the VI is scanning out. `update_screen_regs` is
        // written by the gfx thread immediately before this call, so it is the
        // geometry of the frame that is on screen right now.
        const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        if (vi == nullptr) {
            return;
        }
        // Only a visible mode updates the size the render targets are built for,
        // which is what RT64's VIHistory does (`if (viDifferent && viVisible)
        // viHistory.pushVI(...)`). OB64 blanks the VI while it switches between
        // the 320x240 scene mode and the world map's 640x480 one, and a blanked
        // mode still carries the next mode's serrate bit with `hStart = 0`, so
        // fbSize alone reads that state as a 320x952 image.
        const ViSize size = vi_framebuffer_size(*vi);
        if (vi_visible(*vi) && size.width > 0 && size.height > 0) {
            const uint64_t packed = (static_cast<uint64_t>(size.width) << 32) |
                                    static_cast<uint32_t>(size.height);
            if (vi_scanout_.exchange(packed, std::memory_order_relaxed) != packed &&
                vi_size_keys_.size() < 8 && vi_size_keys_.insert(packed).second) {
                OGRE_MILESTONE("GFX-VI",
                               "VI scanout %dx%d origin=0x%06X status=0x%08X width=%u "
                               "hStart=0x%08X vStart=0x%08X xScale=0x%08X yScale=0x%08X",
                               size.width, size.height, vi->VI_ORIGIN_REG & 0x00FFFFFFu,
                               vi->VI_STATUS_REG, vi->VI_WIDTH_REG, vi->VI_H_START_REG,
                               vi->VI_V_START_REG, vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG);
            }
        }
        request_present(vi->VI_ORIGIN_REG, vi->VI_STATUS_REG);
    }

    // Asks the browser main thread to show the framebuffer at `guest_address`
    // on the canvas at the next flush. Called from the gfx thread.
    void request_present(uint32_t guest_address, unsigned vi_status) {
        std::lock_guard<std::mutex> lock(mutex_);
        present_address_ = guest_address & 0x00FFFFFFu;
        vi_status_ = vi_status;
        present_pending_ = true;
        ++present_requests_;
    }

    // Called from the game's own CPU thread (the recompiled njpeg readback in
    // bankE) before it copies a framebuffer out of RDRAM. The renderer holds
    // those pixels in a GL framebuffer object, so the browser main thread has to
    // render them and write them back first. Returns once that has happened, or
    // after kSyncTimeoutMs so a renderer that never flushes cannot hang the game
    // thread forever.
    void sync_framebuffers() {
        if (rdram_ == nullptr) {
            return;
        }
        const uint32_t request = sync_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Fast path: nothing has been drawn into a colour image that RDRAM does
        // not already hold, so there is nothing for the main thread to write
        // back. The game reaches this several times per background assembly, and
        // each wait costs up to one main-thread flush interval otherwise.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (draw_queue_.empty() && !present_pending_ && fb_writeback_pending_ == 0) {
                sync_done_.store(request, std::memory_order_release);
                return;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        while (sync_done_.load(std::memory_order_acquire) < request) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= kSyncTimeoutMs) {
                if (sync_timeouts_.fetch_add(1) == 0) {
                    OGRE_MILESTONE("GFX-FBSYNC",
                                   "sync_framebuffers: no main-thread flush within %d ms "
                                   "(flushes=%u); the CPU reads RDRAM as it is",
                                   kSyncTimeoutMs,
                                   g_flush_ok.load(std::memory_order_relaxed));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        // Diagnostic: what the CPU is about to read out of RDRAM. The game
        // picks one of the three framebuffers; if the copy comes out black,
        // this says whether RDRAM held the rendered pixels at that moment.
        if (sync_enter_logged_ < 8) {
            ++sync_enter_logged_;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            static const uint32_t kGameFbs[3] = {0x000400, 0x025C00, 0x04B400};
            char buf[256];
            int o = 0;
            for (uint32_t addr : kGameFbs) {
                size_t nz = 0;
                for (uint32_t gy = 0; gy < 240; ++gy) {
                    const uint8_t* row = rdram_ + addr + gy * 640;
                    for (uint32_t x = 0; x < 320; ++x) {
                        const uint16_t v = n64h(row, static_cast<int>(x * 2));
                        if ((v & 0x7FFF) != 0) { ++nz; }
                    }
                }
                o += snprintf(buf + o, sizeof(buf) - o, " 0x%06X=%zu", addr, nz);
            }
            OGRE_MILESTONE("GFX-FBSYNC", "sync %u: waited=%lldms ramNonBlack:%s",
                           request, (long long)ms, buf);
        }
    }

    // Debug: the last framebuffer-source copy, in the guest image's physical
    // byte layout. `ogre_gfx_debug_fb_source` hands it to a probe.
    bool debug_fb_source_copy(const uint8_t** data, int* len) {
        std::lock_guard<std::mutex> lock(fb_source_mutex_);
        if (!source_cache_valid_ || source_cache_bytes_.empty()) {
            return false;
        }
        *data = source_cache_bytes_.data();
        *len = static_cast<int>(source_cache_bytes_.size());
        return true;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_ready_) {
            if (program_ != 0) {
                glDeleteProgram(program_);
            }
            for (auto& [key, entry] : gl_textures_) {
                glDeleteTextures(1, &entry.tex);
            }
            gl_textures_.clear();
            gl_texture_bytes_ = 0;
            decoded_tiles_.clear();
            decoded_bytes_ = 0;
            for (auto& [addr, fb] : gl_framebuffers_) {
                if (fb.fbo != 0) glDeleteFramebuffers(1, &fb.fbo);
                if (fb.tex != 0) glDeleteTextures(1, &fb.tex);
            }
            gl_framebuffers_.clear();
            for (auto& [key, d] : depth_targets_) {
                if (d.rb != 0) glDeleteRenderbuffers(1, &d.rb);
            }
            depth_targets_.clear();
            depth_needs_clear_.clear();
            if (present_tex_ != 0) {
                glDeleteTextures(1, &present_tex_);
                present_tex_ = 0;
            }
        }
        OGRE_MILESTONE("RENDERER", "webgl2 renderer shut down (%u DLs)", task_count_.load());
    }

    uint32_t get_display_framerate() const override { return 60; }

    float get_resolution_scale() const override { return 1.0f; }

    void set_rdram(uint8_t* rdram) { rdram_ = rdram; }

    // ----- debug texture access (ogre_gfx_debug_tex) ---------------------------
    bool debug_last_texture_copy(int unit, std::vector<uint8_t>& out, int* w, int* h,
                                 int* tile, int* fmt, int* siz) {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        const DebugTex& t = debug_last_[unit & 1];
        if (t.width <= 0 || t.height <= 0) {
            return false;
        }
        out = t.pixels;
        if (w) *w = t.width;
        if (h) *h = t.height;
        if (tile) *tile = t.tile;
        if (fmt) *fmt = t.fmt;
        if (siz) *siz = t.siz;
        return true;
    }
    int debug_texture_count() {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        return static_cast<int>(debug_tex_.size());
    }
    bool debug_texture_copy(int index, std::vector<uint8_t>& out, int* w, int* h,
                            int* tile, int* fmt, int* siz) {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        if (index < 0 || index >= static_cast<int>(debug_tex_.size())) {
            return false;
        }
        const DebugTex& t = debug_tex_[index];
        out = t.pixels;
        if (w) *w = t.width;
        if (h) *h = t.height;
        if (tile) *tile = t.tile;
        if (fmt) *fmt = t.fmt;
        if (siz) *siz = t.siz;
        return true;
    }

  private:
    uint8_t* rdram_ = nullptr;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE canvas_handle_ = 0;
    int canvas_width_ = 320;
    int canvas_height_ = 240;
    bool gl_ready_ = false;
    // WIDESCREEN on an expanding scene. The renderer counter-scales the
    // projection's x row by 3/4 so the 4:3 framebuffer, stretched to 16:9 by the
    // page's canvas CSS, keeps the game's proportions and shows more world at
    // the sides. Set once per display list in send_dl (gfx thread), the same
    // thread transform_vertex runs on.
    bool widescreen_expand_ = false;
    // Colour images attach an RGBA16F texture when the context exposes
    // EXT_color_buffer_float (see ensure_gl), RGBA8 otherwise.
    bool hp_fb_ = false;
    // Texture sampler: 0 = GL fixed-function, 1 = shader bilinear, 2 = RT64's
    // three-point filter (the default; see ensure_gl and the fragment shader).
    int texf_ = 2;
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint white_tex_ = 0;   // 1x1 RGBA white, bound for unloaded texture units
    std::atomic<unsigned> task_count_{0};
    std::atomic<unsigned> flush_skipped_{0};
    // Diagnostics for the DL -> GL path (session 19): how many DrawCmds were
    // queued, how many triangles were dropped by the transform, and the NDC
    // extent of everything queued (nothing is visible outside [-1, 1]).
    std::atomic<unsigned> draws_recorded_{0};
    unsigned tris_rejected_ = 0;
    // Shading culling census (see note_shading_census): triangles the
    // screen-space test dropped, and how many lit/unlit triangles were
    // evaluated. Counters, not per-draw work.
    unsigned tris_culled_ = 0;
    unsigned lit_draws_ = 0, unlit_draws_ = 0;
    float ndc_min_[2] = {1e9f, 1e9f};
    float ndc_max_[2] = {-1e9f, -1e9f};
    bool logged_first_flush_ = false;
    unsigned tex_logged_ = 0;    unsigned tri_reject_logged_ = 0;
    unsigned tex_uv_logged_ = 0;
    unsigned draw_logged_ = 0;
    unsigned vert_logged_ = 0;
    unsigned txr_logged_ = 0;
    unsigned tex_state_logged_ = 0;
    unsigned tex_state_dump_logged_ = 0;
    int tex_state_scene_ = -1;
    unsigned frame_rect_logged_ = 0;
    // OGRE_FRAME_DUMP=1: for the mission intro's picture strips, write the
    // source bytes the load read and the TMEM it left, at the moment the draw
    // that samples them is recorded. A timed RDRAM dump cannot bracket this: the
    // heap buffer is reused by later screens, so a dump is either too early
    // (buffer not composed) or too late (buffer reused) (session 194).
    bool frame_dump_enabled_ = false;
    unsigned frame_dump_count_ = 0;
    // OGRE_TEXWINCLAMP: bound a wrapped sample to the tile's own rect. Off by
    // default; the switch is the A/B for the mission intro's picture strips.
    bool tex_win_clamp_ = false;
    unsigned mtx_logged_ = 0;
    unsigned draw_mtx_logged_ = 0;
    // How many of the run's first fill/texrect rectangles the GFX-RECT trace has
    // printed. Eight, so a screen's whole fill sequence is visible: the attract
    // story's letterbox fill is the *fourth* fill of its display list, and a cap
    // of three hid exactly the fill whose colour decode matters (session 149).
    unsigned rect_logged_ = 0;
    // How many repaired fog draws have stated their sampler state (see
    // GFX-FOG-DRAW). Bounded: the title's three groups repeat every list.
    unsigned fog_draw_logged_ = 0;
    // Sampler census: textured draws by G_TF_* filter and how many sample a
    // masked (wrapping/mirroring) tile. A run states which sampling rules the
    // screens actually use, so a claim about filtering is measured, not
    // assumed.
    unsigned samp_point_ = 0, samp_bilerp_ = 0, samp_average_ = 0, samp_masked_ = 0;
    // Draws whose tile asks for a G_TX shift (RT64 `ShiftScaleMap`, applied in
    // the sampler). A screen with a nonzero `shifted=` count is the screen that
    // exercises the multiply.
    unsigned samp_shifted_ = 0;
    // Textured triangle draws whose vertex w's differ (perspective mattered) and
    // draws whose OTHERMODE_H has G_TP_PERSP clear (the sampler's halving rule).
    unsigned samp_persp_ = 0, samp_nonpersp_ = 0;
    // Distinct (kind, OTHERMODE_H) families the G_TP census has printed.
    std::set<uint32_t> tp_seen_;
    // One-shot: a draw whose combiner reads a KEY_CENTER/KEY_SCALE input, and
    // the first vertex that took the per-vertex fog alpha arm.
    bool key_logged_ = false;
    bool fog_logged_ = false;
    // The textured-draw total the census last printed. The report is emitted on
    // the first textured batch and every 400 textured draws after that, so a
    // long screen (the tutorial's dialogue box arrives hundreds of draws into
    // the run) still reports itself; a flush-count cadence can miss it.
    unsigned samp_logged_at_ = 0;
    unsigned samp_draws_ = 0;
    // S2DEX2 opcode census. The njpeg backdrops are the only S2DEX2 lists this
    // game submits, and the opcode byte decides whether a command draws: the
    // renderer decodes 0x01/0x02/0xDA (object rectangles) and 0xDC (the object
    // matrix). 0x05/0x06/0x07 are S2DEX2's G_OBJ_LOADTXTR / G_OBJ_LDTX_SPRITE /
    // G_OBJ_LDTX_RECT (`tools/RT64/src/gbi/rt64_gbi_s2dex2.h`) and collide with
    // F3DEX2's TRI1/TRI2/QUAD, which the F3DEX2 switch below does draw from a
    // vertex buffer an S2DEX2 list never fills; 0x09/0x0A/0x0B (G_BG_1CYC /
    // G_BG_COPY / G_OBJ_RENDERMODE) have no arm at all. The counters are
    // cumulative from the first S2DEX2 command of the run.
    uint32_t s2d_op_counts_[256] = {};
    unsigned s2d_total_ = 0;
    bool s2d_first_printed_ = false;
    unsigned s2d_next_print_ = 0;   // cumulative total at which to print again
    // OGRE_TEX_TRACE: one line per G_TEXRECT with the image it samples and the
    // colour it decodes to. Bounded, like every other diagnostic here.
    bool trace_ = false;
    // OGRE_TEX_TRACE_AFTER_MS / _MS: the trace window (see the constructor).
    // A zero `trace_span_ms_` means "to the end of the run"; a zero
    // `trace_after_ms_` means "from the start".
    uint64_t trace_after_ms_ = 0;
    uint64_t trace_span_ms_ = 0;
    uint32_t trace_draws_ = 0;
    uint32_t trace_tluts_ = 0;
    // True while the trace window is open. The native renderer's clock is
    // `ultramodern::trace_millis()` (ram_key.cpp uses the same one); it starts
    // when the runtime does, not at the first scene.
    bool trace_active() const {
        if (!trace_) {
            return false;
        }
        const uint64_t now = ultramodern::trace_millis();
        if (trace_after_ms_ != 0 && now < trace_after_ms_) {
            return false;
        }
        if (trace_span_ms_ != 0 && now >= trace_after_ms_ + trace_span_ms_) {
            return false;
        }
        return true;
    }
    // OGRE_TLUT_TRACE: the palette-destination check. The decoder keys a CI
    // table by the tile's `palette` field and assumes the load wrote TMEM 0x100,
    // which is RT64's fixed palette base (`RDP_TMEM_PALETTE` 0x800 in bytes);
    // a load anywhere else is reported once per destination. See the note on
    // `tlut_` above and `docs/HANDOFF-2026-10-17-session160.md`.
    bool tlut_trace_ = false;
    std::set<uint64_t> tlut_dest_seen_;
    static constexpr uint32_t kTraceMaxDraws = 1500;
    unsigned wide_logged_ = 0;
    unsigned wide_task_ = 0;   // task whose wide draws the log above covers
    unsigned blit_logged_ = 0;
    unsigned s2d_logged_ = 0;
    unsigned framebuffers_logged_ = 0;
    unsigned framebuffer_readbacks_ = 0;
    unsigned flushed_cmds_ = 0;
    unsigned gl_errors_logged_ = 0;
    unsigned flush_count_ = 0;
    unsigned last_dl_cmds_ = 0;   // commands walked by the last execute_dl
    uint32_t last_task_data_ptr_ = 0;  // raw t.data_ptr of the last task
    unsigned runaway_logged_ = 0;
    std::mutex mutex_;

    // ----- framebuffer indirection (browser main thread) -----------------------
    //
    // The RDP renders into a guest colour image, not into the canvas; the game
    // then reads that buffer out of RDRAM with the CPU (the njpeg backdrop
    // assembly). Each colour image gets a GL framebuffer object keyed by its
    // guest address, created lazily on first use.
    std::map<uint32_t, GlFramebuffer> gl_framebuffers_;
    // Framebuffer-as-texture diagnostic. The TMEM load reads RDRAM on the
    // gfx thread, while the pixels a recorded draw put into a colour image are
    // still in a GL framebuffer until the main thread writes them back, so a
    // G_LOADTILE whose source range lies inside such an image copies stale
    // bytes. `fb_written_` holds the byte range of every colour image a recorded
    // draw has written since the last writeback; a decode that overlaps one is a
    // framebuffer-as-texture load the renderer does not implement ([GFX-FBTEX]).
    // The main thread publishes writebacks through `fb_writeback_epoch_`, and
    // the next check drops the ranges when it sees the epoch move. `fbtex_mutex_`
    // guards the vector (see the diagnostic section below).
    struct FbWrittenRange {
        uint32_t begin = 0;      // first byte of the colour image in RDRAM
        uint32_t end = 0;        // one past its last byte
        uint32_t address = 0;    // the image's guest address
        uint32_t pitch = 0;
        int height = 0;
        uint8_t siz = 0;
        // The guest rows the draws into this image actually covered. The
        // declared image is up to one row taller than what the game renders
        // (the target's height is the scissor's bottom row + 1, and the top
        // row is never drawn), and the heap immediately past the image holds
        // the game's own textures, so a range test alone reports those loads
        // as framebuffer-as-texture when RDRAM is in fact the right source.
        int row_min = 1 << 30;
        int row_max = -1;
        // The byte columns (0..pitch) those rows were drawn across. Rows alone
        // over-report a load whose rect sits in a drawn row but outside every
        // drawn column; `col_max` one past the last byte and `-1` means no
        // draw into this image reported a usable x extent.
        int col_min = 1 << 30;
        int col_max = -1;
    };
    std::vector<FbWrittenRange> fb_written_;
    mutable std::mutex fbtex_mutex_;   // guards fb_written_ and fbtex_logged_
    uint32_t fb_written_epoch_seen_ = 0;
    std::atomic<uint32_t> fb_writeback_epoch_{0};
    std::atomic<unsigned> fbtex_hits_{0};
    std::set<uint64_t> fbtex_logged_;
    // A TMEM load whose source is a colour image the RDP has drawn asks the
    // browser main thread for a private copy of that image's rendered pixels
    // (`fetch_fb_source`). The copy never touches RDRAM; the gfx thread reads it
    // through `LoadSource::shadow`. The request carries the draw count at the
    // time it was made, so the main thread services it only after executing the
    // draws that produced the pixels.
    struct FbSourceRequest {
        uint32_t image = 0;
        uint32_t draws = 0;
        std::vector<uint8_t> bytes;
        uint32_t rects = 0;         // exact rect draws the copy took from the GPU
        bool want_bytes = false;   // the gfx thread holds no copy of this image
        bool hit = false;          // the main thread's cached copy is still exact
        bool ok = false;
    };
    std::mutex fb_source_mutex_;
    FbSourceRequest fb_source_req_;
    std::atomic<uint32_t> fb_source_request_{0};
    std::atomic<uint32_t> fb_source_done_{0};
    uint32_t fb_source_seq_ = 0;              // gfx thread
    uint32_t fb_source_cached_image_ = 0;     // gfx thread: image of the copy
    std::vector<uint8_t> fb_source_bytes_;    // gfx thread: the accepted copy
    unsigned fb_source_copies_ = 0;           // gfx thread
    unsigned fb_source_reuses_ = 0;           // gfx thread
    // Main-thread cache of the last source copy.
    bool source_cache_valid_ = false;
    uint32_t source_cache_image_ = 0;
    uint64_t source_cache_hash_ = 0;
    uint64_t source_cache_rects_ = 0;
    std::vector<uint8_t> source_cache_bytes_;
    unsigned fb_source_builds_ = 0;
    unsigned fb_source_content_logged_ = 0;
    unsigned fb_source_errors_ = 0;
    // A texture load whose source is a colour image the RDP has drawn but not
    // written back yet: the load path publishes the draws and waits for the main
    // thread to write the image's drawn rectangles into RDRAM
    // (`sync_load_source`). The wait is bounded; a page that stops flushing
    // disables further waits until one happens.
    // The copy needs a browser-main-thread flush. A loaded host can take
    // hundreds of milliseconds to schedule one (the port's test runs have seen
    // 19), and a timeout leaves the load reading stale RDRAM, so the bound is
    // generous. A page that never flushes is handled by the backoff below.
    static constexpr int kLoadSyncTimeoutMs = 2000;
    std::atomic<unsigned> fbtex_syncs_{0};
    std::atomic<unsigned> fbtex_sync_skips_{0};
    unsigned fbtex_sync_timeouts_ = 0;
    uint32_t load_sync_blocked_at_flush_ = 0;   // gfx thread
    // The RDP's colour image, kept across display lists (RenderState::cimg_*).
    uint32_t last_cimg_address_ = 0;
    uint32_t last_cimg_row_bytes_ = 0;
    uint16_t last_cimg_width_ = 0;
    uint8_t last_cimg_siz_ = 0, last_cimg_fmt_ = 0;
    std::set<uint64_t> cimg_seen_;   // distinct G_SETCIMG configurations (gfx thread)
    // The RDP's depth image, kept across display lists the same way
    // (RenderState::zimg_*).
    uint32_t last_zimg_address_ = 0;
    uint16_t last_zimg_width_ = 0;
    uint8_t last_zimg_siz_ = 0, last_zimg_fmt_ = 0;
    std::set<uint64_t> zimg_seen_;   // distinct G_SETZIMG configurations (gfx thread)
    // Main thread: the one-off `[GFX-DEPTH] target` line.
    bool depth_target_logged_ = false;
    // Main thread: the depth renderbuffers, keyed by the depth image
    // (depth_target_key) rather than by the colour image. `depth_needs_clear_`
    // holds the renderbuffers that have not had their one-time clear yet.
    std::map<uint64_t, GlDepthTarget> depth_targets_;
    // Pairs of (colour FBO, depth renderbuffer) whose completeness was already
    // reported, so an incomplete pair is logged once.
    std::set<uint64_t> depth_pair_logged_;
    std::set<GLuint> depth_needs_clear_;
    // Guest addresses a display list has named as a depth image (`G_SETZIMG`).
    // A colour write into one of them overwrites the RDP's depth contents,
    // because the hardware has one memory per address.
    std::set<uint32_t> depth_image_addrs_;
    unsigned depth_target_seq_ = 0;
    unsigned depth_targets_logged_ = 0;
    // OGRE_DEPTH: the depth test/write switch (see record_state).
    bool depth_enabled_ = false;
    // OGRE_FBTEX=0 / OGRE_FBWRITE=0: the framebuffer-source copy and the RDRAM
    // writeback switches. Each isolates one half of the framebuffer path.
    bool fbtex_enabled_ = true;
    bool fbtex_publish_only_ = false;
    bool fbtex_rdram_only_ = false;
    bool fb_writeback_enabled_ = true;
    // Main thread: the one-shot `[GFX-DEPTH] first depth-tested draw` check.
    bool depth_verified_ = false;
    // Main thread: the synthetic depth control's readback (debug flag bit 7).
    bool depth_probe_pending_ = false;
    // Gfx thread: the `[GFX-DEPTH]` census counters. They live with the other
    // permanent diagnostics, are incremented per recorded draw, and print on
    // the first depth draw and every 400 after it.
    unsigned depth_draws_ = 0, depth_zcmp_ = 0, depth_zupd_ = 0, depth_prim_ = 0,
             depth_dec_ = 0, depth_dec_logged_ = 0;
    unsigned depth_logged_at_ = 0;
    // Gfx thread: the distinct (colour image, depth image) pairs, with the draw
    // number each first appeared on ([GFX-DEPTH] pair lines).
    std::map<std::pair<uint32_t, uint32_t>, unsigned> depth_pairs_;
    uint32_t depth_censused_fb_ = 0, depth_censused_z_ = 0xFFFFFFFFu;
    // The RDP's TMEM palette, which outlives a display list and a task exactly
    // the way the colour image does. RT64 holds one `RDP::TMEM` for the whole
    // run (`rt64_rdp.cpp` clears it in the constructor only, and `RDP::reset()`
    // does not touch it), so a G_LOADTLUT in one display list is still in TMEM
    // for a CI draw in a later one. The title screen proves the game relies on
    // it: its CI8 sprites render with the palette loaded several scenes earlier,
    // and the whole title scene issues no G_LOADTLUT at all.
    uint16_t tlut_[256]{};
    bool tlut_valid_ = false;
    // The RDP's TMEM: 4 KiB, persistent for the run the way RT64's
    // `RDP::TMEM` is (it is cleared in the constructor only, `RDP::reset()`
    // does not touch it). Every load command copies its source into it with
    // RT64's byte layout (`loadWord`), and a draw decodes its tile out of it at
    // `tmem << 3` with `line << 3` as the row stride. The previous model
    // decoded straight from RDRAM with the `SETTIMG` width as the row stride,
    // which is only the same bytes when the load wrote the rows there
    // contiguously; a G_LOADBLOCK followed by a tile whose `line` differs from
    // that width decoded the wrong rows. The gfx thread is the only reader and
    // writer.
    uint8_t tmem_[4096]{};
    uint64_t tmem_generation_ = 0;   // bumped by every write (image cache key)
    // How many of the 256 entries are non-zero. A LOADTLUT that runs before the
    // game's palette DMA lands fills the table with zeros without failing, and
    // that is otherwise invisible in the log.
    int tlut_nonzero_ = 0;
    uint16_t tlut_count_ = 0;
    uint64_t tlut_generation_ = 0;   // bumped by every G_LOADTLUT (image cache key)
    std::set<uint64_t> vi_size_keys_;   // distinct VI scanout sizes (gfx thread)
    // The last visible VI scanout size, w << 32 | h (see vi_size()).
    std::atomic<uint64_t> vi_scanout_{0};
    // Presentation: the VI's scanout buffer, requested by update_screen() on the
    // gfx thread and consumed by flush_commands() on the main thread.
    uint32_t present_address_ = 0;
    bool present_pending_ = false;
    unsigned vi_status_ = 0;
    unsigned present_requests_ = 0;
    bool canvas_dirty_ = false;   // a draw went straight to the canvas
    GLuint present_tex_ = 0;      // canvas-sized, for a scanout buffer with no FBO
    unsigned presents_logged_ = 0;
    unsigned sync_logged_ = 0;
    uint32_t framebuffer_write_seq_ = 0;
    // RT64 `State::ditherRandomSeed`: advances once per framebuffer writeback.
    // Only `G_CD_NOISE` consumes it, but the sequence is kept identical so a
    // screen that uses the pattern matches RT64's writeback.
    uint32_t dither_random_seed_ = 0;
    unsigned sync_enter_logged_ = 0;
    // Framebuffer sync handshake. The game thread bumps sync_request_ and waits
    // for sync_done_ to catch up; the main thread performs the writeback.
    std::atomic<uint32_t> sync_request_{0};
    std::atomic<uint32_t> sync_done_{0};
    std::atomic<unsigned> sync_timeouts_{0};
    // Draws that landed in a colour image whose pixels are not in RDRAM yet.
    // Guarded by mutex_: send_dl() sets it when it publishes draws, the main
    // thread clears it after a writeback, and sync_framebuffers() reads it to
    // decide whether it has to wait at all.
    uint32_t fb_writeback_pending_ = 0;
    static constexpr int kSyncTimeoutMs = 1000;
    // The largest render target this renderer will allocate a side for. The N64
    // tops out at 640x480; the cap keeps a corrupt G_SETCIMG from asking for a
    // gigabyte texture.
    static constexpr int kMaxFramebufferSide = 1024;
    // The tallest CSS box `#game-canvas` is allowed to become. The element's
    // displayed size is an integer multiple of the drawing buffer (see
    // resize_canvas): the 320x240 VI mode keeps the 2x box index.html declares,
    // and a 640-wide mode is shown 1:1.
    static constexpr int kCanvasCssHeight = 480;

    // Command queues: written by the gfx pthread (execute_dl), drained by the
    // browser main thread (ogre_gfx_flush -> flush_commands).
    std::vector<DrawCmd> draw_queue_;
    std::vector<TexUpload> tex_upload_queue_;
    size_t tex_upload_count_ = 0;   // actual glTexImage2D calls
    size_t tex_reused_ = 0;         // tile images a resident texture already held
    size_t tex_released_ = 0;   // textures dropped by the age/byte eviction
    unsigned tex_missing_pixels_ = 0;   // uploads dropped for want of pixels
    uint32_t test_draw_variant_ = 0;    // test_draw source-change control
    // GL-side texture cache (main thread only): key = tile content key. Entries
    // live for at least kTextureKeepFlushes and until kTextureCacheBytes is
    // exceeded, so a texture used on alternating frames is not re-uploaded.
    std::map<uint64_t, GlTextureEntry> gl_textures_;
    size_t gl_texture_bytes_ = 0;
    uint64_t texture_epoch_ = 0;   // bumped once per flush, for the age test
    static constexpr uint64_t kTextureKeepFlushes = 8;
    static constexpr size_t kTextureCacheBytes = 32u << 20;
    static constexpr size_t kTextureMaxEntries = 16384;

    // Gfx-thread decoded-tile cache. Keyed by the TMEM bytes and the tile fields
    // that decide the decode, so an unchanged source is not decoded again when
    // a later display list reloads it. Entries are bounded by bytes, not by a
    // flush count: the upload queue holds a shared reference until it drains.
    std::map<uint64_t, DecodedTile> decoded_tiles_;
    size_t decoded_bytes_ = 0;
    uint64_t decode_epoch_ = 0;
    static constexpr size_t kDecodeCacheBytes = 16u << 20;
    std::atomic<size_t> tex_decodes_{0};
    std::atomic<size_t> tex_decode_hits_{0};
    std::atomic<unsigned> texcache_mismatches_{0};   // trace-only source-key audit

    // Debug: the last few decoded images, so a probe can look at exactly what
    // the renderer sampled (see ogre_gfx_debug_tex). Written on the gfx thread
    // in load_tile_texture(), copied out on the browser main thread.
    struct DebugTex {
        uint64_t key = 0;
        int width = 0, height = 0, tile = 0;
        int fmt = 0, siz = 0, line = 0;
        std::vector<uint8_t> pixels;
    };
    std::mutex debug_mutex_;
    std::vector<DebugTex> debug_tex_;
    // The two images (TEXEL0, TEXEL1) of the most recent textured draw. The
    // ring above is a window over decodes and spans several frames once the
    // upload de-duplication kicks in, so "the last pair" in the ring is not
    // necessarily the pair a rendered sprite was drawn with.
    DebugTex debug_last_[2];

    // ----- GL init (browser main thread only) ----------------------------------

    // ensure_gl() must run on the browser main thread (the WebGL2 context is
    // owned by it; no GL proxying is used).
    bool ensure_gl() {
        if (gl_ready_) {
            return true;
        }
        if (canvas_handle_ == 0) {
            return false;  // web.js has not handed the canvas over yet
        }
        if (!emscripten_is_main_runtime_thread()) {
            fprintf(stderr, "[web-renderer] ensure_gl called off the main thread; ignoring\n");
            return false;
        }
        EMSCRIPTEN_RESULT rc = emscripten_webgl_make_context_current(canvas_handle_);
        if (rc != EMSCRIPTEN_RESULT_SUCCESS) {
            fprintf(stderr, "[web-renderer] emscripten_webgl_make_context_current(handle=%d) failed (%d)\n",
                    (int)canvas_handle_, (int)rc);
            return false;
        }

        // RT64's high-precision colour buffer (`internalColorFormat`, which
        // defaults to Automatic and resolves through the device's `preferHDR`).
        // WebGL2 has no 16-bit UNORM colour attachment without
        // `EXT_texture_norm16`; RGBA16F is the closest renderable format, and
        // its precision over 8 bits is what keeps a stack of translucent draws
        // from quantizing at every blend. `OGRE_HPFB=0` forces the RGBA8
        // attachment for an A/B (`?OGRE_HPFB=0` on the page).
        const char* hpfb_env = getenv("OGRE_HPFB");
        hp_fb_ = (hpfb_env == nullptr || hpfb_env[0] != '0') &&
                 emscripten_webgl_enable_extension(canvas_handle_, "EXT_color_buffer_float");
        OGRE_MILESTONE("RENDERER", "colour attachment: %s%s",
                       hp_fb_ ? "RGBA16F" : "RGBA8",
                       hp_fb_ ? " (EXT_color_buffer_float)"
                              : " (EXT_color_buffer_float unavailable or OGRE_HPFB=0)");

        // Texture sampling. The RDP's own rules are RT64's sampler port (see
        // the fragment shader): manual bilinear on integer texels, the
        // tile-window clamp, the mask wrap/mirror, and RT64's default
        // three-point filter instead of true bilinear. `OGRE_TEXF=gl` restores
        // the GL fixed-function sampler, and `OGRE_TEXF=1` (or `bilinear`) runs
        // the same shader sampler with true bilinear, so the filtering rule
        // can be A/B'd without touching anything else.
        {
            const char* texf_env = getenv("OGRE_TEXF");
            if (texf_env == nullptr || texf_env[0] == '\0' || strcmp(texf_env, "rt64") == 0) {
                texf_ = 2;
            } else if (strcmp(texf_env, "gl") == 0) {
                texf_ = 0;
            } else if (strcmp(texf_env, "1") == 0 || strcmp(texf_env, "bilinear") == 0) {
                texf_ = 1;
            } else {
                fprintf(stderr, "[web-renderer] unknown OGRE_TEXF=%s; using rt64\n", texf_env);
                texf_ = 2;
            }
            static const char* const names[] = {"GL fixed-function", "shader bilinear",
                                                "RT64 3-point"};
            OGRE_MILESTONE("RENDERER", "texture sampler: %s", names[texf_]);
        }

        GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        GLint ok = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
            fprintf(stderr, "[web-renderer] program link failed: %s\n", log);
            return false;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        // Stride 10: xy, uv, rgba, w, NDC z (the vertex shader's locations
        // 0..4; see DrawCmd::z and transform_vertex).
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(4 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(9 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(8 * sizeof(float)));

        // 1x1 white fallback: a tile that was never loaded must sample white,
        // because TEXEL0/TEXEL1 are often only one of the two in use.
        {
            const uint8_t white[4] = {255, 255, 255, 255};
            glGenTextures(1, &white_tex_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, white_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        // A uniform whose location comes back as -1 silently keeps its default
        // (0), which for a UV scale collapses every sample to texel (0,0). Log
        // the locations once so that failure mode is visible rather than
        // inferred.
        {
            static const char* const names[] = {
                "u_tex0", "u_tex1", "u_tsize0", "u_torig0",
                "u_tcm0", "u_tmask0", "u_tex_filter", "u_texf", "u_tex_persp",
                "u_tsize1", "u_torig1", "u_tcm1", "u_tmask1",
                "u_prim", "u_env", "u_cycle",
            };
            char buf[256];
            int o = 0;
            for (const char* n : names) {
                o += snprintf(buf + o, sizeof(buf) - o, "%s=%d ",
                              n, glGetUniformLocation(program_, n));
                if (o >= static_cast<int>(sizeof(buf)) - 24) break;
            }
            OGRE_MILESTONE("RENDERER", "uniform locations: %s", buf);
        }

        glViewport(0, 0, canvas_width_, canvas_height_);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        gl_ready_ = true;
        OGRE_MILESTONE("RENDERER", "webgl2 renderer initialized (%dx%d)", canvas_width_, canvas_height_);
        return true;
    }

    // ----- DL execution ------------------------------------------------------

    // Per-DL execution context (segments + pending texrect halves).
    struct ExecCtx {
        WebGLRenderer* self = nullptr;
        RenderState* st = nullptr;
        uint32_t segments[16] = {};
        uint32_t rdp_half1 = 0, rdp_half2 = 0;
        bool pending_texrect = false;
        bool pending_texrect_flip = false;
        // Session-19: progress trace. Every kTraceEvery commands the executor
        // logs the command it is about to run, so a DL that never finishes
        // still leaves the offending command in the status log.
        uint32_t cmd_index = 0;
        uint32_t trace_task = 0;
        bool trace_enabled = false;
        uint32_t vp_logged = 0;   // G_MOVEMEM viewport diagnostic counter
        // Session-19: outputs of this execution. The DL is walked WITHOUT
        // holding the renderer mutex (so a slow/stalled gfx thread can never
        // starve the browser main thread's flush); the results are published to
        // the shared queues under a short lock afterwards.
        std::vector<DrawCmd> draws;
        std::vector<TexUpload> tex;
        // Texture keys already queued for upload by this display list (a
        // sprite's two triangles sample the same image).
        std::set<uint64_t> uploaded;
        // Session-23: the order the draws were recorded in, one character each
        // (F = fill rect, T = texrect, t = textured triangle, u = untextured
        // triangle). The title scene dimmed because the full-screen rect that
        // should be its background was drawn over the sprites.
        std::string order;
        // The NDC extent of the first triangle of the frame: whether the intro
        // animates its geometry (a widening ring) or only its colours (a fade)
        // is what decides if a "thin sprite" is our bug or the game's.
        bool first_quad_done = false;
        float quad_w = 0, quad_h = 0;
        bool uv_logged = false;
        float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
        int iw0 = 0, ih0 = 0, iw1 = 0, ih1 = 0;
        // Session-21: a runaway DL walk (budget exhausted far from any real
        // display list) is diagnosed from the first commands walked and the
        // last ones before the budget ran out. The head shows whether the
        // entry pointer itself was bad; the tail shows the bogus branch.
        static constexpr uint32_t kHeadMax = 8, kTailMax = 24;
        gbi::DlCommand head[kHeadMax];
        gbi::DlCommand tail[kTailMax];
        uint32_t tail_count = 0;
        // Session-21: the first command whose offset escapes the low rdram
        // window (0x02000000) is where a bogus G_DL landed; the ring above then
        // holds the commands immediately before the jump.
        bool escape_logged = false;
        // Session-23: the tile texture-setup commands seen so far. A draw's
        // texture is whatever SETTIMG/SETTILE/SETTILESIZE/LOAD* put in TMEM, so
        // printing the whole recipe next to the draw's tile state is what shows
        // whether the *load* rect or the *tile* rect is the right UV basis.
        static constexpr uint32_t kLoadRing = 20;
        gbi::DlCommand load_ring[kLoadRing];
        uint32_t load_ring_count = 0;
        // OGRE_FOG repairs applied to this list (fog_repair_display_list's
        // count), so a draw that samples a repaired masked tile can state its
        // sampler state once per list.
        uint32_t fog_repaired = 0;
    };

    // Publishes the draws and texture uploads an in-progress display list has
    // recorded so far. send_dl() publishes the rest when the walk ends. A
    // framebuffer-source texture load publishes early: the pixels it has to read
    // are produced by draws that are still in this context (session 193).
    void publish_exec(ExecCtx& ctx) {
        if (ctx.draws.empty() && ctx.tex.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (DrawCmd& d : ctx.draws) {
            if (draw_queue_.size() >= 16384) {
                break;
            }
            if (d.fb_address != 0) {
                fb_writeback_pending_ = 1;
            }
            draw_queue_.push_back(std::move(d));
            ++draws_recorded_;
        }
        for (TexUpload& t : ctx.tex) {
            tex_upload_queue_.push_back(std::move(t));
        }
        ctx.draws.clear();
        ctx.tex.clear();
        g_exec_draws.store(draws_recorded_, std::memory_order_relaxed);
    }

    // Makes the pixels a colour image holds on the GPU readable from RDRAM, for
    // a display-list load whose source range is that image. RT64 makes a GPU
    // tile copy here (`RDP::checkFramebufferOverlap`); this renderer keeps one
    // source of truth, so it writes the image's drawn rectangles back instead
    // and the load copies them normally. Runs on the gfx thread: it publishes
    // the draws recorded so far, then waits for the browser main thread's flush
    // to execute them and write the rectangles back.
    //
    // Returns false when the wait timed out, in which case the load reads RDRAM
    // as it is (the pre-session-193 behaviour) and `[GFX-FBTEX]` reports it.
    // Asks the browser main thread for a private copy of one colour image's
    // rendered pixels, in the guest byte layout, and waits for it. A TMEM load
    // whose source range is that image then reads the copy instead of RDRAM
    // (`LoadSource::shadow`). RT64 makes a GPU tile copy for the same reason; it
    // must not write RDRAM, because the game keeps live heap data inside a
    // colour image's byte range (the scene-0x12 sprite cache at 0x243E10 lies
    // inside image 0x228080) and a writeback there corrupts it.
    //
    // Runs on the gfx thread. The draws recorded so far are published first, so
    // the main thread executes the ones that produced these pixels before it
    // reads them. Returns false when the page did not flush in time, or when the
    // image's RDRAM changed since its GPU copy was made (the copy is stale).
    bool fetch_fb_source(ExecCtx* ctx, uint32_t image) {
        if (image == 0 || emscripten_is_main_runtime_thread()) {
            return false;
        }
        // A page that is not flushing must not stall the display list once per
        // load. After one timeout, skip the copy until a flush has happened.
        const uint32_t flushes = g_flush_ok.load(std::memory_order_relaxed);
        if (load_sync_blocked_at_flush_ != 0 && flushes <= load_sync_blocked_at_flush_) {
            if (fbtex_sync_skips_++ == 0) {
                OGRE_MILESTONE("GFX-FBTEX-SYNC",
                               "no flush since the last timeout; reading RDRAM");
            }
            return false;
        }
        if (ctx != nullptr) {
            publish_exec(*ctx);
        }
        if (fbtex_publish_only_) {
            return false;
        }
        // A flush is the only thing that can service the request, and a loaded
        // host can be late to give the browser main thread one. A second
        // request after a timeout usually hits the cache the first one built, so
        // the load does not have to read stale RDRAM.
        uint32_t seq = 0;
        long long ms = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int attempt = 0; attempt < 2; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(fb_source_mutex_);
                fb_source_req_.image = image;
                fb_source_req_.draws = draws_recorded_.load(std::memory_order_relaxed);
                fb_source_req_.want_bytes = (fb_source_cached_image_ != image);
                fb_source_req_.hit = false;
                fb_source_req_.bytes.clear();
                fb_source_req_.ok = false;
                seq = ++fb_source_seq_;
                fb_source_request_.store(seq, std::memory_order_release);
            }
            const auto attempt_start = std::chrono::steady_clock::now();
            while (fb_source_done_.load(std::memory_order_acquire) < seq) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - attempt_start).count();
                if (elapsed >= kLoadSyncTimeoutMs) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start).count();
            if (fb_source_done_.load(std::memory_order_acquire) >= seq) {
                break;
            }
            if (attempt == 1) {
                load_sync_blocked_at_flush_ = flushes;
                if (++fbtex_sync_timeouts_ <= 4) {
                    OGRE_MILESTONE("GFX-FBTEX-SYNC",
                                   "source 0x%06X: no flush serviced the copy within %lldms "
                                   "(flushes=%u executed=%u wanted=%u)",
                                   image, ms, flushes, flushed_cmds_,
                                   draws_recorded_.load(std::memory_order_relaxed));
                }
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(fb_source_mutex_);
        if (!fb_source_req_.ok) {
            return false;
        }
        if (!fb_source_req_.bytes.empty()) {
            fb_source_bytes_ = std::move(fb_source_req_.bytes);
            fb_source_cached_image_ = image;
            fb_source_req_.bytes.clear();
            ++fb_source_copies_;
        } else if (fb_source_cached_image_ == image) {
            // Unchanged image and the gfx thread still holds the copy.
            ++fb_source_reuses_;
        } else {
            return false;
        }
        const unsigned copies = ++fbtex_syncs_;
        if (copies <= 12 || (copies % 300) == 0) {
            OGRE_MILESTONE("GFX-FBTEX-SYNC",
                           "source 0x%06X: copy ready (%zu bytes, %u rect draws, %s) in "
                           "%lldms (copy %u)",
                           image, fb_source_bytes_.size(), fb_source_req_.rects,
                           fb_source_req_.hit ? "cached" : "new", (long long)ms, copies);
        }
        return true;
    }

    // Builds the framebuffer-as-texture source for one colour image: RDRAM's
    // bytes with the image's rect-drawn pixels replaced by the GPU's. A texture
    // load that reads this image sees exactly what the RDP has written, without
    // any writeback to RDRAM. Triangle-drawn areas keep RDRAM's bytes, because
    // their recorded rectangle is a bounding box rather than exact coverage.
    bool build_image_source(uint32_t image, std::vector<uint8_t>& out) {
        if (rdram_ == nullptr) {
            return false;
        }
        auto it = gl_framebuffers_.find(image);
        if (it == gl_framebuffers_.end() || it->second.fbo == 0) {
            return false;
        }
        GlFramebuffer& fb = it->second;
        const int w = fb.width, h = fb.height;
        const uint64_t bytes = static_cast<uint64_t>(fb.row_bytes) * h;
        if (w <= 0 || h <= 0 || fb.row_bytes == 0 ||
            static_cast<uint64_t>(image) + bytes > gbi::kRdramSize) {
            return false;
        }
        out.assign(static_cast<size_t>(bytes), 0);
        std::memcpy(out.data(), rdram_ + image, static_cast<size_t>(bytes));
        if (fb.exact_rects.empty() || fbtex_rdram_only_) {
            return true;   // nothing exact to substitute (or the RDRAM control)
        }
        static std::vector<uint8_t> px;
        static std::vector<float> fpx;
        px.resize(static_cast<size_t>(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (fb.fp) {
            fpx.resize(px.size());
            glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, fpx.data());
            for (size_t i = 0; i < fpx.size(); i += 4) {
                for (int c = 0; c < 4; ++c) {
                    px[i + c] = static_cast<uint8_t>(std::lround(
                        std::clamp(fpx[i + c], 0.0f, 1.0f) * 255.0f));
                }
            }
        } else {
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        const uint8_t bpt = static_cast<uint8_t>(bytes_per_texel(fb.siz));
        if (fb_source_content_logged_ < 8) {
            ++fb_source_content_logged_;
            for (const GlFramebuffer::FbRect& r : fb.exact_rects) {
                OGRE_MILESTONE("GFX-FBCONTENT",
                               "image 0x%06X %dx%d: exact rect x=[%d,%d) y=[%d,%d) = GPU "
                               "pixels",
                               image, w, h, r.x0, r.x1, r.y0, r.y1);
            }
        }
        for (const GlFramebuffer::FbRect& r : fb.exact_rects) {
            for (int wy = r.y0; wy < r.y1; ++wy) {
                if (wy < 0 || wy >= h) {
                    continue;
                }
                // glReadPixels returns window row 0 first; it is the image's
                // bottom row.
                const int gy = h - 1 - wy;
                uint8_t* dst = out.data() + static_cast<uint32_t>(gy) * fb.row_bytes;
                const uint8_t* src = px.data() + (static_cast<size_t>(wy) * w + r.x0) * 4;
                const float* fsrc = fb.fp
                                        ? fpx.data() + (static_cast<size_t>(wy) * w + r.x0) * 4
                                        : nullptr;
                for (int x = r.x0; x < r.x1; ++x, src += 4) {
                    const uint32_t off = static_cast<uint32_t>(x) * bpt;
                    if (fb.siz == gbi::IM_SIZ_32b) {
                        dst[(off + 0) ^ 3] = src[0];
                        dst[(off + 1) ^ 3] = src[1];
                        dst[(off + 2) ^ 3] = src[2];
                        dst[(off + 3) ^ 3] = src[3];
                    } else {
                        uint16_t v;
                        if (fsrc != nullptr) {
                            v = float4_to_rgba16(fsrc[0], fsrc[1], fsrc[2], fsrc[3], 0);
                            fsrc += 4;
                        } else {
                            auto c5 = [](uint8_t c) {
                                return static_cast<uint32_t>(c) >> 3;
                            };
                            v = static_cast<uint16_t>((c5(src[0]) << 11) | (c5(src[1]) << 6) |
                                                      (c5(src[2]) << 1) |
                                                      (((src[3] % 8) & 4) ? 1 : 0));
                        }
                        dst[off ^ 2] = static_cast<uint8_t>(v & 0xFF);
                        dst[(off ^ 2) + 1] = static_cast<uint8_t>(v >> 8);
                    }
                }
            }
        }
        return true;
    }

    // Browser main thread: services a pending `fetch_fb_source` request. Runs
    // after the batch's draws have been executed. A request whose draws have not
    // executed yet is left for the next flush. The copy is cached per image, and
    // reused while the image's exact rects and RDRAM bytes are unchanged.
    void service_fb_source() {
        const uint32_t request = fb_source_request_.load(std::memory_order_acquire);
        if (request == 0 || request <= fb_source_done_.load(std::memory_order_relaxed)) {
            return;
        }
        std::lock_guard<std::mutex> lock(fb_source_mutex_);
        if (request <= fb_source_done_.load(std::memory_order_relaxed)) {
            return;
        }
        FbSourceRequest& r = fb_source_req_;
        if (flushed_cmds_ < r.draws) {
            return;   // the draws that produced these pixels are not executed yet
        }
        auto it = gl_framebuffers_.find(r.image);
        if (it != gl_framebuffers_.end() && it->second.fbo != 0) {
            GlFramebuffer& fb = it->second;
            const uint64_t bytes = static_cast<uint64_t>(fb.row_bytes) * fb.height;
            const uint64_t hash = (rdram_ != nullptr)
                                      ? XXH3_64bits(rdram_ + r.image, static_cast<size_t>(bytes))
                                      : 0;
            const uint64_t rects = fb.exact_rects.size();
            if (source_cache_valid_ && source_cache_image_ == r.image &&
                source_cache_hash_ == hash && source_cache_rects_ == rects) {
                r.hit = true;
                r.ok = true;
                r.rects = static_cast<uint32_t>(rects);
                if (r.want_bytes) {
                    r.bytes = source_cache_bytes_;
                }
            } else if (build_image_source(r.image, r.bytes)) {
                source_cache_image_ = r.image;
                source_cache_hash_ = hash;
                source_cache_rects_ = rects;
                source_cache_valid_ = true;
                source_cache_bytes_ = r.bytes;
                r.hit = false;
                r.ok = true;
                r.rects = static_cast<uint32_t>(rects);
                ++fb_source_builds_;
            } else if (fb_source_errors_ < 8) {
                ++fb_source_errors_;
                OGRE_MILESTONE("GFX-FBTEX-SYNC",
                               "source 0x%06X: no framebuffer copy (%zu bytes, rects=%llu)",
                               r.image, static_cast<size_t>(bytes),
                               (unsigned long long)rects);
            }
        }
        fb_source_done_.store(request, std::memory_order_release);
    }
    // [GFX-S2D-CENSUS]: the opcode bytes an S2DEX2 list actually carries. Printed
    // at the end of the first S2DEX2 task and every 200 S2DEX2 commands after it.
    // `undecoded=` lists the opcodes that were seen and that the S2DEX2 arm of
    // exec_command does not decode (so they either fall through to the F3DEX2
    // switch or do nothing).
    void print_s2d_census(unsigned task) {
        static constexpr uint8_t kNamed[] = {0x01, 0x02, 0x04, 0x05, 0x06, 0x07,
                                             0x08, 0x09, 0x0A, 0x0B, 0xDA, 0xDC, 0xE4};
        char counts[160];
        int o = 0;
        for (uint8_t op : kNamed) {
            o += snprintf(counts + o, sizeof(counts) - static_cast<size_t>(o), " %02X=%u", op,
                          s2d_op_counts_[op]);
        }
        // Any other opcode that appeared, capped so the 384-byte milestone
        // message cannot truncate the undecoded list.
        char other[80];
        int oo = 0;
        for (int op = 0; op < 256; ++op) {
            if (s2d_op_counts_[op] == 0) {
                continue;
            }
            bool named = false;
            for (uint8_t n : kNamed) {
                if (n == static_cast<uint8_t>(op)) {
                    named = true;
                    break;
                }
            }
            if (named) {
                continue;
            }
            if (oo + 12 >= static_cast<int>(sizeof(other))) {
                oo += snprintf(other + oo, sizeof(other) - static_cast<size_t>(oo), " ...");
                break;
            }
            oo += snprintf(other + oo, sizeof(other) - static_cast<size_t>(oo), " %02X=%u", op,
                           s2d_op_counts_[op]);
        }
        char undecoded[80];
        int u = 0;
        for (int op = 0; op < 256; ++op) {
            if (s2d_op_counts_[op] == 0) {
                continue;
            }
            const bool decoded = (op == S2D_OBJ_RECTANGLE || op == S2D_OBJ_SPRITE ||
                                  op == S2D_OBJ_RECTANGLE_R || op == S2D_OBJ_MOVEMEM);
            if (decoded) {
                continue;
            }
            if (u + 8 >= static_cast<int>(sizeof(undecoded))) {
                u += snprintf(undecoded + u, sizeof(undecoded) - static_cast<size_t>(u), " ..");
                break;
            }
            u += snprintf(undecoded + u, sizeof(undecoded) - static_cast<size_t>(u), "%s%02X",
                          u ? "," : "", op);
        }
        OGRE_MILESTONE("GFX-S2D-CENSUS",
                       "task %u: cmds=%u%s others=[%s] undecoded=[%s]",
                       task, s2d_total_, counts, other[0] ? other + 1 : "",
                       undecoded[0] ? undecoded : "none");
    }

    void execute_dl(uint8_t* rdram, uint32_t dl_offset, RenderState& st, ExecCtx& ctx) {
        ctx.self = this;
        ctx.st = &st;
        ctx.trace_task = task_count_.load();
        // Per-command tracing is opt-in (OGRE_TRACE_DL=1): every trace line is a
        // proxied stderr write from the gfx pthread, which throttles the DL walk
        // to a crawl and makes a healthy DL look like a hang.
        static const bool trace_dl_env = [] {
            const char* v = getenv("OGRE_TRACE_DL");
            return v != nullptr && v[0] == '1';
        }();
        ctx.trace_enabled = trace_dl_env && ctx.trace_task >= 2 && ctx.trace_task <= 3;
        if (ctx.trace_enabled) {
            OGRE_MILESTONE("GFX-EXEC", "task %u: begin DL @0x%05X", ctx.trace_task, dl_offset);
        }

        auto visitor = [](void* user, const gbi::DlCommand& c) {
            ExecCtx* ctx = static_cast<ExecCtx*>(user);
            const uint32_t idx = ctx->cmd_index;
            const bool trace = ctx->trace_enabled && idx < 1400;
            g_exec_task.store(ctx->trace_task, std::memory_order_relaxed);
            g_exec_cmd.store(idx, std::memory_order_relaxed);
            g_exec_off.store(c.offset, std::memory_order_relaxed);
            g_exec_op.store(c.op, std::memory_order_relaxed);
            if (trace) {
                // Only the "before" line for every command: the last line
                // printed is therefore the command the executor hung on.
                OGRE_MILESTONE("GFX-EXEC", "cmd %u @0x%05X op=0x%02X w0=0x%08X w1=0x%08X",
                               idx, c.offset, c.op, c.w0, c.w1);
            }
            // A walk that steps outside the low 32 MiB of rdram has followed a
            // corrupt G_DL target: report the command that did it (the ring
            // below still holds the 24 commands before this one).
            if (!ctx->escape_logged && c.offset > 0x02000000u) {
                ctx->escape_logged = true;
                char eb[96];
                OGRE_MILESTONE("GFX-ESCAPE",
                               "task %u: walk escaped to @0x%07X op=0x%02X w0=0x%08X w1=0x%08X "
                               "(cmd %u); previous commands:",
                               ctx->trace_task, c.offset, c.op, c.w0, c.w1, idx);
                for (uint32_t k = 0; k < ctx->tail_count; ++k) {
                    const gbi::DlCommand& p = ctx->tail[k];
                    snprintf(eb, sizeof(eb), "@0x%07X op=0x%02X w0=0x%08X w1=0x%08X",
                             p.offset, p.op, p.w0, p.w1);
                    OGRE_MILESTONE("GFX-ESCAPE", "  prev[%u] %s", k, eb);
                }
            }
            if (idx < ExecCtx::kHeadMax) {
                ctx->head[idx] = c;
            }
            if (ctx->tail_count < ExecCtx::kTailMax) {
                ctx->tail[ctx->tail_count++] = c;
            } else {
                memmove(ctx->tail, ctx->tail + 1, sizeof(gbi::DlCommand) * (ExecCtx::kTailMax - 1));
                ctx->tail[ExecCtx::kTailMax - 1] = c;
            }
            ctx->cmd_index++;
            ctx->self->exec_command(ctx, c);
            if (trace && (idx % 64) == 63) {
                OGRE_MILESTONE("GFX-EXEC", "  ^ returned through cmd %u", idx);
            }
        };

        // OGRE_FOG: repair the stale-tile fog pieces in the submitted list
        // before the walker reaches them (the native does this before RT64
        // parses the list). OGRE_FOG=0 turns it off.
        ctx.fog_repaired = fog_repair_display_list(rdram, dl_offset, ctx.trace_task);

        gbi::walk_dl(rdram, dl_offset, visitor, &ctx);
        last_dl_cmds_ = ctx.cmd_index;
        // The first S2DEX2 task reports the whole shape of its list, then the
        // per-200 cadence in exec_command takes over (print_s2d_census).
        if (st.gbi == GbiKind::S2DEX2 && !s2d_first_printed_ && s2d_total_ > 0) {
            s2d_first_printed_ = true;
            s2d_next_print_ = s2d_total_ + 200;
            print_s2d_census(ctx.trace_task);
        }
        if (!ctx.order.empty() && (task_count_.load() <= 4 || (task_count_.load() % 4) == 0)) {
            OGRE_MILESTONE("GFX-ORDER",
                           "task %u quad0=%.4fx%.4f uv=[%.1f..%.1f, %.1f..%.1f] "
                           "tex0=%dx%d tex1=%dx%d order: %.20s",
                           task_count_.load(), ctx.quad_w, ctx.quad_h,
                           ctx.u0, ctx.u1, ctx.v0, ctx.v1,
                           ctx.iw0, ctx.ih0, ctx.iw1, ctx.ih1, ctx.order.c_str());
        }
        // A walk that never finds G_ENDDL is a runaway: the renderer's input
        // is garbage, and both the analyzer and the executor are burning their
        // 4M-command budget on it. Print the head and tail once so the bogus
        // entry pointer / branch target is visible.
        if (ctx.cmd_index > 20000 && runaway_logged_ < 3) {
            ++runaway_logged_;
            auto fmt_cmd = [](char* buf, size_t n, const gbi::DlCommand& c) {
                snprintf(buf, n, "@0x%07X op=0x%02X w0=0x%08X w1=0x%08X", c.offset, c.op, c.w0, c.w1);
            };
            char b[96];
            OGRE_MILESTONE("GFX-RUNAWAY",
                           "task %u: DL entry @0x%07X walked %u commands with no G_ENDDL (data_ptr=0x%08X)",
                           ctx.trace_task, dl_offset, ctx.cmd_index, last_task_data_ptr_);
            for (uint32_t i = 0; i < ExecCtx::kHeadMax && i < ctx.cmd_index; ++i) {
                fmt_cmd(b, sizeof(b), ctx.head[i]);
                OGRE_MILESTONE("GFX-RUNAWAY", "  head[%u] %s", i, b);
            }
            for (uint32_t i = 0; i < ctx.tail_count; ++i) {
                const uint32_t idx = ctx.cmd_index - ctx.tail_count + i;
                fmt_cmd(b, sizeof(b), ctx.tail[i]);
                OGRE_MILESTONE("GFX-RUNAWAY", "  tail[%u] %s", idx, b);
            }
        }
        if (ctx.trace_enabled) {
            OGRE_MILESTONE("GFX-EXEC", "task %u: end DL, %u commands", ctx.trace_task, ctx.cmd_index);
        }
    }

    void exec_command(ExecCtx* ctx, const gbi::DlCommand& c) {
        RenderState& st = *ctx->st;
        const uint32_t w0 = c.w0, w1 = c.w1;
        const uint8_t op = c.op;

        // [GFX-S2D-CENSUS]: count every opcode byte of an S2DEX2 list (see
        // print_s2d_census). The first task reports its whole list at the end of
        // execute_dl, so the per-200 cadence only starts after that.
        if (st.gbi == GbiKind::S2DEX2) {
            ++s2d_op_counts_[op];
            ++s2d_total_;
            if (s2d_first_printed_ && s2d_total_ >= s2d_next_print_) {
                s2d_next_print_ = s2d_total_ + 200;
                print_s2d_census(ctx->trace_task);
            }
        }

        // Remember the texture-setup commands so dump_tex_state() can print the
        // exact recipe that filled TMEM for this draw.
        if (op == gbi::OP_SETTIMG || op == gbi::OP_SETTILE || op == gbi::OP_SETTILESIZE ||
            op == gbi::OP_LOADTILE || op == gbi::OP_LOADBLOCK || op == gbi::OP_LOADTLUT ||
            op == gbi::OP_TEXTURE) {
            if (ctx->load_ring_count < ExecCtx::kLoadRing) {
                ctx->load_ring[ctx->load_ring_count++] = c;
            } else {
                memmove(ctx->load_ring, ctx->load_ring + 1,
                        sizeof(gbi::DlCommand) * (ExecCtx::kLoadRing - 1));
                ctx->load_ring[ExecCtx::kLoadRing - 1] = c;
            }
        }

        // In an S2DEX2 list 0xDC is G_OBJ_MOVEMEM and 0xDA is
        // G_OBJ_RECTANGLE_R; 0x01/0x02 are G_OBJ_RECTANGLE/G_OBJ_SPRITE. They
        // must be decoded before the F3DEX2 switch, where the same opcodes mean
        // G_MOVEMEM, G_MTX and G_VTX.
        if (st.gbi == GbiKind::S2DEX2) {
            switch (op) {
                case S2D_OBJ_MOVEMEM:
                    set_obj_matrix(ctx, resolve_address(ctx->segments, w1), w0 & 0xFFFFu);
                    return;
                case S2D_OBJ_RECTANGLE_R:
                    draw_obj_rect(ctx, resolve_address(ctx->segments, w1), false);
                    return;
                case S2D_OBJ_RECTANGLE:
                    draw_obj_rect(ctx, resolve_address(ctx->segments, w1), false);
                    return;
                case S2D_OBJ_SPRITE:
                    draw_obj_rect(ctx, resolve_address(ctx->segments, w1), true);
                    return;
                case S2D_RDPHALF_0: {
                    // RT64 pairs this with the next G_RDPHALF_1 and calls the
                    // RDP texrect path. Ogre Battle 64's njpeg lists use only
                    // the object commands, so this is reported rather than
                    // decoded, and only once.
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        OGRE_MILESTONE("GFX-S2D",
                                       "S2DEX2 G_RDPHALF_0 (0x%02X) texrect encoding seen at "
                                       "0x%05X and not implemented; only the object commands draw",
                                       (unsigned)S2D_RDPHALF_0, c.offset);
                    }
                    return;
                }
                case 0x03:  // no S2DEX2 meaning; 0x03 is F3DEX2's G_CULLDL
                case 0x04:  // G_SELECT_DL
                case 0x05:  // G_OBJ_LOADTXTR
                case 0x06:  // G_OBJ_LDTX_SPRITE
                case 0x07:  // G_OBJ_LDTX_RECT
                case 0x08:  // G_OBJ_LDTX_RECT_R
                case 0x09:  // G_BG_1CYC
                case 0x0A:  // G_BG_COPY
                case 0x0B:  // G_OBJ_RENDERMODE
                    // S2DEX2 commands this renderer does not implement. Falling
                    // through would decode 0x05/0x06/0x07 as F3DEX2
                    // G_TRI1/G_TRI2/G_QUAD and draw from the stale vertex
                    // buffer, and 0x03 as G_CULLDL. No OB64 list reaches any of
                    // them (the S2DEX2 task's only list builder emits 0xDA/0xDC;
                    // see docs/HANDOFF-2026-10-24-session189.md), so this is a
                    // guard against a wrong frame, not a reached behaviour.
                    return;
                default:
                    break;
            }
        }

        switch (op) {
            case gbi::OP_MTX: {                // F3DEX2 stores the PUSH bit *inverted*: the SDK macro XORs
                // G_MTX_PUSH into the flags, so a stored 0x00 is
                // MODELVIEW|MUL|PUSH and a stored 0x01 is MODELVIEW|MUL|NOPUSH.
                // RT64 does the same (`(*dl)->p0(0, 8) ^ pushMask` in
                // GBI_F3DEX2::matrix). Reading the bit literally made every
                // object matrix a no-push MUL, so the title screen's
                // per-object matrices accumulated into one another (its two
                // G_POPMTX per object had nothing to pop) and the sprites
                // drifted progressively off-screen.
                const uint32_t flags = p0(w0, 0, 8) ^ gbi::MTX_PUSH;
                const uint32_t addr = resolve_address(ctx->segments, w1);
                Mat4 m = read_matrix(addr);
                if (mtx_logged_ < 10) {
                    ++mtx_logged_;
                    // Row-vector convention: the translation is row 3.
                    OGRE_MILESTONE("GFX-MTX",
                                   "task %u G_MTX raw=0x%02X -> flags=0x%02X proj=%d load=%d push=%d "
                                   "addr=0x%08X diag=[%.4f %.4f %.4f %.4f] trans=[%.3f %.3f %.3f]",
                                   task_count_.load(), p0(w0, 0, 8), flags,
                                   (flags & gbi::MTX_PROJECTION) ? 1 : 0,
                                   (flags & gbi::MTX_LOAD) ? 1 : 0,
                                   (flags & gbi::MTX_PUSH) ? 1 : 0, addr,
                                   m.m[0], m.m[5], m.m[10], m.m[15],
                                   m.m[12], m.m[13], m.m[14]);
                }
                if (flags & gbi::MTX_PROJECTION) {
                    // The projection stack is a single composite (RT64 keeps
                    // one `viewProjMatrix`): LOAD replaces it, MUL multiplies
                    // the new matrix in *front* of it so the new matrix is
                    // applied to the vertex first (RT64 RSP::matrixCommon does
                    // `mul(floatMatrix, viewProjMatrix)`). OB64 loads a
                    // perspective matrix and then multiplies a second matrix
                    // onto it, so ignoring MUL left `projection` holding only
                    // the second matrix.
                    if (flags & gbi::MTX_LOAD) {
                        st.projection = m;
                    } else {
                        st.projection = mul_mat4(m, st.projection);
                    }
                } else {
                    if (flags & gbi::MTX_PUSH) {
                        // Copy first: push_back(vector.back()) is UB when the
                        // push reallocates (the argument reference dangles).
                        const Mat4 pushed = st.modelview_stack.back();
                        st.modelview_stack.push_back(pushed);
                    }
                    Mat4& cur = st.modelview_stack.back();
                    if (flags & gbi::MTX_LOAD) {
                        cur = m;
                    } else {
                        // row-vector: the new matrix applies to the vertex
                        // first, i.e. M = m * M_old (RT64 does the same).
                        cur = mul_mat4(m, cur);
                    }
                }
                break;
            }

            case gbi::OP_POPMTX: {
                const int n = static_cast<int>(w1 >> 6);
                for (int i = 0; i < n && st.modelview_stack.size() > 1; ++i) {
                    st.modelview_stack.pop_back();
                }
                break;
            }

            case gbi::OP_GEOMETRYMODE: {
                // RT64 GBI_F3DEX2::geometryMode (gbi/rt64_gbi_f3dex2.cpp:104-108):
                //   offMask = p0(0, 24); onMask = w1;
                //   geometryMode &= offMask; geometryMode |= onMask;
                // The w0 low 24 bits are the bits to KEEP, not the bits to
                // clear: `gSPClearGeometryMode(word)` passes them as `~word`.
                // The ROM's own reset idiom is `D9000000 00000000` (41 sites),
                // which clears every bit under this rule and would be a no-op
                // under the inverted one; `D9FFFFFF 00000000` never appears.
                // OB64 also emits `D9FFFFFF 00200404` (ROM `0x80180D90`), which
                // keeps the current mode and adds G_LIGHTING|G_SHADE|G_CULL_BACK.
                // Reading w0 as a clear mask wiped G_LIGHTING on every
                // `gSPSetGeometryMode`, so a sub-list that used that idiom lost
                // lighting, fog, culling and G_TEXTURE_GEN (session 185).
                const uint32_t keep = p0(w0, 0, 24);
                const uint32_t on = w1;
                st.geometry_mode = (st.geometry_mode & keep) | on;
                break;
            }

            case gbi::OP_TEXTURE: {
                st.active_tile = static_cast<int>(p0(w0, 8, 3));
                // The scale fields are unsigned 16-bit; RT64 zero-extends them
                // into an int32 (`(int32_t)(textureState.sc)`), so 0x8000 is
                // 32768 (0.5), not -32768.
                st.tex_sc = static_cast<int32_t>(p0(w1, 16, 16));
                st.tex_tc = static_cast<int32_t>(p0(w1, 0, 16));
                st.tex_scale_s = static_cast<float>(st.tex_sc) / 65536.0f;
                st.tex_scale_t = static_cast<float>(st.tex_tc) / 65536.0f;
                st.texture_on = p0(w0, 1, 7) != 0;
                if (tex_state_logged_ < 8) {
                    ++tex_state_logged_;
                    OGRE_MILESTONE("GFX-TXSTATE",
                                   "G_TEXTURE: tile=%d on=%d level=%u sc=%d tc=%d "
                                   "(TEXEL0=tile %d, TEXEL1=tile %d)",
                                   st.active_tile, st.texture_on ? 1 : 0,
                                   static_cast<unsigned>(p0(w0, 3, 3)), st.tex_sc, st.tex_tc,
                                   st.active_tile & (kMaxTiles - 1), (st.active_tile + 1) & (kMaxTiles - 1));
                }
                break;
            }

            case gbi::OP_VTX: {
                const uint32_t n = p0(w0, 12, 8);
                const int dst = static_cast<int>(p0(w0, 1, 7)) - static_cast<int>(n);
                const uint32_t addr = resolve_address(ctx->segments, w1);
                if (dst < 0 || dst + n > kMaxVertices) {
                    break;
                }
                for (uint32_t i = 0; i < n; ++i) {
                    const uint8_t* p = rdram_ + addr + i * 16;
                    RawVertex& v = st.vertices[dst + i];
                    // Field offsets are the N64 Vtx layout; n64h/n64b apply the
                    // runtime's byte-reversed-word addressing (see above).
                    v.x = static_cast<int16_t>(n64h(p, 0));
                    v.y = static_cast<int16_t>(n64h(p, 2));
                    v.z = static_cast<int16_t>(n64h(p, 4));
                    v.flag = n64h(p, 6);
                    v.s = static_cast<int16_t>(n64h(p, 8));
                    v.t = static_cast<int16_t>(n64h(p, 10));
                    v.r = n64b(p, 12);
                    v.g = n64b(p, 13);
                    v.b = n64b(p, 14);
                    v.a = n64b(p, 15);
                }
                break;
            }

            case gbi::OP_TRI1:
                draw_tri(ctx, st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                break;

            case gbi::OP_TRI2:
                draw_tri(ctx, st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                draw_tri(ctx, st, p0(w1, 17, 7), p0(w1, 9, 7), p0(w1, 1, 7));
                break;

            case gbi::OP_QUAD:
                draw_tri(ctx, st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                draw_tri(ctx, st, p0(w1, 17, 7), p0(w1, 9, 7), p0(w1, 1, 7));
                break;

            case gbi::OP_FILLRECT: {
                // Same layout as G_TEXRECT: lrx/lry in w0, ulx/uly in w1. With
                // the two words swapped a full-screen fill decodes as an
                // inverted rect and is dropped by draw_fill_rect()'s guard - so
                // the game's per-frame clear never happened and every frame
                // accumulated on the canvas (the title scene's "trails").
                int xl = static_cast<int>(p0(w1, 12, 12)) >> 2;
                int yl = static_cast<int>(p0(w1, 0, 12)) >> 2;
                int xh = static_cast<int>(p0(w0, 12, 12)) >> 2;
                int yh = static_cast<int>(p0(w0, 0, 12)) >> 2;
                // Fill/copy mode rounds up to the end of the 4-pixel group
                // (RT64 RDP::fillRect: `lrx |= 3; lry |= 3`) and rounds the
                // origin down (RDP::drawRect).
                const uint32_t cyc = (st.othermode_h >> 20) & 3;
                if (cyc == kCycCopy || cyc == kCycFill) {
                    xl &= ~3;
                    yl &= ~3;
                    xh |= 3;
                    yh |= 3;
                }
                draw_fill_rect(ctx, st, xl, yl, xh, yh);
                break;
            }

            case gbi::OP_TEXRECT:
            case gbi::OP_TEXRECTFLIP: {
                // The next two commands carry (s,t) and (dsdx,dtdy).
                //
                // The rectangle itself is `lrx/lry` in w0 and `ulx/uly` in w1
                // (RT64 GBI_RDP::texrect reads `p0` for the lower-right corner
                // and `p1` for the upper-left; the SDK macro splits them the
                // same way). Reading them the other way round turns every
                // `gSPTextureRectangle(pkt, 0, 0, w-1, h-1, ...)` - including
                // the game's full-screen background - into an inverted rect.
                ctx->pending_texrect = (op == gbi::OP_TEXRECT);
                ctx->pending_texrect_flip = (op == gbi::OP_TEXRECTFLIP);
                ctx->rdp_half1 = 0;
                ctx->rdp_half2 = 0;
                int xl = static_cast<int>(p0(w1, 12, 12)) >> 2;
                int yl = static_cast<int>(p0(w1, 0, 12)) >> 2;
                const int xh = static_cast<int>(p0(w0, 12, 12)) >> 2;
                const int yh = static_cast<int>(p0(w0, 0, 12)) >> 2;
                // Copy/fill mode rounds the upper-left corner down to a 4-pixel
                // boundary (RT64 RDP::drawRect).
                const uint32_t cyc = (st.othermode_h >> 20) & 3;
                if (cyc == kCycCopy || cyc == kCycFill) {
                    xl &= ~3;
                    yl &= ~3;
                }
                texrect_geom_[0] = xl;
                texrect_geom_[1] = yl;
                texrect_geom_[2] = xh;
                texrect_geom_[3] = yh;
                texrect_tile_ = static_cast<int>(p0(w1, 24, 3));
                break;
            }

            case gbi::OP_RDPHALF_1:
                if (ctx->pending_texrect) {
                    ctx->rdp_half1 = w1;
                }
                break;

            case gbi::OP_RDPHALF_2:
                if (ctx->pending_texrect) {
                    ctx->rdp_half2 = w1;
                    draw_texrect(ctx);
                    ctx->pending_texrect = false;
                    ctx->pending_texrect_flip = false;
                }
                break;

            case gbi::OP_SETCIMG: {
                // RT64 GBI_RDP::setColorImage: fmt bits 21-23, siz bits 19-20,
                // width = bits 0-11 + 1, and the raw second word masked to the
                // 24-bit RDRAM window (RDP::maskAddress, `address & 0xFFFFFF`).
                // The RDP keeps this state across display lists, so the renderer
                // remembers it and each new task's RenderState starts from it.
                const uint8_t fmt = static_cast<uint8_t>(p0(w0, 21, 3));
                const uint8_t siz = static_cast<uint8_t>(p0(w0, 19, 2));
                const uint32_t width = p0(w0, 0, 12) + 1;
                const uint32_t address = w1 & 0x00FFFFFFu;
                st.cimg_address = address;
                st.cimg_siz = siz;
                st.cimg_fmt = fmt;
                st.cimg_width = static_cast<uint16_t>(width);
                st.cimg_row_bytes = (address != 0) ? (width << siz >> 1) : 0;
                last_cimg_address_ = address;
                last_cimg_row_bytes_ = st.cimg_row_bytes;
                last_cimg_width_ = st.cimg_width;
                last_cimg_siz_ = siz;
                last_cimg_fmt_ = fmt;
                // Log each distinct colour-image configuration once: the
                // addresses and pitches are what the framebuffer writeback has
                // to match, and the game uses more than one (the world map and
                // its 640-pixel-wide targets).
                const uint64_t cimg_key = (static_cast<uint64_t>(address) << 24) |
                                          (width << 4) | (siz << 2) | fmt;
                if (cimg_seen_.size() < 16 && cimg_seen_.insert(cimg_key).second) {
                    OGRE_MILESTONE("GFX-CIMG",
                                   "SETCIMG addr=0x%06X fmt=%u siz=%u width=%u rowBytes=%u",
                                   address, (unsigned)fmt, (unsigned)siz, width,
                                   st.cimg_row_bytes);
                }
                break;
            }

            case gbi::OP_SETZIMG: {
                // RT64 GBI_RDP::setDepthImage: the RDP's depth image is the
                // same word shape as G_SETCIMG (fmt bits 21-23, siz bits 19-20,
                // width = bits 0-11 + 1) with the address in w1 masked to the
                // 24-bit RDRAM window. The RDP keeps it across display lists,
                // like the colour image (send_dl seeds a new task from the last
                // one).
                const uint8_t fmt = static_cast<uint8_t>(p0(w0, 21, 3));
                const uint8_t siz = static_cast<uint8_t>(p0(w0, 19, 2));
                const uint32_t width = p0(w0, 0, 12) + 1;
                const uint32_t address = w1 & 0x00FFFFFFu;
                st.zimg_address = address;
                st.zimg_siz = siz;
                st.zimg_fmt = fmt;
                st.zimg_width = static_cast<uint16_t>(width);
                last_zimg_address_ = address;
                last_zimg_siz_ = siz;
                last_zimg_fmt_ = fmt;
                last_zimg_width_ = st.zimg_width;
                const uint64_t zimg_key = (static_cast<uint64_t>(address) << 24) |
                                          (width << 4) | (siz << 2) | fmt;
                if (zimg_seen_.size() < 8 && zimg_seen_.insert(zimg_key).second) {
                    OGRE_MILESTONE("GFX-ZIMG",
                                   "SETZIMG addr=0x%06X fmt=%u siz=%u width=%u",
                                   address, (unsigned)fmt, (unsigned)siz, width);
                }
                break;
            }

            case gbi::OP_SETTIMG: {
                st.timg_fmt = static_cast<uint8_t>(p0(w0, 21, 3));
                st.timg_siz = static_cast<uint8_t>(p0(w0, 19, 2));
                st.timg_width = static_cast<uint16_t>(p0(w0, 0, 12) + 1);
                st.timg_address = resolve_address(ctx->segments, w1);
                break;
            }

            case gbi::OP_SETTILE: {
                // Tile index lives in w1 bits 24-26 (RT64 GBI_RDP::setTile).
                const int tile = static_cast<int>(p0(w1, 24, 3));
                TileState& t = st.tiles[tile];
                t.fmt = static_cast<uint8_t>(p0(w0, 21, 3));
                t.siz = static_cast<uint8_t>(p0(w0, 19, 2));
                t.line = static_cast<uint16_t>(p0(w0, 9, 9));
                t.tmem = static_cast<uint16_t>(p0(w0, 0, 9));
                t.palette = static_cast<uint8_t>(p0(w1, 20, 4));
                t.cmt = static_cast<uint8_t>(p0(w1, 18, 2));
                t.cms = static_cast<uint8_t>(p0(w1, 8, 2));
                t.maskt = static_cast<uint8_t>(p0(w1, 14, 4));
                t.masks = static_cast<uint8_t>(p0(w1, 4, 4));
                t.shiftt = static_cast<uint8_t>(p0(w1, 10, 4));
                t.shifts = static_cast<uint8_t>(p0(w1, 0, 4));
                claim_tile_load(st, tile);
                break;
            }

            case gbi::OP_SETTILESIZE: {
                const int tile = static_cast<int>(p0(w1, 24, 3));
                TileState& t = st.tiles[tile];
                t.uls = static_cast<uint16_t>(p0(w0, 12, 12));
                t.ult = static_cast<uint16_t>(p0(w0, 0, 12));
                t.lrs = static_cast<uint16_t>(p0(w1, 12, 12));
                t.lrt = static_cast<uint16_t>(p0(w1, 0, 12));
                break;
            }

            // The load's own rect/format are deliberately not decoded here: the
            // render tile that samples the result is configured by the
            // G_SETTILE/G_SETTILESIZE that follow (see note_tile_load).
            case gbi::OP_LOADTILE:
            case gbi::OP_LOADBLOCK: {
                // The command's own rect and the last SETTIMG/SETTILE decide
                // what lands in TMEM (RT64 `RDP::loadTile`/`loadBlock`); the
                // render tile that samples it is configured afterwards.
                tmem_load_command(ctx, st, op == gbi::OP_LOADBLOCK, false, w0, w1);
                note_tile_load(st, static_cast<int>(p0(w1, 24, 3)));
                break;
            }

            case gbi::OP_LOADTLUT: {
                const int tile = static_cast<int>(p0(w1, 24, 3));
                TileState& t = st.tiles[tile];
                // The palette lands in TMEM at the load tile's `tmem << 3`
                // (RT64 `loadTLUTOperation`), and the CI decode reads it from
                // there. `tlut_` stays as the diagnostics' copy.
                tmem_load_command(ctx, st, false, true, w0, w1);
                // Number of 16-bit TLUT entries: lrs is 10.2 fixed -> entries.
                const uint16_t count = static_cast<uint16_t>((p0(w1, 12, 12) >> 2) + 1);
                const uint32_t bank = static_cast<uint32_t>(t.palette) * 16;
                const uint32_t base = st.timg_address + ((p0(w0, 12, 12) >> 2) << st.timg_siz >> 1);
                for (uint16_t i = 0; i < count && bank + i < 256; ++i) {
                    tlut_[bank + i] = n64be16(rdram_, base + i * 2);
                }
                tlut_valid_ = true;
                tlut_count_ = count;
                tlut_nonzero_ = 0;
                for (int i = 0; i < 256; ++i) {
                    if (tlut_[i] != 0) {
                        ++tlut_nonzero_;
                    }
                }
                ++tlut_generation_;
                if (tlut_trace_) {
                    // The one-line report a future screen needs if it loads a
                    // palette somewhere other than the assumed destination.
                    // RT64 writes the load at `loadTile.tmem << 3` and samples
                    // CI4 at TMEM 0x800 + (palette << 7) (16 entries of 8 bytes
                    // per bank) and CI8 at 0x800 + (index << 3)
                    // (`TextureDecoder.hlsli`, `sampleTMEMWithConvert`), so a
                    // load at tmem 0x100 is the destination the fixed base
                    // samples.
                    const uint64_t dest = (static_cast<uint64_t>(t.tmem) << 4) | t.palette;
                    if (tlut_dest_seen_.insert(dest).second) {
                        const bool assumed = (t.tmem == 0x100 && t.palette == 0);
                        OGRE_MILESTONE("GFX-TLUT",
                                       "load dest tmem=0x%X palette=%u entries=%u bytes=%u "
                                       "addr=0x%06X: %s",
                                       (unsigned)t.tmem, (unsigned)t.palette, (unsigned)count,
                                       (unsigned)count * 2u, base,
                                       assumed ? "the assumed destination"
                                               : "NOT the assumed dest (tmem=0x100 palette=0); "
                                                 "the decoder banks CI tables by the tile's "
                                                 "palette field only");
                    }
                }
                if (trace_active()) {
                    ++trace_tluts_;
                    if (trace_tluts_ <= 8) {
                        OGRE_MILESTONE("GFX-TEXTRACE",
                                       "loadtlut t%u gen=%llu addr=0x%06X base=0x%06X count=%u "
                                       "bank=%u first=%04X %04X %04X %04X",
                                       (unsigned)tile, (unsigned long long)tlut_generation_, base,
                                       st.timg_address, (unsigned)count, (unsigned)bank, tlut_[0],
                                       tlut_[1], tlut_[2], tlut_[3]);
                    }
                }
                (void)tile;
                break;
            }

            case gbi::OP_SETCOMBINE: {
                const uint32_t L = w0;  // low word
                const uint32_t H = w1;  // high word
                st.combiner.decode(L, H);
                break;
            }

            case gbi::OP_RDPSETOTHERMODE: {
                // Full 64-bit set: high 24 bits from w0, low from w1.
                st.othermode_h = p0(w0, 0, 24);
                st.othermode_l = w1 & 0xFFFFFF;
                break;
            }

            case gbi::OP_SETOTHERMODE_H: {
                // G_SETOTHERMODE_H: w0 bits 7-0 = field length-1, bits 15-8 =
                // (32 - shift - length). The data word is ALREADY shifted into
                // place (the SDK macro emits `val << sft`), so only the target
                // field is cleared. Masking and shifting `w1` a second time
                // (what this renderer did) drops every bit of the field and
                // leaves OTHERMODE_H at zero - no dithering, no filtering, no
                // perspective correction, and always G_CYC_1CYCLE.
                // RT64 RSP::setOtherModeH does `(H & ~mask) | data`.
                const uint32_t size = p0(w0, 0, 8) + 1;
                const uint32_t off = static_cast<uint32_t>(
                    std::max(0, static_cast<int>(32 - p0(w0, 8, 8) - size)));
                const uint32_t mask = (size >= 32) ? 0xFFFFFFFFu : (((1u << size) - 1) << off);
                st.othermode_h = (st.othermode_h & ~mask) | w1;
                break;
            }

            case gbi::OP_SETOTHERMODE_L: {
                const uint32_t size = p0(w0, 0, 8) + 1;
                const uint32_t off = static_cast<uint32_t>(
                    std::max(0, static_cast<int>(32 - p0(w0, 8, 8) - size)));
                const uint32_t mask = (size >= 32) ? 0xFFFFFFFFu : (((1u << size) - 1) << off);
                st.othermode_l = ((st.othermode_l & ~mask) | w1) & 0xFFFFFF;
                break;
            }

            case gbi::OP_SETSCISSOR: {
                st.scissor_enabled = true;
                st.scissor_x0 = static_cast<int>(p0(w0, 12, 12)) >> 2;
                st.scissor_y0 = static_cast<int>(p0(w0, 0, 12)) >> 2;
                st.scissor_x1 = static_cast<int>(p0(w1, 12, 12)) >> 2;
                st.scissor_y1 = static_cast<int>(p0(w1, 0, 12)) >> 2;
                break;
            }

            case gbi::OP_SETPRIMCOLOR: {
                // G_SET*COLOR packs (R<<24)|(G<<16)|(B<<8)|A.
                st.prim_color[3] = (p0(w1, 0, 8)) / 255.0f;   // A
                st.prim_color[0] = (p0(w1, 24, 8)) / 255.0f;  // R
                st.prim_color[1] = (p0(w1, 16, 8)) / 255.0f;  // G
                st.prim_color[2] = (p0(w1, 8, 8)) / 255.0f;   // B
                break;
            }

            case gbi::OP_SETENVCOLOR: {
                // G_SET*COLOR packs (R<<24)|(G<<16)|(B<<8)|A.
                st.env_color[3] = (p0(w1, 0, 8)) / 255.0f;   // A
                st.env_color[0] = (p0(w1, 24, 8)) / 255.0f;  // R
                st.env_color[1] = (p0(w1, 16, 8)) / 255.0f;  // G
                st.env_color[2] = (p0(w1, 8, 8)) / 255.0f;   // B
                break;
            }

            case gbi::OP_SETFILLCOLOR: {
                // G_SET*COLOR packs (R<<24)|(G<<16)|(B<<8)|A.
                st.fill_color_word = w1;
                st.fill_color[3] = (p0(w1, 0, 8)) / 255.0f;   // A
                st.fill_color[0] = (p0(w1, 24, 8)) / 255.0f;  // R
                st.fill_color[1] = (p0(w1, 16, 8)) / 255.0f;  // G
                st.fill_color[2] = (p0(w1, 8, 8)) / 255.0f;   // B
                break;
            }

            case gbi::OP_SETFOGCOLOR: {
                // The blender's FOG_COLOR input (RT64 Blender::Inputs::fogColor).
                st.fog_color[3] = (p0(w1, 0, 8)) / 255.0f;
                st.fog_color[0] = (p0(w1, 24, 8)) / 255.0f;
                st.fog_color[1] = (p0(w1, 16, 8)) / 255.0f;
                st.fog_color[2] = (p0(w1, 8, 8)) / 255.0f;
                break;
            }

            case gbi::OP_SETBLENDCOLOR: {
                // The blender's BLEND_COLOR input; its alpha is also the
                // G_AC_THRESHOLD alpha-compare reference.
                st.blend_color[3] = (p0(w1, 0, 8)) / 255.0f;
                st.blend_color[0] = (p0(w1, 24, 8)) / 255.0f;
                st.blend_color[1] = (p0(w1, 16, 8)) / 255.0f;
                st.blend_color[2] = (p0(w1, 8, 8)) / 255.0f;
                break;
            }

            case gbi::OP_SETCONVERT: {
                // G_SETCONVERT's six 9-bit fields, three in w0 and three in w1
                // (RT64 GBI_RDP::setConvert). The sign extension and the
                // `2*K+1` scaling happen where the matrix is used
                // (`yuv16_to_rgba8`).
                st.convert_k[0] = static_cast<int32_t>(p0(w0, 13, 9));
                st.convert_k[1] = static_cast<int32_t>(p0(w0, 4, 9));
                st.convert_k[2] = static_cast<int32_t>((p0(w0, 0, 4) << 5) | p0(w1, 27, 5));
                st.convert_k[3] = static_cast<int32_t>(p0(w1, 18, 9));
                st.convert_k[4] = static_cast<int32_t>(p0(w1, 9, 9));
                st.convert_k[5] = static_cast<int32_t>(p0(w1, 0, 9));
                break;
            }

            case gbi::OP_MOVEMEM: {
                // F3DEX2 G_MOVEMEM: the index is the low byte of w0 (RT64
                // GBI_F3DEX2::moveMem), w1 is the address of the payload.
                const uint8_t index = static_cast<uint8_t>(p0(w0, 0, 8));
                if (index == gbi::MV_VIEWPORT) {
                    set_viewport(ctx, st, resolve_address(ctx->segments, w1));
                } else if (index == gbi::MV_LIGHT) {
                    // RT64 GBI_F3DEX2::moveMem (gbi/rt64_gbi_f3dex2.cpp:48-61):
                    // `offset = p0(8, 8) * 8`, `index = offset / 24`; slots 0
                    // and 1 are the lookat X/Y vectors (texgen) and slots >= 2
                    // are lights. RSP_MAX_LIGHTS + 1 slots, RT64 stores 16 bytes
                    // per light; this keeps 24 so the slot stride matches.
                    const uint32_t offset = p0(w0, 8, 8) * 8;
                    const int slot = static_cast<int>(offset / 24);
                    if (slot >= 2 && slot - 2 < RenderState::kMaxLights) {
                        const uint32_t addr = resolve_address(ctx->segments, w1);
                        uint8_t* dst = st.lights[slot - 2];
                        for (int k = 0; k < RenderState::kLightStride; ++k) {
                            dst[k] = n64b(rdram_, static_cast<int>(addr) + k);
                        }
                    } else if (slot < 2) {
                        // Lookat X (slot 0) and Y (slot 1) for G_TEXTURE_GEN.
                        // RT64 `RSP::setLookAt` (hle/rt64_rsp.cpp:969-982)
                        // reads the slot as a `DirLight` and normalizes logical
                        // bytes 8/9/10 (`dirx/diry/dirz`); a zero vector stays
                        // zero instead of dividing by zero.
                        const uint32_t addr = resolve_address(ctx->segments, w1);
                        float d[3];
                        for (int k = 0; k < 3; ++k) {
                            d[k] = static_cast<float>(
                                static_cast<int8_t>(n64b(rdram_, static_cast<int>(addr) + 8 + k)));
                        }
                        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (len > 0.0f) {
                            for (int k = 0; k < 3; ++k) {
                                st.lookat[slot][k] = d[k] / len;
                            }
                        } else {
                            for (int k = 0; k < 3; ++k) {
                                st.lookat[slot][k] = 0.0f;
                            }
                        }
                    }
                }
                break;
            }

            case gbi::OP_MOVEWORD: {
                // RT64 GBI_F3DEX2::moveWord (gbi/rt64_gbi_f3dex2.cpp:64-94)
                // dispatches on p0(16, 8); only the segment write was handled
                // here before.
                const uint8_t type = static_cast<uint8_t>(p0(w0, 16, 8));
                if (type == gbi::MW_NUMLIGHT) {
                    // `setLightCount(w1 / 24)`; the +1 ambient slot is why the
                    // count is clamped below kMaxLights.
                    st.num_lights = static_cast<int>(w1 / 24);
                    if (st.num_lights < 0) st.num_lights = 0;
                    if (st.num_lights > RenderState::kMaxLights - 1) {
                        st.num_lights = RenderState::kMaxLights - 1;
                    }
                } else if (type == gbi::MW_LIGHTCOL) {
                    // `setLightColor(p0(0, 16) / 24, w1)`: RT64 sets the light's
                    // two 32-bit colour words to the packed value
                    // (tools/RT64/src/hle/rt64_rsp.cpp:938). A word in the
                    // runtime's image reads back as its big-endian byte order,
                    // so logical bytes 0..3 (and 4..7) are w1's bytes 3,2,1,0
                    // and byte 0 becomes the light's red channel.
                    const int li = static_cast<int>(p0(w0, 0, 16) / 24);
                    if (li >= 0 && li < RenderState::kMaxLights) {
                        for (int k = 0; k < 4; ++k) {
                            const uint8_t b = static_cast<uint8_t>((w1 >> (24 - 8 * k)) & 0xFFu);
                            st.lights[li][k] = b;
                            st.lights[li][4 + k] = b;
                        }
                    }
                } else if (type == gbi::MW_FOG) {
                    // `setFog((int16_t)p1(16, 16), (int16_t)p1(0, 16))`.
                    st.fog_mul = static_cast<int16_t>(p0(w1, 16, 16));
                    st.fog_offset = static_cast<int16_t>(p0(w1, 0, 16));
                } else {
                    // The RSP keeps only the segment base's high byte; segmented
                    // addresses then resolve as (base << 24) | offset. Storing
                    // the full base made every segmented matrix/vertex address
                    // resolve to garbage.
                    gbi::set_segment_from_moveword(ctx->segments, w0, w1);
                }
                break;
            }

            case gbi::OP_SETPRIMDEPTH: {
                // RT64 RDP::setPrimDepth (hle/rt64_rdp.cpp:1160): the high
                // halfword is the 15-bit screen depth, the low halfword the
                // 16-bit decal delta. `primDepth.x` is 0..1 in the RDP's screen
                // depth space, which is the same space as the viewport
                // transform's output z (see transform_vertex).
                st.prim_depth = static_cast<float>((w1 >> 16) & 0x7FFFu) / 32767.0f;
                st.prim_depth_dz = static_cast<float>(w1 & 0xFFFFu) / 65535.0f;
                break;
            }

            case gbi::OP_SETKEYR: {
                // RT64 GBI_RDP::setKeyR (gbi/rt64_gbi_rdp.cpp:154-159):
                // cR = p1(8,8), sR = p1(0,8), wR = p1(16,12) (width ignored).
                st.key_center[0] = static_cast<float>(p0(w1, 8, 8)) / 255.0f;
                st.key_scale[0] = static_cast<float>(p0(w1, 0, 8)) / 255.0f;
                break;
            }

            case gbi::OP_SETKEYGB: {
                // RT64 GBI_RDP::setKeyGB (gbi/rt64_gbi_rdp.cpp:161-169):
                // cG = p1(24,8), sG = p1(16,8), cB = p1(8,8), sB = p1(0,8);
                // the widths p0(12,12)/p0(0,12) are ignored.
                st.key_center[1] = static_cast<float>(p0(w1, 24, 8)) / 255.0f;
                st.key_center[2] = static_cast<float>(p0(w1, 8, 8)) / 255.0f;
                st.key_scale[1] = static_cast<float>(p0(w1, 16, 8)) / 255.0f;
                st.key_scale[2] = static_cast<float>(p0(w1, 0, 8)) / 255.0f;
                break;
            }

            case gbi::OP_MODIFYVTX:
            case gbi::OP_CULLDL:
            case gbi::OP_BRANCH_Z:
            case gbi::OP_LINE3D:
            case gbi::OP_SPECIAL_1:
            case gbi::OP_DMA_IO:
            case gbi::OP_LOAD_UCODE:
            case gbi::OP_SPNOOP:
            case gbi::OP_RDPLOADSYNC:
            case gbi::OP_RDPPIPESYNC:
            case gbi::OP_RDPTILESYNC:
            case gbi::OP_RDPFULLSYNC:
            case gbi::OP_DL:
            case gbi::OP_ENDDL:
            default:
                break;  // handled by the walker or irrelevant for rendering
        }
    }

    // True when either texel unit a draw can sample holds a decoded image.
    // TEXEL0 is G_TEXTURE's tile and TEXEL1 the tile above it (RT64
    // RasterPS.hlsl: `rdpTileIndex + tileIndex0/1`, lodFraction 1).
    static bool texture_available(const RenderState& st) {
        return st.tiles[st.active_tile & (kMaxTiles - 1)].image_valid ||
               st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image_valid;
    }

    // ----- texture helpers ----------------------------------------------------

    // Session-23 diagnostic: everything that decides what a textured draw
    // samples - G_TEXTURE, the image (TIMG), every programmed tile's rect and
    // addressing, and the tile loads that filled TMEM. Printed for the first
    // few draws only (each line is a proxied stderr write from the gfx thread).
    void dump_tex_state(ExecCtx* ctx, const RenderState& st, const char* kind,
                        const float* uv) {
        // One burst of three for each scene the dispatcher enters, so a screen
        // reached late in a scripted route states its own tile setup instead of
        // the title's. Bounded to three lines per scene.
        const int scene = ogre_web_active_scene();
        if (scene != tex_state_scene_) {
            tex_state_scene_ = scene;
            tex_state_dump_logged_ = 0;
        }
        if (tex_state_dump_logged_ >= 3) {
            return;
        }
        ++tex_state_dump_logged_;
        const TileState& t0 = st.tiles[st.active_tile & (kMaxTiles - 1)];
        const TileState& t1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)];
        OGRE_MILESTONE("GFX-TDSTATE",
                       "%s G_TEXTURE tile=%d on=%d sc=%u tc=%u | TIMG addr=0x%08X fmt=%d siz=%d width=%d | "
                       "TEXEL0=t%d %dx%d@(%d,%d) src=0x%08X TEXEL1=t%d %dx%d@(%d,%d) src=0x%08X",
                       kind, st.active_tile, st.texture_on ? 1 : 0,
                       static_cast<unsigned>(st.tex_sc),
                       static_cast<unsigned>(st.tex_tc),
                       st.timg_address, st.timg_fmt, st.timg_siz, st.timg_width,
                       st.active_tile & (kMaxTiles - 1),
                       t0.image.width, t0.image.height, t0.image.origin_s, t0.image.origin_t,
                       t0.image_address,
                       (st.active_tile + 1) & (kMaxTiles - 1),
                       t1.image.width, t1.image.height, t1.image.origin_s, t1.image.origin_t,
                       t1.image_address);
        if (uv != nullptr) {
            OGRE_MILESTONE("GFX-TDSTATE",
                           "  scene=0x%X uv=[%.1f..%.1f, %.1f..%.1f] comb=%08X/%08X "
                           "omh=0x%06X oml=0x%08X geom=0x%08X",
                           static_cast<unsigned>(scene), uv[0], uv[6], uv[1], uv[7],
                           st.combiner.L, st.combiner.H, st.othermode_h, st.othermode_l,
                           st.geometry_mode);
        }
        for (int i = 0; i < kMaxTiles; ++i) {
            const TileState& t = st.tiles[i];
            if (t.line == 0 && t.tmem == 0 && t.uls == 0 && t.ult == 0 && t.lrs == 0 && t.lrt == 0) {
                continue;  // never programmed
            }
            OGRE_MILESTONE("GFX-TDSTATE",
                           "  tile%d: fmt=%d siz=%d line=%d tmem=%d pal=%d uls=%d ult=%d lrs=%d lrt=%d "
                           "cmt=%d cms=%d maskt=%d masks=%d shiftt=%d shifts=%d",
                           i, t.fmt, t.siz, t.line, t.tmem, t.palette, t.uls, t.ult, t.lrs, t.lrt,
                           t.cmt, t.cms, t.maskt, t.masks, t.shiftt, t.shifts);
        }
        for (uint32_t k = 0; k < ctx->load_ring_count; ++k) {
            const gbi::DlCommand& c = ctx->load_ring[k];
            const uint32_t a = c.w0, b = c.w1;
            char what[160] = "";
            switch (c.op) {
                case gbi::OP_SETTIMG:
                    snprintf(what, sizeof(what), "SETTIMG fmt=%u siz=%u w=%u addr=0x%08X",
                             p0(a, 21, 3), p0(a, 19, 2), p0(a, 0, 12) + 1, b);
                    break;
                case gbi::OP_SETTILE:
                    snprintf(what, sizeof(what),
                             "SETTILE t%u fmt=%u siz=%u line=%u tmem=%u pal=%u cms=%u cmt=%u "
                             "masks=%u maskt=%u shifts=%u shiftt=%u",
                             p0(b, 24, 3), p0(a, 21, 3), p0(a, 19, 2), p0(a, 9, 9), p0(a, 0, 9),
                             p0(b, 20, 4), p0(b, 8, 2), p0(b, 18, 2), p0(b, 4, 4), p0(b, 14, 4),
                             p0(b, 0, 4), p0(b, 10, 4));
                    break;
                case gbi::OP_SETTILESIZE:
                    snprintf(what, sizeof(what),
                             "SETTILESIZE t%u uls=%u ult=%u lrs=%u lrt=%u (texels s%u..%u t%u..%u)",
                             p0(b, 24, 3), p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12),
                             p0(a, 12, 12) >> 2, p0(b, 12, 12) >> 2, p0(a, 0, 12) >> 2, p0(b, 0, 12) >> 2);
                    break;
                case gbi::OP_LOADTILE:
                    snprintf(what, sizeof(what), "LOADTILE t%u uls=%u ult=%u lrs=%u lrt=%u",
                             p0(b, 24, 3), p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12));
                    break;
                case gbi::OP_LOADBLOCK:
                    snprintf(what, sizeof(what), "LOADBLOCK t%u uls=%u ult=%u lrs=%u dxt=%u",
                             p0(b, 24, 3), p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12));
                    break;
                case gbi::OP_LOADTLUT:
                    snprintf(what, sizeof(what), "LOADTLUT t%u uls=%u ult=%u lrs=%u lrt=%u",
                             p0(b, 24, 3), p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12));
                    break;
                case gbi::OP_TEXTURE:
                    snprintf(what, sizeof(what), "TEXTURE tile=%u level=%u on=%u sc=%u tc=%u",
                             p0(a, 8, 3), p0(a, 11, 3), p0(a, 1, 7), p0(b, 16, 16), p0(b, 0, 16));
                    break;
                default:
                    break;
            }
            OGRE_MILESTONE("GFX-TDSTATE", "  [%u] @0x%05X op=0x%02X %s", k, c.offset, c.op, what);
        }
    }

    // ----- TMEM (gfx thread) --------------------------------------------------

    // RT64 `RDP::loadWord`: one 64-bit TMEM word from the 32-bit-granular
    // source. A TLUT word reads two source bytes, RGBA32 splits a source
    // word's low and high halves into TMEM's two halves, and every other
    // format copies eight bytes.
    void tmem_store_word(const LoadSource& source, uint32_t tmem_addr, uint32_t tmem_xor,
                         uint32_t tex_addr, bool rgba32, bool tlut) {
        const uint32_t offset_mask = tlut ? 0x1u : 0x7u;
        auto src = [&](uint32_t i) {
            return source.byte(tex_addr + (i & offset_mask));
        };
        if (rgba32) {
            tmem_[(tmem_addr + 0) ^ tmem_xor] = src(0);
            tmem_[(tmem_addr + 1) ^ tmem_xor] = src(1);
            tmem_[(tmem_addr + 2) ^ tmem_xor] = src(4);
            tmem_[(tmem_addr + 3) ^ tmem_xor] = src(5);
            tmem_[((tmem_addr + 0) ^ tmem_xor) | 0x800u] = src(2);
            tmem_[((tmem_addr + 1) ^ tmem_xor) | 0x800u] = src(3);
            tmem_[((tmem_addr + 2) ^ tmem_xor) | 0x800u] = src(6);
            tmem_[((tmem_addr + 3) ^ tmem_xor) | 0x800u] = src(7);
        } else {
            for (uint32_t i = 0; i < 8; ++i) {
                tmem_[(tmem_addr + i) ^ tmem_xor] = src(i);
            }
        }
    }

    // RT64 `RDP::loadToTMEMCommon`'s non-deferred arm (the GPU tile-copy path
    // is not modelled). `block`/`dxt_increment` reproduce a LOADBLOCK's early
    // row advance.
    void tmem_copy(const LoadSource& source, uint32_t tex_start, uint32_t tex_stride,
                   uint32_t tmem_start, uint32_t tmem_stride, uint32_t words_per_row,
                   uint32_t rows, bool rgba32, bool tlut, uint32_t dxt_increment, bool block) {
        const uint32_t tmem_mask = rgba32 ? 0x7FFu : 0xFFFu;
        const uint32_t tmem_advance = rgba32 ? 4u : 8u;
        const uint32_t texture_advance = tlut ? 2u : 8u;
        uint32_t tex_row = tex_start;
        uint32_t tmem_row = tmem_start & tmem_mask;
        uint32_t tmem_xor = 0;
        uint32_t dxt_counter = 0;
        for (uint32_t row = 0; row < rows; ++row) {
            uint32_t tex_addr = tex_row;
            uint32_t tmem_addr = tmem_row;
            for (uint32_t w = 0; w < words_per_row; ++w) {
                tmem_store_word(source, tmem_addr, tmem_xor, tex_addr, rgba32, tlut);
                // RT64 `loadWordStep`: a block advances a row once per 0x800
                // of `dxt`, then every word advances by the texel size.
                if (block) {
                    dxt_counter += dxt_increment;
                    while (dxt_counter >= 0x800u) {
                        tmem_addr = (tmem_addr + tmem_stride) & tmem_mask;
                        dxt_counter -= 0x800u;
                        tmem_xor ^= 4u;
                    }
                }
                tex_addr += texture_advance;
                tmem_addr = (tmem_addr + tmem_advance) & tmem_mask;
            }
            tmem_row = (tmem_row + tmem_stride) & tmem_mask;
            tex_row += tex_stride;
            tmem_xor ^= 4u;
        }
    }

    // RT64 `RDP::loadYUVTileToTMEM`: a YUV16 tile is de-interleaved into the
    // luma plane in TMEM's upper half and the U/V pairs in the lower half.
    void tmem_load_yuv(const LoadSource& source, uint32_t tex_start, uint32_t tex_stride,
                       uint32_t tmem_start, uint32_t tmem_stride, uint32_t rows) {
        const uint32_t words_per_row = tmem_stride / 2;
        for (uint32_t row = 0; row < rows; ++row) {
            const uint32_t row_base = (tmem_start + row * tmem_stride) & 0xFFFu;
            const uint32_t row_xor = ((row & 1) != 0) ? 4u : 0u;
            for (uint32_t w = 0; w < words_per_row; ++w) {
                const uint32_t word = tex_start + row * tex_stride + w * 4;
                const uint8_t y_even = source.byte(word + 2);
                const uint8_t y_odd = source.byte(word + 0);
                const uint8_t uu = source.byte(word + 3);
                const uint8_t vv = source.byte(word + 1);
                const uint32_t luma = (((row_base + w * 2) ^ row_xor) | 0x800u) & 0xFFFu;
                const uint32_t chroma = ((row_base + w * 2) ^ row_xor) & 0xFFFu;
                tmem_[luma + 0] = y_even;
                tmem_[luma + 1] = y_odd;
                tmem_[chroma + 0] = uu;
                tmem_[(chroma + 1) & 0xFFFu] = vv;
            }
        }
    }

    // Runs one of G_LOADTILE / G_LOADBLOCK / G_LOADTLUT into TMEM from the
    // command's own rect and the last G_SETTIMG/G_SETTILE. RT64
    // `RDP::loadTile`/`loadBlock`/`loadTLUT` are the reference; the load
    // command's rect replaces the tile's, which the previous model did not do
    // (it decoded at draw time from the render tile's rect instead).
    void tmem_load_command(ExecCtx* ctx, RenderState& st, bool block, bool tlut_load,
                           uint32_t w0, uint32_t w1) {
        const int tile = static_cast<int>(p0(w1, 24, 3));
        TileState& lt = st.tiles[tile & (kMaxTiles - 1)];
        const uint16_t uls = static_cast<uint16_t>(p0(w0, 12, 12));
        const uint16_t ult = static_cast<uint16_t>(p0(w0, 0, 12));
        const uint16_t lrs = static_cast<uint16_t>(p0(w1, 12, 12));
        const uint16_t lrt = static_cast<uint16_t>(p0(w1, 0, 12));
        lt.uls = uls;
        lt.ult = ult;
        lt.lrs = lrs;
        lt.lrt = lrt;
        const uint32_t siz = lt.siz;
        const bool rgba32 = (lt.fmt == gbi::IM_FMT_RGBA) && (siz == gbi::IM_SIZ_32b);
        const uint32_t bytes_per_row = static_cast<uint32_t>(st.timg_width) << siz >> 1;
        const uint32_t tmem_start = static_cast<uint32_t>(lt.tmem) << 3;
        if (uls > lrs) {
            return;   // the hardware ignores a reversed load rect
        }
        // The load's own rect in texels: what it copies, and therefore the byte
        // range whose source may be a colour image the RDP has drawn.
        const int load_x0 = static_cast<int>(uls >> 2);
        const int load_w = static_cast<int>((lrs >> 2) - (uls >> 2)) + 1;
        const int load_y0 = block ? static_cast<int>(ult) : static_cast<int>(ult >> 2);
        const int load_rows =
            block ? 1 : static_cast<int>(1 + ((lrt >> 2) - (ult >> 2)));
        // Framebuffer-as-texture. The copy below reads RDRAM, and the pixels of a
        // colour image the RDP has drawn are still in a GL framebuffer object
        // until the main thread writes them back. Detect the overlap first, make
        // the main thread execute the draws recorded so far and write the
        // image's drawn rectangles into RDRAM, and only then copy. RT64 makes a
        // GPU tile copy instead (`RDP::checkFramebufferOverlap`); this renderer
        // has one source of truth, so the load reads RDRAM in both cases.
        LoadSource source;
        source.rdram = rdram_;
        uint32_t overlap_image = 0;
        const bool overlaps =
            fbtex_enabled_ &&
            (g_debug_flags.load(std::memory_order_relaxed) & 8192u) == 0u &&
            note_fb_texture(st.timg_address, static_cast<int>(bytes_per_row), load_y0,
                            load_rows, load_w, load_rows, load_x0, st.timg_fmt, st.timg_siz,
                            &overlap_image, false);
        if (overlaps) {
            // RT64 makes a GPU tile copy here. This renderer asks the main
            // thread for a private copy of the image's rendered pixels and reads
            // the load out of that, so RDRAM is never written mid-display-list.
            if (fetch_fb_source(ctx, overlap_image)) {
                source.shadow = fb_source_bytes_.data();
                source.shadow_origin = overlap_image;
                source.shadow_len = static_cast<uint32_t>(fb_source_bytes_.size());
            } else {
                // No copy: the load reads RDRAM, the pre-session path. Count
                // and log the overlap so a load the copy was meant to serve is
                // visible.
                note_fb_texture(st.timg_address, static_cast<int>(bytes_per_row), load_y0,
                                load_rows, load_w, load_rows, load_x0, st.timg_fmt,
                                st.timg_siz, &overlap_image, true);
            }
        }
        if (block) {
            // RT64 `loadBlockOperation`: the block is contiguous, and `ult`,
            // `dxt` are in whole words/texels rather than 10.2 fixed.
            const uint32_t tex_start = st.timg_address + (static_cast<uint32_t>(uls) << siz >> 1) +
                                       bytes_per_row * ult;
            const uint32_t word_count = ((static_cast<uint32_t>(lrs) - uls) >> (4 - siz)) + 1;
            if (!rgba32 && lt.fmt == gbi::IM_FMT_YUV && siz == gbi::IM_SIZ_16b) {
                tmem_load_yuv(source, tex_start, bytes_per_row, tmem_start,
                              static_cast<uint32_t>(lt.line) << 3, 1);
            } else {
                tmem_copy(source, tex_start, bytes_per_row, tmem_start,
                          static_cast<uint32_t>(lt.line) << 3, word_count, 1, rgba32, false, lrt,
                          true);
            }
        } else if (tlut_load) {
            const uint32_t tex_start =
                st.timg_address + ((static_cast<uint32_t>(uls) >> 2) << siz >> 1) +
                bytes_per_row * (ult >> 2);
            const uint32_t rows = 1 + ((lrt >> 2) - (ult >> 2));
            const uint32_t words_per_row = ((static_cast<uint32_t>(lrs) >> 2) - (uls >> 2)) + 1;
            tmem_copy(source, tex_start, bytes_per_row, tmem_start,
                      static_cast<uint32_t>(lt.line) << 5, words_per_row, rows, rgba32, true, 0,
                      false);
        } else {
            const uint32_t tex_start =
                st.timg_address + ((static_cast<uint32_t>(uls) >> 2) << siz >> 1) +
                bytes_per_row * (ult >> 2);
            const uint32_t rows = 1 + ((lrt >> 2) - (ult >> 2));
            const uint32_t words_per_row =
                (((static_cast<uint32_t>(lrs) >> 2) - (uls >> 2)) >> (4 - siz)) + 1;
            if (!rgba32 && lt.fmt == gbi::IM_FMT_YUV && siz == gbi::IM_SIZ_16b) {
                tmem_load_yuv(source, tex_start, bytes_per_row, tmem_start,
                              static_cast<uint32_t>(lt.line) << 3, rows);
            } else {
                tmem_copy(source, tex_start, bytes_per_row, tmem_start,
                          static_cast<uint32_t>(lt.line) << 3, words_per_row, rows, rgba32, false,
                          0, false);
            }
        }
        ++tmem_generation_;
    }

    // Records a LOADTILE/LOADBLOCK as the source for the *next* render tile.
    //
    // G_LOADTILE/LOADBLOCK always name tile 7 (G_TX_LOADTILE) and the F3DEX2
    // texture-load idiom immediately re-configures the render tile afterwards
    // with G_SETTILE (+ G_SETTILESIZE), so whichever tile G_SETTILE names next is
    // the one that samples this image. The load's own format/rect is NOT what to
    // decode with: the RDP samples TMEM through the render tile, so OB64's mask
    // (16 bytes/row of 8-bit texels, declared as a 4-bit 32x34 tile) must be
    // decoded as 32 4-bit texels per row, not 16 8-bit ones.
    void note_tile_load(RenderState& st, int tile) {
        const int stride = static_cast<int>(st.timg_width) << st.timg_siz >> 1;
        st.pending_load.address = st.timg_address;
        st.pending_load.stride = stride;
        st.pending_load.load_tile = tile;
        st.pending_load.valid = true;
        // Also attach the image to the load tile itself. The SDK idiom loads
        // into G_TX_LOADTILE (7) and then re-configures the render tile, but a
        // display list may load straight into the tile it samples (the
        // synthetic test_draw does), and then no later G_SETTILE claims it.
        set_tile_image(st, tile, st.timg_address, stride);
    }

    // Claims the pending load for `tile` (called from G_SETTILE): the render
    // tile the texture-load idiom configures after the load is the one that
    // samples the image.
    static void claim_tile_load(RenderState& st, int tile) {
        if (!st.pending_load.valid || tile == st.pending_load.load_tile) {
            return;
        }
        set_tile_image(st, tile, st.pending_load.address, st.pending_load.stride);
        st.pending_load.valid = false;
    }

    static void set_tile_image(RenderState& st, int tile, uint32_t address, int stride) {
        TileState& t = st.tiles[tile & (kMaxTiles - 1)];
        t.image_address = address;
        t.image_stride = stride;
        t.image_source_valid = true;
        t.image_valid = false;   // the sampled rect is not known until G_SETTILESIZE
    }

    // Hashes `len` bytes starting at `base`, wrapping at TMEM's 4 KiB boundary.
    // `or_address` selects the upper half for formats that read both halves.
    static uint64_t hash_tmem_bytes(const uint8_t* tmem, uint32_t base, uint32_t len,
                                    uint32_t mask, uint32_t or_address, uint64_t seed) {
        base &= mask;
        while (len > 0) {
            const uint32_t part = std::min<uint32_t>(len, (mask + 1) - base);
            seed = XXH3_64bits_withSeed(tmem + (base | or_address), part, seed);
            len -= part;
            base = 0;
        }
        return seed;
    }

    // The bytes decode_tmem_tile can read for this tile, hashed before the
    // decode. A tile reads `line << 3` bytes per row and `row_bytes` bytes
    // inside one row; when the row stride is wider than the sampled row, each
    // row is hashed on its own so an unrelated load elsewhere in TMEM does not
    // invalidate this tile. The fields that decide the addressing are mixed in
    // after the bytes; the GL wrap fields are not decode inputs and stay out.
    uint64_t tile_source_key(const RenderState& st, const TileState& t, int w, int h,
                             uint32_t lut_mode) {
        const bool rgba32 = (t.fmt == gbi::IM_FMT_RGBA) && (t.siz == gbi::IM_SIZ_32b);
        const bool indexed = (t.fmt == gbi::IM_FMT_CI) && (lut_mode != 0);
        const bool yuv16 = (t.fmt == gbi::IM_FMT_YUV) && (t.siz == gbi::IM_SIZ_16b);
        // Must match decode_tmem_tile's own mask. A CI tile is masked to the
        // lower half there, so hashing with 0xFFF would read different bytes
        // from an address at or above 0x800 and could serve a stale decode.
        const uint32_t mask = (rgba32 || indexed) ? 0x7FFu : 0xFFFu;
        const uint32_t address = static_cast<uint32_t>(t.tmem) << 3;
        const uint32_t stride = static_cast<uint32_t>(t.line) << 3;
        // The decoder's own byte shift; I32 is 3 (4 bytes per texel), and
        // RGBA32 is forced to 2 because its four channels live in two halves.
        const uint32_t shift = rgba32 ? 2u : t.siz;
        // Worst-case byte offset decode_tmem_tile can touch in one row, plus
        // the 4-byte slack the odd-row word swap and the YUV chroma pair need.
        uint32_t row_bytes =
            ((static_cast<uint32_t>(w) - 1) << shift >> 1) + 1u + (shift >= 2 ? 1u : 0u);
        row_bytes = std::min<uint32_t>(row_bytes + 4u, 4096u);
        const bool both_halves = (t.siz == gbi::IM_SIZ_32b) || yuv16;

        uint64_t key = 0x9E3779B97F4A7C15ull;
        const uint32_t rows = static_cast<uint32_t>(h);
        const uint32_t region = mask + 1;
        const uint32_t total = stride * (rows - 1) + row_bytes;
        if (stride >= region || total >= region) {
            // The tile's own footprint already spans TMEM, so hashing the whole
            // 4 KiB changes nothing but avoids a needless row loop.
            key = hash_tmem_bytes(tmem_, 0, 4096, 0xFFF, 0, key);
        } else if (stride <= row_bytes) {
            // Rows are packed; one contiguous span covers every sampled byte.
            key = hash_tmem_bytes(tmem_, address, total, mask, 0, key);
            if (both_halves) {
                key = hash_tmem_bytes(tmem_, address, total, mask, 0x800u, key);
            }
        } else {
            for (uint32_t y = 0; y < rows; ++y) {
                const uint32_t at = address + y * stride;
                key = hash_tmem_bytes(tmem_, at, row_bytes, mask, 0, key);
                if (both_halves) {
                    key = hash_tmem_bytes(tmem_, at, row_bytes, mask, 0x800u, key);
                }
            }
        }
        // A CI tile's colours come from the palette in TMEM's upper half. Hash
        // the whole half; it is 2 KiB and changes only on a TLUT load.
        if (indexed) {
            key = hash_tmem_bytes(tmem_, 0x800u, 0x800u, 0xFFFu, 0, key);
        }

        auto mix = [&key](uint64_t v) {
            key = (key ^ v) * 0x00000100000001B3ull;
        };
        mix(t.tmem);
        mix(t.line);
        mix(t.fmt);
        mix(t.siz);
        mix(t.palette);
        mix(static_cast<uint64_t>(w));
        mix(static_cast<uint64_t>(h));
        mix(lut_mode);
        if (yuv16) {
            for (int i = 0; i < 6; ++i) {
                mix(static_cast<uint32_t>(st.convert_k[i]));
            }
        }
        return key;
    }

    // Stores a decode and evicts least-recently-used entries over the byte cap.
    // The upload queue holds its own shared reference, so an evicted entry's
    // pixels stay alive until the batch that named them has been drawn.
    void cache_decoded_tile(uint64_t source_key,
                            std::shared_ptr<const std::vector<uint8_t>> pixels, int w, int h) {
        auto [it, inserted] = decoded_tiles_.try_emplace(source_key);
        DecodedTile& e = it->second;
        decoded_bytes_ -= e.bytes;
        e.pixels = std::move(pixels);
        e.width = w;
        e.height = h;
        e.last_used = ++decode_epoch_;
        e.bytes = e.pixels->size();
        decoded_bytes_ += e.bytes;
        while (decoded_bytes_ > kDecodeCacheBytes && decoded_tiles_.size() > 1) {
            auto victim = decoded_tiles_.end();
            for (auto c = decoded_tiles_.begin(); c != decoded_tiles_.end(); ++c) {
                if (c->first == source_key) {
                    continue;
                }
                if (victim == decoded_tiles_.end() ||
                    c->second.last_used < victim->second.last_used) {
                    victim = c;
                }
            }
            if (victim == decoded_tiles_.end()) {
                break;
            }
            decoded_bytes_ -= victim->second.bytes;
            decoded_tiles_.erase(victim);
        }
    }

    // Decodes (and uploads, once per key) the image a tile samples, using the
    // tile's own format/size, its rect, and its `line` as the row stride.
    // Replaces "decode the load rect with the load's format", which could not
    // express the mask above and used the wrong stride whenever they differ.
    void ensure_tile_image(ExecCtx* ctx, RenderState& st, int tile, int unit = 0) {
        TileState& t = st.tiles[tile & (kMaxTiles - 1)];
        // RT64 `State::setupDrawCall`: a tile with `line == 0` samples nothing
        // (the RDP's row stride is the tile's own field). The image itself is
        // whatever the last load put in TMEM; that persists across display
        // lists the way RT64's `RDP::TMEM` does, so a draw does not need a load
        // in its own list.
        if (t.line == 0) {
            t.image_valid = false;
            return;
        }
        const int x0 = t.uls >> 2;
        const int y0 = t.ult >> 2;
        // RT64's sample window: the tile's own rect when the axis is clamped,
        // otherwise the mask, whichever is smaller. A texrect whose tile was
        // never given a rect (uls/lrt all zero) therefore samples the mask's
        // size - the title's 64x64 cloud tiles - instead of a single texel.
        const bool clamp_s = (t.masks == 0) || ((t.cms & 2) != 0);
        const bool clamp_t = (t.maskt == 0) || ((t.cmt & 2) != 0);
        // The tile's own rect in texels, whatever the clamp mode. It is the
        // bound `OGRE_TEXWINCLAMP=1` applies to a wrapped sample.
        const int32_t rect_w = std::max((static_cast<int32_t>(t.lrs) -
                                         static_cast<int32_t>(t.uls) + 4) / 4, 1);
        const int32_t rect_h = std::max((static_cast<int32_t>(t.lrt) -
                                         static_cast<int32_t>(t.ult) + 4) / 4, 1);
        const int32_t window_w = clamp_s ? rect_w : 0xFFFF;
        const int32_t window_h = clamp_t ? rect_h : 0xFFFF;
        const int32_t mask_w = (t.masks > 0) ? (1 << t.masks) : 0xFFFF;
        const int32_t mask_h = (t.maskt > 0) ? (1 << t.maskt) : 0xFFFF;
        const int w = std::min(window_w, mask_w);
        const int h = std::min(window_h, mask_h);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            t.image_valid = false;
            return;
        }
        // OGRE_TEXWINCLAMP: the bound the shader's sampler applies to a wrapped
        // sample. The tile's own rect is what the game authored, and a band
        // drawn from it samples one row/column past the rect at its far edge
        // (`w * dsdx / 1024` texels over `w` pixels), where the wrap mask
        // instead reads whatever TMEM holds there. The mission intro's picture
        // strips are the A/B (session 194): the texels past the band are a
        // different image's rows, and their alpha turns the band's transparent
        // green into visible hairlines. Zero means "no bound" (the default).
        t.image.win_w = tex_win_clamp_ ? static_cast<int>(std::min<int32_t>(rect_w, w)) : 0;
        t.image.win_h = tex_win_clamp_ ? static_cast<int>(std::min<int32_t>(rect_h, h)) : 0;
        // The RDRAM source the diagnostic below reports (the load's own stride
        // is the SETTIMG width, which is not what the tile samples from TMEM).
        const int row_bytes = t.image_stride;
        // The source key covers the bytes decode_tmem_tile reads and the fields
        // that decide which bytes those are. It is computed before decoding, so
        // a display list that reloads the same bytes can reuse the previous
        // decode. The GL content key is still computed from the decoded pixels
        // below, so the texture the draw samples is unchanged.
        const uint32_t lut_mode = (st.othermode_h >> 14) & 3u;
        const uint64_t source_key = tile_source_key(st, t, w, h, lut_mode);
        if (t.image_valid && t.image.decode_key == source_key) {
            return;   // already decoded and queued for this draw
        }

        g_load_seq.fetch_add(1, std::memory_order_relaxed);
        g_load_w.store(static_cast<uint32_t>(w), std::memory_order_relaxed);
        g_load_h.store(static_cast<uint32_t>(h), std::memory_order_relaxed);
        g_load_siz.store(t.siz, std::memory_order_relaxed);
        g_load_fmt.store(t.fmt, std::memory_order_relaxed);
        g_load_timgw.store(static_cast<uint32_t>(row_bytes), std::memory_order_relaxed);
        g_load_addr.store(t.image_address, std::memory_order_relaxed);

        // Reuse the decode when the source key matches; otherwise decode TMEM
        // and cache the result. `pixels` is shared from here on, so a cache hit
        // costs one XXH3 pass over the decoded bytes (for the content key) and
        // no copy. The decoded-tile hash is the load's own marker in the
        // `[GFX-TEXPROBE]` control; `g_load_done` stays a decode counter.
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        auto cached = decoded_tiles_.find(source_key);
        const bool decode_hit = (cached != decoded_tiles_.end());
        if (decode_hit) {
            DecodedTile& e = cached->second;
            e.last_used = ++decode_epoch_;
            pixels = e.pixels;
            ++tex_decode_hits_;
            // Trace-only audit of the source key: re-decode the same tile and
            // compare. A mismatch means the key omitted an input, and the fresh
            // decode replaces the cached one so the frame is still correct.
            if (trace_active()) {
                std::vector<uint8_t> scratch;
                decode_tmem_tile(tmem_, t, w, h, static_cast<int>(lut_mode), st.convert_k,
                                 scratch);
                if (scratch.size() != pixels->size() ||
                    std::memcmp(scratch.data(), pixels->data(), scratch.size()) != 0) {
                    const unsigned mismatches =
                        texcache_mismatches_.load(std::memory_order_relaxed);
                    if (mismatches < 8) {
                        OGRE_MILESTONE("GFX-TEXCACHE-MISMATCH",
                                       "key=0x%llX %dx%d fmt=%d siz=%d line=%d tmem=%u "
                                       "cached=%zuB fresh=%zuB",
                                       (unsigned long long)source_key, w, h, (int)t.fmt,
                                       (int)t.siz, (int)t.line, (unsigned)t.tmem,
                                       pixels->size(), scratch.size());
                    }
                    texcache_mismatches_.fetch_add(1, std::memory_order_relaxed);
                    pixels = std::make_shared<const std::vector<uint8_t>>(std::move(scratch));
                    decoded_bytes_ -= e.bytes;
                    e.pixels = pixels;
                    e.bytes = pixels->size();
                    decoded_bytes_ += e.bytes;
                }
            }
        } else {
            auto decoded = std::make_shared<std::vector<uint8_t>>();
            decode_tmem_tile(tmem_, t, w, h, static_cast<int>(lut_mode), st.convert_k, *decoded);
            pixels = decoded;
            ++tex_decodes_;
        }
        g_load_done.fetch_add(1, std::memory_order_relaxed);

        // The GL texture is exactly its decoded RGBA8 bytes, its size, and the
        // wrap modes set on the texture object at creation. Keying on those
        // makes an unchanged reload of the same image reuse the resident GL
        // texture, which is the session-188 open item: the old key started from
        // `tmem_generation_`, so every display list produced a new key even
        // when the bytes were identical. Palette and YUV differences are in the
        // decoded bytes; the per-draw tile origin and filter are uniforms, not
        // texture state, so they stay out.
        uint64_t content_key = XXH3_64bits(pixels->data(), pixels->size());
        auto mix_content = [&content_key](uint64_t v) {
            content_key = (content_key ^ v) * 0x00000100000001B3ull;
        };
        mix_content(static_cast<uint64_t>(w));
        mix_content(static_cast<uint64_t>(h));
        mix_content(t.cms);
        mix_content(t.cmt);
        mix_content(t.masks);
        mix_content(t.maskt);
        if (content_key == 0) {
            content_key = 1;   // 0 means "no texture" to lookup() and the sweep
        }
        if (!decode_hit) {
            cache_decoded_tile(source_key, pixels, w, h);
        }
        // OGRE_TEX_TRACE: the colour the decoded image holds. The native
        // renderer prints the same number from the RDRAM bytes through the
        // palette (`OGRE_TEX_TRACE_COLOUR`), so two runs of one screen can be
        // compared without a screenshot. `opaque` counts the texels whose alpha
        // is high enough to survive RT64's RGBA16 coverage test, which is how a
        // window of transparent texels is told apart from a black one.
        if (trace_active()) {
            uint64_t sum = 0;
            unsigned opaque = 0;
            const size_t count = pixels->size() / 4;
            for (size_t i = 0; i < count; ++i) {
                sum += (*pixels)[i * 4 + 0] + (*pixels)[i * 4 + 1] + (*pixels)[i * 4 + 2];
                if ((*pixels)[i * 4 + 3] >= 128) {
                    ++opaque;
                }
            }
            t.trace_mean = count ? static_cast<float>(double(sum) / double(count) / 3.0) : 0.0f;
            t.trace_opaque = opaque;
        }
        // The decoded colour histogram of the first few loads. A draw whose
        // mean is right but whose letters are the wrong colour differs from one
        // whose whole window is wrong, and only the histogram separates them.
        // Cleared when this call does not compute one, so a later draw never
        // reports an earlier decode's histogram.
        t.image.hist.clear();
        if (trace_active() && trace_draws_ < 8) {
            std::map<uint32_t, uint32_t> colhist;
            for (size_t i = 0; i + 3 < pixels->size(); i += 4) {
                const uint32_t c = (uint32_t((*pixels)[i]) << 16) |
                                   (uint32_t((*pixels)[i + 1]) << 8) |
                                   uint32_t((*pixels)[i + 2]);
                ++colhist[c];
            }
            for (const auto& kv : colhist) {
                t.image.hist.emplace_back(kv.first, kv.second);
            }
            // The palette entries this decode actually read, so the histogram
            // can be reproduced without a second dump.
            OGRE_MILESTONE("GFX-TEXTRACE",
                           "  decode tile=%d img=0x%06X rect=%d,%d..%d,%d siz=%d tmem=%u "
                           "gen=%llu e0=%04X e113=%04X e230=%04X nonzero=%u",
                           tile, t.image_address, x0, y0, x0 + w, y0 + h, (int)t.siz,
                           (unsigned)t.tmem, (unsigned long long)tlut_generation_,
                           (unsigned)tlut_[0], (unsigned)tlut_[113], (unsigned)tlut_[230],
                           tlut_nonzero_);
        }

        // Debug copy for ogre_gfx_debug_tex (probes inspect what was sampled).
        {
            std::lock_guard<std::mutex> lock(debug_mutex_);
            DebugTex dbg;
            dbg.key = content_key;
            dbg.width = w;
            dbg.height = h;
            dbg.tile = tile;
            dbg.fmt = t.fmt;
            dbg.siz = t.siz;
            dbg.line = t.line;
            dbg.pixels = *pixels;
            // Keep a whole frame's loads (12 sprites x mask+colour) so a probe
            // can compare the first rendered sprite with its own texture pair.
            if (debug_tex_.size() >= 24) {
                debug_tex_.erase(debug_tex_.begin());
            }
            debug_tex_.push_back(dbg);
            debug_last_[unit & 1] = std::move(dbg);
        }

        // One upload per key per display list: a sprite's two triangles share it.
        if (ctx->uploaded.count(content_key) == 0) {
            ctx->uploaded.insert(content_key);
            TexUpload up;
            up.key = content_key;
            up.width = w;
            up.height = h;
            up.tile = tile;
            up.clamp_s = (t.cms != 0) || (t.masks == 0);
            up.clamp_t = (t.cmt != 0) || (t.maskt == 0);
            up.bilerp = ((st.othermode_h >> 12) & 3) == 2;  // G_TF_BILERP
            up.decode_hit = decode_hit;
            up.pixels = pixels;
            ctx->tex.push_back(std::move(up));
        }

        // Diagnostics: remember the decoded texel (0,0) and the tile's addressing
        // so the texrect path can report what a dsdx=dtdy=0 rect samples.
        if (pixels->size() >= 4) {
            last_tex_rgba_[0] = (*pixels)[0];
            last_tex_rgba_[1] = (*pixels)[1];
            last_tex_rgba_[2] = (*pixels)[2];
            last_tex_rgba_[3] = (*pixels)[3];
        }
        last_tex_fmt_ = t.fmt;
        last_tex_siz_ = t.siz;
        last_tex_line_ = t.line;
        last_tex_shifts_ = t.shifts;
        last_tex_shiftt_ = t.shiftt;
        last_tex_masks_ = t.masks;
        last_tex_maskt_ = t.maskt;

        t.image.key = content_key;
        t.image.decode_key = source_key;
        t.image.width = w;
        t.image.height = h;
        t.image.origin_s = x0;
        t.image.origin_t = y0;
        t.image_valid = true;
    }


    // ----- drawing (gfx pthread: record commands) --------------------------------

    // Bounded diagnostic for the 640-wide colour images: the title screen's
    // hi-res mode and the world map's three heap targets render into them. The
    // first 64 draws of the first task that targets one are logged with their
    // geometry, because a hi-res frame that comes out as flat horizontal spans
    // is a different fault from one whose texture sampling is wrong.
    void log_wide_draw(const RenderState& st, const char* what, int x0, int y0, int x1,
                       int y1, const char* detail) {
        if (st.cimg_address == 0 || st.cimg_width < 640) {
            return;
        }
        if (wide_task_ == 0) {
            wide_task_ = task_count_.load();
        }
        if (task_count_.load() != wide_task_ || wide_logged_ >= 64) {
            return;
        }
        ++wide_logged_;
        OGRE_MILESTONE("GFX-WIDE",
                       "%s (%d,%d)-(%d,%d) fb=0x%06X cimg=%u target=%dx%d siz=%u "
                       "scissor=%d(%d,%d,%d,%d) %s",
                       what, x0, y0, x1, y1, st.cimg_address, (unsigned)st.cimg_width,
                       target_width_px(st), target_height_px(st), (unsigned)st.cimg_siz,
                       st.scissor_enabled ? 1 : 0, st.scissor_x0, st.scissor_y0,
                       st.scissor_x1, st.scissor_y1, detail != nullptr ? detail : "");
    }

    // Fills the render-state fields of a DrawCmd from the current RDP state.
    void record_state(DrawCmd& cmd, RenderState& st, bool textured) {
        const Combiner& cb = st.combiner;
        // The blender's inputs live in OTHERMODE_L; decode them here so the
        // DrawCmd always carries the mode that is current at draw time.
        st.blender.decode(st.othermode_l, st.othermode_h);
        // Which mux the hardware evaluates first. RT64 ColorCombiner::run()
        // calls runCycle(inputs, twoCycle ? 0 : 1, twoCycle, ...): a 1-cycle
        // draw evaluates the SECOND mux (with COMBINED = 0), a 2-cycle draw
        // evaluates mux 0 and then mux 1. Slot 0 is always the first mux.
        const uint32_t cyc_type = (st.othermode_h >> 20) & 3;
        const int mux = (cyc_type == kCyc2) ? 0 : 1;
        for (int i = 0; i < 2; ++i) {
            const int src = (i == 0) ? mux : 1;
            cmd.ca[i] = cb.ca[src];
            cmd.cb[i] = cb.cb[src];
            cmd.cc[i] = cb.cc[src];
            cmd.cd[i] = cb.cd[src];
            cmd.aa[i] = cb.aa[src];
            cmd.ab[i] = cb.ab[src];
            cmd.ac[i] = cb.ac[src];
            cmd.ad[i] = cb.ad[src];
        }
        // COPY mode bypasses the combiner (shader u_cycle == 0).
        cmd.cycle = (cyc_type == kCycCopy) ? 0 : ((cyc_type == kCyc2) ? 2 : 1);
        cmd.othermode_h = st.othermode_h;
        cmd.othermode_l = st.othermode_l;
        cmd.comb_L = cb.L;
        cmd.comb_H = cb.H;
        for (int i = 0; i < 4; ++i) {
            cmd.prim[i] = st.prim_color[i];
            cmd.env[i] = st.env_color[i];
        }
        cmd.textured = textured;
        cmd.tex_sc = st.tex_sc;
        cmd.tex_tc = st.tex_tc;
        cmd.k4 = static_cast<float>(st.convert_k[4] & 0x1FF) / 255.0f;
        cmd.k5 = static_cast<float>(st.convert_k[5] & 0x1FF) / 255.0f;
        for (int i = 0; i < 3; ++i) {
            cmd.key_center[i] = st.key_center[i];
            cmd.key_scale[i] = st.key_scale[i];
        }
        if (textured) {
            // TEXEL0 = the G_TEXTURE tile, TEXEL1 = the tile above it; each
            // carries the image decoded from its own source and rect.
            const int t0 = st.active_tile & (kMaxTiles - 1);
            const int t1 = (st.active_tile + 1) & (kMaxTiles - 1);
            const LoadedImage& i0 = st.tiles[t0].image;
            const LoadedImage& i1 = st.tiles[t1].image;
            cmd.tex_key = i0.key;
            cmd.tex_key1 = i1.key;
            cmd.tex_scale[0] = i0.width ? 1.0f / static_cast<float>(i0.width) : 1.0f;
            cmd.tex_scale[1] = i0.height ? 1.0f / static_cast<float>(i0.height) : 1.0f;
            cmd.tex_origin[0] = static_cast<float>(i0.origin_s);
            cmd.tex_origin[1] = static_cast<float>(i0.origin_t);
            cmd.tex_scale1[0] = i1.width ? 1.0f / static_cast<float>(i1.width) : 1.0f;
            cmd.tex_scale1[1] = i1.height ? 1.0f / static_cast<float>(i1.height) : 1.0f;
            cmd.tex_origin1[0] = static_cast<float>(i1.origin_s);
            cmd.tex_origin1[1] = static_cast<float>(i1.origin_t);
            // The sampler's tile fields, the way RT64's State::setupDrawCall
            // builds RDPTile from the RDP's tile: `masks` becomes the wrap size
            // `1 << masks`, and a tile with no mask is clamped (the RDP treats
            // mask 0 as clamp). cms/cmt keep the G_TX_* bits.
            for (int u = 0; u < 2; ++u) {
                const TileState& ts = st.tiles[u == 0 ? t0 : t1];
                const LoadedImage& img = u == 0 ? i0 : i1;
                cmd.tex_size[u][0] = static_cast<float>(std::max(img.width, 1));
                cmd.tex_size[u][1] = static_cast<float>(std::max(img.height, 1));
                cmd.tex_cms[u] = ts.cms | ((ts.masks == 0) ? 2 : 0);   // G_TX_CLAMP
                cmd.tex_cmt[u] = ts.cmt | ((ts.maskt == 0) ? 2 : 0);
                cmd.tex_masks[u] = (ts.masks > 0) ? (1 << ts.masks) : 0;
                cmd.tex_maskt[u] = (ts.maskt > 0) ? (1 << ts.maskt) : 0;
                cmd.tex_win[u][0] = img.win_w;
                cmd.tex_win[u][1] = img.win_h;
                cmd.tex_shifts[u] = ts.shifts;
                cmd.tex_shiftt[u] = ts.shiftt;
                cmd.tex_shift[u][0] = tex_shift_scale(ts.shifts);
                cmd.tex_shift[u][1] = tex_shift_scale(ts.shiftt);
            }
            cmd.tex_filter = static_cast<int>((st.othermode_h >> 12) & 3);   // G_TF_*
        }
        // G_TP_PERSP applies to triangles only; RT64's rect paths skip the fixup
        // (`renderFlagRect` in TextureSampler.hlsli:221-223). The bit is
        // OTHERMODE_H bit 19 (`G_MDSFT_TEXTPERSP`,
        // shared/rt64_f3d_defines.h:51); bit 10 is not an RDP field, and the
        // game sets it on rect draws.
        cmd.tex_persp = (cmd.kind != 0) || ((st.othermode_h >> 19) & 1u) != 0;
        cmd.scissor_on = st.scissor_enabled;
        if (st.scissor_enabled) {
            cmd.sx0 = st.scissor_x0;
            cmd.sy0 = st.scissor_y0;
            cmd.sx1 = st.scissor_x1;
            cmd.sy1 = st.scissor_y1;
        }
        // Blender / render mode. RT64 decodes OTHERMODE_L's blender inputs and
        // asks whether the blender reads the framebuffer (Blender::usesAlphaBlend);
        // that - not a "does oml look like XLU" guess - is what decides whether
        // the GL blend is enabled and what alpha it uses.
        const Blender& bl = st.blender;
        cmd.combiner_cycles = bl.cycles;
        cmd.force_blend = bl.force_blend;
        cmd.blend_approx = bl.approx;
        const bool copy_or_fill = (cyc_type == kCycCopy || cyc_type == kCycFill);
        cmd.alpha_blend = !copy_or_fill && bl.alpha_blend &&
                          (g_debug_flags.load(std::memory_order_relaxed) & 1u) == 0;
        // RT64 RasterPS's alpha compare and coverage, from OTHERMODE_L:
        // alphaCompare is bits 0-1, CVG_DST bits 8-9 and CVG_X_ALPHA bit 12.
        cmd.acmp = static_cast<int>(st.othermode_l & 3u);
        cmd.cvg_dst = static_cast<int>((st.othermode_l >> 8) & 3u);
        cmd.cvg_x_alpha = (st.othermode_l & 0x1000u) != 0;
        for (int i = 0; i < 2; ++i) {
            cmd.bp[i] = bl.p[i];
            cmd.bm[i] = bl.m[i];
            cmd.ba[i] = bl.a[i];
            cmd.bb[i] = bl.b[i];
        }
        for (int i = 0; i < 4; ++i) {
            cmd.fog[i] = st.fog_color[i];
            cmd.blend_color[i] = st.blend_color[i];
        }
        cmd.fb_address = st.cimg_address;
        cmd.fb_row_bytes = st.cimg_row_bytes;
        cmd.fb_siz = st.cimg_siz;
        cmd.fb_fmt = st.cimg_fmt;
        cmd.fb_width_px = st.cimg_width;
        cmd.zimg_address = st.zimg_address;
        cmd.zimg_width = st.zimg_width;
        cmd.zimg_siz = st.zimg_siz;
        cmd.zimg_fmt = st.zimg_fmt;
        // RDP depth pipeline (RT64 OtherMode::zCmp/zUpd/zMode/zSource and
        // rasterShader's pipeline flags). COPY bypasses depth entirely. A
        // ZMODE_DEC draw never runs the hardware depth *test* in RT64 - the
        // decal tolerance is a fragment-shader test this renderer does not
        // implement, so only its depth *write* is kept.
        const uint32_t oml = st.othermode_l;
        const bool copy_mode = (cyc_type == kCycCopy);
        cmd.z_mode = static_cast<int>(oml & 0xC00u);
        // `depth_enabled_` (OGRE_DEPTH) gates the test and the write, not the
        // decode: with it off the DrawCmd still carries the exact state RT64
        // would act on, and `[GFX-DEPTH]` still counts it. The mission scene
        // measures worse with the test on (see the handoff), so the pipeline
        // ships off until the vertex depth is understood. The synthetic control
        // (debug flag bit 7) forces it on for its own draws.
        const bool depth_on = depth_enabled_ ||
                              (g_debug_flags.load(std::memory_order_relaxed) & 128u) != 0;
        cmd.depth_test = depth_on && !copy_mode && (oml & 0x10u) != 0 && cmd.z_mode != 0xC00;
        cmd.depth_write = depth_on && !copy_mode && (oml & 0x20u) != 0;
        cmd.z_source_prim = !copy_mode && (oml & 0x04u) != 0;
        cmd.no_near_clip = (st.gbi == GbiKind::F3DEX2_NON_FIFO);
        cmd.prim_depth = st.prim_depth;
        const float sh = screen_height(st);
        cmd.fb_height_px = static_cast<uint16_t>(
            (sh >= 1.0f && sh <= 1024.0f) ? sh : static_cast<float>(canvas_height_));
    }

    // Queues one triangle list (NDC pos + uv + shade + w + NDC z per vertex).
    void queue_triangles(ExecCtx* ctx, DrawCmd& cmd, const float* data, int count) {
        cmd.vertex_count = count;
        if (!ctx->first_quad_done && cmd.kind == 0 && count >= 3) {
            ctx->first_quad_done = true;
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (int i = 0; i < count; ++i) {
                minx = std::min(minx, data[i * 10 + 0]); maxx = std::max(maxx, data[i * 10 + 0]);
                miny = std::min(miny, data[i * 10 + 1]); maxy = std::max(maxy, data[i * 10 + 1]);
            }
            ctx->quad_w = maxx - minx;
            ctx->quad_h = maxy - miny;
        }
        // A G_ZS_PRIM draw takes its depth from G_SETPRIMDEPTH for the whole
        // primitive (RT64 RasterVS.hlsl: `ndcPos.z = primDepth.x * ndcPos.w`),
        // so every vertex's z is replaced here.
        for (int i = 0; i < count; ++i) {
            cmd.pos[i * 2 + 0] = data[i * 10 + 0];
            cmd.pos[i * 2 + 1] = data[i * 10 + 1];
            cmd.uv[i * 2 + 0] = data[i * 10 + 2];
            cmd.uv[i * 2 + 1] = data[i * 10 + 3];
            cmd.shade[i * 4 + 0] = data[i * 10 + 4];
            cmd.shade[i * 4 + 1] = data[i * 10 + 5];
            cmd.shade[i * 4 + 2] = data[i * 10 + 6];
            cmd.shade[i * 4 + 3] = data[i * 10 + 7];
            cmd.w[i] = data[i * 10 + 8];
            cmd.z[i] = cmd.z_source_prim ? (2.0f * cmd.prim_depth - 1.0f) : data[i * 10 + 9];
            // Track the NDC extent of everything queued so far: if the game's
            // geometry lands outside [-1, 1] nothing is visible even though the
            // draw commands exist.
            for (int k = 0; k < 2; ++k) {
                const float v = data[i * 10 + k];
                if (v < ndc_min_[k]) ndc_min_[k] = v;
                if (v > ndc_max_[k]) ndc_max_[k] = v;
            }
        }
        // Execution-local: published to draw_queue_ by the caller under a short
        // lock. Never lock the renderer mutex here - it was held across the
        // whole DL walk before, which both self-deadlocked (mutex_ is not
        // recursive) and starved the browser main thread's flush.
        note_depth_census(*ctx->st, cmd);
        note_fb_write(cmd);
        if (ctx->draws.size() < 16384) {
            ctx->draws.push_back(cmd);
        }
    }

    // RT64's per-vertex lighting for one G_LIGHTING draw.
    // tools/RT64/src/shaders/RSPProcessCS.hlsl:88-108: the ambient term is
    // `rspLight[lightIndex + lightCount - 1].col`, the loop covers
    // `lightIndex .. ambientIndex-1`, and `rgb = min(result, 1)`. Directional
    // lights go through shared/rt64_rsp_light.h:56 `computeDirLight`:
    //   localDir = normalize(mul(float4(posDir, 0), worldMat))
    //   result  += max(dot(norm, localDir), 0) * light.col
    // `worldMat` is RT64's `worldTransforms[]` entry, built in
    // hle/rt64_rsp.cpp:538 as `mul(modelMatrixStack.back(),
    // extended.viewProjMatrix)`. `extended.viewProjMatrix` is the identity
    // unless an RT64 extension set it (this game sets none), so worldMat is the
    // G_MTX MODELVIEW matrix the browser already keeps.
    //
    // Only the directional arm is reachable. RSP::setVertexCommon's
    // `usesPointLighting` is `curGBI->flags.pointLighting && (geometryMode &
    // G_POINT_LIGHTING)` (hle/rt64_rsp.cpp:563), and `pointLighting` is false
    // for every GBI including F3DEX2 (gbi/rt64_gbi.h:39, never assigned), so
    // `light.kc > 0` never selects `computePosLight`. F3DEX2's point-light arm
    // is therefore not implemented here; it would need `pointLighting` to be
    // true first.
    //
    // Returns false when the vertex keeps its raw colour, which is RT64's
    // `else` arm: no G_LIGHTING, or `lightCount == 0`.
    bool compute_vertex_light(const RenderState& st, const RawVertex& v, float rgb[3]) {
        if ((st.geometry_mode & kGeomLighting) == 0 || st.num_lights <= 0) {
            return false;
        }
        // RT64 uploads `lightCount + 1` entries, so the ambient index is
        // num_lights and the loop below must not read past it.
        const int ambient = std::min(st.num_lights, RenderState::kMaxLights - 1);
        // With G_LIGHTING the Vtx's four colour bytes are the normal. RT64
        // reads them through `Buffer<int> srcNorm` (signed bytes) and divides by
        // 127 (RSPProcessCS.hlsl:57-61); the alpha byte stays the vertex alpha.
        const float nx = static_cast<int8_t>(v.r) / 127.0f;
        const float ny = static_cast<int8_t>(v.g) / 127.0f;
        const float nz = static_cast<int8_t>(v.b) / 127.0f;
        // Light bytes in N64 order, as n64b copied them at G_MOVEMEM time. The
        // measured layout of the game's own light data (a mission `[GFX-LIGHT]`
        // line reads `raw 4D 4D 7F 00 4D 4D 7F 00 FD 99 C5 00` for a lit
        // terrain draw) is
        //   col[0..2] = r,g,b, pad at 3, colc[0..2] at 4..6, pad at 7,
        //   dir[0..2] = x,y,z at 8..10, pad at 11.
        // That is what RT64's `DirLight` reads through the runtime's reversed
        // RDRAM image: its `colr/colg/colb` fields at physical bytes 3/2/1 and
        // `dirx/diry/dirz` at physical bytes 11/10/9 are logical 0/1/2 and
        // 8/9/10.
        const uint8_t* amb = st.lights[ambient];
        float acc[3] = {amb[0] / 255.0f, amb[1] / 255.0f, amb[2] / 255.0f};
        const Mat4& world = st.modelview_stack.back();
        for (int i = 0; i < ambient; ++i) {
            const uint8_t* L = st.lights[i];
            const float dx = static_cast<int8_t>(L[8]);
            const float dy = static_cast<int8_t>(L[9]);
            const float dz = static_cast<int8_t>(L[10]);
            // Row-vector multiply, as RT64's `mul(float4(dir, 0), worldMat)`.
            Vec4 d = mul(Vec4{dx, dy, dz, 0.0f}, world);
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len > 0.0f) {
                d.x /= len;
                d.y /= len;
                d.z /= len;
            }
            const float ndotl = std::max(nx * d.x + ny * d.y + nz * d.z, 0.0f);
            acc[0] += ndotl * (L[0] / 255.0f);
            acc[1] += ndotl * (L[1] / 255.0f);
            acc[2] += ndotl * (L[2] / 255.0f);
        }
        for (int k = 0; k < 3; ++k) {
            rgb[k] = std::min(acc[k], 1.0f);
        }
        return true;
    }

    // Transforms a raw vertex through the matrix pipeline into NDC.
    // Returns false if the vertex is behind the camera (w <= 0).
    //
    // `out` is 10 floats: x, y (NDC), s, t, r, g, b, a, w, z_ndc. The depth is
    // the RDP's own screen z, transformed the way RT64 transforms it:
    //   posScreen = tfPos.xyz / float3(w, -w, w) * viewport.scale + translate
    //   (hle/rt64_rsp.cpp:714, `viewport.scale.z = vscale[3] / 1024`,
    //   `translate.z = vtrans[3] / 1024` - the same guest words this function
    //   reads as `view_scale[2]`/`view_trans[2]`, see set_viewport)
    //   RasterVS.hlsl then does `ndcPos.xyz *= ndcPos.w` for a non-rect draw, so
    //   the clip-space z is `screen_z * w` and the NDC z is `screen_z`.
    // GL's NDC z is [-1, 1] and its window depth is `(z_ndc + 1) / 2`, so the
    // value handed to the shader is `2 * screen_z - 1` and `gl_FragCoord.z`
    // comes back as the RDP's screen z. Everything downstream (the depth test,
    // the F3D depth clip at 1022/1024) is stated in that 0..1 space.
    bool transform_vertex(const RenderState& st, const RawVertex& v, float out[10]) {
        // Modelview then projection (row-vector).
        const Mat4& mv = st.modelview_stack.back();
        Vec4 pos = mul(Vec4{static_cast<float>(v.x), static_cast<float>(v.y),
                            static_cast<float>(v.z), 1.0f},
                       mv);
        pos = mul(pos, st.projection);
        // RT64's ProjectionProcessor::adjustProjectionMatrix scales the
        // projection's x column by 1/aspectRatioScale for an Expand pass. Its
        // 16:9 window against the game's 4:3 target is 1/(4/3) = 3/4.
        if (widescreen_expand_) {
            pos.x *= 0.75f;
        }
        if (pos.w <= 0.0f) {
            return false;
        }
        // Perspective divide + viewport. The RSP maps clip space to screen
        // space with the Vp_t scale/translate (screen y points down); the GL
        // framebuffer expects NDC, so the result is converted back.
        const float inv_w = 1.0f / pos.w;
        float sx, sy, sz;
        if (st.viewport_set) {
            const float scr_x = pos.x * inv_w * st.view_scale[0] + st.view_trans[0];
            const float scr_y = -pos.y * inv_w * st.view_scale[1] + st.view_trans[1];
            sx = ndc_xf(scr_x, st);
            sy = ndc_yf(scr_y, st);
            sz = pos.z * inv_w * st.view_scale[2] + st.view_trans[2];
        } else {
            // No viewport seen yet: treat the projection output as NDC.
            sx = pos.x * inv_w;   // [-1, 1] (x)
            sy = -pos.y * inv_w;  // flipped: NDC y up
            // The z of that NDC, moved into the RDP's 0..1 screen depth.
            sz = pos.z * inv_w * 0.5f + 0.5f;
        }
        // RT64's half-pixel screen offset, for the triangle path only.
        // `tools/RT64/src/render/rt64_framebuffer_renderer.cpp`:
        // `halfPixelOffset = { 1.0f / framebuffer.viewport.width, -1.0f /
        // framebuffer.viewport.height }`, assigned as `triangles.screenOffset`
        // and applied in `tools/RT64/src/shaders/RasterVS.hlsl` as
        // `ndcPos.xy = (ndcPos.xy * screenScale) + screenOffset * ndcPos.w;`
        // (`screenScale` is 1 unless the viewport is stretched). That is half a
        // pixel right and half a pixel down in window space, so a quad whose
        // vertex lands on a whole pixel samples half a texel nearer the start
        // of its span. `ndc_yf` maps guest row y to `1 - 2y/H`, so one pixel
        // down is -1/H. RT64's `renderFlagRect` branch skips only the
        // screen-to-NDC transform; the browser's rect vertices already carry
        // the same convention and measure exact against the native target.
        sx += 1.0f / screen_width(st);
        sy -= 1.0f / screen_height(st);
        out[0] = sx;
        out[1] = sy;
        // Texture coordinates: s10.5 texels scaled by G_TEXTURE (RT64
        // RSP::setVertexCommon). The G_TEXTURE scale must NOT be applied to
        // rectangles - RT64's RDP::drawRect uses uls/32 and lrs/32 directly.
        //
        // G_TEXTURE_GEN replaces the vertex's s,t with a projection of its
        // normal onto the two lookat vectors (RT64 `computeTextureGen`,
        // shaders/TextureGen.hlsli:19-33). RT64 enables it only when both
        // G_LIGHTING and G_TEXTURE_GEN are set (`usesTextureGen`,
        // hle/rt64_rsp.cpp:641-643); the vertex colour bytes are then the
        // normal, the same bytes `compute_vertex_light` reads. The upload then
        // carries the raw G_TEXTURE scale as the input UV
        // (`hle/rt64_rsp.cpp:748-753`) and the shader multiplies it by the
        // generated coordinate, so `sc / 65536 * coord` is in the same texel
        // units as the non-texgen arm below. The q meshes of the publisher
        // stills are drawn this way: a 32x32 environment map swept by the
        // surface normal, which is the moving highlight native shows and a
        // fixed patch of the image cannot produce (session 185).
        const bool texgen = (st.geometry_mode & (kGeomLighting | kGeomTextureGen)) ==
                            (kGeomLighting | kGeomTextureGen);
        if (texgen) {
            const float nx = static_cast<int8_t>(v.r) / 127.0f;
            const float ny = static_cast<int8_t>(v.g) / 127.0f;
            const float nz = static_cast<int8_t>(v.b) / 127.0f;
            const Mat4& world = st.modelview_stack.back();
            float gen[2];
            for (int axis = 0; axis < 2; ++axis) {
                const float* L = st.lookat[axis];
                // Row-vector multiply, as RT64's
                // `mul(float4(lookAt.x, 0.0f), worldMatrix)`.
                Vec4 d = mul(Vec4{L[0], L[1], L[2], 0.0f}, world);
                const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > 0.0f) {
                    d.x /= len;
                    d.y /= len;
                    d.z /= len;
                }
                float dot = nx * d.x + ny * d.y + nz * d.z;
                dot = std::clamp(dot, -1.0f, 1.0f);
                if ((st.geometry_mode & kGeomTextureGenLinear) != 0) {
                    gen[axis] = std::acos(-dot) * 325.94932f;
                } else {
                    gen[axis] = (dot + 1.0f) * 512.0f;
                }
            }
            out[2] = st.tex_scale_s * gen[0];
            out[3] = st.tex_scale_t * gen[1];
        } else {
            out[2] = (static_cast<float>(v.s) * st.tex_scale_s) / 32.0f;
            out[3] = (static_cast<float>(v.t) * st.tex_scale_t) / 32.0f;
        }
        float lit[3] = {0.0f, 0.0f, 0.0f};
        if (compute_vertex_light(st, v, lit)) {
            out[4] = lit[0];
            out[5] = lit[1];
            out[6] = lit[2];
        } else {
            out[4] = v.r / 255.0f;
            out[5] = v.g / 255.0f;
            out[6] = v.b / 255.0f;
        }
        // Alpha: RT64 replaces it with the fog alpha when G_FOG is set, and with
        // the vertex colour's own alpha otherwise (`RSPProcessCS.hlsl:70-80`):
        //   fogAlpha   = max(tfPos.z, 0) / tfPos.w * rspFog.mul + rspFog.offset
        //   vertexAlpha = clamp(fogAlpha / 255, 0, 1)
        // `tfPos` is the clip-space position (`mul(mul(viewProjMat, worldMat),
        // v)`), so `tfPos.z / tfPos.w` is the plain NDC z, not the RDP's screen
        // z that `sz` above carries (the viewport scale/translate come after).
        // RT64's `fogIndex > 0` arm is entered for every vertex of a G_FOG
        // workload; `rspFog` exists from the first G_FOG transform with the last
        // G_MW_FOG's mul/offset (RSP::setFog, `hle/rt64_rsp.cpp:990-994`, and
        // the zero default at `:91-92`). The rect paths do not run RSP vertex
        // processing, so fog applies to triangles only, which is this function.
        if ((st.geometry_mode & kGeomFog) != 0) {
            const float ndc_z = pos.z * inv_w;
            const float fog_alpha =
                (std::max(ndc_z, 0.0f) * static_cast<float>(st.fog_mul) +
                 static_cast<float>(st.fog_offset)) / 255.0f;
            out[7] = std::clamp(fog_alpha, 0.0f, 1.0f);
            if (!fog_logged_) {
                fog_logged_ = true;
                OGRE_MILESTONE("GFX-FOG",
                               "per-vertex alpha: z/w=%.4f mul=%d offset=%d -> alpha=%.4f",
                               ndc_z, static_cast<int>(st.fog_mul),
                               static_cast<int>(st.fog_offset), out[7]);
            }
        } else {
            out[7] = v.a / 255.0f;
        }
        // The clip-space w, for perspective-correct interpolation.
        out[8] = pos.w;
        // NDC depth (see the header comment): the RDP's 0..1 screen z mapped
        // onto GL's [-1, 1]. A `G_ZS_PRIM` draw overwrites it with the
        // primitive depth in draw_tri/queue_triangles, because that is
        // per-draw state, not per-vertex.
        out[9] = 2.0f * sz - 1.0f;
        return true;
    }

    // Permanent shading census: one `[GFX-LIGHT]` line for the first lit draw
    // and every 400 lit draws after it (and the same for unlit draws, so a
    // screen with no lighting still states its state). It answers from the log
    // what a scene's draws do: how many are lit, the light count, whether any
    // light carries the point-light `kc` byte, and the first draw's normal and
    // result. Cheap: the light scan runs only on a logged draw.
    void note_shading_census(const RenderState& st, const RawVertex& v0, const float* data,
                             const char* what) {
        const bool lit = (st.geometry_mode & kGeomLighting) != 0 && st.num_lights > 0;
        unsigned& n = lit ? lit_draws_ : unlit_draws_;
        ++n;
        if (n != 1 && (n % 400) != 0) {
            return;
        }
        // RT64 selects computePosLight only when the GBI flag `pointLighting`
        // is set (hle/rt64_rsp.cpp:563), and it is false for every GBI
        // (gbi/rt64_gbi.h:39), so every uploaded light is drawn as directional
        // (`point=0`). The point-light `kc` byte (PosLight::kc) and a
        // directional light's red channel are both light byte 0, so the stored
        // bytes cannot tell the two layouts apart; the flag decides.
        const int point_lights = 0;
        const uint8_t* L0 = st.lights[0];
        const int amb = std::min(st.num_lights, RenderState::kMaxLights - 1);
        const uint8_t* A = st.lights[amb];
        OGRE_MILESTONE("GFX-LIGHT",
                       "%s %s=%u lit=%u unlit=%u culled=%u lights=%u point=%d "
                       "L0=[col %u %u %u dir %d %d %d raw %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] "
                       "amb=[col %u %u %u raw %02X %02X %02X %02X] "
                       "norm=[%.3f %.3f %.3f] shade=[%.3f %.3f %.3f %.3f]",
                       what, lit ? "lit" : "unlit", n, lit_draws_, unlit_draws_, tris_culled_,
                       static_cast<unsigned>(st.num_lights), point_lights,
                       (unsigned)L0[0], (unsigned)L0[1], (unsigned)L0[2],
                       static_cast<int>(static_cast<int8_t>(L0[8])),
                       static_cast<int>(static_cast<int8_t>(L0[9])),
                       static_cast<int>(static_cast<int8_t>(L0[10])),
                       (unsigned)L0[0], (unsigned)L0[1], (unsigned)L0[2], (unsigned)L0[3],
                       (unsigned)L0[4], (unsigned)L0[5], (unsigned)L0[6], (unsigned)L0[7],
                       (unsigned)L0[8], (unsigned)L0[9], (unsigned)L0[10], (unsigned)L0[11],
                       (unsigned)A[0], (unsigned)A[1], (unsigned)A[2],
                       (unsigned)A[0], (unsigned)A[1], (unsigned)A[2], (unsigned)A[3],
                       static_cast<int8_t>(v0.r) / 127.0f,
                       static_cast<int8_t>(v0.g) / 127.0f,
                       static_cast<int8_t>(v0.b) / 127.0f,
                       data[4], data[5], data[6], data[7]);
    }

    // Permanent depth census: one `[GFX-DEPTH]` line for the first draw whose
    // state reaches the depth pipeline and every 400 after it. It answers from
    // the log how much of a scene is depth-tested (zCmp), depth-written (zUpd),
    // primitive-depth (G_ZS_PRIM) and decal (ZMODE_DEC), and which G_SETZIMG
    // configurations the display lists named. A ZMODE_DEC draw prints a one-shot
    // note: RT64 does not depth-*test* those either (the pipeline flag drops
    // zCmp for ZMODE_DEC, render/rt64_framebuffer_renderer.cpp:1729) and its
    // decal test is a fragment-shader depth tolerance (`CoplanarDepthTolerance`,
    // RasterPS.hlsl:90-104) which this renderer does not implement, so a decal
    // draw keeps only its depth write here.
    void note_depth_census(const RenderState& st, const DrawCmd& cmd) {
        ++depth_draws_;
        // Which colour image a depth image is shared with. RT64 keys the depth
        // RenderTarget by the depth image and the colour RenderTarget by the
        // colour image, so a pass that switches CIMG but not ZIMG keeps the
        // depth contents. This line states whether this game does that: one line
        // per distinct (colour, depth) pair, with the draw it first appeared on.
        if (cmd.fb_address != depth_censused_fb_ || cmd.zimg_address != depth_censused_z_) {
            depth_censused_fb_ = cmd.fb_address;
            depth_censused_z_ = cmd.zimg_address;
            auto inserted = depth_pairs_.insert({std::make_pair(cmd.fb_address, cmd.zimg_address),
                                                 depth_draws_});
            if (inserted.second && depth_pairs_.size() <= 12) {
                OGRE_MILESTONE("GFX-DEPTH",
                               "pair #%zu colour=0x%06X depth=0x%06X first_draw=%u",
                               depth_pairs_.size(), cmd.fb_address, cmd.zimg_address,
                               depth_draws_);
            }
        }
        if (cmd.depth_test) ++depth_zcmp_;
        if (cmd.depth_write) ++depth_zupd_;
        if (cmd.z_source_prim) ++depth_prim_;
        if (cmd.z_mode == 0xC00) {
            ++depth_dec_;
            if (depth_dec_logged_ < 2) {
                ++depth_dec_logged_;
                OGRE_MILESTONE("GFX-DEPTH",
                               "draw asks for ZMODE_DEC (oml=0x%06X) - the decal depth tolerance "
                               "is NOT implemented; this draw keeps zCmp=%d zUpd=%d and no decal test",
                               st.othermode_l, cmd.depth_test ? 1 : 0, cmd.depth_write ? 1 : 0);
            }
        }
        if (depth_draws_ != 1 && (depth_draws_ % 400) != 0) {
            return;
        }
        depth_logged_at_ = depth_draws_;
        // The window depth (0..1) this draw's vertices span: the number that
        // decides whether a depth test can order anything. A draw whose whole
        // range is a hundredth of the buffer cannot separate its surfaces.
        float zlo = 1e9f, zhi = -1e9f;
        for (int i = 0; i < cmd.vertex_count; ++i) {
            const float wz = (cmd.z[i] + 1.0f) * 0.5f;
            zlo = std::min(zlo, wz);
            zhi = std::max(zhi, wz);
        }
        OGRE_MILESTONE("GFX-DEPTH",
                       "draws=%u zcmp=%u zupd=%u prim=%u dec=%u zimg_seen=%zu pairs=%zu "
                       "zimg=[addr=0x%06X fmt=%u siz=%u width=%u] "
                       "this=[kind=%d test=%d write=%d primz=%.4f zmode=0x%03X "
                       "wz=%.5f..%.5f vs=%.4f/%.4f]",
                       depth_draws_, depth_zcmp_, depth_zupd_, depth_prim_, depth_dec_,
                       zimg_seen_.size(), depth_pairs_.size(), st.zimg_address,
                       (unsigned)st.zimg_fmt,
                       (unsigned)st.zimg_siz, (unsigned)st.zimg_width, cmd.kind,
                       cmd.depth_test ? 1 : 0, cmd.depth_write ? 1 : 0,
                       static_cast<double>(cmd.prim_depth), (unsigned)cmd.z_mode,
                       static_cast<double>(zlo), static_cast<double>(zhi),
                       static_cast<double>(st.view_scale[2]), static_cast<double>(st.view_trans[2]));
    }

    void draw_tri(ExecCtx* ctx, RenderState& st, int i0, int i1, int i2) {
        // Debug: draw only the first sprite of the list (ogre_gfx_debug_flags
        // bit 1), so a probe can compare one rendered sprite with its textures.
        // 3 = the frame's fill rect plus this sprite's two triangles; a lower
        // bound would cut the sprite in half along the quad's diagonal.
        if ((g_debug_flags.load(std::memory_order_relaxed) & 2u) != 0 && ctx->draws.size() >= 3) {
            return;
        }
        if (i0 >= kMaxVertices || i1 >= kMaxVertices || i2 >= kMaxVertices) {
            ++tris_rejected_;
            return;
        }
        // RT64 draws nothing at all when both cull bits are set
        // (hle/rt64_rsp.cpp:1059), before the index swap and the cross product.
        if ((st.geometry_mode & kGeomCullBoth) == kGeomCullBoth) {
            ++tris_culled_;
            return;
        }
        const RawVertex* v[3] = {&st.vertices[i0], &st.vertices[i1], &st.vertices[i2]};
        {
            int x0 = v[0]->x, x1 = v[0]->x, y0 = v[0]->y, y1 = v[0]->y;
            for (int k = 1; k < 3; ++k) {
                x0 = std::min<int>(x0, v[k]->x); x1 = std::max<int>(x1, v[k]->x);
                y0 = std::min<int>(y0, v[k]->y); y1 = std::max<int>(y1, v[k]->y);
            }
            log_wide_draw(st, "tri", x0, y0, x1, y1,
                          texture_available(st) ? "tex=1" : "tex=0");
        }
        float data[3 * 10];
        for (int i = 0; i < 3; ++i) {
            if (!transform_vertex(st, *v[i], data + i * 10)) {
                if (tri_reject_logged_ < 2) {
                    ++tri_reject_logged_;
                    const Mat4& mv = st.modelview_stack.back();
                    OGRE_MILESTONE("GFX-REJ",
                                   "rejected tri v=(%d,%d,%d) mv=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f] "
                                   "proj=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]",
                                   v[i]->x, v[i]->y, v[i]->z,
                                   mv.m[0], mv.m[1], mv.m[2], mv.m[3], mv.m[4], mv.m[5], mv.m[6], mv.m[7],
                                   mv.m[8], mv.m[9], mv.m[10], mv.m[11], mv.m[12], mv.m[13], mv.m[14], mv.m[15],
                                   st.projection.m[0], st.projection.m[1], st.projection.m[2], st.projection.m[3],
                                   st.projection.m[4], st.projection.m[5], st.projection.m[6], st.projection.m[7],
                                   st.projection.m[8], st.projection.m[9], st.projection.m[10], st.projection.m[11],
                                   st.projection.m[12], st.projection.m[13], st.projection.m[14], st.projection.m[15]);
                }
                ++tris_rejected_;
                return;  // back-facing / clipped
            }
        }
        if (ctx->order.size() < 200) ctx->order += texture_available(st) ? 't' : 'u';
        // Screen-space backface culling, mirroring RT64 RSP::drawIndexedTri
        // (tools/RT64/src/hle/rt64_rsp.cpp:1091-1140):
        //   - `(geometryMode & cullBothMask) == cullBothMask` draws nothing
        //     (the check at :1059, done at the top of draw_tri);
        //   - front-face culling alone swaps the first and last index (:1094);
        //   - U = p1-p0, V = p2-p0, N = cross(V, U), visible = N.z >= 0 (:1132).
        // RT64's posScreen has y down (RSPProcessCS.hlsl `ndcPos = tfPos.xyz /
        // float3(w, -w, w)`); out[0..1] here are NDC with y up, so the test uses
        // (x, -y). The viewport scale/translate are positive and affine, so they
        // do not change the sign.
        const uint32_t geom = st.geometry_mode;
        if (geom & kGeomCullBoth) {
            if ((geom & kGeomCullBoth) == kGeomCullFront) {
                for (int k = 0; k < 10; ++k) {
                    std::swap(data[k], data[20 + k]);
                }
            }
            const float ux = data[10 + 0] - data[0];
            const float uy = -data[10 + 1] + data[1];
            const float vx = data[20 + 0] - data[0];
            const float vy = -data[20 + 1] + data[1];
            const float nz = vx * uy - vy * ux;
            if (!(nz >= 0.0f)) {
                ++tris_culled_;
                note_shading_census(st, *v[0], data, "culled");
                return;
            }
        }
        note_shading_census(st, *v[0], data, "drawn");
        // A draw samples TMEM through its render tiles, so decode the two
        // images now (lazily, once per key) and use their rects as the UV basis.
        // This is not gated on G_TEXTURE's `on` field: whether a texel unit is
        // read at all is decided by the combiner's selectors (RT64
        // ColorCombiner::usesTexture), and an unused unit is simply never
        // sampled by the shader.
        ensure_tile_image(ctx, st, st.active_tile, 0);
        ensure_tile_image(ctx, st, st.active_tile + 1, 1);
        if (draw_mtx_logged_ < 2) {
            ++draw_mtx_logged_;
            const Mat4& mv = st.modelview_stack.back();
            const Mat4& pj = st.projection;
            OGRE_MILESTONE("GFX-MTX",
                           "draw: modelview diag=[%.4f %.4f %.4f %.4f] trans=[%.3f %.3f %.3f] | "
                           "projection diag=[%.4f %.4f %.4f %.4f] trans=[%.3f %.3f %.3f] | "
                           "viewport scale=[%.2f %.2f] trans=[%.2f %.2f] set=%d",
                           mv.m[0], mv.m[5], mv.m[10], mv.m[15], mv.m[12], mv.m[13], mv.m[14],
                           pj.m[0], pj.m[5], pj.m[10], pj.m[15], pj.m[12], pj.m[13], pj.m[14],
                           st.view_scale[0], st.view_scale[1], st.view_trans[0], st.view_trans[1],
                           st.viewport_set ? 1 : 0);
        }
        if (!ctx->uv_logged && texture_available(st)) {
            ctx->uv_logged = true;
            ctx->u0 = ctx->u1 = data[2];
            ctx->v0 = ctx->v1 = data[3];
            for (int i = 1; i < 3; ++i) {
                ctx->u0 = std::min(ctx->u0, data[i * 10 + 2]);
                ctx->u1 = std::max(ctx->u1, data[i * 10 + 2]);
                ctx->v0 = std::min(ctx->v0, data[i * 10 + 3]);
                ctx->v1 = std::max(ctx->v1, data[i * 10 + 3]);
            }
            ctx->iw0 = st.tiles[st.active_tile & (kMaxTiles - 1)].image.width;
            ctx->ih0 = st.tiles[st.active_tile & (kMaxTiles - 1)].image.height;
            ctx->iw1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image.width;
            ctx->ih1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image.height;
        }
        DrawCmd cmd;
        record_state(cmd, st, texture_available(st));
        if (texture_available(st)) {
            dump_tex_state(ctx, st, "tri", data);
        }
        queue_triangles(ctx, cmd, data, 3);
    }

    void draw_fill_rect(ExecCtx* ctx, RenderState& st, int xl, int yl, int xh, int yh) {
        if (ctx->order.size() < 200) ctx->order += 'F';
        // The RDP ignores empty/inverted rectangles (RT64 RDP::fillRect and
        // FixedRect::isEmpty). OB64's title DL contains a TEXRECT with
        // xl=319,yl=239,xh=0,yh=0 - decoding that span as a quad produced a
        // full-screen black cover over the whole scene.
        if (xh < xl || yh < yl) {
            return;
        }
        // The fill colour is written to the colour image in that image's own
        // format, so which half of the G_SETFILLCOLOR word it is depends on the
        // image:
        //
        //   16-bit image (G_IM_SIZ_16b): the low 16 bits are an RGBA5551 value
        //     (RT64's FillRect instance path uses
        //     `ColorConverter::RGBA16::toRGBAF(fillColor & 0xFFFF)` whenever
        //     `fbPair.colorImage.siz != G_IM_SIZ_32b`),
        //   32-bit image: the whole word is RGBA8888
        //     (`ColorConverter::RGBA32::toRGBAF(fillColor)`).
        //
        // The story attract's letterbox fill is `0x4AC14AC1`: as a 16-bit value
        // it is `(74,90,0)`, the olive RT64 clears its 16-bit target with, and as
        // 32-bit RGBA8 it is `(74,193,74)`, a green. The fill colour is the only
        // RDP colour register whose decode depends on the colour image (the
        // combiner's PRIM/ENV/etc. inputs are 8-bit per channel whatever the
        // image is, so `fill_color` keeps the 32-bit decode for the canvas path).
        float c[4] = {st.fill_color[0], st.fill_color[1], st.fill_color[2], st.fill_color[3]};
        if (st.cimg_address != 0 && ((st.cimg_siz & 3u) != gbi::IM_SIZ_32b)) {
            uint8_t rgba8[4];
            rgba16_to_rgba8(static_cast<uint16_t>(st.fill_color_word & 0xFFFFu), rgba8);
            for (int i = 0; i < 4; ++i) {
                c[i] = static_cast<float>(rgba8[i]) / 255.0f;
            }
        }
        {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "fill=[%.2f,%.2f,%.2f,%.2f] word=0x%08X siz=%u cyc=%d",
                     c[0], c[1], c[2], c[3], st.fill_color_word, (unsigned)st.cimg_siz,
                     static_cast<int>((st.othermode_h >> 20) & 3));
            log_wide_draw(st, "fillrect", xl, yl, xh, yh, detail);
        }
        if (rect_logged_ < 8) {
            ++rect_logged_;
            OGRE_MILESTONE("GFX-RECT",
                           "fillrect (%d,%d)-(%d,%d) cyc=%d omh=0x%06X oml=0x%08X "
                           "fill=[%.2f,%.2f,%.2f,%.2f] word=0x%08X siz=%u scissor=%d(%d,%d,%d,%d)",
                           xl, yl, xh, yh, static_cast<int>((st.othermode_h >> 20) & 3),
                           st.othermode_h, st.othermode_l, c[0], c[1], c[2], c[3],
                           st.fill_color_word, (unsigned)st.cimg_siz,
                           st.scissor_enabled ? 1 : 0,
                           st.scissor_x0, st.scissor_y0, st.scissor_x1, st.scissor_y1);
        }
        // G_FILLRECT in fill mode outputs the fill color directly (the
        // combiner is bypassed). Approximate by driving the combiner with the
        // fill color as PRIMITIVE: rgb = (PRIM - 0) * 1 + 0.
        const float x0 = ndc_x(xl, st), x1 = ndc_x(xh + 1, st);
        const float y0 = ndc_y(yl, st), y1 = ndc_y(yh + 1, st);
        float data[6 * 10];
        // The four corners this loop names are (x0,y0), (x0,y1), (x1,y1),
        // (x1,y0): corner 0 is the top-left, and corners increase clockwise,
        // which is the same order the S2DEX object path uses. The two triangles
        // must split that loop on its own diagonal, (0,1,2) and (0,2,3).
        // Emitting (3,0,1) for the second one repeats the first triangle's left
        // edge, so both triangles cover the left half of the rectangle and the
        // right half is never written. A `G_FILLRECT` is the game's per-frame
        // clear, so with that pairing every frame kept the previous frames'
        // pixels on the right half of the screen: the boot intro (scene `0x09`)
        // left its soldier ring from every earlier frame on screen as a wedge of
        // repeated sprites.
        static const int kQuadTris[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            const int corner = kQuadTris[i];
            const float px = (corner == 0 || corner == 1) ? x0 : x1;
            const float py = (corner == 0 || corner == 3) ? y0 : y1;
            data[i * 10 + 0] = px;
            data[i * 10 + 1] = py;
            data[i * 10 + 2] = 0;
            data[i * 10 + 3] = 0;
            data[i * 10 + 4] = c[0];
            data[i * 10 + 5] = c[1];
            data[i * 10 + 6] = c[2];
            data[i * 10 + 7] = c[3];
            data[i * 10 + 8] = 1.0f;
            // RT64's rect vertices carry NDC z = 0 (`rectPosFloats`, hle/
            // rt64_rdp.cpp:1559) and w = 1; a G_ZS_PRIM rect takes the
            // primitive depth instead (queue_triangles applies that).
            data[i * 10 + 9] = 0.0f;
        }

        // Record the draw with the fill-color combiner override.
        DrawCmd cmd;
        const float saved_prim[4] = {st.prim_color[0], st.prim_color[1], st.prim_color[2], st.prim_color[3]};
        const Combiner saved_comb = st.combiner;
        st.prim_color[0] = c[0];
        st.prim_color[1] = c[1];
        st.prim_color[2] = c[2];
        st.prim_color[3] = c[3];
        st.combiner.decode(0, 0);
        for (int i = 0; i < 2; ++i) {
            st.combiner.ca[i] = CSEL_PRIMITIVE;
            st.combiner.cb[i] = CSEL_ZERO;
            st.combiner.cc[i] = CSEL_ONE;
            st.combiner.cd[i] = CSEL_ZERO;
            st.combiner.aa[i] = ASEL_PRIMITIVE;
            st.combiner.ab[i] = ASEL_ZERO;
            st.combiner.ac[i] = ASEL_ONE;
            st.combiner.ad[i] = ASEL_ZERO;
        }

        cmd.kind = 1;
        record_state(cmd, st, false);
        queue_triangles(ctx, cmd, data, 6);

        st.prim_color[0] = saved_prim[0];
        st.prim_color[1] = saved_prim[1];
        st.prim_color[2] = saved_prim[2];
        st.prim_color[3] = saved_prim[3];
        st.combiner = saved_comb;
    }

    // ----- S2DEX2 2D object commands ------------------------------------------
    //
    // Ogre Battle 64 draws its njpeg backgrounds entirely with these: per
    // macroblock the list loads a 16x16 YUV16 block into TMEM and then issues
    // `G_OBJ_MOVEMEM` (the sub matrix) + `G_OBJ_RECTANGLE_R` (the draw). See
    // docs/HANDOFF-2026-09-15-session51.md §2 for the recipe and §5-§6 for the
    // geometry and the YUV16 model. RT64's `GBI_S2DEX::drawObjRectangle` is the
    // reference these two functions mirror.

    // The 24-byte uObjSprite, decoded field by field. The struct is big-endian
    // guest data in word-reversed RDRAM, so a struct cast would swap every
    // 16-bit pair (RT64 `decodeObjSprite`).
    struct ObjSprite {
        int32_t objX = 0, objY = 0;      // s10.2
        uint16_t scaleW = 0, scaleH = 0; // u5.10
        uint16_t imageW = 0, imageH = 0; // u10.5
        uint16_t imageStride = 0;        // texture line, in 64-bit words
        uint16_t imageAdrs = 0;          // TMEM address, in 64-bit words
        uint8_t imageFmt = 0, imageSiz = 0, imagePal = 0, imageFlags = 0;
    };

    static ObjSprite decode_obj_sprite(const uint8_t* rdram, uint32_t addr) {
        const uint8_t* p = rdram + (addr & 0x1FFFFFFFu);
        const uint32_t w0 = rd32(p + 0);
        const uint32_t w1 = rd32(p + 4);
        const uint32_t w2 = rd32(p + 8);
        const uint32_t w3 = rd32(p + 12);
        const uint32_t w4 = rd32(p + 16);
        const uint32_t w5 = rd32(p + 20);
        ObjSprite s;
        s.objX = static_cast<int16_t>(w0 >> 16);
        s.scaleW = static_cast<uint16_t>(w0);
        s.imageW = static_cast<uint16_t>(w1 >> 16);
        s.objY = static_cast<int16_t>(w2 >> 16);
        s.scaleH = static_cast<uint16_t>(w2);
        s.imageH = static_cast<uint16_t>(w3 >> 16);
        s.imageStride = static_cast<uint16_t>(w4 >> 16);
        s.imageAdrs = static_cast<uint16_t>(w4);
        s.imageFmt = static_cast<uint8_t>(w5 >> 24);
        s.imageSiz = static_cast<uint8_t>(w5 >> 16);
        s.imagePal = static_cast<uint8_t>(w5 >> 8);
        s.imageFlags = static_cast<uint8_t>(w5);
        return s;
    }

    // G_OBJ_MOVEMEM: `index` (the low 16 bits of the command word) selects the
    // object matrix (0, 24 bytes) or the object sub matrix (2, 8 bytes).
    void set_obj_matrix(ExecCtx* ctx, uint32_t addr, uint32_t index) {
        RenderState& st = *ctx->st;
        const uint8_t* p = rdram_ + (addr & 0x1FFFFFFFu);
        if (index == 0) {
            st.obj_mtx.a = static_cast<int32_t>(rd32(p + 0));
            st.obj_mtx.b = static_cast<int32_t>(rd32(p + 4));
            st.obj_mtx.c = static_cast<int32_t>(rd32(p + 8));
            st.obj_mtx.d = static_cast<int32_t>(rd32(p + 12));
            const uint32_t w4 = rd32(p + 16);
            const uint32_t w5 = rd32(p + 20);
            st.obj_mtx.x = static_cast<int16_t>(w4 >> 16);
            st.obj_mtx.y = static_cast<int16_t>(w4);
            st.obj_mtx.base_scale_x = static_cast<uint16_t>(w5 >> 16);
            st.obj_mtx.base_scale_y = static_cast<uint16_t>(w5);
            st.obj_mtx.sub_mtx = false;
        } else if (index == 2) {
            const uint32_t w0 = rd32(p + 0);
            const uint32_t w1 = rd32(p + 4);
            st.obj_mtx.a = 0x10000;
            st.obj_mtx.b = 0;
            st.obj_mtx.c = 0;
            st.obj_mtx.d = 0x10000;
            st.obj_mtx.x = static_cast<int16_t>(w0 >> 16);
            st.obj_mtx.y = static_cast<int16_t>(w0);
            st.obj_mtx.base_scale_x = static_cast<uint16_t>(w1 >> 16);
            st.obj_mtx.base_scale_y = static_cast<uint16_t>(w1);
            st.obj_mtx.sub_mtx = true;
        }
    }

    // G_OBJ_RECTANGLE_R / G_OBJ_RECTANGLE (full_matrix = false) and
    // G_OBJ_SPRITE (full_matrix = true): one axis-aligned texture rectangle.
    void draw_obj_rect(ExecCtx* ctx, uint32_t sprite_addr, bool full_matrix) {
        RenderState& st = *ctx->st;
        const ObjSprite s = decode_obj_sprite(rdram_, sprite_addr);
        const RenderState::ObjMtx& m = st.obj_mtx;

        const float imageW = s.imageW / 32.0f;   // u10.5 -> texels
        const float imageH = s.imageH / 32.0f;
        const float scaleW = static_cast<float>(std::max<uint16_t>(s.scaleW, 1)) / 1024.0f;
        const float scaleH = static_cast<float>(std::max<uint16_t>(s.scaleH, 1)) / 1024.0f;
        const float objX = s.objX / 4.0f;        // s10.2 -> pixels
        const float objY = s.objY / 4.0f;
        const float objW = imageW / scaleW + objX;
        const float objH = imageH / scaleH + objY;

        const float mtxX = m.x / 4.0f;
        const float mtxY = m.y / 4.0f;
        const float bsX = static_cast<float>(std::max<uint16_t>(m.base_scale_x, 1)) / 1024.0f;
        const float bsY = static_cast<float>(std::max<uint16_t>(m.base_scale_y, 1)) / 1024.0f;

        float x0, y0, x1, y1;
        if (full_matrix && !m.sub_mtx) {
            const float a = m.a / 65536.0f, b = m.b / 65536.0f;
            const float c = m.c / 65536.0f, d = m.d / 65536.0f;
            x0 = a * objX + b * objY + mtxX;
            y0 = c * objX + d * objY + mtxY;
            x1 = a * objW + b * objH + mtxX;
            y1 = c * objW + d * objH + mtxY;
        } else {
            x0 = mtxX + objX / bsX;
            y0 = mtxY + objY / bsY;
            x1 = mtxX + objW / bsX;
            y1 = mtxY + objH / bsY;
        }

        // The render tile comes from the sprite: the microcode programs it with
        // the object's own texture description (RT64 `drawObjRectangle`).
        const int tile = 0;
        TileState& t = st.tiles[tile];
        t.fmt = s.imageFmt;
        t.siz = s.imageSiz;
        t.line = s.imageStride;
        t.tmem = s.imageAdrs;
        t.palette = s.imagePal;
        t.cms = 0;                     // G_TX_WRAP | G_TX_NOMIRROR
        t.cmt = 0;
        t.masks = 0;                   // G_TX_NOMASK
        t.maskt = 0;
        t.shifts = 0;                  // G_TX_NOLOD
        t.shiftt = 0;
        t.uls = 0;
        t.ult = 0;
        // imageW/imageH are u10.5 and the RDP tile size is 1/4-texel with an
        // inclusive lrs/lrt, so 16 texels is `16*4 - 4 = 60`.
        t.lrs = static_cast<uint16_t>(std::max<int32_t>((static_cast<int32_t>(s.imageW) >> 3) - 4, 0));
        t.lrt = static_cast<uint16_t>(std::max<int32_t>((static_cast<int32_t>(s.imageH) >> 3) - 4, 0));
        // The image this rectangle samples is the block the preceding LOADTILE
        // put in TMEM (the list re-SETTIMGs and re-loads before every macroblock).
        if (st.pending_load.valid) {
            set_tile_image(st, tile, st.pending_load.address, st.pending_load.stride);
            st.pending_load.valid = false;
        }

        const int saved_tile = st.active_tile;
        st.active_tile = tile;
        ensure_tile_image(ctx, st, tile, 0);
        ensure_tile_image(ctx, st, tile + 1, 1);
        if (!texture_available(st)) {
            st.active_tile = saved_tile;
            return;
        }

        // The rectangle covers the tile's whole image, so the UVs run over the
        // sprite's texel extent; the shader normalises with the decoded image's
        // size and origin.
        const bool flipS = (s.imageFlags & 0x01) != 0;   // G_BG_FLAG_FLIPS
        const bool flipT = (s.imageFlags & 0x10) != 0;   // G_BG_FLAG_FLIPT
        const float us0 = flipS ? imageW : 0.0f;
        const float us1 = flipS ? 0.0f : imageW;
        const float vt0 = flipT ? imageH : 0.0f;
        const float vt1 = flipT ? 0.0f : imageH;

        const float nx0 = ndc_xf(x0, st), nx1 = ndc_xf(x1, st);
        const float ny0 = ndc_yf(y0, st), ny1 = ndc_yf(y1, st);
        const float px[4] = {nx0, nx1, nx0, nx1};
        const float py[4] = {ny0, ny0, ny1, ny1};
        const float uv[4][2] = {{us0, vt0}, {us1, vt0}, {us0, vt1}, {us1, vt1}};

        float data[6 * 10];
        const int idx[6] = {0, 1, 3, 0, 3, 2};
        for (int i = 0; i < 6; ++i) {
            const int c = idx[i];
            data[i * 10 + 0] = px[c];
            data[i * 10 + 1] = py[c];
            data[i * 10 + 2] = uv[c][0];
            data[i * 10 + 3] = uv[c][1];
            data[i * 10 + 4] = 1;
            data[i * 10 + 5] = 1;
            data[i * 10 + 6] = 1;
            data[i * 10 + 7] = 1;
            data[i * 10 + 8] = 1.0f;
            // RT64's rect vertices carry NDC z = 0 (rectPosFloats).
            data[i * 10 + 9] = 0.0f;
        }

        DrawCmd cmd;
        cmd.kind = 2;
        record_state(cmd, st, texture_available(st));
        if (s2d_logged_ < 3) {
            ++s2d_logged_;
            OGRE_MILESTONE("GFX-S2D",
                           "obj rect fmt=%u siz=%u img=%.0fx%.0f stride=%u adrs=%u mtx=(%.1f,%.1f,%.4f,%.4f) "
                           "rect=(%.1f,%.1f)-(%.1f,%.1f) textured=%d key=0x%llX texaddr=0x%08X",
                           (unsigned)s.imageFmt, (unsigned)s.imageSiz, imageW, imageH,
                           (unsigned)s.imageStride, (unsigned)s.imageAdrs, mtxX, mtxY, bsX, bsY,
                           x0, y0, x1, y1, cmd.textured ? 1 : 0,
                           (unsigned long long)cmd.tex_key, t.image_address);
        }
        queue_triangles(ctx, cmd, data, 6);
        st.active_tile = saved_tile;
    }

    void draw_texrect(ExecCtx* ctx) {        if (ctx->order.size() < 200) ctx->order += 'T';
        RenderState& st = *ctx->st;
        // A rectangle samples the tile it names, not G_TEXTURE's (RT64
        // RDP::drawTexRect takes the tile as a parameter). Scope the override
        // to this draw.
        const int saved_tile = st.active_tile;
        st.active_tile = texrect_tile_ & (kMaxTiles - 1);
        const int xl = texrect_geom_[0], yl = texrect_geom_[1];
        const int xh = texrect_geom_[2], yh = texrect_geom_[3];
        // A TEXRECT's lower-right corner is exclusive: the RDP covers
        // `[ul, lr)` in 1/4-pixel units and does not snap the corner up the way
        // G_CYC_FILL/G_CYC_COPY fill rects do (`RDP::fillRect` does `lrx |= 3;
        // lry |= 3`, `RDP::drawRect` for a texrect does not). So a rectangle
        // from (0,0) to (640,480) - how the game draws its 640x480 publisher
        // stills - covers rows 0..479 of a 640x480 colour image. Covering
        // `xh`/`yh` inclusive instead made every TEXRECT one pixel wider and
        // taller than the hardware's, and on the 640-wide targets the extra row
        // landed past the end of the colour image: the writeback copied it into
        // the next heap block's allocator metadata, and the game's allocator
        // then panicked (see docs/HANDOFF-2026-10-14-session142.md).
        // `FixedRect::isEmpty` (`tools/RT64/src/common/rt64_common.cpp:34`) is
        // `lrx == ulx || lry == uly`, so an equal corner draws nothing.
        if (xh <= xl || yh <= yl) {
            st.active_tile = saved_tile;
            return;  // empty rectangle: the RDP draws nothing (see draw_fill_rect)
        }
        // A tile with no texels cannot be sampled. `lrs == uls` or `lrt == ult`
        // means the tile covers zero texels (a one-texel-wide tile is
        // `lrs == uls + 4`), so the rectangle has nothing to fetch; decoding it
        // as a one-texel image samples a stale row instead. Ogre Battle 64
        // leaves exactly such a tile behind at the end of its title-screen
        // cloud-layer loops and then draws six right-to-left rectangles with
        // it, which renders as an opaque white band across the scene. This
        // mirrors the same guard in RT64 `RDP::drawTexRect`. A tile with
        // `line == 0` is not configured at all and keeps the existing
        // "no texture" handling.
        {
            const TileState& texTile = st.tiles[st.active_tile & (kMaxTiles - 1)];
            if ((texTile.line != 0) &&
                ((texTile.lrs == texTile.uls) || (texTile.lrt == texTile.ult))) {
                st.active_tile = saved_tile;
                return;
            }
        }
        const int16_t s0 = static_cast<int16_t>((ctx->rdp_half1 >> 16) & 0xFFFF);
        const int16_t t0 = static_cast<int16_t>(ctx->rdp_half1 & 0xFFFF);
        const int16_t dsdx = static_cast<int16_t>((ctx->rdp_half2 >> 16) & 0xFFFF);
        const int16_t dtdy = static_cast<int16_t>(ctx->rdp_half2 & 0xFFFF);
        {
            const TileState& wt = st.tiles[st.active_tile & (kMaxTiles - 1)];
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "tile=%d addr=0x%06X stride=%d fmt=%u siz=%u line=%d "
                     "uls=%u ult=%u lrs=%u lrt=%u s0=%d t0=%d dsdx=%d dtdy=%d",
                     st.active_tile, wt.image_address, wt.image_stride, (unsigned)wt.fmt,
                     (unsigned)wt.siz, (int)wt.line, (unsigned)wt.uls, (unsigned)wt.ult,
                     (unsigned)wt.lrs, (unsigned)wt.lrt, s0, t0, dsdx, dtdy);
            log_wide_draw(st, "texrect", xl, yl, xh, yh, detail);
        }

        if (txr_logged_ < 3) {
            ++txr_logged_;
            ensure_tile_image(ctx, st, st.active_tile, 0);
            ensure_tile_image(ctx, st, st.active_tile + 1, 1);
            char muxbuf[256];
            format_combiner(st.combiner, ((st.othermode_h >> 20) & 3) == kCyc2 ? 0 : 1,
                            muxbuf, sizeof(muxbuf));
            OGRE_MILESTONE("GFX-TXR",
                       "texrect xl=%d yl=%d xh=%d yh=%d s0=%d t0=%d dsdx=%d dtdy=%d flip=%d key=0x%llX "
                       "omh=0x%06X oml=0x%06X cyc=%d mux0=[%s] texel0=(%d,%d,%d,%d) "
                       "fmt=%d siz=%d line=%d sh=%d/%d mask=%d/%d",
                       xl, yl, xh, yh, s0, t0, dsdx, dtdy,
                       ctx->pending_texrect_flip ? 1 : 0,
                       (unsigned long long)st.tiles[st.active_tile & (kMaxTiles - 1)].image.key,
                       st.othermode_h, st.othermode_l,
                       static_cast<int>((st.othermode_h >> 20) & 3),
                       muxbuf,
                       last_tex_rgba_[0], last_tex_rgba_[1], last_tex_rgba_[2], last_tex_rgba_[3],
                       (int)last_tex_fmt_, (int)last_tex_siz_, (int)last_tex_line_,
                       (int)last_tex_shifts_, (int)last_tex_shiftt_,
                       (int)last_tex_masks_, (int)last_tex_maskt_);
        }
        if ((g_debug_flags.load(std::memory_order_relaxed) & 4u) != 0 &&
            (xh - xl) >= 299 && (yh - yl) >= 219) {
            st.active_tile = saved_tile;
            return;   // debug: show the frame without the intro's fade-in
        }
        // Decode the two tile images first: texture_available() reads the
        // result, and a rect that samples a tile loaded straight into it (the
        // synthetic test_draw) has no other place to pick the image up.
        ensure_tile_image(ctx, st, st.active_tile, 0);
        ensure_tile_image(ctx, st, st.active_tile + 1, 1);
        // Only a rectangle whose combiner reads a texel needs one. RT64
        // `RDP::drawTexRect` has no "no texture" test at all: the sampler is
        // simply never read when the mux does not name TEXEL0/TEXEL1, so the
        // primitive colour is drawn. Dropping every rectangle with an
        // unconfigured tile removed the publisher stills' PRIM-only background
        // bands.
        {
            const bool needs_texture =
                combiner_uses_texture(st.combiner, st.othermode_h, 0) ||
                combiner_uses_texture(st.combiner, st.othermode_h, 1);
            if (!texture_available(st) && needs_texture) {
                st.active_tile = saved_tile;
                return;
            }
        }

        const float us = s0 / 32.0f;
        const float vs = t0 / 32.0f;
        // The per-pixel S/T step. RT64 turns the rectangle into a UV pair with
        // (tools/RT64/src/hle/rt64_rdp.cpp drawRect)
        //
        //     lrs = ((uls << 7) + dsdx * (rectWidth << 2)) >> 7
        //     u2  = lrs / 32        ->   u2 - u1 = dsdx * rectWidth / 1024
        //
        // so `dsdx` is in 1/1024 texel-units relative to the s10.5 origin: a
        // 320-texel image stretched over 640 pixels carries dsdx = 512, and
        // 512 / 1024 = 0.5 texels per pixel. Dividing by 32 instead - which is
        // what this did before session 141 - made the sampled span 32x too
        // large. On the title screen, whose 320x240 backdrop is drawn as forty
        // 640x12 `G_TEXRECT` strips, each strip then repeated its own six-row
        // source image about 35 times, which is the horizontal banding
        // `docs/proofs/web-title-640-before-uv-fix.png` shows. It never showed
        // up before because every scene the browser had rendered used triangles
        // or the S2DEX2 object rectangle, and neither goes through this path.
        const float du = dsdx / 1024.0f;
        const float dv = dtdy / 1024.0f;
        // The rectangle's own size in pixels. `lrx`/`lry` are exclusive, so the
        // destination is `xh - xl` by `yh - yl` pixels - the same count the S/T
        // step spans (`u2 - u1 = dsdx * rectWidth / 1024`), which is why the
        // title screen's 640x12 strips are 12 rows and not 13.
        const float w = static_cast<float>(xh - xl);
        const float h = static_cast<float>(yh - yl);

        // UV at the four corners. TEXRECTFLIP swaps the S/T axes.
        float uv[4][2];
        if (ctx->pending_texrect_flip) {
            uv[0][0] = us;              uv[0][1] = vs;                       // (xl, yl)
            uv[1][0] = us + dv * h;     uv[1][1] = vs;                       // (xh, yl)
            uv[2][0] = us;              uv[2][1] = vs + du * w;              // (xl, yh)
            uv[3][0] = us + dv * h;     uv[3][1] = vs + du * w;              // (xh, yh)
        } else {
            uv[0][0] = us;              uv[0][1] = vs;                       // (xl, yl)
            uv[1][0] = us + du * w;     uv[1][1] = vs;                       // (xh, yl)
            uv[2][0] = us;              uv[2][1] = vs + dv * h;              // (xl, yh)
            uv[3][0] = us + du * w;     uv[3][1] = vs + dv * h;              // (xh, yh)
        }

        const float x0 = ndc_x(xl, st), x1 = ndc_x(xh, st);
        const float y0 = ndc_y(yl, st), y1 = ndc_y(yh, st);
        const float px[4] = {x0, x1, x0, x1};
        const float py[4] = {y0, y0, y1, y1};

        float data[6 * 10];
        const int idx[6] = {0, 1, 3, 0, 3, 2};
        for (int i = 0; i < 6; ++i) {
            const int c = idx[i];
            data[i * 10 + 0] = px[c];
            data[i * 10 + 1] = py[c];
            data[i * 10 + 2] = uv[c][0];
            data[i * 10 + 3] = uv[c][1];
            data[i * 10 + 4] = 1;
            data[i * 10 + 5] = 1;
            data[i * 10 + 6] = 1;
            data[i * 10 + 7] = 1;
            data[i * 10 + 8] = 1.0f;
            // RT64's rect vertices carry NDC z = 0 (rectPosFloats).
            data[i * 10 + 9] = 0.0f;
        }

        DrawCmd cmd;
        cmd.kind = 2;
        record_state(cmd, st, texture_available(st));
        // The mission intro's framed illustration is a band of 2D rectangles
        // (its strips, picture and pin). State them once each so a browser/native
        // difference on that band can be read from the draw's own tile source,
        // key and combiner instead of inferred from the decoded ring.
        if (ogre_web_active_scene() == 3 && frame_rect_logged_ < 40 &&
            xl >= 85 && xl <= 230 && yl >= 55 && yl <= 155) {
            ++frame_rect_logged_;
            const TileState& ft = st.tiles[st.active_tile & (kMaxTiles - 1)];
            char muxbuf[256];
            format_combiner(st.combiner, ((st.othermode_h >> 20) & 3) == kCyc2 ? 0 : 1,
                            muxbuf, sizeof(muxbuf));
            OGRE_MILESTONE("GFX-FRAME-RECT",
                           "rect=%d,%d..%d,%d src=0x%06X fmt=%u siz=%u line=%d "
                           "uls=%u ult=%u lrs=%u lrt=%u mask=%d/%d cm=%d/%d sh=%d/%d "
                           "s0=%d t0=%d dsdx=%d dtdy=%d key=0x%llX omh=0x%06X oml=0x%08X "
                           "comb=%08X/%08X mux=[%s] texel0=(%d,%d,%d,%d) textured=%d",
                           xl, yl, xh, yh, ft.image_address, (unsigned)ft.fmt,
                           (unsigned)ft.siz, (int)ft.line, (unsigned)ft.uls,
                           (unsigned)ft.ult, (unsigned)ft.lrs, (unsigned)ft.lrt,
                           (int)ft.masks, (int)ft.maskt, (int)ft.cms, (int)ft.cmt,
                           (int)ft.shifts, (int)ft.shiftt, s0, t0, dsdx, dtdy,
                           (unsigned long long)ft.image.key, st.othermode_h, st.othermode_l,
                           st.combiner.L, st.combiner.H, muxbuf,
                           last_tex_rgba_[0], last_tex_rgba_[1], last_tex_rgba_[2],
                           last_tex_rgba_[3], cmd.textured ? 1 : 0);
            // The strip signature: an RGBA16 tile 92 texels wide (lrs 364) and
            // 4 tall (lrt 12), i.e. one 4-row band of the 92x60 picture. The
            // first two write their source bytes and TMEM.
            if (frame_dump_enabled_ && frame_dump_count_ < 2 && ft.fmt == gbi::IM_FMT_RGBA &&
                ft.siz == gbi::IM_SIZ_16b && ft.lrs == 364 && ft.lrt == 12) {
                dump_frame_source(ft);
            }
        }
        // A repaired fog group's sampler state: what the repair installed
        // (size, wrap mask, filter), and the rectangle it drew. The remaining
        // browser/native difference is on this layer's translucent edge, so a
        // run states the state that produced it instead of leaving it to
        // inference from the decoded image alone.
        if (ctx->fog_repaired > 0 && cmd.textured && cmd.tex_masks[0] > 0 &&
            fog_draw_logged_ < 8) {
            ++fog_draw_logged_;
            OGRE_MILESTONE("GFX-FOG-DRAW",
                           "rect=(%d,%d)-(%d,%d) size=%.0fx%.0f filt=%d cm=(%d,%d) "
                           "mask=(%d,%d) s=%.2f..%.2f t=%.2f..%.2f orig=(%.0f,%.0f) key=0x%llX",
                           xl, yl, xh, yh, cmd.tex_size[0][0], cmd.tex_size[0][1],
                           cmd.tex_filter, cmd.tex_cms[0], cmd.tex_cmt[0],
                           cmd.tex_masks[0], cmd.tex_maskt[0],
                           uv[0][0], uv[1][0], uv[0][1], uv[2][1],
                           cmd.tex_origin[0], cmd.tex_origin[1],
                           (unsigned long long)cmd.tex_key);
        }
        if (rect_logged_ < 8) {
            ++rect_logged_;
            OGRE_MILESTONE("GFX-RECT",
                           "texrect (%d,%d)-(%d,%d) tile=%d cyc=%d textured=%d ablend=%d pri=%d "
                           "omh=0x%06X oml=0x%08X prim=[%.2f,%.2f,%.2f,%.2f] scissor=%d(%d,%d,%d,%d) "
                           "key=0x%llX key1=0x%llX s0=%d t0=%d dsdx=%d dtdy=%d",
                           xl, yl, xh, yh, st.active_tile, cmd.cycle, cmd.textured ? 1 : 0,
                           cmd.alpha_blend ? 1 : 0, cmd.combiner_cycles,
                           st.othermode_h, st.othermode_l,
                           cmd.prim[0], cmd.prim[1], cmd.prim[2], cmd.prim[3],
                           cmd.scissor_on ? 1 : 0, cmd.sx0, cmd.sy0, cmd.sx1, cmd.sy1,
                           (unsigned long long)cmd.tex_key, (unsigned long long)cmd.tex_key1,
                           s0, t0, dsdx, dtdy);
        }
        if (trace_active() && trace_draws_ < kTraceMaxDraws) {
            ++trace_draws_;
            const TileState& tt = st.tiles[st.active_tile & (kMaxTiles - 1)];
            const int px_w = (xh > xl) ? (xh - xl) : 0;
            const int px_h = (yh > yl) ? (yh - yl) : 0;
            // The texel window the rectangle walks, in whole texels, the same
            // way the native trace computes it: s and t are in 1/32 texel and
            // dsdx/dtdy change by 1/1024 texel per pixel.
            const int s_end = s0 + (dsdx * px_w) / 32;
            const int t_end = t0 + (dtdy * px_h) / 32;
            const int s_a = s0 / 32, s_b = s_end / 32;
            const int t_a = t0 / 32, t_b = t_end / 32;
            OGRE_MILESTONE("GFX-TEXTRACE",
                           "n=%u dl=%u rect=%d,%d..%d,%d px=%dx%d s=%d t=%d dsdx=%d dtdy=%d "
                           "window=uls%u ult%u lrs%u lrt%u tile=%d line%u pal%u "
                           "img=0x%06X fmt=%u siz=%u stride=%d tlut=%s ntlut=%u "
                           "texels=%dx%d dec=%dx%d uv0=(%.3f,%.3f) uv3=(%.3f,%.3f) "
                           "mean=%.1f opaque=%u oml=0x%06X lut=%u",
                           trace_draws_, ctx->trace_task, xl, yl, xh, yh, px_w, px_h, s0, t0, dsdx, dtdy,
                           (unsigned)tt.uls, (unsigned)tt.ult, (unsigned)tt.lrs, (unsigned)tt.lrt,
                           st.active_tile, (unsigned)tt.line, (unsigned)tt.palette,
                           tt.image_address, (unsigned)tt.fmt, (unsigned)tt.siz, tt.image_stride,
                           tlut_valid_ ? "yes" : "no", tlut_count_,
                           (s_b > s_a ? s_b - s_a : s_a - s_b) + 1,
                           (t_b > t_a ? t_b - t_a : t_a - t_b) + 1,
                           tt.image_valid ? tt.image.width : 0,
                           tt.image_valid ? tt.image.height : 0, uv[0][0], uv[0][1], uv[3][0],
                           uv[3][1], tt.trace_mean, tt.trace_opaque, st.othermode_l,
                           (st.othermode_h >> 14) & 3u);
            // The palette generation does not tell a loaded palette from an
            // empty one: `tlut_valid_` is set by the first G_LOADTLUT whatever
            // it read. Print the entries the decode used and the colours the
            // decode produced, for the first few draws of a run.
            if (trace_draws_ <= 8) {
                OGRE_MILESTONE("GFX-TEXTRACE",
                               "  tlut gen=%llu n=%u e0=%04X e1=%04X e2=%04X e3=%04X "
                               "e113=%04X e230=%04X",
                               (unsigned long long)tlut_generation_, (unsigned)tlut_count_,
                               tlut_[0], tlut_[1], tlut_[2], tlut_[3], tlut_[113], tlut_[230]);
                if (tt.image.hist.empty() == false) {
                    // The four most common decoded colours and their texel
                    // counts.
                    std::vector<std::pair<uint32_t, uint32_t>> h = tt.image.hist;
                    std::sort(h.begin(), h.end(),
                              [](const std::pair<uint32_t, uint32_t>& a,
                                 const std::pair<uint32_t, uint32_t>& b) {
                                  return a.second > b.second;
                              });
                    char buf[192];
                    int off = 0;
                    const size_t shown = h.size() < 4 ? h.size() : 4;
                    for (size_t i = 0; i < shown; ++i) {
                        off += snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                                        " %06X:%u", h[i].first, h[i].second);
                    }
                    OGRE_MILESTONE("GFX-TEXTRACE", "  decoded colours:%s (%u distinct)", buf,
                                   (unsigned)h.size());
                }
            }
            if (trace_draws_ == kTraceMaxDraws) {
                OGRE_MILESTONE("GFX-TEXTRACE", "trace limit %u reached; %u G_LOADTLUT(s) seen",
                               kTraceMaxDraws, trace_tluts_);
            }
        }
        queue_triangles(ctx, cmd, data, 6);
        st.active_tile = saved_tile;
    }

    // Screen (pixel) -> NDC. The denominator is the size of the *render target*
    // the draw lands in, because the RDP's screen coordinates are framebuffer
    // pixels: the RSP viewport only decides where inside that target the
    // geometry goes.
    //
    // For a colour image the width is the image's own `G_SETCIMG` width, which is
    // the guest's statement of the target size (320 or 640; session 139 §1 lists
    // all six configurations). The RSP viewport is the fallback for a display
    // list that never sets one, and the VI's scanout size the fallback after
    // that. Before session 141 the denominator was the viewport alone, so the
    // world map's 640-wide targets - whose display lists do not all set a
    // viewport - clamped every coordinate past x=319.
    //
    // The viewport scale is in 14.2 fixed pixels and OB64 submits
    // vscale = [640 480 511 0] for its 320x240 screen, so `vscale / 4` is half
    // the screen and `vscale * 2 / 4` is the screen.
    int target_width_px(const RenderState& st) const {
        if (st.cimg_address != 0 && st.cimg_width >= 1 &&
            st.cimg_width <= kMaxFramebufferSide) {
            return st.cimg_width;
        }
        if (st.viewport_set && st.view_scale[0] * 2.0f >= 1.0f) {
            return static_cast<int>(std::lround(st.view_scale[0] * 2.0f));
        }
        const ViSize vi = vi_size();
        if (vi.width > 0 && vi.width <= kMaxFramebufferSide) {
            return vi.width;
        }
        return canvas_width_;
    }

    // The target's height is not in any RDP command. RT64 takes
    // `max(drawn bottom, VI height)` (`framebufferHeightForDisplay`,
    // tools/RT64/src/hle/rt64_framebuffer.h), so the same three inputs are used
    // here: the VI's scanout height, the viewport's screen height, and the bottom
    // row the current scissor allows. A draw that goes to the canvas keeps the
    // canvas height, because the canvas is what its NDC maps onto.
    int target_height_px(const RenderState& st) const {
        int h = 0;
        if (st.viewport_set && st.view_scale[1] * 2.0f >= 1.0f) {
            h = static_cast<int>(std::lround(st.view_scale[1] * 2.0f));
        }
        const ViSize vi = vi_size();
        h = std::max(h, vi.height);
        if (st.scissor_enabled) {
            h = std::max(h, st.scissor_y1 + 1);
        }
        if (st.cimg_address == 0) {
            return canvas_height_;
        }
        if (h < 1) {
            h = canvas_height_;
        }
        return std::min(h, kMaxFramebufferSide);
    }

    // The size of the last *visible* VI mode - RT64's VIHistory top, which is
    // what the native renderer sizes its framebuffers from. Set by update_screen
    // on the gfx thread (packed as width << 32 | height so a reader on the main
    // thread cannot see half of an update).
    ViSize vi_size() const {
        const uint64_t packed = vi_scanout_.load(std::memory_order_relaxed);
        ViSize out;
        out.width = static_cast<int>(packed >> 32);
        out.height = static_cast<int>(packed & 0xFFFFFFFFu);
        return out;
    }

    float screen_width(const RenderState& st) const {
        return static_cast<float>(target_width_px(st));
    }
    float screen_height(const RenderState& st) const {
        return static_cast<float>(target_height_px(st));
    }
    float ndc_xf(float x, const RenderState& st) const {
        return 2.0f * x / screen_width(st) - 1.0f;
    }
    float ndc_yf(float y, const RenderState& st) const {
        return 1.0f - 2.0f * y / screen_height(st);
    }
    float ndc_x(int x, const RenderState& st) const {
        return ndc_xf(static_cast<float>(x), st);
    }
    float ndc_y(int y, const RenderState& st) const {
        return ndc_yf(static_cast<float>(y), st);
    }

    // ----- matrix helpers ------------------------------------------------------

    // G_MOVEMEM MV_VIEWPORT: reads the RSP viewport (Vp_t) and stores the
    // resulting screen-space transform. `Vp_t` is
    //     s16 vscale[4];   // x, y, z, 0 in 14.2 fixed
    //     s16 vtrans[4];   // x, y, z, 0 in 14.2 fixed
    // OB64 submits vscale = [640 480 511 0] for its 320x240 screen - the
    // canonical (x, y, z, pad) order - and RT64 maps index 0/1/2 to x/y/z
    // (RSP::setViewport in hle/rt64_rsp.cpp).
    //
    // The reads go through n64h(), not a plain rd16(): the runtime's rdram is
    // word-byte-reversed, so reading a halfword at its logical offset returns
    // the *other* half of the word (see n64h's comment). The old code read with
    // rd16() and then took x from index 1, y from index 0 and z from index 3,
    // which cancelled that swap by hand: it logged vscale=[480 640 0 511] and
    // was correct only because of the index juggling. Nothing below relies on
    // the swap any more. The renderer applied no viewport at all before session
    // 20, which is why 3D geometry landed in raw clip space (ndc_x up to 3.25)
    // with most of it off-screen.
    void set_viewport(ExecCtx* ctx, RenderState& st, uint32_t addr) {
        int16_t vscale[4], vtrans[4];
        for (int i = 0; i < 4; ++i) {
            vscale[i] = static_cast<int16_t>(n64h(rdram_, static_cast<int>(addr) + i * 2));
            vtrans[i] = static_cast<int16_t>(n64h(rdram_, static_cast<int>(addr + 8) + i * 2));
        }
        st.view_scale[0] = static_cast<float>(vscale[0]) / 4.0f;  // x
        st.view_scale[1] = static_cast<float>(vscale[1]) / 4.0f;  // y
        // The depth axis is not 14.2 screen pixels: RT64 divides it by 1024
        // (`DepthRange`, hle/rt64_rsp.cpp:23; `viewport.scale.z = vp->vscale[3]
        // / DepthRange` and `translate.z = vp->vtrans[3] / DepthRange`, :908-911).
        // RT64's struct read is byte-reversed, so its `vscale[3]` is the guest's
        // third element, which is the value this code holds in `vscale[2]`: the
        // `[GFX-VP]` line for the mission's own Vp_t
        // (`0x00186330`, vscale=[640 480 511 0]) shows x, y, z, pad in array
        // order, and index 2 is the 511. The 511/1024 = 0.499 range is
        // independent of the /4 the x/y axes use; the old `/ 4` on this axis
        // gave a z scale of 127.75.
        st.view_scale[2] = static_cast<float>(vscale[2]) / 1024.0f;   // z
        st.view_trans[0] = static_cast<float>(vtrans[0]) / 4.0f;  // x
        st.view_trans[1] = static_cast<float>(vtrans[1]) / 4.0f;  // y
        st.view_trans[2] = static_cast<float>(vtrans[2]) / 1024.0f;   // z
        st.viewport_set = true;
        if (ctx->trace_task == 2 && ctx->vp_logged < 4) {
            ctx->vp_logged++;
            OGRE_MILESTONE("GFX-VP",
                           "viewport addr=0x%08X vscale=[%d %d %d %d] vtrans=[%d %d %d %d] "
                           "-> scale=[%.2f %.2f %.2f] trans=[%.2f %.2f %.2f]",
                           addr, vscale[0], vscale[1], vscale[2], vscale[3],
                           vtrans[0], vtrans[1], vtrans[2], vtrans[3],
                           st.view_scale[0], st.view_scale[1], st.view_scale[2],
                           st.view_trans[0], st.view_trans[1], st.view_trans[2]);
        }
    }

    Mat4 read_matrix(uint32_t addr) {
        // N64 `Mtx` layout: a 4x4 array of s16 integer parts followed by a 4x4
        // array of u16 fraction parts (64 bytes total), decoded as
        // int + frac/65536. Two things have bitten this decoder:
        //
        //  1. Reading 16 consecutive s16 values only ever read the integer
        //     halves, so any matrix without a non-zero integer part decoded as
        //     all zeros (session 19).
        //  2. The runtime's rdram holds N64 words byte-reversed, so a plain
        //     16-bit read returns each row's columns in the order 1,0,3,2.
        //     RT64 compensates with `j ^ 1`
        //     (FixedMatrix::toFloat in common/rt64_common.cpp); without it
        //     every matrix here came out transposed in column pairs, which is
        //     why the composed projection looked nothing like a perspective
        //     matrix and most triangles were rejected as "behind the camera".
        Mat4 m{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const int i = r * 4 + (c ^ 1);
                const int16_t int_part = static_cast<int16_t>(rd16(rdram_ + addr + i * 2));
                const uint16_t frac_part = rd16(rdram_ + addr + 32 + i * 2);
                m.m[r * 4 + c] = static_cast<float>(int_part) + static_cast<float>(frac_part) / 65536.0f;
            }
        }
        return m;
    }

    static Mat4 mul_mat4(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    sum += a.m[i * 4 + k] * b.m[k * 4 + j];
                }
                r.m[i * 4 + j] = sum;
            }
        }
        return r;
    }

    // Geometry cache for the pending texrect (set by G_TEXRECT, consumed at
    // the following G_RDPHALF_2). A rectangle carries its own tile, which need
    // not be G_TEXTURE's (RT64 RDP::drawTexRect takes it as a parameter).
    int texrect_geom_[4] = {};
    int texrect_tile_ = 0;
    // Diagnostics for the texrect path (last load only).
    uint8_t last_tex_rgba_[4] = {0, 0, 0, 0};
    uint8_t last_tex_fmt_ = 0, last_tex_siz_ = 0;
    uint16_t last_tex_line_ = 0;
    uint8_t last_tex_shifts_ = 0, last_tex_shiftt_ = 0;
    uint8_t last_tex_masks_ = 0, last_tex_maskt_ = 0;
};

WebGLRenderer* g_active_renderer = nullptr;

// The canvas handoff can arrive before the gfx thread creates the renderer
// (JS runs ahead of the boot pthread), so stash it and apply it in
// create_renderer.
struct PendingCanvas {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle = 0;
    int width = 320;
    int height = 240;
};
PendingCanvas g_pending_canvas;

}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)window_handle;
    (void)developer_mode;
    auto renderer = std::make_unique<WebGLRenderer>();
    renderer->set_rdram(rdram);
    if (g_pending_canvas.handle != 0) {
        renderer->set_canvas(g_pending_canvas.handle, g_pending_canvas.width, g_pending_canvas.height);
        g_pending_canvas = {};
    }
    g_active_renderer = renderer.get();
    return renderer;
}

}  // namespace ogre

extern "C" {

// Creates the WebGL2 context on the #game-canvas element. Called from web.js
// on the browser main thread (contexts must be created there for GL proxying
// from the gfx pthread to work). Returns the Emscripten context handle.
int ogre_gfx_create_context(int width, int height) {
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.alpha = false;
    attrs.depth = false;
    attrs.stencil = false;
    attrs.antialias = false;
    attrs.preserveDrawingBuffer = true;
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle =
        emscripten_webgl_create_context("#game-canvas", &attrs);
    if (handle <= 0) {
        fprintf(stderr, "[web-renderer] emscripten_webgl_create_context failed (%ld)\n",
                (long)handle);
        return 0;
    }
    emscripten_set_canvas_element_size("#game-canvas", width, height);
    return static_cast<int>(handle);
}

// Called from web.js after it creates the WebGL2 context on the main thread.
// The renderer uses the handle from the gfx pthread (GL calls are proxied).
void ogre_gfx_set_canvas(int handle, int width, int height) {
    if (ogre::g_active_renderer != nullptr) {
        ogre::g_active_renderer->set_canvas(handle, width, height);
    } else {
        // The renderer is not created yet (boot pthread); stash for later.
        ogre::g_pending_canvas.handle = static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(handle);
        ogre::g_pending_canvas.width = width;
        ogre::g_pending_canvas.height = height;
    }
}

// Returns the accumulated graphics-workload summary (milestone 6).
const char* ogre_gfx_stats() {
    static char buf[8192];
    {
        std::lock_guard<std::mutex> lock(ogre::g_stats_mutex);
        snprintf(buf, sizeof(buf),
                 "%s\nexec: task=%u cmd=%llu @0x%05X op=0x%02X draws=%u flush_ok=%u flush_skip=%u flushed_cmds=%u"
                 "\nload: seq=%u done=%u w=%u h=%u fmt=%u siz=%u timgw=%u addr=0x%08X"
                 "\nfbtex: hits=%u copies=%u (a texture load overlapped a colour image the "
                 "RDP had drawn; copies served it from the GPU, OGRE_FBTEX=1)"
                 "\ntex: cache=%zu bytes=%zu uploads=%zu reused=%zu released=%zu "
                 "decodes=%zu decode_hits=%zu (tile images; age and byte bounded)",
                 ogre::g_stats_snapshot,
                 ogre::g_exec_task.load(std::memory_order_relaxed),
                 (unsigned long long)ogre::g_exec_cmd.load(std::memory_order_relaxed),
                 ogre::g_exec_off.load(std::memory_order_relaxed),
                 ogre::g_exec_op.load(std::memory_order_relaxed),
                 ogre::g_exec_draws.load(std::memory_order_relaxed),
                 ogre::g_flush_ok.load(std::memory_order_relaxed),
                 ogre::g_flush_skip.load(std::memory_order_relaxed),
                 ogre::g_flush_cmds.load(std::memory_order_relaxed),
                 ogre::g_load_seq.load(std::memory_order_relaxed),
                 ogre::g_load_done.load(std::memory_order_relaxed),
                 ogre::g_load_w.load(std::memory_order_relaxed),
                 ogre::g_load_h.load(std::memory_order_relaxed),
                 ogre::g_load_fmt.load(std::memory_order_relaxed),
                 ogre::g_load_siz.load(std::memory_order_relaxed),
                 ogre::g_load_timgw.load(std::memory_order_relaxed),
                 ogre::g_load_addr.load(std::memory_order_relaxed),
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->fbtex_hits() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->fbtex_syncs() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_cache_size() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_cache_bytes() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_uploads() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_reused() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_released() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_decodes() : 0u,
                 ogre::g_active_renderer != nullptr ? ogre::g_active_renderer->texture_decode_hits() : 0u);
    }
    return buf;
}

// Drains the queued draw/texture commands and issues the GL calls. Called
// periodically from web.js on the browser main thread (never from a pthread).
int ogre_gfx_flush() {
    // The browser build has no `sdl_platform.cpp`, so the per-frame work that
    // hangs off the native `update_gfx` callback runs here instead: the scene
    // poll (`OGRE_SCENE` forcing, `OGRE_SCENE_LOG`, `OGRE_SCENE_TRACE`) and the
    // timed RDRAM dump (`OGRE_RAM_KEY_AT_MS` with `OGRE_RAM_KEY_FULL=1`). Both
    // only read guest state or write a file in the wasm filesystem, and RDRAM is
    // shared memory, so the main thread can call them. Without this the browser
    // run had no scene log and no way to write guest memory to disk (session 141
    // left the `OGRE_RAM_KEY_AT_MS` path unverified in the browser).
    if (uint8_t* rdram = ultramodern::get_rdram_base(); rdram != nullptr) {
        ogre::poll_scene();
        ogre::ram_key_tick(rdram);
    }
    if (ogre::g_active_renderer != nullptr) {
        ogre::g_active_renderer->flush_commands();
        return 1;
    }
    return 0;
}

// Test hook: records a synthetic DL (fill rect + textured rect) to validate
// the render pipeline while the game submits only its boot blanking DL.
int ogre_gfx_test_draw() {
    if (ogre::g_active_renderer != nullptr) {
        ogre::g_active_renderer->test_draw();
        return 1;
    }
    return 0;
}

// Debug hook for the probes: copies decoded texture `index` (0 = oldest of the
// kept 8) into a static RGBA8 buffer and reports its size. Returns the pointer,
// or null when the index is out of range. Used to inspect exactly what the
// renderer decoded for a tile (see debug/probes/textures.cjs).
const uint8_t* ogre_gfx_debug_tex(int index, int* out_width, int* out_height,
                                  int* out_tile, int* out_fmt, int* out_siz) {
    static std::vector<uint8_t> out;
    if (ogre::g_active_renderer == nullptr) {
        return nullptr;
    }
    if (!ogre::g_active_renderer->debug_texture_copy(index, out, out_width, out_height,
                                                     out_tile, out_fmt, out_siz)) {
        return nullptr;
    }
    return out.data();
}

// Debug switches for the probes (bit 0: ignore alpha blending).
// The last framebuffer-source copy (`OGRE_FBTEX`): returns its pointer and
// stores its length in `len`. The bytes are the image's physical layout, the
// same bytes a texture load reads through `LoadSource::shadow`.
extern "C" EMSCRIPTEN_KEEPALIVE const uint8_t* ogre_gfx_debug_fb_source(int* len) {
    const uint8_t* data = nullptr;
    int n = 0;
    if (ogre::g_active_renderer != nullptr &&
        ogre::g_active_renderer->debug_fb_source_copy(&data, &n)) {
        if (len != nullptr) {
            *len = n;
        }
        return data;
    }
    if (len != nullptr) {
        *len = 0;
    }
    return nullptr;
}

void ogre_gfx_debug_flags(unsigned flags) {
    ogre::g_debug_flags.store(flags, std::memory_order_relaxed);
}

// Debug hook: the two images (0 = TEXEL0, 1 = TEXEL1) the most recent textured
// draw sampled. This is the pair a rendered sprite must be compared against;
// ogre_gfx_debug_tex's ring can span frames.
const uint8_t* ogre_gfx_debug_last_tex(int unit, int* out_width, int* out_height,
                                       int* out_tile, int* out_fmt, int* out_siz) {
    static std::vector<uint8_t> out;
    if (ogre::g_active_renderer == nullptr) {
        return nullptr;
    }
    if (!ogre::g_active_renderer->debug_last_texture_copy(unit, out, out_width, out_height,
                                                          out_tile, out_fmt, out_siz)) {
        return nullptr;
    }
    return out.data();
}

// Number of decoded textures currently kept for inspection.
int ogre_gfx_debug_tex_count() {
    if (ogre::g_active_renderer == nullptr) {
        return 0;
    }
    return ogre::g_active_renderer->debug_texture_count();
}

// The njpeg readback asks the renderer to make its render targets visible in
// RDRAM before the CPU copies a framebuffer (see tools/njpeg_readback.py). The
// game renders into a guest colour image that this renderer keeps in a GL
// framebuffer object, so the bytes the CPU is about to read are still on the
// GPU until the browser main thread writes them back. This blocks the game
// thread on that handshake; `sync_done_` is set by ogre_gfx_flush().
void ogre_sync_framebuffers() {
    if (ogre::g_active_renderer != nullptr) {
        ogre::g_active_renderer->sync_framebuffers();
    }
}

}  // extern "C"
