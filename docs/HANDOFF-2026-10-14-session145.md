# Session 145 handoff — the browser's RGBA32 decode read the texel one byte off, and that was the title and story hue shift

## Goal

Continue the web renderer (developer, session start). Session 144's open item 1
was the browser's title and story frames being hue-shifted against RT64's target
for the same text. Session 144 concluded from its own trace that the decode was
right and the mechanism was in "the GL sampling or the blend".

## Result

1. **The mechanism is the decode.** `decode_texture_rect`'s `IM_SIZ_32b` case
   read the texel as two big-endian halfwords it called `(alpha,red)` and
   `(green,blue)`, so `rgba[0] = hi & 0xFF` came out as the source G,
   `rgba[1] = lo >> 8` as the source B, `rgba[2] = lo & 0xFF` as the source A and
   `rgba[3] = hi >> 8` as the source R. Every RGBA32 texture the game draws went
   through that case. On the title screen, whose 320x240 RGBA32 backdrop is drawn
   as 3-row strips, a brown-to-white sky decoded magenta-to-white; session 143
   measured a hue shift on the attract story's frames as well, and they use the
   same case.
2. **RT64 reads an RGBA32 texel as four guest bytes R, G, B, A.**
   `RDP::loadWord<RGBA32>` (`tools/RT64/src/hle/rt64_rdp.cpp:527`) puts guest
   bytes 0/1 of each texel in the lower half of TMEM and bytes 2/3 in the upper
   half, and `sampleTMEMRGBA32`
   (`tools/RT64/src/shaders/TextureDecoder.hlsli`) rebuilds
   `(R<<24)|(G<<16)|(B<<8)|A` from those four bytes. `tools/ogre_frame.py`'s
   `fmt == 0, siz == 3` case (`return img.byte(o), img.byte(o+1), img.byte(o+2),
   img.byte(o+3)`) decodes the same order from a dump. The browser's case now
   does the same.
3. **The browser trace's own numbers were the proof.** For the title's backdrop
   (`img=0x217508`, `fmt=0 siz=3 stride=1280`, 3-row rectangles) the trace's mean
   and opaque count moved from the `(G,B,A,R)` decode of the source bytes to the
   `(R,G,B,A)` decode of them, and both before and after values reproduce an
   offline decode of a dump of that image exactly (section 2).
4. **The frame now matches RT64's.** The browser canvas against RT64's own
   `OGRE_CAPTURE_TARGET` of the same title frame: mean absolute difference per
   channel 40.7 before the change and 5.1 after, with one sampled pixel identical
   and the pixel session 144 measured moving from `(53,6,70)` to `(112,91,21)`
   against RT64's `(118,98,31)` (section 3).
5. **Session 144 §4 is corrected.** It compared the browser's decode against the
   browser's canvas, and both agreed, because the trace's `mean`/`opaque` are
   computed by the decode under test. A decode that agrees with the trace that
   measures it is not evidence about the decode.

## Evidence

### 1. The two decoders

The browser's case before the change (`app/src/web_renderer.cpp`):

```cpp
case gbi::IM_SIZ_32b: {
    // RGBA32: two 16-bit halves: (alpha,red) (green,blue).
    const uint16_t hi = n64be16(rdram, byte_addr);       // guest bytes 0,1
    const uint16_t lo = n64be16(rdram, byte_addr + 2);   // guest bytes 2,3
    rgba[0] = static_cast<uint8_t>(hi & 0xFF);   // = guest byte 1 = G
    rgba[1] = static_cast<uint8_t>(lo >> 8);     // = guest byte 2 = B
    rgba[2] = static_cast<uint8_t>(lo & 0xFF);   // = guest byte 3 = A
    rgba[3] = static_cast<uint8_t>(hi >> 8);     // = guest byte 0 = R
    break;
}
```

`n64be16(p, a)` is `n64b(p, a) << 8 | n64b(p, a + 1)`, and `n64b(p, off)` is
`p[off ^ 3]`, the logical byte at guest offset `off`, so the two halfwords are
the texel's four guest bytes in order. RT64's reader for the same bytes:

- `rt64_rdp.cpp:527` `loadWord<RGBA32>` writes
  `TMEM[tmemAddress + 0/1] = RDRAM[tex + 0/1]` and
  `TMEM[(tmemAddress + 0/1) | UpperTMEM] = RDRAM[tex + 2/3]`, i.e. bytes 0/1 in
  the lower half and 2/3 in the upper.
- `TextureDecoder.hlsli` `sampleTMEMWithConvert` reads `pixelValue0/1` from the
  lower half and `pixelValue2/3` from `orAddress = RDP_TMEM_BYTES >> 1`, and
  `sampleTMEMRGBA32` returns `RGBA32ToFloat4((r<<24)|(g<<16)|(b<<8)|a)` with
  `r = pixelValue0`. So byte 0 is R.
- `tools/ogre_frame.py:209-211` decodes `siz == 3` as the four bytes in order.

### 2. Measured on the title scene, same run

The forced title scene (`OGRE_SCENE=title`) with `OGRE_TEX_TRACE=1` and a full
RDRAM dump (`OGRE_RAM_KEY_AT_MS=25000 OGRE_RAM_KEY_FULL=1`). The traced strip
group is the backdrop: `rect=0,80..320,239` in 3-row rectangles, `img=0x217508`
(guest `0x80217508`), `fmt=0 siz=3 stride=1280`, tile window `uls0 ult{t}
lrs1276 lrt{t+8}`.

| rows | before the change (`mean`/`opaque`) | after the change | offline decode, `(G,B,A,R)` | offline decode, `(R,G,B,A)` |
|---|---|---|---|---|
| 0..3 | `25.5` / `0` | `56.8` / `0` | `25.5` / `0` | `56.8` / `0` |
| 3..6 | `28.0` / `17` | `58.7` / `15` | `28.0` / `17` | `58.7` / `15` |
| 6..9 | `31.3` / `50` | `61.3` / `33` | `31.3` / `50` | `61.3` / `33` |
| 9..12 | `37.7` / `111` | `66.5` / `49` | `37.7` / `111` | `66.5` / `49` |
| 12..15 | `45.1` / `144` | `71.7` / `110` | `45.1` / `144` | `71.7` / `110` |
| 15..18 | `53.9` / `208` | `78.3` / `134` | `53.9` / `208` | `78.3` / `134` |
| 18..21 | `70.0` / `333` | `92.1` / `213` | `70.0` / `333` | `92.1` / `213` |
| 21..24 | `87.9` / `430` | `104.6` / `319` | `87.9` / `430` | `104.6` / `319` |

Every line matches to the tenth, in both directions. The image is a static asset,
so the two offline columns are the same bytes in either run's dump: the after
run's own dump (`/tmp/web145f2-rdram.bin`) reproduces `56.8`/`0` for rows 0..3 and
`104.6`/`319` for rows 21..24, and the before run's dump
(`/tmp/web145c3-rdram.bin`) reproduces all eight bands in both columns. The first
traced rectangle of the group is `texel(0,0) = 83 75 4B FF`, so the browser's
`mean=25.5` for rows 0..3 is the decode that calls the source G (0x4B) the red
channel; `mean=56.8` is the one that calls the source R (0x5F) the red channel.

### 3. The frame against RT64's target

RT64's own render target for the same title frame, captured with the recipe in
section 4 and read with the new `tools/rt64_target.py`. The browser canvas is
640x480 at 2x of the 320x240 scanout plus a 1px CSS border, so the comparison
samples every second pixel of the inner area.

| | before | after |
|---|---|---|
| mean abs difference per channel over the frame | `40.7` | `5.1` |
| mean signed difference (browser - target) | — | R `-1.26` G `-1.56` B `-2.47` |
| pixel `(10,101)` | `(162,142,235)` vs `(187,175,153)` | `(187,175,153)` vs `(187,175,153)` |
| pixel `(100,101)` (session 144's row) | `(53,6,70)` vs `(118,98,31)` | `(112,91,21)` vs `(118,98,31)` |
| pixel `(200,120)` | `(187,163,252)` vs `(192,183,159)` | `(196,187,163)` vs `(192,183,159)` |

The residual 5.1 is a small overall darkening, not a channel order: RT64's target
is the pre-present render target in `R16G16B16A16_UNORM`, and the browser has no
dither. It is not measured further this session.

### 4. The native capture recipe, and one knob it needs

```
OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SCENE=title \
  OGRE_CAPTURE_TARGET=/tmp/nt145/t OGRE_CAPTURE_AFTER=200 OGRE_CAPTURE_EVERY=60 \
  OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64 assets/ogre64.z64
python3 tools/rt64_target.py /tmp/nt145/t.540.bin /tmp/nt145/target-540.png 320 240
```

`OGRE_MOD_TEST=1` is required: `build-app/mods/` holds three `.nrm` mods, and an
installed mod forces the start screen, so without the knob the app printed
`[launcher] waiting for a ROM` and wrote no capture even with the ROM on the
command line. The `<path>.<n>.bin` is 8 bytes per texel and the channels are
`R16G16B16A16_UNORM`; reading the first four bytes of a texel as `R8G8B8A8`
produced noise. `tools/rt64_target.py` reads the documented format and is the
reader for it now.

## Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web` | clean |
| title trace + dump, after the change | `node /tmp/web145title.cjs --query 'OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TEX_TRACE=1&OGRE_RAM_KEY_AT_MS=25000&OGRE_RAM_KEY_FULL=1&OGRE_RAM_KEY_OUT=/tmp'` | `img=0x217508` rows 0..3 `mean=56.8 opaque=0`, rows 21..24 `mean=104.6 opaque=319`; the same run's dump decodes to exactly those under `(R,G,B,A)` |
| title trace + dump, before the change | the same probe on the session-144 build | rows 0..3 `mean=25.5 opaque=0` (= `(G,B,A,R)` decode), rows 21..24 `mean=87.9 opaque=430` |
| frame against RT64 | `python3 tools/rt64_target.py` + `/tmp/pngcmp.py` | `40.7 -> 5.1` mean abs per channel (section 3) |
| boot regression | `node debug/probes/boot.cjs --attempts 4 --secs 70 --out web145-boot2` | `RENDERED nonBlack 25644 / colorful 18568`, the same numbers as session 144 |
| synthetic draw | `node debug/probes/testdraw.cjs --warmup 14000` | `test_draw exported= true returned= 1` |
| publishers after the change | `node /tmp/web145title.cjs --query 'OGRE_SCENE=publishers'` | the ATLUS still renders (`/tmp/web145pub-004.png`) |

A natural 250 s route was not run this session. The browser build's idle
trajectory (about one boot in three to five never submits a real display list)
was hit repeatedly while collecting the runs above; every measurement quoted here
comes from a run whose page log has the traced rectangles.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `decode_texture_rect`'s `IM_SIZ_32b` case reads the texel `R,G,B,A` instead of `G,B,A,R`, with the RT64 and `ogre_frame.py` references in the comment |
| `tools/rt64_target.py` | new; reads an `OGRE_CAPTURE_TARGET` `.bin` (`R16G16B16A16_UNORM`, 8 bytes/texel) into a PNG, with `--format u16\|u8\|f16` |
| `docs/guides/app-build.md` | new `OGRE_MOD_TEST=1` row; the `OGRE_CAPTURE_TARGET` row names the tool and the format |
| `docs/proofs/web-title-rgba32-rgba-order-fix.png` | new; the title on the fixed build |
| `docs/proofs/web-title-rgba32-rgba-order-before.png` | new; the same frame on the session-144 build |
| `docs/proofs/native-title-rgba32-reference.png` | new; RT64's render target for the same frame, from `t.540.bin` |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | browser section, decision row, log entry, index |
| `docs/HANDOFF-2026-10-14-session145.md` | this file |

## Probes

- `/tmp/web145title.cjs` (throwaway): boots the wasm build to a forced scene with
  any query string, captures the canvas every 2 s for the whole run, writes the
  page log, and reads an `OGRE_RAM_KEY_FULL` dump out of the wasm filesystem.
- `/tmp/web145rgba32.py` (throwaway): decodes one RGBA32 tile window out of a
  guest dump under both channel orders and prints mean/opaque for each, so a
  trace line can be attributed to an order.
- `/tmp/pngcmp.py` (throwaway): compares a browser canvas PNG (2x plus border)
  against a 320x240 target PNG and prints per-row and per-pixel differences.
- `/tmp/ppm2png.py` and `/tmp/target2png.py` (throwaway) were superseded by
  `tools/rt64_target.py`, which stays: it is the reader for a documented
  diagnostic format, not a probe.
- No probe is left in the tree; no diagnostic was removed.

## Open

1. **The residual frame difference after the fix**: mean absolute difference
   5.1/255 per channel, with the browser slightly darker (R -1.3, G -1.6,
   B -2.5). Candidates are the browser combiner without dithering, the decode's
   rounding, and the 2x present blit's filtering. None is measured.
2. **Only the title frame was compared against RT64.** Session 143 measured the
   hue shift on the attract story (scene `0x0B`) as well, and its frames go
   through the same decode case, but no story frame was captured this session.
3. Session 141/142's items stand: framebuffer-as-texture (a `G_LOADTILE` whose
   source is a rendered colour image reads RDRAM), presentation resolves the VI's
   origin by containment rather than applying its own view rectangle, the
   deployed site has only been exercised headless, the first load fetches the
   whole 21 MB wasm, the browser build assigns gamepads to N64 controllers 2-4,
   and `debug/probes/rdram.cjs` reads one dump per run.
4. Session 141's `divide by zero` at task 277 in the publishers is still
   reachable on a route that reaches scene `0x000A` (session 144 §5). It did not
   reproduce in the forced-publishers run this session.
