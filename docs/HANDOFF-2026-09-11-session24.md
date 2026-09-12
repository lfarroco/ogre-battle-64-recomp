# Handoff — 2026-09-11 — session 24: the title sprites render correctly (two causes behind the "green smudges")

## Outcome

The twelve title-screen soldiers now render as themselves: orange/tan bodies,
dark green helmets, white plumes, magenta sashes, in two mirrored clusters of
three. Before this session the same frame was a tangle of green/purple/blue
slivers, and the report that started it was "green smudges in the helmet area".

Session 23 closed with the sprite path "internally consistent and faithful to
RT64" but a rendered sprite that still did not match its own `colour x mask`
composite. There were **two** independent bugs, and the first one masked how
much of the symptom the second one caused:

1. **`decode_texture_rect` read texture bytes at their logical offset** out of
   the runtime's byte-reversed rdram. Session 23 fixed exactly this for `Vtx`
   (and `read_matrix` had always dodged it with `c ^ 1`); the texture decoder was
   the last place missing it.
2. **The blender folded cycle 0 of the sprite render mode**, which is
   mathematically an identity.

Fixing (1) made the decoded textures clean but left the *rendered* sprite dim and
speckled, which is what pointed at (2). `spritecheck.cjs` now shows the rendered
sprite and its composite as the same character, closing session 23's open item 0.

## Bug 1: texture and TLUT reads needed RT64's `^ 3` byte rule

The runtime stores every N64 32-bit word byte-reversed, so the logical byte at
address `a` lives at physical `a ^ 3`. RT64's TMEM load states the rule exactly
(`tools/RT64/src/hle/rt64_rdp.cpp`, `loadWord`):

```cpp
TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + i) ^ 3];
```

Our decoder used `rd16(p)` / `p[0]` at the logical offset:

- **16-bit texels** (TEXEL1, the RGBA16 colour): `rd16()` at logical offset `o`
  returns the halfword at `o ^ 2`, i.e. the *adjacent* texel - every horizontal
  pair came out swapped.
- **I4/CI4 bytes** (TEXEL0, the 32x34 mask): a byte read at logical offset `b`
  returns logical `b ^ 3`, so the four bytes of every word came back reversed -
  eight texels per group, in 2-texel units. The mask's alpha landed in the wrong
  places, uncovering colour texels the mask exists to hide.

The mask's alpha *histogram* is identical before and after (the bug is a
permutation, so the multiset of nibbles is unchanged). Comparing histograms would
have missed this entirely - the images, not the statistics, are the evidence.

Fix: `n64be16(rdram, addr)` joins `n64h`/`n64b` beside `rd16`;
`decode_texture_rect` reads I4/I8 via `n64b` and 16/32-bit texels via `n64be16`;
`OP_LOADTLUT` reads its entries the same way.

### Why the ROM could not be used as an independent decoder

The tile's source address (`0x001C3C88`, RGBA16, from `[GFX-TDSTATE]`) is a
*RAM* address and does not map linearly to the ROM: decoding `assets/ogre64.z64`
at that offset gives noise. The reference has to be RT64's own loader (or a dump
of the emulator's rdram), not the ROM file.

## Bug 2: the blender folded a cycle that cancels exactly

`[GFX-CMD]` gives the sprite's state:

```
draw 2: verts=3 textured=1 cyc=2 omh=0x182CF0 oml=0x00184240
        cmb=0xFCFFFFFFFFFD7238
        mux0=[rgb=(ZERO-ZERO)*ZERO+TEXEL1 a=(ZERO-ZERO)*ZERO+TEXEL0]
        mux1=[rgb=(ZERO-ZERO)*ZERO+COMBINED a=(ZERO-ZERO)*ZERO+COMBINED]
        prim=[1.00,1.00,1.00,0.00] env=[0.00,0.00,0.00,0.00]
        ablend=1 force=1 approx=0 ccyc=2
        b0=[P=CC M=CC A=CC_A B=ONE]   b1=[P=CC M=FB A=CC_A B=1MA]
```

The combiner is `rgb = TEXEL1, a = TEXEL0` - correct. The colour then goes
through the blender. `forceBlend` is set with a 2-cycle combiner, so
`blendCycleCount` is 2 (RT64 `Blender::blendCycleCount`), and cycle 0
(`P=CC M=CC A=CC_A B=ONE`) runs before cycle 1 does the framebuffer blend.
Cycle 1 feeds cycle 0's colour through as its `CC` input, so cycle 0 **must** be
an identity - and it is, mathematically:

```
(P*a + M*b) / (a + b)   with P == M   ==   P
```

But `blender_run_cycle` is a field-for-field port of RT64
`Blender::runCycle`, which wraps the numerator with `fmod(numerator, 1+8/255)` to
simulate the hardware's accumulator overflow - unconditionally. With
`P=M=CC, B=ONE` the numerator is `CC*(a+1)`, above `1.031` for ordinary colours,
so the fold fires on nearly every pixel. Per channel, so it shifts hue:

```
mask alpha=0.0: orange(255,166,33) -> (255,166, 33)
mask alpha=0.1:                    -> ( 16,166, 33)   green
mask alpha=0.5:                    -> ( 80,166, 33)   green
mask alpha=1.0:                    -> (124, 34, 33)   dark red
```

Red folds first, so mid-alpha pixels turn green and opaque pixels halve in
brightness. OB64's mask has most of its helmet/detail texels at intermediate
alpha, which is where the green appeared.

This is not a porting slip: RT64's `runCycle` computes a `numeratorOverflow` flag
(`!passthrough && (B != B_ONE_MINUS_A)`) and then never uses it, and it already
treats `(P == M) && (B == B_ONE_MINUS_A)` as a passthrough. Treating `P == M` as
an identity is consistent with its own intent.

Fix: apply `mod(..., Overflow)` only when `P != M`.

## Verified

| check | before | after |
|---|---|---|
| `spritecheck.cjs` rendered side | dim green/purple speckle, no resemblance to the composite | the same character as the composite |
| full settled canvas (`shots.cjs`) | green/purple/blue slivers | twelve readable soldiers |
| `shots.cjs` settled `nonBlack` | `239425` | `239425` - unchanged, so this is a colour fix, not a brightness one |
| `textures.cjs` mask silhouette | ragged, 8-texel block steps, detached fragment | smooth plume + figure |
| `testdraw.cjs` | `nonBlack=12312 colorful=10237` | unchanged (its synthetic mode takes the passthrough path, so the blender change does not touch it) |
| `logmode.cjs` | PASS | PASS |
| `[GFX-VP]` scale | `[160.00 120.00 127.75]` | unchanged |

New diagnostics this session (both one-shot, in `[GFX-UV]`):
`k0`/`k1` with found/id, `scl0`/`scl1`, `org1`, the first vertex's `v0`, the
resulting `uv0`/`uv1`, and `glGetError()`. That is what ruled the sampler out.

## Still open

0. **Raw TEXEL1 was reported black** (`--flags 9`, session 23) while its decoded
   image is bright. Not re-measured this session, and with the blender fixed the
   composite comparison no longer shows a discrepancy. Two things to check before
   believing it: (a) the isolation probe compares the **first** sprite against the
   **last** textured draw's texture pair (`ogre_gfx_debug_last_tex`) - with twelve
   sprites those need not be the same character, which is enough to explain the
   old mismatch on its own; (b) `lookup()` returns a 1x1 white texture for an
   unknown key, so a missing key would render *white*, not black. Re-measure with
   the pair of the draw actually isolated.
1. **The fold for `P != M` is still suspect.** The same reasoning applies there:
   a weighted average of two in-range inputs is in range, so `fmod` should not be
   needed at all (`P=CC, M=0.5, a=b=1` folds `1.5` to `0.234` instead of `0.75`).
   Nothing in the title scene exercises it, so it was left alone rather than
   changed blind. A cycle-accurate RDP (angrylion/ParaLLEl) is the way to settle
   what the hardware's accumulator actually does.
2. **The idle trajectory is still the gate**, unchanged from session 21: about
   half of all boots never build a real display list, and the game swaps buffers
   at ~0.25/s. This session changed nothing about pacing - the captures that look
   right all came from a boot that advanced.
3. **Untouched** from session 23: `NOISE`/`K4`/`K5`/`LOD_FRACTION`/
   `PRIM_LOD_FRAC`/`KEY_CENTER`/`KEY_SCALE` combiners are still 0, coverage and
   alpha-compare are ignored, framebuffer/VI indirection is still
   direct-to-canvas, TMEM is still not modelled, and `G_LOADBLOCK`'s `dxt` shape
   is still approximate.
4. **A reference frame** is still worth having, but the case for it is now
   weaker for the title scene: the sprites match their own textures. It remains
   the right check for the combiner/blender cases in (1).

## Repro

```sh
EM_CACHE=/Users/momo/.cache/emscripten-ogre cmake --build build-wasm -j 8
python3 debug/server.py &                          # :8931, COOP/COEP for pthreads
node debug/probes/shots.cjs --attempts 6 --secs 70 --shots 5 --gap 1500 --out s25
node debug/probes/spritecheck.cjs --attempts 3 --secs 60 --out s25-sprite
node debug/probes/textures.cjs --attempts 4 --secs 65 --out g4-tex
node debug/probes/logmode.cjs
node debug/probes/testdraw.cjs --attempts 4 --secs 55 --out s25-testdraw
```

The failure is intermittent (session 21: roughly half of boots never build a real
display list), so every probe needs `--attempts`. `shots.cjs` captures a series
after the game advances, which is how the settled frame is reached - the first
frames are mid-fade and dim.

## Files changed (tracked)

- `app/src/web_renderer.cpp`:
  - `n64be16()` helper; `decode_texture_rect` and `OP_LOADTLUT` read through
    `n64b`/`n64be16` (bug 1);
  - `blender_run_cycle`: `mod(..., Overflow)` only when `P != M` (bug 2);
  - `set_viewport`: reads through `n64h()` with the canonical `(x, y, z, 0)`
    indices, plus a correction to its comment (the hardware layout is
    `(x, y, z, 0)`, not `(y, x, pad, z)`; the old index juggling cancelled the
    byte-reversal swap by hand and was correct only by accident);
  - `[GFX-UV]` diagnostic (keys, ids, scales, origin, `uv0`/`uv1`, `glGetError`)
    and the `tex_uv_logged_` counter.
- `docs/DECISIONS.md` — session-24 entry.
- `PLAN.md` — session-24 status bullet.
- this file.
