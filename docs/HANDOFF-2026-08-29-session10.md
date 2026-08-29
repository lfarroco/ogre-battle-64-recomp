# Handoff — 2026-08-29 (session 10): VI-thread crash fixed (runtime osViSetMode pointer validation); GBI question resolved (auto-detected F3DEX2 is correct — the session-9 "F3DEX override" was wrong); remaining crash is the machine-specific hasvk driver issue

## TL;DR

- **The flaky VI-thread segfault (`vi_thread_func` → `update_vi()` with a garbage
  `mode` pointer) is FIXED.** Root cause: OB64's libultra `osViSetMode` at
  `0x80095820` is really the VI **swap-context** routine (`__osViSwapContext`),
  which **ignores its argument** on real hardware. Its callers pass leftover
  register values — most notably `osCreateViManager`'s init helper
  (`func_8009AB50`, called at boot) calls it with **`$a0 = 0xA4400010`** (the VI
  STATUS MMIO address). The runtime bridge (`ultramodern/src/events.cpp`'s
  `osViSetMode`) translated that via `TO_PTR` to a host pointer **~4.5 GiB past
  the start of rdram** and stored it as the next VI mode; the VI thread's
  `update_vi()` dereferenced it → SIGSEGV. This crashed under **both** the
  Intel hasvk driver and Lavapipe, so it is a real port bug, not driver-specific.
  - Fix (vendored runtime): validate the mode pointer in `osViSetMode`
    (`rdram_vi_mode_ptr()` — only accept KSEG0 addresses, which map 1:1 into the
    512 MiB rdram buffer; otherwise keep the current mode). Also null-guarded
    `update_vi()` (falls back to `dummy_mode`) and `osViSetSpecialFeatures`.
- **The GBI question is RESOLVED: RT64's auto-detected GBI (`F3DEX2`, ucode enum
  7) is CORRECT for OB64 — the session-9 "force F3DEX" override was a mistake.**
  Dumped the game's real display lists: the first gfx task's DL is
  `DE000000 / 000A9EF0 / E9000000 / 0 / DF000000 / 0` =
  `G_DL(0xA9EF0); G_RDPFULLSYNC; G_ENDDL` under RT64's F3DEX2 map
  (`0xDE`=G_DL, `0xE9`=RDPFULLSYNC, `0xDF`=ENDDL); the branch target is an RDP
  color/rect setup (`0xFB..0xF7` setcolor*, `0xFC` setcombine, `0xF5` texrect);
  later boot DLs branch to real KSEG0 targets (`0x801869E8`). RT64's GBI DB
  already matches OB64's ucode hash to `F3DEX2`, so `loadUCodeGBI` does the right
  thing. The plain `F3DEX` GBI does **not** map `0xDE/0xDF` and misparses the
  DLs. `app/src/renderer.cpp` `send_dl` now carries a comment documenting this
  (no override code).
- **The remaining crash is the RT64 render-thread / `libvulkan_intel_hasvk.so`
  SIGSEGV in `submitRasterScene` — this is specific to this dev machine's Intel
  Haswell GPU (driver warns "Haswell Vulkan support is incomplete").** It occurs
  with both the correct and the wrong GBI, so it is not a DL-parsing issue. Under

## Verification (this session)

```sh
cmake --build build-app -j2
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
    timeout 40 ./build-app/ogrebattle64 assets/ogre64.z64 > /tmp/final_lvp.log 2>&1
```

Under Lavapipe: `[renderer] RT64 renderer initialized (api=2)`, 1864
`osSpTaskStartGo` calls, 1864 `sp_complete` events, **zero** SIGSEGV / "No
threads left to run" / `bad_optional_access`. (Boot is slowed ~1000x by the
bring-up `[sch]`/`[mq]`/`[ev]` debug prints in the vendored runtime — see leads.)

## Root cause details (the VI-thread crash)

- `symbol_addrs.txt` bridges `osViSetMode = 0x80095820` to the runtime's native
  `osViSetMode_recomp`. The game's function at `0x80095820` is actually the VI
  swap-context routine: it reads `__osViNext->mode`/`framep` from its own context
  (D_800ABBD4), writes all VI registers `0xA4400000..0xA4400034`, swaps
  `__osViCurr`/`__osViNext`, and **never uses its incoming `$a0`**.
- Its two callers in the game pass garbage: `func_8009AB50` (VI init, called from
  `osCreateViManager`) passes `0xA4400010` (line 0x8009AC3C in `asm/1060.s`);
  `func_800953C0` (viMgrMain) passes a leftover message-queue pointer. On real
  hardware this is harmless (argument unused).
- `TO_PTR(OSViMode, 0xA4400010)` = `rdram[(0xA4400010 - 0xFFFFFFFF80000000)]` =
  `rdram[0x124400010]` (~4.58 GB) — far outside the 512 MiB buffer. The runtime
  stored that host pointer in `ViState::mode`; `update_vi()` dereferenced it
  (`next_mode->comRegs`) → SIGSEGV. The race with `set_dummy_vi` made it flaky.

## GBI evidence (dump of the first gfx task DLs)

First task DL (`data_ptr=0x800C6500`, `data_size=0x18` — 6 words):
```
0000: DE000000 000A9EF0   ; F3DEX2: G_DL branch to 0xA9EF0 (segment 0)
0008: E9000000 00000000   ; G_RDPFULLSYNC
0010: DF000000 00000000   ; G_ENDDL
```
Branch target `0xA9EF0`: `FB/FA/F9/F8/F7` setcolor*, `EE` setprimdepth,
`EC/EB/EA` sync*, `FC` setcombine, `ED` rdploadsync, `F5` texrect — standard RDP
opcodes handled by `GBI_RDP::setup` (shared by every GBI).
A later boot DL (dump #1) shows `DE000000 801869E8` = G_DL to a real KSEG0
address, `E7000000` = G_RDPPIPESYNC, `E3000A01` = SETOTHERMODE_H — all consistent
with RT64's F3DEX2 map.

## The hasvk driver crash (machine-specific, for reference)

`RT64::FramebufferRenderer::submitRasterScene` →
`libvulkan_intel_hasvk.so` at `0x...c7b0`: a driver-internal vertex-data copy
loop, `movdqu 0x40(%rax,%rsi,1),%xmm0` with `rax=0`, `rsi=0xe300007ffff785c4`
(garbage source address). Stack: `submitRasterScene` → `recordFramebuffer` →
`State::fullSync` → `GBI_RDP::fullSync` → `processDisplayLists` →
`Application::processDisplayLists` → `gfx_thread_func`. Mesa 23.2.1-1ubuntu3.1.
Lavapipe handles the same scene fine, so this is not a general bug. Other flaky
symptoms seen on this machine (all downstream of the gfx thread failing):
`std::bad_optional_access` after renderer init, and the runtime abort
`No threads left to run!` (game threads starve because `dp_complete`/VI events
stop when the gfx thread dies).

## Changes (repo)

- `n64modernruntime-ob64.patch` — NEW. `git -C tools/N64ModernRuntime diff`
  snapshot of the vendored runtime, including this session's VI fix and the
  earlier bring-up debug logging / scheduler work. Apply on re-clone with
  `git -C tools/N64ModernRuntime apply ../../n64modernruntime-ob64.patch`.
  (Note: the patch also records the runtime's internal `N64Recomp` submodule
  pointer change — harmless, but keep the runtime's own submodules initialized.)
- `app/src/renderer.cpp` — comment documenting the F3DEX2 auto-detection finding;
  no functional change (the session-9 "F3DEX override" string is gone).
- `docs/HANDOFF-2026-08-29-session10.md` — this file.
- `docs/DECISIONS.md`, `PLAN.md` — status updates.

## Changes (vendored, gitignored — re-apply on re-clone)

N64ModernRuntime (`tools/N64ModernRuntime`, vendored + gitignored):
- `ultramodern/src/events.cpp` — `rdram_vi_mode_ptr()` + `osViSetMode` validation
  + null-guards in `update_vi()`/`osViSetSpecialFeatures()` + `dummy_vi_mode()`.
  (This is THE fix from this session.)
- All other vendored changes (debug logging, scheduler/drainer work) remain.

## How to reproduce / validate

```sh
# apply the vendored runtime changes if re-cloned
git -C tools/N64ModernRuntime apply ../../n64modernruntime-ob64.patch
cmake --build build-app -j2

# software rasterizer (works on any machine)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
    timeout 40 stdbuf -oL -eL ./build-app/ogrebattle64 assets/ogre64.z64

# expected: boot through overlay loads, RSP gfx tasks (type=1, ucode 0x8009F540)
# submitted and completed, RT64 renders frames, no VI-thread crash.
```

## Leads for the next session

1. **Test on a machine with a supported GPU** (any non-Haswell Vulkan device, or
   Lavapipe in a pinch). Expect the `submitRasterScene` hasvk crash to disappear;
   validate actual title-screen rendering.
2. **Gate or remove the runtime debug logging** (`[sch]`/`[mq]`/`[ev]`/`[pi]`
   printf's in `ultramodern/src/*` and `librecomp/src/*`). They slow the game
   ~1000x, which changes timing (and could mask real races). Wrap them in an
   env-var/compile flag once bring-up is stable, then re-time the boot.
3. **`bad_optional_access` after renderer init** was seen (flaky) on this machine
   under hasvk. If it reproduces on a good GPU, investigate RT64's render-worker
   init — it may be the long-standing "flaky renderer-init" race.
4. **The two "falls through to unknown" recompilation notices** remain
   (`func_80093060` → `0x8009337C`, `func_8009953C` → `0x800996C4`) — not
   crashing, but worth checking symbol boundaries in `asm/1060.s` if anything
   points there later.
5. **Next boot milestone**: get past the title screen's first real frame on a
   working GPU, then audit visual correctness (the F3DEX2 GBI should now be
   parsing DLs correctly; verify geometry/textures, not just "no crash").

  **Lavapipe** (software rasterizer) the port runs **cleanly**: 1864 RSP gfx
  tasks submitted and completed, no crashes, renderer live. The user confirmed
  the dev machine can be swapped; the hasvk issue is not a general Linux problem.
