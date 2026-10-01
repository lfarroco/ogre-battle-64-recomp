#!/bin/sh
# Drive the port with the diagnostics for a "this screen is slow" report.
#
# It runs three measurements at once, so the slow second is captured whenever it
# happens and no timing has to be agreed in advance:
#
#   run.log              stdout ([scene], [dl], [prof] T= lines, console output)
#   sample-<n>.txt       a host stack sample of the app every ~1 s (the ground
#                        truth: `sample <pid>` names the function the frame-pump
#                        thread t4 is parked in)
#   cover.txt            every recompiled function the run entered
#   slow.bin / slow.*.ppm  `snap` output: RDRAM at that instant plus the frames
#                        the renderer presented
#   menu-open.ckpt       a checkpoint that rewinds the machine to that instant
#
# Hotkeys while the game window has focus (edge-triggered, one command per press):
#   1  snap   -> writes <dir>/slow.bin (whole 8 MiB RDRAM) and enables the
#               presented-frame capture for 400 ms (PPMs land as
#               /tmp/ogre-shot.<present>.ppm)
#   2  save   -> writes <dir>/menu-open.ckpt, a checkpoint this build can load
#   3  cover  -> prints the function census so far to run.log
#   4  dump   -> writes <dir>/dump-<n>.bin (bare `dump`, one file per press)
#
# Usage:
#   debug/menu-probe.sh [-- <extra env>] [args passed to the app]
#
# Env knobs the caller may set: PROBE_DIR, OGRE_SPEED, OGRE_SAVE, EXTRA_ENV.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$REPO/build-app/ogrebattle64}
ROM=${ROM:-$REPO/assets/ogre64.z64}
DIR=${PROBE_DIR:-/tmp/ogre-menu-probe}

mkdir -p "$DIR"
rm -f "$DIR"/sample-*.txt

echo "[probe] dir=$DIR app=$APP"
echo "[probe] app stdout -> $DIR/run.log"

# The app reads every OGRE_* knob from the environment. Keep the log chatty
# enough to read the slow phase off it and to name the screen. `OGRE_KEY_<n>`
# turns on the key trigger by itself; `OGRE_CONSOLE_FILE` turns on the watched
# file as well, so an agent can drive this run by writing to $DIR/console.txt
# (the console is off unless one of its own variables names it, session 117).
env \
    OGRE_SCENE_LOG=1 \
    OGRE_DL_TRACE=1 \
    OGRE_PROFILE=1 \
    OGRE_COVER="$DIR/cover.txt" \
    OGRE_TRACE_HOOKS="${OGRE_TRACE_HOOKS:-0}" \
    OGRE_CONSOLE_FILE="$DIR/console.txt" \
    OGRE_KEY_1="snap $DIR/slow" \
    OGRE_KEY_2="save $DIR/menu-open.ckpt" \
    OGRE_KEY_3="cover" \
    OGRE_KEY_4="dump $DIR/dump" \
    $EXTRA_ENV \
    "$APP" "$ROM" >"$DIR/run.log" 2>&1 &

APP_PID=$!
echo "[probe] app pid=$APP_PID"

# `sample` writes a call graph of every thread of the target. Run it back to back
# so a slow second is always inside some sample window.
i=0
while kill -0 "$APP_PID" 2>/dev/null; do
    i=$((i + 1))
    n=$(printf '%04d' "$i")
    sample "$APP_PID" 1 -file "$DIR/sample-$n.txt" >/dev/null 2>&1 || true
done

wait "$APP_PID" 2>/dev/null || true
echo "[probe] app exited; samples=$i log=$DIR/run.log"
