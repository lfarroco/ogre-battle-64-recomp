# Session 209 handoff — the public browser page: an experimental notice and battery export

## Goal

The browser renderer is being published on GitHub Pages while it is still
experimental. The developer asked for two things the page did not have: an
experimental disclaimer, and a battery save export. The page already imported a
save, but without an export a player could not take the battery the browser
wrote into the native build.

## Result

- **Experimental notice.** The launcher card carries one (`.launcher-disclaimer`):
  rendering, audio and saving are incomplete, scenes can crash, the build is
  unofficial, and the reader should use "Export save" to keep a copy. The Options
  MAIN tab repeats it in its note.
- **Battery export.** The topbar "Export save" button (`#export-save-btn`, beside
  "Load save") and the Options -> MAIN "Export battery save" row
  (`action: "export-save"`) download the runtime's battery image as
  `ogrebattle64-us-rev1.bin`.
- The image is read from the wasm FS at
  `/ogre/saves/ogrebattle64-us-rev1.bin`, the path `pi.cpp`'s saving thread
  writes with `update_save_file` whenever the game commits SRAM. It is the raw
  32 KiB logical image, which is the file the native build reads from
  `<config>/saves/ogrebattle64-us-rev1.bin`; no conversion or rename is needed.
- The reader checks the size (32768) and the `QuestOG3` device signature at
  `+0x04`, and retries once after 200 ms, because the runtime writes
  `<file>.temp` and copies it over the image. A missing file reports "No battery
  save in this browser yet"; a runtime that has not initialised the FS reports
  "still starting".
- `window.ogreSave = { export(), path, size }` is the page-level handle the probe
  uses. The topbar button and the Options row call the same function.
- The launcher's padding went from 24 px to 18 px so the taller card still fits
  the 642x482 game screen. The disclaimer is 3 lines; the desktop card is 434 px
  tall in a 482 px launcher and the 390x844 mobile card is 234 px in a 281 px
  launcher. Both have no scroll overflow.
- No workflow change was needed: `.github/workflows/pages.yml` already runs
  `make web-dist` and deploys `dist-web/`, so the next push to `main` publishes
  these files.

## Evidence

The static package is the GitHub Pages layout: a plain host with no COOP/COEP,
so the page's service worker supplies the headers and reloads once. The probe
ran against `python3 -m http.server --directory dist-web`, which is the same
configuration the Pages artifact serves.

- `make web` (Emscripten, incremental) succeeded, then `make web-dist` wrote the
  package. Only page files changed, so the wasm binary is unchanged.
- `node debug/probes/save.cjs --url
  'http://127.0.0.1:8940/?OGRE_SAVE=escort_suspend.bin&OGRE_SCENE=title'`
  reported:

  ```
  disclaimer present: "Experimental build: rendering, audio and saving are
  incomplete and scenes can crash. Use Export save to keep a copy. Unofficial,
  not affiliated with Nintendo or Quest."
  exportButton=true  optionsRow=true
  fsImage: /ogre/saves/ogrebattle64-us-rev1.bin (32768)
  filename=true  size=true  magic=true  exact=true
  SAVE ROUND TRIP OK
  ```

  `exact` is a byte comparison with `assets/escort_suspend.bin`. Both files hash
  to `7bb8916399e9b682ebf4258119c7ef9c47e9f89ab9a83cc2d8a912c179b31ec6`.
- `python3 tools/sramsave.py check debug/out/save-8940-ogrebattle64-us-rev1.bin`
  reports a bare 32 KiB logical image, the device magic at `0x04`, both save
  slots' checksums OK, and the map record at `0x30B0` valid.
- The runtime writes the battery file under Emscripten. A fresh boot with no
  import logged

  ```
  [save] no save file at /ogre/saves/ogrebattle64-us-rev1.bin; starting with a blank image (32768 bytes)
  [save] game reads the battery (SRAM) save: dev=0x00000000 size=0x100
  [save] game writes the battery (SRAM) save: dev=0x00000000 size=0x100
  [save] wrote /ogre/saves/ogrebattle64-us-rev1.bin (32768 bytes)
  ```

  and the file then held 32768 bytes, `QuestOG3` at `+0x04`, 1518 non-zero bytes
  — the same deterministic blank image session 66 measured on the native build.
  This is what makes the export current after an in-game save: it reads the file
  the saving thread just wrote, not only an imported image.
- Launcher captured at 1280x800 and 390x844; the card fits both (no
  `scrollHeight > clientHeight`). Proofs `docs/proofs/web-launcher-session209.png`
  and `docs/proofs/web-launcher-mobile-session209.png`.

## Files changed

- `app/web/index.html` — the disclaimer and its styles, the `#export-save-btn`
  topbar button, the launcher padding, `aria-label`s on both save buttons.
- `app/web/web.js` — `SAVE_GAME_ID`/`SAVE_SIZE`/magic constants,
  `readBatteryImage()`, `downloadBytes()`, `exportBatterySave()`,
  `window.ogreSave`, the Options row and its action, the button wiring, and the
  Options note's experimental sentence.
- `debug/probes/save.cjs` — new import -> export round-trip probe.
- `debug/package.json` — the `save` script.
- `docs/guides/web-probes.md` — the export probe and the page-contract entries.
- `docs/WEB-PORT-DEPLOYMENT.md` — the battery export and the notice.
- `docs/DECISIONS.md`, `PLAN.md`, `docs/STATUS-LOG.md`, this handoff.
- `docs/proofs/web-launcher-session209.png` and
  `-mobile-session209.png` — the launcher at 1280x800 and 390x844.

## Verification

- `EM_CACHE=<workspace>/.em-cache make web` (rule 15: `app/web/*` changed).
- `make web-dist`; the package check passed and is 21 MB.
- `debug/probes/save.cjs` on the package, plus `tools/sramsave.py check`.
- The blank-boot writer run (`/tmp/check-writer.cjs`, not committed).
- Launcher layout measured at 1280x800 and 390x844.

## Probes

No generated code was instrumented, so nothing needed reverting. The two
throwaway scripts were `/tmp/check-writer.cjs` and `/tmp/measure-launcher.cjs`.
`debug/probes/save.cjs` is a durable test of the page contract and stays.

The working tree also carried a pre-existing modified `tools/RT64` submodule at
the start and end of the session. This session did not touch it; `git status
--short` shows it as `m tools/RT64`.

## Open

- The export is the raw logical image. An emulator that wants parallel-n64's
  byteswapped `.sra` wrapper still needs `tools/sramsave.py export`; the page
  does not offer that format.
- The 200 ms retry covers the runtime's `.temp` -> image copy, but the mid-copy
  window was not measured at the file-system level; the probe compares a file
  read after the boot, not a read racing a save.
- The GitHub Pages deployment itself was not run in this session. The workflow
  and the deployed site already work; the local static-server run is the same
  package layout, and the push follows this record.
