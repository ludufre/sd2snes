#!/usr/bin/env bash
# Conformance test for sort_dir()/ext_heapsort() (src/sort.c), compiled against
# the REAL source. Verifies that lowering QSORT_MAXELEM -- the threshold where
# the file browser switches from the fast qsort (through the ptrcache[]
# AHB-RAM scratch buffer) to the slower in-place ext_heapsort() -- does not
# change the actual sort result for a directory that crosses the threshold.
#
# Runs sort_cli.c's fixed 1500-entry shuffled table TWICE: once built with
# QSORT_MAXELEM above 1500 (forces the qsort path) and once below it (forces
# ext_heapsort), and requires byte-identical, correctly-sorted output both
# times. See sort_cli.c's header comment for the full rationale.
#
# Quoted #includes resolve in the includer's own directory first (see
# run.sh), so sort.c is compiled from a byte-exact copy in build/ -- that way
# its `#include "config.h"`/`"sort.h"` fall through to the shim headers
# instead of finding the real, board-generated ones next to src/sort.c.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh

mkdir -p build
cp ../../src/sort.c build/sort_under_test.c

SORT_STRLEN=64
fails=0

for maxelem in 2048 1024; do
  echo "== build (QSORT_MAXELEM=$maxelem) =="
  $CC -O1 -g -fsanitize=address,undefined -I shim -I ../../src \
      -DQSORT_MAXELEM=$maxelem -DSORT_STRLEN=$SORT_STRLEN \
      shim.c sort_cli.c build/sort_under_test.c -o "build/sort_cli_$maxelem" || exit 1

  echo "== run (QSORT_MAXELEM=$maxelem) =="
  out=$("./build/sort_cli_$maxelem" 2>&1)
  rc=$?
  echo "$out"
  if san_report "$out"; then
    echo "FAIL: sanitizer report (QSORT_MAXELEM=$maxelem)"
    fails=$((fails+1))
  elif [ "$rc" -ne 0 ]; then
    echo "FAIL: sort_cli exited $rc (QSORT_MAXELEM=$maxelem)"
    fails=$((fails+1))
  fi
done

[ "$fails" -eq 0 ] || exit 1
echo "sort: qsort and ext_heapsort agree on the same 1500-entry table"
