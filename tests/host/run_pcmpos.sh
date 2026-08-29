#!/usr/bin/env bash
# Conformance test for the position/progress maths published by src/pcmplay.c (the menu PCM
# player), compiled against the REAL source with the audio engine stubbed.
#
# What it is really guarding: the 0..255 progress fraction.  Its naive form (pos * 255 / body)
# wraps a 32-bit product once the body passes ~16.8 MB -- about 95 seconds of 44.1 kHz stereo,
# so EVERY real MSU-1 track.  A wrapped fraction makes the bar jump backwards on hardware and
# is exactly the kind of thing that looks fine on a 10-second test clip.
#
# The copy in build/ makes the shim headers win over the REAL firmware headers next to
# src/pcmplay.c, which quoted #includes would otherwise pull in.  SRAM_PCMPLAY_ADDR comes
# straight out of src/memmap.h via -D, so there is no second copy of it to drift.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

PCM_ADDR=$(sed -n 's/^#define[[:space:]]*SRAM_PCMPLAY_ADDR[[:space:]]*(\(0x[0-9A-Fa-f]*L\)).*/\1/p' ../../src/memmap.h)
[ -n "$PCM_ADDR" ] || { echo "cannot read SRAM_PCMPLAY_ADDR from src/memmap.h"; exit 1; }

cp ../../src/pcmplay.c build/pcmplay_under_test.c || exit 1
cp ../../src/pcmplay.h build/pcmplay.h            || exit 1
cp ../../src/msu1.h    build/msu1.h               || exit 1

$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I shim -I build -I ../../src \
    -DSRAM_PCMPLAY_ADDR="$PCM_ADDR" \
    pcmpos_cli.c build/pcmplay_under_test.c -o build/pcmpos_cli || exit 1
exec ./build/pcmpos_cli
