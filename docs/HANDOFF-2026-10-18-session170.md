# Session 170 handoff — the browser's framebuffer writeback wrote the Load Game heap colour image over the part cache's allocator header, and the next scene enter freed it

## Goal

Name the writer of the 16-bit white fill that session 169 found at
`0x80243E10` on the browser's mission route, and fix it so a clean browser build
reaches mission scene `0x03` without the `func_80071950` trap.

Timebox: 100 minutes (the round ran over; the fix landed and is verified).

## Result

1. **Session 169's naming is corrected: `D_8018FC88` is the sprite part cache,
   not a message queue.** Session 132 already established the layout (256
   `0x80`-byte records, presence bitmap at `+0x8000`, sheets at `+0x8200`/
   `+0x82A8`, code table at `+0x8350`). The functions on the faulting chain are
   the cache's lifecycle and the game allocator:
   - `func_8017909C` (`func_80179080` supplies `0x100`) allocates the block and
     writes `D_8018FC88`, `D_80190F68`/`D_80190F6C`/`D_80190F70` and the record
     array (`p[0x78] = 0x1FFF`, `p[0x7A] = 0` per `0x80`-byte record).
   - `func_801792DC` is the cache **release**: `D_8018FC88 = free(D_8018FC88)`,
     four instructions.
   - `func_800712C4` is the allocator's **free/coalesce** path, not a queue
     send. It tests `*(p-8) >= 0x21`, then calls `func_80071950(p-0x20)`, and
     `func_80071950` is a binary-tree node unlink that stores through
     `*(node+0x08)`, `*(node+0x0C)` and `*(node+0x10)`.
   So the words that make the trap are the **allocator block header** at
   `G-0x20 .. G-4` (`G = 0x80243E10`), not queue data: `+0x18` is the size the
   free reads as `*(p-8)`, and `+0x08`/`+0x0C`/`+0x10` are the tree links
   `func_80071950` stores through. `0xFFFDFFFD >= 0x21`, so the free takes the
   unlink arm and stores through `0xFFFDFFFD`.

2. **The flush that traps is not the mission enter's first call.** Native and
   browser both flush `0x801BA770` at the scene-`0x03` enter's first call
   (`func_80173724` calls `func_801792DC` then `func_80179080`), and that block's
   header is valid in both builds. The flush that carries the white header is
   the **next** scene enter's release at `0x80243E10`, after the `0x03` enter has
   re-initialised the cache inside that address.

3. **The writer is the browser renderer's framebuffer writeback:
   `readback_framebuffer` / `readback_all_framebuffers` in
   `app/src/web_renderer.cpp`, writing game framebuffer `0x228080`.** Tagged
   `probe170` output, browser, on the mission route:
   ```
   [probe170] writeback fb=0x228080 covers 0x243DF0: bytes 0x228080..0x24D880
              rows(wy)=[0,239] size=320x240 pitch=640
   ```
   fourteen times, from the scene-`0x12` enter (`[GFX-CIMG] SETCIMG
   addr=0x228080 ... rowBytes=640`, `[GFX-FB] framebuffer 0x228080: 320x240`)
   through the display lists that follow it. Guest `0x80228080` is physical
   `0x228080`; the image spans `0x228080..0x24D880`, so it contains
   `0x243DF0..0x243E10`, the part cache's block header. `0x228080` is a
   320x240 RGBA16 colour image in the game's heap, and the cache block the
   allocator hands out at `0x80243E10` lies inside it.

4. **The guest-side probe shows the fill, and the fix removes it.**
   `func_801792DC` dumped `G`, the eight words at `G-0x20`, eight words at
   `0x8004B400`, the count of `0xFFFD` halfwords in the whole `0x4B400`
   framebuffer, and the `0xFFFD` run in `G ±0x8000`.
   - Before the fix, at the second release (`G = 0x80243E10`):
     `hdr = 80243DD0 8024DA20 800AEDB8 00000000 00000000 00009C10 00000000
     FFFDFFFD`, `fd[0x8023BE10..0x8024BE0E] = 22891`, i.e. the whole ±`0x8000`
     window around the block is `0xFFFD` except the header words the allocator
     rewrote after the fill. `0xFFFD` is the writeback's RGBA5551 white
     (`R31 G31 B30 A1`), the same value session 169 saw.
   - Native at the same point: `hdr = 80243DD0 8024DA20 800AEDB8 00000000
     00000000 00009C10 00000000 B9D8B9D8`, zero `0xFFFD` anywhere in the
     window, and `0x8004B400` holds `0x00010001` (RGBA5551 black), so native
     never holds the pattern.
   - After the fix, the browser's late release reads
     `hdr = 80243DD0 8024DA20 800AEDB8 00000000 00000000 00009C10 00000000
     353212FF` — `G-4 = 0x353212FF` is exactly native's value at the same
     release — and there is **no** `0xFFFD` in the window
     (`fd[0x8023BE10..0x8024BE0E]` is absent, count 0). The renderer logs no
     writeback of `0x228080` at all.

5. **Why the browser wrote it and RT64 did not.** The browser's `ogre_gfx_flush`
   ran `readback_all_framebuffers()` on **every present** (`if (present ||
   sync_pending)`) and wrote back every framebuffer whose rows had been drawn,
   with the full image width, on the browser main thread. That thread is not
   ordered against the guest: the port posts the emulated RSP/DP completion as
   soon as the display list reaches the renderer, so the guest can re-initialise
   the heap while a writeback for an earlier frame is still queued. RT64 does
   this work inside `State::syncFramebuffers` (the game's own CPU-readback
   request, `ogre_sync_framebuffers`, `tools/njpeg_readback.py`) and at a
   workload's retirement, and it treats RDRAM as the source of truth for a
   colour image at the start of a workload (`FramebufferManager::checkRAM` /
   `uploadRAM`), so CPU writes made after a frame are never overwritten by that
   frame's stale pixels. The browser had no such ordering, so the stale pixels
   of `0x228080` landed on the live cache header. This is also why a
   timing-changing probe moves the fill: the extent it reached depended on how
   far the queued writeback had got.

6. **The njpeg/assembly angle is excluded on this route.** The assembly path
   does not run here: the route's browser log has no `M_NJPEGTASK` (RSP type 4)
   and no S2DEX2 display list (`ucode 0x800A5110`); every gfx task is
   `ucode 0x8009F540` (F3DEX2) and every other task is the audio microcode
   `0x8009E050`. `0x80243E10` being the New Game path's `'B5'` assembled-image
   header is the allocator's large-block address, not a shared buffer: the
   cache block (`0x100*0x80` + `0x200` + four ROM assets) and the assembled
   320x240 image both land on the first free block that fits. The `0xFFFD`
   value is pixel data, and the writer is the writeback above.

7. **Fix.** `ogre_gfx_flush` writes framebuffers back only when the game asks
   for them (`sync_pending`, i.e. `ogre_sync_framebuffers`), which is the one
   point where the guest is blocked on the renderer and the pixels it is about
   to read must be in RDRAM. The per-present writeback is gone. This is the
   reference renderer's rule: no framebuffer is written back on a present, and
   the game's readback request is what forces RDRAM. Nothing else changed;
   presentation reads the GL colour image, not RDRAM, so the canvas is
   unaffected.

## Evidence

- Writer probe (browser, before the fix): `/tmp/w170/m3-a6-page.txt` lines 589,
  666, 678, 680, 701, 706, 713, 716, 761, 765, 768, 771, 781, 784
  (`writeback fb=0x228080 covers 0x243DF0`), and the guest dump at line 837.
- Guest dump before the fix: `[probe170] g=0x80243E10 scene=FFFF hdr=... FFFDFFFD
  ... fd[0x8023BE10..0x8024BE0E]=22891` (`/tmp/w170/m3-a6-page.txt:837`).
- Native control: `/tmp/w170/nat.log` lines 1360 (`g=0x801BA770`, the `0x03`
  enter) and 1380 (`g=0x80243E10`, header valid, no `0xFFFD`).
- After the fix: `/tmp/w170/m4-a1-page.txt` (no `writeback fb=0x228080`, guest
  dump at line 760 has `hdr[7]=0x353212FF` and no `0xFFFD` run) and
  `/tmp/w170/m4-run.log` (`REACHED 0x03`, `nonBlack=303148`, `colorful=288300`).
- Framebuffer geometry: `[GFX-CIMG] SETCIMG addr=0x228080 fmt=0 siz=2 width=320
  rowBytes=640` and `[GFX-FB] framebuffer 0x228080: 320x240 ... pitch=640`
  (`/tmp/w170/m3-a6-page.txt`, `/tmp/w169/m-clean-a1-page.txt:609`).
- Part cache layout: session 132 (`docs/DECISIONS.md`), `func_8017909C` in
  `RecompiledFuncs/funcs_9.c`, `func_801792DC` in `RecompiledFuncs/funcs_14.c`.
- Allocator: `mips-linux-gnu-objdump -d build/ogrebattle64.elf
  --start-address=0x80071288 --stop-address=0x800713ec` (`func_80071288`,
  `func_800712C4`) and `--start-address=0x80071950` (`func_80071950`, the tree
  unlink that stores through `node+0x08`).
- RT64's rule: `tools/RT64/src/hle/rt64_state.cpp:659` (`syncFramebuffers`) and
  `:1671` (writeback at workload retirement), `rt64_framebuffer_manager.cpp:911`
  (`checkRAM`) and `:925` (`uploadRAM`).

## What changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `ogre_gfx_flush` writes framebuffers back only on `sync_pending`; the present-path writeback is removed, with the reason in the comment |
| `docs/proofs/web-mission-clean-build.png` | the browser mission frame from the clean build (left) beside native `t.800` (right), 2x each |
| `docs/HANDOFF-2026-10-18-session170.md` | this file |
| `PLAN.md` | item 8(s): the writer, the fix, the correction of session 169's naming |
| `docs/STATUS-LOG.md` | session 170 entry |
| `docs/README.md` | session 170 row |
| `docs/DECISIONS.md` | decision 170 |

No commit was made.

Probes were added and removed. Files that carried `probe170`:
`RecompiledFuncs/funcs_14.c` (`func_801792DC`: the block header, the `0x8004B400`
words, the `0xFFFD` counts) and `app/src/web_renderer.cpp`
(`readback_framebuffer`: whether a writeback covers `0x243DF0`). They were
reverted with `make recomp && make bank-recomp` and the renderer block deleted;
`grep -rn "probe170" RecompiledFuncs/ Bank*Funcs/ app/ debug/` is empty, and
`RecompiledFuncs/funcs_14.c` has no `#include <stdio.h>`.

## What was run for verification

- Native route with the guest probe: `OGRE_SAVE=assets/escort_suspend.bin
  OGRE_SAVE_RESET=1 OGRE_PREF_DIR=/tmp/w170/natpref OGRE_NO_AUDIO=1
  OGRE_SPEED=1 OGRE_SCENE=title OGRE_SCENE_LOG=1 OGRE_TAP_MS=1000
  OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6" OGRE_EXIT_AFTER_MS=25000
  ./build-dist/ogrebattle64 assets/ogre64.z64` (`/tmp/w170/nat.log`).
- Browser route with both probes, before the fix
  (`/tmp/w168/drive.cjs`'s URL, retry driver `/tmp/w170/drive170.cjs`,
  `/tmp/w170/m3*`): reached `0x03`, trapped nowhere (the probe slowed the guest
  enough that the fill did not reach the header), and logged the fourteen
  writebacks of `0x228080`.
- Browser route after the fix, same probes (`/tmp/w170/m4*`): `REACHED 0x03`,
  `nonBlack=303148`, `colorful=288300`, zero writebacks covering `0x243DF0`,
  zero `0xFFFD` in the window.
- `make recomp && make bank-recomp` (probe revert), `make web
  EM_CACHE=/Users/momo/dev/ogre/.emcache` clean,
  `cmake --build build-dist --target ogrebattle64` clean.
- `node debug/probes/boot.cjs --attempts 3 --secs 40` — `RENDERED`.
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` —
  exit 0, `fbtex hits=1 lines=1`.
- The browser route once more on the reverted, rebuilt build (no probes):
  `REACHED 0x03`, `[scene] t=48479ms id=0x0003`, shots at `t=89.2s`/`t=101.2s`
  with `nonBlack=303148`, `colorful=287804`, and **no** `pageerror` /
  `memory access out of bounds` in the page log (`/tmp/w170/m5*`). Session 169's
  clean build trapped on this route; this one does not.
- `docs/proofs/web-mission-clean-build.png` is that canvas beside the native
  `t.800` (`tools/web_frame_cmp.py`: meanAbs `R=23.55 G=15.12 B=7.88`,
  meanSigned `R=-6.60 G=12.74 B=-4.22`, `exact=0.3091`). Session 169's
  probe-timing frame measured `23.54/14.96/7.86` and `0.3094`, so the fix leaves
  the frame unchanged; the remaining differences are the unimplemented lighting
  (916 448 lit draws), depth (516 463) and culling (926 539).

There is no repo test harness; each check is a ROM-dependent game run.

## Open

1. **The Load Game framebuffer's content differs from native**: the browser
   renders `0x228080` with `0xFFFD` white pixels where RT64's image is dark
   (`0xB9D8`-like). That was the *value* the writeback carried; the writeback
   was the mechanism. Rendering the Load Game screen white is a separate
   difference and is not investigated here.
2. **The browser still has no ordering between the guest and the renderer's
   writeback.** Removing the present-path writeback removes the only writer that
   ran without the game asking, so the corruption cannot happen; making the
   per-workload writeback ordered (RT64's `uploadRAM`/`checkRAM` model, or the
   DP completion posted after the writeback) is the general repair.
3. **Depth, lighting and culling remain unimplemented** (unchanged), so the
   mission frame still differs from native `/tmp/w168/nat2/t.800.bin` by the
   named three (`meanAbs 23.55/15.12/7.88`).
4. `OGRE_SCENE=title` still races the boot's first scene on roughly half of the
   browser boots. The retry driver used here aborts an attempt when scene
   `0x0009` (the boot intro) starts, which keeps a route run to about 35 s.
