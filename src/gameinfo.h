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

#define GAMEINFO_STATUS_NONE  (0x00)   /* menu skips the screen (only mk2's parser-less stub) */
#define GAMEINFO_STATUS_OK    (0x01)   /* metadata staged; menu shows the screen. mk3 ALWAYS
                                        * reports OK now: a ROM with no .yml gets a filename
                                        * title + "-" fields + (if present) its .cov in the band */
#define GAMEINFO_STATUS_ERROR (0x02)   /* present but unusable */

#define GAMEINFO_FLAG_IMAGE   (0x01)   /* <rom>.gd (DirectColor) staged ok */
#define GAMEINFO_FLAG_FMV     (0x02)   /* <rom>.fmv animated screenshot streaming; the
                                        * cover is the sibling <rom>.cov (OBJ). Frame 0
                                        * is staged at SRAM_GAMEINFO_TILES_ADDR; the menu
                                        * pumps CMD_FMV_NEXT to advance (gameinfo_fmv_next) */

/* DirectColor .gd image header (utils/gen_dcimage.py); little-endian. */
#define GD_MAGIC0           ('G')
#define GD_MAGIC1           ('D')
#define GD_VERSION          (0x01)
#define GD_HEADER_SIZE      (12)

/* Animated screenshot .fmv container (utils/gen_fmv.py); little-endian. A fixed-size
 * 8bpp DirectColor box (BOX_W x BOX_H tiles) per frame, NO dedup (same slots every
 * frame) so the firmware streams one block per frame and the SNES re-DMAs it in place.
 * Geometry is fixed: it MUST match GI_FMV_W/GI_FMV_H in snes/gameinfo.a65. */
#define FMV_MAGIC0          ('F')
#define FMV_MAGIC1          ('V')
#define FMV_VERSION         (0x01)
#define FMV_HEADER_SIZE     (12)
#define FMV_BOX_W           (12)       /* box width  in 8x8 tiles (96 px) */
#define FMV_BOX_H           (9)        /* box height in 8x8 tiles (72 px) */
#define FMV_FRAME_BYTES     (FMV_BOX_W * FMV_BOX_H * 64)   /* 6912 */

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
  char     description[256];  /* +154 ($9A) one line; SNES word-wraps. 256 = the YAML
                                 line/value cap (YAML_BUFLEN); the parser cannot deliver more */
  uint8_t  fmv_fps;           /* +410 ($19A) FMV playback fps (when GAMEINFO_FLAG_FMV) */
  uint16_t fmv_frames;        /* +411 ($19B) total FMV frames (info/debug; the MCU loops) */
} gameinfo_meta_t;            /* 413 bytes */

/* Parse /sd2snes/info/<rom>.yml (optional), stage the band image (<rom>.fmv animated,
 * else <rom>.gd static, else the sibling <rom>.cov OBJ fallback), write the meta
 * struct. Bounded + fail-safe; never hangs. mk3 always reports status=OK. */
void gameinfo_load(uint8_t *rom_path);

/* CMD_FMV_NEXT pump: stream the next .fmv frame (looping) into SRAM_GAMEINFO_TILES_ADDR
 * for the menu to DMA into the band. No-op unless a .fmv is open. Bounded + fail-safe. */
void gameinfo_fmv_next(void);

/* Stop the FMV (close the file + stop the looping audio); call when the info screen closes. */
void gameinfo_fmv_stop(void);

/* Menu-loop watchdog: stop a lingering FMV if CMD_FMV_NEXT has gone quiet (screen closed
 * without a trailing command, e.g. returning to the Favorites/Recents list). No-op if idle. */
void gameinfo_fmv_idle_check(void);

#endif
