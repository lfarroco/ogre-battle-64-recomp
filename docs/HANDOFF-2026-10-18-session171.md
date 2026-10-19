# Session 171 handoff — G_LIGHTING and backface culling in the browser renderer, and the mission frame goes from `meanAbs 23.72/15.30/8.11` to `5.28/5.05/3.73`

## Goal

Implement the two biggest renderer features mission scene `0x03` uses and the
browser ignored: `G_LIGHTING` (916 448 of 989 100 triangle draws) and backface
culling (926 539). Compare the mission frame against native
`/tmp/w168/nat2/t.800.bin`, keep the tutorial `0x17` box, the title frame and the
world map unchanged.

Timebox: 100 minutes.

## Result

1. **The browser parses the F3DEX2 light commands into `RenderState` and shades
   `G_LIGHTING` vertices the way RT64 does.** `RenderState` gains
   `lights[8][24]` (`kMaxLights = RSP_MAX_LIGHTS + 1`, RT64
   `hle/rt64_rsp.h:163`), `num_lights`, and `fog_mul`/`fog_offset`.
   - `G_MOVEMEM` (0xDC) index 10 (`MV_LIGHT`): `offset = p0(w0, 8, 8) * 8`,
     `slot = offset / 24`, `slot >= 2` is light `slot - 2` read from 24 bytes at
     `w1` — RT64 `GBI_F3DEX2::moveMem`
     (`tools/RT64/src/gbi/rt64_gbi_f3dex2.cpp:48-61`). Slots 0/1 are the lookat
     X/Y vectors and are skipped: this renderer has no `G_TEXTURE_GEN` path.
   - `G_MOVEWORD` (0xDB) type `p0(w0, 16, 8)`: `G_MW_NUMLIGHT` (0x02) sets
     `num_lights = w1 / 24` (`RSP::setLightCount`), `G_MW_LIGHTCOL` (0x0A) writes
     light `p0(w0, 0, 16)/24`'s two colour words (`RSP::setLightColor`,
     `hle/rt64_rsp.cpp:938`), `G_MW_FOG` (0x08) stores the fog vector
     (`setFog`), and `G_MW_SEGMENT` (0x06) keeps the segment write. The bytes go
     through `n64b`, not a `memcpy`, because the runtime's RDRAM stores each
     32-bit word byte-reversed (`n64b(p, off) = p[off ^ 3]`).
2. **`transform_vertex` replaces the vertex colour with RT64's lighting result**
   when `geometry_mode & G_LIGHTING` and `num_lights > 0`
   (`RSPProcessCS.hlsl:88-108`, `shared/rt64_rsp_light.h:56-64`):
   `norm = ((int8)r, (int8)g, (int8)b) / 127` (the four Vtx colour bytes are the
   normal; RT64 reads them as `Buffer<int> srcNorm`), `ambient =
   light[num_lights].col`, and for `i = 0 .. num_lights-1`
   `localDir = normalize(mul(float4(dir, 0), worldMat))`,
   `result += max(dot(norm, localDir), 0) * light[i].col`,
   `rgb = min(result, 1)`. `worldMat` is `st.modelview_stack.back()`, which is
   RT64's `worldTransforms[]` entry `mul(modelMatrixStack.back(),
   extended.viewProjMatrix)` (`hle/rt64_rsp.cpp:538`) with
   `extended.viewProjMatrix` the identity. `num_lights == 0` keeps the raw colour
   (RT64's `else` arm). Alpha stays `v.a / 255`.
3. **The point-light arm is unreachable and is not implemented.**
   `RSP::setVertexCommon`'s `usesPointLighting` is `curGBI->flags.pointLighting &&
   (geometryMode & G_POINT_LIGHTING)` (`hle/rt64_rsp.cpp:563`), and
   `pointLighting` is `false` for every GBI (`gbi/rt64_gbi.h:39`; nothing
   assigns it), so `light.kc > 0` never selects `computePosLight`. The comment at
   `compute_vertex_light` says so, and the census prints `point=0`.
4. **The light byte layout is measured from the game's own data, not assumed.**
   A mission `[GFX-LIGHT]` line reads
   ```
   L0 raw 4D 4D 7F 00 4D 4D 7F 00 FD 99 C5 00   amb raw 4C 4C 7E 00
   ```
   so the logical layout is `col[3] = r,g,b` at bytes 0-2 with a pad at 3,
   `colc[3]` at 4-6 with a pad at 7, and `dir[3] = x,y,z` at 8-10 with a pad at
   11: every pad byte is 0. The other candidate (pad1 at 0) would make the same
   bytes `pad1=0x4D, col=(0x4D,0x7F,0), pad2=0x4D, pad3=0xFD,
   dir=(-103,-59,0)`, i.e. three non-zero pads and a zero blue channel on both
   colour words. A second sample (a different display list) is
   `64 64 00 00 64 64 00 00 20 40 20 00` with ambient `05 05 05 00`, the same
   shape. The 16-byte spacing between the mission's light 0 and light 1
   (`0x801869C8`, `0x801869D8`) agrees with a 4+4+4+pad record. RT64's
   `DirLight` (`hle/rt64_rsp.h:101-112`) is the four-byte-reversed view of this:
   its `colr/colg/colb` at physical 3/2/1 and `dirx/diry/dirz` at physical 11/10/9
   are logical 0/1/2 and 8/9/10, which is what this code reads.
   **Measured cost of getting this wrong:** reading the colour at logical 3/2/1
   (the pad-first reading) pinned red at 0 and gave `meanAbs 21.90/15.79/5.16`
   with `meanSigned R=-19.60`; the measured layout gives `5.28/5.05/3.73` with
   `meanSigned +0.91/+1.16/+0.84`.
5. **Backface culling mirrors RT64's `RSP::drawIndexedTri`.**
   `(geometry_mode & G_CULL_BOTH) == G_CULL_BOTH` draws nothing
   (`hle/rt64_rsp.cpp:1059`), checked at the top of `draw_tri`; when
   `(geometry_mode & G_CULL_BOTH) == G_CULL_FRONT` the first and last vertex are
   swapped (`:1094`); then `U = p1 - p0`, `V = p2 - p0`, `N = cross(V, U)`, and
   the triangle is drawn only when `N.z >= 0` (`:1132`). RT64's `posScreen` has y
   down (`RSPProcessCS.hlsl` `ndcPos = tfPos.xyz / float3(w, -w, w)`), so the test
   uses `(x, -y)` of the NDC the browser already has; the viewport
   scale/translate are positive and affine and do not change the sign. F3DEX2's
   cull bits are `0x200`/`0x400` (`gbi/rt64_gbi_f3dex2.cpp:174-176`), not
   F3DEX's `0x1000`/`0x2000`. That the sign is right is visible: the terrain's
   silhouette and its lit faces match the native, and the top cliff region goes
   from `18.92` to `0.93` mean abs while the culled count stays at 12.8% of the
   triangles evaluated (4345 of 33 872), i.e. the pass drops back faces, not
   whole surfaces.
6. **A permanent `[GFX-LIGHT]` census** prints on the first lit draw and every
   400 lit draws (and on the same schedule for unlit draws, so a screen with no
   lighting states itself): the lit/unlit/culled counts, `num_lights`, `point`,
   light 0's colour, direction and 12 raw bytes, the ambient light's colour and
   raw bytes, the first vertex's normal, and the resulting shade. It costs a
   counter increment per triangle and a light scan only on a logged draw. On an
   `unlit` line the `norm=`/`shade=` fields print the raw Vtx colour bytes (read
   the same way), so they mean a normal only on a `lit` line.
7. **The mission frame now matches the native.** `tools/web_frame_cmp.py` style
   numbers against `/tmp/w168/nat2/t.800` (the session-168 native frame of
   scene `0x03`):
   | | meanAbs R/G/B | meanSigned R/G/B | exact |
   |---|---|---|---|
   | before (session 170's proof frame) | 23.72 / 15.30 / 8.11 | -6.38 / +12.90 / -3.99 | 0.3090 |
   | after (session 171) | **5.28 / 5.05 / 3.73** | +0.91 / +1.16 / +0.84 | **0.4372** |
   Terrain regions (whole frame `15.71 -> 4.69`): top cliff rows 0..38
   `18.92 -> 0.93`, right terrain rows 95..150 x 285..319 `17.94 -> 2.26`,
   bottom middle rows 200..238 x 100..230 `17.18 -> 5.24`, left grass rows
   190..238 x 0..90 `18.10 -> 7.87`. The two frames are **different intro
   phases**: the "Castle Burgund" portrait box sits at a different place and the
   camera is a little further out in the browser, and the animated `MISSION`
   band differs, so the whole-frame number measures the same screen at two
   moments. The terrain regions are static and are where the change is measured.
8. **No regression on the screens the task named.**
   - Tutorial `0x17`: the captured frame is **byte-identical** to session 167's
     post-fix frame (`md5 10b3536365b1bcfb6d2e2116aaf42a24` for both
     `/tmp/w171/tut-00.png` and `/tmp/w167/after/tutb-01.png`), and the box's
     whole-region mean against `/tmp/w166/nat17/t.335.bin` is `0.22` over rows
     166..219, exactly session 167's number. The screen has one unlit triangle
     draw and no lit ones, so it never enters the new arm.
   - Title `0x04`: the best match against the stored native title series
     `/tmp/w162/nat-fog/t.*.bin` is `0.11` mean abs (session 167's post-fix
     build: 0.18). The screen's draw order is one fill and texrects only, so it
     has no triangle draws at all.
   - World map `0x05`: renders (`nonBlack=252820` over two shots). Against
     session 167's map frame the whole-frame difference is a **single gain of
     1.932** — the map's fade — and the residual after that gain is `0.99` mean,
     i.e. no draw changed.
9. **Fog alpha is decoded but not applied.** `G_MW_FOG` stores `mul`/`offset` the
   way `RSP::setFog` does, and `transform_vertex` keeps the vertex alpha. RT64
   would replace alpha with `clamp((max(tfPos.z,0)/tfPos.w * mul + offset)/255,
   0, 1)` when `G_FOG` is set (`RSPProcessCS.hlsl:70-80`). Scene `0x0003`'s
   census has no `G_FOG` sets, and the title's fog is drawn as repaired texrect
   pieces (`OGRE_FOG`), so applying per-vertex fog alpha would change a screen
   this round must not move. It is recorded as open work, not silently skipped.

## Evidence

- RT64 lighting: `tools/RT64/src/shaders/RSPProcessCS.hlsl:50-108` (normals from
  the Vtx colour bytes over 127, fog, ambient at `lightIndex + lightCount - 1`,
  `min(resultColor, 1)`), `tools/RT64/src/shared/rt64_rsp_light.h:46-64`
  (`computeNDotL` is the point-light arm only; `computeDirLight` is
  `max(dot(norm, localDir), 0) * col` with `localDir = normalize(mul(float4(posDir,
  0), worldMatrix))`).
- RT64 world matrix: `tools/RT64/src/hle/rt64_rsp.cpp:538`
  (`worldTransforms.emplace_back(mul(modelMatrixStack.back(),
  extended.viewProjMatrix))`), `:563` (`usesPointLighting`),
  `:928-955` (`setLight`/`setLightColor`/`setLightCount` and the
  `usesPointLighting && light.pos.kc > 0` selection).
- RT64 light/GBI layout: `tools/RT64/src/hle/rt64_rsp.h:78-112` (`RawLight` is
  four words; `PosLight`/`DirLight`), `:163` (`std::array<Light,
  RSP_MAX_LIGHTS + 1>`, `RSP_MAX_LIGHTS` 7), `tools/RT64/src/gbi/rt64_gbi.h:39`
  (`pointLighting = false`), `tools/RT64/src/gbi/rt64_gbi_f3dex2.cpp:43-96`
  (`moveMem` `MV_LIGHT`, `moveWord` `G_MW_NUMLIGHT`/`G_MW_LIGHTCOL`/`G_MW_FOG`),
  `:174-176` (F3DEX2 cull constants).
- RT64 culling: `tools/RT64/src/hle/rt64_rsp.cpp:1059` (both cull bits draw
  nothing), `:1091-1097` (the front-cull index swap), `:1131-1140`
  (`U`/`V`/`N = cross(V, U)`, `visible = N.z >= 0`).
- Light bytes (browser, mission, scene `0x03`):
  `/tmp/w171/m4-a1-page.txt` — `[GFX-LIGHT] ... lights=1 point=0
  L0=[col 77 77 127 dir -3 -103 -59 raw 4D 4D 7F 00 4D 4D 7F 00 FD 99 C5 00]
  amb=[col 76 76 126 raw 4C 4C 7E 00] norm=[0.000 1.000 0.000]
  shade=[0.298 0.298 0.494 1.000]`, `lit=1` line, and the last line
  `lit=31200 unlit=2672 culled=4345 lights=1 point=0`.
- Culling on the mission: `culled=4345` with `lit=31200 unlit=2672` on the same
  log, and `culled=212` at the 212th lit draw.
- Frame comparison: `/tmp/w171/m4-a1-01.png` (browser canvas, 3 shots 22 s after
  `[scene] id=0x0003`, all three identical) against `/tmp/w168/nat2/t.800`
  (614 400 bytes = 320x240 RGBA16, the target session 168/170 used). The script
  that prints the numbers and writes the proof is `/tmp/w171/proof171.py`; it
  also re-measures session 170's frame `/tmp/w170/m5-a1-01.png` (`23.72/15.30/8.11`,
  `exact 0.3090`; session 170 recorded `23.55/15.12/7.88`/`0.3091` for the same
  frame with its own invocation of the same comparator, a 0.17 difference).
- Proof: `docs/proofs/web-mission-lighting.png` — 1928x480, three 640x480
  panels at 2x: browser before (session 170), browser after, native `t.800`.
- Tutorial: `/tmp/w171/tut-00..02.png` and `/tmp/w171/tut-milestones.txt`
  (`?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1`, `shots.cjs --secs 90 --shots 3 --gap
  2000`); box regions against `/tmp/w166/nat17/t.335.bin` with session 167's
  region definitions.
- Title: `/tmp/w171/title-00..02.png` (`?OGRE_SCENE=title`, `--secs 45 --shots
  3 --gap 2000`), matched against `/tmp/w162/nat-fog/t.*.bin` with
  `/tmp/w167/title_phase.py`.
- Map: `/tmp/w171/map-00..01.png` (`?OGRE_SCENE=0x05`, `--secs 80 --shots 2
  --gap 2500`), compared with `/tmp/w167/ab/map-after-00.png` (gain 1.932,
  residual 0.99).

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | F3DEX2 geometry-mode constants (`kGeom*`); `RenderState::lights`/`num_lights`/`fog_mul`/`fog_offset`; `G_MOVEMEM` `MV_LIGHT` and `G_MOVEWORD` `G_MW_NUMLIGHT`/`G_MW_LIGHTCOL`/`G_MW_FOG` parsing; `compute_vertex_light` and its use in `transform_vertex`; RT64's screen-space backface culling in `draw_tri`; the permanent `[GFX-LIGHT]` census |
| `docs/proofs/web-mission-lighting.png` | browser before / browser after / native, 2x each |
| `docs/HANDOFF-2026-10-18-session171.md` | this file |
| `PLAN.md` | item 8(s) continuation and a new open item |
| `docs/STATUS-LOG.md` | session 171 entry |
| `docs/README.md` | session 171 row |
| `docs/DECISIONS.md` | decision 171 |

No commit was made. No probe was added: the `[GFX-LIGHT]` census is a permanent
diagnostic and everything else lives in `/tmp/w171` (outside the repo).
`grep -rn "probe171\|OGRE_P171\|S171AB" app/ debug/ RecompiledFuncs/
Bank*Funcs/` is empty. The working tree's other modified files (`app/src/web_platform.cpp`,
`app/web/*`, `debug/probes/*`, `docs/guides/*`, `docs/scenes.md`) were already
modified by earlier sessions in this checkout; this round touched only
`app/src/web_renderer.cpp` among sources.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean (built three times:
  the first implementation, the corrected light colour bytes, and the final
  source with the constants trimmed).
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1` (on the session-170 build and again on the final
  build).
- `node debug/probes/boot.cjs --attempts 3 --secs 40` — `NO RENDER` on three
  attempts while the machine was at load average 14-24 (the boot's idle
  trajectory). `--attempts 8` then `--attempts 6` on the same build: `RENDERED`
  on attempt 1 both times (`nonBlack=25252` and `25288`).
- Browser mission route: `/tmp/w171/drive171.cjs` (session 170's
  `/tmp/w170/drive170.cjs` with the idle-abort window raised from 30 s to 90 s,
  because the loaded machine stalls the boot),
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
  `--shots 3..4 --gap 3000..4000 --pre-delay 22000 --attempts 8 --secs 200`.
  Attempt 1 reached `[scene] id=0x0003` at t=97.3 s; the three shots at
  t=147.0/152.3/157.5 s are byte-identical (`md5 7b2759a7b762db18fa9771de7a3c7877`),
  `nonBlack=303208`, `colorful=240456`.
- Tutorial, title and map captures as listed under Evidence.
- `grep` for probes (empty), `git status --short` inspected.

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **Depth (`G_ZBUFFER`, 516 463 draws of scene `0x0003`) is still
   unimplemented**, so overlapping terrain surfaces are drawn in submission
   order. It is the largest remaining item for this scene and is not measured
   here.
2. **Per-vertex fog alpha (`G_FOG`) is decoded but not applied** (Result 9). The
   mission's census has no `G_FOG` sets; a scene that has them needs the alpha
   arm and a check that the title's repaired fog pieces do not move.
3. **The boot's idle trajectory is load-sensitive.** `boot.cjs --attempts 3`
   reported `NO RENDER` while the machine was at load average 14-24, and then
   `RENDERED` on attempt 1 when the same command was run with more attempts on
   the same build. Session 166 measured the same thing at load 24-52. A failed
   smoke probe on a loaded machine says nothing about the build; re-run it.
4. **Point lights (`G_POINT_LIGHTING`) remain unimplemented and are unreachable
   in the reference** (`gbi/rt64_gbi.h:39`). If a GBI ever sets
   `flags.pointLighting`, `computePosLight`/`computeAttenuation`
   (`shared/rt64_rsp_light.h:28-53`) is the code to port.
5. **The mission frame comparison is between two intro phases**: the browser's
   camera and portrait box are at slightly different positions from `t.800`'s.
   A frame-locked pair (the same intro tick in both renderers) would make the
   whole-frame number meaningful; the terrain regions are the measurement this
   round stands on.
