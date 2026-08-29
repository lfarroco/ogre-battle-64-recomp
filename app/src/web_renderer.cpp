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
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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

struct TileState {
    uint8_t fmt, siz;
    uint16_t line, tmem;
    uint8_t palette;
    uint8_t cmt, cms;    // clamp/mirror t/s
    uint8_t maskt, masks;
    uint8_t shiftt, shifts;
    uint16_t uls, ult, lrs, lrt;
};

// Decoded 2-cycle combiner mux (see rt64_color_combiner.h).
struct Combiner {
    int ca[2], cb[2], cc[2], cd[2];   // color inputs (0-8)
    int aa[2], ab[2], ac[2], ad[2];   // alpha inputs (0-7)
    bool valid = false;

    void decode(uint32_t L, uint32_t H) {
        for (int cyc = 0; cyc < 2; cyc++) {
            ca[cyc] = cyc ? (L >> 5) & 0xF : (L >> 20) & 0xF;
            cb[cyc] = cyc ? (H >> 24) & 0xF : (H >> 28) & 0xF;
            cc[cyc] = cyc ? (L >> 0) & 0x1F : (L >> 15) & 0x1F;
            cd[cyc] = cyc ? (H >> 6) & 0x7 : (H >> 15) & 0x7;
            aa[cyc] = cyc ? (H >> 21) & 0x7 : (L >> 12) & 0x7;
            ab[cyc] = cyc ? (H >> 3) & 0x7 : (H >> 12) & 0x7;
            ac[cyc] = cyc ? (H >> 18) & 0x7 : (L >> 9) & 0x7;
            ad[cyc] = cyc ? (H >> 0) & 0x7 : (H >> 9) & 0x7;
        }
        valid = true;
    }
};

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

    // Scissor (integer pixels, inclusive bounds).
    bool scissor_enabled = false;
    int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 0, scissor_y1 = 0;

    // Combiner.
    Combiner combiner;

    // Texture state.
    uint32_t timg_address = 0;
    uint8_t timg_fmt = 0, timg_siz = 0;
    uint16_t timg_width = 0;
    TileState tiles[kMaxTiles]{};
    int active_tile = 0;          // G_TEXTURE tile
    int32_t tex_sc = 0, tex_tc = 0;  // G_TEXTURE scale (5-bit fraction)
    bool texture_on = false;      // G_TEXTURE "on" field
    // Palette loaded by G_LOADTLUT (16-bit entries).
    uint16_t tlut[256]{};
    bool tlut_valid = false;

    // Active GL texture for the current draw (set by texture_for_tile()).
    GLuint active_gl_tex = 0;
    uint64_t active_tex_key = 0;  // key of the tile loaded by LOADTILE/LOADBLOCK
    float active_tex_scale[2] = {1, 1};   // uv = (s/32 - uls) / width ... see draw
    int active_tex_width = 0, active_tex_height = 0;
};

// Reads a 32-bit word from rdram with the runtime's byte-reversed storage.
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 0) | (p[1] << 8));
}

inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1);
}

uint32_t resolve_address(const uint32_t* segments, uint32_t addr) {
    // KSEG0/KSEG1 map 1:1 into the 512 MiB rdram region.
    if ((addr & 0xFF000000u) == 0x80000000u || (addr & 0xFF000000u) == 0xA0000000u) {
        return addr & 0x1FFFFFFFu;
    }
    const uint8_t seg = static_cast<uint8_t>(addr >> 24);
    if (seg >= 1 && seg <= 15 && segments[seg] != 0) {
        return (segments[seg] << 24) | (addr & 0x00FFFFFFu);
    }
    // Physical address: use it directly (the port's rdram is 512 MiB, so
    // addresses may exceed the N64's 8 MiB physical window).
    return addr & 0x1FFFFFFFu;
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

// Decodes one RGBA16 texel (5/5/5/1).
inline void rgba16_to_rgba8(uint16_t v, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(((v >> 11) & 0x1F) * 255 / 31);
    out[1] = static_cast<uint8_t>(((v >> 6) & 0x1F) * 255 / 31);
    out[2] = static_cast<uint8_t>(((v >> 1) & 0x1F) * 255 / 31);
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
void decode_texture_rect(const uint8_t* rdram, uint32_t timg_address, uint8_t fmt, uint8_t siz,
                         uint16_t width, int x0, int y0, int w, int h, const uint16_t* tlut,
                         bool tlut_valid, uint8_t palette, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);

    const int bpp = bytes_per_texel(siz);
    // Row stride in bytes of the texture image (16-bit aligned words).
    const int row_bytes = (width * std::max(bpp, 1)) ;  // 4b packs 2 texels/byte

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
                        const uint8_t i = nib * 255 / 15;
                        rgba[0] = rgba[1] = rgba[2] = i;
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
                                rgba[0] = rgba[1] = rgba[2] = v;
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
out vec2 v_uv;
out vec4 v_shade;
uniform vec2 u_uv_scale;   // (1/tex_w, 1/tex_h)
uniform vec2 u_uv_origin;  // (uls, ult)
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = (a_uv - u_uv_origin) * u_uv_scale;
    v_shade = a_color;
}
)GLSL";

// Evaluates the general two-cycle N64 color combiner:
//   rgb   = clamp((A-B)*C + D)   per channel
//   alpha = clamp((Aa-Ba)*Ca + Da)
// with the RDP input selectors decoded per rt64_color_combiner.h.
const char* kFragmentShaderSrc = R"GLSL(#version 300 es
precision mediump float;
in vec2 v_uv;
in vec4 v_shade;
out vec4 fragColor;
uniform sampler2D u_tex0;
uniform vec4 u_prim;
uniform vec4 u_env;
uniform int u_ca0, u_cb0, u_cc0, u_cd0;
uniform int u_aa0, u_ab0, u_ac0, u_ad0;
uniform int u_ca1, u_cb1, u_cc1, u_cd1;
uniform int u_aa1, u_ab1, u_ac1, u_ad1;
uniform int u_cycle;  // 1 or 2
uniform int u_use_tex;
uniform int u_alpha_from_cvg; // reserved

vec3 src_rgb(int sel) {
    if (sel == 0) return vec3(0.0);             // COMBINED (cycle 0)
    if (sel == 1 || sel == 2) return texture(u_tex0, v_uv).rgb;  // TEXEL0/1
    if (sel == 3) return u_prim.rgb;
    if (sel == 4) return v_shade.rgb;
    if (sel == 5) return u_env.rgb;
    if (sel == 6) return vec3(1.0);
    if (sel == 8) return vec3(texture(u_tex0, v_uv).a);  // TEXEL0_ALPHA
    return vec3(0.0);
}
float src_a(int sel) {
    if (sel == 0) return 0.0;
    if (sel == 1 || sel == 2) return texture(u_tex0, v_uv).a;
    if (sel == 3) return u_prim.a;
    if (sel == 4) return v_shade.a;
    if (sel == 5) return u_env.a;
    if (sel == 6) return 1.0;
    return 0.0;
}
void main() {
    vec4 texel = texture(u_tex0, v_uv);
    // Cycle 0.
    vec3 rgb0 = (src_rgb(u_ca0) - src_rgb(u_cb0)) * src_rgb(u_cc0) + src_rgb(u_cd0);
    float a0 = (src_a(u_aa0) - src_a(u_ab0)) * src_a(u_ac0) + src_a(u_ad0);
    vec3 rgb = rgb0;
    float alpha = a0;
    if (u_cycle == 2) {
        // COMBINED inputs in cycle 1 use cycle 0's result.
        vec3 rgb1 = (src_rgb(u_ca1) - src_rgb(u_cb1)) * src_rgb(u_cc1) + src_rgb(u_cd1);
        float a1 = (src_a(u_aa1) - src_a(u_ab1)) * src_a(u_ac1) + src_a(u_ad1);
        rgb = rgb1;
        alpha = a1;
    }
    fragColor = vec4(clamp(rgb, 0.0, 1.0), clamp(alpha, 0.0, 1.0));
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
    bool clamp_s = true, clamp_t = true;
    bool bilerp = false;
    std::vector<uint8_t> pixels;
};

// One queued draw: a triangle list plus the full RDP state needed to render it
// (combiner mux, colors, blend, scissor, texture key). The gfx pthread records
// these; the browser main thread executes them in ogre_gfx_flush().
struct DrawCmd {
    int vertex_count = 0;                 // 3 or 6
    float pos[12] = {};                   // NDC (x,y) per vertex
    float uv[12] = {};                    // texture (u,v) per vertex
    float shade[24] = {};                 // rgba per vertex
    // Combiner mux (decoded).
    int ca[2] = {}, cb[2] = {}, cc[2] = {}, cd[2] = {};
    int aa[2] = {}, ab[2] = {}, ac[2] = {}, ad[2] = {};
    int cycle = 1;
    float prim[4] = {1, 1, 1, 1};
    float env[4] = {1, 1, 1, 1};
    bool textured = false;
    uint64_t tex_key = 0;
    float tex_scale[2] = {1, 1};
    float tex_origin[2] = {0, 0};
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
        constexpr uint32_t kDlOff = 0x1FE00000;   // scratch DL area (unused)
        constexpr uint32_t kTexOff = 0x1FE01000;  // 2x2 RGBA16 texture

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
        // G_FILLRECT (0,0)-(63,47)  [10.2 fixed: 63<<2=252, 47<<2=188]
        put32(o, 0xF6000000); put32(o + 4, (63 << 2) << 12 | (47 << 2)); o += 8;
        // G_SETTIMG RGBA16 width=2 -> texture at kTexOff
        put32(o, 0xFD100001); put32(o + 4, kTexOff); o += 8;
        // G_SETTILE RGBA16, line=1, tmem=0, clamp
        put32(o, 0xF5100200); put32(o + 4, 0x00060000); o += 8;
        // G_SETTILESIZE tile 0: texels (0,0)-(1,1)  [10.2: 1<<2 = 4]
        put32(o, 0xF2000000); put32(o + 4, 0x4004); o += 8;
        // G_LOADTILE tile 0
        put32(o, 0xF4000000); put32(o + 4, 0x4004); o += 8;
        // G_SETCOMBINE TEX0*SHADE (rgb A=1,B=0,C=4,D=0; alpha Aa=1)
        put32(o, 0xFC121000); put32(o + 4, 0x00000000); o += 8;
        // G_TEXRECT (0,0)-(63,47), tile 0; RDPHALF_1 s=0,t=0; RDPHALF_2 dsdx=1,dtdy=1
        put32(o, 0xE4000000); put32(o + 4, (63 << 2) << 12 | (47 << 2)); o += 8;
        put32(o, 0xE1000000); put32(o + 4, 0x00000000); o += 8;
        put32(o, 0xF1000000); put32(o + 4, 0x00010001); o += 8;
        // G_ENDDL
        put32(o, 0xDF000000); put32(o + 4, 0x00000000); o += 8;

        RenderState st;
        st.othermode_l = 0;
        st.othermode_h = 0;
        st.combiner.decode(0, 0);
        execute_dl(rdram_, kDlOff, st);
        fprintf(stderr, "[web-renderer] test_draw: %u bytes of DL recorded\n", o - kDlOff);
    }

    // ----- main-thread flush (ogre_gfx_flush) ------------------------------------

    // Runs on the browser main thread: uploads queued textures, then executes
    // queued draws. This is the only place GL is called.
    void flush_commands() {
        if (!ensure_gl()) {
            return;
        }

        // Texture uploads.
        {
            std::lock_guard<std::mutex> lock(mutex_);
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
            }
            tex_upload_count_ += tex_upload_queue_.size();
            tex_upload_queue_.clear();
        }

        // Draws.
        std::vector<DrawCmd> cmds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmds.swap(draw_queue_);
        }

        glUseProgram(program_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        for (const DrawCmd& cmd : cmds) {
            // Combiner uniforms.
            auto seti = [this](const char* name, int v) {
                glUniform1i(glGetUniformLocation(program_, name), v);
            };
            seti("u_ca0", cmd.ca[0]); seti("u_cb0", cmd.cb[0]); seti("u_cc0", cmd.cc[0]); seti("u_cd0", cmd.cd[0]);
            seti("u_aa0", cmd.aa[0]); seti("u_ab0", cmd.ab[0]); seti("u_ac0", cmd.ac[0]); seti("u_ad0", cmd.ad[0]);
            seti("u_ca1", cmd.ca[1]); seti("u_cb1", cmd.cb[1]); seti("u_cc1", cmd.cc[1]); seti("u_cd1", cmd.cd[1]);
            seti("u_aa1", cmd.aa[1]); seti("u_ab1", cmd.ab[1]); seti("u_ac1", cmd.ac[1]); seti("u_ad1", cmd.ad[1]);
            seti("u_cycle", cmd.cycle);

            glUniform4fv(glGetUniformLocation(program_, "u_prim"), 1, cmd.prim);
            glUniform4fv(glGetUniformLocation(program_, "u_env"), 1, cmd.env);

            if (cmd.textured) {
                GLuint tex = 0;
                auto it = gl_textures_.find(cmd.tex_key);
                if (it != gl_textures_.end()) {
                    tex = it->second;
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(glGetUniformLocation(program_, "u_tex0"), 0);
                glUniform2f(glGetUniformLocation(program_, "u_uv_scale"), cmd.tex_scale[0], cmd.tex_scale[1]);
                glUniform2f(glGetUniformLocation(program_, "u_uv_origin"), cmd.tex_origin[0], cmd.tex_origin[1]);
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

            // Blend.
            if (cmd.blend) {
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
    }

    void send_dl(const OSTask* task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++task_count_;

        // Workload analysis (plan §15) continues to run in the browser even
        // with the WebGL renderer active, feeding ogre_gfx_stats().
        if (rdram_ != nullptr) {
            gbi::analyze_dl(rdram_, task->t.data_ptr & 0x3FFFFFF, task->t.data_size, g_workload);
            refresh_stats_snapshot();
            if (task_count_ == 1 || (task_count_ % 50) == 0) {
                OGRE_MILESTONE("GFX-WORKLOAD", "%s", g_stats_snapshot);
            }
        }

        RenderState st;
        st.othermode_l = 0;
        st.othermode_h = 0;
        st.combiner.decode(0, 0);

        // The DL executes on the gfx pthread and only records draw commands;
        // the browser main thread issues the GL in ogre_gfx_flush().
        execute_dl(rdram_, task->t.data_ptr & 0x3FFFFFF, st);

        if (task_count_ <= 16 || (task_count_ % 300) == 0) {
            const unsigned tc = task_count_.load();
            OGRE_MILESTONE("RSP", "display list submitted (frame %u, type %u, ucode 0x%08X)",
                           tc, static_cast<unsigned>(task->t.type),
                           static_cast<unsigned>(task->t.ucode));
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

  private:
    uint8_t* rdram_ = nullptr;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE canvas_handle_ = 0;
    int canvas_width_ = 320;
    int canvas_height_ = 240;
    bool gl_ready_ = false;
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    std::atomic<unsigned> task_count_{0};
    std::mutex mutex_;

    // Command queues: written by the gfx pthread (execute_dl), drained by the
    // browser main thread (ogre_gfx_flush -> flush_commands).
    std::vector<DrawCmd> draw_queue_;
    std::vector<TexUpload> tex_upload_queue_;
    size_t tex_upload_count_ = 0;
    // GL-side texture cache (main thread only): key = tile load key.
    std::map<uint64_t, GLuint> gl_textures_;

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
    };

    void execute_dl(uint8_t* rdram, uint32_t dl_offset, RenderState& st) {
        ExecCtx ctx;
        ctx.self = this;
        ctx.st = &st;

        auto visitor = [](void* user, const gbi::DlCommand& c) {
            static_cast<ExecCtx*>(user)->self->exec_command(static_cast<ExecCtx*>(user), c);
        };

        gbi::walk_dl(rdram, dl_offset, visitor, &ctx);
    }

    void exec_command(ExecCtx* ctx, const gbi::DlCommand& c) {
        RenderState& st = *ctx->st;
        const uint32_t w0 = c.w0, w1 = c.w1;
        const uint8_t op = c.op;

        switch (op) {
            case gbi::OP_MTX: {
                const uint32_t flags = p0(w0, 0, 8);
                const uint32_t addr = resolve_address(ctx->segments, w1);
                Mat4 m = read_matrix(addr);
                if (flags & 0x04) {  // G_MTX_PROJECTION
                    st.projection = m;
                } else {
                    if (flags & gbi::MTX_PUSH) {
                        st.modelview_stack.push_back(st.modelview_stack.back());
                    }
                    Mat4& cur = st.modelview_stack.back();
                    if (flags & gbi::MTX_LOAD) {
                        cur = m;
                    } else {
                        cur = mul_mat4(cur, m);  // row-vector: cur = cur * m
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
                st.tex_sc = static_cast<int16_t>(p0(w1, 16, 16));
                st.tex_tc = static_cast<int16_t>(p0(w1, 0, 16));
                st.texture_on = p0(w0, 1, 7) != 0;
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
                    v.x = static_cast<int16_t>(rd16(p));
                    v.y = static_cast<int16_t>(rd16(p + 2));
                    v.z = static_cast<int16_t>(rd16(p + 4));
                    v.flag = rd16(p + 6);
                    v.s = static_cast<int16_t>(rd16(p + 8));
                    v.t = static_cast<int16_t>(rd16(p + 10));
                    v.r = p[12];
                    v.g = p[13];
                    v.b = p[14];
                    v.a = p[15];
                }
                break;
            }

            case gbi::OP_TRI1:
                draw_tri(st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                break;

            case gbi::OP_TRI2:
                draw_tri(st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                draw_tri(st, p0(w1, 17, 7), p0(w1, 9, 7), p0(w1, 1, 7));
                break;

            case gbi::OP_QUAD:
                draw_tri(st, p0(w0, 17, 7), p0(w0, 9, 7), p0(w0, 1, 7));
                draw_tri(st, p0(w1, 17, 7), p0(w1, 9, 7), p0(w1, 1, 7));
                break;

            case gbi::OP_FILLRECT: {
                const int xl = static_cast<int>(p0(w0, 12, 12)) >> 2;
                const int yl = static_cast<int>(p0(w0, 0, 12)) >> 2;
                const int xh = static_cast<int>(p0(w1, 12, 12)) >> 2;
                const int yh = static_cast<int>(p0(w1, 0, 12)) >> 2;
                draw_fill_rect(st, xl, yl, xh, yh);
                break;
            }

            case gbi::OP_TEXRECT:
            case gbi::OP_TEXRECTFLIP: {
                // The next two commands carry (s,t) and (dsdx,dtdy).
                ctx->pending_texrect = (op == gbi::OP_TEXRECT);
                ctx->pending_texrect_flip = (op == gbi::OP_TEXRECTFLIP);
                ctx->rdp_half1 = 0;
                ctx->rdp_half2 = 0;
                const int xl = static_cast<int>(p0(w0, 12, 12)) >> 2;
                const int yl = static_cast<int>(p0(w0, 0, 12)) >> 2;
                const int xh = static_cast<int>(p0(w1, 12, 12)) >> 2;
                const int yh = static_cast<int>(p0(w1, 0, 12)) >> 2;
                texrect_geom_[0] = xl;
                texrect_geom_[1] = yl;
                texrect_geom_[2] = xh;
                texrect_geom_[3] = yh;
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

            case gbi::OP_LOADTILE: {
                const int tile = static_cast<int>(p0(w1, 24, 3));
                const uint16_t uls = static_cast<uint16_t>(p0(w0, 12, 12));
                const uint16_t ult = static_cast<uint16_t>(p0(w0, 0, 12));
                const uint16_t lrs = static_cast<uint16_t>(p0(w1, 12, 12));
                const uint16_t lrt = static_cast<uint16_t>(p0(w1, 0, 12));
                load_tile_texture(st, tile, uls, ult, lrs, lrt, false);
                break;
            }

            case gbi::OP_LOADBLOCK: {
                const int tile = static_cast<int>(p0(w1, 24, 3));
                const uint16_t uls = static_cast<uint16_t>(p0(w0, 12, 12));
                const uint16_t ult = static_cast<uint16_t>(p0(w0, 0, 12));
                const uint16_t lrs = static_cast<uint16_t>(p0(w1, 12, 12));
                const uint16_t lrt = static_cast<uint16_t>(p0(w1, 0, 12));  // height-1
                load_tile_texture(st, tile, uls, ult, lrs, lrt, true);
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
                const uint32_t size = p0(w0, 0, 8) + 1;
                const uint32_t off = std::max(0, static_cast<int>(32 - p0(w0, 8, 8) - size));
                const uint32_t mask = (size >= 32) ? 0xFFFFFFFFu : ((1u << size) - 1);
                st.othermode_h = (st.othermode_h & ~(mask << off)) | ((w1 & mask) << off);
                break;
            }

            case gbi::OP_SETOTHERMODE_L: {
                const uint32_t size = p0(w0, 0, 8) + 1;
                const uint32_t off = std::max(0, static_cast<int>(32 - p0(w0, 8, 8) - size));
                const uint32_t mask = (size >= 32) ? 0xFFFFFFFFu : ((1u << size) - 1);
                st.othermode_l = (st.othermode_l & ~(mask << off)) & 0xFFFFFF |
                                 ((w1 & mask) << off) & 0xFFFFFF;
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
                st.prim_color[3] = (p0(w1, 0, 8)) / 255.0f;
                st.prim_color[0] = (p0(w1, 8, 8)) / 255.0f;
                st.prim_color[1] = (p0(w1, 16, 8)) / 255.0f;
                st.prim_color[2] = (p0(w1, 24, 8)) / 255.0f;
                break;
            }

            case gbi::OP_SETENVCOLOR: {
                st.env_color[3] = (p0(w1, 0, 8)) / 255.0f;
                st.env_color[0] = (p0(w1, 8, 8)) / 255.0f;
                st.env_color[1] = (p0(w1, 16, 8)) / 255.0f;
                st.env_color[2] = (p0(w1, 24, 8)) / 255.0f;
                break;
            }

            case gbi::OP_SETFILLCOLOR: {
                st.fill_color[3] = (p0(w1, 0, 8)) / 255.0f;
                st.fill_color[0] = (p0(w1, 8, 8)) / 255.0f;
                st.fill_color[1] = (p0(w1, 16, 8)) / 255.0f;
                st.fill_color[2] = (p0(w1, 24, 8)) / 255.0f;
                break;
            }

            case gbi::OP_MOVEMEM:
                break;  // viewport/matrix/light load: handled below via MOVEWORD? no-op for now

            case gbi::OP_MOVEWORD: {
                const uint8_t type = static_cast<uint8_t>(p0(w0, 16, 8));
                if (type == gbi::MW_SEGMENT) {
                    const uint8_t seg = static_cast<uint8_t>(p0(w0, 2, 4));
                    if (seg < 16) {
                        ctx->segments[seg] = w1;
                    }
                }
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
            case gbi::OP_SETFOGCOLOR:
            case gbi::OP_SETBLENDCOLOR:
            case gbi::OP_DL:
            case gbi::OP_ENDDL:
            default:
                break;  // handled by the walker or irrelevant for rendering
        }
    }

    // ----- texture helpers ----------------------------------------------------

    // Decodes the region loaded by LOADTILE/LOADBLOCK and (re)uploads it.
    void load_tile_texture(RenderState& st, int tile, uint16_t uls, uint16_t ult, uint16_t lrs,
                           uint16_t lrt, bool is_block) {
        const TileState& t = st.tiles[tile];

        // Texel rect of the load (RDP coords are 10.2 fixed; >>2 -> texels).
        const int x0 = uls >> 2;
        const int y0 = ult >> 2;
        int w = (lrs >> 2) - x0 + 1;
        int h = (lrt >> 2) - y0 + 1;
        if (is_block) {
            // LOADBLOCK: one row of (lrs+1) texels; height from dxt is
            // approximated as lrt+1 rows.
            w = (lrs >> 2) - x0 + 1;
            h = static_cast<int>(lrt) + 1;
        }
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            return;
        }

        std::vector<uint8_t> pixels;
        decode_texture_rect(rdram_, st.timg_address, t.fmt, t.siz, st.timg_width, x0, y0, w, h,
                            st.tlut, st.tlut_valid, t.palette, pixels);

        // Queue the upload; the main thread creates/updates the GL texture.
        uint64_t key = make_texture_key(st, tile, uls, ult, lrs, lrt, is_block);
        TexUpload up;
        up.key = key;
        up.width = w;
        up.height = h;
        up.clamp_s = (t.cms != 0) || (t.masks == 0);
        up.clamp_t = (t.cmt != 0) || (t.maskt == 0);
        up.bilerp = ((st.othermode_h >> 12) & 3) == 2;  // G_TF_BILERP
        up.pixels = std::move(pixels);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tex_upload_queue_.push_back(std::move(up));
        }

        // Remember the loaded tile for draws.
        st.active_gl_tex = 0;
        st.active_tex_key = key;
        st.active_tex_width = w;
        st.active_tex_height = h;
        st.active_tex_scale[0] = 1.0f / static_cast<float>(w);
        st.active_tex_scale[1] = 1.0f / static_cast<float>(h);
        loaded_tile_uls_ = x0;
        loaded_tile_ult_ = y0;
    }

    uint64_t make_texture_key(const RenderState& st, int tile, uint16_t uls, uint16_t ult,
                              uint16_t lrs, uint16_t lrt, bool is_block) {
        uint64_t k = st.timg_address;
        k = k * 31 + st.timg_fmt;
        k = k * 31 + st.timg_siz;
        k = k * 31 + st.timg_width;
        k = k * 31 + tile;
        k = k * 31 + uls;
        k = k * 31 + ult;
        k = k * 31 + lrs;
        k = k * 31 + lrt;
        k = k * 31 + (is_block ? 1 : 0);
        k = k * 31 + st.tiles[tile].palette;
        return k;
    }

    // ----- drawing (gfx pthread: record commands) --------------------------------

    // Fills the render-state fields of a DrawCmd from the current RDP state.
    void record_state(DrawCmd& cmd, RenderState& st, bool textured) {
        const Combiner& cb = st.combiner;
        for (int i = 0; i < 2; ++i) {
            cmd.ca[i] = cb.ca[i];
            cmd.cb[i] = cb.cb[i];
            cmd.cc[i] = cb.cc[i];
            cmd.cd[i] = cb.cd[i];
            cmd.aa[i] = cb.aa[i];
            cmd.ab[i] = cb.ab[i];
            cmd.ac[i] = cb.ac[i];
            cmd.ad[i] = cb.ad[i];
        }
        cmd.cycle = ((st.othermode_h >> 20) & 3) == 1 ? 2 : 1;
        for (int i = 0; i < 4; ++i) {
            cmd.prim[i] = st.prim_color[i];
            cmd.env[i] = st.env_color[i];
        }
        cmd.textured = textured;
        if (textured) {
            cmd.tex_key = st.active_tex_key;
            cmd.tex_scale[0] = st.active_tex_scale[0];
            cmd.tex_scale[1] = st.active_tex_scale[1];
            cmd.tex_origin[0] = static_cast<float>(loaded_tile_uls_);
            cmd.tex_origin[1] = static_cast<float>(loaded_tile_ult_);
        }
        cmd.scissor_on = st.scissor_enabled;
        if (st.scissor_enabled) {
            cmd.sx0 = st.scissor_x0;
            cmd.sy0 = st.scissor_y0;
            cmd.sx1 = st.scissor_x1;
            cmd.sy1 = st.scissor_y1;
        }
        const int cyc_type = (st.othermode_h >> 20) & 3;
        const uint32_t blend = st.othermode_l & 0xFFF;
        const bool copy_or_fill = (cyc_type == 2 || cyc_type == 3);
        const uint32_t m2a = (blend >> 6) & 7;
        const uint32_t m2b = (blend >> 9) & 7;
        cmd.blend = !copy_or_fill && (m2a == 2 && (m2b == 3 || m2b == 1));
    }

    // Queues one triangle list (NDC pos + uv + shade per vertex).
    void queue_triangles(DrawCmd& cmd, const float* data, int count) {
        cmd.vertex_count = count;
        for (int i = 0; i < count; ++i) {
            cmd.pos[i * 2 + 0] = data[i * 8 + 0];
            cmd.pos[i * 2 + 1] = data[i * 8 + 1];
            cmd.uv[i * 2 + 0] = data[i * 8 + 2];
            cmd.uv[i * 2 + 1] = data[i * 8 + 3];
            cmd.shade[i * 4 + 0] = data[i * 8 + 4];
            cmd.shade[i * 4 + 1] = data[i * 8 + 5];
            cmd.shade[i * 4 + 2] = data[i * 8 + 6];
            cmd.shade[i * 4 + 3] = data[i * 8 + 7];
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (draw_queue_.size() < 16384) {
                draw_queue_.push_back(cmd);
            }
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
        // Perspective divide + viewport. The default viewport maps the
        // projection space to the full canvas (y flipped).
        const float inv_w = 1.0f / pos.w;
        const float sx = pos.x * inv_w;   // [-1, 1] (x)
        const float sy = -pos.y * inv_w;  // flipped: NDC y up
        // The projection matrix in the game maps to a screen space that the
        // RSP viewport then converts; with the identity viewport the values
        // are already in NDC-like units here.
        out[0] = sx;
        out[1] = sy;
        out[2] = (static_cast<float>(v.s)) / 32.0f;
        out[3] = (static_cast<float>(v.t)) / 32.0f;
        out[4] = v.r / 255.0f;
        out[5] = v.g / 255.0f;
        out[6] = v.b / 255.0f;
        out[7] = v.a / 255.0f;
        return true;
    }

    void draw_tri(RenderState& st, int i0, int i1, int i2) {
        if (i0 >= kMaxVertices || i1 >= kMaxVertices || i2 >= kMaxVertices) {
            return;
        }
        const RawVertex* v[3] = {&st.vertices[i0], &st.vertices[i1], &st.vertices[i2]};
        float data[3 * 8];
        for (int i = 0; i < 3; ++i) {
            if (!transform_vertex(st, *v[i], data + i * 8)) {
                return;  // back-facing / clipped
            }
        }
        DrawCmd cmd;
        record_state(cmd, st, st.texture_on && st.active_tex_key != 0);
        queue_triangles(cmd, data, 3);
    }

    void draw_fill_rect(RenderState& st, int xl, int yl, int xh, int yh) {
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
        st.combiner.ca[0] = 3;  // PRIMITIVE
        st.combiner.cb[0] = 0;  // ZERO
        st.combiner.cc[0] = 6;  // ONE
        st.combiner.cd[0] = 0;  // ZERO
        st.combiner.aa[0] = 3;  // PRIMITIVE alpha
        st.combiner.ab[0] = 0;
        st.combiner.ac[0] = 6;
        st.combiner.ad[0] = 0;
        st.combiner.ca[1] = 3;
        st.combiner.cb[1] = 0;
        st.combiner.cc[1] = 6;
        st.combiner.cd[1] = 0;
        st.combiner.aa[1] = 3;
        st.combiner.ab[1] = 0;
        st.combiner.ac[1] = 6;
        st.combiner.ad[1] = 0;

        record_state(cmd, st, false);
        queue_triangles(cmd, data, 6);

        st.prim_color[0] = saved_prim[0];
        st.prim_color[1] = saved_prim[1];
        st.prim_color[2] = saved_prim[2];
        st.prim_color[3] = saved_prim[3];
        st.combiner = saved_comb;
    }

    void draw_texrect(ExecCtx* ctx) {
        RenderState& st = *ctx->st;
        const int xl = texrect_geom_[0], yl = texrect_geom_[1];
        const int xh = texrect_geom_[2], yh = texrect_geom_[3];
        const int16_t s0 = static_cast<int16_t>((ctx->rdp_half1 >> 16) & 0xFFFF);
        const int16_t t0 = static_cast<int16_t>(ctx->rdp_half1 & 0xFFFF);
        const int16_t dsdx = static_cast<int16_t>((ctx->rdp_half2 >> 16) & 0xFFFF);
        const int16_t dtdy = static_cast<int16_t>(ctx->rdp_half2 & 0xFFFF);

        if (st.active_tex_key == 0) {
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
        record_state(cmd, st, true);
        queue_triangles(cmd, data, 6);
    }

    // Screen (pixel) -> NDC.
    float ndc_x(int x) { return 2.0f * static_cast<float>(x) / static_cast<float>(canvas_width_) - 1.0f; }
    float ndc_y(int y) { return 1.0f - 2.0f * static_cast<float>(y) / static_cast<float>(canvas_height_); }

    // ----- matrix helpers ------------------------------------------------------

    Mat4 read_matrix(uint32_t addr) {
        // N64 FixedMatrix: 4x4 of s16 16.16 fixed-point, row-major.
        Mat4 m{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const uint16_t raw = rd16(rdram_ + addr + (r * 4 + c) * 2);
                m.m[r * 4 + c] = static_cast<int16_t>(raw) / 65536.0f;
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
    // the following G_RDPHALF_2).
    int texrect_geom_[4] = {};
    int loaded_tile_uls_ = 0, loaded_tile_ult_ = 0;
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
        fprintf(stderr, "[web-renderer] emscripten_webgl_create_context failed (%d)\n", handle);
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
    std::lock_guard<std::mutex> lock(ogre::g_stats_mutex);
    return ogre::g_stats_snapshot;
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

}  // extern "C"
