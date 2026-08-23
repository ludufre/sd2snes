#!/usr/bin/env bash
# Conformance test for psram_stream_buf()/psram_stream() (src/psram_io.c), compiled against
# the REAL source.  psram_cli.c pins the exact call sequence at the edges: a zero-sized buffer,
# a short read at EOF (f_read reports it as FR_OK, so the only signal is got != want), which
# FRESULT ends up in file_res, and the pump firing AFTER each chunk lands rather than before.
#
# The copy in build/ makes the shim headers win over the REAL firmware headers next to
# src/psram_io.c, which quoted #includes would otherwise pull in; psram_io.h rides along.
# -I shim comes FIRST: build/ is shared by every suite here and holds copies of REAL headers.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

cp ../../src/psram_io.c build/psram_io_under_test.c || exit 1
cp ../../src/psram_io.h build/psram_io.h            || exit 1

$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I shim -I build -I ../../src \
    psram_cli.c build/psram_io_under_test.c -o build/psram_cli || exit 1
exec ./build/psram_cli
