# Session 111 — the hosted release failed because the data bundle predated the mods rule

Date: 2026-09-27. Follows session 108, which made `build/mods/*.nrm` a required
release input and added the check that then fired.

## Goal

The developer reported that the latest release build failed on GitHub Actions, on
every platform, with:

```
Error: build/mods/*.nrm is missing from files.tar.gz; run 'make example-mods' before tools/data-bundle.sh
Error: Process completed with exit code 1.
```

Find the cause and get the release building.

## Cause

The failing line is the workflow's own unpack check
(`.github/workflows/release.yml`, `mods=$(ls build/mods/*.nrm …)`). It is doing
its job. The input it reads is stale.

- `gh variable list` gives `OGRE_DATA_REPO = lfarroco/ogre-recomp-private` and
  `OGRE_DATA_TOKEN` is set, so `runs-on` takes the hosted branch and every matrix
  job unpacks `files.tar.gz` from that private repository
  (`release.yml` lines 141, 165-196).
- A fresh clone of that repository holds the Sep 19 archive: sha256
  `663a238d95f9e69b64171a0bdd6c184614d347e6361d4d1b8a03beb820560f92`, 188
  entries, byte-identical to this tree's old `dist/ogre-data/files.tar.gz`.
  `tar tzf` lists no `build/mods/` member and no `.nrm`.
- Session 108 changed the contract so the bundle must carry the mods, then added
  four checks. Nothing regenerated the private bundle. The last successful run
  (`Release`, v0.4.0, 2026-09-26) predates the check.

This is the "not fixed" section of `docs/HANDOFF-2026-09-27-session108.md` §"What
is not fixed", which named the regeneration step but could not perform it: the
bundle needs a machine with the ROM and a MIPS cross toolchain.

## What was done

1. Regenerated `dist/ogre-data/files.tar.gz` with `tools/data-bundle.sh` from this
   tree: 190 entries, `build/mods/exp-overflow.nrm` and
   `build/mods/skip-boot-logos.nrm`, no AppleDouble `._*` members, 7.5 MB.
2. Committed it to `lfarroco/ogre-recomp-private` as `4bd61cb` (pushed
   `d451859..4bd61cb`), with that repository's README updated to list
   `build/mods/*.nrm` and to say that regeneration covers mod changes.
3. Re-ran the failed run, `36349726660`.
4. Changed the unpack step's commit check from a tag-only `::warning::` to a
   failure for any build whose `ogre-data.txt` commit differs. The check prints
   the recorded commit, the commit being built, how many commits newer the build
   is when that commit is present, and the regeneration command
   (`make recomp && make bank-recomp && make rsp-recomp && make example-mods && tools/data-bundle.sh`).

## Evidence the regenerated bundle is current

The bundle's provenance is a claim in `ogre-data.txt`; this session checked it
against the tree instead of trusting it.

- `make recomp`: `diff -rq` against the pre-existing `RecompiledFuncs/` reports
  no difference (21 files). The run logged 15 dispatch targets and
  `rewrite: 14 tail-call site(s) repaired to call-and-continue`.
- `make bank-recomp`: all 34 `Bank*Funcs/` directories and
  `app/src/bank_funcs.inc` are identical to the pre-existing ones;
  `cross_bank.py check-banks` reports
  `check OK - no bank unit calls into a swappable range it does not own`.
- Extracting the new archive and diffing it against the freshly regenerated tree
  reports no difference, so the published bundle is exactly what this tree
  produces at commit `4e01bef`.
- The only differences from the old private bundle are the two `.nrm` members and
  `ogre-data.txt` (`71808ec` → `4e01bef`); `tar tzf` entry-list diff shows
  exactly two added lines.

The generated code is therefore unchanged by this session. The public commits
between `71808ec` and `4e01bef` did move recomp inputs (`config/`, `patches/`,
`tools/cross_bank.py`, `tools/gen_bank_funcs.py`, and `fdde395` moved the build
configs under `config/`), so the tree was regenerated rather than assumed, but
the output did not change.

The mods in the bundle were built 2026-09-20 and 2026-09-25, before several
commits that touch the app and the runtime, so the bundle's copies were checked
against a rebuild. `make example-mods` (Docker, `gcc-mips-linux-gnu`) rebuilt both
`.nrm` files, and unzipping the old and new archives gives identical
`mod_syms.bin`, `mod_binary.bin` and `mod.json`; the archives differ only in their
ZIP entry timestamps. The shipped mods therefore match the current source and the
current `modding.h` API, and the bundle did not need republishing for them.

## Verification

- `bash -n tools/data-bundle.sh tools/release-build.sh` — clean.
- `tools/data-bundle.sh` — `Example mods: 2 .nrm`, 190 entries, sha256
  `f2c482e1db81d09c85f199da9aa42bb0e052ff1774388058348f251cd9f7dce0`.
- `tar tzf dist/ogre-data/files.tar.gz` — both `.nrm` members, no `._*`,
  no `.DS_Store`.
- Drift check exercised in a shell harness on three inputs: a stale commit
  (`71808ec`, 47 behind) fails and prints the count, the matching commit passes,
  an unrelated commit fails. `tools/venv/bin/python -c 'import yaml; …'` parses
  the workflow, jobs `plan`, `build`, `release`.
- The private repository's own `git status` after the copy shows only
  `files.tar.gz` and `README.md` modified.
- `make example-mods`: both mods rebuild and their archive payloads are identical
  to the shipped ones.
- The re-run, `36349726660`, completed success. All three build jobs are
  `completed success`, and the unpack step is `completed success` on each. The
  Release job then created the release the dispatch was for,
  `v0.5.0`, as a **draft**, and attached the three archives. Downloaded and
  inspected: `ogre-battle-64-recomp-linux-x86_64.tar.gz` and
  `ogre-battle-64-recomp-macos-arm64.tar.gz` each contain
  `ogre-battle-64-recomp/mods/exp-overflow.nrm` and `skip-boot-logos.nrm`, which
  is the property whose absence produced the empty `mods/` of v0.4.0.

## What is not fixed

Nothing in the release path. `v0.5.0` is a **draft**, so the fixed packages are
not published yet: publishing it is the developer's decision.
The published `v0.4.0` assets still contain an empty `mods/`.

The release matrix is now strict: a bundle whose recorded commit differs from the
commit being released fails the job. Regenerating `files.tar.gz` is part of
preparing a release, and is documented as such in
`docs/guides/app-build.md` → "Building releases on GitHub-hosted runners". The
check exists because the runner has no ROM and cannot test whether an older
bundle still matches the source tree.

## Probes

None. No recompiled file, no app source and no submodule changed, so there is
nothing to revert. `make recomp` and `make bank-recomp` were run and produced
byte-identical output; `git status --short` shows `.github/workflows/release.yml`
modified and `tools/RT64` dirty (the project's own patch set, as before).

## Files changed

- `.github/workflows/release.yml` — the unpack step's commit check now fails
  rather than warning, with the regeneration command.
- `docs/guides/app-build.md` — the bundle contract, the mods requirement and the
  drift failure.
- `PLAN.md` — open issue 9 updated.
- `docs/STATUS-LOG.md` — this session.
- `docs/HANDOFF-2026-09-27-session111.md` — this file.

Outside this repository:

- `lfarroco/ogre-recomp-private`: `files.tar.gz` and `README.md`, commit
  `4bd61cb`.
