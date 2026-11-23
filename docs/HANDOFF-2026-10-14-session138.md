# Session 138 handoff — the browser renderer draws the njpeg backdrop (S2DEX2 + YUV16)

> **Summary.** Session 138 handoff — **the browser renderer draws the New Game njpeg backdrop: S2DEX2 and YUV16 are implemented in the WebGL2 prototype, and the browser build gains the native debug knobs.** Developer request: continue the browser renderer. Three repairs. (1) The wasm build was broken again by session 109's class — `app/src/ram_key.cpp` included the native-only `sdl_platform.hpp` for `console::key_down`, so `cmake --build build-wasm` failed with the SDL port error; the include and the key read are now `#ifndef __EMSCRIPTEN__`. (2) The GBI of a task is identified by XXH3-64 over its ucode text, the way RT64's `GBIManager::getGBIForUCode` does: the digest covers the whole text, and the OSTask's own `ucode_size` is `0x1000` (one IMEM load), so the first version, which hashed that many bytes, matched nothing; the digest is now compared at the database's lengths `0x1390`/`0x18C0`, and both constants were reproduced from the ROM before being written down (word-reversed XXH3-64 of ROM `0x2F940`/`0x1390` = `0xCF55FAE288BFE48D`; ROM `0x35510`/`0x18C0` = `0x9300F34F3B438634`). In an S2DEX2 list `0xDC`/`0xDA` are then decoded as `G_OBJ_MOVEMEM`/`G_OBJ_RECTANGLE_R` before the F3DEX2 switch, and the game's per-macroblock sequence draws 16x16 rectangles on its own grid with the njpeg source stepping `0x300`, exactly as session 51 recorded. (3) `G_SETCONVERT` is parsed and `decode_texture_rect` gained the YUV16 branch: the guest word is `[U, Y(even), V, Y(odd)]` byte-reversed in RDRAM, one word per two texels, converted with the sign-extended `2*K+1` matrix (`R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`, `B = Y + K3*U'`, NTSC JPEG fallback), mirroring RT64's `loadYUVTileToTMEM` + `sampleTMEMYUV16` on the source instead of on TMEM. The backdrop renders (`docs/proofs/web-newgame-cathedral-njpeg.png`, 307200/307200 non-black, 222378 colourful), one quadrant per njpeg pass because each pass draws at its own framebuffer coordinates. Two mechanisms make the browser build testable the way the native one is: `OGRE_*` query parameters are published into Emscripten's module-scope `ENV` before the runtime initialises its environment (so every `getenv` knob in shared code works in the browser), and `OGRE_TAP_MS`/`OGRE_TAP_BUTTON` drive controller 0 from `web.js` with the native vocabulary; the New Game opening is one URL. `make web` builds the target and `.github/workflows/wasm.yml` compiles it in CI. The remaining renderer gap is RDRAM framebuffer indirection — the game assembles its 2x2 backdrop from CPU copies out of the RDP framebuffer in RDRAM, which the prototype never writes, so the dialogue scene draws an empty image over the backdrop (`docs/proofs/web-newgame-cathedral-covered.png`); §5 of the handoff has the design. The route decision (extend the prototype rather than build RT64 with a WebGPU backend) is in `docs/DECISIONS.md`.

## Goal

Developer request: *"this project has a web renderer, but it is still in its early
stages. let's continue working on it!"* The measured state at the start was
session 109's: the wasm build compiles and the title renders, and the three
renderer gaps were the njpeg backdrop path (no S2DEX2, no YUV16) and RDRAM
framebuffer indirection. The developer was asked which route to take and chose to
extend the WebGL2 prototype rather than start RT64's WebGPU backend; the decision
is recorded in `docs/DECISIONS.md`.

## Result

1. **The wasm build was broken again, by the same class session 109 fixed.**
   `app/src/ram_key.cpp` included the native-only `sdl_platform.hpp` for
   `console::key_down`, and `sdl_platform.cpp` is not in the Emscripten source
   list, so `cmake --build build-wasm` failed with `#error "To use the
   emscripten port of SDL use -sUSE_SDL or -sUSE_SDL=2"`. The include and the
   key read are now `#ifndef __EMSCRIPTEN__`; the browser keeps the timed dump
   (`OGRE_RAM_KEY_AT_MS`), which needs no key.
2. **S2DEX2 and YUV16 are implemented in `app/src/web_renderer.cpp`.** The New
   Game njpeg backdrop now decodes and draws in the browser
   (`docs/proofs/web-newgame-cathedral-njpeg.png`).
3. **The browser build reads the native debug knobs.** `OGRE_*` query parameters
   become environment variables before the wasm runtime starts, and
   `OGRE_TAP_MS`/`OGRE_TAP_BUTTON` drive controller 0 from `web.js`. Reaching the
   New Game opening no longer needs a throwaway Playwright script.
4. **`make web` builds the browser target** and `.github/workflows/wasm.yml`
   compiles it in CI, which is what session 109's open item 2 asked for.

The remaining renderer gap is RDRAM framebuffer indirection. It is why the
dialogue scene draws an empty background over the backdrop the moment the pass
ends (`docs/proofs/web-newgame-cathedral-covered.png`): the game's 2x2 backdrop
is assembled by CPU copies **out of the RDP framebuffer in RDRAM**, which the
prototype never writes. §4 has the design.

## Evidence

### 1. The build repair

```
app/src/ram_key.cpp:8:10: error: "To use the emscripten port of SDL use -sUSE_SDL or -sUSE_SDL=2"
```
`git log -S'sdl_platform.hpp' -- app/src/ram_key.cpp` gives session 131's
`ram_key` work. The same trap is recorded for `bank_overlays.cpp` in session 109;
nothing in the repository compiled the Emscripten source list between the two.
`make web` now does, and `EM_CACHE` keeps the 40 MB Emscripten sysroot in
`~/.cache/emscripten-ogre`.

### 2. S2DEX2

The GBI is chosen by hashing the task's ucode text with XXH3-64, the way RT64's
`GBIManager::getGBIForUCode` does, and not by address. The two digests and the
recipe were reproduced from the ROM before being written into the code:

```
$ /tmp/ucohash assets/ogre64.z64 0x2F940 0x1390    # F3DEX2 text
off=0x2F940 len=0x1390 raw=0xFF6759AC8CFA5E38 wordrev=0xCF55FAE288BFE48D
$ /tmp/ucohash assets/ogre64.z64 0x35510 0x18C0    # S2DEX2 text
off=0x35510 len=0x18C0 raw=0x8E0740D0047BE932 wordrev=0x9300F34F3B438634
```

`wordrev` is XXH3-64 over each 32-bit word byte-reversed, which is what the
runtime stores in RDRAM and what `tools/RT64/src/gbi/rt64_gbi.cpp`'s
`textSegments` is keyed on (`{0x1390, 0xCF55FAE288BFE48D}` and
`{0x18C0, 0x9300F34F3B438634}`).

The first version of the code hashed `task->t.ucode_size` bytes and never
matched, because that field is `0x1000` (one RSP IMEM load) and not the text's
length. RT64 grows the hash segment by segment and compares the running digest at
each database length; the code now does the same, and the live run confirms the
match:

```
[WEB-GBI] unknown ucode 0x8009F540 size 0x1000 hashes to 0x421A5D3DBF97F6CC   (before the fix)
```

`0x421A5D3DBF97F6CC` is F3DEX2's first 4 KiB (`/tmp/ucohash assets/ogre64.z64
0x2F940 0x1000`).

Once the mode is S2DEX2, `exec_command` decodes `0xDC` as `G_OBJ_MOVEMEM` and
`0xDA` as `G_OBJ_RECTANGLE_R` (and `0x01`/`0x02` as `G_OBJ_RECTANGLE`/
`G_OBJ_SPRITE`) before the F3DEX2 switch, where the same opcodes mean
`G_MOVEMEM`/`G_MTX`/`G_VTX`. The object matrix, the 24-byte `uObjSprite` decode
(field by field from the guest's big-endian words) and the destination rectangle
follow `GBI_S2DEX::drawObjRectangle` in `tools/RT64/src/gbi/rt64_gbi_s2dex.cpp`.
`G_RDPHALF_0` (`0xE4`, S2DEX2's texrect encoding) is **not** implemented: it logs
once if it is ever seen.

The game's own recipe is the one session 51 §2 recorded, and the run reproduces
it command for command:

```
FD30000F <texaddr>   SETTIMG  fmt=1(YUV) siz=2(16b) width=16
F5300400 07080200    SETTILE  t7 fmt=1 siz=2 line=2 tmem=0
E6000000 00000000    RDPLOADSYNC
F4000000 0703C03C    LOADTILE t7 uls=0 ult=0 lrs=60 lrt=60
DC070002 <ptr>       G_OBJ_MOVEMEM  (sub matrix)
DA000000 8019A390    G_OBJ_RECTANGLE_R (the draw)
```

```
[GFX-S2D] obj rect fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(0.0,0.0,1.0000,1.0000) rect=(0.0,0.0)-(16.0,16.0) textured=1 key=0xA11DE6586286C2EF texaddr=0x001D1030
[GFX-S2D] obj rect fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(16.0,0.0,1.0000,1.0000) rect=(16.0,0.0)-(32.0,16.0) textured=1 key=0xA9DA3E17109F05EF texaddr=0x001D1330
[GFX-S2D] obj rect fmt=1 siz=2 img=16x16 stride=2 adrs=0 mtx=(32.0,0.0,1.0000,1.0000) rect=(32.0,0.0)-(48.0,16.0) textured=1 key=0xB29695D5BEB748EF texaddr=0x001D1630
```

The 16-pixel grid, the `0x300` step between macroblock sources and the 16x16
YUV16 image all match session 51.

### 3. YUV16

`decode_texture_rect` gained a `IM_FMT_YUV`/`IM_SIZ_16b` branch. The prototype
decodes textures from their **source** rather than from TMEM, so the branch reads
the source the way `RDP::loadYUVTileToTMEM` and `sampleTMEMYUV16` together imply:
the guest word is `[U, Y(even), V, Y(odd)]` and the runtime stores it
byte-reversed, so a texel's pair is `Y = n64b(word + (tx & 1 ? 3 : 1))`, `U =
n64b(word + 0)`, `V = n64b(word + 2)`, one word per two texels. The RDP's odd-row
64-bit flip cancels between the loader and the sampler for a source read.

`G_SETCONVERT` was previously in the no-op group; it now stores the six 9-bit
fields (RT64 `GBI_RDP::setConvert`'s bit layout, K2 split across the two words),
and `yuv16_to_rgba8` applies the sign extension and the hardware's `2*K+1`
scaling, `R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`, `B = Y + K3*U'`, with the
standard NTSC JPEG fallback, mirroring `TextureDecoder.hlsli`'s
`sampleTMEMYUV16`. `K4`/`K5` as combiner inputs are still returned as 0 by the
fragment shader; that is unchanged and separately noted.

### 4. Reaching the scene, and what it looks like

The New Game opening is reached with query parameters only:

```
http://127.0.0.1:8931/app/web/index.html?OGRE_TAP_MS=1500&OGRE_TAP_BUTTON=start,a,start,a,start,a,a,a,a,a,a,a,a&OGRE_S2D_TRACE=1
```

`debug/probes/atmark.cjs --wait 'GFX-S2D' --shots 6 --gap 200` waits for the
first S2DEX2 draw and captures around it:

| shot | non-black | colourful |
|---|---|---|
| 0 | 307200/307200 | 222378 |
| 1-5 | 32/307200 | 0 |

Shot 0 is `docs/proofs/web-newgame-cathedral-njpeg.png`: the cathedral, drawn at
the game's own 320x240, with one quadrant per njpeg pass, because each pass draws
at its own framebuffer coordinates and the prototype draws all of them to the
canvas. Shots 1-5 are the dialogue scene, which draws the **assembled** backdrop
— assembled on the CPU from framebuffer bytes the prototype never wrote — over
the top; that is `docs/proofs/web-newgame-cathedral-covered.png` and it is the
form the missing framebuffer indirection takes.

The njpeg RSP path runs in the browser: the run logs four `task type=4
ucode=0x8009ED80` submissions whose `data_ptr` values (`0x801D1030`,
`0x801E5620`, `0x801FBEB0`, `0x80208250`) are the four regions session 57 names.

### 5. What the framebuffer indirection needs (design, not landed)

- `G_SETCIMG` currently `break`s. It carries the RDP's colour-image address,
  width and size, which is the render target for everything that follows.
- Every `DrawCmd` must carry that target, and `flush_commands` must render into a
  GL framebuffer object keyed by the guest address (created lazily, size from the
  colour image).
- `ogre_sync_framebuffers()` runs on the **game** thread while GL runs on the
  browser main thread (`web.js`'s 16 ms `ogre_gfx_flush` interval). It must
  become a handshake: the game thread asks for a flush and readback and waits on
  an atomic until the main thread has copied each dirty framebuffer back into
  RDRAM in the RDP's RGBA5551 layout. The game thread is a pthread, so blocking
  it is allowed; the flush must not hold `mutex_` while waiting.
- Presentation then has to read RDRAM: `update_screen()`/`send_dummy_workload(fb)`
  must blit the framebuffer for the VI's scanout address to the canvas instead of
  the draws going there directly.
- The prototype's `RenderState::pending_load`/`tiles` model already carries
  everything a CPU-side readback needs; no new texture path is required.

## Verification run

```sh
# build (the one that was broken)
make web

# title, no regression: same figures session 109 recorded
NODE_PATH=~/dev/fatos/node_modules node debug/probes/boot.cjs --attempts 1 --secs 60 --out web-regress
#   RENDERED {"w":640,"h":480,"total":307200,"nonBlack":24840,"colorful":15108,...}

# the njpeg backdrop
NODE_PATH=~/dev/fatos/node_modules node debug/probes/atmark.cjs --wait 'GFX-S2D' \
  --shots 6 --gap 200 --timeout 240 --out web-cathedral \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_TAP_MS=1500&OGRE_TAP_BUTTON=start,a,start,a,start,a,a,a,a,a,a,a,a&OGRE_S2D_TRACE=1'
```

Both need the static server (`python3 debug/server.py`, port 8931, COOP/COEP) and
a Chromium; `debug/node_modules` is gitignored, so `NODE_PATH` points at another
checkout's `playwright` 1.62.1 install.

**Not verified:** the CI job's first run (no CI access from this machine); the
`OGRE_RAM_KEY_AT_MS` path in the browser (no caller in the web build); and the
rest of the game's screens under the new GBI identification, which now hashes a
ucode on every task (the two digests are checked in order, 0x1390 + 0x18C0 bytes,
about 11 KB per task).

## Files changed

| file | change |
|---|---|
| `app/src/ram_key.cpp` | `#ifndef __EMSCRIPTEN__` around the `sdl_platform.hpp` include and the key read |
| `app/src/web_renderer.cpp` | ucode identification by XXH3-64 (`GbiKind`), S2DEX2 object commands (`set_obj_matrix`, `decode_obj_sprite`, `draw_obj_rect`), the YUV16 decode branch and `yuv16_to_rgba8`, `G_SETCONVERT` handling |
| `app/web/web.js` | `OGRE_*` query parameters published into Emscripten's `ENV`; `OGRE_TAP_MS`/`OGRE_TAP_BUTTON` scripted pad for controller 0 |
| `Makefile` | `make web` |
| `.github/workflows/wasm.yml` | new: builds the browser target on push/PR |
| `debug/probes/atmark.cjs` | new: capture a canvas series around a log marker |
| `docs/proofs/web-newgame-cathedral-njpeg.png`, `-covered.png` | the backdrop, and the dialogue scene drawing over it |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `AGENTS.md`, `docs/guides/app-build.md` | status, decision, index, the `make web` rule and the browser knob rows |

`git status --short` also shows the pre-existing dirty `tools/RT64` submodule
(unchanged by this session; `git -C tools/RT64 diff` is the tree's own state).

## Probes

Throwaway, in `/tmp`, none committed:

- `/tmp/ucohash.c <rom> <off> <len>` — XXH3-64 of a ROM region, raw and
  word-reversed. It is what established the two digests and the `ucode_size`
  mismatch. Rebuild:
  `cc -O2 -I tools/N64ModernRuntime/thirdparty/xxHash -o /tmp/ucohash /tmp/ucohash.c tools/N64ModernRuntime/thirdparty/xxHash/xxhash.c`
- `/tmp/s2dcheck.cjs`, `/tmp/tapcheck.cjs`, `/tmp/texdump.cjs`, `/tmp/yuvdump.cjs`
  — the exploratory drivers that found the tap schedule and read the decoded
  tiles through `ogre_gfx_debug_tex`. Superseded by `debug/probes/atmark.cjs`.

No probe was left in the tree: `git status --short` lists only the files above.

## Open

1. **RDRAM framebuffer indirection** (§5). This is the last of the three gap
   names, and it is what makes the backdrop appear in the scene that uses it.
2. **`K4`/`K5` in the fragment shader** return 0. `G_SETCONVERT`'s last two
   fields are now parsed, so wiring them as uniforms is small.
3. **RDRAM framebuffer as a texture**: once the RDP's framebuffers exist, the
   game's framebuffer-sourced textures should sample them rather than RDRAM.
4. **The web build's audio is unmuted by default in the native meaning**: the
   browser still mutes until `?audio` (session 22), unchanged.
5. **PLAN open work 8** carries the browser status; the CI job in
   `.github/workflows/wasm.yml` needs its first run watched.
