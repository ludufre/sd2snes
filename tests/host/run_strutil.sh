#!/usr/bin/env bash
# Conformance test for strlcpy_nul()/path_leaf() (src/strutil.c), compiled against the REAL source.
# strutil_cli.c pins the result against an independent model and lets ASan own the bounds,
# using exact-size heap allocations for dst.
#
# src/strutil.c is deliberately dependency-free (no config.h, no ff.h), so like src/crc16.c it
# compiles straight from src/.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I ../../src \
    strutil_cli.c ../../src/strutil.c -o build/strutil_cli || exit 1
exec ./build/strutil_cli
