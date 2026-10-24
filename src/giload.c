/* The info screen's loader (SNES_CMD_GAME_INFO*, gameinfo_load) and the full-description
 * pump (gameinfo_desc_full): the .yml fields, the localized description scan, the cover
 * transcode and the CJK glyph sheet.  Menu-only, card overlay code (see ovl.h); moved out of
 * gameinfo.c to go to the card.  The clip pump, the ROM CRC job and the glyph tables stay
 * resident (gameinfo.c): the menu loop runs them between commands. */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cfg.h"
#include "yaml.h"
#include "cover.h"
#include "msu1.h"
#include "gameinfo.h"
#include "psram_io.h"
#include "util.h"
#include "dirindex.h"
#include "scratch.h"
#include "cjkglyph.h"
#include "gameinfo_int.h"

extern cfg_t CFG;

static unsigned gi_value_scan(const char *key, char *mem, unsigned memcap);

/* Draw every indexed ideograph of the game into SRAM_GAMEINFO_CJK_ADDR: one sequential pass
 * over the sorted font file, merged with the sorted indices. A glyph the font lacks stays
 * blank. Bounded by the file; fail-safe (no file = blank glyphs). */
typedef struct {
  uint16_t cp[GI_CJK_MAX];               /* the indices sorted by code point... */
  uint8_t  idx[GI_CJK_MAX];              /* ...and which index each one is */
  uint8_t  rec[10];
  uint8_t  tile[64];
} gi_cjk_scratch_t;
SCRATCH_FITS(gi_cjk_scratch_t, SCRATCH_LEAF_BYTES);

static void gi_cjk_sheet(void) {
  if(!gi_cjk_code || !gi_cjk_n) return;
  if(!scratch_leaf_take(SCR_GI_CJK)) return;
  gi_cjk_scratch_t *s = SCRATCH_LEAF(gi_cjk_scratch_t);
  int n = gi_cjk_n;
  for(int k = 0; k < n; k++) {           /* insertion sort, n <= 128 */
    int j = k;
    while(j > 0 && s->cp[j - 1] > gi_cjk_cp[k]) { s->cp[j] = s->cp[j - 1]; s->idx[j] = s->idx[j - 1]; j--; }
    s->cp[j] = gi_cjk_cp[k];
    s->idx[j] = (uint8_t)k;
  }
  memset(s->tile, 0, sizeof(s->tile));    /* blank first: a glyph the file lacks stays blank */
  for(int k = 0; k < n; k++)
    sram_writeblock(s->tile, SRAM_GAMEINFO_CJK_ADDR + (uint32_t)k * 64, 64);
  char path[] = "/sd2snes/lang/xx.fnt";
  path[14] = gi_cjk_code[0];
  path[15] = gi_cjk_code[1];
  file_open((const uint8_t *)path, FA_READ);
  if(!file_res) {
    UINT br;
    int want = 0;
    if(f_read(&file_handle, s->rec, 8, &br) == FR_OK && br == 8 && !memcmp(s->rec, "SDF1", 4)) {
      unsigned count = s->rec[4] | (s->rec[5] << 8);
      for(unsigned r = 0; r < count && want < n; r++) {
        if(f_read(&file_handle, s->rec, 10, &br) != FR_OK || br != 10) break;
        uint16_t cp = (uint16_t)(s->rec[0] | (s->rec[1] << 8));
        while(want < n && s->cp[want] < cp) want++;           /* not in the font */
        if(want < n && s->cp[want] == cp) {
          cjk_render_glyph(s->rec + 2, s->tile);
          sram_writeblock(s->tile, SRAM_GAMEINFO_CJK_ADDR + (uint32_t)s->idx[want] * 64, 64);
          want++;
        }
      }
    }
    file_close();
  }
  file_res = 0;                          /* soft: the text still shows, the glyphs blank */
  scratch_leaf_drop();
}

static void gi_cjk_reset(void) {
  gi_cjk_n = 0;
  gi_cjk_code = CFG.language == 8 ? "ja" : CFG.language == 9 ? "zh" : NULL;
}

/* Copy src -> dst (NUL-terminated, bounded), decoding UTF-8 to font byte codes.
 * Plain ASCII is copied verbatim; mapped accents and Cyrillic become their font codes;
 * anything else becomes '?'. Bounded by dstsize; safe on truncated/invalid UTF-8. */
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
    uint8_t fb[3];
    int nf = gi_cp_emit(cp, fb);
    for(int k = 0; k < nf && di < dstsize - 1; k++) dst[di++] = (char)fb[k];
  }
  dst[di] = 0;
}

/* Incremental UTF-8 -> font transcoder state, for streaming a value that may split a
 * multi-byte sequence across f_gets chunks (gameinfo_desc_full). cont = continuation bytes
 * still expected (0 = between codepoints); cp = the sequence accumulator. */
typedef struct { uint32_t cp; int cont; } gi_font_state_t;

/* Feed one input byte; write 0..3 font bytes to `out` and return the count. Byte-for-byte
 * equivalent to gi_utf8_to_font (ASCII verbatim, codepoints via gi_cp_emit, else '?'), but
 * stateful so it survives chunk boundaries. A byte that breaks an in-progress sequence emits
 * '?' for the truncated codepoint and is then reprocessed as a fresh lead/ASCII byte (so up
 * to 2 outputs there; a completed codepoint's stand-in is up to 3). Call gi_font_flush after
 * the last byte to emit a trailing '?' for a value that ended mid-sequence. */
static int gi_font_feed(gi_font_state_t *st, unsigned char c, uint8_t *out) {
  int n = 0;
  if(st->cont) {
    if((c & 0xC0) == 0x80) {                 /* valid continuation byte */
      st->cp = (st->cp << 6) | (c & 0x3F);
      if(--st->cont == 0) n += gi_cp_emit(st->cp, out + n);
      return n;
    }
    out[n++] = '?';                          /* truncated sequence -> '?' ... */
    st->cont = 0;                            /* ... then reprocess c below */
  }
  if(c < 0x80) { out[n++] = (uint8_t)c; return n; }   /* ASCII verbatim */
  if((c & 0xE0) == 0xC0)      { st->cp = c & 0x1F; st->cont = 1; }
  else if((c & 0xF0) == 0xE0) { st->cp = c & 0x0F; st->cont = 2; }
  else if((c & 0xF8) == 0xF0) { st->cp = c & 0x07; st->cont = 3; }
  else out[n++] = '?';                       /* stray continuation / invalid lead */
  return n;
}

/* Emit a trailing '?' if the stream ended mid-sequence (matches gi_utf8_to_font treating the
 * NUL terminator as a bad continuation byte). Returns the count (0 or 1). */
static int gi_font_flush(gi_font_state_t *st, uint8_t *out) {
  if(st->cont) { st->cont = 0; out[0] = '?'; return 1; }
  return 0;
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

/* The `.yml` key that carries the description in the MENU language. English (CFG.language 0) is
 * the canonical `description:`; every other language rides a sibling `description_<code>:` key
 * written next to it. The index order MUST match cfg.h (0: English, 1: Portugues BR, 2: Spanish,
 * 3: German, 4: French, 5: Italian, 6: Russian, 7: Dutch) and the codes the info generator emits. NULL = use the plain
 * `description:` (English, and any out-of-range value -- cfg_load clamps, but never trust it here).
 * A missing/empty localized key falls back to English, so a card written before this existed (or a
 * game with no translation) keeps working unchanged. */
static const char *gi_desc_lang_key(void) {
  static const char *const keys[] = {
    NULL, "description_pt", "description_es", "description_de", "description_fr", "description_it",
    "description_ru", "description_nl", "description_ja", "description_zh",
  };
  return (CFG.language < sizeof(keys) / sizeof(keys[0])) ? keys[CFG.language] : NULL;
}

/* leave a single "-" placeholder for an empty metadata field (the SNES then
 * prints every field unconditionally - no empty check needed there). */
static void gi_dash(char *field) {
  if(!field[0]) { field[0] = '-'; field[1] = 0; }
}

/* Load the standalone /sd2snes/info/<rom>.gcv (paletted 120c cover, DECOUPLED from the .fmv):
 * validate the header, stream the palette into SRAM_GAMEINFO_TMAP_ADDR ($CB0000 -> the SNES DMAs it
 * to CGRAM 48..167) and the 8bpp cover tiles into SRAM_COVER_ADDR (bank C9). Returns 1 on success.
 * Bounded/fail-safe; on any error the cover region just shows the gradient. */
static int gi_load_gcv(const char *path) {
  uint8_t hdr[GCV_HEADER_SIZE];
  UINT got;
  file_open((uint8_t *)path, FA_READ);
  if(file_res) return 0;
  file_res = f_read(&file_handle, hdr, GCV_HEADER_SIZE, &got);
  if(file_res || got != GCV_HEADER_SIZE
     || hdr[0] != GCV_MAGIC0 || hdr[1] != GCV_MAGIC1 || hdr[2] != GCV_VERSION
     || hdr[4] != GCV_W || hdr[5] != GCV_H) {
    file_close(); return 0;
  }
  /* file order: header(8), palette (GCV_PAL_BYTES), tiles (GCV_TILE_BYTES) */
  if(!psram_stream(&file_handle, SRAM_GAMEINFO_TMAP_ADDR, GCV_PAL_BYTES,  0)
  || !psram_stream(&file_handle, SRAM_COVER_ADDR,         GCV_TILE_BYTES, 0)) {
    file_close(); return 0;
  }
  file_close();
  return 1;
}

/* Transcode a just-staged 4bpp OBJ <rom>.cov (at scratch_base, COVER_OFF_* layout from load_cover)
 * into the SAME paletted 8bpp BG layout a real .gcv produces, so a cover that only exists as a
 * browser .cov still shares the band with the .fmv/.gss screenshot. OBJ palettes are hardwired to
 * CGRAM 128..255 and would clash with the screenshot (CGRAM 168..255); a paletted BG cover lives in
 * CGRAM 48..167 (the .gcv range), clear of both the text (0..47) and the screenshot (168..255).
 *
 * Palette packing fits EXACTLY, no dedup: each 4bpp palette P (0..7) has 15 visible colours (entry
 * 0 = transparent, skipped) -> 8*15 = 120 = the cover region (CGRAM 48..167). A pixel of 4bpp value
 * V in a 16x16 block using palette P maps to the 8bpp value 0 (V==0, transparent) or 48+P*15+(V-1).
 * The (2*w_spr)x(2*h_spr)-tile cover is centred in the 16x16 region; the border cells are blanked.
 *
 * Writes the 120-colour palette to SRAM_GAMEINFO_TMAP_ADDR ($CB0000 -> CGRAM 48) and 256 8bpp tiles
 * to SRAM_COVER_ADDR ($C90000 -> window-0), matching gi_load_gcv. Bounded (fixed 256-cell loop, tiles
 * read on demand); returns 1 on success. The caller then sets GAMEINFO_FLAG_COVER (the .gcv path). */
/* ~560 B of scratch, in the shared LEAF region (scratch.h, AHB SRAM) -- NOT plain .bss on the
   main SRAM. As plain statics these 560 B shrank the tiny LPC1756 main-SRAM budget (which also
   holds the stack) just enough that the USB command server went silent on real hardware (INFO/PUT
   returned 0 bytes; menu still worked). Touched ONLY here (SPI PIO, never from an IRQ). All three
   are fully written before read (covpal/blockmap via sram_readblock, gcvpal via memset). */
typedef struct {
  uint8_t covpal[COVER_MAX_PALETTES * 32];               /* up to 8*16 BGR555 = 256 B */
  uint8_t gcvpal[GCV_PAL_BYTES];                         /* 120 colours = 240 B -> CGRAM 48..167 */
  uint8_t blockmap[COVER_OBJ_MAX_W * COVER_OBJ_MAX_H];   /* one palette idx / sprite, <= 64 */
} gi_cov_scratch_t;
SCRATCH_FITS(gi_cov_scratch_t, SCRATCH_LEAF_BYTES);
#define covpal   (SCRATCH_LEAF(gi_cov_scratch_t)->covpal)
#define gcvpal   (SCRATCH_LEAF(gi_cov_scratch_t)->gcvpal)
#define blockmap (SCRATCH_LEAF(gi_cov_scratch_t)->blockmap)

static int gi_cov_to_gcv_body(uint32_t scratch_base) {
  uint8_t meta[COVER_META_SIZE];

  sram_readblock(meta, scratch_base + COVER_OFF_STATUS, COVER_META_SIZE);
  if(meta[0] != COVER_STATUS_OK) return 0;
  unsigned w_spr = meta[1], h_spr = meta[2], npal = meta[3];
  if(w_spr == 0 || w_spr > COVER_OBJ_MAX_W || h_spr == 0 || h_spr > COVER_OBJ_MAX_H
     || npal == 0 || npal > COVER_MAX_PALETTES) return 0;

  /* palette: each OBJ palette's colours 1..15 -> CGRAM 48 + P*15 + (c-1) */
  memset(gcvpal, 0, sizeof(gcvpal));
  sram_readblock(covpal, scratch_base + COVER_OFF_PAL, (uint16_t)(npal * 32));
  for(unsigned p = 0; p < npal; p++) {
    for(unsigned c = 1; c < 16; c++) {
      unsigned di = (p * 15 + (c - 1)) * 2;
      unsigned si = (p * 16 + c) * 2;
      gcvpal[di]     = covpal[si];
      gcvpal[di + 1] = covpal[si + 1];
    }
  }
  sram_writeblock(gcvpal, SRAM_GAMEINFO_TMAP_ADDR, GCV_PAL_BYTES);

  sram_readblock(blockmap, scratch_base + COVER_OFF_BLOCKMAP, (uint16_t)(w_spr * h_spr));

  /* tiles: the cover centred in the 16x16 region, border transparent. The .cov tile name grid is
   * 16 wide, so the cover-local cell (cr,cc) is exactly .cov tile index cr*16+cc. */
  int start_col = 8 - (int)w_spr;   /* (16 - 2*w_spr)/2 in tiles */
  int start_row = 8 - (int)h_spr;
  for(int R = 0; R < 16; R++) {
    for(int C = 0; C < 16; C++) {
      uint8_t out[64];
      memset(out, 0, sizeof(out));
      int cr = R - start_row, cc = C - start_col;
      if(cr >= 0 && cr < 2 * (int)h_spr && cc >= 0 && cc < 2 * (int)w_spr) {
        uint8_t in[32];
        sram_readblock(in, scratch_base + COVER_OFF_TILES + (uint32_t)(cr * 16 + cc) * 32, 32);
        unsigned p = blockmap[(cr >> 1) * w_spr + (cc >> 1)];
        if(p >= npal) p = 0;                         /* defensive: bad blockmap index */
        unsigned base = 47 + p * 15;                 /* 8bpp value = base + V (V = 1..15) */
        for(int row = 0; row < 8; row++) {
          uint8_t p0 = in[2*row], p1 = in[2*row+1], p2 = in[16+2*row], p3 = in[17+2*row];
          for(int x = 0; x < 8; x++) {
            uint8_t bit = (uint8_t)(0x80 >> x);
            unsigned v = ((p0 & bit) ? 1u : 0u) | ((p1 & bit) ? 2u : 0u)
                       | ((p2 & bit) ? 4u : 0u) | ((p3 & bit) ? 8u : 0u);
            if(!v) continue;                          /* transparent pixel -> 8bpp 0 */
            unsigned val = base + v;                  /* 48..167 */
            if(val & 0x01) out[2*row]    |= bit;
            if(val & 0x02) out[2*row+1]  |= bit;
            if(val & 0x04) out[16+2*row] |= bit;
            if(val & 0x08) out[17+2*row] |= bit;
            if(val & 0x10) out[32+2*row] |= bit;
            if(val & 0x20) out[33+2*row] |= bit;
            if(val & 0x40) out[48+2*row] |= bit;
            if(val & 0x80) out[49+2*row] |= bit;
          }
        }
      }
      sram_writeblock(out, SRAM_COVER_ADDR + (uint32_t)(R * 16 + C) * 64, 64);
    }
  }
  return 1;
}
#undef covpal
#undef gcvpal
#undef blockmap

static int gi_cov_to_gcv(uint32_t scratch_base) {
  int r;
  if(!scratch_leaf_take(SCR_GI_COV)) return 0;
  r = gi_cov_to_gcv_body(scratch_base);
  scratch_leaf_drop();
  return r;
}

/* gameinfo_load's working set, in the shared FRAME scratch (scratch.h) -- not the stack (the menu
 * loop is single-threaded and non-reentrant, and the LPC stack is tight) and not .bss (main SRAM
 * is tight; growing it can silently corrupt a global). Every call's first access is a write
 * (memset of meta; gi_join builds path, then base from path, before either is read). */
typedef struct {
  gameinfo_meta_t meta;
  char            base[288];
  char            path[300];
} gi_load_frame_t;
SCRATCH_FITS(gi_load_frame_t, SCRATCH_FRAME_BYTES);

void gameinfo_load(uint8_t *rom_path) {
  gi_load_frame_t *const f = SCRATCH_FRAME(gi_load_frame_t);
  gameinfo_crc_on = 0;                       /* a CRC still running owned the FRAME */
  /* Silence the previous game's clip FIRST. Everything below reads the card (the .yml, the
   * localized description scan, the cover transcode) without pumping the DAC, and a clip left
   * playing through that loops the last 2 KB of its buffer -- the "zeet" heard on every Up/Down
   * between two games that both have a preview soundtrack. */
  gi_fmv_close();                            /* drop any prior open .fmv (reentry) */
  gi_fmv_path[0] = 0;                         /* invalidate the saved reopen path */
  menu_music_stop();                         /* and any prior FMV audio clip */
  int fmv_eligible = 1;                 /* only probe <rom>.fmv if the .yml declares "fmv:" (or
                                         * there is no .yml). Skips a full scan of the (huge)
                                         * info dir for the 99% of games that have no video. */

  gi_cjk_reset();                        /* this game's ideographs: none yet */
  memset(&f->meta, 0, sizeof(f->meta));
  f->meta.magic[0] = GAMEINFO_MAGIC0;
  f->meta.magic[1] = GAMEINFO_MAGIC1;
  /* The screen always shows when ShowGameInfo is on: a ROM with no .yml still
   * gets a title (its filename), "-" metadata, and -- if a sibling <rom>.cov
   * exists -- the OBJ box-art floated in the band where the .gd cover would be. */
  f->meta.status   = GAMEINFO_STATUS_OK;

  /* build "/sd2snes/info/[<ns>/]<BB>/<stem>" (namespace + bucket, extension stripped). stem_off is where
   * <stem> starts -- keep it instead of recomputing the prefix width later. */
  int stem_off = path_asset(f->base, sizeof(f->base), GAMEINFO_DIR, (const char *)rom_path, "");
  if(stem_off < 0) stem_off = 0;
  /* Every sidecar below is opened from this one directory, which on a real card can hold
     thousands of files: index it once, and each open below (and the guides probe) costs no scan */
  if(stem_off > 1) {
    f->base[stem_off - 1] = 0;
    dirindex_prepare(f->base);
    f->base[stem_off - 1] = '/';
  }

  /* /sd2snes/info/<stem>.yml -- now OPTIONAL: a missing .yml is no longer a skip,
   * it just leaves every field empty (filled by the fallbacks below). */
  gi_join(f->path, sizeof(f->path), f->base, ".yml");
  /* save the .yml path for the "full description" (Y) command (gameinfo_desc_full), and
     invalidate the extended-description region so navigating Up/Down between ROMs never
     leaves a previous game's full text behind (a 1st byte of 0 = invalid; the menu then
     uses the struct's description[256]). */
  strlcpy_nul(gi_yml_path, f->path, sizeof(gi_yml_path));
  sram_writebyte(0, SRAM_GAMEINFO_DESCEXT_ADDR);
  yaml_file_open(f->path, FA_READ);
  /* The title is read at full length (GI_TITLEX_LEN) into f->path, free from here on (the .yml
     path is in gi_yml_path), and cut down to meta.title further below; the title row scrolls
     the long one. */
  char *const lt = f->path;
  lt[0] = 0;
  if(!file_res) {
    gi_field("title",        lt,                   GI_TITLEX_LEN);
    gi_field("developer",    f->meta.developer,    sizeof(f->meta.developer));
    gi_field("publisher",    f->meta.publisher,    sizeof(f->meta.publisher));
    gi_field("release_year", f->meta.year,         sizeof(f->meta.year));
    gi_field("players",      f->meta.players,      sizeof(f->meta.players));
    gi_field("genre",        f->meta.genre,        sizeof(f->meta.genre));
    gi_field("special_chip", f->meta.special_chip, sizeof(f->meta.special_chip));
    /* description: English (description) first, as the fallback; the MENU language
     * (description_<code>) replaces it below when present and non-empty. */
    gi_field("description", f->meta.description, sizeof(f->meta.description));
    { yaml_token_t tok; fmv_eligible = yaml_get_itemvalue("fmv", &tok) ? 1 : 0; }
    yaml_file_close();
    /* The localized keys are written LAST in the file, after the other long description lines,
     * and the YAML parser cannot read past a line longer than YAML_BUFLEN: f_gets hands back the
     * rest of such a line as if it were a new one, a continuation parsed as a key swallows the
     * next real line as its value, and description_<code> was then never found -- the screen
     * showed English while Y (gi_value_scan, which only matches a key at the start of a PHYSICAL
     * line) showed the translation. Read it with that same scanner, after the parser is closed
     * (both use the shared file handle). Absent/empty -> meta.description keeps the English. */
    {
      const char *lkey = gi_desc_lang_key();
      if(lkey) gi_value_scan(lkey, f->meta.description, sizeof(f->meta.description));
      if(gi_cjk_code) {              /* a CJK menu: the game's own-language title, when given */
        char tkey[] = "title_xx";
        tkey[6] = gi_cjk_code[0];
        tkey[7] = gi_cjk_code[1];
        gi_value_scan(tkey, lt, GI_TITLEX_LEN);
      }
      file_res = 0;                  /* soft, like a missing .yml: the English stays */
    }
  } else {
    file_res = 0; /* soft fail: no .yml is fine; fmv_eligible stays 1 (probe .fmv as before) */
  }

  /* title fallback: the ROM stem (base is ".../<BB>/<stem>"); "-" for other empty fields.
   * Applied unconditionally so the .yml-less screen is filled. Uses the offset path_asset
   * returned -- the old code hardcoded sizeof(GAMEINFO_DIR)-1+2 for a ONE-char bucket, which is
   * exactly the kind of arithmetic that silently shifts when the layout changes. */
  if(!lt[0])
    gi_utf8_to_font(f->base + stem_off, lt, GI_TITLEX_LEN);
  sram_writeblock(lt, SRAM_GAMEINFO_TITLEX_ADDR, GI_TITLEX_LEN);   /* past the NUL: unread */
  strlcpy_nul(f->meta.title, lt, sizeof(f->meta.title));          /* for an older menu */
  gi_dash(f->meta.developer);
  gi_dash(f->meta.publisher);
  gi_dash(f->meta.year);
  gi_dash(f->meta.players);
  gi_dash(f->meta.genre);
  gi_dash(f->meta.special_chip);

  /* band: paletted cover (left) + paletted screenshot/animation (.fmv clip / .gss snapshot,
   * right), each its own file into its own CGRAM range so they coexist (cover = CGRAM 48..167,
   * screenshot = 168..255, text = 0..47). The cover comes from a real <rom>.gcv, or -- when there
   * is none -- is transcoded on the fly from the browser <rom>.cov into the SAME paletted BG
   * layout (gi_cov_to_gcv) so it still shares the band with the screenshot. All bounded +
   * fail-safe; if nothing exists the band is gradient. The .fmv/.gss f_open scans the (huge) info
   * dir, so it is gated behind the .yml "fmv:" flag. */
  {
    /* DECOUPLED paletted band: the cover (left) and the screenshot/FMV region (right) are
     * SEPARATE files, each into its own CGRAM range. The right region comes from EITHER the animated
     * clip (.fmv) OR the static snapshot (.gss) -- two files, so a future "no preview clip" toggle can
     * fall back to the snapshot. Either region absent -> gradient. */
    gi_join(f->path, sizeof(f->path), f->base, ".gcv");
    if(gi_load_gcv(f->path)) {
      f->meta.flags |= GAMEINFO_FLAG_COVER;                    /* cover -> C9 + CGRAM 48..167 */
    } else if(load_cover(rom_path, SRAM_GAMEINFO_TILES_ADDR)   /* stage the 4bpp .cov to scratch (bank CA,
                                                               * reused by the FMV AFTER this) */
              && gi_cov_to_gcv(SRAM_GAMEINFO_TILES_ADDR)) {    /* transcode it into the paletted BG cover */
      f->meta.flags |= GAMEINFO_FLAG_COVER;                    /* same CGRAM-48..167 BG path as a real .gcv */
    }
    if(fmv_eligible) {
      int shown = 0;
      /* the animated clip (.fmv) is gated by the "Show video" toggle; off -> static snapshot below */
      if(CFG.game_info_video) {
        gi_join(f->path, sizeof(f->path), f->base, ".fmv");
        if(gi_fmv_begin(f->path, &f->meta)) {  /* sets GAMEINFO_FLAG_FMV; N frames -> animated */
          shown = 1;
          if(CFG.game_info_music) {          /* clip soundtrack gated by the "Play video music" toggle */
            gi_join(f->path, sizeof(f->path), f->base, ".qoa");   /* compressed soundtrack first */
            if(menu_music_play(f->path) != 0xA0) {
              gi_join(f->path, sizeof(f->path), f->base, ".pcm");
              menu_music_play(f->path);      /* clip audio; silent if absent */
            }
          }
        }
      }
      if(!shown) {                           /* video off / clip absent: the static snapshot (.gss, 1 frame) */
        gi_join(f->path, sizeof(f->path), f->base, ".gss");
        gi_fmv_begin(f->path, &f->meta);     /* sets GAMEINFO_FLAG_FMV; 1 frame -> the pump no-ops */
      }
    }
  }

  gi_cjk_sheet();                        /* the glyphs the text above indexed */
  sram_writeblock(&f->meta, SRAM_GAMEINFO_ADDR, sizeof(f->meta));
  gi_crc_start(rom_path);                /* takes the FRAME over: last */
}

/* Scan the last-loaded .yml for `key:` and stage its COMPLETE value, font-encoded, into
 * SRAM_GAMEINFO_DESCEXT_ADDR -- or, with `mem`, into that buffer (at most memcap-1 bytes + NUL).
 * Returns the number of font bytes staged (0 = key absent, empty, or any error -- the SRAM region
 * is then left invalid, 1st byte 0; `mem` is then left UNTOUCHED, so a caller can pre-fill it with
 * a fallback). Bounded + fail-safe: never hangs the
 * menu loop. Matches the generator's format -- one physical line per field, the value is either
 * double-quoted (terminates at the next '"', which is always the closer since inner quotes were
 * rewritten to ''') or bare (terminates at end-of-line / EOF). */
/* LEAF scratch (scratch.h): off the tight main SRAM. Fully written before read; touched only
   here (menu-loop, never from an IRQ). */
typedef struct {
  char    chunk[256];                    /* one f_gets line-piece */
  uint8_t obuf[128];                     /* font-encoded output, flushed in bursts */
} gi_scan_scratch_t;
SCRATCH_FITS(gi_scan_scratch_t, SCRATCH_LEAF_BYTES);
#define chunk (SCRATCH_LEAF(gi_scan_scratch_t)->chunk)
#define obuf  (SCRATCH_LEAF(gi_scan_scratch_t)->obuf)

static unsigned gi_value_scan_body(const char *key, char *mem, unsigned memcap) {
  gi_font_state_t st = { 0, 0 };
  uint32_t out_addr = SRAM_GAMEINFO_DESCEXT_ADDR;
  uint32_t scanned  = 0;
  unsigned out_total = 0;                /* font bytes staged (excl. NUL); cap below */
  const unsigned cap = mem ? memcap - 1 : GAMEINFO_DESCEXT_LEN - 1;
  unsigned ob = 0;                       /* bytes buffered in obuf */
  int at_line_start = 1;                 /* the next chunk begins a physical line */
  int in_value = 0;                      /* streaming the description value */
  int quoted   = 0;                      /* value opened with '"' */
  int done     = 0;

  /* 1) invalidate first: if we find nothing, the menu falls back to description[256]. */
  if(!mem) sram_writebyte(0, SRAM_GAMEINFO_DESCEXT_ADDR);
  if(!gi_yml_path[0] || (mem && !memcap)) return 0;

  /* 2) open the last-loaded .yml with the shared handle (free during the info screen; the
   *    FMV has its own gi_fmv_fil). Any error -> return (region stays invalid). */
  file_open((const uint8_t *)gi_yml_path, FA_READ);
  if(file_res) return 0;

  /* 3) scan lines. A chunk begins a physical line only if the previous chunk ended in '\n';
   *    a value that overflows one f_gets buffer continues in the next chunk, and the key must
   *    NOT be matched against such a continuation. Bounded by a 64 KB scan cap on top of EOF
   *    so a pathological file can never spin the loop (the cap has to clear a `.yml` carrying
   *    one description per menu language, with the localized ones written last). */
  while(!done && scanned < 64u * 1024u
        && f_gets(chunk, sizeof(chunk), &file_handle)) {
    int this_start = at_line_start;
    const char *p = chunk;
    unsigned len = (unsigned)strlen(chunk);
    scanned += len;
    at_line_start = (len && chunk[len - 1] == '\n');   /* else the line continues */

    if(!in_value) {
      const char *kk = key;
      if(!this_start) continue;                        /* continuation of a long line */
      while(*p == ' ' || *p == '\t') p++;              /* optional leading indent */
      while(*kk && *p == *kk) { p++; kk++; }
      /* the ':' is part of the match: without it "description" would also swallow the
       * "description_pt:" line (one key is a prefix of the other). */
      if(*kk || *p != ':') continue;                   /* not the line we want */
      p++;                                             /* eat the ':' */
      while(*p == ' ' || *p == '\t') p++;              /* skip spaces before the value */
      in_value = 1;
      if(*p == '"') { quoted = 1; p++; }               /* quoted -> closes at next '"' */
    }

    /* stream the value bytes of this chunk (the rest of a matched line, or a whole
     * continuation chunk). Transcode incrementally so a UTF-8 sequence split across chunks
     * survives; flush to SRAM in bursts. */
    for(; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if(quoted) {
        if(c == '"') { done = 1; break; }              /* closing quote */
        if(c == '\n' || c == '\r') continue;           /* never meaningful inside quotes */
      } else if(c == '\n' || c == '\r') {
        done = 1; break;                               /* bare value ends at EOL */
      }
      uint8_t fo[3];
      int nf = gi_font_feed(&st, c, fo);
      for(int i = 0; i < nf; i++) {
        if(out_total >= cap) { done = 1; break; }
        if(mem) { mem[out_total++] = (char)fo[i]; continue; }
        obuf[ob++] = fo[i];
        out_total++;
        if(ob == sizeof(obuf)) { sram_writeblock(obuf, out_addr, (uint16_t)ob); out_addr += ob; ob = 0; }
      }
      if(done) break;
    }
  }

  /* flush a trailing '?' for a value that ended mid-sequence (fidelity with gi_utf8_to_font),
   * then drain the buffer and terminate. */
  if(out_total < cap) {
    uint8_t fo[1];
    if(gi_font_flush(&st, fo)) {
      if(mem) mem[out_total++] = (char)fo[0];
      else { obuf[ob++] = fo[0]; out_total++; }
    }
  }
  if(mem) {
    if(out_total) mem[out_total] = 0;    /* empty -> keep the caller's fallback text */
  } else {
    if(ob) { sram_writeblock(obuf, out_addr, (uint16_t)ob); out_addr += ob; }
    sram_writebyte(0, out_addr);         /* NUL terminator (re-zeroes byte 0 if empty) */
  }
  file_close();
  return out_total;
}
#undef chunk
#undef obuf

static unsigned gi_value_scan(const char *key, char *mem, unsigned memcap) {
  unsigned r;
  if(!mem) sram_writebyte(0, SRAM_GAMEINFO_DESCEXT_ADDR);   /* the body's step 1, for a refusal */
  if(!scratch_leaf_take(SCR_GI_SCAN)) return 0;
  r = gi_value_scan_body(key, mem, memcap);
  scratch_leaf_drop();
  return r;
}

/* "Full description" (Y) pump. The YAML parser caps a value at YAML_BUFLEN (256), so the struct's
 * description[256] is truncated; this stages the whole text. Same language choice as
 * gameinfo_load: the menu language first, English as the fallback -- so what Y opens is always the
 * text the screen was already showing. Two passes at worst (one per key), each bounded and
 * fail-safe; on failure the region stays invalid and the menu keeps the 256-char copy. */
void gameinfo_desc_full(void) {
  const char *lkey = gi_desc_lang_key();
  if(!(lkey && gi_value_scan(lkey, NULL, 0)))        /* localized text staged, else... */
    gi_value_scan("description", NULL, 0);           /* English (also re-invalidates on failure) */
  gi_cjk_sheet();                                    /* glyphs the full text added */
}
