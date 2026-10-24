# Session 189 handoff — the browser renderer ports RT64's writeback dither, fixes the dropped-draw feedback loop, and keys tile images by content

## Goal

Continue the browser WebGL2 renderer as a web port of RT64's renderer. The
session's recorded residuals were session 188's texture-cache churn, session
175's `G_CD` writeback dither, and the session-181 texture-feedback loop that
dropped fills. The developer chose the incremental WebGL2 route over compiling
RT64's own renderer or starting a WebGPU backend (session 189 question).

## Result

Four changes are in the tree; one recorded residual is closed as a measurement
artefact and one screen defect is verified fixed.

1. **`G_CD` colour dither at the RDRAM writeback**, ported from RT64's
   `shared/rt64_framebuffer_pair.cpp:42-44`, `hle/rt64_framebuffer.cpp:231-232`,
   `shaders/Formats.hlsli:10-45` and `shaders/FbWriteColorCS.hlsl:18-20`. Each
   draw counts its `OTHERMODE_H` bits 6-7 into a per-framebuffer
   `dither_patterns[4]`; a writeback uses
   `Framebuffer::bestDitherPattern()` (the most-used mode, first on a tie) and
   `Float4ToRGBA16` adds the pattern value before the 5-bit shift. The pattern
   coordinate is the guest pixel `(x, guest_y)`; `G_CD_NOISE` seeds
   `initRand(seed, guest_y * width + x, 16)` the way `FbWriteColorCS.hlsl`
   does, and `dither_random_seed_` advances once per writeback like
   `State::ditherRandomSeed`. A 32-bit colour image keeps its undithered
   `Float4ToRGBA32` path.

2. **The session-181 WebGL feedback loop is fixed.** `upload_framebuffer` bound
   the colour target's own texture and left it bound. The flush then drew into
   that framebuffer while the program still sampled the texture, so GL rejected
   the draw with `INVALID_OPERATION` (0x0502). The dither control exposed it:
   the fill read `(0,0,0,0)` and `glGetError()` read 502. `upload_framebuffer`
   now unbinds the texture, and `flush_commands` binds the 1x1 `white_tex_` to
   both sampler units before every draw (a textured draw replaces them). The
   same fill now reads `(0.129,0.161,0.192,0)` with error 0.

3. **Tile images are keyed by content, not by `tmem_generation_`.** The GL key
   is `XXH3-64` of the decoded RGBA8 bytes plus the image size and the
   `cms`/`cmt`/`masks`/`maskt` wrap fields. Palette and YUV differences are in
   the decoded bytes; the per-draw tile origin and filter are uniforms, so they
   stay out. A separate per-display-list `decode_key` (the generation key plus
   the wrap fields) still skips a second decode of an unchanged tile within one
   list. The upload loop now skips `glTexImage2D` when the key is resident, and
   the stats line is `tex: cache=N uploads=N reused=N released=N`. The per-flush
   sweep still keeps only the keys the batch names, but a key named by
   consecutive batches now survives and is reused.

4. **S2DEX2 opcode guard.** An audit (this file, §6) found that OB64's only
   S2DEX2 list builder emits `0xDA`/`0xDC`; the implemented `0x01`/`0x02`/`0xE4`
   are unexercised, and `0x05`/`0x06`/`0x07` used to fall through to the F3DEX2
   `TRI1`/`TRI2`/`QUAD` arms. The S2DEX2 arm now returns for `0x03`..`0x0B`, so
   a future list cannot draw from stale vertices. No reached screen changes.

5. **The title logo/copyright residual is the fog animation phase.** A
   native-only series shows two native logo frames 0.25 s apart differ by
   `7.01/7.03/8.77` in the logo band and `9.78/9.78/10.19` in the copyright
   band, with the same signed pattern as the browser-vs-native pair
   (`4.33/4.37/5.39` and `6.09/6.08/6.35`). The browser is closer to native
   than two native frames a quarter second apart, so session 175's
   `5.32`/`8.49` is fog phase and not a renderer gap (this file, §5).

6. **The mission `0x03` party sprites render.** Session 176's frame-locked pair
   had the five-character top-left cluster and the bottom pair absent
   (`26.54` mean abs, browser max 97 against native 255). The same save route,
   after the feedback fix, draws both clusters on the mission map field
   (`docs/proofs/web-mission-party-session189.png`), beside the `Losing
   condition: Death of Prince Yumil` NOTE and the `MISSION` bands. The unit
   sprites' `OTHERMODE_L` (`0x00504240` pass 1, `0x0C184240` pass 2) has alpha
   compare none and `CVG_X_ALPHA` clear, so session 177's `u_ac` arms do not
   apply; the change that can turn a rejected whole draw into a drawn one is
   this session's feedback fix (this file, §7).

## Evidence

### 1. The dither control is exact

`debug/probes/testdraw.cjs --flags 1024 --expect-dither` draws two 320x240
RGBA16 images with fill colour `0x214D` (RGBA5551 `(4,5,6,1)`, decoded to
`(33,41,49,255)`), one with `G_CD_MAGICSQ` and one with `G_CD_DISABLE`. The
renderer reads each target back from RDRAM:

```
[GFX-DITHER-TEST] target=0xCC0000 probe=1 pattern=0 wrote=1
  no_dither=67200 dithered=9600 other=0 rows=[0,239]
  sample=214D 214D 214D 298F
[GFX-DITHER-TEST] target=0xCF0000 probe=2 pattern=3 wrote=1
  no_dither=76800 dithered=0 other=0 rows=[0,239]
  sample=214D 214D 214D 214D
```

`0x214D` is the undithered word. Dither 7 adds one 5-bit step to every channel
and produces `0x298F`. The magic-square table has two 7s per 4x4 block, so
9600 of 76800 pixels change and the rest do not; `G_CD_DISABLE` changes none.
The counts before the feedback fix were `other=76800` on both targets because
the fill draw was rejected.

The writeback diagnostic now reports the chosen pattern and the counts:
`[GFX-FBREAD] ... dither=0 counts=1/0/0/0 seed=0`. A reached screen that reads
RDRAM states its own `G_CD` family this way.

### 2. The feedback loop, before and after

With the dither control on the pre-fix build, the fill's own pixel read was
`glRead=(0.000,0.000,0.000,0.000) err=502`. `0x0502` is
`GL_INVALID_OPERATION`, and the target's texture had been left bound by
`upload_framebuffer`. After the unbind and the white-unit bind, the same read is
`glRead=(0.129,0.161,0.192,0.000) err=0`, and the writeback finds all 76800
pixels. A new `[GFX-GLERR]` line prints the first GL error of a flush (bounded
to 16 lines) so a future binding mistake is visible instead of silently
dropping draws.

### 3. Content-addressed reuse, measured

`testdraw.cjs --flags 2048 --repeat N` calls the synthetic list N times without
yielding, so the lists share one flush batch. `--expect-texcache` requires one
upload and N-1 reuses of the same key:

| repeat | `[GFX-TEXPROBE]` | result |
|---|---|---|
| 2 | 1 upload + 1 reuse | pass |
| 3 | 1 upload + 2 reuse | pass |

Without the content key the old code re-issued `glTexImage2D` for every queued
entry. The title's animated cloud strips legitimately produce new keys each
frame, so an animated screen does not show a high reuse count; the win is for
static screens, HUD sprites and repeated images within one batch.

### 4. Build and battery

`EM_CACHE=<repo>/.emcache make web` is clean. The synthetic battery on the final
build:

```
--flags 1024 --expect-dither   -> magicsq=67200/9600/0 disable=76800/0/0, exit 0
--flags 96  --expect-fbtex     -> fbtex hits=1 lines=1, exit 0
--flags 128 --expect-depth near-> depth control near rgb=(255,0,0), exit 0
--flags 512 --expect-alpha     -> threshold/full=full, none=none, dither=partial, exit 0
--flags 2048 --repeat 3        -> texture cache upload=1 reuse=2, exit 0
```

A forced-title capture before the content-cache change
(`/tmp/w189-title-05.png`, 640x481, `nonBlack=288096`) shows the title menu with
the logo, New Game/Tutorial/Stereo and copyright text. The same screen after
the cache was not re-captured: every natural and forced boot in the later part
of the session landed on the known idle trajectory (`tasks=0`, scene `0x0000`,
no display list after the boot blanking). The page's own `[web:gfx]` line and
`docs/HANDOFF-2026-09-11-session21.md` name that as a game/runtime pacing
stall. The host was loaded during those attempts: `uptime` read load averages
`28.64 20.21 15.40` at 17:35, with Steam, Chrome and Electron processes on the
machine. A 4-attempt, 80 s natural-title run and a 4-attempt forced-title run
both failed the same way under that load. It is not a renderer fault, and no
post-cache game frame is claimed.

### 5. The title logo/copyright residual is the fog animation phase

Session 175 left the title logo band at `5.32` and the copyright band at `8.49`
mean absolute difference against native, with the clouds at `0.07`. A read-only
subagent localised the draws to the two `OGRE_FOG`-repaired fog groups
(`/tmp/p186force10-pagelog.txt:146,171-178`: the logo wrap and the
bottom-of-screen sweep) and showed a signed, contrast-proportional error. The
mechanism is the fog overlay's scroll phase, not a renderer difference.

A native-only series measures the phase. `build-app/ogrebattle64` with
`OGRE_SCENE=title OGRE_SPEED=1 OGRE_CAPTURE_TARGET=/tmp/w189/nat2/t
OGRE_CAPTURE_EVERY_MS=500 OGRE_EXIT_AFTER_MS=60000` captured the logo page.
Two frames from that one run, 15 presents (about 0.25 s) apart:

| band | native `t.1078` vs `t.1093` | native `t.1078` vs `t.1186` |
|---|---|---|
| logo | `meanAbs [7.01, 7.03, 8.77]`, signed `[+3.90, +1.95, -3.82]` | `[11.40, 11.37, 14.00]`, signed `[+6.57, +3.36, -6.24]` |
| copyright | `[9.78, 9.78, 10.19]`, signed `[-5.72, -5.44, -6.26]` | `[16.01, 16.01, 16.65]`, signed `[-9.43, -8.95, -10.31]` |

The browser-vs-native pair measured from
`docs/proofs/web-title-forced-pair-175.png` is smaller than the native's own
0.25 s difference: logo `[4.33, 4.37, 5.39]`, signed `[+2.32, +1.19, -2.19]`;
copyright `[6.09, 6.08, 6.35]`, signed `[-2.98, -2.77, -3.16]`. The signed
direction matches, and the browser/native phase is closer than two native
frames a quarter second apart. The `5.32`/`8.49` bands are therefore within the
native's own frame-to-frame fog variation and are not evidence of a renderer
gap. Numbered rows in `docs/proofs/web-title-forced-pair-175.png` are 2x
panels: browser at `(0,484)`, native at `(644,484)`, 638x478 each; the
`320x240` native targets use half those row numbers (logo `55..110`, copyright
`196..222`).

The same caveat applies to the mission's `7.32` and the tutorial's `0.22`/`0.30`
(session 174's open item 1): those are different fog/phase or cursor-blink
moments, not frame-locked pairs.

### 6. S2DEX2 audit

The audit was a read-only subagent task; its findings are summarized here.

- RT64 defines 0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
  0xDA, 0xDC and 0xE4 (`tools/RT64/src/gbi/rt64_gbi_s2dex2.h:9-25`,
  `rt64_gbi_s2dex2.cpp:75-99`).
- The browser implements 0x01/0x02/0xDA/0xDC and logs 0xE4 without drawing.
- OB64's only S2DEX2 task is the njpeg backdrop (`ucode 0x800A5110`), and its
  sole list builder emits `0xDC070002` and `0xDA000000` per macroblock
  (`BankEFuncs/funcs_0.c:701-708`, verified at ROM `0x686D0`). No other
  S2DEX2 opcode appears in the builder or in any ROM asset.
- The guard removes the stale-vertex fallthrough. Implementing the rest stays
  deferred until a census shows one.

### 7. The mission party sprites render

Session 176's largest residual was the party's character sprites missing from
the mission map field: the top-left cluster `x18..78 y60..98` measured `26.54`
mean abs, 735 of 2280 pixels differed by more than 30, and the browser's
brightest channel there read 97 against native's 255, while the two cursor
brackets and the terrain were drawn. The field is a pre-rendered njpeg
background plus 2D part sprites: 14 two-texture quads per unit (TEXEL0 an I4
mask from an I8 source at TMEM 408, TEXEL1 the RGBA16 colour at TMEM 0), two
`TRI2` per part, emitted twice per frame
(`docs/HANDOFF-2026-10-11-session133.md:85-110`).

The same route now draws both clusters. Command:

```
node debug/probes/atmark.cjs --wait 'id=0x0003' --shots 14 --gap 2000 \
  --timeout 300 --out /tmp/w189-mission2 \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6,0x03:a:30'
```

`docs/proofs/web-mission-party-session189.png` is shot 2 (2x, 638x479). It shows
the five-character cluster at 2x `x36..156 y120..196` (native `x18..78
y60..98`), the two-character bottom party, the `Losing condition: Death of
Prince Yumil` NOTE, the `MISSION` bands, the river, the road and the castle.
The same clusters are in native's right panel of
`docs/proofs/web-mission-framelocked.png`; that proof's browser panel is the
pre-fix build without them.

The attribution is the feedback fix. The unit-sprite draws carry
`OTHERMODE_L 0x00504240` (pass 1) and `0x0C184240` (pass 2); bits 0-1 are
`G_AC_NONE`, and `CVG_X_ALPHA` (0x1000) is clear, so session 177's alpha-compare
arms are no-ops for this family. Depth on/off mission pairs were bit-identical
(session 173), and a content-cache miss paints an opaque white quad rather than
restoring one. `upload_framebuffer` leaving the colour target's texture bound
rejected a draw into that target with `INVALID_OPERATION`; the party's part
quads are exactly the first draws into their target after it is uploaded, and
the fix removes that rejection. The pre-189 build was not re-run for an A/B, so
this is the mechanism most consistent with the change and the draw state, not a
one-variable proof.

### 8. The Load Game white frame did not reproduce at 4 Hz

Session 175 recorded "the Load Game screen is drawn white in the browser before
its content arrives" and attributed it to a session-174 item that does not
contain it. A dense capture of the same transition shows no white frame:

```
node debug/probes/atmark.cjs --wait 'id=0x0012' --shots 20 --gap 250 \
  --timeout 240 --out /tmp/w189-loadgame \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'
```

The 20 shots cover five seconds after the scene-`0x12` marker. Shots 0-2 are the
title/Load Game page (`nonBlack=304936`, `colorful=292244`), shot 3 is the
switch (`304964`/`227852`), and shots 4-19 are the mission map field
(`303208`/`240384`). No shot is white; the lowest `colorful` is `227852` of
305602 pixels. The white frame, if it exists, is shorter than the 250 ms sample
interval. The candidate the code supports is still the game's own white plate
drawn into the heap colour image at `0x228080` (session 170's dump found
`0xFFFD` there), not a renderer fill, but a 60 Hz in-page capture is needed to
settle it. No fix was made. Session 169's "`meanRGB=255` means a fully white
image" is a misreading; the mean is over R+G+B, so 255 is 85 per channel.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `G_CD` writeback dither; `[GFX-FBREAD]` dither census; `[GFX-DITHER-TEST]` control and flag 10; `upload_framebuffer` unbind; white-unit bind per draw; `[GFX-GLERR]`; content-addressed tile key and upload reuse; `reused=` stats; `[GFX-TEXPROBE]` control and flag 11; S2DEX2 0x03-0x0B guard |
| `debug/probes/testdraw.cjs` | `--repeat`, `--expect-dither`, `--expect-texcache`; parse `GFX-DITHER-TEST`/`GFX-TEXPROBE`; render gate uses the max `[GFX-AFTER]` and the offscreen controls |
| `docs/proofs/web-mission-party-session189.png` | the mission map field with both party clusters |
| `docs/HANDOFF-2026-10-24-session189.md` | this file |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and decision rows |

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on every build.
- Synthetic battery above on the final build.
- The pre-fix fill read `(0,0,0,0)` / error 502; the fixed fill read the fill
  colour / error 0.
- The mission route above reached `id=0x0003` and shot 2 of
  `/tmp/w189-mission2` shows both party clusters
  (`docs/proofs/web-mission-party-session189.png`).
- `node --check debug/probes/testdraw.cjs`.
- `grep -rn "probe189" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is empty. The
  one temporary diagnostic (`[GFX-DITHER-DRAW]`, a pixel read inside the flush)
  was removed before the final build; the `[GFX-DITHER-TEST]` and
  `[GFX-TEXPROBE]` readbacks are kept test controls, not probes.

## Probes used and reverted

`[GFX-DITHER-DRAW]` was a temporary probe in the flush; it is gone. No generated
code, ROM data or submodule was touched.

## Open

- The tile image is still decoded once per display list. The content key is
  computed from the decoded bytes, so it cannot skip the decode itself; a
  pre-decode TMEM hash (the shape in `tools/RT64/src/common/rt64_tmem_hasher.h`)
  would close that. The GL upload and object churn are closed.
- The per-flush sweep keeps only the current batch's keys, so a texture used on
  alternating frames is evicted in the gap and uploaded again. RT64 keeps a
  texture for a per-frame age window (`render/rt64_texture_cache.cpp:328-375`);
  a bounded keep window is the next cache improvement, and it needs a byte cap
  so an animated screen cannot retain every frame's images.
- The Load Game white frame was not reproduced in a 4 Hz capture of the
  transition (§8): shots 0-2 are the Load Game page, shot 3 the switch, shots
  4-19 the mission map, and none is white. If it exists it is shorter than
  250 ms. A 60 Hz in-page `canvas.toDataURL` capture around the scene-`0x12`
  marker is the measurement that would settle it; no fix is justified before
  that. The candidate the code supports is the game's own white plate at
  `0x228080` (session 170's `0xFFFD` dump), presented as its own frame. Session
  169's "`meanRGB=255` means a fully white image" is a misreading of the
  counter; the mean is over R+G+B, so 255 is 85 per channel.
- `0x01`/`0x02`/`0xE4` S2DEX2 remain implemented/logged but unexercised; the
  rest of the opcode set is unimplemented and now guarded.
- The mission party capture is a visual check at one map-field phase, not a
  frame-locked numeric pair: `/tmp/w176/nat/t.289.bin` is gone and the note
  overlay differs. The clusters are present and match native's positions; a
  fresh frame-locked pair would make the number.

