# Handoff — 2026-09-15, session 47: the njpeg decoder was mis-labelled by RSPRecomp (and now decodes the whole image)

## Goal and result

**Goal (session 46's open wall):** the cathedral scene (scene `0x0D` step 2) has no
background — its 320x240 image at guest `0x80243E28` is uniform.

**Result: the `M_NJPEGTASK` (type 4) decoder is now correct — the bug was the
recompilation, not the microcode.** RSPRecomp's `text_address` is the address the
microcode was **assembled** at (masked with `0x1FFF`), not the RDRAM address of the
text: it must be the RSP IMEM DMA address `0x1080` (the boot loader the game ships
loads the text at IMEM `0x080`). With the old value (`0x8009ED80`, low 13 bits
`0x0D80`) every `j` target in the recompiled decoder resolved **0x300 bytes off**,
so it walked the wrong blocks and wrote a single `0x300` block per task — exactly
what session 46 measured. With `text_address = 0x1080` a task now writes **all
`mbs` blocks** and exits cleanly (`Broke`), and the scene no longer stalls.

**The background is still uniform, but for a different reason:** the pixels the
step-2 blit reads back are produced by *rendering* the njpeg YUV macroblocks and
copying the framebuffer with the CPU (`G_SETCIMG` → the DL draws one 16x16 YUV16
texture per macroblock → `func_ovlE_80199D30` copies framebuffer rows into the
image). In the port the copy source is a uniform `0x0843` (RT64) / `0` (null), so
the next wall is the RDP→RDRAM readback of that draw, not the decoder. §5 says
what is and is not established.

## 1. The bug: RSPRecomp's `text_address` is a label base, not an address

RSPRecomp labels the first instruction with `config.text_address & rsp_mem_mask`
and resolves every branch target the same way
(`tools/N64Recomp/RSPRecomp/src/rsp_recomp.cpp:1146` and `:239`); `rsp_mem_mask`
is `0x1FFF`. The input text bytes come from `text_offset`. So the config value
must be the address the microcode's own assembler used:

* the game's boot loader is the standard one (`0x8009ECB0`, ROM `0x2F0B0`). Its
  second half (ROM `0x2F0C0` = vram `0x8009ECC0`) is
  `20071080 addi $7,$0,0x1080` / `40870000 mtc0 $7,$0` (SP_MEM_ADDR) /
  `40820800 mtc0 $2,$1` (SP_DRAM_ADDR = `task->ucode`) / `40831000 mtc0 $3,$2`
  (SP_RD_LEN = `0xF7F`, i.e. a fixed `0xF80` bytes) / … / `00e00008 jr $7`;
  so the text is loaded at **IMEM `0x080`**, i.e. RSP address `0x1080`.
* the ucode's own `j` instructions encode `0x1000`-based addresses
  (`j 0x84001190` at vram `0x8009EE70`, `j 0x840013B0` at `0x8009EFD0`), and the
  jump targets' bit 12 is set — the same convention (`0x1190`, `0x13B0`).

`0x8009ED80 & 0x1FFF = 0x0D80`, so every target was looked up 0x300 bytes late:
`L_1190` was bound to the input-DMA block at image offset `0x410` instead of the
intended image offset `0x110`, and the loop's DMA-write path (the one that
advances the output pointer at `0x8009F170`) was never reached. `text_address =
0x1080` restores the hardware's own control flow. `rsp-njpeg.toml` now documents
this, because the value looks like a typo and a future session would "fix" it.

## 2. What the decoder now produces (measured)

`[probe47]` (temporary, reverted — see §7) dumped the task buffer before/after
`njpeg_ucode` and fingerprinted the first/last `0x300` block:

```
mbs=300 nonzero before=60729 after=168900 reason=1
   block[0]=9FC8EA30 block[1]=2C6DC4E0 block[299]=BAB35AE6
mbs=165 nonzero before=27512 after=92894  reason=1  block[0]=46AB694C … block[164]=A8567791
mbs=180 nonzero before=36549 after=101340 reason=1  block[0]=90D82A4D … block[179]=8B637C84
mbs=99  nonzero before=18265 after=55737  reason=1  block[0]=139EF857 … block[98]=9301F486
```

Before the fix (session 46's file) the same probe showed one `0x300` block per
task. The four images are the four `M_NJPEGTASK`s the cathedral step submits
(`data_ptr`/`mbs` = `0x801D1030`/300, `0x801E5620`/165, `0x801FBEB0`/180,
`0x80208250`/99 — the heap addresses move between builds).

## 3. The pipeline in full (instruction-level, this session)

The New Game step-2 background is an **N64 JPEG** image decoded by the SDK's
`njpg` library. The manual ("N64 Programming Manual, JPEG", mirrored at
<http://ultra64.ca/files/documentation/online-manuals/man-v5-1/ucode/jpeg/>)
documents the format: a `"HUFF"` + `s16 numMB` + entropy-coded payload, decoded
by `njpgHuffDecode` on the **CPU**, then reverse-zigzag/dequant/inverse-DCT in
place by the `njpgdspMain` microcode, which outputs **16-bit YUV** macroblocks
(768 bytes each: 4×128 Y, 128 U, 128 V, 256 unused), meant to be drawn as 16x16
YUV textures.

In OB64 (record 0, unit E; asset 49 = ROM `0x7175A2`, `'HU'` + scale `0xFE`(-2)
+ 320x240 + `'HUFF'` + numMB `0x12C`):

1. `func_ovlE_801988C8` (`'HU'`/`'B5'`/`-lh`/LZ dispatcher) → `func_ovlE_8019A0A8`
   → `func_ovlE_80199A08`, which allocates the state (`D_8019A680`) and the
   `mbs*768` buffer (`state[0x68]`) and calls **`func_8008B250(payload,
   state[0x68])`** — this is the CPU Huffman decode, in the main segment (not
   `func_ovlE_80197E5C`, which session 46 looked at). It runs in the port: the
   probe shows 60729/230400 non-zero bytes at submit for the 320x240 image.
2. `func_ovlE_8019976C` (stage machine) → `func_ovlE_80198E70` builds the OSTask
   at `state+0x10` with `type=4`, `ucode=0x8009ED80`/`0x7C0`,
   `ucode_data=0x800AC050`/`0xF0`, `data_ptr=state[0x68]`,
   `data_size = mbs`, `yield_data_size = scale` — i.e. the game drives the
   microcode directly (no `NJPEGDParam` indirection). Submitted through the
   game's own queue `0x800F8B4C`.
3. The microcode decodes in place (one block at `data_ptr - 0x300 + i*0x300` per
   macroblock) → YUV16.
4. `func_ovlE_80199588(0)` → `func_ovlE_80199130` builds a display list of
   per-macroblock 16x16 **YUV16** textures (`SETTIMG fmt=1 siz=2 width=16`,
   address `state[0x68] + i*0x300`, `LOADTILE 16x16`, `MOVEMEM`, `MTX`), and
   `func_ovlE_80199588(1)` submits it as a **second gfx task with ucode
   `0x800A5110`** (boot `0x8009ECB0`, `ucode_data=0x800BD590`), with
   `G_SETCIMG` = `state[0x64]` (the current framebuffer, e.g. `0x80025C00`).
   Verified submitted: with `OGRE_DL_TRACE=1` the renderer logs exactly **four**
   `(type=1 ucode=0x800A5110 data=0x8004B400)` display lists at the second `0x02`
   enter (t≈27.4 s wall at `OGRE_SPEED=6`). (An earlier, shorter trace run had
   not reached step 2 and made this look like it was never submitted — the DL log
   line is *sampled* unless `OGRE_DL_TRACE=1`.)
5. The stage machine waits, then copies framebuffer rows into its image buffer
   (`func_ovlE_8019976C`'s stage-3 path: `memcpy(state[0x70], state[0x64], w*2)`
   per row, source stride `0x280` = 320*2). The buffers carry a small header in
   front of the pixels (`36 34 00 02` + `u16 w` + `u16 h`, i.e. `'6','4'`, for at
   least the small images the same path decodes; the assembly below starts with
   `42 35` = `'B5'`), which is why the step-2 blit address is not the buffer base.
6. `func_ovlE_80199D30` assembles the container's chunks into a fresh buffer:
   `[8-byte header][16-byte chunk descriptor][pixels]`, so the pixel data starts
   0x18 bytes after the buffer base. The step-2 blit reads exactly that:
   `SETTIMG fmt=RGBA siz=16b width=320 addr=0x80243E28` in 16-row `LOADTILE`
   strips (`OGRE_DL_DECODE`), and an lldb watchpoint on `0x80243E20` shows the
   writer chain `func_80080998 (memcpy) ← func_ovlE_80199D30 (thread 3)` — so the
   assembled image base is `0x80243E10` (a `'B5'`-headed container) and
   `0x80243E28` is its first chunk's pixels. Session 46's "assembles the image
   from `struct->0x88[i]` **into** `0x80243E28`" was one level off. (The pointer
   chain into `state[0x88+i*4]` is from the disassembly, not from a runtime
   probe; the address result is verified.)

## 4. The new wall: the framebuffer the game reads back never receives the draw

`0x80243E28` is `08 43 08 43 …` **throughout the whole 153600-byte image** in RT64
and all-zero in the null build (both before and after the decoder fix), so the
pixels the game assembles come from a framebuffer that never received the YUV
draw.

* The DL's render target is `G_SETCIMG` = `0x80025C00`; in an RT64 run dumped
  right after the four YUV draws (`OGRE_EXIT_AFTER_MS=29500` at
  `OGRE_SPEED=6`) that RDRAM region is a uniform `0x0001`, and `0x0843` fills
  `0x80269660` (the pointer in the descriptor at `0x80243E24`).
* The framebuffer pointer table the readback indexes (`state[0x64] =
  *(u32*)(0x800B8204 + i*4)`) is **zero** in every dump (both builds, mid-run and
  end-of-run), so `state[0x64] = 0`; a copy from guest 0 explains the null
  build's all-zero result but not RT64's `0x0843`.
* **RT64 does have the machinery to serve this**: `Framebuffer::
  copyRenderTargetToNative` → `copyNativeToRAM` (`rt64_state.cpp:1458`/`1479`,
  `rt64_framebuffer.cpp:126`/`147`) copy a *rendered* framebuffer's rows back into
  RDRAM (word-swapped), and `NativeTarget::copyToRAM` implements the readback
  itself — so a CPU read of a rendered framebuffer is supported **provided RT64
  marked that framebuffer pair as used**. It only runs inside the
  `getFramebufferPairs(pairCursor)` loop, i.e. per framebuffer pair with a
  non-empty draw rect.
* **The prime suspect is RT64's YUV texture path**: the per-macroblock draws use
  `SETTIMG fmt=1` = `G_IM_FMT_YUV`/`siz=2` (verified in the decoded DL), and
  `rt64_rdp.cpp:618`/`:745` carry `assert((t.fmt != G_IM_FMT_YUV) && "YUV is not
  currently supported.")` (under `ASSERT_LOAD_METHODS`, so inert in a release
  build). `src/shaders/TextureDecoder.hlsli` *does* have `case G_IM_FMT_YUV`, so
  the decoder may be half-implemented: a constant `0x0843`-everywhere result is
  what a mis-decoded tile would look like, and a dropped draw would leave the
  framebuffer pair unmarked and RDRAM stale — both produce the measurement.

**Next session's discriminating experiment** (in order): (1) check whether the
draw's framebuffer pair is marked at all — break/log in
`rt64_state.cpp` around `copyRenderTargetToNative` (`OGRE_DL_ANALYZE`/RT64
developer mode also prints command warnings); (2) if the pair *is* marked and
RDRAM still stale, suspect the writeback path; (3) if it is not marked, the YUV
tile is being dropped — compare `RDP::loadTile`'s `siz`/`line` handling for
`fmt=1` against `TextureDecoder.hlsli`'s YUV case and implement what is missing
(the project already patches RT64, see `rt64-ob64.patch`). Note that a
`G_IM_FMT_YUV` 16-bit tile has the same `line`/`siz` geometry as RGBA16, so only
the decode is special.

## 5. What is NOT established

* That the decoded YUV pixels are numerically correct (only that all `mbs` blocks
  are written with plausible entropy and the game proceeds). A visual check needs
  a YUV16 MB-layout decoder, which `tools/rdram.py` does not have yet.
* The `state[0x64]`/`0x800B8204` relationship (does the game's overlay-C
  framebuffer setup ever run on this path, and is `0x800B8204` supposed to be
  non-zero?).
* Whether RT64 drops the `0x800A5110` YUV draw (and therefore never marks the
  framebuffer pair, so no writeback) or draws it into a constant. The renderer
  matches no GBI hash for that ucode but also does not log "not supported in
  HLE", and the DL decodes cleanly offline (§3.4).

## 6. Verification runs this session

```sh
# the decoder, after the fix (null build, opt-in knob):
OGRE_NJPEG=1 OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=45000 ./build-null/ogrebattle64
#   -> probe47: all four tasks write every mbs block, reason=1 (Broke); scene 0x0D
#      (step 2) at t≈27.9 s and the run continues

# the decoder in the RT64 build + an RDRAM dump right after the draws:
OGRE_NJPEG=1 OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  OGRE_SCENE_LOG=1 OGRE_DUMP_RDRAM=/tmp/cath_rt64_mid.bin \
  OGRE_EXIT_AFTER_MS=29500 ./build-app/ogrebattle64
#   -> 0x80025C00 (G_SETCIMG) uniform 0x0001, 0x80243E28 uniform 0x0843

# which tasks reach the renderer (the DL log line is sampled without this):
OGRE_SPEED=6 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D OGRE_DL_TRACE=1 \
  OGRE_EXIT_AFTER_MS=75000 ./build-app/ogrebattle64
#   -> 6558 x ucode=0x8009F540 and 4 x ucode=0x800A5110 (the YUV draws)

# who writes the assembled image:
OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
  tools/watch.sh 0x80243E20 --after func_80178920 --ignore 1 --hits 3

# regenerate the microcode (must be re-run after editing rsp-njpeg.toml):
make rsp-recomp && cmake --build build-null -j && cmake --build build-app -j
```

No repo test harness exists; each check is a ROM-dependent 30–75 s game run.

## 7. Files changed

* `rsp-njpeg.toml` — `text_address` `0x8009ED80` → **`0x1080`** (the RSP IMEM DMA
  address; the label base) with the instruction-level explanation. Regenerated
  `RspFuncs/njpeg_ucode.cpp` (gitignored).
* `docs/guides/rsp-microcode.md` — the `njpg` section rewritten: the CPU Huffman
  decode is `func_8008B250`, the decoder is correct now, the label-base rule, and
  the remaining readback wall.
* `docs/guides/app-build.md` — the `OGRE_NJPEG=0` knob row (the decoder is on by
  default).
* `PLAN.md`, `docs/DECISIONS.md`, `docs/README.md`, `AGENTS.md` (load-bearing
  facts) — record updated.
* `docs/HANDOFF-2026-09-15-session47.md` (this file).
* `app/src/rsp.cpp` — the decoder is selected by default (`OGRE_NJPEG=0` forces
  the stub). **Probes only, all reverted**: `// probe47` (the task buffer
  hexdump, the before/after non-zero counts, the per-block fingerprints and the
  decode timer reported in §2). `grep -rn probe47 app/src/ RspFuncs/` is empty
  and both builds are rebuilt clean.
* No runtime/vendored change; `tools/` untouched. `tools/RT64` was already dirty
  before this session (the project's `rt64-*.patch`es on the submodule) and is not
  touched here.
