# Session 36 — jumping straight into a scene, and what the title "fog" really is

Two things were reported at the start of this session:

1. `docs/proofs/native-title-fog-isolation.png` is **not** the title screen — it
   is a crop of the story/lore screen.
2. Testing would be easier if a run could **jump straight to the title screen**.

Both are addressed. Getting there turned up two more real bugs (a SIGBUS in the
scene log and a broken tile-size encoding in the fog repair) plus a missing white
group, and it corrects session 35's conclusion that the fog was already drawn.

## 0. TL;DR

| question | answer |
|---|---|
| does `OGRE_SCENE` work? | it does **now**. It never did with its documented `OGRE_SCENE_AFTER_MS=3000` default: the poke has to land before the boot enters its first scene (~1.1 s). Default is now 0 |
| why did `OGRE_SCENE_LOG=1` crash? | `poll_scene` dereferenced the scene-descriptor word unconditionally; during boot it holds a stale non-KSEG0 value, so `descriptor + 0x10` walked off RDRAM → SIGBUS at ~1 s. Fixed |
| what is the title screen's scene id? | **`0x04`**, and only its later phase (after the prologue text) is the logo + PRESS START. The old `title` = `0x0C` is the unit-description book |
| what was `native-title-fog-isolation.png`? | a crop of the lore/story screen (`OGRE_FOG=alternate` pairs), not the title |
| why was the fog missing? | two bugs in the `OGRE_FOG` repair: `G_SETTILESIZE`/`G_LOADTILE` encode uls/ult/lrs/lrt in **quarter-texel** units and the repair wrote texels (a 64x64 image declared as a ~16x16 tile, so the repaired rectangles sampled a clamped corner and drew nothing); and only the *first* white group per frame was repaired, so the bottom sweep inherited a dead tile |
| why did the fog blink? | the sweep's `s` walks to ~118 texels (it is written against a 320-wide texture) and the repair dropped any group whose `s` passed the 64-texel image, so the fog vanished for a few frames each scroll. The draw tile now wraps (`masks`/`maskt`) instead |
| why was it too heavy? | it wasn't, at the game's own value: the overlay is the layer image's intensity (5.1% mean / 20.4% peak) through the game's `RGB=ONE, ALPHA=TEXEL0` combiner. `OGRE_FOG_SCALE` defaults to **100** = the raw asset; the 45% it briefly used was our over-correction |
| replacement proofs | `docs/proofs/native-title-fog-isolation.png` (logo band, fog off/on/×6) and `docs/proofs/native-title-fog-quest-region.png` (the `©1999 QUEST` sweep) |

## 1. Booting straight into a scene

`OGRE_SCENE=<name|hex>` (or the hex-only `OGRE_FORCE_SCENE`) pokes the attract
scene id so the dispatcher enters a screen without waiting out the attract loop
(which is minutes long at 1×).

```sh
# straight to the attract title (logo + PRESS START over the clouds)
OGRE_SCENE=title ./build-app/ogrebattle64
# the scene changes, with descriptor and record mask
OGRE_SCENE=title OGRE_SCENE_LOG=1 ./build-app/ogrebattle64
# the forcing state, every 500 ms
OGRE_SCENE=title OGRE_SCENE_TRACE=1 ./build-app/ogrebattle64
```

### 1a. It only works early — the default was wrong

`func_80075BC0` (`asm/1060.s @0x80075DB8`) is the scene dispatcher. Every call:

```
block = *(u32*)D_800C4BBC            ; the scene-state block
*(u16*)(block + 4) = *(u16*)D_800E8214   ; 1. copy the pending id into it
if (block->id != pending) goto run;      ; 2. run block->id
...                                       ;    (the equal path re-inits the block)
run: D_800E810E = block->id & 0x1F;      ; 3. record the active id
     (*D_800AF028[block->id])()          ; 4. call the scene's update function
```

A poke is only seen while the boot's own state machine still owns the block.
`OGRE_SCENE_TRACE=1` shows exactly what happens with the old default:

```
# OGRE_SCENE=0C OGRE_SCENE_AFTER_MS=3000
[scene-trace] t=3000ms active=0x0009 pending(D_800E8214)=0x000C block=0x800AEFE0 block->id=0x000C done=0
[scene-trace] t=3500ms active=0x0009 pending(D_800E8214)=0x000C ...
...                    (the poke sticks, but the running scene never yields)
```

versus `OGRE_SCENE_AFTER_MS=0`:

```
[scene] forcing scene 0x0C
[scene] forced scene 0x0C is active
[scene] t=1171ms id=0x000C descriptor=0x8018FB58 mask=0x00003C00
```

Scene 0x09 is entered at t≈1.1 s. A poke that lands after that is overwritten by
the running scene's update function every frame, so the documented default of
3000 ms made `OGRE_SCENE` a silent no-op. `OGRE_SCENE_AFTER_MS` now defaults to
**0**; setting it to ≥1500 reproduces the old broken behaviour for A/B runs.

`force_scene_on_load` (the streamed-DMA hook) still pokes too — a bank load
*inside* a scene update is the one moment the dispatcher re-reads the block id —
and `poll_scene` retries per frame until `D_800E810E` reports the target, then
releases. Both words are written: the block's id and the pending `D_800E8214`.

### 1b. `OGRE_SCENE_LOG=1` crashed the run

```
$ OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=12000 ./build-app/ogrebattle64
Bus error: 10 (exit 138) after ~1 s
```

`poll_scene` logged `descriptor` (`D_800E8294`) and then read `descriptor + 0x10`
without checking it. Before the first scene is established the game leaves a
stale value there (`0xF8DF0DE8` in the session-35 crash snapshot), so the read
walked off RDRAM. The descriptor is now only dereferenced when it is inside
KSEG0/RDRAM. `OGRE_SCENE_LOG=1` runs are stable again.

(The session-35 note "poking before ~2 s crashes the boot" was almost certainly
this SIGBUS, not the poke: the poke is what has to happen before ~1.1 s.)

### 1c. The scene table, verified against captures

Each id was forced with `OGRE_SCENE=<hex> OGRE_SCENE_AFTER_MS=0` and captured
(`OGRE_CAPTURE_PRESENT`). Names in `kScenes` (`app/src/bank_overlays.cpp`):

| name | id | screen (verified visually) |
|---|---|---|
| `title` | `0x04` | attract title: prologue text, then "Ogre Battle 64 / PRESS START" over the scrolling clouds |
| `intro` | `0x09` | boot intro: soldiers, falling cube, Nintendo 64 logo |
| `publishers` | `0x0A` | "Licensed by Nintendo" / ATLUS / QUEST |
| `story` | `0x0B` | world-map story attract |
| `unit-info` | `0x0C` | the unit-description book |
| `menu` | `0x18` | title menu — **still SIGSEGVs** (needs the cross-bank work) |
| `new-game` | `0x02` | New Game / Tutorial — **still SIGSEGVs** (needs the cross-bank work) |

So session 35's `title = 0x0C` was wrong: `0x0C` is the book/unit-description
screen. The scene that contains the title screen is `0x04`, and the logo phase
is *after* its prologue text — `OGRE_SCENE=title` therefore lands on the
attract sequence that ends in the title, it does not skip the prologue.

## 2. The reported proof was from the wrong screen

`native-title-fog-isolation.png` showed a landscape with a dark band — the
story/lore attract screen (scene `0x0B`/the prologue phase of `0x04`), not the
title. It was produced the way session 35 §10 describes: run the attract loop
with `OGRE_FOG=alternate`, keep consecutive presents, and pick the pair with the
largest band difference. That method cannot know *which* screen the pair is on,
and the strongest pairs happen to be on the actively-animating story screens.

The scene-0x04 captures make the phases obvious (`OGRE_SPEED=4`, every 100th
present):

```
presents 1701..1721   story prologue text ("Long ago there was a time ...")
presents 1741..1841   the title (logo + PRESS START)
presents 1861+        world-map story screens
```

The replacement proof is `docs/proofs/native-title-band-isolation.png`,
captured on the title itself (present 1920, `OGRE_SPEED=4`):

```sh
# fixed (the OGRE_FOG repair is off and the zero-texel guard does the work)
OGRE_FOG=0 OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/off/f \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=26000 \
  ./build-app/ogrebattle64
# pre-fix: draw the fabricated one-texel tile instead of skipping the rectangle
OGRE_FOG=0 OGRE_EMPTY_TILE=draw OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/old/f \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=24000 \
  ./build-app/ogrebattle64
```

Panels are N64 rows 64-128 of the two frames at present 1920 (byte-identical
outside the band: control region diff `0.00`, rows 30-60 diff `0.00`), plus the
difference amplified 4×. Band mean 172.0 (pre-fix) vs 118.9 (fixed).

## 3. The missing fog: two bugs in the `OGRE_FOG` repair

Session 35 §7/§10 concluded that the `OGRE_FOG` pass restores the title's faint
white fog. It did not: `OGRE_FOG=1` and `OGRE_FOG=0` produced byte-identical
title frames (`OGRE_EMPTY_TILE=draw` was the only thing that changed the band),
so the pass was rewriting the display lists and then drawing nothing. There were
two separate reasons.

### 3a. The tile extents were written in the wrong units

`G_SETTILESIZE` and `G_LOADTILE` carry `uls/ult/lrs/lrt` in **quarter-texel**
units, not texels: the game's own tiles use `lrs = (width - 1) * 4` (a 320-wide
texture is `lrs = 1276`, a 256-wide one `lrs = 1020`). The repair wrote the texel
extents directly:

```cpp
const uint32_t last = ((img.width - 1) << 12) | (img.height - 1);   // 63,63
```

so a 64x64 image was declared as a ~16x16 tile. The six logo-band rectangles
sample `s = 63..0`, far outside that tile, and RT64 clamped them onto a corner
of the fog image — which is transparent — so the repaired rectangles drew
nothing. `OGRE_RECT_STATE=64-128` showed the wrong tile plainly
(`uls=0 ult=0 lrs=63 lrt=63`) and the handoff read it as correct.

The fix is `last = (((width - 1) * 4) << 12) | ((height - 1) * 4)`. With it, the
same two runs differ in the logo band by **mean 4.6, max 41** (band brightness
131.2 → 137.5) and are still identical everywhere else.

### 3b. Only the first white group of a frame was repaired

The walker cleared `fog_white` after the first repair, on the theory that the
later white groups are cloud wipes that do not fit the layer image. But the title
frame has **three** white groups:

* the layer's wrap behind the logo (the six rectangles, `s = 63..0`, `t = 0`);
* the bottom-of-screen sweep: 64 one-screen-row-tall rectangles at N64 rows
  176-239, `s = 288..1920`, `t = 0..2016` — the `©1999 QUEST` fog the report was
  actually about. They inherit the cloud loop's zero-area tail tile
  (`uls=0 ult=636 lrs=1276 lrt=636`), which points at row 159 of the *cloud*
  texture; the old fallback gave it a one-texel height and let RT64 read that
  dead row, which draws nothing.

The repair now runs for every zero-area white group (layer 0 for the wrap, layer
2 for the sweep). Measured at the same presents (fog on vs off, runs verified in
lockstep outside the fog bands, fog at the default 45% — see §3d):

| region | before | after |
|---|---|---|
| logo band (N64 rows 64-128) | `0.00` | mean 2.2, max 17 |
| `©1999 QUEST` band (rows 176-216) | `0.00` | mean 1.4, max 16 |

and the band now *moves*: consecutive title frames differ by mean ~0.5-1
(max ~10) in the `©1999 QUEST` band, where before every frame was identical.

```sh
# fog on / fog off, same run parameters; compare frames at the same present
OGRE_FOG=1 OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/on/f \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64
OGRE_FOG=0 OGRE_SPEED=4 OGRE_CAPTURE_PRESENT=/tmp/off/f \
  OGRE_CAPTURE_AFTER=1700 OGRE_CAPTURE_EVERY=10 OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64
```

Proofs: `docs/proofs/native-title-fog-isolation.png` (logo band, off / on /
difference ×4) and `docs/proofs/native-title-fog-quest-region.png` (the
`©1999 QUEST` sweep). Both are from the title screen, unlike session 35's.

### 3c. The blink: the sweep's `s` ran off the end of the image

With only §3a/§3b fixed the fog appeared and vanished every few frames. The
decision that caused it was the bounds check: `repair` refused any group whose
first rectangle's `s` was past the image width, falling back to the dead one-row
tile. The sweep walks `s` from 0 to ~118 texels (it is written against a 320-wide
cloud texture), so every time `s` crossed 64 the fog dropped out for a few
frames. `OGRE_FOG_SUMMARY=1` shows it directly — the sweep group reports
`used=0x00000000` on exactly those frames:

```
[fog-summary] dl=1800 ptr=0x802979A0 repaired=2 last_layer=2 last_image=0x801D51E8 white=3 zero=2
```

The draw tile now *wraps* (`masks`/`maskt` = log2 of the image size, so 6 for
64x64) instead of dropping the group, and only a negative coordinate falls back.
The `©1999 QUEST` band after the fix is flat to within 0.84/255 across the title
(it was alternating 63.8/67.5). `OGRE_FOG_SUMMARY=1` logs one line per display
list plus one per white group, which is how this was traced.

### 3d. The level comes from the game, not from a constant

The fog's opacity is not a number the port picks. The game's combiner is
`RGB = ONE, ALPHA = TEXEL0`, alpha compare is off and the blend colour alpha is
0, so the overlay is white modulated by the *sampled texel's alpha* — and for the
layer table's 64x64 `G_IM_FMT_I` images that alpha is the image's intensity:

```
layer 0 image 0x801D2F88  64x64 I8  mean 12.9/255 = 5.1%   peak 52/255 = 20.4%
layer 2 image 0x801D51E8  64x64 I8  mean 12.9/255 = 5.1%   peak 52/255 = 20.4%
```

so **`OGRE_FOG_SCALE=100` is the game's own value** (the asset as authored), and
that is the default. The knob only exists to stage a scaled copy in scratch RDRAM
for A/B runs.

That default is checked against a retail emulator capture of the lower half of
the screen (just after the fade-in), using the fog-free cloud band at N64 rows
129-175 to fit the emulator's brightness offset and then measuring the fog's
contribution in the clean cloud region below the copyright (rows 214-236):

| build | port fog contribution | retail's |
|---|---|---|
| `OGRE_FOG_SCALE=45` | +3.2 | +8.1 |
| `OGRE_FOG_SCALE=100` | **+7.2** | +8.1 |

The same fit reproduces zero fog in the control band (`±0.02`), so the method is
sound; six presents across the matching cloud phase give 112-116% for the 45%
build, i.e. the raw asset. (Runs whose clouds have scrolled out of phase collapse
to nonsense — 1860/1870 in the same sweep — which is why only the phase-matched
presents are quoted.)

The earlier 45% was a port-side over-correction from comparing overall
"brightness" rather than the fog's contribution; it is gone. `OGRE_FOG_SCALE` is
still there for taste (`45` reproduces the softer look).

### 3e. What is still approximate

The sweep is repaired to the *layer image* (a 64x64 fog image, wrapped), not to
the 320-wide cloud texture the game's `SETTIMG` inherits; a 320x64 RGBA32 texture
does not fit in 4 KB of TMEM, which is why the original uploads cannot be
replayed verbatim. The result is the right kind of moving haze in the right
place, and it lines up with the emulator captures above the `©1999 QUEST` line,
but the exact wisp shape is the layer image's, not the cloud texture's. If that
matters, the next step is to synthesise a per-rectangle `LOADTILE` for the
cloud-texture row each sweep rectangle names (each rectangle is preceded by two
`G_RDPPIPESYNC` no-ops, so there is room for one 8-byte command per rectangle).

## 4. Files changed (this session)

* `app/src/renderer.cpp` — the `OGRE_FOG` fixes (**the fix**): quarter-texel
  `G_SETTILESIZE`/`G_LOADTILE` extents; repairing every zero-area white group
  (so the bottom sweep is repaired too); wrapping the draw tile so the sweep's
  scroll past the image edge no longer blinks; `OGRE_FOG_SCALE` left at the
  game's own 100%; `OGRE_FOG_SUMMARY` and `OGRE_FOG_TRACE` per-group traces.
* `docs/proofs/native-title-fog-isolation.png` — title logo-band fog, off / on /
  difference ×4 (replaces session 35's story-screen crop of the same name).
* `docs/proofs/native-title-fog-quest-region.png` — the `©1999 QUEST` sweep, off /
  on / difference ×4.
* `docs/proofs/native-title-fog.png`, `native-title-fog-vs-band.png` — refreshed
  on the title with the repair actually drawing.
* `docs/proofs/native-title-band-isolation.png` — the zero-texel band fix, pre-fix
  white band / fixed / difference ×4.
* `app/src/bank_overlays.cpp` — `OGRE_SCENE_AFTER_MS` default 0, the poke retried
  until the scene is active, both id words written, the descriptor-deref SIGBUS
  fixed, `OGRE_SCENE_TRACE=1`, corrected `kScenes`.
* `app/src/bank_overlays.hpp` — comment updated to match.
* `docs/guides/app-build.md`, `docs/DECISIONS.md`, `PLAN.md`,
  `docs/HANDOFF-2026-09-14-session35.md` (correction note) — documentation.

## 5. Next

1. **Sessions 33/34's open work is unchanged**: make the bank function sets
   deterministic, finish the cross-bank dispatch, then scene `0x18`/`0x02`.
2. The sweep currently samples the layer image rather than the 320-wide cloud
   texture the game names (§3c). If the exact wisp shape matters, synthesise a
   per-rectangle `LOADTILE` for the cloud-texture row each sweep rectangle names.
3. `osViFade` is still unemulated (fades snap).
4. The bottom sweep (§9 of session 35) still needs the app to synthesise the
   missing per-strip uploads, if it is worth chasing now that the band is fixed.
