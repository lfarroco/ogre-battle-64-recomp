# Session 176 handoff — the mission pair is frame-locked: a 4.74 whole-frame mean hides a missing party, and the later phase stipples the portrait frame and MISSION band; publisher `0x0A` not verified

## Goal

Verification round for the browser WebGL2 renderer, with no new features. Produce a
frame-locked browser/native mission pair from one save and one route, decide whether
the renderer matches, and make one last attempt at the publisher stills `0x0A`.
Record a screen that could not be reached as **not verified**.

Timebox: 90 minutes. The machine was loaded for the whole round: load average 15.6 at
the start of the browser series and 50.7 by its end, so the browser screenshots cost
6–13 s each instead of 0.1 s (Result 1.2).

## Result

### 1. Frame-locked mission pair: the global minimum is `4.74/3.59/1.90`, and it is not parity

#### 1.1 Native series

```
OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1 OGRE_PREF_DIR=/tmp/w176/natpref \
OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title OGRE_SCENE_LOG=1 \
OGRE_TAP_MS=1000 OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6" \
OGRE_CAPTURE_TARGET=/tmp/w176/nat/t OGRE_CAPTURE_AFTER=0 OGRE_CAPTURE_EVERY=1 \
OGRE_EXIT_AFTER_MS=30000 ./build-dist/ogrebattle64 assets/ogre64.z64
```

637 targets, `t.4.bin`..`t.640.bin`, 320x240 `R16G16B16A16`, one per present. The
scene log gives `[scene] t=7624ms id=0x0004`, `t=10204ms id=0x0012`,
**`t=17821ms id=0x0003`**, so the mission window is the 377 presents `t.264`..`t.640`
(12.2 s of mission). The run exits at 30 s. Log `/tmp/w176/nat.log`.

#### 1.2 Browser series: 6 mission frames, and why not ~40

Route `/tmp/w173/drive173.cjs`'s URL with the save and the page's own tap schedule:
`?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`.
The runs reached `0x03` on attempt 1 (`[scene]` lines: `t=31.7s scene=0004`,
`t=55.7s scene=0012`, `t=99.7s scene=0003` in `/tmp/w176/web-m1.log`;
`t=85.3s` in `/tmp/w176/web-m4.log`).

The intended series was a shot every 300 ms. It could not be captured with
Playwright: `locator.screenshot()` of the running canvas cost **12.9 s and 13.5 s**
in `/tmp/w176/drive176b.cjs`'s mission window, so two shots 300 ms apart landed
12.9 s apart. Measured directly (`/tmp/w176/shotcost2.cjs`, blank page vs live
page): `locator` 0.07-0.1 s blank and 0.95-9.5 s with the game running; `page.
screenshot({clip})` 0.03-0.6 s blank and 0.6-5.9 s live. The renderer creates its
WebGL2 context with `attrs.preserveDrawingBuffer = true`
(`app/src/web_renderer.cpp:7646`), so `canvas.toDataURL('image/png')` was tried as
the capture (`/tmp/w176/drive176c.cjs`): 17 frames in 19 s at 3-8 ms per grab in
the quiet stretch (`/tmp/w176/web-m4.log`), 0.2-1.0 s in the loaded one.

**`toDataURL` is not a reliable capture.** In `/tmp/w176/web4/` frames 0..13
(mission ms 0..6608) are entirely black and only frames 14..16 (ms 9909, 13841,
18928) carry the frame, so the preserved buffer holds content only after some
presents. The 14 black frames match native `t.264` (its black entry frame)
bit-for-bit, which is how the failure was caught. So the browser side is **six
mission frames in total**, at mission milliseconds 0, 1, 9909, 12885, 13841 and
18928 - three phases sampled three times, not a 300 ms series. The global minimum
is still well defined (1.3).

#### 1.3 Global minimum over every browser frame x every native frame

`/tmp/w176/crosscmp.c` reimplements `web_frame_cmp.compare()`'s arithmetic
(319x239 overlap, `meanAbs` per channel, `meanSigned`, exact fraction, ranking by
the mean of the three channel means) and reads the native `.bin`s directly. Six
browser mission frames exist: three element screenshots (`web/m1-a1-000`,
`web2/m2-a1-000`, `web2/m2-a1-001`) and, from the last run, three `toDataURL`
drawing-buffer frames (`web4/m4-a1-014/015/016`). The frames 0..13 of that run are
black (Result 1.2). 2262 pairs:

| rank | browser frame | native frame | meanAbs R/G/B | meanSigned R/G/B | exact | score |
|---|---|---|---|---|---|---|
| 1 | `m4-a1-014` (mission ms 9909) | `t.289` | **4.74 / 3.59 / 1.90** | -1.12 / -0.81 / -1.40 | **45.08%** | 3.410 |
| 2 | `m4-a1-014` | `t.292` | 4.75 / 3.60 / 1.92 | -1.12 / -0.81 / -1.41 | 45.09% | 3.421 |
| 3 | `m4-a1-014` | `t.293` | 4.75 / 3.60 / 1.92 | -1.12 / -0.81 / -1.41 | 45.06% | 3.424 |
| 4 | `m4-a1-014` | `t.281` | 4.75 / 3.61 / 1.93 | -1.12 / -0.81 / -1.40 | 45.04% | 3.429 |
| 5 | `m4-a1-014` | `t.278` | 4.75 / 3.61 / 1.94 | -1.12 / -0.81 / -1.40 | 45.06% | 3.434 |
| 6 | `m4-a1-014` | `t.277` | 4.76 / 3.61 / 1.94 | -1.12 / -0.80 / -1.40 | 45.03% | 3.437 |

The other five browser frames' own best partners are `t.588` (6.694), `t.290`
(7.303), `t.372` (7.031), `t.290` (7.303) and `t.310` (6.559), so the best pair is
0.9-1.9 better than every other browser frame's best.

**Sharpness.** Ranks 1-6 span 0.027 inside a 0.5 s native window (`t.277`..`t.293`),
so the minimum is flat: the map at this phase is static and the only moving content
is the party's animation. This pair is frame-locked by content, and the phase is
localised: the mission entry is `t.264`, so `t.289` is 25 presents (0.8 s) after the
scene change, and the browser's own frame is 9.9 s after its `0x03` line, i.e. the
browser's first 9 s of `0x03` present content that the native series only shows in
its first second (the two runs have different intro timing; the pixel match is what
locks them). A wrong phase is far outside: the same browser frame measures 6.694
against `t.588`, 34.8 against a pre-`0x03` frame.

#### 1.4 Terrain regions of the best pair (browser `m4-a1-014` vs native `t.289`)

The session-173 region set, 319x239 overlap, 320x240 target:

| region | meanAbs |
|---|---|
| top cliff rows 0..38 | **0.93** |
| left grass rows 190..238 x 0..90 | **1.66** |
| right terrain rows 95..150 x 285..319 | **2.95** |
| bottom middle rows 200..238 x 100..230 | **8.56** |

`docs/proofs/web-mission-framelocked.png` is the two frames side by side, 2x each,
browser left, native `t.289` right. `/tmp/w176/mission-framelocked-diff.png` is the
difference, |d| x 8.

#### 1.5 What the best pair still differs in: the browser does not draw the party

The frame-locked pair's residual is not the terrain. At this phase both frames show
the map, the castle, the road, the river and the party's cursor brackets, and the
browser is missing the characters themselves:

| element | meanAbs | evidence |
|---|---|---|
| party cluster x18..78 y60..98 | **26.54** | 735 of 2280 pixels differ by more than 30; the browser's brightest channel value in the cluster is **97**, native's is **255**. The five party sprites and their shadows are absent; the two dark-blue cursor brackets at the same place are drawn |
| bottom party | in the bottom-middle region's 8.56 | the same absence at the second party position |
| terrain control x230..300 y120..170 | 2.25 | map, cliff and road match |
| top cliff / left grass / right terrain | 0.93 / 1.66 / 2.95 | terrain matches |

`/tmp/w176/zoom-chars2.png` is the cluster at 8x: native has three characters with
bright armour, eyes and shadows; the browser has the terrain and the cursor brackets
only. The whole-frame `meanAbs` of 4.74/3.59/1.90 is therefore a low mean over a
static map plus a missing sprite layer, which is exactly the case the round's brief
warns about.

**A second, unrelated observation at the later phase.** The three element
screenshots are 6.7-7.3 against their best natives, and at those phases (the note
box, the "Castle Burgund" portrait and the MISSION band are up) two elements lose
their texels one at a time: the portrait box `13.58` (31.3% exact) and the MISSION
band `8.94` (18.6%). In rows 190..214 at a 40/255 threshold the browser has **207**
bright glyph pixels and native **1083**, scattered with no lattice period (gap
histogram `1:23, 2:6, 3:1, 4:4, 5:7, 6:9, 7:11, 8:6 ... 133:1, 138:1`) and no row or
column parity bias (76/65 and 67/74; a background patch is 182/181). The note box's
paper texture and both text lines are 79.8% exact (`4.65`).

**Candidate mechanism, one family for both observations: an alpha/coverage
rejection on alpha-tested sprites.** A uniform threshold would pass or reject whole
runs, and a wrong TLUT index or texel size would corrupt the shapes instead of
keeping them and dropping texels. The renderer has both arms:
`G_AC_THRESHOLD` against the blend colour's alpha and `G_AC_DITHER` against a
per-pixel hash (`app/src/web_renderer.cpp:1596-1606`), and `CVG_X_ALPHA` drops a
pixel whose coverage is below one step (`:1614-1617`). A combination of a low
combiner alpha and one of those tests produces a random subset of texels (the
MISSION band's 19%) and, for the character sprites, nothing visible. Session 175
lists the `G_AC_DITHER` hash as unexercised by measurement, so the party sprites are
the screen that would exercise it.

The discriminating check is one run that logs, for one character sprite and for the
MISSION-band rect, the draw's kind, texture address and format, TLUT, `OTHERMODE_L`'s
compare mode and threshold, and the combiner's pre-compare alpha. If the alpha is
about 0.2 of native's on both, the combiner's alpha inputs are the defect; if the
compare mode differs, it is the compare path.

**Verdict: not parity, and the whole-frame number hides the defect.** The
frame-locked pair is `4.74/3.59/1.90` with the terrain at 0.93-2.95 and the party's
character sprites missing (`26.54` on the cluster, 735 of 2280 pixels off by more
than 30, browser max 97 against native 255). At the later phase the same build is
`6.7-7.3` with the portrait frame and the MISSION band stippled. Session 175's
mission `7.32` was not a parity measurement, and the frame-locked `4.74` is not one
either: it is a matched map with a missing sprite layer.

### 2. Publisher `0x0A`: natural boot, six attempts, **not verified**

`/tmp/w176/pub176.cjs` launches the page with no `OGRE_SCENE`, loads the ROM, then
screenshots the canvas as fast as the machine allows for 20 s, reading `#gfxstats`
and the log tail at every shot, and retries up to six boots. Result: 6 boots, 62
shots, every shot `tasks=0 scene=null nonBlack=0 colorful=0`, `maxTasks=0` on every
boot:

```
boot 1: 11 shots   boot 2: 9 shots   boot 3: 10 shots
boot 4: 11 shots   boot 5: 10 shots  boot 6: 11 shots
scenes seen across 6 boots: (none)
NEVER REACHED 0x0A
```

Every boot's page log stops at `[ROM] selecting rom /rom.z64` after the boot thread
starts (`/tmp/w176/pub/p1-a1-page.txt`); no `[scene]` line is printed, so the natural
boot never entered a scene in the browser this round, and no `id=0x000A` line exists
to key on. The same page, same ROM and same machine reached `0x0004` at 31.7 s when
`OGRE_SCENE=title` was in the URL (`/tmp/w176/web-m1.log`), so the boot path is the
difference, not the ROM load. **The still is not compared with
`docs/proofs/native-licensed-screen.png`, `-atlus-` or `-quest-` this round**, and
session 160's `meanAbs 0.0`/100% over 639x475 is not re-confirmed.

A longer window is the obvious next step: this round's 20 s per boot is inside the
29 s that the forced-title boot needed to print its first scene under load 15.

### 3. Bytes and files

- Native series and log: `/tmp/w176/nat/` (637 `.bin`), `/tmp/w176/nat.log`.
- Browser runs: `/tmp/w176/web-m1.log`, `web-m2.log`, `web-m3.log`,
  `/tmp/w176/web/` (1 mission frame), `/tmp/w176/web2/` (2 mission frames),
  `/tmp/w176/web3/` (empty, load average 50).
- Cross-comparator: `/tmp/w176/crosscmp.c` (built with `clang -O2`),
  `/tmp/w176/mkraw.py` (decodes the canvas PNGs with
  `web_frame_cmp.canvas_pixels`), `/tmp/w176/natlist.txt` (`t.264`..`t.640`).
- Region and proof helpers: `/tmp/w176/regions.py`, `/tmp/w176/compose176.py`,
  `/tmp/w176/zoom176.py`.
- `docs/proofs/web-mission-framelocked.png` (browser `m1-a1-000` left, native
  `t.588` right, 2x, 4 px gap).

## What changed

| file | change |
|---|---|
| `PLAN.md` | the Renderer completeness block: the frame-locked result, the three elements that still differ, and the residuals |
| `docs/STATUS-LOG.md` | session 176 entry |
| `docs/README.md` | session 176 row |
| `docs/HANDOFF-2026-10-18-session176.md` | this file |

No renderer or application code was changed: the round is a measurement, and the
defect the measurement names (the alpha-dither stipple) is not a small unambiguous
fix. No temporary probe was added to the tree, so no revert is needed;
`grep -rn "probe176" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is empty. No commit was
made. `docs/proofs/web-mission-framelocked.png` is the only new binary.

## What was run for verification

- Native dense mission capture, 30 s at `OGRE_SPEED=1`, `OGRE_CAPTURE_EVERY=1`
  (637 targets), `/tmp/w176/nat.log`.
- `/tmp/w176/drive176.cjs` (Playwright screenshots, 1 mission frame, attempt 1),
  `/tmp/w176/drive176b.cjs` (13 s per screenshot, 2 mission frames),
  `/tmp/w176/drive176c.cjs` (`toDataURL`, killed at load average 50 before `0x03`).
- `/tmp/w176/shotcost.cjs` and `/tmp/w176/shotcost2.cjs`: the per-capture cost on a
  blank page and on the running game.
- `/tmp/w176/crosscmp /tmp/w176/weball.raw /tmp/w176/natlist.txt 8 319 239` (2262
  pairs over the six browser frames) and `web_frame_cmp.compare()` for the
  native self-similarity table.
- `/tmp/w176/regions.py`, `zoom176.py`, `compose176.py` for the region and element
  numbers and the proof image.
- `/tmp/w176/pub176.cjs --attempts 6 --secs 20 --gap 300` (six boots, no scene).

### What was not verified

- A dense browser series (one shot per 250–400 ms). The capture cost under load is
  the reason; `toDataURL` is the working replacement and was not finished.
- Publisher `0x0A` (Result 2).
- The S2DEX2 opcode census and the other session-175 unexercised items, unchanged.

## Open

1. **The missing party sprites are the largest defect this round found** (Result
   1.5): the mission's character sprites are absent while their cursor brackets, the
   map, the castle, the note box and the portrait frame are drawn. One run that logs
   one character sprite's draw (kind, texture address and format, TLUT,
   `OTHERMODE_L` compare mode/threshold, pre-compare alpha) is the next step, and the
   same log covers the MISSION band's 207-of-1083 stipple.
2. **The lit-terrain tint at the later phase** (Result 1.5, the `m1`/`t.588` pair):
   G +6.2 / B -14.3 in the top cliff and R +6.4 / B -7.6 in the bottom band. At the
   frame-locked phase the same terrain measures 0.93-2.95, so the tint belongs to the
   later phase or to its fade; not named.
3. **A dense browser series** needs `toDataURL` restricted to frames it can carry
   (14 of 17 were black) or a `readPixels` hook in the renderer; the loop itself is
   3-8 ms per grab on a quiet machine.
4. **The publisher natural boot** stops before its first scene in the browser; the
   next attempt should use a ≥45 s window and log the boot milestones.
