#include <cstdio>

#include "librecomp/rsp.hpp"

namespace ogre {

// Stub RSP microcode: reports that the task "completed" (RspExitReason::Broke)
// without doing any work. This keeps the game's task pipeline flowing during
// bring-up. Replaced by RSPRecomp-generated microcode funcs (docs/guides/
// rsp-microcode.md).
static RspExitReason stub_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    return RspExitReason::Broke;
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
            printf("[rsp] task type %u submitted (stub microcode)\n", static_cast<unsigned>(task->t.type));
            return stub_ucode;
        },
    };
}

}  // namespace ogre
