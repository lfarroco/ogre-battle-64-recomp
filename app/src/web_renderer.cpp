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
//   - Renders directly to the canvas (no VI framebuffer indirection yet), so
//     framebuffer-as-texture and VI presentation pass-through are stubbed.
//   - TEXEL1/second-texture combiners fall back to TEXEL0.
//   - LOADBLOCK dxt (arbitrary texel spacing) is approximated.
//   - No lighting/texgen (OB64's title content is unlit 2D).

#include "renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <GLES3/gl3.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "gbi.hpp"
#include "milestones.hpp"

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
//   bit 2 = ignore full-screen rectangles (>= 300x220). OB64's intro draws a
//           full-screen PRIM-alpha rect *after* its sprites and animates the
//           alpha from opaque to transparent, i.e. a fade-in from black;
//           skipping it shows the frame at full brightness immediately.
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
    int width = 0, height = 0;        // loaded rect size in texels
    int origin_s = 0, origin_t = 0;   // tile uls/ult in texels (UV origin)
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

// Rendering state carried across a display list.
struct RenderState {
    // Matrices (RSP row-vector convention).
    Mat4 projection = Mat4::identity();
    std::vector<Mat4> modelview_stack{Mat4::identity()};

    // Geometry mode (G_GEOMETRYMODE/G_CLEARGEOMETRYMODE + OTHERMODE_H bits).
    uint32_t geometry_mode = 0;
    uint32_t othermode_h = 0;  // 24-bit; cycle type at bits 20-21
    uint32_t othermode_l = 0;

    // Vertices.
    RawVertex vertices[kMaxVertices]{};

    // Colors (ABGR bytes -> RGBA floats).
    float prim_color[4] = {1, 1, 1, 1};
    float env_color[4] = {1, 1, 1, 1};
    float fill_color[4] = {0, 0, 0, 1};
    // Blender inputs (G_SETFOGCOLOR / G_SETBLENDCOLOR).
    float fog_color[4] = {0, 0, 0, 1};
    float blend_color[4] = {0, 0, 0, 0};

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
    // Palette loaded by G_LOADTLUT (16-bit entries).
    uint16_t tlut[256]{};
    bool tlut_valid = false;

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
// offset); vertex reads did not.
inline uint16_t n64h(const uint8_t* p, int off) { return rd16(p + (off ^ 2)); }
inline uint8_t n64b(const uint8_t* p, int off) { return p[off ^ 3]; }

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1);
}

// The executor and the DL walker share one resolver (gbi.hpp) so a G_DL
// target cannot mean two different addresses.
inline uint32_t resolve_address(const uint32_t* segments, uint32_t addr) {
    return gbi::resolve_address(segments, addr);
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

// Decodes a rect [x0,x0+w) x [y0,y0+h) of the current texture image into
// RGBA8. `tlut` is used for CI formats; `palette` selects the CI4 TLUT bank.
// `row_bytes` is the source image's row stride in bytes. It is NOT derivable
// from the rect: the RDP loads TMEM with one row stride and samples it with
// another (OB64's mask is 16 bytes/row of 8-bit texels sampled as 32 4-bit
// texels, and a LOADBLOCK's stride is its dxt). RT64 uses
// `bytesPerRow = timg_width << siz >> 1` for the load and `line << 3` for the
// tile; the caller picks.
void decode_texture_rect(const uint8_t* rdram, uint32_t timg_address, uint8_t fmt, uint8_t siz,
                         int row_bytes, int x0, int y0, int w, int h, const uint16_t* tlut,
                         bool tlut_valid, uint8_t palette, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);

    if (row_bytes <= 0) {
        return;
    }
    const int bpp = bytes_per_texel(siz);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int tx = x0 + x;
            const int ty = y0 + y;
            uint8_t rgba[4] = {0, 0, 0, 255};

            if (siz == gbi::IM_SIZ_4b) {
                // Two texels per byte; byte index = (ty*row_bytes + tx/2).
                const uint32_t byte_off = static_cast<uint32_t>(ty * row_bytes + tx / 2);
                const uint8_t* p = rdram + timg_address + byte_off;
                const uint8_t byte = rd16(p);  // byte-swapped storage: byte = p[0]
                const uint8_t nib = (tx & 1) ? (byte & 0xF) : (byte >> 4);
                switch (fmt) {
                    case gbi::IM_FMT_IA: {
                        const uint8_t i = (nib >> 1) * 255 / 7;
                        const uint8_t a = (nib & 1) ? 255 : 0;
                        rgba[0] = rgba[1] = rgba[2] = i;
                        rgba[3] = a;
                        break;
                    }
                    case gbi::IM_FMT_I: {
                        // I4 is intensity only: the texel is (i,i,i,i), so the
                        // alpha carries the intensity too (RT64 I4ToFloat4).
                        // Without this, an I4/I8 image sampled as TEXEL0_ALPHA
                        // has alpha 1 everywhere and its mask does nothing.
                        const uint8_t i = nib * 255 / 15;
                        rgba[0] = rgba[1] = rgba[2] = rgba[3] = i;
                        break;
                    }
                    case gbi::IM_FMT_CI: {
                        if (tlut_valid) {
                            const uint16_t e = tlut[palette * 16 + nib];
                            if ((e >> 15) & 1) {
                                rgba16_to_rgba8(e, rgba);
                            } else {
                                ia16_to_rgba8(e, rgba);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            } else {
                const uint32_t byte_off = static_cast<uint32_t>(ty * row_bytes + tx * bpp);
                const uint8_t* p = rdram + timg_address + byte_off;
                switch (siz) {
                    case gbi::IM_SIZ_8b: {
                        const uint8_t v = p[0];
                        switch (fmt) {
                            case gbi::IM_FMT_IA: {
                                const uint8_t i = (v >> 4) * 255 / 15;
                                const uint8_t a = (v & 0xF) * 255 / 15;
                                rgba[0] = rgba[1] = rgba[2] = i;
                                rgba[3] = a;
                                break;
                            }
                            case gbi::IM_FMT_I:
                                // Intensity replicates into alpha (RT64 I8ToFloat4).
                                rgba[0] = rgba[1] = rgba[2] = rgba[3] = v;
                                break;
                            case gbi::IM_FMT_CI:
                                if (tlut_valid) {
                                    const uint16_t e = tlut[v];
                                    if ((e >> 15) & 1) {
                                        rgba16_to_rgba8(e, rgba);
                                    } else {
                                        ia16_to_rgba8(e, rgba);
                                    }
                                }
                                break;
                            default:
                                break;
                        }
                        break;
                    }
                    case gbi::IM_SIZ_16b: {
                        const uint16_t v = rd16(p);
                        if (fmt == gbi::IM_FMT_IA) {
                            ia16_to_rgba8(v, rgba);
                        } else {
                            rgba16_to_rgba8(v, rgba);
                        }
                        break;
                    }
                    case gbi::IM_SIZ_32b: {
                        // RGBA32: two 16-bit halves: (alpha,red) (green,blue).
                        const uint16_t hi = rd16(p);
                        const uint16_t lo = rd16(p + 2);
                        rgba[0] = static_cast<uint8_t>(hi & 0xFF);
                        rgba[1] = static_cast<uint8_t>(lo >> 8);
                        rgba[2] = static_cast<uint8_t>(lo & 0xFF);
                        rgba[3] = static_cast<uint8_t>(hi >> 8);
                        break;
                    }
                    default:
                        break;
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
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
out vec2 v_uv0;           // TEXEL0's tile
out vec2 v_uv1;           // TEXEL1's tile
out vec4 v_shade;
uniform vec2 u_uv_scale;   // tile 0: (1/tex_w, 1/tex_h)
uniform vec2 u_uv_origin;  // tile 0: (uls, ult)
uniform vec2 u_uv_scale1;  // tile 1
uniform vec2 u_uv_origin1;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv0 = (a_uv - u_uv_origin) * u_uv_scale;
    v_uv1 = (a_uv - u_uv_origin1) * u_uv_scale1;
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
precision mediump float;
in vec2 v_uv0;
in vec2 v_uv1;
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
// Blender / render mode (RT64 Blender::run in shared/rt64_blender.h). The P/M/A/B
// selector enums and the approximation codes are the CPU-side BlendPM/BlendA/
// BlendB/BlendApprox values.
uniform vec4 u_fog_color;
uniform vec4 u_blend_color;
uniform int u_bp0, u_bm0, u_ba0, u_bb0;
uniform int u_bp1, u_bm1, u_ba1, u_bb1;
uniform int u_force_blend;
uniform int u_blend_approx;
uniform int u_combiner_cycles;
// Debug: 1 = output TEXEL1's RGB raw, 2 = output TEXEL0's RGB raw. Set from
// ogre_gfx_debug_flags bits 3-4 by the probes; bypasses the combiner and the
// blender so the sampled texture can be compared with the decoded image.
uniform int u_debug_mode;

// The texel a TEXEL0/TEXEL1 selector samples in the cycle being evaluated.
// In the second cycle of a 2-cycle combiner the RDP swaps the two texel values
// (RT64 fromColorInput: secondCycle ? texVal1 : texVal0). Each unit carries its
// own UV transform, from its tile's uls/ult and loaded size.
vec4 tex_val(int sel, bool second) {
    bool use1 = (sel == 2) != second;
    return use1 ? texture(u_tex1, v_uv1) : texture(u_tex0, v_uv0);
}

// RT64 ColorCombiner::fromColorInput, sel = CSEL_*.
vec4 color_input(int sel, bool second, vec4 combined) {
    if (sel == 0) return combined;                              // COMBINED
    if (sel == 1 || sel == 2) return tex_val(sel, second);      // TEXEL0 / TEXEL1
    if (sel == 3) return u_prim;                                // PRIMITIVE
    if (sel == 4) return v_shade;                               // SHADE
    if (sel == 5) return u_env;                                 // ENVIRONMENT
    if (sel == 6 || sel == 7) return vec4(0.0);                 // KEY_CENTER/KEY_SCALE
    if (sel == 8) return vec4(combined.a);                      // COMBINED_ALPHA
    if (sel == 9) return vec4(tex_val(1, second).a);            // TEXEL0_ALPHA
    if (sel == 10) return vec4(tex_val(2, second).a);           // TEXEL1_ALPHA
    if (sel == 11) return vec4(u_prim.a);                       // PRIMITIVE_ALPHA
    if (sel == 12) return vec4(v_shade.a);                      // SHADE_ALPHA
    if (sel == 13) return vec4(u_env.a);                        // ENV_ALPHA
    if (sel == 14 || sel == 15) return vec4(0.0);               // LOD_FRACTION / PRIM_LOD_FRAC
    if (sel >= 16 && sel <= 18) return vec4(0.0);               // NOISE / K4 / K5
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
        // Simulate the hardware's numerator overflow with fmod.
        const float Overflow = 1.0 + 8.0 / 255.0;
        vec3 numerator = mod(from_input_pm(P, input_color) * a_multiplier +
                             from_input_pm(M, input_color) * b_multiplier, Overflow);
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

void main() {
    if (u_debug_mode == 1) {
        fragColor = vec4(texture(u_tex1, v_uv1).rgb, 1.0);
        return;
    }
    if (u_debug_mode == 2) {
        fragColor = vec4(texture(u_tex0, v_uv0).rgb, 1.0);
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
    // The blender turns the combiner output into the written pixel and, when it
    // reads the framebuffer, into the alpha the GL blend mixes with.
    fragColor = blender_run(c);
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
    std::vector<uint8_t> pixels;
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
    float fog[4] = {0, 0, 0, 1};
    float blend_color[4] = {0, 0, 0, 0};
    // Raw RDP state for the diagnostics (the numeric mux fields alone cannot
    // be checked against the game's own DL words without them).
    uint32_t othermode_h = 0, othermode_l = 0;
    uint32_t comb_L = 0, comb_H = 0;
    float prim[4] = {1, 1, 1, 1};
    float env[4] = {1, 1, 1, 1};
    bool textured = false;
    uint64_t tex_key = 0;    // TEXEL0's tile image
    uint64_t tex_key1 = 0;   // TEXEL1's tile image
    float tex_scale[2] = {1, 1};
    float tex_origin[2] = {0, 0};
    float tex_scale1[2] = {1, 1};
    float tex_origin1[2] = {0, 0};
    int tex_sc = 0, tex_tc = 0;   // raw G_TEXTURE scale fields (diagnostics)
    bool blend = false;
    bool scissor_on = false;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
};

class WebGLRenderer final : public ultramodern::renderer::RendererContext {
  public:
    WebGLRenderer() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        OGRE_MILESTONE("RENDERER", "webgl2 renderer pending canvas handoff");
    }

    ~WebGLRenderer() override = default;

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
        return false;
    }

    void enable_instant_present() override {}

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
        // G_TEXRECT (0,0)-(63,47), tile 0; RDPHALF_1 s=0,t=0; RDPHALF_2 dsdx=1,dtdy=1
        put32(o, 0xE4000000 | (63 << 2) << 12 | (47 << 2)); put32(o + 4, 0); o += 8;
        put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
        put32(o, 0xF1000000); put32(o + 4, 0x00010001); o += 8;
        // G_ENDDL
        put32(o, 0xDF000000); put32(o + 4, 0x00000000); o += 8;

        RenderState st;
        st.othermode_l = 0;
        st.othermode_h = 0;
        st.combiner.decode(0, 0);
        // execute_dl() records DrawCmds into the execution-local buffer; publish
        // them under a short try_lock. This hook runs on the browser main
        // thread, which must never block on the renderer mutex.
        ExecCtx ctx;
        execute_dl(rdram_, kDlOff, st, ctx);
        {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                fprintf(stderr, "[web-renderer] test_draw skipped publish: renderer busy\n");
                return;
            }
            for (DrawCmd& d : ctx.draws) draw_queue_.push_back(std::move(d));
            for (TexUpload& t : ctx.tex) tex_upload_queue_.push_back(std::move(t));
        }
        fprintf(stderr, "[web-renderer] test_draw: %u bytes of DL recorded\n", o - kDlOff);
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
            for (TexUpload& up : tex_upload_queue_) {
                GLuint tex = 0;
                auto it = gl_textures_.find(up.key);
                if (it != gl_textures_.end()) {
                    tex = it->second;
                } else {
                    glGenTextures(1, &tex);
                    gl_textures_[up.key] = tex;
                }
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, up.width, up.height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, up.pixels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                up.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                up.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                up.bilerp ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                up.bilerp ? GL_LINEAR : GL_NEAREST);
                if (tex_logged_ < 6) {
                    ++tex_logged_;
                    size_t nz = 0; uint64_t sum = 0;
                    for (size_t i = 0; i + 3 < up.pixels.size(); i += 4) {
                        const unsigned v = up.pixels[i] + up.pixels[i + 1] + up.pixels[i + 2];
                        if (v) { ++nz; sum += v; }
                    }
                    OGRE_MILESTONE("GFX-TEX",
                                   "tex %u: %dx%d tile=%d key=0x%llX nonzero=%zu/%zu avgRGB=%llu",
                                   tex_logged_, up.width, up.height, up.tile,
                                   (unsigned long long)up.key, nz, up.pixels.size() / 4,
                                   (unsigned long long)(nz ? sum / nz : 0));
                }
            }
            tex_upload_count_ += tex_upload_queue_.size();
            tex_upload_queue_.clear();
        }

        // Draws.
        std::vector<DrawCmd> cmds;
        {
            cmds.swap(draw_queue_);
        }

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

        for (const DrawCmd& cmd : cmds) {
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
            seti("u_bp0", cmd.bp[0]); seti("u_bm0", cmd.bm[0]);
            seti("u_ba0", cmd.ba[0]); seti("u_bb0", cmd.bb[0]);
            seti("u_bp1", cmd.bp[1]); seti("u_bm1", cmd.bm[1]);
            seti("u_ba1", cmd.ba[1]); seti("u_bb1", cmd.bb[1]);

            glUniform4fv(glGetUniformLocation(program_, "u_prim"), 1, cmd.prim);
            glUniform4fv(glGetUniformLocation(program_, "u_env"), 1, cmd.env);
            glUniform4fv(glGetUniformLocation(program_, "u_fog_color"), 1, cmd.fog);
            glUniform4fv(glGetUniformLocation(program_, "u_blend_color"), 1, cmd.blend_color);

            if (cmd.textured) {
                // TEXEL0's and TEXEL1's tile images are separate GL textures,
                // each with its own UV transform (the title sprites take their
                // colour from tile 1 and their alpha mask from tile 0).
                auto lookup = [this](uint64_t key) -> GLuint {
                    auto it = gl_textures_.find(key);
                    return (key != 0 && it != gl_textures_.end()) ? it->second : white_tex_;
                };
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, lookup(cmd.tex_key));
                glUniform1i(glGetUniformLocation(program_, "u_tex0"), 0);
                glUniform2f(glGetUniformLocation(program_, "u_uv_scale"), cmd.tex_scale[0], cmd.tex_scale[1]);
                glUniform2f(glGetUniformLocation(program_, "u_uv_origin"), cmd.tex_origin[0], cmd.tex_origin[1]);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, lookup(cmd.tex_key1));
                glUniform1i(glGetUniformLocation(program_, "u_tex1"), 1);
                glUniform2f(glGetUniformLocation(program_, "u_uv_scale1"), cmd.tex_scale1[0], cmd.tex_scale1[1]);
                glUniform2f(glGetUniformLocation(program_, "u_uv_origin1"), cmd.tex_origin1[0], cmd.tex_origin1[1]);
                glActiveTexture(GL_TEXTURE0);
            }

            // Scissor.
            if (cmd.scissor_on) {
                const int x = cmd.sx0;
                const int y = canvas_height_ - cmd.sy1 - 1;
                const int w = cmd.sx1 - cmd.sx0 + 1;
                const int h = cmd.sy1 - cmd.sy0 + 1;
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

            // Interleave pos/uv/shade into the vertex buffer.
            float vbuf[6 * 8];
            const int count = cmd.vertex_count;
            for (int i = 0; i < count; ++i) {
                vbuf[i * 8 + 0] = cmd.pos[i * 2 + 0];
                vbuf[i * 8 + 1] = cmd.pos[i * 2 + 1];
                vbuf[i * 8 + 2] = cmd.uv[i * 2 + 0];
                vbuf[i * 8 + 3] = cmd.uv[i * 2 + 1];
                vbuf[i * 8 + 4] = cmd.shade[i * 4 + 0];
                vbuf[i * 8 + 5] = cmd.shade[i * 4 + 1];
                vbuf[i * 8 + 6] = cmd.shade[i * 4 + 2];
                vbuf[i * 8 + 7] = cmd.shade[i * 4 + 3];
            }
            glBufferData(GL_ARRAY_BUFFER, count * 8 * sizeof(float), vbuf, GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, count);
        }

        // Read the canvas back right after the batch, so "what the frame looks
        // like once every draw has landed" is measured rather than inferred
        // from a screenshot that may catch a partially flushed frame.
        static unsigned after_logged = 0;
        if (after_logged < 24 && had_rect) {
            static std::vector<uint8_t> px;
            px.resize(static_cast<size_t>(canvas_width_) * canvas_height_ * 4);
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

        // Publish the recorded commands under a short lock.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (DrawCmd& d : ctx.draws) {
                if (draw_queue_.size() >= 16384) {
                    break;
                }
                draw_queue_.push_back(std::move(d));
                ++draws_recorded_;
            }
            for (TexUpload& t : ctx.tex) {
                tex_upload_queue_.push_back(std::move(t));
            }
            g_exec_draws.store(draws_recorded_, std::memory_order_relaxed);
        }

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
        (void)fb_address;
        // The game's dummy framebuffers are not presented in this prototype.
    }

    void update_screen() override {
        // Direct-to-canvas rendering: nothing to draw at VI time; the main
        // thread presents the queued commands via ogre_gfx_flush().
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_ready_) {
            if (program_ != 0) {
                glDeleteProgram(program_);
            }
            for (auto& [key, tex] : gl_textures_) {
                glDeleteTextures(1, &tex);
            }
            gl_textures_.clear();
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
    float ndc_min_[2] = {1e9f, 1e9f};
    float ndc_max_[2] = {-1e9f, -1e9f};
    bool logged_first_flush_ = false;
    unsigned tex_logged_ = 0;    unsigned tri_reject_logged_ = 0;
    unsigned draw_logged_ = 0;
    unsigned vert_logged_ = 0;
    unsigned txr_logged_ = 0;
    unsigned tex_state_logged_ = 0;
    unsigned tex_state_dump_logged_ = 0;
    unsigned mtx_logged_ = 0;
    unsigned draw_mtx_logged_ = 0;
    unsigned rect_logged_ = 0;
    unsigned blit_logged_ = 0;
    unsigned flushed_cmds_ = 0;
    unsigned flush_count_ = 0;
    unsigned last_dl_cmds_ = 0;   // commands walked by the last execute_dl
    uint32_t last_task_data_ptr_ = 0;  // raw t.data_ptr of the last task
    unsigned runaway_logged_ = 0;
    std::mutex mutex_;

    // Command queues: written by the gfx pthread (execute_dl), drained by the
    // browser main thread (ogre_gfx_flush -> flush_commands).
    std::vector<DrawCmd> draw_queue_;
    std::vector<TexUpload> tex_upload_queue_;
    size_t tex_upload_count_ = 0;
    // GL-side texture cache (main thread only): key = tile load key.
    std::map<uint64_t, GLuint> gl_textures_;

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
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));

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
    };

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

        gbi::walk_dl(rdram, dl_offset, visitor, &ctx);
        last_dl_cmds_ = ctx.cmd_index;
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

        switch (op) {
            case gbi::OP_MTX: {
                // F3DEX2 stores the PUSH bit *inverted*: the SDK macro XORs
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
                const uint32_t off = p0(w0, 0, 24);
                const uint32_t on = w1;
                st.geometry_mode = (st.geometry_mode & ~off) | on;
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

            case gbi::OP_SETCIMG:
                break;  // framebuffer target; direct-to-canvas ignores it

            case gbi::OP_SETZIMG:
                break;

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
                note_tile_load(st, static_cast<int>(p0(w1, 24, 3)));
                break;
            }

            case gbi::OP_LOADTLUT: {
                const int tile = static_cast<int>(p0(w1, 24, 3));
                TileState& t = st.tiles[tile];
                // Number of 16-bit TLUT entries: lrs is 10.2 fixed -> entries.
                const uint16_t count = static_cast<uint16_t>((p0(w1, 12, 12) >> 2) + 1);
                const uint32_t bank = static_cast<uint32_t>(t.palette) * 16;
                const uint32_t base = st.timg_address + ((p0(w0, 12, 12) >> 2) << st.timg_siz >> 1);
                for (uint16_t i = 0; i < count && bank + i < 256; ++i) {
                    st.tlut[bank + i] = rd16(rdram_ + base + i * 2);
                }
                st.tlut_valid = true;
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

            case gbi::OP_MOVEMEM: {
                // F3DEX2 G_MOVEMEM: the index is the low byte of w0 (RT64
                // GBI_F3DEX2::moveMem), w1 is the address of the payload.
                const uint8_t index = static_cast<uint8_t>(p0(w0, 0, 8));
                if (index == gbi::MV_VIEWPORT) {
                    set_viewport(ctx, st, resolve_address(ctx->segments, w1));
                }
                break;
            }

            case gbi::OP_MOVEWORD: {
                // The RSP keeps only the segment base's high byte; segmented
                // addresses then resolve as (base << 24) | offset. Storing the
                // full base made every segmented matrix/vertex address resolve
                // to garbage.
                gbi::set_segment_from_moveword(ctx->segments, w0, w1);
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
            case gbi::OP_SETKEYGB:
            case gbi::OP_SETKEYR:
            case gbi::OP_SETCONVERT:
            case gbi::OP_SETPRIMDEPTH:
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
    void dump_tex_state(ExecCtx* ctx, const RenderState& st, const char* kind) {
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

    // Decodes (and uploads, once per key) the image a tile samples, using the
    // tile's own format/size, its rect, and its `line` as the row stride.
    // Replaces "decode the load rect with the load's format", which could not
    // express the mask above and used the wrong stride whenever they differ.
    void ensure_tile_image(ExecCtx* ctx, RenderState& st, int tile, int unit = 0) {
        TileState& t = st.tiles[tile & (kMaxTiles - 1)];
        if (!t.image_source_valid) {
            return;
        }
        const int x0 = t.uls >> 2;
        const int y0 = t.ult >> 2;
        const int w = (t.lrs >> 2) - x0 + 1;
        const int h = (t.lrt >> 2) - y0 + 1;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            t.image_valid = false;
            return;
        }
        const int row_bytes = t.image_stride;
        if (row_bytes <= 0) {
            t.image_valid = false;
            return;
        }
        const uint64_t key = make_image_key(t, tile, t.image_address, x0, y0, w, h, row_bytes);
        if (t.image_valid && t.image.key == key) {
            return;   // already decoded and queued for this draw
        }

        g_load_seq.fetch_add(1, std::memory_order_relaxed);
        g_load_w.store(static_cast<uint32_t>(w), std::memory_order_relaxed);
        g_load_h.store(static_cast<uint32_t>(h), std::memory_order_relaxed);
        g_load_siz.store(t.siz, std::memory_order_relaxed);
        g_load_fmt.store(t.fmt, std::memory_order_relaxed);
        g_load_timgw.store(static_cast<uint32_t>(row_bytes), std::memory_order_relaxed);
        g_load_addr.store(t.image_address, std::memory_order_relaxed);

        std::vector<uint8_t> pixels;
        decode_texture_rect(rdram_, t.image_address, t.fmt, t.siz, row_bytes, x0, y0, w, h,
                            st.tlut, st.tlut_valid, t.palette, pixels);
        g_load_done.fetch_add(1, std::memory_order_relaxed);

        // Debug copy for ogre_gfx_debug_tex (probes inspect what was sampled).
        {
            std::lock_guard<std::mutex> lock(debug_mutex_);
            DebugTex dbg;
            dbg.key = key;
            dbg.width = w;
            dbg.height = h;
            dbg.tile = tile;
            dbg.fmt = t.fmt;
            dbg.siz = t.siz;
            dbg.line = t.line;
            dbg.pixels = pixels;
            // Keep a whole frame's loads (12 sprites x mask+colour) so a probe
            // can compare the first rendered sprite with its own texture pair.
            if (debug_tex_.size() >= 24) {
                debug_tex_.erase(debug_tex_.begin());
            }
            debug_tex_.push_back(dbg);
            debug_last_[unit & 1] = std::move(dbg);
        }

        // One upload per key per display list: a sprite's two triangles share it.
        if (ctx->uploaded.count(key) == 0) {
            ctx->uploaded.insert(key);
            TexUpload up;
            up.key = key;
            up.width = w;
            up.height = h;
            up.tile = tile;
            up.clamp_s = (t.cms != 0) || (t.masks == 0);
            up.clamp_t = (t.cmt != 0) || (t.maskt == 0);
            up.bilerp = ((st.othermode_h >> 12) & 3) == 2;  // G_TF_BILERP
            up.pixels = pixels;
            ctx->tex.push_back(std::move(up));
        }

        // Diagnostics: remember the decoded texel (0,0) and the tile's addressing
        // so the texrect path can report what a dsdx=dtdy=0 rect samples.
        if (pixels.size() >= 4) {
            last_tex_rgba_[0] = pixels[0];
            last_tex_rgba_[1] = pixels[1];
            last_tex_rgba_[2] = pixels[2];
            last_tex_rgba_[3] = pixels[3];
        }
        last_tex_fmt_ = t.fmt;
        last_tex_siz_ = t.siz;
        last_tex_line_ = t.line;
        last_tex_shifts_ = t.shifts;
        last_tex_shiftt_ = t.shiftt;
        last_tex_masks_ = t.masks;
        last_tex_maskt_ = t.maskt;

        t.image.key = key;
        t.image.width = w;
        t.image.height = h;
        t.image.origin_s = x0;
        t.image.origin_t = y0;
        t.image_valid = true;
    }

    // The image key: content is identified by the source address, the tile's
    // sampling parameters and the sampled rect. (Frames reuse addresses, so the
    // bytes are re-decoded and re-uploaded every time the key is queued.)
    uint64_t make_image_key(const TileState& t, int tile, uint32_t address,
                            int x0, int y0, int w, int h, int row_bytes) {
        uint64_t k = address;
        k = k * 31 + t.fmt;
        k = k * 31 + t.siz;
        k = k * 31 + t.line;
        k = k * 31 + tile;
        k = k * 31 + static_cast<uint32_t>(x0);
        k = k * 31 + static_cast<uint32_t>(y0);
        k = k * 31 + static_cast<uint32_t>(w);
        k = k * 31 + static_cast<uint32_t>(h);
        k = k * 31 + static_cast<uint32_t>(row_bytes);
        k = k * 31 + t.palette;
        return k;
    }

    // ----- drawing (gfx pthread: record commands) --------------------------------

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
        if (textured) {
            // TEXEL0 = the G_TEXTURE tile, TEXEL1 = the tile above it; each
            // carries the image decoded from its own source and rect.
            const LoadedImage& i0 = st.tiles[st.active_tile & (kMaxTiles - 1)].image;
            const LoadedImage& i1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image;
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
        }
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
    }

    // Queues one triangle list (NDC pos + uv + shade per vertex).
    void queue_triangles(ExecCtx* ctx, DrawCmd& cmd, const float* data, int count) {
        cmd.vertex_count = count;
        if (!ctx->first_quad_done && cmd.kind == 0 && count >= 3) {
            ctx->first_quad_done = true;
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (int i = 0; i < count; ++i) {
                minx = std::min(minx, data[i * 8 + 0]); maxx = std::max(maxx, data[i * 8 + 0]);
                miny = std::min(miny, data[i * 8 + 1]); maxy = std::max(maxy, data[i * 8 + 1]);
            }
            ctx->quad_w = maxx - minx;
            ctx->quad_h = maxy - miny;
        }
        for (int i = 0; i < count; ++i) {
            cmd.pos[i * 2 + 0] = data[i * 8 + 0];
            cmd.pos[i * 2 + 1] = data[i * 8 + 1];
            cmd.uv[i * 2 + 0] = data[i * 8 + 2];
            cmd.uv[i * 2 + 1] = data[i * 8 + 3];
            cmd.shade[i * 4 + 0] = data[i * 8 + 4];
            cmd.shade[i * 4 + 1] = data[i * 8 + 5];
            cmd.shade[i * 4 + 2] = data[i * 8 + 6];
            cmd.shade[i * 4 + 3] = data[i * 8 + 7];
            // Track the NDC extent of everything queued so far: if the game's
            // geometry lands outside [-1, 1] nothing is visible even though the
            // draw commands exist.
            for (int k = 0; k < 2; ++k) {
                const float v = data[i * 8 + k];
                if (v < ndc_min_[k]) ndc_min_[k] = v;
                if (v > ndc_max_[k]) ndc_max_[k] = v;
            }
        }
        // Execution-local: published to draw_queue_ by the caller under a short
        // lock. Never lock the renderer mutex here - it was held across the
        // whole DL walk before, which both self-deadlocked (mutex_ is not
        // recursive) and starved the browser main thread's flush.
        if (ctx->draws.size() < 16384) {
            ctx->draws.push_back(cmd);
        }
    }

    // Transforms a raw vertex through the matrix pipeline into NDC.
    // Returns false if the vertex is behind the camera (w <= 0).
    bool transform_vertex(const RenderState& st, const RawVertex& v, float out[8]) {
        // Modelview then projection (row-vector).
        const Mat4& mv = st.modelview_stack.back();
        Vec4 pos = mul(Vec4{static_cast<float>(v.x), static_cast<float>(v.y),
                            static_cast<float>(v.z), 1.0f},
                       mv);
        pos = mul(pos, st.projection);
        if (pos.w <= 0.0f) {
            return false;
        }
        // Perspective divide + viewport. The RSP maps clip space to screen
        // space with the Vp_t scale/translate (screen y points down); the GL
        // framebuffer expects NDC, so the result is converted back.
        const float inv_w = 1.0f / pos.w;
        float sx, sy;
        if (st.viewport_set) {
            const float scr_x = pos.x * inv_w * st.view_scale[0] + st.view_trans[0];
            const float scr_y = -pos.y * inv_w * st.view_scale[1] + st.view_trans[1];
            sx = ndc_xf(scr_x);
            sy = ndc_yf(scr_y);
        } else {
            // No viewport seen yet: treat the projection output as NDC.
            sx = pos.x * inv_w;   // [-1, 1] (x)
            sy = -pos.y * inv_w;  // flipped: NDC y up
        }
        out[0] = sx;
        out[1] = sy;
        // Texture coordinates: s10.5 texels scaled by G_TEXTURE (RT64
        // RSP::setVertexCommon). The G_TEXTURE scale must NOT be applied to
        // rectangles - RT64's RDP::drawRect uses uls/32 and lrs/32 directly.
        out[2] = (static_cast<float>(v.s) * st.tex_scale_s) / 32.0f;
        out[3] = (static_cast<float>(v.t) * st.tex_scale_t) / 32.0f;
        out[4] = v.r / 255.0f;
        out[5] = v.g / 255.0f;
        out[6] = v.b / 255.0f;
        out[7] = v.a / 255.0f;
        return true;
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
        const RawVertex* v[3] = {&st.vertices[i0], &st.vertices[i1], &st.vertices[i2]};
        float data[3 * 8];
        for (int i = 0; i < 3; ++i) {
            if (!transform_vertex(st, *v[i], data + i * 8)) {
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
                ctx->u0 = std::min(ctx->u0, data[i * 8 + 2]);
                ctx->u1 = std::max(ctx->u1, data[i * 8 + 2]);
                ctx->v0 = std::min(ctx->v0, data[i * 8 + 3]);
                ctx->v1 = std::max(ctx->v1, data[i * 8 + 3]);
            }
            ctx->iw0 = st.tiles[st.active_tile & (kMaxTiles - 1)].image.width;
            ctx->ih0 = st.tiles[st.active_tile & (kMaxTiles - 1)].image.height;
            ctx->iw1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image.width;
            ctx->ih1 = st.tiles[(st.active_tile + 1) & (kMaxTiles - 1)].image.height;
        }
        DrawCmd cmd;
        record_state(cmd, st, texture_available(st));
        if (texture_available(st)) {
            dump_tex_state(ctx, st, "tri");
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
        if (rect_logged_ < 3) {
            ++rect_logged_;
            OGRE_MILESTONE("GFX-RECT",
                           "fillrect (%d,%d)-(%d,%d) cyc=%d omh=0x%06X oml=0x%08X "
                           "fill=[%.2f,%.2f,%.2f,%.2f] scissor=%d(%d,%d,%d,%d)",
                           xl, yl, xh, yh, static_cast<int>((st.othermode_h >> 20) & 3),
                           st.othermode_h, st.othermode_l,
                           st.fill_color[0], st.fill_color[1], st.fill_color[2], st.fill_color[3],
                           st.scissor_enabled ? 1 : 0,
                           st.scissor_x0, st.scissor_y0, st.scissor_x1, st.scissor_y1);
        }
        if ((g_debug_flags.load(std::memory_order_relaxed) & 4u) != 0 &&
            (xh - xl) >= 299 && (yh - yl) >= 219) {
            return;   // debug: keep the previous frame instead of clearing it
        }
        // G_FILLRECT in fill mode outputs the fill color directly (the
        // combiner is bypassed). Approximate by driving the combiner with the
        // fill color as PRIMITIVE: rgb = (PRIM - 0) * 1 + 0.
        const float x0 = ndc_x(xl), x1 = ndc_x(xh + 1);
        const float y0 = ndc_y(yl), y1 = ndc_y(yh + 1);
        const float c[4] = {st.fill_color[0], st.fill_color[1], st.fill_color[2], st.fill_color[3]};
        float data[6 * 8];
        for (int i = 0; i < 6; ++i) {
            const int corner = i % 4;
            const float px = (corner == 0 || corner == 1) ? x0 : x1;
            const float py = (corner == 0 || corner == 3) ? y0 : y1;
            data[i * 8 + 0] = px;
            data[i * 8 + 1] = py;
            data[i * 8 + 2] = 0;
            data[i * 8 + 3] = 0;
            data[i * 8 + 4] = c[0];
            data[i * 8 + 5] = c[1];
            data[i * 8 + 6] = c[2];
            data[i * 8 + 7] = c[3];
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

    void draw_texrect(ExecCtx* ctx) {
        if (ctx->order.size() < 200) ctx->order += 'T';
        RenderState& st = *ctx->st;
        // A rectangle samples the tile it names, not G_TEXTURE's (RT64
        // RDP::drawTexRect takes the tile as a parameter). Scope the override
        // to this draw.
        const int saved_tile = st.active_tile;
        st.active_tile = texrect_tile_ & (kMaxTiles - 1);
        const int xl = texrect_geom_[0], yl = texrect_geom_[1];
        const int xh = texrect_geom_[2], yh = texrect_geom_[3];
        if (xh < xl || yh < yl) {
            st.active_tile = saved_tile;
            return;  // empty rectangle: the RDP draws nothing (see draw_fill_rect)
        }
        const int16_t s0 = static_cast<int16_t>((ctx->rdp_half1 >> 16) & 0xFFFF);
        const int16_t t0 = static_cast<int16_t>(ctx->rdp_half1 & 0xFFFF);
        const int16_t dsdx = static_cast<int16_t>((ctx->rdp_half2 >> 16) & 0xFFFF);
        const int16_t dtdy = static_cast<int16_t>(ctx->rdp_half2 & 0xFFFF);

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
            return;   // debug: show the frame without the intro's fade-in rect
        }
        // Decode the two tile images first: texture_available() reads the
        // result, and a rect that samples a tile loaded straight into it (the
        // synthetic test_draw) has no other place to pick the image up.
        ensure_tile_image(ctx, st, st.active_tile, 0);
        ensure_tile_image(ctx, st, st.active_tile + 1, 1);
        if (!texture_available(st)) {
            st.active_tile = saved_tile;
            return;
        }

        const float us = s0 / 32.0f;
        const float vs = t0 / 32.0f;
        const float du = dsdx / 32.0f;
        const float dv = dtdy / 32.0f;
        const float w = static_cast<float>(xh - xl + 1);
        const float h = static_cast<float>(yh - yl + 1);

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

        const float x0 = ndc_x(xl), x1 = ndc_x(xh + 1);
        const float y0 = ndc_y(yl), y1 = ndc_y(yh + 1);
        const float px[4] = {x0, x1, x0, x1};
        const float py[4] = {y0, y0, y1, y1};

        float data[6 * 8];
        const int idx[6] = {0, 1, 3, 0, 3, 2};
        for (int i = 0; i < 6; ++i) {
            const int c = idx[i];
            data[i * 8 + 0] = px[c];
            data[i * 8 + 1] = py[c];
            data[i * 8 + 2] = uv[c][0];
            data[i * 8 + 3] = uv[c][1];
            data[i * 8 + 4] = 1;
            data[i * 8 + 5] = 1;
            data[i * 8 + 6] = 1;
            data[i * 8 + 7] = 1;
        }

        DrawCmd cmd;
        cmd.kind = 2;
        record_state(cmd, st, texture_available(st));
        if (rect_logged_ < 3) {
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
        queue_triangles(ctx, cmd, data, 6);
        st.active_tile = saved_tile;
    }

    // Screen (pixel) -> NDC.
    float ndc_xf(float x) const {
        return 2.0f * x / static_cast<float>(canvas_width_) - 1.0f;
    }
    float ndc_yf(float y) const {
        return 1.0f - 2.0f * y / static_cast<float>(canvas_height_);
    }
    float ndc_x(int x) const { return ndc_xf(static_cast<float>(x)); }
    float ndc_y(int y) const { return ndc_yf(static_cast<float>(y)); }

    // ----- matrix helpers ------------------------------------------------------

    // G_MOVEMEM MV_VIEWPORT: reads the RSP viewport (Vp_t) and stores the
    // resulting screen-space transform. `Vp_t` is
    //     s16 vscale[4];  // 14.2 fixed, s16 vtrans[4];
    // and the fields are laid out (y, x, pad, z) as the RSP consumes them:
    // OB64 submits vscale=[480 640 0 511] for its 320x240 screen, so index 1
    // (640) is the x scale and index 0 (480) the y scale. This is exactly
    // RT64's mapping (RSP::setViewport in hle/rt64_rsp.cpp). The renderer never
    // applied any viewport before, which is why 3D geometry landed in raw clip
    // space (ndc_x up to 3.25) with most of it off-screen.
    void set_viewport(ExecCtx* ctx, RenderState& st, uint32_t addr) {
        int16_t vscale[4], vtrans[4];
        for (int i = 0; i < 4; ++i) {
            vscale[i] = static_cast<int16_t>(rd16(rdram_ + addr + i * 2));
            vtrans[i] = static_cast<int16_t>(rd16(rdram_ + addr + 8 + i * 2));
        }
        st.view_scale[0] = static_cast<float>(vscale[1]) / 4.0f;  // x
        st.view_scale[1] = static_cast<float>(vscale[0]) / 4.0f;  // y
        st.view_scale[2] = static_cast<float>(vscale[3]) / 4.0f;  // z
        st.view_trans[0] = static_cast<float>(vtrans[1]) / 4.0f;  // x
        st.view_trans[1] = static_cast<float>(vtrans[0]) / 4.0f;  // y
        st.view_trans[2] = static_cast<float>(vtrans[3]) / 4.0f;  // z
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
                 "\nload: seq=%u done=%u w=%u h=%u fmt=%u siz=%u timgw=%u addr=0x%08X",
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
                 ogre::g_load_addr.load(std::memory_order_relaxed));
    }
    return buf;
}

// Drains the queued draw/texture commands and issues the GL calls. Called
// periodically from web.js on the browser main thread (never from a pthread).
int ogre_gfx_flush() {
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

}  // extern "C"
