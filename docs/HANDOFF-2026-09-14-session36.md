# Session 36 — jumping straight into a scene, and what the title "fog" really is

Two things were reported at the start of this session:

1. `docs/proofs/native-title-fog-isolation.png` is **not** the title screen — it
   is a crop of the story/lore screen.
2. Testing would be easier if a run could **jump straight to the title screen**.

Both are addressed. Getting there turned up two more real bugs and one
correction to session 35's conclusions.

## 0. TL;DR

| question | answer |
|---|---|
| does `OGRE_SCENE` work? | it does **now**. It never did with its documented `OGRE_SCENE_AFTER_MS=3000` default: the poke has to land before the boot enters its first scene (~1.1 s). Default is now 0 |
| why did `OGRE_SCENE_LOG=1` crash? | `poll_scene` dereferenced the scene-descriptor word unconditionally; during boot it holds a stale non-KSEG0 value, so `descriptor + 0x10` walked off RDRAM → SIGBUS at ~1 s. Fixed |
| what is the title screen's scene id? | **`0x04`**, and only its later phase (after the prologue text) is the logo + PRESS START. The old `title` = `0x0C` is the unit-description book |
| what was `native-title-fog-isolation.png`? | a crop of the lore/story screen (`OGRE_FOG=alternate` pairs), not the title |
| is the `OGRE_FOG` repair visible? | **no measurable effect** on the title's logo band. `OGRE_FOG=1` and `OGRE_FOG=0` produce byte-identical frames once two runs are aligned; only `OGRE_EMPTY_TILE=draw` (the pre-fix behaviour) changes the band |
| replacement proof | `docs/proofs/native-title-band-isolation.png` — the title band, pre-fix white band / fixed / difference ×4 |

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
# fixed (guard only; OGRE_FOG=1 is byte-identical, see §3)
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

## 3. Correction: the `OGRE_FOG` repair is not visible

Session 35 §7/§10 concluded that the `OGRE_FOG` pass restores the title's faint
white fog, measured with `OGRE_FOG=alternate`. That measurement does not hold up:

* `OGRE_FOG=1` vs `OGRE_FOG=0`, same run parameters, compared at the same
  present index. At every index where a *third* run (`OGRE_EMPTY_TILE=draw`)
  proves the two runs are in lockstep (control-region diff `0.00`), the fog-on
  and fog-off frames are byte-identical **in the band as well**:

  | present | old-vs-on control | on-vs-off control | on-vs-off band | old-vs-on band |
  |---|---|---|---|---|
  | 1860 | 0.000 | 1.476 | 3.936 | 53.808 |
  | 1920 | 0.000 | 0.000 | **0.000** | 39.621 |
  | 1930 | 0.000 | 0.029 | **0.000** | 39.621 |
  | 1950 | 0.029 | 1.851 | 0.000 | 39.621 |
  | 2080 | 0.000 | 0.035 | **0.000** | 39.621 |
  | 2150 | 0.033 | 1.850 | 0.000 | 39.621 |

* `OGRE_FOG=alternate` on the static title produces **zero** band difference
  between consecutive presents: the game does not resubmit a changed display
  list while the title waits, so there is nothing for the toggle to change.
* Forcing the staged image to pure white (`OGRE_FOG_SCALE=100000`, which makes
  `I8ToFloat4` return alpha 1 and would paint an *opaque* band if the rectangles
  drew) leaves the title band at the same brightness as the default build.
* `OGRE_RECT_STATE=64-128` confirms the repair does reach RT64
  (`tile=0 fmt=4 siz=1 line=8 tmem=0 uls=0 ult=0 lrs=63 lrt=63`, combiner
  `RGB ONE / ALPHA TEXEL0`, blender `P=CC A=CC_A M=FB B=1-A`), so this is not
  "the rewrite never happened" — the repaired rectangle simply does not change
  the presented pixels.

What *is* observable is entirely RT64's empty-tile guard (plus the same guard in
the browser renderer): the fabricated one-texel tile paints an opaque white band
(`OGRE_EMPTY_TILE=draw`, band mean 172) and skipping the rectangle removes it
(band mean 119, clouds). The `OGRE_FOG` pass is kept (it is harmless and the
layer-table lookup it adds may still be needed for the bottom sweep), but it is
**not** currently doing anything visible, and the "fog" in the session-35 proofs
should be treated as unverified until someone shows the repaired rectangle
reaching the framebuffer.

## 4. Files changed (this session)

* `docs/proofs/native-title-band-isolation.png` — new title-screen band proof.
* `docs/proofs/native-title-fog-isolation.png` — **deleted** (wrong screen, and
  the thing it claimed to isolate is not measurable).
* `app/src/bank_overlays.cpp` — `OGRE_SCENE_AFTER_MS` default 0, the poke retried
  until the scene is active, both id words written, the descriptor-deref SIGBUS
  fixed, `OGRE_SCENE_TRACE=1`, corrected `kScenes`.
* `app/src/bank_overlays.hpp` — comment updated to match.
* `docs/guides/app-build.md`, `docs/DECISIONS.md`, `PLAN.md`,
  `docs/HANDOFF-2026-09-14-session35.md` (correction note) — documentation.

## 5. Next

1. **Sessions 33/34's open work is unchanged**: make the bank function sets
   deterministic, finish the cross-bank dispatch, then scene `0x18`/`0x02`.
2. Find out why the repaired fog rectangles do not reach the framebuffer. Start
   from `OGRE_RECT_STATE` (state is correct at `drawTexRect`) and follow the
   draw through the workload queue (`OGRE_WORKLOAD_TRACE=1`,
   `OGRE_FBRENDER_TRACE=1`) to see whether it is culled, overdrawn, or texture-
   decoded to alpha 0. `OGRE_FOG_SCALE=100000` is the fastest probe: if the band
   is still unchanged with an all-white staged image, the draw is lost after
   `drawTexRect`.
3. `osViFade` is still unemulated (fades snap).
4. The bottom sweep (§9 of session 35) still needs the app to synthesise the
   missing per-strip uploads, if it is worth chasing now that the band is fixed.
