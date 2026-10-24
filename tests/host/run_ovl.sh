#!/usr/bin/env bash
# Card overlay loader (src/ovl.c, REAL) on the REAL FatFs and CRC-32, with images stamped by
# the REAL packer (src/ovl/ovlpack.py): see ovl_cli.c.  Then three mutants of the loader
# (no CRC / build id / size check, no ovl_depth / live module guard, id 0 taken as loaded)
# must each fail the run.  Two overlays: one image in the block at a time, an image under
# another overlay's name is refused (the magic carries the overlay number).
#
# Host build quirks as in run_fprintf.sh: integer.h at the MCU widths, _WORD_ACCESS off,
# _USE_MKFS on in the build copy of ffconf.h.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
. ./sanitizers.sh
B=build/ovl
mkdir -p "$B"
rm -f "$B"/*.o "$B/cc.log"
S=../../src
for f in ff.c ff.h ffconf.h diskio.h crc32.c crc32.h ovl.c ovl.h; do cp "$S/$f" "$B/$f" || exit 1; done
sed 's|"\.\./ff\.h"|"ff.h"|' "$S/ccsbcs.c" > "$B/ccsbcs.c"
sed -i.bak -e 's/^#define _WORD_ACCESS[[:space:]]*1/#define _WORD_ACCESS 0/' \
           -e 's/^#define[[:space:]]*_USE_MKFS[[:space:]]*0/#define _USE_MKFS 1/' "$B/ffconf.h"
grep -q '^#define _WORD_ACCESS 0' "$B/ffconf.h" && grep -q '^#define _USE_MKFS 1' "$B/ffconf.h" \
  || { echo "!! could not adjust ffconf.h (_WORD_ACCESS / _USE_MKFS)" >&2; exit 1; }
cat > "$B/integer.h" <<'EOI'
/* host: FatFs' integer types at their MCU widths */
#ifndef _INTEGER
#define _INTEGER
#include <stdint.h>
typedef int INT;
typedef unsigned int UINT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef unsigned char BYTE;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
#endif
EOI

# the Mk.II/Mk.III module block: ptrcache (8 KB) + the free end of the AHB SRAM
BLOCK=11344
FIL_RESERVE=$(sed -n 's/^#define OVL_FIL_RESERVE[[:space:]]*\([0-9]*\).*/\1/p' "$S/ovl.h")
DEFS="-DBLOCK_BYTES=$BLOCK -DOVL_END=((uintptr_t)ptrcache+BLOCK_BYTES)"

# images: a header + entries + pseudo-random code, stamped by the packer; a fake fw.hdr
"$PY" - "$B" "$BLOCK" "$FIL_RESERVE" <<'EOP' || exit 1
import os, random, struct, sys
b, block, filres = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
random.seed(7)
hdr = bytearray(256)
hdr[0:4] = b'FXPK'
struct.pack_into('<I', hdr, 12, 0x5eed1234)
open(os.path.join(b, 'fw.hdr'), 'wb').write(hdr)
struct.pack_into('<I', hdr, 12, 0x5eed1235)     # another firmware build
open(os.path.join(b, 'fw2.hdr'), 'wb').write(hdr)
def raw(n, name, magic=0x31564f53):          # "SOV1"
    d = bytearray(random.getrandbits(8) for _ in range(n))
    struct.pack_into('<IIII', d, 0, magic, 0, 0, 0xdeadbeef)
    open(os.path.join(b, name), 'wb').write(d)
raw(5168, 'good.raw')
raw(3072, 'good2.raw', 0x32564f53)            # "SOV2": the second overlay
# the loader's FIL is sizeof(FIL) (<= the reserve - 8) below the end, 8-aligned: an image up to
# block - reserve always fits, block - 16 never does
raw(block - filres, 'fit.raw')
raw(block - 16, 'over.raw')
EOP
for f in good good2 fit over; do
  "$PY" "$S/ovl/ovlpack.py" "$B/$f.raw" "$B/fw.hdr" "$B/$f.ovl" || exit 1
done
"$PY" "$S/ovl/ovlpack.py" "$B/good.raw" "$B/fw2.hdr" "$B/other.ovl" || exit 1

build() {   # $1 = ovl.c to use, $2 = binary
  for c in ovl_cli.c "$B/ff.c" "$B/ccsbcs.c" "$B/crc32.c"; do
    $CC -O1 -g -Wall -Wno-unused-parameter -fsanitize=address,undefined $DEFS \
        -I "$B" -I shim_ovl -I shim_fileops -I "$S" -c "$c" -o "$B/$(basename "${c%.c}").o" 2>> "$B/cc.log" \
      || { cat "$B/cc.log"; exit 1; }
  done
  $CC -O1 -g -Wall -fsanitize=address,undefined $DEFS -I "$B" -I shim_ovl -I shim_fileops -I "$S" \
      -c "$1" -o "$B/ovl.o" 2>> "$B/cc.log" || { cat "$B/cc.log"; exit 1; }
  $CC -fsanitize=address,undefined "$B/ovl_cli.o" "$B/ff.o" "$B/ccsbcs.o" "$B/crc32.o" "$B/ovl.o" \
      -o "$2" || exit 1
}
ARGS="$B/good.ovl $B/fw.hdr $B/fit.ovl $B/over.ovl $B/other.ovl $B/good2.ovl"
build "$B/ovl.c" "$B/ovl_cli"
"./$B/ovl_cli" $ARGS || exit 1

rc=0
mutant() {  # $1 = name, $2 = sed expression on ovl.c
  sed -e "$2" "$B/ovl.c" > "$B/ovl_m.c"
  cmp -s "$B/ovl.c" "$B/ovl_m.c" && { echo "!! mutant $1 did not apply"; rc=1; return; }
  build "$B/ovl_m.c" "$B/ovl_m"
  if "./$B/ovl_m" $ARGS > "$B/mutant.log" 2>&1; then
    echo "!! mutant $1 survived"; rc=1
  else
    echo "ovl: mutant $1 caught ($(grep -c FAIL "$B/mutant.log") failures)"
  fi
}
mutant no-crc   's/|| crc32_finalize(crc) != h->crc//'
mutant no-build 's/|| h->build != OVL_BUILD//'
mutant no-depth 's/if(ovl_depth || /if(/'
mutant no-live  's/ || module_live()//'
mutant no-size  's/ || size > (uintptr_t)OVL_FIL - (uintptr_t)img//'
mutant id-zero  's/if(!id || id > OVL_COUNT) return 0;//'
mutant why-missing 's/  ovl_err = OVL_ERR_REFUSED;//'
mutant why-stale   's/  ovl_err = 0;//'
mutant magic-id 's/OVL_MAGIC(id)/OVL_MAGIC(1)/'
mutant no-memory  's/if(ovl_fail\[id - 1\]) {/if(0) {/'
mutant no-recheck 's/ovl_fail\[i\] = 0;/;/'
exit $rc
