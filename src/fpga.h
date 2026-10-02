/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   fpga.h: FPGA (re)configuration
*/

#ifndef FPGA_H
#define FPGA_H

#include "bits.h"
#include "config.h"

void fpga_set_prog_b(uint8_t val);
void fpga_set_cclk(uint8_t val);
int fpga_get_initb(void);
/* DONE pin: 1 = a valid bitstream is loaded.  Was fpga.c-internal; exported
   for the NES-load wedge diagnostics (nes_dbg_post_pgm detects fpga_pgm's
   silent open-failure by DONE + fpga_config not being updated). */
int fpga_get_done(void);

void fpga_init(void);
void fpga_postinit(void);
void fpga_pgm(uint8_t* filename);
void fpga_rompgm(void);

extern uint8_t SPI_OFFLOAD;

extern const uint8_t *fpga_config;
extern uint8_t fpga_boot_led;

#define FPGA_CX4 ((const uint8_t*)"/sd2snes/fpga_cx4." FPGA_CONF_EXT)
#define FPGA_OBC1 ((const uint8_t*)"/sd2snes/fpga_obc1." FPGA_CONF_EXT)
#define FPGA_COL20 ((const uint8_t*)"/sd2snes/fpga_col20." FPGA_CONF_EXT)
#define FPGA_GSU ((const uint8_t*)"/sd2snes/fpga_gsu." FPGA_CONF_EXT)
/* mk2-only GSU variant: FX3 mode in, MSU-1 audio DAC out (the full core
   overmaps the Spartan-3).  Selected by smc.c when has_fx3 on CONFIG_MK2. */
#define FPGA_GSU3 ((const uint8_t*)"/sd2snes/fpga_gsu3." FPGA_CONF_EXT)
#define FPGA_SA1 ((const uint8_t*)"/sd2snes/fpga_sa1." FPGA_CONF_EXT)
#define FPGA_SDD1 ((const uint8_t*)"/sd2snes/fpga_sdd1." FPGA_CONF_EXT)
#define FPGA_SGB ((const uint8_t*)"/sd2snes/fpga_sgb." FPGA_CONF_EXT)
/* NES core (fpganes adaptado, verilog/sd2snes_nes) -- mk3-only: so existe
   fpga_nes.bi3; nenhum .bit mk2 e' planejado (ver nes.c). */
#define FPGA_NES ((const uint8_t*)"/sd2snes/fpga_nes." FPGA_CONF_EXT)
#define FPGA_SPC7110 ((const uint8_t*)"/sd2snes/fpga_spc7110." FPGA_CONF_EXT)
#define FPGA_SMS ((const uint8_t*)"/sd2snes/fpga_sms." FPGA_CONF_EXT)
/* Atari 2600 core (verilog/sd2snes_a26) -- mk3-only, like NES/SMS: only fpga_a26.bi3
   exists, no mk2 .bit is planned (see atari.c). */
#define FPGA_A26 ((const uint8_t*)"/sd2snes/fpga_a26." FPGA_CONF_EXT)
/* Game Boy Color core (verilog/sd2snes_gbc, the SGB core promoted to CGB) -- mk3-only:
   only fpga_gbc.bi3 exists, no mk2 .bit is planned (see gbc.c; on the mk2 a .gb/.gbc
   keeps booting the SGB core).  Selected by gbc_id() through sgb_update_romprops. */
#define FPGA_GBC ((const uint8_t*)"/sd2snes/fpga_gbc." FPGA_CONF_EXT)
#define FPGA_BASE ((const uint8_t*)"/sd2snes/fpga_base." FPGA_CONF_EXT)
/* mk2-only variant of the base core (verilog/sd2snes_basex: the same source with BASE_EXT).
   The Spartan-3 base core leaves the cartridge-specific address decoder extensions out so
   that it meets timing; this one keeps them: Sufami Turbo, the Gamars window and the BS
   Memory Pack slot / BS-LoROM remap. Chosen in load_reconfigure_fpga() -- never stored in
   romprops.fpga_conf, so every "is this the base core" test still sees the base core. */
#define FPGA_BASEX ((const uint8_t*)"/sd2snes/fpga_basex." FPGA_CONF_EXT)
#define FPGA_DSP ((const uint8_t*)"/sd2snes/fpga_dsp." FPGA_CONF_EXT)
/* Dedicated ST011 core (verilog/sd2snes_st0011): the uPD96050's 16384-word
   program is fetched from the Bus 2 SRAM through a block-RAM cache, which has no
   room in fpga_dsp next to the DSP1-4 on-chip program ROM. ST010 stays on
   fpga_dsp. Exists for both mk2 (.bit) and mk3 (.bi3). */
#define FPGA_ST0011 ((const uint8_t*)"/sd2snes/fpga_st0011." FPGA_CONF_EXT)
#define FPGA_ST0018 ((const uint8_t*)"/sd2snes/fpga_st0018." FPGA_CONF_EXT)
/* Copy-protected unlicensed LoROM bootlegs (verilog/sd2snes_bootleg): fpga_base
   plus the protection latch/ALU of the Pokemon/KOF2000/Soul Blade/Tekken 2 carts.
   Picked by ROM CRC32 in bootleg.c; the variant travels in chipfeat[2:0]. */
#define FPGA_BOOTLEG ((const uint8_t*)"/sd2snes/fpga_bootleg." FPGA_CONF_EXT)
/* mk2: boot-display bootstrap config ("fpga_mini"), loaded from SD instead of
   baked into the firmware to reclaim ~21 KB of the tight 128 KB flash. See
   fpga_rompgm(). mk3/mk3-stm32 keep it embedded (cfgware). */
#define FPGA_MINI ((const uint8_t*)"/sd2snes/fpga_mini." FPGA_CONF_EXT)
/* RAM wiring test core (verilog/sd2snes_test), used by the menu's "Memory test" entry.
   The only core that wires the MCU to RAM1 (the runtime cores leave RAM_DATA/RAM_ADDR
   undriven), and the same one the official diagnostic firmware uses -- so a result from
   the menu is comparable with one from that image.  Loaded on demand and never left
   configured: main()'s outer loop puts fpga_base back on the next menu boot. */
#define FPGA_MEMTEST ((const uint8_t*)"/sd2snes/fpga_test." FPGA_CONF_EXT)
#define FPGA_ROM ((const uint8_t*)"rom")

#define FPGA_TEST_TOKEN	(0xa5)

// some macros for bulk transfers (faster)
#define SET_CCLK()          do {SET_BIT(FPGA_CCLKREG, FPGA_CCLKBIT);} while (0)
#define CLR_CCLK()          do {CLEAR_BIT(FPGA_CCLKREG, FPGA_CCLKBIT);} while (0)
#define CCLK()              do {SET_CCLK(); CLR_CCLK();} while (0)

#endif