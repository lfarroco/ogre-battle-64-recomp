# Handoff — 2026-09-11 — session 21: segmented addresses (the "runaway walk" root cause), the othermode decode, and an RT64-faithful combiner

## Outcome

Session 20 ended with two open threads: the intro stops ~30 frames in with a
"runaway walk" (4 M commands analysed, `@0x0FE835F0 op=0x00`), and several
renderer-fidelity questions (combiner mux tables, TEXEL1, texture binding).

**The runaway walk is root-caused and fixed.** It was not a corrupt *entry*
pointer at all: OB64's display lists mix KSEG0 pointers with **segmented** ones
(`DE000000 0E000000` = segment 14 + offset 0, `FD180000 0F000000` = segment 15 +
offset 0), and the renderer resolved every segment as
`(high_byte << 24) | offset`. RT64's rule (`RSP::fromSegmented`) is
`segments[(addr >> 24) & 0x0F] + (addr & 0xFFFFFF)` with the **full** base stored
by `G_MOVEWORD G_MW_SEGMENT`. Resolved correctly, the two addresses above are
the game's asset-area display list `0x001CAB20` and texture `0x001BE4E0`; under
the old rule they became physical `0x0E000000` (zeroed rdram → 4 M `G_NOOP`s)
and `0x0F000000` (garbage texels). One bug, two symptoms.

Three more decode bugs were fixed on the way, one of which explains why the
combiner question was unanswerable before:

| # | Bug | Effect |
|---|---|---|
| 1 | `G_SETOTHERMODE_H/L` data word was masked and shifted *again* (it is already pre-shifted: `H = (H & ~mask) \| w1`) | `OTHERMODE_H` was **always 0** → every draw was `G_CYC_1CYCLE`, no dither, point filtering, no perspective correction |
| 2 | alpha **D** selector read `L` in the first cycle (RT64 reads `H` in both) | alpha D mirrored alpha C |
| 3 | the DL *walker* never applied `G_MOVEWORD G_MW_SEGMENT` (only the executor did) and had its own copy of `resolve_address` | walker and executor could disagree about where the walk is |

Renderer state now matches RT64 (`ColorCombiner::run`/`runCycle`,
`RSP::setOtherModeH/L`, `RSP::fromSegmented`), the combiner is evaluated field
for field (position-dependent selectors, second-cycle texel swap, second-cycle
wrap), and a draw binds **two** texture units (colour from TEXEL1, alpha mask
from TEXEL0) instead of one.

Verified title-screen metrics (`boot.cjs`, 640x480 canvas, 1 px inset):

| | session-20 baseline | now |
|---|---|---|
| `nonBlack` | 6 132 | **7 988** |
| `colorful` | 4 988 | **5 852** |
| othermode (per sprite draw) | `omh=0x000000` (bug) | `omh=0x182CF0` = 2-cycle + bilerp + persp |
| combiner, as logged | `cyc=1`, cycle-0 mux only | `cyc=2`, `mux0=[rgb=(ZERO-ZERO)*ZERO+TEXEL1 a=...+TEXEL0]`, `mux1=[...COMBINED]` |
| textures per draw | 1 (last loaded) | 2 (I8 mask 16x34 + RGBA16 colour 20x34) |
| runaway walk | `GFX-RUNAWAY … @0x0FE835F0` from ~task 31 | none seen since the fix (see "Verification") |

## The segmentation bug (the session-20 "runaway walk")

Evidence, from `debug/probes/stats.cjs` (single boot, 200 s) with the new
`GFX-ESCAPE` diagnostic:

```
[GFX-ESCAPE] task 31: walk escaped to @0xE000000 op=0x00 w0=0x00000000 w1=0x00000000 (cmd 577); previous commands:
[GFX-ESCAPE]   prev[21] @0x01C8870 op=0xDB w0=0xDB060038 w1=0x001CAB20      <- G_MOVEWORD: segment 14 = 0x001CAB20
[GFX-ESCAPE]   prev[23] @0x01C8880 op=0xDE w0=0xDE000000 w1=0x0E000000      <- G_DL segment 14 + 0
[GFX-RUNAWAY] task 31: DL entry @0x01C80A0 walked 4000000 commands with no G_ENDDL (data_ptr=0x801C80A0)
```

`0x0E000000` decodes as segment 14, offset 0. The `G_MOVEWORD` two commands
earlier is `0xDB060038` = `G_MW_SEGMENT`, segment `(0x38 >> 2) & 0xF = 14`,
base `0x001CAB20` — so the target is the game's *asset-area display list* at
`0x001CAB20`, and the walk escaped only because the base was being shifted into
the top byte (`0x00 << 24 | 0` = 0) instead of added.

The same mechanism produced the bogus texture address in the counters
(`load: … addr=0x0F000000`, `w=256 h=129 fmt=0 siz=3`): `0x0F000000` is segment
15 + 0, and the preceding `G_MOVEWORD` loaded segment 15 with `0x001BE4E0`. The
renderer was decoding texels from ~250 MB into rdram.

`gbi.hpp` now holds the one resolver both views of the walk use, plus
`set_segment_from_moveword()`; the walker applies `G_MW_SEGMENT` too.

Session-19's note that "the RSP keeps only the base's high byte" is **wrong**:
that conclusion came from changing the base to a full address while keeping the
`(base << 24) | offset` resolution. The two must change together (as they did
here) — the full base only makes sense when it is *added*.

## The combiner, now evaluated the way RT64 does

The session-19 decode was position-independent, which is wrong at the top of
every mux field:

- color A: `6 = ONE`, `7 = NOISE`, `0-5` common, else ZERO
- color B: `6 = KEY_CENTER`, `7 = K4`
- color C: `6 = KEY_SCALE`, `7 = COMBINED_ALPHA`, `8..14` the alpha-ish sources,
  `15 = K5`, and **`16..31` are ZERO** (the field is 5 bits but only 0-15 are
  defined — the first version of the fix returned K5 for 31)
- color D: `6 = ONE`
- alpha A/B/D: `6 = ONE`; alpha C: `0 = LOD_FRACTION`, `6 = PRIM_LOD_FRAC`
- **alpha D reads `H` in both cycles** (the old code read `L` for cycle 0)

`runCycle` semantics now implemented in the fragment shader:

- 1-cycle evaluates the **second** mux (`run()` → `runCycle(…, twoCycle ? 0 : 1,
  twoCycle, …)`), 2-cycle evaluates mux 0 then mux 1, `G_CYC_COPY` bypasses the
  combiner (texel0 passthrough);
- in the second cycle of a 2-cycle combiner the **TEXEL0/TEXEL1 values swap**;
- the second cycle's `COMBINED` inputs are **wrapped**
  (`wrapInputC`/`wrapInputABD`, then `wrapClamp`), not just clamped.

The game's own words are now checked against the ROM. The title sprite combiner
is a single word pair at ROM file offset `0x1EE828`:

```
L=0xFCFFFFFF H=0xFFFD7238
  cycle0 = rgb D=TEXEL1, alpha D=TEXEL0      (colour from the second tile, mask from the first)
  cycle1 = pass-through COMBINED
```

and the live log now prints exactly that (`cmb=0xFCFFFFFFFFFD7238`, `cyc=2`),
which is also what confirms the sprites are a *two-cycle* combiner — the reason
the "1-cycle uses the second mux" question stopped being academic.

## Texture binding: two units, and why tile indices don't select the image

`[GFX-TEX]` gained the tile index this session, and it exposed a surprise: every
`G_LOADTILE`/`G_LOADBLOCK` in the title scene targets **tile 7**, while
`G_TEXTURE` (`D7000002 80008000`) names **tile 0**. That is not a decode
difference — RT64 decodes both identically (`loadTile: p1(24,3)`,
`texture: p0(8,3)`) — the tiles all share `tmem = 0` (the round-1 `SETTILE`s in
the state block at `0x80186E90` are identical apart from the index), so the tile
index does not select the image. The RDP samples TMEM, and the pair of loads per
object (I8 alpha mask, then the RGBA16 colour image) overwrites the same region.

`RenderState` therefore keeps `recent_loads[2]` (`[1]` = most recent load,
`[0]` = the one before it), each with its own UV transform (loaded size +
`uls/ult` origin); the vertex shader emits `v_uv0`/`v_uv1`, the fragment shader
samples `u_tex0`/`u_tex1` per TEXEL selector (with the cycle-1 swap), and an
unloaded unit binds a 1x1 white fallback rather than texture 0.

This is a *model*, not TMEM emulation: the principled replacement is a TMEM
model (per-tile `tmem` address + `line`, the tile's `uls/lrt` rect as the UV
basis, `timg_width` as the row stride). See the open questions.

## Instrumentation added (all capped)

- `[GFX-TASK]` — every task in the suspicious window (1-8, 30-48, then every 50):
  `type`, `ucode`, raw `data_ptr`, both masks, `data_size`, commands walked,
  draws, plus the first four DL words at the walked address. This is what proved
  the DL *entry* was fine (the session-20 note called it a "corrupt data_ptr").
- `[GFX-RUNAWAY]` — a walk that never found `G_ENDDL`: entry offset, the first 8
  commands and the last 24 before the budget ran out.
- `[GFX-ESCAPE]` — the first command outside the low 32 MiB of rdram, with the
  24 commands before it. This is the one that found the bogus `G_DL` target.
- `[GFX-TXSTATE]` — the first 8 `G_TEXTURE`s: tile/on/level/sc/tc and the tile
  each texel unit maps to.
- `[GFX-CMD]` now prints `omh`/`oml`, the raw combiner words (`cmb=`), and both
  muxes as N64 expressions; `[GFX-TEX]` prints the load tile.
- `debug/probes/progress.cjs` (new) — boots repeatedly and reports the highest
  `tasks=` reached plus any `GFX-ESCAPE`/`GFX-RUNAWAY` line, because `boot.cjs`
  stops at the first rendered frame and `stats.cjs` only ever tries one boot.

## Verification

- `boot.cjs --attempts 8`: `RENDERED` with `nonBlack=7988`, `colorful=5852`
  (baseline 6132/4988); canvas shows the ring of twelve sprite soldiers.
- `progress.cjs --attempts 6 --secs 75` (one boot per attempt, tapping Start):
  **no `GFX-ESCAPE` or `GFX-RUNAWAY` in any of the six attempts**, best boot
  reached `tasks=39` (the others 1, 9, 19, 19, 35 - that spread is the pacing
  problem, not a wall). Before the fix, every boot that got as far as task 31
  escaped at command 577 into a 4 M-command runaway.
- The same run's counters show the segmented texture address resolving
  correctly, and the frame count growing:

  ```
  exec: task=35 cmd=1530 @0x1C9548 op=0xDF draws=896 flush_ok=9000 ...  <- walked to G_ENDDL
  load: seq=856 done=856 w=256 h=129 fmt=0 siz=3 timgw=1 addr=0x001BE4E0
        <- segment 15 + 0 = 0x001BE4E0 (the game's G_MOVEWORD base); was 0x0F000000
  ```

## Open questions (ordered)

1. **Pacing / the idle trajectory.** Frames arrive ~50-250 ms apart once
   running (`(RSP) display list submitted` deltas), but roughly half of all
   boots stall at 1-8 display lists and never advance; and a boot that renders
   can still stop at ~13. Session 20 measured 29.4 VI retraces/s (target 60) and
   ~3.5 gfx frames/s. `app/web/web.js`'s `watchBootStall()` message was still
   blaming session 14's "VI-retrace message queue deadlock"; it now says what
   actually happens (the idle trajectory) and points at this handoff.
2. **`G_TEXTURE` scale + UV basis.** `sc = tc = 0x8000`, so RT64's conversion
   `(s * sc) / (65536 * 32)` = `s/64` halves this renderer's `s/32`; the vertex
   UVs observed for the title sprites (`u` up to 66 texels) are > 1.0 under
   either rule for the loaded rect, so the UV *basis* — tile `uls/lrt` and
   `timg_width` as stride, i.e. a real TMEM/tile model — needs to be right
   before the scale can be judged. The horizontal smear on the left of the
   sprite ring is the current clamp behaviour.
3. **Blend/render-mode decode.** `record_state()` still approximates `cmd.blend`
   from `oml & 0xFFF` (a 2-entry heuristic); with `OTHERMODE_L` now decoded
   correctly, implement RT64's `Blender` (render-mode P/A fields per cycle).
   The sprite combiner's alpha mask (TEXEL0) is currently *unused* because
   blending is off, which is why the two-unit texture change did not alter the
   visible pixels yet.
4. **Combiner inputs** still missing: `NOISE`, `K4`, `K5`, `LOD_FRACTION`,
   `PRIM_LOD_FRAC`, `KEY_CENTER/SCALE` are all returned as 0, and `TEXEL1` in a
   1-cycle draw follows RT64's "shifted" hardware bug only to the extent of the
   selector swap. Check the other 20 combiners OB64 uses (the ROM has ~40
   distinct `0xFC` words) before trusting any of them.
5. **Framebuffer/VI indirection** is still stubbed (direct-to-canvas), so
   framebuffer-as-texture effects and any VI filter/AA are untested.
6. Untouched from earlier sessions: native-vs-browser divergence, the milestone
   buffer's 32 KB truncation, `G_LOADBLOCK` dxt approximation.

## Repro

```sh
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8
python3 debug/server.py &                       # :8931, COOP/COEP for pthreads
node debug/probes/boot.cjs --attempts 8 --secs 60 --out base
node debug/probes/progress.cjs --attempts 6 --secs 75 --out prog
node debug/probes/stats.cjs --secs 200 --out counters   # one boot, full counters
```

JS-only changes (`app/web/`) need no rebuild; a renderer change needs
`cmake --build build-wasm` and a page reload. Watch out: `stats.cjs` boots
**once** (it is a counter dump, not an attempt loop), so a low `tasks=` there can
just be the idle trajectory.

## Files changed (tracked)

- `app/src/gbi.hpp` — shared `resolve_address` (RT64 `fromSegmented`),
  `kRdramSize`, `set_segment_from_moveword` (full base).
- `app/src/gbi.cpp` — the walker now tracks `G_MW_SEGMENT` and uses the shared
  resolver.
- `app/src/web_renderer.cpp` — `SETOTHERMODE_H/L` decode, combiner selectors +
  shader (`runCycle`/wrap/copy/second-mux), per-tile/rolling texture state with
  two sampler units, `[GFX-TASK]`/`[GFX-RUNAWAY]`/`[GFX-ESCAPE]`/`[GFX-TXSTATE]`
  diagnostics, `[GFX-CMD]` raw state.
- `debug/probes/progress.cjs` — new intro-progress probe.
- `docs/DECISIONS.md` — session-21 entries.
- `docs/HANDOFF-2026-09-11-session21.md` — this file.
- `docs/guides/web-probes.md`, `debug/README.md` — document the new probe.
