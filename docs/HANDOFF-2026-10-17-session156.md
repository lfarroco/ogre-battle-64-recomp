# Session 156 handoff — the title's cloud bands are a real renderer difference, not a phase artefact, and the alignment method that proves it

## Goal

Continue the browser (WebAssembly) renderer. Session 155 closed the
framebuffer-as-texture item and left the title screen's two moving cloud bands as
the last known browser rendering difference: `meanAbs` about 2.3/2.5/3.1 per
channel against RT64's render target (session 148), concentrated in rows ~64..112
and ~195..238, the browser 12-16 levels darker.

The question this session had to answer: is the difference the clouds being at a
different animation phase, or the renderer drawing the cloud draw differently? A
phase difference is not a bug; a renderer difference is.

## Result

1. **Session 155's claims match the tree.** Checked at file level, quoting the
   working tree:

   - `note_fb_write` (`app/src/web_renderer.cpp`) inverts with
     `const double edge = (1.0 - pos) * 0.5 * fb_height_px;`, takes
     `const int e = static_cast<int>(std::lround(edge));`, treats the vertex as
     an edge (`top = clamp(e, 0, H-1)`, `bottom = min(e - 1, H - 1)`), so a fill
     of rows 100..163 is recorded as `[100,163]`. The lower-right corner is
     exclusive by construction.
   - `test_draw`'s bit-6 branch is `(g_debug_flags.load(...) & 64u) != 0` with
     `kFb2Off = 0x00C28000`, `G_FILLRECT` words
     `0xF6000000 | ((63 << 2) << 12) | (163 << 2)` and
     `w1 = (0 << 2) << 12 | (100 << 2)` (ul (0,100), lr (63,163)), and a
     `G_SETTILESIZE` of ul (0,98) lr (31,99) before the load.
   - `debug/probes/testdraw.cjs` parses `--expect-hits`
     (`opts['expect-hits'] === undefined ? null : h.num(opts['expect-hits'], 0)`),
     makes it a hard check (`const hitsOk = expectHits === null || fbtexHits === expectHits;`)
     and exits 2 on a mismatch.
   - The bit 5-6 fixtures and the detector feed diagnostics only. `fb_written_`
     is written only by `note_fb_write` and read only by `note_fb_texture`,
     whose return value is discarded at its one call site (line 4189); bit 5 and
     bit 6 are read only inside `test_draw`, which only the exported
     `ogre_gfx_test_draw` calls, and that export is called only by the probes.
     Bits 0-4 do change rendering and are set only by `ogre_gfx_debug_flags`,
     which only probes call. No game frame moves.

2. **Session 155's 25-scene sweep is reproducible from its own artifact.**
   `/tmp/155-fbtex-all.txt` still exists: 25 per-scene blocks, `hits=0` in every
   one (sum 0), and the task counts match the handoff's list. The four one-task
   blocks are `0x02`, `0x03`, `0x0D` and `0x0E`, each followed by
   `PAGEERROR ... memory access out of bounds` (the `0x02` stack is
   `func_8007A110 <- func_ovlC_801B7EBC <- func_ovlC_801AFC2C <- func_80178568 <- func_80075BC0`).
   One precision note: the `TOTAL ... 0 over 25` line is printed to stdout by
   `fbtex.cjs`, not written into the `.txt`, which holds the 25 blocks only; the
   file supports the total as their sum. The handoff's later `0x04`/`0x0A`/`0x10`
   numbers (`39`/`25`/`29`) are the separate fixed-build re-check in §5, not the
   sweep's rows (`37`/`56`/`77`), which is what the handoff says.

3. **The native title series.** One bounded run produced 525 target frames of
   the forced title:

   ```
   OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title OGRE_SCENE_LOG=1 \
   OGRE_CAPTURE_TARGET=/tmp/w156/nat2/t OGRE_CAPTURE_AFTER=100 OGRE_CAPTURE_EVERY=1 \
   OGRE_EXIT_AFTER_MS=32000 ./build-app/ogrebattle64 assets/ogre64.z64
   [scene] t=10704ms id=0x0004 descriptor=0x8018FB40 mask=0x00008000
   [capture] wrote .../t.101.bin .. t.625.bin (320x240 format=11)   525 files
   ```

   `OGRE_CAPTURE_EVERY` gates the target capture as well as `OGRE_CAPTURE_PRESENT`
   (`rt64_present_queue.cpp`, `ogreCapture.thisFrame`), so a numbered series
   comes from this one knob. `OGRE_SCENE=title` is used on both sides, so both
   runs enter the scene the same way and the pre-state variable is the same.

4. **The browser title series.** 40 canvas frames at ~0.4 s spacing, same forced
   scene:

   ```
   node debug/probes/shots.cjs \
     --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SCENE=title&OGRE_SCENE_LOG=1' \
     --attempts 2 --secs 200 --shots 40 --gap 400 --out /tmp/w156/web/title
   CAPTURED 40 frames (attempt 1)
   ```

   The element screenshots are 642x482 (the 1 px border around a 640x480 CSS box
   that is the 320x240 drawing buffer at 2x), which is what
   `tools/web_frame_cmp.py` reads as a 320x240 buffer.

5. **The aligned minimum still carries the bands.** Every browser frame was
   compared against the whole native series on a row/column profile first
   (coarse), then exactly on the shortlist and its neighbours, with the same
   canonicalisation as `tools/web_frame_cmp.py`. The minimum is
   `title-03.png` against `t.574.bin`:

   - inset 1 (the presenter's unwritten last column and row excluded):
     `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2%`
   - the official tool on the same pair without the inset:
     `meanAbs=[2.31,2.32,2.71] meanSigned=[-2.26,-2.26,-2.63] exact=79.2%`
     (identical to session 148's `[2.30,2.50,3.12] / 81.3%` within the run).

   The per-row profile at the minimum (`mean abs / mean signed`, browser minus
   native), rows 0-239 in tens:

   ```
   y=  0..79: 0.0-0.2 / +0.0         (the upper sky is exact)
   y= 80: 0.0 +0.0 0.1 -0.0 0.2 -0.2 0.3 -0.3 0.7 -0.6 1.4 -1.4 2.3 -2.3 3.6 -3.6 5.3 -5.3 7.1 -7.1
   y= 90: 8.8 -8.7 7.9 -7.9 7.5 -7.5 8.3 -8.3 8.9 -8.9 9.6 -9.6 10.1 -10.1 8.6 -8.6 7.6 -7.6 8.7 -8.7
   y=100: 8.2 -8.2 ... 10.0 -10.0 (flat around -9)
   y=110: 9.3 -9.3 8.5 -8.5 6.5 -6.5 4.5 -4.5 2.7 -2.7 1.4 -1.4 0.7 -0.7 0.3 -0.3 0.1 -0.1 0.0 +0.0
   y=120..189: 0.0-0.2 / +0.0       (exact)
   y=190: 0.0 0.0 0.0 0.0 0.2 -0.1 0.2 -0.2 0.5 -0.4 1.2 -1.1 2.1 -2.1 3.4 -3.4
   y=200: 5.0 -5.0 6.5 -6.5 8.3 -8.3 9.4 -9.3 10.2 -10.2 11.2 -11.2 12.1 -12.1 12.6 -12.6 13.0 -13.0 12.8 -12.8
   y=210: 12.4 -12.4 ... 10.1 -10.1
   y=220: 11.0 -11.0 11.9 -11.9 12.7 -12.7 13.0 -13.0 12.4 -12.4 11.3 -11.3 9.8 -9.8 8.1 -8.1 6.4 -6.4 4.7 -4.7
   y=230: 2.8 -2.8 1.2 -1.2 0.4 -0.4 0.1 -0.1 0.0 +0.0
   ```

   The band's interior is one-signed: `meanSigned == -meanAbs` to one decimal, so
   every differing pixel is darker in the browser, never brighter. Rows 0-79,
   120-189 and 235-238 are exact.

6. **The phase is excluded.** Native frames of the same phase are identical in
   the bands, and adjacent phases differ by far less than the residual:

   ```
   t.573 vs t.574 (consecutive):  bands (0-80)(80-120)(120-190)(190-240) = 0.00 0.00 0.00 0.00
   t.570 vs t.574 (4 presents):  0.00 0.46 0.00 1.51
   t.560 vs t.574 (14 presents): 0.00 1.79 0.09 3.01
   ```

   A browser frame whose phase fell between two native samples could differ by at
   most about half of one phase step, ~0.75-1.5 in the bands. The measured band
   residual is 8-13, so the clouds are not only a phase artefact: the renderer
   draws the cloud layers differently. The minimum is also well above the
   publisher still's `[0.87,0.84,0.75] / 99.6%`, which is what a purely
   phase-limited comparison would have to approach.

7. **The cloud draw's decoded state is identical in the two renderers.** The
   title's effective draw list is 135 rects per display list: 15 six-row strips
   and 6 64x64 blocks of the RGBA16 sky `0x801C3EA8`, 35 three-row strips of
   `0x801E2918`, 53 three-row strips of `0x80217508`, 24 strips of `0x80249538`
   and two CI sprites. The native trace walks the list before RT64's parser and
   reports 591 texrects; the 456 extra are the rectangles whose tile window is
   degenerate (`lrt == ult`, e.g. the six `0x1E2918` 64x64 blocks and 448 one-row
   `0x217508` strips), which both renderers skip: RT64 in `RDP::drawTexRect`
   (the `OGRE_EMPTY_TILE` guard) and the browser in `draw_texrect`. The effective
   sets agree.

   For the strip at screen rows 101..104, the two traces and the native
   rect-state give the same draw:

   ```
   native  [tex] dl=198 n=70 TEXRECT rect=0,101..320,104 px=320x3 s=0 t=672
                 dsdx=1024 dtdy=1024 window=uls0 ult84 lrs1276 lrt92
                 tile=line80 pal0 timg=0x80217508 fmt=0 siz=3 imgw=320
   native  [rect-state] n=1 rect 0,404..1280,416 cycle=0 ccL=FCFFFFFF ccH=FFFCF279
                 blend(P=CC A=CC_A M=FB B=1-A) texOn=1 tile=0 fmt=0 siz=3 line=80
                 uls=0 ult=84 lrs=1276 lrt=92
   browser [GFX-TEXTRACE] n=64 dl=2 rect=0,101..320,104 px=320x3 s=0 t=672
                 dsdx=1024 dtdy=1024 window=uls0 ult84 lrs1276 lrt92 tile=0 line80
                 pal0 img=0x217508 fmt=0 siz=3 stride=1280 tlut=no ntlut=0
                 texels=321x4 dec=320x3 uv0=(0.000,21.000) uv3=(320.000,24.000)
                 mean=104.6 opaque=319 cyc=1 omh=0x000C30 oml=0x504240 lut=0
                 ablend=1 force=1 ccyc=1
   ```

   That is one variable (the renderer) with the same rectangle, S/T step, texel
   window, image address, format/size, line, palette, LUT mode, cycle type
   (1-cycle), combiner words (`0xFCFFFFFF`/`0xFFFCF279`, TEXEL0 straight through
   for RGB and alpha), render mode (force blend, P=CC A=CC_A M=FB B=1-A), scissor
   and 1:1 texel step. The browser's `cyc/omh/ablend/force/ccyc` fields were
   added to `[GFX-TEXTRACE]` this session to make that comparison from one line.

8. **The texel decode is identical too.** The browser's `mean` for each strip
   equals an independent decode of the same rows of the same image in an RDRAM
   dump (`OGRE_DUMP_RDRAM` of the same forced title):

   ```
   image 0x217508 rows 18..20: independent 92.11   browser trace 92.1
   image 0x217508 rows 21..23: independent 104.55  browser trace 104.6
   image 0x1E2918 rows  0..2 : independent 66.73   browser trace 66.7
   ```

9. **Where the pixels differ, and the open lead.** The band rows are exactly the
   screen rows on which the `0x80217508` strips sample image rows with partial
   alpha (image rows 0..39 and 110..160 map to screen rows 80..119 and
   190..239), and the rows where it samples alpha 255 (image rows 40..109, screen
   120..189) are exact. Inside the bands the browser's pixel equals the raw
   sampled texel while the native's is a blend over the layers below:

   ```
   row  98, image row 18: of the 95 pixels whose texel alpha > 0,
                          browser == texel 94, browser == native 40
   row 101, image row 21: 140 pixels, browser == texel 108
   row 104, image row 24: 156 pixels, browser == texel 155
   row 110, image row 30: 246 pixels, browser == texel 245
   (within the band, restricting to pixels where the texel and the native
    differ by more than 20 levels, the browser equals the texel in 307 of 399)
   ```

   A worked example (row 98, x=40): the `0x80217508` texel is
   `(159,142,103)` with alpha 34, the native pixel is `(172,157,123)` (a blend
   over a brighter layer below), the browser pixel is `(160,142,103)`, the
   texel. The browser's own state for that draw says `ablend=1 force=1 ccyc=1`
   with `b0=[P=CC M=FB A=CC_A B=1MA]` and a TEXEL0 combiner, which is the state
   that should blend with the texel alpha.

10. **The alpha probe was inconclusive and was reverted.** To test whether the
    sampled alpha reaches the blend at all, every RGBA32 texel's alpha was forced
    to 128 (`probe156`) and four title frames were captured. They differ hugely
    from the unprobed frames: the best match against the same native series is
    `meanAbs=[27.26,28.91,18.45] exact=15.3%` against `[2.08,2.23,2.74] / 79.2%`.
    That proves the alpha channel does reach the blend, but it cannot separate
    "the alpha was ignored" from "alpha 128 changes every RGBA32 layer", because
    the same images cover most of the screen; both hypotheses predict a large
    change. The probe therefore settles nothing and was reverted; the lead in
    §9 stands as the next thing to explain.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `[GFX-TEXTRACE]` now prints `cyc=`, `omh=`, `ablend=`, `force=` and `ccyc=` for every reported rectangle, so a draw's cycle type and blend state can be compared with the native `[rect-state]` from one line. No pixel moves: the format string and its five extra arguments are the only change. The `probe156` alpha forcing was reverted (see §10). |
| `docs/HANDOFF-2026-10-17-session156.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session paragraph, log entry, index row, decision row; item 8 gains `(n)` with the series measurement and the open lead |
| `docs/proofs/web-title-clouds-browser-aligned-title03.png` | the browser frame at the aligned minimum (`title-03`, read as the 320x240 drawing buffer) |
| `docs/proofs/native-title-clouds-aligned-t574.png` | the native target frame at the aligned minimum (`t.574`, 320x240) |
| `docs/proofs/web-title-clouds-diff-amplified.png` | `|browser - native| x 8` for that pair: the two bands and nothing else |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/` or `tools/RT64` was
changed. `tools/RT64` still carries session 150's uncommitted fill clip. No
commit was made. The scratch tools of this session are `/tmp/w156/search.py`
(profile shortlist plus exact comparison), `/tmp/w156/nat2`, `/tmp/w156/web` and
`/tmp/w156/rdram.bin`; they are not in the tree.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean after the revert:
  `[100%] Built target ogrebattle64`,
  `web: build-wasm/ogrebattle64.wasm is ready; serve it with python3 debug/server.py`.
- `grep -rn probe156 app/ debug/` — no output, exit 1 (the revert is complete).
- `node debug/probes/boot.cjs --attempts 2 --secs 40 --out /tmp/w156/boot-final` —
  `attempt 1: sawRealDl=true rendered=true`,
  `RENDERED {... "nonBlack":25136,"colorful":16528}`, the same numbers session 155
  recorded.
- Native series (exact command and output in §3).
- Browser series (exact command and output in §4).
- The alignment (§5) with `/tmp/w156/search.py /tmp/w156/nat2 /tmp/w156/web` on
  the tree's canonicalisation, and the official
  `python3 tools/web_frame_cmp.py /tmp/w156/web/title-03.png /tmp/w156/nat2/t.574.bin --inset 1`
  and without `--inset`.
- `python3 tools/web_frame_cmp.py ... --diff-out /tmp/w156/diff-03-574.png` for
  the difference image saved under `docs/proofs/`.
- The trace comparison (§7) from `OGRE_TEX_TRACE=1` on both renderers and
  `OGRE_RECT_STATE=101-104` for the native blend state, plus
  `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_MS=0` for the whole-run native trace.
- The decode comparison (§8) from `OGRE_DUMP_RDRAM=/tmp/w156/rdram.bin` with
  `OGRE_EXIT_AFTER_MS=24000`.

There is no repo test harness; every check above is a ROM-dependent run of 25 s
(native) to several minutes (browser, on a loaded machine the title took ~90 s of
wall time to appear). The machine's load average was 6-16 during the session, so
the browser runs used `--secs 200` and a 90 s boot window.

## Open

1. **The browser's band pixel equals the raw cloud texel while the native's is a
   blend, although the browser's own decoded state says `ablend=1 force=1
   ccyc=1`, TEXEL0 alpha.** The state, the texel window, the image, the combiner,
   the blender inputs and the decode all match. The next discriminating
   experiment, in order of cost:
   (a) print the fragment's combiner alpha for the first `0x217508` strip of a
   title frame (a tagged probe: write `cc.a` into an unused channel or a
   `[probe]` line for `trace_draws_` in a small window, one `make web` and one
   browser run). That answers whether the sampled alpha is the decoded 34/255 or
   1, and splits "texture/upload" from "combiner blend factor" in one run.
   (b) if the sampled alpha is right, log `glGetError` and the effective
   `glIsEnabled(GL_BLEND)`/`glGet(GLenum)` blend factor for that draw.
   (c) if the sampled alpha is 1, compare the CPU pixels the trace measures with
   the pixels actually uploaded for `cmd.tex_key` (the `TexUpload` vector at
   `flush_commands`), which separates the image key/upload dedup from the
   texture object.
2. The two `0x249538` oddities seen while tracing the title stay open and are not
   explained: the last draw of a title display list is a full-screen
   `G_TEXRECT (0,0)-(319,239)` with `s=t=dsdx=dtdy=0` sampling one texel of
   `0x80249538` whose RGBA is `(5,11,15,255)` — it would paint the frame nearly
   black if it were executed, and it is not visible in either renderer's frame —
   and the same image also appears as one 319x239 draw. Both renderers agree on
   the visible result, so this is a question about which colour image those draws
   target, not a known difference.
3. The remaining browser-renderer items are unchanged from session 155: the title
   cloud bands (this item, now measured as a renderer difference), the LOADTLUT
   `tmem` key (needs a reference fact), the canvas's rounded corners and
   unwritten last column/row (cosmetic, needs developer intent), and the screens
   not compared against the reference.

## Correction (session 161)

The "renderer draws the cloud layers differently" conclusion here is corrected.
The two runs do share a scroll state: at the third display list of each run the
135 draws whose tile window is non-degenerate are identical in order, image,
rectangle, `s`, `t`, `dsdx`, `dtdy` and window (the `0x1C3EA8` partial tile is
`rect=0,16..63,80 s=32` in both). Every native list also has 456 draws with a
degenerate tile window (`ult == lrt`): the mirrored `0x1E2918` pieces and the
72-pixel `0x217508` columns. Those are the native app's `OGRE_FOG` repair pieces
(`app/src/renderer.cpp`, `nop_rect::Walker::fog_fix`), which the browser skips at
its own empty-tile guard, so the browser draws no fog layer. Two native series
differing only in `OGRE_FOG=0` give a fog contribution of 3.4 mean abs (rows
64..119 `5.68`), and the browser frame matches the fog-off series at
`meanAbs=0.052` (rows 0.046/0.072/0.044) against `2.349` against the fog-on
series. The residual is the missing fog layer, not the sampler, the blend or the
phase. See `docs/HANDOFF-2026-10-17-session161.md`.
