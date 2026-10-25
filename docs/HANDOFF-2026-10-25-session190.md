# Session 190 handoff — the browser tile cache is two-tier, and the Load Game series is measured per flush

## Goal

Continue the browser WebGL2 renderer. The session closed the two cache items in
`docs/HANDOFF-2026-10-24-session189.md` and replaced the Load Game white-frame
series with a per-flush capture.

1. The tile image was decoded once per display list because the GL content key
   is computed from the decoded bytes. A pre-decode TMEM key should skip the
   decode.
2. The per-flush texture sweep deleted a texture used on alternating frames.
   The GL cache should keep a bounded age window.
3. The Load Game white frame was measured with a "4 Hz" series, and session 189
   concluded it must be shorter than 250 ms. That conclusion is withdrawn.

## Result

1. **Pre-decode source key and decoded-tile cache.** `tile_source_key` hashes
   the TMEM bytes a tile can read and the fields that decide which bytes those
   are. `decoded_tiles_` maps the key to the decoded pixels in a
   `shared_ptr<const std::vector<uint8_t>>`, capped at 16 MiB and evicted LRU. A
   hit skips `decode_tmem_tile`; the upload still carries the shared pixels, so
   an evicted GL texture is re-created without another decode. The content key
   is recomputed from the pixels plus the wrap fields, so a tile that changes
   only `cms`/`cmt`/`masks`/`maskt` cannot reuse the wrong key.

2. **Age- and byte-bounded GL cache.** `gl_textures_` entries carry their byte
   size and the flush that last named them. An entry survives eight flushes
   (`kTextureKeepFlushes = 8`, RT64's `WORKLOAD_QUEUE_SIZE * 2` floor), and the
   cache is capped at 32 MiB and 16 384 entries. Eviction is LRU, but a key the
   current batch named is never a victim. This replaces the sweep that kept only
   the current batch. The decoded-tile cache repairs a GL eviction without a
   decode.

3. **Measurement correction.** `atmark.cjs` waits `--gap` *after* a Playwright
   element screenshot, and one live screenshot costs 0.95-9.5 s (session 176).
   Its nominal 250 ms series therefore spans tens of seconds. Session 189's
   conclusion ("the white frame is shorter than 250 ms") does not follow from
   its data. `debug/probes/atmarkfast.cjs` wraps `Module._ogre_gfx_flush`,
   coalesces the page's two calls per tick, and reads the default framebuffer
   with `gl.readPixels` after each flush. It arms and disarms from `[scene]`
   lines through `ogreLog.onLine`, with no polling and no full-log scan.

4. **The Load Game route reached scene `0x12` once.** The per-flush series has
   two distinct frames: the Load Game page (`319x239 mean3=120.86
   meanPC=40.29 nonBlack=68357 colorful=48515`) at `t=40.432 s`, and an all-black
   frame at `t=45.913 s` (`mean3=0 nonBlack=0`). No frame is white and no frame
   is a candidate (`fracW240 < 0.25`). After the first `A` tap the game submits
   display lists 1-11 and then stops; the mission is not reached, so the
   `0x12 -> 0x03` transition is not in the series.

5. **The cache works on a natural screen.** The same run's
   `[GFX-TEXCACHE]` census at flush 180 reads
   `live=129 bytes=572928 uploads=129 reused=129 released=0 decodes=179 hits=81`.
   At flush 240, after the game stopped submitting lists, it reads
   `live=0 bytes=0 uploads=246 reused=907 released=246 decodes=309 hits=952
   mismatches=0`. The decode hit rate is 952/1261 = 75.5%, and the trace-only
   re-decode audit reports no mismatch.

## Evidence

### 1. The source key hashes a superset of the decoder's bytes

`decode_tmem_tile` reads TMEM at `(t.tmem << 3) + y*(line << 3) + (x << shift >> 1)
+ extra`, masked by `address_mask = (rgba32 || indexed) ? 0x7FF : 0xFFF`, with
`shift = rgba32 ? 2 : t.siz`, plus the palette at `0x800` for an indexed tile.
`tile_source_key` uses the same mask and shift, hashes at least
`((w-1) << shift >> 1) + 1 + (shift >= 2 ? 1 : 0) + 4` bytes per row, hashes the
whole 2 KiB palette half when indexed, and mixes `tmem`, `line`, `fmt`, `siz`,
`palette`, `w`, `h`, `lut_mode`, and `convert_k[0..5]` only for YUV16.

The two false-hit cases a first draft had are fixed:

- the mask was `rgba32 ? 0x7FF : 0xFFF`; an indexed CI tile at `tmem >= 0x100`
  would have been hashed from a different half than the decoder reads;
- `row_bytes` used `min(siz, 2)`, which under-covers a non-RGBA 32-bit tile
  whose shift is 3.

The key is conservative: a superset costs a false miss, never a stale image.

### 2. Synthetic controls

`testdraw.cjs --flags 2048` marks the synthetic list's uploads and the renderer
prints `[GFX-TEXPROBE] ... result=upload|reuse decode=hit|miss`.

| command | result |
|---|---|
| `--flags 2048 --repeat 3 --expect-texcache` | `upload=1 reuse=2 decode_hit=2` |
| `--flags 2048 --repeat 4 --gap 64 --expect-texcache` | `upload=1 reuse=3 decode_hit=3` across four flushes |
| `--flags 6144 --repeat 3 --gap 64 --expect-sourcechange` | `keys=3 miss=3 hit=0` |
| `--flags 2048 --repeat 3 --expect-texcache` with `OGRE_TEX_TRACE=1` | 24 `[GFX-TEXTRACE]` lines, 0 `[GFX-TEXCACHE-MISMATCH]` |

The third row is the negative control: `--flags 4096` writes a different red
texel on every call, so a key missing that byte would hit the first entry and
report fewer keys or a hit.

The regression battery on the final build:

```
--flags 1024 --expect-dither   -> magicsq=67200/9600/0 disable=76800/0/0
--flags 96   --expect-fbtex    -> fbtex hits=1 lines=1
--flags 128  --expect-depth near-> near rgb=(255,0,0)
--flags 512  --expect-alpha    -> threshold/full=full none=none dither=partial
```

### 3. The Load Game series

Command (the successful run):

```
node debug/probes/atmarkfast.cjs --pre 0 --timeout 120 \
  --out /tmp/w190-loadgame \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6'
```

It armed at `scene=0x0012` at `t=40.206 s`, sampled 515 times (838 flush calls,
4.3 effective Hz, max gap 5.26 s) and recorded two distinct frames:

```
n=1 t=40.432s flushN=318 scene=0x0012 taskN=8  319x239 mean3=120.86 meanPC=40.287 nonBlack=68357 colorful=48515
n=2 t=45.913s flushN=327 scene=0x0012 taskN=10 319x239 mean3=0     meanPC=0      nonBlack=0     colorful=0
```

The scene timeline is `0x0000 -> 0x19E8 -> 0x0000 -> 0x0004 (forced title at
23.4 s) -> 0x0012 (39.9 s)`. The six `0x0012` A taps fire at 4 s intervals
after the scene line; display list 11 is submitted at 43.87 s, then the game
stops submitting lists. The page log has no error, and the audio microcode tasks
continue. The first frame is the Load Game page, not a white plate.

Six further attempts under host load 16-27 landed on the known idle trajectory
(`[scene] t=... id=0x0000`, `[scene-trace] ... block=0x00000000`), and the
scene forcing left `pending=0x0004` for a dispatcher that never ran. Three
later attempts at load 3.7-4 also failed: two idled before the title, and one
reached the title and stalled before `0x12`. The capture is unrun for the exact
entry instant and for the `0x12 -> 0x03` transition; it is not disproved.

### 4. Session 170's `0x228080` candidate

The `0xFFFD` halfwords session 170 found at `0x228080` were written by the
per-present framebuffer writeback that session 170 then removed; the current
code writes back only on the game's sync request. The hypothesis that the game
draws a white plate there is still open, and the dump does not support it. The
new probe records a sparse `0xFFFD` count in that image on a flagged frame
(`rdramWhite`) so the two cases are distinguishable: a guest plate decodes to
`(255,255,247)`, the renderer's missing-texture fallback is
`(255,255,255)`.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `tile_source_key`, `hash_tmem_bytes`, `decoded_tiles_` cache, `GlTextureEntry` age/byte cache, `shared_ptr` uploads, `decodes=`/`decode_hits=` stats, `[GFX-TEXCACHE]` census, trace-only `[GFX-TEXCACHE-MISMATCH]`, test flag bit 12 source-change control |
| `debug/probes/testdraw.cjs` | `--gap`, `--expect-sourcechange`, `decode=` in the cache assertion |
| `debug/probes/atmarkfast.cjs` | new per-flush canvas probe with `--arm`/`--disarm`/`--pre`/`--pre-from`, readPixels metrics, `0xFFFD` count |
| `docs/guides/web-probes.md` | cache controls and the per-flush probe |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | status and decision rows |
| `docs/HANDOFF-2026-10-25-session190.md` | this file |

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean.
- Synthetic controls above, all exit 0.
- `node --check debug/probes/testdraw.cjs`, `node --check debug/probes/atmarkfast.cjs`.
- The natural Load Game run above: 952 decode hits, 309 decodes, 0 mismatches.
- `grep -rn "probe190" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is empty. No
  temporary probe was added; `[GFX-TEXCACHE*]` and the test flags are kept
  diagnostics.

## Probes used and reverted

None. No generated code, ROM data or submodule was touched. `tools/RT64` was
already dirty from an earlier session and is unchanged by this work.

## Open

- The Load Game white-frame question is still unrun. The route reached `0x12`
  once and showed the page and then black; the exact entry instant and the
  transition to `0x03` need a run that reaches them. `atmarkfast.cjs
  --pre-from 'id=0x0004' --pre 40 --arm 'id=0x0012'` is the command prepared for
  that; the game stalled after the first `A` in every attempt that reached
  `0x12`.
- Why the game submits display lists 1-11 and then stops after the first `A` on
  the Load Game screen is a new question. It is not renderer-side: the flush
  count keeps rising and the audio tasks continue, but no new display list is
  submitted.
- The natural-screen cache numbers are not frame-locked: the Load Game run
  stopped before the mission, so the mission's party sprites and the `0x03`
  screen were not re-checked after the cache change.
- The byte caps are compile-time constants; an `OGRE_TEX_CACHE_MB` override was
  considered and not added.
- `decoded_tiles_` eviction scans the map for the LRU victim. It is bounded by
  16 MiB / a few hundred entries and did not show in a profile.
