#!/bin/sh
# Run the port with one of the saves in assets/saves as the cartridge battery (SRAM).
#
# OB64's save is the 32 KiB battery-backed SRAM at
#   <config>/saves/ogrebattle64-us-rev1.bin
# and the game reads/writes it through its own PI DMA path. This script puts a
# chosen save there and runs the game, so the title comes up with the save's
# progress already on the battery -- the title's Load Game enters the map
# instead of New Game.
#
#   tools/run-save.sh prologue                 # assets/saves/prologue.n64
#   tools/run-save.sh assets/saves/foo.n64
#   tools/run-save.sh prologue --reset         # re-import, discarding play progress
#   tools/run-save.sh --list                   # what is in assets/saves
#
# Each save gets its own config dir, `.ogre-prefs-save-<name>/` (gitignored, like
# the other `.ogre-prefs-*` sandboxes), so progress the game writes back stays
# with that save and a second run resumes from it. `--reset` re-imports the
# original file.
#
# The save may be a DexDrive `.N64` Controller Pak dump (what assets/saves holds:
# the game's copy/backup notes carry a verbatim copy of the battery slot),
# a bare 32 KiB SRAM image, or any emulator wrapping `tools/sramsave.py` can
# find -- the tool does the conversion.
#
# All OGRE_* knobs pass through, so a scripted, bounded run works as usual:
#   OGRE_TAP_MS=1500 OGRE_TAP_SCENE_BUTTON="title:start:5,0x12:a:3" \
#   OGRE_EXIT_AFTER_MS=60000 tools/run-save.sh prologue
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app="$root/build-app/ogrebattle64"
default_rom="$root/assets/ogre64.z64"
saves_dir="$root/assets/saves"

usage() {
    sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

reset=0
import_only=0
save=""

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --list)
            echo "saves in $saves_dir:"
            for f in "$saves_dir"/*; do
                [ -f "$f" ] && echo "  $(basename "$f")"
            done
            exit 0 ;;
        --reset) reset=1; shift ;;
        --import-only) import_only=1; shift ;;
        --) shift; break ;;
        -*) echo "run-save.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) if [ -z "$save" ]; then save=$1; shift; else break; fi ;;
    esac
done
if [ -z "$save" ]; then
    usage >&2
    exit 2
fi

# Resolve the save: an explicit path, or a name under assets/saves (with or
# without its extension).
src=""
for candidate in "$save" "$saves_dir/$save" "$saves_dir/$save.n64" "$saves_dir/$save.bin"; do
    if [ -f "$candidate" ]; then src=$candidate; break; fi
done
if [ -z "$src" ]; then
    echo "run-save.sh: no such save '$save' (try --list)" >&2
    exit 1
fi

name=$(basename "$src")
name=${name%.*}
name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '-')

pref="$root/.ogre-prefs-save-$name"
dst="$pref/saves/ogrebattle64-us-rev1.bin"
mkdir -p "$pref/saves"

if [ "$reset" = 1 ] || [ ! -f "$dst" ]; then
    python3 "$root/tools/sramsave.py" import "$src" "$dst"
else
    echo "keeping the battery state already at $dst"
    echo "  (--reset re-imports $src and discards progress saved since)"
fi
echo "battery: $dst"
if [ "$import_only" = 1 ]; then
    exit 0
fi

if [ ! -x "$app" ]; then
    echo "run-save.sh: $app is missing; build it with:" >&2
    echo "  cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release && cmake --build build-app -j" >&2
    exit 1
fi

# With no explicit app arguments, boot the usual ROM.
if [ $# -eq 0 ] && [ -f "$default_rom" ]; then
    set -- "$default_rom"
fi

exec env "OGRE_PREF_DIR=$pref" "$app" "$@"
