#!/usr/bin/env bash
# src/sdcrc64.h (the SD write/calibration CRC, lpc175x/sdnative.c) against a bit-by-bit
# CRC16-CCITT per data line and the SD spec's 0xFF example.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I ../../src \
    sdcrc64_cli.c -o build/sdcrc64_cli || exit 1
exec ./build/sdcrc64_cli
