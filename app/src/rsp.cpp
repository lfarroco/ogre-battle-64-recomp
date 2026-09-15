#include <cstdio>
#include <cstdlib>

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

// The game's Nintendo-JPEG decoder (M_NJPEGTASK, type 4), recompiled from the
// ROM by `make rsp-recomp` (RspFuncs/njpeg_ucode.cpp; it is a global symbol).
extern RspExitReason njpeg_ucode(uint8_t* rdram, uint32_t ucode_addr);

namespace ogre {

// Stub RSP microcode: reports that the task "completed" (RspExitReason::Broke)
// without doing any work. This keeps the game's task pipeline flowing during
// bring-up. Replaced by RSPRecomp-generated microcode funcs (docs/guides/
// rsp-microcode.md).
static RspExitReason stub_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    return RspExitReason::Broke;
}

// OB64 submits the Nintendo-JPEG decoder for the full-screen background images
// of the New Game opening sequence (scene 0x0D steps >= 2); with the stub the
// decoded image stays zero and the cathedral renders over black. It is
// identified by its RDRAM address: main segment 0x8009ED80, size 0x7C0 (boot
// loader at 0x8009ECB0, Huffman/quantization tables at 0x800AC050).
//
// Status (session 46): the recompiled microcode runs, but it does not yet
// reproduce the game's decode. Measured with a probe in the generated file:
// task 0 loops its 0x12C iterations reading 0x300 bytes of input each time
// (r28 advances) while writing every result back to the *same* address (r29
// stays at data_ptr-0x300), and tasks 1..3 leave the loop before its head, so
// all four collapse to one 0x300-byte block. With the decoder enabled the
// game's decode state machine (D_8019A680 + 0x7e) then never advances, the
// scene stops submitting display lists, and the null renderer dies in the
// runtime's `do_recv` (osRecvMesg with a null-derived queue 0x80000010,
// msgCount 0). Until that is resolved the stub stays the default and the
// decoder is opt-in via OGRE_NJPEG=1.
// See docs/HANDOFF-2026-09-15-session46.md.
constexpr uint32_t kNjpegUcodeAddr = 0x8009ED80u;

static bool njpeg_enabled() {
    static const bool enabled = getenv("OGRE_NJPEG") != nullptr;
    return enabled;
}

static void log_task(const OSTask* task, const char* path) {
    printf("[rsp] task type=%u ucode=0x%08X ucode_size=0x%X ucode_data=0x%08X "
           "ucode_data_size=0x%X data_ptr=0x%08X data_size=0x%X flags=0x%X (%s)\n",
           static_cast<unsigned>(task->t.type), static_cast<unsigned>(task->t.ucode),
           static_cast<unsigned>(task->t.ucode_size), static_cast<unsigned>(task->t.ucode_data),
           static_cast<unsigned>(task->t.ucode_data_size), static_cast<unsigned>(task->t.data_ptr),
           static_cast<unsigned>(task->t.data_size), static_cast<unsigned>(task->t.flags), path);
}

recomp::rsp::callbacks_t make_rsp_callbacks() {
    return {
        .get_rsp_microcode = [](const OSTask* task) -> RspUcodeFunc* {
            log_task(task, "sp_task_queue/microcode path");
            if ((uint32_t)task->t.ucode == kNjpegUcodeAddr) {
                if (njpeg_enabled()) {
                    printf("[rsp] task type %u submitted (njpeg microcode)\n", static_cast<unsigned>(task->t.type));
                    return njpeg_ucode;
                }
                printf("[rsp] task type %u submitted (njpeg microcode NOT enabled; stub)\n",
                       static_cast<unsigned>(task->t.type));
                return stub_ucode;
            }
            printf("[rsp] task type %u submitted (stub microcode)\n", static_cast<unsigned>(task->t.type));
            return stub_ucode;
        },
    };
}

}  // namespace ogre
