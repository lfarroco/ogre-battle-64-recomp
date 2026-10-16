# Session 153 handoff — a browser gamepad now drives N64 controller 1, and the keyboard works beside it

## Goal

Continue the web renderer (developer, session start). Session 152's open item 4
named the outstanding browser items: framebuffer-as-texture (instrumented, no
screen needs it), the `fldRegs` x/y scale and offset (no browser change
outstanding), the canvas's unwritten edge and rounded corners (cosmetic), the
LOADTLUT `tmem` key (latent), the title's moving-cloud bands, and the screens not
compared against the reference. `PLAN.md` open item 13 was separate and concrete:
the browser build assigned gamepads to N64 controllers 2-4, so a pad could not
reach controller 1 in game, and it needed a wasm build and a browser run to
verify. This session took item 13 because it is the only browser item with a
known defect and a bounded fix, and it is input plumbing the renderer work needs
anyway (a route can only be driven by keyboard or by the scripted tap schedule
today).

## Result

1. **A pad takes the lowest free slot, so the first pad is N64 controller 1.**
   `app/web/web.js` assigned `slot = (pad.index % 3) + 1`, which put every pad in
   slots 1..3 and left slot 0 to the keyboard alone. The native rule is the
   first-free-slot fill (`app/src/sdl_platform.cpp`, the enumeration in
   `pump_sdl_events`: "A pad fills the first free slot, starting at 0"), and
   `get_input` ORs the keyboard into it: `out_buttons |= keyboard_buttons()` for
   `controller_num == 0`, then `out_buttons |= gamecontroller_buttons(controller)`
   for the pad in that same slot. `updateGamepads` now assigns slots 0..3 in that
   order and ORs slot 0's keyboard and tap mask into the pad's buttons
   (`st.buttons |= slot0Mask`), so one pad is controller 1 and the keyboard still
   drives it. A slot that loses its pad goes idle, and slot 0 goes back to the
   keyboard mask.

2. **The pass order was the second half of the defect, and the probe found it.**
   `rafLoop` pushed the keyboard first (`pushInput(0, keyboardButtons() |
   tapMask, ...)`) and then ran `updateGamepads`, which wrote slot 0 again. With
   a pad present the pad's write landed last every frame, so the keyboard's mask
   was overwritten and the keyboard went dead. The synthetic-pad probe measured
   it: with the pad holding `A` and the probe holding `KeyZ`, slot 0 read
   `0x8000` (the pad only) before the reorder and `0xC000` (`A | B`) after it.
   `rafLoop` now computes the slot-0 mask and passes it to `updateGamepads`,
   which runs the pad pass after it, and the pad path ORs the mask in.

3. **`ogre_input_debug` reports one slot, and an empty slot is distinguishable.**
   It returned slot 0's mask only, so a test could not read the slot a pad had
   landed in. It now takes `int slot`, replies `0xFFFF` when that slot has no
   connected device, and the connected idle keyboard still reports `0`. The
   header comment in `app/src/web_platform.cpp` said "slot 0 is the keyboard
   (always connected), slots 1-3 are gamepads", which described the old model; it
   is corrected to the native rule.

4. **New kept probe `debug/probes/gamepad.cjs` makes the check reproducible.**
   A headless browser exposes no real pad, so the probe installs a synthetic
   `standard`-mapping pad holding `A` with `page.addInitScript` before the page
   scripts run (`navigator.getGamepads` returns `[pad, null, null, null]`; no
   input event is synthesized). It then reads all four slots every second, and
   afterwards holds the keyboard's `KeyZ` beside the pad's `A` and requires
   `0xC000`, then releases it and requires `0x8000`. The probe exits 0 only when
   both hold. On the fixed build:

   ```
   input export=true slots(last)=[32768,65535,65535,65535]
   slot0 carried A in some sample: true; in the last sample: true
   keyboard B while the pad holds A: slot0=0xc000 (want 0xc000), after release=0x8000 (want 0x8000)
     [web:input] gamepad connected (pad 0 -> slot 0, slot 1 free, slot 2 free, slot 3 free)
   ```

   On the pre-fix build the same probe printed `[web:input] gamepad connected
   (slots 1-3)` and its last sample was `[0,0,0,0]` (the old helper read slot 0
   only, and slot 0 was an idle keyboard with no pad in it). The connect line is
   logged once per 5 s, so the probe samples a fixed window rather than stopping
   at the first good sample.

5. **The slot-0 rules for the other paths are unchanged.** The scripted tap
   schedule still writes slot 0 (`tapMask` is part of `slot0Mask`), the keyboard
   still writes slot 0, and with no pad the `assigned[0] === null` branch pushes
   the keyboard mask so a tap route keeps working. A 45 s run of
   `OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a&OGRE_SCENE_LOG=1`
   reaches `[scene] t=14393ms id=0x0009` (the intro), and `boot.cjs` and
   `testdraw.cjs --flags 32 --expect-fbtex` pass, so no renderer or input path
   moved.

6. **What is not verified.** No physical pad was tested on a page in a real
   browser; the check is a synthetic pad object in headless Chromium. Analog
   axes are passed through unchanged (`gamepadState`), but nothing measures them.
   The one-pad case is covered; 2-4 pads fill slots 1..3, which this session did
   not exercise.

## Files changed

| file | change |
|---|---|
| `app/web/web.js` | `updateGamepads(now, slot0Mask)` fills the lowest free slot and ORs slot 0's keyboard/tap mask into the pad's buttons; a slot that loses its pad goes idle and slot 0 returns to the keyboard; the connect line names each slot's pad; `rafLoop` computes the slot-0 mask and runs the pad pass after it |
| `app/src/web_platform.cpp` | `ogre_input_debug(int slot)` reads any slot and replies `0xFFFF` for a disconnected one; the file header and the helper comment state the native slot rule |
| `app/src/web_platform.hpp` | the `ogre_input_debug` declaration and comment |
| `debug/probes/gamepad.cjs` | new probe: a synthetic pad through `addInitScript`, the four slot masks, and the keyboard check |
| `debug/package.json` | `npm run gamepad` |
| `docs/guides/web-probes.md` | the `gamepad.cjs` row, the run line, and "The browser gamepad check" |
| `docs/guides/app-build.md` | the browser input paragraph records the pad slot rule and the pass order |
| `PLAN.md` | the session-153 status paragraph, and open item 13 closed |
| `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, `docs/README.md` | decision row, log entry, index |
| `docs/HANDOFF-2026-10-16-session153.md` | this file |

No probe was added to generated code and nothing under `RecompiledFuncs/`,
`Bank*Funcs/` or `RspFuncs/` was touched, so there is no probe to revert and no
`probe153` tag anywhere. `tools/RT64` still carries session 150's fill clip
uncommitted; this session did not change it.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` (three times: the baseline,
  after the slot fix, and after the pass-order fix), each clean.
- `node --check app/web/web.js` after each edit.
- `node debug/probes/gamepad.cjs --secs 20 --out /tmp/ogre-gp-before` on the
  baseline build (the old `ogre_input_debug`, slot 0 only) -> `slots=[0,0,0,0]`,
  `slot0 carried A ... false`, `[web:input] gamepad connected (slots 1-3)`,
  `FAIL`.
- `node debug/probes/gamepad.cjs --secs 20 --out /tmp/ogre-gp-after2` (slot fix
  only) -> `slots=[0x8000,0xFFFF,0xFFFF,0xFFFF]`,
  `[web:input] gamepad connected (pad 0 -> slot 0, ...)`.
- `node debug/probes/gamepad.cjs --secs 14 --out /tmp/ogre-gp-after3` (slot fix
  only) -> `keyboard B while the pad holds A: slot0=0x8000 (want 0xc000)`,
  which located the pass-order defect.
- `node debug/probes/gamepad.cjs --secs 14 --out /tmp/ogre-gp-after4` (final
  build) -> `slots=[0x8000,0xFFFF,0xFFFF,0xFFFF]`,
  `slot0=0xc000 ... after release=0x8000`, exit 0.
- `node debug/probes/boot.cjs --attempts 2 --secs 40` -> `RENDERED`
  (`nonBlack 25648 / colorful 21740`, `sawRealDl=true`).
- `node debug/probes/testdraw.cjs --flags 32 --expect-fbtex` -> `fbtex hits=1
  lines=1` and the session-152 `[GFX-FBTEX]` line for `tex=0xC02000`.
- A tap route on the final build (a one-off script with the harness, 45 s):
  `OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a&OGRE_SCENE_LOG=1`
  -> `[scene] t=14393ms id=0x0009`, `fbtex: hits=0`.
- A 4-trial matrix (pad x pre-`evaluate` x wait time) that isolated the
  pass-order defect: `pad, no pre-eval` and `pad, pre-eval` both read
  `0x8000` while `nopad` read `0x4000` from the same `KeyZ` press.

There is no repo test harness; every check above is a ROM-dependent browser run
of 20-60 s, and the tap route was a throwaway script (not kept).

## Open

1. A physical gamepad on a real page is unverified. The probe proves the JS
   assignment and the wasm atomics; it cannot prove a real `Gamepad` object
   arrives from the browser's HID layer.
2. Two to four pads fill slots 1..3; no check covers that.
3. Session 152's open items stand unchanged: framebuffer-as-texture
   (instrumented, no screen needs it), the native cathedral route
   (`OGRE_SCENE=new-game OGRE_STEP=2` plays `0x09 -> 0x0A -> 0x04` natively and
   reaches `0x0D` in the browser), the `[GFX-FBTEX]` row test's column
   over-report, the canvas's rounded corners and unwritten last column/row, the
   LOADTLUT `tmem` key, the `fldRegs` x/y scale and offset, the title's
   moving-cloud bands, and the screens not compared against the reference.
