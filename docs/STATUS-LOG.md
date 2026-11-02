# Session status log

This is the per-session status log that `PLAN.md` carried from session 3 to
session 97. It is reverse-chronological: the newest session is first. It is
kept for the reasoning and the cross-session corrections it records.

`PLAN.md` now holds the live status, the open-work list and the roadmap. The
authoritative per-session record is `docs/HANDOFF-YYYY-MM-DD-sessionN.md`.

- ✅ **The left stick's Y was inverted on both builds; it now matches the N64 pad (session 211).** Issue #21 reports that pushing up on an Xbox controller moves the world-map cursor down. The native path passed `SDL_GameControllerGetAxis(LEFTY)` through unchanged, and SDL documents that axis as `-32768 (up/left) to 32767 (down/right)` (`SDL_gamecontroller.h`); the browser passed the Gamepad API's axis 1 raw, which is also negative up. The N64 pad's `stick_y` is positive up, and the runtime's `convert_to_n64_range` writes `stick_y = sin(atan2(y, x)) * magnitude`, so physical up arrived as `stick_y = -82`. The game's world-map mover `func_ovlM_8019AF0C` reads the pad at `0x8019B438` and computes `screen_y - stick_y / 8` at `0x8019B48C`/`0x8019B490`, so a negative value moved the cursor down. The native `get_input` now negates the axis and `app/web/web.js` negates `axes[1]`; the right-stick C layer already read SDL's convention. The console's `press ... x y` sets the stick after the SDL read, so it keeps the N64 sign. `make app` and `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` are clean. No controller was attached to this machine, so the fix was verified at the instruction level and by the build, not by a hardware press. See `docs/HANDOFF-2026-11-02-session211.md`.

- ✅ **The browser cathedral backdrop's top-left quadrant was a writeback row-band drop, and the writeback guard is now per row (session 207).** The New Game cathedral assembles four njpeg sub-images into the 320x476 colour image at `0x025C00`. Pass 0 (320x240) copied `14875` non-black pixels instead of `69915`, so the backdrop's top-left quadrant was black. The guard hashed the image's whole `row_bytes * height` range, and `0x025C00..0x070200` contains the next game framebuffer at `0x04B400` (row 240 of the target); that framebuffer's writeback changed the hash and the whole pending band was dropped. The band also mixed the previous screen's rows with the pass's, and the epoch reference kept the previous screen's reference for a row the pass redrew. `GlFramebuffer::era_row_hash` now stores one XXH3-64 per guest row, refreshed immediately before each draw into the row; `readback_framebuffer` compares and skips row by row, and the whole-band pre-test is removed. The second visit (date of birth) renders the complete backdrop (`docs/proofs/web-newgame-cathedral-birth-session207.png`); `cathedral-skip` on the `0x025C00` target reads `69915` at the first sync and `76800` at the second. The probe's `lastScene` now reads `_ogre_web_active_scene` because the `[scene]` log ring evicts after 20000 lines, and `scene.cjs` gained `cathedral-birth`, `cathedral-skip`, `name-entry` and `--log-cap`. `make web` clean; tutorial field unchanged. See `docs/HANDOFF-2026-10-31-session207.md`.

- ✅ **The browser's logo-to-title noise is a present of a blanked VI, and a blank or sizeless VI now presents black (session 206).** The developer's recording shows the QUEST still fade to black, then a full-width noise band at rows 240..320 of a 639x475 canvas for about 0.7 s, then the title. At title entry the VI is blanked (`hStart = 0`, `vi_visible` false) while its origin is already the title's `0x000400` buffer. `update_screen` requested a present on every VI update and `present_framebuffer` sized it from `vi_size()`, the last *visible* mode, so the title's new 320x476 colour image was presented with the publisher's 639x475 area; `framebuffer_containing()` resolved the origin to `0x000400` and the blit sampled rows 240..320, which the title never draws. Those rows had been seeded from RDRAM by `upload_framebuffer`, where they are not image bytes, so the band was noise. RT64 gates the whole present on `viVisible` and, with no colour target, clears the swap chain to black (`tools/RT64/src/hle/rt64_present_queue.cpp`, the `colorTarget == nullptr` arm). The present now carries the visible flag and `fbSize` computed from the same `ViRegs`; `present_framebuffer` resizes from them, and a blank or sizeless VI calls `clear_canvas_black()` without resolving the address. The transition frames read `nonBlack=0` and the title fades in at 319x239; proofs `docs/proofs/web-title-transition-noise-before-session206.png`, `-black-after-session206.png`, `-title-after-session206.png`. `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean; `probe206` removed. The same pipeline is still missing RT64's depth-to-colour present, present-time CPU upload, and clear-on-create of a colour image. See `docs/HANDOFF-2026-10-31-session206.md`.

- ✅ **The browser's mission audio is now generated at the DAC rate; the inline audio task decouples it from the frame pump (session 205).** The AudioWorklet was starved, not at fault: the game generates one 552-frame buffer per RSP completion, and a completion is delivered only while a game thread is blocked or yields, so the mission's ~30 fps frame rate lowered the rate below the 58 buffers/s the 32 kHz DAC consumes. Native has the same pacing: `app/src/sdl_platform.cpp` queues with `SDL_QueueAudio` and reports `SDL_GetQueuedAudioSize()/2` to the same 52 ms `audio_dma_busy` cushion, and session 76 measured one `osAiGetLength` poll per video frame; retail renders the mission at 60 fps (session 199), so its frame-paced audio thread reaches 58 buffers/s. The web mission at ~30 fps measured 40.8-57.4 buffers/s and 1.5-29.8% worklet underrun, against the title's 56.3-58.0/s and 0-2.6%. The runtime now runs `M_AUDTASK` on the submitting thread under a mutex around the shared RSP DMEM (`OGRE_AUDIO_INLINE=0` restores the task-thread path), so `sp_complete()` is queued before the game's `osRecvMesg` and the audio thread refills the cushion in one visit. The mission measures 57.8 buffers/s and 0.1-0.4% underrun, the tutorial practice field 58.0/s and 0%, and the title is unchanged. A 30 s cathedral capture showed the backdrop still assembling; at a 60 s settle it renders the full backdrop; the task-thread capture matches session 203's proof, so that was timing, not a lost completion. A second variant that posted the completion straight to `0x800C49E8` and bypassed the task manager left the title at scene `0x0004` for 180 s and is not used. Kept diagnostics: the worklet's `levels` reply carries `underruns` and `underrunFrames` (starved share `underrunFrames / (frames / 2)`), and `debug/probes/audio.cjs` reports the share and takes `--wait-scene`. See `docs/HANDOFF-2026-10-31-session205.md`.

- ✅ **The browser plays the game's own audio, on by default, and the old "screech" was two arithmetic faults in the worklet, not the audio microcode (session 204).** The game side already worked: the recompiled audio microcode runs in the wasm build and the ring held real PCM at 32 kHz (title ring peak 2073-4237, zero-crossing rate matching the native SDL disk capture). Six page-side faults kept it silent, stale or distorted. `initAudio()` had one caller, the ROM picker's promise continuation, so the stored-ROM auto-start never created an AudioContext; that call is not a user activation, and the early `if (audioContext) return` meant a gesture could not resume a suspended context; SOUNDS defaulted to off on a session-22 comment that predates session 75's recompiled microcode; the worklet's `Int16Array` view used `ringPtr / 2` as a byte offset, so it sampled unrelated wasm memory instead of the ring; it wrote int16 magnitudes into a float output without `1/32768`, so the browser clipped every sample (the actual screech); and it started at ring index 0, so a context that attached late played stale audio. `initAudio()` is now idempotent, runs from `startBoot()` (all boot paths), the picker and the first pointer/key event, and resumes a suspended context; the worklet reads the ring at its byte address, scales by `1/32768`, and adopts the producer's newest frame on `init`/`resync`; `get_frames_remaining` returns int16 samples like SDL (`(head - tail) * 2`), restoring the 52 ms AI cushion. New `debug/probes/audio.cjs` measures the ring and the worklet's own output levels (on the audio thread, so headless timer throttling cannot miss a buffer): output peak 0.099-0.117, muted peak 0 with the ring still draining, unmuted again 0.245-0.294. The page also retries `resume()` on window focus, `pageshow` and visibility, and raises a `SOUND PAUSED · CLICK TO RESUME` button over the screen while autoplay policy keeps the context suspended; a context suspended from the page shows the hint, and an off-canvas click, a `focus` event or the hint's own click resumes it. Two worklet cursor faults then stalled the music after the first drain: `tail` dropped the fractional frame every 128-sample quantum, and a 1e-12 disagreement between the float `readPos` and the integer `tail` was treated as a desync and moved `readPos` to `head`, skipping every queued frame; `head - tail` then looked like a backlog, `audio_dma_busy()` stayed true, and the game's audio thread stopped. `pendingFrames` carries the fraction and the guard clamps to `tail`. The task rate is frame-paced: headless SwiftShader produces 2-3 buffers/s, while a headed ANGLE Metal browser produces 57.4/s and 31 758 ring frames/s, with continuous output at the title (peak 0.33), tutorial (0.52) and mission (0.77). A temporary `probe204` timed the wasm audio ucode at 0.2-0.3 ms per task (spikes 1-3 ms) and was reverted, which rules the ucode out as the reason the frame-paced generation rate was ~1 buffer/s under host load 16. `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` clean; the title route still renders at 29.85 tasks/s with sound on. See `docs/HANDOFF-2026-10-30-session204.md`.

- ✅ **The browser's New Game cathedral backdrop renders: the YUV load's byte order and the writeback's row band (session 203).** Two faults, both in `app/src/web_renderer.cpp`. `tmem_load_yuv` read the njpeg macroblock word with RT64's *native* byte indices through `LoadSource::byte()`, which already applies the guest `^ 3` permutation, so luma and chroma were exchanged: the backdrop drew as green/magenta stripes and its luminance showed through the magenta. It now reads the logical `[U, Y(even), V, Y(odd)]` order, and the TMEM planes equal the values session 51 verified (`luma 28 27 25 24`, `chroma 7A 9A 7B 96`); `OGRE_YUV_TRACE=1` prints both sides. The writeback recorded its row band as `(pos + 1) / 2 * fb.height`, while the draws render through a viewport of `cmd.fb_height_px` rows at the top of the image, so a 320x240 image in the 320x476 target the title grew marked 476 rows as drawn and copied 236 rows over the next framebuffer's start. The band now uses the viewport mapping. The RDRAM reference is taken when a draw era starts (RT64 `hashTracking`) and re-tested once per batch (`checkRAM(updateHashes=true)`), so one stale batch no longer disables an image for the rest of the run; four `[GFX-FBSYNC]` lines now show `0x000400=76800` before each of the four njpeg passes copies. Result: `docs/proofs/web-newgame-cathedral-session203.png`, against the black-backdrop `-black-` and green `-green-` before frames. Title, tutorial field, mission briefing and the tutorial battle still render. New: `OGRE_YUV_TRACE`, a `scene.cjs --route cathedral` route with a `waitLog` step, and `[GFX-S2D]`/`[GFX-FBSYNC]` now print the draw's colour image and the game's readback source/destination. See `docs/HANDOFF-2026-10-30-session203.md`.

- ✅ **The browser's tutorial combat rendered a green/magenta background and froze: a stale framebuffer writeback ran after the game had reused the image's byte range (session 202).** The browser defers every writeback to `ogre_sync_framebuffers()`, and a `GlFramebuffer` keeps its drawn rows until then. The 640x481 screen image at `0x1BA780` spans `0x1BA780..0x1E05C0`; by the tutorial's Field Map → Combat transition the allocator had reused that range for the scene-0x02 njpeg buffers at `0x801CDA00` and `0x801CE3A0` (the RSP task's `data_ptr`). The sync wrote the old image's `0x00010001` pixels over the njpeg input and destination, so the S2DEX2 draw sampled non-YUV bytes (the green/magenta bands) and the next `free()` trapped in `func_80071950` ← `func_800712C4` ← `func_ovlE_80199D30`; tasks froze at 3517 and the page stayed responsive over a black canvas. `readback_framebuffer` now hashes the image's RDRAM and skips the writeback when it differs from the hash at the last upload/writeback (RT64's `FramebufferManager::checkRAM` rule), logging `[GFX-FBWRITE-STALE]`. The final build passes `0x0002 → 0x000D → 0x0003` with no page error and tasks `1435 → 4972`, and the tutorial battle renders (`docs/proofs/web-tutorial-combat-session202.png`); the developer's before frame is `-green-session202.png`. New web debug exports `ogre_guest_rdram`/`ogre_debug_threads`. The njpeg backdrop's green/magenta raw display is a separate pre-existing defect: the unmodified `HEAD` renderer shows the same cathedral frame. Open: the exact `3. Combat` lesson, a per-row hash instead of the whole target, `testdraw`'s scratch above the 8 MiB RDRAM, and that backdrop colour. See `docs/HANDOFF-2026-10-26-session202.md`.

Entries written before session 97 name root-level paths for files that now
live under `config/` and `patches/` (session 97 moved them).

## Status by session (newest first)

- ✅ **Scene captures are one command: `debug/probes/scene.cjs` (session 200).** The probe owns the boot, the route, the `[scene]` wait and the capture that every session used to re-implement. Built-in routes: `title` (scene `0x0004`), `mission-briefing` (scene `0x0003` via `assets/escort_suspend.bin` and the URL taps `title:start:6,0x12:a:6`), `tutorial-field` (natural title -> `0x0017` -> A presses -> scene `0x0003`) and `tutorial` (forced `0x0017`). A route is data (`waitScene`, `sleep`, `press`, `pressUntilScene`); `--scene`/`--query` cover a one-off screen. One run writes `<prefix>-normal.png` / `-texel0.png` / `-texel1.png` (per `--shots`, `--raw` = `0,8,16`), `-samples.json` for `--points`, `-log.txt`, `-meta.json` and `-errors.txt`, and prints the scene timings and the `tasks=` rate. All four routes reached their scene in the session's tests; the tutorial field prints `scene 0x0004 after 27.3 s`, `0x0017 after 0.5 s`, `0x0003 after 10 KeyX presses`. The tool also re-captured the tutorial grass proof and showed N64 `(12,160)` as `(33,33,0)` normal and pure white in `TEXEL0` raw (the untextured fill). Documented in `docs/guides/web-probes.md` -> "Capturing a screen", `debug/README.md` and the AGENTS toolkit list. See `docs/HANDOFF-2026-10-26-session200.md`.
- ⚠️ **The mission's remaining frame cost is outside the browser's flush loop, and the practice field's visible ground is a flat `G_FILLRECT` (session 199).** A draw merge that emitted consecutive same-state draws as one `glDrawArrays` cut the mission's calls from 1 410 777 to 164 198 (8.6 per call) and changed the rate by nothing: **29.39 fps with the merge off, 29.83 fps with it on**, same save and route, session 198's uniform cache in both; the final binary (that cache only) reads **28.66 fps**. A CDP CPU profile of the mission with the merge on spent **87.1% of the main thread idle** (the largest non-idle entries were `(program)` 2.8%, `wasm-to-js` 1.5%, `ogre_gfx_flush` 1.4%, the uniform-cache hash 1.1% and `uniform1i` 0.8%), so the remaining cost is the recompiled game/RSP threads or the GPU and the merge was reverted; the counter is the game's submitted display lists, not RT64's per-VI-field presents. On the tutorial practice stage, a pixel probe at N64 `(12,160)` 12 s after scene `0x0003` logs **five covering draws, all full-screen `G_FILLRECT`s**, the last two `prim=(0.13,0.13,0.00)` and `(0.13,0.16,0.00)`; the frame samples `(34,42,2)`, which is that fill colour. The field's textured 32x64 CI8 ground tiles (`cmb=FC127E24/FFFFF9FC`, `0x2FB540` and eight others) cover the cliffs and roads only; the green ground family (`FC151604`, `0x358F00`, `masks=32`, `shifts=13`) draws at the top-left and off-screen (`x -201..37`, `y -45..10`). `OGRE_DEPTH=0`, culling off and valid shared matrices each change no pixel, so the defect is upstream of the combiner and the sampler. No renderer behaviour changed; the tree keeps session 198's uncommitted uniform cache, and every session-199 probe is reverted. Proofs `docs/proofs/web-tutorial-grass-flat-session199.png` and `-texel0-session199.png`, `docs/proofs/web-mission-fps-session199.png`. See `docs/HANDOFF-2026-10-26-session199.md`.
- ✅ **The browser's per-draw `glGetUniformLocation` was the mission and tutorial-field frame-rate drop, and caching the locations raises the mission from 16.78 to 26.10/28.94 fps; the tutorial field's flat grass is reproduced but not fixed (session 198).** The flush loop resolved about 60 uniforms per draw through `glGetUniformLocation`; a CDP CPU profile of the mission spent 27% of the main thread in it and its UTF-8 conversion (`UTF8ArrayToString`/`findStringEnd`/`webglGetLeftBracePos`) and 12% in the once-per-flush `glGetError` GPU sync. `WebGLRenderer` now keeps `std::unordered_map<std::string_view, GLint> uniform_cache_`, filled by `uloc()` after the program links and cleared on a relink, and the error guard is `OGRE_GL_CHECK=1` (off by default). On the Castle Burgund mission with the same save and route, one 8 s window reads **16.78 -> 26.10 fps**, a re-run **28.94 fps**; the tutorial practice field is about 20 fps. The practice stage is scene `0x0003` after `tutorial (0x17) -> 0x02 -> 0x0D`, and its grass is drawn as a smooth green surface: TEXEL0 raw makes the grass pure white while the cliffs and roads stay textured, no visible triangle or rectangle is dropped for want of a texture, and `OGRE_FOG=0`, culling off and the depth clip off each change no pixel. The large draw logged at the field is a `64x64` CI4 tile at `0x24F4A0` with a TEXEL0-passthrough combiner (`FCFFFFFF/FFFCF279`), but it is the top-left UI panel (its decoded parchment image matches its source); a second layer of `32x32` RGBA16 tiles blends `(TEXEL0 - TEXEL1) * PRIM_A + TEXEL1` (`FC151604/2F1125FF`, `PRIM_A=0.5`) and is textured. The grass layer is not among the logged large draws, so small textured rectangles are the next check. `make web` clean; all `probe198` code and debug bits 23/24/25 reverted. See `docs/HANDOFF-2026-10-26-session198.md`.
- ✅ **The browser has the native Esc overlay's options (session 191).** `app/web/index.html` and `web.js` add MAIN, CONTROLS, SETTINGS and DEBUG tabs (MODS disabled) behind Esc and a topbar Options button, with keyboard and pad navigation. MAIN returns to the launcher with `?menu`; CONTROLS rebinds the 14 N64 rows in localStorage; SETTINGS carries GAME SPEED, WIDESCREEN, WINDOW (browser fullscreen), SOUNDS, VOLUME and MOUSE ENABLED; DEBUG reads the live Chaos Frame and scene id. `settings.cpp` is compiled into the wasm target and `main_web.cpp` calls `load_settings("/ogre")` before `recomp::start`, so GAME SPEED, WIDESCREEN and MOUSE ENABLED are the shared `settings.cfg` values; `web_settings.cpp` exports them plus the Chaos Frame, scene id, widescreen-expansion test and pointer forwarding to the two cursor modules. SOUNDS/VOLUME are the page AudioWorklet gain. WIDESCREEN counter-scales the projection x row by 3/4 on expanding scenes and sets a 16:9 canvas box. The panel is a fixed overlay inside `#stage`, not a modal `<dialog>`, because a modal top-layer element is not part of the fullscreen element. `make web` clean; exports and overlay measured on the final build; page interactions measured on the session-140 wasm because the final build's boots landed on the idle trajectory under load 11-13. See `docs/HANDOFF-2026-10-25-session191.md`.

- ✅ **The browser freeze after 2–3 minutes with no input was a GL-texture leak in the WebGL renderer's tile-image cache (session 188).** Every `G_LOADTILE`/`G_LOADBLOCK`/`G_LOADTLUT` increments `tmem_generation_`, and the image cache key starts from it, so each display list produced new keys and `flush_commands` created a new GL texture for each; only `shutdown()` deleted one. The cache is now swept at the end of every flush, keeping the images the batch's draws name. Before the fix a no-input run reached 5,063 live textures / 20.3 MB at 148 s with one deletion; after it, 4 live / 0.9 MB at 150 s and 7 live / 4.4 MB at 232 s (10,686 created, 10,679 deleted). `ogre_gfx_stats()` reports `tex: cache=N uploads=N released=N`; `shots.cjs` rendered the scene-`0x09` opening; `make web` clean; testdraw 96/128/384 pass. `boot.cjs`'s canvas PNG includes the session-187 launcher overlay, so its `RENDERED` counts are the overlay, not a game frame. See `docs/HANDOFF-2026-10-23-session188.md`.

- ✅ **The browser page is a game shell instead of a debug dump, and the probe contract still holds (session 187).** `app/web/index.html` now has a slim top bar (brand, state pill, Controls, Load save, fullscreen), a metal bezel around the canvas, a launcher overlay, one curated status line, a controls dialog and a `?log` drawer. The title paragraph, the controls paragraph, the log-API paragraph, the visible graphics-workload `<pre>` and the visible `#gfx-status` line are removed. `#rom-input`, `#save-input`, `#game-canvas`, `#status`, `#gfxstats` and `#gfx-status` keep their ids; the two stats elements are hidden but still updated, and `#status` is one curated line by default and the log mirror with `?log`. `#game-canvas` is `box-sizing: content-box`: the page's global border-box would have made `emscripten_set_element_css_size()` include the 1 px border in the declared width, scaling a 640x476 drawing buffer into a 638x474 content box. `web.js` gained `ogreUI` phase/status hooks, launcher dismissal, drag-and-drop, a dialog keyboard guard, fullscreen scaling and a Close log button. `logmode.cjs` `PASS` (`statusLines=1`, `logMode=false` default; `logMode=true` with `?log`), `boot.cjs --attempts 3 --secs 45` `RENDERED` (`w=640 h=481 nonBlack=307840 colorful=6947`), `make web` clean, no page errors, layout checked at 390x844, 768x1024 and 1280x720, fullscreen `--fs-scale 1.5` at 1280x720. Proofs `docs/proofs/web-ui-launcher-session187.png` and `-web-ui-game-session187.png`. See `docs/HANDOFF-2026-10-23-session187.md`.
- ✅ **The QUEST spin's q meshes now show native's moving highlights; the browser's `G_GEOMETRYMODE` keep mask was inverted (session 185).** `OP_GEOMETRYMODE` read `w0`'s low 24 bits as the bits to clear; RT64 keeps them (`geometryMode &= offMask`, `gbi/rt64_gbi_f3dex2.cpp:104-108`, `hle/rt64_rsp.cpp:886-890`). The ROM's own reset idiom `D9000000 00000000` (41 sites; `D9FFFFFF 00000000` never appears) is a reset under the keep rule and a no-op under the inverted one. A native full-RDRAM dump at 14.5 s of a forced `publishers` run reads the q state block (`0x801B85B8`) as `D9FFFFFF 00200405` + `D9FFFFFF 00020000`, then the sub-list at `0x3A4790` adds `G_TEXTURE_GEN`: keep rule `0x260405`, inverted rule `0x040000`. RT64's `usesTextureGen` requires `G_LIGHTING|G_TEXTURE_GEN`, so native sweeps the 32x32 environment map with the surface normal (lookat `(1,0,0)`/`(0,1,0)` at `0x803A2B60`/`0x803A2B70`, `G_TEXTURE` `0x07C0` -> 0..31 texels) while the browser sampled the vertex `s,t` at about 1.3 texels, which is the flat yellow. The decoder now keeps, and `G_TEXTURE_GEN`/`_LINEAR` plus the `MV_LIGHT` lookat slots 0/1 are ported from RT64's `computeTextureGen`. Measured on 34 s 100 ms natural-boot captures: a channel above 200 in native 24 of 24 spin frames (`169-192`), the browser 26 of 27 (`209-235`) after the fix against 6 of 28 before; the geometry fix alone changes no pixel (`0.036` mean absolute difference), which is why session 184's `G_TEXTURE_GEN` implementation was inert. The fixed pose pair (native `190`, browser `234`) has mask IoU `0.791`, whole-frame mean absolute difference `2.31`, mask means native `(80.4,56.5,17.2)` against browser `(80.7,63.5,22.1)`. Neutral outside the stills: scene `0x09` against native `10.032` against `10.109`, and the first scene-`0x04` frame is byte-identical to the pre-change build. `testdraw` 96/128/384 all pass, `grep -rn "probe185" app/ debug/` empty, `make web` clean (wasm `21130960`). Item 20 closed. See `docs/HANDOFF-2026-10-23-session185.md`.

- ⏳ **The QUEST spin's q texture bytes are identical in both renderers; three candidate fixes were built, measured and reverted (session 184).** Same-moment whole-RDRAM dumps (`OGRE_SCENE=publishers`, `OGRE_RAM_KEY_AT_MS` + `_FULL=1` on native, the same query string read back through `Module.FS` in a GPU browser) give byte-identical `0x400`/`0x1400`/`0x2400` at 12.5, 14.5 and 15.5 s (4096/4096 at the spin): the same 4 KB cloud-like environment map, 199 distinct RGB, max `(248,240,224)`. A temporary `probe187` logged the browser's decode of that tile as `32x32 line=8 mean=91,75,35 max=248,240,224`, so the TMEM load and decoder reproduce the guest image; session 183's "brown gradient `161,138,94`" is the same decoder at `t=7.5 s`. The spin's sub-list (`0x3A49..` in the native dump) installs lookat vectors and sets `G_TEXTURE_GEN` but clears `G_LIGHTING`; RT64's `usesTextureGen` requires both, so neither renderer generates coordinates, and an implementation of `G_TEXTURE_GEN` plus the `MV_LIGHT` slots 0/1 changed no pixel and was reverted. The spin's `G_TEXTURE` scale is `0x07C0`, so `s * sc / (65536*32)` puts a vertex with `s=1344` at about 1.3 texels (the hold's `uv0=(42,56)` is the same formula with `sc=0x10000`). The browser also lacks RT64's per-workload framebuffer writeback (`hle/rt64_state.cpp:1655-1670`); porting it made the q frames darker and flatter and it was reverted. Presentation: native has a near-white texel in 24 of 24 sampled spin frames, the browser in 3 of 14, with silhouette IoU up to 0.985 at the best-aligned poses. The decode and the sampler were then ruled out: RT64's `OGRE_TILE_DECODE` (extended with the upload's address) reports only `32x32 line=8 addr=0x000400 nz=0/3072` decodes during the still and no 32-bit `line=1` decode, i.e. the q tiles take the deferred path and the diagnostic's address/content are the next image and a zero TMEM snapshot, not evidence (the line now says so); and one build's three sampler modes (`OGRE_TEXF=rt64`, `=1`, `=gl`) give the same pose-dependent highlight (bright only face-on, `<=160` edge-on). Next: the interpolated `v_tex` of one q triangle in both at the same pose, and the number of q draws that survive depth per frame. `grep -rn "probe187\|kGeomTextureGen" app/ debug/` empty, `EM_CACHE=<repo>/build-wasm/emcache make web` clean, wasm back to `21129871` bytes. Open item 20. See `docs/HANDOFF-2026-10-23-session184.md`.

- ⏳ **The QUEST spin is still not 1:1: the browser draws the textures and the bright highlight, but at the same animation moment the highlight is on a different q (session 183 follow-up).** The settled hold matches native (`0.317` mean absolute difference). A combiner census of the q triangles into `0x250800` reads both families in the browser (`FCFFFFFF/FFFCF87C` evaluated as `TEXEL0`, PRIM 0; `FC121824/FF33FFFF` as `TEXEL0*SHADE`, PRIM 0.98, SHADE 0.5), and bright highlight pixels do appear (browser `frame-228` 6277 above 200, peak `(248,231,190)`; native `frame-188` 6546, peak `(248,240,220)`). The animation clocks differ (q's first appear at capture 20.12 s native, 20.51 s web; the plate at 23.22 s and 23.81 s), and at the offset-adjusted same moment browser `frame-229` has the bright q in the middle where native has it on the right with the middle q edge-on (mask IoU 0.985 for 229/189). Every 32x32 q texture upload decodes from the same RDRAM address (`0x000400`) with identical statistics (`958/1024` nonzero, mean 217) while each gets a new key, which is the lead to check against native's per-sub-list `G_SETTIMG`. Probes removed and the tree rebuilt (`grep -rn "probe184" app/ debug/` empty). Open item 20. See `docs/HANDOFF-2026-10-19-session183.md` §5.

- ✅ **The QUEST still's q meshes render, and the cause was a stale depth image rather than the batch boundary (session 183).** Scene `0x0A` uses guest address `0x2E6840` as both the plate colour image and the depth image; the q draws are depth-tested and depth-written (`OTHERMODE_L 0x00552078` sets bits 4 and 5), and the renderer keeps one depth renderbuffer per depth image, so the plate's colour fills left the depth contents stale and every q fragment failed `GL_LESS`. That is the white-plate/`coloured=0` frame of sessions 180/181. The renderer now mirrors RT64 `depthFbTypeChanged` (`hle/rt64_state.cpp:1581`, `:1630`): a colour draw to an address that a display list named with `G_SETZIMG` queues that depth target for a clear, and the clear runs with the scissor disabled. One build with one environment variable (`OGRE_PROBE183_NOINVAL`, removed afterwards), forced `?OGRE_SCENE=publishers`, holds frames 24-25 at `white=18797, colourful=0` with the invalidation off and `white=17489, colourful=1309` with it on; the clean rebuild presents `17489/1309` (`docs/proofs/web-publisher-quest-q-meshes.png` against `docs/proofs/web-publisher-quest-no-depth-invalidation.png`). A new `--interval-ms` on `tools/intro_capture.py` captured both renderers at 10 Hz: native holds `white=17519, colourful=1281` for 1.9 s, the browser `17489/1309` for 1.6 s, every sample inside the hold carrying the plate and the q colour, and the browser's final frame is `0.317` mean absolute difference from native's. Session 181's batch-boundary explanation is withdrawn. `grep -rn "probe183" app/ debug/` empty. See `docs/HANDOFF-2026-10-19-session183.md`.

- ✅ **The opening is now captured as a 1 fps series in both renderers, and the browser's QUEST hold presents the reference frame at that sampling (session 182).** `tools/intro_capture.py` writes 30 PNGs, one per second from the game's first scene, a contact sheet and a per-frame pixel report, natively and in the browser; the browser half is `debug/probes/intro-capture.cjs`. Native cadence needed a wall-clock interval in RT64 (`OGRE_CAPTURE_EVERY_MS`), because a present count ran at 1 Hz in the 320x240 scenes and 2 Hz after the title's 320x476 switch. Measured at `OGRE_SPEED=1`: native holds the QUEST logo at frames 23-24 (`white=17519, colourful=1281`) and the GPU browser at 24-25 (`white=17489, colourful=1309`); two browser runs produced the reference frame, and the same frames appear with the probe's per-capture flush disabled. The software rasterizer's boot takes about 30 s of game time, so a 30 s window there is all black. See `docs/HANDOFF-2026-10-19-session182.md`.

- ⏳ **The QUEST still's q meshes are in the queue, in list order, and are rasterized; the frames that present them have a black plate (session 180).** Session 179 left the three q meshes missing and named the web walk order for the segmented sub-lists as the next measurement. That measurement refutes the ordering claim: a ring of the last 48 recorded draws reads `16 plate fills (fb 0x2E6840), 16 target fills (fb 0x250800), 16 white bands (offsets 0x186640.., PRIM 0.98), 3 wordmark texrects (0x3ACD60, 0x3ACDC0, 0x3ACE60), then the first q triangle at 0x026C08`, which is the native decode's order, and a walk trace of a still list shows `G_DL 0x801B84B8` + `SETPRIMCOLOR 0xFAFAFAFF` + `SETCOMBINE FFFDF6FB` before the bands and `G_DL 0x801B85B8` + `SETCOMBINE FF33FFFF` before the eight `G_DL 0x0E000000` sub-lists. The draws carry the native state (`G_LIGHTING`, one white light at dir `(0,0,127)`, ambient 128, oml `0x00552078`, no blending, scissor `(0,0,640,480)`, window depth 0.2535, so no F3D depth clip), and the colour image does receive their pixels. The difference is the frame: the batches that present q colour read `white=0 colourful=1988..3088` on the canvas because the 16 plate-copy bands in those lists carry PRIM 0.00 (their combiner `FCFFFFFF FFFDF6FB` evaluates to PRIM only), while the presents that carry a white plate (`white=225183`) carry no triangles. Two session-179 claims are corrected: its "fade bands before the q triangles" was its probe's run-length summary, not the queue, and the q textures are hue ramps (29-32 distinct texels, alpha 0), not one solid colour each. Also corrected: two triangle combiners appear, `FC121824 FF33FFFF` (`TEXEL0*SHADE`) and `FCFFFFFF FFFCF87C` (`TEXEL0` passthrough). No code landed; the next experiment is the ordered PRIM/CIMG/combine walk trace of one black-plate frame with triangles, compared with `OGRE_DL_DECODE` of the same list. No probe remains (`grep -rn "probe180" app/ debug/` empty), no commit, no RT64 or recompiled-file change. See `docs/HANDOFF-2026-10-19-session180.md`.

- ✅ **The browser's QUEST publisher still lost its white plate to a texture-availability test (session 179).** Scene `0x0A`'s final display list clears a 640x480 plate white, clears the frame target black, then draws 16 full-screen `G_TEXRECT` bands whose combiner evaluates to PRIM only (`FCFFFFFF FFFDF6FB`, PRIM `0xFAFAFAFF`) through the preamble's unconfigured tile (`line=0`, zero rect). `draw_texrect` decoded the tile and dropped every rectangle with no image (`if (!texture_available(st)) return;`), so all 16 bands were skipped, the frame kept the black clear, and only the CI8 wordmark (its own loaded tile) appeared. RT64 `RDP::drawTexRect` drops only a non-zero tile with a zero window, and its sampler is never read when the mux does not name TEXEL0/TEXEL1. `combiner_uses_texture` now mirrors RT64 `ColorCombiner::usesTexture(otherMode, index, false)` (copy reads TEXEL0; one-cycle evaluates the second mux and treats TEXEL1 as TEXEL0; two-cycle checks both, swapping the units) and the rectangle needs an image only when it returns true. The white plate and the wordmark now draw (`docs/proofs/web-publisher-quest-white-background.png`, meanRGB 245.6); the natural-boot intro still renders; `testdraw --flags 96 --expect-fbtex --expect-hits 1` reads `fbtex hits=1 lines=1`; `make web` is clean with `EM_CACHE=build-wasm/emcache` (the sandbox denies the default cache's lock file). **Still missing: the three q meshes.** They are queued (756 of the final list's 807 draws) with a valid 32x32 RGBA32 solid-colour texture (alpha 0), `GEOMETRYMODE 0x00020000` (G_LIGHTING), `FC121824 FF33FFFF` (TEXEL0*SHADE) and NDC inside the frame, but the flush records the closing fade bands before the q triangles and gives the triangles the fade's PRIM (black), so the web order for the segmented q sub-lists (`G_MOVEWORD segment[14] = 0x00026C00`, `G_DL 0x0E000000`) is the next measurement. No probe remains (`grep -rn "probe179" app/ debug/` empty), no commit, no RT64 or recompiled-file change. See `docs/HANDOFF-2026-10-19-session179.md`.

- ✅ **The boot intro's soldier trails were the fill quad's second triangle (session 178).** Scene `0x09` draws 12 soldiers once per frame into a triple-buffered 320x240 colour image, and each frame begins with one full-screen black `G_FILLRECT`. `draw_fill_rect` names its corners top-left, bottom-left, bottom-right, top-right and emitted the 6-vertex list as `(0,1,2)` plus `(3,0,1)`; the second pair repeats the first triangle's left edge, so both triangles covered the left half of the rectangle and the right half was never rasterized. RT64 renders a `G_CYC_FILL` rect as a clear of the whole rect (`rt64_framebuffer_renderer.cpp:1620` -> `InstanceDrawCall::Type::FillRect` -> `clearColor`), so the pairing is now `(0,1,2)` and `(0,2,3)`, the same `{0,1,3,0,3,2}`-style fan the S2DEX object rectangle and the texrect already use. The unwritten half kept every earlier frame's right half, so the ring of soldiers running outward left every previous position on screen, clipped by the quad's diagonal: the developer's "triangle on the right where the sprites are not cleaned up". Measured in the renderer's own colour image at present time: the fill wrote 1/3 of the pixels its full-screen rect names, the boundary on the quad's diagonal, and the same picture with `OGRE_DEPTH=0` (so not a depth test). After the fix the browser intro at task 146 shows 3 soldiers per corner and the cube, matching the native RT64 frame `p.155` captured with `OGRE_CAPTURE_PRESENT`; `testdraw --flags 96 --expect-fbtex --expect-hits 1` reads `fbtex hits=1 lines=1` with `drawn rows=[0,239]`, `--flags 128` reads the near triangle, `--flags 384` the far one, `--flags 512` reads `threshold/full=full threshold/none=none dither/partial=partial`, and `make web` is clean. Not re-measured: the screens whose fills are not full-screen (the attract story's letterbox, the publisher stills), which the corrected quad also changes. No probe remains (`grep -rn "probe178" app/ debug/` empty), no commit. The page at `http://127.0.0.1:8931/app/web/index.html` served the same tree and the current build throughout: `debug/server.py` serves the repository root with `Cache-Control: no-store`, the served `build-wasm/ogrebattle64.wasm` hashed identically to the local build, and the build was newer than every source file. See `docs/HANDOFF-2026-10-19-session178.md`.

- ✅ **The mission's stipple was the `G_AC` enum mapping, and the alpha compare now matches RT64 (session 177).** RT64's values are `G_AC_NONE 0`, `G_AC_THRESHOLD 1`, `G_AC_DITHER 3` (`tools/RT64/src/shared/rt64_f3d_defines.h:8-10`), but the browser's fragment shader tested `u_ac == 2` as the threshold arm and `u_ac == 1` as the dither arm. A real `G_AC_THRESHOLD` draw therefore ran the dither arm against a per-pixel hash: with a combiner alpha around 0.2 about a fifth of the texels survived at random, which is the measured scatter on the portrait frame (13.58, 31.3% exact) and the `MISSION` band (8.94, 18.6%, 207 bright glyph pixels against native's 1083, no lattice period and no row/column parity bias). The mission census has 33 770 `acmp=1` (threshold) draws and no `acmp=2`. The shader now runs the threshold compare for `u_ac == 1` (`alpha_compare_value < u_blend_color.a`) and RT64's dither for `u_ac == 3`: `initRand(FrParams.frameCount, instanceIndex * pixel.y * pixel.x, 16)`, the combiner's NOISE input consuming one `nextRand`, and the compare reading the next (`RasterPS.hlsl:189/212-215`, `Random.hlsli:10-33`); the seeds are `present_requests_`/`flushed_cmds_`. **The synthetic control proves the mapping without the game**: `testdraw.cjs --flags 512 --expect-alpha` draws three identical rects with combiner alpha 0.2 - threshold with blend alpha 0 reads `lit=1406/1406 peak=255`, threshold with blend alpha 1 reads `lit=0/1406 peak=0`, dither reads `lit=277/1369 peak=255`, and the probe reports `threshold/full=full threshold/none=none dither/partial=partial`, exit 0. The control's readback was also corrected for the RGBA16F attachment (WebGL2 rejects `GL_UNSIGNED_BYTE` for it, which had read as an all-zero block; it now reads floats as `note_depth_probe` does). `--flags 96` is `fbtex hits=1 lines=1`, `--flags 128` reads the near triangle, `--flags 384` the far one, and `make web` is clean. **The frame-locked mission re-capture is pending, not passed**: the browser boot stalled at scene `0x0000` with `tasks=0` on every attempt from about 01:40 (four mission-route attempts, four forced-`0x03` attempts and three `boot.cjs` attempts; the same build renders the synthetic control and `boot.cjs` passed at 23:16), so whether the mapping fix restores the portrait frame, the `MISSION` band and the party's character sprites is the next measurement (`/tmp/w176/nat/t.289.bin`, party cluster x18..78 y60..98, portrait rows 85..195 x 105..215, MISSION band rows 190..225). The tutorial/title/map captures are not re-run for the same reason; their draws carry no `acmp=1`/`3`, so they are expected byte-identical, which is an expectation and not a measurement. No probe remains (`grep -rn "probe177|OGRE_A177" app/ debug/ RecompiledFuncs/ Bank*Funcs/` empty), no commit. See `docs/HANDOFF-2026-10-19-session177.md`.

- ❌ **The browser mission pair is frame-locked, and the renderer does not match it: a 4.74 whole-frame mean hides a missing party, and the later phase stipples the portrait frame and MISSION band (session 176).** A native mission series was captured from the same save and route as the browser's (`OGRE_SAVE=assets/escort_suspend.bin`, `OGRE_SCENE=title`, `title:start:6,0x12:a:6`, `OGRE_SPEED=1`, `OGRE_CAPTURE_EVERY=1`): 637 `OGRE_CAPTURE_TARGET` frames, mission entered at `[scene] t=17821ms id=0x0003`, so its window is `t.264`..`t.640`. The browser was captured with the session-173/175 route (`0x0003` on attempt 1). A 300 ms series could not be captured: a Playwright screenshot of the running canvas cost 12.9-13.5 s under load average 15-50 (0.07 s on a blank page). `canvas.toDataURL` reads the preserved drawing buffer (`app/src/web_renderer.cpp:7646`) at 3-8 ms per grab on a quiet machine but returned black frames 0..13 of 17, so the browser side is six mission frames at ms 0, 1, 9909, 12885, 13841 and 18928. Every browser frame against every native frame (2262 pairs, `web_frame_cmp.compare()`'s arithmetic in `/tmp/w176/crosscmp.c`): the global minimum is `m4-a1-014` against `t.289` at `meanAbs 4.74/3.59/1.90`, `meanSigned -1.12/-0.81/-1.40`, `exact=45.08%`; ranks 2-5 are `3.421/3.424/3.429/3.434`, flat inside a 0.5 s window because the map is static there; the same browser frame measures 6.694 against `t.588` and 34.8 against a pre-`0x03` frame. Terrain regions of the best pair: top cliff `0.93`, left grass `1.66`, right terrain `2.95`, bottom middle `8.56`. **The residual is not the terrain: the party's character sprites are not drawn.** The party cluster (x18..78 y60..98) measures `26.54`, 735 of 2280 pixels differ by more than 30, the browser's brightest channel value there is 97 against native's 255, and `docs/proofs/web-mission-framelocked.png` shows native's five characters where the browser has the terrain and the cursor brackets only. The three element-screenshot frames measure 6.7-7.3 against their best natives; at those phases the note box is `4.65` (79.8% exact) and two elements lose their texels one at a time, the portrait box `13.58` (31.3%) and the MISSION band `8.94` (18.6%, 207 bright glyph pixels against native's 1083, scattered with no lattice period and no row/column parity bias). That stipple and the absent sprites are one family, an alpha/coverage rejection on alpha-tested sprites (`G_AC_THRESHOLD` `web_renderer.cpp:1596-1599`, `G_AC_DITHER` `:1600-1606`, `CVG_X_ALPHA` coverage `:1614-1617`); the next step is one run logging a character sprite's draw (kind, texture address and format, TLUT, `OTHERMODE_L` compare mode and threshold, pre-compare alpha). Publisher `0x0A` remains **not verified**: six natural boots of 20 s (62 shots, every one `tasks=0 nonBlack=0`, `maxTasks=0`) printed no `[scene]` line at all, so the still was not compared with `docs/proofs/native-licensed-screen.png`/`-atlus-`/`-quest-`. No application or renderer code changed, no probe added, no commit. See `docs/HANDOFF-2026-10-18-session176.md`.

- ✅ **The browser renderer takes `G_TP` from OTHERMODE_H bit 19, applies the tile `ShiftScaleMap`, feeds the chroma-key combiner inputs, and applies per-vertex fog alpha (session 174).** Four decoded-but-unused fields, one at a time, each with a census that says whether it fires. `record_state` read bit 10 for `G_TP`, which is not an RDP field; RT64's `G_MDSFT_TEXTPERSP` is bit 19 (`tools/RT64/src/shared/rt64_f3d_defines.h:51`, consumed at `src/shaders/TextureSampler.hlsli:221-226`), and the rect arm keeps `kind != 0` because RT64's `renderFlagRect` skips the correction. A permanent `[GFX-TP]` census prints one line per (kind, OTHERMODE_H) family with both readings: `changes=0` on all 1 tutorial, 1 title, 2 map, 14 mission and 3 attract-story families, because every observed triangle family has bit 19 **and** bit 10 set while the rect families have bit 19 clear and are drawn as rects (the audit's reading is confirmed, and no draw changes). The tile shift fields are now multiplied into the sampled coordinate exactly as RT64 does (`hle/rt64_state.cpp:33-51` `ShiftScaleMap`, `:306-307`, `TextureSampler.hlsli:226`) before the 1/128 quantization; `shifted=` is 0 on the tutorial, title and map, so those frames cannot move, and the mission's 18 794-24 563 shifted draws of 27 692-33 450 change **no pixel**: a runtime A/B ("apply" against "force the scale to 1") captured five interleaved pairs, four bit-identical (`meanAbs 0.0000`, `exact 1.0000`), the fifth `1.037`/`0.9382` coinciding with the screen's own animation (two settled-phase frames 3 s apart are bit-identical, the next capture differs by `1.614`). `OP_SETKEYR`/`OP_SETKEYGB` left the ignore list and decode RT64's fields (`hle/rt64_rdp.cpp:1223-1236`, `gbi/rt64_gbi_rdp.cpp:154-169`), the shader's inputs 6/7 return `u_key_center`/`u_key_scale` (`RasterPS.hlsl:185-186`, `shared/rt64_color_combiner.h:482-485`), and a one-shot `[GFX-KEY]` names the first draw whose evaluated mux reads one, with `G_CK_KEY` (OTHERMODE_H bit 8); no reached screen logged it. `G_FOG` now replaces the vertex alpha in `transform_vertex` with `clamp((max(tfPos.z,0)/tfPos.w * fog_mul + fog_offset)/255, 0, 1)` (`RSPProcessCS.hlsl:70-80`, `hle/rt64_rsp.cpp:625-640/990-994`), using the clip z/w and not the RDP screen z; a one-shot `[GFX-FOG] per-vertex alpha` note reports the first vertex that takes the arm and no reached screen did. Measured on the final build (depth on): tutorial `0x17` frame rows 166..170 `0.29` against native `t.335` and the box unchanged band by band (0.25 parchment / 0.00 portrait / 0.18 lower, only the cursor glyph band differs: 3.46 against session 167's 0.00); map `0x05` `nonBlack=252820` with fade gain `1.927` and residual `0.84`; mission `0x03` `meanAbs 7.07-7.58`, `exact 0.3489-0.3633` against `t.800` at its own phase. `make web` clean four times, `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0, `--flags 128 --expect-depth near` exit 0 `rgba=(255,0,0)`, `--flags 384 --expect-depth far` exit 0 `rgba=(0,0,255)`. Not verified: publisher stills `0x0A` (the forced entry never advanced) and the title/attract `0.11-0.20` match (the forced title lands on the menu screen, which `/tmp/w162/nat-fog` does not contain; session 171's 0.11 frame is the attract story overlay at a different beat from the one this round reached). No probe kept (`grep -rn "probe174\|shift_off" app/ debug/` empty). See `docs/HANDOFF-2026-10-18-session174.md`.
- ✅ **The browser's depth buffer is keyed by `G_SETZIMG`'s depth image, not by the colour image, and the mission's "depth flattens the top band" was a capture-phase difference (session 173).** RT64 keys its depth `RenderTarget` by the depth image's address/width/siz independently of the colour target (`tools/RT64/src/hle/rt64_state.cpp:1354-1365`, `:1389-1401`, `hle/rt64_framebuffer_manager.cpp:171`), while the browser gave every `GlFramebuffer` (one per `G_SETCIMG` address) its own `GL_DEPTH_COMPONENT24` renderbuffer. The mission is the case the hypothesis named: one depth image (`0x3350C0`) is drawn into from four colour images (`0x3350C0` at draw 5266, `0x025C00` at 5267, `0x04B400` at 6751, `0x000400` at 8235; the new `[GFX-DEPTH] pair` census reads `pairs=11` on scene `0x03`), so every colour target after the first drew against a cleared buffer instead of the shared contents. The renderbuffers now live in `depth_targets_`, keyed by the depth image (`depth_target_key`), the flush attaches the matching one with `glFramebufferRenderbuffer` and skips the call when it is already there, the buffer is sized from the colour target and grown rather than recreated, and it is cleared to 1.0 once on first use. **Measured, and it changes the record:** flag bit 9 toggled `depth_enabled_` at runtime inside one run, and four of five depth-on/depth-off pairs captured 350-400 ms apart (same intro phase) are bit-identical - whole frame and rows 0..38, 200..238 and 40..180 all `0.000` mean abs, with pair 0's `1.482` from a toggle that landed inside a flush - while `[GFX-DEPTH]`'s `zcmp` count shows both states really occurred (14 243 at draws 29 200..31 200, frozen at 31 600..32 800). The frame's top band measures `9.73` mean abs against native `/tmp/w168/nat2/t.800` with the test **on** and, because the pixels are identical, the same `9.73` with it **off**: session 172's `0.93 -> 9.52` came from a different intro phase (rows 0..38 contain the note box's slide-in, the portrait and the scrolling MISSION text), so it is withdrawn as a depth finding and is now open item 8(u) for a frame-locked native pair. A one-shot `[GFX-DEPTH] first depth-tested draw` line rules out the false explanation of the identical frames (a colour-only framebuffer makes `GL_DEPTH_TEST` a no-op): it reads `depthRenderbuffer=11 status=0x8CD5` (`FRAMEBUFFER_COMPLETE`). The test now ships **on** (`OGRE_DEPTH=0` disables it), `testdraw --flags 128 --expect-depth near` -> `rgba=(255,0,0,0)` exit 0 and `--flags 384 --expect-depth far` -> `rgba=(0,0,255,0)` exit 0 on the cleaned build, and `docs/proofs/web-mission-depth-on.png` is the depth-on mission frame. Not re-measured: tutorial `0x17`, title `0x04`, map `0x05`. `make web` clean five times; `grep -rn "probe173\|p173\|OGRE_BAND_TRACE" app/ debug/` empty. See `docs/HANDOFF-2026-10-18-session173.md`.

- ⚠️ **The browser's RDP depth pipeline is implemented and proven by a synthetic control, and the depth test regresses the mission frame so it ships off (session 172).** `G_SETZIMG` (2 360 commands of scene `0x0003`) and `G_SETPRIMDEPTH` (11 740) decode into `RenderState` (`zimg_address = w1 & 0xFFFFFF`, fmt bits 21-23, siz bits 19-20, width bits 0-11 + 1; `prim_depth = ((w1>>16) & 0x7FFF)/32767`, `dz = (w1 & 0xFFFF)/65535`, RT64 `hle/rt64_rdp.cpp:403/1160`). `DrawCmd` carries `depth_test = zCmp && !copy && zMode != ZMODE_DEC`, `depth_write = zUpd && !copy` (RT64 `render/rt64_framebuffer_renderer.cpp:1729-1730`), `z_source_prim`, `prim_depth`, `z_mode` and per-vertex NDC z. Each `GlFramebuffer` owns a `GL_DEPTH_COMPONENT24` renderbuffer created/resized with the colour texture and cleared to 1.0 (RT64's target is `D32_FLOAT`), and the last `G_SETZIMG` configuration is stored on it. `transform_vertex` writes 10 floats: `screen_z = pos.z/w * vscale[2]/1024 + vtrans[2]/1024` (RT64 `hle/rt64_rsp.cpp:23/714/908-911`, `DepthRange = 1024`; the mission's `Vp_t` at `0x00186330` reads `[640 480 511 0]`, so the 511 is at index 2 and the earlier `/4` gave 127.75), and the shader gets `2*screen_z - 1` so `gl_FragCoord.z` is the RDP's 0..1 value; `G_ZS_PRIM` draws use `2*prim_depth - 1`, rectangles use NDC z 0 (RT64 `rectPosFloats`). The flush sets `GL_DEPTH_TEST`, `glDepthFunc(LESS)` when testing and `ALWAYS` when only writing (`rt64_raster_shader.cpp:317`), `glDepthMask(depth_write)`, and the fragment shader runs RT64's F3D depth clip (`1022.0/1024.0`, `RasterPS.hlsl:63-104`) through `u_zclip`. **The synthetic control is the correctness proof**: `test_draw` bit 7 draws the near triangle (red) first and the far one (blue) second with `Z_CMP|Z_UPD` and the renderer reads the target's pixel back - `testdraw.cjs --flags 128 --expect-depth near` reads `rgba=(255,0,0,0)`, `--flags 384 --expect-depth far` (Z_CMP cleared) reads `rgba=(0,0,255,0)`, both exit 0. On the mission `[GFX-DEPTH]` reads `zcmp=zupd=20307` of 44 800 draws (45%, the `0x00552078` group), `prim=0`, `dec=0`, `zimg_seen=2` (`0x228080`, `0x3350C0`, both `fmt=0 siz=0 width=1`; RT64's `setDepthImage` takes only the address). **With the test on the frame regresses**: `meanAbs 7.25/6.63/6.98`, `exact 0.3614`, top cliff 0.93 -> 9.52, and that picture is identical with z inverted or with the z scale zeroed, so the ordering, not the sign or the clip, is what fails; it is behind `OGRE_DEPTH` (off by default) and `ZMODE_DEC` is reported, not approximated. Depth off measures `4.77/4.54/3.19`, `exact 0.4387`, cliff 0.93 (session 171: `5.28/5.05/3.73`, `0.4372`), i.e. no regression from the plumbing. `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 (`fbtex hits=1`), `boot.cjs --attempts 3 --secs 40` `RENDERED`, the map renders (`nonBlack=252820`). The tutorial/title captures this round landed on early trajectories under load and are not usable as regression evidence. See `docs/HANDOFF-2026-10-18-session172.md`.

- ✅ **The natural title drew noise and then only its upper half: a 640x481 depth renderbuffer was attached to its 320x476 colour target, and the GL viewport was sized from the colour image rather than the draw's screen (session 186).** The title after scene `0x0A` renders into three 320x476 colour images and names the depth image `0x2E6840` that the publisher still created at 640x481. The browser keyed depth targets by the `G_SETZIMG` address alone and grew one renderbuffer per address, so `glCheckFramebufferStatus` returned `GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS` (`0x8CD9`) for all three and every title draw was dropped; the colour texture kept its undefined bytes and the presenter blitted them (`sha256 06bff23c…`). Depth targets are now keyed by `(address, width, height)`, which is RT64's `RenderTargetKey` shape, and the alias invalidation walks every size-keyed buffer. With the draws accepted, the title showed only the top half at 2x: the rects record `screen=320x240` but the colour image is 320x476, and `glViewport` used the image's full height. The viewport is now per draw, `glViewport(0, fb.height - cmd.fb_height_px, cmd.fb_width_px, cmd.fb_height_px)`, mapping screen row 0 to guest row 0 and agreeing with the scissor. The browser title's text bands `34..74`, `78`, `87..130` now equal the native capture's (`docs/proofs/web-title-vs-native-session186.png`); `-web-title-before-after-session186.png` holds the noise, stretch and fixed stages. The publisher `0x0A`, the q spin and the `0x09` intro are unchanged (the QUEST still frame is byte-identical, `sha256 e5b14dea…`). `EM_CACHE=<repo>/build-wasm/emcache make web` clean (wasm `21132250`); `testdraw` 96/128/384 pass. The browser trace gained `OGRE_TEX_TRACE_AFTER_MS`/`_MS`. See `docs/HANDOFF-2026-10-23-session186.md`.

- ✅ **The browser renderer implements `G_LIGHTING` and backface culling, and the mission `0x03` frame matches the native (session 171).** `app/src/web_renderer.cpp` now keeps the F3DEX2 lights the way RT64 does (`std::array<Light, RSP_MAX_LIGHTS + 1>`, `tools/RT64/src/hle/rt64_rsp.h:163`): `G_MOVEMEM` index 10 (`MV_LIGHT`, `offset = p0(w0,8,8)*8`, `slot = offset/24`, slots >= 2 = light `slot-2`, RT64 `gbi/rt64_gbi_f3dex2.cpp:48-61`), `G_MOVEWORD` `G_MW_NUMLIGHT` (`w1/24`), `G_MW_LIGHTCOL` (`w1` into the light's two colour words) and `G_MW_FOG`, with the 24 raw bytes read through `n64b` (`p[off^3]`) because the runtime's RDRAM is byte-reversed. `transform_vertex` replaces the vertex colour with RT64's result when `G_LIGHTING` is set and `num_lights > 0` (`shaders/RSPProcessCS.hlsl:88-108`, `shared/rt64_rsp_light.h:56-64`): the Vtx colour bytes are the normal over 127, `ambient = light[num_lights].col`, each light adds `max(dot(norm, normalize(mul(float4(dir,0), worldMat))), 0) * col` with `worldMat = st.modelview_stack.back()` (RT64 `hle/rt64_rsp.cpp:538`; `extended.viewProjMatrix` is the identity), then `rgb = min(result, 1)`; `num_lights == 0` keeps the raw colour. The light byte layout was **measured**, not assumed: a mission `[GFX-LIGHT]` line reads `raw 4D 4D 7F 00 4D 4D 7F 00 FD 99 C5 00` with ambient `4C 4C 7E 00`, so the pads are at 3/7/11 and the logical reads are colour 0/1/2 and direction 8/9/10 (the pad-first reading pins red at 0 and measured `meanSigned R=-19.60`). Point lights are unreachable (`gbi/rt64_gbi.h:39` `pointLighting = false`, never assigned) and are not implemented. Culling mirrors `RSP::drawIndexedTri`: both bits draw nothing (`:1059`), `G_CULL_FRONT` alone swaps the first and last index (`:1094`), `N = cross(V, U)` from screen-space positions (the browser's NDC `(x, -y)`) must satisfy `N.z >= 0` (`:1132`); F3DEX2's bits are `0x200`/`0x400`. A permanent `[GFX-LIGHT]` census prints every 400 lit draws (lit/unlit/culled, `num_lights`, `point`, light 0's raw bytes and the first draw's normal and shade). **Measured** against native `/tmp/w168/nat2/t.800`: `meanAbs 23.72/15.30/8.11` (`exact 0.3090`) -> `5.28/5.05/3.73` (`exact 0.4372`), `meanSigned +0.91/+1.16/+0.84`; terrain regions top cliff rows 0..38 `18.92 -> 0.93`, right terrain `17.94 -> 2.26`, bottom middle `17.18 -> 5.24`, left grass `18.10 -> 7.87`, whole frame `15.71 -> 4.69` (the two frames are different intro phases, so the static terrain regions are the measurement). No regression: the tutorial `0x17` frame is byte-identical to session 167's post-fix frame (`md5 10b3536365b1bcfb6d2e2116aaf42a24`, box 0.22 over rows 166..219), the title's best native match is 0.11 and it has no triangle draws, and the world map differs from session 167's frame only by its fade (gain 1.932, residual 0.99). `make web` clean twice, `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 (`fbtex hits=1 lines=1`); `boot.cjs --attempts 3` reported `NO RENDER` while the machine was at load average 14-24 and `RENDERED` on attempt 1 of a longer run of the same build (`nonBlack=25288`). Depth (516 463 draws) and per-vertex fog alpha remain. See `docs/HANDOFF-2026-10-18-session171.md`.

- ✅ **The browser's framebuffer writeback wrote the Load Game heap colour image over the part cache's allocator header, and the mission now renders (session 170).** Session 169's naming is corrected first: `D_8018FC88` is the sprite **part cache** (session 132's 256 x `0x80`-byte record layout), not a message queue. `func_8017909C` allocates it, `func_801792DC` is its release (`D_8018FC88 = free(D_8018FC88)`, four instructions), `func_800712C4` is the game allocator's free/coalesce path and `func_80071950` is a binary-tree node unlink that stores through `node+0x08`/`+0x0C`/`+0x10`. The words that trap are therefore the **allocator block header** at `G-0x20` (`G = 0x80243E10`): `+0x18` is the size the free tests against `0x21`, and the links are `+0x08`/`+0x0C`/`+0x10`. The flush that fails is the **next** scene enter's release, not the `0x03` enter's first call: both builds flush `0x801BA770` at the `0x03` enter (`func_80173724` calls `func_801792DC` then `func_80179080`), and that block's header is valid in both. **The writer is the browser renderer's framebuffer writeback**, `readback_framebuffer`/`readback_all_framebuffers` in `app/src/web_renderer.cpp`: scene `0x12` (Load Game) renders a 320x240 colour image at guest `0x80228080` (`SETCIMG addr=0x228080 rowBytes=640`, `[GFX-FB] framebuffer 0x228080: 320x240 pitch=640`), which spans `0x228080..0x24D880` and therefore contains the cache block's header at `0x243DF0..0x243E10`. A tagged `probe170` writeback probe logged `writeback fb=0x228080 covers 0x243DF0: bytes 0x228080..0x24D880 rows(wy)=[0,239] size=320x240 pitch=640` fourteen times from the scene-`0x12` display lists, and a guest dump in `func_801792DC` at the next `0x80243E10` release read `hdr=80243DD0 8024DA20 800AEDB8 00000000 00000000 00009C10 00000000 FFFDFFFD` with `fd[0x8023BE10..0x8024BE0E]=22891` (22 891 `0xFFFD` halfwords = the writeback's RGBA5551 white) against native's valid header and zero `0xFFFD`. `ogre_gfx_flush` wrote **every** framebuffer back on **every present** (`if (present || sync_pending)`), on the browser main thread, unordered against a guest that is released as soon as the display list reaches the renderer; RT64 does it in `State::syncFramebuffers` (the game's own `ogre_sync_framebuffers` readback request) and at a workload's retirement, with RDRAM as the source of truth at the start of each workload (`checkRAM`/`uploadRAM`), so CPU writes made after a frame are never overwritten by that frame's stale pixels. **Fix:** write framebuffers back only on `sync_pending`. The mission route then reaches `0x03` with no `0xFFFD` anywhere in `G ±0x8000`, `G-4 = 0x353212FF` (native's own value), and renders (`nonBlack=303148`, `colorful=288300`). The njpeg assembly is excluded on this route: no `M_NJPEGTASK` (RSP type 4) and no S2DEX2 (`ucode 0x800A5110`) task appears; `0x80243E10` is simply the allocator's large-block address, shared by the New Game path's `'B5'` image and the part cache. The clean rebuilt build (no probes) runs the same route to `[scene] t=48479ms id=0x0003` with `nonBlack=303148`, `colorful=287804` and no `pageerror`; `docs/proofs/web-mission-clean-build.png` is that canvas beside native `t.800` (`meanAbs 23.55/15.12/7.88`, `exact=0.3091`, against session 169's probe-run `23.54/14.96/7.86`/`0.3094`). `make recomp && make bank-recomp` (probe revert) + `make web` clean, `boot.cjs --attempts 3 --secs 40` `RENDERED` (`nonBlack=25160`), `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 (`fbtex hits=1 lines=1`), `grep -rn "probe170" RecompiledFuncs/ Bank*Funcs/ app/ debug/` empty. See `docs/HANDOFF-2026-10-18-session170.md`.

- ⚠️ **The browser mission trap is a 16-bit RGBA5551 white fill in the game's own heap block, and the mission renders when the fill lands eight bytes higher (session 169).** The page's `pageerror` stack names the site: a guest `MEM_W` store inside `func_80071950` (message-queue node unlink, wasm `i32.store` at `0x9d2b9`), from `func_800712C4` (queue send) from `func_801792DC` (flush the pending queue message), which is the first call of the mission enter `func_80173724` (`0x8018F350+0x00`); `func_80075BC0` (dispatcher) and `func_80071EB0` (dispatch thread) are above it. The flush loads the global `0x8018FC88`, set by the queue init `func_8017909C` to the allocator's `0x100 * 0x80` block at `0x80243E10`; a tagged `probe169` dump (reverted) shows both builds hold that same pointer at all four flush points of one run, so the pointer is excluded. The words at the block are the difference: native `G-0x20..G-0x18` = `0x80243DD0 / 0x8024DA20 / 0x800AEDB8` and `G-8` = `0`, browser `0xFFFDFFFD` (two `0xFFFD` halfwords = RGBA5551 `(31,31,31,1)`), so the send's `*(a0-8) >= 33` test unlinks a node whose links are that pattern and stores through `0xFFFDFFFD`, i.e. `rdram + 2 GiB`. The bank-link hypothesis is refuted three ways: `libBankPFuncs.a` is in `build-wasm/CMakeFiles/ogrebattle64.dir/linkLibs.rsp`, all 22 `func_ovlP_*` symbols are in `build-wasm/ogrebattle64.wasm` (the record's whole table), and native requests the sixth record (`rom=0x171EC0 ram=0x80214FA0`) only after an A press inside `0x03` (`/tmp/census167/escort-dec-s4.log:985192`), not on the enter path. The clobber is timing-dependent: with the 16-word dump enabled it started at `0x80243E08`, the flush found a valid `G-0x20`, and the browser drew the mission (`nonBlack=303148`, `colorful=287168`) — `docs/proofs/web-mission-first-frame.png` is that canvas beside the native `t.800` (`meanAbs R=23.54 G=14.96 B=7.86`, `exact=0.3094`, against session 168's two-different-screens `81.14/73.78/54.39` and `0.0406`). The clean build still traps, and the writer of the white fill is unnamed: the native watchpoint on `0x80243E10` reports only `func_8017909C`, the port's 16-bit guest-pixel writers target real framebuffer offsets, and the run created only `0x000400`/`0x025C00`/`0x04B400`. No source change landed. `make web` clean, `boot.cjs --attempts 3 --secs 40` `RENDERED` (`nonBlack=24972`), `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0, `grep -rn "probe169\|OGRE_P169" RecompiledFuncs/ Bank*Funcs/ app/ debug/` empty. See `docs/HANDOFF-2026-10-18-session169.md`.

- ⚠️ **The browser boots from a battery save and reaches mission scene `0x03`, and the mission traps before its first frame (session 168).** `?OGRE_SAVE=<url>` fetches a 32 KiB port image, writes it to `/ogre/saves/ogrebattle64-us-rev1.bin` after the IDBFS load, `syncfs(false)`s it, and holds `ogre_start_boot()` until the import settles (`app/web/web.js`; `pi.cpp:288/414/435-440` reads that exact path from `init_saving`). The page also gained `OGRE_TAP_SCENE_BUTTON`, the native scene-keyed tap schedule (`<scene>:<buttons>[:<count>]`, `title 0x04`, `0x12`, ...) keyed on the scene id parsed from the page's own `[scene]` lines, so no wasm export was added. With `assets/escort_suspend.bin` and `?OGRE_SCENE=title&OGRE_TAP_MS=4000&OGRE_TAP_SCENE_BUTTON=title:start:6,0x12:a:6` the browser logs `[web:save] imported assets/escort_suspend.bin (32768 bytes) -> /ogre/saves/ogrebattle64-us-rev1.bin`, then `[save] loaded /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)`, then `[scene] t=85121ms id=0x0003 descriptor=0x8018F350 mask=0x0000038C`, i.e. the mission descriptor. It loads five of the six overlay banks the native run loads (`0x0E4910`, `0x0EBBD0`, `0x101D00`, `0x145230`, `0x14EC00`), then the worker logs `Uncaught RuntimeError: memory access out of bounds` and submits no mission display list; the native continues with `0x171EC0 ram=0x80214FA0 size=0x6030 (22 functions)` and renders. The canvas keeps the last presented frame (the Load Game screen with its `Data will be erased when game is resumed` prompt) byte-identical for 25 s of captures. So there is **no browser mission frame**: `docs/proofs/web-mission-baseline.png` is that frozen canvas beside the native mission frame `t.800` (intro, `Castle Burgund`), and the `meanAbs` `81.14/73.78/54.39` between them measures two different screens. The census's 916 448 lit, 926 539 backface-culled and 516 463 depth-tested draws of scene `0x0003` remain the expected size of the depth/lighting/culling gap, not a measurement. `make web` clean; `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 (`fbtex hits=1 lines=1`). Both knobs stay; no probe in the tree (`grep -rn "probe168\|OGRE_P168" app/ debug/ docs/` empty). See `docs/HANDOFF-2026-10-18-session168.md`.
- ✅ **RT64's half-pixel screen offset is the tutorial `0x17` box's one-texel difference; the triangle path now applies it, and the box matches the native (session 167).** Session 166 left the box's parchment draw as "a one-texel difference, mechanism unnamed". The mechanism is RT64's `halfPixelOffset`: `tools/RT64/src/render/rt64_framebuffer_renderer.cpp:1528` builds `{1/width, -1/height}`, lines 1571-1572 assign it as `triangles.screenOffset`, and `RasterVS.hlsl:25` applies it as `ndcPos.xy = ndcPos.xy * screenScale + screenOffset * ndcPos.w`, i.e. half a pixel right and half a pixel down for every non-rect vertex. The arithmetic reproduces session 166's measurement from its own probe dump: at ~0.98 texel per pixel a -0.49038 texel `v` shift changes `floor(v)` on **24/24** rows of 172..195 and on exactly **1** of 23 rows of 196..218 (row 196, whose fraction is 0.008), and the opposite sign changes 0/24, which fixes the direction (the quad moves **down**); session 166's brief expected 0/23 for 196..218, and the native at row 196 samples the shifted row, so that row agrees with the mechanism. `app/src/web_renderer.cpp`'s `transform_vertex` now does `sx += 1.0f / screen_width(st); sy -= 1.0f / screen_height(st);` before writing `out[0..1]`, with RT64's file and line in the comment; `transform_vertex`'s only caller is `draw_tri`, so the texrect, S2DEX rectangle and fill paths are untouched. Against the session-166 native target at the box frame's `(0,0)` alignment the box goes `5.88 -> 0.22` over rows 166..219, the parchment rows 172..195 `10.49 -> 0.25`, the lower rows 196..218 `2.01 -> 0.17`, the portrait `1.10 -> 0.00`, the name line `13.32 -> 0.00` and the text body `4.01 -> 0.00`, with the texrect border rows `0.33 -> 0.29`. The texel-row test is 24/24 at columns 100, 150, 200 and 285 (3/24 at column 285 before). The backdrop is triangle draws too and moves as RT64's rule says (`9.52 -> 6.21` on rows 0..160, confounded by the native's own scroll phase). The title does not move (best native match `0.20` before, `0.18` after, at its scroll phase) and the world map's frame moves only on its triangle-drawn unit sprite (451 pixels after one fade normalisation, whole-frame shift search still `(0,0)` at `0.87`). No regression: `make web` clean three times (fixed, the A/B build with the two lines commented out, fixed again), `boot.cjs --attempts 3 --secs 40` `RENDERED` (`nonBlack=24808`), `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 `fbtex hits=1 lines=1`. New proof `docs/proofs/web-tutorial-half-pixel-before-after.png` (browser before / browser after / native). No probe kept (`grep -rn "S167AB\|OGRE_P167\|probe167" app/ debug/ docs/` empty). See `docs/HANDOFF-2026-10-18-session167.md`.
- ⚠️ **The tutorial `0x17` box's parchment draw is point-sampled, and RT64 selects a texel one row above the browser's; session 165's "two texels per pixel" is withdrawn (session 166).** Session 165 left the box's remaining 5.4 mean as a "sub-texel phase" of one triangle draw. A tagged `OGRE_P166` probe measured the draw itself: the box interior is **one** quad of two triangles over the whole interior (rows 170..222) of a 64x64 CI4 image (`line=4 tmem=0`, rect texels 0..63, `masks=maskt=6`), `OTHERMODE_H=0x088C30` so **`G_TF_POINT`** (point sampling), `TEXEL0` straight through the combiner, `w=1` on all three vertices, `u` mirrored 265..0 over `x` 27.2..292.8 and `v` 0..51 over `y` 170..222, i.e. about **0.98 texel per pixel**; there is no separate flat lower band, and the row-196 boundary session 165's per-row profile showed is a contrast boundary in the paper texture. The browser's sampled texel equals its decoded image at all 25 probed rows, `maskt=6` wraps, the tile origin is 0, and RT64's `round(uv*128)/128`, `tcScale`/`ulScale`, RSP viewport (`scale (160,120)`, `translate (160,120)` in both) and `RasterVS` screen-to-NDC mapping are identical to the browser's. Against RT64's own capture, aligned on the box frame (`meanAbs 0.33`; portrait 1.10), RT64's texel row is the browser's **minus one** at **24 of 24** rows of 172..195 (3 of 24 at zero offset, 11 of 24 for the best other column), i.e. RT64's quad sits about one screen pixel lower; the texrect border and portrait are exact, and the lower parchment rows differ by only 3 levels because the texture has no contrast there. No mechanism is named in either renderer's code, so **no code changed** and the item stays open with the next experiments named (a synthetic 1:1 triangle with a sharp horizontal edge captured in both renderers, then a probe of the browser's `posScreen`). `make web` clean with and without the probe; `testdraw --flags 96 --expect-fbtex --expect-hits 1` exit 0 `fbtex hits=1`; `boot.cjs` was NO RENDER over 17 boots while the machine's load average was 24-52 (the same build rendered the tutorial in the probe run, `tasks=13`); no probe kept (`grep -rn "probe166\|OGRE_P166\|p166" app/ debug/` empty). See `docs/HANDOFF-2026-10-18-session166.md`.
- ✅ **The browser samples TMEM and runs the RDP's alpha-compare/coverage path, and the tutorial `0x17` dialogue box matches the native (session 165).** The tile decode read RDRAM with the `SETTIMG` width as the row stride, and the RDP samples TMEM, where a load's source stride and the tile's `line` are independent: the tutorial's strip (`img=0x24E340`, `line=1`) and portrait (`0x19F1E0`, CI8 `line=5`) were decoded with a 2-byte row stride, so the portrait tore into stripes and the glyphs (CI4, `line=1`) sheared. `web_renderer.cpp` now keeps a persistent 4 KiB TMEM, copies every LOADTILE/LOADBLOCK/LOADTLUT (and YUV) into it with RT64's `loadWord`/`loadToTMEMCommon`/`loadYUVTileToTMEM` layout, and decodes a tile at `tmem << 3` with `line << 3` over RT64's sample window (`min(tile rect, 1 << mask)`), including the odd-row word swap and the palette at `0x800`; mode-0 CI decodes as I again (the title prologue's two unit sprites had turned into opaque black rectangles without it). The second fault was the missing alpha-compare/coverage path: the strip and glyph draws carry `OTHERMODE_L=0x000A7008`, `CVG_X_ALPHA` and `CVG_DST_CLAMP`, so RT64 discards a texel whose combiner alpha is 0 (`resultCvg = 8/cvgRange * combinerAlpha`); the browser painted its palette colour, which made the box border a bright green band and every glyph cell an opaque black block. The shader now implements `G_AC_THRESHOLD`/`G_AC_DITHER`, the coverage discard and the `CVG_DST` target alpha, and the flush sets the new uniforms (`u_cvg_range` 65535 for RGBA16F, 255 otherwise). The box's portrait, border, parchment and cursor now match RT64's target (`docs/proofs/web-tutorial-fixed-side-by-side.png`): the border rows and the portrait are pixel-exact, and the glyph pixels differ by 7.9 mean over the text region, which is the open measurement. The framebuffer-as-texture check moved to the load (the RDRAM read is there now) with the load's own rect, and `test_draw`'s control `G_LOADTILE`s carry their tiles' rects, so `testdraw --flags 96` is `fbtex hits=1` again. No regression: `boot.cjs` `RENDERED`, title frame 1.56 mean abs against the native at its phase. The `[GFX-SAMPLER]` census gained `shifted=` and now prints every 400 textured draws, and the tile `shifts`/`shiftt` field is unexercised on the reached screens (title 1182 draws, map 198, attract 8885, tutorial box 447; all `shifted=0`), so session 164's open item closes as unexercised. The tutorial box's border rows and portrait are pixel-exact against the native target; the 1-pixel offset session 165 first recorded was an error in its own comparison and is corrected in the handoff. The remaining 5.4 mean over the box is one triangle draw, the upper parchment band, which samples the 64x64 image at about two texels per pixel and lands on a different sub-texel phase than the native's (the flat band below it matches at 1-4 levels); the glyphs sit on it; the next measurement is that draw's texels and weights. The browser now interpolates attributes perspective-correctly from the vertex clip `w` (RT64 `RSPProcessCS`/`RasterVS`) and carries RT64's `G_TP_NONE` halving; the box's quad has constant `w`, so that change leaves it byte-identical. `testdraw --flags 96` is `fbtex hits=1` and `boot.cjs` is `RENDERED` after the vertex format changed from 8 to 9 floats. No probe kept (`grep -rn probe165 app/` empty); the temporary `OGRE_P165`/`OGRE_P166` probes and the `OGRE_NOCVG` control were removed. See `docs/HANDOFF-2026-10-18-session165.md`.
- ✅ **The browser samples RDP tiles with a port of RT64's `TextureSampler.hlsli`, and the title's fog band is point-sampled (session 164).** `OGRE_TEXF` selects `rt64` (default), `bilinear` or `gl` (the old fixed-function path). The port covers `clampWrapMirrorSample` — clamp to the tile window, modulo `masks`/`maskt`, mirror on `G_TX_MIRROR` — `sampleTextureLevel`'s point/bilerp/average selection from OTHERMODE_H, RT64's default three-point filter, and `round(uv*128)/128`; each draw now carries its per-unit texel size, tile origin, clamp/mirror mode, wrap size and `G_TF_*`, so filtering is draw state instead of a property of the uploaded texture. The new `[GFX-SAMPLER]` census measures what the screens sample: forced title 591 textured draws (point 591, masked 456), forced world map 99 (point 99, masked 1), publisher still 56 (point 56, masked 0), tutorial `0x17` 8 (point 2, bilerp 6, masked 6). `[GFX-FOG-DRAW]` shows the repaired fog groups are point-sampled (`filt=0`, `mask=(64,64)`, `cm=(0,0)`) 320x1 strips and 64x64 blocks, which answers session 163's next step and excludes filtering as the 1-3-level fog band's cause. The tutorial's background is a 64x64 tiling I8 image drawn 447x335 texels over 320x240 with `masks=6`, the first reached bilinear draw; the tutorial's dialogue box is broken identically under both samplers and is recorded as a new open item. No regression: `boot.cjs` `RENDERED`, `testdraw --flags 96` exits 0 with `fbtex hits=1` and after-batch `nonBlack=3024`, title series `0.134` mean against the native with band `0.070`. No probe added; `grep -rn probe164 app/ debug/` is empty. See `docs/HANDOFF-2026-10-17-session164.md`.
- ✅ **The browser colour image is a high-precision attachment, and the title/story fog band is not blend rounding (session 163).** RT64's `internalColorFormat` defaults to Automatic, which resolves to High on this machine (`preferHDR`), and its native capture is `R16G16B16A16_UNORM`, so RT64 keeps every blend at 16 bits and quantizes once on the RDRAM writeback. A browser colour image is now `GL_RGBA16F` when `EXT_color_buffer_float` is available and `GL_RGBA8` otherwise or with `OGRE_HPFB=0`; the float attachment needs `type=GL_HALF_FLOAT` (WebGL2 rejects `UNSIGNED_BYTE` with INVALID_OPERATION and an incomplete 0x8CD6 attachment), float staging in `upload_framebuffer`, a `GL_FLOAT` readback through a new `float4_to_rgba16` (RT64 `Float4ToRGBA16`, dither 0, 16-bit `usesHDR` coverage test), and an RGBA8 fallback when the attachment is incomplete; the fragment shader now asks for `highp`. **Measured** at the same native instant (`t.177`): RGBA16F `meanAbs=0.098` (bands 0.014/0.271/0.060) against RGBA8 `0.099` (0.014/0.273/0.060), and the two canvases differ by 0.0091 mean (99.09% identical, 0.906% of channel samples all exactly one level, 1055 below / 988 above). Session 162's "0-3-level blend rounding" is therefore corrected: against the native target converted with its own 8-bit rounding the fog band is still 0.241 (the target's own high-byte-vs-round gap is 0.068 there), and the amplified difference is one wave in rows 81..119, the translucent top edge of the repaired fog layer. The new `[GFX-FOG-SUMMARY]` image fields show that layer is a 64x64 I8 image (`siz=1 fmt=4`, layer 2, `0x801D51E8`), so the next comparison is the sampled intensity and blend alpha of one repaired group. No regression: publisher still `[0.0,0.0,0.0]/100%`, `testdraw --flags 96` `fbtex hits=1`, `boot.cjs` `RENDERED` (`nonBlack=25424`). No probe added; `grep -rn probe163 app/ debug/` is empty. See `docs/HANDOFF-2026-10-17-session163.md`.
- ✅ **The `OGRE_FOG` repair is ported to the browser display-list path, and the title frame now matches the native fog-on target (session 162).** Session 161 verifies from its own artifacts: the browser title frame against the native fog-on series is `meanAbs=2.349` (`[2.08,2.23,2.74]`, 79.2% exact) and against the fog-off series `0.052`, so the missing fog was the whole residual. The repair's rule was read end to end in `app/src/renderer.cpp` (`nop_rect::Walker::fog_fix`, `namespace fog`): a white-combiner texrect (`w1 == 0xFFFF73B9`) whose inherited tile window covers no texels (`lrs == uls` or `lrt == ult`) reads layer `i`'s image record from the table at `D_801B80E0` (`+0xFC + i*0x24`, record byte 2 `fmt`, byte 3 `siz`, 16-bit height/width, `+8` data) and rewrites the inherited `SETTIMG`/`SETTILE t7`/`SETTILE t0`/`LOADTILE`/`SETTILESIZE t0` to sample that whole image (tile 0 carries `masks`/`maskt` for the sweep's wrap; layer indices advance 0, 2, 3, ...). The browser port (`web_renderer.cpp` `FogWalker`, `fog_repair_group`, `fog_repair_display_list`) performs the same reads and the same rewrite on the same bytes, in `execute_dl` before `gbi::walk_dl`; `renderer.cpp` is unchanged. `OGRE_FOG=0` disables it (default on) and `?OGRE_FOG=0` reaches `getenv` through the page's `ENV`. **Measured** over fresh series (native 397 fog-on / 587 fog-off frames at `OGRE_SPEED=1`, browser 40 shots each way): fog-on minimum `t.177` `meanAbs=[0.09,0.09,0.12]` 91.5% exact (bands 0.014/0.273/0.060) against `[2.08,2.23,2.74]`/79.2% before; fog-off minimum `[0.03,0.03,0.03]` 97.3% (0.042/0.011/0.026); the cross-controls are 2.740 and 2.733. The rows-64..119 remainder is 0-3-level blend rounding: the browser's per-band fog overlay is `-0.683/5.691/-0.007/6.719` against the native's `-0.674/5.705/-0.007/6.637`, and the band-minimising native frame gives 0.074 with every difference exactly one level. Publisher still `[0.0,0.0,0.0]/100%` over 639x475, `boot.cjs` `RENDERED` (`nonBlack=25104`), the attract story keeps its fills and letterbox, and none of those runs fires the repair. No probe was added; `grep -rn probe162 app/ debug/` is empty. Session 163 corrects the blend-rounding attribution. See `docs/HANDOFF-2026-10-17-session162.md`.
- ✅ **The title screen's band residual is the browser's missing `OGRE_FOG` repair, and the two runs share a scroll state (session 161).** Continue the browser renderer from session 160 and decide whether the residual is a renderer difference or an attract-scroll state difference. **Session 160 is verified**: `grep -rn probe160 app/ debug/` is empty, `app/web/index.html` no longer carries `border-radius`, `present_framebuffer` sizes the canvas to the visible area, and the publisher still reproduces `meanAbs=[0.0,0.0,0.0] exact=100.0% over 639x475` against `tt.140`. The canvas is 639x475 and all 303525 of its pixels equal the target, so the presenter leaves no canvas pixel unwritten; `border-radius` was the last canvas pixel that was not game output. **The per-draw `s`/`t` comparison.** The browser trace (`/tmp/w156/webtrace2-a1-pagelog.txt`) and the native trace (`/tmp/w156/nat-trace2.log`) were split into display lists. At the third list of each run the 135 non-degenerate draws are identical in order, image, rectangle, `s`, `t`, `dsdx`, `dtdy` and tile window (the sky image `0x1C3EA8`'s partial tile is `0,16..63,80 s=32` in both), so the two runs are at the same scroll state; the other lists are different attract instants (partial tile widths 63, 61, 43, ... vs 63, 38, 37, ...). **The difference is presence, not an offset.** Every native list holds 456 more draws whose tile window is degenerate (`ult == lrt`): the mirrored `0x1E2918` pieces (`s=1984/2016 dsdx=-1024 x=0..383 rows 64..128`) and the 72-pixel `0x217508` columns (`x=0,42,...,402`). Those are the `OGRE_FOG` repair's pieces (`app/src/renderer.cpp`, `nop_rect::Walker::fog_fix`): the native app rewrites their tile setup to sample the layer image, while `web_renderer.cpp`'s `draw_texrect` rejects them with the guard that mirrors RT64's `RDP::drawTexRect`, so the browser draws no fog. **One-variable A/B.** Two forced-title native series differing only in `OGRE_FOG=0`: the fog contributes 3.4 mean abs overall (rows 64..119 `5.68`). The browser frame against the fog-off series is `meanAbs=0.052` (rows 0.046/0.072/0.044, best `t.584` of 492) and against the fog-on series `2.349` (= session 156's `[2.08,2.23,2.74]` averaged, rows 64..119 `4.15`). So the residual is exactly the fog layer and nothing else; the sampled and composited cloud was already exact. No renderer behaviour changed; the port of the fog pass is the open item. See `docs/HANDOFF-2026-10-17-session161.md`.
- ✅ **The canvas is the VI's visible area, so every pixel of it carries frame content, and the rounded corners are gone; the LOADTLUT destination is unexercised on every reachable screen (session 160).** Continue the browser renderer from session 159 and close its two mechanical items. **Session 159 is verified**: `grep -rn probe159 app/ debug/` is empty, `git diff --stat app/` is the pre-session 244-line `web_renderer.cpp` plus 6 lines of `web_platform.cpp`, and `/tmp/w156` recomputes `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2%`. **The TLUT destination was measured before any change.** `probe160` printed each `LOADTLUT`'s destination `tmem`, the tile's `palette`, the entry and byte count, the source address, and for every CI draw once per distinct `(tmem, palette, siz)` the format, size, rect and LUT mode. Over 8 forced scenes (`0x04`, `0x05`, `0x07`, `0x0A`, `0x0B`, `0x0C`, `0x17`, `0x18`; 2/4/19/1/1/0/0/4 display lists) plus the forced publisher still, the distinct pairs are exactly one: all four loads are `tmem=0x100 palette=0` (16 entries/32 bytes at `0x1F16C0` and `0x1F1A90`, 256/512 at `0x1F1A00` and `0x207CA8`), and all six CI draws are `palette=0`, `tmem=0x0` (their own texture address), `siz=0` against the 16-entry tables and `siz=1` against the 256-entry ones, one `lut_mode=0` reading no palette. **RT64's rule, quoted**: a load writes at `loadTile.tmem << 3` with `tmemStride = line << 5` and 8 bytes per entry (`loadToTMEMCommon<..., TLUT=true>`, `loadWord` with `offsetMask = 0x1`), while the sampler reads from the fixed base `#define RDP_TMEM_PALETTE 0x800`, CI4 at `0x800 + (palette << 7) + (index << 3)` and CI8 at `0x800 + (index << 3)`. `tmem=0x100` is byte `0x800`, so the browser's `palette*16` bank over a 256-entry table is the same addressing on every measured screen. The item closes as **unexercised**, and `OGRE_TLUT_TRACE=1` now prints one `[GFX-TLUT]` line per distinct load destination saying whether it is the assumed `tmem=0x100 palette=0`, so a screen that loads elsewhere reports itself. **The unwritten column and row, measured.** `present_framebuffer` sized the drawing buffer to the VI's `fbSize` and `blit_to_canvas` wrote only the visible area (`fbSize` minus the overscan crop) into its top-left, so 639 of 640 columns and 475 of 476 rows were written. Against RT64's `tt.140`: the publisher still's last column (476/476 pixels) and last row (640/640) differ by a mean of 222.4 and 223.1 levels, the four corners show the page background `(20,22,26)` against `(239,231,206)` (deltas -219/-209/-180), and the `border-radius:6px` arc makes 26 pixels of the visible area differ by 1..219 levels. The title's own edge is black, so there only the corners show (13..38 levels). **The fix.** The drawing buffer is the visible area (`resize_canvas(visible.width, visible.height)`; the present fraction is 1.0, so the 1:1 blit writes every pixel), the source rect is unchanged, the RDRAM fallback stages at the VI's pitch, and `#game-canvas` loses `border-radius` because those four corner pixels are game output and clipping them blends them with the page background. **Before/after.** Publisher still `[0.87,0.84,0.75] / 99.6%` over 640x476 becomes `[0.0,0.0,0.0] / 100.0%` over 639x475, with every corner and both edges equal to the target; the same still's presented pixels are otherwise identical (303499 of 303525, and the 26 that changed are exactly the corner arcs), so no frame moved. Title: the stored baseline PNG is unchanged (`[2.08,2.23,2.74] / 79.2%` over 318x238), and the fresh capture measures `[2.31,2.52,3.15] / 81.4%` over 319x239 before and after with the two captures pixel-identical; the four corners go from `(20,22,26)/(30,33,38)/(30,33,38)/(10,11,13)` to the target's `(33,16,0)/(16,8,0)/(0,0,0)/(62,59,29)`. `boot.cjs` reports `RENDERED` (`nonBlack 24756`; the count is frame-dependent, and the corner arcs alone are 92 non-black pixels of it). Both probes are reverted (`grep -rn probe160 app/ debug/` empty) and `make web` is clean. See `docs/HANDOFF-2026-10-17-session160.md`.
- ✅ **The title screen's layers decode to the native's content and alpha, so the band residual is the composition beneath them (session 159).** Continue the browser renderer from session 158, whose two survivors were the alpha the sampler uses for the cloud image `0x217508` and the destination it blends onto, with one native run of `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1` named as the separating measurement. **Session 158 is verified**: `grep -rn probe158 app/ debug/` is empty, the uncommitted `app/` diff is the accumulated 141..157 work with no probe text, `/tmp/w156` recomputes `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2%`, and its correction of session 157 holds against the trace it names (`/tmp/w158/probe-milestones.txt`: 22 `rec` lines, all `img=0x249538`, `rect=0,Y..200,Y+5`, `win=..796,..`, `up=200x5`, so the `s` span is 0..200 texels over texels 0..199; `PLAN.md` and `docs/DECISIONS.md` carry no surviving 320-texel claim for this draw). **The named measurement cannot run as it stands**: `tex_trace::colour_mean` decodes CI8 and RGBA16 only (`app/src/renderer.cpp:637-646`), so the 11546 RGBA32 cloud draws of a 32 s run print no `colour:` line. **The probe.** `probe159` added the tile-window decode of an RGBA32 draw to the native (`x0 = uls>>2`, `y0 = ult>>2`, `w = (lrs>>2)-x0+1`, row stride `(imgw+1)*4`, reporting RGB mean, mean alpha, `alpha>=128` and `alpha==255` counts) and the same alpha statistics to the browser's `[GFX-TEXTRACE]` line. **Result: content and alpha agree.** `native windows: 113, browser draws: 559, match 444, mismatch 0`, including all 54 windows of `0x217508` over 53 native frames (`56.8/0`, `58.7/15`, `61.3/33`, ... identical on both sides and constant across the native span), `0x1E2918` (`66.7/0`, `67.1/18`, `70.6/56`) and the fog (`mean 0.0`, its texel RGB; the fog's drawn colour comes from its combiner). The cloud is a static image, both renderers hold the same RGBA32 bytes and the same alpha, and survivor (a) is excluded at the decode level. **The residual's shape.** On the session-156 pair, rows 120..189 are exact (`slope 1.001 corr 1.000`), rows 190..238 at `x<140` are exact (`1.000`), rows 190..238 at `x>=140` are `slope 1.008 corr 0.858` (native brighter), and the per-row difference is zero at rows 186..193, rises from row 194 to +12.4 mean signed around rows 205..225 and is zero again at row 234; over rows 195..232 the differing pixels (5116) start at a slanted left edge (x≈168 at row 200, x≈152 at row 210) and mean +18.9. It is not a translation (identity shift matches 1268 of 6384 samples; ±1 in x or y matches 518 or fewer) and not an alpha scale on the composite (correlation with the browser's own value 0.011). **Why no mechanism is named.** The two runs' lists differ: the browser's is 135 textured draws with the cloud as 53 full-width 320x3 strips, the native's 590 with 54 full-width strips plus 455 72-pixel column draws at `x = 402..474` (outside the 320-wide image the title presents) and a 13-pixel sky scroll. Session 156's 0.46-3.01-level bound on adjacent native phases keeps the phase out of the residual, but a layer cannot be attributed by comparing the two lists, so the next instrument is a per-draw readback of one target row inside one renderer against that run's own RDRAM. No renderer behaviour changed. Both probes are reverted (`grep -rn probe159 app/ debug/` empty), `make web` and `make app` are clean, the publisher still is `[0.87,0.84,0.75] / 99.6%` and `boot.cjs` reports `RENDERED` (`nonBlack 25136`). See `docs/HANDOFF-2026-10-17-session159.md`.
- ✅ **The title's fog layer samples in-window with no mask, so the RDP tile-window candidate is excluded and the residual stays on the translucent cloud (session 158).** Continue the browser renderer from session 157, whose measured lead was that the fog image `0x249538` is 200 texels wide while its draw's `s` was said to run to 320, with no `masks`/`cms` masking in the browser. **Session 157 is verified**: `grep -rn probe157 app/ debug/` is empty, `make web` builds clean, and `/tmp/w156` recomputes `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2%`. **The path was read first**: `G_SETTILE` fills `cmt`/`cms`/`maskt`/`masks`/`line` (`3795-3811`) and `G_SETTILESIZE` fills `uls`/`ult`/`lrs`/`lrt` (`3814-3821`); `ensure_tile_image` decodes the window as the whole image (`4154-4202`); the only `cms`/`masks` use is the wrap choice `clamp_s = (cms != 0) || (masks == 0)` (`4279-4280`) applied as `GL_CLAMP_TO_EDGE`/`GL_REPEAT` (`2403-2406`); and the vertex shader normalises the texel coordinate by the window, `v_uv0 = (a_uv - u_uv_origin) * u_uv_scale` (`1065`). **RT64's rule** (`tools/RT64/src/shaders/TextureSampler.hlsli` `clampWrapMirrorSample`): clamp to `lrs`/`lrt` only when `cms`/`cmt` has `G_TX_CLAMP`, wrap by `masks`/`maskt` only when those fields are non-zero, then `texelMask`/`texelShift`. **The probe refutes the lead.** `probe158` printed all 22 five-row fog strips: `masks=0 maskt=0 cms=0 cmt=0`, upload `200x5`, both wraps `GL_CLAMP_TO_EDGE`, and the browser's texel equals RT64's rule's texel in 66 of 66 comparisons (same `(x,y)`, same decoded alpha). The `s` span is 0..200 texels (`uv0=(0.000,0.000)`, `uv3=(200.000,5.000)`) over `uls=0, lrs=796` = 0..199, so nothing is out of window; session 157's "s runs to 320" is corrected. **The fog draw is correct.** The probe's `glReadPixels` shows it writes the texel at alpha 255 (`bef=(0,0,0)` -> `aft=(18,39,36)`) and leaves the pixel alone at alpha 0, and the fresh browser frame equals the native capture in 36 of 36 sampled pixels of rows 200..231, which the fog writes opaquely. **Where the residual is.** The fresh pair (`probe-00` vs `t.137`, `[2.32,2.52,3.14] / 81.3%`) gives rows 120..189 `0.03`, rows 80..119 `-7.75` across the whole width, rows 190..238 `0.05` at x < 140 (fog opaque) and `-11.5` at x >= 140 (cloud shows). Every differing row is a row whose visible layer is translucent and every opaque row is exact, so the residual is the partial-alpha composition of `0x217508`. **Open.** One native run with `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_COLOUR=1` compares the native's decoded `mean`/`opaque` per strip with the browser's; equal numbers put the difference in the composition (the destination the cloud blends onto), different numbers put it in the image's content or alpha. No renderer behaviour changed. The probe is reverted, `grep -rn probe158 app/ debug/` is empty, `make web` is clean, the publisher still is `[0.87,0.84,0.75] / 99.6%` and `boot.cjs` reports `RENDERED` (`nonBlack 25136`). See `docs/HANDOFF-2026-10-17-session158.md`.
- ✅ **The browser's cloud blend is correct at the draw, and the title's band residual is a partial-alpha layer of the composition (session 157).** Continue the browser renderer from session 156, whose open item was that the browser's band pixel equals the raw cloud texel while RT64's is a blend, although the decoded state says `ablend=1 force=1 ccyc=1` with a TEXEL0 combiner. **Session 156 is verified**: `grep -rn probe156 app/ debug/` is empty, the `[GFX-TEXTRACE]` format string and its `alpha_blend`/`force_blend`/`combiner_cycles` arguments match one for one, and `/tmp/w156` recomputes the aligned minimum `meanAbs=[2.08,2.23,2.74] meanSigned=[-2.03,-2.17,-2.67] exact=79.2%`. **The path was read before a theory**: RGBA32 decodes bytes 0/1 as R,G and 2/3 as B,A and is uploaded with `glTexImage2D(..., GL_RGBA8, ..., GL_RGBA, GL_UNSIGNED_BYTE)`; the 1-cycle combiner evaluates mux 1, so `0xFCFFFFFF`/`0xFFFCF279` gives `rgb=TEXEL0 a=TEXEL0`; the blender `P=CC M=FB A=CC_A B=1-A` takes the shader's framebuffer-colour branch and outputs `vec4(combiner.rgb, combiner.alpha)`; and `compute_uses_alpha_blend` turns that into `glEnable(GL_BLEND)` with `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA`, which is the RDP's equation. **The probes.** A tagged probe (`probe157`) printed, for the first `0x217508` draw of each flush, `blendEnabled=1 srcAlpha=0x0302 dstAlpha=0x0303 eqRGB=0x8006 err=0`, and a row readback showed the fragment alpha equals the decoded texel alpha (159 at image (8,13), whose decode is `(190,175,157,159)`; 0 at a transparent texel) and that the blended result equals `alpha*frag + (1-alpha)*dst` on 47 of 47 mid-alpha pixels with worst deviation 0. So all three of session 156's candidates are false: blending is on, the alpha is the texel's, and the mapping is the RDP equation. **What the bands are.** The browser and native traces of one display list have the same 136 draws in the same order: an opaque black `G_FILLRECT` (probe: `prim=(0,0,0,1) alphaBlend=0`), then the sky `0x1C3EA8` (rows 0..89 and 16..80), `0x1E2918` (24..129), `0x217508` (80..239) and the fog `0x249538` (126..238). Rows 0..79 and 120..189 are exact because an opaque layer covers them; rows 80..119 and 190..238 are the rows where every layer is translucent. Inside them the two renderers agree exactly where the probed layer is transparent (`rows 200/210/220 alpha-0 pixels: meanSigned 0.0`) and the browser writes the raw texel where it is opaque while the native is 12..44 levels brighter, so the difference is in the *other* translucent layers, not in the cloud draw. **Open.** Point the same row readback at `0x249538` and `0x1E2918`; and the browser has no RDP tile-window masking (`masks`/`cms`), which matters only for `0x249538` because its draw's `s` runs to 320 texels over a 200-texel image. The probe was reverted; `grep -rn probe157 app/ debug/` is empty, `make web` is clean, and `boot.cjs` reports `RENDERED` (`nonBlack 25136`). See `docs/HANDOFF-2026-10-17-session157.md`.
- ✅ **The title's cloud bands are a renderer difference, not an animation phase; the mechanism is localised to the blend of the partial-alpha cloud strips (session 156).** Continue the browser renderer from session 155. The title screen differs from RT64's render target by `meanAbs` about 2.3/2.5/3.1 in two bands (rows ~64..112 and ~195..238, browser darker), and sessions 143-148 could not tell a phase difference from a draw difference. **The measurement.** A native series of 525 `OGRE_CAPTURE_TARGET` frames of the forced title (`OGRE_MOD_TEST=1 OGRE_NO_AUDIO=1 OGRE_SPEED=1 OGRE_SCENE=title OGRE_CAPTURE_TARGET=/tmp/w156/nat2/t OGRE_CAPTURE_AFTER=100 OGRE_CAPTURE_EVERY=1 OGRE_EXIT_AFTER_MS=32000`, 320x240) was compared with a 40-frame browser series of the same forced scene (`shots.cjs --shots 40 --gap 400`, 320x240 buffer at a 640x480 CSS box). The minimum over the series is `title-03` against `t.574`: `meanAbs=[2.08,2.23,2.74]`, `meanSigned=[-2.03,-2.17,-2.67]`, 79.2% exact with the presenter's unwritten edge inset out (`[2.31,2.32,2.71] / 79.2%` without). **Phase is excluded.** Native frames of one phase are identical in the bands (`t.573` vs `t.574` = 0.00 in all four row ranges) and adjacent phases differ by 0.46-3.01, so a browser phase between two native samples costs at most ~1.5; the measured band residual is 8-13 and one-signed (`meanSigned == -meanAbs`), never brighter. **The draw is the same.** For the strip at rows 101..104 both traces give the same rect, `s=0 t=672 dsdx=dtdy=1024 window=uls0 ult84 lrs1276 lrt92`, `img=0x80217508 fmt=0 siz=3 line=80`, palette 0, LUT mode 0; RT64's `[rect-state]` says 1-cycle, combiner `0xFCFFFFFF`/`0xFFFCF279` (TEXEL0 straight through) and force blend `P=CC A=CC_A M=FB B=1-A`, and the browser's new `cyc=1 omh=0x000C30 oml=0x504240 ablend=1 force=1 ccyc=1` says the same. The effective draw lists also agree: the native trace's 591 texrects per frame include 456 whose tile window is degenerate and which both renderers skip (`RDP::drawTexRect`'s `OGRE_EMPTY_TILE` guard and `draw_texrect`), leaving the same 135. The browser's decoded texel mean equals an independent decode of the same RDRAM rows (92.11/92.1, 104.55/104.6, 66.73/66.7). **The open lead.** The band rows are exactly where the `0x80217508` strips sample partial-alpha texels; there the browser's pixel equals the raw texel (94/95, 108/140, 155/156, 245/246 pixels at rows 98/101/104/110) while the native's is a blend over the layers below (row 98 x=40: texel `(159,142,103)` alpha 34, browser `(160,142,103)`, native `(172,157,123)`). So the sampled alpha is not the value the GL blend uses even though the state says it should be; the next experiment is a tagged probe printing the fragment's combiner alpha for one cloud strip. An alpha-forcing probe (`probe156`, every RGBA32 texel alpha 128) changed the frame hugely (`[27.26,28.91,18.45] / 15.3%`) but cannot separate the hypotheses and was reverted. `[GFX-TEXTRACE]` gained `cyc/omh/ablend/force/ccyc`; no pixel moves. Verified: `make web` clean after the revert, `grep -rn probe156 app/ debug/` empty, `boot.cjs --attempts 2 --secs 40` `RENDERED nonBlack 25136 / colorful 16528`. See `docs/HANDOFF-2026-10-17-session156.md`.
- ✅ **The browser's `[GFX-FBTEX]` row inversion was one row up, and no reachable screen needs framebuffer-as-texture (session 155).** Session 154 left two items: the row over-report it had recorded, and the decision on the feature. **The inversion.** `note_fb_write` computed `height - 1 - trunc((pos + 1) / 2 * height)` for a vertex, and `ndc_yf` maps guest row `y` to `1 - 2y/H`, so the recorded row was `y - 1` where the float product lands on or above the integer (194 of the 241 rows of a 240-tall image) and `y` where it lands below; a fill of rows 100..163 was recorded as 99..163, and the `std::max(gy, 0)` clamp hid the error for a draw starting at row 0. The inverse is now `(1 - pos) / 2 * height`, the vertex is treated as an edge (the lower-right corner is exclusive, so the last row is the bottom edge minus one) and `std::lround` recovers a rect's authored integer, so the recorded rows are the rows the draw covers. **The assertion.** `test_draw`'s debug flag bit 6 gained a row control: a second colour image at `0x00C28000`, a `G_FILLRECT (0,100)-(63,163)`, and a load of rows 98..99 (two rows because `draw_texrect` drops a tile with `lrt == ult`). One variable, the inversion: the old build prints `fbtex hits=2 lines=2` with `rows=[98,99] drawn rows=[99,163]` and `FAIL: expected fbtex hits=1, read 2`; the fixed build prints `fbtex hits=1 lines=1` and exits 0. `--flags 32 --expect-fbtex` is unchanged at `hits=1` and `--flags 0 --expect-hits 0` at `hits=0`. **The decision.** The full 25-scene sweep (`fbtex.cjs --scenes 0x00..0x18 --secs 18`) reports `TOTAL framebuffer-as-texture loads: 0 over 25`; 21 of the scenes produced display lists (3..136 tasks), `0x02`/`0x03`/`0x0D`/`0x0E` produced one task and a `memory access out of bounds` page error so they say nothing, and `0x0B` reached only the 3-task idle trajectory. A natural tap route through the opening (`OGRE_TAP_MS=1200&OGRE_TAP_BUTTON=start,a,start,a,a,a,a,a,a,a`, 30 s) reports `tasks=196 hits=0`, and the fixed build re-checks `0x04`/`0x0A`/`0x10` at `hits=0`. The item closes as **no screen needs it, with the sweep recorded**, not as "unimplemented"; the detector and `fbtex: hits=N` stay so a screen that does need it reports itself. Diagnostic only: no rendered frame moves, and `boot.cjs` still reports `RENDERED` (`nonBlack 25136`). See `docs/HANDOFF-2026-10-17-session155.md`.

- ✅ **The browser's framebuffer-as-texture check compares the drawn columns as well as the drawn rows, so it stops reporting loads the draw never covered (session 154).** Session 152 recorded that `note_fb_texture` tested a texture load against the rows a recorded draw wrote and not the columns, so a load whose bytes lie in a drawn row but outside every drawn column was reported as a framebuffer-as-texture hit. The load side had the same shape: it claimed the whole `stride` of each row for a rect that `decode_texture_rect` reads as `x0*bpp .. (x0+w)*bpp`. **The fix.** `FbWrittenRange` carries `col_min`/`col_max`, computed in `note_fb_write` from the same vertex mapping the row test uses (`(pos + 1) / 2 * fb_width_px`, the inverse of `ndc_xf`, times `bytes_per_texel(fb_siz)`) and unioned across the draws into one image; `note_fb_texture` bounds the rect to its own columns and ends its row range at the rect's last row, and requires both axes to overlap. The column test runs only when `stride % pitch == 0` and the rect does not wrap the image row; otherwise the row-only answer stands, which over-reports rather than under-reports. The `[GFX-FBTEX]` line prints both ranges and keys its dedup by the rect origin. `test_draw`'s bit-5 positive fill is narrowed to columns 0..63 of its 320x240 image, and new debug flag bit 6 (64) adds the negative control: a second load from texels 160..191, in the drawn rows and outside the drawn columns. **Verified.** With only the column comparison disabled, `node debug/probes/testdraw.cjs --flags 96` reports `fbtex hits=2 lines=2`, the extra line `cols=[320,384) drawn cols=[0,128)`; with it enabled the same command reports `fbtex hits=1 lines=1` and only `cols=[0,64) drawn cols=[0,128)`. `testdraw.cjs` gained `--expect-hits N`: `--flags 96 --expect-hits 1` and `--flags 0 --expect-hits 0` exit 0, and `--flags 32 --expect-fbtex` is unchanged at `hits=1 lines=1`. No renderer frame moves; `boot.cjs` still reports `RENDERED` (`nonBlack 25136 / colorful 19296`), and the detector still reports 0 on `0x04`/`0x0A` and on `0x10` (`tasks=52 hits=0`), the scene whose 133 range-only hits session 152 removed with the row test. **Correction.** Session 153 claimed `app/src/web_platform.cpp`'s header comment had been corrected to the native slot rule; the committed tree still read "slot 0 is the keyboard (always connected), slots 1-3 are gamepads", and it is rewritten now. Its other claims (`ogre_input_debug(int slot)`, `0xFFFF` for a disconnected slot, the lowest-free-slot fill and the slot-0 OR, `debug/probes/gamepad.cjs`) verify. See `docs/HANDOFF-2026-10-17-session154.md`.

- ✅ **A browser gamepad drives N64 controller 1, and no longer takes the keyboard away from it (session 153).** Open item 13: `app/web/web.js` gave each pad slot `(pad.index % 3) + 1`, so gamepads filled controllers 2-4 and controller 1 was keyboard-only - a pad was invisible in game, the same defect the native build fixed for issue #13 (`app/src/sdl_platform.cpp`: a pad fills the first free slot). **The fix.** `updateGamepads` assigns slots 0..3 in order (the first pad is controller 1) and ORs slot 0's keyboard and tap mask into the pad's buttons; a slot that loses its pad goes idle, and slot 0 goes back to the keyboard. The pass order mattered as much as the assignment: `rafLoop` pushed the keyboard and then the pads, so a pad in slot 0 overwrote the keyboard every frame and the keyboard went dead; the pad pass now runs after the keyboard pass. `ogre_input_debug` takes a slot and replies `0xFFFF` for a slot with no connected device, so a test tells an idle keyboard from an empty slot, and `app/src/web_platform.cpp`'s header comment (which documented slots 1-3) is corrected. **Verified** with the new `debug/probes/gamepad.cjs`, which installs a synthetic `standard`-mapping pad through `page.addInitScript` (a headless browser has no real pad) and reads `Module._ogre_input_debug(slot)`: `slots=[0x8000, 0xFFFF, 0xFFFF, 0xFFFF]` (the pad's `A` in slot 0), `[web:input] gamepad connected (pad 0 -> slot 0, slot 1 free, slot 2 free, slot 3 free)`, and `slot0=0xC000` while the probe holds the keyboard's `B` beside the pad's `A` (`0x8000` after release). Before the fix the same probe printed `gamepad connected (slots 1-3)` with slot 0 idle. No renderer path moved: `boot.cjs` -> `RENDERED`, `testdraw.cjs --flags 32 --expect-fbtex` -> `fbtex hits=1 lines=1`, and a 45 s `start,a,...` tap route still reaches scene `0x09`. A physical pad on a real page is the one thing still unverified. See `docs/HANDOFF-2026-10-16-session153.md`.

- ✅ **The browser reports a framebuffer-as-texture load instead of silently decoding stale RDRAM, and no reachable screen needs one (session 152).** Session 151's open item 1 was that a `G_LOADTILE`/`G_LOADBLOCK` is decoded out of RDRAM on the gfx thread at record time (`decode_texture_rect`) while a colour image the same display list rendered into is still in a GL framebuffer object, written back to RDRAM only by the browser main thread at present or at a `sync_framebuffers` request, so such a load reads bytes the draw never wrote. **The instrument.** A recorded draw's `DrawCmd` already carries the image it renders into (`fb_address`, `fb_row_bytes`, `fb_height_px`, `fb_siz`), so `fb_written_` holds one entry per colour image a recorded draw has written since the last writeback together with the guest rows those draws covered, and the entries are dropped when the main thread publishes a writeback through the new `fb_writeback_epoch_` counter. `note_fb_texture` reports `[GFX-FBTEX]` (once per distinct texture/image pair, capped at 24) and `ogre_gfx_stats()` surfaces `fbtex: hits=N` in `#gfxstats`. Nothing is drawn differently. **The row test is required.** RT64 and the browser size a colour image to `max(viewport, VI height, scissor bottom + 1)`, so a 640x480 mode's target is 640x481 rows while the game writes rows 0..479 (`[GFX-FBSYNC] wrote back 0x1BA780 640x481 siz=2 pitch=1280 guestRows=[0,479]`) and the heap immediately past the image holds the game's own textures; reading RDRAM there is correct. The range-only version of the check reported 142 such loads (scene `0x0A` 9, `0x10` 133), every one at byte offset 614488 (row 480.07) or 614456 (row 480.04); the row test reports `rows=[480,480] drawn rows=[0,479]` and does not fire. **The positive control.** `test_draw` gained a debug-flag-bit-5 branch (`ogre_gfx_debug_flags(32)`) that renders into a colour image at hidden RDRAM `0x00C02000` (320x240 RGBA16), fills it red, and loads a tile back out of the same address, so a zero-hit sweep is told apart from a check that cannot fire: `debug/probes/testdraw.cjs --flags 32 --expect-fbtex` prints `fbtex hits=1 lines=1`, and the same probe with `--flags 0` prints `hits=0 lines=0`. **The result.** With the control firing, all 25 forced scene ids (`debug/probes/fbtex.cjs --secs 18`, one boot each; 19 of them submitted a real display list) and three natural routes (120 s each: the unforced boot to the title, `OGRE_SCENE=new-game&OGRE_STEP=2` to the cathedral, and a `start,a,...` tap route; 351/443/471 display lists) report 0 hits. Framebuffer-as-texture stays unimplemented until a screen reports itself. **Also found:** session 151's native cathedral route does not reproduce in this build. With `OGRE_MOD_TEST=1` (needed for a scripted native run) the run logs `forcing scene 0x02`, `seeded step 2 while scene 0x02 runs`, then `t=2700ms id=0x0009`, `t=5156ms id=0x000A`, `t=9231ms id=0x0004` - the forced New Game loader is overridden by the boot's own scene sequence - while the same query in the browser reaches `0x0D`, so no browser-vs-native cathedral frame comparison was made. New kept probe `debug/probes/fbtex.cjs`; `docs/guides/web-probes.md` has "The framebuffer-as-texture check". See `docs/HANDOFF-2026-10-16-session152.md`.

- ✅ **The browser's scripted tap schedule now matches the native one, and framebuffer-as-texture is the next renderer item (session 151).** Back to the browser's own items after session 150's RT64 fill clip. `app/web/web.js` cycled `OGRE_TAP_BUTTON` forever (`names[slot % names.length]`), so a route could not stop tapping mid-run and the browser has none of the native's scene-keyed knobs (`OGRE_TAP_SCENE`, `OGRE_TAP_NOT_SCENE`, `OGRE_TAP_SCENE_BUTTON`); the native advance is `slot = tap < count ? tap : count - 1` plus `OGRE_TAP_MAX` (`app/src/sdl_platform.cpp`, "the last slot sticks"). `web.js` now does both, so `OGRE_TAP_BUTTON="start,start,none"` means the same on both platforms. **Verified**: a 25 s browser run with `OGRE_TAP_MS=1000&OGRE_TAP_BUTTON=start,none&OGRE_TAP_MAX=3` logs `[web:input] tap schedule done after 3 presses` and still renders; the guide's `OGRE_TAP_BUTTON` row (which said "cycled one per tap") is corrected. Route facts from the attempt to reach the cathedral: the session-54 title route is racy in the browser (this run's forced title landed at t=8189 ms, after the first presses), while `OGRE_SCENE=new-game OGRE_STEP=2` reaches the cathedral on both sides (`id=0x0002` at 5.3 s, `id=0x000D` at 5.7 s) and waits for input there, so a cathedral comparison needs no taps. Nothing was captured or compared against the reference this session. **Next**: framebuffer-as-texture - the gfx thread decodes a `G_LOADTILE`/`G_LOADBLOCK` out of RDRAM at record time (`decode_texture_rect`, `app/src/web_renderer.cpp:3827`) while a colour image the same display list rendered into is still only in a GL framebuffer, written back on the main thread at present or at a `sync_framebuffers` request (`readback_all_framebuffers`), so such a load decodes stale bytes; the two candidate fixes are a gfx-to-main writeback handshake or deferring the decode to the flush. See `docs/HANDOFF-2026-10-16-session151.md`.

- ✅ **The RDP's scissor does clip a fill, so RT64 (not the browser) was painting the attract story's letterbox olive (session 150).** Settle session 149's open item 1. **The reference emulator.** RetroArch with the `parallel_n64` core (ParaLLEl-RDP) run to the attract story at frame 6600 shows the framebuffer's rows 0..16 and the bottom band as `(0,0,0)` - the second fill `0x00010001` -> `0x0001`, black - with the game's gradient rows from 18 and the last column left stale from the previous scene; n64brew's Set Scissor says the same. **The cause.** RT64 submitted a fill as `InstanceDrawCall::Type::FillRect` -> `clearColor(0,0,clearRect.color,...)` with `clearRect.rect = convertFixedRect(call.callDesc.rect,...)`, the rectangle the game asked for, and a clear cannot be scissored (the scissor only reached the writeback bookkeeping `fbPair.drawColorRect`), so the story's fourth fill (`0x4AC14AC1`, scissored to rows 18..193) painted the whole 320x240. **The fix.** `RDP::fillRect` (`tools/RT64/src/hle/rt64_rdp.cpp`) intersects the rectangle with `state->rdp->scissorRectStack[scissorStackSize - 1]` before `drawRect`, returns on an empty intersection and prints a bounded `[rdp] fillRect clipped` line under `OGRE_RDP_TRACE`; the intersection uses the scissor as stored (half-open at the right edge), which is what the reference emulator's last column shows. **Verified.** `cmake --build build-app -j` clean; the story run prints `fillRect clipped (0,0)-(1279,959) -> (0,72)-(1276,776)` and its target's bands are `(0,0,0)` where they were `(74,90,0)` (the scene rows keep the scene; row 192 identical); the developer confirmed the screen ("the attract screen has the correct bg color now! it is black instead of olive!!"); no regression on the publisher stills (`tt.120`..`tt.220` byte-identical to session 146's `/tmp/w146/nat/tt.140.bin`) or the title (`[2.29,2.49,3.10] / 81.4%`, session 146's numbers). **Two corrections.** Session 149's "the browser's fill scissor is one column narrow" is withdrawn as a browser-vs-reference difference (the reference leaves that column unwritten too, so the browser's half-open scissor agrees with it). Session 84's olive-band branch reading is corrected at instruction level: `0x4F00C308` (`0x801A1E10` in `bankA.elf`) is the data word of a `G_SETSCISSOR` pair guarded by `W & 0x100`, and `li a0,0x4AC1` (`0x801A1E80`) is a `beqz` delay slot that runs whenever bit `0x2000` is clear - not two arms of a colour choice - and with the scissor clipping the band colour is fill 2's black either way. The change lives in the `tools/RT64` submodule (already dirty with the port's renderer changes; not committed, rule 10) and `patches/rt64-ob64.patch` was regenerated with `make patch-fix`, so the hosted release path carries it; `make patch-check` PASSes. See `docs/HANDOFF-2026-10-16-session150.md`.

- ✅ **The browser decoded a fill colour as 32-bit RGBA whatever the colour image was, and the attract story's letterbox colour came out green (session 149).** Continue the web renderer (developer, session start); session 148's open item 2 was a screen the publisher still does not exercise. The attract story (`0x0B`, `OGRE_SCENE=story` on both sides) was picked. **The decode.** `draw_fill_rect` used the `G_SETFILLCOLOR` word's 32-bit RGBA8 decode for every target, but the RDP writes the fill colour in the colour image's format: a 16-bit image takes the low 16 bits as RGBA5551, a 32-bit image the whole word as RGBA8888. RT64 does exactly that (`ColorConverter::RGBA16::toRGBAF(fillColor & 0xFFFF)` when `fbPair.colorImage.siz != G_IM_SIZ_32b`, else `RGBA32::toRGBAF(fillColor)`). The story's letterbox fill is `0x4AC14AC1`: `(74,90,0)` as RGBA5551 and `(74,193,74)` as RGBA8, and the browser painted the green; `0x00010001` painted `(0,1,0)` where the 16-bit value is `(0,0,0)`. The fill colour is the only RDP colour register whose decode depends on the image, so `RenderState::fill_color` keeps the 32-bit decode for the canvas path and `fill_color_word` carries the register; `[GFX-RECT] fillrect` prints the effective colour, the word, the image size and the scissor, and `rect_logged_`'s cap is 8 because the fill that matters is the story's fourth. **Verified as pixels.** New kept diagnostic `debug/probes/fills.cjs` sets `ogre_gfx_debug_flags` bit 1 (`draw_tri` stops recording after three draws, `draw_fill_rect` keeps drawing) so the fills are visible; the 640x480 canvas reads before -> after: band rows `(0,1,0) -> (0,0,0)`, row 100 `(74,193,74) -> (74,90,0)`, gradient row 193 `(15,38,15) -> (15,18,0)`, and the final build's trace prints `fill=[0.29,0.35,0.00,1.00] word=0x4AC14AC1 siz=2 scissor=1(0,18,319,194)`. The story's four fills are ground truth from the native `OGRE_DL_DECODE=all` (`dl=115`, all `FILLRECT (0,0)-(319,239)`: `0xFFFCFFFC` full scissor, `0x00010001` full, `0xFFFCFFFC` and `0x4AC14AC1` scissored to rows 18..193). **The rest of that screen is mostly RT64's, and is left open.** RT64 renders a fill as a clear of the *unscissored* rect (`InstanceDrawCall::Type::FillRect` -> `clearColor`, `clearRect.rect = convertFixedRect(call.callDesc.rect)`, the scissor reaching only `fbPair.drawColorRect`), so the story's fourth fill paints the whole 320x240 and RT64's target bands are `(74,90,0)` where the browser has `(0,0,0)`; the bands are 27% of the frame (64 of 240 rows) and the largest term in the `meanAbs=[24.57,30.75,6.49]`. The scene rows animate between RT64's own captures (`s.160` sky `(10,1,27)` against `s.180` `(24,16,47)`), so they are not comparable across runs. Whether hardware clips a fill by the scissor decides which renderer is right, and it is a retail-behaviour claim (rule 8): n64brew's Set Scissor says it clips, which would also make session 84's olive-band branch reading a symptom (the band rows lie outside fill 4's scissor, so they hold fill 2's `0x0001` in either arm); the decisive evidence is a reference emulator's RDRAM at a band row (`0x0001` against `0x4AC1`). **Verified:** `make web` clean twice; `boot.cjs` `RENDERED nonBlack 24788 / colorful 15076`; `testdraw.cjs` `exported=true returned=1`; the story frame against RT64's targets `meanAbs=[24.57,30.75,6.49] exact=13.0%`. See `docs/HANDOFF-2026-10-16-session149.md`.

- ✅ **The publisher still sat two rows low in the browser because the canvas element's CSS box did not match its drawing buffer, and the browser resampled 640x476 into 640x480 (session 148).** Continue the web renderer (developer, session start); session 147's open item 1 was that still, and it named the 476 -> 480 CSS stretch and the unapplied `fldRegs` x/y scale and offset as the candidates. **The mechanism.** `app/web/index.html` pins `#game-canvas` at `width:640px; height:480px` and `present_framebuffer` sizes only the drawing buffer (`emscripten_set_canvas_element_size`), which for that VI mode is 640x476 (`[GFX-VI] VI scanout 640x476 origin=0x1BAC80`); the browser scales the drawing buffer to the CSS box and `image-rendering: pixelated` makes that a nearest-neighbour resample, so the frame on screen was the 476-row buffer stretched over 480 rows. **The measurement.** On session 147's own capture, canvas row `Y` equals RT64's target row `floor((Y + 0.5) * 476 / 480)` for every row: `meanAbs=[0.87,0.84,0.75]` per channel and 99.6% of channel samples identical, against `10.38` for the identity and `7.26` for the best constant shift; the crop offset is confirmed the same way (offsets 0 and 2 give `10.49` and `11.27`). The ATLUS strokes are `x 156..471, y 200..283` in the canvas against `156..471, 198..281` in RT64 - the same box, two rows lower, which is what the stretch does at that row. **The fix.** `resize_canvas` now sets the element's CSS box with `emscripten_set_element_css_size("#game-canvas", width * scale, height * scale)`, `css_scale_for(height) = max(1, min(2, 480 / height))`, so a 320x240 buffer keeps the 2x box index.html declared and the 640x476 mode is shown 1:1; `[GFX-PRESENT]` prints the box. **Verified**: `make web EM_CACHE=/Users/momo/dev/ogre/.emcache` clean; the forced publisher still on the fixed build (`canvas resized to 640x476 (css 640x476, x1)`, `wrote back 0x1BA780 640x481 siz=2 pitch=1280`, `[scene] t=2898ms id=0x000A`) matches RT64's target at `meanAbs=[0.87,0.84,0.75]`, 99.6% exact over 640x476, with the shift search minimising at exactly `(0,0)` = `0.82` and every other shift `>= 6.80`; session 147's capture read back through the same scale gives the identical numbers; the title is unchanged (`[2.30,2.50,3.12] exact=81.3%` against `t.460`, session 147's `[2.32,2.51,3.12] / 81.2%`); a fresh native capture of the same screen is byte-identical at `tt.140` to session 146's (`cmp`); `boot.cjs` `RENDERED nonBlack 25208 / colorful 18264`; `testdraw.cjs` `exported=true returned=1`. **Two session-147 numbers corrected:** the canvas bounding box it recorded (`157..472 x 201..284`) was a crop offset of one pixel right and one row low, and its "9836 = 4 x 2459 native" stroke count is 9836 in both frames at 640 wide (2459 is the native target subsampled by two). `tools/web_frame_cmp.py` gained `--canvas-size`, `--target-size`, `--buffer` (undo a non-integer CSS scale) and `--shift-search`. **The visible area is exact:** with `--inset 1` (638x474) the comparison is `meanAbs=[0.0,0.0,0.0] exact=100.0%` (27 differing channel samples of 907236: the four `border-radius` corner pixels and five at +-1/+-2); the whole-frame signed difference `-0.87/-0.84/-0.75` is the buffer's last column and last row, which the presenter never writes because the visible area is 639x475. **Left open:** those unwritten edges and rounded corners as a presentation detail; the two moving-cloud bands on the title; framebuffer-as-texture; the native's x4 against the browser's x1 saved game speed. See `docs/HANDOFF-2026-10-16-session148.md`.

- ✅ **The browser decoded every palette entry by the entry's own bit 15, so the publisher still's ATLUS strokes drew grey where RT64 paints blue (session 147).** Continue the web renderer (developer, session start); session 146's open item 1 was the still, and it read the native blue render as RT64's palette fault. **The decode selection.** `decode_texture_rect` (`app/src/web_renderer.cpp`) chose RGBA16 when a palette entry's bit 15 was set and IA16 when it was clear. The RDP chooses from the draw's 2-bit texture LUT mode, `OTHERMODE_H` bits 14-15 (`OtherMode::textLUT()`): `G_TT_RGBA16` (2) decodes every entry as 5/5/5/1, `G_TT_IA16` (3) as 8-bit intensity plus 8-bit alpha. RT64's `TextureDecoder.hlsli` switches on that mode and never on the entry. **The screen.** The still is a 640-wide CI8 image at `0x37C898` (`fmt=2 siz=1 stride=320 line40`, 12-row `G_TEXRECT` strips at `dsdx=dtdy=512`, `lut=2`), and the ATLUS strokes are index `0xE6`, whose entry is `0x3AE8` with bit 15 clear: `rgba16(0x3AE8)` = `(57,90,165)` against `ia16(0x3AE8)` = `(58,58,58,232)`, which is what the browser painted. The red T's entry `0xD80A` has bit 15 set, so it matched by accident and the fault looked like one wrong colour rather than a wrong decoder. **Two corrections to session 146.** The native stroke colour is `(57,90,165)` and not `(48,80,160)` (0 occurrences in `/tmp/w146/nat/tt.140.bin`), and RT64's blue render is correct, so its TMEM palette lookup was never at fault; session 146's `colour_mean` CI8 guard change stays, because it is what let the cream be read at all. **The fix.** `decode_texture_rect` takes `int lut_mode` in place of `bool palette_enabled` (mode 0 still reads a CI tile as I8, session 144), and the image cache key carries the whole mode (`tlut_generation_ * 4 + lut_mode`), because modes 2 and 3 decode one entry to two colours. `GFX-TEXTRACE` also gained the load's `tmem`, the palette entries a decode read and the four most common decoded colours, and `loadtlut` counts the palette so an all-zero load shows as `nonzero=0`. **Verified**: `make web` and `cmake --build build-app -j` clean; the browser canvas's colour histogram for the forced publisher still equals RT64's `tt.140` capture entry for entry (cream `(239,231,206)` 189172 px both, shadow `(222,214,181)` 37837 both, strokes `(57,90,165)` 9836 canvas px = 4 x 2459 native, red T `(222,0,41)` 1864 = 4 x 466, `(58,58,58)` 0); the title is unchanged (`meanAbs=[2.32,2.51,3.12] exact=81.2%` against session 146's `[2.29,2.48,3.10] / 81.4%` on the same target); `boot.cjs` `RENDERED`. **Left open:** the still sits about one row low in the browser (`meanAbs` 11.43 at `dy=0`, 6.40 at `dy=-1`, and the differing pixels are swaps of neighbouring still colours), which the 640x480 canvas around a `visible=639x475` VI and the unapplied `fldRegs` scale are the candidates for; the LOADTLUT destination is still keyed on the load tile's `palette` field where RT64 uses its `tmem`, which agrees for every traced load. New kept diagnostic `debug/probes/palette.cjs`. See `docs/HANDOFF-2026-10-16-session147.md`.

- ✅ **An N64 RGBA32 texel is four guest bytes R, G, B, A, and the browser decoded it one byte along, which was the whole title-and-story hue shift (session 145).** Continue the web renderer from session 144's open item 1, "the browser's title and story frames are hue-shifted against RT64's target for the same text". Session 144 concluded the decode was right and the mechanism was in the GL sampling or the blend. `decode_texture_rect`'s `IM_SIZ_32b` case read the texel's two big-endian halfwords as `(alpha,red)` and `(green,blue)` and assigned `rgba = (hi & 0xFF, lo >> 8, lo & 0xFF, hi >> 8)`, which is the source bytes G, B, A, R in the destination R, G, B, A. RT64 reads the same four bytes as R, G, B, A: `RDP::loadWord<RGBA32>` (`tools/RT64/src/hle/rt64_rdp.cpp:527`) puts guest bytes 0/1 in the lower half of TMEM and 2/3 in the upper, and `sampleTMEMRGBA32` (`tools/RT64/src/shaders/TextureDecoder.hlsli`) rebuilds `(R<<24)|(G<<16)|(B<<8)|A`; `tools/ogre_frame.py` decodes the same order from a dump. The title draws a 320x240 RGBA32 backdrop as 3-row `G_TEXRECT` strips (`img=0x80217508 fmt=0 siz=3 stride=1280`), so a brown-to-white sky decoded magenta-to-white; session 143 measured the same hue shift on the attract story's frames. **Verified by the trace against a dump from the same run:** on the fixed build the backdrop's rectangles report `rows 0..3 mean=56.8 opaque=0` and `rows 21..24 mean=104.6 opaque=319`, and the run's own `0x217508` decodes to exactly those under `(R,G,B,A)`, while `(G,B,A,R)` gives `25.5/0` and `87.9/430`, which is what the session-144 build reported. **And by the frame:** the browser canvas against RT64's `OGRE_CAPTURE_TARGET` of the same title frame is 5.1 mean absolute difference per channel against 40.7 before, with pixel `(10,101)` identical and session 144's row-101 pixel moving `(53,6,70) -> (112,91,21)` against RT64's `(118,98,31)` (`docs/proofs/web-title-rgba32-rgba-order-fix.png`, `-before.png`, `native-title-rgba32-reference.png`). Session 144's comparison was the browser's decode against the browser's own trace, and both agreed because the trace's `mean` is computed by the decode under test. New `tools/rt64_target.py` reads an `OGRE_CAPTURE_TARGET` `.bin` (`R16G16B16A16_UNORM`, 8 bytes/texel) into a PNG; `docs/guides/app-build.md` now records both it and that a scripted native run needs `OGRE_MOD_TEST=1` while `build-app/mods/` holds mods, or the start screen waits for a ROM and writes no capture. `boot.cjs` reports `nonBlack 25644 / colorful 18568` (session 144's numbers) and `testdraw.cjs` returns 1. Remaining: the residual 5.1/255 difference (the browser is slightly darker; no dither), the attract story was not captured, and session 141/142's items stand. See `docs/HANDOFF-2026-10-14-session145.md`.

- ✅ **The browser presenter stretched the VI's visible area over the whole canvas, which was the bulk of session 145's residual frame difference, and its scissor was one pixel wider than the RDP's (session 146).** Continue the web renderer (developer, session start); session 145's open item 1 was the 5.1/255 per-channel difference between the browser's canvas and RT64's `OGRE_CAPTURE_TARGET` for the same title frame. **The presenter.** `blit_to_canvas` built a full-canvas quad and interpolated the texture's `[0, u_max] x [v_min, 1]` across it, so session 143's overscan crop - the visible 319x239 of a 320x240 VI - was stretched back onto 320x240. Canvas pixel `X` sampled framebuffer texel `int(0.996875 * (X + 0.5))`, which is `X` for `X <= 160` and `X - 1` above it, and the same vertically past row 120. The measurement that identifies it is the *direction*: the session-145 build's canvas was closer to the target passed through that stretch (`2.30/2.50/3.11` mean abs per channel) than to the target itself (`4.66/5.05/5.54`), and the fixed build reverses it (`2.29/2.48/3.10` against `4.56/4.94/5.47`); exact channel samples went from 39.4% to 81.4%. That geometry is RT64's: `VIRenderer::getViewportAndScissor` builds `viViewRect` from `visibleFramebufferSize(vi)` anchored at `(0,0)` and maps it through `fromSDtoHD`/`fromHDtoWindow`, so with a window of the VI's own size the visible area is written 1:1 at the top-left and the last column and row are never written. `blit_to_canvas` now takes the visible fraction of the canvas and draws its quad over that rectangle. **The scissor.** `glScissor` used `sx1 - sx0 + 1`, while the RDP's scissor is half-open: RT64 turns it into a plume `RenderRect(left, top, right, bottom)` through `convertFixedRect` (which ceils the 1/4-pixel bounds) and plume's rect is exclusive at the right and bottom, so the game's `(0,0)-(319,239)` covers pixels 0..318 by 0..238. That is why RT64's own render target has a black last column and row on every title frame while the browser had drawn them; GL's scissor is half-open too, so the stored pixel bounds are used as they are. **Verified**: `make web` clean (twice, once per change); the fixed build's forced-title frames against the same native targets give `meanAbs=[2.29,2.48,3.1] exact=81.4%` against the session-145 build's `[4.66,5.05,5.54] / 39.4%`, `boot.cjs` `RENDERED`, `testdraw.cjs` `exported=true returned=1`, and the forced publishers still draws the ATLUS logo at `size=640x481 visible=639x475 canvas=640x476`. New `tools/web_frame_cmp.py` is the comparator between a `#game-canvas` screenshot and an `OGRE_CAPTURE_TARGET` `.bin` (it crops the element's 1 px border, undoes the 2x duplication and prints mean abs / mean signed / exact per channel). **Left open:** the remaining title difference is two moving-cloud bands (rows ~64..112 and ~195..238, browser 12-16 darker) and the developer states the port's clouds are not exact against retail yet, so the two runs' cloud phases are not separable there; the session also found that the **native** build renders the publisher still's ATLUS letters blue (`(48,80,160)` in its render target) where the browser draws them black (`(56,56,56)`), and the native's own `OGRE_TEX_TRACE_COLOUR` cannot decode the CI8 draw because `colour_mean` refuses `timg_siz != 2`. The native app's saved game speed is x4 and every native capture used `OGRE_SPEED=1` after the developer pointed it out. See `docs/HANDOFF-2026-10-16-session146.md`.

- ✅ **The black CI tiles are I8, not palette-indexed: a CI tile with texture LUT mode 0 samples intensity, and the browser painted it opaque black (session 144).** Continue the web renderer from session 143's open item 2 ("CI8 tiles decode pure black in display lists that load no palette"). The renderer decoded every `G_IM_FMT_CI` tile through its TLUT and wrote `{0,0,0,255}` when no `G_LOADTLUT` had filled one; the black rectangle over the title screen's prologue is exactly that, two unit sprites at `rect=22,127..94,155` and `22,155..94,175` with `img=0x1C13F8 fmt=2 siz=1 line=9 pal=0` and `oml=0x504240`. `OTHERMODE_H` bits 14-15 are the texture LUT mode, and RT64 treats LUT mode 0 as "not a palette at all": `rt64_framebuffer_manager.cpp` asserts that case into a tile reinterpretation and `FbReinterpretCS.hlsl`'s `ANY8toI8` makes each byte an intensity replicated into RGB and alpha. `decode_texture_rect` now takes the LUT mode (`palette_enabled`) and reads such a tile as I8; the browser trace's mean for the first sprite went from `0.0` to `72.2` with `opaque 2016 -> 597`, and the frame's silhouette matches RT64's (`docs/proofs/web-title-lut-mode0-i8-fix.png`). The palette also moved out of `RenderState` into renderer state, because RT64 clears `RDP::TMEM` in its constructor only and `RDP::reset()` leaves it, so the RDP's TMEM palette outlives a display list. The sprites do not need that part: they trace `tlut=no` and are read as I8. A CI image's cache key now carries the palette generation and the LUT mode. New `OGRE_TEX_TRACE` on the browser (`[GFX-TEXTRACE]`, bounded at 1500 draws) prints what each rectangle decodes, which is what made the comparison against RT64's `OGRE_TEX_TRACE` possible. Verified: `make web` and `make app` clean, `boot.cjs` `RENDERED nonBlack 25644 / colorful 18568`, `testdraw.cjs` `returned=1`, and a 250 s natural run that reaches the publishers and then reproduces session 141's pre-existing `divide by zero` at task 277 (`func_801989AC`), which session 143's run passed only because it took a different route. The title/story hue shift against RT64 stays open: the browser and RT64 decode the same texrect and the browser's decode is a plausible cloud, so the mechanism is in the sampling or the blend, not in the decode. See `docs/HANDOFF-2026-10-14-session144.md`.

- ✅ **The browser presented the whole framebuffer texture, so every frame was squashed into its top half (session 143).** Continue the browser renderer; session 142's open item 1 was the attract story (scene `0x0B`) "only partly drawn", with a noise field in one sample. **The presenter, not the scene.** `app/src/web_renderer.cpp`'s `blit_to_canvas` drew the GL framebuffer with UV `(0,0)..(1,1)` over the whole canvas, and that texture is the RDP colour image at the tallest height `ensure_framebuffer` has ever been asked for (RT64's `Framebuffer::maxHeight`). The publishers' 640x476 VI mode grows the game's own 320x240 images to `320x476`, while the title's VI mode is `320x240`, so the game's 240 rows were drawn into the top 240/476 = 50.4% of a 320x240 canvas and the texture's rows 240..476 - the *next* guest image - appeared below them as noise or black. That arithmetic is in session 142's own proof: `docs/proofs/web-attract-story-partial.png` has its first all-black row at screenshot row 243 of 482. **The fix** ports RT64's `VIRenderer::visibleFramebufferSize` (`tools/RT64/src/render/rt64_vi_renderer.cpp`) as `visible_framebuffer_size()`: the VI's `fbSize` (session 141's `vi_framebuffer_size`) minus the overscan crop `OGRE_OVERSCAN`, default 1, the knob the native presenter reads. `blit_to_canvas(tex, u_max, v_min)` now bounds the sample to `visible.width / texture.width` horizontally and, vertically, to the *high* end of `v` - `v_min = 1 - visible.height / texture.height` - because `upload_framebuffer` flips the image and texture `v = 1` is guest row 0; RT64's present shader maps `videoResolution / textureResolution` over the window for the same reason (`src/shaders/VideoInterfacePS.hlsl`). A first version cropped the low end of `v` and showed the rows past the colour image instead; it was replaced in the same session. `[GFX-PRESENT]` now prints `visible=WxH` beside `size=WxH`, so the two numbers that decide the mapping are one line (`size=320x476 siz=2 visible=319x239`, one present with `nonBlack=76800` = 320x240). Verified on a 250 s natural run (`/tmp/web143final`, captures labelled with the last `[scene] t=` line): the title (`0x04`) at wall 68-127 s with `tasks` 296→415, the attract story (`0x0B`) at 144-239 s; the title's "Ogre Battle 64" logo and the story's parchment page and its four-line text page all fill the frame (`docs/proofs/web-title-640-after-texrect-fix.png` replaces session 142's file of that name, which was the squashed frame; `docs/proofs/web-title-squashed-present-before.png` and `docs/proofs/web-attract-story-full-frame.png` are new), and RT64's own `OGRE_CAPTURE_TARGET` of the same title screen (`t.400`, 320x240) has the same content over the full 240 rows. The 640x476 mode is unchanged (`/tmp/map-probe.cjs` with the developer's battery: `nonBlack 307200`, `visible=639x475` of a 640x481 target, against session 141's 307200/247857). `debug/probes/boot.cjs` still renders (`nonBlack 25644 / colorful 16816` against sessions 138-142's 24840-25496) and `testdraw.cjs` returns 1. A palette-persistence experiment (the RDP's TMEM palette outlives a display list, and `send_dl` builds a fresh `RenderState` per task) was added, A/B'd byte-identical on the publishers' 24 decoded CI8 tiles and reverted (`grep -c last_tlut_ app/src/web_renderer.cpp` = 0). **Open:** the browser's title and story frames are hue-shifted against RT64's target for the same text (cloud band `(128,86,84)` against `(156,134,77)`, sky matching `(59,28,0)` against `(61,29,0)`), and CI8 tiles decode black in display lists that load no palette (13820 display lists, 1359 CI8 `G_SETTIMG`s and 562 `G_LOADTLUT`s in the publishers run). See `docs/HANDOFF-2026-10-14-session143.md`.


- ✅ **The browser build's title-screen fault was the framebuffer writeback running one row past the guest's colour image, and the `G_TEXRECT` corner was the reason (session 142).** Continue the browser renderer from session 141's open item 1 (the `divide by zero` trap at the hi-res title screen) and item 4 (the corrected S/T step had no post-fix frame). **The corner was inclusive.** `app/src/web_renderer.cpp`'s `draw_texrect` built its quad at `ndc_x(xh + 1)`/`ndc_y(yh + 1)` and spanned `w = xh - xl + 1`, `h = yh - yl + 1` of S/T, while the RDP covers `[ul, lr)` for a texrect: `RDP::drawRect` takes the 1/4-pixel corners without snapping them and the `lrx |= 3; lry |= 3` snap belongs to `RDP::fillRect` (`tools/RT64/src/hle/rt64_rdp.cpp:1239` against `:1381`; RT64's own `OGRE_RDP_TRACE=1` prints `fillRect ulx=0 uly=0 lrx=1280 lry=960` for the 320x240 clear, i.e. 320 and 240 as exclusive bounds). Every TEXRECT was therefore one pixel wider and one row taller than the hardware's. The title screen's backdrop is forty 640x12 strips over one 640x480 colour image, so the strip that ends at row 480 (the last of the forty) drew row 480; `readback_framebuffer` copies every row a draw touched, so the writeback wrote 1280 bytes at `image_end` - the next heap block's header. The game's allocator `func_800712C4` (`lw v0,-8(s1)`, `sltiu`, `jal 0x80071950`) read image pixels as a block size and called `func_80071950`, whose body trapped: the browser's page error was `memory access out of bounds` at `func_80071950` <- `func_800712C4` <- `func_801990A8` <- `func_801A3CA0` <- `func_80177E94` <- `func_80072398` <- `func_8008AFE0` (the frame pump), with the task counter frozen at 290 and the presenter still drawing the last framebuffer. Session 141's `divide by zero` in `func_801989AC` is the same corruption landing on a descriptor's width field. The RDRAM dumps prove it: the bytes 32 below the second and third 640x480 colour images held image rows (`ef33ef33`, `deaddead`) where RT64 holds zeros or structured values at the same addresses in the same scene. **Fixed**: the corner is exclusive (`xh <= xl || yh <= yl` is empty, `w`/`h` are the differences, the quad uses `ndc_x(xh)`/`ndc_y(yh)`, and the S/T span follows the same count, which is also why the strips no longer paint a row into the next one). `draw_fill_rect` keeps its `+1` because a fill rect's raw corner is `lry = y<<2 + 3`, so `y + 1` is what covers the last row. **The browser build got the per-frame diagnostics it never had**: `main_web.cpp` has no `update_gfx` callback, so `ogre_gfx_flush()` now calls `ogre::poll_scene()` and `ogre::ram_key_tick()`, which makes `OGRE_SCENE`, `OGRE_SCENE_LOG`, `OGRE_SCENE_TRACE` and `OGRE_RAM_KEY_AT_MS` work in the browser, and new `debug/probes/rdram.cjs` reads the dump out of `Module.FS` so it is analysed with `tools/rdram.py` like a native image. New bounded diagnostic `[GFX-EDGE]` (the first 24 draws that reach a render target's outermost row). Measured after the fix: no page error and 844 display lists in 300 s, with `[scene]` showing the publishers (`0x0A`), the title (`0x04`, t=51 s) and the attract story (`0x0B`, t=119 s); the final probe-free build reached `0x04` at t=95 s and `0x0B` at t=163 s with no page error; `docs/proofs/web-title-640-after-texrect-fix.png` is the 640x476 title screen, which closes session 141's item 4; `debug/probes/testdraw.cjs` still renders and `tools/runlog.py --check` PASSes on the native rebuild. Open: the attract story is only partly drawn (`docs/proofs/web-attract-story-partial.png`), framebuffer-as-texture, and the VI view rectangle. See `docs/HANDOFF-2026-10-14-session142.md`.

- ✅ **The browser renderer is sized by the VI, its `G_TEXRECT` UV step is corrected, and the hi-res title screen turns out to be a pre-existing wall (session 141).** Continue the browser port, starting from session 140's open item 3 (session 139's three renderer items). **(1) The render target and the canvas follow the VI's scanout size.** The target's height was the RSP viewport's, so a display list that never set one fell back to the canvas and the world map's 640-wide colour images were created `640x240`; it is now `max(viewport, VI height, scissor bottom + 1)`, which is RT64's `framebufferHeightForDisplay(drawnBottom, viHeight)`, and `ensure_framebuffer` keeps the tallest height while the width and `siz` hold, the way `FramebufferManager::get` does. The scanout size itself is a port of RT64's `VI::fbSize` over `ultramodern::renderer::get_vi_regs()`: `size.x = width`, halved when the serrate bit is set and the horizontal region is narrow, and `size.y = (vEnd - vStart) / (2 * yScale * size.x / width)` rounded up two rows and down to a multiple of four. A Python reimplementation of that arithmetic reproduces RT64's own `fbSizeX`/`fbSizeY` on every one of a native run's 93 `[vi]` lines, `320x240` and `640x476` included. **Only a visible mode updates it.** OB64 blanks the VI across the mode switch and `events.cpp` then reports `hStart = 0`; `fbSize` alone reads that state - the serrate bit already belongs to the next mode - as `320x952`, which is exactly what the first version of this change produced (`[GFX-FB] framebuffer 0x2E6840: 640x952`). RT64 pushes to `viHistory` only when `VI::visible()` (`status.type != BLANK && hRegion.hStart > 0`), and the port stores the last visible size in one atomic the same way. The NDC denominator is the render target's size (the colour image's own `G_SETCIMG` width, else the viewport, else the VI size), because RDP screen coordinates are framebuffer pixels; with it the 640-wide targets no longer clamp at x=319. The canvas element is resized on a mode switch (`emscripten_set_canvas_element_size` from the main thread's present path), so the 640x476 mode presents 1:1 instead of through a 320x240 drawing buffer. Measured in the browser on the developer's battery save: `[GFX-VI] VI scanout 320x240` then `640x476`, `[GFX-FB] 0x2E6840/0x1BA780/0x250800: 640x481`, `[GFX-PRESENT] canvas resized to 640x476`, and the earlier `320x952` target and 320x952 canvas are gone. **(2) The per-pixel `G_TEXRECT` S/T step was 32x too large.** RT64's `RDP::drawRect` computes `lrs = ((uls << 7) + dsdx * (rectWidth << 2)) >> 7` and then `u2 = lrs / 32`, so `u2 - u1 = dsdx * rectWidth / 1024`; `app/src/web_renderer.cpp` divided `dsdx`/`dtdy` by 32 and multiplied by the rectangle's width. The title screen exposes it because its backdrop is forty 640x12 texrects over one 320x240 CI8 image (`addr=0x38F6C8 stride=320 line=40 dsdx=dtdy=512`, `t0` stepping 192 per strip): correct, each strip samples its own six source rows (512 * 12 / 1024 = 6) and the 2x upscale (512 * 640 / 1024 = 320 texels over 640 pixels); with the old constant each strip repeated those six rows about 35 times, which is the banding in `docs/proofs/web-title-640-before-uv-fix.png`, captured this session with `debug/out`-style screenshots of the canvas. Nothing else the browser had rendered used the path: the intro and cutscenes draw triangles and the S2DEX2 object rectangle takes its UVs from the sprite's image size. **(3) `K4`/`K5` are combiner colour inputs** (`convertK[4] / 255` and `[5] / 255`, RT64 `RasterPS.hlsl`) instead of returning 0; the raw 9-bit fields were already parsed, so this is two uniforms and two shader cases. **The wall this session found: the browser build faults the first time the game renders its hi-res title screen.** Both this build and the untouched session-140 `dist-web` package freeze at exactly task 277 with a black canvas and the same wasm stack (`divide by zero` at `func_801989AC` <- `func_80198ECC` <- `func_801A29B8` <- `func_801A40EC` <- `func_80177EC0` <- `func_80072398` <- `func_8008AFE0`, the frame pump), so it is not a regression from this session's changes; the pre-change package is the control that proves it. `func_801989AC` is the delay slot of unit E's `bne` at `0x801989A8` (`func_ovlE_801988C8`), and `tools/guestmap.py` gives that RAM to unit E with the main ELF's `func_801989AC` sharing it, so the wasm name says the main unit's body ran there - the session-45 class of question, and the next lead. New bounded diagnostics kept: `[GFX-VI]` (each distinct visible scanout size, with the six register words) and `[GFX-WIDE]` (the first 64 draws of the first task that targets a 640-wide colour image, with geometry, fill colour and tile source). See `docs/HANDOFF-2026-10-14-session141.md`, `PLAN.md` open work 8 and `docs/DECISIONS.md`.

- ✅ **The browser build ships as a static package, and a vendored service worker supplies the cross-origin-isolation headers a static host cannot send (session 140).** Developer request: continue the web build, with the intention of a static build servable from GitHub Pages. The pthread build needs `SharedArrayBuffer`, which browsers expose only on a cross-origin-isolated page, and GitHub Pages cannot set the two response headers that provide it, so a `dist-web/` served plainly could not run the game at all. **`app/web/coi-serviceworker.js`** is coi-serviceworker v0.1.7 (MIT) vendored verbatim under a header comment (a diff against upstream proves the body is byte-identical): it adds `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy` to every response it serves and reloads the page once, and it is a no-op where the server already sends them, which is why `debug/server.py` still installs no service worker. **`make web-dist`** runs `make web` and writes one self-contained directory (page, `web.js`, `audio-worklet.js`, `coi-serviceworker.js`, the Emscripten glue and wasm, a generated `ogre-web-config.js` with `window.OGRE_WASM_DIR = "./"`, and `.nojekyll`), failing if any `ogrebattle64.*` artifact the build produced is missing from the package. **`app/web/ogre-web-config.js`** holds the repository layout's `"../../build-wasm/"`, so one page serves both layouts and the build patches no file. **`.github/workflows/pages.yml`** builds the package and deploys `dist-web/` with `upload-pages-artifact` + `deploy-pages`, guarded by `vars.OGRE_DATA_REPO` and needing the repository setting Settings → Pages → Source = GitHub Actions, which the session made (`"build_type": "workflow"`; `https://lfarroco.github.io/ogre-battle-64-recomp/`), so the next push to `main` publishes. New probe **`debug/probes/isolate.cjs`**. Measured: on a plain `python3 -m http.server --directory dist-web` (no COOP/COEP) the page is `isolated` at 811 ms with the service worker controlling and the glue reaching `WASM runtime initialized`, and `boot.cjs` reports `RENDERED` at `nonBlack 239425 / colorful 20076` with `sawRealDl=true`, the same `nonBlack` count as the same build behind `debug/server.py` (`239425 / 19688`, whose 388 differing colourful pixels are the title's fade advancing between the runs) (the session-139 baseline for that probe was `24840 / 15076`, sampled on the frame the first real display list drew). A GitHub-Pages-like subpath (`/ogre-battle-64-recomp/`) isolates at 650 ms and instantiates the wasm, and a server that gzip-encodes `.html`/`.js`/`.wasm` does too, which rules out the `Content-Encoding` mismatch a header-copying worker can introduce. **The session's second finding is that the glue publishes `Module._ogre_*` after `onRuntimeInitialized` fires** — polled from that log line, the first `function` reading is 509 ms on the local package and 2015 ms on the CI-built deployed one, while `moduleReady()` (FS + `HEAPU8`) is true at 0 ms — so a ROM handed to the page inside that window makes `Module.ccall("ogre_start_boot", …)` throw `TypeError: func is not a function` and the game never starts; the page now waits with `whenWasmExportsReady()`, and `debug/lib/harness.cjs`'s `gotoPage` waits for the same export. The package contains no ROM data; the guest picks their own dump, which never leaves the machine. **Deployed and verified:** the Pages workflow ran green (runs `38576916888` and `38576918066`, plus `WebAssembly` `38576917934`), and `boot.cjs` against `https://lfarroco.github.io/ogre-battle-64-recomp/` reports `sawRealDl=true` and `nonBlack 239425 / colorful 20076` — the same `nonBlack` count as the local package (20076) and `debug/server.py` (19688), so all three servers draw the same frame. See `docs/HANDOFF-2026-10-14-session140.md`, `docs/WEB-PORT-DEPLOYMENT.md` and `docs/DECISIONS.md`.

- ✅ **The browser renderer writes the RDP's framebuffers into RDRAM, and the assembled New Game backdrop appears (session 139).** Continue the browser port; session 138's open item 1 was the last of the three named renderer gaps. `G_SETCIMG` was a `break` and every draw went to the canvas, so the game's CPU readback of its own framebuffer read stale RDRAM. Now `app/src/web_renderer.cpp` decodes `G_SETCIMG` the way RT64's `GBI_RDP::setColorImage` does, keeps the colour image across display lists, gives each guest address a GL framebuffer object (created lazily, uploaded from RDRAM first), binds it per draw, records the drawn window-row band, writes that band back in the RDP's own 16-bit layout (`V & 0xFF` at `off ^ 2`, `V >> 8` at `off ^ 2 + 1`, which is what `recomp.h`'s `MEM_H` reads, and the alpha bit is RT64 `Float4ToRGBA16`'s coverage test), and presents the target the VI scans out with the COPY combiner. **The readback is a handshake**, because the game's CPU thread and the browser main thread are different threads and a browser main thread may not block on a pthread mutex: `ogre_sync_framebuffers()` bumps `sync_request_` and waits, bounded at 1000 ms, and `ogre_gfx_flush()` renders the queue and writes back before storing `sync_done_`; a fast path skips the wait when nothing is dirty. **The VI origin is not the colour-image address**: the standard NTSC `OSViMode`'s `fldRegs[0].origin` is `0x280`, so the present resolves the scanout buffer by containment (`0x04B680` is inside the framebuffer at `0x04B400`), which is RT64's `findMostRecentContaining` rule; before that every present missed its framebuffer. **The NDC denominator is now the screen size the viewport defines** (`2 * vscale/4`, which is 320x240 for most of the game and 640x480 for the world map, whose colour images are `SETCIMG width=640` with a 640x480 scissor), and a render target's width comes from the colour image. Measured: at the moment the game's njpeg readback copies, RDRAM holds the whole rendered image (`sync 1: waited=343ms ramNonBlack: 0x000400=75170 0x025C00=76800 0x04B400=75263`), and the cathedral backdrop renders in the browser (`docs/proofs/web-newgame-cathedral-assembled.png`, 304073/307200 non-black, 218964 colourful) where session 138's same probe produced a 32-pixel black frame (`web-newgame-cathedral-covered.png`). Title regression unchanged (`boot.cjs`: 24840 non-black, 15076 colourful, against 24840/15108). New bounded diagnostics `[GFX-CIMG]`, `[GFX-FB]`, `[GFX-FBREAD]`, `[GFX-FBSYNC]`, `[GFX-PRESENT]`. Open: the target's height and the canvas size still follow what the viewport gives a display list rather than the VI mode; framebuffer-as-texture; `K4`/`K5`. See `docs/HANDOFF-2026-10-14-session139.md`.

- ✅ **GAME SPEED offers 1, 2, 4 and 8, `=`/`-` and the pad's R3 change it, and a badge draws the multiplier in the game window's lower-right corner (session 137).** Developer request: *"add the 8x speed (current max is 4x); add a 'speed up' key; add a 'seed down' key; when the speed changes, display '2x', '4x', etc in the lower right corner of the window"*, then *"allow toggling the speed while using a controller"*; "seed down" is **speed down**. Session 93 had dropped 6 and 8 because the game's cursor aiming is fast at those speeds; 8 is back and 6 stays out, and the row, the keys and the pad button now share one table (`kGameSpeeds = {1,2,4,8}`). `step_game_speed(direction)` picks the next offered value numerically, so `OGRE_SPEED=6` steps to 8 up and 4 down, and it stops at the ends (no wrap); `cycle_game_speed()` is the pad's one-button walk of the same table and wraps 8 → 1, joining at the first offered value above a value the table does not hold (6 → 8, 64 → 1). The keys are `=`/keypad `+` up and `-`/keypad `-` down, handled from the SDL event in `pump_sdl_events`; the pad's **R3** (the right stick's click) cycles it from `SDL_CONTROLLERBUTTONDOWN`. R3 is what the default pad map leaves free (`Z` is L3, `BACK` opens the panel), and a player who binds R3 to a button still gets that button because the handler only observes the event. Both are ignored while the `ESC` panel is open (the panel owns the pad and the keyboard and has its own row) and before the game window exists. The badge (`app/src/speed_badge.cpp`, new) samples `game_speed()` per frame, creates its window on the first change, and draws `2x`…`8x` (and `1x`) for 2000 ms at the game window's lower-right corner, 20 px in, at 0.85 window opacity with the panel's bitmap font, re-positioning every drawn frame; it is a second always-on-top SDL window because RT64 owns the game's swap chain and has no UI pass, the technique the Esc overlay uses. `SDL_ShowWindow` makes it the key window on macOS, so the game window is re-raised immediately after each show and the key handler accepts the event from any window. Two limits the developer accepted: no badge in fullscreen, and the badge hides itself while the pointer is over it (SDL2 has no click-through hit test). Verified on `build-dist/ogrebattle64`: `OGRE_SPEED_KEYS="up,up,up,up"` logs `x2 x4 x8` with no fourth change, the badge at `(1087,681) 56x47` in a game window at `(203,28) 960x720` (three PPM captures, the last reading `8x`), a `down`-only run from a file-persisted 8 reaches `4` then `2` then `1` and stops (the extra presses change nothing), `OGRE_SPEED_PAD="r3,r3,r3,r3,r3"` logs `x2 x4 x8 x1 x2` (the wrap), `OGRE_SPEED=6` with two R3 presses logs `x8` then `x1`, two R3 presses with the panel open change nothing, the keys' run draws 1270 display lists in 14 s against 260 at 1x and 1630 at 8x, `OGRE_OVERLAY_KEYS="=,=,="` changes nothing while the panel is open, `Right,Right,Right` on the row writes `game_speed = 8`, and a whole-screen capture shows the badge over the game's own picture. See `docs/HANDOFF-2026-10-13-session137.md`.

- ✅ **The browser renderer draws the New Game njpeg backdrop: S2DEX2 and YUV16 are in the WebGL2 prototype, and reaching a browser scene no longer needs a throwaway script (session 138).** Developer request: *"this project has a web renderer, but it is still in its early stages. let's continue working on it!"* Three repairs and two mechanisms. (1) The wasm build was broken again by session 109's class: `app/src/ram_key.cpp` included the native-only `sdl_platform.hpp` for `console::key_down`, so `cmake --build build-wasm` failed with `#error "To use the emscripten port of SDL use -sUSE_SDL or -sUSE_SDL=2"`; the include and the key read are now `#ifndef __EMSCRIPTEN__`, and the browser keeps the timed dump (`OGRE_RAM_KEY_AT_MS`). (2) The GBI of a task is now identified by XXH3-64 over its ucode text, the way RT64's `GBIManager::getGBIForUCode` does. The digest covers the whole text and the OSTask's `ucode_size` is `0x1000` (one IMEM load), so the running digest must be compared at the database's own lengths; the first version hashed `ucode_size` bytes and matched nothing (`0x421A5D3DBF97F6CC` is F3DEX2's first 4 KiB, not its `0x1390`-byte digest `0xCF55FAE288BFE48D`). In an S2DEX2 list `0xDC`/`0xDA` are then decoded as `G_OBJ_MOVEMEM`/`G_OBJ_RECTANGLE_R` before the F3DEX2 switch, so the game's per-macroblock `SETTIMG/SETTILE/RDPLOADSYNC/LOADTILE/OBJ_MOVEMEM/OBJ_RECTANGLE_R` sequence draws 16x16 rectangles on the game's own grid with the njpeg source stepping `0x300` per macroblock, exactly as session 51 recorded. (3) `G_SETCONVERT` is parsed and `decode_texture_rect` gained the YUV16 branch: the guest word is `[U, Y(even), V, Y(odd)]` byte-reversed in RDRAM, one word per two texels, converted with the sign-extended `2*K+1` matrix (`R = Y + K0*V'`, `G = Y + K1*U' + K2*V'`, `B = Y + K3*U'`, NTSC JPEG fallback), mirroring RT64's `loadYUVTileToTMEM` + `sampleTMEMYUV16` on the source instead of on TMEM. Result: `docs/proofs/web-newgame-cathedral-njpeg.png` (307200/307200 non-black, 222378 colourful), one quadrant per njpeg pass because each pass draws at its own framebuffer coordinates. (4) `OGRE_*` query parameters are published into Emscripten's module-scope `ENV` before the runtime initialises its environment, so every `getenv` knob in shared code works in the browser (`OGRE_SCENE`, `OGRE_STEP`, `OGRE_S2D_TRACE`, `OGRE_TRACE_DL`, ...), and `OGRE_TAP_MS`/`OGRE_TAP_BUTTON` drive controller 0 from `web.js` with the native vocabulary and press length; the New Game opening is `?OGRE_TAP_MS=1500&OGRE_TAP_BUTTON=start,a,start,a,start,a,a,a,a,a,a,a,a`. (5) `make web` builds the target locally and `.github/workflows/wasm.yml` compiles it in CI, which is session 109's open item 2 and the reason the target rotted twice. The remaining renderer gap is RDRAM framebuffer indirection: the game assembles its 2x2 backdrop from CPU copies **out of the RDP framebuffer in RDRAM**, which the prototype never writes, so the dialogue scene draws an empty image over the backdrop (`docs/proofs/web-newgame-cathedral-covered.png`); the handoff's §5 has the design. New probe `debug/probes/atmark.cjs` captures a canvas series around a log marker. See `docs/HANDOFF-2026-10-14-session138.md`.

- ✅ **The black-part fix is generalised: a framebuffer tile copy is served only when the source framebuffer was written in the workload being processed (session 136).** Developer report: session 134's fix removed the black head in the cutscene it was found in, but a second cutscene (a town street reached from `pre_ogre.bin`) still showed it, and the correct sprite flashed for a single frame while the black one lasted seconds; is the fix scene-specific. It is not, and it was incomplete. In that cutscene the shipped write-rect test never fires (`[fbcopy-outside]` = 0) while 23 `[fbcopy]` lines serve 16-32 x 1-3 row unit-sprite tiles from one framebuffer at `0x32AF40` (`bytes=153600`, 320x240 RGBA16), and one `[fbcopy-stale]` is the single correct frame. The fault frame's display list has one `SETCIMG` and it is `0x400`, so `0x32AF40` is a leftover record from an earlier scene whose stale `lastWriteRect` `0,0..319,239` covers the tiles; the addresses it covers hold the game's CPU-built sprite scratch (7 of the frame's 264 texture loads read inside `0x32AF40..0x350740`, with `36 34 00 02 w h` block headers at `0x32C1F0`, `0x32CC40`, `0x32D850`, `0x32DF80`, `0x32E350`). The frame's head is the same signature as session 134's: 162 of 900 pixels in the head's box are exactly `0x0001`, the frame's only `G_SETENVCOLOR` (`0x000001`), i.e. `TEXEL1` sampled as zero and the combiner painted ENV. `OGRE_FB_GPU_COPY=0` on the same route renders the head correctly, which is the A/B. The fix stamps `Framebuffer::lastWriteWorkload` in `State::submitFramebufferPair` from `FramebufferManager::getUsedTimestamp()` and requires it to equal the current `usedTimestamp` in `checkRegionsTMEM` alongside the write-rect test, printing `[fbcopy-nowrite]`; the check is per load because a `RegionTMEM` stamped in an earlier workload survives in `activeRegionsTMEM`. The rule is narrower than the A/B and keeps the copy for a framebuffer rendered in the workload being processed, which `checkFramebufferPair()` running before `state->loadDrawState()` guarantees for a pair's own framebuffers. Verified: the fixed build gives 14 `[fbcopy-nowrite]` (`fbWorkload=9342` against `workload=9583..9660`), 0 `[fbcopy]`, and the developer confirms the head renders; the scene-14 regression run still refuses its head tile (`[fbcopy-outside] fb=0x2E6840 fmt=5 tile=468,65..484,67 writeRect=null`), refuses no legitimate copy, and matches the session-134 verified dump (identical unit-crop colour histogram; `y=120 x=110..119` unchanged). `make patch-check` PASS. See `docs/HANDOFF-2026-10-12-session136.md` and `docs/notes-black-sprite.md` §11.

- ✅ **The Windows release build's four errors are one POSIX call: `app/src/renderer.cpp` used `mkdir(path, 0777)` (session 135).** Run `38188914911`'s "Build Windows (x86-64)" job failed with `no member named 'mkdir' in the global namespace` at lines 586/587 and `use of undeclared identifier 'mkdir'` at 1587/1588, all four added by `b941040` (session 130) for `OGRE_TEX_TRACE_DUMP`. The job builds with clang-cl and the MSVC/UCRT headers, where two-argument `mkdir` has no declaration; the four sites now call a local `ensure_dir` that uses `std::filesystem::create_directories`, which the rest of the app already uses. `<sys/stat.h>` was included only for `mkdir` and is gone. Verified: `make -C build-app src/renderer.o` compiles on this machine, `grep -rn mkdir app/src` is empty, and the other 20 app translation units already compiled on Windows. The Windows job itself was not re-run here: its unpack step refuses a bundle whose recorded commit differs from `GITHUB_SHA`, so a hosted run needs `make release` to regenerate the bundle. See `docs/HANDOFF-2026-10-12-session135.md`.

- ✅ **The black map-unit head is fixed: it was RT64's framebuffer tile copy serving texels the framebuffer never rendered (session 134).** Session 133 left one question — why the head's `TEXEL1` samples as zero with correct RDRAM, descriptors and combiner — and the answer is that the tile never becomes a texture. `OGRE_SPRITE_TRACE=1` (one line per small tile, printed after the upload decision) and `OGRE_TILE_DECODE=1` (the RGBA the renderer decodes out of the TMEM snapshot it hashes, plus the raw-TMEM path) show **13 of the unit's 14 colour tiles reaching `TextureManager::uploadTexture` and the head's 16x15 tile not reaching it at all**. `OGRE_TILE_DECODE=1` also prints every accepted copy: `[fbcopy] tmem=0..60 lineWidth=16 siz=2 fmt=0 fb=0x2E6840 fbTile=468,65..484,67 fbFmt=5 reinterpret=0`, i.e. the tile is served from a **depth** framebuffer at masked `0x2E6840` that is still on record over the heap where `func_8019ABC4` builds the head-down bitmap (`0x802FB0E8`). `FramebufferManager::createTileCopyRecord` fills a tile copy from the framebuffer's `lastWriteRect` only (then discards the rect), so the tile — outside that rect — sampled a copy that was never written: `TEXEL1` read zero and `(1 - TEXEL1) * ENV + TEXEL1` painted the flat `ENV >> 3` = `0x2947`. **The fix** refuses a copy whose texels are not inside the framebuffer's `lastWriteRect`, so the load reads RDRAM the way hardware does. Two intermediate results are recorded: the first version (refuse every depth framebuffer as a colour source) fixed the head but changed one scenery scanline, because a one-row strip legitimately samples the same framebuffer's rendered rows through RT64's depth-to-RGBA16 conversion; and the freshness guard against `fb->RAMHash` fires only on some frames, because `hashTracking` re-hashes every framebuffer at each frame end without touching the GPU copies (which is what the developer saw as the head flashing between correct and black). Verified from the fault frame's dump: the head's box holds **1** pixel of `0x2947` against 76 before and the correct sprite art, the scenery row is byte-identical to its pre-fix values, `[fbcopy]` count is 0, `make patch-check` PASSES and no generated code was touched. **Developer-confirmed.** New: `debug/head-trace.sh`, `debug/head-crop.py`, `debug/black-sprite-133/rdpmodel.py`, `-analyze-spr.py`, `-pngwrite.py`. See `docs/HANDOFF-2026-10-11-session134.md` and `docs/notes-black-sprite.md` §10.

- ⬜ **The black head is captured, and the frame the game renders is in the dump (session 133).** Five key presses on `scene_14.bin` with the new `debug/frame-capture.sh` (one whole 8 MiB RDRAM image per press): press 1 has the head correct, presses 2-5 have it black, and the four failing presses are **byte-identical** at the head's asset block. The game's own framebuffers (`D_800A8204 = {0x80000400, 0x80025C00, 0x8004B400}`, 320x240 RGBA5551, `0x25800` apart) hold the **rendered frame** — 3D terrain, dialogue box and units — so a key press captures the fault with no screenshot and no `OGRE_CAPTURE_PRESENT`. New `tools/ogre_frame.py` renders a dump's `G_TEXRECT` layer to a PNG and models the RDP's TMEM indirection correctly (a `G_TEXRECT` samples the tile's TMEM, whose image is the one the **last load** bound, with the tile's `line` as the stride; the `G_SETTIMG` before the rectangle can be the palette load — the reason the old trace could never report a black part). **What the fault is:** the unit under the dialogue box is the map module's 14-part sprite (14 quads, two textured triangles each), each part drawing an I4 mask (I8 source at TMEM 408) and an RGBA16 colour (TMEM 0). The head is the 16x16/16x15 square part; correct, its colour is `0x802C6BE8` from the preloaded run `0x802C5280`..`0x802C9EB0`, failing it is `0x802FB0E8` from a runtime block at `0x802FB0E0` that `func_8019ABC4` builds (two `36 34` headers + two payloads). The rendered head is a **flat `0x2947`** = the frame's own `G_SETENVCOLOR 0x2A291F >> 3`, over the correct silhouette, in all three framebuffers — i.e. the ENV colour with the texture's alpha and no texture RGB. **The block's bytes are right at dump time** (both payloads decode to the head-down art; the stride is 16 texels by a row-similarity measurement) and the draw's state is identical in the two presses, so the renderer and the dump disagree. Next, and it is one datum: `OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_KEY=2 OGRE_TEX_TRACE_IMAGES=1 debug/frame-capture.sh`, pressing `2` at the black head and `1` to dump — the `[tex] img:` fingerprint is taken at display-list submission, so a fingerprint that does not match the dump says RT64 read the buffer after the game replaced it. **Corrections:** the unit is triangles, not `G_TEXRECT`s (which is why three earlier captures always printed `0 'BLACK DRAW'`), and the earlier "cutscene close-up" is the mission field's unit sprite. See `docs/HANDOFF-2026-10-11-session133.md` and `docs/notes-black-sprite.md` §9.

- ⬜ **The one-part black-tile path is decoded at instruction level: the code stream's `0x8140` command switches the working part sheet, and a part whose code is not in the loaded sheet resolves to `-1`, so its buffer is zeroed and the draw paints palette index 0 (session 132, round 3).** `func_8017A740` is a cursor into a byte stream, not a code lookup: it calls `func_8017B570` on the leading byte (which returns 1 exactly when `0x81 <= code <= 0xFC`, `0x8017B574`/`0x8017B58C`), and on failure jumps to the **sheet tail** at `0x8017A83C`, which selects the working sheet from `0x216DF1A` / `0x218DB76` / `0x218C17E` by `D_8018FC8C` and `D_8018FC98`, stores it in `D_80190F60`, clears the flag (`0x8017A858`) and returns **`-1`**. `beq $v1,0x8140` at `0x8017A7BC` routes the stream's sheet-switch command to that same tail, so it too yields no part. Every other in-range code binary-searches the `u16` table at `D_80190F74` and returns `index + 0x200` or `-1`. This is the first mechanism found that produces the developer's symptom exactly — the replacement head black while the rest of the character draws — because a consumer zeroes a `0x70`-byte buffer on `-1` (`func_801A8124` at `0x801A81FC`/`0x801A8200`). The test on the next capture: read the faulty record's code at `+0x78`, look for it in `D_80190F74`, and read `D_80190F60`/`D_8018FC8C`/`D_8018FC98` in both pose dumps. **Half of that test is already done and is negative:** in both of the developer's pose dumps `D_80190F60` is `0x027203D2` = `func_8009DD28(0x218C17E)` (the base sheet) with both selectors 0, so the switch did not fire between those two presses. Both presses were taken with the head black, so the pair brackets the fault and not the good pose; the mechanism is not cleared, but the sheet is not what differs between those two frames. Useful shortcut: `func_8009DD28(a0) = a0 + 0x594254`, so `0x027203D2` = `0x218C17E`, `0x02721DCA` = `0x218DB76` and `0x0270216E` = `0x216DF1A`, and reading `D_80190F60` names the working sheet with no lookup. Also this session: the dump byte order was corrected (ROM order; both readers fixed), the record layout was measured (links at `+0x00`/`+0x04`, payload at `+0x08`, code at `+0x78`), and **four claims are withdrawn** — session 131's cell-plus-palette model, session 132's own "`timg` eight bytes low" (the test read the upstream record's code field), "the cache is corrupt" (`0x1FFF` is every record's initial code) and "the cache is churning under pressure" (32 used / 224 free in one pose, 35 / 221 in the other, 3 records changing code — no eviction pressure). The fault itself is still **in no capture on disk**: every trace has `0 'BLACK DRAW'` lines. See `docs/HANDOFF-2026-10-11-session132.md`, `docs/guides/sprite-mechanisms.md` and `docs/notes-black-sprite.md` §3d.

- ⬜ **The second capture does not show the fault, and the condition that was present when the sprite was black is still present (session 131, continued).** The developer ran the capture again with `OGRE_TEX_TRACE_MS=0` and two `OGRE_RAM_KEY` presses (`ram-1-full.bin`, `ram-2-full.bin`), and reports the sprite rendered correctly. The evidence says the fault did not occur rather than that it is fixed: the trace (10506 rectangles, two display lists, `t=45326ms` to window close) still contains the two draws that sample **all-zero** bytes (`draw 8` tile `0x8026A5C8` rect `88,41..99,55`; `draw 59` the same tile rect `157,198..168,212`), and `0x8026A5C8` is **byte-identical in both dumps** (`nonZero 12/0x60`, first non-zero at `+72`) — the same state the black-head capture had. The bug is reported at about 10% of cutscenes, so a correct render in one run is not a fix. The developer chose to stop here and treat it as not reproduced; the images and tools stay for a later session. **For a later attempt the trace now reports colour**: `OGRE_TEX_TRACE_COLOUR=1` decodes each sampled window through its palette (CI4/CI8) or as RGBA16 and prints `[tex]   colour: mean N/255 …`, and one `[tex] BLACK DRAW: dl=… n=… rect=… timg=…` line for the first draw at or below `OGRE_TEX_TRACE_BLACK` (default 4) — the combination a black part with a correct cutout has (non-zero source, black mean). A capture with a `BLACK DRAW` line names the faulty draw; a capture with `colour:` lines and no `BLACK DRAW` says the part is not a `G_TEXRECT` at all. Verified on a title-scene run (colour means tracking the fade). `debug/black-sprite-capture.sh` sets it. **The key order is 1 (just before), 2 (at the moment it is black), 1 again (after it)**: the two full images are the same buffers in both states, and the trace covers the bad frames. See `docs/notes-black-sprite.md`.

- ⬜ **The black rectangles in the trace are font cells, not the black part; the capture now guarantees coverage (session 131, continued).** The developer's second capture (`scene_14.bin`, scene `0x14` the Witch's Den, head bowed; `ram-1-full.bin` + `texdump/`) shows the pool cells decode as **letters** (`0x8026A4C8` a capital D, `0x80269CC8` a u, `0x80269C48` a T), so the 11x14 rectangles session 130 followed are the font cache's glyph cells, every captured CI4 rectangle shares **one** present palette, and a blank cell decodes to the transparent colour 0 — so the all-zero rectangles are **not** the black sprite. The three copies of the display list in the dump are F3DEX2 and every character-sized draw in them is non-zero. A byte-order trap cost this session time: display-list words in the dump are not stored in the image data's byte order, and a big-endian hand-decode gave a plausible but wrong list (the renderer's own walker, `OGRE_TEX_TRACE`, is the authority). Landed for the next run: `OGRE_RAM_KEY` now writes a whole-image dump on **every** press, `OGRE_TEX_TRACE_MS=0` holds the trace from the press to the window close, and `OGRE_TEX_TRACE_KEY2` gives a second arming key on its own edge state; `debug/black-sprite-capture.sh` sets all three. See `docs/HANDOFF-2026-10-11-session131.md` and `docs/notes-black-sprite.md`.

- ⬜ **The black sprite part is a cell the engine never filled; the palette is present and is not the fault (session 131).** Session 130 read the 64 zero bytes at `0x8026A5C8` as an unwritten palette. They are the cell's **pixel data**, and the palette is the 32 non-zero bytes at cell `+0x60`: the 12x16 CI4 cell is 8 bytes a row for 16 rows (96 bytes), then 16 RGBA5551 entries. The `OGRE_TEX_TRACE` rectangles are those cells, one per 11x14 glyph, and every black rectangle reads all-zero bytes at the address it binds while its neighbours in the same pool are populated — so the fault is a part whose image bytes are not where the display list binds them, not a palette. The engine is at instruction level now: the task-queue producer `func_ovlA_801A07F0` (`bankRec3`, ROM `0xEBBD0+0x1980`, unit A) indexes a 12-byte-per-entry table at `0x8019F830` with `0x800E7AB8`, pulls the part asset `0x1E39A52` (ROM `0x7175A2`) through the code-list interpreter `func_8007938C(a2 = -6)`, reads that block's first words as **relative** pointers (`0x801A0C24..0x801A0C4C`), and LZ-decompresses each frame with `func_8007A110` into a destination whose offset **accumulates with the returned sizes** (`a0 = s4 + s1`, `s1 += ret`). New capture and reader for the run that names the write: `OGRE_RAM_KEY_FULL=1` writes the whole 8 MiB image on the key press, and `tools/ogre_ram.py` answers `find-ptr` (every word equal to an address, so the table that chose the buffer is named), `cell` (one slot printed as the game's own CI4 image) and `empty-cells`. Open: which write leaves the bound cell empty. See `docs/HANDOFF-2026-10-11-session131.md`.
- ⬜ **The battle character's black head is a part palette that is never written (session 130, continued).** The developer's `OGRE_RAM_KEY` dump of `0x802696C8` (0x1400 bytes) taken at the moment the head is black shows the image the head binds, `0x8026A5C8` (`fmt=2` CI, 64 bytes = a 32-entry RGBA16 palette), is **64 zero bytes**: the non-zero bytes in that 0x80 slot start at +72 and belong to the following entry. Every pixel that indexes the palette reads colour 0, which is the black head. The neighbouring slots are populated (`nonZero 24..55/128`), and a whole-cartridge search for the populated slots' bytes finds **no exact match**, so the table is computed at load time rather than copied from ROM. New instruments landed: **`OGRE_RAM_KEY=<1..9>`** (`_ADDR`/`_LEN`/`_OUT`/`_STRIDE`/`_AT_MS`, `app/src/ram_key.{hpp,cpp}`, ticked from `sdl_platform.cpp`) writes guest RDRAM to `<OGRE_PREF_DIR>/ram-<n>.bin` and `.txt` on a key press, the text ending in a per-slot `nonZero`/`mean` summary that marks an empty slot; and `OGRE_TEX_TRACE` gained `_IMAGES` (report every `SETTIMG` with a byte fingerprint, once per image), `_MIN_PX` and a real one-shot capture window (it previously re-armed every frame, so an earlier capture silently covered the whole run). Earlier in the session two leads were closed at instruction level — the `0x8017909C` sheet cache is the font/text cache, and the mission field rebuilds it on every entry — and the fault was localised with `OGRE_TEX_TRACE` to a per-part image that is all zero. Open: which routine fills these palette slots and why this one is skipped. See `docs/HANDOFF-2026-10-11-session130.md`.
- ⬜ **A sprite part draws solid black on the world-map route into a mission cutscene (session 130).** Developer report: character sprites are drawn in parts, and in about 10% of cutscenes one part (the example: the knight's head) is a black texture; the black part is **static for the whole shot**; the character is a **2D sprite over a pre-rendered background**; and the fault is **path-dependent** — *"if I load the suspend data, the issue doesn't happen. but if I load the save at the world map, move into the mission and trigger the cutscene, it does happen."* Two leads are closed at instruction level. (1) The sprite/bitmap cache at `0x8017909C` (`func_8017909C`, `asm/40E80.s:17123`) is the **font/text cache**, not the character sprite system: its accessor `func_80179404` returns a per-character **advance** that the text walkers sum into a pen position (`func_801794E8` `addu $s0,$s0,$a0` at `0x801798A0`), and the two 42 KiB assets it loads (`0x218C0D4`, `0x218DACC`, ROM `0x2720324`/`0x2721D1C`, `0xA600` each) are glyph sheets. (2) The mission field is **not** drawing through the world map's cache: scene `0x03`'s descriptor word 0 is `func_80173724`, which frees (`0x8017372C`) and reloads (`0x80173734`) the cache on every entry, and scene `0x0D` is always reached through scene `0x02`, whose enter reaches the loader through unit E's `func_80198D28` -> `func_80178D30`. Landed instead is the instrument: **`OGRE_TEX_TRACE`** walks the game's submitted display list and prints one `[tex]` line per `G_TEXRECT` with the rectangle, `s`/`t`/`dsdx`/`dtdy`, the `SETTILESIZE` window, the tile's line/palette and the `SETTIMG` address/format/size/width; `OGRE_TEX_TRACE_DUMP=<dir>` writes the RDRAM each draw reads beside a `.json` and prints a byte summary; `OGRE_TEX_TRACE_AFTER_MS=<n>` arms it at a chosen trace time so one bad frame is a small capture; and new **`tools/ogre_tex_uv.py <dir>`** decodes the dump into a per-draw mean/alpha summary (`--dark`, `--png`). The line format is validated against the app's own `OGRE_DL_DECODE`, and `G_TEXRECT`'s word order came from `app/src/gbi.cpp`'s `OP_TEXRECT`/`OP_RDPHALF_1`/`OP_RDPHALF_2` cases. Two scripted attempts to drive the mission were **discarded**: on this machine the app reaches 5.9 s of game time in 70 s of wall clock with `OGRE_DL_TRACE` on, so a scripted route is not a usable instrument here, and the developer drives instead. The open question is whether the black part's **source bytes are black** (the composite step that should have filled it did not run on that route) or the **source is fine and the rectangle reads the wrong place**; one developer-driven capture with the trace on decides it. See `docs/HANDOFF-2026-10-11-session130.md`.

- ✅ **Mouse support is a setting, MOUSE ENABLED, off by default (session 129).** Developer request: *"to wrap up, let's add a [ ] MOUSE ENABLED option in the settings (default off). when enable, the mouse controls work in the map screen and the mission screen."* The row is the SETTINGS tab's fourth group, `INPUT`, on the panel's shared two-option radio helper: `MOUSE ENABLED   [x] OFF [ ] ON`, stepped by LEFT/RIGHT or SPACE and saved as `mouse = off|on` in `<config>/settings.cfg` (`OGRE_MOUSE=off|on` overrides the file for one run without writing it). `settings.cpp` publishes it to `ogre_map_cursor_set_enabled` and `ogre_mission_cursor_set_enabled`, and each cursor module's `enabled()` now resolves `OGRE_MAP_CURSOR` / `OGRE_MISSION_CURSOR` first (`1`/`on` forces that screen on, `0`/`off` forces it off) and the setting otherwise, so one switch covers both cursors, the click that acts as `A`, and both edge scrolls. A file written before the row existed loads as `OFF`. The operating system pointer is hidden over the game window only while the row is on, and `OGRE_CURSOR=1`/`0` still forces that state. Verified: the launcher's SETTINGS tab draws `INPUT / MOUSE ENABLED [x] OFF [ ] ON` with no settings file, and `OGRE_LAUNCHER_KEYS="Tab,Tab,Tab,Down,Down,Down,Down,Down,Right"` draws `[ ] OFF [x] ON` and writes `mouse = on` (proofs `docs/proofs/native-launcher-mouse-off.png`, `-mouse-on.png`); two clean game runs on the mission route show the switch gating the hook (mouse on: `[missioncursor] hook active on the mission field` and 75 lines; mouse off: no `[missioncursor]` line at all), with `[overlay] shown` count 0 in both so no UI input contaminated them, and `tools/runlog.py --check` PASS on each. **A note on the first attempt:** both of the first two game runs had the Esc overlay opened on them (3 and 4 `[overlay] shown` lines), and the off run's panel turned MOUSE ENABLED on, so its hook ran; the run is discarded, and the probe (`probe129`, reverted) is what separated the contaminated run from the clean one. See `docs/HANDOFF-2026-10-10-session129.md`.
- ✅ **The mission field's view scrolls when the pointer is close to a play-area edge (session 129).** Developer request: *"let's continue the mouse support feature in the mission screen. this time we need to make the camera scroll when the cursor is close to the edges."* The field's camera pans with the **cursor**, not with a held direction: the mover `func_ovlN_801BE4E0` turns the held D-pad into the per-frame delta bytes `state+0x06`/`+0x07` (`0x801BE5A8` `li $v1,26`; the ramp at `0x801BE6D4`), and `func_ovlN_801BECF0` — called right after the mover at `0x801AF130` — reads those same bytes and adds `delta/128 * 8.5 * table[e]` to the eye x and the look-at x of the camera at `*(u32*)0x8018F5AC` (`0x801BEE04`-`0x801BEE1C`), the same for z (`0x801BEE9C`-`0x801BEF5C`), clamped to the map's world rect at `0x801F0DD8`. It only runs while `*(0x801F3630)` is set, which the mover sets only when the cursor pair actually changed on that call (`0x801BEC3C`-`0x801BEC70`), so the pan is coupled to cursor movement. The mover clamps the pointer's pixel to 40..280 / 30..210, so a pointer past that bound cannot place the cursor there and the intent arrives as input — the world map's mechanism: `ogre_mission_cursor_edge_direction` holds the N64 D-pad bit of any play-area bound within `OGRE_MISSION_CURSOR_EDGE` (default 24) pixels of the pointer, and `get_input` ORs it into controller 0. The mover decodes it through the profile `*(0x801F0CC0)`, whose four direction masks in this mode are the N64 bits (right `0x100`, left `0x200`, up `0x800`, down `0x400`), which the hook now prints once per run beside `hook active`. The existing arbitration adopts the game's cursor while a direction is held (the `pad=` signal), so the push and the pointer do not fight and the release still has no jump. **Session 128's "edge scroll is deliberately not implemented for the mission" is withdrawn.** Two defects in session 128's `[missioncursor]` line are fixed: the committed format had 15 conversions for 13 arguments (a stray `(%.2f,%.2f)` after `mouse=`), which the surviving session-128 logs show as a trailing `-> (0.00,0.00)`, and the line now carries the camera and the edge mask. The camera was first read at `0x8019F5AC`; the instruction is `lw $v0,-2644($v0)` with `lui $v0,0x8019`, so the word is at `0x8018F5AC` (`0x80190000 - 0xA54`), and `+0x14` is the eye (its height at `+0x18`), `+0x20` the look-at and `+0x2C` the up vector that `func_ovlA_801AAE30` builds the screen-to-world basis from. Verified: `/tmp/mc-verify2.log` has `hook active on the mission field (profile right=0x0100 left=0x0200 up=0x0800 down=0x0400)`, 1790 `[missioncursor]` lines whose `cam=` values change through the scripted hold, exit 0, and `tools/runlog.py --check` PASS; both `build-app` and `build-null` link. Developer-confirmed twice: *"I could move the camera around by moving the cursor close to the edges"*, and the pointer was hard to control only at the increased game speed, not at normal speed. New knobs `OGRE_MISSION_CURSOR_EDGE=<n>` and the test aid `OGRE_MISSION_CURSOR_EDGE_FORCE=<mask>`. See `docs/HANDOFF-2026-10-10-session129.md`.
- ✅ **The mission field's cursor follows the mouse (session 128).** Developer request: *"our mission now: adding mouse support to the mission screen"*, the open lead session 127 left. Scene `0x03` is not driven by the scene dispatcher — its descriptor's update and hook are no-ops (`func_801737CC` and `func_8016C814` are bare `jr ra`) — and runs from the task its enter queues with `func_80076F5C` at `0x8017375C`, i.e. record 7 (bank unit N). The cursor is two **f32 framebuffer pixels** at `state+0x08`/`+0x0C` (320x240, centre 160,120), where `state = *(u32*)0x801AB870 = 0x800E91D0`: a fixed BSS address that records 7 (`0x801AB870`), 9 (`0x80213690`), 2 (`0x8019E1B4`), 3 (`0x801AB870`), 6, 9b and 9c each hold as a pointer constant. Session 115 read the same object through `*(0x80213690)` and recorded it as a heap pointer; it is fixed, and it is re-laid-out per phase (`func_ovlA_80198C08` bzeroes `0x478` bytes), so the hook is gated by its call site and not by the object. The mover is `func_ovlN_801BE4E0`: `0x801BE510 lw $v1,-18320($v1)`; `0x801BE574`-`0x801BE584` read the **held** button word `*(0x800E8108)` (the base ELF's `func_80077494` fills it with `&D_800E79B0` at `0x800774BC`; its sibling `0x800C4BDC` holds the edge mask `&D_800E8100`), the D-pad masks come from the profile `*(0x801F0CC0)` (+0x0E right, +0x0C left, +0x08 up, +0x0A down), the delta ramp is `±26` then `±8*counter` divided by 128 and multiplied by 32.0, and the pair is stored at `0x801BE974`/`0x801BE988` and clamped to x 40.0..280.0 (`0x801BEB98`/`0x801BEBB8`) and y 30.0..210.0 (`0x801BEBE0`/`0x801BEC00`). **The mission's cursor is digital and the world map's is analog**: no mission bank loads `0x800C4BE0`/`0x800C47F0`, while the map reads the stick at `*(0x800C47F0)+2/+3` (`func_ovlM_8019AF0C`, `0x8019B438`) — which the developer confirmed from play. `app/src/mission_cursor.cpp` writes the pointer's pixel at the generated label `L_801BEC04`, after both clamps and before the tail that reverts the cursor when no unit is focused (`*(0x801F0E08) == -1`, `0x801BEC7C`-`0x801BECB0`); the "glue the cursor back to the unit" branch at `0x801BE890` jumps to the epilogue and is left alone. `tools/mission_cursor.py` inserts the call (it finds the unit that defines `func_ovlN_801BE4E0`) and `make bank-recomp` re-applies it. **The cursor is a servo the game drives continuously** (measured: 1-9 px per frame with nothing held, `none mode0` 123 of 162 frame-pairs), from the mover's integration, its C-LEFT screen-centre pull (profile `+0x1C` = 0x0002; field mode < 2 or == 42) and the snap-to-unit writers, so no threshold-based rule works: adopting whenever the game moved it locks the mouse out (*"the pointer was being pulled to the right, I could move it only with the d-pad"*), and always overwriting discards the game's own recentring. The landed rule hands the cursor to the game only on an explicit signal — a direction held (`pad=`), the centring branch (`centre=`), the mover's own delta sampled at `L_801BEC04` before the mover zeroes it at 0x801BEC48 (`nudge=`), or the frames after a click (`yield=`, default 15) — and **adopts** the game's position then. A second call at `L_801BECC8` does the placement, on both of the mover's paths including the centring branch at 0x801BE890. The click window is what leaves the cursor on the place a command acted on, so a second click confirms it. **Two behaviours the first version got wrong and the developer caught:** pinning the cursor every frame overwrote the game's own D-pad movement, so the arrow keys appeared to move only the camera; the hook now reads the same held word and stands down while a direction is held (logged `pad=1`). With that alone the cursor *"jump[ed] to the previous mouse position"* on release, so while the D-pad owns the cursor the pointer **adopts** the game's position and the handover is invisible. A left click ORs `A` into controller 0 through the same `get_input` path the map uses; both cursors are armed by one click, so `ogre_map_cursor_take_confirm`/`ogre_mission_cursor_take_confirm` now consume one poll **whatever screen is up** and return whether their own screen is active, so an `A` armed on one screen cannot fire on another. Verified: the parked pointer (`OGRE_MISSION_CURSOR_AT=60,200`) read back as `game=(60.00,200.00)`; developer-driven runs with 2960 hook lines and 46 clicks, `112 + 176 + 56` confirm lines (exactly 8 polls per click), and 223 `pad=1` frames; a scripted hold moved the game's cursor `145.00,114.25` → `145.00,120.75` and `64.25,166.00` → `40.00,166.00` while the hook's target was not written; and at the `pad=1` → `pad=0` transition the mouse became `(235.00,147.00)` from `game=(235.18,147.19)` instead of the parked `(200,40)`. Developer-confirmed: the cursor follows the mouse, a click confirms, the arrow keys still move the cursor, and the D-pad no longer jumps the pointer on release. The world map was re-checked after the `take_confirm` change in the same session and is unchanged: 131 cursor lines, 16 edge pushes and one click logged as `click at (186,151) game=(186,151) draw=1`. See `docs/HANDOFF-2026-10-10-session128.md`.
- ✅ **The world map's cursor follows the mouse, and the merged PR that claimed to add mouse support was dead code (session 127).** Developer request: *"I just merged a PR in my project (commit cf5b9285). It claims to add some basic mouse support, but I tested the game and could not reproduce it."* The PR's `ogre_cursor_click_apply` is defined and called from nowhere: `git grep cursor_click` finds it only at its own definition, the generated bank code that would have to call it has no hook table, and it compiles to a `T _ogre_cursor_click_apply` symbol nothing references, so the only observable effect was one `[cursor] click -> x,y` line. Its two `_WIN32`-only fixes (an owned `IFileDialog`, the Quick Edit console mode) and its `tools/elfcheck.py` sign-extension mask are real and are left in the tree. Landed instead: the map's cursor is a screen-space sprite. The state machine `func_ovlM_8019AF0C` keeps `state+0x52`/`+0x54` and clamps them to x `0x28..0x110`, y `0x28..0xC0` (`0x8019B8E0`), and the scene hook's cursor draw `func_ovlM_801A103C` reads the pair at `0x801A1C48`/`0x801A1C4C` before emitting sprite `a2=6`. `app/src/map_cursor.cpp` writes the pointer's framebuffer pixel into that pair at the draw site; the site is bank code and a `.nrm` hook resolves only through the base ELF's sections, so `tools/map_cursor.py` inserts the call and `Makefile`'s `bank-recomp` re-applies it. The cursor is not a node grid (635 logged positions, x 49..240, y 40..176, 1-2 pixel deltas), so the developer cancelled snap-on-rest. A left click ORs N64 `A` (`0x8000`) into controller 0 in `get_input` for 8 polls, and a pointer within 24 pixels of a play-area bound holds the matching D-pad bit, which scrolls the view; both are gated on the map's cursor draw, so they are inert on the title, menus and mission. The operating system pointer is hidden over the game window and shown over the Esc overlay window (`SDL_GetMouseFocus()` plus `overlay_visible()`). Developer-confirmed: the cursor "moves smoothly", a click "worked as expected (the character moved to the target location)", the edge scroll "worked nicely". The click log prints the game's own cursor beside the pointer's, and every click on the loaded map agrees with it (nine at 0-2 pixels, one before the map's first real draw). See `docs/HANDOFF-2026-10-10-session127.md`.
- ✅ **The Item Randomizer leaves the party's starting items alone (session 126).** Developer report: *"the initial items that the player start the game with are all randomized as well. those should be left alone."* The mod's list watch rerolled any owned-item record that was empty and had just been filled, and the game's own starting grant fills several in one frame: the map loader `func_ovlR_80215C38` (bankR record 9d, called once per map from bankN `func_ovlN_801AE880+0x764`, `jal 80215c38` at `0x801AEFE4`) grants `1,1,1,1,1,8,8,22,22` with literal `a0` at `0x80217068` when `lbu(0x8018F4A1)` is 63/64, else `2..5/8/21..23` at `0x802170CC`, and equips each unit it creates from that unit's class row through `func_ovlR_801DD430` (`0x80216F10`-`0x80217028`). `RELOAD_SLOTS 12u` becomes `BULK_SLOTS 1u`: a frame that fills more than one record is a bulk grant, so the mod re-primes both list shadows instead of rerolling, and a single filled record is still an acquisition. The save-field reader hook on `func_800749C0` stays as the precise save-load signal, and the record count now also covers the 40-slot list, whose four records in an early save were why the count alone used to be rejected. **A/B on one New Game from the title** (`OGRE_TAP_BUTTON="start,a,start,a,start,a,a,…"`, `OGRE_SPEED=4`, `OGRE_PREF_DIR` a fresh dir with only this mod): with the old threshold the four bulk frames of the opening reported `frame acq countup reroll 13907 9 0 9`, `14753 9 0 9`, `14754 8 0 8`, `14755 14 0 13` and 39 `replace` lines; with the new one the same frames report `re-prime 9 1 0 0`, `re-prime 0 2 1 0`, `re-prime 8 3 0 0`, `re-prime 0 4 1 0`, `re-prime 4 5 1 0` and no `replace`, both runs reaching the map (scene `0x05`). Developer-confirmed on the fixed build ("I saw the expected items in the inventory"). A third run confirmed the other side of the rule: with the shipped `LOG_LEVEL 0` build and the live console on the same route, slot 200 of the equipment list was empty (`d 80196E40` = `00 00 00 00`), `w 80196E40 01003200` planted one record (the console stores a guest word, so the bytes are `01 00 32 00` = id 0x0100), and the next read 5 s later showed id 0x00D8 with the untouched slot 201 still zero: one filled record in a frame is still rerolled. Probe `LOG_LEVEL 2u // probe126` reverted and the mod rebuilt. See `docs/HANDOFF-2026-10-03-session126.md`.
- ✅ **The Item Randomizer has no options and is marked experimental (session 125).** Developer request: the mod is still experimental, so add a clear note to its description and remove all five MODS panel options. `mods/item-randomizer/mod.toml` now has no `[[manifest.config_options]]`, and `description`/`short_description` begin `EXPERIMENTAL`. `mods/item-randomizer/src/item_randomizer.c` reads no config value: the no-longer-wanted behaviour is fixed in code. SEED is derived from the frame counter at `0x800AEFA4` on the first tick (the only entropy a code mod has, so it is new every boot); CHANCE is 100 (the roll is unconditional); SHOPS stays off (a purchase frame, recognised by falling gold, is always skipped); LOG becomes `LOG_LEVEL`, a compile-time constant in the source, `0` in a shipped build; FLAVOR is `ANYTHING` (the roll covers the whole id space of the list). The `mode` parameter and the `SAME KIND` branch are deleted from `roll_replacement`, `replacement_for`, `replacement_for_slot`, `drop_slot` and the three table walkers, `recomp_get_config_u32` and `config_u32` are deleted, and `item_category` goes with the branch that was its only caller. The one-time announcement is now `start verb` / `start seed` and the `save-read` line is gated on `LOG_LEVEL` too, so a shipped build writes nothing. `run-with-save.sh` no longer writes `mod_config/ogre_item_randomizer.json`. Verified: `tools/build-example-mods.sh mods/item-randomizer` passes end to end (Docker `gcc-mips-linux-gnu` + `RecompModTool`) and the built `.nrm`'s `mod.json` has no `config_options` key, so the MODS row builder (`app/src/ui.cpp`, `Tab::Mods`) adds no option rows, and `short_description` is the experimental text the panel draws. No game run was made; the panel was not captured. See `docs/HANDOFF-2026-10-03-session125.md`.

- ⚠️ **The Tutorial (scene `0x17`) crashes with a SIGBUS in the scene dispatcher, and the Item Randomizer is in no frame of it (session 124).** The developer opened the tutorial from the suspend save and the process died. Four macOS crash reports on 2026-10-03 (16:59:21, 17:38:38, 17:46:34, 17:46:57) are one fault: `EXC_BAD_ACCESS` / `KERN_PROTECTION_FAILURE` on N64 Thread 3, frame 1 at `func_80075BC0` (the scene dispatcher) and frame 2 at `func_80071EB0` (the streamed-overlay loader). Frame 0 carries no symbol and the fault address differs per run (`0x199ab25f0`, `0x1a4e015f0`, `0x1a4c195f0`, `0x19dd635f0`), so it is a data address the guest computed. Every address is above `0xE0000000`, which no guest mapping in this port reaches, so the value is neither a guest pointer nor a guest code address; an unknown guest code address resolves to a `get_function` stub rather than a jump, which is why this reads as a dereference and not a mis-dispatch. The run log ends immediately after the tutorial's two bank records load (`rom=0x069920` -> `0x80197B90` size `0x4D60`; `rom=0x1BA020` -> `0x80220F60` size `0x92B0`) and the save-field reader runs. **The item randomizer mod is in no frame**, writes only guest RAM, and the earliest report predates session 124's change to it. **The A/B that was meant to prove the mod innocent did not run:** `mods/item-randomizer/run-with-save.sh:72` copies `build-app/mods.json` over any `mods.json` written into the preference directory, so the mod was loaded in both runs; the veto must move `<pref>/mods/item-randomizer.nrm` aside. Open: which instruction in `func_80075BC0` computes the bad address, and whether the tutorial is reachable at all from a suspend save rather than through its own flow (`AGENTS.md` §9, a forced or resumed scene enters without the pre-state a natural path builds). **Not reproducible now:** the developer reports the tutorial runs fine, and neither binary changed, so a rebuild cannot explain the difference (`build-app/ogrebattle64` is still `Oct 3 12:06:40`, 27277408 bytes, the binary the reports came from; the mod's `.nrm` is unchanged at 8591 bytes). The leading candidate is the save sandbox, since every report follows another run and `run-with-save.sh` restores the imported save at startup while the port writes the battery back. See `docs/notes-tutorial-crash.md`.

- ✅ **The Item Randomizer recomputes the game's own used count (`+0x02`) for a record it fills, and the reconstruction is verified against the game's byte on three live dumps (session 124).** The developer picked session 123's open item: reimplement `func_8016B774`'s roster scan so a randomized item does not read `1/1` and refuse every equip. `func_8016B774` (`0x8016B774`, `.streamedB`, size `0x29C`) clears `+0x02` for both owned-item lists and adds one per roster reference; a mod cannot call it (`jal` from `0x81000000` to game code fails `R_MIPS_26`, and a hook on it crashes the save load), so `mods/item-randomizer/src/item_randomizer.c` reimplements it: 30 unit records at `0x80197210` stride `0x19` (gate `+0x01 & 1`, 10 byte ids at `+0x0D`, searched in the 40-slot list), 100 character records at `0x80193BE0` stride `0x38` (gate `+0x11`, four `lhu` ids at `+0x2A/2C/2E/30`, searched in the 278-slot list), and four class-derived ids per character from the table at `0x80186FF4` into `0x80187C62` (stride `0x48`, key byte `+0x79`, `lhu +0x62+i*2`). `fix_record_use` writes the count for every record the game newly filled, and only while the record still holds the id the mod wrote, so the replacement is also correct when a unit genuinely uses it. **Verification is an offline check of the scan against the game's own byte:** new `tools/refscan.py` and `make refscan DUMP=<image>` rebuild the column from an `OGRE_DUMP_RDRAM` image and diff it against RAM. Three developer dumps give **0 mismatches** -- 21, 22 and 25 occupied equipment records plus 4, 4 and 6 consumable records. `--item 117` names the contributing records (character records 6 and 17, both class byte 17, each crediting 117 from its class row), which is how each count decomposes. **A run-time sweep that compared both columns in game was removed**: four gates were tried and every one fired while the character records held their gate byte at `+0x11` but not their ids, so the scan correctly returned 0 and the sweep reported 15 false mismatches out of 25; the fourth attempt printed `roster sweep chars units gate id0 20 6 1 0`, which is the evidence that the gates and the id fields are written at different times. A `spoilslot` self-test that forced the stale byte was also removed: its gate tested the drop table's own fingerprint words, which the mod rewrites. **What is still open:** the correction's write has never run in game, because nine runs produced no acquisition that inherited a non-zero `+0x02`; the value it writes is the reconstruction, which is proven. The per-acquisition line `acquire ref game <id> <scan> <game> <characters loaded>` stays and needs no gate. See `docs/HANDOFF-2026-10-03-session124.md`.

- ✅ **The Item Randomizer's option loading, consumable table, item counts and pickup message are fixed, and the map pickup is verified end to end (session 123).** Continued session 122's mod against the developer's new battery save `build-app/saves/before-pickup.bin`. Six changes, each with its own evidence. (1) A stored mod option was ignored: `parse_mod_config_storage` (`librecomp/src/mod_manifest.cpp:815`) rejected any config file with no `mod_id` key, and `Config::load_config` then leaves every option on its default with no error and no log. A/B on one file: unpatched `[mod] start verb mode shops 0 0 0 0` / `start chance seed 100 191837307`, patched `2 1 1 0` / `55 12345`. `tools/patchcheck.py` then showed session 122's `recomp_log` export was in no patch, so no release carried it; both are in `patches/n64modernruntime-ob64.patch` now. (2) A save load was read as a burst of grants: the 40-slot consumable list holds four records in this save, so loading it produced `frame acq countup reroll 4176 4 0 0`. The precise signal is the game's own used-count rebuild `func_8016B774`, which the save loader calls at `0x8016CF30`, but **a mod hook on it crashes the save load** (`func_800749C0` calls it; the regenerated body fails `get_function`). A scene-change signal was tried next and **withdrawn**: the game grants items on scene changes (a mission's unit-init grants, a battle's reward as the battle scene ends), so the re-prime swallowed exactly the grants the mod exists to replace, which the developer saw as a battle reward that was "the expected item from the loot table, not a random one". The signal that works is a hook on the save-field reader `func_800749C0` itself (`.main` code, regenerates cleanly; the crash above is its callee). Verified on both the pickup save and the developer's new suspend save: `save-read`, `re-prime 22 3 0 1` and `re-prime 4 4 1 1` with the forced flag, and no inventory reroll at the load. The record count stays as a fallback (>12 equipment records in one frame re-primes both lists). The list walk also has its own log budget now: the table rewrites can emit 88 events on a mission-entry frame and used to push the re-prime and acquisition lines past the 20-line cap. (3) The consumable definition table was wrong by four bytes: `func_8016F500` (`0x8016F500`, `id*0x0C` then `lw -6420(at)`) reads base `0x8018E6EC` with the name at `+0x00`, not `0x8018E6F0` with the name at `+0x04`, so every consumable failed the name check and none was ever rewritten. Verified: `acquire-consumable 1 20 0 1` then `replace-consumable 1 19 20 1`. The same table holds the key items (ids 24..40, "Package for Gelda"), and the entry's byte at `+0x04` (`0x00`-`0x02` usable, `0x03`/`0x04` key) now keeps them: `acquire-consumable 37 21 0 1` then `keep-quest 37 21 4 1`. (4) The session memo is an involution, so a second grant of one original id wrote the same replacement into a second record and split the game's used count; `replacement_for_slot` now rolls a fresh item and logs `dup-fix`. (5) The two mechanisms fought: a value the mod had written into the drop table was flipped back to the original by the list watch, verified by writing 140 after `drop-table 49 140` and reading `00310001` back. The mod now records what it writes into a source table and logs `table-grant` instead. (6) The per-map ground-item list (`0x801F1002` u8 count, `0x801F1004` u16 ids at `i*2`, parsed by `func_ovlR_80215C38`, read by `func_ovlN_801B9694` into the popup `0x801F36C2` and the grant at `0x801BB930`) is rewritten behind the same record-7 fingerprint, so the popup and the inventory agree; session 122's corruption came from writing it as 32-bit words. Verified on the developer's own save and play: `pickup-table 205 10 0 0` (Old Clothing -> Glamdring), `acquire 10 21 0 1`, `table-grant 10 21 0 1`, `frame acq countup reroll 8975 1 0 0`. **The `1/1` defect is explained and is not the mod's**: the item screen draws `+0x02` then `+0x03` and the equip test is `sltu(+0x02, +0x03)`; the grant writes the id and `+0x03` only, so a record an unequip emptied with a stale `+0x02` reads `1/1` under whatever name is written next. `docs/notes-item-equipped.md` has it at instruction level. **"One find produced four items" is not what happens**: the save's own slots 17..20 already hold Bloody Emblem, Yggdrasil, Ice Chain and Nephrite Sword, and one pickup adds one record (`frame acq countup reroll 3168 1 0 1`); the earlier "four grants in one frame" was the save-load misread. **The battle reward is randomized too, by rewriting the pending-reward queue.** The developer reported twice that a battle gave "the expected item from the loot table from that encounter, not a random one". The log showed why: the reward was id 155 (Leather Armor), which the party already owned seven of, so the grant merged into that stack and the list watch (which only rewrites a slot the game newly filled) had nothing to do. The reward id does not come from the drop table at that moment: with the drop table's entry 10 column C rewritten 155 -> 17, the pending value at `0x801936D8` was still `0x809B` (155). That address is the first of 20 `u16` the mission bank serves a reward from — `func_ovlN_801E49D0` takes `lhu(0x801936D8)` at `0x801E5230` (fallback `lhu(0x801936DA)`), stores it to `0x8021C760`, grants it at `0x801E53DC` and clears it at `0x801E53F4`, and the message is built from the same stored word. The mod now rewrites the queue (bit 15 preserved), which covers a reward the party already owns. Verified with the developer's logged seed: `reward-queue 155 17 2 0` and `reward-queue 94 235 4 0`, with the queue's two values that were already this session's replacements left alone. The developer then reported "we got 'oracion' from a battle, a random item", and then two rewards read back name for name: vanilla Iron Claw (94) -> 3 Falchion, and vanilla Leather Armor (155) -> 193 Magician's Robe, the message naming the same item in both cases. The second reward merged into a stack the party already owned (count 1 -> 2), which the list watch alone could never reach. The flag that stops the writers fighting each other is now `s_memo_target` (every id the mapping has produced), set where the memo records a pairing, which also covers a value an earlier run left in the *saved* queue; the list watch logs `already-replaced`. New `mods/item-randomizer/run-with-save.sh` runs the mod against a chosen battery save; `--reset` matters, because the port writes the battery back and a played-on image comes up on the world map (scene `0x05`, no record 7) instead of the mission (scene `0x03`, record mask `0x38C`). Also new: `docs/notes-bank-hooks.md`, the scope of a hook on streamed bank code. See `docs/HANDOFF-2026-10-03-session123.md`.

- ⚠️ **The Item Randomizer mod randomizes a map pickup, and three defects and two port bugs are open (session 122).** Developer request: *"let's create a crazy mod for this game: an item randomizer"*, scoped to items dropped after defeating an enemy unit and items found on the map, with "anything in the item table" as the replacement. Built `mods/item-randomizer/`: one base-section frame hook (`func_80072944`) that (a) rewrites the per-object-type drop table at `0x801EDB38` (35 entries x 8 B, `{u16 type, u16 item_a, item_b, item_c}`; the game rolls a column with `rand()%3` at bankN `0x801ADD68` and reads it at `0x801ADDB0`) gated on a record-7 fingerprint, and (b) watches both owned-item lists and replaces the id of a slot that was empty and has just been filled. **The two lists are the session's main discovery**: the grant routine `func_ovlN_801DC740` (bankN record 7, `0x801DC740`) selects a list with bit 15 of the item word — set -> the 278-slot equipment list `0x80196B20` (definition table `0x8018C42C`, stride `0x20`, ids 1..277) and clear -> a 40-slot list `0x80193AE0` (definition table `0x8018E6F0`, stride `0x0C`, name pointer `+0x04`, id in the low byte at `+0x00`) whose id 1 is "Heal Leaf". "Heal Leaf" cannot be produced from the equipment table at all, which is why watching only `0x80196B20` was blind to the tutorial's consumable grants. Verified from the mod's own log during developer-driven runs: `acquire 205 21 0 1` then `replace 205 43 21 0` (one empty slot, one reroll, one item), `probe first last present 39 80 1 0` (record 7 resident, table found), and a session with 9 `acquire`, 5 `replace`, 62 `drop-table` lines in which no `acquire` was ever followed by two `replace` lines for one slot. An earlier build corrupted items — it wrote `0x801F1004[index]` for the per-map pickup list, which is off by two bytes (the loader `func_ovlR_80215C38` writes the count at `0x801F1002` and the ids from `0x801F1004`; the developer saw a pickup with no name and garbage icon pixels) — and that whole path was removed rather than repaired. **Open:** an item that reports as `1/1` ("one already equipped") and cannot be equipped; whether the game or the mod adds a second item to a find that yields several; and whether the drop table is the battle-drop source at all. **Two port bugs found:** a mod config file is rejected unless it carries a `mod_id` key (`librecomp/src/mod_manifest.cpp:815`) that the runtime's own writer never emits, so every mod option reads its default forever (a line in `save_mod_config_storage` fixes it); and `RecompModTool` has no case for the MIPS `teq` gcc emits after a division, so `tools/build-example-mods.sh` now passes `-mno-check-zero-division`. Two new port affordances: `OGRE_MOD_TEST=1` boots with mods installed and no start screen, and a `recomp_log` export (in the gitignored vendored tree) lets a mod print at all, because it is compiled without libc. See `docs/HANDOFF-2026-10-03-session122.md`.

- ✅ **The SETTINGS tab gained the rest of RT64's graphics configuration (RESOLUTION, MSAA, WINDOW) and a gamepad now drives both panels (session 121).** Developer direction after a project survey: expose the display settings and make the pad usable in the UI. The runtime already held the values — `ultramodern::renderer::GraphicsConfig` carries `res_option`, `msaa_option` and `wm_option`, `RT64Renderer::update_config` (`app/src/renderer.cpp:917`) already applied all three on a change (window mode through `setFullScreen`, MSAA through `updateMultisampling`), and the port left them at the all-zero default `static GraphicsConfig graphic_config{}`: native resolution, no MSAA, windowed. Only the aspect ratio (WIDESCREEN) and the API pin were reachable, so the port offered no display setting at all. `app/src/settings.{hpp,cpp}` now holds `ResolutionMode` (`NATIVE`/`2X`/`AUTO` -> `Original`/`Original2x`/`Auto`), `AntialiasMode` (`OFF`/`2X`/`4X`/`8X`) and `DisplayMode` (`WINDOWED`/`FULLSCREEN`), written as `resolution`/`msaa`/`window` keys in `<config>/settings.cfg` and overridable for one run with `OGRE_RESOLUTION`/`OGRE_MSAA`/`OGRE_WINDOW`. The `settings.cpp` writer (`publish_graphics`) and `widescreen.cpp` each start from the live config and change only their own fields, so neither resets the other; a run whose widescreen aspect change re-applied a config that still logged `resolution 2x (mult 2.00), msaa 4x, window fullscreen` is that check. A new `[renderer] config:` line prints once per applied change, which is what makes the effective value readable from a log. `app/src/ui.{hpp,cpp}` gained `RowKind::Resolution`/`Antialias`/`Display` on one shared radio-row helper (the measure, draw, step, click and pad paths all go through `is_radio_row`/`radio_row_current`/`radio_row_layout`/`store_radio_row`/`step_radio_row`), which also replaced three per-kind switch copies and the triplicated steppable-kind list that is now `Panel::row_is_steppable`; the CONTROLS tab carries one `MENU PAD NAVIGATION:` section line documenting the panel's own pad controls, and `layout_rows` now wraps a section title to the whole panel width because that line is wider than the left column. A pad drives the same panel: `Panel::pad_command_for_button` maps D-pad/A/Start/B/LB/RB to `Panel::PadCommand`, the launcher and the overlay both handle `SDL_CONTROLLERBUTTONDOWN`, `SDL_CONTROLLER_BUTTON_BACK` opens and closes the overlay only while the input map binds nothing to that button (`pad_button_is_bound`), and the game already receives an idle pad while the overlay is visible (`get_input`), so navigating the panel cannot move the game's cursor. Because this machine has no gamepad, `OGRE_LAUNCHER_PAD` and `OGRE_OVERLAY_PAD` push synthetic controller-button events through the real handler. Verified on `build-app/ogrebattle64`: the SETTINGS tab draws `RESOLUTION [x] NATIVE [ ] 2X [ ] AUTO`, `MSAA [x] OFF …` and `WINDOW [x] WINDOWED [ ] FULLSCREEN`; `OGRE_LAUNCHER_KEYS="Down,Down,Right,Down,Right,Down,Right"` and the equivalent `OGRE_LAUNCHER_PAD` both write `resolution = 2x`, `msaa = 2x`, `window = fullscreen`; the pad script `back,down,down,right` opens the overlay, sets `resolution = 2x` and logs `[input] overlay open: game input suppressed`; `OGRE_RESOLUTION=2x OGRE_MSAA=4x OGRE_WINDOW=fullscreen` logs `resolution 2x (mult 2.00), msaa 4x, window fullscreen` and exits 0; a stock run is `tools/runlog.py --check` PASS with 655 display lists and no stub; the null-renderer build compiles and runs. Proofs refreshed: `docs/proofs/native-launcher-settings.png`, `native-overlay-settings.png` (captured through the pad script, so RESOLUTION reads 2X), `native-launcher-controls.png`. **Then gated, on the developer's report that a player run with them showed visual artifacts:** RESOLUTION and MSAA now ship behind `OGRE_DISPLAY_EXPERIMENTS=1`. With the flag off the two rows are absent from the SETTINGS tab (`Panel::build_rows`) and the two values are not applied at all (`applied_resolution`/`applied_antialias`), whatever `settings.cfg` or the environment says, and the boot prints `resolution 2x and msaa 4x are experimental and not applied; set OGRE_DISPLAY_EXPERIMENTS=1 to enable them`; a configured value is kept in the file rather than overwritten, and a set `OGRE_RESOLUTION`/`OGRE_MSAA` enables the gate on its own so the debug spellings keep working. WINDOW is unaffected and remains a shipped row. Verified: `settings.cfg` with `resolution = 2x`/`msaa = 4x` boots as `resolution native, msaa off` and logs the reason while `window = fullscreen` still applies; `OGRE_DISPLAY_EXPERIMENTS=1` applies `resolution 2x (mult 2.00), msaa 4x, window fullscreen`; the launcher's SETTINGS tab draws two DISPLAY rows without the flag and four with it (`docs/proofs/native-launcher-settings.png`, `native-launcher-settings-experiments.png`); changing WINDOW while gated rewrites `window = fullscreen` and keeps `resolution = 2x`, `msaa = 8x` in the file; a stock run is `tools/runlog.py --check` PASS. Open: one `back,back` scripted run logged a third `[overlay] shown` that four repetitions and a `back` single-press run did not reproduce; the pad toggle is two log lines so it is easy to re-check. The artifacts themselves are PLAN.md open work 14. **Fullscreen was broken and is fixed:** the developer reported the game showing small in the display's top-left corner with the rest black. RT64's fullscreen is a raw `SetWindowPos` on Windows and `[NSWindow toggleFullScreen:]` on macOS, and neither updates SDL's cached window size; `widescreen_update` reported a mode change on its first call, so a run that started fullscreen re-fit the window on its first frame from a size SDL had not updated. On macOS SDL ignores a resize of a window it knows is fullscreen (measured: the window stays 1680x1050 points / 3360x2100 px for the whole run); on Windows SDL had not performed the transition, so it applied the resize and the window became the 4:3 shape anchored at the monitor's origin. Fixed in two places: `widescreen_update` adopts the current mode on its first call without reporting a change, and `widescreen_fit_window` returns early while the live `GraphicsConfig` asks for fullscreen, logging `[widescreen] fullscreen: the renderer owns the window's shape` once. Leaving fullscreen is handled by a new handshake: `RT64Renderer::update_config` calls `widescreen_request_refit()` after `setFullScreen(false)`, and the next frame re-derives the shape. Verified on macOS: a fullscreen run logs the skip line and no `[widescreen] window` line; WIDESCREEN toggled while fullscreen does not resize the window; leaving fullscreen after that toggle re-fits 960x715 -> 1271x715; a windowed WIDESCREEN toggle still re-fits 960x720 -> 1280x720; a stock run is `tools/runlog.py --check` PASS and `build-null` builds. **The Windows re-test is the developer's**: this machine is macOS, where the symptom never reproduced, and the Windows path is the one the mechanism predicts. See `docs/HANDOFF-2026-10-01-session121.md`.

- ✅ **`tools/patchcheck.py` could not see a file a patch creates, so the first v0.6.0 release run failed its own patch check on the hosted runner (session 120, same session as the `make release` entry below).** The run unpacked the regenerated bundle successfully and failed on three lines: `function_trace.cpp: in the patch, and the tree does not change it`, the same for `live_generator_wasm.cpp`, and `plume_vulkan.cpp: changed in the tree and in no patch, so it ships nowhere`. Two defects, both reproduced at the pinned commits with `git worktree add`. (1) `patches/n64modernruntime-ob64.patch` **creates** `ultramodern/src/function_trace.cpp` (`new file mode 100644`, 925 lines); the runner's "Prepare third-party trees" step applies it with a plain `git apply` and never stages it, so the file is untracked, and `git diff HEAD` does not report untracked files, so the check found no block for it. On a developer's machine the file is in the index, which is why the check passed there. `git hash-object` on the patched file is `786872e2de96a25c7cef3e63c6f313f404311996`, the blob the patch names, so the file's content was always correct. (2) `plume_vulkan.cpp` is in `tools/rt64-plume-sdl.patch`, which the hosted path also applies to the plume tree, and not in `patches/rt64-plume-ob64.patch`, the only patch file the check compared that tree against. Fixed: `tree_changes()` is `git diff HEAD` plus `git ls-files --others --exclude-standard` (which respects the third-party `.gitignore` files) diffed against `/dev/null` with `git diff --no-index`, and the check covers each tree's changes with the union of every patch that targets it, with `tools/rt64-plume-sdl.patch` listed in `TARGETS` as the plume tree's second patch. Verified on all three patched pristine trees, and the check still fails on a covered file whose content drifts, on an untracked file no patch carries, and on a tree change no patch carries. See `docs/HANDOFF-2026-10-01-session120.md`.

- ✅ **`make release` regenerates the data bundle, proves it matches the tree, commits it to the private repository and starts the release (session 120).** Developer report: the CI release job failed with `files.tar.gz records public commit c7bd999…, and this release builds dc98d5d… this build is 10 commit(s) newer`, and the ask was a `make release` command that regenerates the bundle and starts a new gh release. New `tools/release.sh` behind the `release` and `release-check` targets: regenerate (`make bank-recomp`, `make recomp`, `make rsp-recomp`, `make example-mods` — the order is `make regenerate`'s, because `make recomp` refuses without the `app/src/bank_funcs.inc` that `make bank-recomp` writes), pack with `tools/data-bundle.sh`, unpack the result and compare every file against the tree, commit and push the bundle to the repository named by the `OGRE_DATA_REPO` Actions variable (`lfarroco/ogre-recomp-private`), then dispatch the Release workflow with `draft=true` and wait for the three platform builds. The workflow creates the tag on the commit the run built. Refusals before any write: an existing tag or release, a tracked tree with uncommitted changes, and a branch tip that is not pushed. `make release-check VERSION=v0.6.0` is the same path with `NO_PUSH=1` and changes nothing outside the tree; it ran to completion here: 34 bank units with `check-banks` OK, 14 tail-call sites repaired, both RSP microcodes, both `.nrm` mods, and `tools/data-bundle.sh` reporting 190 entries with sha256 `70948b5b…`, after which the script's own comparison reported `RecompiledFuncs/`, all 34 `Bank*Funcs/`, `RspFuncs/` and `app/src/bank_funcs.inc` identical to the tree. The private bundle's contents differ in 5 entries, which names what the release would otherwise ship without: `BankNFuncs/funcs_2.c` still yields at `0x80204C48` (48 `yield_self` against 47) and `BankEFuncs/funcs_0.c` has no `ogre_hd_background(rdram)` call, so the old bundle is correct for its own commit (`c7bd999`, the v0.5.1 tag) and predates sessions 114-116. `.github/workflows/release.yml`'s `draft` input now defaults to `true`, so a dispatched unattended run drafts. The release was started as **v0.6.0, draft** at the developer's choice. See `docs/HANDOFF-2026-10-01-session120.md`.

- ✅ **Issue #13: the gamepad was invisible in game because a pad filled N64 controllers 2-4 and the game reads controller 1 (session 118).** Report: *"Controller is recognized in the overlay. When changing controls, the overlay responds and reflects what is pressed. controls.cfg updates appropriately. When in game, controller is not recognized by the game. Keyboard works."* `osContGetReadData` (`tools/N64ModernRuntime/ultramodern/src/input.cpp:164`) writes `get_input(controller, ...)` into `data[controller]`, and this game reads index 0 only: `func_8008A47C` (`asm/1060.s:30185`) passes `D_800C4BF0` to the pad read `func_8008A3A4`, `func_8008A600` (`:30310`) copies one `OSContPad` (`0x18` bytes) out of `D_800C4BF0`, and the per-frame poll `func_8007297C` (`:2163`) calls it. `app/src/sdl_platform.cpp` opened every pad into `Platform::controllers[1..3]` in both the one-time enumeration and the `SDL_CONTROLLERDEVICEADDED` branch, and `get_input` OR'd the keyboard in for `controller_num == 0` only, so the pad landed on a controller the game never polls while the keyboard worked; the assignment and its "slot 0 is keyboard-first" comment go back to the first app skeleton (`0390658`) and predate the CONTROLS tab. Zelda64Recomp confirms the model from the other side: `src/game/controls.cpp:78` `recomp::get_n64_input` returns `false` for every `controller_num != 0` and ORs the keyboard and controller mappings into one button word, `src/game/input.cpp:565` ORs each button across every open SDL controller, and `src/game/input.cpp:522` reports a device for case 0 only. Fixed: both assignment sites fill the first free slot starting at 0, the ADDED branch skips a device whose instance id is already in a slot (SDL queues an ADDED event for every pad connected at init, so one pad was reported as two N64 controllers), and the enumeration logs one `[input] controller <n>: <name>` line per filled slot instead of only slot 1. Verified on `build-dist/ogrebattle64` (static SDL2 2.32.10) with an SDL virtual game controller (`SDL_JoystickAttachVirtualEx`, `SDL_JOYSTICK_TYPE_GAMECONTROLLER`, N64 A held) and the live console `rh 0x800C4BF0 8`, one variable changed between runs: slots 1..3 give `[input] controller 2` and `OSContPad[0].button = 0000`; slots 0..3 give `[input] controller 1` and `8000`, with pads 2-4 zero. Scripted runs need `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`, because without keyboard focus SDL drops button presses in `SDL_PrivateJoystickShouldIgnoreEvent`; that is why the first probe runs read 0 and it does not affect a player. The probe was reverted (`grep -rn probe118 app/src` empty). Open: `app/web/web.js` places browser gamepads in slots 1..3 too. See `docs/HANDOFF-2026-10-01-session118.md`.

- ✅ **The hosted release builds RT64 from `patches/`, and two of those patches had drifted from the trees, so session 107's Windows capture fix was in no release (session 117).** Developer question: *"can you audit if we have other debug behaviors in the prod build? this should never happen (apart from dumping the errors to the log file when something goes wrong)"*, with the context that an earlier slowdown fix did not take effect for players *"or it was an issue with the CI building"*. The CI hypothesis is correct. A GitHub-hosted runner has no vendored trees: it clones each third-party project at its pinned commit and applies `patches/` (workflow step "Patch the RT64 submodules", guarded by `vars.OGRE_DATA_REPO != ''`; `gh variable list` shows `lfarroco/ogre-recomp-private`, and run `36413921688` executed that step on all three platforms). The developer's build uses the dirty trees. `patches/rt64-ob64.patch` still read `ogreCapture.presentPath = getenv("OGRE_CAPTURE_PRESENT")` while `tools/RT64/src/hle/rt64_present_queue.cpp:141` calls `ogre_present_capture_path()` — 29 lines of drift — and `patches/rt64-plume-ob64.patch` had no `plume_d3d12.cpp` hunk, so the guard that stops `setSamplePositions(dstLocation.texture)` dereferencing a placed-footprint (buffer) destination was absent. Both are session 107's fix for v0.4.0's Windows first-present crash: in v0.5.0 and v0.5.1 `snap`'s presented-frame capture silently does nothing (RT64 reads an environment variable nothing sets), and a player who sets `OGRE_CAPTURE_PRESENT` still reaches the D3D12 null-texture read. Fixed: both patches regenerated from their trees (`git diff patches/` is 38 insertions and 11 deletions), both verified to apply to pristine worktrees of RT64 `4337374` and plume `d890ac8`, and a new `tools/patchcheck.py` / `make patch-check` fails when any tracked patch stops describing its tree; `tools/release-build.sh` runs it before packaging. The unit-slowdown report is the same gap in the other direction: **v0.5.1 was built from `c7bd999` (session 112's commit)**, the private `files.tar.gz` records `public commit: c7bd999` (MD5-equal to `dist/ogre-data/files.tar.gz`) and holds **34** `yield_self` sites against the tree's **33**, so session 115's slowdown fix (`8c5cbba`) and session 116's stutter fix (`da8676c`) are in no release. The generated-code channel is already guarded (the workflow fails when the bundle's recorded commit differs from the build's); the patch channel was not. The audit itself, with no environment variables set, also found: a live console channel that opens and executes a fixed path (guest-RAM writes, input injection, checkpoint `save`/`load`) from a 1 kHz loop, ~1000 `fopen`/s, 116 `[rsp]` lines/s, per-display-list `[renderer]` lines, and `recomp_trace_entry`/`recomp_trace_return` on every recompiled function entry and return (28 + 4 instructions at 5316 sites with every gate off). `CMAKE_BUILD_TYPE=Release` does define `NDEBUG`, `assert()` is out, RT64's own logging compiles away, and the presented-frame capture is off by default (verified static and with a cleared-environment `OGRE_SMOKE=1` run reporting `[smoke] capture path: (unset)`). Gating the remaining items is open work. **The live console is now off by default.** `console::tick()` runs from the 1 kHz `update_gfx` loop, so a stock run opened `/tmp/ogre-console.txt` about 1000 times a second and executed whatever it found: the command set holds `w` (a real guest-RAM store), `press` (synthetic pad input), `dump`, and `save`/`load` (a whole-RDRAM checkpoint restore), and the same tick printed a hotkey hint whenever a player pressed `1`..`9`. It is now enabled by `OGRE_LIVE_CONSOLE=1`, or by naming a trigger's own variable for that trigger alone (`OGRE_CONSOLE_FILE`/`OGRE_CONSOLE_AT_MS`, `OGRE_KEY_<n>`), with `0`/`off`/`false`/`no` as an explicit off; the decision is a cached static, so the disabled path is one bool test. `OGRE_CONSOLE_ON_CMD`/`_SCENE`/`_STEP` are unaffected (they call `exec()` directly, and their own variables are the opt-in). Verified on `build-app/ogrebattle64`: nothing set gives 0 `[console]` lines with the command file untouched and still present; `OGRE_LIVE_CONSOLE=1`, `OGRE_CONSOLE_FILE=/tmp/ogre-console.txt` and `OGRE_CONSOLE_AT_MS=100` each read and consume it; `OGRE_KEY_1` alone leaves the file alone; `OGRE_LIVE_CONSOLE=0` is off; `OGRE_CONSOLE_ON_SCENE=0x09 OGRE_CONSOLE_ON_CMD='c'` still fires on the scene; and `snap /tmp/stutter/snap 600` still wrote an 8 MiB RDRAM image plus `/tmp/ogre-shot.*.ppm` frames. Recipes written before this session need `OGRE_LIVE_CONSOLE=1`; `docs/guides/app-build.md`, `AGENTS.md` and `debug/menu-probe.sh` carry it. See `docs/HANDOFF-2026-10-01-session117.md`.
- ✅ **The every-1.5 s "clockwork" stutter was the runtime's unconditional periodic `[snap]` queue snapshot, now behind `OGRE_SNAP` and off by default (session 116).** Player bug report: *"Every couple of seconds I experience a stutter, like clockwork. Game does run, but with the stutter non-stop."*, assumed to be the Windows build. `vi_thread_func` called `ultramodern::debug_dump_queue_snapshot` every 90 VI frames with no gate — a project addition (upstream N64ModernRuntime has no such call) that session 28 had already written down as "expensive and unconditional ... should be behind an env gate (and probably off by default)". The snapshot is heavy by construction: `debug_sample_hot_loop(4)` sleeps 100 times at 200 us (20 ms alone), `debug_dump_call_chain` runs for every parked thread, `debug_dump_alloc_ring`/`debug_dump_alloc_trans` walk the allocator, the heap-cycle DFS visits up to 2000 edges per bucket, and ~80 lines print — all on the Critical-priority VI thread, which is the thread that delivers the retrace being observed. Measured on macOS with four 40 s runs (`OGRE_DL_TRACE=1`, frame gap between display-list submissions, steady state t > 3 s, each run with its own fresh `OGRE_PREF_DIR`, only the gate differing): before the fix p50 33 ms / p90 35 ms / max 866 ms with 26 snapshots and 27 spikes above 1.5x p50, **23 of those 27 in the same 100 ms phase bin of a 1500 ms period**, each spike 33 -> 59 ms and the gaps between them 1499-1502 ms; after the fix p50 33 / p90 35 / max 316 ms, 0 snapshots, 2 boot spikes and no fixed phase; with `OGRE_SNAP=1` the degradation returns (25 snapshots, p90 48 ms, max 952 ms); default again 0 snapshots and no fixed phase. The gate reads `OGRE_SNAP` once outside the loop: unset/`0`/`off`/`false`/`no` disables, `1`/`on`/`true`/`yes` uses 90 VI frames, a number > 1 sets its own period. The Windows size is inferred, not measured: `debug_sample_hot_loop` sleeps 200 us per sample and Windows resolves that to the system timer, which SDL2 pins to 1 ms via `timeBeginPeriod(1)` (`tools/SDL2-static/SDL2-2.32.10/src/timer/windows/SDL_systimer.c:49-63`), so 100 samples cost ~100 ms instead of ~20 ms. The change is carried in `patches/n64modernruntime-ob64.patch` (`ultramodern/src/events.cpp`); the vendored tree is gitignored. Verified: `tools/runlog.py --check` PASS on the fixed binary (1091 display lists, no crash/stub/UNKNOWN/bad RSP exit), the regenerated patch reproduced the old one except for the one hunk, `git worktree add` of the pinned commit `589bbf0` plus `git apply --check` succeeded, and `git apply --check -R` in the vendored tree succeeded so the tree equals the patch's post-image. No probe was added to generated code. `tools/dlgaps.py` is the reusable form of the measurement: it prints the gap distribution and the spikes' phase histogram, and `--check` exits 1 when they are periodic (it reports `base.log` as PERIODIC with 85 % of 27 spikes in one 1500 ms bin, and `fix.log`/`off2.log` as not periodic). Still open: a hang now produces no `[snap]` output unless the run asked for it, and the Windows number needs one run there to confirm. See `docs/HANDOFF-2026-10-01-session116.md`.

- ✅ **The unit-command menu's slow frame was the session-80 class a fifth time; the branch is `0x80204C48` (session 115).** Developer report: *"the game slowdowns when the 'unit menu' is open"* — the menu with `commands` / `unit commands` / `stronghold commands` / `status` that opens when a unit is selected, reproducible in the tutorial practice stage. The runs use the developer's `assets/scene_5_suspend.bin` save and session 112's tap recipe (`OGRE_TAP_MS=2000 OGRE_TAP_SCENE_BUTTON="title:start:1,0x12:a:1,0x03:a:4"`, four presses in scene `0x03`). Pre-fix the mission ran at 75 display lists/s (p50 10 ms, p90 24 ms) before the presses and **37/s (p50 21 ms, p90 50 ms)** after them, with `docs/proofs/native-mission-unit-command-menu-slow.png` capturing the screen (the `Commands` tooltip over the `LEADER Deneb` unit panel). `OGRE_PROFILE=1` named `0x80204C08` at 39-58 % of the frame-pump thread `t4`, and host stack samples (`sample <pid> 1 -file …`, `debug/menu-probe.sh`) put 3893 of 7071 `t4` samples (55 %, 18-82 % per second) in `yield_self` under `func_ovlN_80204C08` (record 9, bank unit N). Its loop at `0x80204C1C..0x80204C48` walks 36-byte entries at `state[+0x60]` (`state = *(0x80213690)`), counts the entries whose word at +8 equals the id in `state[+0x66]`, and exits when the count (set to -1 at `0x80204C08`) equals the signed byte in `state[+0x68]`; the body makes no call and no store, and the invariant-base load the heuristic keys on (`lb $v1, 0x68($t1)` at `0x80204C3C`) is the loop's own exit threshold. It is a work loop: the caller writes `state[+0x68]` at `0x80204874` from its own yield-free scan of the same array at `0x8020484C`, and the captured menu state (`state 0x800E91D0`, array `0x801F3A90`, id 0, threshold 1) makes the loop exit after five iterations, so each call paid four VI-retrace waits. `config/banks/config-bankN.toml`'s `yield_work_loop_branches` gained `0x80204C48` (after `0x801B3008`, `0x801DA2C0`, `0x801D0C08`, `0x8020A910`); regeneration removes exactly one site (48 → 47). Fixed, same save and presses, three runs: p50 8-9 ms, p90 10-19 ms, 0 of 33 stack samples naming the function, `[cover] 0x80204C08 2064` so the path still runs, `check-banks` OK, `elf-rom-check` 0 differing bytes, and `runlog.py --check` PASS on both the fixed app run and a `build-null` boot. The raw ROM word at RAM `0x80204C48` (ROM `0x155DB8`) is `0x1443FFF4` (`bne $v0, $v1, 0x80204C1C`). **Developer-confirmed.** The checkpoint saved for this session does not replay an active scene: after a load the machine submits no display list and no RSP task, all game threads sit in `wait_for_resumed`, and the retrace clock advances while the frame counter is frozen. See `docs/HANDOFF-2026-10-01-session115.md`.

- ✅ **The empty `stdout`/`stderr` sections of every Windows crash report are the Explorer launch state, and the capture records nothing in it (session 119).** Developer question: what can this Windows machine do that the Mac cannot. Taken: session 107 §7's open item. `OGRE_CRASH_TEST=segv` writes one `[boot] ...` line to stderr and then raises, so the report shows exactly what the capture received. Eleven launch modes of the packaged 2026-09-28 binary: every mode that gives the child usable standard handles (a `cmd` redirect, `Start-Process -RedirectStandardOutput`, a parent console, `DETACHED_PROCESS` + redirection, `CREATE_NO_WINDOW` + redirection, `DEVNULL`) puts the line in the stderr section, and the redirected **file** is 0 bytes in all of them; with a console present the app freopens `CONOUT$` over the redirect (`main.cpp:192-196`) and the console receives 12 `[boot]` lines while the file stays empty. With NULL standard handles — the Explorer launch a player uses — **both sections read `(empty)`**, and a fault injected six seconds into a normal boot still leaves them empty, while the same six-second run with valid handles carries **101 `stdout` + 53 `stderr` lines**. The six-second rows rule out a race with the drain thread. So a player report carries no log at all, which explains the empty sections in sessions 94d, 94e, 107 §7 and the v0.4.0 report of session 113. In the code, `crash_log::redirect_start` (`crash_log.cpp:177`) already falls back to `NULL` when `io_dup(target)` fails, so either the remaining `io_dup2(fds[1], target) < 0` return leaves no drain thread, or the UCRT leaves `stdout`/`stderr` unassociated and `fprintf` writes nothing; no compiler here, so they were not separated. Proposed fix, unverified: give fds 1/2 a real destination before `crash_log::install`, and skip the `CONOUT$` freopen when the handle is already a disk or pipe. The v0.4.0 binary behaves the same in the comparable modes. **Defender quarantined `ogrebattle64.exe` four times** (`Behavior:Win32/DefenseEvasion.A!ml`) — twice from the `CreateRemoteThread` fault stub, twice from the reproducer's handles-cleared launch — and the file, byte-identical to the release (sha256 `de69ff2a…`), was restored from the release archive each time; the reproducer was deleted rather than landed. See `docs/HANDOFF-2026-09-30-session119.md`.

- ✅ **The v0.4.0 Windows crash is confirmed fixed by a release, and the D3D12 backend that faulted starts cleanly on the reporting laptop (session 113).** Developer report: the package crashes as soon as the ROM is selected and the game starts, on a GeForce 940MX laptop with an updated driver; `error.log` reads `unhandled exception 0xC0000005`, `fault instruction ...\ogrebattle64.exe+0x10001AE`, `fault address 0x6C`, `access read`. That binary is the **v0.4.0** release (24,655,872 bytes, the size session 107 records for it) and RVA `0x10001AE` is `80 7A 6C 01` = `cmpb $1, 0x6C(%rdx)`, session 107's `plume::d3d12::D3D12CommandList::setSamplePositions` with a null `texture` from the presented-frame readback, so the crash is that defect and no new one. The replacement package (downloaded `ogre-battle-64-recomp-windows-x86_64(1).zip`, exe 24,657,408 bytes, PE timestamp 2026-09-28) carries the fix: the pre-fix `OGRE_CAPTURE_PRESENT=` string is absent and session 107's `capture path:` and `the environment sets OGRE_CAPTURE_PRESENT` strings are present, and its archive carries both `.nrm` mods, so it is newer than v0.4.0's empty `mods/` (session 108). Run on the developer's package folder with `OGRE_EXIT_AFTER_MS=22000 OGRE_SPEED=4`: the window `Ogre Battle 64: Recomp` responds at 13 s, `dxgi.dll`/`d3d12.dll`/`D3D12Core.dll` are loaded and `vulkan-1.dll` is not, so RT64 initialized D3D12 and took no workaround fallback; exit 0 after 19.3 s with `error.log` keeping its pre-run timestamp. This is the Windows run session 107 §6 could not make, and item 1(b) no longer reproduces either: the adapters report Intel `31.0.101.2140` and NVIDIA `32.0.15.8266` (582.66), both above session 94e's fallback thresholds. The battery save was copied to `%TEMP%` first. **Still open:** captured `stdout`/`stderr` is 0 bytes on Windows in both runs, so a report still cannot name the device, the driver or the fallback line (session 107 §7). See `docs/HANDOFF-2026-09-30-session113.md`.

- ✅ **An experiment: a PNG pack under `mods/backgrounds/` replaces a whole dialogue/cutscene backdrop, starting with the New Game cathedral (session 114).** Developer request: a `mods/backgrounds/` directory of `NN.png` files with mappings like `01 -> cathedral`, for dialogue/cutscene screens and combat screens. Agreed before building: the first example swaps the game's own 320x240 backdrop pixels (a native-res reskin), keyed by scene + step. The backdrop is four `njpeg` sub-images, and `func_ovlE_8019976C` (bank unit E) copies one per pass into `0x801AAE90` (320x240), `0x801D06B0` (176x240), `0x801E50D0` (320x144) and `0x801FB8F0` (176x144). A `.nrm` mod cannot hook a bank unit (mod hooks resolve only in `.entry`/`.main`/`.streamedA/B/C`), so `tools/hd_backgrounds.py` inserts `ogre_hd_background(rdram)` at the end of the pass copy, beside `ogre_sync_framebuffers()`, and `make bank-recomp` re-applies it. `app/src/hd_backgrounds.cpp` reads the readback state at `*(0x8019A680)` (`+0x74` destination, `+0x78`/`+0x7A` size, `+0x80` header flag), matches the pack on `D_8018F1C0`, and writes big-endian RGBA5551. The four passes are the cathedral asset's four chunks (ROM `0x7CADAC`+8: `(-229,-235)`, `(90,-235)`, `(-229,4)`, `(90,4)` at 320x240/176x240/320x144/176x144), a 496x384 canvas, so the PNG is box-filtered to 496x384 and split `(0,0)`/`(320,0)`/`(0,240)`/`(320,240)`; pass-0-only left the other three showing the game's art. At the readback the scene word is still the `0x02` loader (`D_800C4C26` held a control value, `D_8018F1C2` held `0x0002`), so the step identifies the image. `OGRE_BG_DEBUG=1` painted the passes red/green/blue/yellow; the measured regions fit one affine map of the canvas (offset `(-57,-216)`, scale `(2.028, 2.229)`), so the game zooms and pans the canvas and crops its top-left. Verified: four `[hdbg] applied ... chunk` lines, and a presented-frame capture of the cathedral with the pack art over the whole backdrop under the game's sprites and dialogue box. `mods/backgrounds/01.png` is a generated placeholder. This is a reskin at the game's resolution, not an HD texture replacement. **Widescreen fill:** with WIDESCREEN on the scene was pillarboxed in the 16:9 window; a scene the pack covers is now added to RT64's Expand set (`scene_expands` in `app/src/widescreen.cpp` calls `hd_backgrounds_covers_scene`), so the backdrop fills the width and reveals more of the canvas at the sides while the dialogue box and text keep their size. `OGRE_WS_SCENES` replaces the default expand set (the mission `0x03`). **Reference extraction:** `OGRE_BG_DUMP=<dir>` makes the app assemble the game's own four sub-images into the 496x384 canvas and write one PNG per assembly, before the pack replaces anything (independent of the pack); `tools/backgrounds.py` and `make bg-extract` drive a run from the mapping and file the result under the gitignored `mods/backgrounds/reference/`. The dump is a live capture, not an offline decode, because the sub-images are Huffman-coded `njpeg` and only the game's RSP microcode decodes them. **Widescreen variants:** a `<id>-wide.png` beside the plain image is used when WIDESCREEN is on and is authored at 16:9; the port scales it to the canvas width **preserving its aspect** rather than stretching it to 496x384 (a 1:1 circle in a 16:9 source measured 0.71 aspect stretched, and is round fitted). The selection reads `widescreen_mode()` because the readback runs one scene early. The placeholder pair is `01.png`/`01-wide.png`, both written by `make-example.py`, and the wide one's round rose window is the aspect check. `OGRE_BG=0` (also `off`/`false`/`no`) runs without the pack, and an explicit `OGRE_BG_DIR` is the only folder searched. Scene `0x0D` is in the default Expand set with or without the pack, because the game's own 496x384 backdrop has content the 4:3 view crops. See `docs/guides/hd-backgrounds.md` and `docs/HANDOFF-2026-09-29-session114.md`.
- ✅ **The unit list's slow screen was the session-80 class a fourth time; the branch is `0x8020A910` (session 112).** Developer report: opening the list of units at a location with several units slows the game down a lot, with a 32768-byte battery save that resumes into scene `0x03`. `OGRE_PROFILE=1` reproduced the phase twice (mission at 12–13 display lists/s, median 79 ms gap, against 60–110/s when the list was closed) and named `0x80207230` at 79–99 % of the frame-pump thread `t4`, but that address is not the defect: `recomp_trace_entry` sets `last_func_vram[tid]` on every entry and `recomp_trace_return` never restores it (`tools/N64ModernRuntime/ultramodern/src/function_trace.cpp:262`), so the sampler reports the most recently entered function. A host `sample <pid> 4` put 1608 of 1670 `t4` samples in `yield_self` under `func_ovlN_80208E84`, whose inner loop at `0x8020A814..0x8020A910` walks five 1-byte records with the bound `a3 = a1 + 5` set at `0x8020A80C`; the recompiler's poll-loop heuristic keyed on the loop-invariant load `lbu $v0, 4($s6)` at `0x8020A820` and injected a blocking `yield_self` at the back edge, so every taken back edge cost about one VI retrace. `config/banks/config-bankN.toml`'s `yield_work_loop_branches` gained `0x8020A910` (after `0x801B3008`, `0x801DA2C0`, `0x801D0C08`); regeneration removes exactly one site (49 → 48). Fixed, same save and trigger: 1032 display lists in 10.5 s of scene `0x03` (~98/s, median 8 ms gap), 0 crashes/stubs/UNKNOWN, `[cover] 0x80208E84 273` so the scan still runs, `check-banks` OK, `elf-rom-check` 0 differing bytes, `build-null` 45 s boot `runlog.py --check` PASS. The raw ROM word at `0x8020A910` is `0x1440FFC0`. **Developer-confirmed.** See `docs/HANDOFF-2026-09-27-session112.md`.
- ✅ **The hosted release failed on all three platforms because the private data bundle still predated the mods rule, and the unpack check is now a drift check (session 111).** Developer report: a `workflow_dispatch` run of `Release` failed every matrix job with `build/mods/*.nrm is missing from files.tar.gz; run 'make example-mods' before tools/data-bundle.sh`. `OGRE_DATA_REPO` is `lfarroco/ogre-recomp-private` (`gh variable list`), so each job takes the hosted path and unpacks that repository's `files.tar.gz`; a clone of it holds the Sep 19 archive (sha `663a238d…`, 188 entries, byte-identical to the old `dist/ogre-data/files.tar.gz`) with no `build/mods/` member at all. Session 108 made `build/mods/*.nrm` a required release input and added the workflow check, but nothing regenerated the private bundle, so the check fired on the next run. The last green run (v0.4.0, Sep 26) predates the check. Fixed: `tools/data-bundle.sh` regenerated from this tree with both mods (190 entries, `build/mods/exp-overflow.nrm` and `skip-boot-logos.nrm`, no AppleDouble members, sha `f2c482e1…`), committed to the private repository as `4bd61cb` with its README updated, and the failed run re-run. The generated code is not merely assumed current: a clean `make recomp` and `make bank-recomp` reproduced the pre-existing `RecompiledFuncs/`, all 34 `Bank*Funcs/` and `app/src/bank_funcs.inc` byte for byte, and the new bundle matches them. The unpack step's commit check changed from a tag-only `::warning::` to a failure for any build whose `ogre-data.txt` commit differs, printing how many commits newer the build is and the regeneration command; the public checkout already used `fetch-depth: 0`, so the check is decidable. Exercised locally on a stale commit (47 behind), a matching commit and an unrelated commit. The re-run completed success on all three platforms, and its Release job produced `v0.5.0` as a draft whose Linux and macOS archives both contain `mods/exp-overflow.nrm` and `mods/skip-boot-logos.nrm`: the property whose absence made v0.4.0 ship an empty `mods/`. Publishing the draft is the developer's decision; the published v0.4.0 assets are unchanged. See `docs/HANDOFF-2026-09-27-session111.md`.

- ✅ **The overlay's second SDL window broke the game window's close button, and the panel's MAIN tab now carries EXIT GAME (session 110).** Two developer requests on macOS. (1) A fresh run closes on the title bar's X, but `Esc` open + `Esc` close then makes the X do nothing. The cause is SDL's own rule: `SDL_SendWindowEvent` synthesises `SDL_QUIT` from a window's `SDL_WINDOWEVENT_CLOSE` only while that window is the last one in SDL's global window list (`tools/SDL2-static/SDL2-2.32.10/src/events/SDL_windowevents.c:221-227`), and the overlay is a second SDL window that is hidden and shown again rather than destroyed, so the first time it is created the game window stops being last. `pump_sdl_events` (`app/src/sdl_platform.cpp`) now quits on a `SDL_WINDOWEVENT_CLOSE` whose `windowID` is the game window's and ignores one for the overlay window, where `Esc` is the dismiss. (2) `ui::Tab::Start` is `Tab::Main` (`MAIN`), `Panel::tab_enabled` enables every overlay tab except MODS, `RowKind::StartGame`/`RowKind::ExitGame` are the MAIN rows (Exit Game is the MAIN tab's last row, after Start Game and the ROM row, so a stray SPACE or ENTER cannot reach it; `Panel::selected_is_exit()` makes ENTER on it quit instead of starting the game), and `RowAction::QuitGame` replaces the unused `CloseOverlay`: the launcher stops its loop on it and the overlay records a request that `update_gfx` polls next to the window-close flag, so both exits share main.cpp's dumps and `ultramodern::quit`. Routing the overlay exit at the runtime exposed a pre-existing defect the overlay was the first thing to reach: `ultramodern::quit` while the game thread is still inside `ultramodern::preinit` (or while RT64 is still building its renderer) leaves `recomp::start` blocked in `game_thread.join()` forever, and once RT64 finishes initialising it instead throws out of the shutdown (`reason: std::terminate`); measured with temporary `[probe151]`/`[probeRT]` prints, both reverted. `main.cpp` now reads the game's own frame counter (`D_800AEFA4`, `0` at 6 s and `0x327` at 12 s of a boot) and, on an interactive exit before 60 frames, prints the same dumps and leaves with `_Exit(EXIT_SUCCESS)` instead of running the runtime's teardown — the rule `OGRE_EXIT_AFTER_MS` already uses. `OGRE_WINDOW_CLOSE_AT_MS=<n>` pushes a synthetic close event for scripted verification. Verified on `build-dist/ogrebattle64` (macOS): six exit paths (overlay EXIT GAME early and mid-run, window close at 9 s and 20 s with and without the overlay, `OGRE_EXIT_AFTER_MS`) all exit 0 with no `error.log`; `Down,Space` on MAIN never quits; launcher Exit Game quits without booting and Start Game still boots; all nine tab captures render; `OGRE_DUMP_RDRAM` still writes 8 MiB from the early path. Proofs refreshed, `native-launcher-main.png` and `native-overlay-main.png` added, `native-launcher-start.png` deleted. The runtime defect itself is not fixed (vendored `N64ModernRuntime`), only avoided. See `docs/HANDOFF-2026-09-27-session110.md`.

- ✅ **The browser build runs the game again, and the WebGL2 prototype is retired as the browser renderer route (session 109).** Developer question: the browser renderer was left unfinished when the project moved to the native port; can that work resume? Two defects blocked the wasm build and neither was in the renderer. `app/src/bank_overlays.cpp` included the native-only `sdl_platform.hpp` (added 2026-09-16 for the live console) and `sdl_platform.cpp` is not in the Emscripten source list, so `cmake --build build-wasm` failed on it; the include and its one call are now `#ifndef __EMSCRIPTEN__`. `app/src/main_web.cpp` registered `recomp::SaveType::None` while `app/src/main.cpp` registers `Sram`, so the game's first SRAM DMA (`base=0x08000000`) took the `!recomp::sram_allowed()` branch in `librecomp/src/pi.cpp:143`, printed `Attempted to use SRAM saving with other save type` and called `ULTRAMODERN_QUICK_EXIT()` at about 350 ms of boot. The runtime's host threads outlive that exit, so the page kept printing VI retraces and queue snapshots: `extbacklog` grew at 60/s, the `resumes` counters and `retrace(D_800C4BCC)` froze, and `running_queue` held `t200(pri5)`. That is an exited game, not the lost scheduler wakeup the first reading of the run suggested; the same tree's native null build submits 2100 display lists in a 20 s run at `OGRE_SPEED=4`. With the two fixes the build links (21 MB wasm), `debug/probes/boot.cjs` reports `RENDERED` (24840 non-black pixels; the title, `docs/proofs/web-title.png`), `debug/probes/progress.cjs` reaches `maxTasks=40 badWalk=no` with 1142 triangles and 5 unique combiners, and the recompiled audio microcode runs with the web layer muted until `?audio`. The WebGL2 prototype still cannot draw the game: `WebGLRenderer::send_dl` always runs the F3DEX2 walker and `app/src/gbi.hpp` reads `0xDA`/`0xDC` as `G_MTX`/`G_MOVEMEM`, which in an S2DEX2 list are the njpeg macroblock draw `G_OBJ_RECTANGLE_R` and `G_OBJ_MOVEMEM`; there is no YUV16 path (`grep -ci yuv app/src/web_renderer.cpp` is 0); and there is no RDRAM framebuffer indirection (`update_screen()` is empty, and scene `0x02` assembles its background from CPU copies of rendered framebuffers in RDRAM). The title's sprite layer draws and its 3D content and the menu screens render wrong (`docs/proofs/web-opening-broken.png`). Decision recorded in `docs/DECISIONS.md`: a browser renderer is scoped as RT64 with a WebGPU backend (plume has Vulkan, D3D12 and Metal backends and no WebGPU one; RT64's HLE and render layers are ~21000 lines of portable C++ and its 3.5k lines of shaders are HLSL), extending the 3380-line prototype was rejected because the framebuffer indirection is architectural, and the wasm target belongs in CI — it is not in CI, which is why it stayed broken for eleven days. See `docs/HANDOFF-2026-09-27-session109.md`.
- ✅ **The published release shipped an empty `mods/` directory, and the packaging path now fails rather than warning (session 108).** Developer report from the releases page. The release does not run on a developer machine: the Actions variable `OGRE_DATA_REPO` is `lfarroco/ogre-recomp-private` (`gh variable list`), so `runs-on` takes the hosted branch and each job unpacks `files.tar.gz` from that private repository. Evidence at three points: `tools/data-bundle.sh`'s tar command named only `ogre-data.txt RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc` and its `missing` check did not test for mods, so `tar tzf` on a clone of the bundle lists no `build/mods` and no `.nrm`; `gh release download v0.4.0 --pattern ogre-battle-64-recomp-macos-arm64.tar.gz` then `tar tzf` prints `ogre-battle-64-recomp/mods/` and `ogre-battle-64-recomp/mod_config/` as the only matches, both empty directories; and the `dist` recipe's copy step was `if ls build/mods/*.nrm >/dev/null 2>&1; then cp ...; else echo "no example mods in build/mods (run 'make example-mods' first)"; fi`, an `else` arm that exits 0, so the package was produced with an empty `mods/`. `tools/smoke-dist.sh` checked the binary, the README and (on Windows) the DLLs and never the directory's contents. Fixed at four layers: a new `dist-mods-check` target is a prerequisite of `dist` and fails with the `make example-mods` instruction when `build/mods/*.nrm` is absent (the copy is unconditional now); `tools/data-bundle.sh` requires `build/mods/*.nrm` and packs it, so the archive carries the mods the runner cannot build (no ROM, no `mips-linux-gnu-gcc`, and a macOS runner has no Linux container for `tools/build-example-mods.sh`'s Docker fallback); `tools/release-build.sh` checks for them before `make dist` so a stale bundle fails before the app build; the workflow's unpack step fails a bundle without them; and `tools/smoke-dist.sh` fails a package whose `mods/` holds no `.nrm`, which is the check that catches a release built from an old bundle. `packaging/README-dist.txt` had said "one example mod" and named only `skip-boot-logos.nrm`; session 103 added `exp-overflow` and `make dist` copies every `.nrm`, so it lists both. **Verified:** `bash -n` on the three scripts; with `build/mods` moved aside `make dist DIST_OS=macos` exits 2 with the new message and never reaches the app build, and `tools/release-build.sh` exits 1 with the bundle instruction; on the happy path `make dist` copies `exp-overflow.nrm` and `skip-boot-logos.nrm`; `tools/data-bundle.sh /tmp/ogre-bundle-check/files.tar.gz` reports `Example mods: 2 .nrm` and `tar tzf` lists both; `tools/smoke-dist.sh` PASSes with `the package carries 2 example mod(s)` and fails a copy with `mods/*.nrm` deleted; `ruby -ryaml` parses the workflow. **Open:** the published v0.4.0 assets still carry the empty directory, and the private `files.tar.gz` must be regenerated after this change is committed and a new tag published. See `docs/HANDOFF-2026-09-27-session108.md`.
- ✅ **The v0.4.0 Windows access violation is the presented-frame capture reading sample positions out of a null texture, and the capture was on because `putenv` copies on Windows (session 107).** The developer pasted one report (`0xC0000005`, `ogrebattle64.exe+0x10001AE`, read of `0x6C`, empty log sections) and said v0.3.0 worked. The release asset for v0.4.0 has `.text` RVA `0x1000..0x12D3066`, and `0x10001AE` is `cmpb $1, 0x6C(%rdx)` — the only such instruction in the image, always a function's `+0xE` byte, at RVA `0xFF8AC0` in rc4/rc5, `0xFF8ED0` in v0.3.0 and `0x10001A0` in v0.4.0. The function is `plume::d3d12::D3D12CommandList::setSamplePositions` (`plume_d3d12.cpp:2483`) and `rdx`, its `texture` argument, is null: `+0x6C` is `desc.multisampling.sampleLocationsEnabled` (`desc` at `+0x40`, `multisampling` at `+0x48`, `sampleCount` `+0x48`, `sampleLocations[16]` `+0x4C`), the TLS vector at `+0x770` is `std::vector<D3D12_SAMPLE_POSITION>`, and the `(size, 1, data)` / `(0, 0, nullptr)` calls are `SetSamplePositions` and the inlined `resetSamplePositions`. Its unguarded callers are `copyTexture`, `resolveTexture`, `resolveTextureRegion` and `copyTextureRegion` (the four `checkFramebufferSamplePositions` sites do guard); in `copyTextureRegion` the compiled order runs `toD3D12` first, which reads a subresource texture at `+0x40`, so only a `PlacedFootprint` (buffer) destination reaches `setSamplePositions` with a null and faults at `+0x6C`. The only footprint-destination calls are the port's own readbacks (`rt64_present_queue.cpp:547`/`:592`), and they ran because `init_capture_env` installed `OGRE_CAPTURE_PRESENT` with `putenv` and hid it by rewriting the first byte of the name — POSIX `putenv` keeps the caller's pointer, the UCRT's `_putenv` copies the string and owns the copy, so the entry stayed visible and the first present issued the readback. That dates the regression: `init_capture_env` came in `bb01fc6` (2026-09-25), not an ancestor of v0.3.0, and the buffer string is in the v0.4.0 exe and in no earlier one. Fixed: `copyTextureRegion` skips `setSamplePositions` for a non-texture destination; `sdl_platform.cpp` owns the path and `snap` toggles it through the new `extern "C" const char* ogre_present_capture_path()` that `rt64_present_queue.cpp` calls, so no toggle frees a string a present thread is reading; `main.cpp`'s smoke path prints the resolved path and `tools/smoke-dist.sh` fails a package whose capture is on. Verified on macOS: three builds clean, `smoke-dist.sh` PASS, the 30 s boot battery `runlog.py --check` PASS, `snap` still writes and stops `/tmp/ogre-shot.<n>.ppm`, and a user-set `OGRE_CAPTURE_PRESENT` wins. Not verified: a Windows run (no MSVC or D3D12 here; the packaged smoke run cannot boot a ROM on CI). See `docs/HANDOFF-2026-09-27-session107.md`.
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

- **Session 181**: measured the QUEST still's q draws against RT64. The draws
  rasterize and the reference frame is produced (`white=279632, coloured=20588`,
  and the canvas read back after the blit shows it) when a flush carries one
  display list; a normal-speed run with two lists per flush presents
  `coloured=0`. Native holds the white+q+wordmark frame about 12 presents.
  Ruled out for the q draws: NDC bounds, NaN, viewport 640x481, scissor
  `(0,1,640,480)`, depth, blend, alpha compare, GL errors, FBO binding, texture
  keys. Found a WebGL feedback-loop bug: `upload_framebuffer`/`blit_to_canvas`
  leave the framebuffer's own texture bound, and 86 fills + 16 texrects per run
  are dropped with `INVALID_OPERATION` (0x0502). No code landed; the next
  experiment is the per-list batch boundary. See
  `docs/HANDOFF-2026-10-19-session181.md`.

- **Session 189**: ported RT64's `G_CD` writeback dither, fixed the session-181
  texture feedback loop, and keyed tile images by content. The dither control
  (`testdraw.cjs --flags 1024 --expect-dither`) reads
  `no_dither=67200 dithered=9600 other=0` on a `G_CD_MAGICSQ` target and
  `no_dither=76800 dithered=0` on `G_CD_DISABLE`; the pre-fix build read
  `other=76800` on both because the fill was rejected with
  `INVALID_OPERATION`. `upload_framebuffer` now unbinds the colour target's
  texture and every draw binds the 1x1 white texture first; the fill reads
  `(0.129,0.161,0.192)` with error 0, and a new `[GFX-GLERR]` line reports the
  first GL error of a flush. The tile-image GL key is `XXH3-64` of the decoded
  RGBA bytes plus size and wrap fields; the upload loop skips a resident key,
  so `--flags 2048 --repeat 3` measures `upload=1 reuse=2`, and
  `ogre_gfx_stats()` reports `tex: cache=N uploads=N reused=N released=N`. An
  S2DEX2 audit found that OB64 uses only `0xDA`/`0xDC`; `0x03`..`0x0B` now
  return instead of falling into the F3DEX2 triangle arms. `make web` clean;
  the dither/fbtex/depth/alpha/cache controls pass. The title logo/copyright
  residual is closed as fog phase: two native logo frames 0.25 s apart differ
  by `7.01/7.03/8.77` (logo) and `9.78/9.78/10.19` (copyright), more than the
  browser-vs-native pair (`4.33/4.37/5.39`, `6.09/6.08/6.35`) and with the same
  signed direction. The mission `0x03` party sprites render now: the save
  route's map field shows both clusters
  (`docs/proofs/web-mission-party-session189.png`), against session 176's
  missing party; the unit sprites' `OTHERMODE_L` has alpha compare none, so the
  feedback fix is the change that can restore a rejected draw. The decode still
  runs once per display list. The Load Game white frame did not reproduce in a
  4 Hz capture of the transition (20 shots, none white); if it exists it is
  shorter than 250 ms. See `docs/HANDOFF-2026-10-24-session189.md`.

- **Session 190**: split the browser tile cache into two tiers. A pre-decode
  `tile_source_key` hashes a superset of the TMEM bytes a tile can read (the
  decoder's `(rgba32 || indexed) ? 0x7FF : 0xFFF` mask, the decoder's byte
  shift, the 2 KiB palette half for an indexed tile, and `convert_k` only for
  YUV16), and `decoded_tiles_` keeps the decoded pixels in 16 MiB, so a hit
  skips `decode_tmem_tile`. The GL texture cache keeps a texture for eight
  flushes with a 32 MiB / 16 384-entry cap instead of sweeping everything the
  batch did not name, so an alternating-frame texture survives; an LRU eviction
  is repaired from the decoded pixels. `testdraw --flags 2048 --repeat 3` gives
  `upload=1 reuse=2 decode_hit=2`; `--gap 64 --repeat 4` gives
  `upload=1 reuse=3 decode_hit=3`; `--flags 6144 --repeat 3 --gap 64` changes
  the source each call and gives `keys=3 miss=3 hit=0`; `OGRE_TEX_TRACE=1`
  re-decodes every hit and reports 0 `[GFX-TEXCACHE-MISMATCH]`. A natural Load
  Game run reached scene `0x0012` and reported `decodes=309 hits=952
  mismatches=0`. The session also withdraws session 189's "4 Hz" Load Game
  series: `atmark.cjs` waits after a Playwright screenshot that costs 0.95-9.5 s,
  so its nominal 250 ms series spans tens of seconds. New
  `debug/probes/atmarkfast.cjs` samples `gl.readPixels` after every flush,
  armed by `[scene]` lines. The successful run's two distinct frames are the
  Load Game page (`mean3=120.86`) and a black frame (`mean3=0`), no white;
  other attempts landed on the idle trajectory under host load 16-27. A native
  RT64 run of the same route captured 184 targets through the `0x12` entry and
  also has no white frame (highest `meanPC=97.73`, highest `fracW240=0.00445`).
  The browser's exact entry instant and the `0x12 -> 0x03` transition are unrun.
  See `docs/HANDOFF-2026-10-25-session190.md`.

- **Session 192**: the mission's 3D ucode is `F3DEX2.NoN.fifo 2.08`
  (`ucode=0x800A08D0`, text hash `0x4C12DAE0534D7135`). RT64 maps the hash to
  `GBIUCode::F3DEX2` with `flags.NoN`; `NoN` only disables the F3D
  near-depth clip (`depthClipEnabled = !NoN`). The browser printed
  `[WEB-GBI] unknown ucode` and fell back to plain F3DEX2, so its `u_zclip` still
  dropped fragments outside `[0, 1022/1024]`. The hash is now in
  `kGbiTextSegments` as `GbiKind::F3DEX2_NON_FIFO`, a draw from that list carries
  `no_near_clip`, and `u_zclip` stays 0. The `[WEB-GBI] unknown` line is gone
  from the mission run. The green lines around the framed target-fort
  illustration are a separate, unfixed path: the frame art is assembled from
  `G_TEXRECT`s whose sources are inside colour image `0x228080` (24
  `[GFX-FBTEX]` loads at scene `0x03`). The browser reads stale RDRAM; RT64 makes
  a per-tile GPU copy with a `RAMHash` stale-RDRAM guard. A pre-load
  `sync_framebuffers()` whole-image writeback was tried and crashed in
  `func_ovlN_801F7C60` (the image's bytes also hold game textures), so it was
  reverted. A browser whole-RDRAM dump of the same mission renders the same
  garbage frame with `tools/ogre_frame.py`, against the native dump's gold frame
  (`docs/proofs/web-mission-intro-rdram-session192.png`,
  `native-mission-intro-rdram-session192.png`). The mission's field is still
  flatter and darker than native; its light is blue (`amb=77,77,127`) and it is
  not explained by the frame's tile copy. New bounded diagnostics: per-scene
  `[GFX-TDSTATE]` and `[GFX-FRAME-RECT]`. `make web` clean; final wasm
  `21197859` bytes. See `docs/HANDOFF-2026-10-25-session192.md`.

- **Session 193**: implemented the framebuffer-as-texture copy session 192 asked
  for, as an opt-in path (`OGRE_FBTEX=1`) that reads a colour image's GPU pixels
  into a private buffer and never writes RDRAM. A same-screen A/B on the mission
  briefing shows it must stay off: off, the NOTE and label read correctly; on,
  they become white bars. The copy of image `0x228080` decodes as a white 320x240
  frame (`[GFX-FBCONTENT]` seven full-image rect draws), while RDRAM holds the
  CPU-composed briefing, so the loads were reading the right bytes and session
  192's premise is corrected. RT64's tile copy depends on its RDRAM re-upload
  (`checkRAM`/`uploadRAM`) and per-workload writeback, which the browser does not
  have; that model is the prerequisite for enabling the copy. The green lines
  around the framed illustration persist with the copy on and off, so they are
  not stale RDRAM; the next lead is the CI4 decode of the 1-row border strips.
  A mid-list RDRAM writeback was tried first and reproduced the `func_80071950`
  heap crash, so it was reverted. `make web` clean; default mission run does not
  crash. See `docs/HANDOFF-2026-10-25-session193.md`.
- **Session 194**: continued session 193's green-line item and corrected its
  lead. The briefing's framed illustration is two RGBA16 images the game
  composes in RDRAM (an L-shaped 19-band frame of 91x4 rects, `line=23`,
  `lrs=364 lrt=12`, `mask=7/5`, `cm=0/0`; an 84x67 picture in three blocks,
  `line=21`; a 20x20 pin), and their transparent texel is `0x07C0`, pure green
  with alpha 0. RT64's `loadTileOperation` on the dumped source reproduces the
  dumped TMEM over all 736 bytes the load writes, and the decode reproduces the
  source texels exactly, so the green is the game's data and not a decode fault.
  The hairline is the last row of each band: 4 rows over 4 pixels makes the last
  pixel centre `t=3.5`, whose bilinear taps are rows 3 and 4; row 4 is outside
  the band and holds the previous occupant of that TMEM base, so the transparent
  green is mixed with an opaque texel and written at alpha near 0.5. Forcing
  alpha blending off turns the whole transparent region green, the control for
  that reading. `OGRE_TEXWINCLAMP=1` bounds a wrapped sample to the tile's rect
  and removes the band-seam hairlines and the notch stripes, but not the frame's
  own outline, which is filtered across the boundary between opaque art and the
  same image's transparent green texels; it stays off by default until the
  `OGRE_FOG` layers and the map's water are compared with it on. New
  diagnostics: `OGRE_FRAME_DUMP` (source bytes, TMEM and decoded image at the
  strip draw), `debug/probes/framedump.cjs` and `debug/probes/stripab.cjs`.
  `make web` clean. See `docs/HANDOFF-2026-10-25-session194.md`.
- **Session 195**: closed session 194's two open items by finding the cause of the
  briefing's green lines, which is the browser renderer's rectangle sampling.
  RT64 draws every rectangle shifted half a pixel right and down
  (`halfPixelOffset` in `rt64_framebuffer_renderer.cpp`; one pixel in NDC is
  `2 / viewport`), so pixel `i` of a 1:1 `G_TEXRECT` samples texel `i`. The
  browser renderer's corner UVs sit on the rectangle's edges and its fragments
  sample at pixel centres, so pixel `i` sampled `us + (i + 0.5) * step`: every
  filtered rectangle was half a texel low and right, a 4-row band read the row
  past its art and an image's right/bottom edge mixed its transparent green
  surround into the opaque art. `OGRE_RECT_HALFPIX` (on by default; `=0` is the
  old behaviour) subtracts half a step from the corner UVs of `draw_texrect` and
  of the S2DEX object rectangle, which is the same sample coordinate as RT64's
  shifted quad and leaves the rectangle's coverage unchanged. Same screen, one
  build: 0 green-dominant pixels in the illustration rectangle with the switch
  on against 398 with it off, the 213 fully opaque samples of the top band exact
  in 213/213 against 0/213, and the region's mean absolute difference from
  RT64's own 320x240 framebuffer 2.44 in two on runs against 22.21 and 22.20 in
  two off runs. The map's upper band also improves (3.02/3.06 against
  8.06/7.94); the NOTE band and the map's lower band do not move. RT64's
  framebuffer confirms the convention: 144/213 samples exactly the decoded texel
  and the other 69 differ by one 5-bit step in one channel (writeback dither).
  `OGRE_TEXWINCLAMP` stays an off-by-default diagnostic. Session 194's native runs that landed on the map had reused a
  battery the game had rewritten; a fresh `OGRE_PREF_DIR` and the route
  `title:start:6,0x12:a:6` reach the briefing. New probe:
  `debug/probes/rectab.cjs`. `make web` clean. See
  `docs/HANDOFF-2026-10-25-session195.md`.
- **Session 196**: withdrew session 195's "the mission field is flatter and
  darker than native". At the same Castle Dunkweld NOTE screen the browser's
  static field regions are within 1-3 levels of RT64's mean and their standard
  deviation and gradient are higher: left field mean `[26.8,28.4,13.3]` against
  `[26.3,27.8,12.4]`, standard deviation `[16.9,16.0,17.3]` against
  `[13.9,12.6,13.8]`, mean absolute difference 4.82 (right 5.25, bottom 6.19,
  top-left 1.20, top-right 2.09). The Castle Burgund briefing reads the same way
  (left field `[30,32,1]` against `[30,33,3]`). `OGRE_RECT_HALFPIX` changes no
  pixel on the boot intro or a publisher still: the static holds are
  byte-identical between the switch-on and switch-off runs
  (`md5 6ffb91f8…` and `880a77a4…`), so session 195's item 1 is closed for those
  screens and the briefing. A live switch, debug flag bit 15
  (`ogre_gfx_debug_flags(32768)`), and `debug/probes/recttoggle.cjs` were added
  for a same-run A/B; two attempts waited 33 and 36 minutes under host load
  11-28 without reaching the briefing, so that pair is still to run. The
  mission's framed illustration is clean on this build
  (`docs/proofs/web-mission-briefing-illustration-session196.png`), and the
  field/browser/native side-by-side is
  `docs/proofs/web-mission-field-vs-native-session196.png`. `make web` clean.
  See `docs/HANDOFF-2026-10-26-session196.md`.
- **Session 197**: ran the same-run `OGRE_RECT_HALFPIX` A/B and applied RT64's
  stale-RDRAM test to `OGRE_FBTEX`. The probe harness's SwiftShader launch was
  the blocker: with `--enable-gpu --use-angle=metal` (now in
  `debug/probes/recttoggle.cjs`) the briefing is reached in about seven minutes
  instead of 33-36. The arrival signal is the illustration's
  `[GFX-FRAME-DUMP]` line and the route `title:start:6,0x12:a:6`; a scene-line
  trigger crosses the briefing's NOTE-page change. Same run: switch-on 0 green
  pixels in the illustration rectangle, switch-off 399 (175 seam rows),
  switch-on-again 0. Proofs
  `docs/proofs/web-mission-briefing-recttoggle-{on,off,zoom}-session197.png`.
  `GlFramebuffer` now keeps an XXH3-64 of its RDRAM at the last
  upload/writeback; the gfx thread refuses a framebuffer-as-texture copy when
  the bytes changed (`[GFX-FBTEX-STALE]`, one hash and no flush round trip), and
  `build_image_source` repeats the check. With `OGRE_FBTEX=1` the briefing's
  NOTE and label match the copy-off frame at `meanAbs 0.057`/`0.024` (max 1)
  instead of white bars; four refusals, no sync timeout, no crash. The copy
  stays opt-in. A read-only audit corrected the top-of-file limitations
  (TEXEL1, `G_TEXTURE_GEN`, lighting and LOADBLOCK `dxt` are implemented) and
  found `G_TEXRECTFLIP`'s half-step wrong for non-square flips, copy-mode
  texrect and fractional-rect differences, and the title residual's real range
  (rows ~81..119). `map` is not a scene name and the `OGRE_SCENE=0x05`
  retry did not reach scene `0x05`; `OGRE_SCENE=title` landed on `0x09`/`0x0A`
  in session 196, and a natural title capture landed on the idle trajectory
  (`id=0x0000`). Those checks need a played state or a retry. `make web`
  clean. See `docs/HANDOFF-2026-10-26-session197.md`.
- **Session 201**: fixed the tutorial practice field's ground in the browser
  renderer, in two steps. A triangle that crosses the eye plane was dropped
  whole: `transform_vertex` failed on a vertex with clip `w <= 0` and `draw_tri`
  dropped the triangle, and the field's ground is one 76x76-unit quad whose near
  corner is at `w = -1.67`, so both of its triangles never drew and the ground
  was the frame's fill. `emit_clipped_tri` now cuts such a triangle against
  `w > 1e-3` and emits the resulting polygon. The ground then drew, but its tile
  was stretched into straight bands because the texture coordinate was
  interpolated in screen space while RT64 interpolates it perspective-correctly
  (`hle/rt64_rsp.cpp` stores it undivided, `shaders/RasterVS.hlsl` multiplies the
  position by `w`); the vertex shader now writes `vec4(a_pos * a_w, a_z * a_w,
  a_w)` by default, with `OGRE_PERSP=0` as the A/B. Grass detail on the same
  region: 341 distinct colours and 1.77 neighbour difference before, 428/2.48
  with clipping and screen-space UVs, 441/2.38 with perspective, against the
  native capture's 726/2.32. The mission briefing and the title measure within
  1.4 mean absolute difference of their screen-space frames, so session 172's
  screen-space choice is reversed for triangles. New toolkit diagnostics:
  `OGRE_DRAW_PROBE` with `_SCENE`/`_AFTER_TASK`, the `[GFX-FAMILY]` census,
  `OGRE_ONLY_COMB`, and a `1<<17` alpha view. Proofs
  `docs/proofs/web-tutorial-field-{flat,bands,}-session201.png`. See
  `docs/HANDOFF-2026-10-26-session201.md`.

Sessions 202-207 are recorded in their handoffs (`docs/HANDOFF-2026-10-26-session202.md`
through `docs/HANDOFF-2026-10-31-session207.md`); this log resumes here.

- **Session 208**: the browser build now reaches GAME SPEED 2x and 4x, and the
  page holds 60 Hz while it does. Measured on one page with the speed changed
  live (mission screen, `assets/escort_suspend.bin`, `OGRE_NO_AUDIO=1`): before,
  the guest ran 59.4/119.8/188.8 frames/s at 1x/2x/4x, and the page's
  `requestAnimationFrame` cadence was 50 ms at its 95th percentile at 2x/4x.
  Replacing `ogre_gfx_flush` with a no-op at 4x reached 239.9 frames/s, so the
  game could run 4x and the main-thread flush was pacing it. Two changes in
  `app/src/web_renderer.cpp`: `flush_commands()` now holds the renderer mutex
  only for the queue exchange (the gfx pthread publishes every display list and
  present through it, and `dp_complete` follows `send_dl`), and a per-draw
  `draw_uniform_key()` skips the uniform block when the values equal the
  previous draw's. Uniform calls at 4x fell from 1 875 153/s to 495 353/s and the
  mean flush from 11.4 ms to 5.4 ms. After: 1x 60.0, 2x 119.7-120.2, 4x
  234.5-240.7 frames/s, rAF p95 16.67 ms at all three; 8x reaches 414.8 of 480.
  The title capture is identical to its pre-change one; the tutorial field and
  mission briefing keep their composition.
  Open: 8x's remaining 14%, the per-draw `glViewport`/`glScissor`/enable state
  calls, `reference_era_rows`' per-row hashing, and the duplicate flush call in
  `updateGfxStatus`. See `docs/HANDOFF-2026-11-01-session208.md`.

- **Session 209**: the public browser page carries an experimental notice and
  exports the battery. The topbar "Export save" button and the Options MAIN
  "Export battery save" row download the runtime's
  `/ogre/saves/ogrebattle64-us-rev1.bin` as `ogrebattle64-us-rev1.bin`, the raw
  32 KiB logical image the native build reads from `<config>/saves/`. The
  import -> export round trip is byte-exact on the static package
  (`debug/probes/save.cjs`, same SHA-256), and `tools/sramsave.py check`
  validates both slots and the map record. A blank boot writes the file from the
  runtime's saving thread. See `docs/HANDOFF-2026-11-01-session209.md`.
