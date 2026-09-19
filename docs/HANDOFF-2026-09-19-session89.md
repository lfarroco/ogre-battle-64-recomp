# Handoff — 2026-09-19, session 89: releases build on GitHub-hosted runners from a private data repository

**Goal (developer):** session 87b ended with releases building only on a machine
that has the ROM, because the recompiler's output is gitignored and a hosted
runner cannot generate it. The developer's next step was *"I will setup a new
private repo"*. Its name is deliberately not written down anywhere in this
repository; it is the value of the `OGRE_DATA_REPO` Actions variable.

**Result:** the private repository holds a `files.tar.gz` produced by
`tools/data-bundle.sh`. `.github/workflows/release.yml` pulls that archive from
it when the Actions variable `OGRE_DATA_REPO` is set and the secret
`OGRE_DATA_TOKEN` exists, and keeps the self-hosted labels when they are not. No
hosted run has happened yet.

---

## 1. What the private repository carries, and why that payload

`RecompiledFuncs/`, the 34 `Bank*Funcs/`, `RspFuncs/` and
`app/src/bank_funcs.inc` are the app's build inputs and are gitignored. The
developer chose to host those four outputs rather than the ROM:

- A hosted runner then needs only clang/CMake/ninja, and no ROM ever reaches CI.
- The alternative (hosting the ROM and recompiling during the release) would need
  `splat` plus a built `N64Recomp`/`RSPRecomp` toolchain on the runner, which this
  repository has no bootstrap step for, and the recompilation of 34 banks would
  run on every release.

Both payloads are covered by the same session-87b conclusion: the generated C is
a mechanical translation of the ROM's instructions. Hosting it privately keeps
it out of this repository and out of the release archives. It does not change
what the published binary contains.

## 2. The archive

`tools/data-bundle.sh` writes `dist/ogre-data/files.tar.gz` (an alternate output
path is its first argument) with these members at the archive root:

```
RecompiledFuncs/
Bank*Funcs/            (34 directories)
RspFuncs/
app/src/bank_funcs.inc
ogre-data.txt
```

`ogre-data.txt` records the public commit, whether that tree was modified, and
the generation time. The workflow warns when it is a tag build and the recorded
commit differs from `GITHUB_SHA`.

The script refuses to run when an input is missing, and writes nothing in that
case. Its checks are the ones `tools/release-build.sh` performs plus the
`Bank*Funcs/` glob that `app/CMakeLists.txt` uses.

The archive is a snapshot. A change that alters the generated code — a
`config*.toml` function size, a `cross_bank.py` dispatch target, a recompiler
pin — needs `make recomp && make bank-recomp && make rsp-recomp` followed by
another `tools/data-bundle.sh`.

## 3. The workflow contract

| name | kind | value |
|---|---|---|
| `OGRE_DATA_REPO` | Actions **variable** | private repository slug, for example `OWNER/PRIVATE-REPO` |
| `OGRE_DATA_TOKEN` | Actions **secret** | fine-grained token with `Contents: Read` on that repository |

`jobs.build.runs-on` is
`${{ vars.OGRE_DATA_REPO != '' && matrix.hosted || fromJSON(matrix.fallback) }}`.
Each matrix entry now carries `hosted` (`macos-14`, `ubuntu-24.04`,
`windows-2022`) and `fallback` (the previous self-hosted label arrays). An empty
variable returns `false` from `&&` and therefore `fromJSON(matrix.fallback)`, so
the self-hosted path is unchanged. `vars` is one of the contexts GitHub allows
in `runs-on`; `actionlint`'s generated availability table lists
`github, inputs, matrix, needs, strategy, vars` for that key.

Steps added for the hosted path, all guarded by `vars.OGRE_DATA_REPO != ''`:

1. **Check the data repository configuration** — fails with the secret's name
   when the variable is set and the token is empty.
2. **Fetch the generated game code** — `actions/checkout@v4` with `repository`
   and `token`, into `ogre-data/`.
3. **Unpack the generated game code** — `tar xzf ogre-data/files.tar.gz -C .`,
   then verifies the four inputs and prints their counts and the metadata.
4. **Install Linux build dependencies** — `libgtk-3-dev`, `libvulkan-dev`,
   `libx11-dev`. The ubuntu-24.04 image has CMake, GCC and ninja but not these.
5. **Ensure the macOS Metal toolchain** — probes `xcrun -sdk macosx metal`, and
   runs `xcodebuild -downloadComponent MetalToolchain` only when absent.
6. **Install Windows build dependencies** — `choco install make -y`; the
   windows-2022 image has CMake and VS 2022 but no `make`, and
   `tools/release-build.sh` drives `make dist`.

**Prepare third-party trees** changed from "fail when `tools/N64ModernRuntime` is
missing" to "clone and patch it when missing", which is what a hosted runner
needs:

```
git clone https://github.com/N64Recomp/N64ModernRuntime.git tools/N64ModernRuntime
git -C tools/N64ModernRuntime checkout 589bbf018a3e6d3646ddf7de1e7919f1b7e99bb1
git -C tools/N64ModernRuntime submodule update --init --recursive
( cd tools/N64ModernRuntime          && git apply ../../n64modernruntime-ob64.patch )
( cd tools/N64ModernRuntime/N64Recomp && git apply ../../../n64modernruntime-n64recomp.patch )
```

The two patches have different targets. `n64modernruntime-ob64.patch` is the
outer tree; `n64modernruntime-n64recomp.patch` is the `N64Recomp` **submodule**
inside it, which `librecomp/CMakeLists.txt` adds with
`add_subdirectory(${PROJECT_SOURCE_DIR}/../N64Recomp …)` and links as
`N64Recomp LiveRecomp`. The separate `tools/N64Recomp` checkout is the
recompiler CLI used by `make recomp`; a dist build only needs `recomp.h`, which
the submodule provides, so the hosted path does not clone it.

The RT64 submodules need patches too, and a hosted runner checks them out
pristine. A second hosted-only step, **Patch the RT64 submodules**, applies
three patches through a small `apply_patch` helper that skips one already in the
tree (`git apply --reverse --check`):

- `rt64-ob64.patch` to `tools/RT64` — 23 files, the whole local RT64 diff.
- `rt64-plume-ob64.patch` to `tools/RT64/src/contrib/plume` — the Metal fix in
  `plume_metal.cpp`.
- `tools/rt64-plume-sdl.patch` to the same directory — the SDL/Vulkan fix in
  `plume_vulkan.cpp`.

The self-hosted path is untouched by that step: its working tree already has the
first two, and `tools/release-build.sh` still attempts the SDL patch itself. The
`src/contrib/dxc` submodule (`rt64/dxc-bin`) arrives through the recursive
checkout, so no step fetches it.

## 4. What was verified, and what was not

Verified in this session:

- `bash -n tools/data-bundle.sh`, and a YAML parse of the workflow.
- `tools/data-bundle.sh` against the current tree: 188 entries, 7.5 MB,
  `RecompiledFuncs/` 21 files, 34 bank units, `RspFuncs/` 2 files, sha256
  `d4478c02b80092adc0795714ad60dbd7de047c9c6eecf436ca658f985727ba45`.
- The archive's layout, by extracting it exactly as the workflow does
  (`tar xzf … -C <dir>`, then checking the four paths): all present, 34 bank
  directories.
- The missing-input path, by running the script from an empty root: exit 1, the
  four names printed, no file written.
- Both patches apply cleanly (`git apply --check`) to a fresh
  `N64ModernRuntime` clone at `589bbf01` with submodules, and after applying
  them `diff -r --exclude=.git --exclude=build` against `tools/N64ModernRuntime`
  reports no difference — a hosted runner builds against the same runtime tree
  this machine does.
- The `runs-on` expression's `vars` context, against `actionlint`'s generated
  table.
- The RT64 patch set, from local objects only (no network): `rt64-ob64.patch` is
  identical to `git -C tools/RT64 diff` except for the `Subproject commit
  …-dirty` line for plume, so the local RT64 tree is the pinned commit plus that
  patch. Both plume patches apply to a pristine `plume` copy, and the
  `apply_patch` helper reports "already applied" on a second pass for all three.

Not verified:

- No hosted run has happened. Nothing here was executed on a GitHub runner.
- The Windows release build has never been built anywhere (session 87), and the
  `choco install make` step is the image's documented gap, not an observed
  failure.
- The Linux package list is `docs/guides/app-build.md`'s list for the RT64 path
  on a local machine, not packages observed missing from the runner image. The
  ubuntu-24.04 README does not list GTK, Vulkan or X11 development packages.
- The macOS Metal probe assumes `xcodebuild -downloadComponent` exists on the
  image's default Xcode; the macos-14 README lists 15.4 as default and 16.x as
  available.
- `tools/rt64-plume-sdl.patch` is **not** in this machine's plume tree (the
  local build compiles the Metal path), so no local build has compiled
  `plume_vulkan.cpp` with it. It exists for SDL before 2.0.22 and is guarded by
  `SDL_VERSION_ATLEAST`, and the Linux hosted job is its first real user.
- `src/contrib/dxc` carries one untracked local file, `lib/x64/libz.dylib`.
  Whether the hosted macOS build fetches or needs it is unexamined.

## 5. What to do next

1. Commit this session's changes, then run `tools/data-bundle.sh` again so
   `ogre-data.txt` reads `public tree: clean`. (The archive produced during this
   session was built while these doc edits were uncommitted, so it recorded
   `modified`; its generated-code contents are unaffected.)
2. Copy `dist/ogre-data/files.tar.gz` into the private data repository and
   commit it there.
3. Create a fine-grained token with `Contents: Read` on that repository. Add it
   as the secret `OGRE_DATA_TOKEN`, and set the variable `OGRE_DATA_REPO` to that
   repository's slug.
4. Run the workflow with `workflow_dispatch` and an existing tag, or push a new
   tag, and read the first hosted failure if there is one.

The `dist/ogre-data/files.tar.gz` from step 1 is gitignored and is not a
repository artifact; the private repository is its home.

## 6. Files changed

* `tools/data-bundle.sh` — new. Produces the private repository's `files.tar.gz`
  and refuses to run without the generated code.
* `.github/workflows/release.yml` — header rewritten around the two code sources;
  matrix gained `hosted`/`fallback` and `runs-on` selects between them; the
  data-repository check, fetch and unpack steps; `Prepare third-party trees` now
  clones and patches the runtime; three hosted dependency steps.
* `docs/guides/app-build.md` — the self-hosted paragraph now points at the new
  option, and a "Building releases on GitHub-hosted runners" section records the
  archive contract, the regeneration trigger and the variable/secret names.
* `PLAN.md` — status entry.
* `docs/DECISIONS.md` — durable decision entry.
* `docs/HANDOFF-2026-09-19-session89.md` — this file.

No probes were added, so none had to be reverted. No generated code, ROM data or
extracted asset was added to this repository. `git status --short` shows the six
files above plus `m tools/RT64`, which is the project's own RT64 patch set
(`rt64-ob64.patch` applied to the submodule) and was already there before this
session.
