/* Host shim for src/ff.h: the minimal FatFs surface patch.c touches,
 * backed by stdio. f_lseek clamps to fsize like FatFs does for files opened
 * read-only -- patch.c's metadata-skip semantics depend on that. */
#ifndef HOST_FF_H
#define HOST_FF_H

#include <stdint.h>
#include <stdio.h>

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef char TCHAR;

/* Same numeric values as the real ff.h: file_res is also printed as a number. */
typedef enum {
  FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY,
  FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME, FR_DENIED,
  FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED, FR_INVALID_DRIVE,
  FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED, FR_TIMEOUT,
  FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES,
  FR_INVALID_PARAMETER
} FRESULT;

typedef struct {
  FILE *fp;
  DWORD fptr;
  DWORD fsize;
  BYTE  err;    /* f_error(fp): never set on the host, but the macro must compile */
} FIL;

typedef struct { int dummy; } DIR;

typedef struct {
  BYTE  fattrib;
  TCHAR fname[13];
  TCHAR *lfname;
  UINT  lfsize;
} FILINFO;

#define FA_READ          0x01
#define FA_OPEN_EXISTING 0x00
#define FA_WRITE         0x02
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10

#define AM_DIR 0x10
#define AM_HID 0x02
#define AM_SYS 0x04

#define f_eof(fp)   ((int)((fp)->fptr == (fp)->fsize))
#define f_error(fp) ((fp)->err)
#define f_tell(fp)  ((fp)->fptr)
#define f_size(fp)  ((fp)->fsize)

#ifndef EOF
#define EOF (-1)
#endif

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, DWORD ofs);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_close(FIL *fp);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new);

/* String functions (_USE_STRFUNC == 2): f_putc/f_puts/f_printf translate '\n'
   to CRLF and f_gets strips '\r'.  The host side has to keep that, or a golden
   config.yml stops matching the card. */
TCHAR *f_gets(TCHAR *buff, int len, FIL *fp);
int    f_putc(TCHAR c, FIL *fp);
int    f_puts(const TCHAR *str, FIL *fp);
int    f_printf(FIL *fp, const TCHAR *fmt, ...);

#endif
