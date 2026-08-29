// Ogre Battle 64 — web shell glue (app/web/web.js)
//
// 1. Reads the user-selected ROM via the File API (local only).
// 2. Writes it into the wasm virtual filesystem at /rom.z64.
// 3. Calls ogre_start_boot() so a pthread runs the runtime boot.
// 4. Polls ogre_poll_milestones() and mirrors the runtime's progress to the
//    status area.
//
// See docs/WEB-PORT.md §10 and docs/WEB-PORT-DEPLOYMENT.md.

(function () {
  "use strict";

  var status = document.getElementById("status");
  var input = document.getElementById("rom-input");
  var lastMilestoneLen = 0;
  var pollTimer = null;

  function log(text) {
    status.textContent += text + "\n";
    status.scrollTop = status.scrollHeight;
  }

  function pollMilestones() {
    if (!Module || typeof Module.ccall !== "function") {
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

  input.addEventListener("change", function () {
    var file = input.files && input.files[0];
    if (!file) {
      return;
    }
    if (!Module || !Module.FS) {
      log("[web] WASM runtime not ready yet; retry in a moment.");
      return;
    }
    log("[web] Loading ROM: " + file.name + " (" + file.size + " bytes)...");

    file.arrayBuffer().then(function (buffer) {
      var bytes = new Uint8Array(buffer);
      Module.FS.writeFile("/rom.z64", bytes);
      log("[web] ROM written to the virtual filesystem. Starting runtime...");

      Module.ccall("ogre_start_boot", "void", [], []);
      log("[web] Boot thread started. Waiting for milestones...");

      if (pollTimer === null) {
        pollTimer = window.setInterval(pollMilestones, 250);
      }
    }).catch(function (err) {
      log("[web] Failed to read the ROM file: " + err);
    });
  });

  // If the runtime initialized before the user got here, note it.
  if (Module && Module.onRuntimeInitialized) {
    // onRuntimeInitialized is handled in index.html; nothing extra to do.
  }
})();
