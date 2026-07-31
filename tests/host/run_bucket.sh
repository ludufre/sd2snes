#!/usr/bin/env bash
# Conformance test for THE bucket rule of the two-letter SD layout (firmware 2.15+).
#
# WHY THIS EXISTS: the rule is implemented twice -- here in the firmware (which READS the paths)
# and in the Web Manager's core/sd-layout.ts (which WRITES them). If the two ever disagree the
# device looks in a different directory than the Manager created, and the user sees saves, cheats
# and covers "disappear". The Manager's vitest spec uses the SAME case table as bucket_cli.c.
#
# path_bucket2/path_asset are pure string functions, so rather than shim the whole of fileops.c
# (FATFS, f_mount, ...) this extracts just those two from the REAL source at build time -- same
# spirit as run.sh copying patch.c into build/. They are never duplicated.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
mkdir -p build

awk '/^void path_bucket2/{p=1} /^\/\* mkdir -p of the directory portion/{p=0} p' \
    ../../src/fileops.c > build/bucket_under_test.c
if ! grep -q "^int path_asset" build/bucket_under_test.c; then
  echo "!! extraction failed -- did path_bucket2/path_asset move or get renamed in fileops.c?" >&2
  exit 1
fi
printf '#include <string.h>\n' | cat - build/bucket_under_test.c > build/.t && mv build/.t build/bucket_under_test.c

$CC -O1 -Wall -Wextra -fsanitize=address,undefined \
    bucket_cli.c build/bucket_under_test.c -o build/bucket_cli || exit 1
exec ./build/bucket_cli
