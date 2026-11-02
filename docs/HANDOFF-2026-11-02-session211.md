# Session 211 handoff — the left stick's Y is no longer inverted

## Goal

Developer request: investigate issue #21 ("Left stick on Xbox controller
inverted. Up goes down and down goes up") and, if it is real, fix it, build both
targets and write the session up.

## Result

The report is real and the fault is in the port's input boundary, on both
builds. The left stick's Y is now negated on its way into the N64 pad.

- Native: `app/src/sdl_platform.cpp`, `get_input` in the `controller != nullptr`
  branch. `*y` is now the negated `SDL_GameControllerGetAxis(controller,
  SDL_CONTROLLER_AXIS_LEFTY)` value times `axis_scale`.
- Browser: `app/web/web.js`, `gamepadState`. `y` is now
  `-Math.max(-1, Math.min(1, axes[1]))`.

Nothing else changed. The right-stick C layer already used SDL's convention
(`RIGHTY < -8000` is C-UP). The console's synthetic `press <buttons> [polls] [x]
[y]` sets the stick after the SDL read, so its Y keeps the N64 positive-up sign.
`docs/guides/app-build.md` now records the convention in the controller section.

No invert-Y option was added. The correct mapping removes the need for one, and
the CONTROLS tab does not expose the left stick.

## Evidence

The sign chain was read end to end before any edit.

1. **SDL defines the axis as negative up.** In
   `tools/SDL2-static/SDL2-2.32.10/include/SDL_gamecontroller.h` (line 722):
   "For thumbsticks, the state is a value ranging from -32768 (up/left) to 32767
   (down/right)." The native branch multiplied that value by `1.0f / 32768.0f`
   and passed it on unchanged, so physical up produced `y = -1.0`.
2. **The runtime treats positive Y as up.**
   `tools/N64ModernRuntime/ultramodern/src/input.cpp:142-161` computes
   `angle = atan2f(y, x)` and `stick_y = sinf(angle) * output_magnitude`, so
   `y = -1.0` produced `stick_y = -82` (the conversion's full radius at that
   angle is 82).
3. **The game treats positive `stick_y` as up.** In `build/bankM.elf`,
   `func_ovlM_8019AF0C` reads the pad pointer at `0x8019B438`
   (`lw v0, 0x47F0(v0)`), reads `stick_y` at `0x8019B43C` (`lbu v1, 3(v0)`),
   sign-extends it, shifts by 3 at `0x8019B48C` (`sra v0, a1, 0x3`), and
   subtracts it from the cursor's screen y at `0x8019B490`
   (`subu a1, v1, v0`). Screen y grows downward, so positive `stick_y` moves the
   cursor up. A negative value moved it down, which is the reported symptom.

X is correct and was not changed: the same function adds `stick_x / 8` to screen
x at `0x8019B474` (`addu a0, v0, v1`), and SDL's positive X is right.

The browser had the same fault: the Gamepad API's standard mapping is also
negative up on axis 1, and `gamepadState` passed `axes[1]` through unchanged.
The web shell feeds that value to `ogre_input_set` → `web_platform.cpp`
`get_input` → the same `convert_to_n64_range`.

GitHub's commit list for `app/src/sdl_platform.cpp` shows the newest commit at
2026-10-13, before the issue was filed on 2026-10-15, so upstream still had the
fault when this session started.

## Verification

- `make app` rebuilt `app/src/sdl_platform.cpp` and linked
  `build-dist/ogrebattle64`; clean apart from the pre-existing
  `renderer.cpp:1052` `-Wxor-used-as-pow` warning and the duplicate SDL2 archive
  warning. `make app` uses `build-dist` because `DIST_STATIC_SDL` defaults to 1.
- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` completed and
  `build-wasm/ogrebattle64.wasm` is present. `sdl_platform.cpp` is native-only,
  so the wasm did not relink; the changed browser file is page JavaScript, which
  `node --check app/web/web.js` accepts.
- The edit was re-read after writing, and both sites carry the negation.

No controller was attached to this machine, so the fix was not confirmed by a
physical press. The mapping is established by the SDL header, the runtime's
conversion and the game's own arithmetic at `0x8019B490`. A hardware press on
the world map is the remaining check: up should decrease the cursor's screen y.

## Files changed

- `app/src/sdl_platform.cpp` (left-stick Y sign and a comment naming the game
  side)
- `app/web/web.js` (left-stick Y sign, same convention)
- `docs/guides/app-build.md` (the controller section records the sign)
- `PLAN.md` (Status: an Input bullet)
- `docs/STATUS-LOG.md` (session 211 entry)
- `docs/DECISIONS.md` (session 211 row)
- `docs/README.md` (handoff index row)
- `docs/HANDOFF-2026-11-02-session211.md` (this file)

No probes were added, and none needed reverting. `git status --short` shows
these files plus the pre-existing dirty `tools/RT64` submodule that was already
modified before this session.
