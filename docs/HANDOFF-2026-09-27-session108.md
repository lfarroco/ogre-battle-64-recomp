# Session 108 handoff — the published release shipped an empty `mods/`

> **Summary.** Session 108 handoff — **the published release shipped an empty `mods/`.** Developer report from the releases page. The repo's `OGRE_DATA_REPO` variable is `lfarroco/ogre-recomp-private`, so the matrix runs on the hosted path and unpacks `files.tar.gz`; that bundle named only `RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc`, and `tools/data-bundle.sh`'s check did not test for mods, so the runner had no `build/mods/`. The `dist` recipe's copy step was an `if ls build/mods/*.nrm; then cp; else echo "no example mods"; fi`, which exits 0, so the package was built with an empty `mods/`; `tools/smoke-dist.sh` never looked at the directory. Verified against the live artifacts: `tar tzf` on the v0.4.0 macOS archive prints only `ogre-battle-64-recomp/mods/`, and on a fresh clone of the private repo lists no `.nrm`. Fixed at four layers: `dist-mods-check` is a prerequisite of `dist` and fails without a `.nrm`; `data-bundle.sh` requires and packs `build/mods/*.nrm`; `release-build.sh` checks before the app build; the workflow's unpack step fails on a bundle without them; and `smoke-dist.sh` fails a package with no `.nrm`. `packaging/README-dist.txt` now lists both shipped mods (session 103 added `exp-overflow` and the text still said "one"). Verified: `make dist` fails (exit 2) with `build/mods` absent and copies both mods when present; `data-bundle.sh` reports `Example mods: 2 .nrm` and the archive carries them; smoke PASSes with `carries 2 example mod(s)` and fails on a copy with the `.nrm` deleted. **Still open:** the published v0.4.0 assets and the private `files.tar.gz` predate the rule; the bundle must be regenerated and a new tag published.

## Goal

Developer report: the archives at
https://github.com/lfarroco/ogre-battle-64-recomp/releases have an empty
`mods/` directory, so the game ships without its mods.

## Result

Root cause found and fixed in the packaging path. `mods/` is now a required
release input at four layers, and no layer warns-and-continues.

## Evidence

The release does not run on a developer machine. Actions variable
`OGRE_DATA_REPO` is `lfarroco/ogre-recomp-private` (`gh variable list --repo
lfarroco/ogre-battle-64-recomp`), so `runs-on` selects `matrix.hosted` and every
job unpacks `files.tar.gz` from that private repository.

1. **The bundle carried no mods.** `gh repo clone
   lfarroco/ogre-recomp-private` then `tar tzf files.tar.gz` lists
   `RecompiledFuncs/ Bank*Funcs/ RspFuncs/ app/ ogre-data.txt` and no
   `build/mods` and no `.nrm`. `tools/data-bundle.sh`'s tar command named only
   `ogre-data.txt RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc`,
   and its `missing` check did not test for mods.
2. **The published archive has the empty directory.** `gh release download
   v0.4.0 --pattern ogre-battle-64-recomp-macos-arm64.tar.gz` then
   `tar tzf` prints `ogre-battle-64-recomp/mods/` and
   `ogre-battle-64-recomp/mod_config/` as the only matches: both are empty
   directories.
3. **`make dist` tolerated it.** The `dist` recipe's copy step was
   `if ls build/mods/*.nrm >/dev/null 2>&1; then cp ...; else echo "no example
   mods in build/mods (run 'make example-mods' first)"; fi`. The `else` arm
   printed a line and the recipe exited 0, so the package was produced with an
   empty `mods/`.
4. **Nothing else checked.** `tools/smoke-dist.sh` verified the binary, the
   README and (on Windows) the DLLs, and never the contents of `mods/`.

The shipping rule the README states is `packaging/README-dist.txt` →
"MODS": a fresh copy carries the example mod(s), and the client's MODS tab
lists every `.nrm` in `mods/`.

## Fix

1. `Makefile` — new `dist-mods-check` target, a prerequisite of `dist`; it fails
   with the `make example-mods` instruction when `build/mods/*.nrm` is absent.
   The copy in the `dist` recipe is unconditional now, and the comment in the
   `example-mods` section says `dist` requires the `.nrm` and does not build it.
2. `tools/data-bundle.sh` — treats `build/mods/*.nrm` as a required input beside
   `RecompiledFuncs/`, and adds `build/mods/*.nrm` to the archive. A bundle made
   without them fails with the two commands to run.
3. `tools/release-build.sh` — checks `build/mods/*.nrm` before `make dist`, so a
   bundle that predates this rule fails before the app build rather than at the
   packaging step, with the `data-bundle.sh` instruction.
4. `.github/workflows/release.yml` — the "Unpack the generated game code" step
   fails when `files.tar.gz` carries no `.nrm` and prints the
   `make example-mods` / `tools/data-bundle.sh` instruction. The header comment
   records that the bundle carries the mods and why.
5. `tools/smoke-dist.sh` — fails a package whose `mods/` holds no `.nrm`, and
   prints the count on success. This is the check that fails a release built
   from an older bundle.
6. `packaging/README-dist.txt` — the MODS section said "one example mod" and
   named only `skip-boot-logos.nrm`; session 103 added `exp-overflow` and
   `make dist` copies every `.nrm`, so it now lists both.

The mods cannot be built on the CI runners: a hosted macOS or Windows image has
no `mips-linux-gnu-gcc`, and a macOS runner has no Linux container runtime for
`tools/build-example-mods.sh`'s Docker fallback. The bundle is the transport.

## Verification

- `bash -n` on `tools/data-bundle.sh`, `tools/release-build.sh` and
  `tools/smoke-dist.sh`; `make -n dist-mods-check` prints the check.
- Failure path: `mv build/mods build/mods.bak && make dist DIST_OS=macos` exits
  2 with `make dist: no example mods in build/mods/` and never reaches the app
  build. `build/mods` restored afterwards.
- Failure path: the same `mv`, then `tools/release-build.sh` exits 1 with the
  `make example-mods` / `data-bundle.sh` message. Restored.
- Happy path: `make dist DIST_OS=macos` copies `exp-overflow.nrm` and
  `skip-boot-logos.nrm` into `dist/ogre-battle-64-recomp/mods/` and exits 0.
- `tools/data-bundle.sh /tmp/ogre-bundle-check/files.tar.gz` reports
  `Example mods: 2 .nrm`, and `tar tzf` on the archive lists
  `build/mods/exp-overflow.nrm` and `build/mods/skip-boot-logos.nrm`.
- `tools/smoke-dist.sh` on the package prints `the package carries 2 example
  mod(s)` and PASS. On a copy with `mods/*.nrm` deleted it fails with
  `the package carries no mods/*.nrm (the README lists the example mods)`.
- `ruby -ryaml` parses `release.yml`; the jobs are `plan`, `build`, `release`.

## What is not fixed

The published v0.4.0 assets still have the empty `mods/` directory. The private
repository's `files.tar.gz` is the file the release runner reads, and it does
not carry the mods yet. After this change is committed:

```sh
make example-mods
tools/data-bundle.sh
cp dist/ogre-data/files.tar.gz /path/to/ogre-recomp-private/
```

then publish a new tag. `tools/data-bundle.sh` records the public commit in
`ogre-data.txt` and prints `public tree: modified` on a dirty tree, so commit
before bundling; the workflow warns when the recorded commit differs from the
release commit.

Not verified: a real hosted release run (a tag push needs the new bundle in the
private repository first).

## Probes

None. No recompiled file, no app source and no submodule changed, so there is
nothing to revert.

## Files changed

- `Makefile`
- `tools/data-bundle.sh`
- `tools/release-build.sh`
- `tools/smoke-dist.sh`
- `.github/workflows/release.yml`
- `packaging/README-dist.txt`
- `docs/guides/app-build.md`
- `docs/README.md`
- `PLAN.md`
- `docs/STATUS-LOG.md`
- `docs/DECISIONS.md`
- `docs/HANDOFF-2026-09-27-session108.md` (this file)

Local artifacts written but gitignored: `dist/ogre-battle-64-recomp/` (the
verified package), `/tmp/ogre-bundle-check/files.tar.gz` and
`/tmp/ogre-smoke-nomods/`.
