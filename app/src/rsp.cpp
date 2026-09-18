#include <cstdio>
#include <cstdlib>

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

// The game's Nintendo-JPEG decoder (M_NJPEGTASK, type 4), recompiled from the
// ROM by `make rsp-recomp` (RspFuncs/njpeg_ucode.cpp; it is a global symbol).
extern RspExitReason njpeg_ucode(uint8_t* rdram, uint32_t ucode_addr);

// The game's audio microcode (M_AUDTASK, type 2), recompiled from the ROM by
// `make rsp-recomp` (RspFuncs/audio_ucode.cpp). Every type-2 task the game
// submits uses ucode pointer 0x8009E050, which is the RSP boot block; the text
// it loads lives at IMEM 0x1120 (rsp-audio.toml).
extern RspExitReason audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

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

// OB64's audio microcode (session 74). The game submits every type-2 task with
// this ucode pointer. The block at that address is the microcode's own boot
// block: it reads the OSTask at DMEM 0xFC0, DMAs segments into IMEM and calls
// the text entry at IMEM 0x1120, so the recompiled function starts at the boot
// block (rsp-audio.toml, text_address 0x1000) and the whole image is compiled
// in place.
//
// **Off by default.** The recompiled audio microcode runs without crashing and
// without unhandled jumps, but it does not yet produce PCM: its command walk
// reads a table at DMEM 0 for every entry and never DMAs the game's audio list
// (session 74 §4 has the trace and the two RSPRecomp bugs that were fixed on the
// way). The stub leaves the game's audio path exactly as it has always been
// (silent, buffers still queued and draining), so runs stay healthy until the
// microcode's expected DMEM/boot state is reproduced. Set `OGRE_AUDIO_UCODE=1`
// to run the recompiled microcode instead.
constexpr uint32_t kAudioUcodeAddr = 0x8009E050u;

static bool njpeg_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("OGRE_NJPEG");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// `OGRE_AUDIO_UCODE=1` runs the recompiled audio microcode; anything else
// (including unset) keeps the stub.
static bool audio_ucode_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("OGRE_AUDIO_UCODE");
        return value != nullptr && value[0] == '1';
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
            if ((uint32_t)task->t.ucode == kAudioUcodeAddr) {
                if (audio_ucode_enabled()) {
                    printf("[rsp] task type %u submitted (audio microcode)\n", static_cast<unsigned>(task->t.type));
                    return audio_ucode;
                }
                printf("[rsp] task type %u submitted (audio microcode disabled; stub)\n",
                       static_cast<unsigned>(task->t.type));
                return stub_ucode;
            }
            printf("[rsp] task type %u submitted (stub microcode)\n", static_cast<unsigned>(task->t.type));
            return stub_ucode;
        },
    };
}

}  // namespace ogre
