# Session status log

This is the per-session status log that `PLAN.md` carried from session 3 to
session 97. It is reverse-chronological: the newest session is first. It is
kept for the reasoning and the cross-session corrections it records.

`PLAN.md` now holds the live status, the open-work list and the roadmap. The
authoritative per-session record is `docs/HANDOFF-YYYY-MM-DD-sessionN.md`.

Entries written before session 97 name root-level paths for files that now
live under `config/` and `patches/` (session 97 moved them).

## Status by session (newest first)

- ✅ **SOUNDS and VOLUME are SETTINGS rows, and the audio callback scales every queued buffer (session 106).** Developer request: a sound control under SETTINGS in the `ESC` overlay — `sounds [x] on [ ] off` and a `volume` slider `0..100%` whose handle moves across the bar (`|----------` at 0, `-----|-----` at 50, `----------|` at 100). The SETTINGS tab gained an `AUDIO` section and two rows after WIDESCREEN. SOUNDS is the same radio group as WIDESCREEN (`SoundMode{On,Off}`, drawn `[x] ON [ ] OFF`), and VOLUME a slider of `kVolumeCells` = 11 characters with the handle on the cell nearest the percent: `LEFT`/`RIGHT`/`SPACE` step 5% and clamp at the ends (SOUNDS wraps), a mouse click sets a marker or the slider cell it lands on (10% each), and the row's text is `volume_text()` = bar + ` <n>%`. The gain reaches the device in `queue_audio_samples` (`app/src/sdl_platform.cpp`): 100 queues the game's buffer unchanged, a lower gain scales a copy into a scratch buffer, and 0 queues silence. The frame count is the same at every gain, because `ultramodern::audio_dma_busy()` paces the game's audio generation from the queued depth, so a mute that queued nothing would report the AI idle and make the game overproduce. Saved as `sounds = off|on` and `volume = <0..100>` in `<config>/settings.cfg`; `OGRE_SOUNDS` / `OGRE_VOLUME` override the file for one run without writing it, the `OGRE_SPEED` rule. **Verified** on `build-app/ogrebattle64` (macOS): the overlay and launcher SETTINGS tabs draw the two rows (`docs/proofs/native-overlay-settings.png`, `-launcher-settings.png` refreshed — both were also stale, showing the pre-session-105 widescreen row); `OGRE_OVERLAY_KEYS="Down,Down,Down,Left x7"` writes `volume = 65` and the row reads `-------|--- 65%`; `Down,Down,Space` writes `sounds = off`; a file with `sounds = off` / `volume = 40` loads as `sounds off, volume 40%`, a file with `volume = 250` / `sounds = maybe` loads as `100%` / `on`, and a file with neither key loads as `on` / `100`; `OGRE_SOUNDS` / `OGRE_VOLUME` win over the file and are not written back. A temporary `probe106` in `queue_audio_samples` (reverted; `grep probe106` is empty) measured the scaling: at gain 50 `peak_out = peak_in/2` (20097→10048, 16245→8122, 3335→1667), at gain 0 `peak_out = 0` with `samples = 1104` and a non-empty device queue (`queued = 7842` int16) on every buffer, so the mute keeps the pacing signal. Not verified: a mouse click on a slider cell (no synthetic-mouse hook exists; the cell rects come from the same fixed-cell geometry as the drawn bar), and the browser build, which does not read `settings.cfg` and has no settings UI, so `web_platform.cpp` still plays at full volume. See `docs/HANDOFF-2026-09-25-session106.md`.
- ✅ **WIDESCREEN is a toggle (`off` / `on`), and the `MISSIONS` / `ALWAYS` split is gone (session 105).** Developer report: the two widescreen options make no difference, because both open the same 16:9 window for the whole run and leave black side bars on the non-mission scenes; the split was a leftover from the first implementation, which resized the window at every scene change. Landed: `settings.{hpp,cpp}` has `WidescreenMode{Off,On}` (`kWidescreenModeCount = 2`), the row draws `WIDESCREEN   [ ] OFF [x] ON`, and the file writes `widescreen = off|on`; `parse_widescreen` still reads the retired `missions` / `always` / `1` / `2` spellings as `on` and `off` / `0` as `off`, in both `settings.cfg` and `OGRE_WIDESCREEN`, so an existing file keeps its behaviour. `on` keeps the shipped `missions` rule: `app/src/widescreen.cpp`'s `wants_expand()` is `widescreen_mode() != Off && active_scene_id() == 0x03`, so the mission field is hor+ 16:9 while the 2D boot/story screens stay 4:3 and pillarboxed; the old `always` mode, which stretched those 2D screens, is gone (session 102 §6 had named it as the mode to drop). The window rule is unchanged: `off` opens 4:3, `on` opens 16:9, and only a toggle change re-fits it. `ui.cpp`, `launcher.cpp` and `overlay.cpp` needed no change, because the radio group reads `kWidescreenModeCount` and the row-kind checks already include `RowKind::Widescreen`. **Verified** on `build-dist/ogrebattle64`: the SETTINGS tab draws `[ ] OFF [x] ON` (`docs/proofs/native-widescreen-launcher.png` refreshed); `Down,Right` writes `widescreen = on` and the next boot logs `game speed x1, widescreen on`; `OGRE_WIDESCREEN=missions|always|on|1|2` all log `widescreen on` and `off|0` log `off`; a 40 s run with `assets/escort_suspend.bin` logs `aspect ratio expand (scene 0x0003, mode ON)` on each of two mission visits and `original` on the intervening scene `0x0002`; an `ESC`-overlay run draws the same row, writes `widescreen = on` and re-fits the window once (`960x720 -> 1280x720`). See `docs/HANDOFF-2026-09-25-session105.md`.
- ✅ **The shared panel lost its ROM tab and gained a fixed position (session 104).** Two developer requests for the `ui::Panel` that the start screen and the `ESC` overlay both draw. (1) The ROM tab is redundant, and a start screen with no ROM can offer the picker on the START GAME tab: `ui::Tab::Rom` is gone, `Panel::build_rows` (new; `rebuild_rows` was refactored into it) puts the `RowKind::Rom` row on START GAME after `RowKind::Start`, and `select_first()` already prefers the first *selectable* row, so with no ROM the ROM row is the selected one and `SPACE`/click opens the picker unchanged. Its left text is now `[x] ROM loaded`, and the launcher's error hint is `PRESS SPACE ON THE ROM ROW TO TRY AGAIN`. (2) The block was centred with the active tab's height, so switching tabs moved the header: `PanelMetrics` gained `layout_height = max(active tab, layout reference tab)` where `kLayoutReferenceTab = Tab::Controls` (the tallest), `measure_panel` computes both through one shared `layout_rows` helper so the measured and drawn heights cannot drift, `Panel::layout_rows_` holds the reference rows from the same `build_rows` call, and both callers centre the block and place the footer with `layout_height` while `draw_panel` fills the background with it. A tab taller than CONTROLS still grows the block, because it is a `max`. Verified on `build-dist/ogrebattle64` (note: **`make app` writes `build-dist/`, not `build-app/`** — the first round measured the stale binary): five launcher PPMs at 2560x1440 have the title at rows 48..117, the rule at 202..205, the panel background starting at 302 and the footer at 1382..1417 on every tab, first differing row 332 (the active tab label's colour). The pre-change build, captured for the A/B, put the title at row 446 on START GAME (one panel row), row 423 on SETTINGS and row 48 on CONTROLS, so the header moved by up to 398 px between tabs; the no-ROM capture shows the ROM row selected on START GAME; the overlay's CONTROLS and SETTINGS captures share their header and tab-bar y; `OGRE_LAUNCHER_KEYS=space` still boots; `build-null` compiles. Proofs `native-launcher-{start,controls,mods,settings}.png` refreshed, `native-launcher-rom.png` deleted. See `docs/HANDOFF-2026-09-25-session104.md`.
- ✅ **WIDESCREEN shipped as a SETTINGS row with `OFF` / `MISSIONS` / `ALWAYS` (session 102).** Developer request (from the Reddit mod-idea thread): widescreen during missions. It is a port feature, not a mod, because the mod API hooks recompiled guest functions and this is app plus RT64 configuration. RT64 already did the widening — `ProjectionProcessor::processScene` (`rt64_projection_processor.cpp:101`) counter-scales the game's projection matrices by `1/aspectRatioScale` for the projections that cover the screen and are wider than tall — and the port already forwarded `config.ar_option` (`renderer.cpp:123`) and handled a runtime change (`update_config`, `:917`). Landed: `app/src/widescreen.{hpp,cpp}` (new) is the only writer of `ar_option`, called once per frame from `update_gfx` beside `poll_scene()`, so `MISSIONS` turns Expand on exactly while the dispatcher's active scene (`D_800E810E`) is `0x03` and restores 4:3 elsewhere; `settings.{hpp,cpp}` gained `WidescreenMode{Off,Missions,Always}`, the `widescreen = off|missions|always` key at `<config>/settings.cfg` and `OGRE_WIDESCREEN` as the one-run override under the `OGRE_SPEED` rule (the loader now matches `key = value` by key, because the old parser returned the first integer in the file and a second key made that ambiguous); `ui.{hpp,cpp}` generalised the GAME SPEED layout into `layout_radio`/`RadioToken`/`RadioLayout` so both radio rows share one measure/draw/click geometry, and `RowKind::Widescreen` is stepped by the launcher and the overlay. Verified with the session-98 suspend save: `[widescreen]` logs `aspect ratio expand (scene 0x0003, mode MISSIONS)` on every mission visit and `original` on each `0x02`/`0x0D` between them (three visits, exit 0), an `off` run logs no transition, `always` logs `expand` at frame 1, the launcher and the overlay both write `widescreen = missions` and the next boot reads it, and a battle in the mission fills the width in `missions` and is 4:3 in `off`. **The window shape follows the mode, not the scene (developer correction):** the first version resized the window at every scene transition in `missions` mode, which the developer rejected (*"the window changes its size while the game is running"*); `off` now opens a 4:3 window (953x715 at a 715 window height, no pillarbox — the developer's first report about the old fixed 1280x720 window), `missions`/`always` open 16:9 (1271x715) and never change, and the 4:3 scenes of a widescreen run are pillarboxed inside it. Only a mode change re-fits the window, and `create_window` re-derives the width from the height the window actually got (the window manager returns 715 for a requested 720). Two things recorded so they are not misread: the `MISSION` banner row clips at both edges in 4:3 too (the game's own scrolling marquee), and `ALWAYS` stretches the boot's full-screen 2D stills, which is why `OFF` is the default. **The large 2D backgrounds cannot show more to the sides:** every njpeg background's art is 4:3 (42 of 75 are 320x240, the widest is 336x256), and the cathedral's final blit loads the source's full 319-px width in twelve 20-texel strips (`LOADTILE ... lrs=1276` then `G_TRI2`) into the full-screen scissor, so nothing is off-screen; only 3D gains real width. Note for the next session: `make app` here writes `build-dist/ogrebattle64` (`DIST_STATIC_SDL ?= 1`), and the first A/B ran the stale `build-app/ogrebattle64`. See `docs/HANDOFF-2026-09-25-session102.md`.
- ✅ **The unit profile's DEF/MDEF "extra digit" was RT64's rect/scissor snap, and it is off by default now (session 101).** Developer report: the numbers read as if a cut-off digit followed them (`28` looked like a cropped `9`, `11` like a cropped `2`), and their retail cutout of the same panel is clean. Scene `0x06` is one F3DEX2 display list (`0x80257E50`); each digit is a 6x7-px `G_TEXRECT` at one texel per pixel, and the font sheet (`0x801FFCF8`) has the digits in 6-texel cells, so the dark column at `x=294` is the next cell's column 0. The panel scissor is `(100,84)-(1180,612)` and `RDP::drawRect` rewrites `lrx` to the scissor edge when the two are within one pixel (`abs(1180 - 1176) == 4`) under RT64's "Fix LR with Scissor" enhancement, so the 6-px glyph rectangle became 7 px and sampled texel 54. A temporary probe confirmed the draw call (`raw 1152..1180`, `cycle=0`, `dsdx=1024`) against the display list's `1176` in the same dump; parallel-rdp, which the developer checked, has no such enhancement and covers the six pixels the game asks for. `app/src/renderer.cpp` now clears `enhancementConfig.rect.fixRectLR` after RT64's setup (`OGRE_RECT_LR_FIX=1` A/Bs the old behaviour); the probe then reports `w=6`, three fixed dumps show column 294 as panel border, and the developer confirms the screen is fixed. The probe is reverted. New tooling: the console's `snap [prefix] [ms]` writes the RDRAM image with the game threads parked plus RT64 present captures. A checkpoint `load` at this screen still SIGSEGVs on N64 thread 19. See `docs/HANDOFF-2026-09-25-session101.md`.
- ✅ **The Linux release had no audio because its statically linked SDL2 had only
  the OSS driver (session 100, issue #8).** Developer report from GitHub: 0.3.0
  on CachyOS KDE and on a fresh Fedora box both print `[SDL] audio unavailable,
  running silently: dsp: No such audio device` and repeat `[SDL] Failed to open
  audio device: Audio subsystem is not initialized`. That string is SDL2's OSS
  driver (`SDL_dspaudio.c:293`), and `strings` on the released
  `ogre-battle-64-recomp-linux-x86_64.tar.gz` binary shows `OSS /dev/dsp standard
  audio` with no `ALSA PCM audio`, `PulseAudio` or `Pipewire`: the package's
  SDL2 2.32.10 was built on a host without the audio development headers, and
  SDL2's CMake drops such a backend without failing (`SDL_ALSA (Wanted: ON):
  OFF`, while `CMakeCache.txt` still reads `SDL_ALSA:BOOL=ON`, so only the built
  archive is a check). Reproduced in a bare `ubuntu:24.04` with the repo's own
  pinned SDL2: without the headers `libSDL2.a` defines only
  `DSP`/`DUMMYAUDIO`/`DISKAUDIO_bootstrap`; with `libasound2-dev libpulse-dev
  libpipewire-0.3-dev` it also defines `ALSA`/`PULSEAUDIO`/`PIPEWIRE_bootstrap`.
  Both reporter machines are healthy and neither is consulted before the error:
  a driver that is not compiled in cannot be tried, so `SDL_AUDIODRIVER` cannot
  help either. Landed: `tools/check-sdl2-audio.sh` (new) rejects an archive
  without a real backend and names the packages; `make sdl2-static` asks for the
  three Linux backends, verifies the archive, and removes and rebuilds a cached
  prefix that fails; `tools/smoke-dist.sh` fails a Linux package whose binary
  carries none of the three, and does so on the real 0.3.0 binary; the hosted
  Linux CI step installs the packages; `app/src/sdl_platform.cpp`'s failure line
  now appends the compiled-in driver list. Reference: Zelda64Recomp links the
  system SDL2 on Linux, so it never met this. No game or runtime code changed.
  Open: the 0.3.0 Linux archive on the releases page is still the broken one, so
  a new tag has to be built and published, and no Linux run with sound was
  possible on this machine.
- ✅ **The C buttons produced the wrong directions because their N64 bit masks
  were rotated one position (session 99).** Developer report: *"it seems that
  the c buttons are mapped wrongly"*, observed `I → right, J → top, K → left,
  L → down`, expected `I → top, K → down, J → left, L → right`. The default key
  table already assigned `I/J/K/L` to the C-UP/C-LEFT/C-DOWN/C-RIGHT rows, so
  the labels were right and the enum values in `app/src/input_map.hpp` were not:
  it labelled `0x0001` as C-UP, but libultra reads `0x0001` as `CONT_C_RIGHT`.
  The four values are now `C_UP 0x0008, C_DOWN 0x0004, C_LEFT 0x0002,
  C_RIGHT 0x0001`, matching
  `tools/RecompFrontend/recompinput/include/recompinput/input_types.h` (the
  layer the recomp frontend uses) and upstream `N64Recomp/RecompFrontend`. The
  word reaches `osContPad.button` unchanged
  (`tools/N64ModernRuntime/ultramodern/src/input.cpp:178`), which is why the bit
  order must be libultra's. `app/web/web.js` held its own copy of the rotated
  values and is fixed too. One change covers the keyboard poll, the fixed
  right-stick C layer, `OGRE_TAP_BUTTON`'s `cu/cd/cl/cr` and the CONTROLS tab;
  saved `controls.cfg` files need no migration. The first pass ran `make app`,
  which `DIST_STATIC_SDL ?= 1` routes to `build-dist/ogrebattle64`, so the
  developer's `./build-app/ogrebattle64` was stale and showed the old mapping;
  `cmake --build build-app --target ogrebattle64` refreshes it, and the trap is
  now in `docs/guides/app-build.md`. The four constants pass a `static_assert`
  compiled with `input_map.cpp`'s own flags; the directions were not played back
  in a run.
- ✅ **The escort mission's slow field is the session-80 class a third time, and
  the fix is one more branch in `yield_work_loop_branches` (session 98).** The
  developer's report — *"during gameplay, it becomes slow during an escort
  mission (battery save attached)"* — is a 32768-byte raw `QuestOG3` battery that
  resumes at scene `0x03` (`0x8018F350`, mask `0x38C`) through the title →
  `0x12` Load Game suspend route. Three profiled runs did not reproduce (100 /
  152 / 187 s); the fourth did, in the seconds before the developer closed it.
  The measured phase is `t=57304..59921 ms` with **no scene change**: display-list
  periods go from 8 ms to **80–183 ms** and then clear, every `[prof] T=` line
  from `T=57000` puts the frame-pump thread `t4` in **`0x801D0B78`** (40 → 64 →
  93 %), and the snapshot repeats `[snap] hotloop t4: 0x801D0B78 x100`.
  `func_ovlN_801D0B78` (bank unit N, record 7) scans the `-1`-terminated
  object-id list at `obj+0xA8`; N64Recomp's poll-loop heuristic put a blocking
  `yield_self` at the `bnel` skip branch `0x801D0C08`, so it ran once per skipped
  entry and `yield_self` blocked about one VI retrace each time. **The reusable
  part is the ownership check**: `0x801D0B78` is RAM shared by five bank units,
  so the address names nothing by itself — `app/src/bank_funcs.inc` registers it
  only for unit N, run 4's last load there before the stall is record 7
  (`t=51984`, `rom=0x101D00`), unit AH never loaded at all, and no unit except N
  has a `yield_self` at that branch. `config/banks/config-bankN.toml` now reads
  `yield_work_loop_branches = [0x801B3008, 0x801DA2C0, 0x801D0C08]`; regeneration
  removes exactly one `yield_self` (50 → 49 tree-wide) and `make recomp` with the
  old bank config leaves `RecompiledFuncs/` byte-identical. Fixed build, same
  save, 180 s: `0x801D0B78` in **0 of 180** `[prof]` t4 lines (pre-fix 4 of 61),
  the longest run of consecutive ≥ 50 ms gaps **20 → 2**, and
  `[cover] 0x801D0B78 276` in the coverage census, so the scan still runs and
  only the wait is gone. `check-banks` OK, `elf-rom-check` 0 differing bytes,
  boot `runlog --check` PASS. **Developer-confirmed**, including a second symptom
  of the same site: the slowdown when **opening the list of units** is gone too,
  which also makes the unit list a deterministic trigger for this class in place
  of waiting for the mission state to drift. See
  `docs/HANDOFF-2026-09-25-session98.md`.
- ✅ **The tree layout was reorganised: every config the build reads is under
  `config/`, and the upstream patches are under `patches/` (session 97).**
  `config/config.{yaml,toml}` are the main splat and N64Recomp configs;
  `config/banks/config-bank<U>.{yaml,toml}` are the 34 bank units;
  `config/symbols/` holds `symbol_addrs*.txt`, `reloc_addrs.txt`,
  `extra_syms.txt`, `relocatable_sections.txt` and the generated
  `undefined_{syms,funcs}_auto.txt`; `config/rsp-{njpeg,audio}.toml` are the
  RSPRecomp configs. The repo root dropped from 181 entries to 64. splat,
  N64Recomp and RSPRecomp resolve paths relative to the config file's directory,
  so `config/config.toml` uses `../build/...` and `../RecompiledFuncs`, and each
  bank yaml sets `base_path: ../..`. Removed: the 10 `.ogre-prefs-*` run
  sandboxes (400 MB of ROM copies), `.repro/`, `.DS_Store`, the tracked
  splat-generated `ogrebattle64.d` (now gitignored), and the unreferenced
  pre-streamed-overlay `ogrebattle64.yaml`. `make regenerate` from the moved
  configs passes, and a fresh link of each affected bank unit from its original
  config is byte-identical to the moved one. Handoff
  `docs/HANDOFF-2026-09-22-session97.md`; `docs/HANDOFF-*.md` written before this
  session name the old root paths.
- ✅ **The Esc overlay has a DEBUG tab, and it shows the live Chaos Frame
  (session 95).** `ui::Tab::Debug` (`app/src/ui.hpp`) adds one read-only row,
  "Chaos Frame", to the shared launcher/overlay panel. The overlay samples the
  game's own RDRAM every frame (`guest_byte` in `app/src/overlay.cpp`) at
  `ogre::CHAOS_FRAME_ADDRESS`, so the value is live while the game runs; the
  launcher has no running game and says `GAME NOT RUNNING`.
  `OGRE_OVERLAY_TAB=debug` opens the tab for a scripted run or a screenshot.
  The row prints the value and its ending band (developer): `LOW` 0-35,
  `NEUTRAL` 36-64, `HIGH` 65-100. Verified in one run: with `last_perfect` on the
  battery, a map checkpoint plus `OGRE_OVERLAY_TAB=debug` captured
  `Chaos Frame 99 HIGH` (`docs/proofs/native-overlay-debug-tab.png`) while the
  live console read the same byte, and a title capture read `50 NEUTRAL`.
  **The Chaos Frame is `0x801936C9`, not the community `0x801936A9`.** The
  game-state initialiser `func_8016C900` bzeroes the flags block at `0x80193698`
  and stores `50` at `0x801936C9` (`0x8016C998`), the documented starting value;
  the scene-`0x13` update `func_801B5128` reads that byte and masks it `0x7F`
  (`0x801B5360`, the only flags-block byte the ending screen reads); and the
  scene-script VM opcode `0xFF` stores a script operand there (`0x80171480`).
  `0x801936A9` has no static read or write in the ROM and reads 0 at the title,
  where the Chaos Frame is 50. See `docs/HANDOFF-2026-09-21-session95.md`.

- ⏳ **The Windows `0xC0000005` is in RT64's renderer setup on an old GPU, and
  `OGRE_GRAPHICS_API` lets a player pin the backend (session 94, part 5).** A
  player reports the first release worked; the developer's own Windows laptop
  fails on every release including that one, and the console-window build
  printed a **Vulkan workaround** line just before the crash. That matches the
  code: with a ROM present the app boots the game and RT64 creates its renderer
  on the gfx thread inside `recomp::start`, while without a ROM only the
  launcher's SDL 2D renderer runs — so deleting the ROM "fixes" the start, and
  the same machine fails on every release. The line is RT64's
  `Falling back to Vulkan due to device workaround.`, printed for an NVIDIA
  driver ≤ 475.14, an AMD driver ≤ Jan 2019, and Intel 6th-gen ≤
  31.0.101.2115; the fallback destroys the D3D12 device and builds a Vulkan
  interface on the same window. Landed:
  `OGRE_GRAPHICS_API=<auto|d3d12|vulkan|metal>` (`app/src/renderer.cpp`), which
  sets RT64's `userConfig.graphicsAPI` before setup (`vulkan` skips the
  transition, `d3d12` is the other side of the A/B), documented in the shipped
  README and the knob table. The next report names the device, the driver
  version, whether the fallback ran, and the faulting module. See
  `docs/HANDOFF-2026-09-20-session94.md` part 5.
- ⏳ **Windows crashes with `0xC0000005` when the game starts, and the
  diagnostics to find it are in (session 94, part 4).** The rc2 Windows package
  boots and the launcher runs, but loading the ROM and starting the game dies on
  an access violation, and restarting with the stored ROM dies the same way
  (deleting the ROM lets the launcher run again). Both reports had a header and a
  non-null RDRAM base and then stopped: empty captured sections, no fault fields.
  Landed, none of which is the fix: the report prints the faulting instruction
  and the address it touched separately, each with its host module and offset,
  plus the access kind for an access violation; the Windows handler uses no
  stdio at all, so it cannot hang on a stream lock and truncate the report (the
  way it did); `OGRE_CONSOLE=1` allocates a console so a double-clicked build
  shows the `[boot]` log; `tools/smoke-dist.sh` check 4 fails a package whose
  crash report does not contain the captured boot line; the release workflow
  takes a `platforms` input so `-f platforms=windows` builds and smoke-tests
  Windows alone in about 8 minutes instead of 30; Windows pins
  `SDL_AUDIODRIVER=wasapi` (Zelda64Recomp's workaround for this runtime); and a
  cl.exe link gets `/OPT:NOICF`, because folding can merge two recompiled
  functions and the runtime patches a function's own code for a mod hook. The AV
  itself is open; its next report names the faulting module. See
  `docs/HANDOFF-2026-09-20-session94.md` part 4.
- ✅ **The Windows build runs, carries its runtime DLLs, and the release workflow
  smoke-tests every package (session 94, part 3).** The developer reported that
  the v0.2.0 Windows build "does nothing": no window, no log, no process. The
  cause was one line: `crash_log::install()` called
  `std::setvbuf(stdout, nullptr, _IOLBF, 0)`, and the MSVC CRT rejects a null
  buffer with size 0 as an invalid parameter, whose handler fast-fails the
  process (`0xC0000409`, `ucrtbase.dll`) before any of our handlers are armed —
  so every Windows launch died silently at the top of `main`. glibc and macOS
  accept 0, so only Windows was affected. `cdb.exe` on the runner gave the stack
  (`ucrtbase!invoke_watson` ← `_setvbuf_internal` ← `ogrebattle64+0x10e40`); the
  fix is a static line-buffer with a real size, plus arming the handlers before
  the capture setup. A second, real defect came out of the new smoke test's
  static check: RT64 statically imports `dxcompiler.dll`/`dxil.dll`, which import
  `MSVCP140.dll`/`VCRUNTIME140.dll`, and the package shipped neither, so a
  machine without the redistributable cannot start it. The app now links the
  static CRT (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, inherited by the
  runtime and RT64 targets), SDL2 is built the same way, and `make dist` copies
  `msvcp140.dll`/`vcruntime140.dll`/`vcruntime140_1.dll` from
  `VCToolsRedistDir` next to the `.exe`, failing the package if they are absent.
  Separately, every early exit used to be silent in a console-less build:
  `init_sdl` failure, game-window failure and start-screen window/renderer
  failure returned without a file or a dialog. They now call
  `report_boot_failure()`, which writes `error.log` and shows the reason.
  `init_sdl` also no longer requires audio: `SDL_Init` fails the whole call when
  any requested subsystem fails, and `SDL_INIT_AUDIO` can fail on a host with no
  output device, which stopped the app before its first window; audio and
  controllers are now optional and the game runs silently without them. The
  release workflow gained the missing smoke test: after packaging it runs
  `tools/smoke-dist.sh` on each runner image, which checks the companion files,
  requires every DLL a Windows package's binaries import to be shipped or a
  system DLL (`tools/pe_imports.py`; a launch on a runner with the redist
  installed cannot check that), and runs the binary with `OGRE_SMOKE=1` under a
  deadline to prove it loads and reaches `main`. `make smoke` runs the same
  locally. Verified: the macOS package passes `make smoke`, the 45 s route run is
  `runlog.py --check` PASS, the v0.2.0 Windows zip fails the PE check naming the
  four missing runtime DLLs and the v0.2.1-rc1 Windows smoke step failed on the
  `setvbuf` fast-fail; `v0.2.1-rc2` is the fixed package. See
  `docs/HANDOFF-2026-09-20-session94.md` part 3.
- ✅ **The distributed build opens no console window, and a crash writes
  `error.log` beside the save (session 94).** macOS ships `Ogre Battle 64.app`
  (`packaging/macos-Info.plist`), so Finder launches it without opening Terminal;
  `SDL_FILESYSTEM_BASE_DIR_TYPE=parent` makes `SDL_GetBasePath` return the folder
  that holds the `.app`, so the ROM, `saves/`, `mods/`, `controls.cfg`,
  `settings.cfg` and `error.log` stay in the package folder. Windows links the
  `.exe` as a GUI-subsystem binary (`WIN32_EXECUTABLE`), and `main.cpp` supplies
  the `WinMain` forwarder that the CRT entry point for that subsystem needs
  (MinGW's CRT already forwards). `app/src/crash_log.cpp` replaces the lost
  console: it tees stdout and stderr into two 128 KiB rings while forwarding
  every byte to the real descriptor, arms the fatal-signal and `std::terminate`
  handlers, and writes `<config dir>/error.log` with the reason, the fault
  address, the host pc, the last lines of the app's own log (including any
  `uncompiled streamed record` or `stub called` line) and the runtime's guest
  call chain. Verified on macOS: the bundle launches with no new Terminal window,
  a synthetic and a live SIGSEGV both write the file in the package folder, and
  `> out.log 2> err.log` still separates the streams. New knob
  `OGRE_CRASH_TEST=<segv|bus|abrt|fpe|ill>`. See
  `docs/HANDOFF-2026-09-20-session94.md`.
- ✅ **Closing the game window no longer freezes the process (session 94, part
  2).** The pre-existing freeze had two layers. The teardown unmapped RDRAM at
  the end of `recomp::start` while the game's N64 threads were still running
  recompiled code, so the next guest access faulted inside the image (the
  freeze's fault was at `rdram + 0xE7A18`); and the crash handler then deadlocked
  instead of killing the process, because `fflush(nullptr)` re-acquired a stdio
  stream lock the faulting thread already held. `recomp.cpp` now skips the RDRAM
  free when the run exits through `ultramodern::quit()` (landed in
  `patches/n64modernruntime-ob64.patch`), `main.cpp` runs the SDL teardown and then
  `_Exit(EXIT_SUCCESS)` instead of running the C++ static destructors with those
  threads live, and the signal path no longer flushes and only runs the
  `printf`-based runtime dumps behind a non-blocking `ftrylockfile` probe.
  Verified: a synthetic `SDL_QUIT`, a real click on the window's close button and
  the standard Quit event all exit 0 with no `error.log`, teardown ≈ 0.5 s; the
  45 s route run is `runlog.py --check` PASS; crash reports still carry the guest
  diagnostics. See `docs/HANDOFF-2026-09-20-session94.md` part 2.
- ✅ **A SETTINGS tab carries GAME SPEED, and the speed changes while the game
  runs (session 93).** The shared `ui::Panel` gained a fifth tab whose only row is
  a radio group, `GAME SPEED  [ ] 1 [ ] 2 [x] 4`; `LEFT`/`RIGHT`/`SPACE` step it
  and a click on a marker sets that speed. 6 and 8 were offered first and dropped:
  the developer reports the game's own cursor aiming cannot keep up at those
  speeds. The value is the runtime's emulated-clock multiplier, the quantity
  `OGRE_SPEED` sets. It is persisted as `<config>/settings.cfg`
  (`app/src/settings.{hpp,cpp}`), applied at startup by `load_settings()` before
  the game boots, and `OGRE_SPEED` overrides the file for one run without writing
  it; a file naming 6 or 8 loads as the closest offered value (4). In `Overlay`
  mode CONTROLS **and** SETTINGS are enabled, so `ESC` can change the speed
  mid-game. The runtime gained `ultramodern::set_speed_multiplier()`, and the
  guest clock is continuous across a change: the clock is piecewise linear, with
  the segment anchors published under a seqlock, so `osGetCount`/`osGetTime` and
  pending `OSTimer` deadlines do not jump. A mutex on that read path cost the boot
  visible time (scene `0x09` at 7.3 s / 6.5 s against the pre-change binary's
  6.1 s / 5.3 s at `OGRE_SPEED=4`); the seqlock version matches it within 50 ms
  on three alternating runs. Proofs:
  `docs/proofs/native-launcher-settings.png`,
  `docs/proofs/native-overlay-settings.png`; `runlog.py --check` PASS on the
  maintained 45 s title-route run; the runtime patch re-applies to pristine
  `589bbf0`.
- ✅ **The start screen is tabbed and owns the controller bindings, and `ESC`
  opens the same panel over the running game (session 92).** The start screen's
  three sections became a tab bar — **START GAME**, **ROM**, **MODS**,
  **CONTROLS** — and the new CONTROLS tab lists one row per N64 button with its
  keyboard key, its gamepad source and the field-map action from the game's
  controls description, plus a reset row. `SPACE` arms a rebind, the next key or
  pad button sets that slot, `BACKSPACE`/`DELETE` clear it. The mapping is data
  now (`app/src/input_map.{hpp,cpp}`, defaults identical to the old hardcoded
  map), persisted to `<config>/controls.cfg`, and read every input poll. Pressing
  `ESC` while playing opens a borderless always-on-top SDL window over the game
  window (opacity 0.90) drawn with the same `ui::Panel` and bitmap font; its tab
  bar shows START GAME / ROM / MODS disabled and only CONTROLS active, and
  `get_input` returns an idle pad while it is up, so the game cannot act on the
  keys used to navigate. The UI moved into `app/src/ui.{hpp,cpp}` so the two
  screens cannot drift. The game does not pause behind the overlay (developer's
  choice). Also fixed: the event pump enumerates already-connected controllers
  on its first call, because the launcher drains SDL's
  `SDL_CONTROLLERDEVICEADDED` events before the game window exists. Proofs:
  `docs/proofs/native-launcher-{start,rom,mods,controls}.png`,
  `docs/proofs/native-overlay-controls.png`. See
  `docs/HANDOFF-2026-09-20-session92.md` and `docs/guides/app-build.md` →
  "Controller bindings and the in-game overlay".

- ✅ **The mod system is on, with an example mod shipped (session 91).** The
  runtime's mod support (`librecomp/src/mods.cpp`) was already present and
  already called by `recomp::start()`, but `GameEntry::mod_game_id` was never
  set, so `wait_for_game_started()` skipped loading and every `.nrm` in `mods/`
  was parsed and discarded. `app/src/main.cpp` now sets it and scans before the
  start screen; the start screen (`app/src/launcher.cpp`) has a **START GAME**
  section (inert until a ROM is loaded), a **ROM** section (`[x] Loaded!` plus the
  file name; selectable, so `UP`/`DOWN` + `SPACE` picks a ROM without a mouse, and
  choosing one returns to the screen with START GAME ready) and a **MODS**
  section that toggles mods and edits their options with the description in a
  second column. It appears when no ROM is loaded and whenever a mod is
  installed, so a shipped mod can always be turned off. `mods/skip-boot-logos/`
  is the
  reference example: one entry hook on scene `0x09`'s update writes the title's
  id into the pending-scene word, so the boot goes straight to the title. Built
  by `make mod-syms` + `make example-mods` and shipped by `make dist` when
  present. The example ships **off** (`enabled_by_default = false` in its
  `mod.toml`; `RecompModTool` did not carry that field, so `patches/n64recomp-ob64.patch`
  adds it), so a fresh package plays the vanilla boot until the player turns it
  on. Verified: the true baseline boots to the title at 27.7 s; with the mod
  enabled the title is at 3.0 s and renders correctly (capture), with the battery
  still loaded. **The skip must come from scene `0x09`, not `0x0A`**: cutting `0x0A` to
  one frame leaves the title producing no display lists at all and the last
  presented frame on screen, because `0x0A`'s enter/leave touch overlay-C globals
  that only its completed state machine leaves consistent. Four runtime fixes
  were needed and are in `patches/n64modernruntime-ob64.patch`: the recompiler-context
  section index must be translated to a code-section index before a regenerated
  call is resolved; HI16/LO16 relocations against sections that hold no code must
  keep their absolute immediates; macOS cannot make the executable's `__TEXT`
  writable, so the page is replaced with a private copy before a hooked function
  is patched; and the periodic `[snap]` queue snapshot dereferenced its guest
  addresses without sign-extending, 4 GiB past RDRAM, which is why the developer's
  run died with `SIGSEGV` in `debug_dump_queue_snapshot` (it had been printing
  garbage since it was written).

  See `docs/HANDOFF-2026-09-20-session91.md` and
  `docs/guides/app-build.md` → "Mods".

- ✅ **The "Use Item" unit-command screen runs at full speed (session 90).** The
  screen's per-frame lookup `func_ovlN_801DA248` (bank unit N) carried an
  injected blocking `yield_self` at its outer loop's backward branch
  `0x801DA2C0` — the session-79/80 misclassified-poll-loop class. The frame-pump
  thread sat in it 99 % of every slow second and the display-list period was
  237–275 ms. `0x801DA2C0` is now in `config/banks/config-bankN.toml`'s
  `yield_work_loop_branches` beside session 80's `0x801B3008`; regenerating
  changes exactly one generated file (51 → 50 yield sites), the boot is
  unchanged, the screen's display-list p99 is 34 ms (was 209 ms), and the
  developer confirms it is normal. The next candidate of the same shape,
  `func_ovlN_80204C08` (branch `0x80204C48`), is named in the handoff and is not
  landed. See `docs/HANDOFF-2026-09-20-session90.md`.

- ✅ **Releases can build on GitHub-hosted runners; the generated code comes from
  a private data repository (session 89).** The repository's name is deliberately
  not recorded in this repository; it is the `OGRE_DATA_REPO` Actions variable.
  Session 87b's blocker was that `RecompiledFuncs/`, the 34 `Bank*Funcs/`, `RspFuncs/`
  and `app/src/bank_funcs.inc` are gitignored and a hosted runner has no ROM to
  make them from. `tools/data-bundle.sh` now bundles them into `files.tar.gz` on
  a machine with the ROM (188 entries, 7.5 MB, plus `ogre-data.txt` recording the
  public commit), and `.github/workflows/release.yml` takes that archive from the
  private repository when the Actions variable `OGRE_DATA_REPO` is set, using the
  `OGRE_DATA_TOKEN` secret. With the variable unset the matrix keeps the
  self-hosted labels, so the existing path is unchanged. The hosted path also
  clones and patches the runtime and the RT64/plume submodules, which the
  workflow never did before: both root patches apply cleanly to a fresh
  `N64ModernRuntime` clone at `589bbf01` and the patched clone is byte-identical
  to `tools/N64ModernRuntime`, and `patches/rt64-ob64.patch` plus the two plume patches
  apply to pristine submodule copies. A full hosted run and the Linux Vulkan
  patch remain unverified. The Windows job failed at the RT64 patch step
  (session 89b): a Windows checkout writes CRLF into both `patches/rt64-ob64.patch` and
  the RT64 tree, and `git apply` then cannot match the two hunks whose last line
  has no newline (`\ No newline at end of file`) — `rt64_tmem_hasher.h:203` and
  `rt64_native_target.cpp:369`, the two failing hunks reported by the job. Root
  `.gitattributes` now pins `*.patch` to `eol=lf`, and the workflow's
  `apply_patch` passes `--ignore-whitespace` to both the reverse check and the
  apply. Verified offline on a CRLF copy of RT64 at `4337374` through the
  workflow's own function: the patch applies, a second pass reports "already
  applied", and the tree is byte-identical to the LF baseline including the
  missing final newlines. The rest of the Windows job is still unrun.
  See `docs/guides/app-build.md` → Releases.

- ✅ **The first hosted run after the CRLF fix got all three platforms past the
  patch step, and the logs name one cause each (session 89c).** macOS and Linux
  failed in `Build and package`, not in the patch step, and had failed there in
  the v0.1.0 run too. Linux: the bundle in the private repository carries
  AppleDouble `._*` members, which extract as ordinary files here, and
  `app/CMakeLists.txt`'s `*.c` glob compiled them
  (`BankAAFuncs/._funcs_0.c:1:2: error: stray '\5' in program`).
  `tools/data-bundle.sh` now sets `COPYFILE_DISABLE=1` and excludes `._*`, and
  the workflow's unpack step deletes any that an existing bundle carries.
  macOS: the runner is arm64, `librecomp/rsp_vu.hpp` includes `<sse2neon.h>` on
  arm64, and `ogrebattle64_rsp` — which does not link `ultramodern` — had no
  include directory for it, so `audio_ucode.cpp` and `njpeg_ucode.cpp` failed
  with `fatal error: 'sse2neon.h' file not found`. The library now includes
  `N64ModernRuntime/thirdparty/sse2neon`; this machine is x86-64, which never
  reads the header, so the bug could not show here. Windows: the runner's CMake
  picks the Visual Studio generator, which installs the static library as
  `SDL2-static.lib`, and the `sdl2-static` guard only accepted `libSDL2.a`; the
  recipe also built and installed Debug (no `--config Release`), whose name
  carries a `d` postfix. The guard now accepts both names, the build and install
  pass `--config Release`, and a failure prints the tail of
  `tools/SDL2-static/build.log` instead of hiding it. The next run got through
  the SDL2 build (`==> static SDL2 ready`) and failed one step later, in RT64's
  `find_package(SDL2)`: SDL2's CMakeLists installs its package config to
  `<prefix>/cmake` under MSVC (`if (WINDOWS AND NOT MINGW)`) and to
  `<prefix>/lib/cmake/SDL2` elsewhere, so the Makefile's `-DSDL2_DIR` pointed at
  a directory with no `SDL2Config.cmake`. The Makefile now passes
  `-DOGRE_STATIC_SDL2=ON` and `app/CMakeLists.txt` locates the config in either
  directory before RT64 is added. The same commit fixes the Visual Studio
  executable path (`build-dist/Release/ogrebattle64.exe`), defines
  `SDL_MAIN_HANDLED` and calls `SDL_SetMainReady()` so the app keeps its own
  `main` instead of SDL2main's, and points RT64 at the app's SDL2 on Windows
  (otherwise RT64 links a second, older bundled SDL2 2.26.3, shared, against a
  window the app created with the SDL2 it links). `dist-zip` falls back to
  `cmake -E tar --format=zip`, since Git Bash for Windows ships no `zip`.
  Verified: from-scratch macOS builds of the committed tree plus these changes
  package both archives, including one with SDL2 installed in the MSVC layout
  (`SDL_INSTALL_CMAKEDIR=cmake`); the regenerated `patches/rt64-ob64.patch` applies to
  pristine RT64 `4337374` and reproduces the working tree. The MSVC compile of
  the game code past RT64's configure has not been observed.
  See `docs/guides/app-build.md` → Releases and `docs/DECISIONS.md` (89c).

- ✅ **Releases publish, with one correctly-named archive per platform
  (session 89d).** The release job ran `gh release create --draft`, so a green
  run left the archives invisible on the repository's Releases page. It now
  publishes; a tag push always publishes, and `workflow_dispatch` keeps a
  `draft` boolean for a review-first flow. The collect step also copied both
  archive flavours to one matrix name, so macOS and Linux attached ZIP data
  under a `.tar.gz` name; the matrix now names the flavour each platform ships
  (`.tar.gz` macOS/Linux, `.zip` Windows) and the step copies only that file.
  See `docs/guides/app-build.md` → Releases and `docs/DECISIONS.md` (89d).

- 🔧 **Windows builds and links; only the zip packaging was left (session 89f).**
  The run after 89e reached `[701/702] Linking CXX executable ogrebattle64.exe`,
  bundled `dxcompiler.dll`/`dxil.dll`, and then failed in the workflow's Collect
  step: `dist/ogre-battle-64-recomp-windows.zip was not produced`. Cause:
  `tools/release-build.sh` ran `make dist-zip` only when `zip` was installed, and
  Git Bash for Windows ships no `zip`, so the Makefile's `cmake -E tar` fallback
  was never reached. The gate now also accepts `cmake`. Verified on macOS with a
  PATH of every tool except `zip`: `make dist-zip` writes a real ZIP and the full
  script produces both archives. See `docs/DECISIONS.md` (89f).

- 🔧 **The Windows job builds with clang-cl under Ninja (session 89e).** The
  run after 89d got through SDL2, the app configure and into the compile, then
  died with 37 `cl : command line error D8021: invalid numeric argument
  '/Wno-unused-variable'` (the app, the RSP lib and all 34 bank units, plus
  `ultramodern`). `cl.exe` cannot take the GCC/Clang spellings the code uses, so
  the Windows leg now uses Zelda64Recomp's configuration:
  `ilammy/msvc-dev-cmd@v1`, `ninja`, and
  `-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` through a
  new `CMAKE_ARGS` matrix field. The app's compiler guards key on
  `CMAKE_CXX_COMPILER_ID` (CMake reports `MSVC` for clang-cl too), the runtime
  patch guards `-Wno-unused-parameter` the same way, and four Windows-only
  compile blockers are branch-fixed: `create_window` returns RT64's
  `{ HWND, thread_id }`, the fingerprint uses `_pgmptr` instead of `readlink`,
  the weak symbol is dropped for `_MSC_VER`, and the POSIX `sigaction` crash
  handler is compiled out. The package carries `dxcompiler.dll`/`dxil.dll`,
  which RT64 loads at runtime. macOS builds with all of this pass; the Windows
  job has not run since. See `docs/guides/app-build.md` → Releases and
  `docs/DECISIONS.md` (89e).

- ✅ **The boot-Start save menu (scene `0x18`) renders (session 88).** Holding
  Start while the game boots takes the boot branch at `0x800721DC`
  (`*(u16*)0x800E79B0 & 0x1000` → pending scene `0x18`, else `0x09`), which
  reaches the **Controller Pak Menu** — the `Save / Load / Erase / Exit` screen
  the developer meant by *"a special menu for saves"*. In the port the scene
  entered and stayed **black**: its enter `func_8017BA60` chunk-DMAs unit H
  (ROM `0x712A0` → RAM `0x8019A7C0`) and then `jal 0x8019D67C`, but
  `config/config.toml` extends `func_8019D568` to `0xCB0`, swallowing `0x8019D67C`, so
  N64Recomp bound the call to `func_8019D568` and emitted the
  redirect-plus-early-`return` shape — the enter returned before the menu
  initialised (no display lists at all; every captured frame measured
  `mean=0 max=0`). Fix: `0x8019D67C` added to `make recomp`'s `cross_bank.py
  dispatch --only` list, so the call is a `LOOKUP_FUNC` into the resident unit H
  and the tail repair restores call-and-continue. Verified on the RT64 build with
  Start held (`OGRE_TAP_MS=150`): scene `0x18` at `t≈2.4 s`, `func_ovlH_8019D67C`
  entered, 436 display lists, every captured frame non-black (mean 83) —
  `docs/proofs/native-boot-start-controller-pak-menu.png`. This also answers the
  session-65 "puzzle" about scene `0x07`'s extra descriptor words: they are
  **scene `0x18`'s own descriptor** at `0x8018FDC0`. **The copy/backup function
  the menu offers is deliberately out of scope** (developer, session 88: *"this
  is a pc port, so it's low priority. will not be fixed now"*) — the render is
  the deliverable. See
  `docs/HANDOFF-2026-09-19-session88.md`.

- ✅ **Regenerating the recompiled code is now a first-class, verified path for a
  fresh clone — and the old recipe was quietly broken (session 87, third
  part).** Tested the way a contributor experiences it: the tracked tree checked
  out into a clean worktree (no generated code, because all 62 MB of it is
  gitignored) with only the third-party trees linked. The pipeline works, but
  the documented recipe led with `make recomp`, which needs the ELF from
  `make resplit` + `make` and whose cross-bank dispatch needs the bank records
  `make bank-recomp` writes — so on a fresh tree **15 call sites were left bound
  to the wrong bank, silently, with a successful build** (the sessions-41/45/55
  class; the fresh and working `RecompiledFuncs/` hashed differently). Fixed
  with **`make regenerate`** (the whole pipeline in the one order that works:
  `resplit → link → bank-recomp → recomp → rsp-recomp`, verified end to end and
  producing a byte-identical `RecompiledFuncs/`) and a **`recomp-prep`** guard
  that refuses to recompile when `bank_funcs.inc` is missing, naming the
  consequence and the fix (tested failing on a pristine checkout and passing on
  a regenerated one). The setup guide now leads with regeneration and records
  the two traps a contributor will meet: `make resplit` always leaves
  `asm/1CE040.s`/`asm/40E80.s` dirty (a `fix-labels` patch round-trip), and 26
  `Bank{B..L}Funcs/` files differ from a fresh regeneration only in
  `recomp_trace_return` depth constants (cosmetic; zero-line diff with those
  lines filtered). See `docs/HANDOFF-2026-09-19-session87.md` §8 Addendum 2.

- ✅ **Cross-platform release builds are wired up, and building for Linux found
  four portability bugs macOS had hidden (session 87, second half).** The
  developer's GitHub-Releases request turned up a blocker first: the recompiled
  code `make dist` links (`RecompiledFuncs/`, 34 `Bank*Funcs/`, `RspFuncs/`,
  `app/src/bank_funcs.inc` — 62 MB) is generated and gitignored, never tracked,
  so a fresh clone cannot build at all and a **GitHub-hosted runner cannot
  either** (it has no ROM to recompile from). Asked whether committing it adds
  copyrighted code to the client, the answer is that the generated C is a
  mechanical translation of the ROM — so it was **not** committed, and releases
  build on a machine that has the ROM (`.github/workflows/release.yml` runs a
  self-hosted matrix; the hosted `runs-on:` labels sit in comments for a
  one-line switch if that policy ever changes). Landed: `tools/release-build.sh`
  (single entry point, checks the generated code and names the commands),
  `packaging/README-dist.txt` (the shipped player README), a platform-generic
  `make dist` (`DIST_OS`, `EXE_NAME`, `dist-tar`), and the tag-driven workflow
  that creates a **draft** release with one archive per platform (session 89d
  publishes instead, and names the flavour per platform). Building for
  Linux in a container fixed four real bugs: **SSE4.1 was never enabled** for
  `librecomp`'s RSP SIMD path (Apple clang's default `-march` masked it; GCC
  fails on `_mm_shuffle_epi8`), a `const ssize_t n`/`size_t n` collision in the
  executable-fingerprint loop, an out-of-order designated-initializer array in
  `gbi.cpp` that GCC rejects, and Linux requesting `SDL_WINDOW_VULKAN`
  unconditionally — which broke the **null-renderer** build on exactly the
  machines it exists for. Verified under `ubuntu:24.04`: the null build runs
  launcher → ROM → boot → save beside the executable. Open: **Windows is
  written but unvalidated** (no MSVC/mingw here), and Linux+RT64 initializes
  Vulkan then segfaults in the first frame under Xvfb + Mesa `lavapipe` —
  undiagnosed, possibly a software-Vulkan artifact. See
  `docs/HANDOFF-2026-09-19-session87.md` §8.

- ✅ **The port is distributable: `make dist` builds a self-contained folder, and
  a fresh launch shows a start screen that asks for the ROM (session 87).** The
  developer's MVP ask: opening the program shows a black window reading
  `OGRE BATTLE 64: RECOMP` / `CLICK TO LOAD YOUR ROM (OR DROP IT IN THIS
  WINDOW)`, and running it creates the save file in the app's own directory.
  New `app/src/launcher.cpp` + `app/src/font.cpp` draw that screen with SDL's 2D
  renderer and a built-in 5x7 anti-aliased ASCII font (the app has no font
  dependency and the screen exists before RT64). A ROM can be supplied by
  clicking (nativefiledialog, already vendored by RT64), by dropping the file on
  the window, or by leaving it next to the executable (`find_exe_rom`); a
  rejected file is a message on the screen, not a process exit. **The config
  directory is now the executable's own directory** (`SDL_GetBasePath`), so the
  battery lands at `<app>/saves/ogrebattle64-us-rev1.bin` beside the executable;
  `OGRE_PREF_DIR` still overrides and a read-only install falls back to the
  per-user pref dir. A stored ROM makes later launches skip the screen
  (`OGRE_LAUNCHER=1` forces it, `OGRE_ROM=` names one). `make dist` produces
  `dist/ogre-battle-64-recomp/` — **one self-contained executable**: SDL2 is
  linked statically (`make sdl2-static` fetches and builds a pinned real SDL2
  into the gitignored `tools/SDL2-static/`, because Homebrew's macOS `sdl2` is
  the SDL3-based `sdl2-compat` shim and ships no static library), so
  `otool -L` shows only system frameworks, and SDL2's zlib license ships in the
  package. Verified by launching the static binary from an empty folder: start
  screen → ROM → game boots → `saves/` written beside the executable.
  `make dist-zip` packages it; `DIST_STATIC_SDL=0` falls back to bundling the
  shared library. Open: the native file picker itself has only been
  code-verified (synthesizing a click needs macOS accessibility permission) —
  the drag/drop, auto-scan, and stored-ROM paths are all exercised. See
  `docs/HANDOFF-2026-09-19-session87.md`.

- ✅ **The credits screen (scene `0x11`) plays — four size-override mis-bindings
  were leaking the frame-pump thread's stack (session 86).** The session-85 wall
  is closed: on the natural route the credits fade from black to "Ogre Battle
  64", scroll the staff roll over the game's backgrounds, show the **Chaos
  Frame** total (scene `0x13`) and return to the attract loop — developer
  confirmed ("it worked!! there were some artifacts in the rendered
  backgrounds, but you've done it!!"). The causes are all the session-41 class
  and all in **`config/config.toml`**, each producing a `jal`-into-a-body-interior
  early `return` or a dropped `jr ra` delay slot (so the caller's frame leaked
  and t4's saved comparator died): `func_801AB76C` `0x22C`→`0x4` (swallowed the
  real `func_801AB770`, the credits text drawer called by `func_801AB998`),
  `func_801AB568` `0x1E0`→`0x208` (its epilogue continuation dropped the
  `addiu sp,sp,56` delay slot, leaking `0x38` per call), `func_801B00D0`
  `0x544`→`0x4` (swallowed the real `func_801B00D4`, called by `func_801B1BBC`
  on the credits call tree) and `func_801AFC2C` `0x4A4`→`0x4A8` (its own delay
  slot, leaking `0x80`). Two throwaway static checks find the shapes: a `jal`
  emitted as call + `recomp_trace_return(); return;` with a duplicated delay
  slot, and a `jr $ra` emitted with no delay slot at all (before: 2, after: 0).
  **Correction to session 85: the forced `OGRE_SCENE=0x11` run does not execute
  the credits** — it plays the intro/opening even when the trace says
  `active=0x0011`; only the natural ending validates it. Verified from the
  developer's suspend save in front of the final boss
  (`assets/saves/suspend_final_boss.n64`, gitignored) at `OGRE_SPEED=8`: display
  lists continuous (`30250 … 34400+`), `D_800AEFA4` tracking `D_800C4BCC`, the
  fixed functions (`0x801B1BBC`/`0x801B00D4`) executing, 0 stubs/UNKNOWN,
  `check-banks`/`cross-bank-check` OK, both builds rebuilt. Open: artifacts in
  the rendered credit backgrounds; three latent leak-shaped `jal`s no reachable
  route executed (notably `func_80079BD8`'s calls to unit A's `0x801AB740`,
  which `cross_bank.py`'s `entry_looks_real` wrongly refuses because the leaf
  starts with `lbu`). See `docs/HANDOFF-2026-09-19-session86.md`.

- ✅ **The streamed-module hunt is exhausted: there is no un-recompiled module on
  any reachable path (session 85).** The developer's ask — locate a module the
  port has no record for and compile it — was run as a hunt, not a guess: two
  hand-played sessions (~2 h emulated; tutorial incl. mode 2, mission incl.
  battles, map, Organize, Load Game, attract routes) plus a forced sweep of every
  scene (`0x02,0x03,0x05..0x0D,0x0E..0x18,0x1A..0x1D`) produced **0**
  `UNKNOWN module`, **0** `UNCOMPILED streamed record`, **0** `streamed function
  stub called` and **0** stub-microcode tasks, and an offline diff of every DMA
  pair in the censuses (179 177 + 171 758 + 42 937) against both registration
  tables found **0** unknown ROM at a known module base and **0** arena-shaped
  load with a known-arena delta at an unknown base. Every registered module
  executed at least one function in run 1 (session 83's two 0% modules — units T
  and U — now both execute). **Nothing was compiled because nothing was
  missing.** Two bugs were fixed in the hunt's own diagnostics:
  `OGRE_SCENE_TRACE` dereferenced the wild scene-object word at the title→story
  handoff (`RT64Renderer::update_screen`, SIGBUS) and is now bounds-checked, and
  the window-close path now dumps both the DMA census and the whole
  `OGRE_DUMP_RDRAM` image (previously only `OGRE_EXIT_AFTER_MS` did, so a
  hand-played hunt lost them — the missing natural credits image is why). Open: a teardown
  crash `func_8007F8E4 + 0x2F8` **after** the census dump on window close, in
  both runs. The only "not compiled" backlog left is static and latent —
  `stubmap`'s 129 missing-function candidates and 288 `jalr` sites, none of which
  fired a stub in ~2 h of play + the full sweep. See
  `docs/HANDOFF-2026-09-19-session85.md`.

- 🧊 **The credits screen (scene `0x11`) freezes the frame pump, and it is not
  missing code (session 85, from the developer's report).** *(Fixed in session
  86 — see the entry above; the diagnosis below stands, and its forced
  `OGRE_SCENE=0x11` repro was later shown to play the intro, not the credits.)* At the end of the
  second hunt run the developer finished the last mission and the credits hung:
  the last display list is `#125090 at t=1173789 ms`, the scene becomes `0x11`
  at `t≈1174273`, and **no frame is submitted for 13 s** while the VI retraces on
  (`D_800AEFA4` frozen at 91/96, `D_800C4BCC` at 16 704 — session 60's
  signature). The credits window streamed **0 records** and hit **0 stubs /
  0 UNKNOWN**, so this is the frame/event path, not a missing module. The frame
  pump `func_8008AFE0` is parked in `osRecvMesg(0x800C4C28)`; with
  `OGRE_DEBUG_TRACES=1 OGRE_DEBUG_VI=1` the VI event still arrives —
  **1160 `retrace -> mq=0x800E8B84 msg=0x29A`** deliveries, t19
  (`func_80088F08`) parked on that queue — so the break is between t19's retrace
  handler and the pump's queue, and `osViSwapBuffer` is called only twice in the
  whole run. A clean A/B at the same trace and 20 s isolates it to the scene:
  **title `0x04` = 1159 retraces / 562 swaps / 64 display lists /
  `D_800AEFA4`=1153, versus credits `0x11` = 1160 retraces / 2 swaps / 3 display
  lists (boot) / `D_800AEFA4`=91 frozen** — the retrace arrives but the game
  stops swapping buffers, so no frame completes. **It is not the forced entry
  and not missing data:** a second forced scene under the identical trace
  (`OGRE_SCENE=0x14`, the Witch's Den) renders normally (1158 retraces / 533
  swaps / 61 display lists / `D_800AEFA4`=1152), and an `OGRE_DUMP_RDRAM` at the
  forced credits freeze shows the credits state fully initialised
  (`*(0x801B94D8)=0x801BA8A0`, state 4, count 3, entry array 0x801BA770 with a
  real callback `0x801ABCF4` — note `lui 0x801c` + a negative immediate wraps to
  `0x801B…`). So the freeze is **credits-specific**. Scene `0x11` is the credits
  (descriptor `0x8018FBAC`, mask
  `0x8000`, enter `func_80177F80` → overlay C `func_801AC944`, update
  `func_80177F9C` → the 6-state `func_801ACC24` on `*(0x801C94D8)`).
  `OGRE_SCENE=0x11` reproduces the same symptom in ~20 s without the
  playthrough (forced entry lacks pre-state — a caveat, not a proven identity).
  Next: walk the retrace from t19's handler through the event registry at
  `0x800F9178`, and checkpoint before the credits. See
  `docs/HANDOFF-2026-09-19-session85.md` §7.

- 🫒 **The attract story's olive background is diagnosed to a single bit (session
  84).** Scene `0x0B`'s background fill is the game's own
  `G_SETFILLCOLOR 0x4AC14AC1` (`FillRect rect=(0,0)-(1276,956)`) — RGBA16
  `(74,90,0)`. It is an immediate (`addiu a0,zero,0x4AC1`) at ROM `0x0EEBE0` =
  guest `0x801A1E80` in record 3, taken only when `*(0x80197C88)` has **neither**
  bit `0x2000` nor `0x1000` set; with either set the same code draws
  `0x4F00C308` = RGBA16 `(0,24,24)`, the near-black retail shows. The port reads
  record 2's *code* bytes there (`0x10400009`, both bits clear) because that
  address is inside record 2's code span, and the rect table it then reads at
  `0x800E7A36` (`0x0600,0x0140,0x0000,0x000D`) is equally wrong — it clamps into
  exactly those bands. A reverted probe **and** a hardware watchpoint proved
  nothing in the port writes either address (only the loader does), and the
  developer's retail screenshot of the same frame confirms the bands are the
  game's own 4:3 letterbox and that retail paints them **black**: the port paints
  exactly `(74,90,0)` (the `0x4AC1` arm), retail `(0,24,8)` (the `0x4F00C308`
  arm) — measured vs derived, see the handoff's colour table. Remaining work:
  read `*(0x80197C88)` from a *readable* reference state (`Mupen64Plus-Next`,
  not `ParaLLEl N64`, whose RZIP RDRAM base did not resolve) to say which address
  feeds the branch wrongly.
  See `docs/HANDOFF-2026-09-18-session84.md`.

- 🧰 **A stub-finder tool answers the whack-a-mole without a run (session 83).**
  `tools/stubmap.py` reads the runtime's own registration tables
  (`app/src/bank_funcs.inc` for the bank records,
  `RecompiledFuncs/recomp_overlays.inl` for the base sections — the only place a
  *reimplemented* function like `osSetIntMask_recomp` has an entry, since it has
  no generated body) and every `jal` in the generated C, then ranks each
  dispatched address by whether `get_function` can resolve it: **DEFINITE** (no
  module registers it at all — no run state can resolve it, the session-82
  shape; or the caller's own bank lacks it), **MISSING FUNCTION** (a bank that
  can be resident has a real prologue there but no entry — a split the
  disassembler could not find), **BANK SELECTION** (the missing bank's bytes are
  a body interior; scene data picks the bank), **INERT** (the scene-descriptor
  masks say the two records are never loaded together), plus `UNRESOLVABLE` and
  the RSP-IMEM window. Current build: **985 static targets, 0 definite, 0
  unresolvable, 129 missing-function, 152 bank-selection, 369 inert, 332
  resolvable** — every static `jal` target is registered by *some* compiled
  module. `stubmap.py log <run.log>` is the arbiter for a real hit: it matches
  the live words the runtime prints at the stub address against each candidate's
  ROM image to name the module that was *actually resident*, then says whether
  the bug is a load that missed `on_streamed_dma` or a wrong-layout dispatch;
  `pointers` lists data words (jalr / callback tables) that point at risky
  targets. Replayed against the pre-session-82 tree it marks `0x801D1508` and
  `0x801D0AAC` DEFINITE; on `ogre-s73-r5.log` it identifies the settings-menu
  stub as record 16 (unit AG) and on `ogre-live4.log` the shop-screen stub as
  record 9ab (unit AB). `make stubmap` / `make stub-check`. See
  `docs/guides/app-build.md` → "Diagnostics toolkit".
- 📐 **And "what is the recomp %?" is now four measured numbers (session 83).**
  `tools/recompcov.py` / `make recompcov`: **99.05%** of the ROM's code span
  (`0x2B11D0` of `0x001000..0x2B8B70`) is compiled, the one `0x6990`-byte gap
  (`0x0DDF80..0x0E4910`) being data with no loader in it; **28/28** loader-pattern
  arenas have a unit (44 bank records, 34 units, 10 base sections, 5037 function
  entries / 4963 addresses); **985** static dispatch targets with **0
  unresolvable** (288 `jalr` sites stay unmeasurable statically); and, per run,
  **execution coverage** — of the registered functions, how many a route actually
  entered. That last one needed a new knob: the runtime already counted every
  function entry for `OGRE_PROFILE` but only printed the per-second top 8, so
  `OGRE_COVER=1` now dumps the whole set at exit (and from the live console's
  `cover`), with the shadow call chain skipped so the run stays near normal
  speed. First measurements: a bare 20 s boot enters 408 functions; the scripted
  mission route (Load Game → map → mission, 180 s at `OGRE_SPEED=8`) enters 1601,
  i.e. **1593/4963 = 32.1%**; and **one mission played by hand enters 2213, i.e.
  2205/4963 = 44.4%** with **exactly two modules never entered at all** (unit T's
  `bankRec18b`, 54 functions, and unit U's `bankRec18c`, 7 — record 18's
  tutorial-practice banks). A module at 0% is the definitive "untested" signal;
  a module that *shares RAM with a sibling bank* has an upper-bounded count,
  because siblings register the same addresses, so the tool marks those `[s]`.
  See
  `docs/guides/app-build.md` → "Diagnostics toolkit".
- ✅ **Neutral encounters now spawn their monster (session 82).** The wild unit is
  created by the battle/setup fragment scene `0x0E`'s enter streams
  (**ROM `0x23A370`, `0xE80` → RAM `0x801D0860`**), which the port had no record
  for — `jal 0x801D0AAC`/`0x801D1508` hit the runtime's streamed stub, so the
  battle's participant table was never built and the battle began with an empty
  enemy side (instant victory). It is now **bank unit AH** (three forced entries,
  `bankRec10s`, code `0xE70` / data `0x10`). `tools/arenamap.py` had listed the
  fragment since session 73 as "no observed run streams"; the neutral-encounter
  path does. Reproduced with a temporary force-the-roll probe (since reverted by
  `make bank-recomp`): `[bank] UNKNOWN module rom=0x23A370 ram=0x801D0860` +
  `streamed function stub called @ 0x801D1508` → then **0 UNKNOWN / 0 stubs**,
  the wild Young Dragon spawns and fights. Proofs
  `docs/proofs/native-neutral-encounter-message.png`, `-battle.png`. The mechanic
  (fully decoded): `func_ovlN_801E7940` rolls per frame in mission state 0,
  picks a moving player unit, reads the tile's terrain and indexes two tables at
  `0x801ED780`/`0x801ED79E` in record 7's data for the class, stores
  `class + 0x100` at `0x801F0E24`, and enters the encounter state. See
  `docs/HANDOFF-2026-09-18-session82.md`.
- 🛠 **The cutscenes' flickering right/bottom line is fixed (session 81), with two
  RT64 changes.** (1) A game framebuffer is now sized from the **VI** rather than
  from the rectangle the frame happened to draw
  (`framebufferHeightForDisplay`, `tools/RT64/src/hle/rt64_framebuffer.h`, used at
  `rt64_workload_queue.cpp:435` and `rt64_state.cpp:551`/`:1294`) — the RDP snaps
  a copy/fill rect's fractional bits (`lrx |= 3; lry |= 3`), so frames whose draws
  stopped at row 238 produced a 239-row target while others produced 240/241;
  measured: the target is now **320x240 in 1361/1361 and 2465/2465** consecutive
  frames, and the stale-edge line on the black frames after a checkpoint `load` is
  gone. (2) The presenter shows only the **319x239 area the game actually draws**
  (`visibleFramebufferSize` in `tools/RT64/src/render/rt64_vi_renderer.cpp`,
  `OGRE_OVERSCAN=<0..8>` override, default 1 px on the right/bottom) — measured
  with `OGRE_DL_DECODE=all`: the game's own scissor and clear are
  `(0,0)-(1276,956)` = 319x239, so its drawing never touches the framebuffer's
  last column/row; those pixels keep leftovers from the boot's njpeg draws (only
  in `0x400`, per a dump watch at t=4..12 s), and the VI's three-buffer rotation
  toggled them against the other buffers' black — **120 of 120 consecutive
  settled-cathedral frames are now clean (was 15 of 40)**, the picture is intact,
  and `OGRE_OVERSCAN=0` restores the old image. A CRT's overscan hid the game's own
  off-by-one; the port now crops it too. Boot/title/load-game/map timelines and
  the intro/Magnus cutscenes are unchanged (all runs exit 0). Reference check
  recorded in the handoff (mupen64plus-core forwards raw VI registers; GLideN64
  and angrylion present a fixed window from `VI_ORIGIN`, where RT64 subtracts one
  row — a separate fidelity item, not this fix; GLideN64's `enableTexCoordBounds`
  has no RT64 equivalent). Both halves are now declared in `patches/rt64-ob64.patch` — it
  was **stale** and did not carry them (`rt64_vi_renderer.cpp` was missing from the
  patch entirely, so a fresh checkout applying it lost the fix); it was regenerated
  and verified to apply to a pristine `tools/RT64` HEAD, and `OGRE_OVERSCAN` is now
  in the `docs/guides/app-build.md` knob table.
- 🛠 **The mission's post-battle lag has a landed fix (session 80): N64Recomp
  no longer injects the blocking `yield_self()` into the mission's
  nearest-target search.** Session 79 diagnosed the lag as one recompiler
  artifact — `func_ovlN_801B2F4C`'s scan over the object list at `obj+0xA8`
  yields **once per candidate** at its backward branch `0x801B3008` because a
  body load (`lwc1 $f2, 8($s0)`) has a loop-invariant base, and `yield_self`
  blocks until an external message arrives (≈ one VI retrace, 16.7 ms), so a
  ~22-entity scan cost 0.4–0.7 s per frame (30 → 1.4 display lists/s). The fix
  is **scoped, not a heuristic rewrite**: `config/banks/config-bankN.toml` now carries
  `yield_work_loop_branches = [0x801B3008]`, a new N64Recomp option (see
  `patches/n64recomp-ob64.patch`) that says "this backward branch is a work loop, not a
  poll loop — emit no yield here". Regeneration changes **exactly one generated
  file** (`BankNFuncs/funcs_3.c`, one line removed) and **52 → 51 yield sites**;
  every other generated file is byte-identical, and the boot's scene timeline is
  unchanged (intro → publishers → title → tutorial → new-game → scene `0x03`).
  The three failed broad repairs of session 79 are why the fix is scoped to the
  one site: the boot deadlocks if the wrong yield is removed, so a one-site
  change in bank unit N (not resident at boot) cannot regress it. **Developer-
  confirmed:** driven to a battle on the tutorial-practice route at
  `OGRE_SPEED=8` (log `/tmp/ogre-lag-s80mission.log`, `--tag s80mission`), the
  mission runs normally — 0 seconds at ≤12 display lists/s, `0x801B2F4C` never
  hot, p50 6 ms / p99 31 ms display-list intervals over 129 s and eight mission
  crossings. See `docs/HANDOFF-2026-09-18-session80.md`.

- 🔎 **The mission's post-battle lag is diagnosed but NOT fixed (session 79).**
  The developer's *"after a battle, when the enemy unit's destruction effect
  ends, the game lags badly — it draws and responds, just very slowly"* is a
  recompiler artifact, not game logic and not the renderer. **N64Recomp's
  poll-loop heuristic injects a blocking `yield_self()` into the mission's
  nearest-target search** (`func_ovlN_801B2F4C`, bank unit N) at the loop's
  backward branch (`0x801B3008`, `BankNFuncs/funcs_3.c:55419`), so the search
  yields **once per entity**; `yield_self` blocks until an external message
  arrives (≈ one VI retrace, 16.7 ms). Measured: submitted display lists go from
  33 ms apart to ~717 ms (**30 → 1.4 lists/s**) inside scene `0x03`, while
  `processDisplayLists` stays at 4–6 ms and the log has **0 stub calls and 0
  `UNKNOWN module`** — so the renderer and the bank map are both healthy and the
  game thread is *waiting*. `OGRE_PROFILE=1` puts the frame-pump thread `t4` in
  `0x801B2F4C` for 93–96 % of the slow seconds. The heuristic accepts the loop
  because a load inside it (`lwc1 $f2, 8($s0)`) uses a base register set once at
  function entry, which really is loop-invariant — the loop is simply not a poll
  loop. **Three candidate repairs were built and all three stalled the boot** (the
  intro scene stops producing frames, `t4` blocked on its message queue), which
  establishes that `yield_self` is also the only external-message pump a running
  thread has and that a non-blocking variant is not sufficient. Everything was
  reverted; the tree and `tools/N64Recomp` are at their recorded baseline. Next
  step: the scoped experiment (that one site only) and the scheduler-ring
  comparison that explains the boot stall. See
  `docs/HANDOFF-2026-09-18-session79.md`; the measurement recipe is
  `tools/run-lag.sh` + `tools/proflog.py --names`.

- ✅ **`assets/saves/*.n64` can be played: they are DexDrive Controller Pak dumps,
  and the battery image is recoverable from them (session 78).** They are not
  battery dumps and hold no battery image at any offset (36928 bytes = a
  0x1040-byte DexDrive header + a 32 KiB pak), but each live 25-page note
  (`OgreBATTLE64 <n>`, the game's own copy/backup feature) carries a verbatim copy
  of a battery slot. The reason a straight copy was rejected — and the blank
  battery written in its place — is the **checksum seed**: `func_8007541C`
  validates a slot's two `u16` header checksums with the slot's **own device
  offset** as the seed (`func_80075A84` = byte sum, `func_80075B00` = set-bit
  count), while the pak-copy path seeds them with 0. `tools/sramsave.py` now
  extracts a pak's live notes into battery slots 0/1 and reseeds them
  (`import`, `pak`, `--all`), and both **`OGRE_SAVE=prologue`** (in-process, into
  the active config dir) and **`tools/run-save.sh prologue`** (per-save config dir)
  boot with one as the battery. Verified by the game's own
  data screen, not by the scene route: GAME DATA 1 reads `Magnus / Prologue / Alba
  / 0:20:42` for `prologue.n64` and `… / 0:07:02` for the older
  `save-mission-1.srm` import in otherwise identical config dirs (a blank battery
  reaches the same scenes, so the route alone proves nothing). See
  `docs/HANDOFF-2026-09-18-session78.md`.

- ✅ **The boot's publisher stills no longer lag (session 77).** Scene `0x0A`
  ("Licensed by Nintendo" → ATLUS → QUEST, the 3-colour 3D "q" logo) was drawing
  at **2 display lists/s instead of 30** because the RSP worker polled 500 ms per
  frame for framebuffers that screen never renders (`OGRE_SYNC_TRACE=1`:
  `[njwait] spins≈1940 found=0 ms=500` on every list). The poll was session 50's
  njpeg guard, which session 57 had already measured as unnecessary (the copy is
  ordered by the game's DP-completion wait plus `ogre_sync_framebuffers()`);
  deleted. Scene duration is unchanged (16.4 s), the frame rate is now 30/s, and
  the njpeg regression still reads its four documented frames. **The same stall
  hit every scene that does not render into those three addresses**, so the fix
  also removed slowdowns elsewhere (developer-confirmed). See
  `docs/HANDOFF-2026-09-18-session77.md`.

- ✅ ROM identified: `Ogre Battle 64 - Person of Lordly Caliber (USA) (Rev A)`,
  40 MB dump, 16-bit byte-swapped (`.n64`). Converted to big-endian `.z64`.
- ✅ Cart header decoded (official N64 layout): entry point `0x80070C00`,
  internal name `OgreBattle64`.
- ✅ Boot stub disassembled: clears BSS `0x800AEDB0..0x800E9C20`, stack at
  `0x800C6D60`, jumps to `main` at `0x8007F880`.
- ✅ splat-based disassembly (see `config/config.yaml`): main code segment ROM `0x1000..`
  mapped at `0x80070C00+`; ~185 KB of code split into `asm/`.
- ✅ ELF built (`make`): `build/ogrebattle64.elf` with relocations
  (`--emit-relocs`) and correct vram/ROM section mapping.
- ✅ **Full main-segment recompilation succeeds**: 807 functions recompiled to C
  (`RecompiledFuncs/`, regenerated with `make recomp`), and the generated C
  **compiles** against the runtime headers.
- 🚧 **Phase 3 (first boot app) in progress**:
  - ✅ `app/` CMake project created (ultramodern + librecomp + RT64 + SDL2 +
    `RecompiledFuncs`); **builds** on Linux (Ubuntu).
  - ✅ RT64 added as submodule (`tools/RT64`); all RT64 contrib submodules
    initialized.
  - ✅ Game entry wired: `rom_hash=0xbe6adaa5c3f8f7a9`, entrypoint
    `0x80070C00`/`recomp_entrypoint`, base overlays + streamed-code stubs.
  - ✅ Platform callbacks: SDL2 window/input/audio, error box, threads.
  - ✅ **Null renderer + stub RSP**: first boot validation — the game now
    **boots without crashing** (2026-08-24, session 3): 7 N64 threads start,
    the VI thread swaps buffers at ~50-60 Hz, and the game runs its init.
  - ✅ **Libultra bridging (first 3 batches)**: 24 libultra functions named in
    `config/symbols/symbol_addrs.txt` so the runtime's native `osXxx_recomp` services replace
    OB64's verbatim libultra (see `docs/LIBULTRA-BRIDGING.md`). Also fixed a
    runtime bug in the initial 1MB DMA (sign-extension of the entrypoint).
  - ✅ **Libultra bridging (batches 4-6, session 4)**: osCont family + timers +
    event registration (`osViSetEvent`/`osSetEventMesg`) + `osSpGetStatus`
    named; corrected `osSendMesg`/`osJamMesg` (the game's primary send is
    `func_80093810` = `osSendMesg`). Added an external-message **drainer
    thread** to the runtime (blocked game threads otherwise deadlock waiting
    for VI/SP events). The game now boots cleanly, **submits its first RSP gfx
    task**, and sets a **real VI mode** (see `docs/LIBULTRA-BRIDGING.md`,
    session 4).
  - ✅ **RT64 renderer (session 5)**: the null renderer is replaced by a real
    RT64/Vulkan renderer (`app/src/renderer.cpp`, window created with
    `SDL_WINDOW_VULKAN`). It initializes, presents frames at ~60 Hz, and the
    game boots through it (see `docs/HANDOFF-2026-08-25-session5.md`).
  - ✅ **Boot stall diagnosed & fixed (session 6)**: the "stalls waiting for a
    second task" was wrong — the game thread busy-spins (`func_80089A10`) and
    starves the runtime's external-message drainer. Fixed by reimplementing the
    spin as a yielding wait, reimplementing the game's DMA-request path
    (`func_8008BC40`, whose PI-manager globals were dead) as a synchronous ROM
    read, and fixing DMA byte order (must use `recomp::do_rom_read`). The game
    now boots through the RSP pipeline, controllers, and 700+ streamed-overlay
    DMA loads into its main loop (streamed functions stubbed). **Next wall:
    streamed-overlay relocation/recompilation = Phase 4.**
    (see `docs/HANDOFF-2026-08-25-session6.md`).
  - ✅ **Phase 4 started — streamed overlays A+B+C recompiled (session 7)**:
    - ✅ Determined the overlay format: each streamed overlay is **plain linked
      MIPS code+data** DMA'd to a **fixed** RAM address; pointers inside are
      already absolute for that address (no runtime relocation pass). The
      boot-resident overlays are A (ROM 0x3F1B0 → RAM 0x800E9C20) and B
      (ROM 0x40E80 → RAM 0x8016AF80); a third, C (ROM 0x1CE040 → RAM
      0x80197B90), is loaded on-demand by the game's streamed loader (segment
      table at ROM 0x387C0).
    - ✅ `config/config.yaml` now disassembles A/B/C as code segments (+ data at
      0x40640/0x5C230/0x1EE540); the bin gaps (0x66E30, 0x1F0A00) are pinned to
      their ROM address so they don't occupy RDRAM VMA space. Overlay C needed
      `bss_size` + a `.bss` subsegment for its 0x801BA550 tail.
    - ✅ Recompiled together: 807 → ~1520 recompiled functions. Cross-overlay
      absolute references (e.g. overlay B taking the address of overlay C's
      `.L8019EE70`) are fixed by `tools/fix_cross_overlay_labels.sh`, wired into
      the Makefile so it re-applies after every `splat split`.
    - ✅ The app registers A+B+C via `load_overlays()` from the GameEntry
      on_init callback (the runtime's `recomp::init()` now loads only the base
      sections — a full 1MB load would register the overlays at wrong
      addresses). **A DMA-hook approach was tried and reverted**: `load_overlays`
      assumes whole-overlay DMAs, but OB64 streams in 0x200-byte chunks, which
      produces an inverted bound range.
    - ✅ Boot now passes the old `func_80075BC0` function-pointer-table crash
      (the entry is a real recompiled overlay-B function that returns a valid
      descriptor), and the game runs ~1520 real functions to load the next
      overlay's data blocks before **spinning on N64 threads 1+3** (≈200% CPU).
      No stub calls and no `get_function` hard-fail in the whole boot.
    (see `docs/HANDOFF-2026-08-25-session7.md`).
  - ✅ **RT64 GBI question resolved (session 10)**: OB64's DLs are F3DEX2-compatible — the first gfx task DL is `0xDE`=G_DL → `0xA9EF0`, `0xE9`=FULLSYNC, `0xDF`=ENDDL, and RT64's hash-based auto-detection (`getGBIForUCode`) already matches the ucode to `GBIUCode::F3DEX2` (ucode enum 7), which parses them correctly. The session-9 "force F3DEX" override was wrong (plain F3DEX does not map 0xDE/0xDF) and was removed. No RT64 changes needed. (see `docs/HANDOFF-2026-08-29-session10.md`).
- ⬜ Phase 4 next walls (session 8 findings):
  - ✅ **The post-boot spin is FIXED** (session 8): Thread 3's `.L80075FB8`
    pure spin on `D_800C4C26` starved the cooperative scheduler's drainer, so
    the VI-retrace events that advance the boot state machine were never
    delivered (same class as session 6's `func_80089A10`). N64Recomp now emits
    `yield_self(rdram);` for poll loops (backward call-free store-free branches
    reading a loop-invariant address), so the game thread yields and the
    drainer can run. Boot now progresses through the overlay-D loads, the
    controller path, and the RSP pipeline into **title-screen display-list
    building** (overlay C `func_801A1FCC` → `func_8019FC68`).
  - **Next crash: `func_8019FC68` at 0x8019FFF4** — stores through
    `0x803ffa7b + entry->[0x34]` where a graphics-object record in the heap
    array at `0x803fefa0` (`D_801B81D0`) has a garbage `[0x34]` (varies per
    run) → address wraps → SIGSEGV. Array is zeroed at alloc; root cause open
    (leads in `docs/HANDOFF-2026-08-25-session8.md`).
  - ✅ **The `func_8019FC68` crash is FIXED (session 9)** — root cause was a
    recompilation bug, not the game: OB64's KMC compiler merges identical
    epilogues into a separate symbol the preceding function falls through into
    (e.g. `func_8019ABC4` falls into `func_8019AF0C`), and N64Recomp never ran
    the shared epilogue, so callee-saved registers and `$sp` were left
    clobbered. N64Recomp now emits fall-through tail calls
    (`next_func(rdram, ctx); return;`) when a function's code falls off the end
    into the next function, and processes its static functions to a fixpoint
    (registering them by address). See
    `docs/HANDOFF-2026-08-25-session9.md`.
  - ✅ **Boot-stall root-caused & fixed (session 15)**: the session-14
    "VI-retrace message-queue deadlock" was a misread — the real cause was a
    **regression**: the session-8 `yield_self` poll-loop emission was missing
    from the vendored N64Recomp fork (reset to upstream; patch lost the hunks),
    so thread 3's spin at `0x80075FB8` starved the cooperative scheduler's
    drainer. Also fixed: the scheduler threw "No threads left to run" when all
    game threads blocked (now idles on external messages), `os*Mesg` drained
    the entire external-message backlog per call (now capped at 32), and the
    debug traces slowed the headless browser ~100× (now gated behind
    `OGRE_DEBUG_TRACES=1`). The web build now boots to a **stable, healthy
    idle state** — no crash, no deadlock, all queues draining at 60 Hz.
    **Next wall (game behavior):** thread 3 blocks on its own count-1 queue
    `0x800C6C98` (sender unknown — audio/AI path?), so the frame-producer
    thread (`func_800893C0` → `func_8008949C` → `osViSwapBuffer`) never runs
    and the game submits only the boot blanking DL. See
    `docs/HANDOFF-2026-08-31-session15.md`.
  - ✅ **First real game frame rendered in the browser (session 19)**: the
    WebGL2 renderer prototype was the only thing blocking rendering. Four bugs
    fixed in `app/src/web_renderer.cpp`: (1) `ogre_gfx_flush()` blocked the
    browser **main thread** on the renderer mutex while `send_dl()` held it for
    the whole DL analysis/execution — a contended pthread mutex cannot be waited
    on by a browser main thread, and this hard-froze the page the moment the
    title path advanced; (2) `queue_triangles()` re-locked that non-recursive
    mutex (already held by `send_dl`) and self-deadlocked on the first real draw
    command, permanently holding the lock; fixed structurally by recording into
    an execution-local buffer and publishing under a short lock; (3)
    `read_matrix()` decoded the N64 `Mtx` as 16 consecutive `s16` instead of
    4x4 `s16` integer parts + 4x4 `u16` fraction parts, so most matrices were
    all-zero and **every 3D triangle was rejected**; (4) segment registers stored
    the full base instead of its high byte. Also fixed R/B swap in
    `G_SET*COLOR` and the shader's `COMBINED` combiner input. Result: the title
    DL executes, its draws reach GL, the canvas shows real (brown/orange) game
    content, and the game submits frames continuously. **Project-model
    correction:** the sessions 16–18 "cb88 NULL frame-2 wall" is a red herring
    (that is the boot blanking task; steady-state frame tasks use key 4 →
    `cb84 = osViSwapBuffer`, enqueued by `func_80073AE4`, gated on
    `D_800E810C`), and the "heap-walk wedge" was a downstream symptom of the
    frozen main thread + deadlocked mutex. **Next:** implement
    `G_MOVEMEM MV_VIEWPORT` (still a no-op, so 3D geometry is partly
    off-screen), then regenerate the combiner mux tables from RT64's
    `rt64_color_combiner.h`. See `docs/HANDOFF-2026-09-10-session19.md`.
  - ✅ **The title scene renders in the browser (session 20)**: seven
    `app/src/web_renderer.cpp` bugs (viewport no-op, `Mtx` column-pair swap,
    ignored projection `MUL`, wrong modelview `MUL` order, F3DEX2's inverted
    `G_MTX` PUSH bit, `push_back(vector.back())` UB, empty/inverted rectangles
    drawn as full-screen covers) took the first title DL from 18/24 triangles
    rejected and a black canvas to 0 rejected and a stable ring of twelve
    sprite soldiers. The on-page console was also replaced by a bounded
    `window.ogreLog` ring buffer (per-line DOM writes were throttling the
    emulator). See `docs/HANDOFF-2026-09-10-session20.md`.
  - ✅ **The title sprites are decoded the way the RDP samples them (session 23)**:
    five decode bugs kept the title scene looking like scrambled slivers. Every
    `Vtx` field was read at its *logical* offset out of the runtime's
    byte-reversed rdram (the accessor rule is `off ^ 2` for halfwords), so the
    renderer actually decoded `(y,x) (flag,z) (t,s) (a,b,g,r)` - the sprite
    quads were transposed and their texture coordinates with them. `G_TEXTURE`'s
    scale (`sc = 0x8000` -> `s/64`, not `s/32`) was ignored; I8/I4 texels did not
    carry their intensity into alpha, so the `TEXEL0_ALPHA` mask was constantly
    1; the blend was guessed from `othermode_l & 0xFFF` with a heuristic that
    read the wrong bits (OB64's sprites are a two-cycle `FORCE_BL` XLU, so
    blending was off); and the texture was decoded with the *load*'s
    format/rect instead of the *render tile*'s (OB64 loads its 32x34 I4 mask as
    16 bytes/row of 8-bit texels). `Blender` now mirrors `rt64_blender.h` field
    for field and `ensure_tile_image()` decodes each tile's rect at the tile's
    format with `line * 8` as the stride. Verified by two new probes:
    `textures.cjs` (dump the decoded images and their `colour x mask`
    composites) and `spritecheck.cjs` (render one sprite and compare it with
    that composite) - they now match. `boot.cjs`: 21 680 non-black / 17 764
    colorful, twelve readable characters. `testdraw.cjs` was also repaired: its
    scratch address resolved through a segment to zeroed memory and its combiner
    word evaluated to 0, so the isolation probe had been drawing black; it now
    shows the synthetic 2x2 texture's four quadrants. A follow-up found the
    real reason the sprites looked like streaked columns: `G_FILLRECT` and
    `G_TEXRECT` take `lrx/lry` from **w0** and `ulx/uly` from **w1** (SDK and
    RT64), and the renderer had the two words swapped - so every full-screen
    rectangle decoded as inverted, the session-20 empty-rect guard dropped it,
    and **the game's per-frame clear never ran: frames accumulated on the
    canvas**. The intro also fades in from black through a full-screen
    PRIM-alpha rect drawn after the sprites, so the first rendered frame is
    black and its pixel counts are not a fidelity measure. Geometry, UVs and
    tile sizes are constant over 48 display lists. Against the reference
    screenshot from a real session the scene's layout now matches; one sprite's
    rendered pixels still do not match its texture pair (see the handoff's
    open item 0). Next: the idle trajectory (unchanged -
    ~half of all boots never build a real display list), a reference frame to
    compare against, and the remaining combiner inputs
    (`NOISE`/`K4`/`K5`/`LOD_FRACTION`/keys). See
    `docs/HANDOFF-2026-09-11-session23.md`.
  - ✅ **The title sprites render correctly (session 24)**: the "green smudges"
    over the soldiers' helmets and the dim colour cast had two independent
    causes, both now fixed and both verified against the sprite's own
    `colour x mask` composite. (1) **Texture decode**: every texture byte was
    read at its *logical* offset out of the runtime's byte-reversed rdram, which
    returns a different byte of the same word - for the RGBA16 colour tile that
    swapped every horizontal texel pair, and for the I4 alpha mask it reversed
    the four bytes of every word (8 texels per group), landing the mask's alpha
    in the wrong places. RT64's `loadWord` (`RDRAM[(textureAddress + i) ^ 3]`) is
    the reference; `n64be16` now sits beside `n64h`/`n64b` and `OP_LOADTLUT` uses
    it too. (2) **Blender**: cycle 0 of the sprite render mode
    (`b0=[P=CC M=CC A=CC_A B=ONE]`) is `(P*a + M*b)/(a+b)` with `P == M`, i.e.
    exactly `P`, but the port of RT64's `Blender::runCycle` folded the numerator
    with `fmod(..., 1+8/255)` unconditionally. The fold is per channel, so a
    bright orange pixel at mask alpha 0.5 came out green - the smudges - and
    opaque pixels came out about half brightness. The fold is now skipped when
    `P == M`, where the division cancels it exactly. The viewport, which was
    correct only because it cancelled the same byte swap by hand behind a comment
    that described the hardware wrongly, now reads through `n64h` with the
    canonical `(x, y, z, 0)` indices. The settled canvas shows twelve readable
    soldiers where it used to be scrambled slivers. See `docs/DECISIONS.md`
    (session 24) and `docs/HANDOFF-2026-09-11-session24.md`.
  - ✅ **The native RT64 build runs on macOS again (session 24, native
    bring-up)**: the web build had become the project's only renderer
    (`web_renderer.cpp` 3 355 lines vs `renderer.cpp` 357, and sessions 19-24 all
    web work), so the native path was brought up before committing to a Linux
    move. The only blockers were two bugs in the app's own SDL glue, both
    macOS-only: `create_window` handed RT64 the `SDL_Window*` where plume casts to
    an `NSWindow*` (instant `EXC_BAD_ACCESS`) and left the `CAMetalLayer*` null -
    the `SDL_Metal_GetLayer` segfault it was stubbing around does not reproduce
    under sdl2-compat 2.32.70; and `poll_input` called `SDL_PumpEvents` from the
    game thread, which Cocoa rejects with `NSInternalInconsistencyException` (2 of
    3 runs aborted there). RT64 now initialises Metal (`api=3`), the window opens,
    and 3/3 runs survive. `docs/guides/linux-migration.md` is revised to
    "optional" accordingly - Linux is a preference (Vulkan is RT64's best-tested
    backend), not a requirement. **The game-side stall is unchanged and
    platform-independent**: every native run submits only its boot blanking
    display list, so the canvas stays black unless input is fed (the web probes
    press Enter every 5 s for exactly this reason). See `docs/DECISIONS.md`
    (session 24 native bring-up).
  - 🚧 **The boot thread's real stack is now readable, and the renderer half is
    proven (session 26)**: the previous "t4 blocks on `0x800C4C28`" was an
    artifact of the snapshot view - that queue *is* fed (the send trace shows
    `func_800891A0 → 0x800C4C28` dozens of times). The recompiled code now emits
    an entry hook (with `ctx->r31`, the caller's return address, and the
    function's name) and a return hook, and the runtime keeps a per-thread
    shadow call chain; `OGRE_CHAIN_HISTORY=3` shows the boot thread going
    `func_80071EB0 → func_8008A1B0 → func_80089660 → func_80089804` and never
    returning from the **frame submit**. Two further results: the session-15
    `func_80089A10_recomp` "yielding spin" fix is **dead code** (the recompiled
    spin is a strong symbol and direct calls link to it, so the boot thread
    really does execute `while (D_800E79A4 != 0);` in a cooperative scheduler);
    and a hand-built F3DEX2 display list (`OGRE_SYNTH_FRAME=1`,
    `app/src/synth_frame.cpp`) now reaches `send_dl`, where RT64 parses and
    executes every command (seven `setFillColor`s, seven `fillRect`s with the
    right colour image) - the window still shows black, so the remaining gap is
    presentation, not parsing or drawing. See
    `docs/HANDOFF-2026-09-12-session26.md`.
  - ✅ **The game renders natively (session 28)**: the boot thread was not stuck
    in the game - it was **stranded by a scheduler bug in the runtime**. A thread
    parked in `wait_for_resumed` could consume a *message-queue poke* as if it
    were a handoff (both used the same `running` semaphore), so two game threads
    ran at once, the cooperative scheduler's invariants broke, and the priority-10
    boot thread was eventually left parked with an empty running queue while the
    per-VI service threads (pri 50-120) ping-ponged. Found with a new race-free
    scheduler event ring (`OGRE_SCHED_TRACE=1` -> `[sched-ring]`, with a global
    sequence number and the running queue after each event). Fixed by splitting
    the semaphores (`running` = strict handoff, `poke` = idle wake) and by
    refusing `NULL` queues in the queue helpers; the VI thread's queue snapshot
    also stopped dereferencing wild KSEG0 values (its guards now use the game's
    RDRAM window, `0x80000000..0x80800000`, not all of KSEG0 - that was an
    `EXC_BAD_ACCESS` on 3 of 4 input runs). Result: 6/6 no-input and 4/4 input
    runs reach the frame loop and submit the game's real display lists
    (`0x801C1520`/`0x801C80A0`, ~2.5/s) instead of only the boot blanking list,
    and the presented swap chain shows the **title screen**:
    `docs/proofs/native-game-title.png`. See
    `docs/HANDOFF-2026-09-12-session28.md`.
  - ✅ **The title runs at the game's 30fps (session 29)**: the "low, inconsistent
    frame rate" was the recompiler, not the game or the renderer. N64Recomp's
    poll-loop heuristic emitted `yield_self` inside ordinary loops: it excluded
    the branch **delay slot** from the loop body (so `bne ..., head` /
    `addiu $v1, $v1, 0x13C` looked loop-invariant) and treated a load's
    destination as a non-write (so a pointer chase looked like a spin on a fixed
    address). The game's malloc free-list walk (`func_80071A3C`), the allocator
    family and `func_80081B08`'s 36-entry object scan each yielded once per
    element; with `yield_self` sleeping for the next VI retrace (~16.7ms) that
    turned a microsecond loop into hundreds of milliseconds, so the frame rate
    was 2-8fps with 0.3-2.8s stalls. Fixed in the heuristic (include the delay
    slot; treat a load destination as a write unless the load's base is a
    `lui`-set constant) - `yield_self` fell 99 -> 14, all 14 being real poll
    loops (`func_80089A10`, `func_80075BC0`, ...). Result: **171 display lists in
    6.2s, median inter-frame gap 34ms (~30fps), no stall over 500ms** (was 8
    display lists in 8s). See `docs/HANDOFF-2026-09-12-session29.md`.
  - ✅ **The title's object table is populated (session 30)**: the "cube never
    falls" diagnosis was a recompilation bug, not a missing overlay. State 9's
    init calls `jal 0x801A1034`, and splat had emitted the *prologue* of
    `func_801A103C` as its own symbol, so N64Recomp compiled a 2-instruction
    stub that returned immediately and the 13 texture records at `base+0x6C0`
    (11 soldier objects + record 10, the object the script walks downward = the
    fall) were never created. `config/config.toml` now overrides
    `func_801A1034 = 0x13C`; with it all 13 records exist with real object
    pointers, `flag=255` on 0-10, and record 10 animating (`x=124 y=-144`).
    `OGRE_SCENE_TRACE=1` shows this live. See
    `docs/HANDOFF-2026-09-12-session30.md`.
  - ✅ **The intro no longer crashes; the game renders continuously (session
    31)**: the `func_801A1FCC` SIGSEGV was a stack-frame defect, not a missing
    scene. Two N64Recomp bugs, both now fixed:
    1. a `jal`/`j` into a body extended by `function_sizes` was still compiled
       and called as a standalone symbol, so `func_80198D28` (a false split
       inside `func_801989AC`) ran `func_801989AC`'s epilogue without its
       `0xB8` frame — leaking 0x10-0x38 of stack per call and corrupting every
       caller's saved `$s3` (the crash's `$s3 = 0x80000318`). Such targets now
       tail-call the containing function (24 sites redirected);
    2. a body whose last instruction is not a control transfer
       (`static_16_801984BC` → the shared epilogue `static_16_801985D8`) ran
       off its end instead of tail-calling the next function. Now emitted as a
       tail call + `return` (1726 sites), which unwinds the frame.
    Session 30's over-wide `func_80197C94` override (`0x948`, which swallowed
    the real function `func_801980A0`) is also corrected to `0x40C`. Result:
    **8s run exits 0 with 205 display lists** (was SIGSEGV at display list 1,
    t≈845ms), a 15s run reaches 395 display lists at t=14.2s (~28fps), and the
    per-frame lists carry real geometry (12-17 triangles, 27-32 `SETTIMG`).
    See `docs/HANDOFF-2026-09-12-session31.md`.
  - ✅ **A visual proof of the intro exists (session 31)**:
    `docs/proofs/native-intro-impact.png` (t≈3.3s: twelve soldiers, impact
    smoke, the block falling between them) and
    `docs/proofs/native-intro-title.png` (t≈8.4s: the N64 logo over the title
    screen), both swap-chain readbacks
    (`OGRE_CAPTURE_PRESENT=… OGRE_CAPTURE_AFTER=…`). Confirmed by eye: soldiers,
    falling block, block → N64 logo, title screen.
  - ✅ **The title screen's white band is fixed (session 35)**: the retail title
    screen has slowly scrolling clouds behind the logo; the port drew a solid
    white band across rows 64-128. The band was not a shader or a 3D/2D
    compositing bug: the game ends each cloud-layer strip loop with a tile that
    covers **zero texels** (`SETTILESIZE uls=0 ult=420 lrs=1276 lrt=420`, i.e.
    `lrt == ult`) and then draws six right-to-left `G_TEXRECT`s with the
    `RGB = ONE, ALPHA = TEXEL0` combiner (`FCFFFFFF FFFF73B9`). RT64 has to
    round an empty sampling rectangle up to one texel (an empty texture cannot
    be decoded), so those rectangles sampled one stale TMEM row - fully opaque
    in the layer-2 texture - and composited as opaque white. `RDP::drawTexRect`
    (and the browser renderer's `draw_texrect`) now skip a rectangle whose
    configured tile covers no texels, which is what the hardware shows: the
    retail title screen has clouds there and no band. Proofs:
    `docs/proofs/native-title-band-before-after.png` (crop) and the refreshed
    `docs/proofs/native-intro-title.png`. See
    `docs/HANDOFF-2026-09-14-session35.md`.
  - ✅ **Scene jumps work, and the title fog is fixed (session 36)**:
    `OGRE_SCENE=<name|hex>` now actually enters a screen. It had been a silent
    no-op — the poke has to land before the boot enters its first scene
    (~1.1 s), but `OGRE_SCENE_AFTER_MS` defaulted to 3000 — and
    `OGRE_SCENE_LOG=1` SIGBUSed on a stale scene-descriptor word at ~1 s. Both
    are fixed (`OGRE_SCENE_AFTER_MS` default 0, descriptor validated, new
    `OGRE_SCENE_TRACE=1`), and the scene table is now verified by capture:
    `title` = `0x04` (logo + PRESS START over the clouds), `intro` = `0x09`,
    `publishers` = `0x0A`, `story` = `0x0B`, `unit-info` = `0x0C`; `0x18`/`0x02`
    still crash on the unfinished cross-bank work. With the title reachable, the
    reported `native-title-fog-isolation.png` (a crop of the story screen) could
    be re-measured, and the `OGRE_FOG` repair turned out to be drawing nothing:
    its `G_SETTILESIZE`/`G_LOADTILE` extents were in texels rather than the GBI's
    quarter-texels (a 64x64 image declared as a ~16x16 tile), and only the first
    white group per frame was repaired, so the `©1999 QUEST` bottom sweep kept a
    dead tile. A follow-up round fixed the fog blinking (the sweep's scroll ran
    past the 64-texel image and the group was dropped; the draw tile now wraps).
    The fog's level is the game's own asset: the combiner is
    `RGB=ONE, ALPHA=TEXEL0`, so the overlay is white modulated by the layer
    image's intensity (5.1% mean / 20.4% peak) and `OGRE_FOG_SCALE` defaults to
    100 — a retail emulator capture measures the port at 113% of the retail fog
    contribution in the clean `©1999 QUEST` cloud region. Proofs:
    `docs/proofs/native-title-fog-isolation.png`,
    `docs/proofs/native-title-fog-quest-region.png`,
    `docs/proofs/native-title-band-isolation.png`. See
    `docs/HANDOFF-2026-09-14-session36.md`.
  - ✅ **New Game no longer crashes; the game reaches scene 0x0D (session 37)**:
    title → Start → New Game died in scene `0x02`'s update (`jal 0x80198D28`
    bound to the containing overlay-C body, reading N64 address 3 with `$a0`
    unset). That one call site is now dispatched through the bank map
    (`cross_bank.py dispatch --only`, wired into `make recomp`, plus a repair
    of the recompiler's tail-call emission at the site), resolving to bank
    unit E's real entry. Scene `0x0D`'s record 4 and record 14's two arena
    modules are newly compiled into unit C (five `function_sizes`), so the
    scene loads 9 bank records with zero stub calls. It then dies in list
    management (`func_80071950`) on a wild data pointer — the next wall,
    reproducible with no input at 1× speed. `OGRE_NO_AUDIO=1` separately
    crashes in early boot (queue-snapshot diagnostic). See
    `docs/HANDOFF-2026-09-14-session37.md`.
  - ✅ **Scene 0x0D init: two more dispatches, record-BSS zeroing, and the
    residue wall (session 38)**: the forced new-game run died in stale
    overlay-C code (`jal 0x801AFC2C` → `func_801AFAF4` while record 10 owns
    the address) — dispatched to bank unit C's real entry, same for
    `jal 0x801980A0` (fragment → unit E's real entry). Record loads now zero
    their segment-table BSS (the port never did; `0x0D`'s list anchors live
    in record 10's BSS). The remaining forced-`0x0D` crash is missing
    pre-state, proven by flag lifecycle probes: `0x0D` reads init flag
    `0x8019F794 == 0` because `0x02`'s single-frame writer never runs, while
    natural boot leaves that region nonzero — so the natural path
    (title → menu → `0x02` → `0x0D`) is required, and it is blocked at menu
    `0x18`'s pre-existing unit-D crash. See
    `docs/HANDOFF-2026-09-14-session38.md`.
  - ✅ **New Game plays into 0x0D and the natural path loops cleanly (session
    39)**: two more false merges fixed — `func_8019C5D4`'s `0x5D8` override
    swallowed the alternate entry `func_8019C69C` (menu `0x18`), and
    `func_ovlC_8023BDA8`'s `0x54` override merged a malloc-tail head with a
    shared-epilogue shim (the `0x0D` list-unlink crash). Splitting the latter
    exposed a latent N64Recomp bug (a body ending in `jal` with fall-through
    dropped its `after_N` label); fixed in `recompilation.cpp` and the patch
    regenerated. The natural tap path is now title → `0x02` → `0x0D` → `0x02`
    → `0x0D`, 0 stub calls, stopping at a NULL into `func_800988A0`. See
    `docs/HANDOFF-2026-09-14-session39.md`.
  - ✅ **0x0D re-entry characterised, and scripted-run tooling (sessions 40/41)**:
    the `0x0D` → `0x02` → `0x0D` re-entry and the ~28.7 s clean first visit are
    the game's own scripted loop (`0x02`'s enter hardcodes next = `0x0D`,
    `0x0D`'s leave – `func_80178B7C` – hardcodes next = `0x02`; the per-frame
    update picks `0x02` when `func_801C8884() == 2`). No keyboard input changes
    the visit length: `0x0D` is a fixed cutscene, not an interactive screen.
    Added `OGRE_TAP_MAX` (stop tapping after tap n) and `OGRE_TAP_BUTTON`
    (a per-press button schedule: press Start through the title, then A/B/Z/D-pad
    without a human). Re-characterised the re-entry crash: `func_ovlC_802399AC`
    is **not** a standalone function — it is the fall-through continuation of
    `func_ovlC_80239874` (verified at instruction level in `build/bankC.elf`),
    whose prologue is what stores the `a2` buffer at `sp+0x1EC`. The
    `jal 0x802399AC` at `0x8022D218` (`func_ovlC_8022D1CC`, only taken when the
    mode byte `D_8018FC39 == 2`) therefore enters mid-function with a 24-byte
    frame, so that slot is never written and `func_800988A0` gets `a1 = NULL`.
    Forcing the mode byte to 0 moves the crash (N64 `-8` in
    `func_ovlC_8023C894`), so the re-entry lacks more than that one branch.
    Two genuine mis-binding bugs were found and fixed on the way: the
    `func_801AFC2C` size override ran 4 bytes past the start of `func_801B00D0`
    (so the recompiler redirected every `jal 0x801B00D0` into it), and the
    `jal 0x801B00D0` sites are now dispatched to bank unit C's real
    `func_ovlC_801B00D0`. See `docs/HANDOFF-2026-09-14-session40.md` and
    `docs/HANDOFF-2026-09-14-session41.md`.
  - ✅ **`D_8018F1C0`'s writer found, and the re-entry's selector-2 branch is
    game data (session 42)**: `D_8018F1C0` is written by the scene-script VM
    `func_80170974` (`sh` to `0($s5)` at `0x80170ADC`, `$s5 = &D_8018F1C0`);
    session 41's audit missed it because every store goes through `$s5`, not
    through the symbol. Probes show the VM writing `1` at script pc 16 and `2`
    at pc 32, and the `0x0D` path reading exactly those
    (`func_80226FA8 F1C0=0x0001` / `0x0002`); `func_80178568`'s own
    `D_8018FC39` store is **not** taken. The selector comes from
    `func_8022683C(-5, n)`: `func_80227E64(n)` reads the decompressed descriptor
    table (asset `0x19A8804` = ROM `0x1F3CA54`) and returns the step's command —
    `n=1 → -7`, `n=2 → -3`. Step 2's `-3` → jtbl index 7 → handler `0x80226E30`
    → `func_80227700(2)` legitimately selects the crashing branch, while step
    1's `-7` → handler `0x80226AC4` → selector 0. So the `jal 0x802399AC` in
    `func_ovlC_8022D1CC` is a path the game's own data asks for. At that call
    the port has `sp=0x800C22A8`, `s0=0`, `*(sp+0x1EC)=0`, while visit 1's
    per-frame `func_ovlC_80239874` frames run at `sp=0x800C1BF0` (their `a2`
    slot lands at `0x800C1DDC`, `0x6B8` below the slot path B reads). Both
    scene-setup callbacks (`func_ovlC_80225A3C` sel 0, `func_ovlC_80226110`
    sel 2) run at the *same* depth, so that read address is fixed by the call
    chain and never written. Also established: the attract loop is
    `title ↔ {story(0x0B), unit-info(0x0C)}` and never enters `0x02`/`0x0D`
    (170 s tap-free run, exit 0), so `0x02`/`0x0D` is the New Game intro; the
    title's entry 3 leads to scene `0x17`, which is blocked *only* by two
    uncompiled bank records (17 `rom=0x069920 ram=0x80197B90 size=0x4D60`,
    18 `rom=0x1BA020 ram=0x80220F60 size=0x92B0`) — the concrete next step.
    Also corrected the scene model: scene ids dispatch through the accessor
    table `D_800AF028` hardcoded in `func_80075BC0`, and the descriptor's
    `+0x00`/`+0x04`/`+0x08`/`+0x0C`/`+0x10` are **enter / update / update-hook /
    leave / record-mask** (previous sessions had this half-wrong). New Game's
    `0x0D` enter `func_80178568` has **two modes** chosen by `D_8018F1C0`:
    `0`/bit15 = movie mode (DMAs records 10a/10b, installs the cutscene vtable
    `&D_801E5AC0`), non-zero = command mode (DMAs 14a/14b, runs the scripted
    command that sets `D_8018FC39`). Both are walls today: command mode dies at
    step 2 on the frame-less `jal 0x802399AC`, movie mode dies in
    `func_801AFC2C(0)` on session 38's decompressor wall because
    `0x8019F794 == 0` (and that flag is 0 even on the natural `title → Start`
    path). See `docs/HANDOFF-2026-09-14-session42.md`.
  - ✅ **Scene `0x17` is the Tutorial, and the last two bank records are
    compiled (session 43)**: records 17 (`rom=0x069920`) and 18
    (`rom=0x1BA020`) are now **bank unit B** (they are RAM-disjoint from each
    other, so one new unit holds both; `BANK_UNITS := A B C D E`). Scene `0x17`
    loads 21 + 125 functions with zero stubs and renders Deneb's tutorial
    dialogue (`docs/proofs/native-tutorial-dialogue.png`) — the title's
    **Tutorial** entry, verified by driving the menu (`Down` + `Start` →
    `[scene] id=0x0017`). The title menu itself is captured
    (`docs/proofs/native-title-menu.png`): `New Game` (cursor) / `Tutorial` /
    `Stereo` — so `func_80177A58` state 3 → `0x17` = Tutorial and state 2 →
    `0x12` = the save-only **Load Game** entry. Also corrected a load-bearing
    address in sessions 38/39/42: the movie-path word is **`0x80197794`**, not
    `0x8019F794` (`lui $v0,0x8019` + `lw $v0,0x7794($v0)` at `0x801B80B4/B8`).
    And measured the `D_8018F1C0` writer with a probe over all four store
    sites: only `0x80170ADC` fires — it is the script VM's **opcode 0x10**
    (`jtbl_80190758[15] = 0x80170AC0`), which sets `F1C0 = var[0]` **and**
    `F1C2 = 0x8002` at pc 16 (`var[0]=1`) and pc 32 (`var[0]=2`), each before
    the `0x02`/`0x0D` visit it causes; the stores that can write 0 never
    execute. **And the first `0x0D` visit is the New Game movie, which the port
    already renders**: with `F1C0 = 1` (session 42's "command mode" — a
    misnomer, it is the cutscene engine) a natural visit 1 runs its full 28.7 s
    (`0x0D` t=11063 ms → `0x02` t=39780 ms) and draws a sepia courtyard
    cutscene with the subtitle *"I promise I'll make you proud."*
    (`docs/proofs/native-newgame-cutscene.png`). So `var[0] = 1` is correct for
    step 1 and the movie-mode branch is not on the New Game path. The scene the
    developer identified as coming *after* the movie is the cathedral dialogue
    `Archbishop Odiron` / *"He who has learned the way of the sword and god's
    teachings,"* — i.e. **step 2**, which is exactly where the port dies
    (`func_ovlC_8022D1CC` path B → `jal 0x802399AC`). That wall is the next
    session's single goal. Also: dialogue/subtitle text is LZ-compressed
    (`tools/ogrelz.py` decodes `func_8007A110`'s format).
    See `docs/HANDOFF-2026-09-15-session43.md`.
  - 🚧 **Step 2 unblocked by restoring the N64's low-RDRAM (KUSEG) alias
    (session 44)**: the New Game **step table** is decoded (`func_80227E64(n)`
    → table asset `0x19A8804` at ROM `0x1F3CA54`, entry `n` = the step asset,
    whose LZ block starts at `rom+4`; the command is the low byte of the
    descriptor's last word). Steps 1..19: cmd `-7` (step 1, the movie), then
    `-3`/`-10`/`-4`; **every step ≥ 2 selects selector 2**, so
    `func_ovlC_8022D1CC`'s path B is normal code. Path B's `jal 0x802399AC`
    targets the **tail of the function whose prologue is `0x80239874`** (no
    `jr $ra` between them, raw ROM verified); the tail's last call is
    `func_800988A0(a0=sp+0x190, a1=*(sp+0x1EC))`, which *writes* to `a1` — the
    slot the prologue fills from its `a2`. The port reached it with
    `*(sp+0x1EC)=0`, and behind it the descriptor interpreter
    (`func_80227030` → `func_ovlC_802282D8`, first opcode `0x80000006`) called
    the display-list emitter `func_ovlC_8023C894` with `a3 = 0` — two stores to
    guest `0` and `8`. Tracing `a3` (queue slot `0x800E7A30` →
    `func_80227FF8`'s relocated-tag leak `0x08880000` → `0xFFFFFFFF` → `0`
    from `func_802329D0`) shows every value comes from *game* code, so retail
    hits the same stores; retail runs the opening, so the low window must be
    writable there. The R4300i's KUSEG is TLB-mapped and the boot ROM maps the
    low RDRAM window into it; the port's `recomp_mem_addr` computed
    `a - 0x80000000` and faulted ~2 GiB below the buffer. **Fix:** return
    `a & 0x003FFFFF` for `a < 0x80000000` (recorded in
    `patches/n64modernruntime-n64recomp.patch`). Effect: the step-2 enter completes —
    the null build runs the scene 110 s with no crash (it died in one frame
    before), the movie still renders, Tutorial/attract unchanged. **Next wall:**
    the RT64 build dies in `do_send` (SIGBUS, guest `0xFE6E2C89`): the guest's PI
    state (`D_800AA400`/`D_800AA408`, never initialised because
    `osCreatePiManager` is stubbed since session 6) holds garbage at the step-2
    enter, and `func_800998C0` feeds it to `osSendMesg` from the asset-load chain
    (`func_ovlC_802282D8 → func_ovlC_8023BF50 → func_8009DBB8 → func_80089F80 →
    func_8008BC40`); the null build does not hit it. Suspect a ROM DMA landing in
    the wrong place (`func_8008BC40_recomp`).
    `tools/ogrelz.py` gained `--asset` and the corrected block-start rule.
    See `docs/HANDOFF-2026-09-15-session44.md`.
  - ✅ **Step 2 renders: the wall was a missing streamed module, not the PI
    manager (session 45)**: the game streams a **second** record-14 arena module
    (`bankRec14c`, ROM `0x2A8CF0` size `0x56A0`) to RAM `0x802395E0` — the RAM
    `bankRec14b` already occupies — for scene `0x0D` steps ≥ 2. The port had only
    rec14b, so N64Recomp bound unit C's 95 calls from records 14/14a into the
    arena as direct calls to rec14b's **body interiors** (`0x80239C24` is a
    mid-body point of rec14b's `func_ovlC_80239874`, no prologue; rec14c has a
    real `addiu sp,sp,-0x60` function there). The step-descriptor interpreter's
    opcode-42 `jal 0x80239C24` therefore ran a prologue-less body with its own
    0x78-byte frame: `sp` leaked `+0x238`, `s3`/`s6` became 0, the interpreter
    walked **guest 0** as its descriptor, read an out-of-bounds table word at
    `base+0x40000` (null build: 0 → harmless; RT64: `0x00010001` → a ~2.5 GB ROM
    DMA that overwrote `D_800AA400`/`D_800AA408` → the session-44 `do_send`
    SIGBUS). **Fix:** rec14b moved out of unit C into **unit F**, rec14c compiled
    as **unit G** (new splat/N64Recomp configs), so unit C's arena calls compile
    as `LOOKUP_FUNC` and the runtime's DMA-driven bank switch picks the resident
    module. Result: the movie visit runs 28.4 s, step 2 renders the **cathedral
    dialogue with Archbishop Odiron** on both renderers
    (`docs/proofs/native-newgame-cathedral.png`), exit 0, 0 stub calls.
    **Correction:** session 44's KUSEG mirror fixed a *symptom* of this
    mis-binding — with the calls dispatched the low-window stores no longer
    happen (RDRAM `0x0..0x40` is zero after the run); the mirror is now
    unexercised on this path and should be A/B'd next session.
    See `docs/HANDOFF-2026-09-15-session45.md`.
  - ❌ **A YUV16 decoder in RT64 was tried and reverted; the background is still
    black (session 49)**: the njpeg macroblock draw does use `fmt=1 siz=2` YUV16
    textures and `TextureDecoder.hlsli`'s `case G_IM_FMT_YUV:` does return
    `float4(0,0,0,1)`, but a decoder written this session turns the frame into a
    **corrupt magenta/green blob**, not the backdrop (A/B in
    `docs/HANDOFF-2026-09-15-session49.md` §2; the shader working tree is back to
    its original state). The mid-session claim that "the backdrop draws" was
    wrong — its capture's bright pixels were the dialogue box and the sprites,
    which are RGBA16 and always rendered. Session 48's "the background renders"
    is also wrong; its buffer-selection patch was never the wall.
    **Next lead:** check GLideN64 — the emulator upstream closed
    [mupen64plus-user-issues#102](https://github.com/mupen64plus/mupen64plus-user-issues/issues/102)
    with — against RT64 before writing another decoder, and decide whether the
    game's readback should be one scene-entry later.
  - 🚧 **The game's CPU readback copies a zero framebuffer (session 49)**: the
    readback (`0x80199884`) runs 4×, all in scene `0x02`, and every copy reads
    zero (`probe52`, reverted: `src=0x80000400 fb0=0 fb1=… fb2=0`); the blit's
    source `0x80243E28` is zeros when sampled. This is **upstream of any decode**.
    A sync hook is in place (`ogre_sync_framebuffers()` →
    `State::syncFramebuffers()`, retire the pending workload and write rendered
    framebuffers back); it is a readback-correctness improvement, not a fix.
  - 🚧 **Why the game's own frame index lands on the placeholder (session 48)**:
    `D_800C4BB8` is the VI manager's "displayed buffer" word
    (written by `func_8007307C`, which `func_80089540` — N64 Thread 5 — calls
    from a message object; a `watch.sh --value` conditional watchpoint caught
    it). Session 49 measured that both that index and the njpeg target resolve to
    `0x000400`, so this is not the backdrop wall; the question is still open.
    See `docs/HANDOFF-2026-09-15-session48.md` §4.
  - ✅ **Boot straight to a New Game step (`OGRE_STEP`, session 48; corrected in
    session 53)**: the opening's steps are the scene-script word `D_8018F1C0`, so
    `OGRE_SPEED=8 OGRE_SCENE=new-game OGRE_STEP=2 OGRE_NJPEG=1 ./build-app/ogrebattle64`
    reaches the cathedral ~2.2 s after boot instead of after the 28.7 s movie.
    **The step is *seeded* on the first frame the selected scene is active, not
    held every frame.** Session 48's per-frame hold overwrote the game's own
    advance: the moment the game wrote the next step the hold put the seeded one
    back, so the opening re-entered the cathedral forever ("the scene is running
    in a loop, restarting the cathedral scene" — developer, session 53). The
    seed is released as soon as the live step moves off it, or after a 1500 ms
    window. This also makes the background fix above reproducible without the
    flaky title-tap timing. See `docs/guides/app-build.md` (`OGRE_STEP`) and
    `docs/scenes.md`.
  - ✅ **The New Game opening advances past the cathedral (session 54)**, and
    session 53's "the cathedral never advances" is corrected: the advance is real
    and reachable. The maintained repro drives the real flow —
    `OGRE_SCENE=title OGRE_SPEED=4 OGRE_TAP_MS=1000
    OGRE_TAP_BUTTON="start,a,start,a,…"` — a Start summons the title menu
    (developer, session 54), the cursor starts on New Game, a second Start
    confirms it, and `A` advances the movie and the cathedral dialogue. Measured:
    `0x04 → 0x02(step 1) → 0x0D` (movie, 3.5 s) `→ 0x02(step 2) → 0x0D`
    (cathedral) `→ 0x07` at `t≈17.9 s` (4×). The engine's exit calls
    `func_80178CB0`, which does `D_8018F1C0 = 0` (`func_801723B4`, `0x801723D8`)
    and `D_800C4C26 = D_8018F1C2 = 0x0007` — so **the step word reading 0 at the
    handoff is correct game behaviour**, and the movie-vtable dispatch session 53
    saw belongs to the `OGRE_STEP` shortcut, which skips step 1 and leaves the
    exit on its `otherwise` arm. See `docs/HANDOFF-2026-09-16-session54.md`.
  - ✅ **Scene `0x07` is the name-entry form, and it renders (session 55)** —
    session 54's black screen is fixed and its diagnosis corrected. The scene's
    descriptor is **`D_8018FDAC`** (streamedB ROM `0x65CAC`; `func_80075BC0`'s
    accessor table maps id 7 → `func_8017B600` → it), **not** `D_8018FB98`;
    enter `func_8017B794`, update `func_8017B858`, hook `func_8017B9C8`, mask
    `0x00000002`. The enter chunk-DMAs a **0x8600-byte code module, ROM
    `0x712A0` → RAM `0x8019A7C0`** (67 × `0x200` chunks) and calls it. The
    module's RAM overlaps **record 3** (unit A) and **overlay C** (the main
    ELF), so N64Recomp bound the calls to overlay C's bodies at those addresses
    (of the module's 24 internal `jal` targets, **0** were main-ELF entries) and
    the form drew nothing. The module is now **bank unit H**
    (`config/banks/config-bankH.yaml`/`.toml`, `BANK_UNITS` gains `H`; code/data split at ROM
    `0x783C0`, where splat also puts it), and the six calls from resident code
    into it are dispatched (`LOOKUP_FUNC`) from `make recomp`'s `--only` list.
    `tools/cross_bank.py`'s `overloaded()` had a bug that hid the range
    (`hi = min(c.hi)` instead of `max(c.hi)`); fixed. Verified: forced
    `OGRE_SCENE=0x07` draws the form (50 frames, 219302 non-black, was 0), and
    the title-driven opening reaches `0x07` at `t≈19.0 s`, draws it, and hands
    off to `0x02`/`0x0D` at `t≈20.7 s`. Proof
    `docs/proofs/native-newgame-name-entry.png`. See
    `docs/HANDOFF-2026-09-16-session55.md`.
  - ✅ **The opening runs through the whole New Game sequence, and the stale
    backdrop is fixed (sessions 56/57, developer-confirmed)**: name form →
    **date of birth** → **personality questions** (steps 3–8) → **scene `0x16`**
    (descriptor `0x8018FC00`, mask `0x400`) at `t≈44.7 s` (4×). The cathedral
    backdrop could come back with a **rectangular stale region** (the previous
    form's content, intermittent). **Session 56** found the artifact baked into
    the game's own display framebuffers and the readback destination
    `0x80243E28` holding a picture of the form, and concluded "render-vs-readback
    timing". **Session 57 corrects that: it is the readback *source selection*,
    not timing.** `tools/njpeg_readback.py` preferred RT64's scratch word `+8`,
    which is only re-set by a YUV-texture-then-colour-image pair and **never
    cleared**, so at the first pass of an assembly it still named the *previous*
    step's framebuffer — by then holding the previous screen (the form). The
    game's own `state[0x64]` (written at `0x80199B98` from the framebuffer table
    `D_800A8204`, indexed at `0x80199AF8`-`0x80199B68` by `D_800C4BB8`; it selects
    the buffer that is *not* displayed) named the correct sub-image at every pass
    measured. The patch now keeps the game's choice whenever `D_800C4BB8` matches
    a table entry and uses the scratch word only as the fallback (the session-48
    case). Measured over 10 runs / 120 stage-3 passes with `OGRE_NJREAD_LOG=1`:
    old rule wrong **13** (always pass 0, always the previous screen), new rule
    wrong **0**. The stale pass was always **pass 0** (destination `0x801AAE90`,
    240x320) — the developer reports the broken on-screen tile as the top-left of
    the 4-image 2x2 backdrop, and pass 0 is the only stale pass, so they are
    almost certainly the same chunk (inferred, not read out of the blit; the
    framebuffer's own chunk order is scrambled — see
    `docs/guides/njpeg-backgrounds.md`). **Live-console A/B** (dump while the dispatcher
    reports scene-`0x0D` step `0x021D8002`): `0x80243E28` is the name-entry form
    under the old rule (5/5) and the cathedral with the fix (5/5) —
    `docs/proofs/native-newgame-backdrop-old-rule.png` / `-backdrop-fixed.png`.
    The ordering hypothesis is also wrong: `sp_complete()` runs
    before `send_dl` (`events.cpp:422`/`:429`) but the game waits for the **DP**
    completion (`:432`, queue `0x800E8BF4`), so a game-thread handshake
    implemented for it never blocked once, even with the njpeg display list
    deliberately delayed 400 ms inside `send_dl`; it was reverted.
    `ogre_sync_framebuffers()` still forces the RDP's pixels back to RDRAM before
    the copy. Proofs: `docs/proofs/native-newgame-readback-stale-source.png` /
    `-correct-source.png`, `docs/proofs/native-newgame-cathedral-post-dob.png`.
    See `docs/HANDOFF-2026-09-16-session57.md` §1.
  - ✅ **Checkpoints: `save`/`load` in the live console (session 58, the
    developer's idea)** — a fast-testing instrument that removes the ~45-60 s
    New Game replay from every experiment. A checkpoint is the whole 8 MiB RDRAM
    image **plus the runtime's overlay state** (which recompiled body is mapped
    at each RAM address; RDRAM alone would restore a later bank's function map
    and run the wrong module's bodies), written/restored by the console commands
    `save [path]` / `load [path]` (`write_checkpoint`/`read_checkpoint` in
    `app/src/sdl_platform.cpp`; `recomp::overlays::get_overlay_state_blob` /
    `restore_overlay_state_blob`). Both wrap the file I/O in
    `ultramodern::checkpoint_pause_begin/end`, which parks every thread executing
    recompiled code at a function-entry boundary — the N64 threads are 1:1
    native, and without the park the image **tears** (first attempts: the
    checkpoint's own checksum did not match its bytes, first `0x300` bytes zero,
    while the game kept submitting RSP tasks during the write). Verified: file
    self-consistency, an in-run rewind that then plays forward (personality
    questions → `0x16`), and a cross-process load. `OGRE_CONSOLE_AT_MS` delays
    the watched-file read so a scripted run can leave the command file in place.
    See `docs/guides/app-build.md` → "Checkpoints" and
    `docs/HANDOFF-2026-09-16-session58.md` §1.
  - ✅ **Scene `0x16`'s module is compiled and the closing movie plays (session
    58)** — session 57's next lead, done. Scene `0x16` (descriptor `0x8018FC00`,
    mask `0x400`) chunk-DMAs ROM `0x244770` (0x7500, 59 × 0x200) → RAM
    `0x801D0860` (record 10's arena). It is now **bank unit I**
    (`config/banks/config-bankI.yaml`/`.toml`: code `0x244770..0x24B3E0`, data to the module
    end `0x801D7D60`, 43 functions), and `bankRec10a` — the *other bank* of that
    RAM — moved out of unit C into **unit J** (`BANK_UNITS := … I J`), because a
    unit cannot hold overlapping records and unit C's own code calls into that
    range. Result: the scene loads, plays and **advances out of it** (repeated
    `0x02 → 0x0D → 0x16` cycles at t≈58.8/66.0/68.6/71.4 s at 4x, no stub
    calls; `20 streamed-overlay record(s), 1975 function(s) armed`).
    Three build-hygiene traps this exposed are fixed in the `Makefile`:
    `build/bank<U>/{asm,assets}` must be cleared before `splat split` (splat
    never deletes a removed segment's output), `RecompiledFuncs/`/`Bank*Funcs/`
    must be cleared before regenerating (N64Recomp never deletes a previous
    run's file, so a symbol that changed section leaves a conflicting definition
    behind), and a `data` subsegment needs an explicit following `bin` gap.
    `tools/gen_bank_syms.py` now also sees spimdisasm's `.Lovl<U>_<addr>`
    references. See `docs/HANDOFF-2026-09-16-session58.md` §2.
  - ⚠️ **The chapter-card crash, first reading (session 58) — SUPERSEDED by
    session 59** (the crash was the record-14 arena's third bank, mis-bound
    through unit C, not a missing engine init; the "engine word" it names is at
    `0x8022A994`, not `0x8023A994`). Kept for the trail: the
    developer's end-of-sequence crash reproduces deterministically: `SIGBUS` in
    `func_ovlC_8022C270` on N64 thread 4, right after a `0x02 → 0x0D` visit,
    with `D_8018F1C0 = 0x0431` = **step 1073** = the **"Prologue" chapter
    animation** (`docs/scenes.md` row 7). Corrected findings: the step table
    (ROM `0x1F3CA54`, asset `0x19A8804`) is a raw `u32` array of **1693**
    entries, so 1073 is *valid*, not an overrun; its descriptor's low opcode
    (`0x1B` < 31) goes to the interpreter's shared case (`0x80228E84` →
    `func_ovlC_8022C270`), which walks a table based at **`*(0x8023A994)`**.
    **The engine init DOES run** — `probe58` (tagged, reverted) showed
    `func_ovlC_8022D1CC` storing `malloc(0x1CB8)` = `0x8019F490` at `0x8022D1E8`
    once per sequence visit, **except the visit that faults** (t=75235 in the
    measured run). And the arena DMA covers the engine word every visit
    (`bankRec14c` 44 chunks → RAM `0x802395E0..0x8023EBE0`; the word is at
    `+0x13B4`), so the missing init leaves the module's own file data there
    (`0x46000086`/`0x3C01801D`/`0x58933768` — exactly the module images' words at
    that offset) and the handler dereferences it. **Next:** the step **command →
    setup selector** dispatch for command **6** (step 1073's command; the movie's
    are 5/1/1/5/1) — `func_80227700(sel)` picks `sel 0`=`func_ovlC_80225A3C` /
    `sel 2`=`func_ovlC_80226110`, both of which call the init; a mis-dispatch
    there is the session-42-class bug. `docs/HANDOFF-2026-09-16-session58.md`
    §3b/§3d.
  - ✅ **The chapter-card crash is FIXED — it was a missing third bank of the
    record-14 arena (session 59)**, and the opening now plays on past the movie
    into the post-movie story and scene `0x05`. The step's **`-8`** command
    (`func_ovlC_80227E64` maps the descriptor's last-word low byte 6 through the
    table at `0x8022ABE0`) selects the `func_ovlC_8022683C` arm at 0x802269DC,
    which DMAs **ROM `0x286BA0` (0x138F0) → RAM `0x8022ACB0`** — the chapter
    animation's module, which the port had no code for. `bankRec14a` (the *other*
    bank of that RAM, ROM `0x29A490`) was compiled into unit C, so the resident
    interpreter's `jal 0x8022C270`/`0x8022C6E4` and the callback's
    `jal 0x8022E3F0` were bound to rec14a's bodies; with the chapter module
    resident the interpreter ran rec14a's layout, left base/pc = 0 and walked
    guest 0. Fixed with **unit K** (the chapter module) + **unit L**
    (`bankRec14a` moved out of unit C), so those calls compile as `LOOKUP_FUNC` —
    plus `bankRec14a`'s ROM end, which was `0x2A82F0` while the game DMAs
    `0xE860` to `0x2A8CF0` (the truncated 0xA00 held its jump tables; N64Recomp
    aborted on `jtbl_ovlL_80239270`). Verified: the **Prologue card renders**
    (`docs/proofs/native-newgame-prologue-card.png`), the story after it renders
    (`-received-for-duty.png`, `-magnus-old-man.png`), scene `0x05` enters with
    **0 stub calls**, exit 0. Session 58's global addresses are corrected
    (`0x8022A970`/`78`/`94`, **not** `0x8023A9xx`). See
    `docs/HANDOFF-2026-09-16-session59.md`.
  - ✅ **Scene `0x05`'s module is compiled (session 59, unit M)** — its enter
    `func_8017B60C` DMAs ROM `0x79750` (0xDAD0) → RAM `0x8019A7C0`, the *other*
    bank of unit H's scene-`0x07` form module; before this the scene called
    `0x8019A7C0` and hit the streamed stub. Unit H's own extent is corrected from
    ROM `0x798A0` (0x8600, session 55's chunk-rounded figure) to `0x79750`
    (0x84B0, the enter's own `subu`). `app/src/bank_overlays.cpp`'s
    `is_known_module` now also requires the chunk's rom→ram delta, which is what
    had hidden this missing module (a plain ROM-range test matched unit H).
    Scene `0x05` shares scene `0x07`'s update/hook and its enter is a near-twin,
    so it looks like another UI/form screen; it rendered **black** until
    session 60 (next bullet).
  - ✅ **The map scene renders (session 60)** — scene `0x05` is the **world map**
    (developer): terrain, rivers, location labels, the route of dots, unit
    markers and the `Flama` cursor in a stone frame
    (`docs/proofs/native-newgame-map-scene.png`). It was black because **no
    frames were produced**: two calls in the scene update/hook
    (`func_8017B858` @0x8017B8A0 `jal 0x8019AF0C`; `func_8017B9C8` @0x8017BA10
    `jal 0x801A103C`) were compiled as "call the containing body + early
    `return`" (a `jal` into a size-overridden body interior), abandoning the
    caller's frame each time. The frame-pump thread (t4, `func_8008AFE0`) then
    read its saved `$s0`/`$s1` from the wrong stack slots; `$s1` is the
    message-type comparator (1) and became 0, so the type-1 dispatch stopped
    matching and the pump died after two frames — the signature is
    `D_800AEFA4` (frame counter) frozen while `D_800C4BCC` (VI retrace) keeps
    counting, with only 2 display lists submitted. Both targets are in the RAM
    the scene module occupies and are now in `make recomp`'s
    `cross_bank.py dispatch --only` list, which also triggers the tool's
    tail-call repair (call-and-continue); `0x8019AF0C` resolves to unit **M**'s
    state-1 handler (scene `0x05`'s enter sets `0x801977E8 = 1`; scene `0x07`'s
    sets `3` and takes `0x8019B340`, which is why the form already worked).
    Verified: forced `OGRE_SCENE=0x05` 2 → **124 display lists**; the natural
    route submits **2296** and draws the map, with the movie and the Prologue
    card unchanged. The no-`--only` full dispatch also fixes it (89 sites /
    7 extra targets) and is the way to clear the remaining backlog.
    `docs/HANDOFF-2026-09-16-session60.md`; `docs/scenes.md` row 10.
  - ✅ **The map's sprites are FIXED (session 65) — the cause was a linker
    artifact, not the renderer, the asset or the game's constants.** Unit M's
    `.data` subsegment was linked **8 bytes above** its ROM address, so every
    `lui/%lo` data reference in the recompiled module read 8 bytes too high —
    including the map's sprite table base: the ROM's builder
    (`func_ovlM_8019F83C`, ROM `0x7E804`) says `addiu v0,v0,0x6FD8`, the bank ELF
    said `0x6FE0`, and `nm` defined `D_ovlM_801A6FD8` at `0x801A6FE0` (150 of the
    unit's 480 address-named symbols were +8). **Every sprite descriptor was read
    one entry late**, which is exactly the "the two calls name each other's
    entries" of sessions 61–63: with the correct table at RAM `0x801A6FD8`,
    `a2=10` = `(32,32)` = **the party** (the full knight sheet frame), `a2=11` =
    `(16,11)` = **the shadow**, `a2=12` = `(144,23)` = **the date plate**, and
    `a2=6` = `(16,16)` = the cursor. The cause of the shift: `mips-linux-gnu-as`
    pads `.text` to 16 bytes and the module's code/data boundary (ROM `0x85838` =
    RAM `0x801A68A8`) is only 8-byte aligned, so the data subsegment started 8
    bytes high. **Fix (config only): the `asm` subsegment now ends at `0x85840`**
    (the 8 zeros there are the module's own padding and nothing references
    `0x801A68A8`), which makes the assembler's pad real bytes and restores every
    label. Result: the party is a 32x32 knight with its 16x11 shadow, the cursor
    is one 24x23 arrow, and the panel is the full `MONTH/DATE | Sombra | 1` plate
    (`docs/proofs/native-newgame-map-scene.png`, **replaced**; the broken render
    is kept as `docs/proofs/map-sprites-before-offby8.png`). **New guard:**
    `tools/elfcheck.py` / `make elf-rom-check` (ELF-vs-ROM byte comparison +
    address-named-symbol audit) — after the fix **all 13 bank ELFs and the main
    ELF are byte-identical to the ROM**; `make bank-recomp` runs it before
    recompiling. Session 64's renderer-side `line = 8` lead is **withdrawn**: for
    RGBA32 the RDP keeps 2 bytes per texel per TMEM half, so `line = 8` is 32
    texels per row — the sheet's row — and RT64's loader/sampler agree with
    angrylion (`docs/HANDOFF-2026-09-17-session65.md` §1–§4).
    **Developer-confirmed in game (same session, §8):** *"the scene looks
    perfect!"* — party, the 4-dot route, the red pin, the crossed swords, the
    cursor and the full `MONTH/DATE | Sombra | 1` panel all correct; the marker
    and route elements session 64 §8b could not see in forced captures are the
    same one-entry shift. Selecting the next mission **advances out of the map
    into the next scene**; **it crashes when the mission actually starts** — that
    is the next wall (ask the developer for the `[crash]`/`[snap]` block at that
    moment; `tools/scenemap.py transitions` should name the target scene).
  - 🚧 **Checkpoints are process-local, so they cannot be handed between runs
    (session 65).** A `save` + `load` pair **in the same run** at the map works
    (verified on the live map, scene `0x05`, with every N64 thread active). Loading
    the *same bytes* written by an **earlier process** (same binary, same build
    id) dies deterministically right after the "loaded" message: `SIGSEGV` in
    `do_send + 0x54C` on an N64 thread, with a garbage host address. The image
    contains **host pointers** — `OSThread::context` is declared "an actual
    pointer regardless of platform" and is written into guest RDRAM, one per N64
    thread (7 found in `/tmp/map-fixed.bin` at `OSThread::context` = `base+0x20`) — and the scheduler dereferences them after the rewind. So
    `/tmp/map-real.ckpt` and `/tmp/map-fixed.ckpt` (both from the developer's
    process) are **inert in any other run**, and the docs' "one-command replay"
    recipe only works within the run that saved. **Fix (a milestone, and worth it
    — checkpoints are the project's main accelerator):** rebuild the runtime's
    host state from the restored guest state — a per-thread `OSThread` →
    `UltraThreadContext*` registry installed by `osCreateThread` and rebound after
    `read_checkpoint`, plus a reset of the scheduler's blocked-thread host state
    (session 58's "cross-process load" claim is corrected in `docs/DECISIONS.md`
    and `docs/guides/app-build.md` → "Checkpoints").
  - 🚧 **The map's party: the two draws are identified (session 63); the wall is
    a zeroed texture buffer. Nothing landed.** *(Superseded by session 65: the
    entries were not "naming each other" — the whole table was read one entry
    late because unit M's data labels were 8 bytes high; the "zeroed buffer" was
    the shadow's own `+0x1068` region read through the 16x11 rect that was in
    fact the shadow's correct entry.)* The developer corrected sessions
    61/62: the party (Magnus, the knight) is the **second** builder call
    (`func_ovlM_801A2A7C`, ROM `0x81BFC`, `a2=0xA`, rect `(a0-16, a1-24)`) — the
    rect **above** the other — and it needs **`16x24`** (his hair is visible in
    some sprites); the **first** call (ROM `0x81B50`, `a2=0xB`, rect `(a0-8,a1)`)
    is the **shadow** and needs `16x11`. Live table values: slot 12 = `(16,24)`,
    slot 10 = `(16,11)`, slot 11 = `(144,23)` — so the two calls currently name
    each other's entries, which is why the shadow drew as the row of ~18
    ellipses (`144x23` over a `7x10`-texel window) and the knight as a small dark
    blob. **The window mechanism is settled:** the caller's `jal` delay-slot store
    (`$t5 = 0x1C028`) lands on the **same display-list word** the builder writes
    (`dl+0x3C`) and wins on **hardware** as well as in N64Recomp's output — not a
    port artifact, and there is no unidentified second writer (corrects session
    61 §4(a) / session 62 §1). **The wall:** with the rects and per-entry windows
    made consistent, both party sprites are clean, correctly-shaped **dark
    blocks**, and a live probe shows why — the knight's texture pointer
    (`0x8021AC88`, from `state 0x80197B18 -> 0x801F1570` field `+0x04` biased by
    `+0x1068`; the shadow's is `0x80264D00`, `+0x10E0`) points at a buffer whose
    words are **all zero** in the live image, and a zero RGBA32 texture with the
    map's alpha combiner renders as that block. The pointer address is faithful
    to the emitted list; the **content** is what is missing, so the next step is
    to watch that buffer from scene entry and find who fills it (or which field
    names a populated one). The **cursor** (`a2=6`) and the **date panel**
    (`a2=12`) are the same rect-vs-window class.
    `docs/HANDOFF-2026-09-16-session63.md`.

  - 🚧 **The map's party, reframed: the port is faithful, so stop editing its
    display lists (session 64).** *(Superseded by session 65: the four checks
    were right that the LZ decode, the RDRAM contents, the GBI and the *raw ROM*
    constants are faithful — but they do not cover the **bank ELF**, and unit M's
    data labels were 8 bytes high, so the *recompiled code* read the sprite table
    one entry late. §4's `line = 8` lead is also withdrawn: for RGBA32, `line = 8`
    is 32 texels per row. See `docs/HANDOFF-2026-09-17-session65.md`.)*
    *"How would an emulator handle it?"* Four
    independent checks say the port is not the defect: the LZ decoder is exact
    (13/13 assets end **on** their declared payload boundary), the port's
    `state[+0x04]` buffer is **21128/21128 bytes identical** to an offline decode
    of asset `0x01DD210A`, scene `0x05`'s ucode `0x8009F540` hashes (XXH3-64, raw,
    len `0x1390`) to RT64's **`F3DEX2.fifo 2.08`** entry so the GBI choice is
    right, and every constant was re-read out of the raw ROM. **Two session-61/62
    mechanisms are withdrawn:** N64Recomp does *not* run a `jal` delay slot twice
    (the duplicate is dead code after `goto after_N`), and the builder
    `func_ovlM_8019F83C` emits **no** `G_SETTILESIZE` at all — the word at
    `0x8019F990` is the TEXRECT's **s,t**, so there was never a second window
    writer. **New model, ROM-verified:** the knight is *not* an asset — the map's
    enter `malloc(0x18000)`s `state[+0x34]` (`0x8019A9E4`) and **composites** an
    RGB555 LUT over an 8-bit index image into it, a **32-texel-wide** RGBA32 sheet
    of **24 frames** (`0x1000` each = 8 directions x 3 frames, selected by
    `3*state[0x1DC] + f`); `state[+0x04]` is asset `0x01DD210A`, the shadow
    draw's `+0x1068` really is zero, and the only translucent-black art in any map
    asset is at `+0x10AC` of that same asset. `state` is a **heap pointer** — read
    `*(0x80197B18)`, never hardcode `0x801F1570`. **New top lead:** the sheet's
    row stride is 128 B (`line` 16) while the draw's render tile declares
    **`line=8`** — the game's own command, so this is a renderer-side question
    about `G_LOADBLOCK`'s block/tile-line interaction, not a game-data one (call
    1's `line=2` + `masks=3` + 7-texel window is *only* consistent read as
    16-bit, the same question). Next: compare RT64's `setTile`/`loadBlock`/
    sampler with GLideN64's and parallel-rdp's, and/or take a reference-emulator
    RDRAM at the same screen (RetroArch + Mupen64Plus-Next is installed, and
    GLideN64 ships `[OGREBATTLE64] graphics2D\enableTexCoordBounds=1` and
    `hack_Ogre64`). **No code landed.** See `docs/guides/emulator-first.md`;
    `docs/HANDOFF-2026-09-17-session64.md`.

  - 🚧 **Pre-state: the map needs army/save data that a forced entry never builds
    (session 64 addendum).** The developer's hypothesis is partly right and the
    split matters: the party's **map position** (`state[+0x40]`/`[+0x44]`,
    `state[+0x5C]`/`[+0x5E]`, read at the party draw's call site `0x801A18F0`),
    the unit markers, the route of dots, mission availability and the date panel
    are all keyed off the army record — so a forced/fresh entry plausibly lacks
    them. But the **sprite art is not**: the knight's 32-wide, 24-frame sheet is
    `malloc(0x18000)`d and composited *by the map's own enter*, which is why a
    forced scene still draws a correct knight sheet. **A fuller save state will
    not fix the party-sprite defect.** Checkpoints (`save`/`load`) already give
    "a save state at that screen", but **a retail save cannot be loaded yet** —
    **corrected in session 65: OB64 saves to a battery-backed cartridge save**
    (SRAM/FlashRAM/EEPROM), and the port still sets
    `entry.save_type = recomp::SaveType::None`, so the game sees no save device
    at all (the Controller Pak is only the *copy/backup* path, and scene `0x12`
    Load Game is documented as appearing once a *pak* save exists).
    **Route to a real map state:** drive it by hand and dump with the live
    console's number keys (`OGRE_KEY_1='c'`, `_2='save /tmp/map-real.ckpt'`,
    `_3='dump /tmp/map-real.bin'`) — an autoplayer stalls on the name-entry form
    (`0x07`) because **a synthetic tap landing in a text field opens a tooltip
    that blocks the advance** (developer); the recorded working shape is a few
    `start,a` pairs then a long run of `a`. **Result (developer dump, same
    session): the pre-state explanation is REFUTED for the sprite defect** — on a
    real playthrough to the map the shadow's source region
    (`state[+0x04]+0x1068`) is **still all zero**, and the knight sheet is present
    and identical. Only 9 of the state struct's 284 words differ (party position,
    tick counters, the sheet's heap address); the sprite source and the knight's
    direction do not. So the defect stays a **renderer/RDP tile-semantics**
    question, and pre-state only explains the markers/route/date. A whole-image
    diff is heap noise — compare the struct, not the dump. **And pre-state IS
    confirmed for the *mission/marker* layer**: from the real map the crossed
    swords (next mission), the red pins, the route dots and the cursor are all
    present, and all were **absent from forced captures** — so a forced scene is
    not a valid repro for anything keyed off the army record. **The party garble
    itself is the row stride, proven offline:** the sheet's frame 23 rendered at
    the declared 16 texels/row splits into two half-columns, and at the true 32
    texels/row it is a clean knight (`docs/proofs/party-sheet-{declared-stride-line8,correct-stride-line16}.png`).
    The draw declares `line=8` for a 128-byte row (ROM-verified) — so the open
    question is purely renderer-side: what the RDP does with a render tile's
    `line` for a `G_LOADBLOCK`-filled tile (GLideN64 keys textures at their image
    width and never consults it; RT64 honours it). **That is the next A/B.**
    `docs/HANDOFF-2026-09-17-session64.md` §8, §8b.

  - 🚧 **The map's sprites are missing/garbled (session 60, developer spec)** —
    the developer supplied the retail screenshot
    (`docs/proofs/map-reference/retail-map-screen.png`) and the screen's spec:
    the framed world map + the **party sprite**, the unit markers, a **cursor**
    (white arrow + red dot) and the bottom-right **date panel** (`MONTH`/`DATE`
    + e.g. `Sombra 1`); plus the map's **`R` menu** (1 Organize / 2 Hugo Report /
    3 Settings / 4 Save, with the two-slot save window, the overwrite prompt and
    the title's `Load Game` cursor default) — all written into `docs/scenes.md`.
    The port draws the terrain, the frame, the I8 road/label mask and the panel's
    month name, but **not the party/cursor/panel box**: the party is a row of
    ~18 repeated dark ellipses. Measured: the party rect is 144x23 px but its
    render tile is a **7x10-texel** window (`SETTILESIZE lrs=28 lrt=40`) of an
    RGBA32 sprite atlas, so the tile repeats; the atlas is asset `0x01DD210A`
    (decoded by `func_8007A110` from unit **M**'s entry into `0x80219C20`), and
    the port's decode matches the offline one byte for byte — the sampled region
    is genuinely transparent. So it reads as a **sprite descriptor the map never
    finished filling**, and the next step is to find what fills it (and whether
    retail animates the party/panel in, since the port's frame is static).
    `docs/HANDOFF-2026-09-16-session60.md` §7. **Session 61 corrected the cause**
    (§1): the `7x10` window **is** in the game's own list, the sprite table is
    static and byte-identical to ROM, and the builder
    `func_ovlM_8019F83C` is faithful — a rect always equals the entry's `(W,H)`
    it reads. The party draw asks for slot **11** (`0x801A7038` = `144x23`) when
    the knight's frames are slots **12-15** (`16x24`); the developer measured
    retail's marker at *10% of height / 5% of width* = **~16x24 px**, matching
    those slots exactly. The call site is `func_ovlM_801A2A7C`'s first builder
    call, ROM `0x81B50` (`addiu $a2, $zero, 0xB`). A **global** `base+8` A/B made
    the party exactly `16x24` but shifted *every* sprite, so it is not the fix; a
    **targeted** `0xB→0xC` shrank the bar to a compact sprite but is not proven.
    Open: the emitted window stays `7x10` even for a `(16,24)` entry, so the
    writer of that window is still unidentified. `docs/HANDOFF-2026-09-16-session61.md`.
  - ✅ **`OGRE_CONSOLE_ON_SCENE` / `_STEP` / `_CMD` (session 58)** — run one
    console command the moment a chosen scene (and step) is live, e.g.
    `OGRE_CONSOLE_ON_SCENE=0x0D OGRE_CONSOLE_ON_STEP=974
    OGRE_CONSOLE_ON_CMD='save /tmp/ck.ckpt'`. This makes checkpointing
    deterministic against the wall-clock route flakiness (a run whose taps
    started before a late title stayed in the attract loop) and is how the
    step-974 checkpoint was taken.
  - ✅ **After the chapter animation comes the post-movie story (session 59)** —
    the "received for duty with other soldiers" content (`General Godeslas` over
    rows of soldiers) and the `Grey-Haired Old Man` / `Magnus` dialogue render
    (`docs/proofs/native-newgame-received-for-duty.png`, `-magnus-old-man.png`),
    then the sequence reaches scene `0x05` (`docs/scenes.md` row 8/10).
  - ✅ **The mission's intro plays on the *natural* route (session 68) — the wall
    was a fourth bank of record 9's arena.** The suspend save (session 67) enters
    scene `0x03` without the sequence that precedes it, so it never exercised the
    module the map -> story -> mission route loads: **ROM `0x195430`
    (`0x2380`) -> RAM `0x80214FA0`**, a bank of the *same* RAM as units P and Q.
    Unit N's own loader (`func_ovlN_801AE880+0x764`, ROM `0x1036E4`) DMAs it and
    calls its entry `0x80215C38`; with no code for it the port logged
    `[bank] UNKNOWN module` + `[overlays] streamed function stub called @
    0x80215C38`, and the mission's intro hung on a garbled target-fort draw whose
    winning-condition NOTE never advanced. It is now **bank unit R**
    (`config/banks/config-bankR.yaml`/`.toml`, `config/symbols/symbol_addrs-bankR.txt`), and the natural route
    runs the whole intro — camera pan, winning condition, panic back, losing
    condition, `MISSION START` — with **0 UNKNOWN modules and 0 stub calls**
    (`docs/proofs/native-mission-intro-win-natural.png`,
    `docs/proofs/native-mission-intro-fort-natural.png`). The route is
    `map (0x05) -> 0x02/0x0D story -> scene 0x16 (movie) -> 0x02/0x0D -> 0x03`.
    Also landed: two driving knobs for scripted runs, `OGRE_TAP_SCENE_BUTTON`
    (a scene-keyed tap schedule — wall-clock slots land on the map in one run and
    in the attract loop in the next) and the live console's `press <buttons>
    [polls] [x] [y]` (a synthetic pad press + analog stick), which is what made
    the map drivable (`press right` needs a long hold — the game polls input far
    faster than 60 Hz). See `docs/HANDOFF-2026-09-17-session68.md`.
  - ✅ **The Organize Screen renders (session 69) — scene `0x06`'s 355 KB module
    is now bank unit S.** Picking `Organize Screen` from the mission's `R` menu
    (hold `R`; entry 1 `Dispatch`, 5 `Mission Objective`, 7 `End`) entered scene
    `0x06` and bounced back to `0x03` in **68 ms** — the developer's *"the screen
    just reloads"*. The scene's enter `func_8017B6D0` DMAs **ROM `0x87220`
    (`0x56D60`) → RAM `0x8019A7C0 … 0x801F1520`** (no BSS) and `jal`s the module's
    entry `0x801C19B0`; the generic scene update `func_8017B858` and hook
    `func_8017B9C8` (shared by scenes `0x05`/`0x06`/`0x07`) dispatch on
    `*(0x801977E8)`, which the enter sets to **2**, so they `jal` `0x801C214C` and
    `0x801B7FC0`. All three stubbed out, so the screen was never built and the
    state machine took its done arm. The RAM is a **swappable arena** (record 7's
    `0x101D00`, record 3's `0x0EBBD0`, overlay C `0x1CE040`, units H and M — and
    the main unit has its *own* real function at `0x801B7FC0`), so all three calls
    are `cross_bank.py dispatch --only` targets. Now: 406 functions,
    `bankS.elf` byte-identical to the ROM with 1382 symbols at their addresses,
    **19 units / 29 records / 3094 functions**, developer-confirmed live
    (`docs/proofs/native-organize-screen.png`). See
    `docs/HANDOFF-2026-09-17-session69.md`.
  - ✅ **Test-automation premise settled (session 70): the port's *timeline* is
    wall-clock, but its *state* is reproducible to the byte — so an input
    recording must be anchored on game state, not on frame/poll index.** Measured
    with a temporary probe (scene-armed, poll-counted script: title → Load Game →
    map → three cursor steps left, then a sample of the map's `state` struct):
    **input polls are ~1:1 with VI retraces** (`min=1 max=2 mean=1.01` per
    retrace), which **corrects the "the game polls input far faster than 60 Hz"
    claim of session 68 and the console help** — a cursor step needs a long hold
    because the cursor integrates a small delta *per retrace*, not because the poll
    rate is high. Two runs at `OGRE_SPEED=4` differ **only** in the absolute clock
    (a 16-19 retrace boot offset that every later event keeps); normalised for it
    they are byte-identical at map entry and after the input (same hexdump, `dir`,
    `statehash80`). A third run at `OGRE_SPEED=2` is byte-identical too, except the
    free-running animation tick `state[+0x108]` (373 vs 374) — an assertion hazard,
    not a state divergence. The VI retrace clock is wall-clock
    (`events.cpp:237`), which is *why* absolute indices desynchronise. Consequence:
    an emulator **savestate** is a data oracle (guest addresses are identical —
    the port runs the ROM's instructions; `OSThread::context` host pointers are
    what cannot cross a process, session 65) and not something to load; an emulator
    **input recording** converts into state-anchored, poll-counted segments, and
    the game's own `.srm` remains the build-independent fixture for the map/menu
    moments (`tools/sramsave.py`; title → `0x12` → `0x05` re-verified). The probe
    was reverted; no files changed net. A `DECISIONS.md` is named by AGENTS rule 10
    but is absent from the tree. See `docs/HANDOFF-2026-09-17-session70.md`.
  - ✅ **The tutorial's practice stage runs its own module (session 71) — bank
    units T and U.** The developer's *"open the tutorial … pick an option … we are
    placed in a map and nothing happens, without enemies"* is scene `0x03` entered
    with the mode word `D_80193700 != 0` (descriptor **`0x8018F364`**, mask
    `0x0004038C` — records 2,3,7,8,9 **and 18**, vs the normal mission's
    `0x8018F350`/`0x0000038C`). That path loads a **second bank of record 18's
    arena** — ROM `0x1C32D0` (`0x5D50`) → RAM `0x8022A860` — which the port had no
    code for: four call sites (`0x8022A860`, `0x8022A88C`, `0x8022AA94`,
    `0x8022B36C`, from unit A's record 3 and unit N's record 7) hit the runtime's
    logging stub, 4608 calls in a 40 s run. The loader is record 18's
    `func_ovlB_80221D50` (RAM `0x80221D50`): it indexes a `0x28`-byte segment table
    at RAM `0x80229DDC` and a `0x1C`-byte mode table at `0x80229E88` by
    `D_80193700`, then `func_8009DA50(0x1C32D0, 0x8022A860, 0x5D50)` and `jalr`s
    the mode record's `+0x08` word. The module's data half is the **tutorial
    instruction text** (`Stationing unit will gather information.`, `This will end
    the instruction on Stronghold Command. Proceed?`, `This concludes the tutorial
    on Use Item command. Proceed?`), so the map was inert without it. Now **unit
    T** (`config-bankT.*`, entries forced for the three cross-record targets,
    BSS `0x802305B0..0x802305F0` in `RAM_END`) and **unit U** for the arena's
    mode-2 bank (ROM `0x1C9020`, `0x5020`, entry `0x8022A87C`; no observed route
    loads it, but it shares the RAM so the bank map must be able to evict unit T).
    Verified with `OGRE_SCENE=tutorial` + scene-keyed A taps:
    `0x17 → 0x02 → 0x0D → 0x03 (0x8018F364)`, **0 stub calls**, the tutorial
    instruction box renders and the lessons advance
    (`docs/proofs/native-tutorial-practice-field.png`, `-command.png`); the
    `down down` lesson selects mode 0 (the normal mission) and is also stub-free.
    See `docs/HANDOFF-2026-09-17-session71.md`.
  - ✅ **Combat and items run (session 72) — eleven more arena banks, and the
    silent wrong-bank hang.** The developer's *"combat and items are very
    important parts of the game"* was driven as an interactive session: the client
    logs to a file, the developer plays, this session reads the log. The new live
    console **`dmatrace`** command prints the accumulated streamed-DMA map on
    demand (`OGRE_DMA_TRACE`/`_FULL`). Three findings, all the swappable-arena
    class:
    1. **Settings menu** (ROM `0x1A4BE0`, `0x4680` → RAM `0x80214FA0`) — `UNKNOWN
       module` and **no stub**: because the port had no record, `on_streamed_dma`
       never evicted unit Q, so `jal 0x80217E50` ran Q's body and the screen froze.
       Now **unit V** (entries `0x802170EC`/`0x80217BE4`/`0x80217E50`/`0x8021905C`).
       *(This session also corrected its own first V config: a bad hex subtraction
       made it `0x14680` instead of `0x4680`, silently swallowing the next bank.
       `elfcheck` cannot see that — the extra bytes are ROM bytes at their ROM
       offsets; re-deriving the loader caught it.)*
    2. **Combat** — record 9's `0x17F9E0` (`0x91A0`, **unit W**; stubs
       `0x80217690`/`0x80217660`) and record 10's `0x22A250` (`0x10120`, **unit X**;
       stub `0x801EFACC`). Developer: *"combat played beautifully."*
    3. **Shop** — `0x1977B0` (`0x4F80`, **unit AB**): its data half is the shop
       dialogue (*"What do you have on sale?"*) and its entry `0x80219594` was
       stubbed **3553×** (once per frame), so nothing drew. Enumerating every
       `jal 8009da50` arena load in unit N showed that **record 9's arena has
       thirteen banks** and only five were compiled; the eight missing ones are
       now **Y, Z, AA, AB, AC, AD, AE, AF** (two-character unit names — the first
       in the project). Also corrected: **unit P**'s size `0x6200 → 0x6030`
       (chunk-rounded, session 59's rule) and **P/Q BSS** zeroing.
    Final run: **0 stubs, 0 UNKNOWN**, scenes `0x04 → 0x12 → 0x03` (mission) →
    `0x06` (Organize) → `0x02`/`0x0D` (dialogue/combat); developer: *"everything
    runs beautifully."* `make bank-recomp`: **32 units, 42 records, 3452
    functions**, `check-banks OK`. The reported slowdowns are render-bound
    (`processDisplayLists` median 4.4 ms at `OGRE_SPEED=4`, whose frame budget is
    ~4.15 ms; one ~1 s stall, likely a Vulkan pipeline compile), not missing
    modules. See `docs/HANDOFF-2026-09-17-session72.md`.
  - ✅ **The arena map is a tool, and every reachable streamed bank is compiled
    (session 73).** `tools/arenamap.py` derives every streamed bank from the
    loader pattern itself — `lui/addiu` ×3 → `jal func_8009DA50` → `subu` (the
    size), bracketed by `func_800900C0(ram_base, code_size)` /
    `func_80090010(code_end, data_size)` / `func_80093380(data_end, bss_size)` —
    and prints per bank: **ROM start, RAM base, size, code/data split, BSS, the
    loader instruction, whether a unit links it, and the cross-record entry
    candidates with a real-function-start check.** It scans all 33 ELFs and finds
    **27 bank loads across 5 arena RAM windows; 26 are compiled.** `--verify`
    confirms the ROM range and size of every compiled record against the loader
    that DMA's it; `--coverage` proves the negative — every `jal 0x8009DA50` in
    the ROM (76) lies inside code the scanned ELFs disassemble, and the two
    uncovered ROM gaps contain **0** of them, so no loader can be hiding. The one
    uncompiled bank is a `0xE80` scene-`0x14` setup fragment
    (`0x0023A370 → 0x801D0860`) whose RAM unit C's record 10b already owns and
    which no observed run streams. `make bank-recomp` is green
    (33 units / 43 records / 3509 functions, `check-banks OK`), `elfcheck --syms`
    is 0 bytes on all 34 ELFs, and driven runs pass `runlog --check` with **0
    stubs / 0 `UNKNOWN module`** (tutorial: 37 bank loads,
    `0x17 → 0x02 → 0x0D → 0x03 ×2`; map: title → `0x12` → `0x03`; scene `0x14`
    below). The whole table is recorded in `docs/scenes.md` under
    *"Every streamed arena the game can load (session 73)"*. See
    `docs/HANDOFF-2026-09-17-session73.md`.
  - ✅ **Scene `0x14` is the shop, and finding it exposed a missing bank —
    record 16, now unit AG (session 73).** The developer asked to boot scene
    `0x14` directly and identify it from a screenshot. The poke lands ~1 run in 5
    (it races the boot), and the scene's first forced run logged
    `[bank] UNCOMPILED streamed record 16: rom=0x279FF0 ram=0x802258B0
    size=0x7840` plus three stubbed calls (`0x80226B7C`, `0x8022859C`,
    `0x80226CE0`) and drew nothing. **Record 16 is not in the segment table** —
    entry 13 declares `rom 0x275820..0x279FF0`, which unit C links, and the game
    then loads a second module over the same arena from `0x279FF0`; its size is
    the table's `ram_end` (`0x8022D0F0`) minus `0x802258B0`, exactly the number
    the port logged. **125 call targets across eight units** reach into that
    window (three other arenas overlap it: unit T's `0x8022A860`, unit K's
    `0x8022ACB0`, unit L's), so every call from unit C's code into the high half
    was bound to record 13's layout — the session-45/67/72 mis-binding class. It
    is now **bank unit AG** (`bankRec16b`, ROM `0x279FF0`, `0x7840` → RAM
    `0x802258B0`; `config-bankAG.*`, `config/symbols/symbol_addrs-bankAG.txt`, `BANK_UNITS`),
    and with it scene `0x14` **renders with 0 stubs** and
    `runlog --check` PASSes: **the Witch's Den**, *"Old Witch / Heh heh heh…
    Can I help you?"*, `WAR FUNDS 0001000 Goth`
    (`docs/proofs/native-scene-14-shop.png`). **Developer-confirmed route:** every
    mission has one city with a **witch den**, and visiting it is how the player
    **revives dead party soldiers** — the developer walked in from normal play and
    it works, so this was a real-route bank, not a test-only one. `--coverage` now leaves only **one**
    uncovered ROM gap. See `docs/HANDOFF-2026-09-17-session73.md` §8.
  - ✅ **Sound: the audio microcode runs and produces PCM (session 75).** Every
    type-2 (`M_AUDTASK`) task now executes the ROM's own recompiled audio
    microcode, and the game's AI buffer carries real stereo samples: a live RDRAM
    dump at t=5 s (scene `0x09`, the intro) shows **1104/1104 non-zero samples**
    in the buffer the game hands `osAiSetNextBuffer` (`*(0x800A9B90)` → buf
    `0x8013EE70`, len `0x8A0`, run-specific), values spanning roughly −9600..+5800
    with a
    smooth waveform. `runlog.py --check` PASSes on a stock 15 s run (809 type-2
    tasks, 0 stubbed non-gfx tasks) and `OGRE_AUDIO_UCODE=0` still gives the old
    silent stub as an A/B escape hatch.
    **The session-74 wall was one wrong constant: `text_address`.** `0x8009E050`
    is not a second boot block — it is the audio text, loaded by the *standard
    libultra RSP boot code* at `task->t.ucode_boot = 0x8009ECB0` (ROM `0x2F0B0`):
    that loader sets `at = 0xFC0`, DMAs `ucode_data` (`0x800ABDA0`, 0x800 bytes)
    to DMEM 0 and a fixed `0xF80` bytes from `ucode` to **IMEM 0x1080**, then
    `jr 0x1080`. Compiling at `0x1000` (session 74) put every absolute
    `j`/`beq` target 0x80 bytes off, so the driver's command-list DMA helper was
    entered mid-instruction and `$29`/`$30` were never set. With `0x1080` the
    driver dispatches its real command ABI. `config/rsp-audio.toml` is now
    `text_offset 0x2E450`, `text_size 0xC60` (`0xF80` would compile the boot
    loader's own bytes into the image and emit `goto L_1064`-style undeclared
    labels), `text_address 0x1080`, plus
    `extra_indirect_branch_targets` = the 16-entry ABI dispatch table held as
    **data in `ucode_data` at DMEM 0** (`lh $at,0($at)` after
    `srl $at,$k0,0x17`), including `0x10B4` for the `jr $5` return. The command
    stream is the standard aspMain ABI (CLEARBUFF/LOADBUFF/SAVEBUFF/MIXER/
    INTERLEAVE/LOADADPCM); opcodes 7/8 are 0 in the game's table (no
    SEGMENT/SETBUFF). Session 74's four RSPRecomp fixes stay in
    `patches/n64recomp-ob64.patch`. `tools/runlog.py` now classifies `M_GFXTASK`=1 /
    `M_AUDTASK`=2 (it had treated type 2 as gfx) and fails `--check` on any
    non-gfx task served by the stub. **One rough edge, found by chasing a 1-in-25
    crash:** the game's own `SETLOOP` (op 0x0F) stores its loop address as a word
    at **DMEM 0x0E**, which is exactly dispatch-table entries 7/8 (both `0x0000`
    in `ucode_data` — this driver has no SEGMENT/SETBUFF). A rare op7/op8
    dispatch therefore `jr`s to half an audio-buffer address
    (`Unhandled jump target 0x1226`), which the recompiler cannot execute; the
    port now logs it and drops that one task (`audio_ucode_guard` in
    `app/src/rsp.cpp`, audio-only) instead of exiting. **Latency (session 76):
    fixed.** The game's audio thread is gated by `osAiGetStatus()`'s
    `AI_STATUS_BUSY` bit (it spins on it before generating each buffer); the
    runtime hardcoded that register to 0, so the game produced 552-frame buffers
    at the video frame rate (60/s = 33 120 frames/s) while the DAC plays 32 000 —
    a 3.5 % surplus that grew the SDL queue from 22 ms to **2 070 ms over 70 s**
    (the developer's "1–2 s delay"). `ultramodern::audio_dma_busy()` now reports
    busy while more than a **52 ms cushion** of audio is queued and
    `osAiGetStatus_recomp` returns `0x80000000`. **The cushion is load-bearing:**
    the literal "busy while anything is queued" cut the queue to 17.2 ms but made
    the game generate only 34 buffers/s (a 40 % underrun, every push onto an
    empty queue) — the "music drags and tears" the developer heard next, because
    the music engine advances per generated buffer. The port's audio loop only
    iterates ~120×/s, coarser than one 17 ms buffer, so the bit must clear before
    the queue empties. A measured sweep (0/36/52/69 ms → 34.0/58.5/58.0/58.0
    pushes/s, empty pushes 68/15/0/0) picked 52 ms. Final: **58.0 pushes/s = the
    DAC rate, 0 underruns, queue 2–57 ms**. Carried in
    `patches/n64modernruntime-ob64.patch` (regenerate with the nested `N64Recomp`
    submodule excluded). See
    `docs/HANDOFF-2026-09-18-session76.md`. **Other open follow-ups:** the
    runtime still wakes the game's audio thread with the session-16 dummy
    response on queue `0x800C49E8`; the real AI-completion event
    (`osSetEventMesg(OS_EVENT_AI, …)`) is registered but never fired — pacing no
    longer depends on it, so it is now only a wake-up mechanism. Audio is
    correct and paced (~58 buffers/s); see
    `docs/HANDOFF-2026-09-18-session75.md` and
    `docs/guides/rsp-microcode.md`.
  - ✅ **The mission renders (session 67).** The developer's **suspend save**
    (`assets/save-mission-1.srm`, third SRAM slot) resumes at **scene `0x03`**,
    the mission — descriptor `0x8018F350`, mask `0x38C` (records 2, 3, 7, 8, 9) —
    and it now enters, runs and draws: 3D terrain, cliffs, woods and rivers, the
    2D party sprite with its selection brackets, the `Stronghold` tooltip and the
    unit panel (`docs/proofs/native-mission-scene.png`,
    `docs/proofs/native-mission-unit-panel.png`). Reaching it took four fixes,
    all of the swappable-RAM class: (1) records **7/8/9** were uncompiled →
    **unit N**; (2) unit A's record 3 called `0x801AD6BC` (record 6's RAM) and —
    both records being in unit A — the recompiler bound it to unit A's record-6
    body, but scene `0x03` loads record 3 **without** record 6 → record 6 moved to
    **unit O** so the call dispatches; (3) cross-*record* `jal` targets are
    invisible to the per-record disassembler, and a `LOOKUP_FUNC` onto a
    non-entry is a **silent no-op** (`get_function` has no interior fallback), so
    the entries are declared in `symbol_addrs-bank{N,O,P,Q}.txt` (a subsegment
    split would insert the assembler's 16-byte `.text` padding and break the
    ELF-vs-ROM check); (4) record 9's arena streams **two more banks** the segment
    table does not describe — units **P** (ROM `0x171EC0`) and **Q** (ROM
    `0x165FE0`), both → RAM `0x80214FA0` — found from the port's stub log and a
    light `do_rom_read` probe (the full DMA trace perturbs the run onto the other
    bank). Also fixed: **`tools/cross_bank.py check-banks` was a silent no-op**
    (its YAML parser dropped the `name:` on `- name: bankRecX` lines), so the
    session-45 invariant had been unchecked; it now parses names, uses the scene
    record masks to tell "always loaded together" from a real split (1728 → 23),
    and allowlists the pre-existing unit-C backlog. See
    `docs/HANDOFF-2026-09-17-session67.md`.
  - ✅ **The battery save works end to end (session 66) — the chip is 32 KiB of
    SRAM, and the runtime now owns it.** Session 65's correction stands (the save
    is a battery, not a Controller Pak; the pak strings are the copy/backup
    feature) and is now resolved: `entry.save_type = recomp::SaveType::Sram`
    (`app/src/main.cpp`). Evidence for SRAM: mupen64plus's database
    (`SaveType=SRAM` for this ROM's CRC) and the game's own code — `func_8008A040`
    builds the save `OSPiHandle` with `baseAddress 0xA8000000` (physical
    `0x08000000`), the boot accessors `func_80074CF0..` read the whole 32 KiB in
    256-byte DMAs at offsets `0..0x7F00`, and `func_80074BF0` → `func_80074C58`
    writes it back the same way with direction 1. That path is the game's own
    non-bridged DMA (`func_8008BC40` → the `D_800AA408` queue), so
    `librecomp/src/pi.cpp`'s inline handler now picks the device from the
    `OSPiHandle` in each `OSIoMesg` (`pi_perform_dma`) instead of treating every
    transfer as ROM. Verified: no file → the game formats the blank battery and
    the port writes a real 32768-byte image (`QuestOG3` signature); restart → the
    game reloads it and does not rewrite it; a garbled image → the game repairs
    and rewrites it; a converted parallel-n64 save with progress → the game
    accepts it, and with it the **title's `Load Game` entry appears and its cursor
    defaults to it** (same taps with no save enter New Game): `0x04 → 0x12 →
    0x05`, dev-confirmed loaded. **New tool `tools/sramsave.py`** imports/exports
    emulator wrappings of the same 32 KiB (parallel-n64's container has the SRAM
    at `0x20800`, 32-bit byteswapped). New knob `OGRE_PREF_DIR` relocates the
    config/save dir. See `docs/guides/app-build.md` → "Saves" and
    `docs/HANDOFF-2026-09-17-session66.md`.
  - ⚠️ **Correction (developer, session 65): the game's own save is a
    *battery-backed cartridge save* (SRAM/FlashRAM/EEPROM), not a Controller Pak.**
    The pak strings are the *copy/backup* feature (`Data loaded to Game Pak.`).
    **Resolved in session 66** — the chip is SRAM; see the entry above.
  - 🚧 **The Controller Pak device is implemented (session 65) — this is the
    *copy/backup* path, not the save.** The missing link for it is the *pak menu*,
    which only unit H's scene can reach. `pak.cpp` in the
    (gitignored) runtime is no longer upstream's stub: it is a real device — a
    flat 32 KiB image in 32-byte blocks, block addresses `>= 1024` ignored (the
    bank/enable register, which is why libultra's own bank probe reports
    `banks == 1`), blocks 1..6 write-protected unless `force == 1`
    (`PFS_LABEL_AREA = 7`, `PFS_FORCE = 1`, read out of this ROM's own
    `__osContRamWrite`), a fresh image formatted like mupen64plus
    `format_mempak`, and the image persisted to
    `<config>/saves/<game id>.mpk` (interchangeable with emulator `.mpk` dumps).
    **Only three functions needed bridging**, because the game's PFS is libultra
    the ROM already contains and `make recomp` already compiles: the SI-touching
    `__osContRamRead` `0x80097BD0`, `__osContRamWrite` `0x80097DC0`,
    `__osPfsGetStatus` `0x80096EC0` (`config/symbols/symbol_addrs.txt` + N64Recomp's
    `reimplemented_funcs`). `osContInit` now reports a pak in controller 1
    (`Pak::ControllerPak`). **The gate:** the pak API is called from **unit H
    only** (the form/UI module) — `func_ovlH_8019C69C` and the three screens
    `0x8019DBA4`/`0x8019DC88`/`0x8019DEB4` — and the pak menu is *scene `0x07`'s
    descriptor callback `+0x18`* (`0x8018FDAC` → `func_8017BB28` → `jal
    0x8019C69C`; `+0x14` is the form). The map's module never calls the pak API,
    and a **forced** map entry's Save (R → right×3 → A → A → Yes) draws the map's
    slot window and the overwrite prompt and then stops: no device call (lldb
    breakpoints never hit), no DMA (`OGRE_DMA_TRACE_FULL=1`: unit H's `0x712A0`
    is never streamed), no scene change (`c`: `pending=0x0005`) and no pak-manager
    call. So a **natural** run to the map is required to exercise the device (a
    forced entry has no army/save state — the same caveat as the mission markers).
    Next: drive `R → Save → Yes` on a natural map and read the log
    (`[pak] formatted/loaded …`) and the on-screen message; drop a real 32 KiB
    `.mpk` at the path above for the Load half. See
    `docs/HANDOFF-2026-09-17-session65.md` §8–§9 and
    `docs/HANDOFF-2026-09-16-session58.md` §3c.
  - ⬜ **The save-system recon (session 58)**, kept for the entry-point map — but
    note its premise is **corrected in session 65**: the *save* is a battery-backed
    cartridge save (SRAM/FlashRAM/EEPROM); what follows describes the
    **Controller Pak copy/backup** path (`osPfs*`), not the save. 105
    `jal`s from the main segment into the PFS cluster
    (`0x8009616C`..`0x80097DC0`); the whole menu is ROM text
    (`0x790EC..0x7967C` — `Controller Pak Menu`, `Save`/`Load`/`Erase`,
    `Insert Controller Pak.`, `1 note 25 pages to save.`, `Data saved to
    Controller Pak.`, `Game Data 1`/`2`) which **exists in unit H's ROM half
    only** (unit M has just `No Data`), because the menu is unit H's code.
    See `docs/HANDOFF-2026-09-16-session58.md` §3c and `docs/scenes.md` row 9.
  - ✅ **Scene `0x16`'s content is confirmed** (session 58): the closing movie's
    five shots all render, in order, and match the developer's retail
    screenshots — `ATLUS USA / presents` → `Developed & licensed by Quest /
    Nintendo` → `Ogre Battle Saga / Episode VI` → `Person of Lordly Caliber`
    over the Lodis flower → a travel montage (campfire, the party on a cliff at
    sunset, the parchment world map with the red route and dagger). Proofs:
    `docs/proofs/native-newgame-movie-{atlus,quest,episode-vi,lodis,campfire,sunset,map}.png`;
    the shot list is recorded in `docs/scenes.md` row 6. Open (uninvestigated,
    and it may be the game's own letterbox rather than a defect): a 1-pixel
    vertical line and grey bands in some transition frames.
  - ✅ **Live debug console (session 56, developer's suggestion)**: the running
    game can now be queried on demand instead of only at a bounded run's exit —
    a watched command file (`OGRE_CONSOLE_FILE`, default
    `/tmp/ogre-console.txt`: command lines run and the file is removed) or the
    number keys `1`..`9` (`OGRE_KEY_<n>`). Commands: `r`/`rh`/`rb`/`rk`, `d`,
    `f`, `fb`, `s`, `k` (checksum for A/B), `w`, `c` (scene/descriptor/mask/step)
    and `dump` (the whole 8 MiB **at that instant** — the fix for "the exit dump
    is always too late"). It runs on the main thread, in
    `app/src/sdl_platform.cpp`; documented in `docs/guides/app-build.md` and
    `AGENTS.md` §4.
  - 🚧 **Why the game's own frame index lands on the placeholder is open
    (session 48)**: `D_800C4BB8` is the VI manager's "displayed buffer" word
    (written by `func_8007307C`, which `func_80089540` — N64 Thread 5 — calls
    from a message object; a `watch.sh --value` conditional watchpoint caught
    it). The fix uses the buffer the draw landed in rather than answering
    whether retail reaches the same mismatched state. See
    `docs/HANDOFF-2026-09-15-session48.md` §4.
  - ✅ **The cathedral scene's background renders correctly (sessions 46/47; 50
    implemented RT64's YUV16 decode; 51 implemented the missing S2DEX2 geometry;
    **52 fixed the YUV -> RGB conversion, and the backdrop is the cathedral**)**.
    `docs/proofs/native-newgame-cathedral.png` and
    `-cathedral-background.png` are the corrected captures; the pipeline below
    still describes how it gets there, and the "still open" note at the end is
    superseded:
    the step-2 display list's first `G_TRI2`
    quads are a full-screen 320x240 RGBA16 blit from guest `0x80243E28`, and that
    buffer was uniform (`0x0843` in RT64, `0` in the null build). The image is an
    **N64 JPEG** (`'HU'` container holding `'HUFF'` + numMB, asset `0x00183352` =
    ROM `0x7175A2`), and the pipeline is: CPU Huffman decode `func_8008B250` →
    **four `M_NJPEGTASK` (type 4) RSP tasks** (`ucode=0x8009ED80`/`0x7C0`, boot
    `0x8009ECB0`/`0xD0`, tables `0x800AC050`/`0xF0`, `data_size = mbs`,
    `yield_data_size = scale`) decoding **in place to 16-bit YUV** → a *second*
    gfx ucode `0x800A5110` drawing one 16x16 YUV16 texture per macroblock into
    `G_SETCIMG` → a CPU framebuffer readback → assembly of a `'B5'`-headed image
    at `0x80243E10` whose pixels are `0x80243E28`. **Session 47 fixed the
    decoder**: RSPRecomp's `text_address` is a *label base* and must be the RSP
    IMEM DMA address `0x1080` (not `0x8009ED80`, whose low 13 bits rotated every
    `j` target by `0x300`); each task now writes all `mbs` blocks, the scene no
    longer stalls, and the decoder is **on by default** (`OGRE_NJPEG=0` forces the
    stub). **Session 50 implemented RT64's YUV16 decode** (its `G_IM_FMT_YUV` arm
    returned black) and bounded the render wait on the RSP worker
    (`Application::waitForGameFramebuffers`, `OGRE_NJ_WAIT_MS`). **Session 51
    found why nothing was ever drawn and fixed it:** the `0x800A5110` ucode is
    **`S2DEX 2.08`** (`GBIUCode::S2DEX2`; the ucode text and data hashes match
    RT64's database exactly), so its per-macroblock `0xDC`/`0xDA` commands are
    **`G_OBJ_MOVEMEM` (the sub matrix) and `G_OBJ_RECTANGLE_R` (the draw)** — not
    F3DEX2 `G_MOVEMEM`/`G_MTX`, which is why the F3DEX2-named analyzer reported
    "no geometry". RT64's `GBI_S2DEX2` mapped neither command; both are
    implemented now, and a live trace shows **300 object rectangles tiling a
    320x240 framebuffer exactly** (one per njpeg macroblock). The YUV16 TMEM
    layout was redone too: a YUV `LOADTILE` de-interleaves into TMEM's upper
    (luma, one byte per texel) and lower (U/V pairs per two texels) halves, and
    the sampler follows the RDP's model (parallel-rdp `sample_texel_yuv16`) —
    session 50's sampler read luma from a half the load never wrote, so it could
    never have worked. The source word is `[U, Y(even), V, Y(odd)]`
    (mupen64plus-rsp-hle `GetUYVY`, verified against a live macroblock dump),
    converted with the matrix the game programs via `G_SETCONVERT`
    (`k0=175 k1=469 k2=423 k3=222`). **Session 52 found why the colours were
    wrong**: RT64 stored the raw unsigned 9-bit `k0..k3` fields and used them
    directly, while the hardware sign-extends each field and scales it
    `2*K+1` (GLideN64 `gDPSetConvert`, parallel-rdp `set_convert`), and the
    green channel paired `K1` with V instead of U. With both fixed the
    background is the cathedral (red carpet, stone walls, statues) and the
    assembled `'B5'` image at `0x80243E28` renders as the full scene.
    See `docs/HANDOFF-2026-09-15-session52.md`, `-session51.md` (§7 for the
    checks that localized it), and `-session50.md` / `-session47.md` /
    `-session46.md` for the decoder fix and the readings of the display list
    that session 51 §1/§6 corrects.
  - ✅ **The publisher screens now render correctly (session 32)**: the
    "Licensed by Nintendo", ATLUS and QUEST stills were drawn as 640x480
    (they are the game's hi-res mode) but scanned out with the runtime's dummy
    320x240 VI geometry, so only the top-left quarter of the RDP framebuffer
    reached the screen. The cause was a **mis-named libultra symbol**: the
    function at `0x80095820` that `config/symbols/symbol_addrs.txt` called `osViSetMode` is
    really `__osViSwapContext`, and the *real* `osViSetMode` (which stores the
    game's `OSViMode` pointer) is `func_800955C0` and was never named — so the
    runtime never learned the game's VI mode and stayed on its dummy mode.
    `func_800955C0` is now `osViSetMode`, `0x80095820` is `__osViSwapContext`
    (a no-op; the runtime's VI thread does the swap), and the game's
    640x480 `D_800AB9B0` mode now reaches RT64. Proofs:
    `docs/proofs/native-licensed-screen.png`,
    `docs/proofs/native-atlus-screen.png`,
    `docs/proofs/native-quest-screen.png`. See
    `docs/HANDOFF-2026-09-12-session32.md`.
  - ✅ **The mode-switch noise is gone (session 32)**: the 320x240 → 640x480
    switch showed ~0.2-0.5s of colour static before each still, because the VI
    went hi-res while the new framebuffer was only partly drawn and the game's
    blank (`osViBlack`, `func_80095B30`) was not bridged. With
    `osViBlack = 0x80095B30` the runtime honours the blank (`update_vi` sets
    `hStart = 0`, so RT64 clears instead of scanning out stale RDRAM). `osViFade`
    (`func_80095780`) is still unemulated, so fades snap. See
    `docs/HANDOFF-2026-09-12-session32.md` §1d.
  - ✅ **The late crash is fixed — streamed-overlay banks swap at the DMA
    (session 33)**: session 32 localised the ~95s abort to the game DMA-ing a
    different overlay bank over overlay C. The **streamed-segment table** (19
    records of 10 words at ROM `0x387C0`) and the **bank descriptor lists** (11
    banks at ROM `0x38AB8`, pointer array at `0x38AFC`) are now decoded, and the
    records the game swaps in at t≈95s are entries 2, 3 and 6. Because their
    RAM ranges overlap overlay C's, they are recompiled in a **separate unit**
    (`config/banks/config-bankA.yaml` → `build/bankA.elf` → `config/banks/config-bankA.toml` →
    `BankAFuncs/`) linked at their true RAM addresses with an `ovlA_` symbol
    prefix; `app/src/bank_overlays.cpp` registers a record's functions when the
    game DMA's it and drops whatever bank occupied that RAM. The runtime gained
    `notify_rom_read` (called from `recomp::do_rom_read`), `unload_overlapping_overlays`
    and `load_function_bank`. Verified: the 170s unattended run is now **exit 0,
    4850 display lists at 168s** (was exit 134 at display list 2770 / 94.8s);
    proof `docs/proofs/native-post-bankswap-present2805.png` (a rendered night
    castle scene at present 2805, after the swap). See
    `docs/HANDOFF-2026-09-12-session33.md`.
  - ✅ **The next attract scene's bank is identified (session 33)**: the attract
    loop is title → "lore" story movie → title → a variant screen; the second
    transition (t≈360.7s) runs scene `0x0C`, whose descriptor at `0x8018FB58`
    carries **record mask `0x3C00` = records 10, 11, 12, 13**. Uncompiled, its
    entry returned through the streamed stub and the scene bounced back to the
    title. Those four records are now **bank unit C** (`config/banks/config-bankC.yaml`,
    `BankCFuncs/`, 848 functions). Units are partitioned by *RAM*, not by game
    bank: records whose RAM ranges overlap cannot share an ELF.
  - ✅ **Overlays stream more code into a per-record arena (session 33)**: a
    segment-table record is only the resident part. The record's `ram_end` word
    (`+0x04`) covers an arena the game fills on demand from code modules in the
    ROM gap before the next record. Record 10's arena modules
    (`bankRec10a` ROM `0x213AE0`→RAM `0x801D0860`, `bankRec10b` ROM
    `0x23B1F0`→RAM `0x801E6FD0`) hold the four functions scene `0x0C` called;
    they are compiled into unit C (147 + 90 functions) and registered by the
    same DMA hook. `tools/gen_bank_syms.py` defines the `D_ovlC_*` data labels
    splat leaves undefined in those `asm` sections.
  - ✅ **The unit-info screen renders (session 33)**: with records 10–13 and
    their arena modules armed (9 records / 1289 functions) scene `0x0C` has
    **zero stub calls** and draws the unit stat screens
    (`docs/proofs/native-unit-info-dragon-tamer.png`,
    `docs/proofs/native-unit-info-griffin.png`), instead of bouncing to the
    title. New debug knobs: `OGRE_SPEED=<n>` (scale the emulated clock),
    `OGRE_FORCE_SCENE=<hex>` (switch to a scene without waiting out the attract
    loop), `OGRE_CAPTURE_EVERY=<n>` (frame-sampling captures).
  - ⬜ **The title menu (New Game / Tutorial) still crashes on a cross-bank
    *direct* call (session 33)**: pressing Start reaches scene `0x18`, which
    asks for **record 1** (`mask 0x2`), and scene `0x02` (New Game / Tutorial)
    asks for **records 0 and 14** (`mask 0x4001`). All three are now compiled
    (record 1 = unit D, record 0 = unit E, record 14 in unit C; the app arms 12
    records / 1398 functions), but the crash persists: N64Recomp compiles a
    `jal` to a known function as a **direct** call, so overlay B's calls into a
    swappable bank range run overlay C's compiled body while a different bank is
    resident (lldb: `func_801989AC + 1062`; 58 such call edges, 53 targets).
    Fix: route calls that cross into a bank-swappable range through
    `get_function` (post-step over `RecompiledFuncs/*.c`, or move overlay C out
    of the main ELF), and seed each bank unit with the cross-bank targets as
    function entries. The attract loop (lore + unit-info) is unaffected.
  - ⬜ **Remaining uncompiled streamed records**: 4, 5, 16, 17, 18, plus each
    record's arena modules (overlay C/record 15 and units A/C/D/E are done).
    Record 17 loads at `0x80197B90` and needs a unit of its own.
  - ⬜ **19 more splat split symbols in overlay C (session 30)**: the same class
    of bug (a symbol cut out of the middle of a logical function makes the
    compiled function return without unwinding its frame, clobbering the
    caller's `$sp`/s-registers). All are now covered by `function_sizes`
    overrides, and since session 31 N64Recomp redirects calls to those split
    symbols back into the containing body, so the workaround is enforced
    instead of relying on the override alone.
  - ✅ **N64Recomp cuts a function that falls through into a discovered shared
    epilogue (session 30's wall) — fixed in session 31**: see the session-31
    entry above and `docs/HANDOFF-2026-09-12-session31.md` §2.
  - ⬜ **The native port needs `OGRE_NO_AUDIO=1` on hosts where
    `SDL_OpenAudioDevice` blocks (session 30, environment)**: the runtime calls
    it from the game start thread's `preinit`, so a blocking open wedges the
    whole boot (RDRAM stays all zero, no display lists). Gate added in
    `app/src/sdl_platform.cpp`.
  - ✅ **The native window renders (session 27)**: the session-26 "window is
    black" conclusion was a measurement artifact. The display is locked, so
    `screencapture -x` returns wallpaper and `screencapture -l<id>` returns the
    last frame the window server committed (a cycling clear colour stays one
    colour across seconds of captures). The new ground truth is
    `OGRE_CAPTURE_PRESENT=<path>`: RT64 GPU-reads back the exact swap-chain
    texture it presents (texture -> buffer readback added to plume's Metal
    backend; `patches/rt64-plume-ob64.patch`). With that, the synthetic-frame probe
    shows the seven bars end to end in **both** paths - the RDRAM upload path
    and the real RDP display-list path - and the result is
    `docs/proofs/native-synth-frame-rdp.png`. Four display-list encoding bugs
    had to be fixed first: two-word commands were emitted as two `emit()` calls
    (null scissor, empty fill rect), `G_FILLRECT` had its operand halves
    swapped, coordinates were not 10.2 fixed point, and `G_RDPSETOTHERMODE` was
    pre-shifted so the cycle type was never `G_CYC_FILL`. Two presenter
    behaviours were also needed: RT64 only presents when the VI or its RDRAM
    copy changes, so a stalled boot freezes the window on an old frame
    (`OGRE_PRESENT_ALWAYS=1`); and its framebuffer manager does not know an
    RDP-only colour image, so the presenter uploaded empty RDRAM instead of the
    drawn render target (`OGRE_PRESENT_FBTARGET=1`). Both are turned on
    automatically by `OGRE_SYNTH_FRAME`. See
    `docs/HANDOFF-2026-09-12-session27.md`.
  - ✅ **Web build: game audio is muted by default (session 22) — native now
    plays it (session 75).** The **native** port runs the game's recompiled
    audio microcode and feeds real PCM to SDL (see the sound entry above). The
    **web** build is unchanged: its AudioWorklet is still created and still
    drains the ring (the game's `get_frames_remaining()` backpressure is
    load-bearing), and its output is still silenced by default — session 22
    added that because the RSP audio task was only auto-completed *then*, so
    what the game handed the AI interface was stale RDRAM and playing it was a
    wall of screeching. `?audio` on the URL, or
    `window.ogreAudio.setEnabled(true)`, unmutes it. Re-pointing the web build
    at the now-working microcode (and removing that default-off) is deferred
    web work. See `docs/HANDOFF-2026-09-11-session22.md` and
    `docs/HANDOFF-2026-09-18-session75.md`.
  - ✅ **The intro's frame-31 "runaway walk" is fixed (session 21)**: OB64's
    display lists mix KSEG0 and **segmented** addresses, and the renderer
    resolved a segment as `(base_high_byte << 24) | offset` instead of RT64's
    `segment_base + offset`, so `DE000000 0E000000` (segment 14) sent the DL
    walker to zeroed rdram (4 M `G_NOOP`s) and `FD180000 0F000000` (segment 15)
    sent the texture decoder to garbage. `G_SETOTHERMODE_H/L` also never applied
    its data word (so every draw was `G_CYC_1CYCLE`), alpha's D selector read the
    wrong word, the DL walker ignored `G_MW_SEGMENT`, and the combiner was
    evaluated with position-independent selectors. With all of that fixed the
    title scene reaches ~39 display lists per 75 s (session 20 escaped at ~31)
    with the correct `G_CYC_2CYCLE` + bilerp + perspective state and a two-unit
    texture abstraction (colour from TEXEL1, alpha mask from TEXEL0). Next:
    pacing (half of all boots still stall at 1-8 display lists; ~3 gfx frames/s),
    the blend/render-mode decode, and a real TMEM/tile UV model. See
    `docs/HANDOFF-2026-09-11-session21.md`.

- ⬜ Streamed/overlay code segments (battle engine, cinematics) — after first boot.
- ⬜ Asset extraction (sprites, text, audio) — after first boot.
