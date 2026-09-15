# Handoff — 2026-09-15, session 50: RT64's YUV16 decode is fixed (the black backdrop's cause); the readback's render timing is now isolated

## Goal and result

**Goal:** the New Game cathedral (scene `0x0D` step 2) has a black background
(`docs/proofs/native-newgame-cathedral-background.png`).

**Result: the first of the two independent causes is fixed and in the tree.**
RT64's texture decoder returned **black for every YUV texel**
(`TextureDecoder.hlsli`: `case G_IM_FMT_YUV: return float4(0,0,0,1)`), and the
njpeg background is drawn as `fmt=1 siz=2` YUV16 macroblock textures — so the
colour image the game reads back was uniformly black *by construction*. That
decoder is now implemented from the RSP decoder's byte layout and the game's own
`G_SETCONVERT` matrix (below), and it is no longer a stub.

**The second cause is measured, not fixed: the game's CPU readback runs before
RT64 has rendered the draw.** The game submits the `0x800A5110` macroblock draw
and, because the port completes the emulated RSP task as soon as the display list
is handed to RT64, immediately copies the framebuffer. At that instant
`framebufferManager` holds 2 entries, neither of them a game framebuffer, and the
copy source (`0x80000400`, or the njpeg target `0x000400` via the readback patch)
holds 3 non-zero bytes out of 7 680. This session added a **bounded wait on the
RSP worker** (`Application::waitForGameFramebuffers`) so the game's own task wait
covers the render; it works (the wait reports `found=1` immediately once the
draw's workload is built) but the four copies at the start of scene `0x02` still
see zeros — see §5, which corrects session 49's reading of those copies.

## 1. The bug: RT64 decodes every YUV texel as black

`tools/RT64/src/shaders/TextureDecoder.hlsli` had, in **four** format switches
(`sampleTMEM4b`, `8b`, `16b`, `32b`):

```hlsl
case G_IM_FMT_YUV:
default:
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
```

The njpeg draw is `[yuv] setTextureImage fmt=1 siz=2 width=16 address=0x001D1030…`
(one 16x16 tile per macroblock) `→ [yuv-at] … cimg=0x00000400`, so every one of
those 302 draw calls painted black. The CPU readback then copies the framebuffer
it chose, which is black — and that is the black backdrop. Session 49's mid-run
"the backdrop draws" reading of a 1280x715 present capture was the dialogue box
and sprites, exactly as its own postmortem says.

## 2. The correct YUV16 layout (from the code that writes it, not by guessing)

The TMEM layout is pinned by the RSP decoder that produces it — mupen64plus-rsp-hle
`jpeg.c`, `jpeg_decode_OB` (Bobby Smiles' upstream fix for
[mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102),
the "Missing backgrounds in Ogre Battle 64" issue) → `GetUYVY`:

```c
static uint32_t GetUYVY(int16_t y1, int16_t y2, int16_t u, int16_t v)
{
    return (uint32_t)clamp_u8(u)  << 24 |
           (uint32_t)clamp_u8(y1) << 16 |
           (uint32_t)clamp_u8(v)  << 8  |
           (uint32_t)clamp_u8(y2);
}
```

and `EmitTilesMode2` stores those words at `address` and `address + 32` with
`address += 64` per iteration — eight 32-bit words per 16-texel line, two texel
rows per line. For a 32-bit word at byte offset `4*(2t + (s >> 1))` the bytes are:

```text
byte0 = Y(s even)   byte1 = V   byte2 = U   byte3 = Y(s odd)
```

The RDP samples it as two planes: the luma byte in the **upper** TMEM half
(`OR 0x800`) and the chroma pair in the lower half, with the same XOR-3 / XOR-4
swap `sampleTMEM16b` already uses for 16-bit texels. `sampleTMEMYUV16` implements
exactly that (RT64's own `sampleTMEM` computes a one-texel-per-16-bit address,
which is wrong for YUV, so YUV gets its own path).

**This is the layout to keep.** Session 49's blobs came from inventing a byte
pairing; the pairing here is the one the decoder writes.

## 3. The conversion matrix is the game's own

`RDP::setConvert` already received `G_SETCONVERT` values but nothing read them.
`OGRE_CONVERT_TRACE=1` shows the game programming exactly one matrix:

```text
[convert] n=2 k0=175 k1=469 k2=423 k3=222 k4=114 k5=42
```

The hardware applies it to the sign-extended chroma bytes as

```text
R = Y + (K0*V + 0x80) >> 8
G = Y + (K1*V + K2*U + 0x80) >> 8
B = Y + (K3*U + 0x80) >> 8
```

\> `sampleTMEMYUV16` applies that matrix, passed down from the pixel shader
(`RasterPS.hlsl` already has `instanceRDPParams[instanceIndex].convertK`), and
falls back to the same NTSC matrix encoded as `175/469/423/222` when nothing has
been programmed (the compute-shader decode path cannot see draw state). K2 is
carried as `k2/32` and scaled back inside, because `G_SETCONVERT`'s K2 field
straddles a bit boundary.

## 4. What was verified

* `make`/`cmake --build build-app` and `build-null` are clean.
* `OGRE_CONVERT_TRACE=1` confirms the matrix above.
* `OGRE_YUV_TRACE=1` confirms 30 `setTextureImage fmt=1 siz=2` per image with
  `cimg=0x000400`.
* The forced shortcut (`OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1`)
  still reaches scene `0x0D` and renders the cathedral dialogue (Montage of
  presented frames: Archbishop Odiron, statues, candles, characters).
* **The background is still black in that scene**, because of §5.

## 5. The four copies ARE the cathedral njpeg readback (post-session follow-up, static trace)

This section replaces an earlier, **wrong** session-50 reading that said the four
copies were "not shown to be the njpeg readback". A read-only instruction-level
trace (no probes, no rebuild) settled it the other way, and session 49's original
reading was right:

* **Sole caller.** `func_ovlE_8019976C` has exactly one `jal` in the whole ROM:
  bankE `0x80199D80` (ROM `0x69020`), inside `func_ovlE_80199D30`'s per-image
  drive loop (wait for stage 6 → call until stage == 6). Its address appears once
  as data, at ROM `0x6E548` = `bankRec17`'s own jump table — record-17 addresses
  that only *numerically* coincide with unit E's layout (rec17 and rec0 both load
  at `0x80197B90`), not a pointer to this function.
* **The four sizes are four separate jobs of ONE `'B5'` asset.** The container is
  8-byte header `{'B','5',byte2,count}` + per sub-image a 16-byte chunk header
  `{4 flags, u16 w, u16 h, u32 size}` + `size` bytes of `'HU'` data. Asset ROM
  `0x7CADAC` is the unique ROM match for the runtime header id bytes
  (`42 35 ?? 04 01 15 00 EB`), and its chunk headers are **exactly**
  320×240 (`0x6DC8`), 176×240 (`0x3338`), 320×144 (`0x42E0`), 176×144 (`0x21C8`) —
  the four measured sizes, in order. Independent confirmation: the four type-4 RSP
  tasks have `data_size` `0x12C, 0xA5, 0xB4, 0x63` = 20×15, 11×15, 20×9, 11×9
  macroblocks. Each sub-image is its own njpeg decode with its own state; they are
  **not** tiles of one job (only sub0's pixels are blitted, and whether the other
  three are resolutions or a split is unproven).
* **The copy source is a real framebuffer, not a placeholder.** `state[0x64]` is
  set in `func_ovlE_80199A08` at `0x80199B88`/`0x80199B98` from the framebuffer
  table `0x800A8204` (ROM `0x38604`), indexed by the 3-way compare of
  `D_800C4BB8` (`0x80199B14-0x80199B68`, gated on `D_800E7A0C == 1`). A live dump
  with unit E resident has `D_800C4BB8 = 0x80000400` → `state[0x64] = 0x80025C00`
  (= table entry 1), and that same buffer is both the `G_SETCIMG` target of the
  `0x800A5110` draw and the RSP task's output (`func_ovlE_80198E70` stores
  `state[0x64]` into `task+0x0C`).
* **The `'B5'` container is built by `func_ovlE_80199D30`'s merge path**
  (`0x80199E44-0x80199F70`: memcpy the asset's 8-byte header, each chunk's 16-byte
  header, alloc, write the node's raw size `w*h*2`, then memcpy pixels from
  `state[0x88+4*i]`). Runtime header at `0x80243E10` =
  `42 35 01 04 | 01 15 00 EB | FF 1B FF 15 01 40 00 F0 | 00 02 58 00 | 80 26 96 50`,
  i.e. `0x80243E28` = +24 = sub0's pixels = the address scene `0x0D` step 2's
  `SETTIMG` uses. So the 320×240 background **is** decoded through
  `func_ovlE_8019976C`, and no other CPU framebuffer copy produces it.
* **Correction to the container magic.** The `sb 0x36 / sb 0x34` (`'6','4'`)
  header written inside `func_ovlE_8019976C` (`0x801998E0-0x80199940`) is the
  *single-image* path (`state[0x80] == 0`); for this `'B5'` asset
  `state[0x80] = 1`, so no header is written there and the `'B5'` header comes
  from the asset via the merge memcpys above. The earlier `'64'`/`'B5'` conflation
  (sessions 47/48/50) is wrong on this point.
* **Why the copies precede scene `0x0D`:** scene `0x02`'s enter `func_80178920`
  calls into unit E and sets next = `0x0D`; scene `0x02`'s mask `0x00004001` loads
  ROM `0x066E30` (unit E `bankRec0`, 21 functions) at `0x80197B90`, whereas
  `0x0D`'s mask `0x40007C14` loads ROM `0x0E4910` (unit A `bankRec2`) over that
  same RAM. The background is therefore decoded during the scene-`0x02` visit that
  precedes `0x0D` — it is preloaded, and finished before `0x0D` starts at
  t≈3.7 s.

**What this means for the wall:** the readback that matters runs in **scene
`0x02`**, and the draw whose pixels it needs is the `0x800A5110` display list
submitted in that same scene. In `/tmp/final_check.log` the RSP-worker wait
reports `[njwait] found=0` throughout scene `0x02` (it times out) and only
`found=1` after `0x0D` starts — so the wait is not covering the draw that matters,
and `[renderer] display list 2 … ucode=0x800A5110 data=0x80025C00` needs checking
for whether that list actually contains geometry.

## 6. The render-timing fix that is in the tree

`app/src/renderer.cpp` (`send_dl`) now calls
`Application::waitForGameFramebuffers(OGRE_NJ_WAIT_MS, default 500)` **after**
`processDisplayLists`, i.e. on the RSP worker thread, not the game thread. The
game's own wait for that RSP task therefore also covers the render, which is what
the CPU readback needs. A bounded wait on the *game* thread deadlocks (the
renderer thread cannot always advance while a game thread is blocked); the RSP
worker can wait safely, and the wait returns as soon as a game framebuffer exists.

`OGRE_NJ_WAIT_MS=0` disables it. Observability: `OGRE_SYNC_TRACE=1` prints
`[njwait] spins=… found=… ms=… fbs=… queueId=… workloadId=…` and, from
`State::syncFramebuffers`, `[syncfb] enter:`.

## 7. Files changed

* `tools/RT64/src/shaders/TextureDecoder.hlsli` — `sampleTMEMYUV16` (YUV16 TMEM
  layout + G_SETCONVERT conversion) and `sampleTMEMWithConvert`; the old
  `sampleTMEM` is a thin wrapper so the compute-shader decode path is unchanged.
* `tools/RT64/src/shaders/TextureSampler.hlsli` — thread the convert matrix
  through `clampWrapMirrorSample` → `sampleTextureLevel` → `sampleTexture`.
* `tools/RT64/src/shaders/RasterPS.hlsl` — pass `convertK[0..3]` for both texture
  stages.
* `tools/RT64/src/hle/rt64_state.{h,cpp}` — `State::waitForGameFramebuffers`;
  `OGRE_SYNC_TRACE` now also prints the game-framebuffer pairs RT64 builds
  (`[njpair]`).
* `tools/RT64/src/hle/rt64_application.{h,cpp}` — pass it through.
* `app/src/renderer.cpp` — the bounded wait in `send_dl`; keeps the
  `OGRE_CONVERT_TRACE` hook in the RT64 side.
* `tools/njpeg_readback.py` — comment only: the render wait now lives on the RSP
  worker (a game-thread wait can deadlock).
* `docs/HANDOFF-2026-09-15-session50.md` (this file), plus `PLAN.md`,
  `DECISIONS.md`, `AGENTS.md`, `docs/README.md` updates.
* `tools/RT64` remains dirty (the project's own patches). **`rt64-ob64.patch` was
  refreshed at the end of the session and verified**: it is
  `git -C tools/RT64 diff HEAD -- . ':(exclude)src/contrib/plume'` (11 files), and
  applying it to a **clean** submodule worktree (`git worktree add --detach` at
  `HEAD` = `4337374`) reproduces the dirty tree byte for byte
  (`diff` of the two `git diff HEAD` outputs is empty). Note that a plain
  `git -C tools/RT64 diff` is **not** enough here — these changes were applied to
  the index, so the diff must be taken against `HEAD`. The pre-refresh file is at
  `/tmp/rt64-ob64.patch.bak` for this session only.
* **No temporary probes left**: `grep -rn "probe49\|probe50\|probe51\|p50call\|p50draw"
  app/src/ tools/RT64/src/ Bank*Funcs/ RecompiledFuncs/` is empty; `build-app` and
  `build-null` were rebuilt after the cleanup.

## 8. Next lead (in order)

1. ~~Identify the caller of the four stage-3 copies.~~ **DONE** (see §5): they
   *are* the njpeg readback, sole caller bankE `0x80199D80` inside
   `func_ovlE_80199D30`, and they run in scene `0x02` as a preload of the four
   sub-images of the `'B5'` asset at ROM `0x7CADAC`.
2. **Find out why the scene-`0x02` wait times out, i.e. why `0x800A5110`'s draw
   produces no game framebuffer.** In `/tmp/final_check.log` every `[njwait]`
   during scene `0x02` reports `found=0 fbs=2` (lines 236/733/1184/1585/2037) and
   the first `found=1` is line 2182, after `0x0D` starts (line 2040). Two candidate
   mechanisms, both cheap to discriminate: (a) the display list really is empty —
   `[renderer] display list 2 … ucode=0x800A5110 data=0x80025C00 entries=0`
   (`entries` is `debug_total_entry_count` delta, so 0 only means no recompiled
   function ran during the parse, not that the list is empty — use
   `OGRE_DL_ANALYZE=1` / `OGRE_DL_DECODE=<n>` for the command count); or (b) the
   draw is parsed and the framebuffer is built, but the wait samples the manager
   before the workload's fbPair is registered. Turn on
   `OGRE_NJ_WAIT_MS=0` + `OGRE_FB_WRITEBACK=1`/`OGRE_SYNC_TRACE=1` for a run and
   read `[fbpair]`/`[fbwrite]` for `addr=0x000400`/`0x025C00` to see which of the
   two it is.
3. **Check the tile `line` field for the YUV tile.** `stride` in RT64 is
   `tile.line << 3`; the working RT64 fork cited for this class of background
   reportedly also needed a `line << 4` fix for YUV texrects. `OGRE_WORKLOAD_TRACE`
   / a one-line trace of `rdpTile.stride` for `fmt=YUV` settles whether the 16x16
   macroblock tile is read as 16 or 32 bytes per line.
4. **Then re-run the A/B in session 49 §4** (framebuffer histogram of `0x000400`)
   with the decode in place, and check the assembled `'B5'` image at `0x80243E28`
   (`42 35 …` header at `0x80243E10`, pixels at `0x80243E28`) — it is still
   uniform in the last 20 s dump.

## 9. New finding: the `0x800A5110` display list carries texture loads but no geometry

Both the app's own analyzer and the full command decode say the macroblock draw
submits **no triangles and no rectangles**:

```text
OGRE_DL_ANALYZE=1 (static, 0x800A5110 lists):
[dl-analyze] dl=2 ptr=0x80025C00 cmds=1821 dls=1 tri1=0 tri2=0 quad=0 texrect=0 fill=0 vtx=0 verts=0 settimg=300 unknown=0
[dl-analyze] dl=3 … cmds=1011 … settimg=165
[dl-analyze] dl=4 … cmds=1101 … settimg=180
[dl-analyze] dl=5 … cmds=615 … settimg=99
```

`OGRE_DL_DECODE=2` (`dl=2 ptr=0x80025C00`, 114 139 bytes) shows the whole list is
`SETOTHERMODE_*` + `SETCIMG fmt=0 siz=2 width=320 addr=0x80000400` + `SETSCISSOR
0,0-1280,960` + `G_SETCONVERT` + `SETCOMBINE` + per macroblock
`{SETTIMG fmt=YUV siz=16b width=16 addr=0x801D1030+, SETTILE t7 fmt=1 siz=2 line=2
tmem=0, RDPLOADSYNC, LOADTILE t7 uls=0 ult=0 lrs=60 lrt=60, MOVEMEM (a matrix),
MTX}` — i.e. 300 × (texture image + tile + load + matrix). **There is no
`G_TRI1/TRI2/QUAD/TEXRECT/FILLRECT` anywhere in it.** The same shape holds for
dl 3/4/5, and for the other unit-E images (dl 9/10 carry `SETTIMG fmt=0`).

Two consequences:

* **RT64 is very likely correct to render nothing into `0x25C00` for these
  lists** — the task asked for a load, not a draw. That is consistent with
  `[njwait] found=0` (no game framebuffer is ever built), and with the readback
  source being empty.
* Session 46's "the second gfx task's DL has 300 macroblocks / the renderer
  receives the draws" was measured by counting `display list N` lines and
  `[yuv]` setTextureImage calls; **it never verified that the list contains
  geometry**. The open question is therefore upstream of the renderer: what turns
  these 300 texture loads into a picture on real hardware — a second half of the
  list that the port does not receive, or the game's own `G_MOVEMEM`/`G_MTX`
  stream being interpreted as vertices by the real ucode.

The builder corroborates the decode. `func_ovlE_80199130` writes the list into
the pointer it loads at `0x8019915c` (`lw s8, 0(a0)`), advances `s8` by 8 per
command, and its **inner** per-macroblock loop (`0x8019944c-0x801994fc`) emits in
order `{G_MTX/G_MOVEMEM (from `state[0x60] + (col + s*(w>>4)) * 8` at
`0x801994e0-0x801994f0`), SETTIMG (advancing `t1` by 768 = 0x300 per macroblock at
`0x801994d8`), SETTILE t7, RDPLOADSYNC, LOADTILE, MOVEMEM, MTX}` and loops until
the column counter reaches `state[0x78] >> 4`. There is no geometry write anywhere
in the function.

**So the previous session's pipeline description is wrong at this step:** the
`0x800A5110` task is not "drawing one 16×16 YUV16 texture per macroblock into
`G_SETCIMG`". It loads all 300 macroblocks into TMEM and emits no primitives. The
thing that consumes them — if anything does — has not been identified, and the
`G_SETCIMG`/`G_SETCONVERT`/`SETCOMBINE`/`SETSCISSOR` prefix may be setup for a
subsequent list rather than for this one.

**Answered by the discriminator run** (`OGRE_DL_TRACE=1`, forced step 2):

```text
display list 2 at t=867ms  (type=1 ucode=0x800A5110 data=0x80025C00)
display list 3 at t=1375ms (type=1 ucode=0x800A5110 data=0x80025C00)
display list 4 at t=1883ms (type=1 ucode=0x800A5110 data=0x80025C00)
display list 5 at t=2387ms (type=1 ucode=0x800A5110 data=0x80025C00)
```

Exactly **four** `0x800A5110` submissions in scene `0x02` — one per sub-image,
each ~508 ms of game time apart, matching the four readbacks — and **no fifth
list follows any of them** before the scene changes. So nothing downstream in the
port consumes the loaded macroblocks: the geometry is either never built by the
game (the piece that would draw them is missing/lost) or is built into a list
this port never receives. The renderer is not the wall for *these* lists.

**The wrappers are resolved and they confirm the list is exactly this:** at
`0x801995b4-0x80199600` the `a0 == 0` path appends `G_RDPFULLSYNC` + `G_ENDDL`
(`e9000000`, `df000000`) to the list and stores the result in `state[0x58]`;
`func_ovlE_80198FE8` (the submit function) builds the same list into `state[0x58]`
and passes it as the task's `data_ptr`. So what the port hands to RT64 really is
`SETOTHERMODE*/SETCIMG/SETSCISSOR/SETCONVERT/SETCOMBINE` + 300 ×
`{SETTIMG, SETTILE, LOADTILE, MOVEMEM, MTX}` + `RDPFULLSYNC` + `ENDDL`, with no
drawing primitive anywhere — and the game then CPU-reads the framebuffer and gets
zeros. The `func_ovlE_80199588(1)` branch (`0x80199604+`) sets up the task fields
(`type=1`, `ucode=0x800A5110`, `data=state[0x58]`, `output=state[0x64]`) and sends
the message; it does not build a second list.

**Open question, now sharpened:** either the macroblock geometry is built
somewhere this port never reaches (a second builder/list for the same textures),
or this task is meant only to *load* and something else must draw. The next cheap
experiment is to grep the ROM for other writers of a display list that references
YUV `SETTIMG` + a triangle/texrect opcode (search unit E for `lui/ori` pairs
equal to `0xBF`/`0xB1`/`0xB6`/`0xB7`/`0xE4`/`0xF1` in the low byte and for stores
adjacent to a `SETTIMG` in the same builder), which would name the drawing list
if it exists.

### Diagnostics that were used and are still available

```sh
# the shortcut to the cathedral (movie skipped)
OGRE_SPEED=6 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 \
  OGRE_SCENE_LOG=1 OGRE_SYNC_TRACE=1 OGRE_CONVERT_TRACE=1 \
  OGRE_EXIT_AFTER_MS=30000 ./build-app/ogrebattle64 assets/ogre64.z64
# -> [njwait] …, [syncfb] …, [njpair] …, [convert] …

# capture the presented frames (a 1x natural run reaches 0x0D at ~33 s, and the
# game reaches the movie first — tap Start to skip it, or use OGRE_STEP)
OGRE_SCENE_LOG=1 OGRE_PRESENT_ALWAYS=1 OGRE_CAPTURE_PRESENT=/tmp/v \
  OGRE_CAPTURE_AFTER=500 OGRE_CAPTURE_EVERY=200 OGRE_EXIT_AFTER_MS=180000 \
  OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D ./build-app/ogrebattle64 assets/ogre64.z64
```

No repo test harness exists; each check is a ROM-dependent run (the forced
shortcut lands in the cathedral ~1.4 s after boot at `OGRE_SPEED=6`).
