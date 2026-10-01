# Handoff — 2026-10-01, session 120: `make release` prepares the bundle and starts the release

## Goal

Developer request: the CI release job failed with

```
Error: files.tar.gz records public commit c7bd9991c78c5524b93e3b716d7d8ee193a6f349, and this release builds dc98d5d70b77a8791a274308e720aa7374e1c996.
Error: this build is 10 commit(s) newer, so the bundle's generated code may predate it.
Error: Regenerate the bundle and commit it to the private data repository, then re-run this job:
Error:     make recomp && make bank-recomp && make rsp-recomp && make example-mods && tools/data-bundle.sh
```

Create a `make release` command that does that and starts a new GitHub release.

## Result

`make release VERSION=vX.Y.Z` (`tools/release.sh`) regenerates the game code and
the example mods, packs `files.tar.gz`, proves the packed code is the tree's
code, commits and pushes it to the private data repository, dispatches the
Release workflow with `draft=true` and waits for the three platform builds. The
tag is created by the workflow's `gh release create` on the commit the run built.
The developer chose the version (`v0.6.0`) and a draft.

The script refuses, before it writes anything:

- a version that already has a tag or a release,
- a tracked tree with uncommitted changes,
- a branch whose tip is not pushed to `origin`.

The three refusals exist because the bundle records `git rev-parse HEAD` and the
workflow's unpack step fails when that differs from the commit being built
(session 111). `make release-check` is the same script with `NO_PUSH=1`:
regenerate, pack, report, change nothing outside the tree.

It also corrects one stale sentence in the private repository's README, which
said the workflow *warns* on a commit mismatch where the unpack step has failed
on it since session 111. `grep -q` gates the edit, so a README that no longer
says it is left alone.

`tools/release.sh` derives the private repository slug from the Actions variable
`OGRE_DATA_REPO` (`gh variable list`), defaults the version to the newest tag with
its last field bumped, and takes `DATA_REPO`, `REGEN=0`, `DRAFT`, `NO_WAIT=1`
and `REPO` as overrides.

### The regeneration is what fixes the reported failure

The private bundle was generated at `c7bd999` (session 112), the commit v0.5.1
was tagged from, and it matches the tooling of that commit. Ten commits have
landed since, including three the release must carry:

| commit | change |
| --- | --- |
| `8c5cbba` | session 115: `0x80204C48` joins `yield_work_loop_branches` (menu slowdown) |
| `da8676c`, `be7efed` | session 116: the periodic `[snap]` stutter is behind `OGRE_SNAP` |
| `65640c5` | the gamepad fills N64 controller slot 0 (issue #13) |
| `8ce9878` onwards | session 114: the HD-background pack, whose hook `tools/hd_backgrounds.py` inserts into `BankEFuncs` |

Measured on the two bundles: the old one holds `yield_self(rdram)` at
`0x80204C48` in `BankNFuncs/funcs_2.c` where the tree does not, 48 `yield_self`
occurrences against the tree's 47, and no `ogre_hd_background(rdram)` call,
which session 114 added after the v0.5.1 tag. So v0.5.1 carries the menu
slowdown of session 115 and nothing from sessions 114 or 116.

A bundle is only ever correct for the commit it records, which is why the
workflow compares the two. Nothing regenerates it on its own, and the release
must be built from a commit whose code the bundle holds, so the regeneration is
part of preparing the release rather than a refresh that can be skipped.

### The first run failed on an unrelated pre-existing defect, which is fixed

The dispatched run `36884300969` unpacked the bundle successfully — the reported
failure is gone — and then failed on macOS at

```
FAIL  patches/n64modernruntime-ob64.patch does not describe tools/N64ModernRuntime
        ultramodern/src/function_trace.cpp: in the patch, and the tree does not change it
FAIL  patches/n64modernruntime-n64recomp.patch does not describe tools/N64ModernRuntime/N64Recomp
        LiveRecomp/live_generator_wasm.cpp: in the patch, and the tree does not change it
FAIL  patches/rt64-plume-ob64.patch does not describe tools/RT64/src/contrib/plume
        plume_vulkan.cpp: changed in the tree and in no patch, so it ships nowhere
```

`tools/patchcheck.py` passed on this machine and failed on the hosted runner,
and the cause is the same in all three lines: **the check could not see a file
that a patch creates.** `patches/n64modernruntime-ob64.patch` creates
`ultramodern/src/function_trace.cpp` (`new file mode 100644`, 925 lines) and
`patches/n64modernruntime-n64recomp.patch` creates
`LiveRecomp/live_generator_wasm.cpp`. The runner's "Prepare third-party trees"
step applies those two patches with a plain `git apply` and never stages them,
so the new files sit untracked in the tree, and `git diff HEAD` does not report
untracked files. `check` therefore saw no block for them and printed "in the
patch, and the tree does not change it". On this machine the files are in the
index, which is why the same check passes here.

Reproduced at the pinned commits: `git -C tools/N64ModernRuntime worktree add
debug/pcheck-rt 589bbf0`, `git apply --ignore-whitespace ../../patches/n64modernruntime-ob64.patch`
there, then `ls-files --others --exclude-standard` lists
`ultramodern/src/function_trace.cpp` and
`git hash-object` on it is `786872e2de96a25c7cef3e63c6f313f404311996`, the blob
the patch's `index 0000000..786872e` line names. The file's content is correct;
only the check's view of the tree is wrong.

The third line is a second defect in the same function. `plume_vulkan.cpp` is in
`tools/rt64-plume-sdl.patch`, which the hosted path applies to the plume tree as
well, and not in `patches/rt64-plume-ob64.patch`, which was the only patch file
`check` compared that tree against. Reproduced with `git -C tools/RT64 worktree
add debug/pcheck-rt64 4337374`, `git submodule update --init --recursive`, and the
workflow's three `git apply --ignore-whitespace` commands.

Both are fixed in `tools/patchcheck.py`:

- `tree_changes()` is `git diff HEAD` plus an untracked-file pass. The untracked
  pass asks `git ls-files --others --exclude-standard`, which respects the
  repositories' own `.gitignore` files, and diffs each remaining file against
  `/dev/null` with `git diff --no-index`, which produces the "new file mode"
  block a patch carries. Diffing against the empty tree instead does not report
  an untracked path.
- The check now groups the targets by tree and covers each tree's changes with
  the union of every patch that targets it, so a path only a sibling patch
  carries is not a missing hunk. `tools/rt64-plume-sdl.patch` is in `TARGETS` as
  the plume tree's second patch rather than being special-cased by name.

Still checked, on the patched pristine trees: a covered file whose content
drifts (`librecomp/src/heap.cpp: the tree's diff differs from the patch`), an
untracked new file no patch carries, and a tree change no patch carries.
`--fix` still refuses a tree that more than one patch targets.

### Verified

- `make release-check VERSION=v0.6.0` ran the whole path on this machine: `make
  bank-recomp` (34 units, `check-banks` OK), `make recomp` (14 tail-call sites
  repaired), `make rsp-recomp` (njpeg + audio), `make example-mods` (Docker
  `gcc-mips-linux-gnu`, both `.nrm` written), then `tools/data-bundle.sh`
  reporting 190 entries, 21 `RecompiledFuncs` files, 34 bank units, 2 `RspFuncs`
  files, 2 `.nrm`, sha256 `70948b5b…`.
- The script's own drift check unpacked that bundle and compared it with the
  tree: `RecompiledFuncs/`, all 34 `Bank*Funcs/`, `RspFuncs/` and
  `app/src/bank_funcs.inc` identical, and `ogre-data.txt` records the HEAD
  commit. This is the check the workflow cannot make.
- The tree-clean refusal fired on the uncommitted `Makefile` and workflow edits
  before regeneration, and the post-regeneration check caught a stale function
  name during the dry run, which was fixed.
- The stale-bundle report was exercised with a clean, pushed tree and a bundle
  whose `ogre-data.txt` named `c7bd999`: it printed `the existing bundle was
  generated at c7bd999, 11 commit(s) behind 318e2d6…, and is replaced`.
- The README correction was run against the private repository's own
  `README.md` copy and produced the intended two lines.
- The bundle was compared with the private repository's copy: 5 differing
  entries (`ogre-data.txt`, `BankEFuncs/funcs_0.c`, `BankNFuncs/funcs_2.c`, both
  `.nrm`). The two `.nrm` files are the same size and differ only in their ZIP
  member timestamps.
- The first dispatched run (`36884300969`) cleared the unpack step that the
  developer reported and failed later at `tools/patchcheck.py`; that defect is
  fixed and verified as above. `SKIP_BUNDLE=1` was added to `tools/release.sh` so
  the retry reuses the bundle already committed to the private repository
  (`b89d7ba`, `generated code at 2ef54ec, with the example mods`) instead of
  regenerating it; its drift check still runs.

### Open

- The `.nrm` mods are not byte-reproducible: two builds of the same source differ
  in the ZIP timestamps. It does not affect the package.
- `tools/data-bundle.sh` records `public tree: clean|modified` from
  `git status --porcelain --ignore-submodules=all`. A release run sees `clean`
  because the script refuses a modified tree; the dry run recorded `modified`
  because a documentation edit was in the tree.

## Files changed

- `tools/release.sh` (new).
- `Makefile` — `release` and `release-check` targets, added to `.PHONY`.
- `.github/workflows/release.yml` — the `draft` input defaults to `true`.
- `docs/guides/app-build.md` — the release section documents `make release`,
  `make release-check` and the overrides.
- `tools/patchcheck.py` — see new files and a second patch for one tree.
- `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, this file.

No probe was added. Generated code was regenerated, not edited, and the
regeneration reproduced the tree byte for byte. The two test worktrees
(`debug/pcheck-rt`, `debug/pcheck-rt64`) were removed; `git -C tools/RT64
worktree list` and `git -C tools/N64ModernRuntime worktree list` show only their
main trees.
