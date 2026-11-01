# Session 206 handoff — the logo-to-title noise is a present of a blanked VI

## Goal

The developer reported that when the publisher logos end, a few frames of noise
appear before the title screen, and that the native renderer does not show them.
Find the native behaviour the browser renderer is missing there, and fix it.

## Result

The noise is the title's new 320x476 colour image presented with the *publisher*
mode's 640x476 visible area while the VI is blanked. `update_screen` requested a
present on every VI update, and `present_framebuffer` sized that present from
`vi_size()`, the last *visible* mode, not from the VI registers that produced the
request. `framebuffer_containing()` resolved the blanked VI's origin
(`0x000680`, the title's buffer `0x000400` plus 0x280) to the title's new
framebuffer, and the blit sampled its rows 240..320 into a 639x475 canvas. The
title never draws those rows, and they held whatever RDRAM had there, converted
as RGBA5551. That is the band.

RT64 does the opposite in two places:

- `State::updateScreen` computes `viVisible = newVI.visible()` and only copies a
  framebuffer, and only sizes a present, when that is true
  (`tools/RT64/src/hle/rt64_state.cpp:2065`, `:2092`).
- `PresentQueue` has no colour target for a blanked VI and clears the swap chain
  to black (`tools/RT64/src/hle/rt64_present_queue.cpp`, the
  `colorTarget == nullptr` arm calls `commandList->clearColor()`).
  `app/src/renderer.cpp`'s `OGRE_VI_TRACE` comment states the same rule: the
  native presenter only draws when the VI is visible and has a nonzero size.

The fix makes the present carry the VI state that produced it:

- `request_present` takes the visible flag and the `fbSize` computed from the
  same `ViRegs`, and `flush_commands` copies them with the address.
- `present_framebuffer(address, vi_ok, vi_w, vi_h)` uses that size and resizes
  the canvas from it. A blanked VI, or one with no usable size, calls
  `clear_canvas_black()`: the canvas keeps its size and is cleared, which is
  RT64's `colorTarget == nullptr` present. The address is not resolved and no
  framebuffer is sampled.
- `send_dummy_workload` passes the last visible size, or black when there is
  none; the boot scratch buffer is black either way.

## Evidence

### The artifact

The attached recording (`Screen Recording 2026-10-31 at 21.19.17.mov`) shows the
QUEST still fade to black, then a full-width noise band at rows 240..320 of a
639x475 canvas for about 0.7 s, then the title's clouds fade in.

The headless capture reproduces it with the existing probe, no new probe needed:

```
node debug/probes/intro-capture.cjs --seconds 80 --interval-ms 250 \
  --out /tmp/w206/intro --attempts 2 \
  --url 'http://127.0.0.1:8931/app/web/index.html?OGRE_NO_AUDIO=1'
```

Frames 105-107 (the first after the `[scene] id=0x0004` line) are 639x475 with
`nonBlack=2499 colourful=2091`, byte-identical to each other; frame 104 is
`nonBlack=0`, and frame 109 is the full title. The band is
`docs/proofs/web-title-transition-noise-before-session206.png`.

### The VI is blanked, and its origin already points at the title

A temporary `probe206` printed the VI registers on every mode change (the
`[GFX-VI]` milestone only prints *visible* size changes, so it hid this):

```
[probe206] vi origin=0x250A80 status=0x0000311E width=320 h=0x00000000 v=0x002501FF xs=0x00000200 ys=0x00000400 visible=0 size=320x240
[probe206] vi origin=0x000680 status=0x00013012 width=320 h=0x00000000 v=0x002501FF xs=0x00000200 ys=0x00000400 visible=0 size=320x240
```

`hStart = 0` makes both invisible (`vi_visible`, RT64 `VI::visible`). The second
origin is the title's buffer, `0x000400 + 0x280`, written before the mode is
unblanked. The first *visible* title mode follows after the scene line and
resizes the canvas to 319x239.

The title's colour image is `[GFX-FB] framebuffer 0x000400: 320x476`, because
`target_height_px` takes the tallest of the viewport, the VI height and the
scissor, and the last visible VI is still the publisher's 640x476. The title's
fog draws only reach row 240 (`[GFX-FOG-DRAW] rect=(0,239)-(320,240)` is the last
one).

So the old presenter took address `0x000680` (title image, 320x476), the visible
area of the publisher mode (639x475), and the containing framebuffer `0x000400`,
and blitted the whole 320x476 texture over a 639x475 canvas. Rows 240..320 are
the un-drawn part, and `upload_framebuffer` had filled them from RDRAM at
creation, where they are not image bytes.

### Before and after

The same route at the same point:

- before: `docs/proofs/web-title-transition-noise-before-session206.png`
  (639x475, the noise band);
- after: `docs/proofs/web-title-transition-black-after-session206.png`
  (639x475, black) and
  `docs/proofs/web-title-transition-title-after-session206.png` (the title's
  clouds at 319x239 once the visible mode arrives).

The after capture reads `nonBlack=0 colourful=0` for every frame before the
canvas resizes to 319x239, then the title's fade-in (`nonBlack` 10430 -> 15004
-> 16473 -> ... -> 17990), the same sequence the title showed after the noise in
the before run.

## Verification

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean after the probe was
  removed.
- The title transition on the final build: the manifest's scene timeline is
  `0x0009` at 7.1 s, `0x000A` at 26.4 s, `0x0004` at 43.0 s. Every 639x475 frame
  from the title line to the visible mode reads `nonBlack=0 colourful=0`; the
  canvas then resizes to 319x239 and the title fades in (`nonBlack` 10430,
  15004, 16473, ... 17990).
- The intro and publishers still render on the final build. A scene-armed
  capture from `[scene] id=0x0009` reaches the intro's frames (319x239,
  `nonBlack` 1162-4254) and the QUEST still (`0x000A`, 639x475,
  `nonBlack=19126 colourful=1307`), the same two counts the pre-fix series read
  for that still. The traces show `visible=1` throughout those scenes, so the
  present uses the same size it did before; the only behavioural change is the
  blank/no-size case.
- One diagnostic contract needed repair with the fix. The first version returned
  from `present_framebuffer` before the `[GFX-PRESENT]` line, so a boot whose
  first presents are blanked never logged `(present 1)`, and
  `debug/probes/intro-capture.cjs` waits for exactly that line and timed out on
  three attempts. The log now runs before the blank branch. The intro/publisher
  check above uses a scene-armed capture from the same probe library instead of
  the `(present 1)` wait.
- `grep -rn "probe206" app/` is empty.
- The native reference is `tools/RT64/src/hle/rt64_present_queue.cpp` and
  `rt64_state.cpp`; no native run was needed, and none was taken.

## Files changed

| file | change |
|---|---|
| `app/src/web_renderer.cpp` | `request_present` carries the VI visible flag and size; `present_framebuffer` takes them, resizes from them, and clears the canvas black for a blank or sizeless VI (`clear_canvas_black`); `send_dummy_workload` passes the last visible size or black |
| `docs/proofs/web-title-transition-noise-before-session206.png` | the noise band at title entry |
| `docs/proofs/web-title-transition-black-after-session206.png` | the same point after the fix |
| `docs/proofs/web-title-transition-title-after-session206.png` | the title after the visible mode arrives |
| `docs/HANDOFF-2026-10-31-session206.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, `docs/README.md`, `docs/scenes.md` | status, log, decision, index, title row |
| `docs/guides/web-probes.md` | the `intro-capture.cjs` `(present 1)` caveat under host load, and the scene-armed alternative |

The working tree already carried sessions 201-205's uncommitted changes when this
session started (`app/web/`, `app/src/web_platform.cpp`, `docs/HANDOFF-2026-10-2*`,
`tools/RT64` and others). This session did not modify or revert them.

## Probes used and reverted

- `probe206` (`[probe206] vi ...` and `[probe206] mismatch present ...`) was a
  temporary `fprintf` block in `update_screen` and `present_framebuffer`, gated on
  `OGRE_VI_TRACE` (the native renderer's existing knob). It named the blanked VI
  and the origin/buffer pair. Removed, `make web` rebuilt, and
  `grep -rn "probe206" app/` is empty.
- The capture probe is the existing `debug/probes/intro-capture.cjs`; no probe
  was added to the repository.

## Open

The rest of the present pipeline, compared with RT64 (`rt64_present_queue.cpp`,
`rt64_state.cpp`) and not ported:

1. **A present whose last write was depth is converted to colour natively**
   (`isLastWriteDifferent` + `copyFromTarget`). The browser has no depth
   writeback and presents the colour target. Reachability is unmeasured.
2. **A presented framebuffer's CPU-written bytes are uploaded at present time
   natively** (the `uploadRAM`/`RAMHash` arm of `updateScreen`). The browser
   uploads a colour image only when it is created, and defers its own writeback
   to `ogre_sync_framebuffers()`. This is the same family as the per-workload
   writeback gap (sessions 193-197).
3. **A new colour image is cleared natively and receives only tracked CPU
   changes**; the browser uploads the whole RDRAM range at creation. The title's
   un-drawn rows 240..320 are that difference. It is hidden now because a blanked
   VI no longer presents those rows, but a *visible* mode that is taller than the
   drawn area would still show them.
4. **The browser presents on every VI update**; RT64 pushes a present only when
   the VI changed or the framebuffer changed. Not a visual difference observed so
   far.
