// F3DEX2 display-list walker + workload analyzer (see gbi.hpp).
//
// The walker (`walk_dl`) is shared by the workload analyzer (analyze_dl) and
// the WebGL2 renderer prototype (app/src/web_renderer.cpp): both consume the
// same command stream in execution order, with G_DL push/branch semantics,
// segment registers, and loop protection handled by the walker.

#include "gbi.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ogre::gbi {

namespace {

// rdram allocation in this project is 512 MiB (docs/WEB-PORT-REPORT.md §1.5).

// rdram stores 32-bit words byte-reversed (the runtime's MEM_W macro is a
// direct *(int32_t*) read, so memory is little-endian on little-endian hosts
// and yields the game's big-endian value; see N64Recomp/include/recomp.h).
// Read a 32-bit word the way the game sees it:
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// p0/p1 bit extraction, matching RT64's DisplayList::p0/p1 (LSB-based).
inline uint32_t p0(uint32_t w, uint8_t pos, uint8_t bits) {
    return (w >> pos) & ((1u << bits) - 1);
}

// Hard safety limits so a corrupt DL cannot hang the walker.
constexpr uint32_t kMaxBranchDepth = 64;
constexpr uint64_t kMaxCommandsPerDl = 4'000'000;

// Debug: when OGRE_DUMP_DL=1, hexdumps the first words of every DL entered
// (top-level + branch targets) to stderr, for offline inspection.
bool dump_dl_enabled() {
    static const bool enabled = [] {
        const char* v = getenv("OGRE_DUMP_DL");
        return v != nullptr && v[0] == '1';
    }();
    return enabled;
}

void dump_dl_words(const uint8_t* rdram, uint32_t offset, uint32_t words) {
    if (!dump_dl_enabled()) {
        return;
    }
    fprintf(stderr, "[gbi] DL @ 0x%08X:\n", offset);
    for (uint32_t i = 0; i < words && offset + (i + 1) * 8 <= kRdramSize; ++i) {
        const uint8_t* p = rdram + offset + i * 8;
        fprintf(stderr, "  %04X: %08X %08X\n", i * 8, rd32(p), rd32(p + 4));
    }
}

}  // namespace

void walk_dl(uint8_t* rdram, uint32_t dl_offset, DlVisitor visitor, void* user) {
    if (rdram == nullptr || visitor == nullptr) {
        return;
    }

    // Segment registers (G_MOVEWORD G_MW_SEGMENT); index 0 = physical.
    uint32_t segments[16] = {};

    // Walks one linear DL; returns when it ends (G_ENDDL / out of range /
    // budget exhausted) or on a non-returning G_DL branch (*continue_offset
    // set, *continued true).
    struct LinearWalker {
        uint8_t* rdram;
        uint32_t segments[16];
        DlVisitor visitor;
        void* user;

        void walk(uint32_t offset, uint32_t depth, uint64_t& budget, uint32_t& continue_offset,
                  bool& continued) {
            continued = false;
            continue_offset = 0;
            if (depth > kMaxBranchDepth) {
                return;
            }

            // Shared with the executor (gbi.hpp) so both views of the walk
            // resolve every address identically.
            auto resolve = [this](uint32_t addr) -> uint32_t {
                return gbi::resolve_address(segments, addr);
            };

            for (;;) {
                if (offset + 8 > kRdramSize || offset + 8 < offset) {
                    break;
                }
                if (budget == 0) {
                    break;
                }
                --budget;

                const uint8_t* p = rdram + offset;
                const uint32_t w0 = rd32(p);
                const uint32_t w1 = rd32(p + 4);
                const uint8_t op = static_cast<uint8_t>(w0 >> 24);

                DlCommand cmd{w0, w1, op, offset};
                visitor(user, cmd);

                if (op == OP_ENDDL) {
                    return;
                }

                // G_MOVEWORD G_MW_SEGMENT: without this the walker's segment
                // registers stayed zero, so a G_DL to a segmented address was
                // followed as if it were physical.
                if (op == OP_MOVEWORD) {
                    set_segment_from_moveword(segments, w0, w1);
                    offset += 8;
                    continue;
                }

                if (op == OP_DL) {
                    const bool push = ((w0 >> 16) & 1) == DL_PUSH;
                    const uint32_t target = resolve(w1);
                    if (target + 8 > kRdramSize) {
                        return;
                    }
                    dump_dl_words(rdram, target, 8);
                    if (!push) {
                        continue_offset = target;
                        continued = true;
                        return;
                    }
                    // Push DL: execute the target now; on its ENDDL we continue
                    // at offset+8 below.
                    walk(target, depth + 1, budget, continue_offset, continued);
                    if (continued) {
                        return;
                    }
                    offset += 8;
                    continue;
                }

                // G_BRANCH_Z is the only 16-byte F3DEX2 command.
                offset += (op == OP_BRANCH_Z) ? 16 : 8;
            }
        }
    };

    dump_dl_words(rdram, dl_offset, 8);

    LinearWalker walker{rdram, {}, visitor, user};
    uint64_t budget = kMaxCommandsPerDl;
    uint32_t offset = dl_offset;
    uint32_t depth = 1;
    for (;;) {
        uint32_t continue_offset = 0;
        bool continued = false;
        walker.walk(offset, depth, budget, continue_offset, continued);
        if (!continued || budget == 0) {
            break;
        }
        offset = continue_offset;
        ++depth;
    }
}

namespace {

// Visitor that accumulates workload statistics (plan §15).
struct StatsVisitor {
    WorkloadStats* stats;
    uint32_t dl_entries = 0;

    void cmd(const DlCommand& c) {
        WorkloadStats& s = *stats;
        ++s.commands;
        ++s.cmd_counts[c.op];

        // Anything outside the recognized command set counts as unknown. A
        // switch rather than a designated-initializer table: GCC rejects
        // `[OP_X] = true` array initializers unless the designators are in
        // declaration order ("non-trivial designated initializers not
        // supported"), and the OP_* constants below are not in opcode order.
        const auto known_op = [](uint8_t op) {
            switch (op) {
                case OP_VTX: case OP_MODIFYVTX: case OP_CULLDL:
                case OP_BRANCH_Z: case OP_TRI1: case OP_TRI2:
                case OP_QUAD: case OP_LINE3D: case OP_SPECIAL_1:
                case OP_DMA_IO: case OP_TEXTURE: case OP_POPMTX:
                case OP_GEOMETRYMODE: case OP_MTX: case OP_MOVEWORD:
                case OP_MOVEMEM: case OP_LOAD_UCODE: case OP_DL:
                case OP_ENDDL: case OP_SPNOOP: case OP_RDPHALF_1:
                case OP_SETOTHERMODE_L: case OP_SETOTHERMODE_H:
                case OP_TEXRECT: case OP_TEXRECTFLIP:
                case OP_RDPLOADSYNC: case OP_RDPPIPESYNC:
                case OP_RDPTILESYNC: case OP_RDPFULLSYNC:
                case OP_SETKEYGB: case OP_SETKEYR: case OP_SETCONVERT:
                case OP_SETSCISSOR: case OP_SETPRIMDEPTH:
                case OP_RDPSETOTHERMODE: case OP_LOADTLUT:
                case OP_RDPHALF_2: case OP_SETTILESIZE:
                case OP_LOADBLOCK: case OP_LOADTILE: case OP_SETTILE:
                case OP_FILLRECT: case OP_SETFILLCOLOR:
                case OP_SETFOGCOLOR: case OP_SETBLENDCOLOR:
                case OP_SETPRIMCOLOR: case OP_SETENVCOLOR:
                case OP_SETCOMBINE: case OP_SETTIMG: case OP_SETZIMG:
                case OP_SETCIMG: case 0x00:  // G_NOOP
                    return true;
                default:
                    return false;
            }
        };
        if (!known_op(c.op)) {
            ++s.unknown_cmds;
        }

        switch (c.op) {
            case OP_ENDDL:
                return;

            case OP_DL:
                ++s.dls_walked;
                return;

            case OP_VTX: {
                const uint32_t n = p0(c.w0, 12, 8);  // vertex count (bits 12-19)
                ++s.vtx_calls;
                s.vertices += n;
                return;
            }

            case OP_TRI1:
                ++s.tri1;
                ++s.triangles;
                return;

            case OP_TRI2:
                ++s.tri2;
                s.triangles += 2;
                return;

            case OP_QUAD:
                ++s.quad;
                s.triangles += 2;
                return;

            case OP_LINE3D:
                ++s.line3d;
                return;

            case OP_BRANCH_Z:
                ++s.branch_z;
                return;

            case OP_MODIFYVTX:
                ++s.modifyvtx;
                return;

            case OP_CULLDL:
                ++s.cull_dl;
                return;

            case OP_TEXRECT:
                ++s.texrect;
                return;

            case OP_TEXRECTFLIP:
                ++s.texrect_flip;
                return;

            case OP_FILLRECT:
                ++s.fillrect;
                return;

            case OP_SETTIMG: {
                ++s.settimg;
                const uint8_t fmt = static_cast<uint8_t>(p0(c.w0, 21, 3));
                const uint8_t siz = static_cast<uint8_t>(p0(c.w0, 19, 2));
                const uint16_t width = static_cast<uint16_t>(p0(c.w0, 0, 12) + 1);
                ++s.timg_formats[(static_cast<uint32_t>(fmt) << 20) |
                                 (static_cast<uint32_t>(siz) << 18) | width];
                return;
            }

            case OP_SETTILE:
                ++s.settile;
                return;

            case OP_SETTILESIZE:
                ++s.settilesize;
                return;

            case OP_LOADTILE:
                ++s.loadtile;
                return;

            case OP_LOADBLOCK:
                ++s.loadblock;
                return;

            case OP_LOADTLUT:
                ++s.loadtlut;
                return;

            case OP_SETCOMBINE:
                s.combiners.insert((static_cast<uint64_t>(c.w0) << 32) | c.w1);
                return;

            case OP_SETOTHERMODE_H:
                s.othermode_h.insert(c.w1);
                return;

            case OP_SETOTHERMODE_L:
                s.othermode_l.insert(c.w1);
                return;

            case OP_RDPSETOTHERMODE:
                s.othermode_h.insert(p0(c.w0, 0, 24));
                s.othermode_l.insert(c.w1);
                return;

            case OP_MTX:
                ++s.mtx;
                return;

            case OP_POPMTX:
                ++s.popmtx;
                return;

            case OP_GEOMETRYMODE:
                ++s.geometrymode;
                return;

            case OP_TEXTURE:
                ++s.texture_cmd;
                return;

            case OP_MOVEMEM:
                ++s.movemem;
                return;

            case OP_MOVEWORD:
                ++s.moveword;
                return;

            case OP_SETPRIMCOLOR:
                ++s.setprimcolor;
                return;

            case OP_SETENVCOLOR:
                ++s.setenvcolor;
                return;

            case OP_SETBLENDCOLOR:
                ++s.setblendcolor;
                return;

            case OP_SETFOGCOLOR:
                ++s.setfogcolor;
                return;

            case OP_SETFILLCOLOR:
                ++s.setfillcolor;
                return;

            case OP_SETSCISSOR:
                ++s.setscissor;
                return;

            case OP_SETPRIMDEPTH:
                ++s.setprimdepth;
                return;

            case OP_SETCIMG:
                ++s.setcimg;
                return;

            case OP_SETZIMG:
                ++s.setzimg;
                return;

            default:
                // NOOP, SPECIAL_1, DMA_IO, LOAD_UCODE, SPNOOP, RDPHALF_1/2,
                // key/setconvert, syncs: no workload semantics.
                return;
        }
    }
};

void stats_visitor(void* user, const DlCommand& cmd) {
    static_cast<StatsVisitor*>(user)->cmd(cmd);
}

}  // namespace

void analyze_dl(uint8_t* rdram, uint32_t dl_offset, uint32_t dl_size_hint, WorkloadStats& stats) {
    (void)dl_size_hint;
    if (rdram == nullptr) {
        return;
    }
    ++stats.tasks;

    StatsVisitor visitor{&stats};
    walk_dl(rdram, dl_offset, stats_visitor, &visitor);
    ++stats.dls_walked;  // the top-level DL
    stats.max_branch_depth = std::max<uint32_t>(stats.max_branch_depth, 1);
}

void reset(WorkloadStats& stats) {
    WorkloadStats fresh{};
    stats = fresh;
}

const char* opcode_name(uint8_t op) {
    switch (op) {
        case OP_VTX: return "G_VTX";
        case OP_MODIFYVTX: return "G_MODIFYVTX";
        case OP_CULLDL: return "G_CULLDL";
        case OP_BRANCH_Z: return "G_BRANCH_Z";
        case OP_TRI1: return "G_TRI1";
        case OP_TRI2: return "G_TRI2";
        case OP_QUAD: return "G_QUAD";
        case OP_LINE3D: return "G_LINE3D";
        case OP_SPECIAL_1: return "G_SPECIAL_1";
        case OP_DMA_IO: return "G_DMA_IO";
        case OP_TEXTURE: return "G_TEXTURE";
        case OP_POPMTX: return "G_POPMTX";
        case OP_GEOMETRYMODE: return "G_GEOMETRYMODE";
        case OP_MTX: return "G_MTX";
        case OP_MOVEWORD: return "G_MOVEWORD";
        case OP_MOVEMEM: return "G_MOVEMEM";
        case OP_LOAD_UCODE: return "G_LOAD_UCODE";
        case OP_DL: return "G_DL";
        case OP_ENDDL: return "G_ENDDL";
        case OP_SPNOOP: return "G_SPNOOP";
        case OP_RDPHALF_1: return "G_RDPHALF_1";
        case OP_SETOTHERMODE_L: return "G_SETOTHERMODE_L";
        case OP_SETOTHERMODE_H: return "G_SETOTHERMODE_H";
        case OP_TEXRECT: return "G_TEXRECT";
        case OP_TEXRECTFLIP: return "G_TEXRECTFLIP";
        case OP_RDPLOADSYNC: return "G_RDPLOADSYNC";
        case OP_RDPPIPESYNC: return "G_RDPPIPESYNC";
        case OP_RDPTILESYNC: return "G_RDPTILESYNC";
        case OP_RDPFULLSYNC: return "G_RDPFULLSYNC";
        case OP_SETKEYGB: return "G_SETKEYGB";
        case OP_SETKEYR: return "G_SETKEYR";
        case OP_SETCONVERT: return "G_SETCONVERT";
        case OP_SETSCISSOR: return "G_SETSCISSOR";
        case OP_SETPRIMDEPTH: return "G_SETPRIMDEPTH";
        case OP_RDPSETOTHERMODE: return "G_RDPSETOTHERMODE";
        case OP_LOADTLUT: return "G_LOADTLUT";
        case OP_RDPHALF_2: return "G_RDPHALF_2";
        case OP_SETTILESIZE: return "G_SETTILESIZE";
        case OP_LOADBLOCK: return "G_LOADBLOCK";
        case OP_LOADTILE: return "G_LOADTILE";
        case OP_SETTILE: return "G_SETTILE";
        case OP_FILLRECT: return "G_FILLRECT";
        case OP_SETFILLCOLOR: return "G_SETFILLCOLOR";
        case OP_SETFOGCOLOR: return "G_SETFOGCOLOR";
        case OP_SETBLENDCOLOR: return "G_SETBLENDCOLOR";
        case OP_SETPRIMCOLOR: return "G_SETPRIMCOLOR";
        case OP_SETENVCOLOR: return "G_SETENVCOLOR";
        case OP_SETCOMBINE: return "G_SETCOMBINE";
        case OP_SETTIMG: return "G_SETTIMG";
        case OP_SETZIMG: return "G_SETZIMG";
        case OP_SETCIMG: return "G_SETCIMG";
        default: return "??";
    }
}

namespace {

const char* fmt_name(uint8_t fmt) {
    switch (fmt) {
        case IM_FMT_RGBA: return "RGBA";
        case IM_FMT_YUV: return "YUV";
        case IM_FMT_CI: return "CI";
        case IM_FMT_IA: return "IA";
        case IM_FMT_I: return "I";
        default: return "?";
    }
}

const char* siz_name(uint8_t siz) {
    switch (siz) {
        case IM_SIZ_4b: return "4b";
        case IM_SIZ_8b: return "8b";
        case IM_SIZ_16b: return "16b";
        case IM_SIZ_32b: return "32b";
        default: return "?";
    }
}

}  // namespace

std::string format_summary(const WorkloadStats& s) {
    char buf[4096];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n,
                  "tasks=%llu dls=%llu cmds=%llu unknown=%llu maxdepth=%u\n",
                  static_cast<unsigned long long>(s.tasks),
                  static_cast<unsigned long long>(s.dls_walked),
                  static_cast<unsigned long long>(s.commands),
                  static_cast<unsigned long long>(s.unknown_cmds), s.max_branch_depth);

    n += snprintf(buf + n, sizeof(buf) - n,
                  "geometry: vtx_calls=%llu vertices=%llu tri1=%llu tri2=%llu quad=%llu "
                  "triangles=%llu line3d=%llu texrect=%llu texrect_flip=%llu fillrect=%llu\n",
                  static_cast<unsigned long long>(s.vtx_calls),
                  static_cast<unsigned long long>(s.vertices),
                  static_cast<unsigned long long>(s.tri1),
                  static_cast<unsigned long long>(s.tri2),
                  static_cast<unsigned long long>(s.quad),
                  static_cast<unsigned long long>(s.triangles),
                  static_cast<unsigned long long>(s.line3d),
                  static_cast<unsigned long long>(s.texrect),
                  static_cast<unsigned long long>(s.texrect_flip),
                  static_cast<unsigned long long>(s.fillrect));

    n += snprintf(buf + n, sizeof(buf) - n,
                  "textures: settimg=%llu (formats: ", static_cast<unsigned long long>(s.settimg));
    for (const auto& [key, count] : s.timg_formats) {
        const uint8_t fmt = static_cast<uint8_t>((key >> 20) & 0x7);
        const uint8_t siz = static_cast<uint8_t>((key >> 18) & 0x3);
        const uint16_t width = static_cast<uint16_t>(key & 0x3FF);
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s x%u=%llu ", fmt_name(fmt), siz_name(siz),
                      static_cast<unsigned>(width), static_cast<unsigned long long>(count));
    }
    n += snprintf(buf + n, sizeof(buf) - n, ") settile=%llu settilesize=%llu loadtile=%llu "
                  "loadblock=%llu loadtlut=%llu\n",
                  static_cast<unsigned long long>(s.settile),
                  static_cast<unsigned long long>(s.settilesize),
                  static_cast<unsigned long long>(s.loadtile),
                  static_cast<unsigned long long>(s.loadblock),
                  static_cast<unsigned long long>(s.loadtlut));

    n += snprintf(buf + n, sizeof(buf) - n,
                  "combiner: unique_configs=%zu\n",
                  s.combiners.size());
    n += snprintf(buf + n, sizeof(buf) - n,
                  "othermode_h: %zu unique values; othermode_l: %zu unique values\n",
                  s.othermode_h.size(), s.othermode_l.size());
    n += snprintf(buf + n, sizeof(buf) - n,
                  "state: mtx=%llu popmtx=%llu geomode=%llu texture=%llu movemem=%llu moveword=%llu "
                  "setprimcolor=%llu setenvcolor=%llu setscissor=%llu setcimg=%llu setzimg=%llu\n",
                  static_cast<unsigned long long>(s.mtx),
                  static_cast<unsigned long long>(s.popmtx),
                  static_cast<unsigned long long>(s.geometrymode),
                  static_cast<unsigned long long>(s.texture_cmd),
                  static_cast<unsigned long long>(s.movemem),
                  static_cast<unsigned long long>(s.moveword),
                  static_cast<unsigned long long>(s.setprimcolor),
                  static_cast<unsigned long long>(s.setenvcolor),
                  static_cast<unsigned long long>(s.setscissor),
                  static_cast<unsigned long long>(s.setcimg),
                  static_cast<unsigned long long>(s.setzimg));

    return std::string(buf, static_cast<size_t>(std::max(n, 0)));
}

namespace {

// Decodes one command into `line` (offset + raw words are added by the caller).
// `pending` carries the G_TEXRECT halves (G_RDPHALF_1/2 follow the rect command
// in the F3DEX2 stream; RT64's rt64_gbi_rdp.cpp texrect() reads exactly those
// two following words for uls/ult and dsdx/dtdy).
struct DecodeVisitor {
    std::string* out;
    // Pending texrect state.
    bool rect_pending = false;
    bool rect_flip = false;
    uint32_t rect_w0 = 0, rect_w1 = 0;
    bool have_st = false;
    int16_t uls = 0, ult = 0;
    // Last texture setup, echoed on each texrect.
    uint32_t timg_addr = 0;
    uint8_t timg_fmt = 0, timg_siz = 0;
    uint32_t timg_width = 0;

    void line(const DlCommand& c, const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        char head[64];
        snprintf(head, sizeof(head), "%05X %02X %08X %08X  ", c.offset, c.op, c.w0, c.w1);
        *out += head;
        *out += buf;
        *out += '\n';
    }

    void cmd(const DlCommand& c) {
        const uint32_t a = c.w0, b = c.w1;
        switch (c.op) {
            case OP_MOVEWORD:
                if (p0(a, 16, 8) == MW_SEGMENT) {
                    line(c, "MOVEWORD segment[%u] = 0x%08X", p0(a, 2, 4), b);
                } else {
                    line(c, "MOVEWORD type=0x%02X idx=%u value=0x%08X", p0(a, 16, 8), p0(a, 0, 16), b);
                }
                return;
            case OP_SETTIMG:
                timg_fmt = static_cast<uint8_t>(p0(a, 21, 3));
                timg_siz = static_cast<uint8_t>(p0(a, 19, 2));
                timg_width = p0(a, 0, 12) + 1;
                timg_addr = b;
                line(c, "SETTIMG fmt=%s siz=%s width=%u addr=0x%08X", fmt_name(timg_fmt),
                     siz_name(timg_siz), timg_width, timg_addr);
                return;
            case OP_SETTILE:
                line(c, "SETTILE t%u fmt=%u siz=%u line=%u tmem=0x%X pal=%u cms=%u cmt=%u "
                        "masks=%u maskt=%u shifts=%u shiftt=%u",
                     p0(b, 24, 3), p0(a, 21, 3), p0(a, 19, 2), p0(a, 9, 9), p0(a, 0, 9),
                     p0(b, 20, 4), p0(b, 8, 2), p0(b, 18, 2), p0(b, 4, 4), p0(b, 14, 4),
                     p0(b, 0, 4), p0(b, 10, 4));
                return;
            case OP_SETTILESIZE:
                line(c, "SETTILESIZE t%u uls=%u ult=%u lrs=%u lrt=%u", p0(b, 24, 3),
                     p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12));
                return;
            case OP_LOADBLOCK:
                line(c, "LOADBLOCK t%u uls=%u ult=%u texels=%u dxt=%u", p0(b, 24, 3),
                     p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 11));
                return;
            case OP_LOADTILE:
                line(c, "LOADTILE t%u uls=%u ult=%u lrs=%u lrt=%u", p0(b, 24, 3),
                     p0(a, 12, 12), p0(a, 0, 12), p0(b, 12, 12), p0(b, 0, 12));
                return;
            case OP_LOADTLUT:
                line(c, "LOADTLUT t%u uls=%u ult=%u count=%u", p0(b, 24, 3), p0(a, 12, 12),
                     p0(a, 0, 12), p0(b, 14, 10));
                return;
            case OP_TEXRECT:
            case OP_TEXRECTFLIP:
                rect_pending = true;
                rect_flip = (c.op == OP_TEXRECTFLIP);
                rect_w0 = a;
                rect_w1 = b;
                have_st = false;
                return;
            case OP_RDPHALF_1:
                if (rect_pending && !have_st) {
                    uls = static_cast<int16_t>(p0(b, 16, 16));
                    ult = static_cast<int16_t>(p0(b, 0, 16));
                    have_st = true;
                    return;
                }
                line(c, "RDPHALF_1 0x%08X", b);
                return;
            case OP_RDPHALF_2:
                if (rect_pending && have_st) {
                    const int16_t dsdx = static_cast<int16_t>(p0(b, 16, 16));
                    const int16_t dtdy = static_cast<int16_t>(p0(b, 0, 16));
                    line(c, "%s ulx=%d uly=%d lrx=%d lry=%d tile=%u s=%d t=%d dsdx=%d dtdy=%d "
                            "(timg fmt=%u siz=%u w=%u @0x%08X)",
                         rect_flip ? "TEXRECTFLIP" : "TEXRECT", (int)p0(rect_w1, 12, 12),
                         (int)p0(rect_w1, 0, 12), (int)p0(rect_w0, 12, 12), (int)p0(rect_w0, 0, 12),
                         p0(rect_w1, 24, 3), (int)uls, (int)ult, (int)dsdx, (int)dtdy, timg_fmt,
                         timg_siz, timg_width, timg_addr);
                    rect_pending = false;
                    return;
                }
                line(c, "RDPHALF_2 0x%08X", b);
                return;
            case OP_FILLRECT:
                // RT64 GBI_RDP::fillRect reads the upper-left corner from the
                // second word (p1) and the lower-right from the first (p0),
                // like TEXRECT.
                line(c, "FILLRECT ulx=%d uly=%d lrx=%d lry=%d", (int)p0(b, 12, 12), (int)p0(b, 0, 12),
                     (int)p0(a, 12, 12), (int)p0(a, 0, 12));
                return;
            case OP_SETSCISSOR:
                line(c, "SETSCISSOR ulx=%d uly=%d lrx=%d lry=%d mode=%u", (int)p0(a, 12, 12),
                     (int)p0(a, 0, 12), (int)p0(b, 12, 12), (int)p0(b, 0, 12), p0(b, 24, 2));
                return;
            case OP_SETCIMG:
            case OP_SETZIMG:
                line(c, "%s fmt=%u siz=%u width=%u addr=0x%08X",
                     (c.op == OP_SETCIMG) ? "SETCIMG" : "SETZIMG", p0(a, 21, 3), p0(a, 19, 2),
                     p0(a, 0, 12) + 1, b);
                return;
            case OP_SETCOMBINE:
                line(c, "SETCOMBINE");
                return;
            case OP_SETOTHERMODE_H:
                line(c, "SETOTHERMODE_H shift=%u len=%u data=0x%08X", p0(a, 8, 8), p0(a, 0, 8), b);
                return;
            case OP_SETOTHERMODE_L:
                line(c, "SETOTHERMODE_L shift=%u len=%u data=0x%08X", p0(a, 8, 8), p0(a, 0, 8), b);
                return;
            case OP_SETPRIMCOLOR:
                line(c, "SETPRIMCOLOR lodfrac=%u mip=%u 0x%08X", p0(a, 8, 8), p0(a, 0, 8), b);
                return;
            case OP_SETENVCOLOR:
            case OP_SETBLENDCOLOR:
            case OP_SETFOGCOLOR:
            case OP_SETFILLCOLOR:
                line(c, "%s 0x%08X", opcode_name(c.op), b);
                return;
            case OP_GEOMETRYMODE:
                line(c, "GEOMETRYMODE clear=0x%08X set=0x%08X", a & 0xFFFFFF, b);
                return;
            case OP_TEXTURE:
                line(c, "TEXTURE tile=%u level=%u on=%u sc=%u tc=%u", p0(a, 8, 3), p0(a, 11, 3),
                     p0(a, 1, 7), p0(b, 16, 16), p0(b, 0, 16));
                return;
            case OP_VTX:
                line(c, "VTX n=%u v0=%u addr=0x%08X", p0(a, 12, 8), p0(a, 1, 7), b);
                return;
            default:
                line(c, "%s", opcode_name(c.op));
                return;
        }
    }
};

void decode_trampoline(void* user, const DlCommand& cmd) {
    static_cast<DecodeVisitor*>(user)->cmd(cmd);
}

}  // namespace

std::string decode_dl(uint8_t* rdram, uint32_t dl_offset) {
    std::string out;
    out.reserve(64 * 1024);
    DecodeVisitor visitor{&out};
    walk_dl(rdram, dl_offset, &decode_trampoline, &visitor);
    return out;
}

}  // namespace ogre::gbi
