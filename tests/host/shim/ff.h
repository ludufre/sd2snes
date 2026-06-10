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

typedef enum {
  FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY,
  FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME, FR_DENIED
} FRESULT;

typedef struct {
  FILE *fp;
  DWORD fptr;
  DWORD fsize;
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

#define AM_DIR 0x10
#define AM_HID 0x02
#define AM_SYS 0x04

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, DWORD ofs);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

#endif
