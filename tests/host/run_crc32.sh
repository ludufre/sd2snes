#!/usr/bin/env bash
# CRC-32 (src/crc32.c, REAL; copied next to the shim config.h): see crc32_cli.c.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh
B=build/crc32
mkdir -p "$B"
cp ../../src/crc32.c ../../src/crc32.h "$B/" || exit 1
$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I "$B" -I shim_fileops \
    crc32_cli.c "$B/crc32.c" -o "$B/crc32_cli" || exit 1
exec "./$B/crc32_cli"
