#!/usr/bin/env bash
# Golden gate for the config.yml serializer (src/cfg.c + src/yaml.c): pins both
# directions byte for byte -- what cfg_save() writes, and what cfg_load() makes
# of the fixtures in golden/.
#
# MODES
#   --check       (default) rebuild and compare against golden/.  Exit 0/1.
#   --capture     (re)write the golden .bin files from the current source.  It
#                 deliberately does NOT touch the .yml goldens.
#   --capture-yml [--force-yml[=<fixture>]]
#                 write the golden .yml files as well.  An existing one is kept
#                 unless --force-yml is passed too; a MISSING one is created,
#                 which is how a new fixture is added.  --force-yml=<fixture>
#                 forces only that one; bare --force-yml re-captures every anchor.
#
# - Goldens are per config: SGBSprIncrease is the one key inside an
#   "#ifdef CONFIG_MK3" in cfg_items[], so mk2 and mk3 genuinely disagree about
#   the file.
# - The .yml cases compare FILTERED against filtered (comment and blank lines
#   dropped, everything else byte for byte), so what is pinned is the set of
#   "Key: value" lines and their order.  The .bin cases are compared raw.
# - The .yml goldens are ANCHORS carrying a comment banner this code never wrote;
#   do NOT re-capture them.  A genuinely new or renamed key is added BY HAND.
# - Fixtures: default (CFG_DEFAULT), allchanged (every serialized field off its
#   default), alternating (every boolean set from the parity of its line in
#   cfg_items[]) and altoffset (from the parity of its byte offset in cfg_t).
#   The last two break the symmetry allchanged cannot: with every boolean equal,
#   swapping two field names between CFGI lines changes nothing at all.
#
# WHAT IS COMPARED (per config, mk2 and mk3)
#   yml-<fixture>      save-<fixture>          vs golden .yml   (filtered)
#   bin-<fixture>      load-dump(golden .yml)  vs golden .bin   (raw)
#   bin-legacy/illtyped/clamped/over           vs golden .bin   (raw)
#   filter-neutral-*   load-dump(filtered)     vs golden .bin   (raw)
#   roundtrip-*        load(filtered)+save     vs the filtered golden
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
MODE="${1:---check}"
FORCE_YML="${2:-}"
# The golden corpus is not part of this tree; point CFG_GOLDEN at it.
GOLDEN="${CFG_GOLDEN:-}"
GEN=build/cfg

case "$MODE" in
  --capture|--capture-yml|--check) ;;
  *) echo "usage: $0 [--check|--capture|--capture-yml [--force-yml[=<fixture>]]]" >&2; exit 2 ;;
esac
case "$FORCE_YML" in
  '') ;;
  --force-yml|--force-yml=*)
    [ "$MODE" = --capture-yml ] || { echo "--force-yml only applies to --capture-yml" >&2; exit 2; } ;;
  *) echo "usage: $0 [--check|--capture|--capture-yml [--force-yml[=<fixture>]]]" >&2; exit 2 ;;
esac
# "" = force nothing, "*" = force every anchor, otherwise the one fixture named.
FORCE_ONLY=${FORCE_YML#--force-yml}
case "$FORCE_YML" in
  '')            FORCE_ONLY= ;;
  --force-yml)   FORCE_ONLY='*' ;;
  *)             FORCE_ONLY=${FORCE_ONLY#=} ;;
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

# Every BSRAM address cfg.c touches comes out of the REAL map header, never out
# of a copy next to the tests that could drift while every case still passes.  A
# missing symbol fails the build here rather than compiling against a stale one.
MAPDEFS=
for sym in SRAM_FAVORITEGAMES_ADDR SRAM_GAMEINFO_ADDR \
           SRAM_MENU_CFG_ADDR SRAM_LASTGAME_DIR_ADDR SRAM_LASTGAME_FILE_ADDR; do
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

# Hand-written, versioned inputs.  cfg_clamped.yml is split out of the legacy
# file because yaml_get_value takes the FIRST occurrence of a key, and that file
# already spells three of the clamped keys another way.  See each file's header.
LEGACY="$GOLDEN/cfg_legacy_ok.yml"
ILLTYPED="$GOLDEN/cfg_illtyped.yml"
CLAMPED="$GOLDEN/cfg_clamped.yml"
OVER="$GOLDEN/cfg_over.yml"
for f in "$LEGACY" "$ILLTYPED" "$CLAMPED" "$OVER"; do
  [ -f "$f" ] && continue
  echo "!! missing $f (it is versioned input, not generated)" >&2
  exit 1
done

# An existing .yml golden is an ANCHOR: regenerating it would replace the
# expectation with the current output.  Creating a MISSING one is the opposite.
# --force-yml takes a fixture name so overriding one does not re-capture the rest.
yml_forced() { # <path>
  case "$FORCE_ONLY" in
    '')  return 1 ;;
    '*') return 0 ;;
  esac
  case "$(basename "$1")" in
    cfg_"$FORCE_ONLY".*) return 0 ;;
  esac
  return 1
}

capture_yml() { # <cli mode> <path>
  if [ -f "$2" ] && ! yml_forced "$2"; then
    echo "keep   $2 (anchor -- --capture-yml never overwrites one; --force-yml=<fixture> if you must)"
    return 0
  fi
  $CLI "$1" "$2"
}

if [ "$MODE" = "--capture" ] || [ "$MODE" = "--capture-yml" ]; then
  echo "== capture =="
  for cfg in mk2 mk3; do
    CLI=./build/cfg_cli_$cfg
    if [ "$MODE" = "--capture-yml" ]; then
      capture_yml save-default     "$GOLDEN/cfg_default.$cfg.yml"     || exit 1
      capture_yml save-allchanged  "$GOLDEN/cfg_allchanged.$cfg.yml"  || exit 1
      capture_yml save-alternating "$GOLDEN/cfg_alternating.$cfg.yml" || exit 1
      capture_yml save-altoffset   "$GOLDEN/cfg_altoffset.$cfg.yml"   || exit 1
    fi
    $CLI load-dump "$GOLDEN/cfg_default.$cfg.yml"     "$GOLDEN/cfg_default.$cfg.bin"     || exit 1
    $CLI load-dump "$GOLDEN/cfg_allchanged.$cfg.yml"  "$GOLDEN/cfg_allchanged.$cfg.bin"  || exit 1
    $CLI load-dump "$GOLDEN/cfg_alternating.$cfg.yml" "$GOLDEN/cfg_alternating.$cfg.bin" || exit 1
    $CLI load-dump "$GOLDEN/cfg_altoffset.$cfg.yml"   "$GOLDEN/cfg_altoffset.$cfg.bin"   || exit 1
    $CLI load-dump "$LEGACY"   "$GOLDEN/cfg_legacy_ok.$cfg.bin" || exit 1
    $CLI load-dump "$ILLTYPED" "$GOLDEN/cfg_illtyped.$cfg.bin"  || exit 1
    $CLI load-dump "$CLAMPED"  "$GOLDEN/cfg_clamped.$cfg.bin"   || exit 1
    $CLI load-dump "$OVER"     "$GOLDEN/cfg_over.$cfg.bin"      || exit 1
    echo "captured $cfg: $(ls -1 $GOLDEN/cfg_*.$cfg.bin | tr '\n' ' ')  (.yml anchors untouched)"
  done
  echo "== captured -- READ THE DIFF before committing =="
  exit 0
fi

echo "== run =="
for cfg in mk2 mk3; do
  CLI=./build/cfg_cli_$cfg
  for which in default allchanged alternating altoffset; do
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

  # legacy / out-of-range forms, and ill-typed scalars -> one deterministic image each
  for which in legacy_ok illtyped clamped over; do
    case "$which" in
      legacy_ok) IN="$LEGACY";   NAME=legacy ;;
      illtyped)  IN="$ILLTYPED"; NAME=illtyped ;;
      clamped)   IN="$CLAMPED";  NAME=clamped ;;
      over)      IN="$OVER";     NAME=over ;;
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
