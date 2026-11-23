# Session 142 handoff — the browser build died on the title screen because every TEXRECT was one pixel too big

> **Summary.** Session 142 handoff — **the browser build's title-screen fault was the framebuffer writeback running one row past the guest's colour image, because a `G_TEXRECT`'s lower-right corner was treated as inclusive.** Session 141 left the `divide by zero` trap at the hi-res title screen as open item 1 and the corrected S/T step with no post-fix frame as item 4. `app/src/web_renderer.cpp`'s `draw_texrect` built its quad at `ndc_x(xh + 1)`/`ndc_y(yh + 1)` and spanned `w = xh - xl + 1`, `h = yh - yl + 1` of S/T; the RDP covers `[ul, lr)` for a texrect (`RDP::drawRect` takes the 1/4-pixel corners without snapping, and the `lrx |= 3; lry |= 3` snap belongs to `RDP::fillRect`; RT64's `OGRE_RDP_TRACE=1` prints `fillRect ulx=0 uly=0 lrx=1280 lry=960` for the 320x240 clear), so every texrect was one pixel wider and one row taller than hardware. The title screen's backdrop is forty 640x12 strips over one 640x480 colour image, so the strip that ends at row 480 (the last of the forty) drew row 480; `readback_framebuffer` copies every row a draw touched, so the writeback wrote 1280 bytes at `image_end`, the next heap block's header. The game's allocator `func_800712C4` read image pixels as a block size and called `func_80071950`, whose body trapped (`memory access out of bounds` at `func_80071950` <- `func_800712C4` <- `func_801990A8` <- `func_801A3CA0` <- `func_80177E94` <- `func_80072398` <- `func_8008AFE0`), with the task counter frozen at 290; session 141's `divide by zero` in `func_801989AC` is the same corruption on a descriptor's width field, and the RDRAM dumps show the two later colour images' block headers holding image rows (`ef33ef33`, `deaddead`) where RT64 holds zeros or structured values. The corner is exclusive now and `draw_fill_rect` keeps its `+1` (a fill rect's raw corner is `lry = y<<2 + 3`). The browser build also got the per-frame diagnostics it never had: `ogre_gfx_flush()` calls `ogre::poll_scene()` and `ogre::ram_key_tick()`, so `OGRE_SCENE`, `OGRE_SCENE_LOG`, `OGRE_SCENE_TRACE` and `OGRE_RAM_KEY_AT_MS` work, and new `debug/probes/rdram.cjs` reads a dump out of `Module.FS` for `tools/rdram.py`. New bounded diagnostic `[GFX-EDGE]`. After the fix the browser has no page error in 250-300 s runs and reaches the publishers (`0x0A`), the title (`0x04`) and the attract story (`0x0B`); `docs/proofs/web-title-640-after-texrect-fix.png` is the 640x476 title screen (item 4 closed). Open: the attract story is only partly drawn (`docs/proofs/web-attract-story-partial.png`), framebuffer-as-texture, the VI view rectangle, the headless-only deployment.

## Goal

Continue the browser renderer (developer, session start). Session 141's open item
1 is the `divide by zero` trap the browser build hit the first time the game
rendered its hi-res title screen; items 2-5 (framebuffer-as-texture, the VI's
view rectangle, the missing post-UV-fix frame, the headless-only deployment) were
left standing.

## Result

1. **The trap is the framebuffer writeback overrunning the guest's colour image,
   and the overrun is one row of `G_TEXRECT`.** `app/src/web_renderer.cpp`'s
   `draw_texrect` treated the RDP's lower-right corner as inclusive
   (`xh + 1` / `yh + 1` in the geometry, `w = xh - xl + 1` / `h = yh - yl + 1` in
   the S/T span). The RDP covers `[ul, lr)` for a texrect: `RDP::drawRect` takes
   the corners in 1/4-pixel units without snapping them, and the `lrx |= 3;
   lry |= 3` snap belongs to `RDP::fillRect` only. Every TEXRECT the browser drew
   was therefore one pixel wider and one row taller than the hardware's. The
   title screen's backdrop is forty 640x12 strips over one 640x480 colour image,
   so the strip that ends at row 480 - the last of the forty,
   `(0,468)-(640,480)` - is 12 rows on hardware and 13 in the browser, and its
   13th lands on guest row 480, the first row past the end of the image. The
   writeback copies every row a draw touched (`readback_framebuffer`), so it
   wrote 1280 bytes at `image_end` — the block header of the next heap
   allocation. The game's allocator `func_800712C4`
   (`lw v0,-8(s1)`; `sltiu`; `jal 0x80071950`) then read image pixels as a block
   size and called `func_80071950`, whose body touched a bad address and trapped.
   Session 141's `divide by zero` is the same corruption landing on another
   field: `func_801989AC` divides 4096 by a row size derived from a descriptor's
   width, and a corrupted descriptor gives width 0.
2. **Fix**: the texrect's corner is exclusive now. `xh <= xl || yh <= yl` is the
   empty rectangle, `w`/`h` are `xh - xl` / `yh - yl`, and the quad's corners are
   `ndc_x(xh)` / `ndc_y(yh)`. `draw_fill_rect` keeps its `+1`: a fill rect
   decodes its corner, ORs the low two bits of the pixel value (`xh |= 3`, which
   is RT64's `lrx |= 3`) and then covers one more pixel, which is what finishes
   the last row for a raw corner such as `959` (`239.75`). The game writes the
   two commands' corners with different conventions, and RT64's own trace shows
   the fill idiom as `lrx=1280 lry=960`.
3. **The browser build got its per-frame diagnostics.** `main_web.cpp` has no
   `update_gfx` callback, so `ogre::poll_scene()` (which is `OGRE_SCENE` forcing,
   `OGRE_SCENE_LOG` and `OGRE_SCENE_TRACE`) and the timed RDRAM dump
   (`OGRE_RAM_KEY_AT_MS`) never ran in the browser. `ogre_gfx_flush()` now calls
   both, and `debug/probes/rdram.cjs` reads the dump out of the wasm filesystem
   (`Module.FS.readFile`) and writes it next to the page log, so a browser bug is
   read offline with `tools/rdram.py` like a native `OGRE_DUMP_RDRAM` image.
4. **`[GFX-EDGE]` (new, bounded)**: the first 24 draws whose vertices reach a
   render target's outermost row, with the target size and the framebuffer. That
   line is how the overrun was found; it stays.
5. **Session 141's open item 4 is closed**: `docs/proofs/web-title-640-after-texrect-fix.png`
   is the 640x476 title screen with the corrected strips.

## Evidence

### 1. The browser stopped dying

Before the change, `node /tmp/web142shot.cjs --secs 300` (`debug/out`-style
capture of the page log, the canvas every 12 s, and the task count every 2 s)
stalled at task 290, 70 s in, with the game thread dead and the presenter still
drawing the last framebuffer:

```
[stderr] worker sent an error! ... Uncaught RuntimeError: memory access out of bounds
    at func_80071950 <- func_800712C4 <- func_801990A8 <- func_801A3CA0
     <- func_80177E94 <- func_80072398 <- func_8008AFE0   (the frame pump)
```

After the change, the same probe run has a 1-byte `errors.txt` (only the
trailing newline), 672 display lists at 250 s, and this scene timeline from
`[scene]` (the browser build's scene log, item 3):

```
[scene] t=10657ms id=0x0009
[scene] t=76580ms id=0x000A     (the publisher stills, where the old build died)
[scene] t=95171ms id=0x0004     (the title screen)
[scene] t=163420ms id=0x000B    (the attract story)
```

The earlier fixed-build run reached task 844 in 300 s with no page error, from
`crashed=false`.

### 2. Which draws reached past the image

`[GFX-EDGE]` on the pre-fix build (bounded to 24 lines) names both kinds, and the
texrects are the ones on the image's last row:

```
[probe142] edge kind=2 wy=0 target=640x481 fb=0x1BA780 siz=2 vcount=6 pos0=(-1.000,-0.946)
[probe142] edge kind=1 wy=481 target=640x481 fb=0x2E6840 siz=2 vcount=6 pos0=(-1.000,1.000)
```

`wy = 0` is window row 0, i.e. guest row `height - 1 = 480`; `kind = 2` is a
texrect and `kind = 1` a fill rect. The fill's `wy = 481` is its *top* edge
(clamped to 480, guest row 0), which is inside the image. After the change the
same diagnostic prints only `row=481` lines, one per target:

```
[GFX-EDGE] draw kind=1 row=481 target=640x481 fb=0x1BA780 siz=2 vertices=6 first=(-1.000,1.000)
[GFX-EDGE] draw kind=2 row=481 target=640x481 fb=0x1BA780 siz=2 vertices=6 first=(-1.000,1.000)
...
```

The geometry itself is in the renderer's own `[GFX-WIDE]` lines: the title's
strips are `(0,0)-(640,12)`, `(0,12)-(640,24)`, `(0,24)-(640,36)` …, 640x12 each,
forty of them over the image's 480 rows. That diagnostic keeps at most 64 lines
per task, so the capture holds only the first 32 strips; the line above is what
proves a texrect had `yh = 480`, because `ndc_y(481)` is `-1` on a 481-row target
and `nh-1-0` is guest row 480.

The DL's corners are exclusive in 1/4-pixel units, which RT64's own trace shows
for the same idiom on the 320x240 screen:

```
OGRE_RDP_TRACE=1 ./build-app/ogrebattle64
[rdp] fillRect ulx=0 uly=0 lrx=1280 lry=960        # 320<<2, 240<<2
```

### 3. The bytes the writeback left in RDRAM

Both browser dumps are the same scene (0x0A, the publisher stills) and the same
addresses. `before` is the browser before the change (`OGRE_RAM_KEY_AT_MS=68000`
through `debug/probes/rdram.cjs`), `after` the browser after it
(`OGRE_RAM_KEY_AT_MS=52000`), `native` is `OGRE_DUMP_RDRAM` from RT64 in the same
scene. The four bytes are the file's own bytes, so the values are in the runtime's
reversed word order:

```
                          before    after     native
0x80250780  fbA image end ef33ef33  8aa888ff  ff88a88a
0x802E6800  fbB image end ef33ef33  00000000  00000000
0x802507F8  fbB header-8  ef33ef33  feeeefff  ffefeefe
0x802E6838  fbC header-8  deaddead  00000000  00000000
```

`ef33ef33` and `deaddead` are the previous image's pixels: the two later colour
images' block headers held image rows and nothing else. After the change those
addresses hold the same kind of content RT64 leaves there — zero for fbC, and for
fbA a value that is RT64's own bytes reversed (`8aa888ff` against `ff88a88a`).
The two dumps are taken at different points of the scene, so the remaining
difference at `fbB header-8` is not a claim that the two renderers agree byte for
byte.

### 4. The browser's new diagnostics

```
node debug/probes/rdram.cjs --secs 120 --out /tmp/web142dump2 \
  --query 'OGRE_RAM_KEY_AT_MS=52000&OGRE_RAM_KEY_FULL=1&OGRE_RAM_KEY_OUT=/tmp&OGRE_SCENE_LOG=1'
DUMP /tmp/ram-1-full.bin -> /tmp/web142dump2-rdram.bin (8388608 bytes)
```

The page log carries `[ramkey] key 0: wrote /tmp/ram-1-full.bin (8388608 bytes,
whole image)` and the `[scene]` lines, so both new paths work in the browser.
`tools/rdram.py <dump> word <addr>` reads the image.

### 5. Verification runs

| what | command | result |
|---|---|---|
| native smoke, after the generated-code revert | `OGRE_PREF_DIR=/tmp/run142i OGRE_NO_AUDIO=1 OGRE_SPEED=4 OGRE_EXIT_AFTER_MS=45000 OGRE_DMA_TRACE=1 OGRE_SCENE_LOG=1 ./build-app/ogrebattle64` then `tools/runlog.py /tmp/run142i/run.log --check` | `PASS: no crash, no stub call, no unknown module, no bad RSP exit, no stubbed non-gfx task`; 3 bank loads, 528 display lists |
| browser, fixed build, 300 s | `/tmp/web142fix` (page log, screenshots, tasks) | no page error, 844 display lists, title at t=51 s, attract at t=119 s |
| browser, final build (probes reverted), 250 s | `/tmp/web142final` | no page error, 672 display lists, title at t=95 s, attract at t=163 s |
| synthetic renderer self-check | `node debug/probes/testdraw.cjs --warmup 14000 --out web142-testdraw` | `test_draw exported= true returned= 1` (its canvas count, 261209 and 263777 on the two builds, tracks whatever frame the game is presenting under the synthetic rect) |
| browser RDRAM after the fix | `node debug/probes/rdram.cjs --query 'OGRE_RAM_KEY_AT_MS=52000&OGRE_RAM_KEY_FULL=1'` | the region past each image matches RT64's |
| `make web` | every source change | clean |

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `draw_texrect`: exclusive lower-right corner (empty test, `w`/`h`, quad corners); the `test_draw` comment's texel arithmetic; `ogre_gfx_flush()` calls `ogre::poll_scene()` and `ogre::ram_key_tick()`; new `[GFX-EDGE]` diagnostic |
| `debug/probes/rdram.cjs` | new; boots the wasm build with the dump knobs, reads the image out of `Module.FS`, writes `<out>-rdram.bin`, the page log, the samples and the errors |
| `debug/package.json` | `rdram` script |
| `docs/proofs/web-title-640-after-texrect-fix.png` | new; the 640x476 title screen with the corrected strips |
| `docs/proofs/web-attract-story-partial.png` | new; the attract story (scene 0x0B) 5 s in, drawn only in its upper part |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status, decision, log |
| `app/src/web_renderer.cpp`, `RecompiledFuncs/funcs_3.c`, `RecompiledFuncs/funcs_4.c` | the tagged `probe142` prints, reverted (§Probes) |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session).

## Probes

`probe142` was a temporary print, not a keeper:

- `RecompiledFuncs/funcs_4.c` — at `func_80198ECC`'s entry, the descriptor
  address, width, height and return address of the first 24 calls and of every
  call with width 0. Native (`/tmp/run142f/run.log`) and browser
  (`/tmp/web142edge-pagelog.txt`) both print the same two descriptors
  (`0x801C2770` 72x72 and `0x801BCE60` 60x62), and neither prints a zero width,
  so the sprite builder's own arguments were never the fault. Deleted.
- `RecompiledFuncs/funcs_3.c` — at `func_801989AC`'s `div`, the operands if the
  divisor is 0. Deleted.
- `app/src/web_renderer.cpp` — the draw-loop edge print, which is the `[GFX-EDGE]`
  diagnostic above and stays (bounded to 24 lines and to targets taller than 300
  rows).

Reverted with `make recomp` and proved:
`grep -rn probe142 app/ RecompiledFuncs/ Bank*Funcs/ RspFuncs/` is empty, and
`build-app` and `build-wasm` were rebuilt afterwards (§Result, §Verification).

## Open

1. **The attract story (scene 0x0B) is only partly drawn.** 5 s into the scene the
   canvas has the parchment's stone frame and two text plates in its upper third
   and black below (`docs/proofs/web-attract-story-partial.png`); one sample from
   the same scene showed a noise field instead. This is now the browser's next
   rendering gap, and it is reachable without any input in about three minutes.
2. Session 141's items stand: framebuffer-as-texture (a `G_LOADTILE` whose source
   is a rendered colour image reads RDRAM), presentation ignores the VI's view
   rectangle, the deployed site has only been exercised headless, the first load
   fetches the whole 21 MB wasm, and the browser build assigns gamepads to N64
   controllers 2-4.
3. `debug/probes/rdram.cjs` reads the image out through `Module.FS`; a dump taken
   after a `divide by zero` trap is not possible (the wasm instance is gone), so
   the timed dump has to bracket the fault. `OGRE_RAM_KEY_AT_MS` takes one dump
   per run.
4. The writeback band is bounded by the render target's height, and a 640-wide
   target is 481 rows tall while the guest's colour images are 480. The texrect
   path was the one observed to draw that row; a fill rect whose raw lower-right
   corner reaches row 481 would too. No such draw appears on the screens the
   browser has run (`[GFX-EDGE]` prints only `row=481` lines, which are the top
   edge), and the fill path's own overshoot is clipped by the target height.
