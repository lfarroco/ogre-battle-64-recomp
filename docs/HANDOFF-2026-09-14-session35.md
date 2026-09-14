# Session 35 — the title screen's fog: a zero-area texture tile

Goal: fix the reported visual regression on the title screen — a solid **white
band across the middle of the scene** where the retail game shows scrolling
clouds behind the "Ogre Battle 64" logo, and then restore the **very faint white
fog** that moves over those clouds.

Done: the band is gone and the fog is drawn. Neither was a shader or a 3D/2D
compositing problem. The game draws six texture rectangles with a **tile that
covers zero texels**; RT64 synthesised a one-texel image for them (an opaque
white band), and the fix is a two-part one: `RDP::drawTexRect` skips such a
rectangle, and an app-side `OGRE_FOG` pass rewrites the texture setup it
inherits so it samples the fog layer's own 64x64 image (which is what the
retail title screen shows).

## 0. State at the start and at the end

```sh
# before: a solid white band over rows 64..128 of the title screen
OGRE_TAP_MS=4000 OGRE_CAPTURE_PRESENT=/tmp/title OGRE_CAPTURE_AFTER=20 \
  OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64
# after: clouds behind the logo, matching the retail title screen
```

| check | before | after |
|---|---|---|
| title screen rows 64-128 | opaque white | clouds + the faint fog (retail) |
| `OGRE_FOG_TRACE=1` | — | `layer 0 image=0x801D2F88 64x64 siz=1 fmt=4 line=8`, `layer 2 image=0x801D51E8 ...` |
| boot into a screen | hex scene id only | `OGRE_SCENE=title\|menu\|new-game` (+ hex) |
| `OGRE_SPEED=4`, 130 s attract run | exit 0 | **exit 0**, 1457 display lists, **0 streamed stubs** |
| `make`/`cmake --build build-app`, `build-null`, `build-wasm` | clean | clean |
| smoke / 3D logo characters (session 34) | present | present (unchanged) |
| publisher screens, unit info, attract scenes | correct | correct (unchanged) |

## 1. How the culprit was found

`OGRE_DL_DECODE=1087` (the title scene's display list) plus a new bisect tool.

### 1a. The new bisect tool: `OGRE_NOP_RECT`

`app/src/renderer.cpp` gained an env-gated patcher that walks the submitted
display list (F3DEX2, with `G_DL` push/branch and segment registers), and
replaces every matching `G_TEXRECT` **and its `RDPHALF_1`/`RDPHALF_2` pair** with
`G_SPNOOP` before RT64 parses it. Selectors:

```
any            every texture rectangle
tex:<hex>      the current SETTIMG address (e.g. tex:801E2918)
flip           dsdx < 0 (a right-to-left rectangle)
x:<a>-<b>      the rectangle's screen columns overlap [a,b)
y:<a>-<b>      the rectangle's screen rows overlap [a,b)
n:<index>      the nth texture rectangle of the list
```

```sh
# remove only the right-to-left rectangles of the title scene
OGRE_SPEED=4 OGRE_NOP_RECT=flip OGRE_CAPTURE_PRESENT=/tmp/nf \
  OGRE_CAPTURE_AFTER=100 OGRE_CAPTURE_EVERY=50 OGRE_EXIT_AFTER_MS=60000 \
  ./build-app/ogrebattle64
```

That run reproduces the retail title screen exactly (see
`docs/proofs/native-title-band-before-after.png`, bottom half), and a diff of the
two captures showed the frames are bit-identical outside the band (`meanDiff
0.00` above the band, `0.05` below it, `41.5` inside it). So the six `dsdx < 0`
rectangles are the whole story.

### 1b. What those rectangles are

```
TEXRECT ulx=0  uly=256 lrx=24  lry=512 tile=0 s=160  t=0 dsdx=-1024 dtdy=1024
TEXRECT ulx=24 uly=256 lrx=280 lry=512 tile=0 s=2016 t=0 dsdx=-1024 dtdy=1024
TEXRECT ulx=280 … lrx=536 …
TEXRECT ulx=536 … lrx=792 …
TEXRECT ulx=792 … lrx=1048 …
TEXRECT ulx=1048 … lrx=1304 …
```

Six pieces tiling the full width, rows 64-128, sampling the layer-2 texture
`0x801E2918` right-to-left (the `s` values scroll over time; the `x` split moves
with them). The combiner set immediately before them is `FCFFFFFF FFFF73B9`.
Decoding it with the `GCCc0w0`/`GCCc0w1` packing from the N64 SDK's
`include/PR/gbi.h` (`_SHIFTL(a0,20,4) | _SHIFTL(c0,15,5) | _SHIFTL(Aa0,12,3) |
_SHIFTL(Ac0,9,3)` for `w0`, and `b0`@28, `Aa1`@21, `Ac1`@18, `d0`@15, `Ab0`@12,
`Ad0`@9, `d1`@6, `Ab1`@3, `Ad1`@0 for `w1`):

```
cycle 0/1: RGB   = (ZERO - ZERO) * ZERO + ONE        -> white
           ALPHA = (ZERO - ZERO) * ZERO + TEXEL0      -> the sampled texel's alpha
```

so it is a **white overlay modulated by the texture's alpha**. (RT64's own
`ColorCombiner::cycleColorText`/`cycleAlphaText` decode agrees; the new
`OGRE_RECT_STATE` trace prints exactly this.)

The tile those rectangles name is the one the layer-2 strip loop left behind:

```
SETTILESIZE t0 uls=0 ult=420 lrs=1276 lrt=420
```

`lrt == ult`: the tile covers **zero texels**. (A one-texel-tall tile would be
`lrt == ult + 4`.) The game leaves the same kind of tile at the end of the
layer-3 loop (`ult == lrt == 636`) and draws that loop's degenerate tail strip
with it.

### 1c. Why that painted white

`State::loadDrawState` (`src/hle/rt64_state.cpp`) derives the sampling rectangle
as `sampleHeight = max((lrt - ult + 4) / 4, 1)`. The `max(..., 1)` is necessary —
an empty texture cannot be decoded or sampled — but for a zero-height tile it
*fabricates* a one-texel-tall image out of whatever TMEM holds at that moment.
The `LOADTILE` that ran just before loaded texture row 105 of `0x801E2918`, and
rows 60-105 of that texture are **fully opaque** (alpha 255 once the RDRAM is read
with the runtime's `^3` byte order; reading the raw bytes the other way round is
what made a first pass at this look as if the row had alpha ~0.5).

Evidence trail:

* `OGRE_TILE_TRACE=1` (new) showed the draws resolve to a cached `320x1` texture
  (`idx=6`/`idx=111`, `tcScale=1,1`, `sample=320x1`, `rawTMEM=0`).
* `OGRE_DUMP_TEX=<dir>` (new) writes RT64's own `4 KiB .tmem` + `.tile.json` per
  decoded texture. Decoding `c41a25a86b03c472.v5.tmem` with the RGBA32 TMEM
  layout (`TMEM[i*2]`/`TMEM[i*2+1]` = R,G; `TMEM[0x800 + i*2]`/`+1` = B,A) gives
  `R=121 G=108 B=68 A=255` for texel 0 — an opaque row.

So: `RGB = ONE`, `ALPHA = 255` → opaque white, for every pixel of the six
rectangles. On the retail title screen the same region is clouds, so the
hardware is not sampling a texel from that tile at all.

## 2. The fix

`RDP::drawTexRect` (`src/hle/rt64_rdp.cpp`) returns early when the named tile is
**configured** (`line != 0`) and covers **no texels**:

```cpp
if (skipEmptyTile && (rectTile.line != 0) &&
    ((rectTile.lrs == rectTile.uls) || (rectTile.lrt == rectTile.ult))) {
    return;
}
```

* `line != 0` keeps a never-configured tile (all four bounds zero) on RT64's
  existing "no texture" path — those rectangles are real draws (the publisher
  screens and the intro's fades use them) and are untouched.
* `lrs == uls` / `lrt == ult` cannot catch a legitimate one-texel tile
  (`lrs == uls + 4`).
* `OGRE_EMPTY_TILE=draw` restores the old behaviour for A/B runs;
  `OGRE_EMPTY_TILE_TRACE=1` names every skipped rectangle.

`app/src/web_renderer.cpp`'s `draw_texrect` gets the same guard so the browser
renderer cannot diverge from the native one (`ensure_tile_image` did the same
`(lrt >> 2) - (ult >> 2) + 1` fabrication).

The skipped rectangles in the title scene are exactly the pathological ones
(`OGRE_EMPTY_TILE_TRACE=1`):

```
[empty-tile] skipped rect 0,516..1280,520 tile=0 uls=0 ult=420 lrs=1276 lrt=420   (layer-2 tail strip)
[empty-tile] skipped rect 0,256..256,512   tile=0 uls=0 ult=420 lrs=1276 lrt=420   (overlay piece 1)
...                                                                                  (pieces 2-6)
[empty-tile] skipped rect 0,956..1280,960  tile=0 uls=0 ult=636 lrs=1276 lrt=636    (layer-3 tail strip)
```

## 3. Diagnostics added this session

All env-gated; the RT64-side ones live in `rt64-ob64.patch`.

| knob | where | what it does |
|---|---|---|
| `OGRE_NOP_RECT=<selectors>` | app | NOP matching `G_TEXRECT`s in the submitted DL (see §1a) |
| `OGRE_RECT_STATE=<y0>-<y1>` | RT64 | for each rectangle contained in `[y0,y1)`: cycle type, both combiner cycles decoded by RT64, blender inputs, prim colour, and the tile descriptor it samples |
| `OGRE_TILE_TRACE=1` | RT64 | every tile whose sampling rectangle is degenerate, with the texture the cache returned (`hash`, `idx`, dims, `tcScale`, `rawTMEM`) |
| `OGRE_DUMP_TEX=<dir>` | RT64 | dumps the `4 KiB .tmem` and `.tile.json` of every decoded texture |

## 4. The checked-in RT64 patch was stale

`rt64-ob64.patch` did not carry `OGRE_CAPTURE_EVERY` (added in sessions 33/34 and
already documented in `docs/guides/app-build.md`) — the working tree had drifted
from the patch file. It is regenerated now:

```sh
git -C tools/RT64 diff --ignore-submodules=all > rt64-ob64.patch
```

(`--ignore-submodules=all` keeps `src/contrib/plume` out; it has its own patch,
`rt64-plume-ob64.patch`.) Anyone applying the patch to a fresh RT64 checkout now
gets the same tree this session built and ran.

## 5. Files changed (this session)

* `tools/RT64/src/hle/rt64_rdp.cpp` — the empty-tile guard (**the fix**), plus the
  `OGRE_RECT_STATE` trace.
* `tools/RT64/src/render/rt64_framebuffer_renderer.cpp` — `OGRE_TILE_TRACE`.
* `rt64-ob64.patch` — regenerated (includes the previously missing
  `OGRE_CAPTURE_EVERY`).
* `app/src/renderer.cpp` — `OGRE_NOP_RECT`, and `OGRE_DUMP_TEX` (which only has
  to point RT64's existing `state->dumpingTexturesDirectory` at a directory
  instead of opening its file dialog).
* `app/src/web_renderer.cpp` — the same empty-tile guard for the browser renderer.
* `docs/DECISIONS.md`, `PLAN.md`, this file,
  `docs/proofs/native-title-band-before-after.png` (new crop),
  `docs/proofs/native-intro-title.png` (refreshed),
  `docs/proofs/native-attract-loop-title-fixed.png` (new attract montage).

The vendored `tools/RT64` checkout is **not** committed (a submodule only records
an upstream SHA), so the RT64 changes only exist in `rt64-ob64.patch`. Apply it
after checking the submodule out:

```sh
git -C tools/RT64 apply ../../rt64-ob64.patch
```

## 6. Next

1. **Sessions 33/34's open work is unchanged** — see
   `docs/HANDOFF-2026-09-12-session34.md` §6: make the bank function sets
   deterministic, then finish the cross-bank dispatch, then scene `0x18`/`0x02`.
2. `osViFade` is still unemulated (fades snap).
3. The session-31 idle-stall oddity, and the bank swap being "latest load wins".
4. **Optional:** the same zero-texel tile guard has not been exercised in a
   browser build (the wasm variant builds, but this session did not run the
   probes). `docs/guides/web-probes.md` has the harness.

## 7. Addendum — the missing fog (same rectangles, the other half of the bug)

The band fix alone left the title screen missing the **very faint white fog**
that moves over the retail's clouds. It is the same six rectangles: they are the
layer's horizontally scrolling "wrap" (a 64x64 image, mirror-sampled 1:1 and
repeated across the width), drawn with the white combiner
(`FCFFFFFF FFFF73B9` = `RGB ONE, ALPHA TEXEL0`), so the image's own intensity
becomes a faint white overlay.

### Where the image is

`func_8019C5D4(0)` (overlay C, 0x8019C5D4) is the wrap pass for layer 0 of the
game's *effect* layer table and intends to upload that layer's image before
drawing it; the upload is skipped in the display lists the port sees, so the
rectangles inherit the layer-2 loop's stale, zero-area tile instead. The image
is nevertheless reachable through the game's own table:

```
D_801B80E0                     -> layer table pointer (0x801BA770 in the title scene)
table + 0xFC + i*0x24          -> layer i's record
record[0]                      -> image record
image record: +8 = the data, +4/+6 = height/width, byte 3 = siz, byte 2 = fmt
```

For the title screen, layer 0 is **`0x801D2F88`, a 64x64 8-bit intensity image**
(mean intensity 0.051 — "very faint", matching the retail), and layer 2 is the
same kind of image at `0x801D51E8`.

### The repair

`app/src/renderer.cpp`'s `OGRE_FOG` pass (on by default, `OGRE_FOG=0` to
disable) reuses the proven `OGRE_NOP_RECT` walker: whenever a `G_TEXRECT` is
about to sample a zero-area tile while the white combiner is active, it rewrites
the last `SETTIMG` / `SETTILE t7` / `LOADTILE` / `SETTILE t0` /
`SETTILESIZE t0` in the list so the rectangle samples the layer's own image.
`OGRE_FOG_TRACE=1` prints each repair. Verified with `OGRE_RECT_STATE=64-128`:
the rectangles now reach RT64 as `fmt=4 siz=1 line=8 uls=0 ult=0 lrs=63 lrt=63`
— the 64x64 fog image — and the band region shows the fog
(`docs/proofs/native-title-fog.png`, and `native-title-fog-vs-band.png` for the
fog / no-fog / old-white-band comparison).

## 8. Addendum — booting straight into a screen

`OGRE_SCENE=<name|hex>` (app/src/bank_overlays.cpp) replaces the hex-only
`OGRE_FORCE_SCENE`. The dispatcher (`asm/1060.s @0x80075DB8`) runs the per-scene
update function of `u16(D_800C4BBC + 4)` and stores it in `D_800E810E`, so
poking that word enters the scene. Known names:

| name | id | screen |
|---|---|---|
| `title` | 0x0C | attract title (clouds + logo + fog) |
| `menu` | 0x18 | title menu (New Game / Tutorial / Stereo) |
| `new-game` | 0x02 | New Game / Tutorial path |

`OGRE_SCENE_LOG=1` logs every scene change with its descriptor and record mask —
how the ids are found. The poke is deliberately on the streamed-DMA path (scene
loads), not per frame: poking every frame fights the scene state machine and
crashes it. `OGRE_SCENE_AFTER_MS` (default 3000) delays the first poke; poking
before ~2 s crashes the boot.

**Caveat:** scenes `0x18`/`0x02` still rely on the unfinished cross-bank work
(sessions 33/34), so forcing them can still abort; the attract-loop scenes
(`0x0C`) are the safe ones.


## 9. Addendum — the bottom-of-screen fog (the `©1999 QUEST` region)

The user reports the fog is most visible around the `©1999 QUEST` copyright line
(N64 rows ~185-206, x ~186-254), i.e. the **bottom sweep**, not the logo band.
That sweep is a *different* draw from the logo-area fog: 448 one-screen-row-tall
`G_TEXRECT`s (offsets 0x29A4D8.., white combiner `FCFFFFFF FFFF73B9`) whose `t`
advances one texel per row and whose `s` walks from 2752 down to 0 while the
x-extent grows — a diagonal cloud wipe. It inherits the layer-3 loop's stale,
zero-area tile (`uls=0 ult=636 lrs=1276 lrt=636`), and its `s` range (86 texels)
does **not** fit the 64-texel layer image, so it is not the layer-image pass.

What the port does now (after the session-35 fog work):

* the **logo-area** group (the first white group of the frame, `s` = 63..0) is
  rewritten to sample the layer's own 64x64 image (the faint fog);
* the **bottom sweep** keeps the game's tile and only gets a one-texel height,
  so RT64 decodes exactly the stale row out of TMEM and draws the wipe the way
  the hardware does, instead of skipping it.

Verified in the `©1999 QUEST` window: the region now shows **moving** content
(six consecutive title frames in
`docs/proofs/native-title-fog-quest-region.png`), and its mean brightness
(42.2) matches the retail reference (42.8) more closely than the previous
skip-everything build (46.2).

Known limitation: that pass is *meant* to sample a 320-wide cloud texture at a
diagonal scroll, but only three of its rows fit in TMEM and the upload never
appears in the display lists, so no renderer can reconstruct the intended cloud
shape; the sweep therefore reads as smooth moving bands rather than wispy
clouds. Reconstructing it properly needs the app to synthesise the missing
per-strip uploads (it knows the texture address from the `SETTIMG` the rects
inherit).

Diagnostics used for this: `OGRE_FOG_TRACE=1` (per-group repair report with the
rectangle's `s`/`t`), `OGRE_FOG=alternate` (applies the repair on every other
display list, so two consecutive presents differ by exactly what it draws) and
`OGRE_RECT_STATE=<y0>-<y1>` (RT64's view of the tile a rectangle ends up with).


## 10. Addendum — measuring the fog, and how strong it is

Runs of the attract loop do not line up frame-for-frame, so comparing two runs
("fog on" vs "fog off") is dominated by which scenes each run happened to
capture. `OGRE_FOG=alternate` removes that: the repair is applied on every other
display list, so **two consecutive presents in one run** differ by exactly what
the fog draws. Over 293 consecutive pairs (skipping pairs whose control region
moved, i.e. scene cuts), the fog band (rows 64-128) differs by a median of 0.63
and up to **17/255**, while a control region (rows 150-230) stays under 2. So the
logo-area fog really is drawn, and its magnitude matches the retail's excess in
that band (retail 132.2 vs 117 without the fog).

`docs/proofs/native-title-fog-isolation.png` shows the strongest such pair:
the band with the fog off, the same band one frame later with it on, and the
difference amplified 6x — the fog's wispy shape is plainly visible there.

`OGRE_FOG_SCALE=<percent>` scales the image's intensity when it is staged in
scratch RDRAM (the last 64 KiB, which OB64 leaves untouched). At 1000 the
visible contribution saturates at ~18, i.e. the fog covers most of what the
unoccluded rows can show; the default 100 is already the right order of
magnitude, so use the knob only to taste.

## 11. What is still open (for a fresh session, not a quick fix)

The **bottom sweep** (the `©1999 QUEST` band, offsets 0x29A4D8..) is *meant* to
sample the 320-wide layer-3 cloud texture at a diagonal scroll. Only three rows
of that texture fit in TMEM at a time, and the display list contains no per-row
upload, so no renderer can reconstruct the intended cloud shape from what the
game submits: the port draws the stale row the way the hardware does. Its mean
brightness in that window (42.2) already matches the retail (42.8), so
reconstructing the uploads is speculative work, and it needs the app to
synthesise per-strip `SETTIMG`/`LOADTILE` sequences (the texture address is
available from the `SETTIMG` the rectangles inherit). A second, more likely
candidate for any remaining difference is RT64 adding `G_TX_CLAMP` to every tile
whose `masks`/`maskt` are 0 (`State::loadDrawState`), where the hardware treats
`mask == 0` as "no masking" and reads on through TMEM.
