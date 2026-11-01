# Session 210 handoff — the web renderer has a design document

## Goal

Developer request: document the design of the browser build's WebGL2 renderer,
and add a link to that document in the README.

## Result

New `docs/guides/web-renderer.md`. It has twelve sections:

1. scope: what the renderer is, its independence from RT64 and its HLE scope,
   and the three supported GBIs;
2. build and runtime integration: the CMake selection, `create_renderer`, the
   page-owned WebGL2 context, the C exports;
3. the frame pipeline: the gfx pthread records, the browser main thread executes
   GL, the `try_lock` queue exchange, the `sync_framebuffers` handshake;
4. the display-list front end: `identify_gbi` and its three hashes,
   `gbi::walk_dl`, and the command table;
5. RSP-side emulation: matrices, viewport, vertex transform, lighting, texture
   generation, fog alpha, eye-plane clipping, culling, S2DEX2 object draws;
6. RDP state and shading: `RenderState`, `DrawCmd`, the combiner and blender
   decodes, the one GLSL program, the sampler, alpha compare and coverage,
   depth;
7. textures and TMEM: the persistent 4 KiB TMEM and palette, the load commands,
   the decode formats including YUV16, the two caches, framebuffer-as-texture;
8. framebuffers, writeback and presentation: per-image FBOs, depth targets, the
   per-row writeback guard and dither, the VI present and canvas sizing;
9. the performance model and the measured frame-rate numbers;
10. diagnostics and test hooks: the `OGRE_*` switches, the debug-flag bits,
    `test_draw`, the log censuses;
11. limitations;
12. where to change what, with line anchors.

It is linked from `README.md` (the "Experimental: Web Version" section),
`docs/README.md` (the document index) and
`docs/guides/app-architecture.md`. `PLAN.md`'s browser-port section points to it,
and its "current state" reference is corrected from session 189 to session 209.

## Evidence

The document is derived from the code, not from the session prose. The line
anchors were chosen by reading the functions:

- `WebGLRenderer` is at `app/src/web_renderer.cpp:2567`; `send_dl` at `:5558`;
  `flush_commands` at `:4451`; `sync_framebuffers` at `:5744`.
- `identify_gbi` is at `:962`; the GBI hash table at `:946`; `gbi::walk_dl` at
  `app/src/gbi.cpp:63`; `exec_command` at `:7093`.
- The vertex path is `project_clip` at `:9065`; `draw_tri` at `:9357`;
  `draw_texrect` at `:9892`; `draw_obj_rect` at `:9749`.
- The texture path is `decode_tmem_tile` at `:1139` and `tmem_load_command` at
  `:8063`.
- The framebuffer path is `GlFramebuffer` at `:2466`, `ensure_framebuffer` at
  `:3350`, `readback_framebuffer` at `:3595`, `present_framebuffer` at `:4276`.
- The renderer is 10,639 lines at `0ff77e5`, from 1,595 lines at `37ca551`, over
  38 commits between 2026-08-29 and 2026-11-01.

No code changed in this session.

## Verification

The document has 59 markdown links. A Python check resolved every relative path
and every `#L<n>` fragment against the target file and its line count; 0 links
failed. The new doc was read once end to end after writing for the style rules in
`AGENTS.md`.

No source, CMake or page file changed, so `make web` was not run.
`git status --short` shows only the documentation files listed below and the
pre-existing dirty `tools/RT64` submodule.

## Files changed

- `docs/guides/web-renderer.md` (new)
- `README.md` (link)
- `docs/README.md` (index row)
- `docs/guides/app-architecture.md` (link)
- `PLAN.md` (pointer and the current-state reference)
- `docs/HANDOFF-2026-11-01-session210.md` (this file)

No probes were added, and none needed reverting.
