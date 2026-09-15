#!/usr/bin/env bash
# Watch a *guest* address under lldb.
#
# The mechanics that cost this project runs every time: the guest address is not
# a host address (RDRAM is a heap buffer, `rdram + (guest - 0x80000000)`), the
# base is only known at runtime, and an lldb batch script needs the watchpoint
# armed *after* the game has booted. This script does all three, and prints a
# backtrace per hit.
#
# Usage:
#   tools/watch.sh 0x80243E28                      # watch writes, from boot
#   tools/watch.sh 0x80243E28 --after func_80178920  # arm at the 2nd hit of it
#   tools/watch.sh 0x80243E28 --ignore 1 --hits 6    # skip the first hit
#   tools/watch.sh 0x80243E28 --size 2 --read        # 2-byte read watchpoint
#   tools/watch.sh 0x80243E28 --exe build-app/ogrebattle64
#
# Environment: the app's `OGRE_*` variables are inherited, so e.g.
#   OGRE_SPEED=4 OGRE_TAP_MS=1500 OGRE_TAP_NOT_SCENE=new-game,0x0D \
#     tools/watch.sh 0x80243E28 --after func_80178920
# reproduces the New Game step-2 window.
#
# Notes:
#  * `--after SYMBOL` is the *guest* function whose entry arms the watchpoint
#    (default `recomp_entrypoint`, i.e. arm immediately after boot). Use a
#    symbol that runs just before the window you care about — arming from boot
#    on a heap address produces thousands of hits.
#  * `--ignore N` makes the breakpoint skip its first N hits, so `--after
#    func_80178920 --ignore 1` arms on the *second* scene-`0x02` enter (the
#    transition into step 2).
#  * lldb stops once per *instruction* that writes the watched range, so a
#    memcpy loop hits it every `--size` bytes. Keep `--hits` small.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUEST=""
AFTER="recomp_entrypoint"
IGNORE=0
HITS=4
SIZE=8
MODE="write"
EXE="$ROOT/build-null/ogrebattle64"
SCRIPT="/tmp/ogre-watch.$$.lldb"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --after) AFTER="$2"; shift 2 ;;
        --ignore) IGNORE="$2"; shift 2 ;;
        --hits) HITS="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --read) MODE="read"; shift ;;
        --write) MODE="write"; shift ;;
        --exe) EXE="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) GUEST="$1"; shift ;;
    esac
done

if [[ -z "$GUEST" ]]; then
    echo "usage: tools/watch.sh <guest-address> [--after SYMBOL] [--ignore N] [--hits N] [--size N] [--read] [--exe PATH]" >&2
    exit 2
fi

# Normalise to a guest address and compute the RDRAM offset lldb will add.
GUEST_DEC=$((GUEST))
if (( GUEST_DEC < 0x80000000 )); then
    echo "note: $GUEST is below 0x80000000; treating it as a physical RDRAM offset" >&2
    OFFSET=$GUEST_DEC
else
    OFFSET=$((GUEST_DEC - 0x80000000))
fi
if (( OFFSET < 0 || OFFSET + SIZE > 0x800000 )); then
    echo "error: offset 0x$(printf '%X' "$OFFSET") is outside the 8 MiB RDRAM" >&2
    exit 2
fi

{
    echo "breakpoint set -n $AFTER"
    if (( IGNORE > 0 )); then
        echo "breakpoint modify -i $IGNORE 1"
    fi
    echo "run"
    echo "expr unsigned char* \$rdram = (unsigned char*)ultramodern_get_rdram_base()"
    echo "p/x (unsigned long)\$rdram"
    echo "watchpoint set expression -w $MODE -s $SIZE -- (void*)((unsigned long)\$rdram + 0x$(printf '%X' "$OFFSET"))"
    for ((i = 0; i < HITS; i++)); do
        echo "continue"
        echo "bt 12"
    done
    echo "quit"
} > "$SCRIPT"

echo "guest 0x$(printf '%08X' "$GUEST_DEC") -> rdram + 0x$(printf '%X' "$OFFSET")"
echo "watching $MODE size $SIZE, after $AFTER (ignore $IGNORE), $HITS hit(s)"
echo "lldb script: $SCRIPT"
echo
exec lldb -b -s "$SCRIPT" "$EXE"
