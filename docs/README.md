# Documentation

Technical documentation for the Ogre Battle 64 (USA, Rev A) PC port project.

| Document | Purpose |
|---|---|
| [DECISIONS.md](DECISIONS.md) | Living log of technical decisions and their rationale. |
| [HANDOFF-2026-08-24.md](HANDOFF-2026-08-24.md) | Session handoff: current state, the first-boot SIGBUS, and next steps. |
| [HANDOFF-2026-08-25.md](HANDOFF-2026-08-25.md) | Session 4 handoff: first RSP task + real VI mode achieved; drainer thread; next steps. |
| [HANDOFF-2026-08-25-session5.md](HANDOFF-2026-08-25-session5.md) | Session 5 handoff: RT64 renderer integrated; game stalls after its first RSP task; F3DEX 2.08 short-format GBI mismatch. |
| [HANDOFF-2026-08-25-session6.md](HANDOFF-2026-08-25-session6.md) | Session 6 handoff: the stall was a scheduler busy-spin deadlock; 3 fixes (spin, PI DMA, byte order) unblock boot into the main loop; Phase 4 (streamed overlays) starts. |
| [HANDOFF-2026-08-25-session7.md](HANDOFF-2026-08-25-session7.md) | Session 7 handoff: Phase 4 begins — overlays are plain linked code (no relocation); streamed overlays A+B+C recompiled and registered; boot passes the old crash and reaches the next overlay's data load, then spins on N64 threads 1+3. |
| [HANDOFF-2026-08-25-session8.md](HANDOFF-2026-08-25-session8.md) | Session 8 handoff: the post-boot spin was a cooperative-scheduler deadlock; N64Recomp now emits `yield_self` for poll loops; boot reaches the title-screen display-list build (next crash in `func_8019FC68`). |
| [HANDOFF-2026-08-25-session9.md](HANDOFF-2026-08-25-session9.md) | Session 9 handoff: the `func_8019FC68` crash was a KMC shared-epilogue recompilation bug; N64Recomp now emits fall-through tail calls (with a static-function fixpoint); boot reaches real RT64 rendering (next crash in the RT64/Vulkan render thread). |
| [LIBULTRA-BRIDGING.md](LIBULTRA-BRIDGING.md) | The libultra-bridging plan: findings, identification methodology, seed symbol table, staged steps. |
| [guides/app-build.md](guides/app-build.md) | How to build and run the PC port app. |
| [guides/app-architecture.md](guides/app-architecture.md) | App structure, runtime flow, callback responsibilities. |
| [guides/rsp-microcode.md](guides/rsp-microcode.md) | RSP microcode research and recompilation notes. |
| [guides/linux-migration.md](guides/linux-migration.md) | Moving the dev environment to Ubuntu: prerequisites and macOS-specific code to remove. |
| [PLAN.md](../PLAN.md) | The high-level project plan and roadmap. |

## Conventions

- ROM dumps / extracted assets are **never** committed to this repository.
- All third-party tools live under `tools/` as git submodules; the app project
  lives under `app/`.
- Local patches to upstream tools are kept as `*.patch` files in the repo root
  (`n64recomp-ob64.patch`, `rt64-plume-sdl.patch`) and applied to the submodule /
  vendored clone — never commit modifications *inside* a submodule (a submodule
  only records an upstream commit SHA, so such changes are invisible to this repo).
- Code is written against the `N64ModernRuntime` (ultramodern + librecomp)
  callback interfaces; platform I/O (window, input, audio) is isolated in the
  app so the game code never touches SDL directly.
- **Document discoveries and decisions as you go.** As a session progresses, record
  findings, root causes, and choices in `DECISIONS.md` (top entry) and write a
  self-contained handoff (`HANDOFF-YYYY-MM-DD-sessionN.md`) at session end so a fresh
  context can continue without re-deriving anything. Vendored (gitignored) code changes
  are not committed, so list them explicitly in the handoff's "Changes" section.
