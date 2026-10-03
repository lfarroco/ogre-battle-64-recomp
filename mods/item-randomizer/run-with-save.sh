#!/bin/sh
# Run the port with the Item Randomizer installed and one battery save loaded.
#
# This wraps `tools/run-save.sh`: that tool imports a save into its own config
# dir (`.ogre-prefs-save-<name>/`) and runs the app, but it knows nothing about
# mods, so this script also copies the built mod, `mods.json` and the mod's
# option file into the same config dir. `OGRE_MOD_TEST=1` is set so the start
# screen does not appear; without it the launcher waits for a ROM row press and
# the run never reaches the game.
#
#   mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin
#   mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin --reset
#
# `--reset` re-imports the save file and discards the progress the game wrote
# back. The port writes the battery on play and on exit, so a second run without
# `--reset` resumes from wherever the first one stopped. That is the difference
# between resuming the mission at scene 0x03 and coming up on the world map at
# scene 0x05.
#
# Every OGRE_* knob passes through:
#   OGRE_EXIT_AFTER_MS=120000 OGRE_LIVE_CONSOLE=1 \
#   mods/item-randomizer/run-with-save.sh build-app/saves/before-pickup.bin
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"

save=""
reset=""
rom=""
for arg in "$@"; do
    case "$arg" in
        --reset) reset="--reset" ;;
        -*) ;;
        *)
            if [ -z "$save" ]; then
                save=$arg
            else
                rom=$arg
            fi
            ;;
    esac
done
if [ -z "$save" ]; then
    echo "usage: $0 <save file> [rom] [--reset]" >&2
    exit 2
fi
[ -n "$rom" ] || rom="$root/assets/ogre64.z64"

if [ ! -f build/mods/item-randomizer.nrm ]; then
    echo "$0: build/mods/item-randomizer.nrm is missing; run 'make example-mods'" >&2
    exit 1
fi

# The name run-save.sh derives, so both tools agree on the config dir.
name=$(basename "$save")
name=${name%.*}
name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '-')
pref="$root/.ogre-prefs-save-$name"

if [ -n "$reset" ]; then
    tools/run-save.sh "$save" --reset --import-only
else
    tools/run-save.sh "$save" --import-only
fi

mkdir -p "$pref/mods" "$pref/mod_config"
cp build/mods/item-randomizer.nrm "$pref/mods/"
# `build-app/mods.json` is the developer's toggle list; write one when it is
# absent so a fresh clone still enables the mod.
if [ -f build-app/mods.json ]; then
    cp build-app/mods.json "$pref/mods.json"
else
    printf '{"enabled_mods":["ogre_item_randomizer"],"mod_order":["ogre_item_randomizer"],"latest_game_mode":""}\n' \
        > "$pref/mods.json"
fi
# LOG=VERBOSE and CHANCE=100 make a test run report every acquisition and reroll
# every one of them. A file with no "mod_id" key is accepted by this port.
if [ ! -f "$pref/mod_config/ogre_item_randomizer.json" ]; then
    printf '{"storage":{"log":"VERBOSE","mode":"ANYTHING","chance":100,"shops":false,"seed":0}}\n' \
        > "$pref/mod_config/ogre_item_randomizer.json"
fi

exec env "OGRE_PREF_DIR=$pref" "OGRE_MOD_TEST=1" "$root/build-app/ogrebattle64" "$rom"
