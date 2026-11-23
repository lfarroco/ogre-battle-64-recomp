# Session 216 handoff — bundled code mods in the browser build

> **Summary.** Session 216 handoff — **the browser build runs the four bundled code mods.** A wasm build cannot load a code mod: `LiveRecomp` needs sljit and the native hook path overwrites a function prologue, which WebAssembly does not allow. `tools/gen_web_mods.py` recompiles each `build/mods/*.nrm` ahead of time with `OfflineModRecomp`, links it into the app, and embeds the archive; `app/src/web_mods.cpp` installs each archive with `register_embedded_mod`, so the manifest, config, `mods.json` and the MODS tab are the runtime's. `librecomp` gains a wasm-only `StaticModCodeHandle`, and `tools/web_mod_hooks.py` inserts the entry hook/replacement calls into the generated game code. `OfflineModRecomp` gained the normal driver's static-function pass and a no-fall-through-tail mode for mods. Verified: `[boot] 4 bundled mod(s)`, all four load, `ogre_skip_boot_logos` takes the boot `0x09 -> 0x04` (vanilla is `0x09 -> 0x0A -> 0x04`), the MODS tab toggles and writes `/ogre/mods.json`, `make web` and `make app` clean. The data bundle was regenerated with `app/src/web_mods/` and the change was pushed as `264bca3`; the WebAssembly and Pages runs are green and the live site lists the four mods (`ogre_skip_boot_logos` boots `0x09 -> 0x04`). Open: the permanent-suspend replacement is not played through, and third-party code mods stay unsupported.

## Goal

Enable mods for the WebAssembly build. The developer chose the option that keeps
the port's four shipped mods usable in the browser (`mods/skip-boot-logos`,
`mods/exp-overflow`, `mods/permanent-suspend`, `mods/item-randomizer`), rather
than data-only mods or arbitrary third-party `.nrm` code mods.

## Result

The browser build now installs the four bundled `.nrm` archives through the
runtime's normal mod system and runs their code. The page's MODS tab lists them,
an OFF/ON row writes the same `mods.json` the native build writes, and a toggle
takes effect the next time the game starts.

The code path is different from native in exactly two places:

1. **Code loading.** WebAssembly has no runtime code generation. `LiveRecomp` is
   sljit, which emits x86-64/AArch64 machine code, and the native hook path
   overwrites a recompiled function's prologue, which a wasm function body
   cannot support. `tools/gen_web_mods.py` recompiles each `.nrm`'s MIPS ahead
   of time with `OfflineModRecomp` and links the C++ into the wasm app.
   `librecomp`'s wasm-only `StaticModCodeHandle` hands the runtime that compiled
   table instead of asking `LiveRecomp` for one.
2. **Hook and replacement dispatch.** `tools/web_mod_hooks.py` inserts a guarded
   call at the entry of each game function a bundled mod targets.
   `ogre_web_mod_entry_hook` calls `recomp::mods::run_hook` for the registered
   slot; `ogre_web_mod_replace` calls a registered replacement and returns so the
   original body does not run. A disabled mod registers nothing, so the injected
   calls are no-ops. Native builds compile the calls out (`#ifdef
   __EMSCRIPTEN__`).

A bundled mod can import an extended base export (the item randomizer imports
`recomp_log`). Native `ShimFunction` makes the two-argument trampoline with the
live recompiler; the wasm stub now keeps a fixed pool of 64 C++ trampolines that
carry `(ext_func, argument)`, so the import calls the same function with the same
mod index. Without this the import resolved to `nullptr` and a mod that called
it would trap.

## Evidence

### The four mods load and the hook fires

Browser run, `ogre_skip_boot_logos` enabled before the ROM was loaded, page
served by `python3 debug/server.py`, URL
`.../app/web/index.html?OGRE_SCENE_LOG=1`:

```
[stderr] [LiveRecomp] no runtime code generation in this (wasm) build; only the bundled precompiled mods can load.
[stderr] [boot] 4 bundled mod(s)
[stdout] Loading mod ogre_skip_boot_logos
[stderr] [scene] t=2144ms id=0x0009 descriptor=0x8018FB70 mask=0x00008000
[stderr] [scene] t=2623ms id=0x0004 descriptor=0x8018FB40 mask=0x00008000
```

Scene `0x09` goes straight to the title. The vanilla boot enters the publisher
stills (`0x0A`) between them; the native mod is verified the same way and its
comment records why `0x0A` must not be entered.

A second run enabled all four mods at once:

```
[stdout] Loading mod ogre_permanent_suspend
[stdout] Loading mod ogre_skip_boot_logos
[stdout] Loading mod ogre_item_randomizer
[stdout] Loading mod ogre_exp_overflow
[stderr] [scene] ... id=0x0009 ...
[stderr] [scene] ... id=0x0004 ...
```

No `Error loading mods`, no page error, and the title is still reached. That
covers the three entry-hook mods loading their code and
`ogre_permanent_suspend` registering its replacement; the suspend-save behaviour
itself is not played through (open item 1).

### The MODS tab and `mods.json`

`ogre_web_mods_scan` returns 4. The tab renders one OFF/ON row per mod with the
manifest's short description. Clicking the ON cell of `Permanent Suspend Save`
changes the row to ON and writes:

```json
{
    "enabled_mods": [
        "ogre_permanent_suspend"
    ],
    "latest_game_mode": "",
    "mod_order": [
        "ogre_permanent_suspend",
        "ogre_skip_boot_logos",
        "ogre_item_randomizer",
        "ogre_exp_overflow"
    ]
}
```

The first UI attempt did not write the file: `setModEnabled` called the raw
export through `callWasm`, which passes a JS string as a number. It now uses
`Module.ccall(..., ["string", "number"], ...)`.

### Builds

- `EM_CACHE=/Users/momo/dev/ogre/.emcache make web` — clean, `[100%] Built target
  ogrebattle64`.
- `make app` — clean, native `ogrebattle64` links; the injected call sites are
  inside `#ifdef __EMSCRIPTEN__` and `web_mods.cpp` is not in the native sources.
- `make patch-check`: the three patches this session changed pass
  (`n64modernruntime-ob64`, `n64modernruntime-n64recomp`, `n64recomp-ob64`).
  `patches/rt64-ob64.patch` fails with `src/render/rt64_framebuffer_renderer.cpp:
  in the patch, and the tree does not change it`. That drift pre-dates this
  session: `tools/RT64`'s working tree does not contain the change the patch
  carries, and `make patch-fix` would rewrite the patch from the tree and drop
  that release fix. The patch was restored with `git checkout --
  patches/rt64-ob64.patch` and the drift is left for the RT64 owner.

## What is in the tree

| file | change |
|---|---|
| `tools/gen_web_mods.py` | new: `.nrm` -> `OfflineModRecomp` -> `app/src/web_mods/*.cpp`, embedded archive table, `instrumented.txt` |
| `tools/web_mod_hooks.py` | new: apply / revert / check the entry call sites, driven by `instrumented.txt` |
| `app/src/web_mod_recomp.h` | new: the ABI the generated mod C++ compiles against (context, `MEM_*`, reloc macros) without `recomp.h`'s single `section_addresses`/`get_function` globals |
| `app/src/web_mods.{hpp,cpp}` | new: embedded-mod registration, the page exports, and the two C helpers the game code calls |
| `app/src/web_mods/` | generated, gitignored: four `<mod id>.cpp`, `bundled_mods.{h,cpp}`, `instrumented.txt` |
| `app/src/main_web.cpp` | sets `entry.mod_game_id`; factors `make_game_entry()`; `ogre_web_early_init()` registers the config path, game and mods so the page can list them before a ROM is picked |
| `app/CMakeLists.txt` | builds `ogrebattle64_web_mods` from the generated files, adds `web_mods.cpp`, exports the `_ogre_web_mods_*` functions |
| `app/web/index.html` | MODS tab enabled |
| `app/web/web.js` | MODS rows, toggle, scan after IDBFS mount, `ccall` string marshalling |
| `tools/N64ModernRuntime/librecomp/{include/librecomp/mods.hpp,src/mods.cpp,src/recomp.cpp}` | `StaticModInfo`, `StaticModCodeHandle`, wasm replacement map, `save_config_now`/`flush_mods_config`, `run_entry_hook`/`run_static_replacement`/`find_static_mod` |
| `tools/N64ModernRuntime/N64Recomp/LiveRecomp/live_generator_wasm.cpp` | message now says only the bundled precompiled mods can load; `ShimFunction` uses a fixed pool of C++ trampolines so an extended export import (`recomp_log`) is callable |
| `tools/N64Recomp/{include/recompiler/context.h,src/recompilation.cpp,OfflineModRecomp/main.cpp}` | `emit_fallthrough_tails`; static-function pass; the flag is off for mods |
| `Makefile` | `web-mods`, `web-mods-check`; `make web` depends on the check and applies the call sites; `make recomp` applies them |
| `tools/data-bundle.sh` | requires and ships `app/src/web_mods/` |
| `docs/guides/app-build.md` | build order and "The browser build: bundled code mods" |
| `PLAN.md`, `docs/DECISIONS.md`, this file | record |

## GitHub Pages packaging

The Pages site serves `dist-web/`, which `make web-dist` writes from the same
`make web` build, so the mods travel in `ogrebattle64.wasm`; the package needs no
`mods/` directory and no extra request. Verified on the package itself, served by
`python3 -m http.server --directory dist-web 8940` (no COOP/COEP, so the service
worker path is used, which is the Pages configuration):

```
url= http://127.0.0.1:8940/
isolated= true
mods= [{"id":"ogre_permanent_suspend",...},{"id":"ogre_skip_boot_logos",...},
       {"id":"ogre_item_randomizer",...},{"id":"ogre_exp_overflow",...}]
tab has EXP Overflow= true Skip Boot Logos= true
ERRORS []
```

`grep -ac ogre_skip_boot_logos dist-web/ogrebattle64.wasm` is 2, and the package's
wasm is byte-identical to `build-wasm/ogrebattle64.wasm`.

The Pages build takes its generated game code and mod C++ from the private data
bundle, so the deployment was two steps:

1. `tools/data-bundle.sh` wrote `dist/ogre-data/files.tar.gz` at commit
   `264bca3` (7.5 MB, sha256 `e0f8d87c…`, public tree clean). It contains
   `app/src/web_mods/{bundled_mods.cpp,h,instrumented.txt}` and the four
   `<mod id>.cpp`. It was committed to `lfarroco/ogre-recomp-private` as
   `9017934`.
2. The code was pushed to `main` as `264bca3`. Workflow runs
   `45218823360` (`WebAssembly`) and `45218823415` (Pages) are both green, and
   the deploy job published the package.

Both browser workflows fail the unpack step with the regeneration command when
a bundle has no `app/src/web_mods/bundled_mods.cpp` or `instrumented.txt`,
instead of failing later at `make web`.

The live site was measured after the deploy:
`https://lfarroco.github.io/ogre-battle-64-recomp/` is cross-origin isolated,
`ogre_web_mods_scan` returns the four mods, the MODS tab renders them, and with
`ogre_skip_boot_logos` enabled and a ROM loaded the live page logs
`[scene] id=0x0009 -> id=0x0004`.

## Probes

No probe was added to the tree and none was reverted. Verification used
throwaway Node/Playwright scripts in `/tmp` (`testmods`, `dbgmods*`,
`uitest2`), driving the page's own exports and its MODS tab. The injected call
sites in `RecompiledFuncs/` are the feature, not a probe; `make recomp` re-applies
them and `tools/web_mod_hooks.py --check` proves the set.

`grep -rl probe216 RecompiledFuncs/ Bank*Funcs/ app/` is empty.

## Open

1. **`permanent-suspend` is not played through in the browser.** The replacement
   registers and its call site is injected, but the route that loads a suspend
   save and checks `0x30B0` (session 212's native test) has not been run in the
   browser. The next check is a browser run with `?OGRE_SAVE=...` that enters
   Load Game and loads slot 15, then reads the battery image from `/ogre/saves/`.
2. **Third-party `.nrm` code mods remain unsupported.** A wasm build can only run
   code compiled into it. A future bundled mod with a hook or replacement must
   run `make web-mods` so `instrumented.txt` and the generated table include it;
   a return hook is still refused (`HooksUnavailable`).
3. **The private data bundle must be regenerated** with `make example-mods &&
   make web-mods` before `tools/data-bundle.sh` for the hosted wasm job to have
   `app/src/web_mods/`. `make web` instruments `RecompiledFuncs/` itself, so a
   bundle whose game code predates the call sites still builds; the generated mod
   directory itself cannot be produced on the runner.
4. **`patches/rt64-ob64.patch` drift** is pre-existing and unrelated; see the
   build section above.
