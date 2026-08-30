#!/usr/bin/env bash
# Conformance test for the MCU half of the RAM trainer (src/trainer.c), compiled against the
# REAL source over a fake 16 MB PSRAM.  The record layout a freeze produces is the one thing
# here with no hardware fallback: get it wrong and the firmware freezes the wrong address, or
# a byte of a previous YAML cheat's description shows through -- neither of which crashes.
#
# The copy into build/ makes the shim headers win over the REAL firmware headers that sit
# next to src/trainer.c; -I shim_trainer comes FIRST for the same reason, and -I ../../src
# after it is what lets the shim memory.h pull in the REAL memmap.h (so the addresses under
# test are the shipping ones, not a copy that could drift).
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh
mkdir -p build

cp ../../src/trainer.c build/trainer_under_test.c || exit 1
cp ../../src/trainer.h build/trainer.h            || exit 1

$CC -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I shim_trainer -I build -I ../../src \
    trainer_cli.c build/trainer_under_test.c -o build/trainer_cli || exit 1
exec ./build/trainer_cli
