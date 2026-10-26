# Session 198 handoff — the per-draw uniform lookup, and the tutorial field's flat grass

## Goal

Fix the two defects the developer reported on the tutorial practice stage
(Jangurri): the grass is solid green where native is textured, and the frame rate
drops by about half against native.

## Result

1. **The frame-rate drop was the browser's per-draw uniform lookup, and it is
   fixed and measured.** The flush loop resolved every uniform with
   `glGetUniformLocation(program_, name)` on every draw (about 60 lookups per
   draw). A CDP CPU profile of the mission screen spent 27% of the main thread in
   `_emscripten_glGetUniformLocation` and its UTF-8 conversion
   (`UTF8ArrayToString`, `findStringEnd`, `webglGetLeftBracePos`), and another
   12% in the once-per-flush `glGetError`, which synchronises with the GPU.

   `WebGLRenderer` now keeps `std::unordered_map<std::string_view, GLint>
   uniform_cache_`; `uloc(name)` resolves a name once after the program links and
   the flush loop uses it everywhere. The cache is cleared when the program is
   relinked. The per-flush `glGetError` guard is now `OGRE_GL_CHECK=1`
   (default off) and keeps the session-181 diagnostic available.

   Measured on the Castle Burgund mission, same save and route, same 8 s window:
   **16.78 fps before, 26.10 fps after** on one run and **28.94 fps** on a
   re-run; the tutorial practice field measures about 20 fps. The remaining cost
   is the draw count itself (about 1 600 `glDrawArrays` per frame), which RT64
   avoids by batching; that is not part of this change.

2. **The grass is reproduced and bounded, and it is not fixed.** In the browser,
   the practice field draws its grass as a smooth green surface over the whole
   battlefield; native RT64's same field is a fine mottled texture. Evidence from
   the current build:

   - Toggling `ogre_gfx_debug_flags(16)` (output TEXEL0 raw) turns the grass area
     **pure white** while the cliffs, roads and trees keep their textures. White
     is the 1x1 fallback for a draw that samples no tile.
   - No visible large triangle at scene `0x03` fails `texture_available`, and no
     rectangle is dropped by `draw_texrect`'s
     `!texture_available && needs_texture` guard. `OGRE_FOG=0`, backface culling
     disabled and the F3D depth clip disabled each change no pixel.
   - The one large triangle pair the probe logged at the field (a `64x64` CI4
     tile at `0x24F4A0`, TEXEL0 passthrough, UV 0..157) covers the **top-left UI
     panel**, not the grass. Its decoded image is a parchment noise texture, as
     is the `0x24F4A0` source, so that decode is correct.
   - The field's second family is small `32x32` RGBA16 triangles with
     `comb=FC151604/2F1125FF` = `(TEXEL0 - TEXEL1) * PRIM_A + TEXEL1`,
     `PRIM_A=0.5`; they are textured (`avail=1`) but the visible ones logged so
     far lie outside the camera.
   - The field has about 161 000 rectangles per run; the probe filtered
     rectangles to 40x40 and saw only the tutorial banner, so the grass layer is
     either a set of smaller rectangles or the per-frame clear. `TEXEL0` white
     says the grass pixels are covered by draws that sample the 1x1 fallback. A
     further probe of rectangles down to 8x8 logged only the UI's 16x16 text
     strips (`y=205`), so the grass layer is still unlocated; the next filter
     must exclude the UI rows and log every rectangle covering the grass.

## Evidence

- **Mission fps, one build and route:** `debug/probes`-style probe
  `/tmp/w198/mission.cjs`: `scene 0x0003 at 14.7 s`, then
  `frames 948 -> 1181 in 8.1 s = 28.94 fps`; the pre-change build measured
  `frames 684 -> 819 in 8.0 s = 16.78 fps` on the same screen.
- **CPU profile before the change:** `/tmp/w198c/run.log` (CDP `Profiler`,
  4 s): `_emscripten_glGetUniformLocation` 676 ms, `UTF8ArrayToString` 249 ms,
  `findStringEnd` 142 ms, `webglGetLeftBracePos` 50 ms, `getError` 495 ms over a
  4.1 s profile.
- **Tutorial field:** the route is the real title menu
  (`start`, `down`, `start`) then A through Deneb's dialogue; the practice stage
  is scene `0x0003` (the `0x17 -> 0x02 -> 0x0D -> 0x03` sequence). Browser frame
  `/tmp/w198u/field-1.png`; TEXEL0 raw
  `/tmp/w198u/field-1-texel0.png`; native reference
  `docs/proofs/native-tutorial-practice-field.png`.
- **Combiner decodes:** the field's two families are
  `(TEXEL0 - TEXEL1) * PRIM_A + TEXEL1` then `COMBINED * SHADE`, and the base is
  `TEXEL0` (both cycles), read through the same decoder `[GFX-CMD]` uses.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean after the change and
  after the probe revert.
- The mission route boots and presents; the frame is
  `docs/proofs/web-mission-fps-session198.png`.
- The natural boot reaches the title with no page errors.
- `git status --short` shows `app/src/web_renderer.cpp`, the proof and the docs,
  plus the pre-existing `m tools/RT64`.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `uniform_cache_` + `uloc()`; every per-draw `glGetUniformLocation` replaced; the cache cleared at program link; `OGRE_GL_CHECK` gates the per-flush `glGetError` |
| `docs/proofs/web-mission-fps-session198.png` | the mission frame on the fixed build |
| `docs/HANDOFF-2026-10-26-session198.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/guides/app-build.md` | status, log, decision, index and the `OGRE_GL_CHECK` knob |

## Probes used and reverted

`probe198` in `app/src/web_renderer.cpp` (the triangle, rectangle and
rectangle-drop logs, and debug bits 23/24/25 for culling, post-blend noise and
the depth clip) is removed; `grep -rn "probe198\|PROBE198" app/ debug/` is
empty. No generated code and no `tools/RT64` file was edited.

## Open

1. **The tutorial field's grass.** The grass pixels are covered by draws that
   sample the 1x1 fallback (TEXEL0 raw is white), and the only large logged
   triangle there is the UI panel. Log the small (under 40x40) textured
   rectangles at the field with their tile state and decoded image size; the
   mission's Burgund field samples a textured grass layer on the same code path,
   so the difference is in this map's tile setup or its decode.
2. **The remaining frame cost is the draw count.** About 1 600 draw calls per
   frame on the mission and tutorial field; RT64 batches triangles into one
   buffer. A state-batched triangle list (one VBO, one draw per pipeline state)
   is the next step after the uniform cache.
