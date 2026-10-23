# Session 184 handoff — the QUEST spin's q texture bytes are identical in both renderers; the decode diagnostic and the sampler are ruled out, and three candidate fixes were built, measured and reverted

## Goal

Developer request: *"continue the work on fixing the untextured q's in the game's
intro"*. The open item is `PLAN.md` 20 / `docs/HANDOFF-2026-10-19-session183.md`
§5: the developer reports that in the browser the three `q` meshes have "no
texture, they are solid yellow while rotating", while native "reflects light".

## Result

No code landed. The session's measurements remove the texture data as the cause
and remove three candidate fixes:

1. **At the same game time the three q texture addresses hold identical bytes in
   both renderers**, and the browser decodes them correctly. RDRAM is not the
   difference.
2. **The spin's display list sets `G_TEXTURE_GEN` and installs lookat vectors**;
   implementing them changed no pixel, because RT64's `usesTextureGen` also
   requires `G_LIGHTING` and the sub-list clears it. This is a measured negative,
   not a missing fix.
3. **RT64's per-workload framebuffer writeback** (which the browser does not
   implement) was ported and rebuilt; it changed the q frames for the worse and
   was reverted.

The decode and the sampler are ruled out as well (§6): RT64's decoded-tile
diagnostic shows these tiles going through the deferred load path, so it cannot
name the q image, and all three of the browser's sampler modes produce the same
pose-dependent highlight. The remaining candidates are the interpolated
coordinate and the set of q draws that survives depth; §5 names them.

## 1. Same-moment RDRAM dumps: the q textures are byte-identical

Native: `OGRE_SCENE=publishers`, `OGRE_SPEED=1`, `OGRE_RAM_KEY_AT_MS=<n>`,
`OGRE_RAM_KEY_FULL=1`. Browser: the same knobs through the query string, read
back with `debug/probes/rdram.cjs` and a GPU Chromium launch (the probe's own
default launch is SwiftShader and never leaves the boot).

| dump | native | browser |
|---|---|---|
| 12.5 s | `0x400`, `0x1400`, `0x2400`: 199 distinct RGB, min `(0,0,0)`, max `(248,240,224)` | the same three ranges, the same statistics |
| 14.5 s | the same | the same |
| 15.5 s | the same | the same |

At 15.5 s — inside the QUEST spin — `0x400..0x1400` is **4096 of 4096 bytes
identical** between the two dumps. The three addresses hold the same 4 KB image
(a cloud-like environment map with bright texels), and the image is static
between 12.5 s and 15.5 s. Both dumps at 12.5 s and 14.5 s also agree with each
other.

Two earlier runs that were not usable are recorded here so the numbers are not
confused with these: dumps taken while `OGRE_SCENE=publishers` had not taken
effect (the scene log read `id=0x0009`) show `0x400` all zero in the browser and
the intro-movie frame in native. That is a forced-scene failure, not a finding.
`?OGRE_SCENE=publishers` lands only in some boots (three of five in this
session); check the page log for `forced scene 0x0A is active` before reading a
dump.

## 2. The browser decodes the q texture correctly

A temporary probe (`probe187`) logged every fresh `ensure_tile_image` decode of a
32-bit RGBA tile whose source address is `0x400`, `0x1400` or `0x2400`:

```
[GFX-P187] q tex=0x000400 32x32 line=8 mean=91,75,35 max=248,240,224
[GFX-P187] q tex=0x001400 32x32 line=8 mean=91,75,35 max=248,240,224
```

The mean and the maximum are the same statistics the dump gives for the same 4 KB,
so the browser's TMEM load and `decode_tmem_tile` reproduce the guest image.

Session 183's "brown vertical gradient `[0]=161,138,94` .. `[31]=103,74,60`" is
the same decoder at a different moment. A ring poll over one forced run logged
`mean=139,108,81 max=161,138,94` at `t=7.5 s` (the stills' early phase) and the
`mean=91,75,35 max=248,240,224` decode at the spin.

## 3. The spin's display list, from the 15.5 s native dump

The q sub-lists are at `0x3A49..`. One of them, untruncated:

```
3A4940: DE000000 801B85B8   G_DL the q state block (combiner FC121824, OTHERMODE_L 00552078)
3A4950: D9FFFFFF 00040000   G_GEOMETRYMODE clear=0xFFFFFF set=0x00040000 (G_TEXTURE_GEN)
3A4958: D7000002 07C007C0   G_TEXTURE  scale sc=tc=0x07C0
3A4960: FCFFFFFF FFFCF87C   G_SETCOMBINE  TEXEL0 passthrough
3A4968: DA380000 803A4210   G_MTX (the q rotation)
3A4978: FD180000 80001400   G_SETTIMG    RGBA32 width=1 at 0x1400
3A4980: F5180000 07014050   G_SETTILE    t7 fmt=0 siz=32b line=1 masks=maskt=5 cms=cmt=0 (wrap)
3A4990: F3000000 073FF080   G_LOADBLOCK  0..0x3FF, dxt=0x80
3A49A0: F5181000 00014050   G_SETTILE    t0 (the render tile)
3A49A8: F2000000 0007C07C   G_SETTILESIZE 0..31 x 0..31
3A49C0: DB060038 00026C00   G_MOVEWORD  segment[14] = 0x26C00
3A49D0: DE000000 0E000000   G_DL        the q geometry (G_VTX + TRI2 only)
```

Two consequences:

- The spin's texture coordinate is `s * sc / (65536 * 32)` (RT64
  `RSP::setVertexCommon`, `hle/rt64_rsp.cpp:727-733`). With `sc = 0x07C0` and a
  vertex `s = 1344` (`[probe180]`'s value) that is `1344 * 1984 / 2097152 = 1.27`
  texels, so a q surface samples a small patch of the 32x32 image. The hold's
  `uv0 = (42,56)` of session 183 is the same formula with `sc = 0x10000`.
- The sub-list sets `G_TEXTURE_GEN` **without** `G_LIGHTING` (the `D9` clears all
  three bytes first). RT64's `usesTextureGen` is
  `(geometryMode & (G_LIGHTING|G_TEXTURE_GEN)) == (G_LIGHTING|G_TEXTURE_GEN)`
  (`hle/rt64_rsp.cpp:641-643`), so RT64 does **not** generate the coordinates for
  this draw either. The lookat vectors the state block installs are unused.

## 4. What the two renderers present

`python3 tools/intro_capture.py --interval-ms 100` from a natural boot, the
QUEST spin window in both (native frames 168-191, browser 205-231):

| measurement | native | browser |
|---|---|---|
| frames with a near-white texel (`max channel > 200`) | 24 of 24 | 3 of 14 sampled |
| brightest texel in the spin | `(248,240,224)` | `(247,231,191)` in those 3, `<=193` otherwise |
| q silhouette match at the same pose | mask IoU up to 0.985 | |

A crop pair at the best-aligned poses (native `frame-184`, browser `frame-223`)
shows the same shape and a similar highlight; the browser's highlight is attached
to a different part of the mesh. That is the difference the developer describes.

The 100 ms series also shows the animation clock offset session 183 recorded: the
q's appear about 0.4 s later in the browser and the offset grows, so a frame index
is not a shared clock and the pose has to be matched by silhouette.

## 5. Where the remaining candidates are

The plan was to compare the **sampled texel**. Section 6 ran the first two steps:
RT64's decode diagnostic cannot name the q tile (deferred load), and the sampler
filter/edge behaviour was A/B'd three ways with no change. What remains is the
interpolated coordinate and the set of draws that survives depth:

1. Log the three vertices' `v_tex` for one q triangle in both renderers at the
   same pose. The browser's is `v.s * sc / (65536*32)`; a difference there moves
   the sampled region and is the fix.
2. Count the q draws that pass the depth test per frame in each. Two combiner
   families are drawn for these meshes, and a pass that the browser rejects (or
   draws first) would remove a highlight without changing any texture.

## 6. Continued measurement: the `[dec]` line cannot name the q tile, and no sampler setting changes the spin

RT64's `OGRE_TILE_DECODE` already prints a decoded image's size, format, line,
mean and first texels. It was extended (kept, `tools/RT64/src/hle/rt64_rdp_tmem.cpp`)
with `addr`/`timgw`, the RDP's texture image at upload time, so the q tile could be
identified without hashing. The extension answered the opposite question:

- A forced `publishers` run to 20 s with `OGRE_TILE_DECODE=1` prints 32 `[dec]`
  lines. The burst in the QUEST still (display lists 310..410, `t≈11.7-14.8 s`)
  is a series of `32x32` `fmt=0 siz=3 line=8` decodes: 30 of the 32 lines, every
  one `addr=0x000400` and `nz=0/3072 mean=0.0`. There is **no** 32-bit `line=1`
  decode in the run at all; the only `line=1` decode is a CI4 `16x12` (the other
  non-zero decode is a `64x64` IA tile at `0x249538`). The picture at those times
  is the textured q's.
- The address is therefore the RDP's *current* `G_SETTIMG` at upload time and not
  the tile's, and the CPU TMEM snapshot is zero: these tiles are served by the
  deferred load path (`loadBlockOperation` → `checkRegionsTMEM`), and the
  diagnostic's image fields are not evidence about what the sampler reads. The
  comment in the diagnostic says so now.

The sampler was A/B'd on the same built wasm, because the filter was the
cheapest remaining explanation:

| run | bright (`max channel > 200`) frames in the spin |
|---|---|
| default (`OGRE_TEXF=rt64`, 3-point) | face-on frames only: 3 of 14 |
| `?OGRE_TEXF=1` (shader bilinear) | 2 of 10, `(244,228,170)` and `(244,228,169)` |
| `?OGRE_TEXF=gl` (GL fixed-function) | 2 of 10, `(243,227,168)` at the same poses |

All three agree: the highlight appears only when a q faces the camera, and the
edge-on frames stay at `max channel <= 160`. Native has a texel above 200 in 24 of
24 spin frames. The filter, the wrap and the two sampler paths are ruled out.

What is left of §5 is the interpolated `s,t` and the set of draws that survives
depth. The next measurement is the three vertices' `v_tex` for one q triangle in
both renderers at the same pose, then the number of q draws that pass the depth
test per frame in each.

## Verification

- `EM_CACHE=<repo>/build-wasm/emcache make web` clean after every change and
  after the final revert; `build-wasm/ogrebattle64.wasm` is back to
  `21129871` bytes.
- Native dumps: `OGRE_NO_AUDIO=1 OGRE_MOD_TEST=1 OGRE_SPEED=1
  OGRE_SCENE=publishers OGRE_RAM_KEY_AT_MS=<n> OGRE_RAM_KEY_FULL=1
  OGRE_EXIT_AFTER_MS=<n+1500>` at 7, 12.5, 14.5 and 15.5 s.
- Browser dumps: a GPU Chromium probe with the same query string, dumps read
  back through `Module.FS.readFile` (`debug/probes/rdram.cjs`'s `readFsFile`).
- Web capture: `python3 tools/intro_capture.py --mode web --seconds 30
  --interval-ms 100` (natural boot) and the same tool forced to `publishers`.
- Probe removal: `grep -rn "probe187\|OGRE_PROBE187\|GFX-P187\|kGeomTextureGen"
  app/ debug/` is empty, and `git diff app/src/web_renderer.cpp` is back to the
  six hunks sessions 179/183 carried.
- The synthetic controls on the final build: `testdraw.cjs --flags 96
  --expect-fbtex --expect-hits 1` -> `fbtex hits=1 lines=1`; `--flags 128
  --expect-depth near` -> `rgb=(255,0,0)`; `--flags 384 --expect-depth far` ->
  `rgb=(0,0,255)`.
- The sampler A/Bs in §6: three natural-boot web runs at 100 ms, one each of
  `?OGRE_TEXF=rt64` (default), `?OGRE_TEXF=1` and `?OGRE_TEXF=gl`, with the spin
  windows found from the report's `white=0, colourful>0` rows.
- `make patch-check` PASS after `make patch-fix`; `cmake --build build-app
  --target ogrebattle64 -j` clean.

## Files changed

| file | change |
|---|---|
| `docs/HANDOFF-2026-10-23-session184.md` | this file |
| `PLAN.md` | open item 20 rewritten with the measurements above |
| `docs/STATUS-LOG.md` | session entry |
| `docs/README.md` | handoff index row |
| `docs/DECISIONS.md` | the negative results and the reverted changes |
| `docs/scenes.md` | the QUEST row's browser status |
| `tools/RT64/src/hle/rt64_rdp_tmem.cpp` | `OGRE_TILE_DECODE`'s `[dec]` line gained `addr`/`timgw` (the RDP's texture image at upload) with a comment stating that a deferred tile load makes those fields the next image and that the TMEM snapshot can be zero (measured in §6). Kept as a toolkit diagnostic; `patches/rt64-ob64.patch` regenerated, `make patch-check` PASS |
| `patches/rt64-ob64.patch` | regenerated for that line |

No rendering code changed. The native binary was rebuilt for the diagnostic
(`cmake --build build-app --target ogrebattle64 -j`).

## Probes used and reverted

- `probe187` in `ensure_tile_image` (`OGRE_PROBE187`), one `[GFX-P187]` line per
  fresh q-tile decode. Removed, rebuilt, `grep` empty.
- A per-workload framebuffer writeback in `flush_commands`, mirroring RT64
  `hle/rt64_state.cpp:1655-1670` (`copyRenderTargetToNative` →
  `copyNativeToRAM` for the rows a pair wrote). Built and measured; the q frames
  became darker and flatter than before (spin frames 114-140: no pixel above 200,
  against 3 bright frames in the same window of the clean build), and session 170
  records why a writeback that is not the game's own readback is dangerous.
  Reverted rather than landed; the browser still writes only at
  `ogre_sync_framebuffers()`, which is a known divergence from RT64 and is
  recorded here as such.
- `G_TEXTURE_GEN` and the `MV_LIGHT` lookat slots 0/1 in `transform_vertex`
  (RT64 `computeTextureGen`, `shaders/TextureGen.hlsli:19-33`). Built; the spin
  capture's per-frame statistics were unchanged (pre-fix frames 205-231 and
  post-fix frames 208-232 carry the same values at the same phases, e.g.
  `(2051, 1843, max 247,231,191)` against `(2199, 998, max 244,228,169)`), so the
  `G_LIGHTING` condition in §3 keeps it inert. Reverted: inert code with no
  reached draw is not a fix.
