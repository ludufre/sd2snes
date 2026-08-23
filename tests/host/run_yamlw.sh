#!/usr/bin/env bash
# Conformance test for yaml_open_write()/yaml_put_quoted() (src/yamlw.c) plus the escaping and
# entity-decoding halves of src/yaml.c, compiled against the REAL sources.  These write the
# per-ROM cheat and patch sidecars, whose values are filenames and hand-typed labels: a '"' that
# reaches the file unescaped ends the scalar early, and an unescaped '&' is re-decoded as an
# entity on the next load.  The round-trip cases here pin both.
#
# The copies in build/ make the shim headers win over the REAL firmware headers next to
# src/yamlw.c and src/yaml.c, which quoted #includes would otherwise pull in (config.h ->
# autoconf.h, which only the firmware build generates); yaml.h and yamlw.h ride along.
# -I shim comes FIRST: build/ is shared by every suite here and holds copies of REAL headers.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

echo "== guard: every yaml_put_quoted prefix ends in the opening quote =="
# yaml_put_quoted writes the CLOSING quote and nothing else, so a prefix that does not carry
# the opening one produces  Key: value"  -- valid to write, unparseable to read back.  The
# contract is unenforceable in C, so enforce it here over the real call sites.
bad=0
while IFS= read -r line; do
  case "$line" in
    *'yaml_put_quoted('*'\"", '*) ;;
    *) echo "!! prefix does not end in \\\": $line" >&2; bad=1 ;;
  esac
done <<EOF
$(grep -hn 'yaml_put_quoted(' ../../src/*.c | grep -v 'void yaml_put_quoted')
EOF
[ "$bad" -eq 0 ] || exit 1
echo "ok"

echo "== build =="
cp ../../src/yamlw.c build/yamlw_under_test.c || exit 1
cp ../../src/yaml.c  build/yaml_under_test.c  || exit 1
cp ../../src/yamlw.h build/yamlw.h            || exit 1
cp ../../src/yaml.h  build/yaml.h             || exit 1

# -Wno-format: yaml.c prints uint32_t with %ld, right on the LPC175x (a long IS 32 bits there);
# those calls sit behind DBG_YAML, i.e. a while(0).
$CC -O1 -Wall -Wextra -Wno-format -fsanitize=address,undefined \
    -I shim -I build -I ../../src \
    yamlw_cli.c build/yamlw_under_test.c build/yaml_under_test.c -o build/yamlw_cli || exit 1
exec ./build/yamlw_cli
