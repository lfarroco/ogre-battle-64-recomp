#!/usr/bin/env bash
# Prepare and start a GitHub release of this port.
#
#   make release VERSION=v0.6.0
#
# The release workflow cannot generate the recompiled game code: it needs the
# ROM. A GitHub-hosted runner instead unpacks `files.tar.gz` from the private
# data repository, and the unpack step fails when that bundle records a public
# commit other than the one being built. That failure is what this script
# prevents. It:
#
#   1. regenerates the generated code (make recomp, bank-recomp, rsp-recomp)
#      and the example mods,
#   2. packs `files.tar.gz` with tools/data-bundle.sh and proves that the code
#      in it is byte for byte the code in the tree,
#   3. commits and pushes the bundle to the private data repository, and
#   4. dispatches the Release workflow, which builds every platform and creates
#      the draft GitHub release.
#
# The public checkout must be clean (ignored files and submodules do not count)
# and pushed, because the bundle records `git rev-parse HEAD` and the workflow
# compares that with the commit it builds.
#
# Environment:
#   VERSION  / $1        the release tag, `vX.Y.Z` (default: the newest tag with
#                        its numeric last field bumped)
#   DATA_REPO            private repository slug (default: the Actions variable
#                        OGRE_DATA_REPO, see `gh variable list`)
#   REGEN=0              skip the regeneration in step 1
#   SKIP_BUNDLE=1        reuse dist/ogre-data/files.tar.gz (a retry after the
#                        workflow failed; the drift check still runs)
#   NO_PUSH=1            stop after packing; do not push or dispatch
#   NO_WAIT=1            do not wait for the workflow run
#   DRAFT=false          create a published release instead of a draft
#   REPO                 public repository slug (default: the `origin` remote)
#
# `make release-check VERSION=v0.6.0` is the same script with NO_PUSH=1: it
# regenerates, packs and reports, and changes nothing outside this tree.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

warn() { printf '\033[33m==> %s\033[0m\n' "$*"; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[31mrelease: %s\033[0m\n' "$*" >&2; exit 1; }

# The commit a bundle records, from its own ogre-data.txt.
bundle_commit() {
    tar xzOf "${1:-dist/ogre-data/files.tar.gz}" ogre-data.txt 2>/dev/null \
        | sed -n 's/^public commit: //p' || true
}

version=${VERSION:-${1:-}}
regen=${REGEN:-1}
skip_bundle=${SKIP_BUNDLE:-0}
no_push=${NO_PUSH:-0}
no_wait=${NO_WAIT:-0}
draft=${DRAFT:-true}
bundle_out=$root/dist/ogre-data/files.tar.gz

# --- the version ------------------------------------------------------------
# The newest tag with the numeric part of its last field bumped, so the common
# case needs no argument. `--sort=-v:refname` orders v0.10.0 above v0.9.0.
if [ -z "$version" ]; then
    latest=$(git tag -l 'v*' --sort=-v:refname | head -1)
    [ -n "$latest" ] || die "no v* tag to derive a version from; pass VERSION=vX.Y.Z"
    version=$(printf '%s' "$latest" | awk -F. -v OFS=. '{ $NF = $NF + 1; print }')
    warn "no VERSION given; $latest -> $version"
fi
case "$version" in
    v[0-9]*.[0-9]*.[0-9]*) ;;
    *) die "VERSION must look like vX.Y.Z, got '$version'" ;;
esac

# --- where the public and private repositories are --------------------------
repo=${REPO:-$(git remote get-url origin)}
repo=${repo#git@github.com:}
repo=${repo#https://github.com/}
repo=${repo%.git}
data_repo=${DATA_REPO:-$(gh variable list --repo "$repo" --json name,value \
    --jq '.[] | select(.name == "OGRE_DATA_REPO") | .value' 2>/dev/null || true)}
[ -n "$data_repo" ] || die "no private data repository: set DATA_REPO=OWNER/REPO or the
       Actions variable OGRE_DATA_REPO (gh variable list --repo $repo)"
echo "    public:  $repo"
echo "    private: $data_repo"
echo "    version: $version (draft: $draft)"

git rev-parse -q --verify "refs/tags/$version" >/dev/null \
    && die "tag $version already exists here"
gh release view "$version" --repo "$repo" >/dev/null 2>&1 \
    && die "a release for $version already exists in $repo"

# --- the tree the bundle will describe --------------------------------------
# tools/data-bundle.sh records `git rev-parse HEAD`, and the workflow fails the
# job when that differs from the commit it builds, so a release is only
# meaningful from a clean, pushed commit. Generated code, build output and
# submodule dirt are gitignored or expected and do not count. An untracked file
# does not change an existing file's contents, so it is reported and not
# refused: this script itself may be the untracked file the first time it runs.
commit=$(git rev-parse HEAD)
branch=$(git rev-parse --abbrev-ref HEAD)
tracked_dirty() {
    git status --porcelain --ignore-submodules=all \
        | grep -v -e '^??' || true
}
untracked() {
    git status --porcelain --ignore-submodules=all | grep -e '^??' || true
}
if [ -n "$(tracked_dirty)" ]; then
    tracked_dirty >&2
    die "the tracked tree is modified. Commit it first: the bundle would record
       HEAD while carrying code generated from these uncommitted changes."
fi
if [ -n "$(untracked)" ]; then
    warn "untracked files are present; the bundle does not carry them:"
    untracked | sed 's/^/    /'
fi
if git rev-parse -q --verify "refs/remotes/origin/$branch" >/dev/null; then
    if [ "$(git rev-parse "origin/$branch")" != "$commit" ]; then
        die "HEAD ($commit) is not pushed to origin/$branch. Push it first: the
       workflow builds the branch tip, and the bundle records this commit."
    fi
fi
echo "    commit:  $commit ($branch)"

# --- the private data repository --------------------------------------------
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ogre-release.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
if [ "$no_push" = 1 ]; then
    echo "    NO_PUSH=1: the bundle will not be pushed and the workflow will not run"
else
    step "[1/4] cloning $data_repo"
    gh repo clone "$data_repo" "$tmp/data" -- --quiet
    git -C "$tmp/data" log --oneline -1
fi

# --- regenerate and pack ----------------------------------------------------
# SKIP_BUNDLE=1 reuses the bundle already in dist/ogre-data. It is for a retry
# after the *workflow* failed: the bundle is committed, so regenerating it costs
# minutes and cannot change the outcome. The drift check below still runs, so a
# bundle that is not this commit's code is still refused.
if [ "$skip_bundle" = 1 ]; then
    [ -f "$bundle_out" ] || die "SKIP_BUNDLE=1 but $bundle_out does not exist"
    step "[2/4] SKIP_BUNDLE=1: reusing dist/ogre-data/files.tar.gz"
elif [ "$regen" = 1 ]; then
    step "[2/4] regenerating the game code and the example mods"
    make bank-recomp
    make recomp
    make rsp-recomp
    make example-mods
    if [ -n "$(tracked_dirty)" ]; then
        tracked_dirty >&2
        die "the regeneration changed tracked files (above). Commit them, then
       re-run: the bundle records the commit, so it must describe the tree."
    fi
else
    warn "REGEN=0: using the generated code already in this tree"
fi

if [ "$skip_bundle" != 1 ]; then
    step "[3/4] packing the data bundle"
    # Report the drift before packing, because this is where the release gains
    # the work committed after the previous bundle: the bundle records the commit
    # it was generated from, and only the commit check makes the workflow refuse
    # one that no longer describes the tree. A bundle is correct for its own
    # commit, so a stale one is not detectable by comparison alone.
    if [ -f "$bundle_out" ]; then
        previous=$(bundle_commit || true)
        if [ -n "$previous" ] && [ "$previous" != "$commit" ]; then
            if git cat-file -e "$previous^{commit}" 2>/dev/null; then
                warn "the existing bundle was generated at ${previous:0:7}, $(git rev-list --count "$previous..$commit") commit(s) behind $commit, and is replaced"
            else
                warn "the existing bundle was generated at ${previous:0:7}, which is not in this checkout, and is replaced"
            fi
        fi
    fi
    tools/data-bundle.sh
fi

# The bundle carries code generated from the ROM at whatever state this tree
# was in when it was last regenerated. Nothing in the bundle records the inputs
# that code came from, and the workflow can only compare commits, so compare the
# packed code with the tree here: a bundle holding code generated before a change
# to the ROM-derived inputs fails at this point instead of shipping.
#
# The *recorded commit* is a weaker question, and this is the only place that can
# answer it correctly. The generated code is a function of the ROM and
# config/, so a commit that changes only tooling or documentation leaves a bundle
# byte-identical, and the workflow has no ROM to check that and refuses on the
# commit alone. When the code matches, record the commit being released.
echo "==> checking that the bundle is this tree's generated code"
tar xzf "$bundle_out" -C "$tmp" ogre-data.txt RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc
recorded=$(sed -n 's/^public commit: //p' "$tmp/ogre-data.txt")
drift=$(diff -rq "$tmp/RecompiledFuncs" RecompiledFuncs; \
        diff -rq "$tmp/RspFuncs" RspFuncs; \
        diff -q "$tmp/app/src/bank_funcs.inc" app/src/bank_funcs.inc; \
        for d in Bank*Funcs; do diff -rq "$tmp/$d" "$d"; done; true)
if [ -n "$drift" ]; then
    printf '%s\n' "$drift" >&2
    die "the bundle holds code this tree does not have (above), because it was
       packed before the current config/ or the ROM. Re-run with the
       regeneration: REGEN=1, without SKIP_BUNDLE=1."
fi
if [ "$recorded" != "$commit" ]; then
    warn "the code is identical, but the bundle records ${recorded:0:7}; repacking with $commit"
    manifest=$root/ogre-data.txt
    trap 'rm -rf "$tmp"; rm -f "$manifest"' EXIT
    {
        printf 'public commit: %s\n' "$commit"
        printf 'public tree: clean\n'
        printf 'generated: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$manifest"
    COPYFILE_DISABLE=1 tar czf "$bundle_out" \
        --exclude '._*' --exclude '*/._*' --exclude '.DS_Store' --exclude '*/.DS_Store' \
        ogre-data.txt RecompiledFuncs Bank*Funcs RspFuncs app/src/bank_funcs.inc build/mods/*.nrm
    rm -f "$manifest"
fi
echo "    identical: RecompiledFuncs/ Bank*Funcs/ RspFuncs/ app/src/bank_funcs.inc"
echo "    records:   $(bundle_commit) (the commit this release builds)"

if [ "$no_push" = 1 ]; then
    step "[4/4] NO_PUSH=1: stopping here"
    echo "    bundle:       $bundle_out"
    echo "    would commit: generated code at ${commit:0:7}, with the example mods"
    echo "    would push:   $data_repo  ($branch)"
    echo "    would run:    Release workflow, tag $version, draft=$draft"
    exit 0
fi

# --- commit and push the bundle ---------------------------------------------
step "[4/4] committing the bundle to $data_repo"
cp "$bundle_out" "$tmp/data/files.tar.gz"
# The README carried "The workflow warns when a tagged release builds a commit
# other than the one ogre-data.txt records", which the unpack step has failed
# on since session 111. Correct a copy that still says it. A README that does
# not match is left alone, so this is a no-op once it is fixed.
if [ -f "$tmp/data/README.md" ] \
   && grep -q '^The workflow warns when a tagged release' "$tmp/data/README.md"; then
    sed -i.bak 's/^The workflow warns when a tagged release.*/The workflow fails when a tagged release builds a commit other than the one/' \
        "$tmp/data/README.md"
    sed -i.bak 's/^`ogre-data.txt` records\.$/`ogre-data.txt` records, and prints how many commits newer the build is./' \
        "$tmp/data/README.md"
    rm -f "$tmp/data/README.md.bak"
    echo "    corrected the README's claim that the workflow warns"
fi
git -C "$tmp/data" add files.tar.gz README.md
if git -C "$tmp/data" diff --cached --quiet; then
    echo "    the private repository already holds this bundle; nothing to commit"
else
    git -C "$tmp/data" commit -q -m "generated code at ${commit:0:7}, with the example mods"
    git -C "$tmp/data" log --oneline -1
fi
# Always push the branch tip: a commit made but not pushed leaves the workflow
# unpacking the previous bundle, which is the failure this script exists to
# prevent.
git -C "$tmp/data" push -q origin HEAD
echo "    pushed $data_repo $(git -C "$tmp/data" rev-parse --short HEAD)"

# --- start the release ------------------------------------------------------
# workflow_dispatch rather than a tag push: the workflow only reads its `draft`
# input on a dispatch. `gh release create` in the workflow's Release job
# creates the tag at the built commit, which is the commit this bundle records.
step "dispatching the Release workflow for $version"
gh workflow run Release --repo "$repo" --ref "$branch" \
    -f "tag=$version" -f "draft=$draft" -f "platforms=all"

run_id=""
for _ in $(seq 1 30); do
    sleep 2
    run_id=$(gh run list --repo "$repo" --workflow Release --event workflow_dispatch \
        --limit 5 --json databaseId,headSha,createdAt \
        --jq "[.[] | select(.headSha == \"$commit\")] | .[0].databaseId // empty")
    [ -n "$run_id" ] && break
done
[ -n "$run_id" ] || die "the workflow did not appear in 'gh run list'; check $repo/actions"
echo "    run $run_id: https://github.com/$repo/actions/runs/$run_id"

if [ "$no_wait" = 1 ]; then
    echo
    echo "NO_WAIT=1: not waiting for the run. Watch it with:"
    echo "    gh run watch $run_id --repo $repo"
    exit 0
fi

step "waiting for the run (three platform builds, about 30 minutes)"
while true; do
    read -r status conclusion < <(gh run view "$run_id" --repo "$repo" \
        --json status,conclusion --jq '"\(.status) \(.conclusion // "")"')
    case "$status" in
        completed) break ;;
    esac
    printf '    %s %s\n' "$(date -u +%H:%M:%S)" "$status"
    sleep 60
done

if [ "$conclusion" != "success" ]; then
    echo
    gh run view "$run_id" --repo "$repo" --log-failed 2>/dev/null | tail -40 || true
    die "the Release run failed ($conclusion): https://github.com/$repo/actions/runs/$run_id"
fi

step "run $run_id succeeded"
if [ "$draft" = true ]; then
    echo "Draft release $version: https://github.com/$repo/releases"
    echo "Review the assets, then publish it:"
    echo "    gh release edit $version --repo $repo --draft=false"
else
    echo "Release $version: https://github.com/$repo/releases/tag/$version"
fi
