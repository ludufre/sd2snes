#!/usr/bin/env bash
# Conformance test for the patch-selector display name (patch_display_name).
#
# Like run_bucket.sh, this compiles the function straight out of the REAL source
# instead of shimming all of patch.c (FatFs, the FPGA SPI window, ...): the
# function is pure string code, so extracting it keeps the test honest without
# duplicating a line of it.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
mkdir -p build

awk '/^int patch_display_name/{p=1} p; /^}/{if(p) exit}' \
    ../../src/patch.c > build/patchname_under_test.c
if ! grep -q "^int patch_display_name" build/patchname_under_test.c; then
  echo "!! extraction failed -- did patch_display_name move or get renamed in patch.c?" >&2
  exit 1
fi
printf '#include <string.h>\n' | cat - build/patchname_under_test.c > build/.t \
  && mv build/.t build/patchname_under_test.c

$CC -O1 -Wall -Wextra -fsanitize=address,undefined \
    patchname_cli.c build/patchname_under_test.c -o build/patchname_cli || exit 1
exec ./build/patchname_cli
