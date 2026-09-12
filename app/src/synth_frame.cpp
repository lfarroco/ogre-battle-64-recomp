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

// Fill one rectangle of the framebuffer with a solid colour. Coordinates are in
// *pixels*; the RDP wants 10.2 fixed point, so this shifts by 2 before packing.
//
// NOTE: every RDP command here is exactly TWO words, so it must be a single
// `emit(w0, w1)` call. Emitting the two halves as separate emit() calls turns
// one command into two, and the second one is garbage. That bug made every
// G_FILLRECT here carry lrx=lry=0 (an empty rectangle) and made the
// G_SETSCISSOR a null scissor, so RT64 recorded the fills but never rasterized
// them - the display list "executed" but the framebuffer stayed black.
void emit_fill(int x0, int y0, int x1, int y1, uint32_t rgba) {
    // G_SETCIMG: fmt=RGBA(0), siz=16b(2), width-1, address.
    emit(0xFF000000u | (0u << 21) | (2u << 19) | (kScreenWidth - 1u),
         kFramebufferVram);
    // G_RDPSETOTHERMODE: RT64 keeps the 24-bit "mode0" verbatim in w0's low
    // bits and the 8-bit mode1 in w1, and tests fields at their full-word
    // positions (OtherMode::cycleType() is `H & (3 << G_MDSFT_CYCLETYPE)`, i.e.
    // bits 20-21 of H). So the cycle type must sit at bit 20 of w0, not be
    // pre-shifted right by 8.
    emit(0xEF000000u | (kFillOtherMode & 0xFFFFFFu), 0u);
    // G_SETFILLCOLOR: RGBA, alpha in the high byte.
    emit(0xF7000000u, rgba);
    // G_FILLRECT: RT64 decodes ulx/uly from w1 and lrx/lry from w0 (see
    // GBI_RDP::fillRect), which is also what libultra's gDPFillRectangle emits:
    //   w0 = opcode | lrx(12b)@12 | lry(12b)@0
    //   w1 = ulx(12b)@12 | uly(12b)@0
    // The values are 10.2 fixed point (pixel << 2).
    const uint32_t ulx = ((uint32_t)x0 << 2) & 0xFFFu;
    const uint32_t uly = ((uint32_t)y0 << 2) & 0xFFFu;
    const uint32_t lrx = ((uint32_t)x1 << 2) & 0xFFFu;
    const uint32_t lry = ((uint32_t)y1 << 2) & 0xFFFu;
    emit(0xF6000000u | (lrx << 12) | lry,
         (ulx << 12) | uly);
}

void build_display_list() {
    g_dl = reinterpret_cast<uint32_t*>(g_synth.rdram + (kDisplayListVram - 0x80000000u));

    // Full-screen scissor. RT64's RDP::drawRect only merges the rect into
    // drawColorRect when a scissor is set, so without this the fills are
    // recorded but never rasterized.
    //   w0 = opcode | ulx(12b)@12 | uly(12b)@0
    //   w1 = mode(2b)@24 | lrx(12b)@12 | lry(12b)@0
    // (one command: a single two-word emit) The rect is 10.2 fixed point.
    emit(0xED000000u | (0u << 12) | 0u,
         (0u << 24) | (((uint32_t)kScreenWidth << 2) << 12) | ((uint32_t)kScreenHeight << 2));

    // Seven vertical bars, so the frame is unmistakable in a screenshot and a
    // partial draw (only some bars) is visible too.
    //
    // The colour image is 16-bit (G_IM_SIZ_16b), so G_SETFILLCOLOR must carry an
    // RGBA16 (5/5/5/1) value, not RGBA8888: RT64's fill path does
    // `RGBA16::toRGBAF(call.callDesc.fillColor & 0xFFFF)` for a 16-bit target,
    // so a 0xFF0000FF-style value becomes 0x00FF (essentially black/transparent)
    // and the fill rasterizes as nothing.
    static const uint32_t kColors[] = {
        0x0000F801u,  // red
        0x000007C1u,  // green
        0x0000003Fu,  // blue
        0x0000FFFFu,  // white
        0x0000FFC1u,  // yellow
        0x0000F83Fu,  // magenta
        0x0000C618u,  // light grey
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
        // OGRE_SYNTH_ANIMATE=1 rotates the palette every VI. RT64's presenter is
        // change-driven (it only pushes a present when the VI or the RDRAM copy
        // of the VI framebuffer changes), so a *static* framebuffer is never
        // re-presented - the window keeps the last frame it was shown.
        static uint32_t raw_phase = 0;
        const bool animate = getenv("OGRE_SYNTH_ANIMATE") != nullptr;
        if (animate) {
            raw_phase++;
        }
        for (int y = 0; y < (int)kScreenHeight; y++) {
            for (int x = 0; x < (int)kScreenWidth; x++) {
                int bar = (x * 7) / (int)kScreenWidth;
                uint16_t c = raw_colors[(bar + raw_phase) % 7];
                // The runtime stores RDRAM words byte-reversed: pack the two
                // framebuffer bytes back-to-front.
                fb[y * kScreenWidth + x] = (uint16_t)((c >> 8) | (c << 8));
            }
        }
        // Read the bars back out of the same buffer RT64 reads, so "the raw
        // write did not land where the presenter looks" cannot be confused with
        // "the presenter ignores it".
        if ((g_synth.vi_counter % 120) == 0) {
            const uint8_t* p = g_synth.rdram + (kFramebufferVram - 0x80000000u);
            uint32_t first = (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
            uint32_t nonZero = 0;
            for (uint32_t b = 0; b < kScreenWidth * kScreenHeight * 2; b++) {
                if (p[b] != 0) nonZero++;
            }
            fprintf(stderr, "[synth] raw readback at rdram+0x%06X: first=0x%08X nonZero=%u\n",
                    kFramebufferVram - 0x80000000u, first, nonZero);
            fflush(stderr);
        }
    }
    // Pin the VI to the framebuffer the probe draws into. Without this the
    // probe is timing-dependent: on some boots the game has repointed the VI at
    // one of its own (black) framebuffers by the time the capture runs, and the
    // presented frame is the game's, not the probe's. OGRE_SYNTH_FORCE_VI=0
    // disables the pin.
    if (getenv("OGRE_SYNTH_FORCE_VI") == nullptr || getenv("OGRE_SYNTH_FORCE_VI")[0] != '0') {
        osViSwapBuffer(g_synth.rdram, (int32_t)kFramebufferVram);
    }

    // Re-submit on every Nth VI so the frame stays on screen for a capture.
    g_synth.vi_counter++;
    uint32_t period = 30;
    if (const char* p = getenv("OGRE_SYNTH_PERIOD")) {
        period = (uint32_t)strtoul(p, nullptr, 10);
    }
    if (period == 0 || (g_synth.vi_counter % period) == 0) {
        // OGRE_SYNTH_NO_DL=1 keeps the raw-framebuffer diagnostic but does not
        // submit a display list, so the RAM upload path can be measured without
        // the RDP also owning a render target at the same address.
        if (getenv("OGRE_SYNTH_NO_DL") == nullptr) {
            submit_task(rdram);
        }
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
