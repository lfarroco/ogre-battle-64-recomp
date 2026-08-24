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

recomp::rsp::callbacks_t make_rsp_callbacks() {
    return {
        .get_rsp_microcode = [](const OSTask* task) -> RspUcodeFunc* {
            printf("[rsp] task type %u submitted (stub microcode)\n", static_cast<unsigned>(task->t.type));
            return stub_ucode;
        },
    };
}

}  // namespace ogre
