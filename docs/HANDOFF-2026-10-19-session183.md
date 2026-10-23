# Session 183 handoff — the QUEST q meshes were failing the depth test against a stale depth image; the fix is verified with one variable and recorded

## Goal

Developer request: *"using the capture tool from the previous session, continue
working on fixing the quest logo"*. The tool is `tools/intro_capture.py`
(session 182). The open item is `PLAN.md` 19: the browser presents the QUEST
still's white plate and wordmark, and the three q meshes appear only in some
runs.

## Result

The three q meshes render, and the presented frame matches native. The cause is
the depth image's contents, not the batch/present boundary that session 181
named. Scene `0x0A` uses guest address `0x2E6840` as both the stills' plate
colour image and their depth image. The q draws are depth-tested and
depth-written: their `OTHERMODE_L` is `0x00552078` and
`cmd.depth_test = depth_on && !copy && (oml & 0x10)` is true. The plate's colour
fills write `0x2E6840`, so the depth image's contents are overwritten; the
renderer kept its depth renderbuffer, and every q fragment compared against that
stale contents. The renderer now mirrors RT64's `depthFbTypeChanged`
(`tools/RT64/src/hle/rt64_state.cpp:1581`, `:1630`): when a colour draw targets
an address that a display list has named with `G_SETZIMG`, the depth target is
queued for a clear, and the clear runs with the scissor disabled.

The change was already in the working tree when this session started, with a
comment naming an unrecorded "session 181b",
`docs/proofs/web-publisher-quest-q-meshes.png` and no handoff. This session measured it with one variable, rebuilt the clean
tree, and writes it into the record.

### 1. The A/B: one build, one environment variable

The invalidation was gated on `OGRE_PROBE183_NOINVAL` for the measurement only.
Both runs used the same wasm build, forced scene `0x0A`
(`?OGRE_SCENE=publishers`), 30 s at 1 fps, and the same probe command:

```sh
node debug/probes/intro-capture.cjs --seconds 30 --out /tmp/p183-off \
  --url "http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=publishers&OGRE_PROBE183_NOINVAL=1"
node debug/probes/intro-capture.cjs --seconds 30 --out /tmp/p183-on \
  --url "http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=publishers"
```

| run | frames 24-25 (160x120 sample) | picture |
|---|---|---|
| invalidation off | `white=18797, colourful=0` | plate + wordmark, no q colour (`docs/proofs/web-publisher-quest-no-depth-invalidation.png`) |
| invalidation on | `white=17489, colourful=1309` | plate + three q meshes + wordmark (`docs/proofs/web-publisher-quest-q-meshes.png`) |

`white=18797, colourful=0` is the frame session 181 measured at normal speed
(`white=296537` of 303525 full-resolution pixels, `dark=2192`, `coloured=0`).
The two runs differ by `18797 - 17489 = 1308` white samples, and the enabled run
counts `1309` colourful samples: the q fragments cover exactly the plate pixels
that the off run counts as white. The batch content is the same in both runs;
only the depth clear differs, so the batch-boundary hypothesis is withdrawn.

The black-plate q animation differs in the same way: off, frames 21-23 measure
`nonBlack=181/0/0`; on, `nonBlack=507/544/942` with `colourful=498/242/318`.
Off, the q fragments are dropped in every frame, not only in the white hold.

### 2. The dense series: every 100 ms of the hold

`tools/intro_capture.py` gained `--interval-ms`. Both renderers were captured at
100 ms for 30 s from a natural boot, so the whole still sequence is measured and
not one sample of it:

| series | frames | QUEST hold | values per 100 ms sample |
|---|---|---|---|
| native | 300/300 | `t=23.22..25.08` | `white=17519, colourful=1281` |
| browser (clean build) | 299/300, 1 missed | `t=23.81..25.33` | `white=17489, colourful=1309` |

Every sample inside the hold carries both the plate and the q colour, and the
values are constant to the last digit except the two first and the fading last
frame (`17490/17492`, `17402/1304`). The frames before the hold are the q
animation on the black plate: native reaches `colourful=1274` at `t=22.68`, the
browser `1305..1322` at `t=23.30..23.50`. No 100 ms sample presents the plate
without the q colour on the current tree.

The settled hold was compared with native directly: browser
`debug/out/intro-final2/web/frame-24.png` (639x475) against native
`debug/out/intro-dense/native/frame-200.png` (640x480) at offset `(0,0)`
measures a mean absolute difference of `0.317` per channel over the 639x475
overlap. The first held native frame (194, `0.3 s` earlier) differs by `3.943`,
because the q meshes are still settling there.

### 3. Correction of sessions 180 and 181

- Session 180 and session 181 list `depth_test=0 depth_write=0` among the q
  draw's properties. The draw's own `OTHERMODE_L` is `0x00552078`, and
  `record_state` (`app/src/web_renderer.cpp:6631`) derives
  `depth_test = depth_on && !copy && (oml & 0x10) != 0 && z_mode != 0xC00`, which
  is true for that word. The q draws are depth-tested. The depth contents decide
  whether they land, which is what the A/B above shows.
- Session 181's next experiment (give `DrawCmd` a display-list id and compare a
  one-list flush with a two-list flush) is not needed for this defect. The
  difference between the failing and the reference frame was the depth
  invalidation, and it reproduces with the same batching.

The still shares `0x2E6840` between a colour image and a depth image, which is
the case RT64 handles with `everUsedAsDepth`/`depthFbTypeChanged`. A colour write
to a depth image's address invalidates the depth target.

### 4. The capture tool's interval

`tools/intro_capture.py --interval-ms <n>` (minimum 20, default 1000) passes
`OGRE_CAPTURE_EVERY_MS=<n>` to RT64 and `--interval-ms` to the browser probe,
which schedules its captures on an absolute `n` ms grid. The frame count is
`seconds * 1000 / interval-ms`; the report prints milliseconds with three
decimals, and a series of more than 60 frames is laid out in 12 columns. The
browser probe's frame naming and manifest carry the same value. This is what
made the 0.4-1.8 s QUEST hold measurable instead of sampled once.

## 5. The rotation phase is still not 1:1 (open)

The developer's follow-up: *"in the web version, the q's have no texture, they are
solid yellow while rotating. but in the native version, they 'reflect' light."*
The settled hold matches native, so the difference is in the spin, and it is
real: at the same animation moment the lit q is a different one.

- The browser does draw both q state families. A combiner census of the q
  triangles into `0x250800` reads `FCFFFFFF/FFFCF87C` (evaluated mux `TEXEL0`,
  PRIM 0) and `FC121824/FF33FFFF` (`TEXEL0*SHADE`, PRIM 0.98, SHADE 0.5). The
  second is the lit family.
- The browser does produce the bright highlight. `web/frame-228.png` holds 6277
  pixels above 200 (peak `(248,231,190)`), native's `frame-188.png` 6546 (peak
  `(248,240,220)`).
- The animation clocks differ. The q's first appear at capture `t=20.12` native
  and `t=20.51` web; the plate at `23.22` and `23.81`. The offset grows, so the
  browser's game clock advances more slowly in wall time.
- At the same animation time the lit q is in a different place. Browser
  `frame-229` (t=22.91) has the bright q in the middle; native `frame-188`
  (t=22.55, the same offset-adjusted moment) has the bright q on the right and
  the middle q edge-on. The silhouettes are close (mask IoU 0.985 for the
  229/189 pair) but the highlight is attached to a different q.
- Every 32x32 q texture upload observed decodes from the same RDRAM address
  (`0x000400`) with identical statistics (`958/1024` nonzero, mean channel sum
  `217`), while each upload gets a new key from `tmem_generation_`. That is the
  game re-loading one image per sub-list, and it is the lead to check against
  native's per-sub-list `G_SETTIMG`.

Proof frames: `docs/proofs/web-quest-rotation.png` (browser `frame-229`) and
`docs/proofs/native-quest-rotation.png` (native `frame-188`).

The probes that measured this were removed and the tree rebuilt:
a per-q-triangle state line (texture key, tile origin, uv, position, shade,
combiner), a 32x32 texture-upload line (key, source address, first texel,
nonzero count, mean), and a combiner census. `grep -rn "probe184" app/ debug/`
is empty and `build-wasm/ogrebattle64.wasm` is back to 21129871 bytes.

Next measurement: render the same q sub-list in both and compare the state, or
capture both at 20 ms through the spin and match poses by the q silhouette, then
compare the texture key and combiner of the triangle that wins the depth test.

A second round of probes narrowed it to the shading and the texture content:

- The visible q triangle (`centroid (0.129, 0.414)`, `uv0=(42.00, 56.00)`, the
  same vertex session 180 recorded) is drawn with the lit combiner
  `FC121824/FF33FFFF` and PRIM 0.98. The earlier "all unlit" census counted
  off-screen geometry at the same target; filtering by centroid removed it.
- All three vertices of that triangle carry `shade=(0.50,0.50,0.50)`. The
  permanent `[GFX-LIGHT]` census shows the stills' light is white
  (`col 255 255 255`, `dir 0 0 127`) with ambient 128, so 0.50 is ambient only
  for a normal of `(0,0,-1)`. Native's bright rims are the triangles whose
  normals catch the direction, so the question is whether the browser's
  transformed light direction matches RT64's for these draws.
- Every 32x32 q texture upload in that run decodes from `0x000400` to the same
  vertical brown gradient (`[0]=161,138,94` .. `[31]=103,74,60`, `1024/1024`
  nonzero), so the spin's q draws sample one low-contrast image rather than the
  three hue ramps of the held frame. Whether the game intends that during the
  spin is not yet established.
- `[GFX-LIGHT]`'s late lines for the stills read `lights=1`, light
  `255 255 255` with `dir 0 0 127`, ambient `128 128 128`, and shade `1.000`
  only for normals of `(0,0,1)`; normals of `(0,0,±1)` with a transformed
  direction of zero z give 0.502.

So the q's are textured and lit, but the spin's flat faces are ambient-only and
the sampled image is a brown gradient. The next comparison is per-vertex shade
for one q sub-list against RT64's formula for the same list (light direction,
modelview, normals), plus the DL's per-sub-list `G_SETTIMG` addresses against
the browser's `t_image_address`.

## Verification

- `EM_CACHE=<repo>/build-wasm/emcache make web` for the A/B build and again after
  the probe variable was removed; both clean.
- The A/B above: two forced-`publishers` browser runs on one build, one variable.
- `python3 tools/intro_capture.py --mode web --seconds 30` on the clean build:
  30/30 frames, QUEST at frames 24-25 `white=17489, colourful=1309`. Repeated
  after the final `make web` (a comment-only change): the same two frames.
- The synthetic controls on the final build: `testdraw.cjs --flags 96
  --expect-fbtex --expect-hits 1` -> `fbtex hits=1 lines=1`; `--flags 128
  --expect-depth near` -> `rgba=(255,0,0,0)`; `--flags 384 --expect-depth far`
  -> `rgba=(0,0,255,0)`.
- `python3 tools/intro_capture.py --mode both --seconds 30 --interval-ms 100`
  for the native dense series, and `--mode web` for the browser one; results in
  section 2.
- `grep -rn "probe183\|OGRE_PROBE183" app/ debug/` is empty after the revert.
- Native was not changed. `tools/RT64` was not changed by this session.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | the depth-image invalidation and the scissor-free depth clear (already in the tree at session start; measured and recorded here) |
| `tools/intro_capture.py` | `--interval-ms`, frame count, 3-decimal report, 12-column contact sheet for dense series |
| `debug/probes/intro-capture.cjs` | `--interval-ms` and the absolute `n` ms schedule |
| `docs/proofs/web-publisher-quest-no-depth-invalidation.png` | the A/B frame with the invalidation off |
| `docs/proofs/web-publisher-quest-q-meshes.png` | the reference frame (already in the tree) |
| `docs/HANDOFF-2026-10-19-session183.md` | this file |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/scenes.md` | the fix, its measurement and the tool option |
| `docs/guides/app-build.md`, `docs/guides/web-probes.md`, `debug/README.md` | the `--interval-ms` option and the dense series |

## Probes used and reverted

One temporary A/B gate in `app/src/web_renderer.cpp`, tagged `probe183` and read
once per draw from `OGRE_PROBE183_NOINVAL`. It was removed, `make web` was run
again, and `grep -rn "probe183" app/ debug/` is empty. No other probe was added.
