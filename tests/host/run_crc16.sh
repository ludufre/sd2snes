#!/usr/bin/env bash
# Conformance test for crc16_update() (src/crc16.c), compiled against the REAL source.
# crc16_cli.c keeps the 256-entry lookup table as the oracle: it and the bitwise
# implementation must agree on every input.
#
# src/crc16.c has no firmware dependencies beyond crc16.h, so it compiles straight from src/.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I ../../src \
    crc16_cli.c ../../src/crc16.c -o build/crc16_cli || exit 1
exec ./build/crc16_cli
