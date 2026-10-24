/* sd2snes (ludufre fork) - menu THEME loader (MCU side). See theme.h.
 *
 * .thm format (little-endian), produced by utils/pack_theme.py:
 *     off  size  field
 *     0    8     magic  "FXTHEME1"
 *     8    1     version (=THEME_VERSION)
 *     9    1     N = number of regions
 *     10   2     flags  (bit0 = has logo; bit1 = outline off; bit2 = AA off,
 *                        see the font remap notes below)
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
#include "uart.h"     /* printf(): see the note in uart.h -- every printf caller includes this */
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cfg.h"
#include "theme.h"
#include "psram_io.h"

extern cfg_t CFG;

static const char THEME_MAGIC[8]  = { 'F','X','T','H','E','M','E','1' };
static const char GFXPTR_MAGIC[8] = { '_','G','F','X','P','T','R','_' };
#define THEME_VERSION   1
#define THEME_NSLOTS    9

/* Font edge remaps: .thm header flags bit1/bit2 ask the MCU to edit the menu
   font's edge pixels, matching what the official editor does to the tiles it
   bakes into a menu.bin -- but WITHOUT the .thm carrying the font. The 2bpp
   font (slot 0) uses pixel value 1 = fill, 2 = the 1px outline ring, 3 = the
   anti-alias step between them:
     bit1 outline off: value 2 -> 0 (transparent), so the HDMA backdrop
                       gradient shows through where the ring was;
     bit2 AA off:      value 3 -> 1 (fill), so the glyph edge is hard.
   Both are applied to the PSRAM copy before the SNES runs its genfonts DMA;
   font.a65 and the ROM are untouched. Flags clear -> no-op.
   NB: the AA-off remap makes the edge hard regardless of the palette. The
   Theme Creator ALSO writes shade 0x03 as the group's own text colour for the
   same effect on firmware that predates this flag; the two agree, so a theme
   built either way looks the same here.

   The same two remaps are also user options (CFG.text_outline /
   CFG.text_antialias), and those apply with OR without a theme. The two sources
   compose by OR -- the user toggle only ever turns an edge OFF in addition to
   what the theme asked for, so a theme that wants no ring keeps its look even
   with the toggles left on. That is why the remap does NOT run inside
   theme_apply anymore: it is theme_font_edges() (called from main.c right after
   theme_apply) that combines the file flags published in theme_font_flags with
   the config, so the "no theme" path gets the toggles too. */
#define SLOT_FONT               0
#define THEME_FLAG_OUTLINE_OFF  0x0002u
#define THEME_FLAG_AA_OFF       0x0004u
#define THEME_FONT_LEN          0x1000u   /* 256 tiles x 16 B == snes/font.a65
                                             asset size == bytes genfonts DMAs.
                                             Kept in sync by pack_theme.py. */
#define THEME_MENU_SIZE         0x30000u  /* snes/Makefile MENU_SIZE (192 KB, 3 banks) */

/* Window of the loaded menu image scanned for the _GFXPTR_ magic. The table's
 * offset shifts with the menu layout: ~0x2EB1 in the old 128px-logo build, but
 * ~0x103C once the header logo went full-width (256px) and const.a65 was
 * reorganised. Start low enough to catch both -- the 8-byte magic won't false-
 * match in code/palette/tile data, and the scan stops at the first hit. */
#define THEME_GFXPTR_SCAN_START  0x0800
#define THEME_GFXPTR_SCAN_END    0x6000

/* Font edge flags of the theme that is live on the current menu image, i.e.
 * (flags & (OUTLINE_OFF|AA_OFF)) of the .thm theme_apply just finished applying.
 * Cleared at the top of every theme_apply so a theme that was removed, skipped
 * or that failed halfway can never leave stale flags behind for
 * theme_font_edges to OR in. */
static uint16_t theme_font_flags;

/* 1 once SRAM_FONT_ORIG_ADDR holds the pristine font of the menu image that is
   currently in PSRAM. Cleared by theme_apply (a fresh image, possibly with a
   different theme font, invalidates the copy). */
static uint8_t theme_font_orig_ok;

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
  14336,  /* 8 logo_tiles (full-width 256x56) */
};

/* Stream `len` bytes from the open theme file, writing the first min(len,cap)
 * of them to PSRAM at `dest`. The full `len` is always consumed so the file
 * stays aligned for the next region (cap==0 -> consume only). Bounded by
 * file_buf; returns 1 on success, 0 on read error/short read.
 * The tail past `cap` is SEEKED over rather than read: theme_apply's atomicity
 * guard has already proven every region fits inside fsize, so the seek can
 * never land past EOF (which f_lseek would silently clamp instead of failing). */
static int theme_stream(uint32_t dest, uint32_t len, uint32_t cap) {
  uint32_t keep = (len > cap) ? cap : len;
  if(!psram_stream(&file_handle, dest, keep, 0)) return 0;
  if(len > keep) {
    file_res = f_lseek(&file_handle, file_handle.fptr + (len - keep));
    if(file_res) return 0;
  }
  return 1;
}

/* The header logo is 8bpp, char-base $300, laid out row-major (tile r*COLS+c)
 * over THEME_LOGO_ROWS rows. The full-width region is 32 cols/row; a legacy
 * (non-[full]) theme carries only the 16-col half. Row width is derived from the
 * region cap at runtime so it tracks the menu geometry (not re-hard-coded). */
#define THEME_SLOT_LOGO_TILES  8
#define THEME_LOGO_ROWS        7

/* Place a half-width (cap/2) legacy logo LEFT-ANCHORED in the full-width region:
 * per row, stream the theme's left 16 cols and blank the right 16 cols with
 * transparent (pixel 0 -> backdrop gradient) tiles. A plain linear copy would
 * re-flow the 16-wide rows into the 32-wide grid and scramble the logo. Reuses
 * theme_stream (bounded read+stage) and sram_memset (bounded fill). */
static int theme_stream_logo_left(uint32_t dest, uint32_t cap) {
  uint32_t row_full = cap / THEME_LOGO_ROWS;   /* 2048: one 32-col row */
  uint32_t row_half = row_full / 2;            /* 1024: 16-col half     */
  for(uint32_t r = 0; r < THEME_LOGO_ROWS; r++) {
    uint32_t rowdst = dest + r * row_full;
    if(!theme_stream(rowdst, row_half, row_half)) return 0;  /* left: theme */
    sram_memset(rowdst + row_half, row_half, 0);             /* right: blank */
  }
  return 1;
}

/* Locate "_GFXPTR_" in the loaded menu PSRAM and read its THEME_NSLOTS word
 * pointers (16-bit bank-$C0 offsets) into gfxptr[]. Also reads the byte after
 * the 9 words (`.byt ^font` in snes/const.a65) into *font_bank -- slot 0 (font)
 * lives in bank $C1, so its .word alone can't carry the bank. Returns 1 on ok. */
static int theme_read_gfxptr(uint16_t gfxptr[THEME_NSLOTS], uint8_t *font_bank) {
  uint8_t buf[512];
  const uint32_t step = sizeof(buf) - 8;   /* overlap so the magic isn't split */
  for(uint32_t off = THEME_GFXPTR_SCAN_START; off < THEME_GFXPTR_SCAN_END; off += step) {
    sram_readblock(buf, SRAM_MENU_ADDR + off, sizeof(buf));
    for(uint32_t i = 0; i + 8 <= sizeof(buf); i++) {
      if(!memcmp(buf + i, GFXPTR_MAGIC, 8)) {
        uint8_t words[THEME_NSLOTS * 2 + 1];   /* 9 words + font bank byte */
        sram_readblock(words, SRAM_MENU_ADDR + off + i + 8, sizeof(words));
        for(int k = 0; k < THEME_NSLOTS; k++)
          gfxptr[k] = (uint16_t)words[k * 2] | ((uint16_t)words[k * 2 + 1] << 8);
        *font_bank = words[THEME_NSLOTS * 2];  /* .byt ^font after the 9 words */
        return 1;
      }
    }
  }
  return 0;
}

/* Font edge remap: the menu font is 2bpp, each 8x8 tile = 8 rows of
 * [low-plane byte, high-plane byte]; pixel value = (high<<1)|low. Only the
 * high plane is touched, so every pixel keeps its low bit:
 *   high &= ~low  ->  value 3 (high=1,low=1) becomes 1 (fill)        [AA off]
 *   high &=  low  ->  value 2 (high=1,low=0) becomes 0 (transparent) [ring off]
 * Values 0 and 1 have high=0 already and survive both. Applying both leaves
 * high = 0 throughout: ring gone, AA folded into the fill -- exactly what the
 * official editor's antiAlias() produces with both boxes unchecked.
 * One pass over the whole THEME_FONT_LEN font: read from `src`, stash the bytes
 * untouched at `save` when that is non-zero, then apply the requested remaps and
 * write the result to `dst`. dst == src remaps in place; src == the pristine copy
 * rebuilds from scratch, which is what makes an edge that was removed come back.
 * Chunked to keep the stack small; the buffer size is even so [low,high] pairs
 * never straddle a chunk boundary. */
static __attribute__((noinline))
void theme_font_pass(uint32_t dst, uint32_t src, uint32_t save,
                     int aa_off, int outline_off) {
  uint8_t buf[512];
  uint32_t rp = src, wp = dst, sp = save, remaining = THEME_FONT_LEN;
  while(remaining) {
    uint16_t chunk = (remaining > sizeof(buf)) ? (uint16_t)sizeof(buf)
                                               : (uint16_t)remaining;
    sram_readblock(buf, rp, chunk);
    if(save) sram_writeblock(buf, sp, chunk);
    for(uint16_t i = 0; i + 1 < chunk; i += 2) {
      uint8_t lo = buf[i], hi = buf[i + 1];
      if(aa_off)      hi &= (uint8_t)~lo;   /* AA step   -> fill        */
      if(outline_off) hi &= lo;             /* ring      -> transparent */
      buf[i + 1] = hi;
    }
    sram_writeblock(buf, wp, chunk);
    rp += chunk;
    wp += chunk;
    sp += chunk;
    remaining -= chunk;
  }
}

/* Open `path` into file_handle and read its `n`-byte header, which must start with
   the 8-byte `magic` and carry `ver` right after it. 0 (and the file closed) when
   the file is missing, short or not of that kind; the .thm and the .sfn share it. */
static __attribute__((noinline)) int theme_open(const char *path, uint8_t *hdr, UINT n,
                                                const char *magic, uint8_t ver) {
  UINT got;
  file_open((const uint8_t*)path, FA_READ);
  if(!file_res) file_res = f_read(&file_handle, hdr, n, &got);
  if(file_res || got != n || memcmp(hdr, magic, 8) || hdr[8] != ver) {
    printf("theme: %s rejected\n", path);
    file_close();
    return 0;
  }
  return 1;
}

void theme_apply(void) {
  const char *skin = (const char*)CFG.skin_name;
  /* Every exit below leaves the font untouched, so start from "no theme flags":
     theme_font_edges then sees the user toggles only. */
  theme_font_flags = 0;
  /* ...and from "no pristine copy": this is a fresh menu image, so whatever
     SRAM_FONT_ORIG_ADDR holds belongs to the previous one. */
  theme_font_orig_ok = 0;
  /* skin_name holds the FULL SD path of the chosen .thm (captured from the
     browser selection, so themes can live in any visible folder). Anything not
     an absolute path -- empty, or the "sd2snes.skin" sentinel -- means
     "no theme / baked-in default look". */
  if(skin[0] != '/') return;

  uint8_t hdr[16];
  UINT got;
  if(!theme_open(skin, hdr, sizeof(hdr), THEME_MAGIC, THEME_VERSION)) return;
  uint16_t flags = (uint16_t)hdr[10] | ((uint16_t)hdr[11] << 8);
  uint8_t n = hdr[9];
  if(n == 0 || n > THEME_NSLOTS) { file_close(); return; }

  uint8_t toc[THEME_NSLOTS * 4];
  file_res = f_read(&file_handle, toc, (UINT)n * 4, &got);
  if(file_res || got != (UINT)n * 4) { file_close(); return; }

  /* atomicity guard: require the full payload up front so a .thm truncated
     after the header can't leave the live menu half-themed (e.g. new palette
     but stale logo tiles). Bail before touching menu PSRAM if the file is short. */
  uint32_t need = 16 + (uint32_t)n * 4;
  for(int r = 0; r < n; r++) {
    need += (uint16_t)toc[r * 4 + 2] | ((uint16_t)toc[r * 4 + 3] << 8);
  }
  if(need > file_handle.fsize) { file_close(); return; }

  uint16_t gfxptr[THEME_NSLOTS];
  uint8_t  font_bank = 0;
  if(!theme_read_gfxptr(gfxptr, &font_bank)) {
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
    /* legacy half-width logo (len == cap/2) on a full-width region: render it
       left-anchored instead of letting a linear copy scramble it. */
    if(slot == THEME_SLOT_LOGO_TILES && (uint32_t)len * 2 == cap) {
      if(!theme_stream_logo_left(SRAM_MENU_ADDR + gfxptr[slot], cap)) {
        printf("theme: logo stream error\n");
        file_close();
        return;
      }
      continue;
    }
    if(!theme_stream(SRAM_MENU_ADDR + gfxptr[slot], len, cap)) {
      printf("theme: stream error slot %u\n", slot);
      file_close();
      return;
    }
  }

  /* Font edge remaps (flags bit1 outline-off / bit2 AA-off) are NOT done here:
     they have to compose with the user toggles, which also apply when there is
     no theme at all. Publish what this file asked for and let theme_font_edges
     (main.c, right after us) do the single remap pass. Published only on the
     success path, so a theme that bailed halfway leaves the flags at 0. */
  theme_font_flags = flags & (THEME_FLAG_OUTLINE_OFF | THEME_FLAG_AA_OFF);

  file_close();
  printf("theme: applied %s\n", skin);
}

/* What the last theme_font_edges() actually put into the PSRAM font, so the menu
   can be reloaded when the user flips a toggle: the remap is destructive and only
   a fresh menu image can bring an edge back. Bit0 = AA off, bit1 = outline off. */
static uint8_t theme_font_applied;

/* One edge, resolved: the option is tri-state (0 follow the theme, 1 force the edge
   ON, 2 force it OFF) rather than a bool, because the remap can only ever REMOVE an
   edge. With a plain on/off the theme had to win -- a .thm asking for outline-off
   left the "on" setting doing nothing visible, which reads as a broken option.
   Returns 1 when the edge has to be remapped away. */
static uint8_t theme_edge_off(uint8_t mode, uint16_t theme_flag) {
  if(mode == TEXT_EDGE_OFF) return 1;
  if(mode == TEXT_EDGE_ON)  return 0;
  return theme_flag ? 1 : 0;              /* TEXT_EDGE_THEME */
}

/* The effective remap state, as the two bits above. */
static uint8_t theme_font_wanted(void) {
  uint8_t w = 0;
  if(theme_edge_off(CFG.text_antialias_mode, theme_font_flags & THEME_FLAG_AA_OFF))      w |= 1;
  if(theme_edge_off(CFG.text_outline_mode,   theme_font_flags & THEME_FLAG_OUTLINE_OFF)) w |= 2;
  return w;
}

int theme_font_edges_stale(void) {
  return theme_font_wanted() != theme_font_applied;
}

/* Resolve the PSRAM address of the menu font, 0 if it cannot be trusted. */
static __attribute__((noinline)) int theme_font_locate(uint32_t *font_addr) {
  /* theme_apply's own scan is local to it and does not run at all when there is
     no theme, so redo it here through the same helper. */
  uint16_t gfxptr[THEME_NSLOTS];
  uint8_t  font_bank = 0;
  if(!theme_read_gfxptr(gfxptr, &font_bank)) {
    printf("theme: _GFXPTR_ not found in menu image\n");
    return 0;
  }
  /* The font is in bank $C1 (the menu; $C2 in the tour, whose $C1 is full) but
     gfxptr[SLOT_FONT] only carries the low word, so add the bank byte from the
     _GFXPTR_ table -- that byte is also what tells a menu with the ABI from an old
     one. Don't test gfxptr[SLOT_FONT] != 0 as a guard: the font sits at offset 0,
     so zero is its real address (that test used to skip the remap on every build). */
  if(font_bank < 0xC0 || font_bank > 0xC2) return 0;
  *font_addr = SRAM_MENU_ADDR
             + (((uint32_t)(font_bank - 0xC0)) << 16)
             + gfxptr[SLOT_FONT];
  return (*font_addr + THEME_FONT_LEN <= SRAM_MENU_ADDR + THEME_MENU_SIZE);
}

void theme_font_edges(void) {
  /* Compose the two sources by OR: the toggle only ever removes an edge on top
     of what the theme asked for (see the font remap notes above). */
  uint8_t  wanted = theme_font_wanted();
  uint32_t font_addr;
  int aa_off      = wanted & 1;
  int outline_off = wanted & 2;
  /* Nothing wanted and nothing ever remapped -> the font in PSRAM is still the
     pristine one, so don't even scan for _GFXPTR_. This is the default state
     (both options on "theme", no theme flags), so the no-theme path costs nothing. */
  if(!wanted && !theme_font_orig_ok) { theme_font_applied = 0; return; }
  /* Record the state up front: whatever this call ends up doing (including the
     bail-outs below), the font in PSRAM is the one this menu image was loaded
     with, and that is what a later stale check has to compare against. */
  theme_font_applied = wanted;
  if(!theme_font_locate(&font_addr)) return;
  if(theme_font_orig_ok) {
    /* Rebuild from the pristine copy. This is the whole point of keeping one:
       the remap only clears bits, so bringing an edge back -- or swapping which
       of the two is gone -- is impossible from the remapped font. Also covers
       wanted == 0, where the pass is a plain restore. */
    theme_font_pass(font_addr, SRAM_FONT_ORIG_ADDR, 0, aa_off, outline_off);
  } else {
    /* First remap for this menu image: the font is still untouched, so capture
       it on the way through instead of paying a second pass for it. */
    theme_font_pass(font_addr, font_addr, SRAM_FONT_ORIG_ADDR, aa_off, outline_off);
    theme_font_orig_ok = 1;
  }
}

/* .sfn: a menu font on its own, picked in the browser like a .thm.  Format:
 *     off  size  field
 *     0    8     magic "SD2SFNT1"
 *     8    1     version (=SFN_VERSION)
 *     9    7     reserved
 *     16   32    mask: bit (t & 7) of byte (t >> 3) set = the file brings tile t
 *     48   4096  the 256 tiles, 2bpp x 16 B, in snes/font.a65 order
 * Written over the font of the just-loaded menu image in PSRAM (menu and tour), so it
 * costs no memory of its own.  The mask is what keeps a font drawn against another
 * build from repainting the icons, window borders and control codes: utils/pack_font.py
 * sets only the text tiles unless told otherwise. */
static const char SFN_MAGIC[8] = { 'S','D','2','S','F','N','T','1' };
#define SFN_VERSION   1
#define SFN_HDR_LEN   48
#define SFN_TILE_LEN  16

void theme_font_file(void) {
  const char *path = (const char*)CFG.font_name;
  uint32_t font_addr;
  if(path[0] != '/') return;
  if(!theme_font_locate(&font_addr)) return;
  uint8_t hdr[SFN_HDR_LEN];
  if(!theme_open(path, hdr, sizeof(hdr), SFN_MAGIC, SFN_VERSION)) return;
  /* the whole body up front, like theme_apply: a short file never leaves half a font */
  if(file_handle.fsize >= SFN_HDR_LEN + THEME_FONT_LEN) {
    /* a tile the mask leaves out is skipped (theme_stream with cap 0 seeks past it) */
    for(int t = 0; t < 256; t++) {
      uint32_t cap = ((hdr[16 + (t >> 3)] >> (t & 7)) & 1) ? SFN_TILE_LEN : 0;
      if(!theme_stream(font_addr + (uint32_t)t * SFN_TILE_LEN, SFN_TILE_LEN, cap)) break;
    }
  }
  file_close();
  /* the font in PSRAM is a new one: theme_font_edges has to capture it as the pristine copy */
  theme_font_orig_ok = 0;
}

static void theme_set_str(uint8_t *dst, const char *name, const char *none) {
  /* strncpy for the zero padding: the whole cfg_t goes to the shared BSRAM,
     tail included (see CK_STR in cfg.c). */
  strncpy((char*)dst, (name && name[0]) ? name : none, sizeof(CFG.skin_name) - 1);
  dst[sizeof(CFG.skin_name) - 1] = 0;   /* every CK_STR field has this length (cfg.c) */
  cfg_save();
}

void font_select(const char *name) {
  theme_set_str(CFG.font_name, name, "");
}

void theme_clear(int font) {
  if(font) font_select(NULL);
  else theme_select(NULL);
}

void theme_pick(const char *name) {
  const char *ext = strrchr(name, '.');
  if(ext && !strcasecmp(ext + 1, "SFN")) font_select(name);
  else theme_select(name);
}

void theme_select(const char *name) {
  theme_set_str(CFG.skin_name, name, THEME_DEFAULT);
}
