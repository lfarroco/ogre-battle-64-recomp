# Session 175 handoff — browser WebGL2 renderer close-out: quiet-machine smoke, a fresh title pair, the S2DEX2 opcode census, and the residual list

## Goal

Final verification round for the browser WebGL2 renderer, with no new features:
re-run the smoke battery on an idle machine, finish the two small diagnostics (the
S2DEX2 opcode census, the `G_CD` decision), re-verify the screens session 174 left
open (publisher `0x0A`, title/attract, the mission `0x03`), and record whether the
browser renderer is complete. A screen or comparison that could not be reached is
recorded as **not verified**.

Timebox: 100 minutes. The round started at load average 5.2 (1 min) / 18.4
(5 min) / 19.9 (15 min) and was quiet from about 10 minutes in.

## Renderer completeness

The browser renderer implements the RDP state the reached screens exercise. The
list below is the implemented feature set, with the session that landed it and the
measurement that verifies it.

| feature | evidence |
|---|---|
| WebGL2 context, VI-sized drawing buffer, canvas = the VI's visible area, 1:1 present | `[GFX-PRESENT] canvas resized to 319x239 (css 638x478, x2)`; publisher still was `[0.0,0.0,0.0]`/100% over 639x475 (session 160); title/mission canvases are 319x239 |
| RDRAM framebuffer indirection, writeback only on `ogre_sync_framebuffers` | `[GFX-FBREAD]`/`[GFX-FBSYNC]` absent on every screen this round and in session 174's logs; the fps/`[GFX-PRESENT]` path samples the GL colour image |
| TMEM (4 KiB, RT64's load layout) + tile decode + YUV16 | tutorial box `0.25/0.00/0.18` by band (session 174) |
| RT64 `TextureSampler.hlsli` port (clamp/mask/mirror, point/bilerp/average, 3-point, 1/128 quantisation, `ShiftScaleMap`, `G_TP` from bit 19) | `[GFX-SAMPLER]`/`[GFX-TP]` censuses; mission A/B identical with the shift on and off (sessions 164/174) |
| alpha compare (`G_AC_THRESHOLD`/`G_AC_DITHER`), `CVG_X_ALPHA`/`CVG_DST` coverage | tutorial box glyph cells exact after session 167 |
| combiner (2-cycle, key centre/scale, K4/K5, alpha dither inputs) | `[GFX-CMD]` mux decode; `[GFX-KEY]` never fires on a reached screen |
| blender (`P/M/A/B` both cycles), fog colour, `G_FOG` per-vertex alpha | mission frame `7.32` whole (this round) |
| lighting (`G_LIGHTING`, 7 lights + ambient, Vtx normal), backface culling | mission `15.71 -> 4.69` (session 171); `[GFX-LIGHT]` census |
| depth: `G_SETZIMG`/`G_SETPRIMDEPTH`, RT64 screen-z, `LESS`/`ALWAYS`, target keyed by the depth image | `testdraw --flags 128 --expect-depth near` -> `rgba=(255,0,0,0)`; `--flags 384 --expect-depth far` -> `rgba=(0,0,255,0)` (this round); mission on/off pairs bit-identical (session 173) |
| S2DEX2: `0x01`/`0x02`/`0xDA` object rectangles, `0xDC` object matrix, njpeg YUV16 backdrop | njpeg cathedral assembled (session 139); the new `[GFX-S2D-CENSUS]` is unverified this round (see Result 4) |
| half-pixel triangle offset | tutorial box `5.88 -> 0.22` (session 167) |
| high-precision colour attachment (`GL_RGBA16F`) | 0.009 mean against the RGBA8 attachment (session 163) |
| the `OGRE_FOG` display-list repair (browser port) | title/story pair `0.24/0.25/0.28` (this round, story beat) |

Verified screens with the numbers this round measured:

| screen | measurement (browser vs native) |
|---|---|
| boot smoke | `boot.cjs --attempts 3 --secs 40` -> `RENDERED` on attempt 1, `nonBlack=24452 colorful=10140` |
| synthetic controls | `--flags 96` `fbtex hits=1 lines=1`; `--flags 128` reads the near (red) triangle, `--flags 384` the far (blue) one |
| title `0x04`, attract story beat | match to native `t.640` `meanAbs=[0.24,0.25,0.28]`, `exact=85.1%` over 319x239 |
| title `0x04`, logo beat | match to native `t.1080` `meanAbs=[1.88,1.96,2.23]`, `exact=72.1%` over 319x239; clouds `0.07`/`0.08`, logo band `5.32`, subtitle `1.65`, copyright band `8.49` |
| mission `0x03` | `nonBlack=303208`, `colorful=294108`; vs native `t.800` whole frame `meanAbs R=7.88 G=7.18 B=6.91`, `exact=0.3481`; terrain top cliff `10.30`, left grass `7.57`, right terrain `4.62`, bottom middle `9.48` |
| publisher `0x0A` | **not verified**: no presented still (Result 2) |
| S2DEX2 opcode census | **not exercised**: no S2DEX2 list on a reached screen (Result 4) |

Residuals:

1. **`G_CD` colour dither at the writeback is not implemented** (Result 5).
2. **No frame-locked browser/native pair**: the attract beat and the mission intro
   phase both move, so the whole-frame numbers compare two moments of one screen.
   The title pair is the closest to frame-locked (the story beat's text is
   identical in both, `0.24-0.28`); the mission's `7.32` is not.
3. **The Load Game white-frame difference** (session 174 open item) still stands:
   the Load Game screen is drawn white in the browser before its content arrives.
4. **Unexercised features**, by measurement, not by inspection: `G_CD` dither,
   `ZMODE_DEC`, the `G_AC_DITHER` hash, `G_TEXGEN`, `G_SHADE`, the depth image's
   RDRAM transfer, `a_w` in `gl_Position`, `G_OBJ_SPRITE`'s full-matrix path, the
   S2DEX2 `0x05`/`0x06`/`0x07`/`0x09`/`0x0A`/`0x0B` commands, `OGRE_RAM_KEY_AT_MS`
   in a browser, and the deployed GitHub Pages site by hand.
5. **The publisher stills `0x0A` do not present** (Result 2); the forced scene now
   advances 30 display lists but every captured frame is black or the unpresented
   canvas.
6. **The title logo/copyright sprites differ** (Result 3): `5.32` and `8.49` mean
   abs on those two bands while the clouds are `0.07`. No mechanism is named.

## Result

### 1. Quiet-machine smoke passes

Load average at the start of this block: 5.2 (1 min), 18.4 (5 min), 19.9
(15 min), the highest one-minute figure about 5.6.

```
node debug/probes/boot.cjs --attempts 3 --secs 40 --out /tmp/w175/boot
attempt 1: sawRealDl=false rendered=true
RENDERED {"w":638,"h":478,"total":304964,"nonBlack":24452,"colorful":10140,
          "top":["#000x291428","#100x4408","#110x3748","#210x1592","#010x852","#111x808"]}
EXIT=0
```

```
node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1
test_draw exported= true returned= 1 flags= 96
canvas pixels: {"w":640,"h":480,"total":307200,"nonBlack":0,"colorful":0,"top":["#000x307200"]} after-batch nonBlack=3024
fbtex hits=1 lines=1
  [GFX-FBTEX] load tex=0xC02000 rect=32x32 stride=640 fmt=0 siz=2 reads colour image 0xC02000 pitch=640 h=240 siz=2 rows=[0,31] drawn rows=[0,239] cols=[0,64) drawn cols=[0,128) by an earlier draw in this batch; the load read RDRAM, not the rendered pixels
EXIT=0
```

```
node debug/probes/testdraw.cjs --flags 128 --expect-depth near
depth control: expected near to win, read near rgb=(255,0,0)
  [GFX-DEPTH-TEST] target=0xC4E000 320x240 pixel=(64,96) rgba=(255,0,0,0) zcmp=1 zupd=1 zmode=0x000
EXIT=0
node debug/probes/testdraw.cjs --flags 384 --expect-depth far
depth control: expected far to win, read far rgb=(0,0,255)
  [GFX-DEPTH-TEST] target=0xC4E000 320x240 pixel=(64,96) rgba=(0,0,255,0) zcmp=0 zupd=1 zmode=0x000
EXIT=0
```

Session 174's `NO RENDER` at load average 10-12 is environmental: the same build
renders on attempt 1 on a quiet machine. Full log `/tmp/w175/battery.log`.

### 2. Publisher `0x0A`: advances now, but no still presents — **not verified**

The natural boot does not reach the scene in the browser: `shots.cjs` with no
`OGRE_SCENE` and `--secs 6` reported `tasks=0` on three attempts, and the boot's
own log ends at `[scene] t=1066ms id=0x0000` with only the boot blanking display
list. The forced entry advances:

```
node debug/probes/shots.cjs --url '...?OGRE_SCENE=publishers&OGRE_SCENE_LOG=1' --attempts 3 --secs 45 --shots 6 --gap 1500
shot 0: tasks=11 nonBlack=29707
shot 1: tasks=24 nonBlack=59340
shot 2: tasks=30 nonBlack=0
shot 3: tasks=30 nonBlack=0
shot 4: tasks=30 nonBlack=0
shot 5: tasks=30 nonBlack=0
CAPTURED 6 frames (attempt 1)
```

`/tmp/w175/pub-forced-gfxstats.txt` reads `tasks=4 dls=34 cmds=3234`, `texrect=136
fillrect=96`, `triangles=0`, `othermode_h: 7 unique values`. So the scene submits
2D work (a 640x480 target, 136 texrects) and 30 display lists arrive, which
session 174 did not see. The canvas does not carry the still: shots 2..5 are
`nonBlack=0` (black), and shot 1 is the unpresented WebGL canvas (the noise
pattern session 174 described, `/tmp/w175/pub-forced-01.png`). No frame with a
publisher logo was captured, so the still is **not compared** with
`docs/proofs/native-licensed-screen.png`, `native-atlus-screen.png` or
`native-quest-screen.png`, and the earlier exact measurement (meanAbs 0.0 / 100%
over 639x475) is not re-confirmed this round.

### 3. Title/menu: a fresh native pair, the story beat matches at 0.24-0.28, the logo beat at 1.88-2.23

The stored `/tmp/w162/nat-fog` series is the attract **backdrop** (clouds) and the
story overlay, not the logo screen, so session 174 was right that it cannot
verify a browser menu frame. This round captured a fresh native forced-title
series of the same scene as the browser's:

```
OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title \
OGRE_CAPTURE_TARGET=/tmp/w175/nat-title60/t OGRE_CAPTURE_AFTER=0 \
OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=60000 ./build-dist/ogrebattle64 assets/ogre64.z64
```

80 targets, 320x240, presents 0..1600. The series passes through the same beats the
browser shows: clouds (`t.10`), the story overlay (`t.350`: "Long ago, there was a
time when strength ruled and evil held the…"), the logo with "PRESS START / 1999
Nintendo / 1999 QUEST / Licensed to and published by ATLUS USA" (`t.1600`), and a
logo beat without the PRESS START line (`t.1080`).

**The forced entry is not deterministic** in the browser: three `?OGRE_SCENE=title`
runs this round landed on the logo (twice) and on the story overlay (once). That
is the reason session 174 could not reproduce the 0.11-0.20 number, and the fresh
pair makes both beats measurable.

| browser frame | native target | meanAbs (R,G,B) | exact |
|---|---|---|---|
| `/tmp/w175/post-title-02.png` (story overlay) | `nat-title60/t.640.bin` | **0.24, 0.25, 0.28** | 85.1% |
| `/tmp/w175/pre-title-02.png` (logo) | `nat-title60/t.1080.bin` | **1.88, 1.96, 2.23** | 72.1% |

`tools/web_frame_cmp.py ... --buffer 319x239`. The story pair reproduces the
0.11-0.20 measurement with a same-scene native reference and a lower number; the
logo pair is new and is the screen the browser's forced title lands on most often.
The four frames side by side (browser story / native `t.640` above, browser logo /
native `t.1080` below, 2x) are
`docs/proofs/web-title-forced-pair-175.png`.

The logo pair's residual is localised: clouds rows 0..50 `0.07` (98.3% exact) and
rows 150..194 `0.08`, the logo band rows 55..110 `5.32`, the subtitle rows
112..126 `1.65`, the copyright band rows 196..222 `8.49` (33.5% exact). A ±1
pixel shift search puts the minimum at `(0,0)` for both the logo (`5.32` against
`13.3-18.0`) and the copyright band (`8.49` against `28.8-45.0`), so the
difference is not a placement offset. The sprite content or alpha is the
difference, and no mechanism is named this round. Amplified difference:
`/tmp/w175/title-logo-diff.png`.

### 4. S2DEX2 opcode census: added, permanent, and **not exercised**

`app/src/web_renderer.cpp` now counts every opcode byte of every S2DEX2 list and
prints `[GFX-S2D-CENSUS] task N: cmds=M 01=.. 02=.. 04=.. 05=.. 06=.. 07=.. 08=..
09=.. 0A=.. 0B=.. DA=.. DC=.. E4=.. others=[..] undecoded=[..]`, at the end of the
first S2DEX2 task and every 200 S2DEX2 commands after it. `undecoded=` lists every
seen opcode the S2DEX2 arm does not decode, so a screen states its own gap.

The opcode meanings are from `tools/RT64/src/gbi/rt64_gbi_s2dex2.h`:
`0x01` `G_OBJ_RECTANGLE`, `0x02` `G_OBJ_SPRITE`, `0x04` `G_SELECT_DL`, `0x05`
`G_OBJ_LOADTXTR`, `0x06` `G_OBJ_LDTX_SPRITE`, `0x07` `G_OBJ_LDTX_RECT`, `0x08`
`G_OBJ_LDTX_RECT_R`, `0x09` `G_BG_1CYC`, `0x0A` `G_BG_COPY`, `0x0B`
`G_OBJ_RENDERMODE`, `0xDA` `G_OBJ_RECTANGLE_R`, `0xDC` `G_OBJ_MOVEMEM`, `0xE4`
`G_RDPHALF_0`. The renderer decodes `0x01`/`0x02`/`0xDA`/`0xDC`; `0xE4` is
reported once and not decoded. **`0x05`/`0x06`/`0x07` are the dangerous ones: they
fall through to the F3DEX2 switch's `OP_TRI1`/`OP_TRI2`/`OP_QUAD` arms
(`web_renderer.cpp` `exec_command`), which draw triangles from a vertex buffer an
S2DEX2 list never fills, so such a byte would draw garbage rather than nothing.**
`0x09`/`0x0A`/`0x0B` have no arm and draw nothing.

**No reached screen submitted an S2DEX2 list this round**, so no census line was
printed: the title, mission and publisher runs log zero `GFX-S2DEX`/`GFX-S2D-CENSUS`
lines, and the publisher's own statistics are `triangles=0` with 136 texrects, i.e.
F3DEX2. The njpeg backdrop path (scene `0x02`, the cathedral) is the screen that
would print it, and the forced route to it did not run:

```
node debug/probes/shots.cjs --url '...?OGRE_SCENE=new-game&OGRE_SCENE_LOG=1' --attempts 2 --secs 60
attempt 1: idle trajectory (tasks=1)
attempt 2: idle trajectory (tasks=1)
NEVER ADVANCED
```

So the census is implemented and compiled but **unverified**: the claim that the
njpeg lists use only `0xDA`/`0xDC` still rests on session 51's `OGRE_S2D_TRACE`
trace, not on this census, and this round did not observe a single S2DEX2 opcode
byte. A run that reaches the cathedral with the census on is the whole of the
remaining work for this item.

The change adds counters and one log line and touches no dispatch: on a non-S2DEX2
list the new code is one comparison against a `GbiKind` that is already in the
execution state, and the title/mission runs of this round report zero S2DEX2
commands. No frame on those screens can move, which is why no before/after pair is
reported for them; an S2DEX2 screen would have needed the census to be flag-gated
to A/B it, and that control does not exist.

### 5. `G_CD` colour dither at the writeback: not implemented, with numbers

**The game does enable colour dither.** The `[GFX-TP]` family census from session
174 prints each family's OTHERMODE_H, and `G_CD` is bits 6-7
(`tools/RT64/src/shared/rt64_f3d_defines.h:22-26`, `G_MDSFT_RGBDITHER` 6). Reading
the census values:

| screen | families by `G_CD` |
|---|---|
| title `0x04` | MAGICSQ 1 |
| map `0x05` | MAGICSQ 2 |
| mission `0x03` | MAGICSQ 9, BAYER 3, DISABLE 7 |
| attract story `0x0B` | MAGICSQ 3 |
| tutorial `0x17` | BAYER 1 |

**RT64 dithers only in the native-format writeback.** `FramebufferPair::addGameCall`
counts the dither mode of each game call (`hle/rt64_framebuffer_pair.cpp:38-44`) and
`Framebuffer::getDominantDitherPattern` picks the majority mode
(`hle/rt64_framebuffer.cpp:225-232`), which `rt64_state.cpp:557` attaches to the
colour framebuffer. The pattern is applied when the render target is copied to
RDRAM: `src/shaders/FbWriteColorCS.hlsl:19-21` (`ditherValue = DitherPatternValue(...)`,
`Float4ToUINT(color, siz, fmt, oddColumn, ditherValue, usesHDR)`) and
`src/shaders/FbReinterpretCS.hlsl:20-23` (`Float4ToRGBA16(inputColor, ditherValue,
usesHDR)`), whose `DitherPatternValue` adds `0..7` to the 8-bit value before `>>3`
(`src/shaders/Formats.hlsli:27-38`, `:100-112`). The presented frame does not
include it: RT64's presenter samples the GPU render target, and the browser's
`present_framebuffer` samples its GL colour image (`[GFX-PRESENT] ... fbo=1`).

The browser's writeback already implements the same conversion with dither 0
(`float4_to_rgba16`, `web_renderer.cpp`, RT64's `Float4ToRGBA16` with the
`usesHDR` coverage rule), so the only difference a `G_CD` dither could produce is
in the RDRAM bytes, and those are read back only when the game calls
`ogre_sync_framebuffers()`.

**No screen measured this round reads a framebuffer back.** `[GFX-FBREAD]` and
`[GFX-FBSYNC]` are absent from the mission page log
(`/tmp/w175/mission-a1-page.txt`: 0 occurrences of either) and from every
session-174 run (`/tmp/w174/*page.txt`, `*-milestones.txt`: 0). The only browser
run with readbacks in `/tmp/w17*` is `/tmp/w170/m1-a1-page.txt` (16 `[GFX-FBREAD]`
lines on the pre-session-170 build, which still wrote every framebuffer back on
every present), which is the behaviour session 170 removed. The one screen whose
content is built from a CPU readback is the njpeg backdrop assembly in scene
`0x02`, and its presented result in a browser was not reached this round.

So: **not implemented, because the presented canvas reads the GL colour image and
no reached screen reads RDRAM back, so an implementation could not be verified to
change a frame.** The evidence that it is a readback-path difference is the pair of
facts above: RT64's dither is inside its writeback shader only, and the browser's
writeback runs only for the game's own sync request, which no reached screen makes.
A future session that reaches the cathedral (scene `0x02`) can implement it and
measure: the pattern is MAGICSQ for the mission/title/map families, so the writeback
would change the 5-bit value on the pixels whose 8-bit value is within 7 of a
5-bit boundary, up to one 5-bit level (8 levels when expanded to 8 bits).

### 6. Mission `0x03` on the final build

Route `/tmp/w173/drive173.cjs` with
`?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
`--shots 3 --gap 3000 --pre-delay 22000`, one attempt:

```
[scene] t=19.8s scene=0004 -> t=42.7s scene=0012 -> t=81.1s scene=0003
MISSION 0x03 active at t=91.3s of attempt 1
shot 0: tasks=36 nonBlack=303208 colorful=294108
shot 1: tasks=36 nonBlack=303208 colorful=293872
shot 2: tasks=36 nonBlack=303208 colorful=293872
REACHED 0x03   (exit 0)
```

Against native `/tmp/w168/nat2/t.800`, aligned per `/tmp/w173/proof173.py`
(319x239 drawing buffer, 320x240 target):

| region | this build |
|---|---|
| whole frame `meanAbs` R/G/B | **7.88 / 7.18 / 6.91** (whole-frame mean `7.32`), `meanSigned +3.36/+3.79/-3.42`, `exact=0.3481` |
| top cliff rows 0..38 | 10.30 |
| left grass rows 190..238 x 0..90 | 7.57 |
| right terrain rows 95..150 x 285..319 | 4.62 |
| bottom middle rows 200..238 x 100..230 | 9.48 |

The whole-frame figure is inside session 174's `7.07-7.58`/`0.3489-0.3633`, so the
build is unchanged on this screen. The terrain regions are not comparable to
session 171's `0.93 / 7.87 / 2.26 / 5.24`: rows 0..38 contain the "Castle Burgund"
portrait box, the note box's slide-in and the scrolling `MISSION` text, and rows
200..238 contain the portrait, so both bands carry the intro phase.

**Same-screen caveat.** The browser frame and `t.800` are different moments of the
mission intro (the portrait box sits differently and the camera distance differs),
so a whole-frame number is a measurement of "the same screen, two moments". A
frame-locked pair needs a deterministic tick both renderers can start from. This
round did not produce one; the terrain-only rows that the phase does not touch are
the only like-for-like measure, and this round's are the four above.

## Evidence

- Smoke battery: `/tmp/w175/boot.log`, `/tmp/w175/battery.log`,
  `/tmp/w175/f96-*`, `/tmp/w175/f128-*`, `/tmp/w175/f384-*`.
- Publisher: `/tmp/w175/pub-natural-*` (never advanced), `/tmp/w175/pub-forced-*`
  (30 display lists, black canvas), `/tmp/w175/pub-forced-01.png` (unpresented
  canvas).
- Title: `docs/proofs/web-title-forced-pair-175.png` (the four frames),
  `/tmp/w175/nat-title60/` (80 native targets, `nat-title60.log`),
  `/tmp/w175/nat-title/` (37 targets, 15 s), `/tmp/w175/pre-title-0{0..4}.png`,
  `/tmp/w175/post-title-0{0..4}.png`, `/tmp/w175/title-logo-diff.png`.
  Comparators: `tools/web_frame_cmp.py --buffer 319x239` and
  `tools/rt64_target.py`.
- Mission: `docs/proofs/web-mission-session175.png` (frame 0),
  `/tmp/w175/mission-a1-{00,01,02}.png`, `/tmp/w175/mission-a1-page.txt`,
  comparator `/tmp/w173/proof173.py`'s region set with the whole-frame `compare()`.
- `G_CD` census values: `[GFX-TP]` lines in `/tmp/w174/*-milestones.txt` and
  `/tmp/w174/mission-a1-page.txt`, decoded with bits 6-7.

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `[GFX-S2D-CENSUS]`: `s2d_op_counts_[256]`/`s2d_total_`/`s2d_first_printed_`/`s2d_next_print_`, the count in `exec_command`'s S2DEX2 arm, the first-task print at the end of `execute_dl`, and `print_s2d_census` |
| `docs/HANDOFF-2026-10-18-session175.md` | this file |
| `PLAN.md` | the browser section's completeness statement and the residual list |
| `docs/STATUS-LOG.md` | session 175 entry |
| `docs/README.md` | session 175 row |
| `docs/DECISIONS.md` | decision 175 (the `G_CD` decision and the census) |

No commit was made. No temporary probe was added, so no revert is needed: the new
`[GFX-S2D-CENSUS]` line is a permanent diagnostic of the kind this renderer
already carries, and `grep -rn "probe175" app/ debug/ RecompiledFuncs/ Bank*Funcs/`
is empty. `make web` was rebuilt once with the change and the wasm is current.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` (one incremental build with
  the census).
- `node debug/probes/boot.cjs --attempts 3 --secs 40`
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`
- `node debug/probes/testdraw.cjs --flags 128 --expect-depth near`
- `node debug/probes/testdraw.cjs --flags 384 --expect-depth far`
- `node debug/probes/shots.cjs` forced title (pre-build series, post-build
  series), forced `publishers`, forced `new-game`, natural boot at `--secs 6`.
- `./build-dist/ogrebattle64` forced title, 15 s and 60 s, `OGRE_NO_AUDIO=1
  OGRE_SPEED=1`.
- `/tmp/w173/drive173.cjs` for the mission route.

### What was not verified

- The publisher stills `0x0A` (Result 2).
- The S2DEX2 opcode census (Result 4): no reached screen submitted an S2DEX2
  list, and the forced `new-game` route never advanced (`tasks=1`).
- A frame-locked mission pair (Result 6).
- The title logo/copyright residual's mechanism (Result 3).
- The tutorial `0x17` and world map `0x05` frames were not re-captured this round;
  their last measurements are session 174's (`0.25/0.00/0.18` by band and
  `nonBlack=252820` with fade gain 1.927).

## Open

1. **Why the publisher `0x0A` scene's 30 display lists present as black.** The
   2D work arrives (`texrect=136 fillrect=96`), so the open question is which
   target the still draws into and which address the VI scans out at that moment.
   `[GFX-PRESENT]` for that run would name it; the run's page log is
   `/tmp/w175/pub-forced-*`.
2. **The title logo and copyright bands (`5.32`, `8.49`) against exact clouds.**
   The sprites differ in content or alpha, not in position. A per-draw comparison
   of those texrects (image, `line`, LUT mode, alpha) is the next step.
3. **`G_CD` dither**, to be implemented and measured only on a screen that reads a
   framebuffer back (scene `0x02`'s njpeg assembly).
4. **`ZMODE_DEC`, `G_TEXGEN`, `G_SHADE`, the `G_AC_DITHER` hash, the depth image's
   RDRAM transfer and `a_w` in `gl_Position`** are still unimplemented, unchanged
   from sessions 172-174.
5. **`shifted=` is nonzero on the mission while the multiply does not move the
   frame** (session 174's open item 2), unchanged.
