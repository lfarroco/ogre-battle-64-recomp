# Session 143 handoff — the browser presented the whole framebuffer texture, so every frame was squashed into its top half

> **Summary.** Session 143 handoff — **the browser presented the whole framebuffer texture, so every frame was squashed into its top half.** Session 142's open item 1 was the attract story (scene `0x0B`) "only partly drawn", with a noise field in one sample. `app/src/web_renderer.cpp`'s `blit_to_canvas` drew the GL framebuffer with UV `(0,0)..(1,1)` over the whole canvas. A GL framebuffer is the RDP colour image at the tallest height `ensure_framebuffer` has ever been asked for (RT64's `Framebuffer::maxHeight` rule), and the publishers' 640x476 mode grows the game's 320x240 images to `320x476` while the title's own VI mode is `320x240`, so the game's 240 rows landed in the top 240/476 = 50.4% of the canvas and the texture's rows 240..476 — the *next* guest image — showed below them as noise or black. The 50.4% line is in session 142's own proof: `docs/proofs/web-attract-story-partial.png` has its first all-black row at screenshot row 243 of 482. `visible_framebuffer_size()` now ports RT64's `VIRenderer::visibleFramebufferSize` (the VI's `fbSize` minus `OGRE_OVERSCAN`, default 1, the knob the native presenter reads) and `blit_to_canvas(tex, u_max, v_min)` samples only that area; vertically the crop takes the *high* end of `v`, because `upload_framebuffer` flips and texture `v = 1` is guest row 0 — the first version cropped the low end and made the title worse. `[GFX-PRESENT]` now prints `visible=WxH` beside `size=`. Verified on a 250 s natural run: the title's grown `320x476` targets present `visible=319x239`, the "Ogre Battle 64" logo and the story parchment page fill the frame (`docs/proofs/web-title-640-after-texrect-fix.png` replaces session 142's squashed file; `web-title-squashed-present-before.png` and `web-attract-story-full-frame.png` are new), the 640x476 world-map mode is unchanged (`nonBlack 307200`, `visible=639x475` of a 640x481 target), `boot.cjs` renders and `testdraw.cjs` still returns 1. Open: the browser's title/story frames are hue-shifted against RT64's target for the same text (cloud band `(128,86,84)` against `(156,134,77)`, sky matching), CI8 tiles decode black in display lists that load no palette, and session 141/142's items stand.

## Goal

Continue the browser renderer (developer, session start). Session 142's open item
1 was the attract story (scene `0x0B`) being "only partly drawn", with a noise
field in one sample. Items 2-4 (framebuffer-as-texture, the VI's view rectangle,
the deployed site exercised only headless) were left standing.

## Result

1. **The partial frame was the presenter, not the scene.** `blit_to_canvas` drew
   the framebuffer texture with UV `(0,0)..(1,1)` over the whole canvas. A GL
   framebuffer is the RDP colour image at the tallest height
   `ensure_framebuffer` has ever been asked for (RT64's `Framebuffer::maxHeight`
   rule), and the publishers' 640x476 mode grows the game's 320x240 images to
   `320x476` while the title's own VI mode is `320x240`. Presenting the whole
   texture therefore put the game's 240 rows in the top 240/476 = 50.4% of the
   canvas and showed the texture's rows 240..476 — the *next* guest image — below
   them, as noise or black. That boundary is visible in session 142's own proof:
   `docs/proofs/web-attract-story-partial.png` has its first all-black row at
   screenshot row 243 of 482, i.e. canvas row 242, the 50.4% line.
2. **The presenter now shows only the area the VI scans out**, the way RT64 does.
   `visible_framebuffer_size()` ports `VIRenderer::visibleFramebufferSize`
   (`tools/RT64/src/render/rt64_vi_renderer.cpp`): the VI's `fbSize` minus the
   overscan crop (`OGRE_OVERSCAN`, default 1, the knob the native presenter
   reads). `blit_to_canvas(tex, u_max, v_min)` bounds the sample to
   `visible.width / texture.width` and, vertically, to the *high* end of v:
   `upload_framebuffer` flips, so texture `v = 1` is guest row 0 and the visible
   rows are `1 - visible.height / texture.height .. 1`. RT64's present shader
   maps `videoResolution / textureResolution` over the window for the same
   reason (`src/shaders/VideoInterfacePS.hlsl`).
3. **`[GFX-PRESENT]` carries the crop now**: `size=320x476 siz=2 visible=319x239
   nonBlack=...`, so the two numbers that decide the mapping are on one line.
4. **A first version of the fix cropped the wrong end of v** (`v in [0,
   visible/texture]`) and made the title *worse*: the canvas then showed the
   texture's rows past the colour image. The flip is the whole subtlety; the
   frames from that version are not kept.
5. The session also measured a **second, separate gap** and did not fix it: the
   browser's title and story frames are hue-shifted against RT64's own render
   target for the same text, and CI8 tiles decode pure black in display lists
   that load no palette. Both are written up under Open with their evidence.

## Evidence

### 1. The framebuffer that was presented and its visible area

From the final natural-route run (`/tmp/web143final-pagelog.txt`), the title
scene (`0x04`) presents from a grown 320x476 target while the VI scans out
320x240:

```
[GFX-FB] framebuffer 0x000400: 320x240 siz=2 fmt=0 pitch=640      (boot, line 267)
[GFX-VI] VI scanout 640x476 origin=0x1BAC80 ...                   (publishers, line 2474)
[GFX-FB] framebuffer 0x000400: 320x476 siz=2 fmt=0 pitch=640      (line 2597)
[GFX-PRESENT] VI scanout 0x04B680 -> fb 0x04B400: fbo=1 size=320x476 siz=2
              visible=319x239 nonBlack=71165 ... (present 3900)
[GFX-PRESENT] VI scanout 0x000680 -> fb 0x000400: fbo=1 size=320x476 siz=2
              visible=319x239 nonBlack=91673 ... (present 5400)
[GFX-PRESENT] VI scanout 0x025E80 -> fb 0x025C00: fbo=1 size=320x476 siz=2
              visible=319x239 nonBlack=76800 ... (present 6300)
```

`nonBlack=76800` is `320*240`, i.e. the whole drawn band holds colour. Before the
change the same targets were drawn to the canvas as `v = 0..1`, which is 476 rows
into 240.

### 2. The screens

| screen | before (session 142's build) | after |
|---|---|---|
| title logo (`0x04`, 320x240 mode, canvas 320x240 at 2x) | `docs/proofs/web-title-squashed-present-before.png`: the logo in the top half, black below | `docs/proofs/web-title-640-after-texrect-fix.png`: the logo and its clouds full frame |
| story prologue text (`0x0B`) | session 142's `/tmp` frames: parchment in the top 40%, noise below | `docs/proofs/web-attract-story-full-frame.png`: the parchment page with its two label plates, full frame |
| attract story text page (`0x0B`) | — | `/tmp/web143final-048.png`: four lines of text over the full-height page |

The proof files came from a 250 s natural run (intro → publishers → title →
attract story) with `/tmp/web143scene.cjs`, a throwaway probe that labels every
canvas capture with the last `[scene] t=` line, so no capture is attributed to a
scene by eye:

```
n=24 wall=67.9  tasks=296 scene=id=0x0004    n=48 wall=144.4 tasks=456 scene=id=0x000B
n=36 wall=105.0 tasks=340 scene=id=0x0004    n=60 wall=239.5 tasks=559 scene=id=0x000B
```

`docs/proofs/web-title-squashed-present-before.png` is the pre-change build on the
same natural route at the same screen (`/tmp/web143nat-canvas-048.png`, session
142's `make web` output); `docs/proofs/web-title-640-after-texrect-fix.png`
**replaces** session 142's file of that name, which was the squashed frame
presented as "the 640x476 title screen with the corrected strips".

### 3. The native reference for the same screen

`OGRE_SCENE=title OGRE_CAPTURE_TARGET=/tmp/nt-title-target/t
OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=200` writes the 320x240
`R16G16B16A16_UNORM` texture the VI renderer sampled. `t.400` is the same
prologue screen as the browser's title frame ("Long ago, there was a time
when"), and its content occupies the full 240 rows (no crop, no letterbox). The
browser's full-frame title now has the same layout; the colour difference is
item 1 of Open.

### 4. The 640-wide mode is unchanged by the crop

`node /tmp/map-probe.cjs --save assets/save-mission-1.srm` (session 141's probe,
which writes the developer's battery into the wasm filesystem) reaches the
640x476 VI mode with 640x481 targets:

```
MAP {"w":640,"h":480,"nonBlack":307200,"colorful":307182}
    vi=[320x240,640x476]
    fb=[000400:320x240,025C00:320x240,04B400:320x240,2E6840:640x481,
        1BA780:640x481,250800:640x481,000400:320x476,025C00:320x476,04B400:320x476]
```

and the same mode in the natural run presents `visible=639x475` of that 640x481
target (`[GFX-PRESENT] VI scanout 0x1BAC80 -> fb 0x1BA780: fbo=1 size=640x481
siz=2 visible=639x475 nonBlack=294182`). Session 141 measured the same screen as
307200/307200 non-black and 247857 colourful on the pre-crop build, so the
one-row crop costs six rows of a 481-row target and nothing else.

### 5. Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web` | clean, after every source change; the last run compiles the reverted source |
| natural route, 250 s | `node /tmp/web143scene.cjs --secs 250 --out /tmp/web143final` | no page error; title at wall 68-127 s (tasks 296→415, so the title animates), story at 144-239 s (456→559) |
| forced publishers → title | `--query 'OGRE_SCENE=publishers'` | the prologue text full frame (`/tmp/web143pub-030.png`), the world-map story page full frame (`/tmp/web143pub-048.png`) |
| forced title | `shots.cjs --url '...?OGRE_SCENE=title'` | clouds full frame (`/tmp/web143title-final-03.png`); the forced entry's own black rectangle and frozen task counter do not appear on the natural route, so they are forced-entry artifacts |
| boot regression | `node debug/probes/boot.cjs --attempts 2 --secs 75 --out web143-boot-final` | `RENDERED nonBlack 25644 / colorful 16816` (sessions 138-142 measured 24840-25496 / 15076-18868; the count moves with the intro's fade) |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000 --out web143-testdraw` | `test_draw exported= true returned= 1` |
| native build | not run | `app/src/web_renderer.cpp` is only in `app/CMakeLists.txt`'s Emscripten list (`OGRE_RENDERER_SOURCES`, line 185) |

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `overscan_crop()` / `visible_framebuffer_size()` (RT64's `visibleFramebufferSize`); `blit_to_canvas(tex, u_max, v_min)` and the two present call sites; `[GFX-PRESENT]` prints `visible=WxH` |
| `docs/proofs/web-title-640-after-texrect-fix.png` | replaced; the full-frame title where the file held the squashed frame |
| `docs/proofs/web-title-squashed-present-before.png` | new; the pre-change frame on the same natural route |
| `docs/proofs/web-attract-story-full-frame.png` | new; the story page session 142 showed as partial |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status, decision, log, index |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Probes

- `/tmp/web143scene.cjs` (throwaway): boots the wasm build, samples the canvas
  every 8 s and records the last `[scene] t=` line and the task count with each
  capture. `/tmp/ppm2png.py` and `/tmp/target2png16.py` convert RT64's P6 and
  `R16G16B16A16_UNORM` captures for comparison.
- **A palette-persistence experiment was added and reverted.** The RDP's TMEM
  palette outlives a display list, and `send_dl` constructs a fresh `RenderState`
  per task, carrying only the colour image, so a CI draw in a list that loads no
  palette decodes against an empty `tlut`. The experiment carried
  `st.tlut`/`st.tlut_valid` across display lists in three hunks; the A/B was
  `debug/probes/textures.cjs` against `OGRE_SCENE=publishers`, whose 24 decoded
  CI8 320x6 tiles were byte-identical before and after (`sha256
  7a86bc6f42beefeb21a206424b5d2890f99f68130ee90b6153e4e2b59c4f90dd`, a black
  image), so it fixed nothing that was measured and was removed. Proven:
  `grep -c last_tlut_ app/src/web_renderer.cpp` is 0 and `make web` was re-run.
  The finding it came from is Open item 2.
- No probe is left in the tree; no diagnostic was removed.

## Open

1. **The browser's title and story frames are hue-shifted against RT64's target
   for the same text.** Sampling the cloud band (rows 60..100 of a 320x240 image,
   every 4th column): browser `(128,86,84)` against native `(156,134,77)`, while
   the sky above it matches (`(59,28,0)` against `(61,29,0)`). Compared:
   `/tmp/web143map-640-08.png` (browser, title prologue at 640x476) against
   `/tmp/nt-title-target/t.400-noflip.png` (RT64's target for the same screen).
   The cloud strips are CI8 in COPY mode (`[GFX-TXR] ... fmt=2 siz=1 line=40
   cyc=0 omh=0x008C30 oml=0x0A4000`), so the difference is in the CI8 tile decode
   or in the blender; it is not this session's change (session 142's proof has
   the same colours). The next step is to print the TLUT the strip binds and the
   palette entries in use, and to compare them with RT64's `OGRE_TEX_TRACE_COLOUR`
   decode of the same rectangle.
2. **CI8 tiles decode pure black in display lists that load no palette.**
   `st.tlut` is filled only by `G_LOADTLUT`, and `send_dl` builds a fresh
   `RenderState` per display list, so the browser's palette does not persist the
   way the RDP's TMEM does. The publishers run's analyzer counts 13820 display
   lists, 1359 CI8 `G_SETTIMG`s and 562 `G_LOADTLUT`s, so most CI draws are in a
   list with no palette load, and `debug/probes/textures.cjs` against
   `OGRE_SCENE=publishers` dumps 24 all-black CI8 320x6 tiles. Carrying the
   palette across display lists did not change those dumps (see Probes), so that
   is not the whole mechanism — the black may be the sampled window, not the
   palette. The screen is not all black: the same run's QUEST still shows the CI8
   backdrop's cream pattern on its right half, and its left half and the black
   scanlines through the pattern are wrong in a way this session did not
   identify (`/tmp/web143map-640-02.png`).
3. Session 141/142's items stand: framebuffer-as-texture (a `G_LOADTILE` whose
   source is a rendered colour image reads RDRAM), presentation still resolves
   the VI's origin by containment rather than applying its own view rectangle,
   the deployed site has only been exercised headless, the first load fetches the
   whole 21 MB wasm, the browser build assigns gamepads to N64 controllers 2-4,
   and `debug/probes/rdram.cjs` reads one dump per run.
