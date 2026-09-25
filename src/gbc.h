/* sd2snes - Game Boy Color experimental core launch

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   gbc.h: routing a Game Boy image to the FPGA_GBC core instead of the SGB one.

   The Game Boy PARSER is sgb_id() (sgb.c) and stays that way: header, mapper,
   ROM/RAM sizes and the .srm/.gtc plumbing are identical for both cores.  What
   differs is only which bitstream boots the image, which boot ROM it needs and
   which SNES-side image drives the screen.  gbc_id() is therefore a POLICY pass
   that runs right after sgb_id() (memory.c) and rewrites exactly four things --
   SGBFW (boot ROM), SGBSR (SNES-side image), fpga_sgbfeat (CHIPFEAT $EF) and
   sgb_romprops.core_is_gbc -- so every consumer downstream keeps working unchanged.

   mk3-only: there is no Spartan-3 build of the core, so on the mk2 gbc.c compiles
   to no-op stubs and a .gb/.gbc keeps booting the SGB exactly as before (a .gbc is
   NEVER refused there -- unlike .nes/.sms/.a26, the Game Boy has a working mk2
   path).  Same shape as sms.c / atari.c.

   sgb.h has to be included before this header (it defines sgb_romprops_t), the way
   sgb.h itself relies on smc.h for snes_romprops_t.
*/

#ifndef GBC_H
#define GBC_H

#include <stdint.h>

/* SNES-side player, loaded as the booted LoROM image in place of sgbN_snes.bin.
   Built from snes/gbc/gbc_snes.asm; the .bi3 and this file are a COUPLED PAIR
   (GBC-CORE-CONTRACT.md sec. 13) and are never shipped apart. */
#define GBC_PLAYER_FILE_STR "/sd2snes/gbc_snes.bin"
#define GBC_PLAYER_FILE ((const uint8_t*)GBC_PLAYER_FILE_STR)
/* CGB boot ROM, 2304 bytes ($0000-$00FF + $0200-$08FF), staged at PSRAM 0x800000
   (GBC-CORE-CONTRACT.md sec. 1).  Loaded through SGBFW, so it goes through the
   same load_sram_offload + missing-file pre-check as the SGB boot ROM. */
#define GBC_BOOT_FILE_STR "/sd2snes/cgb_boot.bin"
#define GBC_BOOT_FILE   ((const uint8_t*)GBC_BOOT_FILE_STR)
/* Accepted boot ROMs.  The Nintendo dump cannot be redistributed but is accepted
   when the user supplies it; the release ships the freely licensed SameBoy build.
   A third binary that circulates reads $FF50 as a private options register and is
   deliberately NOT accepted -- it would boot into a different machine. */
#define GBC_BOOT_CRC_NINTENDO 0x41884E46
#define GBC_BOOT_CRC_SAMEBOY  0x8113B4D8
/* Both dumps are exactly this long; a file of another size is a different ROM. */
#define GBC_BOOT_SIZE   2304

/* CFG.gbc_mode (config.yml "GbcMode") -- which core a Game Boy image boots on.
   $0143 bit 7 marks an image the CGB understands: $80 = dual (runs on both),
   $C0 = CGB-only.  Do not renumber: the value is the clamp max in cfg.c. */
typedef enum {
  GBC_MODE_AUTO       = 0,  /* .gbc extension or $0143 bit 7 -> GBC, else SGB */
  GBC_MODE_PREFER_SGB = 1,  /* only CGB-only ($0143 == $C0) images -> GBC */
  GBC_MODE_PREFER_GBC = 2   /* every Game Boy image -> GBC */
} gbc_mode_t;

/* CFG.gbc_sync (config.yml "GbcSync") -- how the GB clock relates to the SNES.
   Genlock locks the GB frame to the SNES one (60.0988 Hz, +0.62%) so the read
   window never crosses the GB's own vblank; Exact runs the real 4.194304 MHz and
   accepts the periodic skipped snapshot that comes with the drift. */
typedef enum {
  GBC_SYNC_GENLOCK = 0,
  GBC_SYNC_EXACT   = 1
} gbc_sync_t;

/* CHIPFEAT $EF, written before the core leaves reset (GBC-CORE-CONTRACT.md sec. 2).
   Bits [2:0] are the DAC volume boost, exactly as the SGB core reads them. */
#define GBC_FEAT_VOL_MASK   0x0007
#define GBC_FEAT_FORCE_DMG  0x0008  /* mask $0143 bit 7 from the boot ROM.  DEBUG ONLY:
                                       $0143 is inside the title checksum, so this also
                                       changes the DMG palette the boot ROM picks -- it
                                       reproduces no real hardware and has no CFG. */
#define GBC_FEAT_CGB_HDR    0x0010  /* informative: the header has $0143 bit 7 */
#define GBC_FEAT_SYNC_EXACT 0x0020  /* 1 = Exact, 0 = genlock */

/* Config block the player reserves inside its own image (gbc_snes.bin), at a fixed
   offset of the file: "GBCF", a version byte, then one byte per option.  The player
   ships it with its defaults and the MCU rewrites the option bytes in the copy
   staged in PSRAM, before the SNES leaves reset -- so the user's choices reach the
   player without a new MCU<->SNES channel.  An image without the magic is an older
   player: nothing is written and it runs with what it was built with.
   The block only ever GROWS: a new field is appended and the version bumped, no
   existing offset moves.  So the firmware accepts any version >= the one it knows
   and writes only the fields it knows. */
#define GBC_CFG_BLK_OFFSET   0x7F80
#define GBC_CFG_BLK_VERSION  0x01   /* lowest version that has every field below */
#define GBC_CFG_BLK_STRETCH  5      /* +5: CFG.gbc_stretch (0/1) */

/* Policy pass over the romprops sgb_id() just filled in.  No-op unless has_sgb. */
void gbc_id(sgb_romprops_t *props, uint8_t *filename);
/* Are the two GBC-only files on the card?  Same SGB_BIOS_* states as
   sgb_bios_state(): OK / MISMATCH (boot ROM is not one of the accepted dumps) /
   MISSING.  Only cgb_boot.bin is CRC-checked -- gbc_snes.bin is ours and is
   version-checked at runtime through the status block, not by CRC. */
uint8_t gbc_bios_state(void);
/* Write the player options into the config block of the image load_rom just
   streamed to `image_addr` (PSRAM).  No-op unless this load boots the GBC core. */
void gbc_stage_config(uint32_t image_addr);

#endif
