/* sd2snes - Game Boy Color experimental core launch

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   gbc.c: route a Game Boy image to the FPGA_GBC core. See gbc.h for the model.

   mk3-only: on the mk2 (LPC1754, tight flash and no Spartan-3 build of the core)
   everything below compiles to no-op stubs, so a .gb/.gbc keeps booting the SGB
   there exactly as it did -- same shape as the SMS launch in sms.c.
*/

#include <string.h>
#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "smc.h"
#include "sgb.h"
#include "gbc.h"
#include "cfg.h"
#include "uart.h"
#include "util.h"
#include "memory.h"

extern cfg_t CFG;
extern sgb_romprops_t sgb_romprops;

#ifndef CONFIG_MK2

/* Both names are written into the SGB globals, so they have to fit them. */
_Static_assert(sizeof(GBC_BOOT_FILE_STR) <= sizeof(SGBFW),
               "GBC_BOOT_FILE does not fit SGBFW[]");
_Static_assert(sizeof(GBC_PLAYER_FILE_STR) <= sizeof(SGBSR),
               "GBC_PLAYER_FILE does not fit SGBSR[]");

/* case-insensitive ".gbc" extension check.  path_is_gb() (fileops.c) already let
   BOTH .gb and .gbc in as Game Boy images -- what is decided here is only which of
   the two cores runs them, so this is a plain extension test, not a second
   definition of "is a Game Boy file". */
static uint8_t gbc_ext_is_gbc(const uint8_t *filename) {
  if (!filename) return 0;
  char *dot = strrchr((char*)filename, '.');
  return (dot && !strcasecmp(dot + 1, "gbc")) ? 1 : 0;
}

void gbc_id(sgb_romprops_t *props, uint8_t *filename) {
  /* Not a Game Boy image at all: sgb_id() left everything zeroed and there is
     nothing to route. */
  if (!props->has_sgb) return;

  /* $0143 (the last byte of the 16-byte title field): $80 = the image knows about
     the CGB and also runs on a DMG, $C0 = CGB-only. */
  uint8_t cgb = props->header.name[15];
  uint8_t want_gbc;

  switch (CFG.gbc_mode) {
    case GBC_MODE_PREFER_SGB:
      /* Keep the SGB (borders, its own colourisation) wherever it can run the
         game at all; only an image the DMG cannot boot has to move. */
      want_gbc = (cgb == 0xC0);
      break;
    case GBC_MODE_PREFER_GBC:
      want_gbc = 1;
      break;
    case GBC_MODE_AUTO:
    default:
      /* The extension is part of the test, not just the header: a .gbc the user
         named that way is the case they expect to see in colour, and a handful of
         homebrew/hacks carry a CGB image under a cleared $0143. */
      want_gbc = (gbc_ext_is_gbc(filename) || (cgb & 0x80)) ? 1 : 0;
      break;
  }
  if (!want_gbc) return;

  props->core_is_gbc = 1;
  /* The four things the two cores disagree about.  Everything downstream --
     sgb_update_file, sgb_update_romprops, sgb_load_sram, the missing-file
     pre-checks in load_check_prereqs and the CHIPFEAT cascade in load_rom --
     reads these and needs no GBC branch of its own. */
  strlcpy_nul(SGBFW, GBC_BOOT_FILE_STR, sizeof(SGBFW));
  strlcpy_nul(SGBSR, GBC_PLAYER_FILE_STR, sizeof(SGBSR));
  props->fpga_sgbfeat = (uint16_t)(
      ((uint16_t)CFG.sgb_volume_boost & GBC_FEAT_VOL_MASK)
    | ((cgb & 0x80)  ? GBC_FEAT_CGB_HDR    : 0)
    | (CFG.gbc_sync  ? GBC_FEAT_SYNC_EXACT : 0));
  /* GBC_FEAT_FORCE_DMG is deliberately never set here: it is a debug switch with
     no CFG (see gbc.h). */

  printf("GBC:  cgb=0x%02x  mode=%d  sync=%d  chipfeat=0x%04x\n", cgb,
    CFG.gbc_mode,
    CFG.gbc_sync,
    props->fpga_sgbfeat);
}

uint8_t gbc_bios_state(void) {
  uint8_t state = SGB_BIOS_OK;
  uint32_t crc = 0;

  if (!file_crc32(GBC_BOOT_FILE, &crc)) {
    state = SGB_BIOS_MISSING;
  }
  else if (  (crc != GBC_BOOT_CRC_NINTENDO)
          && (crc != GBC_BOOT_CRC_SAMEBOY)
          ) {
    printf("GBC cgb_boot.bin CRC mismatch: 0x%08x\n", (unsigned int)crc);
    state = SGB_BIOS_MISMATCH;
  }

  /* The player is ours: existence is all that can be checked here, since it is
     rebuilt with every release.  Whether the .bi3 next to it speaks the same wire
     version is settled at runtime by the status block, not by a CRC table. */
  file_open((uint8_t*) GBC_PLAYER_FILE, FA_READ);
  if (file_res) state = SGB_BIOS_MISSING;
  file_close();

  return state;
}

void gbc_stage_config(uint32_t image_addr) {
  if (!sgb_romprops.has_sgb || !sgb_romprops.core_is_gbc) return;

  uint32_t blk = image_addr + GBC_CFG_BLK_OFFSET;
  uint8_t hdr[5];
  sram_readblock(hdr, blk, sizeof(hdr));
  /* Any version from 1 up: the block only grows by appending fields, so +5 means
     the same thing in every later version and a newer player still gets it. */
  if (memcmp(hdr, "GBCF", 4) || hdr[4] < GBC_CFG_BLK_VERSION) {
    printf("GBC:  player has no GBCF block\n");
    return;
  }
  sram_writebyte(CFG.gbc_stretch ? 1 : 0, blk + GBC_CFG_BLK_STRETCH);
  printf("GBC:  player config stretch=%d\n", CFG.gbc_stretch ? 1 : 0);
}

#else /* CONFIG_MK2: no-op stubs -- the GBC core is mk3-only (no Spartan-3 build of
         fpga_gbc, and the LPC1754 flash does not pay for the loader).  A .gb/.gbc
         is NOT refused here the way .nes/.sms/.a26 are: core_is_gbc stays 0 and
         the image boots the SGB, which is a complete Game Boy on this board. */

void gbc_id(sgb_romprops_t *props, uint8_t *filename) {
  (void)props;
  (void)filename;
}

uint8_t gbc_bios_state(void) {
  return SGB_BIOS_MISSING;
}

void gbc_stage_config(uint32_t image_addr) {
  (void)image_addr;
}

#endif /* CONFIG_MK2 */
