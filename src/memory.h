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
extern uint8_t exp_warn_dismissed;   /* the experimental-core box said "don't warn again"; see menucmd_launch_rom */
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
/* Read the game's SaveGameSlot (its per-game file, gameopt.h) into srm_slot/srm_slot_sel (both 0 when
   CFG off / absent / invalid). Called at game load. Bounded (one f_read). */
void srm_slot_load(uint8_t *filename);
/* Write the sidecar (in-game SET). Updates srm_slot_sel; NEVER touches the live srm_slot.
   Bounded (one f_write). */
void srm_slot_save(uint8_t *filename, uint8_t slot);
uint8_t slot_sidecar_state(const char *src);   /* the savestate slot (0..3) a game starts with */
void slot_sidecar_state_set(const char *src, uint8_t ss);

/* The menu's saves screen and the in-game SAVES tab: SAVES_BLK (SRAM_SAVES_ADDR in
   memmap.h, lockstep with SAVES_BLK / SVB_* in snes/memmap.i65).  A header, then one
   entry per slot: the four battery saves, then the four savestates. */
#define SAVES_SLOTS         4
#define SS_SLOT_COUNT       4
#define SAVES_KIND_SRM      0
#define SAVES_KIND_STATE    1
#define SAVES_VERSION       1
#define SAVES_HDR_SIZE      8
#define SAVES_H_MAGIC       0         /* 'S','V'; the menu zeroes +0 before asking */
#define SAVES_H_VERSION     2
#define SAVES_H_RESULT      3         /* SAVES_RES_* of the last action */
#define SAVES_H_SRM_SEL     4         /* the battery-save slot the game starts with, 0..3 */
#define SAVES_H_SS_SEL      5         /* the savestate slot the game starts with, 0..3 */
#define SAVES_H_FLAGS       6
#define SAVES_HF_SRM_SLOTS  0x01      /* EnableSramSlots: slots 2..4 of the battery saves */
#define SAVES_HF_SS_SLOTS   0x02      /* EnableSavestateSlots: slots 2..4 of the savestates */
#define SAVES_HF_STATES     0x04      /* the savestate entries were staged (the menu) */
#define SAVES_ENTRY_SIZE    32
#define SAVES_E_FLAGS       0
#define SAVES_EF_EXISTS     0x01
#define SAVES_E_DATE        1         /* "DD/MM/YYYY HH:MM" in the menu language's order, NUL */
#define SAVES_DATE_LEN      17
#define SAVES_E_SIZE        18        /* "8 KB", NUL */
#define SAVES_SIZE_LEN      8
#define SAVES_BLK_SIZE      (SAVES_HDR_SIZE + 2 * SAVES_SLOTS * SAVES_ENTRY_SIZE)
#define SAVES_OP_INFO       0         /* SNES_CMD_SAVES MCU_PARAM+9 */
#define SAVES_OP_USE        1
#define SAVES_OP_COPY       2
#define SAVES_OP_DELETE     3
#define SAVES_RES_OK        0
#define SAVES_RES_FAIL      1
uint8_t saves_stage(const char *src, uint8_t states, uint8_t result);
void saves_command(const char *src, uint8_t op, uint8_t kind, uint8_t slot, uint8_t dst);

uint32_t migrate_and_load_srm(uint8_t *filename, uint32_t base_addr);
uint32_t load_sram(uint8_t* filename, uint32_t base_addr);
uint32_t load_sram_offload(uint8_t* filename, uint32_t base_addr, uint8_t flags);
uint32_t load_sram_rle(uint8_t* filename, uint32_t base_addr);
uint32_t load_bootrle(uint32_t base_addr);
void load_dspx(const uint8_t* filename, uint16_t coretype);
void sram_hexdump(uint32_t addr, uint32_t len);
uint8_t sram_readbyte(uint32_t addr);
uint16_t sram_readshort(uint32_t addr);
uint32_t sram_readlong(uint32_t addr);
void sram_writebyte(uint8_t val, uint32_t addr);
void sram_writeshort(uint16_t val, uint32_t addr);
void sram_writelong(uint32_t val, uint32_t addr);
uint16_t sram_readblock(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_readstrn(void* buf, uint32_t addr, uint16_t size);
/* Directory string: display\0 [filesystem\0]. The second string, when present, is the UTF-8
   name FatFs can open; the first is what the menu prints (CJK-encoded). */
uint16_t sram_read_fsname(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_writestrn(void* buf, uint32_t addr, uint16_t size);
void sram_readlongblock(uint32_t* buf, uint32_t addr, uint16_t count);
uint16_t sram_writeblock(void* buf, uint32_t addr, uint16_t size);
void save_srm(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
void saveinfo_stage(uint8_t *filename);
/* The name a game's sidecars in /sd2snes/saves are keyed off: the patch of a patched
   load, the ROM otherwise (what the .srm and the per-game .yml use). */
const char *game_sidecar_src(uint8_t *filename);
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
/* .srm path of the live slot for a game load (reads the slot sidecar), NO legacy migration:
   used by the NES battery (nesave.c), whose file must never be renamed out of the ROM folder. */
int srm_path_for_load(char *buf, size_t buflen, uint8_t *filename);
int srm_path_for_save(char *buf, size_t buflen, uint8_t *filename); /* live slot, folder created */

#endif
