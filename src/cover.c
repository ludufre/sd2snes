/* sd2snes - per-ROM cover preview (MCU side). See cover.h for the format and
 * the hard safety contract (bounded, fail-safe, never hangs). */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cover.h"
#include "msu1.h"   /* menu_sfx_pump: keep a playing effect fed during streams */

/* derived from the rom path by swapping the extension to ".cov" */
static uint8_t cover_path[260];

/* write the 8-byte meta block the SNES polls. Always called before returning
 * so the menu never waits on stale state. */
static void cover_set_status(uint32_t base, uint8_t status, uint8_t w,
                             uint8_t h, uint8_t cg_base, uint8_t ncolors_m1,
                             uint8_t flags) {
  uint8_t meta[COVER_META_SIZE];
  meta[0] = status;
  meta[1] = w;
  meta[2] = h;
  meta[3] = cg_base;
  meta[4] = ncolors_m1;
  meta[5] = flags;
  meta[6] = 0;
  meta[7] = 0;
  sram_writeblock(meta, base + COVER_OFF_STATUS, sizeof(meta));
}

/* stream `size` bytes from the open file into bank-C9 at `addr`, in chunks
 * bounded by file_buf. Returns 1 on success, 0 on read error / short read. */
static int cover_stream(uint32_t addr, uint32_t size) {
  UINT got = 0;
  while(size) {
    menu_sfx_pump();  /* keep a playing menu effect fed while streaming a cover */
    UINT want = (size > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)size;
    file_res = f_read(&file_handle, file_buf, want, &got);
    if(file_res || got != want) return 0;
    sram_writeblock(file_buf, addr, (uint16_t)got);
    addr += got;
    size -= got;
  }
  return 1;
}

/* stream an explicit .cov path (v4, 16x16 OBJ sprites) into sram_addr in the
 * COVER_OFF_* layout. Bounded + fail-safe; writes a status byte on every path.
 * Used by the game-info screen to stage the cover (-> OBJ name table 0) and the
 * screenshot (-> OBJ name table 1) into separate banks. */
int load_cover_path(const char *cov_path, uint32_t sram_addr) {
  UINT got = 0;
  uint8_t hdr[COVER_HEADER_SIZE];

  /* fail-safe default: mark "no cover" up front; only OK after a clean load */
  cover_set_status(sram_addr, COVER_STATUS_NONE, 0, 0, 0, 0, 0);

  printf("cover: try %s\n", cov_path);
  file_open((uint8_t*)cov_path, FA_READ);
  if(file_res) {
    printf("cover: open failed res=%d (no cover)\n", file_res);
    return 0;
  }

  /* header */
  file_res = f_read(&file_handle, hdr, COVER_HEADER_SIZE, &got);
  if(file_res || got != COVER_HEADER_SIZE
     || hdr[0] != COVER_MAGIC0 || hdr[1] != COVER_MAGIC1
     || hdr[2] != COVER_VERSION || hdr[8] != COVER_BPP) {
    printf("cover: bad header res=%d got=%u magic=%c%c ver=%u bpp=%u\n",
           file_res, got, hdr[0], hdr[1], hdr[2], hdr[8]);
    file_close();
    cover_set_status(sram_addr, COVER_STATUS_ERROR, 0, 0, 0, 0, 0);
    return 0;
  }

  uint8_t flags      = hdr[3];
  uint8_t w_spr      = hdr[4];
  uint8_t h_spr      = hdr[5];
  uint8_t n_palettes = hdr[6];

  /* v4 OBJ sprite image: a w_spr x h_spr grid of 16x16 sprites, <=8 OBJ palettes,
   * 4bpp tiles in a 16-wide name grid ((2*h)*16 tiles). OBJ palettes are at CGRAM
   * 128..255, so there is no cg_base to validate. */
  if(w_spr == 0 || h_spr == 0
     || w_spr > COVER_OBJ_MAX_W || h_spr > COVER_OBJ_MAX_H
     || n_palettes == 0 || n_palettes > COVER_MAX_PALETTES) {
    printf("cover: bad dims w_spr=%u h_spr=%u npal=%u\n", w_spr, h_spr, n_palettes);
    file_close();
    cover_set_status(sram_addr, COVER_STATUS_ERROR, 0, 0, 0, 0, 0);
    return 0;
  }

  uint32_t pal_size      = (uint32_t)n_palettes * 32;   /* np*16*2,  <= 256  */
  uint32_t blockmap_size = (uint32_t)w_spr * h_spr;     /*           <= 64   */
  uint32_t tiles_size    = (uint32_t)h_spr * 1024;      /* (2*h)*16*32, <= 8192 */

  /* file order matches cover_conv.py: PALETTES, BLOCKMAP, TILES */
  if(!cover_stream(sram_addr + COVER_OFF_PAL,      pal_size))      goto trunc;
  if(!cover_stream(sram_addr + COVER_OFF_BLOCKMAP, blockmap_size)) goto trunc;
  if(!cover_stream(sram_addr + COVER_OFF_TILES,    tiles_size))    goto trunc;

  file_close();

  /* meta args map to: w_spr, h_spr, n_palettes (the old cg_base slot), 0, flags */
  cover_set_status(sram_addr, COVER_STATUS_OK, w_spr, h_spr, n_palettes, 0, flags);
  printf("cover: ok %ux%u px, %ux%u sprites, %u palettes, %lu tiles\n",
         w_spr * 16, h_spr * 16, w_spr, h_spr, n_palettes,
         (unsigned long)(tiles_size / 32));
  return 1;

trunc:
  printf("cover: truncated/read error res=%d\n", file_res);
  file_close();
  cover_set_status(sram_addr, COVER_STATUS_ERROR, 0, 0, 0, 0, 0);
  return 0;
}

int load_cover(const uint8_t *rom_path, uint32_t sram_addr) {
  /* fail-safe default: mark "no cover" up front; only OK after a clean load */
  cover_set_status(sram_addr, COVER_STATUS_NONE, 0, 0, 0, 0, 0);

  /* build "<rom>.cov": copy the full path (bounded) and rewrite the extension */
  size_t len = 0;
  while(rom_path[len] && len < sizeof(cover_path) - 5) {
    cover_path[len] = rom_path[len];
    len++;
  }
  cover_path[len] = 0;
  char *dot = strrchr((char*)cover_path, '.');
  if(!dot) {
    printf("cover: no extension in %s\n", cover_path);
    return 0;
  }
  strcpy(dot, ".cov");

  return load_cover_path((char*)cover_path, sram_addr);
}
