# Handoff — 2026-09-25, session 99: the C-button bits were rotated

**Goal (developer):** *"it seems that the c buttons are mapped wrongly"*, with the
observed mapping `I → right, J → top, K → left, L → down` and the expected
mapping `I → top, K → down, J → left, L → right`. *"compare with the zelda
recomp if necessary"*.

**Result:** the keyboard defaults were already right; the N64 button-bit
constants for the four C buttons in `app/src/input_map.hpp` were rotated one
position. The app labels `I` as C-UP and sent `0x0001`, which libultra reads as
C-RIGHT. The four values are corrected to libultra's
`CONT_C_UP/DOWN/LEFT/RIGHT` = `0x0008/0x0004/0x0002/0x0001`. `app/web/web.js`
carried its own copy of the same rotated values and is corrected the same way.

---

## 1. Why the labels were right and the output was wrong

`app/src/input_map.hpp` names one row per N64 button and gives each row the
16-bit mask that the runtime writes into `osContPad.button`. The default key
table in `app/src/input_map.cpp` (lines 77-82) maps the rows to scancodes:

```
A=X  B=Z  Z=C  START=Enter  L=Q  R=E  UP/DOWN/LEFT/RIGHT=arrows
C-UP=I  C-DOWN=K  C-LEFT=J  C-RIGHT=L
```

That is the mapping the developer asks for. The rotation was in the enum values:

| row | key | value before | libultra name of that value | value after |
|---|---|---|---|---|
| C-UP | I | `0x0001` | `CONT_C_RIGHT` | `0x0008` |
| C-DOWN | K | `0x0002` | `CONT_C_LEFT` | `0x0004` |
| C-LEFT | J | `0x0008` | `CONT_C_UP` | `0x0002` |
| C-RIGHT | L | `0x0004` | `CONT_C_DOWN` | `0x0001` |

Pressing `I` set bit `0x0001`; the game read that bit as C-RIGHT. The reported
`I → right, J → top, K → left, L → down` is exactly this table.

The bits reach the game unchanged. `tools/N64ModernRuntime/ultramodern/src/input.cpp`
`osContGetReadData` (line 178) writes the callback's `buttons` word straight into
`data[controller].button`, so the app's word must be in libultra's bit order.

## 2. The reference values

`tools/RecompFrontend/recompinput/include/recompinput/input_types.h` (the input
layer the recomp frontend uses; the same file upstream at
`N64Recomp/RecompFrontend`) defines:

```
DEFINE_INPUT(C_UP,    0x0008, "C Up")
DEFINE_INPUT(C_DOWN,  0x0004, "C Down")
DEFINE_INPUT(C_LEFT,  0x0002, "C Left")
DEFINE_INPUT(C_RIGHT, 0x0001, "C Right")
```

The other ten bits in the repo's enum (`A 0x8000`, `B 0x4000`, `Z 0x2000`,
`START 0x1000`, D-pad `0x0800/0x0400/0x0200/0x0100`, `L 0x0020`, `R 0x0010`) were
already correct.

## 3. Consumers

Every native consumer reads the enum, so the four corrected values fix all of
them at once: the keyboard poll (`keyboard_buttons_from_map`), the fixed
right-stick C layer (`sdl_platform.cpp` lines 881-884), the `OGRE_TAP_BUTTON`
parser (`sdl_platform.cpp` lines 79-85, so `cu`/`cd`/`cl`/`cr` now set the bit
the game reads as that direction), and the launcher/overlay CONTROLS tab.

The web build is separate: `app/web/web.js` builds the mask in JavaScript and
pushes it through `ogre_input_set`, and `app/src/web_platform.cpp` forwards it
with no mapping. Only `web.js` mattered there, and its `BTN` table is corrected.
A saved `controls.cfg` stores scancode names per button tag, so existing configs
need no change.

## 4. Verification

- The four values are asserted at compile time. A throwaway translation unit
  including `input_map.hpp`, compiled with `input_map.cpp`'s own flags from
  `build-app/compile_commands.json`, passes `static_assert(N64_BTN_C_UP ==
  0x0008)` and the three others (exit 0).
- `cmake --build build-app --target ogrebattle64` (exit 0) rebuilt
  `input_map.cpp.o` (Sep 25 15:05:24) and relinked `build-app/ogrebattle64`
  (Sep 25 15:05:26). The header change also rebuilt `ui.cpp`, `launcher.cpp`,
  `overlay.cpp` and `sdl_platform.cpp`.
- No `controls.cfg` exists in `build-app/`, so the defaults above are the live
  mapping on the next launch.
- The web `BTN` table and the C rows now match
  `tools/RecompFrontend/recompinput/include/recompinput/input_types.h` and the
  upstream file fetched from `N64Recomp/RecompFrontend`.
- Not run: no in-game tap sequence was played to confirm the four directions on
  screen. The bit values are read from the reference, not observed in a run. The
  code path between the mask and `osContPad.button` has no other transform.

## 4a. The first verification named the wrong binary

The first pass ran `make app` and reported that it linked `build-app/ogrebattle64`.
That is wrong. `Makefile` line 425 sets `DIST_STATIC_SDL ?= 1`, and line 500
selects `APP_BUILD_DIR := build-dist` when it is 1, so `make app` built
`build-dist/ogrebattle64` (Sep 25 15:00) and left `build-app/ogrebattle64`
(Sep 25 14:45) untouched. The developer ran `build-app/ogrebattle64` and saw the
old mapping. The fix was in the source the whole time; the tested binary was
stale. `cmake --build build-app --target ogrebattle64` now refreshes the binary
the developer runs. The trap is recorded in `docs/guides/app-build.md`.

## 5. Files changed

- `app/src/input_map.hpp` — the four C button values, with a comment naming the
  reference.
- `app/web/web.js` — the `BTN` C values.
- `docs/DECISIONS.md`, `docs/STATUS-LOG.md`, this file.

No generated file, no recompiler input and no submodule was touched.
`tools/RT64` was already modified in the working tree at session start and is
not part of this change.
