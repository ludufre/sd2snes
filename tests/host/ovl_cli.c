/* Card overlay loader (src/ovl.c, REAL) on the REAL FatFs (src/ff.c) over a RAM disk that
 * FatFs formats itself, with the REAL CRC-32 (src/crc32.c).  The images are stamped by the
 * REAL packer (src/ovl/ovlpack.py, run by run_ovl.sh): the loader has to accept what the
 * build writes and refuse everything else without running off the block.
 *
 * The module block is an array the size of the Mk.II/Mk.III one (ptrcache + the AHB tail,
 * 11344 B), and the loader's FIL sits at its end exactly as on the MCU (OVL_END is the
 * array's end); ASan watches every byte past it.
 *
 *   good image / reuse without reading / forget + missing file / magic / build id / CRC /
 *   a valid image of another build /
 *   size field / too big for the block / largest that fits / shorter than a header /
 *   bad id / ovl_depth guard / live module guard / the module image is forgotten /
 *   the reason of each refusal (ovl_err: missing or refused, what the menu's popup says) /
 *   a refusal remembered (file not opened again, reason kept) until ovl_recheck, not
 *   cleared by ovl_forget, per overlay
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "ff.h"
#include "diskio.h"
#include "ovl.h"

/* ---- RAM disk ---- */
#define NSECT 16384u
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
/* f_printf is linked in with ff.c, never called here */
int vsnprintf_hexup(char *b, size_t n, const char *f, va_list a) { (void)b; (void)n; (void)f; (void)a; return 0; }
int dirindex_find(const char *path, uint32_t *clust, uint32_t *size) { (void)path; (void)clust; (void)size; return -1; }

/* ---- what the loader sees of the firmware ---- */
uint32_t ptrcache[BLOCK_BYTES / 4];
char fwhdr[256];                          /* genhdr's header: the image CRC at +12 */
static int live, gates;
int  module_live(void) { return live; }
void module_gate(uint8_t cmd) { if(cmd == 0) gates++; }

static FATFS fs;
static int fails;
static uint8_t *good;
static long good_len;

static void put(const char *name, const uint8_t *d, long n) {
  FIL f;
  UINT bw;
  f_unlink(name);
  if(f_open(&f, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK || f_write(&f, d, (UINT)n, &bw) != FR_OK
     || bw != (UINT)n || f_close(&f) != FR_OK) {
    printf("  FAIL writing %s\n", name);
    exit(1);
  }
}

static uint8_t *slurp(const char *path, long *n) {
  FILE *f = fopen(path, "rb");
  uint8_t *d;
  if(!f) { printf("  FAIL no %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  d = malloc(*n + 16);
  if(fread(d, 1, *n, f) != (size_t)*n) exit(1);
  fclose(f);
  return d;
}

static void check(const char *what, int got, int want) {
  if(got != want) { printf("  FAIL %s: ovl_ensure %d, want %d\n", what, got, want); fails++; }
}

/* the reason the menu's popup shows (main.c publishes it at every menu load) */
static void why(const char *what, int want) {
  if(ovl_err != want) { printf("  FAIL %s: ovl_err %d, want %d\n", what, ovl_err, want); fails++; }
}

/* a refused load that must not touch the image area (the loader's FIL at the end of the
   block is its own: FatFs writes it on every open) */
#define IMG_AREA (BLOCK_BYTES - OVL_FIL_RESERVE)
static void untouched(const char *what) {
  static uint8_t before[IMG_AREA];
  memset(ptrcache, 0xa5, sizeof(ptrcache));
  memcpy(before, ptrcache, sizeof(before));
  check(what, ovl_ensure(OVL_ID_MENU1), 0);
  if(memcmp(before, ptrcache, sizeof(before))) { printf("  FAIL %s: the image area was written\n", what); fails++; }
}

int main(int argc, char **argv) {
  uint8_t *img;
  long n, fit;
  if(argc != 7) { printf("usage: ovl_cli good.ovl fw.hdr big-fit.ovl big-over.ovl other-build.ovl good2.ovl\n"); return 2; }
  disk = calloc(NSECT, 512);
  if(!disk) return 2;
  if(f_mount(&fs, "", 0) != FR_OK || f_mkfs("", 1, 0) != FR_OK) { printf("  FAIL mkfs\n"); return 1; }
  f_mkdir("/sd2snes");
  good = slurp(argv[1], &good_len);
  img = slurp(argv[2], &n);
  memcpy(fwhdr, img, n > 256 ? 256 : n);
  free(img);

  /* the image the build writes */
  put("/sd2snes/ovl1.im3", good, good_len);
  gates = 0;
  check("good image", ovl_ensure(OVL_ID_MENU1), 1);
  why("good image", 0);
  if(memcmp(ptrcache, good, good_len)) { printf("  FAIL good image: not in the block\n"); fails++; }
  if(gates != 1) { printf("  FAIL the module image was not forgotten (module_gate(0) x%d)\n", gates); fails++; }
  /* loaded: the next call does not read the card */
  f_unlink("/sd2snes/ovl1.im3");
  check("loaded image reused", ovl_ensure(OVL_ID_MENU1), 1);
  ovl_forget();
  untouched("forgotten, file missing");
  why("file missing", OVL_ERR_MISSING);
  /* a refusal is remembered until ovl_recheck (the menu load): the file is not opened
     again, the reason stays, and ovl_forget (a sort, a module) does not clear it */
  put("/sd2snes/ovl1.im3", good, good_len);
  {
    static uint8_t fil_before[OVL_FIL_RESERVE];
    memset((uint8_t *)ptrcache + IMG_AREA, 0x5a, sizeof(fil_before));
    memcpy(fil_before, (uint8_t *)ptrcache + IMG_AREA, sizeof(fil_before));
    check("refusal remembered", ovl_ensure(OVL_ID_MENU1), 0);
    why("refusal remembered", OVL_ERR_MISSING);
    if(memcmp(fil_before, (uint8_t *)ptrcache + IMG_AREA, sizeof(fil_before))) {
      printf("  FAIL refusal remembered: the loader opened the file again\n"); fails++;
    }
  }
  ovl_forget();
  check("refusal kept across ovl_forget", ovl_ensure(OVL_ID_MENU1), 0);
  ovl_recheck();
  check("file back after ovl_recheck", ovl_ensure(OVL_ID_MENU1), 1);
  why("file back after ovl_recheck", 0);

  /* each header field */
  {
    static const struct { const char *what; int off; uint8_t x; } bad[] = {
      { "bad magic", 0, 0x01 }, { "build id of another firmware", 4, 0x80 },
      { "size field", 8, 0x04 }, { "crc field", 12, 0x01 },
      { "a byte of code", 40, 0x10 }, { "the last byte", -1, 0x01 },
    };
    for(unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
      img = malloc(good_len);
      memcpy(img, good, good_len);
      img[bad[k].off < 0 ? good_len - 1 : bad[k].off] ^= bad[k].x;
      put("/sd2snes/ovl1.im3", img, good_len);
      free(img);
      ovl_recheck();
      check(bad[k].what, ovl_ensure(OVL_ID_MENU1), 0);
      why(bad[k].what, OVL_ERR_REFUSED);
      /* refused: not trusted (nor read) again until the recheck, then loaded again */
      put("/sd2snes/ovl1.im3", good, good_len);
      check("refused, not read again", ovl_ensure(OVL_ID_MENU1), 0);
      why("refused, not read again", OVL_ERR_REFUSED);
      ovl_recheck();
      check("reload after a refusal", ovl_ensure(OVL_ID_MENU1), 1);
      why("reload after a refusal", 0);
      ovl_forget();
    }
  }
  /* a whole, valid image of another firmware build (what an update of firmware.im3 alone
     leaves on the card) */
  img = slurp(argv[5], &n);
  put("/sd2snes/ovl1.im3", img, n);
  ovl_recheck();
  check("valid image, other firmware build", ovl_ensure(OVL_ID_MENU1), 0);
  why("valid image, other firmware build", OVL_ERR_REFUSED);
  ovl_depth = 1;   /* refused = not loaded: a nested call must not take it as loaded */
  check("other build is not left as loaded", ovl_ensure(OVL_ID_MENU1), 0);
  ovl_depth = 0;
  free(img);
  /* sizes: the largest image that leaves the FIL room fits, one word more does not */
  img = slurp(argv[3], &fit);
  put("/sd2snes/ovl1.im3", img, fit);
  ovl_recheck();
  check("largest image that fits", ovl_ensure(OVL_ID_MENU1), 1);
  free(img);
  ovl_recheck();
  img = slurp(argv[4], &n);
  put("/sd2snes/ovl1.im3", img, n);
  untouched("one word too big for the block");
  why("one word too big for the block", OVL_ERR_REFUSED);
  free(img);
  img = malloc(8);
  memcpy(img, good, 8);
  put("/sd2snes/ovl1.im3", img, 8);
  ovl_recheck();
  untouched("shorter than a header");
  why("shorter than a header", OVL_ERR_REFUSED);
  free(img);

  /* run-time guards (a guard refusal is not remembered: "good image again" below) */
  put("/sd2snes/ovl1.im3", good, good_len);
  ovl_recheck();
  ovl_depth = 1;
  untouched("overlay code running, image gone");
  ovl_depth = 0;
  live = 1;
  gates = 0;
  untouched("a module is live");
  {
    /* the guards refuse before FatFs: not even the FIL area is written */
    static uint8_t fil_before[OVL_FIL_RESERVE];
    memcpy(fil_before, (uint8_t *)ptrcache + IMG_AREA, sizeof(fil_before));
    check("a module is live (FIL)", ovl_ensure(OVL_ID_MENU1), 0);
    if(memcmp(fil_before, (uint8_t *)ptrcache + IMG_AREA, sizeof(fil_before))) {
      printf("  FAIL a module is live: the loader opened the file\n"); fails++;
    }
  }
  if(gates) { printf("  FAIL a live module was dropped\n"); fails++; }
  live = 0;
  check("id 0", ovl_ensure(0), 0);
  check("id past OVL_COUNT", ovl_ensure(OVL_COUNT + 1), 0);
  check("good image again", ovl_ensure(OVL_ID_MENU1), 1);
  ovl_depth = 1;
  check("nested call, same image", ovl_ensure(OVL_ID_MENU1), 1);
  ovl_depth = 0;

  /* the second overlay: one image in the block at a time */
  {
    long n2;
    uint8_t *good2 = slurp(argv[6], &n2);
    put("/sd2snes/ovl2.im3", good2, n2);
    check("overlay 2", ovl_ensure(OVL_ID_MENU2), 1);
    if(memcmp(ptrcache, good2, n2)) { printf("  FAIL overlay 2: not in the block\n"); fails++; }
    check("overlay 2 reused", ovl_ensure(OVL_ID_MENU2), 1);
    check("back to overlay 1", ovl_ensure(OVL_ID_MENU1), 1);
    if(memcmp(ptrcache, good, good_len)) { printf("  FAIL back to overlay 1: not in the block\n"); fails++; }
    /* overlay 1 code running: overlay 2 must not be loaded over it */
    ovl_depth = 1;
    check("overlay 2 while overlay 1 runs", ovl_ensure(OVL_ID_MENU2), 0);
    ovl_depth = 0;
    if(memcmp(ptrcache, good, good_len)) { printf("  FAIL overlay 1 was overwritten while running\n"); fails++; }
    check("overlay 1 still loaded", ovl_ensure(OVL_ID_MENU1), 1);
    /* overlay 1's image under overlay 2's name */
    put("/sd2snes/ovl2.im3", good, good_len);
    check("overlay 1 image as ovl2", ovl_ensure(OVL_ID_MENU2), 0);
    check("refused image is not left loaded", ovl_ensure(OVL_ID_MENU2), 0);
    why("refused image is not left loaded", OVL_ERR_REFUSED);
    /* overlay 2's refusal is its own: overlay 1 still loads */
    check("overlay 1 after overlay 2's refusal", ovl_ensure(OVL_ID_MENU1), 1);
    put("/sd2snes/ovl2.im3", good2, n2);
    check("overlay 2 fixed, before the recheck", ovl_ensure(OVL_ID_MENU2), 0);
    ovl_recheck();
    check("overlay 2 fixed, after the recheck", ovl_ensure(OVL_ID_MENU2), 1);
    free(good2);
  }

  printf(fails ? "ovl: %d FAILED\n" : "ovl: all passed\n", fails);
  return fails != 0;
}
