# Session 174 handoff — four renderer-state gaps closed: G_TP from bit 19, the tile shift scale, the chroma-key combiner inputs, and per-vertex fog alpha

## Goal

Close the four remaining renderer-state gaps the session-166 gap audit ranked
(items 3, 11, 12 and 5), one variable at a time, and re-verify every screen this
port reaches on the final build with depth on by default: the tutorial `0x17`
box, the title `0x04` / attract frames, the world map `0x05`, the mission `0x03`
route, the publisher stills `0x0A` and the attract story `0x0B`.

Timebox: 100 minutes. The round ran to about 100 and the machine was at load
average 10-12 throughout, which cost the boot retries.

## Result

1. **`G_TP` now comes from OTHERMODE_H bit 19.** `record_state` read bit 10,
   which is not an RDP field; RT64's `G_MDSFT_TEXTPERSP` is bit 19
   (`tools/RT64/src/shared/rt64_f3d_defines.h:51`) and the sampler consumes it at
   `src/shaders/TextureSampler.hlsli:221-226`. `cmd.tex_persp` is now
   `(kind != 0) || ((othermode_h >> 19) & 1)`; the rect arm keeps `kind != 0`
   because RT64's `renderFlagRect` skips the perspective correction. The new
   permanent `[GFX-TP]` census (`family kind= omh= old_persp= new_persp=
   changes=`) printed **changes=0 for every family on every reached screen**, so
   no draw changed:
   | screen | families (kind, omh) | changes |
   |---|---|---|
   | tutorial `0x17` | (0, 0x08AC70) | 0 |
   | title `0x04` | (2, 0x000C30) | 0 |
   | map `0x05` | (2, 0x000C30), (2, 0x008C30) | 0 |
   | mission `0x03` | 8 triangle families: 0x082C30, 0x083CF0, 0x088C30, 0x08AC30, 0x08AC70, 0x08BCF0, 0x182C30, 0x182C70 | 0 |
   | mission `0x03` | 6 rect families: 0x000C30, 0x002C30, 0x003CF0, 0x008C30, 0x008C70, 0x008CF0 | 0 |
   | attract story `0x0B` | (0, 0x082C30), (0, 0x08AC30), (0, 0x18AC30) | 0 |

   Every observed triangle family has bit 19 set **and** bit 10 set, so the old
   and new readings agree there, exactly as the audit said. The rect families
   have bit 19 clear and bit 10 set, so the rects are unaffected by the change
   and are drawn with the correction suppressed by the `kind != 0` rule. The
   audit's "triangle draws all have bit 19 set while the 0x002CF0 family is
   rects" is confirmed; no family changes.
2. **The tile `ShiftScaleMap` is applied in the sampler.** `RenderState`'s tile
   carries `shifts`/`shiftt` from `G_SETTILE` (bits 0-3 and 10-13), and
   `record_state` now stores RT64's table value for each field
   (`tex_shift_scale`, the 16 entries of `hle/rt64_state.cpp:33-51`; RT64 stores
   the same value in `RDPTile::shifts`/`shiftt` at `:306-307`). The sampler
   multiplies the coordinate in the same expression as RT64
   (`shaders/TextureSampler.hlsli:226`):
   `uv = v_tex * (u_tex_persp ? 1 : 0.5) * u_tshift<unit>`, i.e. before
   `sample_unit`'s `round(uv * 128) / 128` quantization.
   - `[GFX-SAMPLER] shifted=` is **0** on the tutorial, title and map, so those
     frames cannot move (and they do not: see Result 3).
   - The mission is an **exercised** screen for this item: `shifted=18794` of
     27692 textured draws (and 23114-24563 of 31825-33450 on a second route).
     An interleaved A/B on the final build ("apply the shift" against "force the
     scale to 1", flag bit 9, 400-500 ms between the pair) captured 5 pairs over
     two runs: **4 are bit-identical** (`meanAbs 0.0000`, `exact 1.0000`) and the
     one that differs (pair 1 of the second run, `1.037`, `exact 0.9382`)
     coincides with the screen's own animation: two frames of the settled intro
     captured 3 s apart in one run are bit-identical (`mission-a1-00` vs `-01`),
     and the next capture 24 s later differs by `1.614` (`exact 0.7747`). So the
     multiply changes no pixel of the mission frame. The census counts all four
     fields of **both** tile units, so the nonzero values on that screen are on a
     unit or a coordinate the draw does not sample; this round did not separate
     the two and does not need to, because the frame is measured unchanged.
   - The attract story `0x0B` reports `shifted=229` of 2438 textured draws. No
     A/B was run on that screen, so "unchanged" there rests on the mission
     measurement and not on its own.
   - No synthetic shift control was added. The existing `test_draw` target is
     the canvas and the depth control's scratch image, and a shift control needs
     a sampled coordinate whose texel changes with the scale plus a readback;
     that is more than the round had left. The item is exercised by the mission
     A/B instead and by no synthetic control.
3. **The chroma-key combiner inputs exist.** `OP_SETKEYR`/`OP_SETKEYGB` left the
   ignore list and decode RT64's fields (`gbi/rt64_gbi_rdp.cpp:154-169`):
   `cR = p1(8,8)`, `sR = p1(0,8)`; `cG = p1(24,8)`, `sG = p1(16,8)`,
   `cB = p1(8,8)`, `sB = p1(0,8)`; widths ignored. `RDP::setKeyR`/`setKeyGB`
   (`hle/rt64_rdp.cpp:1223-1236`) divide each by 255, and the shader's
   `color_input` now returns `u_key_center` for `C_KEY_CENTER` and
   `u_key_scale` for `C_KEY_SCALE` (`shared/rt64_color_combiner.h:482-485`,
   `RasterPS.hlsl:185-186`). A one-shot `[GFX-KEY]` line reports the first draw
   whose evaluated mux reads either input, with the registers, the `G_CK_KEY`
   bit (OTHERMODE_H bit 8, `shared/rt64_f3d_defines.h:28-30`) and the cycle
   count. **No `[GFX-KEY]` line appeared on the tutorial, title, map, attract
   story, mission or publisher stills**, so the item changes no frame; a screen
   that selects a key input will report itself.
4. **Per-vertex fog alpha is applied.** `transform_vertex` now takes RT64's arm
   when `G_FOG` (`0x00010000`, `shared/rt64_f3d_defines.h:102`) is set
   (`shaders/RSPProcessCS.hlsl:70-80`, `hle/rt64_rsp.cpp:625-640`):
   `alpha = clamp((max(tfPos.z, 0) / tfPos.w * fog_mul + fog_offset) / 255, 0, 1)`
   with `tfPos.z / tfPos.w` the **clip** z/w (`pos.z * inv_w` in
   `transform_vertex`), not the RDP screen z that `out[9]` carries (that one has
   the viewport scale/translate applied). The colour stays the lit or raw colour.
   RT64's `fogIndex > 0` arm is entered for every vertex of a `G_FOG` workload
   and its `rspFog` vector exists from the first fogged transform with the last
   `G_MW_FOG`'s mul/offset (`RSP::setFog`, `hle/rt64_rsp.cpp:990-994`, default
   zero at `:91-92`), so the browser uses the decoded `fog_mul`/`fog_offset`
   with no additional gate. The rect paths do not run RSP vertex processing, so
   fog is applied on the triangle path only, which is where `transform_vertex`
   is called. A one-shot `[GFX-FOG] per-vertex alpha:` line reports the first
   vertex that takes the arm; **none of the six reached screens logged it**, so
   no frame moves: their census has no `G_FOG` set.
5. **Frames (before -> after).** The regression numbers this round measured on
   the final build, depth on by default:

   | screen | measurement | session 173/171 | this build |
   |---|---|---|---|
   | tutorial `0x17` | frame rows 166..170 vs native `t.335` | 0.29 | **0.29** |
   | tutorial `0x17` | box rows 166..219 vs native | 0.22 | **0.30** (0.18 without the cursor band) |
   | tutorial `0x17` | parchment 172..195 / portrait / lower-no-cursor | 0.25 / 0.00 / 0.18 | **0.25 / 0.00 / 0.18** |
   | title `0x04` | renders | 288 104 nonBlack | **288 104 nonBlack** |
   | map `0x05` | nonBlack; fade gain / residual vs session 167 frame | 252 820; 1.932 / 0.99 | **252 820; 1.927 / 0.84** |
   | mission `0x03` | whole frame meanAbs R/G/B; exact vs native `t.800` | 7.25; 0.3609 | **7.07-7.58; 0.3489-0.3633** |

   The tutorial box's 0.30 against session 167's 0.22 is the dialogue cursor's
   blink phase and nothing else: band by band the two frames agree
   (`rows166..195` 0.25 vs 0.25, `rows196..218 x16..276` 0.11 vs 0.11,
   `x291..304` 1.55 vs 1.55), and only the glyph band `x276..291` differs
   (3.46 against 0.00). The tutorial's `[GFX-SAMPLER]` still reads
   `shifted=0 persp=0 nonpersp=0`, i.e. no family changed its `G_TP` reading.
6. **The title comparison the task named could not be reproduced, and the reason
   is a screen identity.** The stored native series `/tmp/w162/nat-fog/t.*.bin`
   (397 frames) contains the attract backdrops: `t.131` is clouds only and
   `t.469` is the clouds with "Long ago, there was a time when strength ruled…".
   Session 171's `?OGRE_SCENE=title` capture matched `t.469` at `0.11`, and that
   frame is the attract **story** overlay. This round's forced-title captures
   landed on the title **menu** (logo, New Game/Tutorial/Stereo, `nonBlack`
   288 096/288 104, 8 frames over 35 s identical), which no frame of that series
   shows: its best match is `14.78` at `t.115`. The forced story `0x0B` showed a
   different attract beat (the globe with "…Long ago, when gods walked among
   men…"), best match `46.94` at `t.248`. So the 0.11-0.20 number is not
   reproduced and no claim is made that the story frame is unchanged: the title
   menu is `rect`-only with one `[GFX-TP]` family (`changes=0`) and
   `shifted=0`, so the four items cannot move it, but the attract overlay the
   series shows was not reached this round.

## Evidence

- `G_TP`: `tools/RT64/src/shared/rt64_f3d_defines.h:51`
  (`G_MDSFT_TEXTPERSP 19`), `src/shaders/TextureSampler.hlsli:221-226`
  (`texturePerspective = otherMode.textPersp() == G_TP_PERSP`,
  `applyCorrection = !texturePerspective && !renderFlagRect(renderFlags)`,
  `perspCorrectionMod = applyCorrection ? 0.5 : 1.0`).
- Shift scale: `src/hle/rt64_state.cpp:33-51` (`ShiftScaleMap`: 1, 1/2 … 1/1024,
  32, 16, 8, 4, 2), `:306-307` (`dstRDPTile.shifts = ShiftScaleMap[tile.shifts]`),
  `src/shaders/TextureSampler.hlsli:226`
  (`uvCoord = inputUV * perspCorrectionMod * float2(rdpTile.shifts, rdpTile.shiftt)`),
  `:230-234` (the 1/128 rounding that follows).
- Key colour: `src/hle/rt64_rdp.cpp:1223-1236` (centre and scale over 255,
  widths ignored), `src/gbi/rt64_gbi_rdp.cpp:154-169` (the field extraction),
  `src/shaders/RasterPS.hlsl:185-186` (`ccInputs.keyCenter`/`keyScale`),
  `src/shared/rt64_color_combiner.h:482-485` (`C_KEY_CENTER`/`C_KEY_SCALE`
  return those `float3`s).
- Fog: `src/shaders/RSPProcessCS.hlsl:70-80` (`fogAlpha = (max(tfPos.z, 0) /
  tfPos.w) * rspFog.mul + rspFog.offset`, `clamp(fogAlpha / 255, 0, 1)`),
  `src/hle/rt64_rsp.cpp:625-640` (`usesFog = geometryMode & G_FOG`,
  `curFogIndex = vertexFogIndex + 1`), `:990-994` (`setFog`), `:91-92` (the
  zero default), `src/shared/rt64_f3d_defines.h:102` (`G_FOG 0x00010000`).
- Family census and the new diagnostics: `[GFX-TP]`, `[GFX-KEY]`, `[GFX-FOG]
  per-vertex alpha:` lines in the runs under `/tmp/w174`:
  `tut-milestones.txt`, `title2-milestones.txt`, `map-milestones.txt`,
  `mission-a1-page.txt`, `story-milestones.txt`.
- Shift A/B: `/tmp/w174/shiftab-a1-on{0..2}.png` / `-off{0..2}.png`,
  `/tmp/w174/shiftab2-a1-on{0..3}.png` / `-off{0..3}.png`, and the page logs
  `shiftab-a1-page.txt` (`shifted=21665` at the 24 000th textured draw) and
  `shiftab2-a1-page.txt` (`shifted=18794` of 27692).
- Frame comparison: `tools/web_frame_cmp.py` for the canvas-vs-`OGRE_CAPTURE_TARGET`
  numbers and `/tmp/w167/regions2.py`'s region definitions for the tutorial,
  `/tmp/w167/title_phase.py` for the title series, `/tmp/w171/proof171.py`'s
  region set for the mission.

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `cmd.tex_persp` from OTHERMODE_H bit 19; `tex_shift_scale()` (RT64's table) and `DrawCmd::tex_shift`, `u_tshift0`/`u_tshift1` multiplied into the sampler coordinate before the 1/128 rounding; `RenderState::key_center`/`key_scale`, `OP_SETKEYR`/`OP_SETKEYGB` decode, `DrawCmd::key_center`/`key_scale`, `u_key_center`/`u_key_scale` as combiner inputs 6/7; `kGeomFog` and the `G_FOG` per-vertex alpha arm in `transform_vertex`; the permanent `[GFX-TP]` family census and the one-shot `[GFX-KEY]`/`[GFX-FOG] per-vertex alpha` notes |
| `docs/HANDOFF-2026-10-18-session174.md` | this file |
| `PLAN.md` | 8(t)/8(u) plus the new open items |
| `docs/STATUS-LOG.md` | session 174 entry |
| `docs/README.md` | session 174 row |
| `docs/DECISIONS.md` | decision 174 |

No commit was made. The temporary probe `probe174` (debug flag bit 9 forcing the
shift scale to 1 for the mission A/B) was removed and the build rebuilt;
`grep -rn "probe174\|shift_off" app/ debug/ RecompiledFuncs/ Bank*Funcs/` is
empty. The `[GFX-TP]`, `[GFX-KEY]` and `[GFX-FOG] per-vertex` lines are permanent
diagnostics of the kind this renderer already carries.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` — clean four times (the
  implementation, the permanent-census cleanup, the A/B build, and the cleaned
  build).
- Tutorial `0x17`: `node debug/probes/shots.cjs --url
  "...?OGRE_SCENE=0x17&OGRE_SCENE_LOG=1" --secs 90 --shots 3 --gap 2000` and a
  second series `--shots 4 --gap 800` (`/tmp/w174/tut*`).
- Title `0x04`: `?OGRE_SCENE=title`, `--secs 45 --shots 3 --gap 2000` (two
  attempts failed with `tasks=1` at load average 12, one succeeded with
  `--secs 60 --attempts 4`), then `--shots 8 --gap 5000` (`/tmp/w174/title*`).
- Map `0x05`: `?OGRE_SCENE=0x05`, `--secs 80 --shots 2 --gap 2500`
  (`/tmp/w174/map*`).
- Mission `0x03`: `/tmp/w173/drive173.cjs` with
  `?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6`,
  `--shots 3 --gap 3000 --pre-delay 22000` (`/tmp/w174/mission*`), and
  `/tmp/w173/drive173ab.cjs` twice for the shift A/B.
- Attract story `0x0B`: `?OGRE_SCENE=story&OGRE_SCENE_LOG=1`, `--secs 60
  --shots 3 --gap 4000` (`/tmp/w174/story*`).
- `testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` -> exit 0,
  `fbtex hits=1 lines=1`; `--flags 128 --expect-depth near` -> exit 0, readback
  `rgba=(255,0,0)`; `--flags 384 --expect-depth far` -> exit 0,
  `rgba=(0,0,255)`. Log: `/tmp/w174/battery.log`.
- `boot.cjs --attempts 3 --secs 40` -> `NO RENDER` (`sawRealDl=false
  rendered=false` on all three attempts) while the machine was at load average
  10-12, which is the session-171 caveat for this probe: a failed smoke probe on
  a loaded machine says nothing about the build. The retry did not run (the
  background invocation produced an empty log), so the smoke probe is **not
  re-run on a quiet machine** this round.
- Publisher `0x0A`: **not reached.** `?OGRE_SCENE=publishers` and
  `?OGRE_SCENE=0x0A` both failed to advance (`tasks=0`/`tasks=1` on every
  attempt, no `[scene]` line, no milestone in the page log); the one canvas
  captured is the unpresented WebGL canvas (its bottom two thirds are noise,
  `nonBlack=134449`), not a publisher still. The scene is therefore not
  confirmed to render this round.

### What was not verified

- The title/attract 0.11-0.20 comparison (Result 6): the series' beat was not
  reached.
- The attract story's shift count (`229`) was not A/B'd.
- Publisher `0x0A` (closed above) and the `boot.cjs` smoke probe on a quiet
  machine.

## Open

1. **The tutorial box's 0.22 -> 0.30 is the dialogue cursor's blink phase**, not
   a draw. A capture that takes several frames 800 ms apart and picks the phase
   where the glyph matches native is what makes the number comparable
   (session 167 measured 0.22 on a lucky phase; the same build measures 0.30 on
   the other).
2. **`shifted=` is nonzero on the mission (18 794-24 563 draws) and the attract
   story (229) while the mission frame does not move with the multiply.** The
   census counts both tile units, so the next step is to count only the unit the
   combiner samples; that turns the A/B result into a mechanism.
3. **`G_TEXGEN`, `G_SHADE`, the `G_AC_DITHER` hash, `ZMODE_DEC`, the depth image's
   RDRAM transfer and `a_w` in `gl_Position` are still open** (unchanged from
   sessions 172/173; see the gap audit's items 6, 8, 13, 1 and session 172's
   open list).
4. **The title/attract comparison needs a frame-locked native pair** for the same
   reason the mission does: the browser's capture time relative to the attract
   loop is not controlled, and the forced entry lands on a different beat.
