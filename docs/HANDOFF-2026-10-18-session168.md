# Session 168 handoff — the browser boots from a battery save and reaches mission scene `0x03`, and the mission traps before its first frame

## Goal

Make the browser build able to boot from a battery save (Part A), use the
existing mission save to reach the 3D mission scene `0x03` (Part B), and capture
the first browser-vs-native comparison of it (Part C). Depth, lighting and
culling are out of scope; this round is the enabler plus the baseline
measurement.

Timebox: 100 minutes.

## Result

1. **`?OGRE_SAVE=<url>` installs a 32 KiB battery image at
   `/ogre/saves/ogrebattle64-us-rev1.bin` before the runtime's first save read.**
   The path is the one `pi.cpp` reads: `init_saving` calls
   `set_save_file_path(u8"", recomp::current_game_id())`
   (`tools/N64ModernRuntime/librecomp/src/pi.cpp:436`), the folder is
   `config_path / "saves"` (`pi.cpp:288-293`) and the file is
   `<game id>.bin`; the game id is `ogrebattle64-us-rev1`
   (`app/src/game.hpp:16`), so the browser writes exactly what the native
   `OGRE_SAVE` writes (`app/src/save_import.cpp`, called from
   `app/src/main.cpp:534`). The read that has to see the file is
   `read_save_file` (`pi.cpp:414`), reached from `init_saving` before the game
   entrypoint runs.

2. **The import runs after the IDBFS load and the boot waits for it.**
   `mountPersistentStorage`'s `FS.syncfs(true)` callback now calls
   `onStorageReady()` (`app/web/web.js:844`), which starts the import. Writing
   before that load would let IndexedDB's copy replace the imported file.
   `startBoot` checks the gate first (`web.js:353-370`): with an armed, unsettled
   import it logs `[web:save] boot deferred until the OGRE_SAVE import settles`,
   registers a waiter and returns, and `settleSaveImport` (`web.js:94`) resumes
   it. Both boot paths go through `startBoot` - the stored-ROM auto-start (from
   `onStorageReady`) and the ROM picker (`#rom-input` change) - so both wait. A
   fetch or write failure settles the gate as failed and the boot proceeds
   without a battery. `writeBatterySave` (`web.js:150`) writes the bytes, logs
   `[web:save] imported <url> (<N> bytes) -> /ogre/saves/...`, then calls
   `FS.syncfs(false, ...)` and settles on the callback, so the file is in
   IndexedDB before the runtime reads it. The FS guard is `moduleReady()` plus
   `Module.FS`; when the FS is absent the import is skipped and the gate is
   settled (`web.js:862-870`).

3. **The observed order in a browser run.** `/tmp/w168/web-escort/m-f-a1-page.txt`:
   ```
   [web:save] OGRE_SAVE=assets/escort_suspend.bin -> /ogre/saves/ogrebattle64-us-rev1.bin (imported after the /ogre mount, before the boot)
   [web:save] persistent storage mounted at /ogre
   [web:save] OGRE_SAVE=assets/escort_suspend.bin: fetching /assets/escort_suspend.bin
   [web:save] imported assets/escort_suspend.bin (32768 bytes) -> /ogre/saves/ogrebattle64-us-rev1.bin
   ...
   [stdout] [save] loaded /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)
   ```
   The relative URL resolves from the server root: the page is served from
   `/app/web/`, so `assets/...` would otherwise become
   `/app/web/assets/...`. `resolveSaveUrl` (`web.js:118`) prepends `/`.
   A second `[save] wrote` pair later in the log is the game writing the battery
   back after it consumes the suspend record.

4. **Both boot orders were run, and the second is the one that needs the gate.**
   `/tmp/w168/orders.cjs` loads the page, picks the ROM (`#rom-input`), waits
   for the boot, then navigates the same browser context again, which is the
   stored-ROM auto-start. Path 1 logs the import and then `[save] loaded
   /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)`. Path 2 logs `[web]
   Stored ROM found - starting the game with the previously loaded ROM.`,
   `[web:save] boot deferred until the OGRE_SAVE import settles`, the import,
   and then `[web] Boot thread started`; the auto-start had reached `startBoot`
   before the fetch finished and the gate held it. No page errors on either.
   A missing image is the failure path: `?OGRE_SAVE=assets/does-not-exist.bin`
   logs `[web:save] OGRE_SAVE=assets/does-not-exist.bin: import failed: Error:
   HTTP 404 - booting without it` and `[web] Boot thread started`, with no page
   error (`/tmp/w168/failboot.cjs`).

5. **`OGRE_TAP_SCENE_BUTTON` is now a page knob.** Parsing and dispatch mirror
   `app/src/sdl_platform.cpp:205-330`: `<scene>:<buttons>[:<count>],...`, scene
   names from `kScenes` (`app/src/bank_overlays.cpp:109-118`:
   `title 0x04`, `intro 0x09`, `publishers 0x0A`, `story 0x0B`,
   `unit-info 0x0C`, `tutorial 0x17`, `menu 0x18`, `new-game 0x02`) or a hex id,
   buttons joined with `+`, unlimited when `count` is absent, the first entry that
   still matches and has presses left wins, and it replaces the wall-clock
   `OGRE_TAP_BUTTON` schedule while it is set (`web.js:522-610`). The active
   scene comes from the `[scene] t=<ms> id=0x....` lines the page already
   receives: `trackActiveScene` (`web.js:443`) subscribes to
   `window.ogreLog.onLine`, so no wasm export was added. The press log line is
   `[web:input] automation scene tap <n>/<count> scene=0x.... buttons=...`, the
   page's copy of the native `[SDL] automation scene tap` line.

6. **The browser reaches scene `0x03` with the census's route, from the same
   save.** Command:
   `node /tmp/w168/drive.cjs 'http://127.0.0.1:8931/app/web/index.html?OGRE_SAVE=assets/escort_suspend.bin&OGRE_SCENE=title&OGRE_SCENE_LOG=1&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6' /tmp/w168/web-escort/m-f --shots 6 --gap 3000 --pre-delay 15000 --attempts 2 --secs 260`.
   The scene timeline (game time from the log):
   ```
   [stderr] [scene] t=31407ms id=0x0004 descriptor=0x8018FB40 mask=0x00008000   (forced title)
   [web:input] automation scene tap 1/6 scene=0x0004 buttons=start
   [stderr] [scene] t=60697ms id=0x0012 descriptor=0x8018FBC0 mask=0x00008000   (Load Game)
   [web:input] automation scene tap 1/6 scene=0x0012 buttons=a
   [stdout] [save] game writes the battery (SRAM) save: dev=0x00000000 size=0x100
   [stderr] [scene] t=85121ms id=0x0003 descriptor=0x8018F350 mask=0x0000038C   (the mission)
   ```
   `0x0003` is the mission descriptor `0x8018F350` with mask `0x38C`
   (records 2, 3, 7, 8, 9), the descriptor `docs/scenes.md` records for the
   mission. The Load Game screen showed both slots and the suspend record
   (`GAME DATA 1 OTA Ch.1 Scene 4 Zenobian Border`, `SUSPEND DATA OTA Ch.1
   Scene 5 Mylesia`); the A press opened the `Data will be erased when game is
   resumed. Proceed?` prompt and the following press answered it.

7. **The mission produces no frame in the browser: the worker traps during the
   mission's bank streaming.** After the `0x0003` line the page logs five of the
   six overlay loads the native run makes and then the trap
   (`app/web/index.html`'s `printErr` hook puts it in the page log):
   ```
   [bank] loading overlay record rom=0x0E4910 ram=0x80197B90 size=0x72C0 (40 functions)
   [bank] loading overlay record rom=0x0EBBD0 ram=0x8019EE70 size=0xE440 (96 functions)
   [bank] loading overlay record rom=0x101D00 ram=0x801AD5C0 size=0x43530 (300 functions)
   [bank] loading overlay record rom=0x145230 ram=0x801F4050 size=0x99D0 (85 functions)
   [bank] loading overlay record rom=0x14EC00 ram=0x801FDA90 size=0x173E0 (97 functions)
   [stderr] worker sent an error! .../ogrebattle64.js:1: Uncaught RuntimeError: memory access out of bounds
   ```
   The native run loads one more record, `rom=0x171EC0 ram=0x80214FA0 size=0x6030
   (22 functions)`, and then renders. The browser submits no display list for
   the mission at all: the last `[GFX-TASK]` line before the `0x0003` line is
   task 30 (`ucode=0x8009F540`, 565 draws), which is the Load Game screen, and
   no `[GFX-TASK]` follows. The canvas keeps the last presented frame, the Load
   Game screen with the warning prompt, byte-identical for the 25 s of captures
   (`m-f-a1-00.png` .. `-05.png`, 90 602 bytes each). The earlier `m-c` run
   trapped at the same point (`m-c-a2-page.txt:913`).

8. **The native reference takes the same save and the same route, and renders
   the mission.** Command:
   ```
   OGRE_SAVE=assets/escort_suspend.bin OGRE_SAVE_RESET=1 OGRE_PREF_DIR=/tmp/w168/natpref2 \
   OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title OGRE_SCENE_LOG=1 \
   OGRE_TAP_MS=1000 OGRE_TAP_SCENE_BUTTON="title:start:6,0x12:a:6" \
   OGRE_CAPTURE_TARGET=/tmp/w168/nat2/t OGRE_CAPTURE_AFTER=0 OGRE_CAPTURE_EVERY=10 \
   OGRE_EXIT_AFTER_MS=30000 ./build-dist/ogrebattle64 assets/ogre64.z64
   ```
   Timeline: `[scene] t=2539ms id=0x0004`, `[scene] t=5788ms id=0x0012`,
   six `automation scene tap ... scene=0x0012 buttons=a`, then
   `[scene] t=13318ms id=0x0003`. Screen: the mission's first intro phase, the
   `NOTE` *Winning condition: Taking over enemy headquarters.* with the target
   fort illustration `Castle Burgund` over the 3D terrain and the `MISSION`
   banners. The last captured frame `t.800.bin` (present 800, wall 28.7 s,
   `[capture] wrote /tmp/w168/nat2/t.800.bin (320x240 format=11)`) is the one
   used below. The mission animates (the `MISSION` banners scroll), so `t.700`
   differs from `t.800`; `t.800` is the latest captured instant.

9. **The comparison is not a same-instant screen comparison, and the numbers say
   that.** `docs/proofs/web-mission-baseline.png` is browser left, native right
   at 2x: the browser panel is the canvas 17 s after the `0x0003` line (the
   frozen Load Game frame), the native panel is `t.800`. `tools/web_frame_cmp.py`'s
   `compare` over the 319x239 drawing buffer and the 320x240 target gives
   `meanAbs R=81.14 G=73.78 B=54.39`, `meanSigned R=54.76 G=39.96 B=23.25`,
   `exact=0.0406`. Those numbers measure two different screens, so they are the
   distance between the browser's last frame and the native mission frame, not a
   renderer difference; the browser produced no mission frame to compare.

10. **What the mission's own draw census predicts, and what is therefore still
   unmeasured.** `/tmp/census167/escort-dec-s4.json` records 989 100 triangle
   draws in scene `0x0003`: 916 448 with `G_LIGHTING` set, 926 539 with
   `G_CULL_BACK`, 516 463 with `G_ZBUFFER` and with `Z_CMP`+`Z_UPD`
   (`OTHERMODE_L 0x00552078`), 2 360 `SETZIMG` and 11 740 `SETPRIMDEPTH`. The
   browser renderer ignores all three (`depth` and `lighting`/`cull` are not
   implemented; `app/src/web_renderer.cpp` is untouched this round), so the
   first frame that does reach the canvas is expected to differ in exactly those
   three ways - shaded models drawn with the vertex colour, near geometry
   overpainting far geometry, and backfaces drawn. This round did **not** measure
   any of them, because there is no browser mission frame.

## Evidence

- The save path and the read: `tools/N64ModernRuntime/librecomp/src/pi.cpp:288`
  (`set_save_file_path`, `config_path / "saves" / (name + ".bin")`), `:414`
  (`read_save_file`), `:435-440` (`init_saving` ->
  `set_save_file_path(u8"", recomp::current_game_id())` -> `read_save_file`);
  called from `tools/N64ModernRuntime/librecomp/src/recomp.cpp:872`, i.e. before
  the game entrypoint. `app/src/game.hpp:16` has the game id.
- The native equivalent: `app/src/save_import.cpp:300` reads `OGRE_SAVE`,
  `:393` logs `[save] OGRE_SAVE=...: wrote <path> (<N> bytes)`, and
  `app/src/main.cpp:534` calls it before `recomp::start`.
- The page import: `app/web/web.js:83` (path), `:94` (`settleSaveImport`),
  `:118` (`resolveSaveUrl`), `:150` (`writeBatterySave`, the `[web:save]
  imported` line and the `syncfs(false)`), `:197` (`importBatterySave`),
  `:353-370` (`startBoot`'s gate), `:844` (`onStorageReady`), `:862-870`
  (no-FS guard). `app/web/index.html:59-62` is the optional `#save-input`
  ("Load save"), which writes through `writeBatterySave` and reloads.
- The scene-keyed taps: `app/web/web.js:431-435` (the scene table), `:443`
  (`trackActiveScene`), `:522-610` (`initSceneTapSchedule`), `:589` (the press
  log line). Native reference: `app/src/sdl_platform.cpp:205-330`,
  `:1052-1064`, and the names `app/src/bank_overlays.cpp:109-118`.
- The census: `/tmp/census167/escort-dec-s4.json` scene `"0x0003"` -
  `"draws": 989100`, `"draw_bit": {"CULL_BACK": 926539, "LIGHTING": 916448,
  "SHADE": 989100, "ZBUFFER": 516463}`, `"rm_zcmp": 516463`,
  `"rm_zupd": 516463`, `"zimg": 2360`, `"primdepth": 11740`,
  `"othermode_l_vals": {"00552078": 516463, ...}`.
- The browser run: `/tmp/w168/web-escort/m-f-a1-page.txt` (the log above),
  `m-f-a1-*.png` (the frozen canvas), `m-f-result.json`.
- The native run: `/tmp/w168/nat2.log` and `/tmp/w168/nat2/t.800.bin`
  (converted with `python3 tools/rt64_target.py /tmp/w168/nat2/t.800.bin
  /tmp/w168/nat2/t.800.png 320 240`).
- The comparison and the proof compose: `/tmp/w168/compose.py` (calls
  `tools/web_frame_cmp.py`'s `canvas_pixels`, `compare`, `write_png`).

## What changed

`app/web/web.js` gains two page knobs and one optional file input's handler.

- `OGRE_SAVE=<url>`: fetch the image, write it to
  `/ogre/saves/ogrebattle64-us-rev1.bin` after the IDBFS load, `syncfs(false)`,
  log one `[web:save] imported ...` line, and hold the boot until the import has
  settled (success or failure). Relative URLs resolve from the server root.
- `OGRE_TAP_SCENE_BUTTON`: the native scene-keyed tap schedule, keyed on the
  scene id parsed from the page's own `[scene]` lines; it replaces
  `OGRE_TAP_BUTTON` while set. `OGRE_TAP_MS` is its interval as before.

`app/web/index.html` adds the `#save-input` file input and one sentence about
`OGRE_SAVE` in the controls paragraph. `app/src/web_renderer.cpp` is untouched.
No file under `RecompiledFuncs/`, `Bank*Funcs/`, `RspFuncs/`, `tools/RT64` or
`app/src/main_web.cpp` was changed.

## Files changed

| file | change |
|---|---|
| `app/web/web.js` | `OGRE_SAVE` import (fetch, write, `syncfs(false)`, boot gate, `[web:save]` lines); `OGRE_TAP_SCENE_BUTTON` with the scene tracker; `#save-input` handler |
| `app/web/index.html` | `#save-input` input and the `OGRE_SAVE` sentence |
| `docs/proofs/web-mission-baseline.png` | browser canvas at the mission instant (left) and the native mission frame `t.800` (right), 2x each |
| `docs/HANDOFF-2026-10-18-session168.md` | this file |
| `PLAN.md`, `docs/STATUS-LOG.md`, `docs/README.md`, `docs/DECISIONS.md`, `docs/guides/web-probes.md`, `docs/guides/app-build.md` | session entry, the two page knobs documented, decisions 168 |

No commit was made.

## What was run for verification

- `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` - clean (`[100%] Built target
  ogrebattle64`).
- `node debug/probes/testdraw.cjs --flags 96 --expect-fbtex --expect-hits 1` -
  exit 0 (`fbtex hits=1 lines=1`).
- `node debug/probes/boot.cjs --attempts 3 --secs 40` - `RENDERED` on attempt 1,
  `nonBlack=24972`, `colorful=16468` (`sawRealDl=false`, the known boot
  trajectory label; the canvas holds the drawn frame). The save-import run also
  rendered the title (`nonBlack=292472`, `colorful=270668` at `0x0004`).
- The browser route run `m-f` (command in Result 6), the native capture run
  (command in Result 8), and the compose/compare script (Result 9).
- `/tmp/w168/orders.cjs` (both boot orders) and `/tmp/w168/failboot.cjs` (the
  failed-import path).
- `grep -rn "probe168\|OGRE_P168" app/ debug/` - empty (the only matches in
  `docs/` are the handoff and `DECISIONS.md` lines that quote this grep). No
  renderer probe was added this round; the temporary driver, compose and
  both-orders scripts live in `/tmp/w168/` (`drive.cjs`, `compose.py`,
  `orders.cjs`, `smoke.cjs`).

There is no repo test harness; each check is a ROM-dependent run.

## Open

1. **The browser traps in the mission (`memory access out of bounds`) after five
   of the six bank loads.** The native run loads
   `rom=0x171EC0 ram=0x80214FA0 size=0x6030 (22 functions)` next and renders;
   the browser never logs that load. The next measurement is the crash site:
   `OGRE_DUMP_RDRAM` is not available at a wasm trap, so the cheap route is to
   log each `[bank]` load's `ram`/`size` and the mission enter's call targets, or
   to run the same save natively with `tools/watch.sh` on `0x80214FA0` and
   compare. Until it is fixed there is no browser mission frame.
2. **No browser mission frame, so no depth/lighting/culling measurement.**
   The census numbers (916 448 lit draws, 926 539 backface-culled, 516 463
   depth-tested, 2 360 `SETZIMG`, 11 740 `SETPRIMDEPTH`) are the size of the
   expected difference, not a measurement of it. The renderer round that
   implements those three reads this handoff's proof only as "the browser has no
   frame yet".
3. **The `#save-input` reload path is written but not exercised in a run.** Its
   write goes through the same `writeBatterySave`, so the `OGRE_SAVE` runs cover
   the write and the log line; the `window.location.reload()` after the sync is
   unverified end to end.
4. **The Load Game screen's cursor starts on `GAME DATA 1` (the Zenobian Border
   save), not on the suspend record.** The A presses that reached `0x03` answered
   the "data will be erased" prompt for the suspend slot, so the route reads the
   suspend record; whether a different cursor position changes which record is
   resumed was not measured.
