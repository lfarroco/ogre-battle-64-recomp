# Handoff — 2026-10-11, session 134: the black head is RT64's framebuffer tile copy serving a region the framebuffer never rendered

> **Summary.** Session 134 handoff — **the black map-unit head is fixed: RT64's framebuffer tile copy was serving texels the framebuffer never rendered.** The head's 16x15 colour tile never reached `TextureManager::uploadTexture` (13 of the unit's 14 colour tiles do), because `FramebufferManager::checkRegionsTMEM` answered it from a framebuffer at masked `0x2E6840` whose `lastWriteFmt` is `G_IM_FMT_DEPTH`; that framebuffer's declared range reaches into the heap where `func_8019ABC4` builds the head-down bitmap (`0x802FB0E8`), and `createTileCopyRecord` fills a tile copy from the framebuffer's `lastWriteRect` only, so the tile (`468,65..484,67`) sampled a copy that was never written: `TEXEL1` read zero and `(1 - TEXEL1) * ENV + TEXEL1` painted the flat `ENV >> 3` = `0x2947`. The fix refuses a copy whose texels lie outside `lastWriteRect`, so the load reads RDRAM. A blanket refusal of depth sources also fixed the head but changed one scenery scanline (a one-row strip legitimately samples the same framebuffer's rendered rows), and a freshness guard against `fb->RAMHash` fires only on some frames because `hashTracking` re-hashes every framebuffer at each frame end. Verified from the fault frame's dump: 1 pixel of `0x2947` in the head's box against 76 before, the correct art, the scenery row byte-identical to its pre-fix values, `make patch-check` PASS, developer-confirmed. New: `debug/head-trace.sh`, `debug/head-crop.py`, `debug/black-sprite-133/rdpmodel.py`, and `OGRE_SPRITE_TRACE`/`OGRE_TILE_DECODE`/`OGRE_FB_GPU_COPY`.

Developer request: *"read the latest handoff docs about the black head sprite, and
continue the work on fixing the issue. I can help drive runs to test"*, then five
driven runs on `scene_14.bin` and the report *"the character's head is black"*,
*"the issue still happens ... in the first one, the head sprite was flashing
between the expected and black"*, and finally *"it works now! the issue is gone"*.

**Result.** The fault is found and fixed. Session 133 left one question — why the
head's `TEXEL1` samples as zero while its RDRAM, its tile descriptors and its
combiner are all right — and the answer is that this tile never becomes a texture
at all. RT64's **framebuffer tile copy** serves it: the load's source range
(`0x802FB0E8`, the runtime block `func_8019ABC4` builds) overlaps a framebuffer
RT64 has on record at masked `0x2E6840` whose recorded format is `G_IM_FMT_DEPTH`,
so `checkRegionsTMEM` answered the sampling tile from that framebuffer's GPU copy
instead of from the texture cache. **`FramebufferManager::createTileCopyRecord`
fills a tile copy from the framebuffer's `lastWriteRect` only** (it calls
`colorTarget.copyFromTarget(..., r.left, r.top, r.width, r.height)` with
`r = fb->lastWriteRect`, then `discardLastWrite()`s it), and the head's tile
(texels `468,65..484,67`) lies outside that rect, so the copy it sampled was never
written: `TEXEL1` read `(0,0,0)` and the combiner `(1 - TEXEL1) * ENV + TEXEL1`
painted the frame's ENV colour, `0x2947`, over the mask's correct silhouette.

The fix refuses a tile copy whose texels are not inside the framebuffer's
`lastWriteRect`, so the load reads RDRAM the way hardware does. The developer
confirmed the head renders, and the frame in the last run's dump measures it: the
head's box holds **1** pixel of `0x2947` against **76** before, and the one scenery
scanline an earlier, too-broad version of the fix had changed is byte-identical to
its pre-fix values again.

## 1. The instrument that found it: which path does each tile take

Session 133 could not see the fault because `OGRE_TILE_TRACE_ALL=1` prints nothing
for a tile that is served by a copy, and `OGRE_TILE_TRACE_MISS=1` prints nothing
when the texture cache has the texture — which it did. The new
`OGRE_SPRITE_TRACE=1` prints one line per small tile **after** the upload decision,
with the draw-call state and the resolved hash, and `OGRE_TILE_DECODE=1` prints the
RGBA the renderer decodes out of the TMEM snapshot it is hashing (a CPU copy of
`TextureDecodeCS.hlsl`'s `sampleTMEM`) and the raw-TMEM path that never reaches
`uploadTexture`. Both are in `patches/rt64-ob64.patch`; `debug/head-trace.sh` runs
both plus `OGRE_TILE_TRACE_ALL=1 OGRE_TILE_TRACE_MAXH=64` and the whole-image key
press.

The first run's log gave the fault in one line: **13 of the unit's 14 colour tiles
have a `[dec]` line and the head's does not.**

```
colour line=3 tmem=0 12x19 -> 528      (draw 54)
colour line=5 tmem=0 20x30 -> 528      (draw 55)
...
colour line=4 tmem=0 16x15 -> 0        <- the head, never hashed
colour line=4 tmem=0 16x23 -> 528      (draw 62)
```

`[dec]` is printed by `TextureManager::uploadTexture`, so the head's colour tile
was not uploaded. The `[spr]` line for the same tile (from the second run, with the
cap raised and the filter narrowed to two-texture draws) says which branch it took.

## 2. What the tile was served from

`OGRE_TILE_DECODE=1` also prints a line from `checkRegionsTMEM` for every tile
whose TMEM range is covered by a region that has a framebuffer tile copy:

```
[fbcopy] tmem=0..60 lineWidth=16 siz=2 fmt=0 tileId=0000000000000008 fb=0x2E6840
         fbTile=468,65..484,67 fbLine=16 fbSiz=2 fbFmt=5 reinterpret=0
[fbcopy] tmem=0..60 lineWidth=16 siz=2 fmt=0 tileId=0000000000000002 fb=0x2E6840
         fbTile=468,65..484,67 fbLine=16 fbSiz=2 fbFmt=5 reinterpret=0
```

`fbTile.address` is `fb->addressStart` (`makeFramebufferTile` sets
`outTile.address = fb->addressStart`), so the framebuffer is at masked `0x2E6840`
= guest `0x802E6840`, inside the same heap as the display-list buffers
(`0x802E2490`) and the sprite block (`0x802FB0E8`). `fbFmt=5` is
`G_IM_FMT_DEPTH` (`rt64_f3d_defines.h`), which is what
`State::submitFramebufferPair` writes into `depthFb->lastWriteFmt` on a depth
write. The region is covered for `tmem=0..60`, i.e. the colour tiles' TMEM range
(`(line << 3) * (height - 1) + lastRowBytes` = 480 bytes = 60 words for the head's
16x15 tile, and the same for a 12x20 tile).

The mask tile is not affected, and that is not luck: `checkRegionsTMEM` accepts a
copy only when the line width and the pixel size match, and the mask's
`siz = G_IM_SIZ_4b` against the framebuffer's `siz = G_IM_SIZ_16b` fails both
compatible-size branches. The mask therefore stayed a texture upload, which is why
its silhouette was always correct.

`0x2E6840` appears **nowhere** in the fault frame's 8 MiB image (`0x802E6840`,
`0x000E6840` and `0x2E6840` all have zero occurrences at any 2-byte alignment), so
the depth image was set in an earlier frame or scene whose display list is no longer
resident. `FramebufferManager::get` sizes a framebuffer
`addressStart + imageRowBytes(width) * height` with the **colour** image's width
and `framebufferHeightForDisplay(drawnBottom, viHeight)`'s height, and the copy's
own numbers give a row stride of 1284 bytes (width 642) and a row of 65 for the
load's offset `0x2FB0E8 - 0x2E6840 = 0x148A8`. A 642-wide, 240-row 16-bit image is
308 KB, and the sprite bitmap is inside it.

## 3. The mechanism, and the one call that explains the zero

`FramebufferManager::createTileCopyRecord` (`rt64_framebuffer_manager.cpp:183`)
builds the copy:

```cpp
const FixedRect &r = fbIt->second.lastWriteRect;
colorTarget.copyFromTarget(renderWorker, &depthTarget, r.left(false), r.top(false),
                           r.width(false, true), r.height(false, true), shaderLibrary);
fbIt->second.discardLastWrite();
```

**Only the framebuffer's last write rect is copied.** A tile that lies outside it
samples a colour target that was never written for those texels, so `TEXEL1` reads
zero and the combiner returns ENV. That is the whole fault: the tile is inside the
framebuffer's *declared* range and outside everything the framebuffer ever
*rendered*.

Two consequencs of that reading were checked. Session 133's H1 (the renderer reads
the texture late) and H2 (the game fills the block after the draw) are both dead:
`uploadTexture` copies the whole 4 KiB of TMEM into the upload batch synchronously
(`newUpload.bytesTMEM = std::vector<uint8_t>(bytes, bytes + bytesCount)`), and this
tile was never uploaded anyway. The `OGRE_TILE_TRACE_MISS` and `found=` traces could
not have found it: the copy branch prints nothing at all.

## 4. The first attempt was too broad, and the second run caught it

The first fix refused **any** tile copy whose framebuffer was a depth framebuffer,
on the reading that a depth image is not colour data. It fixed the head
(`[fbcopy-depth] depth framebuffer 0x2E6840 refused as colour source tmem=0..60`)
and the developer's next report was *"the issue still happens ... in the first one,
the head sprite was flashing between the expected and black"*. Two things came out
of that pair of runs:

* **The flashing was my freshness guard, not the fault.** The first version also
  refused a copy when `XXH3_64bits(RDRAM[fb]) != fb->RAMHash`. That test fires only
  on the frames where RDRAM changed since RT64 last synchronised with it, and
  `FramebufferManager::hashTracking` re-hashes **every** framebuffer at the end of
  every frame (`rt64_state.cpp:1788`, under `renderToRDRAM`) without touching the
  GPU copies. So most frames passed the test and took the copy. The log has both
  outcomes for the same tile: one `[dec]` line for the head's colour
  (`nz=347/720 mean=53.0`) and two `[fbcopy]` lines for it.
* **The depth refusal changed one scenery scanline.** A diff of the frame the
  developer saved either side of the second change puts 256 pixels apart, of which
  123 are the head and about 50 are one row of the field at y=120. That row is a
  one-row strip that legitimately samples the *same* framebuffer's rendered rows
  (RT64's depth-to-RGBA16 copy converts them, which is what makes it terrain rather
  than the raw depth words), so refusing every depth source broke it. Its values
  before and after the broad refusal:

  | y=120, x=110..119 | values |
  |---|---|
  | before (and after the final fix) | `5a49 624b 51c7 5a09 5a09 628b 6a8b 628b 72cd 7b0f` |
  | with the depth refusal | `4b62 495a 095a c751 8b62 095a 8b62 8b6a 0f7b cd72` |

  So the two cases cannot be separated by the framebuffer or its format. They are
  separated by **which texels the copy actually contains**, which is
  `lastWriteRect`.

## 5. The fix

`FramebufferManager::checkRegionsTMEM` now refuses a copy it would otherwise
accept when the tile's texels are not inside the framebuffer's `lastWriteRect`:

```cpp
Framebuffer *copyFb = find(it->fbTile.address);
const bool insideWrite = (copyFb != nullptr) && !copyFb->lastWriteRect.isNull() &&
    copyFb->lastWriteRect.fullyInside(FixedRect(it->fbTile.left * 4, it->fbTile.top * 4,
                                                it->fbTile.right * 4, it->fbTile.bottom * 4));
if (!insideWrite) { validCopy = false; }
```

`lastWriteRect` is in the fixed-point units `merge`/`left`/`top` use, hence the
`* 4`; `fbTile`'s fields are whole framebuffer texels. **`FixedRect::fullyInside`
reads backwards for its name**: `A.fullyInside(B)` returns true when **B** is
inside **A** (`rt64_common.cpp:87`: `(rect.ulx >= ulx) && ...`), so the call is
`lastWriteRect.fullyInside(tileRect)`.

A refusal leaves the region in `activeRegionsTMEM` with an invalid `fbTile`
(`RegionTMEM newRegion = { }`), so `checkRegionsTMEM` skips it, `tileCopyUsed`
stays false and `uploadTile` calls `uploadTexture` on the TMEM the load replay
wrote from RDRAM. The freshness guard stays as well, tagged `[fbcopy-stale]`; it
can only turn a copy into an RDRAM read, and its limitation is in §7.

`OGRE_FB_GPU_COPY=0` (`app/src/renderer.cpp`) turns RT64's
`emulatorConfig.framebuffer.copyWithGPU` off for one run, which removes the whole
copy path. It was the planned fallback and was not needed.

## 6. What was run for verification

| run | evidence |
|---|---|
| `debug/head-trace.sh`, run 1 | `[dec]` for 13 of the unit's 14 colour-tile shapes and 0 for `fmt=0 siz=2 line=4 tmem=0 16x15`; 662 `[dec]` lines for the head's mask shape `fmt=4 siz=0 line=1 tmem=408 16x15` |
| the same, run 2 (freshness guard only) | `[fbcopy] tmem=0..60 … fb=0x2E6840 fbFmt=5` twice; `[fbcopy-stale] … storedHash=8933DF4944E3B848 rdramHash=7D9E98458CD0E21B load=0x2FB2D0..0x2FB348`; 1 `[dec]` line for the head's colour, so the guard fired on some frames only |
| the same, run 3 (depth refusal) | `[fbcopy-depth] depth framebuffer 0x2E6840 refused … tmem=0..60 siz=2 fmt=5 (sampling siz=2 fmt=0)`; the head's colour uploaded (`nz=347/720 mean=53.0`); the frame put 256 pixels away from the previous run, 123 of them the head and ~50 one scenery row |
| the same, run 4 (final fix) | `[fbcopy-outside] fb=0x2E6840 fmt=5 tile=468,65..484,67 writeRect=null tmem=0..60 siz=2 fmt=0 -> copy refused, reading RDRAM`; `[fbcopy]` count 0; the head's box has **1** pixel of `0x2947` (was 76) and the correct sprite art; `y=120` is byte-identical to the pre-fix values in §4; **developer-confirmed** |
| `tools/ogre_frame.py` / `debug/head-crop.py` on each `ram-1-full.bin` | the frame rendered from the dump, so the head is checked without a screenshot |
| `make patch-check` | PASS, after `tools/patchcheck.py --fix` (2431 lines, 26 files) |
| `grep -rl probe RecompiledFuncs/ Bank*Funcs/ app/` | empty: no generated code and no probe was touched |

## 7. Corrections and what is left open

* **Session 133's §14 reading is right about the cycle type and wrong about where
  to look.** `FC62FE03 2F0D73FD` is a 2-cycle combiner and the head's draw is
  `G_CYC_2CYCLE` (`H = 0x001A2CF0`). A first walk of this session read the cycle
  type as `1CYCLE` because it applied the *F3D* `setOtherModeH` decode
  (`size = p0(0,8)`); `GBI_F3DEX2::setOtherModeH` is
  `size = p0(0,8) + 1; off = max(0, 32 - p0(8,8) - size)`, and with that decode
  `E3001402` (a `G_MDSFT_TEXTCONV` write) no longer clears the cycle bits. Any
  other walker of these lists needs the F3DEX2 form. **The session's `TEXEL1` =
  zero is a symptom of the copy path, not of the sampler**: `checkRegionsTMEM`
  never reached `uploadTexture` for this tile, so there was no texture to sample.
* **A freshness test against `fb->RAMHash` is not reliable on its own.**
  `hashTracking` re-hashes every framebuffer at the end of each frame, which
  records RDRAM the GPU copy does not have. The shipped fix does not depend on it
  (the write-rect test is CPU state and does not vary per frame), but the guard
  that is in the tree fires only on some frames and should not be used as a
  completeness argument.
* **The writer of the depth image at `0x802E6840` is not identified.** The address
  is not in the fault frame's image, so the command is from an earlier frame or
  scene. `OGRE_RDP_TRACE=2`'s colour-image histogram and a new print of each
  `FramebufferManager::get` would name it; neither was run.
* Not re-checked: every other scene with a framebuffer-as-texture use. The refusal
  only removes copies whose texels the framebuffer never rendered, so a legitimate
  copy is unaffected, but the measurement is one screen.

## 8. Files changed

| file | what |
|---|---|
| `tools/RT64/src/hle/rt64_framebuffer_manager.cpp` | the write-rect refusal in `checkRegionsTMEM` and its `[fbcopy-outside]`/`[fbcopy-depth]` diagnostics |
| `tools/RT64/src/hle/rt64_rdp.cpp` | the RDRAM freshness guard in `checkFramebufferOverlap` and its `[fbcopy-stale]` line |
| `tools/RT64/src/hle/rt64_rdp_tmem.cpp` | `OGRE_TILE_DECODE=1`: the CPU decode of the hashed TMEM, deduplicated by hash, plus the raw-TMEM path |
| `tools/RT64/src/hle/rt64_state.cpp` | `OGRE_SPRITE_TRACE=1`: one line per small tile with the draw-call state, the descriptor, the resolved hash and the RDP/GPU tile fields |
| `tools/RT64/src/render/rt64_framebuffer_renderer.cpp` | `[gpu-tile] copy=1` for the tile-copy branch, which printed nothing before |
| `app/src/renderer.cpp` | `OGRE_FB_GPU_COPY=0/1` |
| `debug/head-trace.sh` | the one-command driven run (diagnostics + save + whole-image key press) |
| `debug/head-crop.py` | render the unit's screen area out of a dump |
| `debug/black-sprite-133/rdpmodel.py`, `-analyze-spr.py`, `-pngwrite.py` | an offline model of RT64's TMEM load and sampler, the log reader, and a stdlib PNG writer |
| `debug/black-sprite-134/` | the evidence images: `fault-head.png`/`-unit.png` from session 133's pre-fix dump (`frame-capture/ram-4-full.bin`) and `fixed-head.png`/`-unit.png` from the last run's (`head-trace/ram-1-full.bin`), rendered by `debug/head-crop.py` |
| `patches/rt64-ob64.patch` | regenerated (2431 lines, 26 files), `make patch-check` PASS |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/guides/app-build.md`, `docs/notes-black-sprite.md` | the record |
| `docs/HANDOFF-2026-10-11-session134.md` | this file |

The session-132/133 files (`debug/frame-capture.sh`, `debug/black-sprite-133/`,
`tools/ogre_frame.py`, `tools/ogre_ram.py`, `docs/guides/sprite-mechanisms.md`,
their handoffs) were uncommitted when this session started and are left as they
were, apart from `docs/notes-black-sprite.md` gaining §10.
