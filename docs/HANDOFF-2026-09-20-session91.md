# Handoff — 2026-09-20, session 91: the mod system is on, with a shipped example

**Goal (developer):** the game is 99.05 % recompiled, one intention of the
project is modding, so ship example mods with the client and let the player turn
them off. Candidate ideas offered: skip the intro with Start, widescreen, mouse
support, hard mode. The developer chose, from a first batch of questions:
enable mods plus a toggle in the launcher plus one reference mod; the example
set is **skip boot logos** (a code mod with an enum config); hard mode is
dropped for now.

**Result:** the runtime's mod system was already complete and already called
from `recomp::start()`, but `GameEntry::mod_game_id` was never set, so no mod was
ever loaded. The port now sets it, scans before the start screen, and gives the
start screen a **MODS** panel that toggles mods and edits their options.
`mods/skip-boot-logos/` is the reference example (one entry hook that sends the
boot straight to the title) and `make example-mods` builds it. Verified: the true
baseline reaches the title at 27.7 s; with the mod the title is at 3.0 s and a
swap-chain capture shows it rendering correctly. Three runtime bugs blocked hooks
on this port and are fixed in `n64modernruntime-ob64.patch`. A first version that
skipped the stills scene instead of never entering it broke the title, and the
developer's report of that is section 3.

The developer's brief for the example changed during the session: the first
version carried an `Enum` option (`Off` / `Publisher stills` / `Boot intro and
stills`) and hooked both scene updates. The instruction then was *"let's simplify
the mod. it always skips everything. so, in practice, what it does is booting at
the title scene (after loading the battery)"*, so the option and the second hook
are gone and the mod always goes to the title.

---

## 1. What was already there, and the one line that was missing

`recomp::start()` calls `recomp::mods::initialize_mods()` and `scan_mods()`
(`tools/N64ModernRuntime/librecomp/src/recomp.cpp:946`), which create
`<config>/mods`, `<config>/mod_config` and read `mods.json`. The whole system is
present: manifest parsing, code mods with entry and return hooks, replacements,
native libraries, per-mod config schemas, ordered enable/disable, embedded mods,
and the authoring tools `RecompModTool` / `OfflineModRecomp` /
`RecompModMerger`.

The load is gated on `GameEntry::mod_game_id`:

```c
// recomp.cpp, wait_for_game_started()
if (!game_entry.mod_game_id.empty()) {
    mod_load_errors = mod_context->load_mods(...);
}
```

`app/src/main.cpp` set `entry.game_id` and never `entry.mod_game_id`, so the scan
opened and parsed every mod and threw it away. `app/src/game.hpp` now adds
`MOD_GAME_ID = "ogrebattle64"` (stable, separate from the revision-specific
`GAME_ID` that names the ROM and save file), and `main.cpp` sets it, then calls
`initialize_mods()` + `scan_mods()` before the start screen. The second scan
inside `recomp::start` is safe: `scan_mod_folder` begins with `close_mods()`
and `load_mods_config()` re-reads `mods.json`, so the toggles written by the
screen survive.

`app/src/renderer.cpp:9` records that the port was adapted from RecompFrontend
"minus the RecompFrontend UI / texture-pack / mod wiring". The mod wiring is now
present and the UI is the start screen.

## 2. The start screen

`app/src/launcher.cpp` gains a `ModPanel`: a flat list with one row per mod and
one row per visible option of that mod.

* `UP` / `DOWN` move the selection.
* `SPACE` toggles the selected mod, or steps the selected option.
* `LEFT` / `RIGHT` step the selected option's value.
* `ENTER` plays when a ROM is ready; a click on a row activates it, a click
  elsewhere plays (or opens the ROM picker when no ROM is ready).
* The selected mod's `short_description` is shown under the list, and the
  controls are printed at the bottom.

A toggle calls `recomp::mods::enable_mod`, which writes `mods.json`; an option
value calls `recomp::mods::set_mod_config_value`, which the runtime's config
thread writes to `mod_config/<mod id>.json`. Enum, bool and number options are
editable; a string option is displayed only.

The screen is shown whenever a ROM is ready **and** at least one mod is
installed, so a shipped mod always has a switch. A vanilla install (empty
`mods/`) still boots straight in, which keeps the scripted harnesses and the
"put the ROM next to the app" flow unchanged.

`dist/ogre-battle-64-recomp/` already carried `mods/`, `mod_config/` and
`mods.json` (created by `initialize_mods` on a local run); `make dist` now also
copies `build/mods/*.nrm` into `mods/`.

## 3. The example mod

`mods/skip-boot-logos/` is a complete code mod:

| file | what it is |
|---|---|
| `src/skip_boot_logos.c` | the entry hook and why it targets scene `0x09` |
| `include/modding.h` | the section macros (`RECOMP_HOOK`, `RECOMP_PATCH`, ...) |
| `mod.toml` | the manifest |
| `mod.ld` | the mod link script |
| `README.md` | what it does and how to build and install it |

The boot runs scene `0x09` (boot intro, ~9.4 s), scene `0x0A` (publisher stills,
~16.3 s) and scene `0x04` (title, prologue text over the scrolling clouds). Each
scene's per-frame update writes the next scene into the pending-scene word
`D_800C4C26` as `0x8000 | id`. The cartridge save is read before the first scene,
so the title is reached with the battery loaded.

The mod puts one entry hook on scene `0x09`'s update and writes the title's id:

```
func_80177DCC  scene 0x09's update -> 0x8004   (the title)
```

The boot then runs `0x09` for one frame and enters the title at t ≈ 3.0 s.

### Scene `0x0A` must not be entered

The first version of this mod hooked **both** updates and let the option `mode`
choose how much to skip. The first run of the always-skip behaviour produced a
broken title, reported by the developer with a screenshot: a black frame with a
dark band and a band of RGB noise, and the game then stopped producing frames.

Measured, with the swap-chain capture (`OGRE_CAPTURE_PRESENT`) and
`OGRE_SCENE_LOG=1`:

| configuration | scene `0x0A` | title | display lists in the run | title renders |
|---|---|---|---|---|
| no mod | 16.3 s | 27.7 s | 99 | yes |
| mod installed, option `Off` | 16.3 s | 30.0 s | 93 | **yes** — clouds and party |
| mod forcing `0x0A` -> `0x04` | 0.28 s | 12.5 s | **35** | **no** — no display list after the title enters |
| mod forcing `0x09` -> `0x04` (never `0x0A`) | never entered | 3.0 s | 46 | **yes** — prologue text and clouds |

The `mode = Off` row is the control for "the two functions were regenerated and
patched": the title renders normally, so the runtime fixes are not the cause. The
last row is the fix.

The cause is `0x0A`'s own setup and teardown. Its enter `func_801A3A10`
(`.streamedC`) allocates buffers and initialises overlay-C globals; its update
`func_801A3CA0` is a 17-state machine over `D_801CA70C` that plays the stills and
returns non-zero only when the sequence is complete; its leave `func_801A3BB8`
frees the buffers. Forcing the transition from an entry hook means the state
machine is abandoned one frame in, and the title then inherits inconsistent
overlay-C state — its update `func_80177A58` reads `func_8019BD0C()` and takes no
branch that draws. Entering the title from `0x09` never runs `0x0A`, and it
renders.

## 4. Three runtime fixes a hook needed

All three are in `n64modernruntime-ob64.patch`, regenerated with the documented
command (`git -C tools/N64ModernRuntime diff HEAD -- . ':(exclude)N64Recomp'`),
and verified to `git apply --check` against a tree archived from the pinned
commit `589bbf0`.

**(a) The recompiler's section index is not the code-section position.**
A mod's hook is resolved by `section_rom` + `function_vram`. The runtime
regenerates the vanilla function and `populate_reference_symbols`
(`mods.cpp`) resolves each call through
`get_func_by_section_index_function_offset(jump_details.section, offset)`, which
indexes `sections_info.code_sections` **by position**. A generated
`RelocEntry.target_section` is the recompiler's number for an ELF section; in
this project the code sections are 3/6/9/12/16 of 23, and `func_80177DCC`'s two
`jal` relocs carry 12 (`.streamedB`) and 16 (`.streamedC`). Both are ≥
`num_code_sections` (5), so the lookup returned null, `apply_regenlist` returned
an empty handle and the mod failed with no detail:

```
Error loading mods:
: Failed to load mod code (Code mod loading internal error)
```

`recomp::overlays::get_code_section_index_from_section_index` translates the
index, and `context_from_regenerated_list` uses it. This was diagnosed from the
data, not a probe: `RecompiledFuncs/recomp_overlays.inl` holds
`func_80177DCC` at section offset `0xCE4C` with relocs `0xce54` (target 12),
`0xce64` (target 16) and `0xce74/0xce78` (target 8).

**(b) HI16/LO16 relocations against sections that hold no code.**
`0xce74`/`0xce78` target section 8 (`.main_bss`) at offset `0x15E76` — the
`lui at, 0x800C` / `sh v0, 0x4C26(at)` pair that writes `D_800C4C26`. This
project recompiles with `use_absolute_symbols = true`, so the indirect
already holds the final address (the offline C is `ctx->r1 = S32(0X800C << 16);
MEM_H(0X4C26, ctx->r1)`), and the runtime registers a load address for the code
sections only — `section_addresses[8]` is zero. Regenerating the reloc as a
reference symbol emitted a load from address 0 and the following store ran off
the end of RDRAM:

```
[crash] signal 10 on N64 thread 4 fault=0x1af911e74 rdram=0x12f8fc000 offset=0x80015E74
```

`context_from_regenerated_list` now leaves `R_MIPS_HI16` / `R_MIPS_LO16`
immediates alone when the target section has no code section.

**(c) macOS will not make the executable's `__TEXT` writable.**
`patch_func` redirects a vanilla function to its hooked version by writing
`movabs rax, <hook>; jmp rax` over its entry, around `mprotect`-based
`unprotect`. On this machine (x86-64 macOS) both fail:

```
mprotect(..., PROT_READ|PROT_WRITE) -> -1 EACCES
vm_protect(..., VM_PROT_READ|VM_PROT_WRITE[|VM_PROT_COPY]) -> 2 (KERN_PROTECTION_FAILURE)
```

The write then raises `SIGBUS` inside `apply_regenlist + 0x2CE` (the `movw
$0xb848, (%r12)` that starts `patch_func`). `linker -segprot,__TEXT,rwx,rwx`
sets `maxprot 0x7` but the write still faults. A standalone test showed the
working sequence: copy the page, then `vm_remap(mach_task_self(), &page, ...,
VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), copy, FALSE, &cur, &max,
VM_INHERIT_NONE)` with `max = rwx`, which replaces the page with a private copy
whose maxprot includes write. `unprotect` does that for each page in the patched
range and then `mprotect`s it `rw`; `protect` restores `r-x`. The copy is taken
before the remap, so the page's bytes do not change.

**Not verified on arm64.** The arm64 path is the same code, and an unsigned
macOS process may map `rwx` anonymous memory, but no Apple Silicon machine was
available. If it fails there, `vm_remap` returns an error and the symptom is the
same `SIGBUS`.

## 5. What was run

* `make mod-syms` and `make example-mods` (the latter builds MIPS in a
  `debian:bookworm-slim` container; Apple's clang has no MIPS target and only
  `mips-linux-gnu-binutils` is installed here).
* True baseline with nothing in `mods/`:
  `OGRE_PREF_DIR=/tmp/ogre-baseline OGRE_TEST_DROP=assets/ogre64.z64
  OGRE_SCENE_LOG=1 OGRE_EXIT_AFTER_MS=32000 ./build-app/ogrebattle64`
  → `/tmp/ogre-baseline.log`: `0x09` at 1938 ms, `0x0A` at 11370 ms, `0x04`
  (title) at **27687 ms**.
* The simplified mod installed, same command plus the capture
  (`OGRE_CAPTURE_PRESENT=/tmp/cap-e OGRE_CAPTURE_AFTER=1 OGRE_CAPTURE_EVERY=60`,
  `OGRE_EXIT_AFTER_MS=16000`) → `/tmp/cap-e.log`: `0x09` at 2758 ms, `0x04` at
  **3021 ms**, 46 display lists; the capture `/tmp/cap-e.360.ppm`
  (`/tmp/title-e.png`) shows the title's prologue text over the clouds.
* The four configurations compared for the title bug, all with
  `OGRE_CAPTURE_PRESENT`/`OGRE_SCENE_LOG=1` (`/tmp/cap-a.log`, `/tmp/cap2-b.log`,
  `/tmp/cap2-c.log`, `/tmp/cap-d.log`): baseline and mod-with-option-`Off` render
  the title (99 and 93 display lists); forcing `0x0A` -> `0x04` produces 35
  display lists and no frame after the title enters; forcing `0x09` -> `0x04`
  renders (`/tmp/cap-d.480.ppm`, `/tmp/title-d.png`).
* The start screen with the mod installed, captured with `screencapture`:
  `/tmp/ogre-launcher-shot.png` (title, prompt, error from a deliberately bad
  `OGRE_TEST_DROP`, the `MODS` panel with `[x] Skip Boot Logos`, its description
  and the controls line, and the save path). The shot is from the earlier
  two-option version, so it also shows the old `Skip: Publisher stills` row.
  **The panel's key and mouse handlers were not exercised end to end**: driving
  them needs synthetic SDL input, and `osascript`/System Events blocked on an
  accessibility permission this session could not grant. What is verified is
  that the panel renders with the mod; that the calls it makes
  (`recomp::mods::enable_mod`, `set_mod_config_value`) are the runtime's own; and
  that the runtime's write path works — a fresh config directory with only the
  `.nrm` present auto-enables the mod and writes `mods.json` with it in both
  `enabled_mods` and `mod_order` (the same `enable_mod` the panel calls). A
  hand-written `mod_config/ogre_skip_boot_logos.json` with `mode = Off` was read
  back and took the old two-hook version's hooks out of the picture, which is how
  the title bug was isolated to the skip rather than to the regeneration.
* `make dist` into `dist/ogre-battle-64-recomp/`, and the packaged `mods/` folder
  holds `skip-boot-logos.nrm`.
* `cmake --build build-app --target ogrebattle64 -j8` and the same for
  `build-null` (both succeed).
* `git apply --check` of the regenerated `n64modernruntime-ob64.patch` against a
  tree archived from `589bbf0`.

**No probes.** Nothing is tagged, no generated file was hand-edited, and no
`.nrm` is committed. The three changes to the vendored submodule are deliberate
and captured in `n64modernruntime-ob64.patch`.

## 6. Files changed

* `app/src/game.hpp` — `MOD_GAME_ID`.
* `app/src/main.cpp` — `entry.mod_game_id`, the early `initialize_mods()` +
  `scan_mods()`, and the start screen when mods are installed.
* `app/src/launcher.hpp` — `LauncherContext::ready_rom` and `mod_game_id`.
* `app/src/launcher.cpp` — `ModPanel` (model, input and rendering) and the
  mod-aware event loop.
* `mods/skip-boot-logos/**` — the example mod (new).
* `tools/build-example-mods.sh` — compiles the mods to MIPS and packages them
  (new).
* `Makefile` — `mod-syms` and `example-mods` targets, and the `dist` copy of
  `build/mods/*.nrm`.
* `.gitignore` — `mods/reference/*.toml`, `mods/*/build/`.
* `packaging/README-dist.txt` — the MODS section, and the start screen in HOW TO
  PLAY.
* `docs/guides/app-build.md` — a Mods section.
* `n64modernruntime-ob64.patch` — regenerated (three fixes above).
* `PLAN.md`, `docs/DECISIONS.md`, this file.

Vendored (gitignored) and re-applied from the patch: submodule
`tools/N64ModernRuntime`, files `librecomp/src/mods.cpp`,
`librecomp/src/overlays.cpp`, `librecomp/include/librecomp/overlays.hpp`.

## 7. Next leads

1. **Hooks in a streamed bank unit are unproven.** `get_vrom_to_section_map()`
   holds the sections registered by `register_base_overlays()`; a bank unit
   (`app/src/bank_overlays.cpp`) is not in it. A mod that wants to hook a battle
   or mission function needs that path, and the failure mode to expect is
   `InvalidHook` (section not found) rather than a silent miscompile.
2. **The panel is the only UI.** Per-mod option *descriptions* are not shown,
   a string option cannot be edited, and there is no mod ordering (which matters
   when two mods patch the same function). `recomp::mods::set_mod_index` is the
   runtime call for ordering.
3. **A second example mod would widen the reference.** A `Number` option and a
   `RECOMP_PATCH` (a replacement rather than a hook) are the two API surfaces the
   example does not exercise. Widescreen and mouse belong in client settings
   (RT64 has `AspectRatio` and nothing calls `set_graphics_config`), and
   mouse-to-stick is host input, not a mod.
4. **`make dist` does not build the mods.** It ships `build/mods/*.nrm` when
   present and prints a note when not. A release that should carry the example
   needs `make example-mods` first, or a CI step with Docker.
