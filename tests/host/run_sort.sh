#!/usr/bin/env bash
# Conformance test for the browser sort order (src/sort.c), compiled against the REAL source.
# sort_cli.c lays a directory table out the way scan_dir does and checks the order sort_dir()
# leaves behind: ".." first, folders, files, and the MSU-1 .pcm tracks last in numeric order.
#
# Built twice: QSORT_MAXELEM decides between the qsort path and the in-place heapsort the
# firmware falls back to for big folders, and both have to agree with the same comparator.
#
# The copies live in their own build/sort/ so the quoted includes of the copied source find the
# shim headers (shim_sort first, then shim) instead of the REAL ones next to src/sort.c. The
# SNES_FTYPE enum is extracted from the real filetypes.h rather than duplicated.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build/sort

cp ../../src/sort.c build/sort/sort_under_test.c || exit 1
cp ../../src/sort.h build/sort/sort.h            || exit 1
awk '/^typedef enum \{/{p=1} p{print} /\} SNES_FTYPE;/{p=0}' ../../src/filetypes.h > build/sort/.enum
if ! grep -q "TYPE_PCM" build/sort/.enum; then
  echo "!! enum extraction failed -- did SNES_FTYPE move in src/filetypes.h?" >&2
  exit 1
fi
{ printf '#ifndef HOST_FILETYPES_H\n#define HOST_FILETYPES_H\n'; cat build/sort/.enum; printf '#endif\n'; } \
  > build/sort/filetypes.h

rc=0
for maxelem in 2048 4; do
  $CC -O1 -Wall -Wextra -fsanitize=address,undefined \
      -DCONFIG_MCU_H='"config.h"' -DQSORT_MAXELEM=$maxelem -DSORT_STRLEN=256 \
      -I build/sort -I shim_sort -I shim \
      sort_cli.c build/sort/sort_under_test.c -o build/sort/sort_cli_$maxelem || exit 1
  ./build/sort/sort_cli_$maxelem || rc=1
done
exit $rc
