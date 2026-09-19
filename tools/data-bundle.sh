#!/usr/bin/env bash
# Bundle the generated game code for the private data repository.
#
# The release workflow cannot generate this code. It is the recompiler's output
# for a ROM that is not in this repository, it is gitignored (see .gitignore),
# and it is what `app/CMakeLists.txt` compiles into the app. A machine with the
# ROM produces `files.tar.gz` here, and the private repository named by the
# `OGRE_DATA_REPO` Actions variable hosts that archive. See
# docs/guides/app-build.md -> "Releases (GitHub Actions)".
#
#     make && make recomp && make bank-recomp && make rsp-recomp
#     tools/data-bundle.sh                        # -> dist/ogre-data/files.tar.gz
#     cp dist/ogre-data/files.tar.gz /path/to/<private-repo>/
#
# The archive holds the paths the app build consumes, at the archive root, plus
# `ogre-data.txt`, which records the public commit the code was generated from.
# `tools/data-bundle.sh [output]` writes somewhere else.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

out=${1:-dist/ogre-data/files.tar.gz}
case "$out" in
    /*) ;;
    *) out="$root/$out" ;;
esac

# `tools/release-build.sh` checks the first three of these before it builds;
# `Bank*Funcs/` is globbed by app/CMakeLists.txt and `app/src/bank_funcs.inc` is
# included by the app's overlay registration.
missing=""
[ -d RecompiledFuncs ]        || missing="$missing RecompiledFuncs/"
[ -d RspFuncs ]               || missing="$missing RspFuncs/"
[ -f app/src/bank_funcs.inc ] || missing="$missing app/src/bank_funcs.inc"
banks=$( (ls -d Bank*Funcs 2>/dev/null || true) | wc -l | tr -d ' ')
[ "$banks" -gt 0 ]            || missing="$missing Bank*Funcs/"
if [ -n "$missing" ]; then
    cat >&2 <<EOF
data-bundle.sh: the recompiled game code is missing:$missing

Generate it first, on a machine that has the ROM:

    make && make recomp && make bank-recomp && make rsp-recomp

See docs/guides/app-build.md. Nothing was written.
EOF
    exit 1
fi

# The workflow can only warn about a mismatch, so record enough to see one.
meta=$root/ogre-data.txt
trap 'rm -f "$meta"' EXIT
# Submodule dirt is normal here (tools/RT64 carries the project's patch set) and
# does not affect the generated code, so it does not count. Ignored files
# (dist/, build*/) do not count either.
if [ -n "$(git status --porcelain --ignore-submodules=all)" ]; then
    tree=modified
else
    tree=clean
fi
{
    printf 'public commit: %s\n' "$(git rev-parse HEAD)"
    printf 'public tree: %s\n' "$tree"
    printf 'generated: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$meta"

mkdir -p "$(dirname -- "$out")"
tar czf "$out" ogre-data.txt RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc

# `shasum` is on macOS, `sha256sum` on Linux; one of them exists.
sha=$( (shasum -a 256 "$out" 2>/dev/null || sha256sum "$out") | awk '{print $1}')

echo "==> $out"
ls -lh "$out" | awk '{print "    " $5}'
echo "    entries:         $(tar tzf "$out" | wc -l | tr -d ' ')"
echo "    RecompiledFuncs: $(find RecompiledFuncs -type f | wc -l | tr -d ' ') files"
echo "    Bank units:      $banks"
echo "    RspFuncs:        $(find RspFuncs -type f | wc -l | tr -d ' ') files"
echo "    sha256: $sha"
cat "$meta" | sed 's/^/    /'
echo
echo "Copy it into the private repository and commit it there:"
echo "    cp \"$out\" /path/to/<private-repo>/files.tar.gz"
