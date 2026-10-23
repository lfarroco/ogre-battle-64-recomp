# Session 185 handoff — the browser's `G_GEOMETRYMODE` mask was inverted, and that alone hid `G_LIGHTING` and `G_TEXTURE_GEN` at the QUEST q's

## Goal

Developer request: *"continue the work on fixing the untextured q's in the game's
intro"*. The open item was `PLAN.md` 20 / `docs/HANDOFF-2026-10-23-session184.md`
§5-§6: the browser presents the QUEST spin's three `q` meshes as a flat yellow
surface, native shows moving highlights.

## Result

The q meshes now show the environment-map highlights native shows, and the
cause was one inverted bit mask in the display-list decoder.

`app/src/web_renderer.cpp`'s `OP_GEOMETRYMODE` read the low 24 bits of `w0` as
the bits to clear (`geometry_mode & ~off`). RT64 reads them as the bits to
keep: `GBI_F3DEX2::geometryMode` does `offMask = p0(0, 24)`, then
`state->rsp->modifyGeometryMode(offMask, w1)`, and `RSP::modifyGeometryMode`
does `geometryMode &= offMask; geometryMode |= onMask`
(`tools/RT64/src/gbi/rt64_gbi_f3dex2.cpp:104-108`,
`tools/RT64/src/hle/rt64_rsp.cpp:886-890`).

Every `gSPSetGeometryMode` therefore wiped the browser's geometry mode to its
own set bits. The QUEST q state block is a chain of those calls, and the result
was `geom=0x040000` (`G_TEXTURE_GEN` only) instead of native's `0x260405`
(`G_ZBUFFER|G_SHADE|G_CULL_BACK|G_SHADING_SMOOTH|G_LIGHTING|G_TEXTURE_GEN`).
Without `G_LIGHTING` the vertex colour bytes stayed as a colour instead of a
normal, and RT64's `usesTextureGen` — which requires both bits — never
fired. The list then samples the vertex `s,t`, which the spin's `G_TEXTURE`
scale `0x07C0` puts at about 1.3 texels: a small, nearly constant patch of the
32x32 image, which is the flat yellow the developer saw.

Two changes are in the tree:

1. The geometry-mode decode uses RT64's keep mask.
2. `G_TEXTURE_GEN` and `G_TEXTURE_GEN_LINEAR` are implemented as RT64
   implements them (`computeTextureGen`, `shaders/TextureGen.hlsli:19-33`), with
   the `G_MOVEMEM` `MV_LIGHT` slots 0/1 (the lookat X/Y vectors,
   `RSP::setLookAt`, `hle/rt64_rsp.cpp:969-982`).

## 1. The keep mask is the ROM's own idiom

The game's display lists settle which reading is right, without a run:

| word | count | keep-mask reading | clear-mask reading |
|---|---|---|---|
| `D9000000 00000000` | 41 | clear every geometry-mode bit (the reset idiom) | no-op |
| `D9FFFFFF 00000000` | 0 | no-op | clear every bit |
| `D9FFFFFF 00200404` (ROM `0x80180D90`) | — | add `G_LIGHTING\|G_SHADE\|G_CULL_BACK` | clear all, set those three |

`gSPClearGeometryMode(word)` has to pass `~word` to a keep mask, which is the
only way one data word can express both set and clear. A no-op emitted 41 times
is not a command any compiler or hand-written builder produces.

## 2. The q state block, from a whole-RDRAM dump

`OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=publishers OGRE_RAM_KEY_AT_MS=14500
OGRE_RAM_KEY_FULL=1 OGRE_PREF_DIR=/tmp/p185n` on the native build writes the
8 MiB image. Guest `0x801B85B8` (physical `0x1B85B8`) holds the block the q
sub-lists `G_DL`:

```
1B85B8: E7000000 00000000   G_RDPPIPESYNC
1B85C0: D9FFFFFF 00200405   keep all, set G_SHADING_SMOOTH|G_CULL_BACK|G_SHADE|G_ZBUFFER
1B85C8: D9FFFFFF 00020000   keep all, set G_LIGHTING
1B85D0: DB020000 00000018   G_MOVEWORD
1B85D8: DC08060A 801B8498   G_MOVEMEM MV_LIGHT slot 2 (light 0)
1B85E0: DC08090A 801B8490   G_MOVEMEM MV_LIGHT slot 3 (light 1)
1B85E8: FC121824 FF33FFFF   G_SETCOMBINE  TEXEL0*SHADE
1B85F0: E200001C 00552078   G_SETOTHERMODE_L
...
1B8628: DF000000 00000000   G_ENDDL
```

and the sub-list at `0x3A4778` runs it, then adds texgen:

```
3A4778: DE000000 801B85B8   G_DL the block above
3A4790: D9FFFFFF 00040000   keep all, set G_TEXTURE_GEN
3A4798: FCFFFFFF FFFCF87C   G_SETCOMBINE  TEXEL0 passthrough
3A47A0: DA380000 803A4210   G_MTX
3A47B0: FD180000 80000400   G_SETTIMG at 0x400
```

Geometry mode after `0x3A4790`, computed both ways:

| rule | value | `G_LIGHTING` | `G_TEXTURE_GEN` |
|---|---|---|---|
| keep (RT64, now the browser) | `0x260405` | 1 | 1 |
| clear (the browser before this session) | `0x040000` | 0 | 1 |

This corrects session 184 §3. That section read `D9FFFFFF 00040000` as
"clear=0xFFFFFF", concluded the sub-list clears `G_LIGHTING`, and therefore
concluded that RT64's `usesTextureGen` cannot fire in either renderer. RT64
fires it, and session 184's own `G_TEXTURE_GEN` implementation found no pixel
change because the inverted decode kept `G_LIGHTING` out of the state — the
condition it was gated on.

## 3. The lookat vectors are (1,0,0) and (0,1,0)

`MV_LIGHT` slots 0/1 appear four times in the dump. The static pair at
`0x0AEE30`/`0x0AEE40` is all zero; the per-frame pair the q sub-lists install
is:

| command | address | bytes 8/9/10 | normalized |
|---|---|---|---|
| `DC08000A` | `0x803A2B60` | `7F 00 00` | `(1, 0, 0)` |
| `DC08030A` | `0x803A2B70` | `00 7F 00` | `(0, 1, 0)` |

`computeTextureGen` projects the vertex normal on each and maps `[-1,1]` to
`[0,1024]`, so the draw samples the whole 32x32 image as the normal turns:
`sc/65536 * 1024 = 31` texels with `sc = 0x07C0`. That is the environment map.
`G_TEXTURE_GEN_LINEAR` is not set on the q lists, so the `acos` arm is
implemented but unexercised.

## 4. Measurements

The browser was captured with `python3 tools/intro_capture.py --mode web
--seconds 34 --interval-ms 100` from a natural boot, the native side with
`--mode native` and the same arguments. The spin window is the frames where the
q meshes are the only content.

| build | spin frames | frames with a channel above 200 |
|---|---|---|
| native | 169-192 (24) | 24 |
| browser, before | 205-232 (28) | 6 |
| browser, geometry fix only | 208-233 (26) | 6 |
| browser, geometry fix + texgen | 209-235 (27) | 26 |

The one frame that stays under 200 in the fixed build is the first appearance
(`196`); every other frame is `233`-`248`, and `248,240,224` is the texture's
own maximum.

The geometry fix alone changing nothing is the second part of the proof. With
the mask corrected, `G_LIGHTING` reaches the q draws and `S` in
`TEXEL0*SHADE` becomes the lit value instead of a normal byte — and the picture
is byte-comparable to the old one (`0.036` mean absolute difference on the
matched frames). Only the texgen arm moves the sample, so the two changes are
one cause: the mask kept both bits out.

Pose-matched pair (native frame 190, `t=22.749 s`, against browser frame 234,
`t=23.410 s`; the animation clocks differ, so the pose is matched by silhouette):
mask IoU `0.791` over a 160x120 mask, whole-frame mean absolute difference
`2.31` per channel, and the mask means are native `(80.4, 56.5, 17.2)` against
browser `(80.7, 63.5, 22.1)`. `docs/proofs/native-quest-rotation.png` and
`docs/proofs/web-quest-rotation.png` are that pair. `docs/proofs/web-quest-rotation-flat.png`
is a pre-change browser frame of the same spin, kept so the flat surface the
developer reported stays in the record.

Non-QUEST regression checks, one build:

- Scene `0x09` (the publisher logos), one second into the scene, against native
  at the same point in the scene: `10.032` mean absolute difference with the
  fix against `10.109` without. The scene's own fade timing is the residual.
- The first scene-`0x04` frame is byte-identical between the two builds
  (`sha256 06bff23c…`). That frame is a pre-existing browser defect (coloured
  stripes over a black lower half, while native shows the prologue text over
  the scrolling clouds); it is unchanged here and is not part of this item.

## 5. What the change touches

`RenderState` gains `lookat[2][3]`. `OP_MOVEMEM` reads slots 0/1 as
`DirLight` logical bytes 8/9/10 and normalizes them, as `RSP::setLookAt` does.
`transform_vertex` branches on
`(geometry_mode & (G_LIGHTING|G_TEXTURE_GEN)) == (G_LIGHTING|G_TEXTURE_GEN)`:
the vertex colour bytes become the signed normal over 127 (the same bytes
`compute_vertex_light` already reads), each axis is
`dot(norm, normalize(lookat * modelview))`, clamped to `[-1,1]`, then
`(v + 1) * 512` or `acos(-v) * 325.94932`, and the tile coordinate is
`(sc, tc) / 65536 * that`, matching RT64's
`tcFloats.emplace_back(TextureSc)` (`hle/rt64_rsp.cpp:748-753`) and
`(inputUV / 65536) * texgenUV`.

`app/src/gbi.cpp`'s `OGRE_DL_DECODE` line printed `GEOMETRYMODE clear=…`; it
now prints `keep=…` with the same numbers, because the label named the inverted
rule.

## Verification

- `EM_CACHE=<repo>/build-wasm/emcache make web` clean after every change and
  after the probe removal; `build-wasm/ogrebattle64.wasm` is `21130960` bytes
  (the session-184 tree built `21129871`).
- `cmake --build build-app --target ogrebattle64 -j` clean.
- The two capture series in §4 (`--mode web` and `--mode native`, 34 s at
  100 ms), plus the geometry-fix-only series.
- Native RDRAM dump at 14.5 s of a forced `publishers` run (§2, §3), read
  offline with `struct.unpack('>II')` at the guest offsets.
- ROM scan of `build-app/ogrebattle64-us-rev1.z64` for every `0xD9` command
  (§1) and for the `0x80180D90` builder in `RecompiledFuncs/funcs_6.c`.
- Synthetic controls on the final build: `testdraw.cjs --flags 96
  --expect-fbtex --expect-hits 1` -> `fbtex hits=1 lines=1`; `--flags 128
  --expect-depth near` -> `rgb=(255,0,0)`; `--flags 384 --expect-depth far` ->
  `rgb=(0,0,255)`.
- Probe removal: `grep -rn "probe185\|PROBE185\|GFX-P185" app/ debug/` empty.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `OP_GEOMETRYMODE` uses RT64's keep mask; `kGeomTextureGen`/`kGeomTextureGenLinear`; `RenderState::lookat`; `MV_LIGHT` slots 0/1 decoded; `G_TEXTURE_GEN`/`_LINEAR` in `transform_vertex` |
| `app/src/gbi.cpp` | `OGRE_DL_DECODE` prints `GEOMETRYMODE keep=` instead of `clear=` (same numbers) |
| `docs/proofs/native-quest-rotation.png`, `docs/proofs/web-quest-rotation.png` | replaced with the same-pose pair of §4 |
| `docs/proofs/web-quest-rotation-flat.png` | new: the pre-change browser spin frame, for the record |
| `docs/HANDOFF-2026-10-23-session185.md` | this file |
| `PLAN.md` | item 20 closed; the open list's entry updated |
| `docs/DECISIONS.md` | new row; the session-184 row carries the correction |
| `docs/STATUS-LOG.md`, `docs/README.md`, `docs/scenes.md` | session entry, index row, QUEST row |

## Probes used and reverted

`probe185`, tagged and gated on `OGRE_PROBE185`: two logs, one line per change
of the draw state (`[GFX-P185]`: geometry mode, texture scale, lookat, combiner)
and one per `G_GEOMETRYMODE` command (`[GFX-P185G]`: `w0`, `w1`, result). It
showed the browser's early draws at `geom=0x000000`, and its forced-`publishers`
runs never reached the q sub-lists: `?OGRE_SCENE=publishers` lands in only some
boots (session 184), and three attempts stayed on the boot or the plate. The
decoder conclusion therefore came from the ROM's `D9` idiom and the native
RDRAM dump, both offline. The probe was removed, the tree rebuilt, and
`grep -rn "probe185\|PROBE185\|GFX-P185" app/ debug/` is empty.
