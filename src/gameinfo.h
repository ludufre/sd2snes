/* sd2snes+ - pre-boot game info screen (MCU side).
 *
 * Parses /sd2snes/info/<ROM>.yml and stages one composited DirectColor BG image
 * (cover + screenshot, see utils/gen_dcimage.py) so the SNES menu can show a "game
 * details" screen before a ROM boots. SAFETY: gameinfo_load() is bounded and
 * fail-safe - on ANY error (missing
 * .yml, missing/garbled image, parse failure) it writes a status byte and RETURNS;
 * it must never hang the menu command loop (that would wedge USB-serial too).
 *
 * The parsed metadata is written as a packed struct to bank-$FF SRAM at
 * SRAM_GAMEINFO_ADDR ($FF6000). The SNES menu reads it by fixed offset (mirror the
 * GI_* offsets in snes/memmap.i65 - keep in sync, just like cfg_t/CFG_ADDR). The
 * DirectColor image is staged as: 8bpp tiles -> SRAM_GAMEINFO_TILES_ADDR ($CA0000),
 * 16-bit tilemap -> SRAM_GAMEINFO_TMAP_ADDR ($CB0000). All text fields are already
 * font-encoded (accents -> codes 130..159).
 */

#ifndef GAMEINFO_H
#define GAMEINFO_H

#include <stdint.h>

#define GAMEINFO_DIR        "/sd2snes/info/"

#define GAMEINFO_MAGIC0     ('G')
#define GAMEINFO_MAGIC1     ('I')

#define GAMEINFO_STATUS_NONE  (0x00)   /* no /sd2snes/info/<rom>.yml -> menu skips the screen */
#define GAMEINFO_STATUS_OK    (0x01)   /* metadata staged; menu shows the screen */
#define GAMEINFO_STATUS_ERROR (0x02)   /* present but unusable */

#define GAMEINFO_FLAG_IMAGE   (0x01)   /* image.gd (DirectColor) staged ok */

/* DirectColor .gd image header (utils/gen_dcimage.py); little-endian. */
#define GD_MAGIC0           ('G')
#define GD_MAGIC1           ('D')
#define GD_VERSION          (0x01)
#define GD_HEADER_SIZE      (12)

/* packed metadata struct -> SRAM_GAMEINFO_ADDR. Byte offsets are mirrored in
 * snes/memmap.i65 (GI_*). Text fields are font-encoded NUL-terminated strings (so
 * the SNES just hiprint's them); year/players keep the raw YAML text. Sizes are
 * sanity caps; the SNES wraps/clips. */
typedef struct __attribute__((__packed__)) _gameinfo_meta {
  uint8_t  magic[2];          /* +0   'G','I' */
  uint8_t  status;            /* +2   GAMEINFO_STATUS_* */
  uint8_t  flags;             /* +3   GAMEINFO_FLAG_* */
  uint8_t  img_w_tiles;       /* +4   image width in 8x8 tiles */
  uint8_t  img_h_tiles;       /* +5   image height in tiles */
  uint16_t img_num_tiles;     /* +6   unique 8bpp tiles staged */
  char     title[40];         /* +8   ($08) */
  char     developer[40];     /* +48  ($30) */
  char     year[6];           /* +88  ($58) e.g. "1994" */
  char     players[4];        /* +94  ($5E) e.g. "2" */
  char     genre[32];         /* +98  ($62) */
  char     special_chip[24];  /* +130 ($82) free string from the YAML */
  char     description[512];  /* +154 ($9A) one line; SNES word-wraps */
} gameinfo_meta_t;            /* 666 bytes */

/* Parse /sd2snes/info/<rom>.yml, stage cover/screenshot, write the meta struct.
 * Bounded + fail-safe; never hangs. status=NONE when there is no .yml. */
void gameinfo_load(uint8_t *rom_path);

#endif
