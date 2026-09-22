/* sd2snes - SD card based universal cartridge for the SNES

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   bootleg.h: copy-protected unlicensed LoROM bootlegs (fpga_bootleg core)
*/

#ifndef BOOTLEG_H
#define BOOTLEG_H

#include <stdint.h>
#include "smc.h"

/* protection variant, sent to the core as chipfeat[2:0] (see bootleg.v) */
#define BOOTLEG_NONE      0
#define BOOTLEG_BITSWAP   1   /* standard latch, read bits re-ordered 0,6,7,1,2,3,4,5 */
#define BOOTLEG_CONSTANT  2   /* Soul Blade: 55,0F,AA,F0 */
#define BOOTLEG_ALU       3   /* Tekken 2 / SF EX Plus Alpha: 4-bit count/shift unit */
#define BOOTLEG_PORT6     4   /* A Bug's Life / Bananas: "port 6xxx" at 00-3F:6000-6FFF */
#define BOOTLEG_BITSWAP40 5   /* Marvel vs SF: standard bitswap latch at 40-4F:8000-FFFF */
#define BOOTLEG_KOF98     6   /* KOF98: standard bitswap + ROM bank switch at C0:8788 */

extern uint8_t bootleg_scan;

/* Identify a bootleg by the CRC32 of the headerless image in the currently open
   file.  Only runs for image sizes that occur in the table, so ordinary loads pay
   nothing.  image_offset = file offset of the ROM image (copier header + combo
   offset).  Returns the variant (BOOTLEG_NONE if not a known bootleg). */
uint8_t bootleg_identify_file(uint32_t image_offset, uint32_t image_size);

/* Apply a detected variant to the rom properties (core, mapper, chipfeat,
   ROM size).  Must run before smc_id derives romsize_bytes from the header. */
void bootleg_apply(snes_romprops_t *props, uint8_t variant, uint32_t image_size);

#endif
