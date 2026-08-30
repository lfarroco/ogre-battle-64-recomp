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

  var status = document.getElementById("status");
  var input = document.getElementById("rom-input");
  var lastMilestoneLen = 0;
  var pollTimer = null;
  var bootStarted = false;
  var bootStartTime = 0;
  var audioContext = null;

  function log(text) {
    status.textContent += text + "\n";
    status.scrollTop = status.scrollHeight;
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
      status.textContent += text.slice(lastMilestoneLen);
      lastMilestoneLen = text.length;
      status.scrollTop = status.scrollHeight;
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

  // --- Input: keyboard (slot 0) + gamepads (slots 1-3) ------------------------
  // N64 button bits, parity with app/src/sdl_platform.cpp.

  var BTN = {
    A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000,
    UP: 0x0800, DOWN: 0x0400, LEFT: 0x0200, RIGHT: 0x0100,
    L: 0x0020, R: 0x0010,
    C_LEFT: 0x0008, C_RIGHT: 0x0004, C_DOWN: 0x0002, C_UP: 0x0001
  };

  var KEYMAP = {
    KeyX: BTN.A, KeyZ: BTN.B, KeyC: BTN.Z, Enter: BTN.START,
    KeyQ: BTN.L, KeyE: BTN.R,
    ArrowUp: BTN.UP, ArrowDown: BTN.DOWN, ArrowLeft: BTN.LEFT, ArrowRight: BTN.RIGHT,
    KeyI: BTN.C_UP, KeyK: BTN.C_DOWN, KeyJ: BTN.C_LEFT, KeyL: BTN.C_RIGHT
  };

  var pressedKeys = new Set();

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

  function updateGamepads(now) {
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    var connectedAny = false;
    for (var i = 0; i < pads.length; i++) {
      var pad = pads[i];
      if (!pad || !pad.connected) {
        continue;
      }
      // Stable per-physical-pad slot in 1..3 (slot 0 is keyboard-first).
      var slot = (pad.index % 3) + 1;
      var st = gamepadState(pad);
      pushInput(slot, st.buttons, st.x, st.y, 1);
      connectedAny = true;
    }
    // Disconnect any slot without a live pad.
    for (var slotN = 1; slotN <= 3; slotN++) {
      var has = false;
      for (var k = 0; k < pads.length; k++) {
        if (pads[k] && pads[k].connected && ((pads[k].index % 3) + 1) === slotN) {
          has = true;
          break;
        }
      }
      if (!has) {
        pushInput(slotN, 0, 0, 0, 0);
      }
    }
    if (connectedAny && now - lastGamepadLog > 5000) {
      lastGamepadLog = now;
      log("[web:input] gamepad connected (slots 1-3)");
    }
  }

  function rafLoop(now) {
    pushInput(0, keyboardButtons(), 0, 0, 1);
    updateGamepads(now);
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
        ringFrames: 32768  // kAudioRingFrames in app/src/web_platform.cpp
      });
      node.connect(audioContext.destination);
      audioContext.resume().then(function () {
        log("[web:audio] AudioWorklet connected (" + audioContext.sampleRate + " Hz context, state " + audioContext.state + ")");
        // Report once audio actually starts flowing.
        var audioWatch = window.setInterval(function () {
          if (typeof Module._ogre_audio_frames_available === "function") {
            var frames = Module._ogre_audio_frames_available();
            if (frames > 0) {
              log("[web:audio] game audio is flowing (" + frames + " frames buffered)");
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
          initWebGL();
          startBoot();
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

  // The game is currently stuck at its boot screen on every platform (it only
  // ever submits the 32-command boot blanking DL, which has no draw commands),
  // so the canvas stays black even though the renderer works. Surface that
  // clearly instead of leaving the page looking broken.
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
        log("[web:gfx] The game is still at its boot screen (only the boot blanking display list was submitted, " +
            "no draw commands). This is the known boot-stall that affects every platform - the renderer itself is " +
            "fine, but the game's VI-retrace message queues deadlock and the boot never advances (see " +
            "docs/HANDOFF-2026-08-30-session14.md). The screen will stay black until the boot-stall is fixed.");
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
      initWebGL();  // inside the user gesture (WebGL2 context creation)
      startBoot();
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
