/* sd2snes - per-ROM cover preview (MCU side). See cover.h for the format and
 * the hard safety contract (bounded, fail-safe, never hangs). */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cover.h"

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
    UINT want = (size > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)size;
    file_res = f_read(&file_handle, file_buf, want, &got);
    if(file_res || got != want) return 0;
    sram_writeblock(file_buf, addr, (uint16_t)got);
    addr += got;
    size -= got;
  }
  return 1;
}

int load_cover(const uint8_t *rom_path, uint32_t sram_addr) {
  UINT got = 0;
  uint8_t hdr[COVER_HEADER_SIZE];

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
  printf("cover: try %s\n", cover_path);

  file_open((uint8_t*)cover_path, FA_READ);
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
  uint8_t w          = hdr[4];
  uint8_t h          = hdr[5];
  uint8_t cg_base    = hdr[6];
  uint8_t ncolors_m1 = hdr[7];

  uint32_t ntiles  = (uint32_t)w * h;
  uint32_t ncolors = (uint32_t)ncolors_m1 + 1;
  uint32_t cg_end  = (uint32_t)cg_base + ncolors;   /* exclusive CGRAM end */
  /* the cover palette must fit in CGRAM and must NOT overlap the CGRAM the menu
   * always shows: 0..15 (UI text) or 64..95 (header logo, COVER_LOGO_CGB..+32).
   * Previously only the PC converter enforced cg_base>=16; enforce it here too
   * so a hand-made/corrupt .cov can't recolour the menu UI or the logo. */
  if(w == 0 || h == 0 || w > COVER_MAX_COLS || h > COVER_MAX_ROWS
     || ntiles > COVER_MAX_TILES
     || cg_base < 16 || cg_end > 256
     || (cg_base < 96 && cg_end > 64)) {
    printf("cover: bad dims w=%u h=%u cgbase=%u ncolors=%lu\n",
           w, h, cg_base, (unsigned long)ncolors);
    file_close();
    cover_set_status(sram_addr, COVER_STATUS_ERROR, 0, 0, 0, 0, 0);
    return 0;
  }

  uint32_t pal_size   = ncolors * 2;     /* <= 512   */
  uint32_t tiles_size = ntiles * 64;     /* <= 14336 */

  /* file order matches cover_conv.py: PALETTE then TILES */
  if(!cover_stream(sram_addr + COVER_OFF_PAL, pal_size))     goto trunc;
  if(!cover_stream(sram_addr + COVER_OFF_TILES, tiles_size)) goto trunc;

  file_close();

  cover_set_status(sram_addr, COVER_STATUS_OK, w, h, cg_base, ncolors_m1, flags);
  printf("cover: ok %ux%u px, %lu colours @ CGRAM %u, %lu tiles\n",
         w * 8, h * 8, (unsigned long)ncolors, cg_base, (unsigned long)ntiles);
  return 1;

trunc:
  printf("cover: truncated/read error res=%d\n", file_res);
  file_close();
  cover_set_status(sram_addr, COVER_STATUS_ERROR, 0, 0, 0, 0, 0);
  return 0;
}
