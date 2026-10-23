# Session 180 handoff — the QUEST still's q draws are recorded in list order and do rasterize; the frame they land in has a black plate

## Goal

Continue the browser fix of the QUEST publisher logo in scene `0x0A`
(`?OGRE_SCENE=publishers`). Session 179 landed the white plate and the `QUEST`
wordmark and left the three 3D "q" meshes missing, with the next measurement
named as "the web walk order for the segmented q sub-lists".

## Result

No code landed. The session's next measurement was run, and it refutes
session 179 §4's ordering claim. The q draws are recorded in the list's own
order, with the state the native decode shows, and the browser does rasterize
them. What differs is the frame they are drawn into: the q lists draw their
640x480 plate copy with PRIM black, so the q triangles land on a black plate,
and no presented frame carries both the white plate and q colour.

Sections 1-4 are the evidence, section 5 is the corrected statement of the
defect, section 6 is the next experiment.

### 1. The q draws are recorded in list order (session 179 §4 is refuted)

A ring of the last 48 recorded draws, dumped when the first lit triangle is
recorded (`[probe180]` lines, browser, forced `publishers`), reads:

```
16 x fill   off=0x1863B0.. fb=0x2E6840 prim=1.00     <- plate white
16 x fill   off=0x1863B0.. fb=0x250800 prim=0.00     <- target black
16 x texrect off=0x186640..0x186988 fb=0x250800 prim=0.98 tex=1   <- the white bands
 3 x texrect off=0x3ACD60,0x3ACDC0,0x3ACE60 fb=0x250800 prim=0.98 tex=1  <- wordmark
 1 x tri2   off=0x026C08 fb=0x250800 prim=0.98 lit=1   <- the first q triangle
```

`0x026C08` is the first `G_TRI2` of the sub-list at `0x26C00`, i.e. the target
of `G_MOVEWORD segment[14] = 0x26C00` + `G_DL 0x0E000000`. The order is the
native decode's order (`bands, wordmark, q's, fade`), so no reordering exists in
the browser's queue: session 179's "the queued order puts the fade bands before
the q triangles" was an artefact of its probe's run-length summary.

The same order is visible one level down, in the walk itself. A trace of the
`G_DL`, `G_SETCIMG`, `G_SETPRIMCOLOR` and `G_SETCOMBINE` commands the executor
runs, for a still list whose target is `0x2E6840` (task 380):

```
cmd 186 off=0x3A46C8  SETCIMG   0x002E6840
cmd 274 off=0x3A46F8  G_DL     0x801863A0    <- 16 plate fill rects
cmd 363 off=0x3A4730  G_DL     0x801B84B8    <- state block
cmd 369 off=0x1B84E0  SETPRIMCOLOR 0xFAFAFAFF
cmd 370 off=0x1B84E8  SETCOMBINE   0xFFFDF6FB
cmd 373 off=0x3A4738  G_DL     0x80186630    <- 16 band texrects, PRIM white
cmd 488 off=0x3A4740  G_DL     0x801B8520
cmd 491 off=0x1B8530  SETCOMBINE   0xFFFCF87C
cmd 553 off=0x3A4918  G_DL     0x801B85B8    <- the q state block
cmd 560 off=0x1B85E8  SETCOMBINE   0xFF33FFFF  (TEXEL0*SHADE)
cmd 581,633,685,...   G_DL     0x0E000000    <- the q sub-lists, 8 in this list
```

The q state block runs before the q sub-lists and the plate-copy block runs
before the bands. The walk applies `G_MOVEWORD G_MW_SEGMENT` (session 21's fix)
and there is one resolver, so a segmented `0x0E000000` reaches `0x26C00`.

### 2. The q draws carry the state the native decode shows

The first lit triangles (`[probe180]`):

```
gy=0x00020000 lights=1 L0=[col 255 255 255 dir 0 0 127] amb=[128 128 128]
oml=0x00552078 omh=0x082C70 ablend=0 force=0 scissor=1(0,0,640,480)
tile=0 tile0=[fmt=0 siz=3 line=8 masks=5 maskt=5] timg=0x001400
v0 raw=(36,156,-28) nrm=0,0,-127 uv=(1344,1792) a=255
 -> ndc=(0.111,0.365) uv=(42.00,56.00) shade=(0.502,0.502,0.502,1.000) w=1.00 z=-0.4929
```

Every one of those fields matches the display list's own commands in
`/tmp/w179/d-17500.bin` (session 179's native RDRAM dump). The window depth is
`(z+1)/2 = 0.2535`, well inside the F3D clip at `1022/1024`, so `u_zclip` cannot
drop these fragments. The blender decodes to `alpha_blend = 0`, so the combiner's
alpha (0 for the `TEXEL0*SHADE` variant) is not used for blending.

Two triangle-state families appear in the scene, not one:

| combiner | evaluated mux | PRIM when seen |
|---|---|---|
| `FC121824 FF33FFFF` | `TEXEL0*SHADE`, alpha `TEXEL0.a*SHADE.a` | 0.98 |
| `FCFFFFFF FFFCF87C` | `TEXEL0` passthrough, alpha `SHADE.a` | 0.00 and 0.98 |

Session 179 saw only the first. The second is the one that appears first in a
run, so "the q draws use `TEXEL0*SHADE`" does not hold for every q frame.

### 3. The q textures are gradients, not solid colours

Session 179 wrote "every texel of each is that colour"; it read the first 32
bytes only. All 1024 texels of each of the three RGBA32 images (read from
`/tmp/w179/d-17500.bin`, texel `R,G,B,A` at the logical address):

| image | texels | range |
|---|---|---|
| `0x80000400` | 32 distinct | `(222,218,39,0)` .. `(215,197,41,0)` |
| `0x80001400` | 29 distinct | `(110,183,18,0)` .. `(215,218,32,0)` |
| `0x80002400` | 29 distinct | `(40,92,186,0)` .. `(40,106,151,0)` |

So these images carry the hue ramp the reference shows. Alpha is 0 in every
texel of all three, which is why the `TEXEL0*SHADE` alpha is 0 and why turning
alpha blending on for these draws makes the meshes vanish.

### 4. The geometry is rasterized, but every q frame has a black plate

A readback of the colour image at the end of a batch that contains q triangles
reads coloured pixels inside it, so the triangles reach the framebuffer:

```
[probe180] flush 1531: fb=0x1BA780 (640x481) colourful=966 bbox=(0,0)-(638,365)
```

The frames are presented with those batches and are still almost black. A canvas
readback at each present (`white = 231,120,200 > 200`, `greenish = g > r+16 && b < g`):

```
present 696: fb=0x250D00 batch=552  white=0 colourful=0    green=0
present 699: fb=0x250D00 batch=552  white=0 colourful=1986 green=0
present 705: fb=0x1BAC80 batch=1104 white=0 colourful=3088 green=0
present 714: fb=0x250D00 batch=1608 white=0 colourful=0    green=0
```

The presents that carry a plate batch (72 draws) read `white=225183
colourful=258419`, and those never contain a triangle. A 24 s screenshot scan at
160x120 confirms the two families never mix: the frames with `white > 13000` have
`green = 0`, and the frames with `green > 500` have `white < 30`.

The batch that draws the q triangles shows why. Its run summary:

```
[F fb=0x2E6840 p=1.00 x16][F fb=0x250800 p=0.00 x16]
[T fb=0x250800 p=0.00 notex x16][t fb=0x250800 p=0.00 tex x500]
```

The 16 `T` draws are the plate copy (combiner `FCFFFFFF FFFDF6FB`, which
evaluates to PRIM only) and they carry PRIM 0.00, so the plate they paint is
black. In the list whose trace is in section 1 the same 16 bands carry PRIM 0.98.
So the browser does draw a white plate with the q's in some lists, and a black
one in others, and only the black ones reach the canvas with triangles.

### 5. The corrected statement of the defect

The q meshes are not missing from the draw queue, are not reordered, and are not
clipped. For the frames that present q colour, the 640x480 plate copy that the
same list draws is painted with PRIM black, so the q triangles end up on a black
plate; the frames whose plate is white present without triangles. The reference
(`docs/proofs/native-quest-screen.png`) has both at once.

Two candidate mechanisms remain, and they are separable with one measurement:

- the bands' PRIM in those lists is set to black by an earlier command than the
  walk trace of section 1 shows (a per-list difference), or
- the recorded PRIM for those bands is right and the list itself fades the plate
  to black before the q's, which would put the fault in the *combination* of a
  fade and a q pass rather than in one state.

### 6. Next experiment

Log, for one frame whose canvas is black and has triangles, the ordered list of
commands that write PRIM (`G_SETPRIMCOLOR`), CIMG and combiner, with the walk
offset of each, and the run summary of the same batch. Then compare that frame's
list with `OGRE_DL_DECODE=<dl>` of the same display list. The question to answer
is which `G_SETPRIMCOLOR` the 16 bands of that batch inherit: `0x1B84E0`
(`0xFAFAFAFF`) or the fade block's `0x000000FF`.

Do not patch the q draws before that. Their alpha is 0 in the `TEXEL0*SHADE`
variant, and their texture is a hue ramp, so a change to the q state changes the
colour of a mesh that is already visible inside its colour image.

## Verification

- `EM_CACHE=<repo>/build-wasm/emcache make web` clean, before and after the
  probe was reverted (the default cache `~/.cache/emscripten-ogre` is outside
  the workspace and the sandbox denies its lock file).
- The evidence above comes from browser runs against `debug/server.py` with GPU
  Chromium (`--enable-gpu --use-angle=metal`), forced `?OGRE_SCENE=publishers`,
  canvas 639x475.
- The revert is proved: `grep -rn "probe180\|OGRE_PROBE_ORDER\|OGRE_PROBE_PERSIST"
  app/ debug/` is empty, and `git diff app/src/web_renderer.cpp` is session 179's
  94-line change again.
- Not measured: the ATLUS and Licensed stills after any change (none was made);
  the native frame at the same animation step (the reference screenshot is a
  window capture with no recorded time or list number).

## Files changed

| file | change |
|---|---|
| `docs/HANDOFF-2026-10-19-session180.md` | this file |
| `PLAN.md` | status paragraph and open-work item 19 corrected |
| `docs/DECISIONS.md` | the correction row |
| `docs/STATUS-LOG.md` | session entry |
| `docs/README.md` | index row |
| `docs/scenes.md` | the QUEST still's status |
| `app/src/web_renderer.cpp` | **no net change**: session 179's fix only (the probe was removed) |
| `build-wasm/` | rebuilt by `make web` (gitignored) |

## Probes used and reverted

All of it was tagged `probe180` and gated on `OGRE_PROBE_ORDER` /
`OGRE_PROBE_PERSIST`, and all of it is gone. It was: a ring of the last 48
recorded draws; a per-lit-triangle state line; a per-triangle readback of the
colour image (float and 8-bit attachment paths); a per-batch run-length summary
with target and PRIM; a per-batch full per-draw dump; a distinct-triangle-state
set; a per-flush sequence and target history; a per-present canvas statistics
line; a PPM dump of the batch's colour image; a walk trace of the state-setting
commands; and an A/B that kept one `RenderState` across display lists
(`OGRE_PROBE_PERSIST`), which changed nothing measurable.

No recompiled code and no RT64 file was touched. `tools/RT64` was already
modified before this session (the submodule's patch state).

## Instrument notes

- The publisher stills are 640x480 and the VI's origin is `fb + 0x500`, so a
  present logs `0x1BAC80`/`0x250D00` for the buffers at `0x1BA780`/`0x250800`.
  The scan should match on the VI origin, not the framebuffer address.
- The stills' colour images are RGBA16F. `glReadPixels(..., GL_UNSIGNED_BYTE)`
  fails on them with `INVALID_OPERATION` and returns zeros, which reads as a
  black image. Read `GL_FLOAT` and scale when `GlFramebuffer::fp` is set.
- `?OGRE_SCENE=publishers` enters scene `0x0A` and then plays the whole still
  sequence (N64 logo, Licensed, ATLUS, QUEST) from `t≈0` of that scene. The q
  frames come about 20 s after `[scene] forced scene 0x0A is active`.
