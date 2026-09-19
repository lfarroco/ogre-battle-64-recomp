#!/usr/bin/env bash
# Build a distributable package for this machine's platform (or DIST_OS).
#
# This is the one path CI and a developer both use, so the release artifacts are
# exactly what `make dist` produces locally.
#
#   tools/release-build.sh                    # host platform
#   DIST_OS=linux tools/release-build.sh      # explicit (CI passes this)
#   DIST_STATIC_SDL=0 tools/release-build.sh  # bundle the shared SDL2 instead
#
# Output: dist/ogre-battle-64-recomp/ plus dist/ogre-battle-64-recomp-<os>.tar.gz
# and/or .zip.
#
# It refuses to run when the generated game code is missing, because on a fresh
# clone it always is: RecompiledFuncs/, Bank*Funcs/, RspFuncs/ and
# app/src/bank_funcs.inc are gitignored and are produced by a recompilation that
# needs a ROM. See docs/guides/app-build.md.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# --- what platform are we packaging? ----------------------------------------
if [ -z "${DIST_OS:-}" ]; then
    case "$(uname -s)" in
        Darwin) DIST_OS=macos ;;
        Linux)  DIST_OS=linux ;;
        MINGW*|MSYS*|CYGWIN*|Windows_NT) DIST_OS=windows ;;
        *) echo "release-build.sh: unknown platform $(uname -s); set DIST_OS" >&2; exit 2 ;;
    esac
fi
export DIST_OS

# --- the generated game code must be present --------------------------------
missing=""
[ -d RecompiledFuncs ] || missing="$missing RecompiledFuncs/"
[ -f app/src/bank_funcs.inc ] || missing="$missing app/src/bank_funcs.inc"
[ -d RspFuncs ] || missing="$missing RspFuncs/"
if [ -n "$missing" ]; then
    cat >&2 <<EOF
release-build.sh: the recompiled game code is missing:$missing

It is not in the repository (it is generated from the ROM and gitignored), so
this script cannot build without it. On a machine that has your ROM:

    tools/venv/bin/splat split config.yaml     # once, if asm/ assets/ are absent
    make && make recomp && make bank-recomp

Then run this script again. See docs/guides/app-build.md.
EOF
    exit 1
fi

# --- third-party trees ------------------------------------------------------
if [ ! -d tools/N64ModernRuntime ]; then
    echo "release-build.sh: tools/N64ModernRuntime is missing (git submodule / clone step)" >&2
    exit 1
fi
if [ ! -d tools/RT64/src ]; then
    echo "release-build.sh: tools/RT64 is missing (git submodule / clone step)" >&2
    exit 1
fi

# The RT64 SDL compatibility patch is needed on systems with SDL < 2.0.22 and is
# harmless elsewhere. Applying it twice fails, hence the `|| true`.
if [ -f rt64-plume-sdl.patch ] && [ -d tools/RT64/src/contrib/plume ]; then
    git -C tools/RT64/src/contrib/plume apply ../../../../rt64-plume-sdl.patch >/dev/null 2>&1 || true
fi

# --- build ------------------------------------------------------------------
echo "==> packaging for $DIST_OS (static SDL2: ${DIST_STATIC_SDL:-1})"
make dist "DIST_OS=$DIST_OS"

# --- archive ----------------------------------------------------------------
# zip for Windows (what a player expects), tar.gz everywhere; make whichever
# tools exist, and fail only if neither worked.
made=0
if command -v tar >/dev/null 2>&1; then
    make dist-tar "DIST_OS=$DIST_OS" >/dev/null && made=1
fi
if command -v zip >/dev/null 2>&1; then
    make dist-zip "DIST_OS=$DIST_OS" >/dev/null && made=1
fi
if [ "$made" = 0 ]; then
    echo "release-build.sh: neither tar nor zip is available to create an archive" >&2
    exit 1
fi

# `make dist-zip`/`dist-tar` each re-run `dist`; the payload is identical, so
# the result is just the folder plus both archives.
echo "==> done:"
ls -lh dist/
