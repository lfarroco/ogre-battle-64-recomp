# Session 179 handoff — the QUEST still's background was a dropped PRIM-only rectangle

## Goal

The developer reported that the QUEST publisher logo in the browser build is
broken: three 3D "q" shapes should appear and then resolve into the QUEST
wordmark (`docs/proofs/native-quest-screen.png` is the reference). The fault is
in the web renderer. Timebox: one round.

## Result

The 640x480 plate and the wordmark now draw. The three q meshes still do not,
and the lead is named in section 4.

### 1. The scene's draw structure, from native RT64

- Forced entry `?OGRE_SCENE=publishers` runs scene `0x0A`. Native capture
  (`OGRE_CAPTURE_TARGET`, 640x480 targets) shows a white plate, three coloured
  tube-"q" meshes and the black QUEST wordmark with the ® mark.
- A live native RDRAM dump at t=17.5 s (`OGRE_LIVE_CONSOLE=1`,
  `OGRE_CONSOLE_FILE=...`, `OGRE_CONSOLE_AT_MS=17500`, command
  `dump /tmp/w179/d-17500.bin`) reads the three publisher colour images:
  `0x1BA780` and `0x250800` both hold the finished logo (luminance mean 242.4),
  `0x2E6840` holds the plate (225.6).
- Native `OGRE_DL_DECODE=all` (40 MB log) decodes the final list
  (`dl=475 ptr=0x803ACBB0`, 74885 walked bytes):
  1. `SETZIMG/SETCIMG 0x2E6840`, `SETFILLCOLOR 0xFFFCFFFC`, 16 `G_FILLRECT`
     bands that clear the plate white;
  2. `SETCIMG <target>`, `SETFILLCOLOR 0x00010001`, 16 `G_FILLRECT` bands that
     clear the frame buffer black;
  3. `G_DL 0x801B84B8` (PRIM `0xFAFAFAFF`, `SETCOMBINE FCFFFFFF FFFDF6FB`) then
     `G_DL 0x80186630`: 16 `G_TEXRECT` bands tiling the 640x480 target. Their
     combiner evaluates to PRIM only, and the tile is the preamble's
     `line=0` / `size 0` tile;
  4. the CI8 wordmark: `SETTIMG 0x8000C408` (CI8, width 144), `LOADTLUT`, two
     `G_TEXRECT` halves, then a third for the ® (`0x8000D408`);
  5. the q meshes: `G_DL 0x801B85B8` sets `GEOMETRYMODE clear=0xFFFFFF
     set=0x00200405` and then `set=0x00020000` (G_LIGHTING only), one ambient
     and one directional light, `SETCOMBINE FC121824 FF33FFFF` (TEXEL0*SHADE,
     alpha TEXEL0.a*SHADE.a) and `OTHERMODE_L 0x00552078`; the geometry comes
     from `G_MOVEWORD segment[14] = 0x00026C00` and `G_DL 0x0E000000`;
  6. `SETPRIMCOLOR 0x000000FF` then `G_DL 0x80186630` again: the same 16 bands
     with PRIM black, the fade to black before the title.
- The q textures are 32x32 RGBA32, one solid colour each, all with alpha 0:
  `0x80001400` = (47,162,11), `0x80002400` = (40,92,193), `0x80000400` =
  (222,218,39). Every texel of each is that colour.

### 2. The defect: a rectangle with no texture was dropped

`draw_texrect` decoded the tile and then required a texture for every rectangle:

```c
ensure_tile_image(ctx, st, st.active_tile, 0);
ensure_tile_image(ctx, st, st.active_tile + 1, 1);
if (!texture_available(st)) { st.active_tile = saved_tile; return; }
```

The 16 white background bands draw through the preamble's tile 0, which has
`line == 0` and a zero rect. `ensure_tile_image` returns without an image for
that tile, `texture_available` is false, and all 16 bands were dropped. The frame
kept the black clear from step 2 and only the wordmark appeared, which is the
developer's screenshot: a black 640x480 canvas with the wordmark inside its own
white box.

RT64 has no such test. `RDP::drawTexRect` drops a rectangle only when the tile
is a non-zero tile with a zero window (`lrs == uls` or `lrt == ult`, guarded by
`line != 0`), and the sampler is never read when the evaluated mux does not name
TEXEL0/TEXEL1. The PRIM-only mux draws the primitive colour without a texture.

### 3. The fix

`app/src/web_renderer.cpp`:

- New `combiner_uses_texture(cb, othermode_h, index)` mirrors RT64
  `ColorCombiner::usesTexture(otherMode, textureIndex, hardwareBugCheck =
  false)` (`tools/RT64/src/shared/rt64_color_combiner.h:270-340`): copy mode
  reads TEXEL0; one-cycle mode evaluates the second mux and treats TEXEL1
  references as TEXEL0; two-cycle mode checks both cycles, with the
  TEXEL0/TEXEL1 roles swapped on the second cycle. Colour and alpha selectors
  both count.
- `draw_texrect` now drops a rectangle only when a decoded image is missing
  **and** the evaluated mux reads a texel. Every other rectangle is drawn, with
  `record_state(..., texture_available(st))` marking it untextured.

Result (browser, `?OGRE_SCENE=publishers`): the plate is white and the wordmark
and ® draw. `docs/proofs/web-publisher-quest-white-background.png` is that
frame. The q meshes are still absent.

### 4. Still open: the three q meshes

The q draws reach the queue. The final list queues 807 draws, and the flush of
that list contains 756 q triangles. A temporary probe (reverted, tag
`probe179`) read them as: geometry mode `0x00020000` (G_LIGHTING), texture
`0x1400` fmt RGBA siz 32b line 8 window 32x32, decoded 32x32 mean 120,
`mask=(5,5)`, combiner `FC121824 FF33FFFF` (TEXEL0*SHADE), `oml=0x00552078`,
NDC inside the frame (e.g. `(0.111,0.365) (0.126,0.438) (0.152,0.440)`). They
are not visible on the canvas.

The run summary of the final flush (temporary `probe179` logging) reads:

```
Fct/2E6840/white x16 | Fct/<target>/black x16 | Tct/<target>/white x16 |
Tct/<target>/white x3 | Tct/<target>/black a x16 | tri/<target>/black a x756
```

The list order is bands -> wordmark -> q's -> fade. The queued order puts the
fade bands before the q triangles, and the q triangles carry the fade's PRIM
(black) rather than the plate's (0.98). The likely fault is therefore the web
walk order for the segmented (`MOVEWORD segment[14]`, `G_DL 0x0E000000`) q
sub-lists, not the q draw's own state. The next experiment is to log the task
number and the walked command offset beside each queued draw and compare that
order with `OGRE_DL_DECODE=<dl>` for the same frame; do not patch the q draws
before that.

Two properties make this worth doing carefully. The q textures are uniform, so
texture coordinates and G_TEXTURE_GEN cannot change the q colours (G_TEXTURE_GEN
is used by the animation frames, `set=0x00040000`, and is still unimplemented in
the browser, but these final q draws do not set it). The q alpha is 0 and the
combiner's alpha is TEXEL0.a*SHADE.a, so any change that turns alpha blending on
for these draws makes the meshes disappear; the web's blender decode currently
reads `alpha_blend = false` for `oml=0x00552078`, which matches RT64's
one-cycle `Blender::run` path (`lastCycle && !forceBlend` writes the combiner
colour).

## Verification

- Browser, forced `publishers` scene, GPU Chromium (`--enable-gpu
  --use-angle=metal`): the white plate and wordmark draw
  (`docs/proofs/web-publisher-quest-white-background.png`, meanRGB 245.6,
  `colorful=336`).
- Natural boot (`?OGRE_SCENE_LOG=1`, 20 s): the intro renders (soldiers, cube,
  N64 logo), so the rectangle change does not regress it.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`:
  `fbtex hits=1 lines=1`, exit 0.
- `EM_CACHE=<repo>/build-wasm/emcache make web` clean. The default cache path
  (`~/.cache/emscripten-ogre`) is outside the workspace and the sandbox denies
  the emscripten lock file there, so the build was run with a copy of that cache
  under `build-wasm/emcache` (`build-wasm/` is gitignored).

Not measured: the q meshes (still missing); the ATLUS and Nintendo stills'
pixel match after the rectangle change; other scenes that draw a PRIM-only
rectangle with an unconfigured tile (none was compared).

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `combiner_uses_texture` added; `draw_texrect` requires a texture only when the combiner reads a texel |
| `docs/proofs/web-publisher-quest-white-background.png` | the browser frame after the fix |
| `PLAN.md` | status paragraph and open-work item 19 |
| `docs/DECISIONS.md` | decision row |
| `docs/STATUS-LOG.md` | session entry |
| `docs/HANDOFF-2026-10-19-session179.md` | this file |
| `build-wasm/` | rebuilt by `make web` (gitignored) |

## Probes used and reverted

All diagnostic code was tagged `probe179` and is gone
(`grep -rn "probe179" app/ debug/` is empty):

- a per-draw log of the lit triangle draws (`GFX-QUEST`, `GFX-QUEST-NDC`);
- a per-rectangle log of the full-screen texrects (`GFX-BIGRECT`) placed before
  the tile guards;
- a per-flush run-length summary of the 700+ draw lists (`GFX-BIGFLUSH`);
- the counters those probes used.

No recompiled code or RT64 file was touched. `tools/RT64` was already modified
before this session (the submodule's patch state).

## Instrument notes

- The forced `publishers` scene enters at different times across runs (the poke
  races the boot's first scene). The Quest still is reached 13-16 s after
  `[scene] forced scene 0x0A is active`, so a capture waits for that line and
  then samples.
- Headless Chromium without `--enable-gpu --use-angle=metal` advances the boot
  at about 1 display list/s and never reaches the still in a useful window.
- `OGRE_DL_DECODE=all` is the cheapest way to read the game's own draw order for
  this scene; the app decoder prints `G_TEXRECT` for opcode `0xE4`, which is
  this game's F3DEX2 TEXRECT (the geometric corners are in `0xE4`'s two words,
  the S/T in `0xE1`, the steps in `0xF1`).
