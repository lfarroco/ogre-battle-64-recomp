#pragma once

// F3DEX2 display-list parser + graphics workload analyzer for the Ogre
// Battle 64 browser port (docs/WEB-PORT.md §14-15, milestone 6).
//
// OB64's gfx microcode is "RSP Gfx ucode F3DEX fifo 2.08", which RT64's
// database matches (by ucode hash) to GBIUCode::F3DEX2 (see
// docs/HANDOFF-2026-08-29-session10.md). The tables and bit layouts below are
// taken from RT64's GBI interpreters:
//   tools/RT64/src/gbi/rt64_gbi_f3dex2.h     (RSP-side F3DEX2 opcodes)
//   tools/RT64/src/shared/rt64_f3d_defines.h (RDP opcodes + mode bits)
//   tools/RT64/src/gbi/rt64_gbi_f3dex2.cpp   (command decodes)
//   tools/RT64/src/gbi/rt64_gbi.cpp          (DisplayList::p0/p1)
//
// This is a *workload analyzer*, not a renderer: it walks display lists and
// counts what the game actually uses (commands, geometry, textures, combiner
// configs, render modes) so the browser-renderer decision (§16) can be made
// from data. The same walker is also the front-end of the WebGL2 renderer
// prototype (app/src/web_renderer.cpp).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ogre::gbi {

// F3DEX2 opcode constants (RT64 rt64_gbi_f3dex2.h + rt64_f3d_defines.h).
// RSP-side commands:
constexpr uint8_t OP_VTX = 0x01;            // G_VTX
constexpr uint8_t OP_MODIFYVTX = 0x02;      // G_MODIFYVTX
constexpr uint8_t OP_CULLDL = 0x03;         // G_CULLDL
constexpr uint8_t OP_BRANCH_Z = 0x04;       // G_BRANCH_Z (16-byte command)
constexpr uint8_t OP_TRI1 = 0x05;           // G_TRI1
constexpr uint8_t OP_TRI2 = 0x06;           // G_TRI2 (two triangles in one word)
constexpr uint8_t OP_QUAD = 0x07;           // G_QUAD (two triangles in one word)
constexpr uint8_t OP_LINE3D = 0x08;         // G_LINE3D
constexpr uint8_t OP_SPECIAL_1 = 0xD5;      // G_SPECIAL_1
constexpr uint8_t OP_DMA_IO = 0xD6;         // G_DMA_IO
constexpr uint8_t OP_TEXTURE = 0xD7;        // G_TEXTURE
constexpr uint8_t OP_POPMTX = 0xD8;         // G_POPMTX
constexpr uint8_t OP_GEOMETRYMODE = 0xD9;   // G_GEOMETRYMODE
constexpr uint8_t OP_MTX = 0xDA;            // G_MTX
constexpr uint8_t OP_MOVEWORD = 0xDB;       // G_MOVEWORD
constexpr uint8_t OP_MOVEMEM = 0xDC;        // G_MOVEMEM
constexpr uint8_t OP_LOAD_UCODE = 0xDD;     // G_LOAD_UCODE
constexpr uint8_t OP_DL = 0xDE;             // G_DL
constexpr uint8_t OP_ENDDL = 0xDF;          // G_ENDDL
constexpr uint8_t OP_SPNOOP = 0xE0;         // G_SPNOOP
constexpr uint8_t OP_RDPHALF_1 = 0xE1;      // G_RDPHALF_1
constexpr uint8_t OP_SETOTHERMODE_L = 0xE2; // G_SETOTHERMODE_L
constexpr uint8_t OP_SETOTHERMODE_H = 0xE3; // G_SETOTHERMODE_H

// RDP-side commands (shared opcode space with the RSP commands above):
constexpr uint8_t OP_TEXRECT = 0xE4;        // G_TEXRECT
constexpr uint8_t OP_TEXRECTFLIP = 0xE5;    // G_TEXRECTFLIP
constexpr uint8_t OP_RDPLOADSYNC = 0xE6;
constexpr uint8_t OP_RDPPIPESYNC = 0xE7;
constexpr uint8_t OP_RDPTILESYNC = 0xE8;
constexpr uint8_t OP_RDPFULLSYNC = 0xE9;
constexpr uint8_t OP_SETKEYGB = 0xEA;
constexpr uint8_t OP_SETKEYR = 0xEB;
constexpr uint8_t OP_SETCONVERT = 0xEC;
constexpr uint8_t OP_SETSCISSOR = 0xED;
constexpr uint8_t OP_SETPRIMDEPTH = 0xEE;
constexpr uint8_t OP_RDPSETOTHERMODE = 0xEF; // G_RDPSETOTHERMODE (full 64-bit set)
constexpr uint8_t OP_LOADTLUT = 0xF0;
constexpr uint8_t OP_RDPHALF_2 = 0xF1;
constexpr uint8_t OP_SETTILESIZE = 0xF2;
constexpr uint8_t OP_LOADBLOCK = 0xF3;
constexpr uint8_t OP_LOADTILE = 0xF4;
constexpr uint8_t OP_SETTILE = 0xF5;
constexpr uint8_t OP_FILLRECT = 0xF6;
constexpr uint8_t OP_SETFILLCOLOR = 0xF7;
constexpr uint8_t OP_SETFOGCOLOR = 0xF8;
constexpr uint8_t OP_SETBLENDCOLOR = 0xF9;
constexpr uint8_t OP_SETPRIMCOLOR = 0xFA;
constexpr uint8_t OP_SETENVCOLOR = 0xFB;
constexpr uint8_t OP_SETCOMBINE = 0xFC;
constexpr uint8_t OP_SETTIMG = 0xFD;
constexpr uint8_t OP_SETZIMG = 0xFE;
constexpr uint8_t OP_SETCIMG = 0xFF;

// Texture image formats (G_IM_FMT_*).
constexpr uint8_t IM_FMT_RGBA = 0;
constexpr uint8_t IM_FMT_YUV = 1;
constexpr uint8_t IM_FMT_CI = 2;
constexpr uint8_t IM_FMT_IA = 3;
constexpr uint8_t IM_FMT_I = 4;

// Texture image sizes (G_IM_SIZ_*).
constexpr uint8_t IM_SIZ_4b = 0;
constexpr uint8_t IM_SIZ_8b = 1;
constexpr uint8_t IM_SIZ_16b = 2;
constexpr uint8_t IM_SIZ_32b = 3;

// G_MOVEMEM indices (F3DEX2).
constexpr uint8_t MV_VIEWPORT = 8;
constexpr uint8_t MV_MATRIX = 14;
constexpr uint8_t MV_LIGHT = 10;

// G_MOVEWORD types.
constexpr uint8_t MW_MATRIX = 0x00;
constexpr uint8_t MW_NUMLIGHT = 0x02;
constexpr uint8_t MW_CLIP = 0x04;
constexpr uint8_t MW_SEGMENT = 0x06;
constexpr uint8_t MW_FOG = 0x08;
constexpr uint8_t MW_LIGHTCOL = 0x0A;
constexpr uint8_t MW_PERSPNORM = 0x0E;

// G_MTX flags (w0 bits 0-7).
constexpr uint32_t MTX_PUSH = 0x01;
constexpr uint32_t MTX_LOAD = 0x02;
constexpr uint32_t MTX_PROJECTION = 0x04;

// G_DL flag: bit 16 of w0. 0 = push (execute then return), 1 = branch (no return).
constexpr uint32_t DL_PUSH = 0x00;
constexpr uint32_t DL_BRANCH = 0x01;

// Aggregate workload statistics across any number of analyzed tasks
// (plan §15). All counts are cumulative.
struct WorkloadStats {
    // --- DL structure -----------------------------------------------------
    uint64_t tasks = 0;          // tasks fed to the analyzer
    uint64_t dls_walked = 0;     // top-level + branched DLs executed
    uint64_t commands = 0;       // 64-bit commands consumed
    uint64_t unknown_cmds = 0;   // opcodes with no known meaning
    uint32_t max_branch_depth = 0;
    uint64_t cmd_counts[256] = {};  // histogram by opcode byte

    // --- Geometry ---------------------------------------------------------
    uint64_t vtx_calls = 0;
    uint64_t vertices = 0;       // total vertices loaded
    uint64_t tri1 = 0, tri2 = 0, quad = 0, line3d = 0;
    uint64_t triangles = 0;      // tri1 + 2*tri2 + 2*quad
    uint64_t texrect = 0, texrect_flip = 0, fillrect = 0;

    // --- Textures ---------------------------------------------------------
    uint64_t settimg = 0;
    // key = (fmt << 20) | (siz << 18) | width   (width in pixels, 1..1024)
    std::map<uint32_t, uint64_t> timg_formats;
    uint64_t settile = 0, settilesize = 0, loadtile = 0, loadblock = 0, loadtlut = 0;

    // --- Combiner / render state ------------------------------------------
    // Full 64-bit SETCOMBINE commands (w0<<32 | w1); each unique value is one
    // distinct color-combiner configuration.
    std::set<uint64_t> combiners;
    // Raw OTHERMODE values (w1 for G_SETOTHERMODE_H/L; the 64-bit G_RDPSETOTHERMODE
    // contributes its low word here).
    std::set<uint32_t> othermode_h;
    std::set<uint32_t> othermode_l;
    uint64_t setprimcolor = 0, setenvcolor = 0, setblendcolor = 0, setfogcolor = 0;
    uint64_t setfillcolor = 0, setscissor = 0, setprimdepth = 0, setcimg = 0, setzimg = 0;

    // --- Matrix / other RSP state ------------------------------------------
    uint64_t mtx = 0, popmtx = 0, geometrymode = 0, texture_cmd = 0;
    uint64_t movemem = 0, moveword = 0, modifyvtx = 0, cull_dl = 0, branch_z = 0;
};

// Walks the F3DEX2 display list rooted at `dl_offset` (an offset into rdram)
// and accumulates statistics into `stats`. `dl_size_hint` is the OSTask
// data_size (informational; branches may leave it). Safe against loops and
// out-of-range reads.
void analyze_dl(uint8_t* rdram, uint32_t dl_offset, uint32_t dl_size_hint, WorkloadStats& stats);

// A single 64-bit display-list command as seen by the walker.
struct DlCommand {
    uint32_t w0;        // first word, game-endian
    uint32_t w1;        // second word
    uint8_t op;         // w0 >> 24
    uint32_t offset;    // rdram offset of the command
};

// Visitor callback for walk_dl: called once per command in execution order
// (including G_DL and G_ENDDL themselves).
using DlVisitor = void (*)(void* user, const DlCommand& cmd);

// Walks a display list in execution order (G_DL push/branch semantics, segment
// registers from G_MOVEWORD, loop protection). Shared by the workload analyzer
// and the WebGL2 renderer.
void walk_dl(uint8_t* rdram, uint32_t dl_offset, DlVisitor visitor, void* user);

// Resets a stats object to the empty state.
void reset(WorkloadStats& stats);

// Formats a human-readable summary of the aggregate workload.
std::string format_summary(const WorkloadStats& stats);

// Opcode byte -> short name (for debugging; returns "??" for unknowns).
const char* opcode_name(uint8_t op);

}  // namespace ogre::gbi
