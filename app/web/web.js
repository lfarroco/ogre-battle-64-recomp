// Ogre Battle 64 — web shell glue (app/web/web.js)
//
// Milestone 5 responsibilities:
//   1. Read the user-selected ROM via the File API (local only) and write it
//      into the wasm virtual filesystem at /rom.z64.
//   2. Capture keyboard/gamepad input on the browser main thread and push it
//      into wasm atomics via the exported ogre_input_set().
//   3. Set up WebAudio (AudioWorklet) that consumes the game's audio straight
//      from the wasm SharedArrayBuffer (app/web/audio-worklet.js).
//   4. Mount IDBFS at /ogre so the validated ROM copy / save data persist
//      across page loads; auto-start when a stored ROM exists.
//   5. Call ogre_start_boot() (boot runs on a pthread) and mirror the
//      runtime's milestones to the status area.
//
// See docs/WEB-PORT.md §10 and docs/WEB-PORT-DEPLOYMENT.md.

(function () {
  "use strict";

  // --- Debug knobs from the query string (OGRE_*) ------------------------------
  //
  // The native build reads its debug knobs from the environment: `OGRE_SCENE`,
  // `OGRE_STEP`, `OGRE_TRACE_DL`, `OGRE_S2D_TRACE`, `OGRE_DUMP_RDRAM`, ...
  // (docs/guides/app-build.md). Emscripten's `getenv` reads a module-scope `ENV`
  // object that the glue creates as a page global, so an `OGRE_*` query
  // parameter can be published there and every shared knob works in the browser
  // unchanged.
  //
  // This must run before the wasm runtime initialises its environment. The glue
  // is loaded by `document.write` in index.html, so it has executed by the time
  // this file runs, but its `createWasm()` only *starts* the fetch here; the
  // C runtime (and the `environ_get` that captures ENV) runs in a later task.
  (function publishOgreEnv() {
    if (typeof ENV === "undefined" || ENV === null) {
      return;
    }
    var query = window.location.search.replace(/^\?/, "");
    if (!query) {
      return;
    }
    query.split("&").forEach(function (pair) {
      if (!pair) {
        return;
      }
      var eq = pair.indexOf("=");
      var key = decodeURIComponent(eq < 0 ? pair : pair.slice(0, eq));
      var value = eq < 0 ? "1" : decodeURIComponent(pair.slice(eq + 1));
      // Only this port's own knobs, so a query string cannot rewrite PATH/HOME.
      if (/^OGRE_[A-Z0-9_]*$/.test(key)) {
        ENV[key] = value;
      }
    });
  })();

  var status = document.getElementById("status");
  var input = document.getElementById("rom-input");
  var lastMilestoneLen = 0;
  var pollTimer = null;
  var bootStarted = false;
  var bootStartTime = 0;
  var audioContext = null;

  // --- Audio switch -------------------------------------------------------------
  //
  // Game audio is MUTED by default. The audio microcode is not emulated yet (the
  // RSP audio task is auto-completed, so whatever the game hands the AI is not
  // real audio), and playing it is a wall of screeching. The AudioWorklet is
  // still created and still drains the ring buffer, so the game sees exactly
  // the same backpressure as before - only the samples written to the output
  // are silenced. Add ?audio to the URL (or call
  // window.ogreAudio.setEnabled(true) at runtime) to hear it while working on
  // the audio path.
  function urlFlag(name) {
    var m = new RegExp("(^|[?&])" + name + "(=([^&]*))?(&|$)").exec(window.location.search);
    if (!m) {
      return null;
    }
    var v = m[3];
    if (v === undefined || v === "" || v === "1" || v === "true" || v === "on") {
      return true;
    }
    if (v === "0" || v === "false" || v === "off") {
      return false;
    }
    return true;
  }

  // The raw value of a query parameter, or null when it is absent. Used for the
  // knobs that are not switches (`OGRE_TAP_MS`, `OGRE_TAP_BUTTON`).
  function urlParam(name) {
    var m = new RegExp("(^|[?&])" + name + "=([^&]*)(&|$)").exec(window.location.search);
    return m ? decodeURIComponent(m[2]) : null;
  }
  var audioEnabled = urlFlag("audio") === true;

  // The log lives in window.ogreLog (index.html), which is a bounded ring buffer
  // and does not touch the DOM unless ?log was passed. Never append to the page
  // per line: the wasm threads' printf is proxied onto the main thread, so
  // per-line DOM writes throttle the emulator.
  function log(text) {
    if (window.ogreLog && typeof window.ogreLog.append === "function") {
      window.ogreLog.append(text);
      return;
    }
    status.textContent += text + "\n";
  }

  function moduleReady() {
    // Runtime is ready once the FS + heap are wired up. (Module.calledRun does
    // not exist in this emscripten's glue.)
    return !!(Module && Module.FS && Module.HEAPU8 && Module.HEAPU8.buffer);
  }

  function whenModuleReady(cb) {
    if (moduleReady()) {
      cb();
      return;
    }
    var prev = Module.onRuntimeInitialized;
    Module.onRuntimeInitialized = function () {
      if (typeof prev === "function") {
        prev();
      }
      cb();
    };
  }

  // The pthread build does not have every `_ogre_*` export on `Module` when
  // `onRuntimeInitialized` fires. They appear a moment later: measured 0.5 s
  // after the log line on a local `make web-dist` package and 2 s on the
  // deployed GitHub Pages build. Every entry point below calls one of them, so
  // wait for the two that are called first instead of racing them. A ROM picked
  // quickly, or the auto-start from a stored ROM, otherwise stops in `ccall`
  // with `TypeError: func is not a function` and never boots.
  function whenWasmExportsReady(cb) {
    if (typeof Module._ogre_start_boot === "function" &&
        typeof Module._ogre_gfx_create_context === "function") {
      cb();
      return;
    }
    window.setTimeout(function () { whenWasmExportsReady(cb); }, 100);
  }

  // --- Milestone polling ------------------------------------------------------

  function pollMilestones() {
    if (!moduleReady() || typeof Module.ccall !== "function") {
      return;
    }
    var ptr = Module.ccall("ogre_poll_milestones", "number", [], []);
    if (!ptr) {
      return;
    }
    var text = Module.UTF8ToString(ptr);
    if (text.length > lastMilestoneLen) {
      log(text.slice(lastMilestoneLen));
      lastMilestoneLen = text.length;
    }
  }

  // --- Graphics workload stats (milestone 6) ----------------------------------

  var gfxStats = document.getElementById("gfxstats");
  var lastGfxStats = "";

  function pollGfxStats() {
    if (!moduleReady() || typeof Module._ogre_gfx_stats !== "function") {
      return;
    }
    var ptr = Module._ogre_gfx_stats();
    if (!ptr) {
      return;
    }
    var text = Module.UTF8ToString(ptr);
    if (text !== lastGfxStats) {
      lastGfxStats = text;
      gfxStats.textContent = text || "(no display lists submitted yet)";
    }
  }
  window.setInterval(pollGfxStats, 1000);

  function startBoot() {
    if (bootStarted) {
      return;
    }
    bootStarted = true;
    bootStartTime = Date.now();
    Module.ccall("ogre_start_boot", "void", [], []);
    log("[web] Boot thread started. Waiting for milestones...");
    if (pollTimer === null) {
      pollTimer = window.setInterval(pollMilestones, 250);
    }
  }

  // --- Input: keyboard + gamepads (all in slot 0) -----------------------------
  // N64 button bits, parity with app/src/sdl_platform.cpp.

  var BTN = {
    A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000,
    UP: 0x0800, DOWN: 0x0400, LEFT: 0x0200, RIGHT: 0x0100,
    L: 0x0020, R: 0x0010,
    C_UP: 0x0008, C_DOWN: 0x0004, C_LEFT: 0x0002, C_RIGHT: 0x0001
  };

  var KEYMAP = {
    KeyX: BTN.A, KeyZ: BTN.B, KeyC: BTN.Z, Enter: BTN.START,
    KeyQ: BTN.L, KeyE: BTN.R,
    ArrowUp: BTN.UP, ArrowDown: BTN.DOWN, ArrowLeft: BTN.LEFT, ArrowRight: BTN.RIGHT,
    KeyI: BTN.C_UP, KeyK: BTN.C_DOWN, KeyJ: BTN.C_LEFT, KeyL: BTN.C_RIGHT
  };

  var pressedKeys = new Set();

  // --- Scripted controller 0 (OGRE_TAP_MS / OGRE_TAP_BUTTON) -------------------
  //
  // The native build drives a run with a synthetic pad: `OGRE_TAP_MS=<n>` makes
  // controller 0 press one button from `OGRE_TAP_BUTTON`'s schedule for 150 ms
  // out of every n ms (docs/guides/app-build.md). The browser had no equivalent,
  // so reaching a scene needed a throwaway Playwright script per session
  // (session 109). This is the same knob, in the JS input path that already
  // owns controller 0.
  //
  // The button vocabulary and the press length match the native side. The slot
  // index advances with wall time, as it does natively, and **the last slot
  // sticks** (`sdl_platform.cpp`: `slot = tap < count ? tap : count - 1`), so
  // `OGRE_TAP_BUTTON="start,start,none"` presses Start twice and then stops
  // tapping while the run continues - which is how a route reaches a screen that
  // waits for input (the New Game cathedral does). `OGRE_TAP_MAX=<n>` stops after
  // n presses, also as natively.
  var TAP_NAMES = {
    start: BTN.START, a: BTN.A, b: BTN.B, z: BTN.Z, l: BTN.L, r: BTN.R,
    up: BTN.UP, down: BTN.DOWN, left: BTN.LEFT, right: BTN.RIGHT,
    c_up: BTN.C_UP, c_down: BTN.C_DOWN, c_left: BTN.C_LEFT, c_right: BTN.C_RIGHT,
    none: 0
  };
  var TAP_PRESS_MS = 150;
  var tapMask = 0;
  (function initTapSchedule() {
    var ms = parseInt(urlParam("OGRE_TAP_MS"), 10);
    if (!isFinite(ms) || ms <= 0) {
      return;
    }
    var names = (urlParam("OGRE_TAP_BUTTON") || "start").split(",").map(function (s) {
      return s.trim().toLowerCase();
    });
    var max = parseInt(urlParam("OGRE_TAP_MAX"), 10);
    if (!isFinite(max) || max <= 0) {
      max = -1;
    }
    var slot = 0;
    log("[web:input] OGRE_TAP_MS=" + ms + " OGRE_TAP_BUTTON=" + names.join(",") +
        (max > 0 ? " OGRE_TAP_MAX=" + max : ""));
    var timer = window.setInterval(function () {
      if (max > 0 && slot >= max) {
        window.clearInterval(timer);
        tapMask = 0;
        log("[web:input] tap schedule done after " + slot + " presses");
        return;
      }
      var name = names[slot < names.length ? slot : names.length - 1];
      slot++;
      var mask = TAP_NAMES[name];
      if (mask === undefined) {
        log("[web:input] OGRE_TAP_BUTTON: unrecognised button '" + name + "'; using start");
        mask = BTN.START;
      }
      tapMask = mask;
      window.setTimeout(function () {
        tapMask = 0;
      }, TAP_PRESS_MS);
    }, ms);
  })();

  function pushInput(controller, buttons, x, y, connected) {
    if (!moduleReady() || typeof Module._ogre_input_set !== "function") {
      return;
    }
    Module._ogre_input_set(controller, buttons, x, y, connected);
  }

  function keyboardButtons() {
    var mask = 0;
    pressedKeys.forEach(function (code) {
      mask |= KEYMAP[code] || 0;
    });
    return mask;
  }

  window.addEventListener("keydown", function (e) {
    if (KEYMAP[e.code]) {
      e.preventDefault();
    }
    pressedKeys.add(e.code);
    pushInput(0, keyboardButtons(), 0, 0, 1);
  });
  window.addEventListener("keyup", function (e) {
    pressedKeys.delete(e.code);
    pushInput(0, keyboardButtons(), 0, 0, 1);
  });
  window.addEventListener("blur", function () {
    pressedKeys.clear();
    pushInput(0, 0, 0, 0, 1);
  });

  // Standard Gamepad mapping -> N64 bits (parity with sdl_platform.cpp):
  //   0=A 1=B 2=X 3=Y 4=LB 5=RB 9=Start 10=LeftStick(->Z)
  //   12-15=D-pad; right stick -> C buttons.
  var GP_BUTTONS = [
    BTN.A,                    // 0 A
    BTN.B,                    // 1 B
    BTN.C_UP,                 // 2 X
    BTN.C_DOWN,               // 3 Y
    BTN.L,                    // 4 LB
    BTN.R,                    // 5 RB
    0, 0,                     // 6 LT, 7 RT
    0,                        // 8 Back (unused)
    BTN.START,                // 9 Start
    BTN.Z,                    // 10 LeftStick
    0                         // 11 RightStick (unused)
  ];
  var GP_DPAD = [BTN.UP, BTN.DOWN, BTN.LEFT, BTN.RIGHT];  // buttons 12-15

  function gamepadState(pad) {
    var buttons = 0;
    var b = pad.buttons;
    for (var i = 0; i < GP_BUTTONS.length && i < b.length; i++) {
      if (GP_BUTTONS[i] && b[i].pressed) {
        buttons |= GP_BUTTONS[i];
      }
    }
    for (var j = 0; j < 4; j++) {
      var bi = 12 + j;
      if (bi < b.length && b[bi].pressed) {
        buttons |= GP_DPAD[j];
      }
    }
    var axes = pad.axes || [];
    // Right stick -> C buttons (axis 2 = RX, 3 = RY).
    if (axes.length > 2) {
      if (axes[3] < -0.5) buttons |= BTN.C_UP;
      if (axes[3] > 0.5) buttons |= BTN.C_DOWN;
      if (axes[2] < -0.5) buttons |= BTN.C_LEFT;
      if (axes[2] > 0.5) buttons |= BTN.C_RIGHT;
    }
    var x = axes.length > 0 ? Math.max(-1, Math.min(1, axes[0])) : 0;
    var y = axes.length > 1 ? Math.max(-1, Math.min(1, axes[1])) : 0;
    return { buttons: buttons, x: x, y: y };
  }

  var lastGamepadLog = 0;
  // Slot assignment, recomputed every frame: each connected pad takes the
  // lowest slot that is free, so the first pad lands in slot 0. Slot 0 is N64
  // controller 1, the only controller this game reads, and slot 0 carries the
  // keyboard and the scripted tap schedule as well (`slot0Mask`), which are
  // OR'd with the pad's buttons - the rule `sdl_platform.cpp` uses, and the
  // native fix for issue #13: a pad that never reaches slot 0 is invisible in
  // game, and a pad that takes the slot away from the keyboard is just as
  // wrong. `padSlot` is the assignment the previous frame used, so a change is
  // logged once.
  var padSlot = [null, null, null, null];

  function updateGamepads(now, slot0Mask) {
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    var assigned = [null, null, null, null];
    var connectedAny = false;
    for (var i = 0; i < pads.length; i++) {
      var pad = pads[i];
      if (!pad || !pad.connected) {
        continue;
      }
      var slot = assigned.indexOf(null);
      if (slot === -1) {
        break;              // four pads fill every slot; the rest get nothing
      }
      assigned[slot] = pad.index;
      padSlot[slot] = pad.index;
      var st = gamepadState(pad);
      // Slot 0 keeps the keyboard and the taps on top of the pad's own state.
      if (slot === 0) {
        st.buttons |= slot0Mask;
      }
      pushInput(slot, st.buttons, st.x, st.y, 1);
      connectedAny = true;
    }
    for (var slotN = 0; slotN < 4; slotN++) {
      if (assigned[slotN] === null) {
        if (padSlot[slotN] !== null) {
          log("[web:input] pad " + padSlot[slotN] + " left slot " + slotN);
        }
        padSlot[slotN] = null;
        // Slot 0 stays connected to the keyboard; the other slots go idle.
        if (slotN > 0) {
          pushInput(slotN, 0, 0, 0, 0);
        } else {
          pushInput(0, slot0Mask, 0, 0, 1);
        }
      }
    }
    if (connectedAny && now - lastGamepadLog > 5000) {
      lastGamepadLog = now;
      log("[web:input] gamepad connected (" +
          assigned.map(function (idx, slot) {
            return idx === null ? "slot " + slot + " free" : "pad " + idx + " -> slot " + slot;
          }).join(", ") + ")");
    }
  }

  function rafLoop(now) {
    var slot0Mask = keyboardButtons() | tapMask;
    // The pad pass runs last: it writes slot 0, so a keyboard update first
    // would be overwritten every frame by a connected pad.
    updateGamepads(now, slot0Mask);
    requestAnimationFrame(rafLoop);
  }
  requestAnimationFrame(rafLoop);

  // --- Audio: AudioContext + AudioWorklet --------------------------------------

  function initAudio() {
    if (audioContext) {
      return;
    }
    try {
      var Ctx = window.AudioContext || window.webkitAudioContext;
      audioContext = new Ctx();
    } catch (err) {
      log("[web:audio] could not create AudioContext: " + err);
      audioContext = null;
      return;
    }

    audioContext.audioWorklet.addModule("audio-worklet.js").then(function () {
      if (!moduleReady() || typeof Module._ogre_audio_state_ptr !== "function") {
        log("[web:audio] wasm audio exports not available yet");
        return;
      }
      var statePtr = Module._ogre_audio_state_ptr();
      var ringPtr = Module._ogre_audio_ring_ptr();
      var node = new AudioWorkletNode(audioContext, "ogre-audio-processor", {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2]
      });
      node.port.postMessage({
        type: "init",
        sab: Module.HEAPU8.buffer,
        statePtr: statePtr,
        ringPtr: ringPtr,
        ringFrames: 32768,  // kAudioRingFrames in app/src/web_platform.cpp
        muted: !audioEnabled
      });
      node.connect(audioContext.destination);
      // The worklet keeps draining the ring even while muted, so the game's
      // audio thread sees the same producer/consumer backpressure either way.
      window.ogreAudio = {
        enabled: function () { return audioEnabled; },
        setEnabled: function (on) {
          audioEnabled = !!on;
          node.port.postMessage({ type: "mute", muted: !audioEnabled });
          log("[web:audio] " + (audioEnabled ? "unmuted (game audio will play - it screeches until the audio microcode is emulated)"
                                             : "muted"));
        }
      };
      audioContext.resume().then(function () {
        log("[web:audio] AudioWorklet connected (" + audioContext.sampleRate + " Hz context, state " +
            audioContext.state + "), game audio " +
            (audioEnabled ? "ENABLED (?audio)" : "MUTED - add ?audio to the URL to hear it"));
        // Report once the game actually starts queueing samples, so a silent
        // page is distinguishable from a stalled audio path.
        var audioWatch = window.setInterval(function () {
          if (typeof Module._ogre_audio_frames_available === "function") {
            var frames = Module._ogre_audio_frames_available();
            if (frames > 0) {
              log("[web:audio] game is queueing audio (" + frames + " frames buffered" +
                  (audioEnabled ? "" : ", muted") + ")");
              window.clearInterval(audioWatch);
            }
          }
        }, 1000);
      }).catch(function (err) {
        log("[web:audio] resume failed: " + err);
      });
    }).catch(function (err) {
      log("[web:audio] AudioWorklet failed to load: " + err);
    });
  }

  // --- Persistent storage (IDBFS at /ogre) -------------------------------------

  function mountPersistentStorage() {
    if (!moduleReady() || !Module.FS || typeof Module.FS.mount !== "function") {
      return;
    }
    var IDBFSMod = Module.IDBFS || (typeof IDBFS !== "undefined" ? IDBFS : null);
    if (!IDBFSMod) {
      log("[web:save] IDBFS not linked; save persistence disabled (MEMFS only).");
      return;
    }
    try {
      Module.FS.mkdir("/ogre");
    } catch (e) {
      // already exists
    }
    try {
      Module.FS.mount(IDBFSMod, {}, "/ogre");
      Module.FS.syncfs(true, function (err) {
        if (err) {
          log("[web:save] IDBFS load failed: " + err);
          return;
        }
        log("[web:save] persistent storage mounted at /ogre");
        // If a previously validated ROM is persisted, boot without re-picking.
        if (Module.FS.analyzePath("/ogre/ogrebattle64-us-rev1.z64").exists) {
          log("[web] Stored ROM found - starting the game with the previously loaded ROM.");
          whenWasmExportsReady(function () {
            initWebGL();
            startBoot();
          });
        }
      });
    } catch (err) {
      log("[web:save] IDBFS mount failed: " + err);
    }
  }

  function syncStorage() {
    if (moduleReady() && Module.FS && typeof Module.FS.syncfs === "function") {
      Module.FS.syncfs(false, function (err) {
        if (err) {
          log("[web:save] sync failed: " + err);
        }
      });
    }
  }
  window.setInterval(syncStorage, 10000);
  window.addEventListener("beforeunload", syncStorage);

  // --- Graphics: WebGL2 canvas handoff (milestone 7) ---------------------------

  var gfxStatus = document.getElementById("gfx-status");

  function initWebGL() {
    if (gfxStatus.getAttribute("data-done")) {
      return;
    }
    if (!moduleReady() || typeof Module._ogre_gfx_create_context !== "function") {
      // Runtime/glue not ready yet; keep the "waiting" text (the caller can
      // retry on the next ROM pick).
      return;
    }
    gfxStatus.setAttribute("data-done", "1");
    try {
      var handle = Module._ogre_gfx_create_context(320, 240);
      if (!handle) {
        gfxStatus.textContent =
          "graphics: WebGL2 context creation FAILED (no WebGL2?) - the renderer cannot start; " +
          "the workload analyzer keeps running";
        log("[web:gfx] ERROR: emscripten_webgl_create_context returned 0 (WebGL2 unavailable in this browser?)");
        return;
      }
      Module._ogre_gfx_set_canvas(handle, 320, 240);
      gfxStatus.textContent =
        "graphics: WebGL2 context ready (320x240) - renderer takes over once the game submits a display list";
      log("[web:gfx] WebGL2 context handed to the renderer (320x240)");
      // Poll the queued draw commands on the main thread (the gfx pthread only
      // records them; all GL runs here).
      if (!window.__ogreGfxFlushStarted) {
        window.__ogreGfxFlushStarted = true;
        window.setInterval(function () {
          Module._ogre_gfx_flush();
          updateGfxStatus();
        }, 16);
      }
    } catch (e3) {
      gfxStatus.textContent = "graphics: context handoff failed: " + e3;
      log("[web:gfx] ERROR: context handoff threw: " + e3);
    }
  }

  // Live #gfx-status line: reflects the actual renderer state instead of
  // staying on the initial "waiting for the WebGL2 context" text forever.
  // Derived from existing exports (ogre_gfx_flush returns 1 once the renderer
  // object exists; #gfxstats carries the workload snapshot).
  var lastGfxStatusLine = "";
  var bootStallLogged = false;

  function updateGfxStatus() {
    if (gfxStatus.getAttribute("data-done") !== "1") {
      return;  // context not created yet; keep the "waiting" text
    }
    var rendererUp = false;
    try {
      rendererUp = typeof Module._ogre_gfx_flush === "function" && Module._ogre_gfx_flush() !== 0;
    } catch (e) {
      rendererUp = false;
    }
    var stats = gfxStats.textContent || "";
    var tasks = 0, hasDraws = false;
    var m = /tasks=(\d+)/.exec(stats);
    if (m) {
      tasks = parseInt(m[1], 10);
    }
    var f = /fillrect=(\d+)/.exec(stats);
    var t = /texrect=(\d+)/.exec(stats);
    if (f || t) {
      hasDraws = (f ? parseInt(f[1], 10) : 0) + (t ? parseInt(t[1], 10) : 0) > 0;
    }
    var line;
    if (!rendererUp) {
      line = "graphics: WebGL2 context ready (320x240) - renderer not active yet (boot still starting)";
    } else if (tasks === 0) {
      line = "graphics: WebGL2 renderer active - waiting for the first display list";
    } else {
      line = "graphics: WebGL2 renderer active - " + tasks + (tasks === 1 ? " display list processed" : " display lists processed") +
             (hasDraws ? "" : " (only the boot blanking DL so far - no pixels drawn yet)");
    }
    if (line !== lastGfxStatusLine) {
      lastGfxStatusLine = line;
      gfxStatus.textContent = line;
    }
  }

  // Roughly half of all boots land on the "idle trajectory": the game submits
  // only the 32-command boot blanking DL and never builds its own display list,
  // so the canvas stays black even though the renderer works. Surface that
  // instead of leaving the page looking broken, and say what it is - the old
  // message blamed a session-14 "VI-retrace deadlock" that later sessions
  // disproved (the renderer draws the title scene whenever the game advances).
  function watchBootStall() {
    window.setInterval(function () {
      if (bootStallLogged || !bootStartTime || Date.now() - bootStartTime < 15000) {
        return;
      }
      var stats = gfxStats.textContent || "";
      var m = /tasks=(\d+)/.exec(stats);
      var tasks = m ? parseInt(m[1], 10) : 0;
      if (tasks <= 1) {
        bootStallLogged = true;
        log("[web:gfx] Only the boot blanking display list has been submitted after 15s, so the canvas will stay " +
            "black. This is the known idle trajectory: about half of all boots stall before the game builds its " +
            "first real display list. It is a game/runtime pacing issue, not a renderer problem - reload to get a " +
            "different trajectory (the probes retry for this reason). See docs/HANDOFF-2026-09-11-session21.md.");
      }
    }, 1000);
  }
  watchBootStall();

  // --- ROM picker ---------------------------------------------------------------

  input.addEventListener("change", function () {
    var file = input.files && input.files[0];
    if (!file) {
      return;
    }
    if (!moduleReady() || !Module.FS) {
      log("[web] WASM runtime not ready yet; retry in a moment.");
      return;
    }
    log("[web] Loading ROM: " + file.name + " (" + file.size + " bytes)...");

    file.arrayBuffer().then(function (buffer) {
      var bytes = new Uint8Array(buffer);
      Module.FS.writeFile("/rom.z64", bytes);
      log("[web] ROM written to the virtual filesystem. Starting runtime...");
      initAudio();  // inside the user gesture so autoplay is allowed
      // initWebGL and the boot call wasm exports that the glue publishes a
      // moment after the runtime is up; see whenWasmExportsReady.
      whenWasmExportsReady(function () {
        initWebGL();  // first GL call from the main thread
        startBoot();
      });
    }).catch(function (err) {
      log("[web] Failed to read the ROM file: " + err);
    });
  });

  // --- Startup ------------------------------------------------------------------

  whenModuleReady(function () {
    log("[web] WASM runtime initialized. Select a ROM to begin (or use the stored one).");
    mountPersistentStorage();
  });
})();
