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

   memory.h: RAM operations
*/

#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include CONFIG_MCU_H
#include "smc.h"
/* PSRAM/BSRAM address map.  Included here because nearly every user of this
   header wants both halves. */
#include "memmap.h"

extern char current_filename[];

#define LOADROM_WITH_SRAM   (1)
#define LOADROM_WITH_RESET  (2)
#define LOADROM_WAIT_SNES   (4)
#define LOADROM_WITH_FPGA   (8)
#define LOADROM_WITH_COMBO  (16)

#define LOADRAM_AUTOSKIP_HEADER (1)

#define SAVE_BASEDIR    ("/sd2snes/saves/")

#define min(a,b) \
 ({ __typeof__ (a) _a = (a); \
 __typeof__ (b) _b = (b); \
 _a < _b ? _a : _b; })

uint32_t load_rom(uint8_t* filename, uint32_t base_addr, uint8_t flags);
void assert_reset(void);
void init(uint8_t *filename);
void deassert_reset(void);
uint32_t load_spc(uint8_t* filename, uint32_t spc_data_addr, uint32_t spc_header_addr);
/* Multi-slot battery SRAM (CICLO 2). SRM_SLOT_COUNT slots: internal index 0..3,
   UI "SLOT 1..4". Slot 0 = the legacy <stem>.srm (byte-identical); slots 1..3 =
   <stem>.02/03/04.srm. Gated by CFG.enable_sram_slots (OFF -> srm_slot forced 0). */
#define SRM_SLOT_COUNT 4
extern uint8_t srm_slot;      /* LIVE session slot used for save/load naming. Set once in
                                 migrate_and_load_srm (game load), IMMUTABLE for the session so
                                 an in-game slot switch can never misroute an autosave. */
extern uint8_t srm_slot_sel;  /* selected/next slot (== sidecar). == srm_slot at load; the in-game
                                 SET command writes the sidecar + updates this; applies next load. */
/* Build the slot's file extension (".srm" for slot 0, ".0N.srm" N=slot+1 for 1..3) into ext. */
void srm_slot_ext(char *ext, size_t extlen, uint8_t slot);
/* Read the /sd2snes/saves/<stem>.slot sidecar into srm_slot/srm_slot_sel (both 0 when
   CFG off / absent / invalid). Called at game load. Bounded (one f_read). */
void srm_slot_load(uint8_t *filename);
/* Write the sidecar (in-game SET). Updates srm_slot_sel; NEVER touches the live srm_slot.
   Bounded (one f_write). */
void srm_slot_save(uint8_t *filename, uint8_t slot);

uint32_t migrate_and_load_srm(uint8_t *filename, uint32_t base_addr);
uint32_t load_sram(uint8_t* filename, uint32_t base_addr);
uint32_t load_sram_offload(uint8_t* filename, uint32_t base_addr, uint8_t flags);
uint32_t load_sram_rle(uint8_t* filename, uint32_t base_addr);
uint32_t load_bootrle(uint32_t base_addr);
void load_dspx(const uint8_t* filename, uint8_t st0010);
void sram_hexdump(uint32_t addr, uint32_t len);
uint8_t sram_readbyte(uint32_t addr);
uint16_t sram_readshort(uint32_t addr);
uint32_t sram_readlong(uint32_t addr);
void sram_writebyte(uint8_t val, uint32_t addr);
void sram_writeshort(uint16_t val, uint32_t addr);
void sram_writelong(uint32_t val, uint32_t addr);
uint16_t sram_readblock(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_readstrn(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_writestrn(void* buf, uint32_t addr, uint16_t size);
void sram_readlongblock(uint32_t* buf, uint32_t addr, uint16_t count);
uint16_t sram_writeblock(void* buf, uint32_t addr, uint16_t size);
void save_srm(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
void saveinfo_stage(uint8_t *filename);
extern uint8_t current_ips_srm_source[256];
extern uint8_t current_ips_flags;
extern uint8_t rom_export_active;
extern uint32_t patch_export_size;
extern uint8_t patch_last_ok;
int  save_sram(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
uint32_t calc_sram_crc(uint32_t base_addr, uint32_t size, uint32_t crc);
uint16_t calc_sram_sum(uint32_t base_addr, uint32_t size);
uint8_t sram_reliable(void);
void sram_memset(uint32_t base_addr, uint32_t len, uint8_t val);

/* BS 8M Memory Pack (the BS_PACK_ADDR/BS_PACK_SIZE region lives in memmap.h). */
uint8_t load_bs_pack(uint8_t* filename);  /* returns 1 if a real pack was loaded */
void save_bs_pack(uint8_t* filename);
uint32_t calc_sram_crc_raw(uint32_t addr, uint32_t size); /* no reset bail: caller holds reset */
uint32_t calc_pack_crc_inreset(void);     /* reset-tolerant pack CRC for prepare_reset */

#endif
