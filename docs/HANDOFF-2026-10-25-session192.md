# Session 192 handoff — the mission intro's ucode is F3DEX2.NoN, and its frame art is a framebuffer tile copy

## Goal

Continue the browser renderer. On the mission start screen the developer sees two
defects that native does not: the framed target-fort illustration has bright
green lines around it, and the grass field is flat dark green instead of
textured. Fix both.

## Result

Two separate findings, one fix landed and one root cause isolated.

1. **The mission's 3D ucode is `F3DEX2.NoN.fifo 2.08` and the browser did not
   know it.** Every display list at scene `0x03` carries `ucode=0x800A08D0`; its
   first `0x1390` text bytes hash to `0x4C12DAE0534D7135`, which is RT64's
   `F3DEX2_NON_FIFO_2_08` entry (`tools/RT64/src/gbi/rt64_gbi.cpp:229`). RT64
   maps it to `GBIUCode::F3DEX2` with `flags.NoN`, and `NoN` only disables the
   F3D microcodes' near-depth clip (`render/rt64_raster_shader.cpp:315`,
   `depthClipEnabled = !NoN`). The browser printed `[WEB-GBI] unknown ucode` and
   fell back to plain F3DEX2, so its `u_zclip` still dropped fragments outside
   `[0, 1022/1024]`. The hash is now in `kGbiTextSegments` as
   `GbiKind::F3DEX2_NON_FIFO`, and a draw from that list carries
   `no_near_clip`, which leaves `u_zclip` at 0. The `[WEB-GBI] unknown` line is
   gone from the mission run. This is a fidelity fix; on its own it does not
   remove the green lines.

2. **The framed illustration is a framebuffer-as-texture screen, and the browser
   reads stale RDRAM for it.** The frame's strips and its place label are
   `G_TEXRECT`s whose sources are in the colour image the RDP has just rendered:
   the mission log prints 24 `[GFX-FBTEX]` loads at scene `0x03` with sources
   like `0x23FA00` inside colour image `0x228080`. RT64's
   `checkFramebufferOverlap` either flushes the framebuffer pair or makes a GPU
   tile copy (with the `RAMHash` stale-RDRAM guard at
   `hle/rt64_rdp.cpp:146-230`). The browser has no tile copy, so `tmem_copy`
   reads the bytes an earlier frame left in RDRAM.

   Evidence that the bad pixels are the data, not the WebGL path: a browser
   `OGRE_RAM_KEY_AT_MS=165000` whole-RDRAM dump of the same mission, rendered
   with `tools/ogre_frame.py --dl 0x8036BBE0`, produces the same striped garbage
   frame (`web-mission-intro-rdram-session192.png`). A native RDRAM dump of the
   same mission and phase produces the gold frame with the same tool
   (`native-mission-intro-rdram-session192.png`). The browser renderer is
   faithfully drawing source bytes that are stale.

   A naive repair was tried and rejected: before an overlapping load, flush and
   write the whole colour image back (`sync_framebuffers()` from the gfx
   thread). The browser run then crashed with `memory access out of bounds` in
   `func_ovlN_801F7C60` right after `[GFX-FBSYNC] wrote back 0x228080`. A
   mid-list whole-image writeback writes over the game's own textures that live
   inside the image's byte range; RT64's per-tile copy exists to avoid exactly
   that. The writeback was reverted.

## Evidence

- Mission ucode line before the fix:
  `[WEB-GBI] unknown ucode 0x800A08D0 ... hashes 0x4C12DAE0534D7135/len 0x1390 ...`.
  After: no `[WEB-GBI]` line in `/tmp/w192-nonn-log.txt`.
- The mission's two display-list roots are `0x8036BBE0` and `0x80319890`, both
  `ucode=0x800A08D0` (`[GFX-TASK]` tasks 30-33 in the browser log).
- The frame's own draw state, logged by the new `[GFX-FRAME-RECT]` line at
  scene `0x03`, matches RT64 for the strips: source `0x2C4B70`, `fmt=0 siz=2
  line=23`, `uls=0 ult=0 lrs=364 lrt=12`, `omh=0x002C30 oml=0x00504240`,
  `comb=FCFF9BFF/FF34FE7F`, and the same 1-cycle `P=CC A=CC_A M=FB B=1-A`
  blend RT64 prints. The difference is not the state.
- `[GFX-FBTEX]` hits at scene `0x03`: 24 loads, for example
  `load tex=0x23FA00 rect=256x1 stride=2 ... reads colour image 0x228080 ...
  the load read RDRAM, not the rendered pixels`.
- Browser RDRAM dump `ram-1-full.bin` (8 MiB, `ram_key_dump_full` logical order)
  at `t=165 s`; its `ogre_frame.py` render is
  `docs/proofs/web-mission-intro-rdram-session192.png`.
- Native same mission, losing-condition frame: `docs/proofs/native-mission-intro-dunkweld-session192.png`
  and the CPU render of its RDRAM `docs/proofs/native-mission-intro-rdram-session192.png`.
  The browser's losing-condition frame is
  `docs/proofs/web-mission-intro-nonn-session192.png`.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean; final wasm
  `21197859` bytes.
- NoN run (same route as the proof):
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=2000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6,0x03:a:30`.
  Scene `0x03` reached, `[WEB-GBI]` absent, no page errors; shots under
  `/tmp/w192-nonn-*`.
- Browser RDRAM dump: `node debug/probes/rdram.cjs --query
  'OGRE_RAM_KEY_AT_MS=165000&OGRE_RAM_KEY_FULL=1'`; CPU render matches the
  browser's garbage frame.
- Native same-mission capture: `OGRE_TAP_SCENE_BUTTON=...0x03:a:4`,
  `OGRE_CAPTURE_PRESENT` every 1000 ms; frame `f.725` is the losing-condition
  screen. A `snap` run produced the RDRAM for the CPU render.
- The whole-image sync A/B: crash in `func_ovlN_801F7C60`; reverted, and
  `grep -n "sync_framebuffers();" app/src/web_renderer.cpp` now names only the
  public `ogre_sync_framebuffers()` bridge.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `GbiKind::F3DEX2_NON_FIFO`, the `0x4C12DAE0534D7135` GBI hash, `DrawCmd::no_near_clip`, `u_zclip` gating; per-scene `[GFX-TDSTATE]` burst (reset on scene change, UV/comb/othermode line); `[GFX-FRAME-RECT]` diagnostic for the mission's frame band |
| `docs/proofs/web-mission-intro-nonn-session192.png`, `-rdram-session192.png`, `native-mission-intro-dunkweld-session192.png`, `native-mission-intro-rdram-session192.png` | before/after and browser/native data proofs |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md` | status and decision rows |
| `docs/HANDOFF-2026-10-25-session192.md` | this file |

## Probes used and reverted

The whole-image framebuffer sync described above was added to
`tmem_load_command` and reverted in the same session; the final source has no
pre-load sync. No generated code (`RecompiledFuncs/`, `Bank*Funcs/`) carries a
probe, and `tools/RT64` is unchanged by this work. The `[GFX-FRAME-RECT]` and
per-scene `[GFX-TDSTATE]` blocks are bounded diagnostics that stay.

## Open

1. **Implement the tile copy, which is the actual fix for the frame.** On a load
   whose source overlaps a colour image drawn earlier in the batch, RT64 copies
   the tile from the framebuffer (or flushes the pair) and refuses the copy when
   RDRAM changed since its last sync. A whole-image writeback is not a substitute:
   it crashes the game. The browser needs a per-rect readback/GL copy with the
   same stale guard. The browser RDRAM dump and the CPU renders above are the
   comparison pair for that work.
2. **The grass field.** The terrain textures are static 32x64 RGBA16 tiles and
   decode correctly (`[GFX-TDSTATE] scene=0x3 uv=[...] tex0=32x64`), but the
   losing-condition field is much flatter and darker than native. The field's
   light is blue (`amb=[col 77 77 127]`, `shade=[0.302 0.302 0.498]`), and the
   mission is 3D; this is not explained by the frame's tile copy. Isolate the
   grass draw with the TEXEL0 debug path or a per-draw lighting comparison
   before changing it.
3. The `[GFX-FBTEX]` row/column test still over-reports; the frame proof above
   uses its hits as a locator only. Its stale-RAM guard is the part to port with
   the tile copy.
