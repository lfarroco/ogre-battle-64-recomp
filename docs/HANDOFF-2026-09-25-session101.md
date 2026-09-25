# Handoff — 2026-09-25, session 101: the unit profile's DEF/MDEF "extra digit" was RT64's rect/scissor snap

**Goal (developer):** *"when viewing a units profile, the DEF and MDEF show some
'artifacts' to the right of the numbers, making it look like an additional cutoff
number. In the example screenshot, we have '11' def, with some dots to the right,
appearing to form a '2'."* Follow-up: *"the numbers are 28 and 88. there's some
additional black pixels to the right of them, making it look like they are some
cropped 9 (but only 28 and 88 are the correct stats)"*. The developer supplied a
**retail cutout** of the same panel (`26` / `84`) with no such pixels, which
settled the port-versus-retail question.

**Result:** **fixed.** The extra pixels were **RT64's "Fix LR with Scissor"
enhancement** snapping the right edge of the rightmost digit's `G_TEXRECT` to the
panel scissor's right edge. The game's rectangle is `ulx=1152 lrx=1176` (pixels
288..294, 6 px); the panel scissor is `(100,84)-(1180,612)`, so the gap is exactly
4 subpixels (one pixel) and RT64 rewrote `lrx` to `1180`, making the rectangle
7 px. Its seventh pixel samples texel 54 of the font sheet, which is the **first
column of the next cell** (`8` → `9`, `1` → `2`). The app now clears
`enhancementConfig.rect.fixRectLR` after RT64's setup, with
`OGRE_RECT_LR_FIX=1` to restore RT64's default for an A/B. The developer confirms
the screen is fixed.

The game and its display list were never wrong. No generated file changed.

---

## 1. The screen and the display list

The screen is the **Organize Screen**, scene `0x06` (descriptor `0x8018FD84`,
mask `0x00000002`). The frame is one display list, `data=0x80257E50`, F3DEX2
(`ucode=0x8009F540`, the ucode session 64 hashed into RT64's `F3DEX2.fifo 2.08`
entry). Every sampled `[renderer] display list` line in scene `0x06` carries that
address, so a dump holds the whole frame.

Read out of `/tmp/ogre-profile.bin` (and confirmed in every later dump):

* The DEF value is drawn as **one 6x7-px rectangle per digit, right to left**:
  `28` is `(288,66)-(294,73)` with `s=48.0` and `(281,66)-(287,73)` with
  `s=12.0`; the MDEF row repeats the pair at `y=87`. `dsdx=dtdy=1024` = one texel
  per pixel.
* The font sheet is in the same image at guest `0x801FFCF8` (64x64 CI4, loaded by
  `LOADBLOCK` with `dxt=512`, drawn through tile 0 as CI4, line 4). Its first row
  is `0123456789` in **6-texel cells**, so `s=12` is cell 2 (`2`) and `s=48` is
  cell 8 (`8`).
* The panel's scissor is set at `0x80259908` to `(100,84)-(1180,612)`, i.e.
  pixels `(25,21)-(295,153)`. The rightmost digit rectangle's `lrx` (`1176`) is
  exactly four subpixels from the scissor's `lrx` (`1180`).

## 2. The artifact is the next font cell's first column

The glyph `8` occupies `x=288..292`; `x=293` is its sixth (background) texel;
`x=294` is dark at rows 66, 67, 68, 69 and 71 and bright at rows 70 and 72. Cell
9 (`9`) column 0 is `0,0,0,0,E,0,E` — ink at rows 0-3 and 5, background at rows 4
and 6. The patterns are identical. The same holds for the MDEF row, and for the
developer's `11` case, where the column after cell 1 (`1`) is cell 2's column 0.

The identical call for the stat numbers (`VIT: 158`, `(110,64)-(116,71)` s=48,
same tile) leaves the column beside it clean in the same frame, because it is far
from the scissor edge.

## 3. Which draw, and which edge

`OGRE_NOP_RECT` experiments on the submitted display list:

* `x:294-293` (the four panel-border pieces; the inverted range is the way to
  select a rectangle without also matching every full-width one) — the developer
  reports the artifact stays. The first spelling tried, `x:294-300`, matched
  **171 rectangles per frame** by overlap and blacked out the title screen's
  background; that damage was the test selector, not the build.
* `n:43` — the walker's index 43 is the `(288,66)-(294,73)` rectangle
  (`walked=195 removed=1` on every frame of scene `0x06`), and the dump shows
  `x=288..294` blank in the DEF row. So that rectangle drew the column.

`OGRE_RECT_PROBE` (a temporary RT64 probe, `probe101`, now reverted) printed the
draw call's own rectangle:

```
px 288,66..294,73  raw 1152,264..1180,292  cycle=0  dsdx=1024  tile=0  t.lrs=252  vp w=7   <- with the enhancement on
px 288,66..294,73  raw 1152,264..1176,292  cycle=0  dsdx=1024  tile=0  t.lrs=252  vp w=6   <- with it off
```

`cycle=0` rules out the copy-mode adjustment (`lrx |= 3`, `dsdx >>= 2`, which
would print `dsdx=256`), and `t.lrs=252` (63 texels) rules out a tile-window
limit. The display list word in the *same dump* is `ulx=1152 lrx=1176`.

## 4. The mechanism, at instruction level

`tools/RT64/src/hle/rt64_rdp.cpp`, `RDP::drawRect`:

```cpp
const FixedRect &scissorRect = state->rdp->scissorRectStack[scissorStackSize - 1];
const bool fixRectLR = state->ext.enhancementConfig->rect.fixRectLR;   // default true
if (!scissorRect.isNull() && fixRectLR) {
    if ((abs(scissorRect.lrx - lrx) <= 4) && (ulx < scissorRect.lrx)) {
        lrx = scissorRect.lrx;
    }
    ...
}
```

RT64's comment above it says the enhancement exists because *"there's a very
common error in many games where rectangles are incorrectly configured with one
less pixel than what's required to fill out the screen"*. For this rectangle
`abs(1180 - 1176) == 4 <= 4` and `1152 < 1180`, so `lrx` becomes `1180`; the
rectangle grows from six pixels to seven and its last pixel samples one texel
past the glyph cell.

`fixRectLR` is `EnhancementConfiguration::Rect::fixRectLR`
(`rt64_enhancement_configuration.cpp:14`, default `true`), exposed in RT64's own
settings UI as *"Fix LR with Scissor"*. The app never loaded an RT64 configuration
file (`app_config.useConfigurationFile = false`, `app/src/renderer.cpp`), so
every run started with RT64's default.

The reference emulator is clean because it has no such enhancement: parallel-rdp
converts the same `G_TEXRECT` to `x/width` in pixels and stores
`lrx = x + width - 4` in 1/4-px units (`CommandBuilder::tex_rect`), and rasterizes
the span with the RDP's own snapping (`span_setup.comp`: `(x >> 12) | sticky` per
edge), so it covers the six pixels the display list asks for. The developer's
suggestion to check the parallel plugin is what identified the difference: the
fault was not in the sampler but in an enhancement that rewrites the rectangle
before it is drawn.

## 5. The fix

`app/src/renderer.cpp`, after `app_->setup(...)` (it must be after `setup`,
because `updateEnhancementConfig()` writes through the shared queue resources
that `setup` creates — putting it before `setup` segfaults at boot):

```cpp
const char* lr_fix = getenv("OGRE_RECT_LR_FIX");
app_->enhancementConfig.rect.fixRectLR = (lr_fix != nullptr) && (lr_fix[0] != '0');
app_->updateEnhancementConfig();
```

Off by default, `OGRE_RECT_LR_FIX=1` restores RT64's behaviour for an A/B. The
build logs `rect/scissor snap (fixRectLR) = 0`.

**Verification (fixed build, same route):**

* The probe reports `raw 1152,264..1176,292`, `vp w=6` for both equipment rows.
* Three dumps taken on the screen (`/tmp/ogre-fix-03/06/08.bin`, all three VI
  framebuffers in each) show column 294 as panel border on every glyph row; the
  pre-fix dumps show it as font ink there.
* **Developer-confirmed:** *"its fixed!!"*

The enhancement is a widescreen/under-draw hack, not emulation, so turning it off
restores the rectangles the game authors wrote. Session 81's right/bottom stale
edge is handled separately (`framebufferHeightForDisplay`, `visibleFramebufferSize`,
`OGRE_OVERSCAN`), so this change does not reopen it.

## 6. Tools added this session

* **`snap [prefix] [ms]`** (`app/src/sdl_platform.cpp`, `sdl_platform.hpp`,
  `main.cpp`): one console command that writes the whole 8 MiB RDRAM image with
  the game threads parked and turns RT64's presented-frame capture on for `ms`
  (default 400) so `/tmp/ogre-shot.<present>.ppm` files land for the same screen.
  The toggle installs one static `OGRE_CAPTURE_PRESENT` entry with `putenv` at
  boot (`console::init_capture_env`, called from `main` before the runtime starts
  its threads) and enables it by rewriting the **first byte of the entry's name**;
  `setenv`/`unsetenv` would free the value string the present thread holds and
  dereferences later in the same present.
* The console `dump` command, driven from the shell every 5 s, is what produced
  the correlated display-list-plus-framebuffer images used here. `snap` adds the
  presented frames on top.
* `docs/guides/app-build.md` records `snap` and `OGRE_RECT_LR_FIX`.

**The checkpoint instrument does not work at this screen.** `save`/`load` restore
(`scene 0x0009 -> scene 0x0006`) and the process then dies with SIGSEGV on N64
thread 19 (`build-app/error.log`). Session 58 validated checkpoints at the
cathedral.

## 7. Files changed

* `app/src/renderer.cpp` — clear `enhancementConfig.rect.fixRectLR` after
  `setup`, gated by `OGRE_RECT_LR_FIX`.
* `app/src/sdl_platform.cpp` — the `snap` command, the capture toggle, the shared
  `write_rdram_image` helper, the capture countdown, the `help` text.
* `app/src/sdl_platform.hpp` — `console::init_capture_env`.
* `app/src/main.cpp` — the call to `console::init_capture_env()` before the
  runtime starts.
* `docs/guides/app-build.md` — the `snap` row and `OGRE_RECT_LR_FIX`.
* `docs/HANDOFF-2026-09-25-session101.md` (this file), `PLAN.md`,
  `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md`.

**Probe revert (AGENTS §6):** the only probe was `probe101` in
`tools/RT64/src/render/rt64_framebuffer_renderer.cpp`.
`grep -rn "probe101\|OGRE_RECT_PROBE\|rect-probe" tools/RT64/src app/src` is
empty, and `git -C tools/RT64 diff` is byte-identical to
`patches/rt64-ob64.patch` apart from the pre-existing `src/contrib/plume`
dirty-submodule line. No `RecompiledFuncs/`, `Bank*Funcs/` or `config/` change.

## 8. Evidence files (outside the tree)

| file | what |
|---|---|
| `ogre-profile.bin` | first dump on the screen, artifact present (VI framebuffer `0x400`) |
| `ogre-auto-04.bin`, `-14.bin` | later dumps on the screen, artifact present |
| `ogre-t2.bin` | `OGRE_NOP_RECT=n:43` run; the DEF `8` is blank and its column is gone |
| `ogre-fix-03/06/08.bin` | fixed build on the screen; column 294 is border |
| `ogre-*.log` | run logs: scene timeline, `[vi]`, probe lines, console commands |
| `ogre-shot.*.ppm` | RT64 present readbacks |

Throwaway analysis scripts are outside the tree (`/tmp/dl_decode.py`,
`/tmp/dl_decode2.py`, `/tmp/dl_rec.py`, `/tmp/font*.py`, `/tmp/fb_*.py`,
`/tmp/ppm_crop.py`, `/tmp/ppm2png.py`, `/tmp/same.py`).
