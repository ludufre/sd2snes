/* sd2snes - Sega Master System experimental core launch (M7.4b)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   sms.h: .sms launch via the FPGA_SMS core.

   A .sms boots the SNES-side player (/sd2snes/sms_snes.bin) which renders the
   FPGA-translated SMS frame ($E0-$E3 -> PSRAM 0x380000). The .sms ROM itself is
   staged at PSRAM 0x300000 where the FPGA's Z80 fetches it. Mirrors the SGB
   foreign-system launch (sgb.c): detect by extension, swap the booted file to a
   SNES-side player, stage the foreign ROM separately.
*/

#ifndef SMS_H
#define SMS_H

#include <stdint.h>

/* SNES-side player loaded as the booted LoROM image. Built from snes/sms/sms_snes.asm
   by build.sh (asar) and shipped in the release zip -- same deal as nes_snes.bin; the
   naming follows it (<system>_snes.bin = the SNES-side half of that system). */
#define SMS_PLAYER_FILE ((const uint8_t*)"/sd2snes/sms_snes.bin")
/* where the FPGA Z80 fetches the .sms ROM (matches sms_inst SMS_ROM base) */
#define SMS_ROM_PSRAM   0x300000

/* set by sms_id() when the booted file is a .sms. Always 0 under CONFIG_MK2: the
   launch is compiled out there (sms.c) and load_rom aborts a .sms with the
   "needs mk3" popup long before sms_id() runs. */
extern uint8_t sms_active;

/* detect a .sms by extension; sets sms_active and remembers the rom path */
void sms_id(uint8_t *filename);
/* swap *filename_ref to the SNES-side player; returns 0 if the player is missing */
uint8_t sms_update_file(uint8_t **filename_ref);
/* stage the .sms ROM into PSRAM (call after fpga_pgm, before the SNES boots) */
void sms_load_rom(void);

#endif
