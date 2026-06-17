/* sd2snes+ - pre-boot game info screen (MCU side). See gameinfo.h for the SRAM
 * contract and the hard safety rules (bounded, fail-safe, never hangs). */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "yaml.h"
#include "gameinfo.h"

#ifndef CONFIG_MK2

/* UTF-8 codepoint -> sd2snes font byte. MUST match the ACCENTS map in
 * snes/utils/build_const.py (and snes/font.a65). Only the Latin accents the font
 * has glyphs for (codes 130..159); everything else renders as '?'. */
static const struct { uint16_t cp; uint8_t code; } gi_accents[] = {
  {0x00E1,130},{0x00E0,131},{0x00E2,132},{0x00E3,133},{0x00E9,134},{0x00EA,135},
  {0x00ED,136},{0x00F3,137},{0x00F4,138},{0x00F5,139},{0x00FA,140},{0x00E7,141},
  {0x00C1,142},{0x00C0,143},{0x00C2,144},{0x00C3,145},{0x00C9,146},{0x00CA,147},
  {0x00CD,148},{0x00D3,149},{0x00D4,150},{0x00D5,151},{0x00DA,152},{0x00C7,153},
  {0x00F1,154},{0x00D1,155},{0x00FC,156},{0x00DC,157},{0x00BF,158},{0x00A1,159},
};

/* Copy src -> dst (NUL-terminated, bounded), decoding UTF-8 to font byte codes.
 * Plain ASCII is copied verbatim; mapped accents become 130..159; anything else
 * becomes '?'. Bounded by dstsize; safe on truncated/invalid UTF-8. */
static void gi_utf8_to_font(const char *src, char *dst, int dstsize) {
  int di = 0;
  const unsigned char *s = (const unsigned char *)src;
  while(*s && di < dstsize - 1) {
    unsigned char c = *s;
    if(c < 0x80) { dst[di++] = (char)c; s++; continue; }
    /* lead byte: how many continuation bytes follow */
    uint32_t cp; int cont;
    if((c & 0xE0) == 0xC0)      { cp = c & 0x1F; cont = 1; }
    else if((c & 0xF0) == 0xE0) { cp = c & 0x0F; cont = 2; }
    else if((c & 0xF8) == 0xF0) { cp = c & 0x07; cont = 3; }
    else { s++; dst[di++] = '?'; continue; } /* stray continuation/invalid lead */
    s++;
    int ok = 1;
    for(int k = 0; k < cont; k++) {
      if((*s & 0xC0) != 0x80) { ok = 0; break; }   /* incl. *s==0 (truncated) */
      cp = (cp << 6) | (*s & 0x3F);
      s++;
    }
    if(!ok) { dst[di++] = '?'; continue; }
    uint8_t code = 0;
    for(unsigned k = 0; k < sizeof(gi_accents) / sizeof(gi_accents[0]); k++) {
      if(gi_accents[k].cp == cp) { code = gi_accents[k].code; break; }
    }
    dst[di++] = code ? (char)code : '?';
  }
  dst[di] = 0;
}

/* dst = a + b, bounded (no snprintf dependency). */
static void gi_join(char *dst, int dstsize, const char *a, const char *b) {
  int n = strlen(a);
  if(n > dstsize - 1) n = dstsize - 1;
  memcpy(dst, a, n);
  int m = strlen(b);
  if(n + m > dstsize - 1) m = dstsize - 1 - n;
  memcpy(dst + n, b, m);
  dst[n + m] = 0;
}

/* If a key is present, font-encode its value into the field (bounded). */
static void gi_field(const char *key, char *field, int size) {
  yaml_token_t tok;
  if(yaml_get_itemvalue(key, &tok)) {
    gi_utf8_to_font(tok.stringvalue, field, size);
  }
}

/* leave a single "-" placeholder for an empty metadata field (the SNES then
 * prints every field unconditionally - no empty check needed there). */
static void gi_dash(char *field) {
  if(!field[0]) { field[0] = '-'; field[1] = 0; }
}

/* stream `size` bytes from the open file to PSRAM at `addr` (bounded chunks). */
static int gi_stream(uint32_t addr, uint32_t size) {
  UINT got;
  while(size) {
    UINT want = (size > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)size;
    file_res = f_read(&file_handle, file_buf, want, &got);
    if(file_res || got != want) return 0;
    sram_writeblock(file_buf, addr, (uint16_t)got);
    addr += got; size -= got;
  }
  return 1;
}

/* Load the DirectColor image.gd: validate the header, stream the tilemap into
 * SRAM_GAMEINFO_TMAP_ADDR and the 8bpp tiles into SRAM_GAMEINFO_TILES_ADDR, and
 * record the geometry in the meta struct. Returns 1 on success. Bounded/fail-safe. */
static int gi_load_image(const char *path, gameinfo_meta_t *meta) {
  uint8_t hdr[GD_HEADER_SIZE];
  UINT got;
  file_open((uint8_t *)path, FA_READ);
  if(file_res) return 0;
  file_res = f_read(&file_handle, hdr, GD_HEADER_SIZE, &got);
  if(file_res || got != GD_HEADER_SIZE
     || hdr[0] != GD_MAGIC0 || hdr[1] != GD_MAGIC1 || hdr[2] != GD_VERSION) {
    file_close(); return 0;
  }
  uint8_t  w = hdr[4], h = hdr[5];
  uint16_t ntiles = (uint16_t)(hdr[6] | (hdr[7] << 8));
  /* VRAM holds at most 512 unique 8bpp tiles (two 256-tile windows, see snes/gameinfo.a65) */
  if(w == 0 || w > 32 || h == 0 || h > 32 || ntiles == 0 || ntiles > 512) {
    file_close(); return 0;
  }
  /* file order: header, tilemap (w*h*2), tiles (ntiles*64) */
  if(!gi_stream(SRAM_GAMEINFO_TMAP_ADDR,  (uint32_t)w * h * 2)) { file_close(); return 0; }
  if(!gi_stream(SRAM_GAMEINFO_TILES_ADDR, (uint32_t)ntiles * 64)) { file_close(); return 0; }
  file_close();
  meta->img_w_tiles   = w;
  meta->img_h_tiles   = h;
  meta->img_num_tiles = ntiles;
  meta->flags |= GAMEINFO_FLAG_IMAGE;
  return 1;
}

void gameinfo_load(uint8_t *rom_path) {
  gameinfo_meta_t meta;
  char base[288];
  char path[300];

  memset(&meta, 0, sizeof(meta));
  meta.magic[0] = GAMEINFO_MAGIC0;
  meta.magic[1] = GAMEINFO_MAGIC1;
  meta.status   = GAMEINFO_STATUS_NONE;

  /* build "/sd2snes/info/<stem>" (basename of the ROM, extension stripped) */
  const char *leaf = strrchr((const char *)rom_path, '/');
  leaf = leaf ? leaf + 1 : (const char *)rom_path;
  gi_join(base, sizeof(base), GAMEINFO_DIR, leaf);
  { char *dot = strrchr(base, '.'); if(dot) *dot = 0; }

  /* /sd2snes/info/<stem>.yml */
  gi_join(path, sizeof(path), base, ".yml");
  yaml_file_open(path, FA_READ);
  if(file_res) {
    /* no info for this ROM -> menu boots straight through (status NONE) */
    file_res = 0; /* soft fail */
    sram_writeblock(&meta, SRAM_GAMEINFO_ADDR, sizeof(meta));
    return;
  }

  gi_field("title",        meta.title,        sizeof(meta.title));
  gi_field("developer",    meta.developer,    sizeof(meta.developer));
  gi_field("release_year", meta.year,         sizeof(meta.year));
  gi_field("players",      meta.players,      sizeof(meta.players));
  gi_field("genre",        meta.genre,        sizeof(meta.genre));
  gi_field("special_chip", meta.special_chip, sizeof(meta.special_chip));
  gi_field("description",  meta.description,  sizeof(meta.description));
  yaml_file_close();

  /* title fallback: the ROM stem (no extension; base is ".../<stem>"); "-" for
   * other empty fields */
  if(!meta.title[0])
    gi_utf8_to_font(base + (sizeof(GAMEINFO_DIR) - 1), meta.title, sizeof(meta.title));
  gi_dash(meta.developer);
  gi_dash(meta.year);
  gi_dash(meta.players);
  gi_dash(meta.genre);
  gi_dash(meta.special_chip);

  /* stage the composited DirectColor image (fail-safe; missing -> no image) */
  gi_join(path, sizeof(path), base, "/image.gd");
  gi_load_image(path, &meta);

  meta.status = GAMEINFO_STATUS_OK;
  sram_writeblock(&meta, SRAM_GAMEINFO_ADDR, sizeof(meta));
}

#else /* CONFIG_MK2: flash is too tight for the parser. Stub that always reports
       * "no info" so the (fail-safe) SNES side skips the screen and boots. */

void gameinfo_load(uint8_t *rom_path) {
  (void)rom_path;
  gameinfo_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.magic[0] = GAMEINFO_MAGIC0;
  meta.magic[1] = GAMEINFO_MAGIC1;
  meta.status   = GAMEINFO_STATUS_NONE;
  sram_writeblock(&meta, SRAM_GAMEINFO_ADDR, sizeof(meta));
}

#endif /* CONFIG_MK2 */
