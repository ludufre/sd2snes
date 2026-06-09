/* sd2snes (ludufre fork) - menu THEME loader (MCU side). See theme.h.
 *
 * .thm format (little-endian), produced by utils/pack_theme.py:
 *     off  size  field
 *     0    8     magic  "FXTHEME1"
 *     8    1     version (=THEME_VERSION)
 *     9    1     N = number of regions
 *     10   2     flags
 *     12   4     reserved
 *     16   N*4   TOC: per region { u8 slot ; u8 _rsv ; u16 length }
 *     ...        payload: region blobs concatenated in TOC order
 *
 * `slot` indexes the menu's _GFXPTR_ table (see snes/const.a65 gfxptr_info):
 *   0 font  1 logo_pal  2 hdma_math_src  3 hdma_bar_color_src  4 hdma_pal_src
 *   5 oam_data_l  6 oam_data_h  7 palette  8 logo_tiles
 * The table is read from the loaded menu PSRAM so this survives menu rebuilds;
 * only theme_slot_max (the per-region byte cap) is build-coupled.
 */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cfg.h"
#include "theme.h"

extern cfg_t CFG;

static const char THEME_MAGIC[8]  = { 'F','X','T','H','E','M','E','1' };
static const char GFXPTR_MAGIC[8] = { '_','G','F','X','P','T','R','_' };
#define THEME_VERSION   1
#define THEME_NSLOTS    9

/* Window of the loaded menu image scanned for the _GFXPTR_ magic. The table
 * sits a few KB in (~0x2B3x in the fork); this range gives generous headroom. */
#define THEME_GFXPTR_SCAN_START  0x2000
#define THEME_GFXPTR_SCAN_END    0x6000

/* Per-slot byte cap == the region's size in the fork menu build. Writes are
 * clamped to this so a malformed/oversized theme can never overrun a region
 * into adjacent menu code. 0 == slot is never themed. Keep in sync with
 * utils/pack_theme.py SLOT_MAX. */
static const uint16_t theme_slot_max[THEME_NSLOTS] = {
  0,      /* 0 font (not themed)      */
  64,     /* 1 logo_pal               */
  19,     /* 2 hdma_math_src          */
  8,      /* 3 hdma_bar_color_src     */
  55,     /* 4 hdma_pal_src           */
  96,     /* 5 oam_data_l             */
  9,      /* 6 oam_data_h             */
  512,    /* 7 palette                */
  7168,   /* 8 logo_tiles             */
};

/* Stream `len` bytes from the open theme file, writing the first min(len,cap)
 * of them to PSRAM at `dest`. The full `len` is always consumed so the file
 * stays aligned for the next region (cap==0 -> consume only). Bounded by
 * file_buf; returns 1 on success, 0 on read error/short read. */
static int theme_stream(uint32_t dest, uint32_t len, uint32_t cap) {
  uint32_t done = 0;
  UINT got;
  while(len) {
    UINT want = (len > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)len;
    file_res = f_read(&file_handle, file_buf, want, &got);
    if(file_res || got != want) return 0;
    if(done < cap) {
      uint16_t w = (done + got > cap) ? (uint16_t)(cap - done) : (uint16_t)got;
      if(w) sram_writeblock(file_buf, dest + done, w);
    }
    done += got;
    len  -= got;
  }
  return 1;
}

/* Locate "_GFXPTR_" in the loaded menu PSRAM and read its THEME_NSLOTS word
 * pointers (16-bit bank-$C0 offsets) into gfxptr[]. Returns 1 on success. */
static int theme_read_gfxptr(uint16_t gfxptr[THEME_NSLOTS]) {
  uint8_t buf[512];
  const uint32_t step = sizeof(buf) - 8;   /* overlap so the magic isn't split */
  for(uint32_t off = THEME_GFXPTR_SCAN_START; off < THEME_GFXPTR_SCAN_END; off += step) {
    sram_readblock(buf, SRAM_MENU_ADDR + off, sizeof(buf));
    for(uint32_t i = 0; i + 8 <= sizeof(buf); i++) {
      if(!memcmp(buf + i, GFXPTR_MAGIC, 8)) {
        uint8_t words[THEME_NSLOTS * 2];
        sram_readblock(words, SRAM_MENU_ADDR + off + i + 8, sizeof(words));
        for(int k = 0; k < THEME_NSLOTS; k++)
          gfxptr[k] = (uint16_t)words[k * 2] | ((uint16_t)words[k * 2 + 1] << 8);
        return 1;
      }
    }
  }
  return 0;
}

void theme_apply(void) {
  const char *skin = (const char*)CFG.skin_name;
  /* skin_name holds the FULL SD path of the chosen .thm (captured from the
     browser selection, so themes can live in any visible folder). Anything not
     an absolute path -- empty, or the "sd2snes.skin" sentinel -- means
     "no theme / baked-in default look". */
  if(skin[0] != '/') return;

  file_open((const uint8_t*)skin, FA_READ);
  if(file_res) { printf("theme: open %s failed res=%d\n", skin, file_res); return; }

  uint8_t hdr[16];
  UINT got;
  file_res = f_read(&file_handle, hdr, sizeof(hdr), &got);
  if(file_res || got != sizeof(hdr)
     || memcmp(hdr, THEME_MAGIC, 8) || hdr[8] != THEME_VERSION) {
    printf("theme: bad header in %s\n", skin);
    file_close();
    return;
  }
  uint8_t n = hdr[9];
  if(n == 0 || n > THEME_NSLOTS) { file_close(); return; }

  uint8_t toc[THEME_NSLOTS * 4];
  file_res = f_read(&file_handle, toc, (UINT)n * 4, &got);
  if(file_res || got != (UINT)n * 4) { file_close(); return; }

  uint16_t gfxptr[THEME_NSLOTS];
  if(!theme_read_gfxptr(gfxptr)) {
    printf("theme: _GFXPTR_ not found in menu image\n");
    file_close();
    return;
  }

  for(int r = 0; r < n; r++) {
    uint8_t  slot = toc[r * 4];
    uint16_t len  = (uint16_t)toc[r * 4 + 2] | ((uint16_t)toc[r * 4 + 3] << 8);
    uint16_t cap  = (slot < THEME_NSLOTS) ? theme_slot_max[slot] : 0;
    if(cap == 0 || gfxptr[slot] == 0) {
      /* unknown/unthemable slot: consume payload, keep file aligned */
      if(!theme_stream(0, len, 0)) { file_close(); return; }
      continue;
    }
    if(!theme_stream(SRAM_MENU_ADDR + gfxptr[slot], len, cap)) {
      printf("theme: stream error slot %u\n", slot);
      file_close();
      return;
    }
  }
  file_close();
  printf("theme: applied %s\n", skin);
}

void theme_select(const char *name) {
  if(!name || name[0] == 0) {
    strcpy((char*)CFG.skin_name, THEME_DEFAULT);
  } else {
    strncpy((char*)CFG.skin_name, name, sizeof(CFG.skin_name) - 1);
    CFG.skin_name[sizeof(CFG.skin_name) - 1] = 0;
  }
  cfg_save();
}
