/* Host CLI over the REAL src/atari.c bankswitch detector.
 *
 * The detector is exercised end to end -- a26_id() itself, including the
 * streaming 512 B scan and its 2-byte carry-over -- by backing the fileops
 * globals it borrows with a stdio-based FatFs, the same trick tests/host/shim.c
 * plays for patch.c. Nothing of the scan or the decision table is transcribed
 * here; a wrong verdict is a bug in the firmware file, not in this harness.
 *
 * Usage: a26_detect_cli <image.a26>
 * Prints the firmware's own diagnostic line first (it goes to printf, i.e.
 * stdout on the host), then ONE stable result line:
 *
 *   SCHEME=<2K|4K|F8|F6|F4|-|n/a> SC=<0|1> ERROR=<OK|NOIMPL:<param>> SIZE=<bytes>
 *
 * SCHEME is "-" whenever ERROR is not OK (the scheme field is meaningless then)
 * and "n/a" when the file is not a .a26 by extension, i.e. when the detector
 * never ran. run_a26_detect.sh greps the line by its "SCHEME=" prefix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"   /* shim: pulls the system headers, neutralizes attributes */
#include "ff.h"       /* shim: the minimal FatFs surface */
#include "fileops.h"  /* shim: file_handle / file_res */
#include "memory.h"   /* shim: load_sram_offload prototype */
#include "cfg.h"      /* shim: the stand-in cfg_t */

#include "atari.h"    /* the real src/atari.h */

/* --- fileops/FatFs globals atari.c borrows -------------------------------
 * shim.c defines its own copies for the patch harness; this CLI does not link
 * it, so the few symbols the detector touches are provided right here. */
FIL     file_handle;
FRESULT file_res;

/* a26_id() folds CFG.a26_video_width into the CHIPFEAT word it hands the core.
   Left zero-initialized: that is the firmware default (160 px, the native TIA
   raster), and the picture width does not affect the bankswitch verdict. */
cfg_t CFG;

FRESULT f_lseek(FIL *fp, DWORD ofs) {
  if (!fp->fp) return FR_INT_ERR;
  if (ofs > fp->fsize) ofs = fp->fsize;   /* FatFs clamps read-only files */
  if (fseek(fp->fp, (long)ofs, SEEK_SET) != 0) return FR_DISK_ERR;
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  size_t got;
  if (!fp->fp) return FR_INT_ERR;
  got = fread(buff, 1, btr, fp->fp);
  fp->fptr += (DWORD)got;
  if (br) *br = (UINT)got;
  return FR_OK;
}

/* the player never exists on the host: a26_update_file() is compiled, not run */
FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
  (void)path; (void)fno;
  return FR_NO_FILE;
}

uint32_t load_sram_offload(uint8_t *filename, uint32_t base_addr, uint8_t flags) {
  (void)filename; (void)base_addr; (void)flags;
  return 0;
}

/* --- result formatting (host-side presentation only) --------------------- */
static const char *scheme_name(const a26_romprops_t *p) {
  /* not a .a26 at all: the scan never ran, so every other field is untouched
     zero -- say so instead of printing the zero scheme as if it were a verdict */
  if (!p->has_a26) return "n/a";
  if (p->error != 0) return "-";
  switch (p->scheme) {
    case A26_BS_2K: return "2K";
    case A26_BS_4K: return "4K";
    case A26_BS_F8: return "F8";
    case A26_BS_F6: return "F6";
    case A26_BS_F4: return "F4";
    default:        return "?";
  }
}

int main(int argc, char **argv) {
  const char *path;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <image.a26>\n", argv[0]);
    return 2;
  }
  path = argv[1];

  file_handle.fp = fopen(path, "rb");
  if (!file_handle.fp) {
    perror(path);
    return 2;
  }
  fseek(file_handle.fp, 0, SEEK_END);
  file_handle.fsize = (DWORD)ftell(file_handle.fp);
  fseek(file_handle.fp, 0, SEEK_SET);
  file_handle.fptr = 0;

  a26_id(&a26_romprops, (uint8_t *)path);

  printf("SCHEME=%s SC=%d ERROR=%s%s SIZE=%lu\n",
         scheme_name(&a26_romprops),
         a26_romprops.superchip,
         a26_romprops.error ? "NOIMPL:" : "OK",
         a26_romprops.error ? (const char *)a26_romprops.error_param : "",
         (unsigned long)a26_romprops.romsize_bytes);

  fclose(file_handle.fp);
  return a26_romprops.has_a26 ? 0 : 1;   /* 1 = not detected as a .a26 at all */
}
