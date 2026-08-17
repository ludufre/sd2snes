/* sd2snes - Atari 2600 experimental core launch

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   atari.h: .a26 launch via the FPGA_A26 core.

   An .a26 boots the SNES-side player (/sd2snes/a26_snes.bin) which renders the
   frame the core packs into PSRAM; the cartridge image itself is staged at PSRAM
   A26_ROM_PSRAM, where the core copies it into BRAM at reset. Mirrors the SMS
   foreign-system launch (sms.h): detect by extension, swap the booted file to a
   SNES-side player, stage the foreign ROM separately.

   Unlike the SMS, the cartridge type is NOT in a header -- an .a26 is a raw dump,
   so the bankswitch scheme has to be derived from the image itself. atari.c does
   that scan and publishes the result here; memory.c forwards feat16 to the core
   via CHIPFEAT before the reset. See ATARI-CORE-CONTRACT.md (sec. 7) and
   ATARI-BANKSWITCH.md at the workspace root.
*/

#ifndef ATARI_H
#define ATARI_H

#include <stdint.h>

/* SNES-side player loaded as the booted LoROM image. Built from snes/a26/
   a26_snes.asm by build.sh and shipped in the release zip, same as the NES stub
   and the SMS player (<system>_snes.bin = the SNES-side half of that system). */
#define A26_PLAYER_FILE ((const uint8_t*)"/sd2snes/a26_snes.bin")
/* where the core reads the cartridge image from -- lockstep with the ROM fetch
   base of verilog/sd2snes_a26 (ATARI-CORE-CONTRACT.md sec. 1). <=32768 B; the
   core copies it into BRAM at reset and never reads PSRAM again. */
#define A26_ROM_PSRAM   0x300000L

/* Bankswitch schemes the v0 core implements. The value IS feat16[3:0]
   (ATARI-CORE-CONTRACT.md sec. 7) -- do not renumber. */
typedef enum {
  A26_BS_2K = 0,
  A26_BS_4K = 1,
  A26_BS_F8 = 2,
  A26_BS_F6 = 3,
  A26_BS_F4 = 4
  /* 5..15 reserved */
} a26_bs_t;

typedef struct __attribute__((__packed__)) _a26_romprops {
  uint8_t  has_a26;           /* booted file is a .a26 (by extension) */
  uint8_t  scheme;            /* a26_bs_t; only meaningful when error == MENU_ERR_OK */
  uint8_t  superchip;         /* 1 = 128B Superchip RAM (W $1000-$107F / R $1080-$10FF) */
  uint8_t  size_class;        /* 0=2K 1=4K 2=8K 3=16K 4=32K -> feat16[11:8] */
  uint32_t romsize_bytes;     /* image size as found on the card */
  /* Exact 16-bit word written via fpga_set_chipfeat() (opcode 0xef, CHIPFEAT) and
     wired in main.v to the core's a26_feat_out. Bit layout (ATARI-CORE-CONTRACT.md
     sec. 7):
       [3:0]   scheme
       [4]     superchip
       [5]     video_width (0 = 160, 1 = 256)
       [6]     tv
       [11:8]  size_class
     Bits [7] and [15:12] are reserved and always 0. */
  uint16_t feat16;
  uint8_t  error;             /* MENU_ERR_NOIMPL on an unmappable image, else MENU_ERR_OK */
  const uint8_t *error_param; /* scheme/size label shown by the menu popup ("3F", "12K", ...) */
  /* Detector evidence, kept for the diagnostic line and the host gate. Saturated
     at 255; the decision thresholds are far below that (see atari.c). Only
     abs_stores_3f, addr_distinct_ua and sc_read_refs take part in a verdict --
     e0_slices_hit in particular is published but never acted on. */
  uint8_t  score_3f, score_ua, addr_distinct_3f, addr_distinct_ua, e0_slices_hit;
  uint8_t  abs_stores_3f;     /* "STA $003F" (8D 3F 00) windows found */
  uint8_t  sc_read_refs;      /* distinct operands read from $1080-$10FF */
} a26_romprops_t;

extern a26_romprops_t a26_romprops;

/* detect a .a26 by extension, scan the image and fill *props (never blocks:
   the scan is one linear pass over at most 32KB through a 512B buffer) */
void a26_id(a26_romprops_t*, uint8_t *filename);
/* swap *filename_ref to the SNES-side player; returns 0 if the player is missing */
uint8_t a26_update_file(uint8_t **filename_ref);
/* stage the .a26 image into PSRAM (call after fpga_pgm, before the SNES boots) */
void a26_load_rom(void);

#endif
