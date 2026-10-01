#!/usr/bin/env python3
"""Fail when a tracked patch no longer matches the tree it is supposed to describe.

The third-party trees (`tools/N64ModernRuntime`, its nested `N64Recomp`,
`tools/N64Recomp`, `tools/RT64`, and RT64's nested `plume`) are gitignored
checkouts of a pinned upstream commit plus the project's local changes. The
*tracked* copy of those changes is the patch file in `patches/`. A developer
builds from the tree; the hosted release builds from the patch files, applied to
a pristine checkout of the same commit.

When the two drift, a fix that exists in the tree is not in the shipped binary,
and nothing fails: the build is green and the release is missing the change.
That happened and shipped:

* `patches/rt64-ob64.patch` still read `getenv("OGRE_CAPTURE_PRESENT")` while
  the tree called `ogre_present_capture_path()`, so no hosted build contained
  session 107's capture fix and `snap`'s frame capture was a no-op in v0.5.0
  and v0.5.1;
* `patches/rt64-plume-ob64.patch` had no `plume_d3d12.cpp` hunk at all, so the
  D3D12 null-texture guard from the same session was absent from every release.

The content check is exact and needs no worktree: for each file a patch touches,
the repository's own `git diff HEAD -- <file>` must equal that patch's block for
the file, `index` lines ignored. When that holds, applying the patch to a
pristine checkout of `HEAD` reproduces the tree for that file byte for byte. A
second pass requires every non-gitlink file the tree changes to be covered by
some patch, which catches a tree change that no patch carries at all.

Two patch files can target one tree with disjoint file sets, and the hosted path
applies all of them. A path that only the other patch carries is therefore not a
missing hunk: the check compares each tree's changes against the union of every
patch that targets it. `tools/rt64-plume-sdl.patch` is the case: it edits
`plume_vulkan.cpp`, which the tracked `patches/rt64-plume-ob64.patch` does not
carry, is applied only on the hosted path, and has no post-image in the tree.

Usage:
    tools/patchcheck.py              # check everything, exit 1 on drift
    tools/patchcheck.py --verbose    # also list every covered file
    tools/patchcheck.py --fix        # rewrite the stale patches from the trees
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (repository directory relative to the repo root, patch file).
TARGETS = [
    ("tools/N64ModernRuntime", "patches/n64modernruntime-ob64.patch"),
    ("tools/N64ModernRuntime/N64Recomp", "patches/n64modernruntime-n64recomp.patch"),
    ("tools/N64Recomp", "patches/n64recomp-ob64.patch"),
    ("tools/RT64", "patches/rt64-ob64.patch"),
    ("tools/RT64/src/contrib/plume", "patches/rt64-plume-ob64.patch"),
]


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(repo), *args],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git -C {repo} {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def normalize(diff: str) -> list[str]:
    """Drop `index` lines: their hashes differ when the patch was written against
    the same content from a different object database."""
    return [line for line in diff.splitlines() if not line.startswith("index ")]


def split_patch(text: str) -> dict[str, str]:
    """Map path -> the patch's block for that path, in `diff --git` order."""
    blocks: dict[str, str] = {}
    current: str | None = None
    buffer: list[str] = []
    for line in text.splitlines(keepends=True):
        if line.startswith("diff --git "):
            if current is not None:
                blocks[current] = "".join(buffer)
            current = line.split()[-1][2:]
            buffer = [line]
        elif current is not None:
            buffer.append(line)
    if current is not None:
        blocks[current] = "".join(buffer)
    return blocks


def gitlinks(repo: Path) -> set[str]:
    """Paths recorded as submodules; their 'diff' is a commit pointer that no
    patch file carries."""
    links: set[str] = set()
    for line in git(repo, "ls-files", "--stage").splitlines():
        if line.startswith("160000 "):
            links.add(line.split("\t", 1)[1])
    return links


def tree_diff(repo: Path, verbose: bool = False) -> str:
    links = gitlinks(repo)
    args = ["diff", "HEAD", "--no-color"]
    if links:
        args.append("--")
        args += [f":(exclude){path}" for path in sorted(links)]
    if verbose and links:
        print(f"      (skipping gitlink(s): {', '.join(sorted(links))})")
    return git(repo, *args)


def untracked_diff(repo: Path) -> str:
    """Diff blocks for untracked, non-ignored files, as new files.

    A patch that creates a file leaves that file untracked when it is applied to
    a pristine checkout, and `git diff HEAD` does not show untracked files. The
    hosted runner applies the patches and then runs this check, so without this
    every patch that adds a file reads as "the tree does not change it" there
    while passing on a developer's tree, where the file was committed or staged.

    `git diff <empty tree>` does not report an untracked path, so each file is
    diffed against /dev/null with `--no-index`, which produces the same
    "new file mode" block the patch carries.
    """
    listed = git(repo, "ls-files", "--others", "--exclude-standard")
    blocks: list[str] = []
    for rel in listed.splitlines():
        if not rel or not (repo / rel).is_file():
            continue
        result = subprocess.run(
            ["git", "-C", str(repo), "--no-pager", "diff", "--no-index",
             "--no-color", "--src-prefix=a/", "--dst-prefix=b/",
             os.devnull, rel],
            capture_output=True, text=True)
        # `--no-index` exits 1 when the two inputs differ, the expected case.
        if result.returncode not in (0, 1):
            raise RuntimeError(f"git diff --no-index {rel}: {result.stderr.strip()}")
        blocks.append(result.stdout)
    return "".join(blocks)


def tree_changes(repo: Path, verbose: bool = False) -> str:
    return tree_diff(repo, verbose) + untracked_diff(repo)


def check_one(repo_rel: str, patched: dict[str, str], covered: set[str],
              blocks: dict[str, str], verbose: bool) -> list[str]:
    """Compare one patch's blocks with the tree's diff.

    `covered` is every path any patch targeting this tree carries. A path the
    tree changes and only *another* patch carries is not a problem: the hosted
    path applies that patch too.
    """
    problems: list[str] = []
    for path, block in patched.items():
        if path not in blocks:
            problems.append(f"{path}: in the patch, and the tree does not change it")
        elif normalize(block) != normalize(blocks[path]):
            problems.append(f"{path}: the tree's diff differs from the patch")
        elif verbose:
            print(f"      ok       {path}")
    for path in blocks:
        if path not in covered:
            problems.append(f"{path}: changed in the tree and in no patch, so it ships nowhere")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true",
                    help="list every file the check compared")
    ap.add_argument("--fix", action="store_true",
                    help="rewrite each stale patch from its tree (git diff HEAD)")
    args = ap.parse_args(argv)

    # Group by tree first: the coverage question ("is this tree change in some
    # patch?") is about the tree, while the content question is per patch.
    per_tree: dict[str, list[tuple[str, Path]]] = {}
    failed = False
    for repo_rel, patch_rel in TARGETS:
        repo = ROOT / repo_rel
        patch_path = ROOT / patch_rel
        if not patch_path.is_file() or not (repo / ".git").exists():
            print(f"skip  {patch_rel}: {repo_rel} is not checked out")
            continue
        per_tree.setdefault(repo_rel, []).append((patch_rel, patch_path))

    for repo_rel, patches in per_tree.items():
        repo = ROOT / repo_rel
        blocks = split_patch(tree_changes(repo, args.verbose))
        covered: set[str] = set()
        texts = {patch_rel: patch_path.read_text() for patch_rel, patch_path in patches}
        for text in texts.values():
            covered |= set(split_patch(text))

        for patch_rel, patch_path in patches:
            problems = check_one(repo_rel, split_patch(texts[patch_rel]), covered,
                                 blocks, args.verbose)
            if not problems:
                print(f"ok    {patch_rel} describes {repo_rel}")
                continue
            failed = True
            print(f"FAIL  {patch_rel} does not describe {repo_rel}")
            for problem in problems:
                print(f"        {problem}")
            if args.fix:
                if len(patches) > 1:
                    # One tree, several patches: writing the whole tree diff
                    # into one of them would duplicate the other's hunks.
                    print("        more than one patch targets this tree; not rewriting")
                else:
                    regenerated = tree_changes(repo)
                    patch_path.write_text(regenerated)
                    print(f"        rewritten from {repo_rel}: "
                          f"{len(regenerated.splitlines())} lines, "
                          f"{len(split_patch(regenerated))} file(s)")

    if failed:
        print("\nThe hosted release builds from these patch files, applied to a pristine\n"
              "checkout, so a stale patch ships old code. Regenerate with\n"
              "`tools/patchcheck.py --fix`, review the diff and commit the patch.\n",
              file=sys.stderr)
        return 1
    print("\nPASS: every patch describes its tree, and every tree change is in a patch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
