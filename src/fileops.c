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

   fileops.c: simple file access functions
*/

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "diskio.h"
#include "util.h"

#include <string.h>
#include <stdarg.h>

BYTE file_buf[512] __attribute__((aligned(4)));
FATFS fatfs;
FIL file_handle;
FRESULT file_res;
uint8_t file_lfn[258];
uint8_t file_path[256];
uint16_t file_block_off, file_block_max;
enum filestates file_status;

int newcard;

/* Log-side FRESULT spelling: only ever reaches a serial console, so it follows
   CONFIG_UART_DEBUG. The user-facing fresult_friendly_names[] below does NOT -- it is
   drawn on screen by the early-boot "Could not load menu ROM!" failure. */
#ifdef CONFIG_UART_DEBUG
static const char *const fresult_names[] = { "FR_OK", "FR_DISK_ERR", "FR_INT_ERR",
  "FR_NOT_READY", "FR_NO_FILE", "FR_NO_PATH", "FR_INVALID_NAME",
  "FR_DENIED", "FR_EXIST", "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED",
  "FR_INVALID_DRIVE", "FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED",
  "FR_TIMEOUT", "FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
  "FR_INVALID_PARAMETER" };
#endif

/* Same FRESULT order as fresult_names above, but worded for the user instead of the log.
   English only: the one screen that shows these is the early boot "Could not load menu ROM!"
   failure, drawn by the firmware itself long before a menu exists to localize anything. */
static const char *const fresult_friendly_names[20] = {
  "No error", "Card I/O error", "Internal FS driver error",
  "Drive not ready", "File not found", "Directory not found", "Invalid path name",
  "Access denied", "Access denied (exists)", "Invalid file object", "Write protected",
  "Invalid drive specified", "No work area", "Not a valid file system", "mkfs() aborted",
  "Drive access timeout", "Shared access locked", "Not enough memory", "Too many open files",
  "Invalid parameter" };

void file_init() {
  file_res=f_mount(&fatfs, "/", 1);
  newcard = 0;
  file_path[0] = '/';
  file_path[1] = 0;
}

void file_open(const uint8_t* filename, BYTE flags) {
  file_res = f_open(&file_handle, (TCHAR*)filename, flags);
  file_block_off = sizeof(file_buf);
  file_block_max = sizeof(file_buf);
  file_status = file_res ? FILE_ERR : FILE_OK;
  print_fresult(file_res, "file_open (%s, %02x)", filename, flags);
}

void file_close() {
  file_res = f_close(&file_handle);
}

void file_seek(uint32_t offset) {
  file_res = f_lseek(&file_handle, (DWORD)offset);
}

UINT file_read() {
  UINT bytes_read;
  file_res = f_read(&file_handle, file_buf, sizeof(file_buf), &bytes_read);
  return bytes_read;
}

UINT file_write(size_t len) {
  UINT bytes_written;
  file_res = f_write(&file_handle, file_buf, len, &bytes_written);
  if(bytes_written < len) {
    printf("wrote less than expected - card full?\n");
  }
  return bytes_written;
}

UINT file_readblock(void* buf, uint32_t addr, uint16_t size) {
  UINT bytes_read;
  file_res = f_lseek(&file_handle, addr);
  if(file_handle.fptr != addr) {
    return 0;
  }
  file_res = f_read(&file_handle, buf, size, &bytes_read);
  return bytes_read;
}

UINT file_writeblock(void* buf, uint32_t addr, uint16_t size) {
  UINT bytes_written;
  file_res = f_lseek(&file_handle, addr);
  if(file_res) return 0;
  file_res = f_write(&file_handle, buf, size, &bytes_written);
  return bytes_written;
}

uint8_t file_getc() {
  if(file_block_off == file_block_max) {
    file_block_max = file_read();
    if(file_block_max == 0) file_status = FILE_EOF;
    file_block_off = 0;
  }
  return file_buf[file_block_off++];
}

/* --- Two-letter bucket layout (firmware 2.15+) -------------------------------------------------
 * Per-game assets live under a TWO-CHARACTER bucket directory:
 *   /sd2snes/info/SU/<stem>.yml   /sd2snes/saves/SU/<stem>.srm   etc.
 * WHY: a FAT lookup is linear and long-name compares are expensive. On a real card
 * /sd2snes/cheats held 2121 entries and /sd2snes/info/S held 1512, costing ~720ms and ~300ms per
 * game load (measured). Two characters takes the median directory from ~770 files to ~40.
 *
 * THE RULE  bucket(leaf) = f(leaf[0]) + f(leaf[1]),  f(c) = upper(c) if [0-9A-Z] else '_',
 *           a missing character -> '_'.
 * Derived from the RAW leaf (before the extension is stripped). That is equivalent to deriving it
 * from the stem -- the only index-1 difference is a one-character stem, where the raw leaf gives
 * '.'->'_' and the stem gives the pad '_', the same answer. Said explicitly because the mirrors
 * could otherwise drift by picking the other order.
 *
 * THIS FUNCTION IS THE ONE THE DEVICE RUNS, and the Web Manager is what CREATES these paths --
 * its core/sd-layout.ts bucketOf() must match this exactly, or the device looks in a different
 * directory than the Manager wrote to and the user sees saves/cheats/covers "disappear".
 * tests/host/run_bucket.sh pins this side against the shared case table (and already caught a
 * read-past-NUL here); the Manager's spec uses the same table.
 */
void path_bucket2(const char *path, char *out) {
  const char *leaf = strrchr(path, '/');
  int i, end = 0;
  leaf = leaf ? leaf + 1 : path;
  for(i = 0; i < 2; i++) {
    unsigned char c;
    /* `end` latches at the NUL: without it, a one-character (or empty) leaf reads leaf[1] PAST
       the terminator. ASan caught exactly that in tests/host/run_bucket.sh. */
    if(end) { out[i] = '_'; continue; }
    c = (unsigned char)leaf[i];
    if(!c) { end = 1; out[i] = '_'; continue; } /* leaf shorter than 2 -> pad */
    if(c >= 'a' && c <= 'z') c -= 32;
    out[i] = ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) ? (char)c : '_';
  }
  out[2] = 0;
}

/* Does the firmware load this ROM through the Super Game Boy core?
 * THE definition -- sgb_id() (sgb.c) and path_asset() below both call it, so the core that boots
 * the game and the directory its save lands in can never disagree.
 * The rule: the extension STARTS WITH "gb", case-insensitive -> .gb, .gbc.
 * ".sgb" DOES NOT MATCH (it starts with 's'); such a file loads as a plain SNES ROM. That is not
 * an oversight -- the Web Manager mirrors this exact test in core/sd-layout.ts isGbRom(), even
 * though its own SYSTEM_BY_EXT calls the .sgb extension's system 'SGB'.
 * `name` may be a full path or a bare leaf; only the last '.' matters. */
int path_is_gb(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && (dot[1] | 32) == 'g' && (dot[2] | 32) == 'b';
}

/* THE Sufami Turbo test, for the same reason path_is_gb exists: without a namespace
   of its own "Tetris.st" and "Tetris.sfc" would share one .srm/.yml/.state.  Exact
   match -- unlike .gb, ".st" has no family of longer relatives to cover. */
int path_is_st(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && (dot[1] | 32) == 's' && (dot[2] | 32) == 't' && dot[3] == 0;
}

/* Case-insensitive compare of a NUL-terminated extension against a lowercase literal. */
static int ext_is(const char *ext, const char *lit) {
  while(*lit) { if((*ext | 32) != *lit) return 0; ext++; lit++; }
  return *ext == 0;
}

static const struct { const char *ext; const char *ns; } ns_by_ext[] = {
  { "nes", "nes" },
  { "sms", "sms" },
  { "a26", "a26" },
};

const char *path_ns(const char *name) {
  const char *dot = strrchr(name, '.');
  unsigned i;
  if(path_is_gb(name)) return "sgb";
  if(path_is_st(name)) return "sft";
  if(!dot) return 0;
  for(i = 0; i < sizeof(ns_by_ext) / sizeof(ns_by_ext[0]); i++)
    if(ext_is(dot + 1, ns_by_ext[i].ext)) return ns_by_ext[i].ns;
  return 0;
}

/* Build "<root>[sgb/]<BB>/<stem><ext>" into buf. `root` MUST end in '/'.
 * The bucket AND the stem come from the SAME `src` leaf -- never split those across two strings,
 * or a patched game's .srm and .state land in different buckets (see memory.c/savestate.c, which
 * pick current_ips_srm_source before naming).
 * `ext` "" gives the bare stem (the gameinfo/manual shape). src's own extension is stripped.
 * Unlike the old append_file_basename this confines the '.' search to the LEAF, so a '.' in the
 * root or in the bucket can never be clobbered.
 * Returns the offset of <stem> within buf, or -1 if it did not fit. On -1 the buffer is left as
 * the EMPTY STRING, never a truncated path: callers pass `buf` straight to f_open/f_unlink/f_stat,
 * and an unterminated or truncated name would either read past the buffer or make two different
 * games share one save file. Still CHECK THE -1 on write paths -- an empty name fails cleanly, but
 * only the caller can report it. */
int path_asset(char *buf, int buflen, const char *root, const char *src, const char *ext) {
  const char *leaf = strrchr(src, '/');
  const char *dot, *ns;
  int n = 0, stem_off, leaflen;

  if(buflen > 0) buf[0] = 0;                    /* every -1 below leaves this in place */
  leaf = leaf ? leaf + 1 : src;
  dot  = strrchr(leaf, '.');
  leaflen = dot && dot != leaf ? (int)(dot - leaf) : (int)strlen(leaf);

  while(*root && n < buflen - 1) buf[n++] = *root++;

  /* The system's namespace directory, if it has one (see path_ns above for why).
     Taken from `src` like the bucket and the stem, so the writer and the menu-side delete-SRM
     path (main.c) always agree: both start from the same string. sgb_romprops.has_sgb would NOT
     work here -- sgb_id() only runs inside load_rom, so in the menu it holds the LAST LOADED
     game and the delete would miss.
     Known wart: a patched .gb names its save from current_ips_srm_source (a .ips path), so the
     .srm falls outside sgb/ while sgb.c's .gtc stays in. That configuration is already
     non-functional -- the patch lands on the SGB SNES BIOS at 0x880000, not on the GB ROM at 0. */
  ns = path_ns(leaf);
  if(ns) {
    if(n > buflen - 9) { buf[0] = 0; return -1; }  /* no room for "xxx/" + "BB/" + NUL */
    memcpy(buf + n, ns, 3);
    buf[n + 3] = '/';
    n += 4;
  }

  if(n > buflen - 5) { buf[0] = 0; return -1; }   /* no room for "BB/" + NUL */
  path_bucket2(src, buf + n);
  n += 2;
  buf[n++] = '/';
  stem_off = n;
  if(n + leaflen + (int)strlen(ext) >= buflen) { buf[0] = 0; return -1; }
  memcpy(buf + n, leaf, (size_t)leaflen);
  n += leaflen;
  strcpy(buf + n, ext);
  return stem_off;
}

/* mkdir -p of the directory portion of an already-built asset path (up to and including the last
 * '/'). Terminates in place and restores, so the caller does not need a second 256-byte buffer on
 * an already-deep frame. ONLY call this on a WRITE path, and only AFTER the name is built --
 * creating from the bare root would make an empty bucket dir on every read. */
FRESULT path_asset_mkdir(char *path) {
  char *slash = strrchr(path, '/');
  FRESULT res;
  char save;
  if(!slash) return FR_OK;
  save = slash[1];
  slash[1] = 0;                                 /* check_or_create_folder wants a trailing '/' */
  res = check_or_create_folder((TCHAR *)path);
  slash[1] = save;
  return res;
}

FRESULT check_or_create_folder(TCHAR *dir) {
  FRESULT res;
  FILINFO fno;
  /* we are not interested in the file name of the existing object
     so no extra LFN buffer needs to be allocated. */
  fno.lfname = NULL;
  TCHAR buf[256];
  TCHAR *ptr = buf;
  strlcpy_nul(buf, dir, sizeof(buf));
  while(*(ptr++)) {
    if(*ptr == '/') {
      *ptr = 0;
      res = f_stat(buf, &fno);
      printf("checking folder %s... res=%d\n", buf, res);
      if(res != FR_OK) {
        res = f_mkdir(buf);
        printf("creating folder, res=%d\n", res);
        if(res != FR_OK) {
          printf("FATAL: could not create folder %s\n", buf);
          return res;
        }
      } else {
        if(!(fno.fattrib & AM_DIR)) {
          printf("FATAL: %s exists but is not a directory.\n", buf);
          return FR_NO_PATH;
        }
      }
      *ptr = '/';
    }
  }
  return FR_OK;
}

#ifdef CONFIG_UART_DEBUG
char *get_fresult_name(FRESULT res) {
  return (char *)fresult_names[res];
}
#endif

char *get_fresult_friendlyname(FRESULT res) {
  return (char *)fresult_friendly_names[res];
}

#ifdef CONFIG_UART_DEBUG
void vprint_fresult(FRESULT res, const char *fmt, va_list arglist) {
  vprintf(fmt, arglist);
  printf(": %s(%d)\n", get_fresult_name(res), res);
}

void print_fresult(FRESULT res, const char *fmt, ...) {
  va_list arglist;
  va_start(arglist, fmt);
  vprint_fresult(res, fmt, arglist);
  va_end(arglist);
}
#endif /* CONFIG_UART_DEBUG */
