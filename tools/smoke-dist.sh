#!/usr/bin/env bash
#
# Smoke-test a built package. Run on the machine that built it; `make smoke`
# does, and the release workflow runs it on every runner image so a package that
# cannot start fails the release instead of reaching a player.
#
#   tools/smoke-dist.sh [dist-dir]        # default: dist/ogre-battle-64-recomp
#
# Three checks:
#
#   1. the app binary and its companion files exist;
#   2. the package is self-contained. Windows: every DLL the shipped binaries
#      import is either shipped beside them or a Windows system DLL
#      (tools/pe_imports.py). This is the check that finds a missing runtime DLL
#      even on a machine that has the redistributable installed, where launching
#      the program would succeed anyway (session 94: dxcompiler.dll imports
#      MSVCP140.dll/VCRUNTIME140.dll and neither was in the package). Linux: the
#      statically linked SDL2 must carry an ALSA, PulseAudio or PipeWire backend,
#      because the OSS-only build that is otherwise possible cannot open a device
#      on any PipeWire or PulseAudio machine (session 100, issue #8);
#   3. the binary loads and reaches main: it is run with OGRE_SMOKE=1, which
#      exits 0 as soon as main is reached, under a deadline so a loader error
#      dialog or a hang cannot stall a release run.
#
# `SMOKE_TIMEOUT` (seconds, default 90) bounds check 3.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
dist=${1:-"$root/dist/ogre-battle-64-recomp"}
timeout_seconds=${SMOKE_TIMEOUT:-90}

[ -d "$dist" ] || { echo "smoke-dist: no such package directory: $dist" >&2; exit 1; }

# --- what kind of package is this? ------------------------------------------
kind=linux
if [ -f "$dist/ogrebattle64.exe" ]; then
    kind=windows
elif [ -d "$dist/Ogre Battle 64.app" ]; then
    kind=macos
fi
echo "smoke-dist: $dist (kind=$kind)"

fail() { echo "smoke-dist: FAIL: $*" >&2; exit 1; }

case "$kind" in
    windows) app="$dist/ogrebattle64.exe" ;;
    macos)   app="$dist/Ogre Battle 64.app/Contents/MacOS/ogrebattle64" ;;
    linux)   app="$dist/ogrebattle64" ;;
esac
[ -f "$app" ] || fail "the app binary is missing ($app)"
[ -f "$dist/README.txt" ] || fail "README.txt is missing from the package"

# --- 2. is the package self-contained? --------------------------------------
if [ "$kind" = windows ]; then
    for dll in dxcompiler.dll dxil.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
        [ -f "$dist/$dll" ] || fail "$dll is missing from the package"
    done
    python=""
    for candidate in python3 python py; do
        if command -v "$candidate" >/dev/null 2>&1; then python="$candidate"; break; fi
    done
    if [ -n "$python" ]; then
        if [ "$python" = py ]; then python="py -3"; fi
        # shellcheck disable=SC2086
        $python "$root/tools/pe_imports.py" "$dist" || fail "the package does not carry every DLL its binaries import"
    else
        echo "smoke-dist: no python found; skipping the PE import check"
    fi
elif [ "$kind" = macos ] && command -v otool >/dev/null 2>&1; then
    extra=$(otool -L "$app" | tail -n +2 | grep -vE "System/Library|/usr/lib|@executable_path" || true)
    [ -z "$extra" ] || fail "non-system dynamic dependencies: $extra"
    echo "smoke-dist: no non-system dynamic dependency"
elif [ "$kind" = linux ]; then
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$app" 2>/dev/null | grep -q "not found"; then
            ldd "$app" | grep "not found" >&2
            fail "the executable has unresolved shared libraries"
        fi
        echo "smoke-dist: every shared library resolves"
    fi
    # SDL2 is linked statically, so its audio backends are compiled in and the
    # binary itself names them. SDL2's CMake drops a backend whose development
    # headers are missing with no error, and on Linux the OSS driver's header is
    # the one that is always present: a static SDL2 built without
    # libasound2-dev/libpulse-dev/libpipewire-0.3-dev keeps only `dsp`, which no
    # PipeWire or PulseAudio desktop can open ("dsp: No such audio device"), and
    # the game runs silently (issue #8). Fail the release here, and not on the
    # player's machine.
    if ! LC_ALL=C grep -aqE "ALSA PCM audio|PulseAudio|Pipewire" "$app"; then
        fail "the binary carries no ALSA, PulseAudio or PipeWire SDL audio backend (issue #8)"
    fi
    echo "smoke-dist: the binary carries a Linux SDL audio backend"
fi

# --- 3. does it load and reach main? ----------------------------------------
host=other
case "$(uname -s)" in
    Darwin) host=macos ;;
    Linux) host=linux ;;
    MINGW*|MSYS*|CYGWIN*) host=windows ;;
esac
if [ "$host" != "$kind" ]; then
    echo "smoke-dist: package is $kind but this host is $host; skipping the launch check"
    echo "smoke-dist: PASS (static checks only)"
    exit 0
fi

work=$(mktemp -d 2>/dev/null || mktemp -d -t ogre-smoke)
trap 'rm -rf "$work"' EXIT
log="$work/smoke.log"

OGRE_SMOKE=1 OGRE_PREF_DIR="$work" "$app" >"$log" 2>&1 &
pid=$!
# A watchdog, because a loader failure can raise a dialog and wait for a click.
( sleep "$timeout_seconds"; kill -9 "$pid" 2>/dev/null ) &
watchdog=$!
set +e
wait "$pid"
code=$?
set -e
kill "$watchdog" 2>/dev/null || true
wait "$watchdog" 2>/dev/null || true

if [ "$code" -ne 0 ]; then
    echo "smoke-dist: the app exited $code" >&2
    [ -s "$log" ] && { echo "--- output ---"; cat "$log" >&2; }
    [ -f "$work/error.log" ] && { echo "--- error.log ---"; cat "$work/error.log" >&2; }
    fail "the packaged binary did not reach main (or did not exit cleanly)"
fi
if [ -s "$log" ]; then
    grep -q "\[smoke\] main reached" "$log" || {
        cat "$log" >&2
        fail "the app exited 0 without reaching the smoke marker"
    }
fi
echo "smoke-dist: the binary loaded, reached main and exited 0"

# --- 4. does a crash leave a report that contains the captured log? ----------
# `OGRE_CRASH_TEST=segv` raises SIGSEGV right after the logger is armed, so the
# report must exist and must contain the [boot] line printed immediately before
# the raise. That line is only in the report if the stdout/stderr capture
# reached the ring: a handler that dies on a stdio lock, or a capture that never
# ran, leaves a report with empty sections -- exactly what a Windows report
# looked like in session 94. `error.log` goes to the executable's own directory,
# which is the package root for every package kind.
report_file="$dist/error.log"
rm -f "$report_file"
set +e
# The braces and the redirect swallow the shell's own "Segmentation fault"
# message for the intentional crash.
{ OGRE_CRASH_TEST=segv OGRE_PREF_DIR="$work" "$app" >/dev/null 2>&1; } 2>/dev/null
set -e
if [ ! -f "$report_file" ]; then
    fail "OGRE_CRASH_TEST=segv wrote no error.log"
fi
if ! grep -q "OGRE_CRASH_TEST" "$report_file"; then
    echo "--- error.log ---" >&2
    cat "$report_file" >&2
    fail "the crash report did not contain the captured boot log"
fi
echo "smoke-dist: the crash report captured the boot log"
rm -f "$report_file"

echo "smoke-dist: PASS"
