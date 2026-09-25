#!/usr/bin/env bash
#
# Fail when a static SDL2 has no audio backend that can open a real device.
#
#   tools/check-sdl2-audio.sh <libSDL2.a>
#
# SDL2's CMake switches a backend off *without an error* when its development
# headers are missing: `SDL_ALSA`, `SDL_PULSEAUDIO` and `SDL_PIPEWIRE` each call
# `check_include_file()` and fall back to OFF (SDL-2.32.10 CMakeLists.txt lines
# 443/449/451). The configure summary prints `SDL_ALSA (Wanted: ON): OFF` while
# the CMake cache variable still reads ON, so the built archive is the only place
# that says what was compiled in. On Linux the OSS driver's header is the one
# that is always present, so a build host without `libasound2-dev`,
# `libpulse-dev` and `libpipewire-0.3-dev` produces an SDL2 whose only output
# driver is `/dev/dsp`. No PipeWire or PulseAudio desktop has `/dev/dsp`, so
# `SDL_InitSubSystem(SDL_INIT_AUDIO)` fails with "dsp: No such audio device" and
# the game runs silently (issue #8). `dsp` is therefore not accepted here.
#
# The system SDL2 on a normal Linux distribution (what Zelda64Recomp links)
# always has these backends; this script only exists because this project builds
# SDL2 from source for the static link.
set -eu

lib=${1:?usage: check-sdl2-audio.sh <libSDL2.a>}
[ -f "$lib" ] || { echo "check-sdl2-audio: no such archive: $lib" >&2; exit 1; }

# The bootstrap structs are named `<BACKEND>_bootstrap`, with a leading
# underscore in Mach-O's symbol table. Only a *defined* symbol counts: the object
# that fills SDL2's bootstrap array also references each enabled backend, so an
# undefined (`U`) entry would say "referenced", not "compiled in". Both are behind
# the same `#ifdef`, so an undefined entry cannot appear alone in practice, and
# the symbol type is still checked so the script means exactly what it says.
backends='^_?(ALSA|PULSEAUDIO|PIPEWIRE|COREAUDIO|WASAPI|DSOUND|AAUDIO|openslES)_bootstrap$'

# `nm` ships with binutils on Linux and with Xcode on macOS, and reads the
# archive's symbol table whether or not the objects inside were stripped.
if nm -g "$lib" 2>/dev/null |
    awk -v re="$backends" '$NF ~ re && $(NF - 1) != "U" { found = 1 } END { exit(found ? 0 : 1) }'; then
    exit 0
fi

{
    echo "check-sdl2-audio: $lib has no usable SDL audio backend."
    echo "Compiled-in drivers:"
    nm -g "$lib" 2>/dev/null |
        awk '$NF ~ /_bootstrap$/ && $(NF - 1) != "U" { name = $NF; sub(/^_/, "", name); print "  " name }' |
        sort -u || true
    echo "Install the audio development headers and rebuild the static SDL2:"
    echo "  Debian/Ubuntu: sudo apt-get install -y libasound2-dev libpulse-dev libpipewire-0.3-dev"
    echo "  Fedora:        sudo dnf install -y alsa-lib-devel pulseaudio-libs-devel pipewire-devel"
    echo "Then remove the cached build: rm -rf tools/SDL2-static/prefix tools/SDL2-static/build"
} >&2
exit 1
