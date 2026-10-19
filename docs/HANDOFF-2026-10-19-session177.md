# Session 177 handoff — the mission stipple was the G_AC enum mapping, and the alpha compare now matches RT64

## Goal

Session 176's frame-locked mission pair exposed a per-texel stipple on the
"Castle Burgund" portrait frame and the `MISSION` band (207 bright glyph pixels
against native's 1083) and a missing party-sprite layer. Session 176 named an
alpha/coverage rejection as the candidate. This round fixes it.

Timebox: 100 minutes. The browser boot stalled for the second half of the round,
so the mission re-capture is pending and stated as such.

## Result

1. **The stipple is the `G_AC` enum mapping.** RT64's values are
   `G_AC_NONE 0`, `G_AC_THRESHOLD 1`, `G_AC_DITHER 3`
   (`tools/RT64/src/shared/rt64_f3d_defines.h:8-10`). The browser stored
   `cmd.acmp = OTHERMODE_L & 3` and the fragment shader tested `u_ac == 2` as
   the threshold arm and `u_ac == 1` as the dither arm. A real
   `G_AC_THRESHOLD` draw (`acmp=1`) therefore ran the **dither** arm against a
   per-pixel hash; with a combiner alpha around 0.2 it kept about a fifth of
   the texels at random, which is exactly the measured scatter (no lattice
   period, no row/column parity bias, glyph shapes otherwise correct). The
   census has 33 770 `acmp=1` (threshold) draws in mission scene `0x0003`, and
   no `acmp=2` draws at all.

2. **The fix is the value mapping, plus RT64's dither for the real dither arm.**
   `u_ac == 1` is now `G_AC_THRESHOLD` (`alpha_compare_value <
   u_blend_color.a` -> discard); `u_ac == 3` is `G_AC_DITHER`, ported from
   `RasterPS.hlsl:212-215` and `Random.hlsli`: `randomSeed =
   initRand(FrParams.frameCount, instanceIndex * pixel.y * pixel.x, 16)`, the
   combiner's NOISE input consumes one `nextRand`, and the compare reads the
   next one. The seeds are `present_requests_` and `flushed_cmds_`, uploaded as
   `u_dither_frame`/`u_dither_instance`. `u_ac == 2` no longer has an arm (2 is
   not a `G_AC` value).

3. **The synthetic control proves the mapping without the game.**
   `test_draw` debug flag bit 9 (512) draws three identical textured rects with
   a combiner alpha of 0.2, differing only in the compare and the blend alpha:
   `G_AC_THRESHOLD` with blend alpha 0 (all fragments pass), `G_AC_THRESHOLD`
   with blend alpha 1 (all discarded), `G_AC_DITHER` (a partial count).
   Measured on the final build:
   ```
   [GFX-AC-TEST] rect=1 acmp=1 blend_a=0.000 om_l=0x000001 lit=1406/1406 peak=255
   [GFX-AC-TEST] rect=2 acmp=1 blend_a=1.000 om_l=0x000001 lit=0/1406 peak=0
   [GFX-AC-TEST] rect=3 acmp=3 blend_a=0.000 om_l=0x000003 lit=277/1369 peak=255
   alpha control: threshold/full=full threshold/none=none dither/partial=partial
   ```
   `node debug/probes/testdraw.cjs --flags 512 --expect-alpha` exits 0. The
   dither's 277/1369 is the 0.2 combiner alpha through RT64's sequence; the
   threshold arms are exact. The probe's readback was also corrected for the
   renderer's `RGBA16F` attachment: WebGL2 rejects `GL_UNSIGNED_BYTE` for it
   (`INVALID_OPERATION`, an all-zero block), so the control now reads floats as
   `note_depth_probe` does.

4. **The existing controls still pass**: `--flags 96 --expect-fbtex
   --expect-hits 1` (`fbtex hits=1 lines=1`), `--flags 128 --expect-depth near`
   (`rgba=(255,0,0,0)`), `--flags 384 --expect-depth far` (`rgba=(0,0,255,0)`).
   `make web` is clean.

5. **The mission re-capture is pending, and that is the open item.** The
   frame-locked pair predicted that the mapping fix restores the portrait
   frame, the `MISSION` band and the party sprites in one change (all are
   alpha-tested 2D layers; the threshold draws were the ones being dithered).
   This round could not capture a fixed-build mission frame: the browser boot
   stopped at scene `0x0000` with `tasks=0` on every attempt from about 01:40
   (four mission-route attempts, four forced-`0x03` attempts, three
   `boot.cjs` attempts; the same build renders the synthetic control and the
   same machine's `boot.cjs` passed at 23:16). The boot lottery is documented
   and environmental, but the mission verification did not happen.
   The re-measurement is the same frame-locked comparison
   (`/tmp/w176/nat/t.289.bin`, the party cluster x18..78 y60..98, the portrait
   rows 85..195 x 105..215, the `MISSION` band rows 190..225) once a boot
   advances.

6. **The tutorial/title/map captures are not re-run** for the same boot reason.
   The change only touches draws whose `acmp` is 1 or 3; the tutorial's box
   draw is `OTHERMODE_L=0x000A4000` (alpha compare 0), the title/map draw state
   in the session-174 `[GFX-TP]` census has no `acmp=1`/`3` family, so those
   frames are expected byte-identical. That is an expectation, not a
   measurement this round.

## Evidence

- RT64 enum: `tools/RT64/src/shared/rt64_f3d_defines.h:8-10`; the browser's old
  mapping was `app/src/web_renderer.cpp`'s fragment shader (`u_ac == 2`
  threshold, `u_ac == 1` dither) and `DrawCmd.acmp`'s comment.
- RT64 compare/dither: `tools/RT64/src/shaders/RasterPS.hlsl:173-216`
  (`ColorCombiner::run(..., alphaCompareValue)`, `nextRand(randomSeed)`), the
  seed at `:65`, NOISE at `:189`; `tools/RT64/src/shaders/Random.hlsli:10-33`.
- The mission census: `/tmp/census167/escort-dec-s4.json` scene `"0x0003"`,
  `"aa": {"0": ..., "1": 33770}`.
- Session 176's pair and element numbers:
  `docs/HANDOFF-2026-10-18-session176.md` §1.3-1.5,
  `docs/proofs/web-mission-framelocked.png`, `/tmp/w176/nat/t.289.bin`,
  `/tmp/w176/web/m4-a1-014.png`.
- The synthetic output: the `[GFX-AC-TEST]` lines above, from
  `debug/probes/testdraw.cjs --flags 512 --expect-alpha`.

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `G_AC` mapping: `u_ac == 1` threshold, `u_ac == 3` dither; RT64 `initRand`/`nextRand` port with `u_dither_frame`/`u_dither_instance` and the NOISE pre-consumption; `DrawCmd.acmp` comment; the synthetic alpha control (flag 512) in `test_draw` and its `note_ac_probe` readback, now float-aware for the RGBA16F attachment |
| `debug/probes/testdraw.cjs` | `--flags 512 --expect-alpha` control: reads the three `[GFX-AC-TEST]` lines and requires full/none/partial |
| `docs/HANDOFF-2026-10-19-session177.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md` | session entry, the stipple cause/fix, the pending mission verification |

No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/renderer.cpp` was changed. No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean after the fix and
  after the readback correction.
- `node debug/probes/testdraw.cjs --flags 512 --expect-alpha` — exit 0,
  `threshold/full=full threshold/none=none dither/partial=partial`, the three
  `[GFX-AC-TEST]` counts above.
- `--flags 96 --expect-fbtex --expect-hits 1` — exit 0, `fbtex hits=1 lines=1`.
- `--flags 128 --expect-depth near` — `rgba=(255,0,0,0)`; `--flags 384
  --expect-depth far` — `rgba=(0,0,255,0)`.
- The mission re-capture attempts (all idle): four with
  `/tmp/w176/drive176c.cjs` on
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
  four with `?OGRE_SCENE=0x03&OGRE_SAVE=...`, and `boot.cjs --attempts 3
  --secs 40` (NO RENDER). Page logs stop at scene `0x0000` with `tasks=0`.
- `grep -rn "probe177\|OGRE_A177" app/ debug/ RecompiledFuncs/ Bank*Funcs/` —
  no output.

There is no repo test harness; each check is a ROM-dependent browser run.

## Open

1. **Re-measure the frame-locked mission pair with the fixed build** (the
   round's prediction: the portrait frame, `MISSION` band and party sprites
   become solid). Compare browser vs `/tmp/w176/nat/t.289.bin`; the party
   cluster, portrait and `MISSION` element numbers are the test. If the stipple
   is gone but the party sprites are still absent, that is a second mechanism
   (the sprites may be a different draw family) and must be named, not assumed.
2. **The tutorial/title/map re-captures** (expected byte-identical, not
   measured).
3. **The lit-terrain colour offset** (signed G +6.2 / B -14.3 on the top cliff)
   is separate and unnamed.
4. **NOISE input**: `color_input` sel 16 still returns 0, so the dither arm's
   random sequence matches RT64 only when the combiner does not read NOISE.
   No reached screen uses `G_AC_DITHER` (`acmp=3`), so this is unexercised.
5. The rest of session 176's residuals: `G_CD` dither (readback only), the
   publisher stills, the Load Game white frame, the S2DEX2 census, and the
   unexercised list.
