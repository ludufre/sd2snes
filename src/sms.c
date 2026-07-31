/* sd2snes - Sega Master System experimental core launch (M7.4b)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   sms.c: .sms launch via the FPGA_SMS core. See sms.h for the model.
*/

#include <string.h>
#include "config.h"
#include "fileops.h"
#include "ff.h"
#include "memory.h"
#include "fpga.h"
#include "sms.h"
#include "uart.h"

uint8_t sms_active = 0;
static char sms_rompath[256];

/* case-insensitive ".sms" extension check */
void sms_id(uint8_t *filename) {
  sms_active = 0;
  if (!filename) return;
  char *dot = strrchr((char*)filename, '.');
  if (dot && !strcasecmp(dot + 1, "sms")) {
    sms_active = 1;
    strncpy(sms_rompath, (char*)filename, sizeof(sms_rompath) - 1);
    sms_rompath[sizeof(sms_rompath) - 1] = 0;
  }
}

/* boot the SNES-side player instead of the .sms; the .sms is staged separately */
uint8_t sms_update_file(uint8_t **filename_ref) {
  if (!sms_active) return 1;
  FILINFO fno;
  fno.lfname = NULL;            /* _USE_LFN=1: f_stat writes the long name through
                                   fno.lfname; NULL it or it writes through stack garbage
                                   (wild pointer) on every load. Same guard as file_exists(). */
  if (f_stat((const TCHAR*)SMS_PLAYER_FILE, &fno) != FR_OK) {
    printf("SMS: player %s missing\n", (char*)SMS_PLAYER_FILE);
    return 0;
  }
  *filename_ref = (uint8_t*)SMS_PLAYER_FILE;
  return 1;
}

/* stage the .sms ROM where the FPGA Z80 fetches it (PSRAM 0x300000) */
void sms_load_rom(void) {
  if (!sms_active) return;
  printf("SMS: staging %s -> PSRAM %06x\n", sms_rompath, SMS_ROM_PSRAM);
  load_sram_offload((uint8_t*)sms_rompath, SMS_ROM_PSRAM, 0);
}
