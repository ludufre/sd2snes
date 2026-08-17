#!/usr/bin/env bash
# Host gate for the .a26 bankswitch detector.
#
# Builds a small CLI around the REAL src/atari.c (a26_id + the scan and decision
# table it drives, with a stdio-backed FatFs under it -- see a26_detect_cli.c)
# and runs it over a corpus of .a26 images, comparing every verdict against a
# manifest.  Nothing about the detector is duplicated on this side: a mismatch
# means the firmware changed its mind about an image.
#
# CORPUS (default ../../../atari/tests/corpus, override with $1) -- it is
# generated outside this repo, so an absent or empty directory is a SKIP:
#   <name>.a26     the image
#   expected.txt   one line per image, '#' comments and blank lines ignored:
#
#     [XFAIL|]<file>|SCHEME=<2K|4K|F8|F6|F4|-> SC=<0|1> ERROR=<OK|NOIMPL:<param>> [SIZE=<bytes>]
#
#                  <file> is the basename; after the first '|' comes a list of
#                  key=value fields.  EVERY field the manifest carries has to
#                  appear identically in the CLI's "SCHEME=" line; fields the CLI
#                  prints on top of it (SIZE=) are informational, so a manifest
#                  that only pins the verdict stays valid.  SCHEME is "-" whenever
#                  ERROR is not OK.  Example:
#                    f8_clean.a26|SCHEME=F8 SC=0 ERROR=OK
#                    tf3f.a26|SCHEME=- SC=0 ERROR=NOIMPL:3F
#
#                  A leading XFAIL| marks a verdict the detector is KNOWN not to
#                  reach and that the policy accepts (the manifest still spells
#                  out the right answer, so the cost of the limitation stays on
#                  the record).  Such a line is expected to mismatch: XFAIL is
#                  not a failure, but an XFAIL line that starts matching IS one,
#                  because the manifest has gone stale -- drop the marker then.
#
# Without expected.txt the observed lines are printed instead (seed the manifest
# from them AFTER checking each one by hand) and the gate exits 0.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
CORPUS="${1:-../../../atari/tests/corpus}"

echo "== build =="
mkdir -p build
# Quoted #includes resolve in the includer's own directory first, which would
# pull the REAL firmware headers next to src/atari.c. Compiling a byte-exact
# copy from build/ makes the shim headers win while atari.c stays unmodified.
cp ../../src/atari.c build/atari_under_test.c || exit 1
$CC -O1 -g -fsanitize=address,undefined -I shim -I ../../src \
    a26_detect_cli.c build/atari_under_test.c -o build/a26_detect_cli || exit 1
CLI=./build/a26_detect_cli

echo "== corpus ($CORPUS) =="
if [ ! -d "$CORPUS" ]; then
  echo "SKIP: no corpus directory $CORPUS (generated outside this repo)"
  exit 0
fi
roms=$(ls -1 "$CORPUS"/*.a26 2>/dev/null)
if [ -z "$roms" ]; then
  echo "SKIP: no *.a26 in $CORPUS (generated outside this repo)"
  exit 0
fi
EXPECTED="$CORPUS/expected.txt"
if [ ! -f "$EXPECTED" ]; then
  echo "no expected.txt in $CORPUS -- observed verdicts:"
  for rom in $roms; do
    got=$("$CLI" "$rom" 2>&1 | grep '^SCHEME=')
    echo "  $(basename "$rom")|$got"
  done
  exit 0
fi

pass=0; fail=0; miss=0; xfail=0

echo "== run =="
for rom in $roms; do
  base=$(basename "$rom")
  got=$("$CLI" "$rom" 2>&1 | grep '^SCHEME=')
  # a matching line comes back as "[XFAIL ]<fields>"
  want=$(awk -v n="$base" '
    /^[ \t]*#/ { next }
    {
      line = $0; mark = ""
      if (line ~ /^[ \t]*XFAIL\|/) { mark = "XFAIL "; sub(/^[ \t]*XFAIL\|/, "", line) }
      name = line; sub(/\|.*/, "", name)
      if (name != n) next
      sub(/^[^|]*\|/, "", line)
      print mark line
      exit
    }' "$EXPECTED")

  known=0
  case "$want" in
    "XFAIL "*) known=1; want=${want#XFAIL } ;;
  esac

  ok=1
  for field in $want; do
    case " $got " in
      *" $field "*) ;;
      *) ok=0 ;;
    esac
  done

  if [ -z "$want" ]; then
    echo "MISS  $base: no line in $(basename "$EXPECTED") -- got: $got"
    miss=$((miss+1))
  elif [ "$known" -eq 1 ] && [ "$ok" -eq 0 ]; then
    echo "XFAIL $base: accepted limitation"
    echo "      want: $want"
    echo "      got:  $got"
    xfail=$((xfail+1))
  elif [ "$known" -eq 1 ]; then
    echo "XPASS $base: $got"
    echo "      the XFAIL| marker is stale -- drop it from $(basename "$EXPECTED")"
    fail=$((fail+1))
  elif [ "$ok" -eq 1 ]; then
    echo "PASS  $base: $got"
    pass=$((pass+1))
  else
    echo "FAIL  $base"
    echo "      want: $want"
    echo "      got:  $got"
    fail=$((fail+1))
  fi
done

echo "== summary: $pass pass, $xfail xfail, $fail fail, $miss without expectation =="
[ "$fail" -eq 0 ] && [ "$miss" -eq 0 ] || exit 1
