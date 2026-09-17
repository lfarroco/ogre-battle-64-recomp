# Handoff — 2026-09-17, session 64: the map's sprites, reframed — **how an emulator handles it**, and what that proves about the port

## Goal and result

**Goal (developer):** sessions 61–63 spent themselves on the map screen's
sprites (`scene 0x05`) and landed nothing. Reframe: *how would an emulator
handle it?* The mupen source and other emulator repos are available to read.

**Result:** the reframe works and it produced a decision, not another patch.

* **Nothing is landed in the port** — no code, no `Makefile`, no `tools/`
  changes. `git status --short` is doc-only plus the pre-existing `tools/RT64`
  submodule modification.
* **The port is faithful at every layer this session could verify**, proven
  four separate ways (§2). Sessions 60–63's shared premise — that the port's
  display list or decoded data is wrong, so the *constants in it* must be wrong
  too — is not supported. Two of their specific mechanisms are **wrong and are
  corrected here** (§1).
* **New sprite model of the map's party, verified at instruction level** (§3):
  the knight is **not** a decoded asset at all. It lives in a **`malloc(0x18000)`
  buffer the map's enter composites at runtime**, 32 texels wide, 24 frames of
  `0x1000` bytes selected by `3*direction + frame`. Both of the party's draws,
  their texture bases, and the animated frame offset are now known exactly.
* **One concrete, ROM-verified anomaly is found** and is the new top lead (§4):
  the knight's composited sheet is **32 texels wide (128 B/row)** while the
  draw's render tile declares **`line = 8` (64 B/row)** — exactly half. This is
  the game's own command (verified against the raw ROM), so it is not a
  recompiler artifact; it is either an RDP `G_LOADBLOCK`/tile-line semantics
  question or a renderer difference, and it is answerable from emulator sources
  without touching game data.
* **The emulator-side answer is written down** as a guide:
  `docs/guides/emulator-first.md`, including GLideN64's two per-game OB64
  workarounds (§5) and the recipe to verify which GBI a display list got.

## 1. Corrections (read this before session 61/62's prose)

### 1a. N64Recomp does **not** run a `jal` delay slot twice, and there is no "second window writer"

Session 62 §1 concluded that the caller's `G_SETTILESIZE` word wins over the
builder's because *"N64Recomp emits the delay-slot store, then the call, then a
`goto after_N` that re-emits the delay-slot store"* — i.e. the store executes
twice, after the callee. **That is false.** `BankMFuncs/funcs_0.c` (`func_ovlM_801A2A7C`)
emits:

```c
// 0x801A2C34: jal         0x8019F83C
// 0x801A2C38: sw          $t5, 0x44($v0)
MEM_W(0X44, ctx->r2) = ctx->r13;      // delay slot, executed BEFORE the call
func_ovlM_8019F83C(rdram, ctx);
    goto after_0;
// 0x801A2C38: sw          $t5, 0x44($v0)
MEM_W(0X44, ctx->r2) = ctx->r13;      // DEAD: the goto skips it
after_0:
```

The duplicate exists so that a *fall-through entry* at `jal+4` still works; it is
unreachable from the call. The delay slot runs **once, before the callee** —
which is MIPS, and which is what the port does. So `0x1C028` is simply the
game's own `G_SETTILESIZE` right/bottom, written once, and nothing clobbers it.

Consequence: `docs/DECISIONS.md`'s entry ending *"On hardware the delay slot runs
after the callee, so the callee's store wins… check the `jal` delay slot of the
call site first"* is **wrong** and is corrected by the appended entry there.

### 1b. The builder emits no `G_SETTILESIZE` at all — session 61 §4(a)'s "second writer" is a phantom

Session 61 §4(a) left "the window writer" unidentified; session 62 §1 answered it
with the delay-slot store. In fact there was never a second writer to find.
`func_ovlM_8019F83C` (`build/bankM.elf`, `0x8019F83C..0x8019F9DC`) emits exactly:

| at | words | command |
|---|---|---|
| `t1+0x00` | `E7000000 00000000` | `G_RDPPIPESYNC` |
| `t1+0x08` | `E4 \| (x+W)<<12 \| (y+H)` , `(x)<<12 \| (y)` | `G_TEXRECT` w0/w1 (lower-right, then upper-left) |
| `t1+0x10` | `E1000000` , `=(u<<5)<<16 \| (v<<5)` | `G_RDPHALF_1` carrying **s,t** |
| `*DL+0` | `F1000000 04000400` | `G_RDPHALF_2` + **dsdx = dtdy = 1.0** |
| `*DL+8` | `E7000000 00000000` | `G_RDPPIPESYNC` |

The word at `0x8019F990` that sessions 61/62 called "the builder's own window"
(`((W+u)&0xFFC)<<12 | ((H+v)&0xFFC)`) is the **TEXRECT's `s`/`t` halfword pair**:
`u` and `v` are shifted into the s,t positions (`u<<21 | v<<5`), not into a
tile-size word. There is no `SETTILESIZE` in the builder, so the caller's
`0x0001C028` is the only one and it applies to that draw. The "uv-derived window
is degenerate for `u=v=0`" reasoning built on it is void.

## 2. What the port is faithful about (four independent checks)

1. **The LZ decoder.** `tools/ogrelz.py` decodes the map's 13 real assets and the
   compressed token stream ends **exactly** on each asset's declared payload
   boundary (`consumed == payload`) for **13/13**, payloads `0x178`..`0x2439`,
   outputs `0x2A8`..`0x7E20`. A misread token desyncs the stream; landing exactly
   on the boundary 13 times is not a coincidence. The decoder is correct.
2. **The port's decoded bytes.** The live RDRAM at the map's `state[+0x04]`
   (`0x80219C20`, `OGRE_DUMP_RDRAM` image) is **21128/21128 bytes identical** to
   the offline decode of asset `0x01DD210A`. Nothing is missing from that buffer.
3. **The ucode / GBI dispatch.** Scene `0x05`'s gfx tasks carry
   `ucode = 0x8009F540`. XXH3-64 of the **raw** RDRAM there at the database's
   text-segment length `0x1390` is `0xCF55FAE288BFE48D` = RT64's
   **`F3DEX2.fifo 2.08`** entry (`tools/RT64/src/gbi/rt64_gbi.cpp`). RT64 picks
   the F3DEX2 GBI, so reading that list as F3DEX2 is correct — the sessions
   60–63 geometry readings are not a ucode-dispatch artifact. (Recipe:
   `docs/guides/emulator-first.md` §3.)
4. **The recompiled code vs. the ROM.** Every constant this session relied on was
   read back out of `assets/ogre64.z64` at `ROM = 0x79750 + (ram - 0x8019A7C0)`
   and matches the bank-M ELF: `0x81B50` `2406000b` (`a2 = 0xB`), `0x81BFC`
   `2406000a` (`a2 = 0xA`), `0x81BB8` `24e71068` (`a3 += 0x1068`), `0x799F0`
   `252a0020` (composer row = `0x20`), `0x79A40` `26100004`, `0x79A48` `28c20300`,
   `0x799C4` `8c500034`.

Also checked and negative: **no RSP task is being swallowed for this screen.**
Scene `0x05` (forced, 4×) submits only type-1 (gfx, `ucode=0x8009F540`) and
type-2 (audio, `ucode=0x8009E050`, 1706 tasks) — the audio stub is expected (the
port has no audio yet). So "a missing task filled that buffer" is not a live
hypothesis for the map.

## 3. The map's party, verified at instruction level

`state` is `*(0x80197B18)`. The map's enter (`func_ovlM_8019A7C0`) mallocs a
`0x470`-byte screen state at `0x8019A7E4`-ish and stores it there, then fills its
fields by **software-pipelined asset decodes** — each `jal func_8009DD38` writes
the *previous* call's return into a field from its delay slot:

```c
// 0x8019A854: jal func_8009DD38        (a0 = 0x01D9197A)
// 0x8019A86C: jal func_8009DD38        (a0 = 0x01DD210A)
// 0x8019A870: sw $v0, 0x0($v1)         -> state[+0x00] = decode(0x01D9197A)
// 0x8019A888: sw $v0, 0x4($v1)         -> state[+0x04] = decode(0x01DD210A)
```

Live (exit dump `/tmp/map3.bin`, `state = 0x801F1570` — a heap address, not a
fixed one): `state[+0x00] = 0x801F1A00`, `state[+0x04] = 0x80219C20`,
`state[+0x08] = 0` (**this field is never written by the enter** — it is not a
failed decode), `state[+0x28] = 0x8021EED0` (asset `0x01DC2B6C`, which decodes to
only `0x2A8` bytes), and the rest densely packed up to `state[+0x38]`.

**`state[+0x34]` is not an asset.** At `0x8019A9E0..0x8019A9F4` the enter does
`a0 = 0x18000; v0 = func_80070F30(a0); state[+0x34] = v0` — a **malloc**. Then
`0x8019AA30..0x8019AAC0` composites two more decoded assets into it (one is an
RGB555 LUT, one an 8-bit index image; both are freed afterwards):

```
inner loop: a3 = t1 .. t1+0x20        ; 0x20 source bytes
            v0 = 2*lbu(a3) + LUT      ; index into the 16-bit LUT
            v1 = R<<27 | G<<19 | B<<11 | alpha8      ; a 32-bit texel
            sw v1, 0(s0) ; s0 += 4
outer loop: a2 = 0 .. 0x300 (768) ; t1 += 0x20
```

so the destination is `768 * 32 * 4 = 0x18000` bytes of RGBA32 — **32 texels
wide, 1536 rows**.

### The party draw: `func_ovlM_801A2A7C(x, y)` — corrected

| | call 1 (ROM `0x81B50`) | call 2 (ROM `0x81BFC`) |
|---|---|---|
| rect origin | `(x-8, y)` — **lower** | `(x-16, y-24)` — **upper** |
| sprite-table entry | `a2 = 0xB` = 11 = `(144,23)` | `a2 = 0xA` = 10 = `(16,11)` |
| texture address | `state[+0x04] + 0x1068` (**static**) | `state[+0x34] + ((3*state[0x1DC] + f) << 12)` (**animated**) |
| its own tile setup | `line=2`, `cms=WRAP masks=3`, `SETTILESIZE lrs=28 lrt=40` (7x10 texels) | `line=8`, clamp, `SETTILESIZE lrs=124 lrt=124` (31x31 texels) |
| dsdx/dtdy | `1.0` (from the builder) | `1.0` (from the builder) |

`f` is `(state[+0x108] * 0xAAAAAAAB) >> 34 & 3`-style mod-3 from the divisor
sequence at `0x801A2B18`, so the frame index is `3*direction + f` in `0..23` —
**24 frames of `0x1000` bytes**, i.e. 8 directions × 3 animation frames, which is
exactly `0x18000 / 0x1000`. The knight therefore *is* animated and *is* drawn
from the composited buffer; the developer's "you can see his hair in some
sprites" matches a real, partially-correct draw.

`state[+0x1DC]` is the direction and is zeroed by the enter's tail
(`0x8019AB14: sb $zero, 0x1DC(v1)`), i.e. the map starts on direction 0 frame 0.

### The shadow is a separate, static read of a **genuinely empty** region

`state[+0x04]` is asset `0x01DD210A` (byte-verified, §2.2). Its byte `+0x1068` is
**all zero** — 24 zero bytes then `0c` — and the only translucent-black run
(RGB `000000`, alpha varying `0x65`..`0xAA`, i.e. a soft shadow) anywhere in any
of the map's decoded assets is in that same asset at **`+0x10AC`, a run of 47
texels**. So the shadow art exists, it is in the asset the draw reads, and it
starts `0x44` bytes after the offset the ROM uses. **Whether that `0x44` means
anything is not established** — do not "fix" it by editing `0x1068`.

## 4. The new top lead (ROM-verified, renderer-side)

The knight's composited sheet is **32 texels wide**, row stride **128 bytes =
`line` 16**, and its frames are `0x1000` bytes. The draw's render tile
(`+0x28` of the caller's block) is

```
F5181000   ->  fmt=RGBA (0), siz=32b (3), line = p0(9,9) = 8, tmem = 0
```

`line = 8` is **half** the sheet's row stride. RT64 reads that field the standard
way (`tools/RT64/src/gbi/rt64_gbi_rdp.cpp` `setTile`: `line = p0(9,9)`), so the
sampler advances 64 bytes per drawn row while the data advances 128 — each drawn
row would take 16 texels, alternating halves of successive sheet rows. That is a
plausible mechanism for a garbled-but-recognisable knight, and it is the same
class of statement as session 22's fog work: **the RDP's `G_LOADBLOCK` /
tile-line interaction, not game intent.**

Note the arithmetic that *does* line up if the stride is read as 16-bit:
`line=8` = 64 B = **32 RGBA16 texels**, and the window `lrs=124` = 31 texels fits
inside 32; call 1's `line=2` = 16 B = **8 RGBA16 texels**, matching its
`cms=WRAP masks=3` (wrap at 8) and `lrs=28` (7 texels) *exactly*, whereas read as
32-bit (`line=2` = 4 texels) a 7-texel window cannot fit a 4-texel row. **A
16-bit read of both tiles is internally consistent; the 32-bit read is not.** The
composer writes 32-bit words, so this is a real question about how OB64 feeds the
RDP, not a typo.

**This is the experiment to run next**, and it needs no game-data theory:
compare RT64's `loadBlock`/tile sampler with GLideN64's and parallel-rdp's for a
32-bit `SETTIMG` whose render tile declares a `line` half the image row, and
check what each does with `G_LOADBLOCK`'s `lrs`/`dxt` (`t0 = 0x073FF080`:
`lrs = 0x73F`, `dxt = 0x080` — not the `1<<11` a 32-bit block would suggest).

## 5. What the emulator sources actually say (the reframe's answer)

Recorded in full in **`docs/guides/emulator-first.md`**; the load-bearing parts:

* **The three pillars.** An emulator renders correctly because it (i) identifies
  the GBI by hashing the ucode, (ii) runs a faithful RSP (HLE for known ucodes,
  LLE otherwise), and (iii) *is* ground truth. The port has (i) — RT64's
  `getGBIForUCode` — but `app/src/rsp.cpp` is a **stub for everything but the
  Nintendo-JPEG decoder**, and the port has no reference. Sessions 60–63 got
  their "the game's data is wrong" conclusions from treating the port's own
  decoder output as ground truth.
* **GLideN64 ships per-game OB64 handling** — `ini/GLideN64.custom.ini`:
  `[OGREBATTLE64] graphics2D\enableTexCoordBounds=1`, described by
  `src/mupenplus/Config_mupenplus.cpp` as *"Bound texture rectangle texture
  coordinates to the values they take in native resolutions. It prevents garbage
  due to fetching out of texture bounds, but can result in hard edges."*
  Implemented in `src/uCodes/S2DEX.cpp` (`S2DEX_Obj_Sprite`, `gDP.m_texCoordBounds`).
  Second hook: `src/RSP.cpp` matches romname `OgreBattle64` → `hack_Ogre64`, used
  only by `gSPObjRectangleR` → `_drawYUVImageToFrameBuffer` (*"Ogre Battle needs
  to copy YUV texture to frame buffer"* — the njpeg background this port already
  has). Both hooks are in GLideN64's **S2DEX** handler and the party draw is
  F3DEX2, so neither is obviously *this* bug — but together they establish that
  OB64's 2D drawing leans on RDP texture-window behaviour that naive
  implementations get wrong, which is the shape of §4.
* **mupen64plus-rsp-hle knows exactly one OB64 thing** — the JPEG ucode
  (`src/hle.c`, `sum 0x130de` / `0x278b0` → `jpeg_decode_OB`, "found in Ogre
  Battle, Bottom of the 9th"), already implemented here — and its default for an
  unrecognised non-audio task is to hand the display list to the gfx plugin. It
  has **no** gfx ucodes; graphics HLE lives in the video plugin, which is why
  GLideN64 is the reference for sprites and rsp-hle is not.
* **A reference emulator is already on this machine**: RetroArch +
  `mupen64plus_next_libretro.dylib` + `parallel_n64_libretro.dylib`, and
  `~/Documents/RetroArch/system/Mupen64plus/mupen64plus.ini` is mupen64plus's
  game database (OB64 US entries `E6419BC5 69011DE3` / `0ADAECA7 B17F9795`, both
  with `SaveType=SRAM`). §5 of the guide is the recipe for turning that into an
  RDRAM ground-truth dump.

## 6. Files changed

* `docs/guides/emulator-first.md` (**new**) — the methodology, the GBI-hash
  recipe, the five-check pre-theory checklist, the reference-emulator recipe, the
  GLideN64/mupen findings, and the "stop patching generated C" pattern.
* `docs/HANDOFF-2026-09-17-session64.md` (this file), `docs/README.md`,
  `PLAN.md`, `docs/DECISIONS.md`.
* **No code, `Makefile` or `tools/` changes.** `/tmp/atlas.py`, `/tmp/cmp.py`,
  `/tmp/render*.py`, `/tmp/bo.py` are throwaway analysis scripts outside the
  tree. Generated trees (`Bank*Funcs/`, `RecompiledFuncs/`) were not touched this
  session, so no probe revert was needed; nothing was probed.

**Vendored/gitignored touched:** none. `tools/RT64/src/contrib/mupen64plus-core`
(existing) and a throwaway `git clone` of `mupen64plus-rsp-hle` and `GLideN64`
into `/tmp/emu-research/` were read; nothing was copied into the repo.

## 7. Next leads, in order

1. **The tile `line` vs. the sheet stride (§4).** Compare RT64's
   `setTile`/`loadBlock`/texture-address path (`rt64_gbi_rdp.cpp`,
   `rt64_rdp_tmem.cpp`, the sampler) with GLideN64's `F3DEX2_SetTile`/
   `gDPLoadBlock` and parallel-rdp's `loadBlock`/`Tile`. Specifically: what does
   each do when a `G_LOADBLOCK`-loaded tile's declared `line` is half the source
   image's row, and what `lrs`/`dxt` does a 32-bit block use? **No game data
   changes.** This is the cheapest route to a real fix.
2. **Reference RDRAM at the same screen** (`docs/guides/emulator-first.md` §5):
   RetroArch + Mupen64Plus-Next to the map, save state, pull RDRAM from it with
   mupen64plus's `savestate.c`, and diff `state[+0x04]+0x1068`,
   `state[+0x04]+0x10AC` and `state[+0x34]` against the port's `/tmp/map3.bin`.
   That settles §4 *and* the "the port reaches this screen in a different state"
   possibility in one run. `state` is a heap pointer that varies per run — read
   it from `*(0x80197B18)` in each image, never hardcode `0x801F1570`.
3. **`enableTexCoordBounds`' equivalent in RT64** (guide §6): if RT64 has none,
   an A/B of bounding a TEXRECT's s/t to its tile window is a renderer-only
   experiment that needs no theory about the game.
4. **Only then**, the entry indices (`0xB`/`0xA`) and the shadow offset
   (`0x1068` vs `0x10AC`) — and only if §1/§2 have not already explained them.
   They are the game's own constants (ROM-verified); changing them is
   fabricating game data (AGENTS §7) and must not be the first move.
5. The cursor (`func_ovlM_801A1C50`, `a2=6`) and the date panel (`a2=12`) are the
   same rect-vs-window class and should be re-examined once §4 is settled.
