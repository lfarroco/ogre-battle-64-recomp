#include <cstdio>
#include <cstdlib>

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

// The game's Nintendo-JPEG decoder (M_NJPEGTASK, type 4), recompiled from the
// ROM by `make rsp-recomp` (RspFuncs/njpeg_ucode.cpp; it is a global symbol).
extern RspExitReason njpeg_ucode(uint8_t* rdram, uint32_t ucode_addr);

namespace ogre {

// Stub RSP microcode: reports that the task "completed" (RspExitReason::Broke)
// without doing any work. It is the fallback for every microcode the port has
// not recompiled yet (notably the audio ucode).
static RspExitReason stub_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    return RspExitReason::Broke;
}

// OB64 submits the Nintendo-JPEG decoder for the full-screen background images
// of the New Game opening sequence (scene 0x0D steps >= 2). It is identified by
// its RDRAM address: main segment 0x8009ED80, size 0x7C0 (boot loader at
// 0x8009ECB0, Huffman/quantization tables at 0x800AC050). The game's CPU
// Huffman decode (func_8008B250) has already produced the `mbs * 0x300`-byte
// entropy-decoded buffer at `data_ptr`; the microcode reverse-zigzags,
// dequantizes (scale = `yield_data_size`, -2 here) and inverse-DCTs it in place
// into the 16-bit YUV macroblock layout that the game then draws.
//
// The decoder is correct since session 47: RSPRecomp's `text_address` label base
// was 0x0D80 (the RDRAM address's low 13 bits) instead of 0x1080 (the RSP IMEM
// DMA address the game's boot loader loads the text at), which rotated every `j`
// target by 0x300 and made the recompiled decoder write a single 0x300-byte
// block per task.
//
// `OGRE_NJPEG=0` forces the stub (an A/B escape hatch); anything else, including
// unset, runs the decoder, which costs 0-1 ms per image.
// See docs/HANDOFF-2026-09-15-session47.md and docs/guides/rsp-microcode.md.
constexpr uint32_t kNjpegUcodeAddr = 0x8009ED80u;

static bool njpeg_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("OGRE_NJPEG");
        return value == nullptr || value[0] != '0';
    }();
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
                printf("[rsp] task type %u submitted (njpeg microcode disabled; stub)\n",
                       static_cast<unsigned>(task->t.type));
                return stub_ucode;
            }
            printf("[rsp] task type %u submitted (stub microcode)\n", static_cast<unsigned>(task->t.type));
            return stub_ucode;
        },
    };
}

}  // namespace ogre
