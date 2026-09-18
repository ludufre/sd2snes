#!/usr/bin/env bash
# Conformance test for the MCU half of the cheat EDITOR (src/cheatedit.c + src/cheatcode.c),
# compiled against the REAL sources over a fake 16 MB PSRAM.  What it pins down has no
# hardware fallback: an ADD must shift THREE parallel arrays in lockstep (records, code
# strings, flag mirror) and a REPLACE must leave a long name and
# the enable bit alone.  Getting any of those wrong does not crash -- the wrong cheat gets
# toggled, or a name from the neighbour shows through.
#
# Same layout trick as run_trainer.sh: the copies into build/ make the shim headers win
# over the REAL firmware headers next to the sources, -I shim_cheatedit comes FIRST, and
# -I ../../src after it lets the shim memory.h pull in the REAL memmap.h and the REAL
# cheatedit.h / cheatcode.h, so the addresses and offsets under test are the
# shipping ones.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh
mkdir -p build

cp ../../src/cheatedit.c  build/cheatedit_under_test.c || exit 1
cp ../../src/cheatcode.c  build/cheatcode_under_test.c || exit 1
cp ../../src/cheatedit.h  build/cheatedit.h            || exit 1
cp ../../src/cheatcode.h  build/cheatcode.h            || exit 1

$CC -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I shim_cheatedit -I build -I ../../src \
    cheatedit_cli.c build/cheatedit_under_test.c build/cheatcode_under_test.c \
    -o build/cheatedit_cli || exit 1
exec ./build/cheatedit_cli
