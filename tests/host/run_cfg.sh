#!/usr/bin/env bash
# Golden gate for the config.yml serializer (src/cfg.c + src/yaml.c): pins both
# directions byte for byte -- what cfg_save() writes, and what cfg_load() makes
# of the fixtures in golden/.
#
# MODES
#   --capture   (re)write golden/ from the current source.  Only ever run this
#               after reading the diff: it is the moment the expectation is set.
#   --check     (default) rebuild and compare against golden/.  Exit 0/1.
#
# THE FILTER
#   The .yml goldens carry a comment banner cfg_save() no longer writes; yaml.c
#   truncates comments before parsing, so the .yml cases compare filtered against
#   filtered and what is pinned is the set of "Key: value" lines and their order.
#   The .yml goldens are ANCHORS -- do NOT re-capture them to strip the comments.
#   The .bin cases are compared raw -- no filter, no tolerance.
#
# WHAT IS COMPARED (per config, mk2 and mk3)
#   yml-default        save-default            vs golden .yml   (filtered)
#   yml-allchanged     save-allchanged         vs golden .yml   (filtered)
#   bin-default        load-dump(golden .yml)  vs golden .bin   (raw)
#   bin-allchanged     load-dump(golden .yml)  vs golden .bin   (raw)
#   bin-legacy         load-dump(legacy .yml)  vs golden .bin   (raw)
#   bin-illtyped       load-dump(illtyped .yml) vs golden .bin  (raw)
#   filter-neutral-*   load-dump(filtered)     vs golden .bin   (raw)
#   roundtrip-*        load(filtered)+save     vs the filtered golden
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
MODE="${1:---check}"
# The golden corpus is not part of this tree; point CFG_GOLDEN at it.
GOLDEN="${CFG_GOLDEN:-}"
GEN=build/cfg

case "$MODE" in
  --capture|--check) ;;
  *) echo "usage: $0 [--capture|--check]" >&2; exit 2 ;;
esac

if [ -z "$GOLDEN" ] || [ ! -d "$GOLDEN" ]; then
  if [ -n "${CFG_GOLDEN_REQUIRED:-}" ]; then
    echo "FAIL: golden corpus not found ($GOLDEN) and CFG_GOLDEN_REQUIRED is set" >&2
    exit 1
  fi
  echo "SKIP: golden corpus not found ($GOLDEN)"
  exit 0
fi
mkdir -p build "$GEN"

echo "== guard: f_printf specifiers =="
# f_printf (src/ff.c) is its own tiny formatter -- no %f, no %p, its own idea of
# a width, and 'l' meaning a 32-bit long.  cfg_cli.c models it conversion by
# conversion, so vet a new specifier against both before widening the list here.
# Only f_printf lines are grepped, so a %04X in a plain printf is fine.
bad=
for spec in $(grep 'f_printf' ../../src/cfg.c | grep -oE '%[0-9]*l?[a-zA-Z]' | sort -u); do
  case "$spec" in
    %s|%d|%ld|%06lX|%08lX) ;;
    *) bad="$bad $spec" ;;
  esac
done
if [ -n "$bad" ]; then
  echo "!! src/cfg.c uses f_printf specifier(s) the host CLI does not vouch for:$bad" >&2
  echo "   check them against f_printf() in src/ff.c, then widen the list here." >&2
  exit 1
fi
echo "ok"

echo "== build =="
# Quoted #includes resolve in the includer's own directory first, which would
# pull the REAL firmware headers next to src/cfg.c.  Compiling byte-exact copies
# from build/ makes the shim headers win while cfg.c and yaml.c stay unmodified;
# yaml.h is relocated for the same reason one step down (it includes "config.h",
# which from src/ resolves to the real one and its build-generated autoconf.h).
cp ../../src/cfg.c  build/cfg_under_test.c  || exit 1
cp ../../src/yaml.c build/yaml_under_test.c || exit 1
cp ../../src/yaml.h build/yaml.h            || exit 1

# The two S-RTC converters cfg.c calls are pure functions of their arguments, so
# extract just those two from the REAL source rather than shim the whole platform
# rtc.c -- same spirit as run_bucket.sh.
{
  printf '#include <stdint.h>\n'
  awk '/^void bcdtime2srtctime/,/^}/'  ../../src/lpc175x/rtc.c
  awk '/^uint64_t srtctime2bcdtime/,/^}/' ../../src/lpc175x/rtc.c
} > build/rtc_under_test.c
for fn in bcdtime2srtctime srtctime2bcdtime; do
  grep -q "^\(void\|uint64_t\) $fn" build/rtc_under_test.c && continue
  echo "!! extraction failed -- did $fn move or get renamed in src/lpc175x/rtc.c?" >&2
  exit 1
done

# cfg.c static-asserts that the favorites SRAM mirror stops short of the game
# info block.  Both addresses live in src/memory.h, which the host build
# replaces with a shim carrying only what the tests need, so lift the two values
# out of the REAL header rather than transcribing them here -- same spirit as
# the rtc extraction above.
MAPDEFS=
for sym in SRAM_FAVORITEGAMES_ADDR SRAM_GAMEINFO_ADDR; do
  val=$(grep -hE "^#define[[:space:]]+$sym[[:space:]]" ../../src/memmap.h ../../src/memory.h 2>/dev/null | head -1 | awk '{print $3}')
  case "$val" in
    0[xX][0-9a-fA-F]*|\(0[xX][0-9a-fA-F]*) ;;
    *) echo "!! $sym not found as a plain hex literal in src/memmap.h or src/memory.h (got '$val')" >&2; exit 1 ;;
  esac
  MAPDEFS="$MAPDEFS -D$sym=$val"
done

# shim_cfg MUST come before shim: it holds the config.h that leaves __attribute__
# alone, so cfg_t stays __packed__ and the offsetof _Static_asserts in cfg.c are
# checked here too.  -Wno-format: yaml.c prints uint32_t with %ld, which is right
# on the target; those calls sit behind DBG_YAML anyway.  src/strutil.c is linked
# in whole so cfg.c calls the REAL strlcpy_nul, not a stub free to disagree about
# truncation and NUL placement.
build_one() { # <suffix> [extra cflags]
  $CC -O1 -g -Wall -Wno-format -fsanitize=address,undefined ${2:-} $MAPDEFS \
      -I build -I shim_cfg -I shim -I ../../src \
      cfg_cli.c build/cfg_under_test.c build/yaml_under_test.c build/rtc_under_test.c \
      ../../src/strutil.c \
      -o "build/cfg_cli_$1"
}
build_one mk2 || exit 1
build_one mk3 -DCONFIG_MK3 || exit 1
echo "sizeof(cfg_t) = $(./build/cfg_cli_mk2 size) bytes"

# Drop comment lines and blank lines (CR-only included), keep everything else
# byte for byte -- CRLF endings and the leading "---" survive.
filter() { LC_ALL=C awk '{ l=$0; sub(/\r$/,"",l); if (l=="" || substr(l,1,1)=="#") next; print $0 }' "$1"; }

pass=0; fail=0

cmp_case() { # <name> <got> <want>
  if cmp -s "$2" "$3"; then
    echo "PASS  $1"
    pass=$((pass+1))
  else
    echo "FAIL  $1: $2 != $3"
    diff <(od -An -tx1 -c "$3" | head -40) <(od -An -tx1 -c "$2" | head -40) \
      | sed 's/^/      /' | head -20
    fail=$((fail+1))
  fi
}

# The legacy input is hand-written and versioned: it carries the forms an older
# config.yml (or a hand edit) can still contain, chosen so TODAY's cfg_load has
# exactly one possible answer for each.  See the file's own header.
LEGACY="$GOLDEN/cfg_legacy_ok.yml"
# Same idea, one step further out: every line of the ill-typed input hands a key
# the wrong kind of scalar.  cfg_load states a type per key, so each one has a
# single defined answer -- a number on a boolean key is true when non-zero, a
# boolean on a numeric key is 1 or 0, and anything else leaves the default.
ILLTYPED="$GOLDEN/cfg_illtyped.yml"
for f in "$LEGACY" "$ILLTYPED"; do
  [ -f "$f" ] && continue
  echo "!! missing $f (it is versioned input, not generated)" >&2
  exit 1
done

if [ "$MODE" = "--capture" ]; then
  echo "== capture =="
  for cfg in mk2 mk3; do
    CLI=./build/cfg_cli_$cfg
    $CLI save-default    "$GOLDEN/cfg_default.$cfg.yml"    || exit 1
    $CLI save-allchanged "$GOLDEN/cfg_allchanged.$cfg.yml" || exit 1
    $CLI load-dump "$GOLDEN/cfg_default.$cfg.yml"    "$GOLDEN/cfg_default.$cfg.bin"    || exit 1
    $CLI load-dump "$GOLDEN/cfg_allchanged.$cfg.yml" "$GOLDEN/cfg_allchanged.$cfg.bin" || exit 1
    $CLI load-dump "$LEGACY"   "$GOLDEN/cfg_legacy_ok.$cfg.bin" || exit 1
    $CLI load-dump "$ILLTYPED" "$GOLDEN/cfg_illtyped.$cfg.bin"  || exit 1
    echo "captured $cfg: $(ls -1 $GOLDEN/cfg_*.$cfg.* | tr '\n' ' ')"
  done
  echo "== captured -- READ THE DIFF before committing =="
  exit 0
fi

echo "== run =="
for cfg in mk2 mk3; do
  CLI=./build/cfg_cli_$cfg
  for which in default allchanged; do
    G_YML="$GOLDEN/cfg_$which.$cfg.yml"
    G_BIN="$GOLDEN/cfg_$which.$cfg.bin"
    if [ ! -f "$G_YML" ] || [ ! -f "$G_BIN" ]; then
      echo "FAIL  $which-$cfg: no golden ($G_YML / $G_BIN) -- run $0 --capture"
      fail=$((fail+1))
      continue
    fi

    # (a) what cfg_save() writes, compared filtered-against-filtered
    $CLI "save-$which" "$GEN/gen_$which.$cfg.yml" || { echo "FAIL  save-$which-$cfg: CLI error"; fail=$((fail+1)); continue; }
    filter "$GEN/gen_$which.$cfg.yml" > "$GEN/gen_$which.$cfg.flt"
    filter "$G_YML"                   > "$GEN/gold_$which.$cfg.flt"
    cmp_case "yml-$which-$cfg" "$GEN/gen_$which.$cfg.flt" "$GEN/gold_$which.$cfg.flt"

    # (b) what cfg_load() makes of the golden file, raw
    $CLI load-dump "$G_YML" "$GEN/load_$which.$cfg.bin" || { echo "FAIL  bin-$which-$cfg: CLI error"; fail=$((fail+1)); continue; }
    cmp_case "bin-$which-$cfg" "$GEN/load_$which.$cfg.bin" "$G_BIN"

    # (b') the filter is semantics-free: the same file without its comments has
    #      to load to the same bytes, or the .yml comparison above is a lie
    $CLI load-dump "$GEN/gold_$which.$cfg.flt" "$GEN/loadflt_$which.$cfg.bin" || { echo "FAIL  filter-neutral-$which-$cfg: CLI error"; fail=$((fail+1)); continue; }
    cmp_case "filter-neutral-$which-$cfg" "$GEN/loadflt_$which.$cfg.bin" "$G_BIN"

    # (c) load+save is a fixed point: the values a saved file carries must
    #     survive being read back and written out again
    $CLI roundtrip "$GEN/gold_$which.$cfg.flt" "$GEN/rt_$which.$cfg.yml" || { echo "FAIL  roundtrip-$which-$cfg: CLI error"; fail=$((fail+1)); continue; }
    filter "$GEN/rt_$which.$cfg.yml" > "$GEN/rt_$which.$cfg.flt"
    cmp_case "roundtrip-$which-$cfg" "$GEN/rt_$which.$cfg.flt" "$GEN/gold_$which.$cfg.flt"
  done

  # legacy / clamped forms, and ill-typed scalars -> one deterministic image each
  for which in legacy_ok illtyped; do
    case "$which" in
      legacy_ok) IN="$LEGACY";   NAME=legacy ;;
      illtyped)  IN="$ILLTYPED"; NAME=illtyped ;;
    esac
    G_BIN="$GOLDEN/cfg_$which.$cfg.bin"
    if [ ! -f "$G_BIN" ]; then
      echo "FAIL  bin-$NAME-$cfg: no golden ($G_BIN) -- run $0 --capture"
      fail=$((fail+1))
      continue
    fi
    $CLI load-dump "$IN" "$GEN/$which.$cfg.bin" || { echo "FAIL  bin-$NAME-$cfg: CLI error"; fail=$((fail+1)); continue; }
    cmp_case "bin-$NAME-$cfg" "$GEN/$which.$cfg.bin" "$G_BIN"
  done
done

echo "== summary: $pass pass, $fail fail =="
[ "$fail" -eq 0 ] || exit 1
