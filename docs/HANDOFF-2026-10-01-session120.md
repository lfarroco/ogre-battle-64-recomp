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
- The bundle was compared with the private repository's copy: 5 differing
  entries (`ogre-data.txt`, `BankEFuncs/funcs_0.c`, `BankNFuncs/funcs_2.c`, both
  `.nrm`). The two `.nrm` files are the same size and differ only in their ZIP
  member timestamps.

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
- `PLAN.md`, `docs/STATUS-LOG.md`, `docs/DECISIONS.md`, this file.

No probe was added. Generated code was regenerated, not edited, and the
regeneration reproduced the tree byte for byte.
