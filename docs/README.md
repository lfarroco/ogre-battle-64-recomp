# Documentation

Technical documentation for the Ogre Battle 64 (USA, Rev A) PC port project.

| Document | Purpose |
|---|---|
| [DECISIONS.md](DECISIONS.md) | Living log of technical decisions and their rationale. |
| [HANDOFF-2026-08-24.md](HANDOFF-2026-08-24.md) | Session handoff: current state, the first-boot SIGBUS, and next steps. |
| [HANDOFF-2026-08-25.md](HANDOFF-2026-08-25.md) | Session 4 handoff: first RSP task + real VI mode achieved; drainer thread; next steps. |
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
- Code is written against the `N64ModernRuntime` (ultramodern + librecomp)
  callback interfaces; platform I/O (window, input, audio) is isolated in the
  app so the game code never touches SDL directly.
