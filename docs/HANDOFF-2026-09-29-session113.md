# Session 113 — HD backgrounds: a PNG pack replaces a dialogue/cutscene backdrop

Date: 2026-09-29. Follows session 112.

## Goal

Developer request: an "HD backgrounds mod" — a `mods/backgrounds/` directory of
`NN.png` files with mappings like `01 -> cathedral`, applied to the
dialogue/cutscene backdrops and to combat screens, starting with the cathedral
(scene `0x0D`, New Game step 2). Agreed with the developer before building:

* the first example swaps the game's own 320x240 backdrop pixels (a native-res
  reskin, with true HD deferred), and
* the image is matched to a background by **scene + step**.

## Result

`mods/backgrounds/` is a working pack. One PNG replaces the whole cathedral
backdrop: all four of the game's backdrop chunks come from the pack image, and
the game's own sprites and dialogue box draw over it. `mods/backgrounds/01.png`
is a generated placeholder (1280x960), `backgrounds.txt` holds the mapping, and
`make-example.py` regenerates the placeholder.

The feature is a **reskin at the game's resolution**: the PNG is box-filtered to
the game's 496x384 backdrop canvas and split across the four njpeg sub-images.
True HD needs the renderer to sample a larger texture, which is RT64 texture
replacement and is not implemented.

## 1. Where the backdrop comes from, and why a mod cannot hook it

The New Game cathedral backdrop is an `njpeg` asset decoded by the RSP and drawn
by S2DEX2 (`docs/guides/njpeg-backgrounds.md`). `func_ovlE_8019976C` (bank unit
E, ROM `0x066E30` -> RAM `0x80197B90`) copies the drawn framebuffer into one
pass destination per sub-image, and `func_ovlE_80199D30` merges the four into
the `'B5'` image the blit reads.

One pass, logged live at scene `0x0D` step 2:

| pass | destination | size |
|---|---|---|
| 0 | `0x801AAE90` | 320x240 |
| 1 | `0x801D06B0` | 176x240 |
| 2 | `0x801E50D0` | 320x144 |
| 3 | `0x801FB8F0` | 176x144 |

The pass's own state is `state = *(u32*)0x8019A680` (`lui 0x801A` /
`lw -0x5980`, read at `0x80199788`, `0x80199854`, `0x80199868` and elsewhere in
the function): `+0x74` is the destination buffer, `+0x78`/`+0x7A` the width and
height, and `+0x80` selects the `'B5'` path (pixels at `state[0x74]`) or the
single-image path (`memcpy` of an 8-byte header, pixels at `+8`).

**A `.nrm` code mod cannot hook this code.** Mod hooks resolve through
`get_vrom_to_section_map()`, which holds the base ELF's sections
(`.entry`/`.main`/`.streamedA/B/C`); a bank unit is not covered
(`docs/guides/app-build.md` -> "What a hook can reach"). The port therefore
calls in from the generated unit: `tools/hd_backgrounds.py` inserts
`ogre_hd_background(rdram)` in `func_ovlE_8019976C`, between the pass-counter
store at `0x80199950` and the common return label `L_80199954`. Every other
state-machine path jumps straight to `L_80199954`, so the call runs only on the
path that copied pixels, after the pixels are in their destination and before
the merge. `make bank-recomp` re-applies it after regeneration, next to
`tools/njpeg_readback.py`'s `ogre_sync_framebuffers()`.

## 2. The canvas, and the four-way split

The four sub-images are the chunks of the `'B5'` asset at ROM `0x7CADAC`. Its
8-byte file header is `42 35 03 04 01 15 00 EB`, then each chunk has a 16-byte
header and its `njpeg` data:

| chunk | `+0`/`+2` (i16) | `+4`/`+6` (u16) | `+8` size |
|---|---|---|---|
| 0 | -229, -235 | 320x240 | `0x6DC8` |
| 1 | 90, -235 | 176x240 | `0x3338` |
| 2 | -229, 4 | 320x144 | `0x42E0` |
| 3 | 90, 4 | 176x144 | `0x21C8` |

Taking `(-229,-235)` as the canvas origin, the chunks are a **496x384 canvas**:
a 320-wide left column, a 176-wide right column, a 240-tall top row and a
144-tall bottom row. `app/src/hd_backgrounds.cpp` box-filters the pack PNG to
496x384 and writes each pass its sub-rectangle: `(0,0)`, `(320,0)`, `(0,240)`
and `(320,240)`.

**Writing only pass 0 is visibly wrong**, and that was the first measurement:
the pack image appeared in the top-left region with the game's cathedral around
it. The four-way split is what makes one PNG cover the backdrop.

The on-screen geometry was measured with `OGRE_BG_DEBUG=1`, which paints each
pass a solid colour (320x240 red, 176x240 green, 320x144 blue, 176x144 yellow).
On a presented frame at the cathedral (`/tmp/dbg2.600.ppm`, 953x715):

```
R x[0,591]   y[72,317]     G x[593,949] y[72,317]
B x[0,591]   y[319,639]    Y x[593,949] y[319,639]
```

Those boundaries fit one affine map of the 496x384 canvas: offset
`(-57,-216)`, scale `(2.028, 2.229)`. The canvas is drawn zoomed and panned, so
the visible part is its bottom-right; the top-left of the top row is cropped.
The same transform applies to the game's own art, and the split is correct.

## 3. Keying

At the readback, the current scene is the `0x02` loader that preloads the `0x0D`
backdrop, not `0x0D`: in every observed pass `D_800C4C26` held a control value
(`0xFFFF`) and `D_8018F1C2` held `0x0002`. The scripted step `D_8018F1C0` was
`2` and is the value that identifies the image, so `backgrounds.txt`'s `scene`
is used only when it matches the current or pending scene and the step is the
fallback. The mapping row is `01 cathedral 0x0D 2`.

The image is written in the runtime's byte order as big-endian RGBA5551:
`MEM_H` is a native `u16` at `offset ^ 2` (`recomp.h`), the RDP colour image is
16-bit and the alpha bit is set.

## 4. What was run for verification

* `make bank-recomp` (regenerated all 34 units, then applied
  `tools/njpeg_readback.py` and `tools/hd_backgrounds.py`; `cross_bank.py
  check-banks` OK), then `cmake --build build-app`.
* `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 OGRE_BG_LOG=1
  OGRE_SCENE_LOG=1 ./build-app/ogrebattle64 assets/ogre64.z64` — four
  `[hdbg] applied ... chunk (x,y) WxH to 0x...` lines, one per pass, at scene
  `0x02` entering scene `0x0D`.
* A presented-frame capture of the cathedral
  (`OGRE_CAPTURE_PRESENT`/`OGRE_CAPTURE_EVERY`, `/tmp/cap2.1000.ppm`): the whole
  backdrop is the pack art, with the game's soldiers, statues and dialogue box
  drawn over it.
* The `OGRE_BG_DEBUG=1` colours above (`/tmp/dbg2.600.ppm`).
* The pack is a no-op without `mods/backgrounds/`, and `OGRE_BG_LOG=1` prints
  nothing then.

No temporary probe was added, so there is nothing to revert. `OGRE_BG_DEBUG`
and `OGRE_BG_LOG` are permanent env-gated diagnostics.

During the runs the installed `.nrm` files were moved from `build-app/mods/` to
`/tmp/ogre-mods-backup/` so an argument ROM boots without the start screen, and
they were restored afterwards.

## 5. Files

New:

* `mods/backgrounds/backgrounds.txt` — the mapping.
* `mods/backgrounds/01.png` — the generated plain placeholder.
* `mods/backgrounds/01-wide.png` — the generated widescreen placeholder.
* `mods/backgrounds/make-example.py` — writes both, in canvas space.
* `app/src/hd_backgrounds.hpp`, `app/src/hd_backgrounds.cpp` — pack load, PNG
  decode (stb_image with `STB_IMAGE_STATIC`), box filter, RGBA5551, the injected
  hook, and the debug/log modes.
* `tools/hd_backgrounds.py` — the generated-code insertion, with `--revert`.
* `tools/backgrounds.py` — the reference extractor (`extract`/`list`).
* `docs/guides/hd-backgrounds.md`.
* `.gitignore` — `mods/backgrounds/reference/` (extracted art).

Changed:

* `Makefile` — `bank-recomp` runs `tools/hd_backgrounds.py`.
* `app/CMakeLists.txt` — `src/hd_backgrounds.cpp` in both source lists and the
  RT64 contrib include directory for `stb/stb_image.h`.
* `app/src/main.cpp` — `ogre::hd_backgrounds_init(pref_dir)` after
  `load_settings`.
* `app/src/widescreen.hpp`, `app/src/widescreen.cpp` — the expand scene set
  (`expand_scenes`/`scene_expands`), the `OGRE_WS_SCENES` override, and
  `hd_backgrounds_covers_scene`.
* `Makefile` — `bg-extract`.
* `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`,
  `.gitignore`.

Generated (gitignored): `BankEFuncs/funcs_0.c` carries both code-generation
fixes.

## 6. Widescreen fill

With WIDESCREEN on, the window is 16:9 and scene `0x0D` is pillarboxed, because
the port's toggle only turns RT64's `Expand` on for the mission
(`app/src/widescreen.cpp`). A captured frame (`/tmp/ws-1400.png`, 1271x715)
shows the 4:3 cathedral image in the middle with black bars at the sides.

`scene_expands()` now also returns true for a scene the HD-background pack
covers (`hd_backgrounds_covers_scene`), so with the pack loaded and WIDESCREEN
on, scene `0x0D` expands: the log prints
`[widescreen] aspect ratio expand (scene 0x000D, mode ON)` and the backdrop fills
the whole width (`/tmp/ws3-last.png`). The wider view reveals more of the canvas
rather than stretching it: the stained glass spans about 66 % of a 4:3 frame and
47 % of the expanded frame, with the pillars and candle sconces visible at the
sides, and the dialogue text spans about 57 % of the 4:3 width against 43 % of
the 16:9 width, so the dialogue box, portrait and text keep their own size.

`OGRE_WS_SCENES=<hex>[,<hex>...]` replaces the default expand set (the mission
`0x03`) for a developer run; the pack always contributes its scenes. With
WIDESCREEN off the aspect stays `Original` and the image is unchanged. The
game's own vertical letterbox at the top and bottom of the dialogue screen is
part of the scene and is not filled.

**A `<id>-wide.png` beside a plain image is the widescreen variant**, authored at
16:9 and used when WIDESCREEN is on. Two measurements decided the mapping. A
1:1 calibration square in a `1920x1080` source, stretched to the 496x384 canvas,
displayed at 0.71 aspect (narrow) in the wide view, while a canvas-aspect source
displayed at 0.96: the canvas-to-screen draw is ~uniform, so stretching a 16:9
source to the canvas squeezes it. The port therefore maps the wide image
**preserving its aspect** (`to_canvas_fit` in `app/src/hd_backgrounds.cpp`):
scale to the canvas width, anchor to the canvas bottom, and repeat the first row
into the rows above (the draw shows the lower part of the canvas). A 16:9 source
becomes 496x279, and its round rose window measures round on screen. The plain
image keeps the stretch-to-canvas mapping. The selection reads
`widescreen_mode()` and not the live `ar_option`, because the readback runs in
the `0x02` loader where the aspect is still `Original`. `mods/backgrounds/01.png`
is the 4:3 arch (unchanged) and `01-wide.png` is a rose window between two
towers at `1920x1080`. The log names the variant:
`applied 'cathedral' (01 wide)` with WIDESCREEN on, `applied 'cathedral' (01)`
with it off.

**Running with and without the pack.** `hd_backgrounds_init` reads `OGRE_BG`:
`0`, `off`, `false` or `no` skips the pack for one run and logs
`[hdbg] pack disabled (OGRE_BG=0)`. `OGRE_BG_DIR`, when set, is now the *only*
folder searched instead of the first of four, so pointing it at an empty folder
(`OGRE_BG_DIR=/tmp/nobg`) also runs vanilla and logs
`[hdbg] no backgrounds.txt found (looked in 1 place(s))`; pointing it at another
folder selects that pack. All three cases were run and their `[hdbg]` lines
checked.

**Widescreen with the pack off.** The developer asked for the backdrop to expand
in widescreen even without custom art, which corrects session 102's note that the
large 2D backgrounds have no side data. A 4:3/wide capture pair with
`OGRE_BG=0 OGRE_WIDESCREEN=on` shows the wide view revealing statues and columns
at the edges that the 4:3 view crops (`/tmp/van-off-600.png` against
`/tmp/van-on-600.png`). `expand_scenes()` in `app/src/widescreen.cpp` therefore
defaults to the mission `0x03` **and the dialogue/cutscene scene `0x0D`**, so
`[widescreen] aspect ratio expand (scene 0x000D, mode ON)` fires with or without
the pack; a scene the pack covers is still added on top, and `OGRE_WS_SCENES`
still replaces the default set. `hd_backgrounds_covers_scene` no longer gates
`0x0D`.

## 7. Reference extraction

To draw the replacement, the artist needs the game's own backdrop. It is written
by the port: with `OGRE_BG_DUMP=<dir>` set, `ogre_hd_background` assembles the
four sub-images into the 496x384 canvas and writes `original-step<NNNN>.png` per
assembly, before the pack replaces anything, so the switch works with no pack
installed. `tools/backgrounds.py` drives a run from `backgrounds.txt` and files
the result:

```
backgrounds.py list
backgrounds.py extract 01
backgrounds.py extract 01 --step 6
backgrounds.py extract --all
backgrounds.py extract 01 --from /tmp/dump
```

`make bg-extract` (`BG_ID=01` by default) is the same command. The output is
`mods/backgrounds/reference/<id>-<name>-original.png`, and the directory is
gitignored because it is extracted art. A run of `backgrounds.py extract 01`
wrote `01-cathedral-original.png` (496x384, the throne-room backdrop), which is
the reference for the placeholder.

The dump is a live capture of the readback, not an offline ROM decode: the four
sub-images are Huffman-coded `njpeg` and only the game's RSP microcode decodes
them. One run through the opening writes the reference for every backdrop it
assembles, so a later background needs a mapping line and a run that reaches it.

## 8. Open

* **True HD is not implemented.** The pack image is downscaled to the game's
  canvas, so this is a reskin. A larger texture needs RT64 texture replacement,
  keyed by a texture hash recorded from a run; RT64 has the machinery
  (`TextureCache::loadReplacementDirectory`, `upscale2D = ScaledOnly`) but the
  port does not load a replacement directory.
* **The four-chunk layout is hardcoded** to the cathedral asset's sizes. A
  backdrop with a different layout is left alone and the log says
  `no canvas rect`.
* The dialogue box and portraits are not replaced.
* The pack is not part of `make dist`; it lives in the repository's `mods/` and
  is found through `<config>/../mods/backgrounds` when the app runs from
  `build-app/`.
