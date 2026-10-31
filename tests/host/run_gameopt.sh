#!/usr/bin/env bash
# Host test of the per-game settings file (src/gameopt.c): the <stem>.yml in saves/ with
# config.yml's keys, its two slot keys, the .slot of before, unknown lines kept, CFG
# overlaid / swapped for cfg_save / restored, the live change, GAMEOPT_BLK.  Compiled
# against the REAL src/gameopt.c + src/cfg.c + src/yaml.c, with cfg_cli.c's FatFs string
# functions (CRLF out, '\r' stripped in) and a card re-rooted under build/.
#
# Built twice: gameopt_list flags Mk.III-only options, and config.yml itself has a key the
# Mk.II firmware does not know (SGBSprIncrease).
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
mkdir -p build

echo "== build =="
# Copies in build/ so the shim headers win over the real ones next to the sources (see
# run_cfg.sh); the two S-RTC converters cfg.c calls are extracted from the real rtc.c.
cp ../../src/cfg.c     build/cfg_under_test.c     || exit 1
cp ../../src/yaml.c    build/yaml_under_test.c    || exit 1
cp ../../src/gameopt.c build/gameopt_under_test.c || exit 1
cp ../../src/yaml.h    build/yaml.h               || exit 1
cp ../../src/gameopt.h build/gameopt.h            || exit 1
cp ../../src/scratch.c build/scratch_under_test.c || exit 1   # the LEAF the rewrite keeps its lines in
{
  printf '#include <stdint.h>\n'
  awk '/^void bcdtime2srtctime/,/^}/'  ../../src/lpc175x/rtc.c
  awk '/^uint64_t srtctime2bcdtime/,/^}/' ../../src/lpc175x/rtc.c
} > build/rtc_under_test.c
grep -E '^#define (FEAT_2100|FEAT_2100_LIMIT\(x\)|FEAT_CC92|FEAT_PF94)[[:space:]]' ../../src/fpga_spi.h > build/gameopt_feat.h
[ "$(wc -l < build/gameopt_feat.h)" -eq 4 ] || { echo "!! FEAT_2100 / _LIMIT / FEAT_CC92 / FEAT_PF94 not found in src/fpga_spi.h" >&2; exit 1; }

MAPDEFS=
for sym in SRAM_FAVORITEGAMES_ADDR SRAM_GAMEINFO_ADDR SRAM_MENU_CFG_ADDR SRAM_CFG_PLAYER_REPEAT_ADDR \
           SRAM_LASTGAME_DIR_ADDR SRAM_LASTGAME_FILE_ADDR SRAM_GAMEOPT_ADDR SRAM_CHEAT_FLAGS_ADDR; do
  val=$(grep -hE "^#define[[:space:]]+$sym[[:space:]]" ../../src/memmap.h ../../src/memory.h 2>/dev/null | head -1 | awk '{print $3}')
  case "$val" in
    0[xX][0-9a-fA-F]*|\(0[xX][0-9a-fA-F]*) ;;
    *) echo "!! $sym not found as a plain hex literal in src/memmap.h (got '$val')" >&2; exit 1 ;;
  esac
  MAPDEFS="$MAPDEFS -D$sym=$val"
done

# shim_gameopt first (smc.h / fpga_spi.h), then shim_cfg (the real cfg.h and a config.h
# that keeps cfg_t packed), then shim.  cfg_cli.c brings the card and the sram_* array;
# its main is renamed out of the way.
build_one() { # <suffix> [extra cflags]
  local inc="-I build -I shim_gameopt -I shim_cfg -I shim -I ../../src"
  $CC -O1 -g -Wall -Wno-format -fsanitize=address,undefined ${2:-} $MAPDEFS $inc \
      -Dmain=cfg_cli_main -c cfg_cli.c -o "build/gameopt_cfgcli_$1.o" || return 1
  $CC -O1 -g -Wall -Wno-format -fsanitize=address,undefined ${2:-} $MAPDEFS $inc \
      gameopt_cli.c build/gameopt_under_test.c build/cfg_under_test.c build/yaml_under_test.c \
      build/rtc_under_test.c ../../src/strutil.c build/scratch_under_test.c "build/gameopt_cfgcli_$1.o" \
      -o "build/gameopt_cli_$1"
}
build_one mk2 -DCONFIG_MK2 || exit 1
build_one mk3 -DCONFIG_MK3 || exit 1

rc=0
for b in mk2 mk3; do
  echo "== $b =="
  rm -rf build/sd2snes
  out=$(cd build/.. && ./build/gameopt_cli_$b 2>&1); r=$?
  echo "$out"
  if [ $r -ne 0 ] || san_report "$out"; then rc=1; fi
done
[ $rc -eq 0 ] && echo "ALL PASS" || echo "FAILED"
exit $rc
