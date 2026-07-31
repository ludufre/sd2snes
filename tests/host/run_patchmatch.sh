#!/usr/bin/env bash
# Conformance test for the patch-selector match rule (patch_belongs_to_rom).
#
# Like run_patchname.sh / run_bucket.sh, this compiles the functions straight out
# of the REAL source rather than shimming all of patch.c (FatFs, the FPGA SPI
# window, ...).  They are pure string code, so extracting them keeps the test
# honest without duplicating a line of the rule under test.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
mkdir -p build

SRC=../../src/patch.c
OUT=build/patchmatch_under_test.c

# patch_belongs_to_rom leans on both of these, so all three come along.
{
  printf '#include <string.h>\n'
  # Keep the type constants in lockstep with the header instead of restating them.
  grep -E '^#define PATCH_TYPE_(IPS|BPS)[[:space:]]' ../../src/patch.h
  awk '/^static int istartswith/{p=1} p; /^}/{if(p) exit}'      "$SRC"
  awk '/^int patch_ext_type/{p=1} p; /^}/{if(p) exit}'          "$SRC"
  awk '/^int patch_belongs_to_rom/{p=1} p; /^}/{if(p) exit}'    "$SRC"
} > "$OUT"

for fn in istartswith patch_ext_type patch_belongs_to_rom; do
  if ! grep -q "$fn" "$OUT"; then
    echo "!! extraction failed -- did $fn move or get renamed in patch.c?" >&2
    exit 1
  fi
done
for def in PATCH_TYPE_IPS PATCH_TYPE_BPS; do
  if ! grep -q "^#define $def" "$OUT"; then
    echo "!! extraction failed -- $def missing from patch.h?" >&2
    exit 1
  fi
done

$CC -O1 -Wall -Wextra -fsanitize=address,undefined \
    patchmatch_cli.c "$OUT" -o build/patchmatch_cli || exit 1
exec ./build/patchmatch_cli
