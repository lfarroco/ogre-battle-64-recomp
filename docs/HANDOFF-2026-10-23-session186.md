# Session 186 handoff — the title drew noise, then only its upper half: a shared-depth framebuffer and a viewport sized from the colour image

## Goal

Developer report: *"the title scene is broken in the web build, just some noise
shows up"*, with a screenshot of the natural title showing vertical colour
stripes over the top third of a 319x239 canvas and black below. A follow-up:
*"the web version is only the upper half of the scene, stretched to fill."*

## Result

The natural title (publisher stills → title) now draws the full prologue scene:
the three-line text, the scrolling clouds, the dark foreground rock, then the
`Ogre Battle 64` logo, and it advances to the attract story `0x0B`. The text
rows match the native capture exactly.

Two independent defects were fixed in `app/src/web_renderer.cpp`.

1. **The colour target and depth renderbuffer had different sizes.** The title
   renders into 320x476 colour images (`0x000400`, `0x025C00`, `0x04B400`) and
   names the depth image `0x2E6840` that scene `0x0A` created for its 640x481
   targets. The browser keyed depth renderbuffers by the `G_SETZIMG` address
   alone and grew one buffer to 640x481, then attached it to a 320x476 colour
   texture. GL reports `GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS` (`0x8CD9`) and
   drops every draw, so the canvas kept the colour texture's undefined contents:
   the stripes and black of the report. Depth targets are now keyed by
   `(address, width, height)`, which is RT64's
   `RenderTargetKey(addressStart, width, siz, Depth)` shape, and the alias
   invalidation walks every size-keyed buffer.

2. **The GL viewport used the colour image's tallest height, not the height the
   draw's NDC was built for.** After the first fix the title showed only its
   upper half, stretched. The title's 320x240 rects record with
   `screen=320x240` (`target_height_px`), while the colour image is 320x476
   because an earlier draw grew it. `ensure_framebuffer` keeps the tallest
   height, but `flush_commands` set `glViewport(0, 0, fb.width, fb.height)`, so
   the 320x240 NDC was stretched over the 476-row image. The presenter samples
   the top 239 rows, which is the top half of the stretched image. The viewport
   is now per draw: `glViewport(0, fb.height - cmd.fb_height_px,
   cmd.fb_width_px, cmd.fb_height_px)`, which maps screen row 0 to guest row 0
   and agrees with the scissor (`y = target_height - sy1`). It is the same
   mapping RT64 gets by transforming screen coordinates to framebuffer pixels.

The order matters: the first defect hid the second, and the second is what the
developer saw after the first was fixed. Both are in the same run.

## Evidence

### 1. The incomplete framebuffer

A temporary completeness check after each depth attach printed, on the natural
title (the check is kept as a one-shot guard):

```
[GFX-DEPTH] depth target #4 for SETZIMG addr=0x2E6840 640x481 (colour 0x2E6840 640x481, GL_DEPTH_COMPONENT24)
[GFX-DEPTH] colour 0x000400 320x476 with depth 0x2E6840 640x481: status=0x8CD9
[GFX-DEPTH] colour 0x025C00 320x476 with depth 0x2E6840 640x481: status=0x8CD9
[GFX-DEPTH] colour 0x04B400 320x476 with depth 0x2E6840 640x481: status=0x8CD9
```

`0x8CD9` is `GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS`. After the change the title
creates `depth target #5 ... 320x476` and no pair reports an incomplete status.

### 2. The viewport, from a temporary probe

The probe printed the title's rect calls and the target size each used:

```
[P186] texrect rect=0,0..320,6 screen=320x240
[P186] texrect rect=0,6..320,12 screen=320x240
...
```

The colour image is 320x476 (the present log:
`VI scanout 0x04B680 -> fb 0x04B400 ... size=320x476 visible=319x239`), and
`flush_commands` set the viewport to the image's full height. The 320x240 NDC
therefore spans all 476 rows, and the presenter's 239-row visible area shows the
top half of the scene at 2x, which is the frame the developer described and the
middle panel of `docs/proofs/web-title-before-after-session186.png`. The fix
maps the draw into `cmd.fb_height_px` rows at the top of the image.

### 3. The frames

- `docs/proofs/web-title-noise-before-session186.png` is the first defect:
  `sha256 06bff23c…`, the frame session 185 recorded as a pre-existing defect.
- `docs/proofs/web-title-before-after-session186.png` places three 1:1 stages
  in one image: the noise, the top-half stretch, and the fixed frame.
- `docs/proofs/web-title-prologue-session186.png` is the fixed natural title
  with the three-line prologue.
- `docs/proofs/web-title-vs-native-session186.png` is the fixed frame beside
  the native capture of the same page. The text bands are identical:
  `34..74`, `78`, `87..130` in both.
- `docs/proofs/native-title-prologue-session186.png` is the native
  `OGRE_CAPTURE_TARGET` crop (the native target is 320x476; the title image
  occupies its top 240 rows).
- `docs/proofs/web-title-logo-session186.png` is the logo stage (frame 88 of
  the 100 s run); the run then logs `[scene] … id=0x000B`.

The fixed title animates: a 48 s capture at 100 ms has 220 title frames and 198
distinct hashes; the broken build repeated one of three static images.

### 4. The draw list was right in both cases

`OGRE_TEX_TRACE=1` on the broken build already showed the title's backdrop
decoding correctly (`img=0x217508 fmt=0 siz=3`, rows `80..239`, `mean=56.8` /
`58.7` / `61.3` / `66.5`, the session-145 values). The recorded title draws
match the native trace's image set (`0x217508`, `0x1E2918`, `0x249538`,
`0x1C3EA8`), so the faults were in target creation and the NDC-to-viewport
mapping, not the display list or the texture decode.

## Verification

- `EM_CACHE=<repo>/build-wasm/emcache make web` clean; `build-wasm/ogrebattle64.wasm`
  is `21132250` bytes (final build, probe removed).
- Natural browser capture, 46 s at 1 s on the final build: the title's text
  bands are `34..74`, `78`, `87..130`, identical to the native capture's. The
  full scene (clouds and the dark foreground rock) fills the frame, and the run
  reaches the logo and `0x000B`.
- Natural browser capture, 55 s at 500 ms: the publisher stills and the QUEST
  q spin still render; the QUEST still frame is byte-identical
  (`sha256 e5b14dea…`) to the capture before the viewport change.
- Natural browser capture, 48 s at 100 ms: 220 title frames, 198 distinct
  hashes.
- Intro scene `0x09` renders `Licensed by Nintendo` at 639x475; the boot probe
  reports `RENDERED`.
- Synthetic controls on the final build: `testdraw.cjs --flags 96
  --expect-fbtex --expect-hits 1` → `fbtex hits=1 lines=1`; `--flags 128
  --expect-depth near` → `rgb=(255,0,0)`; `--flags 384 --expect-depth far` →
  `rgb=(0,0,255)`.
- The viewport change only differs where `cmd.fb_height_px` is smaller than the
  colour image; for a target whose height equals the image's it is the same
  viewport as before. The publisher stills (640x481) and the 320x240 intro are
  unchanged.
- The map and mission were not re-run: they need the save/played route (rule
  11). The change only affects a draw shorter than its colour image, and there
  it anchors the draw where the scissor already points; a target whose height
  equals the image's is untouched.
- `git status --short` shows only the intended files plus the pre-existing
  `tools/RT64` worktree marker.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | depth targets keyed by `(address, width, height)`; alias invalidation over every size-keyed buffer; per-draw viewport height (`fb.height - cmd.fb_height_px`); one-shot depth-pair completeness check; `OGRE_TEX_TRACE_AFTER_MS` / `OGRE_TEX_TRACE_MS` |
| `docs/proofs/web-title-noise-before-session186.png` | the noise frame (`sha256 06bff23c…`) |
| `docs/proofs/web-title-before-after-session186.png` | noise / top-half stretch / fixed, 1:1 |
| `docs/proofs/web-title-prologue-session186.png` | the fixed natural title, prologue page |
| `docs/proofs/web-title-vs-native-session186.png` | fixed browser frame beside the native crop |
| `docs/proofs/native-title-prologue-session186.png` | native reference crop |
| `docs/proofs/web-title-logo-session186.png` | the fixed natural title, logo stage |
| `docs/HANDOFF-2026-10-23-session186.md` | this file |
| `PLAN.md` | web open item 8 carries the session |
| `docs/DECISIONS.md` | new decision row |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/scenes.md` | session entry, index row, title row |
| `docs/guides/app-build.md` | the browser trace window knobs |

## Probes used and reverted

`probe186` (tagged, gated on `OGRE_PROBE186`): one `[P186]` line per title
texrect with the screen size used for its NDC, and a one-shot readback/profile
of `0x04B400`. It named the `screen=320x240` / 320x476 mismatch. Removed, the
tree rebuilt, and `grep -rn "probe186\|P186\|p186" app/ debug/` is empty.

The two additions that stay are diagnostics: the trace window mirrors the
native renderer's `OGRE_TEX_TRACE_AFTER_MS`/`_MS`, and the depth-pair
completeness check is a one-shot guard (`GFX-DEPTH ... status=`). No generated
code, ROM data or submodule was touched.
