/* sd2snes - per-ROM cover preview (MCU side)
 *
 * Loads a sibling "<rom>.cov" (8bpp BG cover, .cov v3) from the SD card into
 * the bank-C9 staging area shared with the SNES menu. The MCU only validates
 * the header and STREAMS bytes; it never decodes images.
 *
 * SAFETY: every code path in load_cover() is bounded and fail-safe. On ANY
 * error (missing file, bad header, truncated read) it writes a status byte
 * and RETURNS - it must never hang, or it would wedge the menu command loop
 * and kill USB-serial access.
 *
 * .cov v3 on-disk format (produced by utils/cover_conv.py, consumed by
 * snes/cover.a65). All multi-byte fields little-endian:
 *   off  size  field
 *   0    2     magic "CV"
 *   2    1     version (=3)
 *   3    1     flags (bit0 = dithered; rest reserved)
 *   4    1     w_tiles   (cover width  in 8px tiles)
 *   5    1     h_tiles   (cover height in 8px tiles)
 *   6    1     cg_base   (CGRAM index the palette loads at)
 *   7    1     ncolors-1 (palette colour count - 1)
 *   8    1     bpp       (=8)
 *   9    1     reserved
 *   10   2     reserved
 *   12   nc*2          PALETTE (nc = ncolors BGR555 LE entries)
 *   ..   w*h*64        TILES   (8bpp planar 8x8 tiles, row-major)
 */

#ifndef COVER_H
#define COVER_H

#include <stdint.h>

#define COVER_MAGIC0        ('C')
#define COVER_MAGIC1        ('V')
#define COVER_VERSION       (0x03)
#define COVER_BPP           (0x08)

/* hard limits - guard against malformed files and buffer overflow. The 8bpp
 * cover tiles reuse the logo VRAM region (14 KB = 224 tiles of 64 bytes). */
#define COVER_MAX_COLS      (32)
#define COVER_MAX_ROWS      (7)    /* cover lives in the 56px (7-row) header band */
#define COVER_MAX_TILES     (224)

#define COVER_HEADER_SIZE   (12)

/* bank-C9 staging layout (byte offsets from SRAM_COVER_ADDR), shared with the
 * SNES menu (see snes/memmap.i65 COVER_* defines - keep in sync). */
#define COVER_OFF_TILES     (0x0000)   /* up to 224*64 = 0x3800 bytes */
#define COVER_OFF_PAL       (0x3800)   /* up to 256*2 = 0x200 bytes  */
#define COVER_OFF_STATUS    (0x3A00)   /* 8-byte meta block (below)   */

/* meta block at COVER_OFF_STATUS:
 *   +0 status   +1 w_tiles  +2 h_tiles  +3 cg_base
 *   +4 ncolors-1 +5 flags   +6 reserved +7 reserved */
#define COVER_META_SIZE     (8)

#define COVER_STATUS_NONE   (0x00)   /* no cover / missing */
#define COVER_STATUS_OK     (0x01)   /* valid cover staged */
#define COVER_STATUS_ERROR  (0x02)   /* file present but invalid */

/* load_cover: derive "<rom>.cov" from rom_path, validate, stream into
 * sram_addr (bank C9), and write the meta block. Returns 1 on success,
 * 0 otherwise. Never hangs. */
int load_cover(const uint8_t *rom_path, uint32_t sram_addr);

#endif
