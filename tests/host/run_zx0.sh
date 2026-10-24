#!/usr/bin/env bash
# The embedded boot images re-packed as ZX0 (src/Makefile): the build expands each RLE image
# with src/utils/rled (the firmware's old in-memory RLE reader, byte for byte), compresses it
# with src/utils/zx0c -w 8192, and the firmware expands it with src/zx0.c through the 8 KB
# ptrcache.  This runs that same chain on the host and requires the firmware's decoder to give
# back exactly the bytes the old RLE reader sent:
#   - the SNES boot program (src/snesboot.h), always;
#   - the boot bitstream (fpga_mini.bi3, a build product: FPGA_MINI_BI3, default
#     ../../verilog/sd2snes_mini/fpga_mini.bi3), SKIPped when absent unless
#     FPGA_MINI_REQUIRED=1.  For it the window matters, so the same stream through a 4 KB ring
#     must come out WRONG -- otherwise this test could not see a window mismatch.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
B=build/zx0
mkdir -p "$B"
U=../../src/utils

$CC -O1 -Wall -Wextra -fsanitize=address,undefined "$U/rled.c" -o "$B/rled" || exit 1
$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I "$U" "$U/bootimg.c" -o "$B/bootimg" || exit 1
$CC -O2 "$U"/zx0/zx0.c "$U"/zx0/optimize.c "$U"/zx0/compress.c "$U"/zx0/memory.c -o "$B/zx0c" || exit 1
$CC -O1 -Wall -Wextra -fsanitize=address,undefined -I ../../src zx0_cli.c ../../src/zx0.c -o "$B/zx0_cli" || exit 1

rc=0
# chain <name> <rle file>: rled -> zx0c -w 8192 -> zx0.c (8 KB ring) == rled
chain() {
  local n=$1
  "$B/rled" "$2" "$B/$n.raw" || { echo "FAIL $n: rled"; rc=1; return 1; }
  "$B/zx0c" -f -w 8192 "$B/$n.raw" "$B/$n.zx0" >/dev/null || { echo "FAIL $n: zx0c"; rc=1; return 1; }
  "$B/zx0_cli" "$B/$n.zx0" "$B/$n.out" 8192 || { echo "FAIL $n: decoder"; rc=1; return 1; }
  if cmp -s "$B/$n.raw" "$B/$n.out"; then
    echo "ok   $n: $(wc -c < "$B/$n.raw" | tr -d ' ') B from a $(wc -c < "$B/$n.zx0" | tr -d ' ') B stream"
  else
    echo "FAIL $n: the decoder's output differs from the RLE reader's"; rc=1; return 1
  fi
}

"$B/bootimg" "$B/boot.rle" || exit 1
chain boot "$B/boot.rle"

BI3="${FPGA_MINI_BI3:-../../verilog/sd2snes_mini/fpga_mini.bi3}"
if [ -f "$BI3" ]; then
  if chain cfgware "$BI3"; then
    "$B/zx0_cli" "$B/cfgware.zx0" "$B/cfgware.4k" 4096 || true
    if cmp -s "$B/cfgware.raw" "$B/cfgware.4k"; then
      echo "FAIL cfgware: a 4 KB ring gives the same bytes, the window is not being exercised"; rc=1
    else
      echo "ok   cfgware: a 4 KB ring breaks it (the stream does use the 8 KB window)"
    fi
  fi
elif [ "${FPGA_MINI_REQUIRED:-0}" = 1 ]; then
  echo "FAIL cfgware: $BI3 not found"; rc=1
else
  echo "SKIP cfgware: $BI3 not found (set FPGA_MINI_BI3)"
fi
exit $rc
