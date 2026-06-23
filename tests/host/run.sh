#!/usr/bin/env bash
# Build the host harness against the REAL src/patch.c, generate the corpus and
# run every case. Exit codes from the harness:
#   0 ok / 1 clean failure / 2 wrong output / 124 HANG / 125 region OVERFLOW
#
# A case may carry an XFAIL=<code> marker: a known bug makes it currently exit
# <code> instead of the wanted code. XFAIL cases don't fail the suite; once the
# bug is fixed the case starts PASSing and the marker should be removed.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"

echo "== build =="
# Quoted #includes resolve in the includer's own directory first, which would
# pull the REAL firmware headers next to src/patch.c. Compiling a byte-exact
# copy from build/ makes the shim headers win while patch.c stays unmodified.
mkdir -p build
cp ../../src/patch.c build/patch_under_test.c
$CC -O1 -g -fsanitize=address,undefined -I shim -I ../../src \
    shim.c harness.c build/patch_under_test.c -o harness || exit 1

echo "== corpus =="
python3 gen_corpus.py corpus || exit 1

pass=0; fail=0; xfail=0; xpass=0

run_case() { # <name> <want> <xfail-code|-> <mode> <patch> <rom> [expected_target]
  local name=$1 want=$2 xf=$3 mode=$4 patch=$5 rom=$6 tgt=${7:-}
  local out rc
  out=$(./harness "$mode" "corpus/$patch" "corpus/$rom" ${tgt:+corpus/$tgt} 2>&1)
  rc=$?
  if [ "$rc" -eq "$want" ]; then
    if [ "$xf" != "-" ]; then
      echo "XPASS $name (exit $rc) -- bug fixed, remove the XFAIL marker"
      xpass=$((xpass+1))
    else
      echo "PASS  $name (exit $rc)"
    fi
    pass=$((pass+1))
  elif [ "$xf" != "-" ] && [ "$rc" -eq "$xf" ]; then
    echo "XFAIL $name (exit $rc, want $want) -- known bug"
    xfail=$((xfail+1))
  else
    echo "FAIL  $name: exit $rc, want $want"
    echo "$out" | sed 's/^/      /' | head -5
    fail=$((fail+1))
  fi
}

echo "== run =="
#        name                want xfail  mode   patch                  rom        [target]
run_case ok-small-bps        0    -      apply  ok-small.bps           rom4k.bin  ok-small.bps.target
run_case huge-target         1    -      apply  huge-target.bps        rom4k.bin
run_case overflow-window     1    -      apply  overflow-window.bps    rom7m.bin
run_case target-overrun      1    -      apply  target-overrun.bps     rom4k.bin
run_case source-oob          1    -      apply  source-oob.bps         rom4k.bin
run_case tiny-truncated      1    -      apply  tiny-truncated.bps     rom4k.bin
run_case vli-straddles-eof   1    -      apply  vli-straddles-eof.bps  rom4k.bin
run_case probe-tiny          1    -      probe  tiny-truncated.bps     rom4k.bin
run_case ok-small-ips        0    -      apply  ok-small.ips           rom4k.bin  ok-small.ips.target
run_case ips-oob-offset      1    -      apply  oob-offset.ips         rom4k.bin
run_case ips-truncated-noeof 1    -      apply  truncated-noeof.ips    rom4k.bin

echo "== summary: $pass pass, $fail fail, $xfail xfail, $xpass xpass =="
[ "$fail" -eq 0 ] || exit 1
