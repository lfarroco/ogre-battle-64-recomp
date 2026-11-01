# Session 207 handoff — the browser cathedral backdrop's first njpeg pass lost its rows

## Goal

The developer reports that in the browser renderer the cathedral backdrop loses
its top-left quadrant after the first New Game question: the date-of-birth
dialogue draws over black where the 320x240 backdrop sub-image should be. The
same artifact appeared in the native renderer for a while (the sessions 56/57
stale backdrop). Find whether the browser case is the same fault and fix it.

## Result

The top-left quadrant is the njpeg assembly's pass 0 (320x240). It was not a
decode or a draw fault: the S2DEX2 draw ran, but its rows were dropped before the
CPU readback copied them. The writeback guard hashed the colour image's whole
`row_bytes * height` range and dropped the whole pending band when that hash
changed. A 320x476 target at `0x025C00` spans `0x025C00..0x070200`; the game's
next framebuffer starts at `0x04B400`, which is row 240 of that target
(`0x04B400 - 0x025C00 = 0x25800 = 640 * 240`). The writeback of `0x04B400`
therefore changed `0x025C00`'s whole-image hash, and pass 0's rows 0..239 were
dropped with it. The same band also mixed the movie's rows with the pass's rows,
and the epoch reference kept the movie's reference for a row the pass redrew.

The fix is a per-row writeback reference:

- `GlFramebuffer::era_row_hash` holds one XXH3-64 per guest row. It is refreshed
  immediately before every draw into that row (`reference_era_rows`), so a CPU
  write that the draw overwrites is not mistaken for a later write.
- `readback_framebuffer` compares exactly the rows in its pending band and skips
  only a row whose RDRAM changed after its last draw. It still drops the band
  when every row is stale, which is the session-202/203 protection for a
  framebuffer range the game has reused.
- The once-per-batch whole-band pre-test in `snapshot_fb_ram_before_draw` is
  removed; the writeback's per-row comparison replaces it. The whole-image
  `rdram_hash` stays for the framebuffer-as-texture guard
  (`build_image_source`), where the image's entire range is the right unit.

The second visit (date of birth) now renders the complete backdrop:
`docs/proofs/web-newgame-cathedral-birth-session207.png`.

## Evidence

### Before: pass 0 copied a mostly black buffer

The failing route reached the cathedral with the njpeg target `0x025C00`. The
four S2DEX2 passes are `[GFX-S2D] task 4066..4069 ... cimg=0x025C00`; the first
sync then read the game's own source, `state[0x64] = 0x80025C00`, and copied
320x240:

```
[GFX-FBWRITE-STALE] fb 0x025C00 320x476: RDRAM changed since the GPU copy
                    (hash C27A17C0300F2C98 != A74790337EDFD324); leaving RDRAM as the CPU left it
[GFX-FBSYNC] sync 1: waited=60ms ramNonBlack: 0x000400=27719 0x025C00=14875
             0x04B400=5747 state=0x801AACE0 src=0x80025C00 dst=0x801AAE90
             njpeg_in=0x801D1030 copy=320x240
[GFX-FBREAD] fb 0x025C00 rows=[0,240] nonBlack=75678/77120 ...
```

The stale line is the whole-image comparison of the session-203 code. `14875` is
the previous frame's leftover, not the 320x240 pass. The next sync writes the
same target for pass 1 and the band is then full (`76800`).

The range arithmetic at the same point: the target is `0x025C00` at 320x476 with
pitch 640, so the range is `0x025C00 + 0x4A600 = 0x070200`. `0x04B400` is inside
it, and `0x04B400`'s own `ramNonBlack` changes (5747, 5502, 5741, 5734) during
the four passes. The whole-image hash cannot distinguish those bytes from the
`0x025C00` rows.

### After: only the changed rows are skipped

The same route on the fixed build targets `0x025C00`, and the guard names the
rows it leaves alone:

```
[GFX-FBWRITE-STALE] fb 0x025C00 320x476 row=262: RDRAM changed since the GPU copy
                    (hash F056E7C849749867 != ED82700D9CE6E10C); leaving that row ...
... (rows 255..262, the movie's lower rows)
[GFX-FBSYNC] sync 1: waited=129ms ramNonBlack: 0x000400=27719 0x025C00=69915
             0x04B400=49579 state=0x801AACE0 src=0x80025C00 dst=0x801AAE90
             njpeg_in=0x801D1030 copy=320x240
[GFX-FBSYNC] sync 2: ... 0x025C00=76800 ... copy=176x240
```

`69915` of `76800` is the drawn 320x240 sub-image; the previous build copied
`14875`. Rows 255..262 are above the pass's 240 rows in the 476-row target and
belong to the previous frame, so skipping them is correct.

The second visit's assembly uses `0x000400`; its first sync reads
`0x000400=76800` and the dialogue frame is complete
(`docs/proofs/web-newgame-cathedral-birth-session207.png`, the date-of-birth
question over the full cathedral).

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean on the final tree.
- `node debug/probes/scene.cjs --route cathedral-birth --settle 15000 --repeat 3
  --repeat-gap 5000 --points '160,60;160,150;40,120;300,120'
  --out debug/out/w207/birth-rows-only --log-cap 300000`.
  The route reaches the title, the New Game sequence, scene `0x0007` after 14
  `KeyX` presses, then the second `0x000D`; the framework reads
  `scene 0x0007` and the second visit from `_ogre_web_active_scene`.
- `node debug/probes/scene.cjs --route cathedral-skip ...` on the same build
  twice: one run targets `0x000400` (`76800` at sync 1), one targets `0x025C00`
  (`69915` at sync 1, `76800` at sync 2), which is the pre-fix black case.
- `node debug/probes/scene.cjs --route tutorial-field`: scene `0x0003`, 15.26
  display lists/s, the `Movement & Liberation` box over the textured practice
  field (`debug/out/w207/regress-tutorial-normal.png`).
- The cathedral `--route cathedral` check before the fix is
  `debug/out/w207/birth-before-log-attempt1.txt` (the log quoted above).

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `GlFramebuffer::era_row_hash`; `reference_era_rows` and `clear_era_rows`; `readback_framebuffer` compares and skips per row; the whole-band pre-test is removed; the whole-image hash stays for the framebuffer-as-texture guard |
| `debug/probes/scene.cjs` | `cathedral-birth`, `cathedral-skip` and `name-entry` routes; `lastScene` reads `Module._ogre_web_active_scene` (the `[scene]` ring evicts after 20000 lines); `--log-cap`; `OGRE_NO_AUDIO=1` on the cathedral routes |
| `docs/proofs/web-newgame-cathedral-birth-session207.png` | the date-of-birth dialogue over the complete backdrop |
| `docs/HANDOFF-2026-10-31-session207.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/scenes.md`, `docs/guides/web-probes.md`, `docs/guides/njpeg-backgrounds.md` | status, log, decision, index, scene rows, probe routes, the browser writeback trap |

## Probes used and reverted

No generated code and no `app/src` probe was added. The probes are the existing
`debug/probes/scene.cjs` routes; the log-cap and `_ogre_web_active_scene` changes
are part of the tool and stay. A temporary build that waited on an in-flight
display list (`dl_in_flight_`) and read the sync request before the draw-queue
swap was measured on `cathedral-skip`; it left the artifact unchanged, so both
changes were reverted and are not in the tree. The observation is recorded under
Open.

## Open

1. **The readback can be requested while a display list is still being walked.**
   The runtime posts `sp_complete` before `send_dl`, and the game's
   `func_80089A10` spin is on the RSP counter `D_800E79A4`, not the DP message.
   `sync_framebuffers` therefore has a window in which the njpeg pass is not in
   `draw_queue_` yet. In the runs measured here the display list had published
   (`dlwait=0`), so the failing artifact was the row guard, not this window; an
   in-flight counter plus the sync request read before the queue swap did not
   change the frame. The window is real at the instruction level and is the next
   thing to measure if a first-pass copy comes out stale on another screen.
2. **The row reference hashes the full row width.** A CPU write to the same guest
   row outside the columns the draw covered would skip the whole row. No reached
   screen writes columns beside a drawn row; narrowing the reference to the
   draw's columns is the next refinement if one does.
3. **The session-202 heap protection is now per row.** That crash was a stale
   640x481 image whose whole range the allocator had reused; every row the game
   wrote there is skipped, which is the protection it needs, but rows the game
   did not touch are still written back. Re-run the tutorial `5. Unit Commands`
   battle (the route needs the hand-driven lesson menu) before relying on the
   guard for a freed range.
