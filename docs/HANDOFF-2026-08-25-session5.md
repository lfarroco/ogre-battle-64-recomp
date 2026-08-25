# Handoff — 2026-08-25 (session 5): RT64 renderer integrated; game stalls after its first RSP task

## TL;DR

- **The null renderer is replaced by a real RT64 renderer** (`app/src/renderer.cpp`):
  RT64's `Application` (Vulkan on Linux) is wired behind ultramodern's
  `RendererContext`. It initializes successfully on the Intel HD 4400, presents
  frames at ~60 Hz, and the game boots cleanly through it.
- The game still stalls at the same point it did with the null renderer: it
  submits **one** RSP gfx task (a 3-command F3DEX "sync" DL), that task
  completes correctly end-to-end (SP + DP completion messages delivered, the
  game's task-done pump consumes the done message), and then the game waits for
  a **second** task that never comes. No streamed-code stub is ever reached.
- Two important discoveries this session:
  1. **The game's ucode is `RSP Gfx ucode F3DEX fifo 2.08`** (text at ROM
     `0x2F940`, data at ROM `0x3C540`, i.e. RDRAM `0x8009F540` /
     `0x800AC140`). Its display lists use the **F3DEX "short-format" opcode
     set** (`G_SETOTHERMODE_H=0xDE`, `G_SETOTHERMODE_L=0xDF`,
     `G_RDPFULLSYNC=0xE9`, ...).
  2. **RT64 matches this ucode to `GBIUCode::F3DEX2`** (hash DB), whose opcode
     semantics differ (F3DEX2 treats `0xDE` as `G_DL`, `0xDF` as `G_ENDDL`).
     The game's first DL is therefore misparsed (it "branches" into MIPS code
     at RDRAM `0xA9EF0`). RT64 has no GBI map for the F3DEX-2.08 short set.

## What changed this session (repo — committed)

- **`app/src/renderer.cpp`**: null renderer replaced by `RT64Renderer`
  (`RT64::Application` wrapper). Key wiring:
  - `RT64::Application::Core` with `RDRAM`, DMEM/IMEM slots, DPC registers,
    VI registers from `ultramodern::renderer::get_vi_regs()`,
    `checkInterrupts = dummy`.
  - `send_dl`: `state->rsp->reset()` → `interpreter->loadUCodeGBI(ucode,
    ucode_data, true)` → `app->processDisplayLists(rdram, data_ptr, 0, true)`.
  - `update_screen` → `app->updateScreen()`; `send_dummy_workload` fills a
    320×240 black rect (the runtime's dummy VI mode).
  - Config mappers (`set_application_user_config`, `to_rt64(...)`) adapted
    from N64Recomp/RecompFrontend's `rt64_render_context.cpp`.
  - Adapted from RecompFrontend; the render hooks (`SetRenderHooks`) are
    optional (null-checked in RT64) and are **not** wired.
- **`app/CMakeLists.txt`**: repeated RT64's `include_directories` (src,
  contrib/plume, contrib/imgui, contrib/hlslpp/include,
  contrib/mupen64plus-core/src/api, contrib/nativefiledialog-extended/src/include)
  onto the `ogrebattle64` target — RT64's own `include_directories` only apply
  inside its subdirectory.
- **`app/src/sdl_platform.cpp`**: `create_window` now adds `SDL_WINDOW_VULKAN`
  on Linux / `SDL_WINDOW_METAL` on macOS (required for the Vulkan surface;
  without it `SDL_Vulkan_CreateSurface` fails and RT64 segfaults during setup).
- **`app/src/rsp.cpp`**: added `log_task` — non-gfx tasks (the only ones that
  reach `get_rsp_microcode`) now log their full OSTask (ucode, ucode_data,
  data_ptr, sizes). Gfx tasks go straight to the renderer's `send_dl` and
  never reach this callback.

## What changed this session (vendored runtime — gitignored, re-apply on re-clone)

Temporary debug instrumentation in `tools/N64ModernRuntime` (remove when done):

## Boot state (verified this session, ~20-25 s window)

- Window + RT64/Vulkan init → `[renderer] RT64 renderer initialized (api=2)`.
- Game boots through the libultra bridge: controllers, audio request/response,
  7 game threads + drainer, heap init.
- The game submits **one** gfx task: `type=1 ucode=0x8009F540
  ucode_data=0x800AC140 data_ptr=0x800C6500 data_size=0x18` (3 GBI commands:
  `00 00 00 DE | F0 9E 0A 00`, `00 00 00 E9 | 00 00 00 00`,
  `00 00 00 DF | 00 00 00 00` — SETOTHERMODE_H / FULLSYNC / SETOTHERMODE_L).
- Task pipeline works end-to-end: `osSpTaskStartGo` → gfx thread → `sp_complete`
  (→ `0x800E8BBC` msg `0x29B`) + `send_dl` + `dp_complete` (→ `0x800E8BF4`
  msg `0x29C`) → `func_80089200` wakes on SP, sends the task-done message to
  the task's `done_mq` (`0x800E9BA8`) → `func_80089540` (the task-done pump)
  consumes it and dispatches.
- **The dispatch does nothing**: the callback slots `0x800B9E84/0x800B9E88/
  0x800B9E8C` are all **zero** (verified via RDRAM dump). `func_80089540`
  re-waits on `0x800E9BA8` for the next task's done message.
- The game thread (`func_80071EB0`) next steps are `func_8009DA50` (the
  **streamed-code ROM DMA** — the docs' session-4 watch point) followed by
  calls to the streamed functions `0x800E9CEC` / `0x800E9C20` (currently
  log-and-return stubs). It never reaches them in the observed window.

## Key technical findings

1. **RDRAM byte order**: rdram holds host-order 32-bit words. `do_rom_read`
   byte-swaps via `MEM_B` (`^3`), the recompiled `MEM_W` is a plain host deref.
   RT64's `processDisplayLists` reads `DisplayList` natively — consistent, no
   swapping needed (and the OSTask fields the runtime reads confirm host order).
2. **GBI identification**: `RT64::GBIManager::getGBIForUCode` hashes the ucode
   text/data against a database and matched OB64's "F3DEX 2.08" to
   `GBIUCode::F3DEX2` (7). This is **wrong for OB64's display lists**: F3DEX
   2.08 uses the short opcode set (0xDE/0xDF/0xE9...) while RT64's F3DEX2 map
   assigns those values to `G_DL`/`G_ENDDL`/… . When real DLs flow this will
   misparse them. The first (sync) DL is already misparsed: RT64 treats the
   first command as `G_DL` and "branches" to RDRAM `0xA9EF0`, which contains
   **MIPS code** (ROM `0x3F2F0`), not a DL.
3. **Thread map** (from a debug run): thread 1 = `func_8007F8E4` main (busy-
   spins on a function pointer at `0x800E7A18`, pri 0); 3 = `func_80071EB0`
   game (waits on `0x800C6490`); 16 = `func_80089358` (waits `0x800B9C40`);
   18 = `func_80089200` task submitter (waits `0x800E8B14` for requests,
   `0x800E8BBC` for SP); 19 = `func_80088F08` audio (waits `0x800E8B84`);
   4 = `func_8008AFE0` (waits `0x800C4C28`); 200 = drainer.
4. **VI state**: game sets `VI_STATUS=0x0000311E`, `VI_ORIGIN` alternates
   `0x00700280`/`0x00725A80`, `VI_WIDTH=0x140` (320). The screen is black
   (the sync DL draws nothing; RT64 presents the untouched framebuffers).
5. **Boot is flaky**: occasionally the game stalls/segfaults right after
   renderer init (before the game thread starts) instead of booting. Re-running
   usually succeeds. Suspect a race in runtime thread setup under heavy
   llvmpipe/software-rendering load during boot.

## How to reproduce

```sh
cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j
timeout -s KILL 25 stdbuf -oL -eL ./build-app/ogrebattle64 assets/ogre64.z64
```

Expected (trimmed): `[renderer] RT64 renderer initialized (api=2)`, boot logs,
`[sp] osSpTaskStartGo task=0x800E7DA0 type=1 ... done_mq=0x800E9BA8`, one
`send_dl`, `sp_complete`/`dp_complete`, no streamed stub, no crash for 25 s.

## Next session

1. **Find who is supposed to submit the game's second RSP task** (the request
   path into `func_80089200`'s queue `0x800E8B14`). The first task's request
   did not come through `osSendMesg` (nothing logged for `0x800E8B14`), and
   `func_80080F70` (the only osSendMesg→0x800E8B14 caller) has no callers in
   the recompiled funcs — investigate `osJamMesg`/direct queue writes and how
   the boot decides to issue the next task. Once the game proceeds it will hit
   `func_8009DA50` (streamed DMA) and then the streamed functions `0x800E9CEC`/
   `0x800E9C20` — **Phase 4 (overlay loading) starts there**.
2. **Fix the RT64 GBI match**: OB64's F3DEX 2.08 short-format opcodes are not
   covered by RT64's F3D, F3DEX, or F3DEX2 maps. Options: (a) add a GBI map for
   the short-format set, (b) override `loadUCodeGBI`/`getGBIForUCode` in the
   app to force the correct GBI for OB64's ucode addresses, or (c) check the
   RT64 hash DB entries for "F3DEX 2.08" — maybe a correct entry exists and
   just isn't being matched (the match landed on F3DEX2).
3. **Verify actual rendering**: once the game draws a real DL (title screen),
   confirm the framebuffer content on screen (capture with `xwd` +
   a small Python PIL decoder; the XWD file is a BE header + host-order 32-bit
   pixels at offset `len - w*h*4`).
4. **Audio**: no audio tasks (type 5) are submitted yet; the stub ucode is
   still fine. RSPRecomp is only needed for the audio ucode (gfx DLs are parsed
   by RT64's GBI interpreters, not by recompiled microcode).


- `librecomp/src/sp.cpp` `osSpTaskStartGo_recomp`: logs task address/type/ucode/
  ucode_data/data_ptr plus the game's done queue/message (raw reads past the
  runtime's 0x40-byte `OSTask_s`: `task+0x40` = msgq, `task+0x44` = msg).
- `ultramodern/src/events.cpp`: `osSetEventMesg` and `sp_complete`/`dp_complete`
  log the registered/delivered queues+mesg values.
- `ultramodern/src/mesgqueue.cpp`: `do_send` logs failures (full queue) and
  successes for the SP/task-done queues.
- `librecomp/src/ultra_translation.cpp`: `osSendMesg_recomp`/`osRecvMesg_recomp`
  log calls/returns for queues `0x800E9BA8`, `0x800E8B14`, `0x800C6490`,
  `0x800E8BBC`.
