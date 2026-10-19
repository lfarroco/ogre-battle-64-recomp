# Session 172 handoff — the browser's RDP depth pipeline: state, target, transform, synthetic control, and why the depth test ships off

## Goal

Implement the RDP depth pipeline in `app/src/web_renderer.cpp` for mission scene
`0x03`: `G_SETZIMG` (2 360 commands), `G_SETPRIMDEPTH` (11 740), and the
516 463 of 989 100 draws whose `OTHERMODE_L` is `0x00552078` (`Z_CMP|Z_UPD`,
`ZMODE_OPA`, `ZS_PIXEL`), with RT64 as the reference. Prove the pipeline works
with a synthetic control that does not need the game, then measure the mission
frame.

Timebox: 100 minutes (the round ran over; the measurements below are why).

## Result

1. **The depth state is decoded and carried on every draw.** `RenderState` gains
   `zimg_address`/`zimg_width`/`zimg_siz`/`zimg_fmt` from `G_SETZIMG` (address
   `w1 & 0xFFFFFF`, fmt bits 21-23, siz bits 19-20, width bits 0-11 + 1, the same
   word shape as `G_SETCIMG`) and `prim_depth`/`prim_depth_dz` from
   `G_SETPRIMDEPTH` (`z = (w1>>16 & 0x7FFF)/32767`, `dz = (w1 & 0xFFFF)/65535`,
   RT64 `RDP::setPrimDepth`, `hle/rt64_rdp.cpp:1160`). `OP_SETZIMG` was a bare
   `break` and `OP_SETPRIMDEPTH` was in the ignore list; both now decode. The RDP
   keeps the depth image across display lists, so `send_dl` seeds a new task from
   the last `G_SETZIMG` the way it already did for the colour image.
2. **`DrawCmd` carries the pipeline flags RT64's pipeline uses**, decoded in
   `record_state` from `OTHERMODE_L`:
   `depth_test = zCmp && !copy && zMode != ZMODE_DEC`,
   `depth_write = zUpd && !copy` (`render/rt64_framebuffer_renderer.cpp:1729-1730`),
   plus `z_source_prim` (`G_ZS_PRIM`, bit 2), `prim_depth`, `z_mode` (for the
   decal note) and `z[6]` (NDC depth per vertex).
3. **The vertex depth is RT64's screen z.** `transform_vertex` now writes 10
   floats: `screen_z = pos.z/w * view_scale[2] + view_trans[2]`
   (`posScreen = tfPos.xyz / float3(w,-w,w) * scale + translate`,
   `hle/rt64_rsp.cpp:714`), and hands the shader `2*screen_z - 1` as GL's NDC z,
   so `gl_FragCoord.z` comes back as the RDP's 0..1 screen depth. `G_ZS_PRIM`
   draws replace it with `2*prim_depth - 1` in `queue_triangles` (RT64
   `RasterVS.hlsl`: `ndcPos.z = primDepth.x * ndcPos.w`). Rectangles use NDC
   `z = 0`, which is RT64's `rectPosFloats` value (`hle/rt64_rdp.cpp:1559`).
   The vertex buffer stride is 10 and the shader has `a_z` (location 3) and `a_w`
   (location 4).
4. **The viewport z axis is `/1024`, and the earlier index was wrong in both
   directions.** RT64 uses `viewport.scale.z = vpscale/1024` and
   `translate.z = vptrans/1024` (`DepthRange = 1024.0f`, `hle/rt64_rsp.cpp:23`,
   `:908-911`), not the `/4` of the x/y axes. The browser's `[GFX-VP]` line for
   the mission's own `Vp_t` (`0x00186330`) reads
   `vscale=[640 480 511 0] vtrans=[640 480 511 0]`, i.e. x, y, z, pad in array
   order, so the 511 is at **index 2**. The task brief said index 3 (from RT64's
   byte-reversed `vp->vscale[3]`); that is RT64's struct index, and using it here
   picked the pad word 0. Measured: with `vscale[3]` the depth scale is 0, the
   census prints `vs=0.0000/0.0000`, and every draw lands at window z 0. The
   working form is `view_scale[2] = vscale[2]/1024.0f` (0.4990) and
   `view_trans[2] = vtrans[2]/1024.0f`.
5. **Each colour-image target owns a depth attachment.** `GlFramebuffer` gains
   `depth_rb` (a `GL_DEPTH_COMPONENT24` renderbuffer, RT64's target is
   `D32_FLOAT`), created and resized with the colour texture, attached to
   `GL_DEPTH_ATTACHMENT`, and cleared to 1.0 when it is created
   (`glClearDepthf(1.0)` + `glClear(GL_DEPTH_BUFFER_BIT)`). It is deleted with
   the target and on the incomplete-framebuffer path. The colour path is
   unchanged; the canvas (default framebuffer) keeps `attrs.depth = false` and no
   depth state, because every game scene sets `G_SETCIMG` and the canvas path is
   the prototype fallback. The target also stores the last `G_SETZIMG`
   configuration.
6. **The flush applies the state per draw**: `GL_DEPTH_TEST` on when the target
   has a depth attachment and the draw tests or writes depth, `glDepthFunc(LESS)`
   when it tests and `ALWAYS` when it only writes (RT64
   `depthFunction = zCmp ? LESS : ALWAYS`, `render/rt64_raster_shader.cpp:317`),
   `glDepthMask(depth_write)`; disabled otherwise and on the canvas. The present
   blit disables the test explicitly.
7. **The fragment shader runs RT64's F3D depth clip**
   (`RasterPS.hlsl:63-104`): with `u_zclip` set (a triangle whose depth source is
   `G_ZS_PIXEL`) a fragment is discarded when `gl_FragCoord.z < 0` or
   `> 1022.0/1024.0`. Rectangles and `G_ZS_PRIM` draws skip it, as RT64's
   `MaxDepth` does.
8. **The synthetic control proves the GL pipeline without the game.**
   `test_draw` debug flag bit 7 (128) appends: a `G_SETCIMG`/`G_SETZIMG` pair on
   scratch targets, `OTHERMODE_L = Z_CMP|Z_UPD|AA_EN|IM_RD` (`0x78`), a
   `(ZERO-ZERO)*COMBINED+PRIM` combiner, a `G_MTX` modelview with 1/32768 on the
   diagonal (fraction 2), six vertices in two sets, and two identical triangles -
   the **near** one (red, z `-16384`) drawn **first** and the **far** one (blue,
   z `+16384`) second. The last triangle is marked `depth_probe`, and the flush
   reads its target's centre pixel back and logs `[GFX-DEPTH-TEST]`.
   - `testdraw.cjs --flags 128 --expect-depth near` -> `rgba=(255,0,0,0)`, i.e.
     the far triangle was rejected. exit 0.
   - `testdraw.cjs --flags 384 --expect-depth far` (bit 8 clears `Z_CMP`, keeps
     `Z_UPD`) -> `rgba=(0,0,255,0)`, i.e. the far triangle wins. exit 0.
   The pair is the assertion: without the comparison the second draw wins, with
   it the first does, and the probe reads the pixel from the renderer's own
   target rather than from the canvas.
9. **A permanent `[GFX-DEPTH]` census** prints on the first recorded draw and
   every 400: the zCmp/zUpd/prim/DEC counts, the distinct `G_SETZIMG` count and
   the draw's configuration, and this draw's `kind`/`test`/`write`/`primz`/`zmode`
   plus the window-z range its vertices span and the viewport z scale/translate.
   A one-off `[GFX-DEPTH] target ...` line states the first target's depth
   attachment, and `[GFX-ZIMG]` logs each distinct `G_SETZIMG` configuration once.
   A `ZMODE_DEC` draw logs a one-shot note (see Result 11).
10. **The mission's depth state, measured on the route.** `[GFX-DEPTH]` on the
    browser route reads `draws=44800 zcmp=20307 zupd=20307 prim=0 dec=0
    zimg_seen=2` (45% of draws carry both flags), `zimg=[addr=0x3350C0 fmt=0
    siz=0 width=1]`, and the two configurations seen are `0x228080` (scene
    `0x12`'s image) and `0x3350C0` (the mission), both `fmt=0 siz=0 width=1`. The
    game's `G_SETZIMG` words therefore carry a meaningless format (width 1, siz
    4-bit); RT64's `RDP::setDepthImage` takes only the address, so the browser
    does the same and sizes the depth buffer from the colour target. A triangle's
    vertex window z spans e.g. `0.83787..0.86738` and a rectangle's is
    `0.50000..0.50000`; `vs=0.4990/0.4990` confirms the viewport z transform.
11. **`ZMODE_DEC` is not implemented and is reported.** The one-shot note prints
    when a draw asks for it; the mission has `dec=0`, and `ZMODE_INTER`
    (`0x0F0A4000`, 101 408 draws in the census) carries neither `Z_CMP` nor
    `Z_UPD`, so no recorded scene exercises the decal path. RT64's decal test is
    a fragment-shader depth tolerance (`CoplanarDepthTolerance`, RasterPS
    `:90-104`), not a depth function; approximating it with `GL_LEQUAL` would be
    a silent behaviour change, so a `ZMODE_DEC` draw keeps only its depth write.
12. **The depth test regresses the mission frame, so it ships off.** The whole
    pipeline is exercised by `?OGRE_DEPTH=1` (a new switch read once in the
    renderer constructor; `record_state` gates `depth_test`/`depth_write` on it
    and the synthetic control forces it on for its own draws). Measured against
    native `/tmp/w168/nat2/t.800` on the same route, same capture recipe:

    | build | meanAbs R/G/B | meanSigned | exact | top cliff rows 0..38 |
    |---|---|---|---|---|
    | session 171 (no depth) | 5.28 / 5.05 / 3.73 | +0.91 / +1.16 / +0.84 | 0.4372 | 0.93 |
    | **session 172, depth off (final)** | **4.77 / 4.54 / 3.19** | +0.38 / +0.64 / +0.22 | **0.4387** | **0.93** |
    | session 172, `OGRE_DEPTH=1` | 7.25 / 6.63 / 6.98 | +1.35 / +2.23 / -3.55 | 0.3614 | 9.52 |
    | `OGRE_DEPTH=1` with `a_w` in `gl_Position` | 7.74 / 7.04 / 7.06 | +1.18 / +2.55 / -3.27 | 0.3554 | 10.27 |
    | `OGRE_DEPTH=1`, inverted z | 7.27 / 6.67 / 7.06 | +1.35 / +2.20 / -3.64 | 0.3611 | 9.73 |
    | `OGRE_DEPTH=1`, z scale 0 (the index-3 bug) | 8.09 / 7.41 / 7.39 | +1.69 / +2.97 / -3.08 | 0.3470 | 10.31 |

    The terrain regions with depth off: left grass `5.24`, right terrain `2.28`,
    bottom middle `3.99`, whole frame `4.17` (session 171: 7.87 / 2.26 / 5.24 /
    4.69). The two mission frames are different intro phases (the task's caveat:
    the "Castle Burgund" portrait box and the camera differ), so the static
    terrain regions are the measurement.
13. **What moves with the test on, and why it is not a shader bug.** The top of
    the frame (rows 0..38, where the far terrain is) becomes a flat dark green
    `(25,28,0)` instead of the native's varied dark terrain (`(22,25,0)`,
    `(23,19,16)`, `(9,7,7)`): one surface wins every pixel there, which is what
    the frame looks like when all depths are equal. The same picture appears with
    the z scale 0 (all window z 0) and with z inverted, so the ordering - not the
    sign, the clip, or the interpolation - is what fails. The census says the
    values are not constant (a triangle spans 0.03 of the buffer), so the next
    step is a per-draw comparison of the browser's window z against RT64's
    `posScreen.z` for the same display list (`[GFX-DEPTH]`'s `wz` field prints
    it), starting with a draw whose surfaces the native frame shows in a known
    order. The depth test is therefore off by default and is **not** claimed to
    work on the game's data; the synthetic control is the correctness proof that
    is landed.
14. **`a_w` is uploaded but not in `gl_Position`.** RT64 multiplies the clip
    position by w (`RasterVS.hlsl`: `ndcPos.xyz *= ndcPos.w`), which makes the
    varying interpolation perspective-correct. Writing w here measured worse on
    every region (row 3 above), so the clip position keeps `w = 1` and
    interpolates in screen space, which is what the stored native frame agrees
    with. The attribute stays in the buffer because it is RDP state and the
    sampler census reads it.

## Evidence

- RT64 depth defines and state: `tools/RT64/src/shared/rt64_f3d_defines.h:12-14`
  (`G_ZS_PRIM = 0x4`), `:85-97` (`Z_CMP 0x10`, `Z_UPD 0x20`, `ZMODE_OPA 0`,
  `ZMODE_INTER 0x400`, `ZMODE_XLU 0x800`, `ZMODE_DEC 0xC00`, mask `0xC00`),
  `src/shared/rt64_other_mode.h:86-99` (`zCmp`/`zUpd`/`zMode`/`zSource`).
- RT64 vertex depth: `src/hle/rt64_rsp.cpp:23` (`DepthRange 1024.0f`), `:714`
  (`posScreen = tfPos.xyz / float3(w,-w,w) * viewport.scale + translate`),
  `:908-911` (`scale.z = vscale[3]/DepthRange`, `translate.z =
  vtrans[3]/DepthRange`), `src/shaders/RasterVS.hlsl` (`ndcPos.xyz *= ndcPos.w`;
  `ndcPos.z = primDepth.x * ndcPos.w` for `zSourcePrim`).
- RT64 depth clip and decal: `src/shaders/RasterPS.hlsl:63-104` (`MaxDepth =
  1022.0f/1024.0f` for a non-rect non-prim draw, the `z < 0` test, the
  `ZMODE_DEC` tolerance using `primDepth.y`/derivatives).
- RT64 depth image and prim depth: `src/hle/rt64_rdp.cpp:403`
  (`setDepthImage`, address only), `:1160` (`setPrimDepth`,
  `(z & 0x7FFF)/32767` and `(dz & 0xFFFF)/65535`).
- RT64 pipeline: `src/render/rt64_framebuffer_renderer.cpp:1729-1730`,
  `src/render/rt64_raster_shader.cpp:317` (`LESS` when `zCmp`),
  `:310-315` (`D32_FLOAT` depth target).
- RT64 rect depth: `src/hle/rt64_rdp.cpp:1559` (`rectPosFloats`, `z = 0.0`,
  `w = 1.0`).
- Mission census (`/tmp/census167/escort-dec-s4.json`, scene `0x0003`):
  `draws 989100`, `rm_zcmp 516463`, `rm_zupd 516463`, `zimg 2360`,
  `primdepth 11740`, `othermode_l_vals {"00552078": 516463, "00552048": 139240,
  "0C192048": 160462, "0F0A4000": 101408, "00504240": 37757, "00504241": 33770}`.
  `0x00552078` = `AA_EN|Z_CMP|Z_UPD|IM_RD` with `zMode 0` (`OPA`) and
  `zSource 0` (`PIXEL`).
- Viewport: `/tmp/w172/m6-a1-page.txt` and `/tmp/w172/m7-a2-page.txt`
  (`[GFX-VP] viewport addr=0x00186330 vscale=[640 480 511 0] vtrans=[640 480 511 0]`).
- Mission census lines: `/tmp/w172/m7-a2-page.txt` (depth off,
  `zcmp=0 zupd=0`, `wz` ranges, `vs=0.4990/0.4990`) and `/tmp/w172/m3-a1-page.txt`
  (depth on, `zcmp=18247 zupd=18247` of `draws=41200`).
- Frame comparisons: `/tmp/w172/proof172.py` (session 171's comparator with the
  same native `t.800` and region definitions) over
  `/tmp/w172/m7-a2-00.png` (depth off), `/tmp/w172/m3-a1-01.png` (depth on,
  `w = 1`), `/tmp/w172/m2-a1-01.png` (depth on, `a_w`), `/tmp/w172/m1-a3-01.png`
  (z scale 0), `/tmp/w172/m8-a1-00.png` (inverted z). Proof image:
  `docs/proofs/web-mission-depth.png` (depth off / depth on / native, 2x).
- Synthetic control: `/tmp/w172` runs of
  `node debug/probes/testdraw.cjs --flags 128 --expect-depth near` ->
  `[GFX-DEPTH-TEST] target=0xC4E000 320x240 pixel=(64,96) rgba=(255,0,0,0)
  zcmp=1 zupd=1 zmode=0x000`, and `--flags 384 --expect-depth far` ->
  `rgba=(0,0,255,0) zcmp=0 zupd=1`.
- Regression runs: `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex
  --expect-hits 1` -> exit 0, `fbtex hits=1`; `node debug/probes/boot.cjs
  --attempts 3 --secs 40` -> `RENDERED` on attempt 1 (`nonBlack=24808`);
  the world map still renders (`nonBlack=252820`, the same count session 171
  reported).

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `RenderState` depth image + prim depth; `OP_SETZIMG`/`OP_SETPRIMDEPTH` decode; `DrawCmd` depth flags, `z[6]`, `depth_probe`; `record_state` pipeline decode and the `OGRE_DEPTH` gate; `transform_vertex` 10-float output with NDC z; `set_viewport` z at `/1024` from index 2; `GlFramebuffer` `GL_DEPTH_COMPONENT24` renderbuffer (create/resize/clear/delete) and the zimg config; per-draw GL depth state and `u_zclip`; the F3D depth clip in the fragment shader; the vertex shader's `a_z` and 10-float stride; `[GFX-ZIMG]`, `[GFX-DEPTH]`, `[GFX-DEPTH-TEST]`; `test_draw` bits 7/8 (the synthetic control) and `note_depth_probe` |
| `debug/probes/testdraw.cjs` | `--expect-depth near|far` and the `[GFX-DEPTH-TEST]` parse, in `result.json` as `depthRgb`/`depthLines` |
| `docs/proofs/web-mission-depth.png` | browser depth off / browser depth on / native, 2x each |
| `docs/HANDOFF-2026-10-18-session172.md` | this file |
| `PLAN.md` | 8(s) continuation and open item 8(u) |
| `docs/STATUS-LOG.md` | session 172 entry |
| `docs/README.md` | session 172 row |
| `docs/DECISIONS.md` | decision 172 |

No commit was made. One temporary probe was added and reverted
(`probe172`: `sz = 1.0f - sz` in `transform_vertex`, to test whether the game's z
is inverted) with `make web` afterwards;
`grep -rn "probe172" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is empty.
`git status --short` shows `app/src/web_renderer.cpp`, `debug/probes/testdraw.cjs`
and the docs changed by this round; the other modified files in the checkout
(`app/src/web_platform.cpp`, `app/web/*`, `debug/probes/fbtex.cjs`,
`docs/guides/*`, `docs/scenes.md`) were already modified by earlier sessions.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` clean (six times; the final
  build is the one the regression runs used).
- `node debug/probes/testdraw.cjs --flags 128 --expect-depth near` -> exit 0,
  near (red) wins; `--flags 384 --expect-depth far` -> exit 0, far (blue) wins.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` ->
  exit 0, `fbtex hits=1 lines=1`.
- `node debug/probes/boot.cjs --attempts 3 --secs 40` -> `RENDERED`.
- Browser mission route (`/tmp/w171/drive171.cjs`,
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
  `--shots 2..3 --gap 3000 --pre-delay 22000 --attempts 8 --secs 200`): five
  runs, all reaching `[scene] id=0x0003` and capturing
  `nonBlack=303208`; the depth-on runs add `?OGRE_DEPTH=1`.
- `docs/proofs/web-mission-depth.png` is the depth-off frame `/tmp/w172/m7-a2-00.png`,
  the depth-on frame `/tmp/w172/m3-a1-01.png` and native `t.800` at 2x
  (1928x480).

There is no repo test harness; each check is a ROM-dependent game run.

### What was not verified

The tutorial `0x17`, title `0x04` and map `0x05` captures taken this round did
not reproduce session 167/171's frames: under the machine's load the runs landed
on early trajectories (the tutorial capture had 12 gfx tasks at the 90 s mark and
its box region measures 150 mean abs instead of 0.22; the title's best match
against `/tmp/w162/nat-fog/t.*.bin` is 71.6 instead of 0.11). The map did render
(`nonBlack=252820`, session 171's exact count). So the "no regression" evidence
this round is the mission frame with depth off (4.77/4.54/3.19, better than
session 171's 5.28/5.05/3.73, cliff unchanged at 0.93), `boot.cjs`, both testdraw
controls and testdraw 96 - not a re-measured tutorial/title box. The tutorial and
title frames must be re-captured on an idle machine before this round is called
regression-free on those two screens.

## Open

1. **The mission's depth ordering is not usable yet** (Result 13). The values are
   not constant - a triangle spans about 0.03 of the buffer and the viewport z
   transform measures 0.4990 - but with the test on, the far terrain at the top of
   the frame is replaced by one flat surface while the terrain elsewhere is
   unchanged, and neither the sign nor the clip explains it. `[GFX-DEPTH]`'s `wz`
   field is the instrument: compare a mission draw's window-z range against
   RT64's `posScreen.z` for the same display list, then decide whether the fault
   is this vertex transform or the frame's draw order. `OGRE_DEPTH=1` turns the
   test on for that work.
2. **`ZMODE_DEC` (the decal path) is not implemented.** No recorded scene reaches
   it (`dec=0` on the mission); RT64's test is `CoplanarDepthTolerance` in the
   fragment shader, `abs(pixelDepth - surfaceDepth) > max(tolerance, dz)`, which
   needs a depth-buffer read the browser has no path for.
3. **The depth image is not written back to or uploaded from RDRAM.** RT64 keeps
   a real depth image and its `FbWriteDepth` path can write it back; this
   renderer's depth exists only on the GPU. Nothing this port renders reads a
   depth image with the CPU, and the game's `G_SETZIMG` words are degenerate
   (width 1, siz 0), so the guest address is recorded for diagnostics only.
4. **The tutorial `0x17` and title `0x04` frames need a re-capture** on an idle
   machine (see "What was not verified").
5. **`a_w` is not in `gl_Position`** (Result 14). Perspective-correct varying
   interpolation is RT64's behaviour and measured worse here; if it is landed it
   needs its own A/B against the native frame, region by region.
