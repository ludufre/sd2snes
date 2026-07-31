/* Host shim for src/fileops.h: the globals/functions patch.c uses,
 * implemented in shim.c over stdio. */
#ifndef HOST_FILEOPS_H
#define HOST_FILEOPS_H

#include <stdint.h>
#include "ff.h"

extern BYTE    file_buf[512];
extern FIL     file_handle;
extern FRESULT file_res;
extern uint8_t file_lfn[258];

int     path_asset(char *buf, int buflen, const char *root, const char *src, const char *ext);
FRESULT path_asset_mkdir(char *path);

void file_open(const uint8_t *filename, BYTE flags);
void file_close(void);

#endif
