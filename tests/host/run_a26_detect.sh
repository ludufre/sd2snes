#!/usr/bin/env bash
# Host gate for the .a26 bankswitch detector.
#
# Builds a small CLI around the REAL src/atari.c (a26_id + the scan and decision
# table it drives, with a stdio-backed FatFs under it -- see a26_detect_cli.c)
# and runs it over a corpus of .a26 images, comparing every verdict against a
# manifest.  Nothing about the detector is duplicated on this side: a mismatch
# means the firmware changed its mind about an image.
#
# CORPUS: $1, else $A26_CORPUS; without either the suite is skipped.  It is
# generated outside this repo, so an absent or empty directory is a SKIP;
# A26_CORPUS_REQUIRED=1 turns every reason for checking nothing into a failure.
# Either way the last line states how many images were checked.
#
# Layout:
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
# from them AFTER checking each one by hand) and the gate exits 0 -- unless
# A26_CORPUS_REQUIRED says otherwise.
#
# TWO BOARDS: atari.c is built twice, as on the Mk.III and with -DCONFIG_MK2
# (the Mk.II adds FE, Superchip by Stella's mirror rule, and refuses F4 because
# its core holds 16 KB of ROM).  The corpus is checked against expected.txt by
# the first build and against expected.mk2.txt (same format) by the second; a
# corpus without expected.mk2.txt is a MISS for the Mk.II pass.
#
# SYNTHETIC CASES: before the corpus, a handful of images generated here (no
# corpus needed, so they run in every checkout) pin what only the Mk.II build
# does and the scan paths a clean file never takes: a read error mid-scan and a
# LEAF scratch that is already held.  Neither may ever produce a Superchip.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
CORPUS="${1:-${A26_CORPUS:-}}"
REQUIRED="${A26_CORPUS_REQUIRED:-0}"
nrom=0    # images FOUND; the summary reports it even when none were checked
sfail=0   # failed synthetic cases (they fail the gate even when the corpus is skipped)

# Nothing to check: a skip when the corpus is optional, a failure when the
# caller has declared it must be there.  Always states the count.
nothing_checked() { # <reason>
  if [ "$sfail" -ne 0 ]; then
    echo "FAIL: $sfail synthetic case(s) failed (see above)" >&2
    exit 1
  fi
  if [ "$REQUIRED" != "0" ]; then
    echo "FAIL: $1" >&2
    echo "      A26_CORPUS_REQUIRED is set, so checking nothing is a failure." >&2
    echo "      Point \$A26_CORPUS at the corpus, or unset A26_CORPUS_REQUIRED." >&2
    echo "== summary: 0 of $nrom images checked ($CORPUS) -- REQUIRED =="
    exit 1
  fi
  echo "SKIP: $1"
  echo "== summary: 0 of $nrom images checked ($CORPUS) =="
  exit 0
}

echo "== build =="
mkdir -p build
# Quoted #includes resolve in the includer's own directory first, which would
# pull the REAL firmware headers next to src/atari.c. Compiling a byte-exact
# copy from build/ makes the shim headers win while atari.c stays unmodified.
# scratch.c (the shared scan buffer a26_id borrows) gets the same treatment.
cp ../../src/atari.c build/atari_under_test.c || exit 1
cp ../../src/scratch.c build/scratch_under_test.c || exit 1
$CC -O1 -g -fsanitize=address,undefined -I shim -I ../../src \
    a26_detect_cli.c build/atari_under_test.c build/scratch_under_test.c \
    -o build/a26_detect_cli || exit 1
$CC -O1 -g -fsanitize=address,undefined -DCONFIG_MK2 -I shim -I ../../src \
    a26_detect_cli.c build/atari_under_test.c build/scratch_under_test.c \
    -o build/a26_detect_cli_mk2 || exit 1
CLI=./build/a26_detect_cli
CLI_MK2=./build/a26_detect_cli_mk2

# ---- synthetic cases --------------------------------------------------------
# Filler = LCG bytes below $80: no "8D 3F 00", no read-port reference ($10xx/$F0xx
# with a low byte >= $80) and no FE signature can occur by accident, so every
# verdict below comes from what each image puts there on purpose.
echo "== synthetic =="
SYN=build/synth
rm -rf "$SYN"; mkdir -p "$SYN"
python3 - "$SYN" <<'EOF' || exit 1
import sys, os
out = sys.argv[1]
def filler(n, seed):
    s, b = seed, bytearray(n)
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        b[i] = (s >> 16) & 0x7F
    return b
def mirror(img, banks):            # Stella: bytes 0..127 of a 4K bank repeat at 128..255
    for k in banks:
        o = k * 4096
        img[o + 128:o + 256] = img[o:o + 128]
    return img
def put(name, img):
    with open(os.path.join(out, name), "wb") as f:
        f.write(bytes(img))
put("plain8k.a26", filler(8192, 1))
put("halfmirror8k.a26", mirror(filler(8192, 2), [0]))
put("stella8k.a26", mirror(filler(8192, 3), [0, 1]))
put("stella16k.a26", mirror(filler(16384, 4), [0, 1, 2, 3]))
fe = filler(8192, 5); fe[0x800:0x805] = bytes((0x20, 0x00, 0xD0, 0xC6, 0xC5))   # JSR $D000; DEC $C5
put("fe8k.a26", fe)
f8 = bytearray(fe); f8[0x900:0x903] = bytes((0x8D, 0xF9, 0x1F)); f8[0xA00:0xA03] = bytes((0x8D, 0xF9, 0x1F))
put("fe_f8sta8k.a26", f8)                     # "STA $1FF9" twice: potential F8, so not FE
put("f4_32k.a26", filler(32768, 6))
EOF

# <label> <cli> <env assignment or -> <image> <expected fields>
synth() {
  local label=$1 cli=$2 env=$3 img=$4; shift 4
  local want="$*" got field ok=1
  if [ "$env" = "-" ]; then
    got=$("$cli" "$SYN/$img" 2>&1 | grep '^SCHEME=')
  else
    got=$(env "$env" "$cli" "$SYN/$img" 2>&1 | grep '^SCHEME=')
  fi
  for field in $want; do
    case " $got " in *" $field "*) ;; *) ok=0 ;; esac
  done
  if [ "$ok" -eq 1 ]; then
    echo "PASS  $label: $got"
  else
    echo "FAIL  $label"
    echo "      want: $want"
    echo "      got:  $got"
    sfail=$((sfail+1))
  fi
}
nsyn=0
s() { nsyn=$((nsyn+1)); synth "$@"; }
s "mk3 plain8k"              $CLI     -                  plain8k.a26      SCHEME=F8 SC=0 ERROR=OK
s "mk2 plain8k"              $CLI_MK2 -                  plain8k.a26      SCHEME=F8 SC=0 ERROR=OK
# Stella's rule (Mk.II only): every bank mirrored -> Superchip; the Mk.III rule wants a
# constant lead-in plus read-port references, which these images do not have
s "mk3 stella8k"             $CLI     -                  stella8k.a26     SCHEME=F8 SC=0 ERROR=OK
s "mk2 stella8k"             $CLI_MK2 -                  stella8k.a26     SCHEME=F8 SC=1 ERROR=OK
s "mk2 stella16k"            $CLI_MK2 -                  stella16k.a26    SCHEME=F6 SC=1 ERROR=OK
s "mk2 halfmirror8k"         $CLI_MK2 -                  halfmirror8k.a26 SCHEME=F8 SC=0 ERROR=OK
# a scan that never saw a bank must not vouch for it: skipped (LEAF held) or cut short
s "mk2 plain8k LEAF held"    $CLI_MK2 A26_LEAF_BUSY=1    plain8k.a26      SCHEME=F8 SC=0 ERROR=OK
s "mk2 stella16k LEAF held"  $CLI_MK2 A26_LEAF_BUSY=1    stella16k.a26    SCHEME=F6 SC=0 ERROR=OK
s "mk2 halfmirror8k cut@4K"  $CLI_MK2 A26_READ_FAIL_AT=4096 halfmirror8k.a26 SCHEME=F8 SC=0 ERROR=OK
s "mk2 stella16k cut@8K"     $CLI_MK2 A26_READ_FAIL_AT=8192 stella16k.a26 SCHEME=F6 SC=0 ERROR=OK
s "mk2 stella8k cut@0"       $CLI_MK2 A26_READ_FAIL_AT=0 stella8k.a26     SCHEME=F8 SC=0 ERROR=OK
s "mk3 plain8k LEAF held"    $CLI     A26_LEAF_BUSY=1    plain8k.a26      SCHEME=F8 SC=0 ERROR=OK
# FE (Mk.II only): a Stella signature and no "potential F8"
s "mk3 fe8k"                 $CLI     -                  fe8k.a26         SCHEME=F8 SC=0 ERROR=OK
s "mk2 fe8k"                 $CLI_MK2 -                  fe8k.a26         SCHEME=FE SC=0 ERROR=OK
s "mk2 fe_f8sta8k"           $CLI_MK2 -                  fe_f8sta8k.a26   SCHEME=F8 SC=0 ERROR=OK
# F4: the Mk.II core holds 16 KB of ROM
s "mk3 f4_32k"               $CLI     -                  f4_32k.a26       SCHEME=F4 SC=0 ERROR=OK
s "mk2 f4_32k"               $CLI_MK2 -                  f4_32k.a26       SCHEME=- SC=0 ERROR=NOIMPL:F4
echo "== synthetic: $((nsyn - sfail)) of $nsyn pass =="

echo "== corpus ($CORPUS) =="
[ -d "$CORPUS" ] || nothing_checked "no corpus directory $CORPUS (it is generated outside this repo)"
roms=$(ls -1 "$CORPUS"/*.a26 2>/dev/null)
[ -n "$roms" ] || nothing_checked "no *.a26 in $CORPUS (it is generated outside this repo)"
nrom=$(printf '%s\n' "$roms" | wc -l | tr -d ' ')

EXPECTED="$CORPUS/expected.txt"
if [ ! -f "$EXPECTED" ]; then
  echo "no expected.txt in $CORPUS -- observed verdicts:"
  for rom in $roms; do
    got=$("$CLI" "$rom" 2>&1 | grep '^SCHEME=')
    echo "  $(basename "$rom")|$got"
  done
  nothing_checked "$nrom image(s) in $CORPUS but no expected.txt -- nothing to compare the verdicts against; seed it from the lines above AFTER checking each one by hand"
fi

pass=0; fail=0; miss=0; xfail=0

# <label> <cli> <manifest>: every corpus image against its line in <manifest>
run_pass() {
  local label=$1 cli=$2 EXPECTED=$3 rom base got want known ok field
  echo "== run: $label ($(basename "$EXPECTED")) =="
  if [ ! -f "$EXPECTED" ]; then
    echo "MISS  $label: no $(basename "$EXPECTED") in $CORPUS -- $nrom image(s) without expectation"
    miss=$((miss+nrom))
    return
  fi
for rom in $roms; do
  base="$label $(basename "$rom")"
  got=$("$cli" "$rom" 2>&1 | grep '^SCHEME=')
  # a matching line comes back as "[XFAIL ]<fields>"
  want=$(awk -v n="$(basename "$rom")" '
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
}

run_pass mk3 "$CLI"     "$CORPUS/expected.txt"
run_pass mk2 "$CLI_MK2" "$CORPUS/expected.mk2.txt"

echo "== summary: $((pass + xfail + fail + miss)) of $((2 * nrom)) image verdicts checked, $nrom images x 2 boards ($CORPUS): $pass pass, $xfail xfail, $fail fail, $miss without expectation; synthetic $((nsyn - sfail))/$nsyn =="
[ "$fail" -eq 0 ] && [ "$miss" -eq 0 ] && [ "$sfail" -eq 0 ] || exit 1
