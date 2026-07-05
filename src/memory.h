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

#include CONFIG_MCU_H
#include "smc.h"

extern char current_filename[];

#define MENU_ADDR_BRAM_SRC           (0xFF00)

#define SRAM_ROM_ADDR                (0x000000L)
#define SRAM_SAVE_ADDR               (0xE00000L)

#define SRAM_MENU_ADDR               (0xC00000L)
#define SRAM_DIR_ADDR                (0xC20000L)
#define SRAM_DB_ADDR                 (0xC80000L)
#define SRAM_COVER_ADDR              (0xC90000L) /* bank C9: per-ROM cover preview staging */
#define SRAM_GAMEINFO_TILES_ADDR     (0xCA0000L) /* bank CA: game-info DirectColor 8bpp tiles (up to ~48KB) */
#define SRAM_GAMEINFO_TMAP_ADDR      (0xCB0000L) /* bank CB: game-info 16-bit BG tilemap */

#define SRAM_NUM_CHEATS              (0xFF0700L)
#define SRAM_CHEAT_OVL_GATE_ADDR     (0xFF0710L) /* 1 byte the firmware arms at game load = CFG.enable_cheat_overlay && !special_chip. The in-game overlay probe (snes/savestate.a65) reads it; 0 => don't open. Lives in the free $FF0701..$FF07FF gap between NUM_CHEATS and CHEAT_NAMES. */
#define SRAM_PPU_CLEAR_GATE_ADDR     (0xFF0711L) /* 1 byte the firmware arms in load_rom (before releasing the SNES) = CFG.clear_ppu_on_boot && ips_pending_index>0. game_handshake (snes/main.a65) reads it before boot; 1 => clear VRAM/CGRAM/OAM for a patched romhack that skips PPU init. Lockstep with PPU_CLEAR_GATE in snes/memmap.i65; same free $FF0701..$FF07FF gap. */
#define SRAM_CHEAT_ADDR              (0xD00000L) /* up to 512 cheat records (512 bytes each), spans banks D0..D3 */
#define SRAM_CHEAT_CODE_STRINGS_ADDR (0xD40000L) /* per-code display strings, 12 bytes each. cheat_idx*512 + code_idx*12. Spans D4..D7, leaving D0..D3 free for up to 512 cheat records. */

#define SRAM_CHEAT_TITLE_ADDR        (0xD80000L) /* 256 bytes "Cheats for <game>" null-terminated, in cheat region past any plausible cheat count */
#define SRAM_CHEAT_FLAGS_ADDR        (0xFF0500L) /* 512 bytes BSRAM mirror of cheat flag byte 0 (cheats 0..511). SNES reads/writes here for instant visual toggle. */
#define SRAM_CHEAT_NAMES_ADDR        (0xFF0800L) /* in-game cheat overlay: first CHEAT_NAME_INGAME_MAX names, CHEAT_NAME_INGAME_LEN bytes each (31 visible + NUL), staged at game load. 64*32 = 2 KB -> fills FF0800..FF0FFF (up to FF1000=SRAM_CMD_ADDR). */

#define SRAM_SKIN_ADDR               (0xF00000L)

#define SRAM_SPC_DATA_ADDR           (0xFD0000L)
#define SRAM_SPC_HEADER_ADDR         (0xFE0000L)
#define SRAM_SAVESTATE_HANDLER_ADDR  (0xFE1000L)

#define SRAM_MENU_FILEPATH_ADDR      (0xFF0000L)
#define SRAM_MENU_CFG_ADDR           (0xFF0100L)
#define SRAM_CMD_ADDR                (0xFF1000L)
#define SRAM_PARAM_ADDR              (0xFF1004L)
#define SRAM_MCU_STATUS_ADDR         (0xFF1100L)
#define SRAM_SNES_STATUS_ADDR        (0xFF1110L)
#define SRAM_SYSINFO_ADDR            (0xFF1200L)
#define SRAM_LASTGAME_ADDR           (0xFF1420L)
#define SRAM_LASTGAME_DIR_ADDR       (0xFF1F00L)
/* WiFi status/scan + connect block, RESERVED for the Companion port. Its OWN dedicated
   437-byte block (0xFF4000..0xFF41B5) in the free gap left when favorites moved off the
   old 0xFF4000 slot to 0xFF6000 -- no longer aliases SRAM_SYSINFO_ADDR, so the WiFi and
   sysinfo screens can coexist. Layout = WIFI_OFF_* in src/snes.h; lockstep with WIFI_BLK
   in snes/memmap.i65. LAST_GAME_FILE (0xFF4A00) stays clear above it. */
#define SRAM_WIFI_ADDR               (0xFF4000L)
/* Favorites mirror, 20*256 = 0x1400 bytes -> 0xFF6000..0xFF73FF.  Relocated out of
   the old 0xFF4000 slot (which only fit 10 entries before LAST_GAME_FILE) into the
   free gap past IPS_LIST so growing to 20 needed only this one address (kept in
   lockstep with FAVORITE_GAMES in snes/memmap.i65).  The old 0xFF4000 slot now hosts
   SRAM_WIFI_ADDR (437 B); the rest of 0xFF4000..0xFF49FF stays free.  MAX_FAVORITE_GAMES
   (cfg.h) sizes this; nothing else lives up to SCRATCHPAD. */
#define SRAM_FAVORITEGAMES_ADDR      (0xFF6000L)
/* base ROM basename of the most recent game, for reset_to_menu==3 (Rom) pre-select.
   Distinct from SRAM_LASTGAME_ADDR[0] (the recents *display* name, which for a
   patch-aware "<rom>\t<patch>" entry is the patch name and would never match a
   TYPE_ROM entry in the folder). Lives in the (now fully) free gap before
   IPS_LIST (0xFF5000); the favorites list was moved off 0xFF4000 to 0xFF6000. */
#define SRAM_LASTGAME_FILE_ADDR      (0xFF4A00L)
/* IPS/BPS patch list staged by ips_find_patches(). Layout (see patch.h):
   +0 num_patches, +1 display names (8*IPS_NAME_LEN=512 -> [1,513)), +IPS_PATH_BASE(520)
   full SD paths (8*IPS_PATH_LEN=2048 -> [520,2568) = 0xFF5000..0xFF5A08). Ends well below
   the favorites mirror at 0xFF6000 (0x5F8 = 1528 bytes of slack). Lockstep with IPS_LIST
   in snes/memmap.i65 (the menu reads only the name region, not the paths). */
#define SRAM_IPS_LIST_ADDR           (0xFF5000L)
/* packed gameinfo_meta_t for the pre-boot info screen (see gameinfo.h). Sits AFTER the
   favorites mirror (0xFF6000..0xFF73FF) -- it must NOT overlap it, or opening a favorite's
   info panel clobbers the favorites list (lockstep with GAMEINFO in snes/memmap.i65). */
#define SRAM_GAMEINFO_ADDR           (0xFF7400L)
/* full (untruncated) info-screen description, char[2048] NUL-terminated, staged on demand
   by SNES_CMD_GI_DESC_FULL (the "full description" Y mode). The struct's description[256]
   above is capped by the YAML parser (YAML_BUFLEN); this region carries the complete text,
   read with a streaming line scanner outside the YAML parser. A 1st byte of 0 = invalid ->
   the menu falls back to the struct's description[256]. Sits after the struct's end
   ($FF759D) and before SRAM_SCRATCHPAD ($FFFF00); lockstep with GI_DESC_EXT in
   snes/memmap.i65. */
#define SRAM_GAMEINFO_DESCEXT_ADDR   (0xFF7600L)
#define SRAM_SCRATCHPAD              (0xFFFF00L)
#define SRAM_DIRID                   (0xFFFFF0L)
#define SRAM_RELIABILITY_SCORE       (0x100)

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
extern uint8_t current_ips_srm_source[256];
void save_sram(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
uint32_t calc_sram_crc(uint32_t base_addr, uint32_t size, uint32_t crc);
uint16_t calc_sram_sum(uint32_t base_addr, uint32_t size);
uint8_t sram_reliable(void);
void sram_memset(uint32_t base_addr, uint32_t len, uint8_t val);

/* BS 8M Memory Pack: 1MB in PSRAM at 0x900000, mapped LoROM $C0-$DF / HiROM $E0-$EF
   when FEAT_BSSLOT is set (0x900000 must match BS_PACK_HIT in address.v).  SD: .mpk */
#define BS_PACK_ADDR  0x900000
#define BS_PACK_SIZE  0x100000
uint8_t load_bs_pack(uint8_t* filename);  /* returns 1 if a real pack was loaded */
void save_bs_pack(uint8_t* filename);
uint32_t calc_pack_crc_inreset(void);     /* reset-tolerant pack CRC for prepare_reset */

#endif
