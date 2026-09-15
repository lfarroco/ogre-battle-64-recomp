# Documentation

Technical documentation for the Ogre Battle 64 (USA, Rev A) PC port project.

| Document | Purpose |
|---|---|
| [../AGENTS.md](../AGENTS.md) | **Read this first if you are an AI agent**: working rules for this repo — ask the developer what the current scene should display rather than inferring intent, treat handoffs as hypotheses, verify the consumer before naming a struct, isolate one variable per experiment, revert probes by regenerating. |
| [DECISIONS.md](DECISIONS.md) | Living log of technical decisions. **Read the "Durable decisions" table at the top first** — it is the ~13 decisions that still bind the port; the entries below are session reasoning (superseded ones carry a banner). |
| [scenes.md](scenes.md) | **What each screen is supposed to show**, sourced from the developer (the AGENTS §1 oracle), with the port's status per screen and the New Game opening's five parts. |
| [symbols.md](symbols.md) | Proposed symbol names: address → name → evidence → confidence, plus the naming convention and what a rename costs. Read before naming a function or calling one "the X function". |
| [HANDOFF-2026-08-24.md](HANDOFF-2026-08-24.md) | Session handoff: current state, the first-boot SIGBUS, and next steps. |
| [HANDOFF-2026-08-25.md](HANDOFF-2026-08-25.md) | Session 4 handoff: first RSP task + real VI mode achieved; drainer thread; next steps. |
| [HANDOFF-2026-08-25-session5.md](HANDOFF-2026-08-25-session5.md) | Session 5 handoff: RT64 renderer integrated; game stalls after its first RSP task; F3DEX 2.08 short-format GBI mismatch. |
| [HANDOFF-2026-08-25-session6.md](HANDOFF-2026-08-25-session6.md) | Session 6 handoff: the stall was a scheduler busy-spin deadlock; 3 fixes (spin, PI DMA, byte order) unblock boot into the main loop; Phase 4 (streamed overlays) starts. |
| [HANDOFF-2026-08-25-session7.md](HANDOFF-2026-08-25-session7.md) | Session 7 handoff: Phase 4 begins — overlays are plain linked code (no relocation); streamed overlays A+B+C recompiled and registered; boot passes the old crash and reaches the next overlay's data load, then spins on N64 threads 1+3. |
| [HANDOFF-2026-08-25-session8.md](HANDOFF-2026-08-25-session8.md) | Session 8 handoff: the post-boot spin was a cooperative-scheduler deadlock; N64Recomp now emits `yield_self` for poll loops; boot reaches the title-screen display-list build (next crash in `func_8019FC68`). |
| [HANDOFF-2026-08-25-session9.md](HANDOFF-2026-08-25-session9.md) | Session 9 handoff: the `func_8019FC68` crash was a KMC shared-epilogue recompilation bug; N64Recomp now emits fall-through tail calls (with a static-function fixpoint); boot reaches real RT64 rendering (next crash in the RT64/Vulkan render thread). |
| [HANDOFF-2026-08-29-session10.md](HANDOFF-2026-08-29-session10.md) | Session 10 handoff: VI-thread crash fixed; GBI question resolved (F3DEX2 auto-detection is correct); stable under Lavapipe. |
| [HANDOFF-2026-08-29-session11.md](HANDOFF-2026-08-29-session11.md) | Session 11 handoff: WebAssembly port kickoff — RT64 optional, null renderer, web shell; native macOS boot fixed (stale ELF/funcs, missing runtime fixes, osSpGetStatus bridge, broken debug probe). |
| [HANDOFF-2026-09-15-session44.md](HANDOFF-2026-09-15-session44.md) | Session 44 handoff (latest): the New Game **step table** is decoded (steps 1..19 → asset id, ROM, command; every step ≥ 2 selects selector 2); step 2's crash was two stores to guest `0`/`8` (the `0x80239874` shared tail's `*(sp+0x1EC)`, and the descriptor interpreter's emitter with `a3 = 0`), and every value feeding them comes from game code — so retail must tolerate them. **Fix: the N64's low-window (KUSEG) RDRAM alias restored in `recomp_mem_addr`** (`a < 0x80000000` → `a & 0x003FFFFF`, recorded in `n64modernruntime-n64recomp.patch`) — the step-2 enter now completes; the next wall is RT64-only (`do_sendP + 0xC4`, guest `0xFE6E2C89`). `tools/ogrelz.py` gained `--asset` and the rule that an asset's LZ block starts at `rom+4`. |
| [HANDOFF-2026-09-15-session43.md](HANDOFF-2026-09-15-session43.md) | Session 43 handoff: bank unit B compiles records 17/18 and scene `0x17` renders as the **Tutorial** (the title menu is captured: New Game / Tutorial / Stereo); the **first `0x0D` visit is the New Game movie and already renders** (sepia courtyard cutscene, "I promise I'll make you proud."), so the wall is **step 2** — the cathedral dialogue with `Archbishop Odiron` the developer identified; the movie-path word is `0x80197794`, not `0x8019F794`; dialogue text is LZ-compressed (`tools/ogrelz.py`). |
| [HANDOFF-2026-09-14-session35.md](HANDOFF-2026-09-14-session35.md) | Session 35 handoff: the title screen's white band — six `G_TEXRECT`s drawn with a zero-texel tile, and the guard that skips them; the `OGRE_NOP_RECT` bisect tool; the RT64 patch regenerated. |
| [HANDOFF-2026-09-12-session34.md](HANDOFF-2026-09-12-session34.md) | Session 34 handoff: the intro's missing smoke and 3D logo characters (a stale linker script meant overlay C was never loaded) and the narrowed bank-swap eviction; cross-bank `jal` routing built but blocked. |
| [HANDOFF-2026-09-12-session33.md](HANDOFF-2026-09-12-session33.md) | Session 33 handoff: the streamed-overlay segment table and bank descriptors decoded; banks are recompiled as a second unit and swapped on the game's DMA; the 170s run completes. |
| [HANDOFF-2026-09-12-session32.md](HANDOFF-2026-09-12-session32.md) | Session 32 handoff: the "Licensed by Nintendo"/ATLUS/QUEST stills render full-frame (a mis-named `osViSetMode` kept the game's 640x480 VI mode from the runtime); the ~95s crash is confirmed to be a streamed-overlay **bank swap**. Sessions 12–31 are in `docs/` (session 31: the intro renders — 205 display lists, ~28fps). |
| [HANDOFF-2026-09-12-session25.md](HANDOFF-2026-09-12-session25.md) | Session 25 handoff: native runs drive themselves (`OGRE_TAP_MS`/`OGRE_EXIT_AFTER_MS`); the idle trajectory reproduced on native and localised to the RSP/frame worker queues. |
| [WEB-PORT.md](WEB-PORT.md) | The WebAssembly / browser port plan (null renderer first, renderer decision later). |
| [WEB-PORT-REPORT.md](WEB-PORT-REPORT.md) | Web port implementation report (audit, RT64 deps, Emscripten issues, file changes). |
| [WEB-PORT-DEPLOYMENT.md](WEB-PORT-DEPLOYMENT.md) | Browser deployment requirements (COOP/COEP headers, local server). |
| [LIBULTRA-BRIDGING.md](LIBULTRA-BRIDGING.md) | The libultra-bridging plan: findings, identification methodology, seed symbol table, staged steps. |
| [guides/app-build.md](guides/app-build.md) | How to build and run the PC port app. |
| [guides/app-architecture.md](guides/app-architecture.md) | App structure, runtime flow, callback responsibilities. |
| [guides/rsp-microcode.md](guides/rsp-microcode.md) | RSP microcode research and recompilation notes. |
| [guides/linux-migration.md](guides/linux-migration.md) | Moving the dev environment to Ubuntu: prerequisites and macOS-specific code to remove. |
| [guides/web-probes.md](guides/web-probes.md) | Headless-browser probes (`debug/`): serving the wasm build, running the probes, the page contract. |
| [PLAN.md](../PLAN.md) | The high-level project plan and roadmap. |

## Conventions

- **If you are an AI agent, read [`../AGENTS.md`](../AGENTS.md) first.** It holds
  the working rules this project learned the hard way; the most important one is
  to ask the developer what a scene should display instead of inferring intent
  from the code.
- ROM dumps / extracted assets are **never** committed to this repository.
- All third-party tools live under `tools/` as git submodules; the app project
  lives under `app/`.
- Our own debugging/verification tooling lives under `debug/` (the headless
  browser probes); it is not part of the app build. See
  [guides/web-probes.md](guides/web-probes.md).
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
