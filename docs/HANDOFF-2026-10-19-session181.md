# Session 181 handoff — the QUEST q meshes do rasterize and the reference frame is produced; a normal-speed run presents the still without them

## Goal

Continue the browser fix of the QUEST publisher logo (`?OGRE_SCENE=publishers`,
scene `0x0A`). Session 180 left the q draws recorded in list order and
rasterized into the colour image, with "the frames that present them have a
black plate" and a named next experiment (the PRIM writer of one black-plate
frame). The developer's ask for this session was to compare how RT64 and the
native renderer handle that scene.

## Result

No code landed. The session's central claim is corrected: the renderer does
rasterize the q triangles **and** produces the reference frame (white plate,
three coloured q's, wordmark), and the canvas read back right after the present
blit shows it. What fails is the batch that reaches the present: with two
display lists in one flush the presented target holds no q colour, although the
same batch recorded 756 q triangles into it. The native sequence was measured as
the reference. A real, separate renderer bug was found and measured (a WebGL
texture-feedback loop that drops draws); its first fix attempt regressed the
stills and was reverted.

Sections 1-5 are the evidence, section 6 the reverted work, section 7 the next
experiment.

### 1. The native sequence, measured with `OGRE_CAPTURE_PRESENT`

`build-app/ogrebattle64` with `OGRE_SCENE=publishers`, `OGRE_CAPTURE_PRESENT`,
`OGRE_CAPTURE_EVERY=2`, `OGRE_EXIT_AFTER_MS=9000`. The capture is the swap
chain (1366x768, the game image letterboxed). Reading each PPM:

```
f.166  white=0       col=48906   green=0       <- q's animate in, black plate
f.174  white=386     col=9818
f.180  white=95      col=49781
f.182  white=390     col=49766
f.184  white=716819  col=54491  green=0        <- white plate + q's + wordmark
f.186  white=716779  col=54320  green=21213
f.188  ... f.194   identical
f.196  white=0       col=0                     <- black
f.198  white=0       col=0
f.202  white=0       col=435138                <- title clouds
```

So the reference frame is held for about 12 presents (0.2 s), after a black-plate
q animation. There is no white-plate/wordmark-only frame in the native
sequence.

### 2. The native display-list recipe for the q's

`OGRE_DL_DECODE=all` (30 s forced publishers run, 656 MB log). The QUEST lists
(`dl=700`, `ptr=0x803ACBB0`, `cmds` counted by the decode) are:

1. `SETSCISSOR` `(0,0,640,480)`, `SETCIMG 0x2E6840`, `SETFILLCOLOR 0xFFFCFFFC`,
   16 `G_FILLRECT` bands clearing the plate white.
2. `SETCIMG 0x250800` (or `0x1BA780` on the other list), `SETFILLCOLOR
   0x00010001`, 16 `G_FILLRECT` bands clearing the target black.
3. `G_DL 0x801B84B8` (`GEOMETRYMODE` 0, `SETPRIMCOLOR 0xFAFAFAFF`,
   `SETCOMBINE FCFFFFFF FFFDF6FB`) then `G_DL 0x80186630`: 16 `G_TEXRECT`
   bands, tile 0 with `line=0`, i.e. PRIM-only.
4. `G_DL 0x801B8520` (`SETCOMBINE FFFCF87C`, `SETTIMG 0x8000C408` CI8),
   3 wordmark texrects.
5. The q sub-lists, each preceded by the recipe (`ptr=0x803ACEB8` onwards):

```
DE000000 801B85B8   G_DL q state block (GEOMETRYMODE, SETCOMBINE FC121824 FF33FFFF, TEXTURE on)
DA380000 803AC780   G_MOVEMEM (matrix)
E3001001 00000000   SETOTHERMODE_H
FD180000 80001400   SETTIMG RGBA32 width=1 at 0x80001400
F5180000 07094250   SETTILE t7 fmt=0 siz=3 line=8 tmem=0
E6000000            G_RDPLOADSYNC
F3000000 073FF080   G_LOADBLOCK uls=0 lrs=0x3FF dxt=0x80
E7000000            G_RDPPIPESYNC
F5181000 00094250   SETTILE t0 (tmem=0)
F2000000 0007C07C   SETTILESIZE t0 31x31
E7000000
DB060038 00026C00   MOVEWORD segment[14] = 0x26C00
DE000000 0E000000   G_DL the q geometry
```

A q sub-list is `G_VTX` + `G_TRI2`/`G_TRI1` only (decoded from a RDRAM dump at
`0x80026C00`); it contains no texture load and no state.

### 3. The browser draws the reference frame

A temporary probe (section 6) read back the colour image after the flush's
draw loop, and read back the canvas right after the blit:

```
[GFX-P181-FB] flush 1040 fb=250800 fbo=93 tex=92 640x481 cmds=807 tris=756
              white=279632 dark=2824 col=20588
[GFX-P181-CANVAS] flush 1054 fb=250800 canvas 639x475 white=275957 dark=2192 col=20580
```

`dark` is the wordmark; `col` is the q colour. So both the colour image and the
canvas hold the reference frame. In this (probe-slowed) run every QUEST list is
one flush.

Ruled out for the q draws by the same probe (756 triangles per batch):

| check | measurement |
| --- | --- |
| NDC bounds | `inNDC=756`, `ndc=[-0.48..0.39, -0.19..0.52]` |
| finite positions/z | `nan=0 znan=0 wbad=0` |
| triangle area | `amax=0.006..0.24`, `flat=0` |
| viewport | `vp=0,0,640,481` |
| scissor | enabled `(0,1,640,480)` |
| depth | `depth_test=0 depth_write=0` |
| blend | `alpha_blend=0`, `oml=0x00552078` |
| alpha compare / coverage | `ac=0 cvg=0/0` |
| GL errors on `kind=0` draws | none (the errors are on fills and texrects, section 4) |
| framebuffer binding | `nofb=0 bindbad=0` |
| texture keys | `found=756`, uploaded mean 114/255 |
| combiner | `FC121824 FF33FFFF` and `FCFFFFFF FFFCF87C`, both with `TEXEL0` |

A forced constant fragment colour for triangles (`u_debug_mode=3`, triangles
only) plus a forced full-screen vertex override produced 640 magenta pixels in
one row: the draws are issued and accepted, so the q fragments are not reaching
the presented target through this path in the failing configuration.

### 4. What differs: one list per flush against two

A second probe gated on `OGRE_PROBE181_CANVAS`/`OGRE_PROBE181_FLUSH` logs the
canvas readback after every present plus the ordered run-length of each flush.
In a normal-speed run one flush carries two lists:

```
[GFX-P181-FLUSH] flush 2895 cmds=1614 tris=1512
  byfb=1BA780:756 250800:756
  [F2E6840 p1.00/0.00 x16][F1BA780 ... x16][T1BA780 p0.98 x16][T1BA780 p0.98 x3]
  [t1BA780 p0.98 x756]
  [F2E6840 p1.00/0.00 x16][F250800 ... x16][T250800 p0.98 x16][T250800 p0.98 x3]
  [t250800 p0.98 x756]
[GFX-P181-CANVAS] flush 2895 fb=250800 canvas 639x475
  white=296537 dark=2192 col=0
```

The same draw order and state, 756 triangles into the presented target, and the
canvas has no q colour. With one list per flush the same draws paint
`col=20580`. The wash (`white=296537`, `dark=2192`) is the plate plus the
wordmark, which is the developer's screenshot.

The developer's report reproduces on the restored tree: a 42 s canvas capture
records the QUEST hold as `white=18797/19200`, `col=0`, and the ATLUS still as
`white=13330`, `col=15592`.

### 5. A separate measured bug: WebGL texture feedback drops draws

`upload_framebuffer` binds the colour image's own texture to the active sampler
unit and does not restore the binding; `blit_to_canvas` leaves it bound too. The
program's samplers are active for every draw, so a following draw into that
framebuffer is a WebGL feedback loop: `glDrawArrays` raises
`INVALID_OPERATION` (0x0502) and the draw is skipped. A probe counted:

```
glerr=102/0,86,16    (total / kind0 triangles / kind1 fills / kind2 texrects)
[GFX-P181-ERR] draw error 0x0502 kind=1 fb=2E6840 fbtex=10590 tex0=10590 tex1=10549
```

86 fills and 16 texrects per run, at the stills' plate `0x2E6840`. Triangle
draws never error, so this is not the q fault.

### 6. Probes and the reverted fix

All probe code lived in `app/src/web_renderer.cpp` behind
`OGRE_PROBE181`/`OGRE_PROBE181_*` and is reverted. It added: a per-task
run-length of the recorded draws; a per-flush ordered run-length plus a
colour-image readback (`GFX-P181-FB`); a canvas readback after the blit
(`GFX-P181-CANVAS`); a per-present line with the FBO identity and the last
triangle's state (`GFX-P181-PRES`); NDC/depth/NaN/area counters and a
`glGetError()` count per draw; viewport/scissor logging at the first triangle
(`GFX-P181-VP`); a per-key mean of each uploaded texture; and the debug-mode-3
and full-screen vertex overrides. `app/src/milestones.hpp` (4 MiB page buffer)
and `app/web/index.html` (300 000-line log ring) were raised for the run and are
restored. Proof of revert: `grep -rl probe181 app/` is empty and `make web`
builds.

Two fix attempts were tried and reverted:

- Restoring the previous texture binding after `upload_framebuffer` and after
  `blit_to_canvas`. It removes the feedback drops in principle, but it did not
  change the presented QUEST frame, so it is not landed.
- Binding the 1x1 white fallback instead of the previous binding. This made the
  stills present black (the fallback changes what the untextured draws sample),
  so it is wrong, not merely incomplete.

`app/src/web_renderer.cpp` now holds exactly session 179's
`combiner_uses_texture` change (91 insertions, 3 deletions against `HEAD`).

### 7. Next experiment

The defect is at the batch/present boundary. Give `DrawCmd` the display-list id
that published it (a counter in `send_dl`), then log for every present: the VI
target, the last list that drew into that target, and how many of its q
triangles were recorded. Compare a flush with one list against the same flush
with two. If the presented target's last list is the q list and its q pixels are
missing, the difference is inside the draw loop's framebuffer switch; if the
presented target's last list is not the q list, the difference is what
`present_framebuffer` resolves and the batching is faithful. Do not change the q
draw state first.

A second, independent item: land the feedback fix with an A/B on the plate's
RDRAM contents (`0x2E6840`), because the plate fills are the draws it drops and
the stills' bands sample the plate in some lists.

## Files changed

- `PLAN.md` — corrected the session-180 status bullet, added the feedback-loop
  item, rewrote open item 19.
- `docs/DECISIONS.md` — one row: the q fault is a batch/present problem.
- `docs/STATUS-LOG.md` — session 181 entry.
- `docs/README.md` — handoff index row.
- `docs/scenes.md` — the QUEST row's browser status.
- `docs/HANDOFF-2026-10-19-session181.md` — this file.

No recompiled code and no RT64 file was touched. `tools/RT64` was already
modified before this session (the submodule's patch state).

## Verification run

- `make web` (Emscripten, `EM_CACHE=<repo>/build-wasm/emcache`) after the
  revert and after each probe build.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1`:
  `fbtex hits=1 lines=1`.
- Browser forced-publishers runs with Playwright/Chromium
  (`--enable-gpu --use-angle=metal`), 30-60 s each, canvas screenshots and
  readbacks as above.
- Native runs: `OGRE_NO_AUDIO=1 OGRE_MOD_TEST=1 OGRE_SCENE=publishers` with
  `OGRE_DL_DECODE=all` and with `OGRE_CAPTURE_PRESENT`/`OGRE_CAPTURE_EVERY`.
