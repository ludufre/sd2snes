#!/usr/bin/env bash
# Conformance test for the position/progress maths published by the menu PCM player, the
# card module src/modules/pcmplay/pcmmod.c: the REAL module, compiled natively the way host
# builds of the firmware link it (-DMODULE_HOST_LINKED), driven through a fake module_api_t
# (the audio engine stubbed).
#
# What it is really guarding: the 0..255 progress fraction.  Its naive form (pos * 255 / body)
# wraps a 32-bit product once the body passes ~16.8 MB -- about 95 seconds of 44.1 kHz stereo,
# so EVERY real MSU-1 track.  A wrapped fraction makes the bar jump backwards on hardware and
# is exactly the kind of thing that looks fine on a 10-second test clip.
#
# Only the real firmware headers are on the include path (module.h, ff.h, memmap.h,
# pcmplay.h, msu1.h): SRAM_PCMPLAY_ADDR and the Repeat byte come straight out of memmap.h.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

S=../../src
$CC -O1 -Wall -Wextra -fsanitize=address,undefined -DMODULE_HOST_LINKED -I "$S" \
    pcmpos_cli.c "$S/modules/pcmplay/pcmmod.c" -o build/pcmpos_cli || exit 1
exec ./build/pcmpos_cli
