# Handoff — 2026-09-12 — session 25: self-driving native runs, and the idle trajectory localised on native

## Outcome

Session 24 ended with the title sprites correct in the browser and the native
RT64 build running but black, because it never received input and no native
measurement could be repeated without a human at the keyboard.

This session removed that blocker and used it:

1. **The native app drives itself.** `OGRE_TAP_MS=<n>` presses Start for 150 ms
   out of every `n` ms (the native equivalent of the web harness's Enter tap),
   `OGRE_EXIT_AFTER_MS=<n>` bounds a run, and at exit the app prints the last
   recompiled function each game thread entered. Both default to 0, so
   interactive runs are unchanged.
2. **The idle trajectory is reproduced on native and localised.** With traces
   on, a 25 s run makes only **five** successful message-queue sends. The frame
   message `0x800E7D90` is delivered once; `func_8008AFE0` (thread 4) blocks
   forever on its own queue `0x800C4C28`, and the RSP task thread t16
   (`func_80089358`) stays parked on `0x800B9C40` with a resume count of 3. Gfx
   tasks are submitted from those RSP threads, which is exactly why a stalled
   boot has one display list (the blanking one) and no more.
3. **The wall is not input and not the renderer.** Taps do reach the game - they
   moved `bootstate` off the `0xBF880415` stall and started the audio path
   (1085 audio tasks in 60 s) - but produced no second display list. The web
   build, with its own input pump, reached no real frame on 4/4 `progress.cjs`
   attempts this session either.

## What changed

### `app/src/sdl_platform.{hpp,cpp}` — scripted-run knobs

`configure_automation()` (called from `main.cpp` right after `init_sdl`) reads
`OGRE_TAP_MS` / `OGRE_EXIT_AFTER_MS` once into `Platform`. `automation_buttons()`
returns `N64_BTN_START` for the first 150 ms of each window and logs each press;
it is OR-ed into `keyboard_buttons()`, so it contributes **button state only**
and never synthesises SDL events - the main-thread-only `SDL_PumpEvents`
constraint from session 24 stays intact. `pump_sdl_events` (main thread) turns
`exit_after_ms` into a quit request and prints the per-thread table on the way
out:

```
[SDL] per-thread last recompiled function:
[SDL]   t1   last func 0x80099740
...
```

### `docs/DECISIONS.md`, `PLAN.md` — session-25 entries

## Evidence

### A stalled boot has one display list

```
[renderer] display list 1 (type=1 ucode=0x8009F540 data=0x800C6500)
```

That is the boot blanking DL. `screencapture -l <window id>` of a live run
returns a 2560x1496 PNG, and the canvas is black (`colorful=0`) - consistent,
not a renderer failure.

### Runs (native unless noted)

| run | input | display lists | RSP tasks | `bootstate(D_800AEF98)` |
|---|---|---|---|---|
| 40 s, no env | none | 1 | 0 | `0xBF880415` |
| 60 s, `OGRE_TAP_MS=5000` | 11 taps | 1 | 1085 (all type 2) | `0x00000060` |
| 45 s, `OGRE_TAP_MS=3000` | 14 taps | 1 | 0 | `0xBF880415` |
| `progress.cjs` 4x70 s (web) | Enter taps | 1 (`maxTasks=1`) | - | - |

Session 21's six `progress.cjs` attempts reached `tasks` 1, 9, 19, 19, 35, 39;
this session's four all stopped at 1. The **stall point varies** (`0xBF880415`
vs `0x00000060`) and so does the outcome, which is the signature of a boot race.

### The whole 25 s message trace is five sends

```
T=147  do_send OK queue=0x800C6490 msg=0x800E8B10 count=1/1   PI-manager handshake
T=177  do_send OK queue=0x800E8B4C msg=0x800E7D90 count=1/8   boot frame -> t17
T=178  do_send OK queue=0x800E8BBC msg=0x0000029B             SP done
T=182  do_send OK queue=0x800E8BF4 msg=0x0000029C             SI done
T=193  do_send OK queue=0x800E9BA8 msg=0x800E7D90 count=1/8   boot frame -> t5
```

and the exit table (also reproduced across snapshots by `[snap] resumes:`):

```
t1  last func 0x80099740   PI/DMA busy-wait
t3  last func 0x800901C0   osCont family
t4  last func 0x80089054   osSetEventMesg wrapper / func_8008AFE0's loop
t5  last func 0x80089540   queue worker: osCreateMesgQueue + osRecvMesg
t16 last func 0x80089358   RSP task thread: osRecvMesg + osSpGetStatus (resumes stuck at 3)
t17 last func 0x800901C0   osCont family (resumes stuck at 3)
t18 last func 0x80089200   RSP task thread
t19 last func 0x800891A0   VI-retrace worker (receives 0x29A at ~60/s)
```

Reading:

* **VI is fine.** `[vi-debug] retrace -> mq=0x800E8B84 msg=0x0000029A` fires
  ~60/s and t19 consumes it. Session 21 measured the same (59.2 retraces/s,
  0.25 swaps/s).
* **The RSP submit path is starved, not broken.** t16's `func_80089358` is
  `osRecvMesg` + `osSpGetStatus` + submit; it gets one message at boot (T=193,
  to t5's queue `0x800E9BA8`) and nothing after. t5 and t17 received the
  `0x800E7D90` frame messages and then blocked.
* **`func_8008AFE0` (t4, priority 50) is the clearest single blocker.** It
  creates `0x800C4C28` (buffer `0x800BE1A0`, count 8), registers an event
  (`func_80089054` with type 3), then loops on `osRecvMesg(0x800C4C28, flags=1)`
  forever. It re-blocks ~45 times/s and **nothing ever sends to that queue**
  (0 sends in 25 s of trace). Its two message callbacks are `D_800AA090` /
  `D_800AA094`, invoked for message halfwords `1` / `2`; the caller that should
  post them is the next thing to identify.

### Correction: `[snap]`'s `bootstate` is a word read of a byte

`func_80071EB0` (`asm/1060.s` 0x80071FEC) does `sb $zero, D_800AEF98`, and every
reference to `D_800AEF98` in `asm/1060.s` is `lbu`/`sb`. The snapshot prints it
with `snap_w`, so `0xBF880415` is one real byte plus three bytes of neighbours.
It is a usable *fingerprint* of which stall point a boot reached, but the word
value means nothing. The same caution applies to the rest of that line.

## Verified

| check | result |
|---|---|
| `cmake --build build-app -j 8` | clean; only the pre-existing sdl2-compat deployment-target link warning |
| interactive run (no env vars) | unchanged: `api=3`, window opens, DL 1, stall |
| `OGRE_EXIT_AFTER_MS` | exits on time and prints the per-thread table |
| `screencapture -l <window id>` | works; find the id with the Swift snippet below |
| `progress.cjs --attempts 4 --secs 70` | `NO PROGRESS: best maxTasks=1` (exit 2) |

Finding the native window id for a capture (no pyobjc needed):

```sh
swift - <<'EOF'
import Foundation
import CoreGraphics
let opts = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
for w in (CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]]) ?? [] {
    let owner = (w[kCGWindowOwnerName as String] as? String) ?? ""
    if owner.lowercased().contains("ogre") {
        print(w[kCGWindowNumber as String] ?? "", owner, w[kCGWindowBounds as String] ?? "")
    }
}
EOF
screencapture -x -o -l <id> /tmp/ogre-native.png
```

## Still open

0. **Who owes `0x800C4C28` and `0x800B9C40` a message.** This is the wall, now
   in the narrowest form it has had. `func_8008AFE0` (the `0x800C4C28` owner) is
   the first target: find its senders (`osSendMesg` call sites whose queue
   argument resolves to `0x800C4C28`) and check whether the *sender's* thread is
   itself parked. `func_800893C0` (the frame producer) and `func_80073AE4` (the
   `D_800E810C`-gated frame kick) are still the frame-display suspects from
   session 21.
1. **Taps vs. race A/B.** The taps changed the stall point in one run and not in
   another, so input and the boot race are confounded. Run N boots with taps and
   N without and compare the `bootstate` fingerprints; the native harness now
   makes that cheap.
2. **The native canvas has never shown the title scene.** Everything measured
   this session stalled before a second display list. The first real goal for
   the new harness is a run that reaches the title and a `screencapture` of it -
   that is also the "reference frame" session 23 wanted, and it would validate
   `renderer.cpp` (RT64) against the browser's WebGL2 renderer.
3. **Untouched from session 24:** the `P != M` blender fold, the raw-TEXEL1
   black report, the remaining combiner inputs
   (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys), coverage/alpha-compare, framebuffer/VI
   indirection, the TMEM model, and `G_LOADBLOCK`'s `dxt` shape.
4. **`[snap]`'s callback-list walkers print nothing** (`dispatcher callbacks`,
   `retrace handlers` are empty on both platforms) even though the game registers
   three events. Either the game's lists really are empty at that moment or the
   reader's offset formula is wrong; it is a five-minute check (compare
   `snap_w(addr)` against `MEM_W(addr)` for one address) and it matters because
   those lines have been used as evidence in earlier sessions.

## Repro

```sh
cmake --build build-app -j 8

# baseline (interactive-equivalent, unbounded; Ctrl-C or close to stop)
./build-app/ogrebattle64

# a bounded, self-driving run with taps every 5 s
OGRE_TAP_MS=5000 OGRE_EXIT_AFTER_MS=60000 ./build-app/ogrebattle64 > /tmp/ogre-tap.out 2>&1

# the message-queue trace (very chatty; keep the run short)
OGRE_DEBUG_TRACES=1 OGRE_TAP_MS=5000 OGRE_EXIT_AFTER_MS=25000 ./build-app/ogrebattle64 > /tmp/ogre-traces.out 2>&1
#   then:  grep -c "do_send OK" /tmp/ogre-traces.out        -> 5
#          grep "do_send OK"   /tmp/ogre-traces.out
#          grep -A 12 "per-thread last" /tmp/ogre-traces.out

# the web side, for comparison
python3 debug/server.py &                # :8931 (already running this session)
node debug/probes/progress.cjs --attempts 4 --secs 70 --out s25-web
```

## Files changed (tracked)

- `app/src/sdl_platform.hpp` — `tap_ms`, `exit_after_ms`, `start_ticks`
  fields; `configure_automation()` declaration.
- `app/src/sdl_platform.cpp` — `platform_millis()`, `env_millis()`,
  `configure_automation()`, `automation_buttons()` (150 ms Start press per
  `tap_ms` window, logged), the `kTapHoldMs` constant, the exit request and the
  per-thread last-function table in `pump_sdl_events()`.
- `app/src/main.cpp` — calls `ogre::configure_automation(ogre::g_platform)`
  after `init_sdl()`.
- `docs/DECISIONS.md` — session-25 entry (the automation decision, the
  input-is-not-the-gate finding, the five-sends localisation, the `bootstate`
  read-width correction).
- `PLAN.md` — session-25 status bullet.
- this file.

No vendored code was changed this session.
