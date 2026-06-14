/* sd2snes - per-ROM cover preview (MCU side)
 *
 * Loads a sibling "<rom>.cov" (4bpp OBJ-sprite cover, .cov v4) from the SD card into
 * the bank-C9 staging area shared with the SNES menu. The MCU only validates
 * the header and STREAMS bytes; it never decodes images.
 *
 * SAFETY: every code path in load_cover() is bounded and fail-safe. On ANY
 * error (missing file, bad header, truncated read) it writes a status byte
 * and RETURNS - it must never hang, or it would wedge the menu command loop
 * and kill USB-serial access.
 *
 * .cov v4 on-disk format (produced by utils/cover_conv.py, consumed by
 * snes/cover.a65). The cover is drawn with 16x16 OBJ sprites floating OPAQUE
 * over the (untouched) file list, so the list keeps its full Mode-5 hi-res 18
 * rows. Sprites are 4bpp (16 colours each, index 0 = transparent) using up to 8
 * OBJ palettes (CGRAM 128..255); each 16x16 block picks one palette (the
 * BLOCKMAP). All multi-byte fields little-endian:
 *   off  size  field
 *   0    2     magic "CV"
 *   2    1     version (=4)
 *   3    1     flags (bit0 = dithered; rest reserved)
 *   4    1     w_spr      (cover width  in 16x16 sprites)
 *   5    1     h_spr      (cover height in 16x16 sprites)
 *   6    1     n_palettes (number of OBJ palettes, 1..8)
 *   7    1     reserved
 *   8    1     bpp        (=4)
 *   9    1     reserved
 *   10   2     reserved
 *   12   np*32         PALETTES (np palettes * 16 BGR555 LE entries; entry 0 transparent)
 *   ..   w_spr*h_spr   BLOCKMAP (one palette index 0..np-1 per 16x16 block, row-major)
 *   ..   (2*h_spr)*16*32  TILES (4bpp planar 8x8 tiles, OBJ name-grid order:
 *                              (2*h_spr) rows x 16 cols; sprite (sx,sy) tile = (2*sy)*16+2*sx)
 */

#ifndef COVER_H
#define COVER_H

#include <stdint.h>

#define COVER_MAGIC0        ('C')
#define COVER_MAGIC1        ('V')
#define COVER_VERSION       (0x04)
#define COVER_BPP           (0x04)

/* hard limits - guard against malformed files and buffer overflow. The 4bpp OBJ
 * tiles live in a single 256-tile name table at VRAM $2000 (the freed BG-cover
 * region). A w_spr x h_spr grid of 16x16 sprites needs (2*h_spr)*16 tiles (16-wide
 * name grid), so 32*h_spr <= 256 -> h_spr <= 8, and the grid is 16 wide -> w_spr <= 8.
 * w_spr*h_spr (<=64) sprites fit the 128-entry OAM. */
#define COVER_OBJ_MAX_W     (8)
#define COVER_OBJ_MAX_H     (8)
#define COVER_MAX_OBJ_TILES (256)
#define COVER_MAX_PALETTES  (8)

#define COVER_HEADER_SIZE   (12)

/* bank-C9 staging layout (byte offsets from SRAM_COVER_ADDR), shared with the
 * SNES menu (see snes/memmap.i65 COVER_* defines - keep in sync). */
#define COVER_OFF_TILES     (0x0000)   /* up to (2*8)*16*32 = 0x2000 bytes  */
#define COVER_OFF_PAL       (0x2000)   /* up to 8*16*2 = 0x100 bytes        */
#define COVER_OFF_BLOCKMAP  (0x2100)   /* up to 8*8 = 64 bytes              */
#define COVER_OFF_STATUS    (0x3A00)   /* 8-byte meta block (below)         */

/* meta block at COVER_OFF_STATUS:
 *   +0 status   +1 w_spr   +2 h_spr   +3 n_palettes
 *   +4 reserved +5 flags   +6 reserved +7 reserved */
#define COVER_META_SIZE     (8)

#define COVER_STATUS_NONE   (0x00)   /* no cover / missing */
#define COVER_STATUS_OK     (0x01)   /* valid cover staged */
#define COVER_STATUS_ERROR  (0x02)   /* file present but invalid */

/* load_cover: derive "<rom>.cov" from rom_path, validate, stream into
 * sram_addr (bank C9), and write the meta block. Returns 1 on success,
 * 0 otherwise. Never hangs. */
int load_cover(const uint8_t *rom_path, uint32_t sram_addr);

#endif
