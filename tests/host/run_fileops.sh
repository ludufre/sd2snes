#!/usr/bin/env bash
# Conformance test for the file manager engine (the card module src/modules/fileops/fopmod.c):
# the REAL module, compiled natively the way host builds of the firmware link it
# (-DMODULE_HOST_LINKED), over the REAL FatFs (src/ff.c + src/ccsbcs.c) on a RAM disk, with
# the REAL path_asset / path_asset_mkdir pulled out of src/fileops.c (as run_bucket.sh does)
# and a fake module_api_t: a flat 16 MB PSRAM, a fake clock, a stub font table.
#
# The RAM disk starts as a FAT32 image made by mkfs.vfat (1 KB clusters, so a byte count and
# a cluster count disagree and the free-space check is really tested).  Every case starts
# from that pristine image, builds its tree through FatFs, runs the module to the end
# (answering its questions like the menu would) and checks the tree it left.
#
# Host build quirks:
#   - integer.h is replaced (build/fileops/integer.h): FatFs' DWORD is "unsigned long", 64
#     bits on the host, and the module and FatFs must agree on FIL / DIR / FILINFO.
#   - _WORD_ACCESS is turned off in the build copy of ffconf.h: unaligned word loads are
#     fine on the MCU and UBSan errors on the host.
#   - FatFs warns over UART about plain reads above 512 bytes; the shim uart.h silences it.
#
# FOP_SPARE=<bytes> sizes the module's spare RAM (default: 3072, the host firmware's).  The
# runner also does a pass at 2400 (what is left behind an 8.5 KB image on the card) and at
# the 1792 minimum.  FOP_MUTANT=<name> applies one deliberate defect to the module copy; the
# suite must FAIL on every one of them (the list is at the bottom).
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh
B="${FOP_BUILD:-build/fileops}"
mkdir -p "$B"
find "$B" -type f ! -name disk.img -delete

S=../../src
for f in ff.c ff.h ffconf.h diskio.h module.h; do cp "$S/$f" "$B/$f" || exit 1; done
sed 's|"\.\./ff\.h"|"ff.h"|' "$S/ccsbcs.c" > "$B/ccsbcs.c"
sed -i.bak 's/^#define _WORD_ACCESS[[:space:]]*1/#define _WORD_ACCESS 0/' "$B/ffconf.h"
grep -q '^#define _WORD_ACCESS 0' "$B/ffconf.h" || { echo "!! could not turn _WORD_ACCESS off" >&2; exit 1; }
cat > "$B/integer.h" <<'EOF'
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
EOF

# path_bucket2 .. check_or_create_folder, the REAL ones
{
  printf '#include <string.h>\n#include "config.h"\n#include "uart.h"\n#include "ff.h"\n#include "util.h"\n'
  printf 'FRESULT check_or_create_folder(TCHAR *dir);\n'
  awk '/^void path_bucket2/{p=1} p && /^#ifdef CONFIG_UART_DEBUG/{exit} p' "$S/fileops.c"
} > "$B/asset_under_test.c"
grep -q "^FRESULT check_or_create_folder(TCHAR \*dir) {" "$B/asset_under_test.c" \
  && grep -q "^int path_asset" "$B/asset_under_test.c" \
  || { echo "!! extraction failed -- did path_asset / check_or_create_folder move in fileops.c?" >&2; exit 1; }

cp "$S/modules/fileops/fopmod.c" "$B/fopmod_under_test.c" || exit 1
M="$B/fopmod_under_test.c"
case "${FOP_MUTANT:-}" in
  "") ;;
  nointoself)  perl -0pi -e 's/under\(m->q, m->p\)\)/0)/' "$M" ;;
  noround)     perl -0pi -e 's/ \+ \(\(size % m->clus\) \? 1 : 0\)//' "$M" ;;
  nospacechk)  perl -0pi -e 's/if\(m->need > m->nfree\)/if(0)/' "$M" ;;
  norecount)   perl -0pi -e 's/if\(!m->recount\) \{/if(0) {/' "$M" ;;
  nounlink)    perl -0pi -e 's/(static void copy_abort.*?\n.*?\n.*?\n)  m->api->f_unlink\(m->q\);\n/$1/s' "$M" ;;
  noallkind)   perl -0pi -e 's/if\(m->answer_all && kind <= 3\) m->all\[kind - 1\] = a;/if(m->answer_all) { m->all[0] = m->all[1] = m->all[2] = a; }/' "$M" ;;
  mixedrepl)   perl -0pi -e 's/ && kind != FM_ASK_MIXED//' "$M" ;;
  nosame)      perl -0pi -e 's/m->same = eqi\(m->p, m->q\);/m->same = 0;/' "$M" ;;
  nomergedel)  perl -0pi -e 's/if\(fl & \(FRM_MERGECUT \| FRM_RMDIR\)\) r = api->f_unlink/if(fl \& FRM_RMDIR) r = api->f_unlink/' "$M" ;;
  nocaseren)   perl -0pi -e 's/if\(eqi\(m->p, m->q\)\) \{\n    \/\*/if(0) {\n    \/*/' "$M" ;;
  stateany)    perl -0pi -e 's/return isdig\(r\[0\]\) && isdig\(r\[1\]\) && eqi\(r \+ 2, ".state"\);/return (isdig(r[0]) \&\& eqi(r + 1, ".state")) || (isdig(r[0]) \&\& isdig(r[1]) \&\& eqi(r + 2, ".state"));/' "$M" ;;
  copysaves)   perl -0pi -e 's/i < \(copy \? 2u : 5u\)/i < 5u/' "$M" ;;
  noclipchk)   perl -0pi -e 's/ \|\| clip_has\(m, leaf\(m->a\)\)/ || (0 \&\& clip_has(m, leaf(m->a)))/' "$M" ;;
  nopatchmap)  perl -0pi -e 's/np = q \+ qd \+ 1;/np = patch;/' "$M" ;;
  nolists)     perl -0pi -e 's/  lists_map\(m, m->isdir\);\n//' "$M" ;;
  nocrlf)      perl -0pi -e "s/if\(c == '\\\\r' && r.pos < limit && vpeek\(m, &r\) == '\\\\n'\) vadv\(&r\);//" "$M" ;;
  nobom)       perl -0pi -e 's/r = s; \} \}/} }/' "$M" ;;
  nocp1252)    perl -0pi -e 's/cp = utf \? \(uint32_t\)c : cp1252\(\(uint8_t\)c\);/cp = (uint32_t)c;/' "$M" ;;
  nowrapsp)    perl -0pi -e 's/if\(v->sp >= FM_VIEW_COLS - 16\)/if(0)/' "$M" ;;
  notab)       perl -0pi -e 's/for\(n = 4 - \(v.col & 3\); n; n--\)/for(n = 1; n; n--)/' "$M" ;;
  nobrk)       perl -0pi -e 's/if\(brk > base\) \{ end = brk; v.lines = brk_lines; goto done; \}//' "$M" ;;
  drainshort)  perl -0pi -e 's/if\(m->st_drain < m->st_fill\) return 0;/if(m->st_drain + m->bufsz < m->st_fill) return 0;/' "$M" ;;
  stageover)   perl -0pi -e 's/m->st_fill \+ m->bufsz > FOP_STAGE_SIZE/m->st_fill > FOP_STAGE_SIZE/' "$M" ;;
  countread)   perl -0pi -e 's/(    m->st_fill \+= br;\n)/$1    m->h.bytes_done += br;\n/; s/    m->h.bytes_done \+= n;\n//' "$M" ;;
  nodrainreset) perl -0pi -e 's/  m->draining = 0;\n  m->st_fill = 0;\n  return m->src_eof;/  m->draining = 0;\n  return m->src_eof;/' "$M" ;;
  ctl01)       perl -0pi -e 's/if\(out\[i\] < 2\) out\[i\] = .\?.;/(void)out[i];/; s/out\[0\] = \(cp < 0x20 \|\| cp == 0x7f\) \? .\?. : \(uint8_t\)cp;/out[0] = (uint8_t)cp;/' "$M" ;;
  rm_nosys)    perl -0pi -e 's/if\(!rm_allowed\(m->p\)\)/if(!rm_allowed(m->p) \&\& 0)/' "$M" ;;
  rm_noroots)  perl -0pi -e 's/      if\(under\(p \+ 9, r\)\) return 1;/      (void)r;/' "$M" ;;
  rm_nodirdel) perl -0pi -e 's/if\(fl & \(FRM_MERGECUT \| FRM_RMDIR\)\)/if(fl \& FRM_MERGECUT)/' "$M" ;;
  rm_noask)    perl -0pi -e 's/    m->h.ask = FM_ASK_RMDIR;\n    m->phase = PH_ASK;\n    return;/    rm_go(m);\n    return;/' "$M" ;;
  rm_nofatal)  perl -0pi -e 's/if\(r == FR_DISK_ERR \|\|/if(r == FR_INVALID_OBJECT ||/' "$M" ;;
  rm_errlast)  perl -0pi -e 's/if\(!isdir \|\| !m->h.err\) m->h.err =/m->h.err =/' "$M" ;;
  rm_noclip)   perl -0pi -e 's/  m->h.flags \|= FM_FL_UNCLIP;\n//' "$M" ;;
  rm_cliprest) perl -0pi -e 's/ \|\| !rm_covered\(m\)\) return;/ || (!rm_covered(m) \&\& 0)) return;/' "$M" ;;
  rm_nolists)  perl -0pi -e 's/    if\(!isdir\) m->lists = 1; /    /' "$M" ;;
  rm_sizekb)   perl -0pi -e 's/    m->rm_rest &= 1023;\n/    m->rm_rest = 0;\n/' "$M" ;;
  rm_nodots)   perl -0pi -e 's/if\(\(lf\[0\] == .\.. && \(!lf\[1\] \|\| \(lf\[1\] == .\.. && !lf\[2\]\)\)\) \|\| /if(/' "$M" ;;
  rm_selsys)   perl -0pi -e 's/if\(m->scan && \(t == TYPE_FILE \|\| !rm_allowed\(m->p\)\)\)/if(0 \&\& m->scan \&\& (t == TYPE_FILE || !rm_allowed(m->p)))/' "$M" ;;
  rm_selclip)  perl -0pi -e 's/if\(pjoin\(m->p, m->slen, m->b\) >= 0\) r = under\(m->l, m->p\);/(void)0;/' "$M" ;;
  rm_selhide)  perl -0pi -e 's/if\(out\[k\] == 1\) out\[k\] = 0x2e;/(void)k;/' "$M" ;;
  *) echo "unknown mutant $FOP_MUTANT" >&2; exit 2 ;;
esac
if [ -n "${FOP_MUTANT:-}" ] && cmp -s "$S/modules/fileops/fopmod.c" "$M"; then
  echo "mutant $FOP_MUTANT did not change the source" >&2; exit 2
fi

if [ ! -s "$B/disk.img" ]; then
  command -v mkfs.vfat >/dev/null || { echo "!! mkfs.vfat (dosfstools) is needed to make the RAM disk" >&2; exit 1; }
  rm -f "$B/disk.img"
  mkfs.vfat -C -F 32 -S 512 -s 2 -n FMTEST "$B/disk.img" 81920 >/dev/null || exit 1
fi

$CC -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fsanitize=address,undefined -DMODULE_HOST_LINKED \
    -I "$B" -I shim_fileops -I "$S" \
    fileops_cli.c "$M" "$B/ff.c" "$B/ccsbcs.c" "$B/asset_under_test.c" "$S/strutil.c" hexup_shim.c \
    -o "$B/fileops_cli" 2> "$B/cc.log" || { cat "$B/cc.log"; exit 1; }
grep -v "ff.c\|ccsbcs.c\|asset_under_test.c" "$B/cc.log" | grep -q "warning" && { cat "$B/cc.log"; exit 1; }

if [ -n "${FOP_SPARE:-}" ] || [ -n "${FOP_MUTANT:-}" ]; then
  FOP_DISK="$B/disk.img" exec "./$B/fileops_cli"
fi
rc=0
for sp in 3072 2400 1792; do
  echo "== spare $sp"
  FOP_SPARE=$sp FOP_DISK="$B/disk.img" "./$B/fileops_cli" || rc=1
done
exit $rc

# Mutants (each must FAIL the suite): nointoself noround nospacechk norecount nounlink noallkind
# mixedrepl nosame nomergedel nocaseren stateany copysaves noclipchk nopatchmap nolists
# nocrlf nobom nocp1252 nowrapsp notab nobrk ctl01 drainshort stageover countread nodrainreset
# rm_nosys rm_noroots rm_nodirdel rm_noask rm_nofatal rm_errlast rm_noclip rm_cliprest rm_nolists
# rm_sizekb rm_nodots rm_selsys rm_selclip rm_selhide
