/* Super Game Boy BIOS check, the System Information screen's only (card overlay code, see
   ovl.h): out of sgb.c so that it goes to the card with the screen. */
#include "config.h"
#include "uart.h"
#include "fileops.h"
#include "cfg.h"
#include "smc.h"
#include "sgb.h"

extern cfg_t CFG;

uint8_t sgb_bios_state(void) {
  uint8_t state = SGB_BIOS_OK;
  uint32_t crc = 0;

  snprintf(SGBFW, sizeof(SGBFW), "/sd2snes/sgb%d_boot.bin", CFG.sgb_bios_version);
  snprintf(SGBSR, sizeof(SGBSR), "/sd2snes/sgb%d_snes.bin", CFG.sgb_bios_version);

  if (!file_crc32((uint8_t*) SGBFW, &crc)) {
    state = SGB_BIOS_MISSING;
  }
  else if (state <= SGB_BIOS_MISMATCH
       && (  (crc != 0x53d0dd63) // sgb2_boot.bin
          && (crc != 0xec8a83b9) // sgb1_boot.bin
          && (crc != 0x73bd96dc) // sgb2_boot.bin (SameBoy)
          && (crc != 0xedac680e) // sgb1_boot.bin (SameBoy)
          )
       ) {
    printf("SGB sgb%d_boot.bin CRC mismatch: 0x%08x\n", CFG.sgb_bios_version, (unsigned int)crc);
    state = SGB_BIOS_MISMATCH;
  }

  if (!file_crc32((uint8_t*) SGBSR, &crc)) {
    state = SGB_BIOS_MISSING;
  }
  else if (state <= SGB_BIOS_MISMATCH
       && (  (crc != 0xcb176e45) // sgb2 bios (JP)
          && (crc != 0x2e35edbb) // sgb bios v1.0 (JU)
          && (crc != 0x27a03c98) // sgb bios v1.1 (JU)
          && (crc != 0x8a4a174f) // sgb bios v1.2 (UE)
          )
       ) {
    printf("SGB sgb%d_snes.bin CRC mismatch: 0x%08x\n", CFG.sgb_bios_version, (unsigned int)crc);
    state = SGB_BIOS_MISMATCH;
  }
  
  return state;
}
