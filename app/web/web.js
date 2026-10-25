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
  var loadedRomName = "";
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
  // --- The Esc overlay's settings, bindings and audio --------------------------
  //
  // The native build opens a panel over the game (app/src/overlay.cpp) with
  // MAIN, CONTROLS, SETTINGS and DEBUG. The browser has the same tabs. SETTINGS
  // reads and writes the shared C++ settings module through the
  // `_ogre_web_settings_*` exports, so it is the same `settings.cfg` the native
  // port uses (GAME SPEED, WIDESCREEN, MOUSE ENABLED). CONTROLS has to live in
  // JavaScript because the browser input path is JavaScript; its bindings are
  // kept in localStorage. SOUNDS/VOLUME control the page's AudioWorklet, and
  // WINDOW is the browser's Fullscreen API. DEBUG reads the live Chaos Frame
  // byte through `_ogre_web_chaos_frame`.

  var WEB_CONTROLS_KEY = "ogre.web.controls.v1";
  var WEB_AUDIO_KEY = "ogre.web.audio.v1";

  function readStoredJson(key, fallback) {
    try {
      var raw = window.localStorage.getItem(key);
      if (!raw) {
        return fallback;
      }
      var value = JSON.parse(raw);
      return value === null || value === undefined ? fallback : value;
    } catch (e) {
      return fallback;
    }
  }

  function writeStoredJson(key, value) {
    try {
      window.localStorage.setItem(key, JSON.stringify(value));
    } catch (e) {
      // Storage can be disabled (private mode, a file:// page). The setting
      // still applies for this page; only persistence is lost.
    }
  }

  // Pad sources use the browser's Standard Gamepad indices, including the two
  // triggers and Guide, so the CONTROLS tab can bind every button the browser
  // reports. A right-stick direction is an axis source.
  var PAD_BUTTON_LABELS = [
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start",
    "L-Stick", "R-Stick", "D-Up", "D-Down", "D-Left", "D-Right", "Guide"
  ];

  // The same fourteen N64 rows and field descriptions as `input_map.cpp`.
  var N64_BUTTONS = [
    { name: "A", bit: 0x8000, field: "Selects the highlighted object.", key: "KeyX", pad: { type: "button", index: 0 } },
    { name: "B", bit: 0x4000, field: "Cancels a selection.", key: "KeyZ", pad: { type: "button", index: 1 } },
    { name: "Z", bit: 0x2000, field: "No field action listed.", key: "KeyC", pad: { type: "button", index: 10 } },
    { name: "START", bit: 0x1000, field: "Pauses the game.", key: "Enter", pad: { type: "button", index: 9 } },
    { name: "L", bit: 0x0020, field: "No field action listed.", key: "KeyQ", pad: { type: "button", index: 4 } },
    { name: "R", bit: 0x0010, field: "Brings up the action menu.", key: "KeyE", pad: { type: "button", index: 5 } },
    { name: "D-UP", bit: 0x0800, field: "Moves the cursor.", key: "ArrowUp", pad: { type: "button", index: 12 } },
    { name: "D-DOWN", bit: 0x0400, field: "Moves the cursor.", key: "ArrowDown", pad: { type: "button", index: 13 } },
    { name: "D-LEFT", bit: 0x0200, field: "Moves the cursor.", key: "ArrowLeft", pad: { type: "button", index: 14 } },
    { name: "D-RIGHT", bit: 0x0100, field: "Moves the cursor.", key: "ArrowRight", pad: { type: "button", index: 15 } },
    { name: "C-UP", bit: 0x0008, field: "Raises the camera angle.", key: "KeyI", pad: { type: "button", index: 2 } },
    { name: "C-DOWN", bit: 0x0004, field: "Lowers the camera angle.", key: "KeyK", pad: { type: "button", index: 3 } },
    { name: "C-LEFT", bit: 0x0002, field: "Toggles the overview map.", key: "KeyJ", pad: { type: "none" } },
    { name: "C-RIGHT", bit: 0x0001, field: "Toggles scroll modes, and pauses the game while held.", key: "KeyL", pad: { type: "none" } }
  ];

  var KEY_LABELS = {
    Enter: "Enter", NumpadEnter: "Num Enter", Space: "Space", Tab: "Tab",
    ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
    Backspace: "Backspace", Delete: "Delete", Escape: "Esc",
    ShiftLeft: "L-Shift", ShiftRight: "R-Shift", ControlLeft: "L-Ctrl",
    ControlRight: "R-Ctrl", AltLeft: "L-Alt", AltRight: "R-Alt",
    Backquote: "`", Minus: "-", Equal: "=", BracketLeft: "[", BracketRight: "]",
    Backslash: "\\", Semicolon: ";", Quote: "'", Comma: ",", Period: ".",
    Slash: "/", CapsLock: "Caps", Home: "Home", End: "End", PageUp: "PgUp",
    PageDown: "PgDn", Insert: "Insert", NumLock: "Num", ScrollLock: "Scroll",
    PrintScreen: "PrtSc", Pause: "Pause", ContextMenu: "Menu"
  };

  function keyLabel(code) {
    if (!code) {
      return "-";
    }
    if (KEY_LABELS[code]) {
      return KEY_LABELS[code];
    }
    if (/^Key[A-Z]$/.test(code)) {
      return code.slice(3);
    }
    if (/^Digit[0-9]$/.test(code)) {
      return code.slice(5);
    }
    if (/^Numpad[0-9]$/.test(code)) {
      return "Num " + code.slice(6);
    }
    var fn = /^F([0-9]{1,2})$/.exec(code);
    if (fn) {
      return "F" + fn[1];
    }
    return code;
  }

  function clonePad(pad) {
    if (!pad || pad.type === "none") {
      return { type: "none" };
    }
    if (pad.type === "axis") {
      return { type: "axis", axis: pad.axis, dir: pad.dir };
    }
    return { type: "button", index: pad.index };
  }

  function padSourceLabel(pad) {
    if (!pad || pad.type === "none") {
      return "-";
    }
    if (pad.type === "axis") {
      if (pad.axis === 3) {
        return pad.dir < 0 ? "R-Stick Up" : "R-Stick Down";
      }
      if (pad.axis === 2) {
        return pad.dir < 0 ? "R-Stick Left" : "R-Stick Right";
      }
      return "-";
    }
    var label = PAD_BUTTON_LABELS[pad.index];
    return label === undefined ? "-" : label;
  }

  function validPad(pad) {
    if (!pad || pad.type === "none") {
      return { type: "none" };
    }
    if (pad.type === "button" && Number.isInteger(pad.index) && pad.index >= 0 &&
        pad.index < PAD_BUTTON_LABELS.length) {
      return { type: "button", index: pad.index };
    }
    if (pad.type === "axis" && (pad.axis === 2 || pad.axis === 3) &&
        (pad.dir === -1 || pad.dir === 1)) {
      return { type: "axis", axis: pad.axis, dir: pad.dir };
    }
    return { type: "none" };
  }

  function defaultBindings() {
    return N64_BUTTONS.map(function (button) {
      return { key: button.key, pad: clonePad(button.pad) };
    });
  }

  function loadBindings() {
    var stored = readStoredJson(WEB_CONTROLS_KEY, null);
    var out = defaultBindings();
    if (!Array.isArray(stored) || stored.length !== N64_BUTTONS.length) {
      return out;
    }
    for (var i = 0; i < out.length; i++) {
      var row = stored[i];
      if (!row || typeof row !== "object") {
        continue;
      }
      if (typeof row.key === "string" || row.key === null) {
        out[i].key = row.key;
      }
      if (row.pad !== undefined) {
        out[i].pad = validPad(row.pad);
      }
    }
    return out;
  }

  var inputBindings = loadBindings();
  updateInputHint();

  function saveBindings() {
    writeStoredJson(WEB_CONTROLS_KEY, inputBindings);
  }

  function resetBindings() {
    inputBindings = defaultBindings();
    saveBindings();
    updateInputHint();
  }

  function updateInputHint() {
    var el = document.getElementById("input-hint");
    if (!el) {
      return;
    }
    function keyFor(name) {
      for (var i = 0; i < N64_BUTTONS.length; i++) {
        if (N64_BUTTONS[i].name === name) {
          return keyLabel(inputBindings[i].key);
        }
      }
      return "-";
    }
    el.textContent = keyFor("A") + " A · " + keyFor("B") + " B · " +
                     keyFor("START") + " Start · " +
                     keyFor("D-UP") + "/" + keyFor("D-DOWN") + "/" +
                     keyFor("D-LEFT") + "/" + keyFor("D-RIGHT") + " Move";
  }

  var webAudioSettings = readStoredJson(WEB_AUDIO_KEY, {});
  if (urlFlag("audio") !== null) {
    webAudioSettings.enabled = urlFlag("audio") === true;
  }
  if (typeof webAudioSettings.enabled !== "boolean") {
    webAudioSettings.enabled = false;
  }
  if (typeof webAudioSettings.volume !== "number" || !isFinite(webAudioSettings.volume)) {
    webAudioSettings.volume = 100;
  }
  webAudioSettings.volume = Math.max(0, Math.min(100, Math.round(webAudioSettings.volume)));
  var audioEnabled = webAudioSettings.enabled;
  var audioVolume = webAudioSettings.volume;

  function saveAudioSettings() {
    writeStoredJson(WEB_AUDIO_KEY, { enabled: audioEnabled, volume: audioVolume });
  }

  // The fullscreen state is the browser's, not a saved native setting: a page
  // cannot enter fullscreen without a user gesture. The row reflects the live
  // state and toggling it calls the same button the chrome uses.
  function browserFullscreen() {
    return !!document.fullscreenElement;
  }

  function wasmInt(name, fallback) {
    if (!moduleReady() || typeof Module[name] !== "function") {
      return fallback;
    }
    try {
      return Module[name]();
    } catch (e) {
      return fallback;
    }
  }

  function settingsGameSpeed() {
    return wasmInt("_ogre_web_settings_game_speed", 1);
  }
  function settingsWidescreen() {
    return wasmInt("_ogre_web_settings_widescreen", 0) !== 0;
  }
  function settingsMouse() {
    return wasmInt("_ogre_web_settings_mouse", 0) !== 0;
  }
  function settingsDisplayExperiments() {
    return wasmInt("_ogre_web_settings_display_experiments", 0) !== 0;
  }

  function callWasm(name) {
    if (!moduleReady() || typeof Module[name] !== "function") {
      return false;
    }
    try {
      var args = Array.prototype.slice.call(arguments, 1);
      Module[name].apply(Module, args);
      return true;
    } catch (e) {
      log("[web:options] " + name + " failed: " + e);
      return false;
    }
  }

  function setGameSpeed(value) {
    callWasm("_ogre_web_settings_set_game_speed", value);
    syncStorage();
    updateWidescreenAspect();
    refreshOptions();
  }
  function setWidescreen(on) {
    callWasm("_ogre_web_settings_set_widescreen", on ? 1 : 0);
    syncStorage();
    updateWidescreenAspect();
    refreshOptions();
  }
  function setMouseEnabled(on) {
    callWasm("_ogre_web_settings_set_mouse", on ? 1 : 0);
    syncStorage();
    refreshOptions();
  }

  // --- The in-game options overlay (Esc) --------------------------------------
  //
  // Mirrors the native panel's tabs and rows (app/src/ui.cpp build_rows). The
  // page owns the rendering and keyboard/pad navigation; the values that live
  // in the runtime go through the web_settings exports.

  var optionsDialog = document.getElementById("options-dialog");
  var optionsBody = document.getElementById("options-body");
  var optionsTabs = document.getElementById("options-tabs");
  var optionsState = {
    open: false,
    tab: "main",
    selected: 0,
    capture: -1,
    rows: [],
    padPrev: {}
  };

  function escapeHtml(text) {
    return String(text == null ? "" : text)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function radioTokens(values, labels, current) {
    var parts = [];
    for (var i = 0; i < values.length; i++) {
      var on = current === i;
      parts.push('<span class="opt is-clickable' + (on ? " is-on" : "") +
                 '" data-option="' + i + '">' +
                 (on ? "[x] " : "[ ] ") + escapeHtml(labels[i]) + "</span>");
    }
    return parts.join("");
  }

  function radioRow(label, values, labels, current, setting, desc) {
    return {
      kind: "row", label: label, setting: setting, values: values,
      radioLabels: labels, current: current, desc: desc || ""
    };
  }

  function volumeValue(percent) {
    var cells = 11;
    var handle = Math.max(0, Math.min(cells - 1, Math.round(percent / 10)));
    var out = '<span class="options-slider">';
    for (var i = 0; i < cells; i++) {
      out += '<span class="cell' + (i === handle ? " is-on" : "") +
             '" data-option="' + i + '">' + (i === handle ? "|" : "-") + "</span>";
    }
    out += '<span class="pct">' + percent + "%</span></span>";
    return out;
  }

  function chaosBand(value) {
    if (value <= 35) return "LOW";
    if (value <= 64) return "NEUTRAL";
    return "HIGH";
  }

  function optionsRows() {
    var rows = [];
    var tab = optionsState.tab;
    if (tab === "main") {
      rows.push({ kind: "row", label: "Start Game", value: "ALREADY RUNNING", disabled: true });
      rows.push({
        kind: "row", label: "ROM",
        value: loadedRomName || "ROM LOADED",
        desc: "Use EXIT GAME to return to the launcher and choose a different file."
      });
      rows.push({
        kind: "row", label: "Exit Game",
        value: "Return to the launcher",
        desc: "Syncs the battery and reloads the page without auto-starting.",
        action: "exit"
      });
      rows.push({
        kind: "note",
        text: "The browser cannot restart the runtime in place, so EXIT GAME " +
              "returns to the launcher. The selected ROM and save stay in this " +
              "browser's storage."
      });
    } else if (tab === "controls") {
      rows.push({ kind: "section", label: "KEYBOARD + GAMEPAD TO N64" });
      for (var i = 0; i < N64_BUTTONS.length; i++) {
        var capture = optionsState.capture === i;
        var key = keyLabel(inputBindings[i].key);
        var pad = padSourceLabel(inputBindings[i].pad);
        rows.push({
          kind: "row", label: N64_BUTTONS[i].name, binding: i,
          value: capture ? "PRESS A KEY OR PAD BUTTON..." : ("KEY " + key + "   PAD " + pad),
          desc: capture ? "Esc cancels. Backspace clears the key, Delete clears the pad."
                        : N64_BUTTONS[i].field
        });
      }
      rows.push({
        kind: "row", label: "Reset bindings to defaults",
        value: "Restore the default keyboard and gamepad map.",
        action: "reset-bindings"
      });
      rows.push({ kind: "section", label: "L-STICK: N64 ANALOG STICK" });
      rows.push({ kind: "section", label: "R-STICK: ALSO PRESSES C" });
      rows.push({
        kind: "note",
        text: "Click a row, then press a key or a pad button. Backspace clears " +
              "the key and Delete clears the pad. The right stick's four " +
              "directions always press the C buttons in addition to these " +
              "bindings."
      });
    } else if (tab === "settings") {
      var speedValues = [1, 2, 4, 8];
      var speed = settingsGameSpeed();
      var speedIndex = speedValues.indexOf(speed);
      rows.push(radioRow("GAME SPEED", speedValues,
                         speedValues.map(String), speedIndex, "speed",
                         "The emulated clock's multiplier."));
      rows.push({ kind: "section", label: "DISPLAY" });
      rows.push(radioRow("WIDESCREEN", [0, 1], ["OFF", "ON"],
                         settingsWidescreen() ? 1 : 0, "widescreen",
                         "16:9 view on the mission and dialogue scenes."));
      if (settingsDisplayExperiments()) {
        rows.push({
          kind: "row", label: "RESOLUTION", value: "NATIVE RENDERER ONLY",
          desc: "The browser renderer has no resolution scale.", disabled: true
        });
        rows.push({
          kind: "row", label: "MSAA", value: "NATIVE RENDERER ONLY",
          desc: "The browser renderer has no multisample target.", disabled: true
        });
      }
      var fullscreenAvailable = typeof document.documentElement.requestFullscreen === "function";
      rows.push(radioRow("WINDOW", [0, 1], ["WINDOWED", "FULLSCREEN"],
                         browserFullscreen() ? 1 : 0, "window",
                         fullscreenAvailable ? "Browser fullscreen."
                                             : "Fullscreen is unavailable in this browser."));
      if (!fullscreenAvailable) {
        rows[rows.length - 1].disabled = true;
      }
      rows.push({ kind: "section", label: "AUDIO" });
      rows.push(radioRow("SOUNDS", [1, 0], ["ON", "OFF"],
                         audioEnabled ? 0 : 1, "sound",
                         "The browser's audio output."));
      rows.push({
        kind: "row", label: "VOLUME", slider: true, value: audioVolume,
        desc: "Output gain for the game audio."
      });
      rows.push({ kind: "section", label: "INPUT" });
      rows.push(radioRow("MOUSE ENABLED", [0, 1], ["OFF", "ON"],
                         settingsMouse() ? 1 : 0, "mouse",
                         "The pointer drives the world map and mission cursors."));
      rows.push({
        kind: "note",
        text: "GAME SPEED, WIDESCREEN and MOUSE ENABLED are the same " +
              "settings.cfg values the native port writes. The browser keeps " +
              "its own CONTROLS bindings in localStorage."
      });
    } else if (tab === "debug") {
      rows.push({ kind: "section", label: "GAME STATE" });
      var chaos = wasmInt("_ogre_web_chaos_frame", -1);
      rows.push({
        kind: "row", label: "Chaos Frame",
        value: chaos < 0 ? "GAME NOT RUNNING" : (chaos + "  " + chaosBand(chaos)),
        desc: chaos < 0 ? "The running game has no RDRAM image yet."
                        : "0-35 low, 36-64 neutral, 65-100 high."
      });
      var scene = wasmInt("_ogre_web_active_scene", 0xFFFF);
      rows.push({
        kind: "row", label: "Scene",
        value: scene === 0xFFFF ? "UNKNOWN" : "0x" + ("000" + scene.toString(16)).slice(-4),
        desc: "The dispatcher's active scene id (D_800E810E)."
      });
    }
    return rows;
  }

  function rowSelectable(row) {
    return row && row.kind === "row" && !row.disabled;
  }

  function firstSelectable(rows) {
    for (var i = 0; i < rows.length; i++) {
      if (rowSelectable(rows[i])) {
        return i;
      }
    }
    return 0;
  }

  function clampSelection() {
    if (!rowSelectable(optionsState.rows[optionsState.selected])) {
      optionsState.selected = firstSelectable(optionsState.rows);
    }
  }

  function renderValue(row) {
    if (row.radioLabels) {
      return radioTokens(row.values, row.radioLabels, row.current);
    }
    if (row.slider) {
      return volumeValue(row.value);
    }
    return escapeHtml(row.value || "");
  }

  function renderOptions() {
    if (!optionsState.open || !optionsBody) {
      return;
    }
    optionsState.rows = optionsRows();
    clampSelection();
    var html = [];
    for (var i = 0; i < optionsState.rows.length; i++) {
      var row = optionsState.rows[i];
      if (row.kind === "section") {
        html.push('<div class="options-row is-section" role="presentation">' +
                  '<span class="options-label">' + escapeHtml(row.label) + "</span></div>");
        continue;
      }
      if (row.kind === "note") {
        html.push('<p class="options-note">' + escapeHtml(row.text) + "</p>");
        continue;
      }
      var cls = "options-row";
      if (rowSelectable(row)) {
        cls += " is-selectable";
      }
      if (row.disabled) {
        cls += " is-disabled";
      }
      if (i === optionsState.selected && rowSelectable(row)) {
        cls += " is-selected";
      }
      html.push('<div class="' + cls + '" role="button" data-row="' + i + '"' +
                (row.disabled ? ' aria-disabled="true"' : "") + '>' +
                '<span class="options-label">' + escapeHtml(row.label) + "</span>" +
                '<span class="options-value">' + renderValue(row));
      if (row.desc) {
        html.push('<span class="options-desc">' + escapeHtml(row.desc) + "</span>");
      }
      html.push("</span></div>");
    }
    optionsBody.innerHTML = html.join("");
    var tabs = optionsTabs ? optionsTabs.querySelectorAll(".options-tab") : [];
    for (var t = 0; t < tabs.length; t++) {
      tabs[t].setAttribute("aria-selected",
                           tabs[t].getAttribute("data-tab") === optionsState.tab ? "true" : "false");
    }
    var hint = document.getElementById("options-hint");
    if (hint) {
      hint.textContent = optionsState.capture >= 0
        ? "PRESS A KEY OR PAD BUTTON · BACKSPACE CLEARS KEY · DELETE CLEARS PAD · ESC CANCELS"
        : "TAB SWITCHES TAB · UP/DOWN SELECT · LEFT/RIGHT CHANGE · ESC RESUMES THE GAME";
    }
  }

  function refreshOptions() {
    if (optionsState.open) {
      renderOptions();
    }
  }

  function selectTab(tab) {
    if (tab === "mods") {
      return;
    }
    if (["main", "controls", "settings", "debug"].indexOf(tab) < 0) {
      return;
    }
    optionsState.tab = tab;
    optionsState.capture = -1;
    optionsState.rows = optionsRows();
    optionsState.selected = firstSelectable(optionsState.rows);
    renderOptions();
  }

  function openOptions(tab) {
    if (!optionsDialog) {
      return;
    }
    if (!optionsState.open) {
      optionsState.open = true;
      optionsState.capture = -1;
      optionsState.padPrev = {};
      pressedKeys.clear();
      pushInput(0, 0, 0, 0, 1);
      callWasm("_ogre_web_mouse_leave");
      optionsDialog.hidden = false;
      log("[web:options] shown");
    }
    selectTab(tab || optionsState.tab || "main");
    if (typeof optionsDialog.focus === "function") {
      optionsDialog.focus();
    }
    updateWidescreenAspect();
  }

  function closeOptions() {
    if (!optionsState.open) {
      return;
    }
    optionsState.open = false;
    optionsState.capture = -1;
    if (optionsDialog) {
      optionsDialog.hidden = true;
    }
    pressedKeys.clear();
    pushInput(0, 0, 0, 0, 1);
    log("[web:options] hidden");
  }

  function exitToLauncher() {
    log("[web:options] exit game -> launcher");
    closeOptions();
    syncStorage();
    window.setTimeout(function () {
      window.location.href = window.location.pathname + "?menu";
    }, 120);
  }

  function selectableIndexFrom(index, delta) {
    var rows = optionsState.rows;
    if (!rows.length) {
      return 0;
    }
    var next = index;
    for (var step = 0; step < rows.length; step++) {
      next = (next + delta + rows.length) % rows.length;
      if (rowSelectable(rows[next])) {
        return next;
      }
    }
    return index;
  }

  function moveSelection(delta) {
    optionsState.selected = selectableIndexFrom(optionsState.selected, delta);
    renderOptions();
  }

  function setSoundEnabled(on) {
    audioEnabled = !!on;
    saveAudioSettings();
    if (window.ogreAudio) {
      window.ogreAudio.setEnabled(audioEnabled);
    }
    refreshOptions();
  }

  function setVolumePercent(percent) {
    audioVolume = Math.max(0, Math.min(100, Math.round(percent)));
    saveAudioSettings();
    if (window.ogreAudio) {
      window.ogreAudio.setVolume(audioVolume);
    }
    refreshOptions();
  }

  function chooseRowOption(index, optionIndex) {
    var row = optionsState.rows[index];
    if (!row || row.disabled) {
      return;
    }
    if (row.slider) {
      setVolumePercent(optionIndex * 10);
      return;
    }
    if (!row.values || optionIndex < 0 || optionIndex >= row.values.length) {
      return;
    }
    var value = row.values[optionIndex];
    switch (row.setting) {
      case "speed": setGameSpeed(value); break;
      case "widescreen": setWidescreen(value !== 0); break;
      case "window":
        if (browserFullscreen() !== (value !== 0)) {
          var button = document.getElementById("fullscreen-btn");
          if (button) {
            button.click();
          }
        }
        break;
      case "sound": setSoundEnabled(value !== 0); break;
      case "mouse": setMouseEnabled(value !== 0); break;
      default: break;
    }
    renderOptions();
  }

  function stepRow(index, delta) {
    var row = optionsState.rows[index];
    if (!row || row.disabled) {
      return;
    }
    if (row.slider) {
      setVolumePercent(audioVolume + delta * 5);
      return;
    }
    if (row.values) {
      var current = row.current >= 0 ? row.current : (delta > 0 ? -1 : row.values.length);
      var next = (current + delta + row.values.length) % row.values.length;
      chooseRowOption(index, next);
    }
  }

  function activateRow(index) {
    var row = optionsState.rows[index];
    if (!row || !rowSelectable(row)) {
      return;
    }
    optionsState.selected = index;
    if (typeof row.binding === "number") {
      optionsState.capture = row.binding;
      renderOptions();
      return;
    }
    if (row.action === "exit") {
      exitToLauncher();
      return;
    }
    if (row.action === "reset-bindings") {
      resetBindings();
      log("[web:options] bindings reset to defaults");
      renderOptions();
      return;
    }
    if (row.slider) {
      return;
    }
    if (row.values) {
      stepRow(index, 1);
    }
  }

  function optionsKeyDown(e) {
    if (!optionsState.open) {
      return;
    }
    if (optionsState.capture >= 0) {
      if (e.code === "Escape") {
        optionsState.capture = -1;
      } else if (e.code === "Backspace") {
        inputBindings[optionsState.capture].key = null;
        saveBindings();
        optionsState.capture = -1;
      } else if (e.code === "Delete") {
        inputBindings[optionsState.capture].pad = { type: "none" };
        saveBindings();
        optionsState.capture = -1;
      } else if (e.code) {
        inputBindings[optionsState.capture].key = e.code;
        saveBindings();
        optionsState.capture = -1;
      }
      e.preventDefault();
      e.stopPropagation();
      updateInputHint();
      renderOptions();
      return;
    }
    if (e.code === "Escape") {
      e.preventDefault();
      e.stopPropagation();
      closeOptions();
      return;
    }
    if (e.code === "Tab") {
      var order = ["main", "controls", "settings", "debug"];
      var at = order.indexOf(optionsState.tab);
      at = (at + (e.shiftKey ? order.length - 1 : 1)) % order.length;
      selectTab(order[at]);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "ArrowUp") {
      moveSelection(-1);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "ArrowDown") {
      moveSelection(1);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "ArrowLeft") {
      stepRow(optionsState.selected, -1);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "ArrowRight") {
      stepRow(optionsState.selected, 1);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "Enter" || e.code === "Space") {
      activateRow(optionsState.selected);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.code === "Home") {
      optionsState.selected = firstSelectable(optionsState.rows);
      renderOptions();
      e.preventDefault();
      return;
    }
    if (e.code === "End") {
      for (var i = optionsState.rows.length - 1; i >= 0; i--) {
        if (rowSelectable(optionsState.rows[i])) {
          optionsState.selected = i;
          break;
        }
      }
      renderOptions();
      e.preventDefault();
    }
  }

  // The pad drives the panel too, with the native mapping: D-pad moves, A
  // selects, B is back, LB/RB switch tab, and SELECT opens the panel from the
  // game (only while no binding uses that button).
  function optionsPadFrame(pads) {
    for (var p = 0; p < pads.length; p++) {
      var pad = pads[p];
      if (!pad || !pad.connected) {
        continue;
      }
      var previous = optionsState.padPrev[pad.index] || [];
      var pressed = [];
      for (var b = 0; b < pad.buttons.length; b++) {
        pressed[b] = !!pad.buttons[b] && pad.buttons[b].pressed;
      }
      function edge(code) {
        return !!pressed[code] && !previous[code];
      }
      if (optionsState.open && optionsState.capture >= 0) {
        var bound = false;
        for (var bi = 0; bi < pressed.length; bi++) {
          if (pressed[bi] && !previous[bi]) {
            inputBindings[optionsState.capture].pad = { type: "button", index: bi };
            saveBindings();
            optionsState.capture = -1;
            bound = true;
            break;
          }
        }
        if (bound) {
          updateInputHint();
          renderOptions();
        }
      } else if (optionsState.open) {
        if (edge(12)) { moveSelection(-1); }
        if (edge(13)) { moveSelection(1); }
        if (edge(14)) { stepRow(optionsState.selected, -1); }
        if (edge(15)) { stepRow(optionsState.selected, 1); }
        if (edge(0)) { activateRow(optionsState.selected); }
        if (edge(1)) { closeOptions(); }
        if (edge(4)) {
          var tabs = ["main", "controls", "settings", "debug"];
          var at = tabs.indexOf(optionsState.tab);
          selectTab(tabs[(at + tabs.length - 1) % tabs.length]);
        }
        if (edge(5)) {
          var tabsNext = ["main", "controls", "settings", "debug"];
          var atNext = tabsNext.indexOf(optionsState.tab);
          selectTab(tabsNext[(atNext + 1) % tabsNext.length]);
        }
      } else if (edge(8) && bootStarted) {
        var backBound = inputBindings.some(function (row) {
          return row.pad && row.pad.type === "button" && row.pad.index === 8;
        });
        if (!backBound) {
          openOptions("main");
        }
      }
      optionsState.padPrev[pad.index] = pressed;
    }
  }

  function updateWidescreenAspect() {
    var wide = false;
    if (moduleReady() && settingsWidescreen()) {
      wide = wasmInt("_ogre_web_widescreen_expand", 0) !== 0;
    }
    document.body.classList.toggle("is-widescreen", wide);
  }
  window.setInterval(updateWidescreenAspect, 250);

  if (optionsDialog) {
    window.addEventListener("keydown", optionsKeyDown, true);
    optionsDialog.addEventListener("click", function (e) {
      if (e.target === optionsDialog) {
        closeOptions();
      }
    });
    var optionsClose = document.getElementById("options-close");
    if (optionsClose) {
      optionsClose.addEventListener("click", closeOptions);
    }
    if (optionsBody) {
      optionsBody.addEventListener("click", function (e) {
        var optionEl = e.target.closest ? e.target.closest("[data-option]") : null;
        var rowEl = e.target.closest ? e.target.closest("[data-row]") : null;
        if (rowEl) {
          var index = parseInt(rowEl.getAttribute("data-row"), 10);
          if (optionEl) {
            chooseRowOption(index, parseInt(optionEl.getAttribute("data-option"), 10));
          } else {
            activateRow(index);
          }
          return;
        }
      });
    }
    if (optionsTabs) {
      optionsTabs.addEventListener("click", function (e) {
        var tabButton = e.target.closest ? e.target.closest("[data-tab]") : null;
        if (tabButton && !tabButton.disabled) {
          selectTab(tabButton.getAttribute("data-tab"));
        }
      });
    }
  }

  window.ogreOptions = {
    open: openOptions,
    close: closeOptions,
    refresh: refreshOptions,
    isOpen: function () { return optionsState.open; },
    getBindings: function () { return inputBindings.map(function (row) {
      return { key: row.key, pad: clonePad(row.pad) };
    }); }
  };

  window.setInterval(function () {
    // The DEBUG tab's two readouts are live. Rebuilding the whole panel would
    // reset the scroll position of the CONTROLS list, so the other tabs redraw
    // only when a value changes.
    if (optionsState.open && optionsState.tab === "debug") {
      renderOptions();
    }
  }, 500);

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

  var pressedKeys = new Set();

  function isBoundGameKey(code) {
    for (var i = 0; i < inputBindings.length; i++) {
      if (inputBindings[i].key === code) {
        return true;
      }
    }
    return false;
  }

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
      for (var i = 0; i < inputBindings.length; i++) {
        if (inputBindings[i].key && inputBindings[i].key === code) {
          mask |= N64_BUTTONS[i].bit;
        }
      }
    });
    return mask;
  }

  // A modal dialog owns the keyboard while it is open, so arrow keys can
  // scroll it instead of being swallowed as D-pad input.
  function dialogOwnsKeyboard() {
    // The options panel is a normal fixed overlay (not a modal <dialog>): a
    // modal element lives in the browser's top layer, which is not part of the
    // element the page puts in fullscreen, so the panel vanished when the
    // WINDOW row entered fullscreen.
    if (optionsState && optionsState.open) {
      return true;
    }
    return !!(document.querySelector && document.querySelector("dialog[open]"));
  }

  window.addEventListener("keydown", function (e) {
    if (!optionsState.open && bootStarted && e.code === "Escape") {
      e.preventDefault();
      openOptions("main");
      return;
    }
    if (dialogOwnsKeyboard()) {
      return;
    }
    if (isBoundGameKey(e.code)) {
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

  // Standard Gamepad mapping through the editable bindings. The CONTROLS tab's
  // pad column supplies the button indices; the fixed right-stick layer for the
  // C buttons stays, as it does in the native input path.
  function gamepadState(pad) {
    var buttons = 0;
    var b = pad.buttons || [];
    var axes = pad.axes || [];
    for (var i = 0; i < inputBindings.length; i++) {
      var source = inputBindings[i].pad;
      if (!source || source.type === "none") {
        continue;
      }
      if (source.type === "button") {
        if (source.index < b.length && b[source.index] && b[source.index].pressed) {
          buttons |= N64_BUTTONS[i].bit;
        }
      } else if (source.type === "axis") {
        var value = axes[source.axis] || 0;
        if (source.dir < 0 ? value < -0.5 : value > 0.5) {
          buttons |= N64_BUTTONS[i].bit;
        }
      }
    }
    // Right stick -> C buttons (axis 2 = RX, 3 = RY), always on top of the map.
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
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    optionsPadFrame(pads);
    if (optionsState.open) {
      // The panel owns the pad while it is up. Slot 0 stays connected with an
      // idle pad so the game's input poll sees a steady "no buttons" state.
      pushInput(0, 0, 0, 0, 1);
      for (var other = 1; other < 4; other++) {
        pushInput(other, 0, 0, 0, 0);
      }
      requestAnimationFrame(rafLoop);
      return;
    }
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
        muted: !audioEnabled,
        gain: audioVolume / 100
      });
      node.connect(audioContext.destination);
      // The worklet keeps draining the ring even while muted, so the game's
      // audio thread sees the same producer/consumer backpressure either way.
      window.ogreAudio = {
        enabled: function () { return audioEnabled; },
        volume: function () { return audioVolume; },
        setEnabled: function (on) {
          audioEnabled = !!on;
          saveAudioSettings();
          node.port.postMessage({ type: "mute", muted: !audioEnabled });
          log("[web:audio] " + (audioEnabled ? "unmuted (game audio will play - it screeches until the audio microcode is emulated)"
                                             : "muted"));
        },
        setVolume: function (percent) {
          audioVolume = Math.max(0, Math.min(100, Math.round(percent)));
          saveAudioSettings();
          node.port.postMessage({ type: "gain", gain: audioVolume / 100 });
          log("[web:audio] volume " + audioVolume + "%");
        }
      };
      audioContext.resume().then(function () {
        log("[web:audio] AudioWorklet connected (" + audioContext.sampleRate + " Hz context, state " +
            audioContext.state + "), game audio " +
            (audioEnabled ? "ENABLED (overlay or ?audio)" : "MUTED - add ?audio or use SETTINGS > SOUNDS"));
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
    // EXIT GAME reloads with `?menu`: keep the launcher and do not auto-start
    // from the stored ROM, so the player can choose it again (or another one).
    var menuMode = urlFlag("menu") === true;
    // If a previously validated ROM is persisted, boot without re-picking.
    var storedRom = moduleReady() && Module.FS && Module.FS.analyzePath &&
        Module.FS.analyzePath("/ogre/ogrebattle64-us-rev1.z64").exists;
    if (storedRom && !menuMode) {
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

  // --- Pointer input for the MOUSE ENABLED setting -----------------------------
  //
  // The native platform forwards the SDL pointer to map_cursor.cpp and
  // mission_cursor.cpp; the browser forwards the canvas's pointer events to the
  // same exports. `draw_w`/`draw_h` are the drawing buffer, except in a
  // widescreen expand scene where the 4:3 buffer is stretched to the canvas
  // box: there the 320x240 guest resolution is passed so the mapping covers the
  // full width instead of the cursor modules' aspect-fit pillarbox.
  var mouseCanvas = document.getElementById("game-canvas");

  function pointerToFramebuffer(e) {
    if (!mouseCanvas) {
      return null;
    }
    var rect = mouseCanvas.getBoundingClientRect();
    if (!rect.width || !rect.height) {
      return null;
    }
    var wide = document.body.classList.contains("is-widescreen");
    return {
      x: Math.round(e.clientX - rect.left),
      y: Math.round(e.clientY - rect.top),
      winW: Math.round(rect.width),
      winH: Math.round(rect.height),
      drawW: wide ? 320 : mouseCanvas.width,
      drawH: wide ? 240 : mouseCanvas.height
    };
  }

  function forwardPointerMove(e) {
    var p = pointerToFramebuffer(e);
    if (p) {
      callWasm("_ogre_web_mouse_move", p.x, p.y, p.winW, p.winH, p.drawW, p.drawH);
    }
  }

  if (mouseCanvas && mouseCanvas.addEventListener) {
    mouseCanvas.addEventListener("pointermove", forwardPointerMove);
    mouseCanvas.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) {
        return;
      }
      var p = pointerToFramebuffer(e);
      if (p) {
        callWasm("_ogre_web_mouse_click", p.x, p.y, p.winW, p.winH, p.drawW, p.drawH);
      }
    });
    mouseCanvas.addEventListener("pointerleave", function () {
      callWasm("_ogre_web_mouse_leave");
    });
    mouseCanvas.addEventListener("pointercancel", function () {
      callWasm("_ogre_web_mouse_leave");
    });
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
    loadedRomName = file.name;
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
