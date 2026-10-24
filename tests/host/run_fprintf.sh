#!/usr/bin/env bash
# f_printf over the firmware's formatter (src/ff.c + src/printf.c, both REAL), on a RAM disk
# that FatFs formats itself: see fprintf_cli.c.
#
# Host build quirks (as in run_fileops.sh): integer.h at the MCU widths, _WORD_ACCESS off
# (UBSan), and _USE_MKFS on in the build copy of ffconf.h so no mkfs tool is needed.
# printf.c's snprintf/vsnprintf are renamed so they do not take over the host libc's.
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh
B=build/fprintf
mkdir -p "$B"
rm -f "$B"/*.o "$B/cc.log"
S=../../src
for f in ff.c ff.h ffconf.h diskio.h printf.c; do cp "$S/$f" "$B/$f" || exit 1; done
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
REN="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Dsnprintf=fw_snprintf -Dvsnprintf=fw_vsnprintf"
for c in fprintf_cli.c "$B/ff.c" "$B/ccsbcs.c"; do
  $CC -O1 -g -Wall -Wno-unused-parameter -Wno-format -fsanitize=address,undefined \
      -I "$B" -I shim_fileops -I "$S" -c "$c" -o "$B/$(basename "${c%.c}").o" 2>> "$B/cc.log" \
    || { cat "$B/cc.log"; exit 1; }
done
$CC -O1 -g -Wall -fsanitize=address,undefined $REN -I shim_fileops -c "$B/printf.c" -o "$B/printf.o" \
    2>> "$B/cc.log" || { cat "$B/cc.log"; exit 1; }
$CC -fsanitize=address,undefined "$B/fprintf_cli.o" "$B/ff.o" "$B/ccsbcs.o" "$B/printf.o" -o "$B/fprintf_cli" || exit 1
exec "./$B/fprintf_cli"
