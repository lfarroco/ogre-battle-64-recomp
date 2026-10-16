# Session 150 handoff — the RDP's scissor does clip a fill, so RT64 (not the browser) was painting the attract story's letterbox olive

## Goal

Settle session 149's open item 1: does the RDP's scissor clip a FILL-mode write?
The answer decides whether RT64's olive letterbox bands on the attract story
(`0x0B`) are a native-renderer bug or the browser's black bands are the accident,
and it decides whether session 84's olive-band branch reading was chasing a
symptom.

## Result

1. **It clips. The reference emulator shows the bands as the second fill's
   black.** RetroArch with the `parallel_n64` core (ParaLLEl-RDP, the accurate
   software RDP) run to the attract story at frame 6600 shows framebuffer rows
   0..16 uniformly `(0,0,0)` (5440 samples of a 4-column grid, all `(0,0,0)`) and
   the bottom band `(0,0,0)` too. The only `(0,0,0)` source in that region is
   fill 2 (`0x00010001` → `0x0001` → black) under a scissor that excludes the
   band rows; fill 4's arm colour would have to be exactly black for the other
   reading. n64brew says the same ("pixels that are outside of this region will
   not be processed in the pixel pipeline and will not be written to memory",
   with FILL/COPY edge notes and fill-triangle hazards). So RT64, which clears
   the rectangle the game asked for, was wrong, and the browser's `glScissor`
   was right.

2. **Fixed RT64.** `RDP::fillRect` (`tools/RT64/src/hle/rt64_rdp.cpp`) now
   intersects the rectangle with the current scissor
   (`state->rdp->scissorRectStack[scissorStackSize - 1]`) before handing it to
   `drawRect`, returns when the intersection is empty, and prints a bounded
   `[rdp] fillRect clipped (a,b)-(c,d) -> (e,f)-(g,h)` line under
   `OGRE_RDP_TRACE`. The intersection uses the scissor as stored, i.e. half-open
   at the right edge, which is what the reference emulator's last column shows
   (item 5).

3. **Verified natively.** The story target's bands go from `(74,90,0)` to
   `(0,0,0)` (rows 0, 5, 17, 194, 235, 239), the scene rows 18..193 keep the
   scene (the small run-to-run differences there are the animated sky), and the
   trace prints

   ```
   [rdp] fillRect clipped (0,0)-(1279,959) -> (0,0)-(1276,956)
   [rdp] fillRect clipped (0,0)-(1279,959) -> (0,72)-(1276,776)
   ```

   **The developer confirmed it on screen**: "the attract screen has the correct
   bg color now! it is black instead of olive!!"

4. **No regression on the two other screens with full-screen fills.** The
   publisher stills are byte-identical: a fresh native capture's
   `tt.120`..`tt.220` equal session 146's `/tmp/w146/nat/tt.140.bin` exactly
   (`tt.80`/`tt.100` are an earlier fade frame). The title against the session-146
   targets is `meanAbs=[2.29,2.49,3.10] exact=81.4%` (session 146/147/148:
   `[2.29,2.48,3.10] / 81.4%`).

5. **Session 149's "the browser's fill scissor is one column narrow" is
   withdrawn as a browser-vs-reference difference.** The emulator's story frame
   leaves framebuffer column 319 holding *stale* content from the previous scene
   (a purple/blue stripe at the right edge, `(122,54,237)` at row 5 and
   `(0,14,144)` at row 50), i.e. the reference does not write that column either;
   the browser's half-open `glScissor` agrees, and its column 319 is black
   because the port's RDRAM starts zeroed. n64brew's "FILL/COPY is inclusive on
   the right edge" note does not show up in the reference's last column, so the
   port keeps the half-open behaviour and the note stays a possible 1-pixel
   nuance. RT64 now behaves the same way (its column 319 and row 239 are black).

6. **Session 84's branch reading, corrected at instruction level.** The two
   immediates it named are at `0x801A1E10` and `0x801A1E80` in `bankA.elf`
   (record 3), and they are not two arms of one colour choice:

   ```
   801a1e0c: lui   a0,0x4f
   801a1e10: ori   a0,a0,0xc308      ; a0 = 0x4F00C308
   801a1e3c: sw    a0,12(v0)         ; ...stored as the DATA word of a
                                     ; G_SETSCISSOR pair (a1 = 0xED000048 at
                                     ; 0x801a1dfc/0x801a1e00), guarded by W & 0x100
   ...
   801a1e70: andi  v0,v1,0x2000
   801a1e74: bnez  v0,0x801a1e84
   801a1e78: andi  v0,v1,0x1000      ; delay slot
   801a1e7c: beqz  v0,0x801a22c8
   801a1e80: li    a0,0x4AC1         ; delay slot: runs whenever bit 0x2000 is clear
   ```

   `0x4F00C308` is the data word of a `G_SETSCISSOR` (command `0xED`, uly 72,
   lry 776, mode `0x4F`), not a fill colour, and `li a0,0x4AC1` is a delay slot
   that executes on both sub-cases of bit `0x1000` when bit `0x2000` is clear.
   Session 84's "bit `0x2000` or `0x1000` set → `a0 = 0x4F00C308`; both clear →
   `0x4AC1`" does not follow from those instructions. It no longer matters for
   this screen either way: the band rows are outside fill 4's scissor, so the
   colour that shows there is fill 2's black whatever the branch does.

## Evidence

### 1. The reference emulator run

```
cat /tmp/w150/ra.cfg      # audio off, no vsync, integer scale, no widgets/cheevos,
                          # screenshot/savestate dirs under /tmp/w150, fastforward unlimited
/Applications/RetroArch.app/Contents/MacOS/RetroArch --appendconfig=/tmp/w150/ra.cfg \
  -L parallel_n64 --max-frames=6600 --max-frames-ss \
  --max-frames-ss-path=/tmp/w150/shots/p6600.png \
  /Users/momo/dev/ogre/assets/ogre64.z64
```

6600 frames is 110 s of emulated time (the intro, the publisher stills, the
title's prologue pages and logo, then the attract story). Frame 600 is the
"Licensed by Nintendo" still, 1800 the title's cloud movie, 3600 the prologue
text, 5400 the logo, 6600 the attract story. `docs/proofs/reference-attract-story-parallel-n64.png`
is that frame, cropped from the 2x screenshot to 320x240.

### 2. The band measurement

`/tmp/w150/shots/p6600.png` is 640x480, i.e. the 320x240 VI image at the
integer 2x `video_scale_integer` was told to use, so screen row `2r` is
framebuffer row `r`:

| framebuffer row | colour |
|---|---|
| 0..16 | `(0,0,0)` |
| 17 | `(3,1,8)` |
| 18 | `(7,2,16)` |
| 19 | `(12,2,25)` |
| 20 | `(16,3,33)` |
| 21 | `(21,4,44)` |
| 22..29 | `(21,4,44)`..`(21,4,45)` |
| 210..239 | `(0,0,0)` |

The gradient rows are the game's own fade-in texrects (the port's browser shows
the same shape: `(15,18,0)`, `(29,36,0)`, ... over the olive fill). The content
begins one row earlier than the browser's row 18 because the VI scans out from
`framebuffer + fldRegs[].origin` (`0x280` = one 320-wide 16-bit row = the
framebuffer's pitch), so the emulator's screenshot row 0 is the framebuffer's
row 1 and its row 17 is row 18 (`AGENTS.md`: the VI's scanout address is not the
RDP's colour-image address).

### 3. The RT64 change

`tools/RT64/src/hle/rt64_rdp.cpp`, `RDP::fillRect`, after the FILL/COPY
lower-right snap and before `drawRect`:

```cpp
        const FixedRect &scissorRect = state->rdp->scissorRectStack[scissorStackSize - 1];
        if (!scissorRect.isNull()) {
            const FixedRect clippedRect = scissorRect.intersection(FixedRect(ulx, uly, lrx, lry));
            if (clippedRect.isEmpty()) {
                return;
            }
            ... trace ...
            ulx = clippedRect.ulx; uly = clippedRect.uly;
            lrx = clippedRect.lrx; lry = clippedRect.lry;
        }
```

`RDP::drawRect` already intersects with the same scissor for the *writeback*
bookkeeping (`fbPair.drawColorRect.merge(intRect)`), and returns early on an
empty rect, which is why an empty clip can simply return here. The clip is
applied before `drawRect` adds the extended-alignment offsets, so a widescreen
alignment of a few pixels could carry a fill past the scissor; no screen has
shown it and `OGRE_RECT_LR_FIX` is off in this port (session 101).

### 4. The story target, before and after

`/tmp/w149/nat/s.160.bin` (session 149) against `/tmp/w150/nat/s.160.bin`:

| row | before | after |
|---|---|---|
| 0, 5, 17 | `(74,90,0)` | `(0,0,0)` |
| 18 | `(3,2,6)` | `(3,3,7)` |
| 100 | `(12,1,30)` | `(13,3,31)` |
| 192 | `(9,6,5)` | `(9,6,5)` |
| 194, 235, 239 | `(74,90,0)` | `(0,0,0)` |

Row 192 (the castle) is identical; the sky rows differ by 1-3 levels because the
two runs are at different animation phases, which is the same reason the story's
scene rows cannot be compared across runs at all.

## Verification runs

| what | command | result |
|---|---|---|
| the reference emulator | RetroArch + `parallel_n64`, `--max-frames=6600 --max-frames-ss` (above) | attract story at 110 s; bands `(0,0,0)`; last column stale |
| native build | `cmake --build build-app -j` | clean |
| the story, after the clip | `OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=story OGRE_SCENE_LOG=1 OGRE_RDP_TRACE=1 OGRE_CAPTURE_TARGET=/tmp/w150/nat/s OGRE_CAPTURE_AFTER=60 OGRE_CAPTURE_EVERY=20 OGRE_EXIT_AFTER_MS=12000 ./build-app/ogrebattle64 assets/ogre64.z64` | bands `(0,0,0)`; `fillRect clipped ... -> (0,72)-(1276,776)`; `[scene] t=3471ms id=0x000B` |
| the publisher stills, no regression | `... OGRE_SCENE=publishers OGRE_CAPTURE_TARGET=/tmp/w150/nat3/tt ... OGRE_EXIT_AFTER_MS=16000` | `tt.120`..`tt.220` byte-identical to session 146's `tt.140` |
| the title, no regression | `... OGRE_SCENE=title OGRE_CAPTURE_TARGET=/tmp/w150/nat4/t ... OGRE_EXIT_AFTER_MS=20000` then `tools/web_frame_cmp.py /tmp/w148/title-00.png '/tmp/w150/nat4/t.*.bin'` | best `t.160`: `meanAbs=[2.29,2.49,3.10] exact=81.4%` |
| the browser renderer, unchanged | not rebuilt | this change is native-only; `app/src/web_renderer.cpp` is not part of the native build and RT64 is not part of the wasm build |
| the RT64 patch | `make patch-check` | FAIL before (`src/hle/rt64_rdp.cpp: the tree's diff differs from the patch`), PASS after `make patch-fix` rewrote `patches/rt64-ob64.patch` (26 files, 2532 lines) |

## Files changed

| file | change |
|---|---|
| `tools/RT64/src/hle/rt64_rdp.cpp` | `RDP::fillRect` clips by the current scissor, returns on an empty intersection, and prints `[rdp] fillRect clipped` under `OGRE_RDP_TRACE` |
| `patches/rt64-ob64.patch` | regenerated by `make patch-fix`, so the hosted release path (which applies the patch files to a pristine checkout) carries the clip; `make patch-check` PASSes |
| `docs/proofs/reference-attract-story-parallel-n64.png` | new; the reference emulator's attract story frame (320x240) |
| `docs/proofs/native-story-bands-fill-clip-side-by-side.png` | new; RT64's story target before (olive bands) and after (black bands) |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session150.md` | this file |

`tools/RT64` is a submodule and is already dirty with the port's own renderer
changes (26 files in the patch, session 50 onward), so this change is added there
and is not committed (rule 10); its tracked copy is `patches/rt64-ob64.patch`,
which this session regenerated. It is a change to the port's renderer, not to
RT64 upstream.

## Open

1. **The attract story's scene rows cannot be compared across runs** (its sky
   animates), and its bands are now fixed on both renderers. The screens to
   compare next are the ones a still does not cover: the New Game cathedral
   backdrop (njpeg/S2DEX2), the world map's sprites, and the tutorial practice
   field.
2. Session 148's items stand: the canvas element's rounded corners clip four
   pixels and the presenter never writes its last column and row (cosmetic;
   `tools/web_frame_cmp.py --inset N`); framebuffer-as-texture (a `G_LOADTILE`
   whose source is a colour image rendered in the same task reads RDRAM); the
   LOADTLUT destination keyed on the load tile's `palette` field where RT64 uses
   its `tmem`; the `fldRegs` x/y scale and offset.
3. The title's two moving-cloud bands are still a phase difference between runs,
   not a renderer difference (the developer states the port's clouds are not
   exact against retail yet).
4. `OGRE_SCENE=publishers` still lands on scene `0x0009` in some native runs and
   `0x000A` in others; this session's runs read `0x000A`.
