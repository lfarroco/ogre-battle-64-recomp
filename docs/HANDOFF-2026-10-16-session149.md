# Session 149 handoff — the browser decoded a fill colour as 32-bit RGBA whatever the colour image was, and the attract story's letterbox colour came out green

## Goal

Continue the web renderer (developer, session start). Session 148's open item 2
was the next step: put a screen the publisher still does not exercise against
RT64's reference at the pixel-level bar the still now sets. The attract story
(scene `0x0B`) was picked: it is reachable with `OGRE_SCENE=story` on both sides.

## Result

1. **The browser's `G_FILLRECT` decoded the fill colour the same way for every
   target.** `draw_fill_rect` used the `G_SETFILLCOLOR` word's 32-bit RGBA8
   decode (`(R<<24)|(G<<16)|(B<<8)|A`) whatever the colour image was. The RDP
   writes the fill colour *in the colour image's format*: a 16-bit image takes
   the low 16 bits as RGBA5551, a 32-bit image the whole word as RGBA8888. RT64
   does exactly that (`rt64_framebuffer_renderer.cpp`, the FillRect instance
   path: `ColorConverter::RGBA16::toRGBAF(fillColor & 0xFFFF)` when
   `fbPair.colorImage.siz != G_IM_SIZ_32b`, else `RGBA32::toRGBAF(fillColor)`).
   The attract story's letterbox fill is `0x4AC14AC1`: as 16-bit RGBA5551 it is
   `(74,90,0)` (olive), as 32-bit RGBA8 it is `(74,193,74)` (green), and the
   browser painted the green. The near-black fill `0x00010001` painted
   `(0,1,0)` where the 16-bit value is `(0,0,0)`.

2. **Fixed and verified as pixels.** `draw_fill_rect` now computes the effective
   colour from the colour image's size (`st.cimg_siz != G_IM_SIZ_32b` and a
   colour image set → `rgba16_to_rgba8(word & 0xFFFF)`; otherwise the 32-bit
   decode, which is also the canvas path's). The four story fills, isolated with
   `ogre_gfx_debug_flags` bit 1 (`draw_tri` stops recording after three draws,
   `draw_fill_rect` keeps drawing), read:

   | buffer row | before | after |
   |---|---|---|
   | 0, 5, 17 (band) | `(0,1,0)` | `(0,0,0)` |
   | 100 (inside the scissor) | `(74,193,74)` | `(74,90,0)` |
   | 193 (gradient row) | `(15,38,15)` | `(15,18,0)` |
   | 194, 239 (band) | `(0,1,0)` | `(0,0,0)` |

   `(74,90,0)` is RT64's decode of the same word. The trace agrees: the final
   build prints all four fills with
   `fill=[0.29,0.35,0.00,1.00] word=0x4AC14AC1 siz=2 scissor=1(0,18,319,194)`.
   `rect_logged_`'s cap was raised from 3 to 8, because the fill whose colour
   matters is the *fourth* of the story's list and a cap of three hid it.

3. **The story screen's whole difference is now accounted for, and two thirds of
   it is not the browser.** The display list (native `OGRE_DL_DECODE=all`,
   `dl=115`) issues four fills, all `FILLRECT (0,0)-(319,239)`:

   | # | fill colour | scissor |
   |---|---|---|
   | 1 | `0xFFFCFFFC` | `(0,0)-(319,239)` |
   | 2 | `0x00010001` | `(0,0)-(319,239)` |
   | 3 | `0xFFFCFFFC` | `(0,18)-(319,194)` |
   | 4 | `0x4AC14AC1` | `(0,18)-(319,194)` |

   then the scene (1676 triangles, 9 texrects, one full-screen texrect). Against
   RT64's `s.160` target the browser is `meanAbs=[25.21,30.50,5.73]`, and the two
   causes are:
   - **The letterbox bands.** RT64's target has `(74,90,0)` in rows 0..17 and
     194..239, the browser `(0,0,0)`. RT64 implements a fill as a *clear of the
     unscissored rect* (`InstanceDrawCall::Type::FillRect` →
     `clearColor(0, 0, clearRect.color, clearRects, clearRectCount)`, with
     `clearRect.rect = convertFixedRect(call.callDesc.rect, ...)`; the scissor
     only reaches `fbPair.drawColorRect`, the writeback bookkeeping), so fill 4
     paints the whole 320x240. The browser clips the fill with `glScissor`. The
     bands are 64 of the frame's 240 rows (27%) and the largest single term in
     the difference: 54.7 mean abs over those rows, 0.267 * 54.7 = 14.6 of the
     frame's 20.6 mean over the three channels.
   - **The animated sky.** RT64's own captures differ across the run (`s.160`
     sky `(10,1,27)`, `s.180` `(24,16,47)`), so the scene rows cannot be compared
     across runs at all; only the bands and the text are static.

4. **Session 84's olive-band conclusion is entangled with that, and is stated
   here as two hypotheses.** Session 84 recorded the bands as the game's own 4:3
   letterbox and read the olive as the *wrong arm* of a two-way branch on
   `*(0x80197C88)` (the port reads record-2 code where retail reads runtime
   data). That branch does choose fill 4's colour, but the band rows are outside
   fill 4's scissor, so which colour is visible there depends on whether the
   scissor clips a fill:
   - **If it does** (n64brew's Set Scissor: "pixels that are outside of this
     region will not be processed in the pixel pipeline and will not be written
     to memory", plus its FILL/COPY edge and fill-triangle hazards), the bands
     are fill 2's `0x0001` = black in *both* arms, the arm colour is invisible on
     this screen, and RT64's olive bands are RT64's fault.
   - **If it does not**, the bands are fill 4's arm colour: the port's olive
     `(74,90,0)` and retail's other arm `(0,24,8)`, both "black" to the eye, and
     session 84's branch reading is the explanation.
   The cheapest discriminating experiment is a reference emulator's RDRAM: reach
   the attract story in RetroArch + Mupen64Plus-Next and read the halfword at
   `0x2CA1C0 + y*640` for a band row. `0x0001` means the scissor clips fills
   (`0x4AC1` means it does not, and the port's bands are the wrong arm). The
   emulator's own image cannot settle it — both candidates look black.
   **No change was made to RT64**: this is a retail-behaviour claim and rule 8
   needs that evidence first.

5. **The browser's own fill scissor is one column narrow.** Hardware's FILL and
   COPY modes have the scissor's right edge inclusive and its lower edge
   exclusive (n64brew Set Scissor hazards), while the browser passes the stored
   half-open pixel bounds to `glScissor` for every draw (session 146's fix, which
   is right for 1-cycle/2-cycle draws). The story's fills are scissored to
   `lrx=1276` = pixel 319, so hardware writes column 319 and the browser writes
   columns 0..318. It is invisible here (the VI's visible width is 639 of 640)
   and is left as open item 2.

## Evidence

### 1. The story's fill sequence

`OGRE_DL_DECODE=all` block `dl=115`, with the state that precedes each
`FILLRECT`:

```
2F6330 G_SETFILLCOLOR 0xFFFCFFFC   SETSCISSOR ulx=0 uly=0    lrx=1276 lry=956
2F6388 G_SETFILLCOLOR 0x00010001   SETSCISSOR ulx=0 uly=0    lrx=1276 lry=956
2F64C0 G_SETFILLCOLOR 0xFFFCFFFC   SETSCISSOR ulx=0 uly=72   lrx=1276 lry=776
2F6500 G_SETFILLCOLOR 0x4AC14AC1   SETSCISSOR ulx=0 uly=72   lrx=1276 lry=776
```

All four are `FILLRECT ulx=0 uly=0 lrx=1276 lry=956`. The 1/4-pixel scissor
`uly=72 lry=776` is rows 18..193.

### 2. The isolated fills, before and after

`debug/probes/fills.cjs` (new, kept) boots the browser build at
`OGRE_SCENE=story`, sets `ogre_gfx_debug_flags(2)` and screenshots the canvas
once a later task has executed. The canvas is 640x480 (the 2x box of the
320x240 buffer), so buffer row `r` is canvas row `2r`:

```
row   before         after
  0   (0, 1, 0)      (0, 0, 0)
  5   (0, 1, 0)      (0, 0, 0)
 17   (0, 1, 0)      (0, 0, 0)
100   (74, 193, 74)  (74, 90, 0)
193   (15, 38, 15)   (15, 18, 0)
194   (0, 1, 0)      (0, 0, 0)
239   (0, 0, 0)      (0, 0, 0)
```

Before the fix the trace read `fill=[0.29,0.76,0.29,...]` for `0x4AC14AC1`; the
final build reads, for the four fills,

```
fillrect (0,0)-(319,239) cyc=3 omh=0x382C00 ... fill=[1.00,1.00,0.97,0.00] word=0xFFFCFFFC siz=2 scissor=1(0,0,319,239)
fillrect (0,0)-(319,239) cyc=3 omh=0x382C00 ... fill=[0.00,0.00,0.00,1.00] word=0x00010001 siz=2 scissor=1(0,0,319,239)
fillrect (0,0)-(319,239) cyc=3 omh=0x382C30 ... fill=[1.00,1.00,0.97,0.00] word=0xFFFCFFFC siz=2 scissor=1(0,18,319,194)
fillrect (0,0)-(319,239) cyc=3 omh=0x382C30 ... fill=[0.29,0.35,0.00,1.00] word=0x4AC14AC1 siz=2 scissor=1(0,18,319,194)
```

### 3. RT64's fill is a clear with the unscissored rect

`tools/RT64/src/render/rt64_framebuffer_renderer.cpp`:

```cpp
case InstanceDrawCall::Type::FillRect: {
    ...
    clearRect.rect = convertFixedRect(call.callDesc.rect, ...);   // the draw rect
```

and the clear itself:

```cpp
    worker->commandList->clearColor(0, clearRect.color, clearRects, clearRectCount);
```

`RDP::drawRect` merges the scissor intersection into `fbPair.drawColorRect` (the
writeback rect) only; `call.callDesc.rect` stays the rect the game asked for. So
the olive fill clears all 320x240.

### 4. The scene is animating

RT64's own captures of the same screen, sky sample at `(160,60)`:

| capture | `s.80` | `s.120` | `s.160` | `s.180` | `s.200` |
|---|---|---|---|---|---|
| sky | `(18,18,37)` | `(18,18,37)` | `(10,1,27)` | `(24,16,47)` | `(31,24,57)` |

and the band is `(74,90,0)` in every one.

## Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` | clean (twice: the fix, then the trace cap) |
| the fills, before | `node /tmp/w149/fillprobe.cjs --query 'OGRE_SCENE=story&OGRE_SCENE_LOG=1' --out /tmp/w149/fill-before --secs 70` | band `(0,1,0)`, middle `(74,193,74)` |
| the fills, after | `node debug/probes/fills.cjs --query 'OGRE_SCENE=story&OGRE_SCENE_LOG=1' --out /tmp/w149/fills-keep --secs 70` | band `(0,0,0)`, middle `(74,90,0)` |
| the story frame against RT64 | `python3 tools/web_frame_cmp.py /tmp/w149/storyfix-00.png '/tmp/w149/nat/s.*.bin'` | `meanAbs=[24.57,30.75,6.49] exact=13.0%`; bands 54.7 mean abs (the scissor difference), scene rows the sky phase |
| the story's fills, native ground truth | `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=story OGRE_SCENE_LOG=1 OGRE_DL_DECODE=all OGRE_VI_TRACE=1 OGRE_EXIT_AFTER_MS=8000 ./build-app/ogrebattle64 assets/ogre64.z64` | `dl=115`, the four fills above; `[scene] t=4940ms id=0x000B` |
| the RT64 target, native | `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=story OGRE_CAPTURE_TARGET=/tmp/w149/nat/s OGRE_CAPTURE_AFTER=60 OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=12000 ./build-app/ogrebattle64 assets/ogre64.z64` | `s.80`..`s.200`, band `(74,90,0)` |
| title (`0x04`) regression | `node debug/probes/palette.cjs --query 'OGRE_SCENE=title&OGRE_SCENE_LOG=1' --out /tmp/w149/titlefix --secs 70 --shots 1 --min-colorful 100000` | the capture sampled the 640x476 publisher still instead (the boot got there first): cream 191300, shadow 38824, strokes `(57,90,165)` 9836, red T 1868 — session 147/148's numbers |
| boot regression | `node debug/probes/boot.cjs --attempts 3 --secs 70 --out /tmp/w149/bootfix` | `RENDERED nonBlack 24788 / colorful 15076` (a fade-in frame) |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000 --out /tmp/w149/testdrawfix` | `exported=true returned=1` |

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `draw_fill_rect` decodes the fill colour in the colour image's format (16-bit → `rgba16_to_rgba8(word & 0xFFFF)`, else the 32-bit word); `RenderState::fill_color_word`; the `[GFX-RECT] fillrect` line carries the effective colour, the word, the image size and the scissor; `rect_logged_`'s cap 3 → 8 |
| `debug/probes/fills.cjs` | new; captures a frame with only the fill rectangles drawn, so a fill colour is read as pixels |
| `docs/proofs/web-story-fill-decode-before.png` | new; the story's fills with the 32-bit decode (green) |
| `docs/proofs/web-story-fill-decode-after.png` | new; the same fills with the RGBA5551 decode (olive) |
| `docs/proofs/web-story-fill-decode-side-by-side.png` | new; before left, after right |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/guides/web-probes.md` | browser section, decision row, log entry, index, probe table |
| `docs/HANDOFF-2026-10-16-session149.md` | this file |

## Probes

- No probe was added to generated code and none was reverted; nothing under
  `RecompiledFuncs/`, `Bank*Funcs/` or `RspFuncs/` was touched.
- `/tmp/w149/fillprobe.cjs` (throwaway) is the first form of the isolation
  capture; `debug/probes/fills.cjs` is the kept version, with the same body
  against the harness rather than an absolute require path.
- `tools/RT64` is dirty with the port's own renderer changes from earlier
  sessions; nothing in it was touched this session. The fill-scissor finding in
  §3 is a reading of it, not a change to it.

## Open

1. **Does the RDP's scissor clip FILL-mode writes?** This decides whether RT64's
   story bands are a native-renderer bug (and whether session 84's olive-band
   branch reading was chasing a symptom) or whether the browser's black bands are
   the accident. n64brew says the scissor clips every pixel-pipeline write, which
   would make RT64 wrong; the decisive evidence is a reference emulator's RDRAM
   at the band rows (`0x0001` vs `0x00C3`), which `docs/guides/emulator-first.md`
   §5's savestate recipe can produce. Fixing RT64 means intersecting
   `clearRect.rect` with `fbPair.scissorRect` in the `Type::FillRect` case, with
   FILL mode's inclusive right edge (`scissorRect.lrx | 3`) and exclusive lower
   edge.
2. **The browser's fill scissor is one column narrow** for FILL/COPY modes
   (hardware's scissor right edge is inclusive there, GL's is half-open). It does
   not show on the story (the visible width is 639 of 640) and needs a screen
   whose last column matters, or a synthetic draw, to verify.
3. Session 148's items stand: the canvas's rounded corners clip four pixels and
   the presenter never writes its last column and row (cosmetic); the title's two
   moving-cloud bands; framebuffer-as-texture; the LOADTLUT `tmem` key; the
   `fldRegs` x/y scale and offset.
4. The attract story is not a good comparison screen: its scene rows animate
   between runs. The publisher stills, the cathedral backdrop and the map's
   sprites are the screens to compare next, and the fills' decode now matches
   RT64 on all of them.
