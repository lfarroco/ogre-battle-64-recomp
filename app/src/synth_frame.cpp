// Synthetic frame probe (session 26).
//
// Purpose: prove, end to end, that a display list *does* render and present in
// the native RT64 build while the game's boot is wedged. A stalled boot only
// ever submits its blanking display list, so the canvas is black and there is
// no way to tell "the renderer never got a real list" apart from "the renderer
// dropped it".
//
// With OGRE_SYNTH_FRAME=1 the probe builds a small self-contained F3DEX2
// display list in RDRAM (colour bars through the RDP fill path, so no texture
// data or TMEM is involved) and submits it through the runtime's normal
// graphics-task path on the next VI. `OGRE_SYNTH_AT_MS` delays the first
// submission so the game's own boot frame is presented first.
//
// This is a diagnostic, not a renderer feature: it never runs unless the
// environment variable is set, and it draws into the framebuffer the game
// itself uses, so it is presented through the same swap chain.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#include "recomp.h"
#include "synth_frame.hpp"

namespace ogre {

namespace {

// Parked in the 8 MiB RDRAM region RDRAM's first 8 MiB, in the gap between the
// recompiled code/data (ends ~0x800C6xxx in the low 1 MiB) and the 16 MiB
// boundary where the runtime's recomp heap starts. The task header lives
// 0x8000 bytes above the display list.
//
// IMPORTANT: RT64 receives `data_ptr & 0x3FFFFFF` (a physical RDRAM offset), so
// the list must live below 0x84000000 for that mask to be lossless.
constexpr uint32_t kDisplayListVram = 0x80500000u;
constexpr uint32_t kFramebufferVram = 0x80700000u;   // the address the boot DL uses
constexpr uint32_t kTaskVram = kDisplayListVram + 0x8000u;
constexpr uint32_t kScreenWidth = 320;
constexpr uint32_t kScreenHeight = 240;
// Same other-mode word as RT64Renderer::send_dummy_workload (renderer.cpp).
constexpr uint32_t kFillOtherMode = 0x382C30u;

struct SynthState {
    bool enabled = false;
    bool ready = false;
    uint32_t first_at_ms = 0;
    uint32_t submissions = 0;
    uint32_t vi_counter = 0;
    uint32_t data_size = 0;
    uint8_t* rdram = nullptr;
};

SynthState g_synth;
uint32_t* g_dl = nullptr;

void emit(uint32_t w0, uint32_t w1) {
    *g_dl++ = w0;
    *g_dl++ = w1;
}

// Fill one rectangle of the framebuffer with a solid colour. Rect coordinates
// are 10.2 fixed point; lrx/lry are exclusive.
void emit_fill(int x0, int y0, int x1, int y1, uint32_t rgba) {
    // G_SETCIMG: fmt=RGBA(0), siz=16b(2), width-1, address.
    emit(0xFF000000u | (0u << 21) | (2u << 19) | (kScreenWidth - 1u),
         kFramebufferVram);
    // G_RDPSETOTHERMODE: high 24 bits in w0's low half, low 8 bits in w1.
    emit(0xEF000000u | (kFillOtherMode >> 8), kFillOtherMode & 0xFFu);
    // G_SETFILLCOLOR: RGBA, alpha in the high byte.
    emit(0xF7000000u, rgba);
    // G_FILLRECT: ulx(12b)@12|uly(12b)@0 in w0, lrx(12b)@12|lry(12b)@0 in w1.
    emit(0xF6000000u | (((uint32_t)x0 & 0xFFFu) << 12) | ((uint32_t)y0 & 0xFFFu),
         0u);
    emit((((uint32_t)x1 & 0xFFFu) << 12) | ((uint32_t)y1 & 0xFFFu), 0u);
}

void build_display_list() {
    g_dl = reinterpret_cast<uint32_t*>(g_synth.rdram + (kDisplayListVram - 0x80000000u));

    // Full-screen scissor. RT64's RDP::drawRect only merges the rect into
    // drawColorRect when a scissor is set, so without this the fills are
    // recorded but never rasterized.
    //   w0 = opcode | ulx(12b)@12 | uly(12b)@0
    //   w1 = mode(2b)@24 | lrx(12b)@12 | lry(12b)@0
    emit(0xED000000u, ((uint32_t)0 << 12) | 0u);
    emit(0u, ((uint32_t)kScreenWidth << 12) | (uint32_t)kScreenHeight);

    // Seven vertical bars, so the frame is unmistakable in a screenshot and a
    // partial draw (only some bars) is visible too.
    static const uint32_t kColors[] = {
        0xFF0000FFu,  // opaque red
        0xFF00FF00u,  // opaque green
        0xFFFF0000u,  // opaque blue
        0xFFFFFFFFu,  // opaque white
        0xFF00FFFFu,  // opaque yellow
        0xFFFF00FFu,  // opaque magenta
        0xFFC0C0C0u,  // opaque light grey
    };
    const int bars = (int)(sizeof(kColors) / sizeof(kColors[0]));
    const int bar_width = (int)kScreenWidth / bars;
    for (int i = 0; i < bars; i++) {
        int x0 = i * bar_width;
        int x1 = (i == bars - 1) ? (int)kScreenWidth : (x0 + bar_width);
        emit_fill(x0, 0, x1, (int)kScreenHeight, kColors[i]);
    }

    // G_RDPFULLSYNC (the game's own lists end this way) then G_ENDDL.
    emit(0xE9000000u, 0);
    emit(0xDF000000u, 0);

    g_synth.data_size = (uint32_t)((uint8_t*)g_dl -
                                   (g_synth.rdram + (kDisplayListVram - 0x80000000u)));

}

// Called from the VI thread once per vblank; that thread owns the action queue,
// so enqueueing a graphics task from here is safe (the game's own task
// submission goes through the same queue).
void submit_task(uint8_t* rdram) {
    // The task only carries type/ucode/data; RT64's interpreter takes the DL.
    uint8_t* task_mem = g_synth.rdram + (kTaskVram - 0x80000000u);
    OSTask* task = reinterpret_cast<OSTask*>(task_mem);
    std::memset(task, 0, sizeof(OSTask));
    task->t.type = M_GFXTASK;
    // Reuse the game's own gfx microcode. RT64 identifies the GBI from this
    // ucode, and the boot display list proves the value is right for OB64.
    task->t.ucode = 0x8009F540u;
    task->t.ucode_data = 0x800AC140u;
    // RT64 reads the list at `RDRAM + (data_ptr & 0x3FFFFFF)`, so the task
    // field must hold a physical offset, not the KSEG0 virtual address.
    task->t.data_ptr = (uint64_t)(kDisplayListVram & 0x3FFFFFFu);
    task->t.data_size = g_synth.data_size;

    printf("[synth] submitting synthetic display list #%u (dl=0x%08X size=%u) at T=%llu\n",
           g_synth.submissions + 1, kDisplayListVram, g_synth.data_size,
           (unsigned long long)ultramodern::trace_millis());
    fflush(stdout);

    // NOTE: the runtime's PTR() values are int32_t and TO_PTR sign-extends them,
    // so the pointer must be a KSEG0 address (0x8000_0000+) like the game's own
    // pointers. A bare physical offset (0x0D00_0000) is positive and sign-extends
    // to 0xFFFF_FFFF_0D00_0000, which lands outside RDRAM (bus error).
    ultramodern::submit_rsp_task(rdram, (int32_t)kTaskVram);
    g_synth.submissions++;
}

}  // namespace

// Called from the VI thread's per-vblank callback (sdl_platform.cpp registers
// it). First call captures RDRAM and builds the list; later calls wait for the
// configured delay and then submit exactly once.
void synth_frame_vi_tick(uint8_t* rdram) {
    if (!g_synth.enabled) {
        return;
    }
    if (!g_synth.ready) {
        g_synth.rdram = rdram;
        build_display_list();
        g_synth.ready = true;
        printf("[synth] enabled: %u bytes of display list at 0x%08X, first submit at %u ms\n",
               g_synth.data_size, kDisplayListVram, g_synth.first_at_ms);
        fflush(stdout);
    }
    if (ultramodern::trace_millis() < g_synth.first_at_ms) {
        return;
    }
    // Raw-framebuffer diagnostic: write bars straight into RDRAM (no display
    // list) to separate "the display list does not draw" from "the framebuffer
    // is never presented".
    if (getenv("OGRE_SYNTH_RAW") != nullptr) {
        uint16_t* fb = reinterpret_cast<uint16_t*>(g_synth.rdram + (kFramebufferVram - 0x80000000u));
        static const uint16_t raw_colors[] = {0xF801, 0x07C1, 0x003F, 0xFFFF, 0xFFC1, 0xF83F, 0xC618};
        for (int y = 0; y < (int)kScreenHeight; y++) {
            for (int x = 0; x < (int)kScreenWidth; x++) {
                int bar = (x * 7) / (int)kScreenWidth;
                // The runtime stores RDRAM words byte-reversed: pack the two
                // framebuffer bytes back-to-front.
                uint16_t c = raw_colors[bar];
                fb[y * kScreenWidth + x] = (uint16_t)((c >> 8) | (c << 8));
            }
        }
    }
    // Re-submit on every Nth VI so the frame stays on screen for a capture.
    g_synth.vi_counter++;
    uint32_t period = 30;
    if (const char* p = getenv("OGRE_SYNTH_PERIOD")) {
        period = (uint32_t)strtoul(p, nullptr, 10);
    }
    if (period == 0 || (g_synth.vi_counter % period) == 0) {
        submit_task(rdram);
    }
}

bool synth_frame_enabled() {
    const char* enable = getenv("OGRE_SYNTH_FRAME");
    if (enable == nullptr || enable[0] == '0') {
        return false;
    }
    g_synth.enabled = true;
    const char* at = getenv("OGRE_SYNTH_AT_MS");
    g_synth.first_at_ms = at != nullptr ? (uint32_t)strtoul(at, nullptr, 10) : 2000u;
    return true;
}

}  // namespace ogre
