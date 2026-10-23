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

  // --- Battery save import (OGRE_SAVE=<relative url>) --------------------------
  //
  // The native build's `OGRE_SAVE` installs a 32 KiB SRAM battery image at
  // `<config>/saves/<game id>.bin` before the runtime reads it (app/src/main.cpp
  // -> app/src/save_import.cpp). The browser has no local paths, so the page
  // fetches the image instead and writes it to the same place in the IDBFS mount:
  // `/ogre/saves/ogrebattle64-us-rev1.bin` (the game id is `ogre::GAME_ID` in
  // app/src/game.hpp, and pi.cpp's `init_saving` reads exactly that path).
  //
  // Two things have to hold for the import to be seen:
  //
  //   1. It runs after `FS.syncfs(true)` has loaded the persisted /ogre contents,
  //      otherwise the load would replace the imported file (or the import would
  //      be lost), and
  //   2. it finishes before `ogre_start_boot()` runs, because pi.cpp's
  //      `read_save_file()` reads the battery while the runtime initialises.
  //
  // So the import is started by the storage-ready callback
  // (`onStorageReady`) and the boot path waits for it (`startBoot` ->
  // `whenSaveImportSettled`). A fetch failure settles the import as "failed" and
  // the boot proceeds without a battery, the same way the native `OGRE_SAVE`
  // reports the problem and boots.
  //
  // The URL is resolved from the server root, not from the page: the page is
  // served from `/app/web/`, so `OGRE_SAVE=assets/escort_suspend.bin` has to mean
  // `/assets/escort_suspend.bin` (the file the repository serves and the path
  // the native `OGRE_SAVE` example names). An absolute URL or a leading `/` is
  // used as written.
  var SAVE_IMPORT_PATH = "/ogre/saves/ogrebattle64-us-rev1.bin";
  var saveImport = {
    spec: urlParam("OGRE_SAVE"),
    armed: false,
    storageReady: false,
    started: false,
    settled: false,
    waiters: []
  };
  saveImport.armed = !!(saveImport.spec && saveImport.spec.length > 0);

  function settleSaveImport() {
    if (saveImport.settled) {
      return;
    }
    saveImport.settled = true;
    var waiters = saveImport.waiters;
    saveImport.waiters = [];
    waiters.forEach(function (fn) {
      try {
        fn();
      } catch (e) {
        log("[web:save] save-import waiter threw: " + e);
      }
    });
  }

  function whenSaveImportSettled(cb) {
    if (saveImport.settled) {
      cb();
      return;
    }
    saveImport.waiters.push(cb);
  }

  function resolveSaveUrl(spec) {
    if (/^[a-z][a-z0-9+.-]*:/i.test(spec) || spec.charAt(0) === "/") {
      return spec;
    }
    return "/" + spec;
  }

  // FS.mkdirTree is present in current Emscripten; the fallback loop is for an
  // older glue and for a path that already exists.
  function ensureDir(path) {
    try {
      if (typeof Module.FS.mkdirTree === "function") {
        Module.FS.mkdirTree(path);
        return;
      }
    } catch (e) {
      // fall through to the component loop
    }
    var parts = path.split("/").filter(function (p) { return p.length > 0; });
    var cur = "";
    for (var i = 0; i < parts.length; i++) {
      cur += "/" + parts[i];
      try {
        Module.FS.mkdir(cur);
      } catch (e) {
        // already exists
      }
    }
  }

  // Writes `bytes` to the battery path, syncs IDBFS and settles the gate. Used
  // by the query-string import and by the page's "Load save" file input.
  function writeBatterySave(bytes, source, done) {
    if (!moduleReady() || !Module.FS) {
      log("[web:save] " + source + ": the wasm FS is not ready; no battery written");
      if (done) {
        done(false);
      }
      return;
    }
    try {
      ensureDir("/ogre/saves");
      Module.FS.writeFile(SAVE_IMPORT_PATH, bytes);
    } catch (e) {
      log("[web:save] " + source + ": write to " + SAVE_IMPORT_PATH + " failed: " + e);
      if (done) {
        done(false);
      }
      return;
    }
    log("[web:save] imported " + source + " (" + bytes.length + " bytes) -> " + SAVE_IMPORT_PATH +
        (bytes.length === 32768 ? "" : " (warning: expected 32768 bytes)"));
    // Persist so a later page load boots from the same battery; the game's own
    // writes go through the same 10 s interval (syncStorage).
    var synced = false;
    function finish() {
      if (synced) {
        return;
      }
      synced = true;
      if (done) {
        done(true);
      }
    }
    try {
      Module.FS.syncfs(false, function (err) {
        if (err) {
          log("[web:save] sync failed after import: " + err);
        }
        finish();
      });
    } catch (e) {
      log("[web:save] syncfs(false) threw after import: " + e);
      finish();
    }
  }

  // Starts the query-string import once the IDBFS mount has finished loading.
  // Idempotent: the storage-ready callback and the boot path both call it.
  function importBatterySave() {
    if (saveImport.started || !saveImport.armed) {
      return;
    }
    if (!saveImport.storageReady) {
      return;  // onStorageReady() calls again
    }
    saveImport.started = true;
    var url = resolveSaveUrl(saveImport.spec);
    log("[web:save] OGRE_SAVE=" + saveImport.spec + ": fetching " + url);
    uiSetStatus("Loading save data…");
    fetch(url, { cache: "no-store" }).then(function (resp) {
      if (!resp.ok) {
        throw new Error("HTTP " + resp.status);
      }
      return resp.arrayBuffer();
    }).then(function (buffer) {
      writeBatterySave(new Uint8Array(buffer), saveImport.spec, function () {
        uiSetStatus("Save data loaded.");
        settleSaveImport();
      });
    }).catch(function (err) {
      log("[web:save] OGRE_SAVE=" + saveImport.spec + ": import failed: " + err +
          " - booting without it");
      uiSetStatus("Save data could not be loaded. Booting without it.");
      settleSaveImport();
    });
  }

  var status = document.getElementById("status");
  var input = document.getElementById("rom-input");
  var saveInput = document.getElementById("save-input");
  var lastMilestoneLen = 0;
  var pollTimer = null;
  var bootStarted = false;
  var bootStartTime = 0;
  var audioContext = null;

  // The shell (index.html) owns the chrome, the status pill and the launcher.
  // These helpers keep the UI calls optional, so a page that embeds web.js
  // without the shell still runs.
  function uiSetPhase(phase, label) {
    if (window.ogreUI) {
      window.ogreUI.setPhase(phase, label);
    }
  }
  function uiSetStatus(text) {
    if (window.ogreUI) {
      window.ogreUI.setStatus(text);
    }
  }
  function uiDismissLauncher() {
    if (window.ogreUI) {
      window.ogreUI.dismissLauncher();
    }
  }

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

  var bootPending = false;

  function startBoot() {
    if (bootStarted || bootPending) {
      return;
    }
    // pi.cpp's `init_saving` -> `read_save_file` runs inside `ogre_start_boot()`,
    // so an OGRE_SAVE import that is still in flight has to finish first.
    if (saveImport.armed && !saveImport.settled) {
      bootPending = true;
      log("[web:save] boot deferred until the OGRE_SAVE import settles");
      importBatterySave();  // no-op once started; covers a ROM picked early
      whenSaveImportSettled(function () {
        bootPending = false;
        startBoot();
      });
      return;
    }
    bootStarted = true;
    bootStartTime = Date.now();
    uiSetPhase("loading", "Loading");
    uiSetStatus("Starting the game…");
    uiDismissLauncher();
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
  // `OGRE_TAP_MAX=<n>` stops after
  // n presses, also as natively.
  //
  // `OGRE_TAP_SCENE_BUTTON` (below) is the scene-keyed sibling: it presses a
  // button while a named scene is active instead of at a fixed wall-clock slot,
  // so a route does not depend on how long the boot or a scene took. When it is
  // set it replaces the wall-clock schedule.
  var TAP_NAMES = {
    start: BTN.START, a: BTN.A, b: BTN.B, z: BTN.Z, l: BTN.L, r: BTN.R,
    up: BTN.UP, down: BTN.DOWN, left: BTN.LEFT, right: BTN.RIGHT,
    c_up: BTN.C_UP, c_down: BTN.C_DOWN, c_left: BTN.C_LEFT, c_right: BTN.C_RIGHT,
    none: 0
  };
  var TAP_PRESS_MS = 150;
  var tapMask = 0;

  // The scene table `OGRE_SCENE`/`OGRE_TAP_SCENE_BUTTON` names resolve through,
  // mirrored from `kScenes` in app/src/bank_overlays.cpp. A hex id works for
  // every scene.
  var SCENE_NAMES = {
    "title": 0x0004, "intro": 0x0009, "publishers": 0x000A, "story": 0x000B,
    "unit-info": 0x000C, "tutorial": 0x0017, "menu": 0x0018, "new-game": 0x0002
  };

  // The dispatcher's active scene id, learned from the `[scene]` lines the page
  // already receives (`app/src/bank_overlays.cpp` prints one per scene change,
  // `[scene] t=<ms> id=0x0004 ...`). `-1` means "not known yet". The native
  // `OGRE_TAP_SCENE_BUTTON` reads the same id (`ogre::active_scene_id()`,
  // `D_800E810E`) rather than the log, so the two can disagree by one scene
  // change at most; the schedule only presses after the line has arrived.
  var activeSceneId = -1;
  (function trackActiveScene() {
    function note(line) {
      var m = /\[scene\] t=\d+ms id=0x([0-9a-fA-F]{1,4})/.exec(line);
      if (m) {
        activeSceneId = parseInt(m[1], 16);
      }
    }
    if (window.ogreLog && typeof window.ogreLog.onLine === "function") {
      window.ogreLog.onLine(function (parts) {
        for (var i = 0; i < parts.length; i++) {
          note(parts[i]);
        }
      });
    }
    if (window.ogreLog && typeof window.ogreLog.text === "function") {
      String(window.ogreLog.text() || "").split("\n").forEach(note);
    }
  })();

  function sceneNameOf(id) {
    for (var name in SCENE_NAMES) {
      if (SCENE_NAMES[name] === id) {
        return name;
      }
    }
    return null;
  }

  function parseSceneSpec(spec, ok) {
    var s = String(spec || "").trim().toLowerCase();
    if (Object.prototype.hasOwnProperty.call(SCENE_NAMES, s)) {
      return SCENE_NAMES[s];
    }
    if (/^0x[0-9a-f]{1,4}$/.test(s) || /^[0-9a-f]{1,4}$/.test(s)) {
      return parseInt(s, 16);
    }
    return -1;
  }

  // Parses `OGRE_TAP_BUTTON`'s comma-separated slots (the wall-clock schedule).
  function initWallClockTapSchedule(ms) {
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
  }

  // `OGRE_TAP_SCENE_BUTTON=<scene>:<buttons>[:<count>],...` — the page's copy of
  // the native scene-keyed schedule (app/src/sdl_platform.cpp). It replaces the
  // wall-clock `OGRE_TAP_BUTTON` schedule when it is set, presses for
  // `TAP_PRESS_MS` once per `OGRE_TAP_MS` while the named scene is active, and
  // spends at most `<count>` presses per entry (default: unlimited). Entries for
  // the same scene are consumed in order, and the first entry that still matches
  // and has presses left wins.
  function initSceneTapSchedule(ms, spec) {
    var schedule = [];
    spec.split(",").forEach(function (entry) {
      var text = entry.trim();
      if (!text) {
        return;
      }
      var parts = text.split(":");
      if (parts.length < 2) {
        log("[web:input] OGRE_TAP_SCENE_BUTTON: '" + text + "' is not <scene>:<buttons>[:<count>]");
        return;
      }
      var scene = parseSceneSpec(parts[0]);
      if (scene < 0) {
        log("[web:input] OGRE_TAP_SCENE_BUTTON: unknown scene '" + parts[0] + "' in '" + text + "'; skipping");
        return;
      }
      var mask = 0;
      var names = [];
      var bad = null;
      parts[1].split("+").forEach(function (raw) {
        var name = raw.trim().toLowerCase();
        if (!name) {
          return;
        }
        var bit = TAP_NAMES[name];
        if (bit === undefined) {
          bad = name;
          return;
        }
        mask |= bit;
        names.push(name);
      });
      if (bad !== null || names.length === 0) {
        log("[web:input] OGRE_TAP_SCENE_BUTTON: unrecognised button" +
            (bad !== null ? " '" + bad + "'" : "") + " in '" + text + "'; skipping");
        return;
      }
      var count = parts.length > 2 ? parseInt(parts[2], 10) : -1;
      if (!isFinite(count)) {
        count = -1;
      }
      schedule.push({ scene: scene, mask: mask, label: names.join("+"), count: count, used: 0 });
    });
    if (!schedule.length) {
      return;
    }
    var described = schedule.map(function (item) {
      return "0x" + ("000" + item.scene.toString(16)).slice(-4) + ":" + item.label +
             (item.count >= 0 ? ":" + item.count : "");
    }).join(", ");
    log("[web:input] OGRE_TAP_MS=" + ms + " OGRE_TAP_SCENE_BUTTON=" + described +
        " (scene-keyed; replaces OGRE_TAP_BUTTON)");
    var timer = window.setInterval(function () {
      if (activeSceneId < 0) {
        return;
      }
      for (var i = 0; i < schedule.length; i++) {
        var item = schedule[i];
        if (item.count >= 0 && item.used >= item.count) {
          continue;
        }
        if (item.scene !== activeSceneId) {
          continue;
        }
        item.used++;
        tapMask = item.mask;
        log("[web:input] automation scene tap " + item.used +
            (item.count >= 0 ? "/" + item.count : "") +
            " scene=0x" + ("000" + activeSceneId.toString(16)).slice(-4) +
            " buttons=" + item.label);
        window.setTimeout(function () {
          tapMask = 0;
        }, TAP_PRESS_MS);
        break;
      }
      // Every entry with a count has spent its presses: stop the timer, the same
      // way the wall-clock schedule stops at OGRE_TAP_MAX.
      var live = schedule.some(function (item) {
        return item.count < 0 || item.used < item.count;
      });
      if (!live) {
        window.clearInterval(timer);
        tapMask = 0;
        log("[web:input] scene tap schedule done");
      }
    }, ms);
  }

  (function initTapSchedule() {
    var ms = parseInt(urlParam("OGRE_TAP_MS"), 10);
    if (!isFinite(ms) || ms <= 0) {
      return;
    }
    var sceneSpec = urlParam("OGRE_TAP_SCENE_BUTTON");
    if (sceneSpec) {
      initSceneTapSchedule(ms, sceneSpec);
      return;
    }
    initWallClockTapSchedule(ms);
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

  // A modal dialog owns the keyboard while it is open, so arrow keys can
  // scroll it instead of being swallowed as D-pad input.
  function dialogOwnsKeyboard() {
    return !!(document.querySelector && document.querySelector("dialog[open]"));
  }

  window.addEventListener("keydown", function (e) {
    if (dialogOwnsKeyboard()) {
      return;
    }
    if (KEYMAP[e.code]) {
      e.preventDefault();
    }
    pressedKeys.add(e.code);
    pushInput(0, keyboardButtons(), 0, 0, 1);
  });
  window.addEventListener("keyup", function (e) {
    pressedKeys.delete(e.code);
    if (dialogOwnsKeyboard()) {
      return;
    }
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

  // Runs once the persisted /ogre contents are loaded (or once it is known that
  // they cannot be). Order matters: the OGRE_SAVE import is started here so it
  // overwrites any battery that came out of IndexedDB, and the stored-ROM
  // auto-start goes through `startBoot`, which waits for the import.
  function onStorageReady() {
    if (saveImport.storageReady) {
      return;
    }
    saveImport.storageReady = true;
    // If a previously validated ROM is persisted, boot without re-picking.
    var storedRom = moduleReady() && Module.FS && Module.FS.analyzePath &&
        Module.FS.analyzePath("/ogre/ogrebattle64-us-rev1.z64").exists;
    if (storedRom) {
      log("[web] Stored ROM found - starting the game with the previously loaded ROM.");
      uiSetStatus("Restoring the previous session…");
      whenWasmExportsReady(function () {
        initWebGL();
        startBoot();
      });
    } else {
      uiSetPhase("ready", "Ready");
      uiSetStatus("Ready. Select a ROM to begin.");
    }
    importBatterySave();
  }

  function mountPersistentStorage() {
    if (!moduleReady() || !Module.FS || typeof Module.FS.mount !== "function") {
      // No FS to import into; an OGRE_SAVE import cannot run, so let the boot
      // proceed instead of waiting for a write that will never happen.
      log("[web:save] the wasm FS is not available; OGRE_SAVE is ignored");
      settleSaveImport();
      saveImport.storageReady = true;
      return;
    }
    var IDBFSMod = Module.IDBFS || (typeof IDBFS !== "undefined" ? IDBFS : null);
    if (!IDBFSMod) {
      log("[web:save] IDBFS not linked; save persistence disabled (MEMFS only).");
      onStorageReady();
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
          onStorageReady();
          return;
        }
        log("[web:save] persistent storage mounted at /ogre");
        onStorageReady();
      });
    } catch (err) {
      log("[web:save] IDBFS mount failed: " + err);
      onStorageReady();
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
        uiSetPhase("error", "Graphics error");
        uiSetStatus("WebGL2 is unavailable in this browser.");
        return;
      }
      Module._ogre_gfx_set_canvas(handle, 320, 240);
      gfxStatus.textContent =
        "graphics: WebGL2 context ready (320x240) - renderer takes over once the game submits a display list";
      log("[web:gfx] WebGL2 context handed to the renderer (320x240)");
      uiSetPhase("loading", "Loading");
      uiSetStatus("Starting the renderer…");
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
      uiSetPhase("error", "Graphics error");
      uiSetStatus("The WebGL2 context could not be created.");
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
      if (!rendererUp) {
        uiSetPhase("loading", "Loading");
        uiSetStatus("Starting the renderer…");
      } else if (tasks === 0 || !hasDraws) {
        uiSetPhase("loading", "Loading");
        uiSetStatus("Waiting for the first frame…");
      } else {
        uiSetPhase("playing", "Playing");
        uiSetStatus("Running");
      }
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
        uiSetPhase("stalled", "Still loading");
        uiSetStatus("Still loading. The first frame can take a while; reload if the screen stays black.");
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
      uiSetStatus("The runtime is still starting. Try again in a moment.");
      return;
    }
    log("[web] Loading ROM: " + file.name + " (" + file.size + " bytes)...");
    uiSetPhase("loading", "Loading");
    uiSetStatus("Reading " + file.name + "…");

    file.arrayBuffer().then(function (buffer) {
      var bytes = new Uint8Array(buffer);
      Module.FS.writeFile("/rom.z64", bytes);
      log("[web] ROM written to the virtual filesystem. Starting runtime...");
      uiSetStatus("Starting the game…");
      uiDismissLauncher();
      initAudio();  // inside the user gesture so autoplay is allowed
      // initWebGL and the boot call wasm exports that the glue publishes a
      // moment after the runtime is up; see whenWasmExportsReady.
      whenWasmExportsReady(function () {
        initWebGL();  // first GL call from the main thread
        startBoot();
      });
    }).catch(function (err) {
      log("[web] Failed to read the ROM file: " + err);
      uiSetPhase("error", "ROM error");
      uiSetStatus("Could not read that ROM file.");
    });
  });

  // --- "Load save" file input ---------------------------------------------------
  //
  // Optional companion to OGRE_SAVE: pick a 32 KiB battery image and it is
  // written to the same path through the same writer, then the page reloads. The
  // reload keeps the image in IDBFS, and the stored ROM auto-starts from it, so
  // no query string is needed. (OGRE_SAVE re-imports on every page load; this
  // input is a one-off write.)
  if (saveInput) {
    saveInput.addEventListener("change", function () {
      var file = saveInput.files && saveInput.files[0];
      if (!file) {
        return;
      }
      if (!moduleReady() || !Module.FS) {
        log("[web:save] WASM runtime not ready yet; retry in a moment.");
        uiSetStatus("The runtime is still starting. Try again in a moment.");
        return;
      }
      log("[web:save] loading save file: " + file.name + " (" + file.size + " bytes)...");
      uiSetStatus("Importing save data…");
      file.arrayBuffer().then(function (buffer) {
        writeBatterySave(new Uint8Array(buffer), file.name, function (ok) {
          if (ok) {
            uiSetStatus("Save data imported. Reloading…");
            log("[web:save] reloading the page so the runtime reads the imported battery");
            window.setTimeout(function () { window.location.reload(); }, 250);
          } else {
            uiSetStatus("The save data could not be imported.");
          }
        });
      }).catch(function (err) {
        log("[web:save] failed to read the save file: " + err);
        uiSetStatus("Could not read that save file.");
      });
    });
  }

  // --- Startup ------------------------------------------------------------------

  whenModuleReady(function () {
    log("[web] WASM runtime initialized. Select a ROM to begin (or use the stored one).");
    uiSetStatus("Preparing local storage…");
    if (saveImport.armed) {
      log("[web:save] OGRE_SAVE=" + saveImport.spec + " -> " + SAVE_IMPORT_PATH +
          " (imported after the /ogre mount, before the boot)");
    }
    mountPersistentStorage();
  });
})();
