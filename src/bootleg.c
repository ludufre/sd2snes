/* sd2snes - SD card based universal cartridge for the SNES

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   bootleg.c: copy-protected unlicensed LoROM bootlegs (fpga_bootleg core)

   The protection hardware of these carts leaves nothing in the ROM header to
   key on (the headers are copied from whatever the pirates started from), so
   they are identified by the CRC32 of the headerless image, the same CRCs
   fullsnes and MAME's software list use.

   Cost: the scan only runs when the image size equals one of the sizes in the
   table below (1, 2 or 3 MB).  For those loads the whole image is read once
   more from the SD card and CRC'd, unless every table entry of that size has
   a 64 KB "fingerprint" filled in -- then only the first 64 KB is read for a
   non-matching game and the full CRC is computed only on a fingerprint hit.
   utils/bootleg_fp.py prints the fingerprint column for a set of ROM files.
*/

#include <string.h>
#include "config.h"
#include "uart.h"
#include "fileops.h"
#include "crc32.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "snes.h"
#include "bootleg.h"

#define BOOTLEG_FP_SIZE (0x10000UL)

/* Set by load_identify() around its smc_id() call for game loads only, so the
   menu load and any other smc_id() caller never pay for a CRC scan. */
uint8_t bootleg_scan = 0;

typedef struct {
  uint32_t crc;      /* CRC32 of the whole headerless image */
  uint32_t fp;       /* CRC32 of the first 64 KB, 0 = unknown (forces a full scan) */
  uint32_t size;     /* image size in bytes */
  uint8_t  variant;
} bootleg_entry_t;

static const bootleg_entry_t bootleg_tbl[] = {
  /* standard bitswap latch */
  { 0x752A25D3, 0x1B3981AC, 0x200000, BOOTLEG_BITSWAP  },  /* Aladdin 2000 */
  { 0x4F660972, 0xBFF5A00B, 0x200000, BOOTLEG_BITSWAP  },  /* Digimon Adventure */
  { 0xA7813943, 0xAFB48AB7, 0x300000, BOOTLEG_BITSWAP  },  /* King of Fighters 2000 */
  { 0x892C6765, 0x37C1D442, 0x200000, BOOTLEG_BITSWAP  },  /* Pocket Monster (Picachu) */
  { 0x7C0B798D, 0x6CB71798, 0x200000, BOOTLEG_BITSWAP  },  /* Pokemon Gold Silver */
  { 0xF863C642, 0x435AE4AD, 0x200000, BOOTLEG_BITSWAP  },  /* Pokemon Stadium (uses the 90/98 mirrors) */
  { 0x5E4ADA04, 0xEE4A038D, 0x200000, BOOTLEG_BITSWAP  },  /* Soul Edge Vs Samurai */
  { 0x40242231, 0xFA8C7F16, 0x200000, BOOTLEG_BITSWAP  },  /* X-Men vs. Street Fighter */
  { 0xBAD1D9B8, 0x3BD84207, 0x200000, BOOTLEG_BITSWAP  },  /* Squirrel (verified in emulation) */
  /* Soul Blade constant pattern */
  { 0xC97D1D7B, 0x100B21AC, 0x300000, BOOTLEG_CONSTANT },  /* Soul Blade */
  { 0x45874D3D, 0xF0BA418B, 0x200000, BOOTLEG_CONSTANT },  /* Hercules (verified in emulation) */
  { 0x5BBA4EB3, 0x0D223726, 0x200000, BOOTLEG_CONSTANT },  /* Dragon Ball Z Final Bout (verified in emulation; dump lacks banks 07-0A) */
  { 0xDD7AFCB9, 0x0D223726, 0x200000, BOOTLEG_CONSTANT },  /* DBZ Final Bout, sound restored from SF II (utils/repair_dbz_sound.py) */
  /* Tekken 2 ALU/flipflop -- SF EX Plus Alpha uses it too (nocash, nesdev t=15510),
     although fullsnes lists it under bitswap */
  { 0x066687CA, 0x4303973A, 0x200000, BOOTLEG_ALU      },  /* Tekken 2 */
  { 0xDAD59B9F, 0x8BA8E6E5, 0x200000, BOOTLEG_ALU      },  /* Street Fighter EX Plus Alpha */
  /* "port 6xxx" (nocash, nesdev t=15510); the bitswap code in these is dead */
  { 0x014F0FCF, 0xAE463A96, 0x200000, BOOTLEG_PORT6    },  /* A Bug's Life (verified in emulation) */
  { 0x52B0D84B, 0xE425A3C9, 0x100000, BOOTLEG_PORT6    },  /* Bananas de Pijamas (verified in emulation) */
  /* bitswap latch on other address lines: write 4x:xxx2, read 4x:xxx0 (nocash, nesdev t=15510) */
  { 0xCDB590E4, 0x8D538965, 0x200000, BOOTLEG_BITSWAP40 },  /* Marvel Super Heroes vs SF (verified in emulation) */
  /* bitswap + bank switch: C0:8788 = 82 maps ROM bank 02 over bank 00 (Revenant/nocash, nesdev t=15510) */
  { 0x6C303FC9, 0x29CE930D, 0x200000, BOOTLEG_KOF98    },  /* King of Fighters '98 (verified in emulation) */
};
#define BOOTLEG_TBL_N (sizeof(bootleg_tbl) / sizeof(bootleg_tbl[0]))

/* CRC `len` bytes of the open file starting at `offset`, continuing from `crc`
   (pre-finalization value).  Returns 0 on a read error / short file. */
static int bootleg_crc_range(uint32_t offset, uint32_t len, uint32_t *crc) {
  UINT got;
  if(f_lseek(&file_handle, offset) != FR_OK || file_handle.fptr != offset) return 0;
  while(len) {
    UINT want = (len > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)len;
    if(f_read(&file_handle, file_buf, want, &got) != FR_OK || got != want) return 0;
    for(UINT i = 0; i < got; i++) *crc = crc32_update(*crc, file_buf[i]);
    len -= got;
  }
  return 1;
}

uint8_t bootleg_identify_file(uint32_t image_offset, uint32_t image_size) {
  uint8_t  size_hit = 0, fp_known = 1;
  uint32_t crc, fp;

  for(unsigned i = 0; i < BOOTLEG_TBL_N; i++) {
    if(bootleg_tbl[i].size != image_size) continue;
    size_hit = 1;
    if(!bootleg_tbl[i].fp) fp_known = 0;
  }
  if(!size_hit) return BOOTLEG_NONE;

  crc = crc32_init();
  if(!bootleg_crc_range(image_offset, BOOTLEG_FP_SIZE, &crc)) return BOOTLEG_NONE;

  if(fp_known) {
    /* cheap reject: first 64 KB must match a fingerprint of this size */
    fp = crc32_finalize(crc);
    uint8_t fp_hit = 0;
    for(unsigned i = 0; i < BOOTLEG_TBL_N; i++) {
      if(bootleg_tbl[i].size == image_size && bootleg_tbl[i].fp == fp) fp_hit = 1;
    }
    if(!fp_hit) return BOOTLEG_NONE;
  }

  if(!bootleg_crc_range(image_offset + BOOTLEG_FP_SIZE, image_size - BOOTLEG_FP_SIZE, &crc)) {
    return BOOTLEG_NONE;
  }
  crc = crc32_finalize(crc);
  printf("bootleg: image crc32=%08lx\n", crc);

  for(unsigned i = 0; i < BOOTLEG_TBL_N; i++) {
    if(bootleg_tbl[i].size == image_size && bootleg_tbl[i].crc == crc) {
      return bootleg_tbl[i].variant;
    }
  }
  return BOOTLEG_NONE;
}

void bootleg_apply(snes_romprops_t *props, uint8_t variant, uint32_t image_size) {
  snes_header_t *header = &(props->header);
  if(variant == BOOTLEG_NONE) return;
  /* whatever chip the (copied) header claimed is not there */
  props->has_dspx = 0;  props->has_st0010 = 0; props->has_st0011 = 0;
  props->has_st0018 = 0; props->has_cx4 = 0;   props->has_obc1 = 0;
  props->has_col20 = 0; props->has_gsu = 0;    props->has_fx3 = 0;
  props->has_sa1 = 0;   props->has_sdd1 = 0;   props->has_spc7110 = 0;
  props->has_spc7110_rtc = 0;
  props->dsp_fw = NULL;
  props->fpga_features &= ~(FEAT_DSPX | FEAT_ST0010 | FEAT_BSLOROM);
  props->error = MENU_ERR_OK;
  props->error_param = NULL;
  props->mapper_id    = 1;               /* all of them are plain LoROM */
  props->fpga_conf    = FPGA_BOOTLEG;
  props->fpga_dspfeat = variant;         /* -> chipfeat[2:0] (CMD 0xef) */
  /* pirate headers are unreliable: size the ROM mask from the image (smc_id
     derives romsize_bytes from header->romsize right after this) */
  header->romsize = 0;
  while(((uint32_t)1024 << header->romsize) < image_size) header->romsize++;
  printf("bootleg: protection variant %d -> %s\n", variant, FPGA_BOOTLEG);
}
