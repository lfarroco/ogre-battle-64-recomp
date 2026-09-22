#include <cstdio>
#include <cstdlib>

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

// The game's Nintendo-JPEG decoder (M_NJPEGTASK, type 4), recompiled from the
// ROM by `make rsp-recomp` (RspFuncs/njpeg_ucode.cpp; it is a global symbol).
extern RspExitReason njpeg_ucode(uint8_t* rdram, uint32_t ucode_addr);

// The game's audio microcode (M_AUDTASK, type 2), recompiled from the ROM by
// `make rsp-recomp` (RspFuncs/audio_ucode.cpp). See the block comment at
// `kAudioUcodeAddr` below for the load path.
extern RspExitReason audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

namespace ogre {

// Wrapper around the recompiled audio microcode. The game's own ucode has one
// property the port cannot execute faithfully: its SETLOOP command (opcode
// 0x0F) stores a loop address as a word at DMEM 0x0E, which is *also* dispatch
// table slots 7 and 8 (the table lives at DMEM 0; entries 7/8 are 0x0000 in the
// game's `ucode_data`, i.e. this driver has no SEGMENT/SETBUFF and sacrifices
// those slots to the loop variable). If the game ever dispatches opcode 7 or 8,
// the "handler address" is the high/low half of that loop address — an
// essentially arbitrary 16-bit value, and not necessarily an instruction
// boundary. Session 75 saw exactly that once in ~45 runs: `Unhandled jump target
// 0x1226` (= the low half of a loop address), which killed the process because
// `run_task`'s failure exits.
//
// A recompiler cannot jump to a non-instruction target, so the port has no
// faithful behaviour to offer there. Rather than take the process down, log it
// loudly and report the task as completed: the game's audio driver retries next
// frame, so the cost is one silent buffer. This is deliberately scoped to the
// audio ucode — njpeg keeps the runtime's loud failure (docs/HANDOFF-2026-09-18-session75.md §6).
static RspExitReason audio_ucode_guard(uint8_t* rdram, uint32_t ucode_addr) {
    RspExitReason reason = audio_ucode(rdram, ucode_addr);
    if (reason != RspExitReason::Broke) {
        fprintf(stderr,
                "[rsp] audio microcode bailed (reason %d, likely the SETLOOP/dispatch-table "
                "alias); dropping this task so the game keeps running\n",
                static_cast<int>(reason));
        return RspExitReason::Broke;
    }
    return reason;
}

// Stub RSP microcode: reports that the task "completed" (RspExitReason::Broke)
// without doing any work. It is the fallback for every microcode the port has
// not recompiled yet, and for `OGRE_NJPEG=0` / `OGRE_AUDIO_UCODE=0`.
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

// OB64's audio microcode (M_AUDTASK, type 2). The game submits every type-2 task
// with `task->t.ucode = 0x8009E050`, `task->t.ucode_data = 0x800ABDA0`, and runs
// it through the *standard libultra RSP boot code* at `task->t.ucode_boot =
// 0x8009ECB0`: that loader DMAs `ucode_data` (0x800 bytes) to DMEM 0 and the text
// (a fixed 0xF80 bytes) from `ucode` to IMEM 0x1080, then `jr 0x1080`. The
// recompiled function therefore starts at IMEM 0x1080 (config/rsp-audio.toml,
// `text_address = 0x1080`), and the runtime's own task load (DMEM 0xFC0 for the
// task, DMEM 0 for `ucode_data`) reproduces the rest.
//
// Session 74 misread 0x8009E050 as a second boot block and compiled it at IMEM
// 0x1000, which put every internal branch target 0x80 bytes off: the driver ran
// its command-list DMA helper at the wrong entry, read the dispatch table for
// the wrong opcode, and never produced PCM. With 0x1080 the command loop
// dispatches the real audio ABI and the buffers carry samples (session 75).
//
// The game keeps DMEM 0x000-0x07FF as `ucode_data` (the driver state and the
// dispatch table live there, not in a game-visible structure), so the microcode
// has no RDRAM output of its own: the game hands it command lists through
// `data_ptr` and reads the mixed PCM back from buffers named by the list.
//
// `OGRE_AUDIO_UCODE=0` forces the old stub (an A/B escape hatch); anything else,
// including unset, runs the recompiled microcode.
constexpr uint32_t kAudioUcodeAddr = 0x8009E050u;

static bool njpeg_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("OGRE_NJPEG");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// `OGRE_AUDIO_UCODE=0` forces the stub; anything else (including unset) runs the
// recompiled audio microcode.
static bool audio_ucode_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("OGRE_AUDIO_UCODE");
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
            if ((uint32_t)task->t.ucode == kAudioUcodeAddr) {
                if (audio_ucode_enabled()) {
                    printf("[rsp] task type %u submitted (audio microcode)\n", static_cast<unsigned>(task->t.type));
                    return audio_ucode_guard;
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
