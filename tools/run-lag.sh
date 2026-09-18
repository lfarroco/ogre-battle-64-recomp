#!/bin/sh
# Run the port with the diagnostics needed to measure a "this screen got slow"
# report, and keep the output in a file an agent can read.
#
# Why a script: the interesting evidence is per-display-list timing, and the
# knobs that produce it are off by default, so a normal play session leaves
# nothing behind (session 79).
#
#   tools/run-lag.sh                    # keep the config-dir battery (your progress)
#   tools/run-lag.sh --save prologue    # start from assets/saves/<name> instead
#   tools/run-lag.sh --pref .ogre-prefs-mission
#   tools/run-lag.sh --tag dispatch     # log to /tmp/ogre-lag-dispatch.log
#
# All OGRE_* environment variables pass through. The log path is printed at
# start; the same path is appended to /tmp/ogre-lag-latest for convenience.
#
# What is enabled and why:
#   OGRE_DL_TRACE=1   one line per submitted display list with its trace_millis
#                     and the recompiled-function entries the frame spent, plus
#                     `processDisplayLists <us>` -- the interval between lines is
#                     the frame period, so "slow" becomes a number, and the pair
#                     (interval, process time) separates a CPU-bound frame from
#                     a render-bound one.
#   OGRE_SCENE_LOG=1  the scene timeline, so every measurement can be attributed
#                     to a screen.
#   OGRE_SYNC_TRACE=1 [syncfb]/[njpair] at the CPU framebuffer readback.
#   OGRE_DMA_TRACE=1  every streamed module load, grouped by (rom, ram). A load
#                     over a bank's RAM is what a wrong-bank/slow frame looks
#                     like.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app="$root/build-app/ogrebattle64"
default_rom="$root/assets/ogre64.z64"

usage() {
    sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

tag=run
pref=""
save=""
rom=""

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --tag) tag=$2; shift 2 ;;
        --pref) pref=$2; shift 2 ;;
        --save) save=$2; shift 2 ;;
        --rom) rom=$2; shift 2 ;;
        --) shift; break ;;
        -*) echo "run-lag.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) if [ -z "$rom" ]; then rom=$1; shift; else break; fi ;;
    esac
done

[ -x "$app" ] || { echo "run-lag.sh: $app is missing; build with: cmake --build build-app -j" >&2; exit 1; }
[ -n "$rom" ] || rom=$default_rom
[ -f "$rom" ] || { echo "run-lag.sh: no ROM at $rom" >&2; exit 1; }

if [ -n "$pref" ]; then
    case "$pref" in /*) ;; *) pref="$root/$pref" ;; esac
    [ -d "$pref" ] || { echo "run-lag.sh: no such pref dir $pref" >&2; exit 1; }
fi

log="/tmp/ogre-lag-$tag.log"
printf 'run-lag.sh: -> %s\n' "$log"
printf '%s\n' "$log" > /tmp/ogre-lag-latest

# Log to a file *and* to the terminal, so the run can be watched live. The app
# flushes its diagnostics, so no stdbuf is needed on the pipe.
set -- env \
    OGRE_DL_TRACE=1 \
    OGRE_SCENE_LOG=1 \
    OGRE_SYNC_TRACE=1 \
    OGRE_DMA_TRACE=1

[ -n "$pref" ] && set -- "$@" "OGRE_PREF_DIR=$pref"
[ -n "$save" ] && set -- "$@" "OGRE_SAVE=$save"
set -- "$@" "$app" "$rom"

exec "$@" 2>&1 | tee "$log"
