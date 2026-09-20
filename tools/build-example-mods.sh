#!/usr/bin/env bash
# Build the example mods in mods/ into build/mods/<name>.nrm.
#
# A mod's code is compiled to big-endian MIPS and then packaged by RecompModTool,
# which recompiles that ELF against the base game's symbols and writes the `.nrm`
# the client loads. Both steps need the reference symbols that `make mod-syms`
# writes to mods/reference/.
#
#   tools/build-example-mods.sh              # every mod under mods/
#   tools/build-example-mods.sh mods/foo     # one of them
#
# A MIPS cross compiler is required. Apple's clang has no MIPS target, so when
# `mips-linux-gnu-gcc` is not on PATH this runs `gcc-mips-linux-gnu` inside a
# debian:bookworm-slim container.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

recomp_tool=tools/N64Recomp/build/RecompModTool
if [ ! -x "$recomp_tool" ]; then
    echo "build-example-mods.sh: $recomp_tool is missing; build N64Recomp first" >&2
    exit 1
fi
if [ ! -f mods/reference/dump.toml ]; then
    echo "build-example-mods.sh: mods/reference/dump.toml is missing." >&2
    echo "  Run 'make mod-syms' first (it needs the linked build/ogrebattle64.elf)." >&2
    exit 1
fi

mods=${1:-}
if [ -z "$mods" ]; then
    mods=$(find mods -mindepth 1 -maxdepth 1 -type d ! -name reference | sort)
fi

# The same flags the upstream mod template uses: MIPS II, no abicalls (the
# runtime relocates the sections), no builtin calls (nothing provides memcpy),
# no PIC (Debian's gcc defaults to PIE, which needs abicalls on MIPS).
MIPS_CFLAGS="-mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg \
-fno-builtin -ffreestanding -nostdinc -fno-pic -fno-pie \
-Wall -Wextra -Wno-unused-parameter"

use_docker=0
if ! command -v mips-linux-gnu-gcc >/dev/null 2>&1; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "build-example-mods.sh: no mips-linux-gnu-gcc on PATH and no docker." >&2
        echo "  mips-linux-gnu-binutils is not enough; a cross gcc is needed." >&2
        exit 1
    fi
    use_docker=1
    echo "build-example-mods.sh: using docker (gcc-mips-linux-gnu)"
fi

# Runs one command in the mod's directory, either directly or in the container.
# Runs one shell command in the mod's directory, either directly or in the
# container. Docker runs as root (apt needs it); Docker Desktop maps the files it
# writes back to the invoking user, so the output is not left root-owned.
run_in_mod() {
    local mod=$1
    shift
    if [ "$use_docker" = 1 ]; then
        docker run --rm -v "$root":/w -w "/w/$mod" debian:bookworm-slim \
            bash -lc 'apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq gcc-mips-linux-gnu >/dev/null 2>&1 && '"$*"
    else
        (cd "$mod" && eval "$*")
    fi
}

built=0
for mod in $mods; do
    if [ ! -f "$mod/mod.toml" ]; then
        continue
    fi
    if ! ls "$mod"/src/*.c >/dev/null 2>&1; then
        echo "build-example-mods.sh: $mod has no src/*.c" >&2
        exit 1
    fi

    echo "== $mod =="
    mkdir -p "$mod/build"

    # One command for the whole mod: the container installs the toolchain once,
    # compiles every source and links.
    command=""
    objs=""
    for source in "$mod"/src/*.c; do
        base=$(basename "${source%.c}")
        objs="$objs build/$base.o"
        command="$command mips-linux-gnu-gcc $MIPS_CFLAGS -I include -c src/$base.c -o build/$base.o &&"
    done
    command="$command mips-linux-gnu-ld -nostdlib -T mod.ld --unresolved-symbols=ignore-all --emit-relocs -e 0 -o build/mod.elf$objs"
    run_in_mod "$mod" "$command"

    mkdir -p build/mods
    "$recomp_tool" "$mod/mod.toml" build/mods
    built=$((built + 1))
done

if [ "$built" = 0 ]; then
    echo "build-example-mods.sh: no mod.toml found under mods/" >&2
    exit 1
fi

echo
ls -l build/mods/*.nrm
