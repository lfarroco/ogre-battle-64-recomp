# HD backgrounds (`mods/backgrounds/`)

A pack of PNGs that replaces the game's dialogue and cutscene backdrops. The
game draws those backdrops from full-screen `njpeg` images
(`docs/guides/njpeg-backgrounds.md`); this feature writes the pack's image over
the pixels the game copies out of the decoder, before the game composes and
blits them.

The first worked example is the New Game cathedral (Archbishop Odiron), scene
`0x0D` step 2.

## The pack

```
mods/backgrounds/
  backgrounds.txt     the mapping
  01.png              the image for one background
  make-example.py     the script that wrote 01.png (a placeholder)
```

`backgrounds.txt` is one line per background, whitespace separated:

```
<id>   <name>        <scene>  <step>
01     cathedral     0x0D     2
```

* `id` names the image file in the same folder: `<id>.png`, or `<name>.png` if
  that is missing.
* `name` is a label for the log.
* `scene` is the scene id that draws the backdrop (`docs/scenes.md`).
* `step` is the scripted step number in `D_8018F1C0`.

`#` starts a comment; blank lines are ignored. An image whose file is missing is
reported and skipped, and a pack with no loadable entry is a no-op.

The port looks for a pack in this order and uses the first folder that contains
a `backgrounds.txt`:

1. `$OGRE_BG_DIR` — when set, this is the **only** folder searched
2. `<config>/mods/backgrounds` (`<config>` is the executable's folder by
   default; see `docs/guides/app-build.md` → "Config directory")
3. `<config>/../mods/backgrounds` — this is the repository's own `mods/` when
   the app runs from `build-app/`
4. `<working directory>/mods/backgrounds`

### Running with and without the pack

The pack is loaded automatically when a `backgrounds.txt` is found. To compare a
run against the vanilla art:

```sh
# with the pack (default, when mods/backgrounds/ exists)
./build-app/ogrebattle64 assets/ogre64.z64

# without it, for one run
OGRE_BG=0 ./build-app/ogrebattle64 assets/ogre64.z64          # 0, off, false or no

# without it, by pointing the pack folder somewhere empty
OGRE_BG_DIR=/tmp/no-backgrounds ./build-app/ogrebattle64 assets/ogre64.z64

# a different pack folder than the default search
OGRE_BG_DIR=/path/to/another-pack ./build-app/ogrebattle64 assets/ogre64.z64
```

`OGRE_BG=0` logs `[hdbg] pack disabled (OGRE_BG=0)`; an empty `OGRE_BG_DIR` logs
`[hdbg] no backgrounds.txt found (looked in 1 place(s))`. With the pack off, the
game's own `njpeg` backdrop is drawn unchanged and `OGRE_BG_LOG` prints no
`[hdbg] pass` lines. WIDESCREEN is unaffected: scene `0x0D` is in the Expand set
whether or not the pack is installed, and the game's own 496x384 backdrop has
content that the 4:3 view crops, so the wide view reveals it.

## Extracting the original (the reference to draw over)

`tools/backgrounds.py` produces the game's own backdrop as a PNG at the canvas
size, for an artist to draw the replacement over:

```sh
make bg-extract                              # mapping 01, the cathedral
make bg-extract BG_ID=02                     # another mapping
tools/backgrounds.py extract 01 --step 6     # another step of scene 0x0D
tools/backgrounds.py extract --all           # a plain boot; keep every dump
tools/backgrounds.py extract 01 --from /tmp/dump   # file an existing dump
```

The tool runs the app with `OGRE_BG_DUMP=<dir>`, which writes one
`original-step<NNNN>.png` per assembly, before the pack replaces anything, and
files it as `mods/backgrounds/reference/<id>-<name>-original.png`. That
directory is gitignored: it is extracted art and the repository never commits
it. The port ignores the subdirectory when it loads the pack, so a reference
cannot be mistaken for a replacement.

The dump is a live capture of the readback, not an offline ROM decode: the four
`njpeg` sub-images are Huffman-coded and only the game's RSP microcode decodes
them. It runs on every pass the readback copies, so one run through the opening
writes the reference for every backdrop it assembles; a later background needs
only a mapping line and a run that reaches it.

## What the image should be

The game's backdrop is a **496x384 canvas** tiled by four `njpeg` sub-images:

| pass | copy destination | size | canvas rect |
|---|---|---|---|
| 0 | `0x801AAE90` | 320x240 | `(0, 0)` |
| 1 | `0x801D06B0` | 176x240 | `(320, 0)` |
| 2 | `0x801E50D0` | 320x144 | `(0, 240)` |
| 3 | `0x801FB8F0` | 176x144 | `(320, 240)` |

The rects are the asset's own chunk positions (ROM `0x7CADB4` and following: the
chunk headers hold `(-229,-235)`, `(90,-235)`, `(-229,4)` and `(90,4)` with those
sizes, which is the same tiling with the canvas origin at `(-229,-235)`).

Author the PNG as the whole backdrop. The port box-filters it to 496x384,
splits it by those rects, and writes each part over the pass's pixels. Author the
plain image at the canvas aspect (496x384, or a multiple such as 1984x1536) so
the port maps it without a second scaling; `tools/backgrounds.py` extracts
exactly this canvas as the reference to draw over.

A file named `<id>-wide.png` (or `<name>-wide.png`) beside the plain image is
the **widescreen variant**, used when the WIDESCREEN setting is on. Author it in
the widescreen aspect (16:9, for example 1920x1080). The port does not stretch
it to the canvas: it scales it uniformly to the canvas width and anchors the
result to the canvas bottom, so circles stay round. (A 16:9 source stretched to
the 496x384 canvas instead displays 29 % narrow: a circle drawn 1:1 measured
0.71 aspect against 0.96 for a canvas-aspect source.) The rows above the image
repeat its first row, but the draw shows the lower part of the canvas, so those
rows are off-screen in practice. `mods/backgrounds/01-wide.png` is the example;
its round rose window is also the aspect check. Without the variant, the plain
image is used in both modes.

The replacement is at the game's resolution. RT64 renders 2D at
`upscale2D = ScaledOnly`, so the port's internal resolution setting still scales
the result, but no detail beyond the game's 320x240 backdrop survives. True HD
needs the renderer to sample a larger texture (RT64 texture replacement), which
this feature does not do.

## Widescreen

With WIDESCREEN on, the window is 16:9 and a scene the toggle does not expand is
pillarboxed: the 4:3 game image sits in the middle with black bars at the sides.
The mission (`0x03`) and the dialogue/cutscene scene (`0x0D`) expand, so the
backdrop fills the whole width with more of the canvas at the sides. Scene `0x0D`
does this **with or without the pack**: the game's own backdrop already has
content that the 4:3 view crops. The dialogue box, the portrait and the text keep
their own size and position in the wider view.

The scene list is `expand_scenes()` in `app/src/widescreen.cpp`: `0x03` and
`0x0D` by default, plus every scene in `backgrounds.txt`. `OGRE_WS_SCENES`
replaces the default set for a developer run:

```sh
OGRE_LAUNCHER_KEYS=space OGRE_WIDESCREEN=on OGRE_WS_SCENES=0x03,0x0D \
  OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_CAPTURE_PRESENT=/tmp/ws OGRE_CAPTURE_EVERY=100 OGRE_EXIT_AFTER_MS=25000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
```

`widescreen.cpp` logs `[widescreen] aspect ratio expand (scene 0x000D, mode ON)`
when scene `0x0D` expands. With WIDESCREEN off the aspect stays `Original` and
the image is unchanged.

The image the pack writes is chosen by the setting, not by the live aspect: the
backdrop is assembled one scene early, in the `0x02` loader, where the aspect is
still `Original`. With WIDESCREEN on and a `<id>-wide.png` present, the wide
image is used; otherwise the plain image is. The log tells them apart:
`applied 'cathedral' (01 wide) ...` against `applied 'cathedral' (01) ...`.

The game's own vertical letterbox (black at the top and bottom of the dialogue
screen) is part of the scene and is not filled.

## How it works

1. The game decodes each `njpeg` sub-image with the RSP and draws it into a game
   framebuffer.
2. `func_ovlE_8019976C` (bank unit E) copies the framebuffer into one of four
   fixed buffers, one pass per sub-image. Its per-pass state is
   `state = *(u32*)0x8019A680`: `state[0x74]` is the destination, `state[0x78]`
   and `state[0x7A]` are its width and height, and `state[0x80]` selects the
   `'B5'` path (pixels at `state[0x74]`) or the single-image path (pixels at
   `+8`).
3. Bank units are outside the `.nrm` mod hook map (`docs/guides/app-build.md` →
   "What a hook can reach"), so the app calls in from the generated unit
   instead. `tools/hd_backgrounds.py` places `ogre_hd_background(rdram)` at the
   end of the pass copy, next to the `ogre_sync_framebuffers()` call that
   `tools/njpeg_readback.py` places there. Both are re-applied by
   `make bank-recomp`.
4. `ogre_hd_background` reads the current step and scene, looks the step up in
   the pack, and writes the sub-rectangle of the image over the pass's pixels.
   The write is in the runtime's byte order: `MEM_H` is a native `u16` at
   `offset ^ 2` and `MEM_B` a native `u8` at `offset ^ 3` (`recomp.h`). The
   image is 16-bit big-endian RGBA5551.

The app-side code is `app/src/hd_backgrounds.cpp`; the pack is loaded once at
startup from `main.cpp`.

## Verifying

`OGRE_BG_LOG=1` prints a line per pass and per applied chunk:

```
[hdbg] 01         cathedral    scene=0x0D step=2   <- 01.png (1280x960 -> 496x384)
[hdbg] pack /Users/momo/dev/ogre/mods/backgrounds: 1 background(s) loaded
[hdbg] pass dest=0x801AAE90 320x240 step=2 scene=0x7FFF next=0x02 -> cathedral
[hdbg] applied 'cathedral' (01) step=2 chunk (0,0) 320x240 to 0x801AAE90
[hdbg] pass dest=0x801D06B0 176x240 step=2 scene=0x7FFF next=0x02 -> cathedral
[hdbg] applied 'cathedral' (01) step=2 chunk (320,0) 176x240 to 0x801D06B0
[hdbg] pass dest=0x801E50D0 320x144 step=2 scene=0x7FFF next=0x02 -> cathedral
[hdbg] applied 'cathedral' (01) step=2 chunk (0,240) 320x144 to 0x801E50D0
[hdbg] pass dest=0x801FB8F0 176x144 step=2 scene=0x7FFF next=0x02 -> cathedral
[hdbg] applied 'cathedral' (01) step=2 chunk (320,240) 176x144 to 0x801FB8F0
```

At the readback the scene word `D_800C4C26` still holds the `0x02` loader (which
preloads the backdrop for the `0x0D` visit), and `D_8018F1C2` holds `0x02`, so
neither matches the mapped `0x0D`. The entry's step is the identifying value and
the scene is preferred only when it matches; `scene=0x7FFF` in the line above is
`D_800C4C26` holding a control value (`0xFFFF`).

`OGRE_BG_DEBUG=1` paints each pass a solid colour instead of the pack image —
320x240 red, 176x240 green, 320x144 blue, 176x144 yellow — so a screen capture
reads the four-pass composition directly.

The fastest way to the cathedral is the `OGRE_STEP` shortcut:

```sh
OGRE_LAUNCHER_KEYS=space OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 \
  OGRE_NJPEG=1 OGRE_BG_LOG=1 OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=20000 \
  ./build-app/ogrebattle64 assets/ogre64.z64
```

`OGRE_LAUNCHER_KEYS=space` is needed when `.nrm` mods are installed in
`<config>/mods`: the start screen is shown for any install with mods, and
without a key the run waits there and never reaches the game. The alternative is
to move the `.nrm` files aside for the run.

The composed backdrop is drawn a few seconds after scene `0x0D` starts; a live
RDRAM snapshot taken at the scene's first frame holds the raw `njpeg` chunks and
not the composed screen.

## Limits

* Only the four-chunk layout above is recognized. A backdrop with different
  chunk sizes or positions is left alone (the log says `no canvas rect`).
* The pack replaces the backdrop only, not the dialogue box or the portraits.
* The image is downscaled to the canvas, so this is a reskin at the game's
  resolution, not an HD texture replacement. See "What the image should be".
