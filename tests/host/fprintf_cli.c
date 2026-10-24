/* f_printf conformance: the REAL src/ff.c (f_printf -> f_puts -> f_write) and the REAL
 * src/printf.c (the formatter f_printf renders with), on a RAM disk formatted by FatFs'
 * own f_mkfs, so nothing outside this repository is needed.
 *
 * Every format string the firmware hands to f_printf (cheat.c, cfg.c, gameopt.c,
 * patchmeta.c) is written with values at their edges, the file is read back and compared
 * byte for byte with what FatFs' own formatter produced: upper-case %X, LF -> CRLF, '-'
 * for a negative %d, width and padding.  The return value is checked too (characters
 * put, the inserted CRs included).  A line longer than f_printf's buffer must come out whole
 * or cut short, but terminated and in bounds (ASan watches the read).
 *
 * The values are passed as the MCU passes them: %lX with a 32-bit unsigned (long is 32
 * bits there, and printf.c reads every number as an int), hence -Wno-format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "diskio.h"

/* ---- RAM disk ---- */
#define NSECT 16384u                       /* 8 MB */
static uint8_t *disk;
volatile enum diskstates disk_state;
int sd_offload, ff_sd_offload, sd_offload_tgt, newcard, sd_offload_partial;
uint16_t sd_offload_partial_start, sd_offload_partial_end;
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | ((DWORD)10 << 21) | ((DWORD)24 << 16); }
DSTATUS disk_initialize(BYTE d) { (void)d; return 0; }
DSTATUS disk_status(BYTE d) { (void)d; return 0; }
DRESULT disk_read(BYTE d, BYTE *b, DWORD s, UINT c) {
  (void)d;
  if((size_t)s + c > NSECT) return RES_PARERR;
  memcpy(b, disk + (size_t)s * 512, (size_t)c * 512);
  return RES_OK;
}
DRESULT disk_write(BYTE d, const BYTE *b, DWORD s, UINT c) {
  (void)d;
  if((size_t)s + c > NSECT) return RES_PARERR;
  memcpy(disk + (size_t)s * 512, b, (size_t)c * 512);
  return RES_OK;
}
DRESULT disk_ioctl(BYTE d, BYTE cmd, void *buf) {
  (void)d;
  if(cmd == GET_SECTOR_COUNT) *(DWORD *)buf = NSECT;
  else if(cmd == GET_BLOCK_SIZE) *(DWORD *)buf = 1;
  else if(cmd == GET_SECTOR_SIZE) *(WORD *)buf = 512;
  return RES_OK;
}
int dirindex_find(const char *path, uint32_t *clust, uint32_t *size) { (void)path; (void)clust; (void)size; return -1; }

static FATFS fs;
static FIL fil;
static char want[8192];
static size_t wlen;
static int fails;

/* append what FatFs' formatter wrote for one call; check what f_printf returned */
static void expect(int got, const char *s) {
  size_t n0 = wlen;
  for(; *s; s++) {
    if(*s == '\n') want[wlen++] = '\r';
    want[wlen++] = *s;
  }
  if(got != (int)(wlen - n0)) {
    printf("  FAIL return value %d, want %d (for \"%.*s\")\n", got, (int)(wlen - n0), (int)(wlen - n0), want + n0);
    fails++;
  }
}

int main(void) {
  static const uint32_t u32[] = { 0, 1, 0x9, 0xa, 0xabcdef, 0x7fffffff, 0x80000000u, 0xffffffffu, 0x12345678 };
  static const int ints[] = { 0, 1, 9, 10, 15, 255, -1, -128, 2147483647, -2147483647 - 1 };
  char line[64];
  disk = calloc(NSECT, 512);
  if(!disk) return 2;
  if(f_mount(&fs, "", 0) != FR_OK || f_mkfs("", 1, 0) != FR_OK) { printf("  FAIL mkfs\n"); return 1; }
  if(f_open(&fil, "t.yml", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { printf("  FAIL open\n"); return 1; }

  /* cheat.c */
  expect(f_printf(&fil, "  Enabled: %s\n", "true"), "  Enabled: true\n");
  expect(f_printf(&fil, "  Enabled: %s\n", "false"), "  Enabled: false\n");
  expect(f_printf(&fil, "  Code:\n"), "  Code:\n");
  for(unsigned i = 0; i < sizeof(u32) / sizeof(u32[0]); i++) {
    snprintf(line, sizeof line, "  - \"%08X\"    ", u32[i]);
    expect(f_printf(&fil, "  - \"%08lX\"    ", u32[i]), line);
    snprintf(line, sizeof line, "# GG code: %04X-%04X\n", u32[i] >> 16, u32[i] & 0xffff);
    expect(f_printf(&fil, "# GG code: %04lX-%04lX\n", u32[i] >> 16, u32[i] & 0xffff), line);
  }
  expect(f_printf(&fil, "  - \"%-9s\"   ", "7E0DBF63"), "  - \"7E0DBF63 \"   ");
  expect(f_printf(&fil, "  - \"%-9s\"   ", "C2A-0F3E"), "  - \"C2A-0F3E \"   ");
  expect(f_printf(&fil, "  - \"%-9s\"   ", "7E0DBF6300"), "  - \"7E0DBF6300\"   ");
  expect(f_printf(&fil, "  - \"%-9s\"   ", ""), "  - \"         \"   ");
  expect(f_printf(&fil, "# GG code: N/A (WRAM cheat)\n"), "# GG code: N/A (WRAM cheat)\n");
  /* cfg.c */
  for(unsigned i = 0; i < 256; i += 51) {
    snprintf(line, sizeof line, "Language: %u\n", i);
    expect(f_printf(&fil, "%s: %d\n", "Language", (uint8_t)i), line);
  }
  expect(f_printf(&fil, "%s: %06lX%08lX\n", "BSXTime", 0x260124u, 0x09153000u), "BSXTime: 26012409153000\n");
  expect(f_printf(&fil, "%s: %06lX%08lX\n", "BSXTime", 0u, 0xau), "BSXTime: 0000000000000A\n");
  expect(f_printf(&fil, "%s: %s\n", "ShowCovers", "true"), "ShowCovers: true\n");
  expect(f_printf(&fil, "%s: %s\n", "SkinName", ""), "SkinName: \n");
  /* gameopt.c: %d can be negative there */
  for(unsigned i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
    snprintf(line, sizeof line, "SaveStateSlot: %d\n", ints[i]);
    expect(f_printf(&fil, "%s: %d\n", "SaveStateSlot", ints[i]), line);
  }
  /* patchmeta.c */
  expect(f_printf(&fil, "  Header: \"%s\"\n", "auto"), "  Header: \"auto\"\n");
  /* the longest real line: a key and a 127-character path (cfg.c CK_STR) */
  {
    char path[128], exp[200];
    memset(path, 'p', 127); path[0] = '/'; path[127] = 0;
    snprintf(exp, sizeof exp, "FontName: %s\n", path);
    expect(f_printf(&fil, "%s: %s\n", "FontName", path), exp);
  }
  /* longer than f_printf's buffer: whole, or cut short -- terminated and in bounds either way */
  {
    char big[400];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    int r = f_printf(&fil, "%s\n", big);
    if(r != 401 && (r < 150 || r > 255)) { printf("  FAIL a long line gave %d characters\n", r); fails++; }
    for(int i = 0; i < r; i++) want[wlen++] = i < 399 ? 'x' : "\r\n"[i - 399];
  }
  if(f_close(&fil) != FR_OK) { printf("  FAIL close\n"); return 1; }

  static char got[8192];
  UINT br = 0;
  if(f_open(&fil, "t.yml", FA_READ) != FR_OK || f_read(&fil, got, sizeof got, &br) != FR_OK) {
    printf("  FAIL read back\n"); return 1;
  }
  f_close(&fil);
  if(br != wlen || memcmp(got, want, wlen)) {
    size_t i = 0;
    while(i < br && i < wlen && got[i] == want[i]) i++;
    printf("  FAIL file differs at byte %zu (got %u bytes, want %zu): got \"%.30s\" want \"%.30s\"\n",
           i, br, wlen, got + i, want + i);
    fails++;
  }
  printf("f_printf: %s (%zu bytes)\n", fails ? "FAIL" : "ok", wlen);
  free(disk);
  return fails ? 1 : 0;
}
