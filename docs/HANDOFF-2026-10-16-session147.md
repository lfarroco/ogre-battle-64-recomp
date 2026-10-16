# Session 147 handoff — the browser decoded every palette entry by its own bit 15, so the publisher still's ATLUS strokes drew grey where RT64 paints blue

## Goal

Continue the web renderer (developer, session start). Session 146's open item 1
was the ATLUS still: the native build's render target has `(57,90,165)` for the
logo's strokes, the browser canvas had `(56,56,56)`, and RT64's own colour decode
of the same draw reads cream, so session 146 left the cause in RT64's palette
lookup.

## Result

1. **The browser chose the palette entry's decoder from the entry's own bit 15.**
   `decode_texture_rect` selected RGBA16 when bit 15 was set and IA16 otherwise.
   The RDP selects it from the draw's 2-bit texture LUT mode
   (`OTHERMODE_H` bits 14-15): `G_TT_RGBA16` (2) decodes every entry as 5/5/5/1 and
   `G_TT_IA16` (3) decodes every entry as 8-bit intensity plus 8-bit alpha. RT64's
   `TextureDecoder.hlsli` switches on that mode and never on the entry.
2. **The ATLUS stroke is entry `0x3AE8`, whose bit 15 is 0.** `rgba16(0x3AE8)` is
   `(57,90,165)`; `ia16(0x3AE8)` is `(58,58,58,232)`. The bit-15 test produced the
   second. The red T's entry is `0xD80A`, bit 15 set, so it matched by accident,
   which is why one colour was right and one was wrong.
3. **Fixed and verified: the browser's stroke colour is now `(57,90,165)`,
   exactly RT64's.** The whole-frame colour histograms of the browser canvas and
   RT64's capture now agree entry for entry: cream `(239,231,206)`, the drop
   shadow `(222,214,181)`, the strokes `(57,90,165)` (9836 canvas pixels = 4 x
   2459 native pixels, the 2x duplication), the red T `(222,0,41)` (1864 = 4 x
   466). `(58,58,58)` no longer occurs.
4. **The correction to session 146's number.** That session recorded the native
   strokes as `(48,80,160)`. The measured value in `/tmp/w146/nat/tt.140.bin` is
   `(57,90,165)`; `(48,80,160)` occurs 0 times. The mis-read colour is not a
   different fault (raw `0x32A8` also has bit 15 clear), so the fix covers it.
5. **The earlier conclusion that this is an RT64 fault is withdrawn.** Session
   146's section "The ATLUS logo's letters render blue in the native build" reads
   the blue as RT64's error and points at `RDP_TMEM_PALETTE` versus
   `loadTile.tmem << 3`. The native blue is correct, and it is the browser that
   was wrong. Session 146's `colour_mean` guard change was still needed to see
   the cream at all, and stays.
6. **The remaining publisher difference is a one-row vertical offset.** After the
   fix, canvas against RT64's target at the same screen is
   `meanAbs=[8.26, 9.53, 15.92]` with `meanSigned=[-0.09, -0.08, -0.07]` (58.0%
   exact pixels over 320x240). A shift search, including sub-pixel positions by
   bilinear sampling, minimises at exactly `dy=-1` (`meanAbs 6.35` after moving
   the canvas up one row) against `11.43` at `dy=0` and `6.50` at `dy=-1.1`, so
   the offset is one whole row and not a fraction. The largest differing pixel
   pairs are colour swaps of neighbouring still colours (white against cream,
   4593 and 4527 pixels), which is what a one-row offset produces and not what a
   palette or channel error produces. The strokes' bounding boxes agree in size
   and differ by that row: 157..472 x 201..284 in the canvas against 156..471 x
   198..281 in RT64. The residual `6.35` under the shift is the still's own
   dithering, because the two frames differ by 0.00 from each other across six
   native captures (`tt.140` .. `tt.240`), so the offset is the whole of the
   difference that is not dither.

## Evidence

### 1. The decode selection

`app/src/web_renderer.cpp` before:

```cpp
case gbi::IM_FMT_CI: {
    if (tlut_valid) {
        const uint16_t e = tlut[v];
        if ((e >> 15) & 1) {          // the entry's alpha bit chose the decode
            rgba16_to_rgba8(e, rgba);
        } else {
            ia16_to_rgba8(e, rgba);
        }
    }
    break;
}
```

after (CI8; the CI4 arm and the cache key changed the same way):

```cpp
const uint32_t lut_mode = (st.othermode_h >> 14) & 3u;
const bool tlut_rgba16 = (lut_mode == 2);
...
const uint16_t e = tlut[v];
if (tlut_rgba16) {
    rgba16_to_rgba8(e, rgba);
} else {
    ia16_to_rgba8(e, rgba);
}
```

`decode_texture_rect` now takes `int lut_mode` in place of `bool palette_enabled`;
mode 0 still reinterprets a CI tile as I8 (session 144's rule), which is what
`palette_enabled` was for. The image cache key now carries the whole mode
(`tlut_generation_ * 4 + lut_mode`) instead of a bool, because modes 2 and 3
decode one entry to two different colours.

### 2. The two decodes of entry `0x3AE8`

| | value |
|---|---|
| raw entry | `0x3AE8` = r5 7, g5 11, b5 20, a 0 |
| `rgba16(0x3AE8)` (RT64, `G_TT_RGBA16`) | `(57, 90, 165)` |
| `ia16(0x3AE8)` (the old bit-15 test) | `(58, 58, 58)` alpha 232 |

The native and browser proofs, both 640-wide prescreens:

| | strokes | red T | cream | shadow |
|---|---|---|---|---|
| RT64 `docs/proofs/native-publishers-atlus-reference.png` | 9836 px `(57,90,165)` | 1864 `(222,0,41)` | 189172 `(239,231,206)` | 37837 `(222,214,181)` |
| browser before `docs/proofs/web-publishers-atlus-present-fix.png` | 9836 px `(58,58,58)` | 1864 | 189172 | 37837 |
| browser after `docs/proofs/web-publishers-atlus-lutmode-fix.png` | 9836 px `(57,90,165)` | 1864 | 189172 | 37837 |

The 9836/1864 counts are the canvas's 2x duplication of 2459 and 466 native
pixels; the (2x-sampled) native counts are 2459 and 466.

### 3. The measurement commands

```
# the browser frame (scene 0x000A, the ATLUS still)
node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_TEX_TRACE=1&OGRE_SCENE_LOG=1' \
     --out /tmp/w147/fix1 --secs 110 --attempts 3 --shots 1 --min-colorful 60000
# -> kept 0: nonBlack 306096 colorful 268842 top ["#eec"...(239,231,206), "#35a"...(57,90,165), "#d02"...(222,0,41)]
#    [scene] t=4135ms id=0x000A descriptor=0x8018FB84
```

The colour histograms above were read from that PNG and from
`/tmp/w146/nat/tt.140.bin` with the same decoder used by `tools/web_frame_cmp.py`
(`R16G16B16A16_UNORM`, high byte per channel, 8 bytes a texel). RT64's still is
static: `tt.140`, `tt.160`, `tt.180`, `tt.200`, `tt.220` and `tt.240` differ by
0.00 from each other, so one capture is the whole still.

### 4. The palette itself was never wrong

Session 146 read the eight traced `G_LOADTLUT`s at `addr=0x3A22C8` as zeros
(`first=0000 0000 0000 0000`) and left the palette's contents as the open
question. This session's run reproduces the same eight zero loads and the still
still decodes cream, so those eight loads are not the still's palette: the trace
caps at 8 and the still's own load is later. The decode path is not the question,
because the same `tlut_` produced the correct cream and the correct red T at the
same time as the grey strokes. Nothing in the palette load was changed this
session.

New diagnostic (kept): `GFX-TEXTRACE` now prints, for the first eight decodes,
`decode tile=.. img=.. rect=.. siz=.. tmem=.. gen=.. e0=.. e113=.. e230=..
nonzero=..` and the four most common decoded colours, and the `loadtlut` line's
palette is counted so an all-zero load is visible as `nonzero=0`.

## Verification runs

| what | command | result |
|---|---|---|
| wasm build | `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` | clean; `built target ogrebattle64` |
| native build | `cmake --build build-app -j` | clean |
| the ATLUS still, fixed | `node debug/probes/palette.cjs --query 'OGRE_SCENE=publishers&OGRE_TEX_TRACE=1&OGRE_SCENE_LOG=1' --out /tmp/w147/fix1 --secs 110 --shots 1 --min-colorful 60000` | strokes `(57,90,165)`, `[scene] id=0x000A` |
| the frame against RT64 | Python comparison of `docs/proofs/web-publishers-atlus-lutmode-fix.png` with `/tmp/w146/nat/tt.140.bin` at 320x240 | `meanAbs=[8.26,9.53,15.92] meanSigned=[-0.09,-0.08,-0.07] exact=58.0%`; shift minimum `dy=-1` at `6.40`, `dy=0` at `11.43` |
| the title, no regression | `node debug/probes/palette.cjs --query 'OGRE_SCENE=title' --out /tmp/w147/title --secs 70 --shots 1 --min-colorful 100000` then `python3 tools/web_frame_cmp.py /tmp/w147/title-00.png /tmp/w146/nt/t.160.bin` | `meanAbs=[2.32,2.51,3.12] exact=81.2%` against session 146's `[2.29,2.48,3.10] / 81.4%` on the same target (the two-cloud-band phase, session 146 §6) |
| boot regression | `node debug/probes/boot.cjs --attempts 3 --secs 70 --out /tmp/w147/boot` | `RENDERED nonBlack 24836 / colorful 15076` (a fade-in frame) |

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `decode_texture_rect` takes the 2-bit LUT mode and selects RGBA16 only for `G_TT_RGBA16`; the image cache key carries the whole mode; `tlut_nonzero_` counts the loaded palette; `GFX-TEXTRACE` prints the load's `tmem`, the palette entries it read and the four most common decoded colours |
| `debug/probes/palette.cjs` | new; boots the wasm build at a forced scene, waits for a bright canvas, writes frames and the page log (the `GFX-TEXTRACE` lines are the point); the reusable form of the `w146bright.cjs` throwaway |
| `docs/proofs/web-publishers-atlus-lutmode-fix.png` | new; the publisher still on the fixed build |
| `docs/proofs/native-publishers-atlus-reference.png` | new; RT64's `tt.140` target for the same still, converted with `tools/rt64_target.py` |
| `docs/proofs/web-title-lutmode-fix.png` | new; the title on the fixed build (no regression) |
| `PLAN.md`, `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | open work 8(h) closed, decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session147.md` | this file |

## Probes

- No probe was added to generated code and none was reverted; nothing under
  `RecompiledFuncs/`, `Bank*Funcs/` or `RspFuncs/` was touched.
- `debug/probes/palette.cjs` is kept: it reads a documented diagnostic
  (`GFX-TEXTRACE` plus a `#game-canvas` screenshot) and repeats a check that
  otherwise costs a throwaway script.
- The `GFX-TEXTRACE` additions are part of the port's diagnostics (rule 12), not
  a probe: they are bounded to the first eight decodes and off unless
  `OGRE_TEX_TRACE=1`.
- Build residue: `.emcache/` (an Emscripten cache) was written at the repository
  root because the sandbox cannot write `~/.cache/emscripten-ogre`, whose
  `symbol_lists/*.json.lock` it refused. It is untracked and new in this session;
  `make web` uses `EM_CACHE` as before. It should be deleted or gitignored.
- `tools/RT64` is dirty with the port's own renderer changes from earlier
  sessions; nothing in it was touched this session.

## Open

1. **The publisher still sits about one row low in the browser.** `meanAbs`
   falls from `11.43` to `6.40` when the canvas is compared one row up, and the
   differing pixels are swaps of neighbouring still colours. The 640-wide mode's
   canvas element is 640x480 for a `visible=639x475` VI (`[GFX-PRESENT]`), so the
   476 -> 480 CSS stretch (session 146 open item 4) is one candidate; the
   `fldRegs` x/y scale and offset (session 141/142 open item) is another. The
   320x240 mode does not show it.
2. Session 146's other items stand: the two moving-cloud bands on the title; the
   native's x4 saved game speed against the browser's x1; the canvas CSS size not
   following the drawing buffer; framebuffer-as-texture; the
   `OGRE_SCENE=publishers` native runs landing in scene `0x0009` in some runs
   (this session's browser run read `id=0x000A`).
3. The LOADTLUT destination is still keyed on the load tile's `palette` field
   (`bank = t.palette * 16`) where RT64 uses the load tile's `tmem`
   (`loadTile.tmem << 3`). The two agree for `tmem=0x100` with `palette=0`, which
   is every traced load so far, so no screen has needed the difference. The
   browser run's scene `0x0009` shows two 16-entry loads at different addresses
   both landing on `bank=0`; that is the shape to test when a screen disagrees.
