/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   fileops.h: simple file access functions
*/

#ifndef FILEOPS_H
#define FILEOPS_H
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include "config.h"   /* CONFIG_UART_DEBUG gates print_fresult() below */
#include "ff.h"

enum filestates { FILE_OK=0, FILE_ERR, FILE_EOF };

extern BYTE file_buf[512] __attribute__((aligned(4)));
extern FATFS fatfs;
extern FIL file_handle;
extern FRESULT file_res;
extern uint8_t file_lfn[258];
extern uint8_t file_path[256];
extern uint16_t file_block_off, file_block_max;
extern enum filestates file_status;

void file_init(void);
void file_open(const uint8_t* filename, BYTE flags);
FRESULT dir_open_by_filinfo(DIR* dir, FILINFO* fno_param);
void file_open_by_filinfo(FILINFO* fno);
void file_close(void);
void file_seek(uint32_t offset);
UINT file_read(void);
UINT file_write(size_t len);
UINT file_readblock(void* buf, uint32_t addr, uint16_t size);
UINT file_writeblock(void* buf, uint32_t addr, uint16_t size);

uint8_t file_getc(void);

/* Two-letter bucket layout (firmware 2.15+). See fileops.c for THE RULE and the two host mirrors
   that must match it (utils/sd_bucket.py, the Manager's sd-layout.ts). */
int     path_is_gb(const char *name);                /* THE Game Boy test; .sgb does NOT match */
int     path_is_st(const char *name);                /* THE Sufami Turbo test (.st only) */
/* The namespace directory this ROM's sidecars live under ("sgb"/"sft"/"nes"/"sms"/"a26"), always
   THREE letters, or NULL for plain SNES. See the table in fileops.c. */
const char *path_ns(const char *name);
void    path_bucket2(const char *path, char *out);   /* writes 3 bytes: "SU\0" */
int     path_asset(char *buf, int buflen, const char *root, const char *src, const char *ext);
FRESULT path_asset_mkdir(char *path);
FRESULT check_or_create_folder(TCHAR *dir);

char *get_fresult_friendlyname(FRESULT res);

/* print_fresult() is log-only, so it follows the serial console (see lpc175x/uart.h).
   The disabled form keeps its arguments live for -Wall while the dead `if (0)` drops
   the call; print_fresult_real() is never defined, so a stray reference is a link error.
   No format attribute here on purpose: the live declaration below has none either, and
   adding one would make the Mk.II reject %s calls passing uint8_t*. */
#ifdef CONFIG_UART_DEBUG
char *get_fresult_name(FRESULT res);
void print_fresult(FRESULT res, const char *fmt, ...);
void vprint_fresult(FRESULT res, const char *fmt, va_list arglist);
#else
void print_fresult_real(FRESULT res, const char *fmt, ...);
#define print_fresult(...)           do { if (0) print_fresult_real(__VA_ARGS__); } while (0)
#define vprint_fresult(res, fmt, ap) do { (void)(res); (void)(fmt); (void)(ap); } while (0)
#endif

#endif
