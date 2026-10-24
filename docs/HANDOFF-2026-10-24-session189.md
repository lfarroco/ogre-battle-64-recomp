# Session 189 handoff — the browser renderer ports RT64's writeback dither, fixes the dropped-draw feedback loop, and keys tile images by content

## Goal

Continue the browser WebGL2 renderer as a web port of RT64's renderer. The
session's recorded residuals were session 188's texture-cache churn, session
175's `G_CD` writeback dither, and the session-181 texture-feedback loop that
dropped fills. The developer chose the incremental WebGL2 route over compiling
RT64's own renderer or starting a WebGPU backend (session 189 question).

## Result

Four changes are in the tree.

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

4. **S2DEX2 opcode guard.** An audit (`docs/HANDOFF-2026-10-24-session189.md`
   §5) found that OB64's only S2DEX2 list builder emits `0xDA`/`0xDC`; the
   implemented `0x01`/`0x02`/`0xE4` are unexercised, and `0x05`/`0x06`/`0x07`
   used to fall through to the F3DEX2 `TRI1`/`TRI2`/`QUAD` arms. The S2DEX2 arm
   now returns for `0x03`..`0x0B`, so a future list cannot draw from stale
   vertices. No reached screen changes.

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
stall. It is not a renderer fault, and no post-cache game frame is claimed.

### 5. S2DEX2 audit

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

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `G_CD` writeback dither; `[GFX-FBREAD]` dither census; `[GFX-DITHER-TEST]` control and flag 10; `upload_framebuffer` unbind; white-unit bind per draw; `[GFX-GLERR]`; content-addressed tile key and upload reuse; `reused=` stats; `[GFX-TEXPROBE]` control and flag 11; S2DEX2 0x03-0x0B guard |
| `debug/probes/testdraw.cjs` | `--repeat`, `--expect-dither`, `--expect-texcache`; parse `GFX-DITHER-TEST`/`GFX-TEXPROBE`; render gate uses the max `[GFX-AFTER]` and the offscreen controls |
| `docs/HANDOFF-2026-10-24-session189.md` | this file |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and decision rows |

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on every build.
- Synthetic battery above on the final build.
- The pre-fix fill read `(0,0,0,0)` / error 502; the fixed fill read the fill
  colour / error 0.
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
- The title logo/copyright band residual (session 175's `5.32`/`8.49`) and the
  Load Game white frame were not re-measured. Every boot in the later part of
  the session was idle, and no frame-locked pair was produced.
- `0x01`/`0x02`/`0xE4` S2DEX2 remain implemented/logged but unexercised; the
  rest of the opcode set is unimplemented and now guarded.
