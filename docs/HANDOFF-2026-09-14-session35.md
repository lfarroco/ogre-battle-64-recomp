# Session 35 — the title screen's white band: a zero-area texture tile

Goal: fix the reported visual regression on the title screen — a solid **white
band across the middle of the scene** where the retail game shows scrolling
clouds behind the "Ogre Battle 64" logo.

Done: the band is gone. It was not a shader or a 3D/2D compositing problem; the
game draws six texture rectangles with a **tile that covers zero texels**, and
RT64 was synthesising a one-texel image for them. `RDP::drawTexRect` (and the
browser renderer's `draw_texrect`) now skip such rectangles, which is what the
hardware does.

## 0. State at the start and at the end

```sh
# before: a solid white band over rows 64..128 of the title screen
OGRE_TAP_MS=4000 OGRE_CAPTURE_PRESENT=/tmp/title OGRE_CAPTURE_AFTER=20 \
  OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=20000 ./build-app/ogrebattle64
# after: clouds behind the logo, matching the retail title screen
```

| check | before | after |
|---|---|---|
| title screen rows 64-128 | opaque white | clouds (retail) |
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
