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

   snes.h: SNES hardware control and monitoring
*/

#ifndef SNES_H
#define SNES_H

#define SNES_CMD_LOADROM             (0x01)
#define SNES_CMD_SETRTC              (0x02)
#define SNES_CMD_SYSINFO             (0x03)
#define SNES_CMD_LOADLAST            (0x04)
#define SNES_CMD_LOADSPC             (0x05)
#define SNES_CMD_LOADFAVORITE        (0x06)
#define SNES_CMD_SET_ALLOW_PAIR      (0x07)
#define SNES_CMD_SET_VIDMODE_GAME    (0x08)
#define SNES_CMD_SET_VIDMODE_MENU    (0x09)
#define SNES_CMD_READDIR             (0x0a)
#define SNES_CMD_FPGA_RECONF         (0x0b)
#define SNES_CMD_LOAD_CHT            (0x0c)
#define SNES_CMD_SAVE_CHT            (0x0d)
#define SNES_CMD_SAVE_CFG            (0x0e)
#define SNES_CMD_LED_BRIGHTNESS      (0x12)
#define SNES_CMD_ADD_FAVORITE_ROM    (0x13)
#define SNES_CMD_ADD_FAVORITE_RECENT (0x14)
#define SNES_CMD_REMOVE_FAVORITE_ROM (0x15)
#define SNES_CMD_REMOVE_RECENT_ROM   (0x16)
#define SNES_CMD_SET_AUTOBOOT_ROM    (0x17) /* set autoboot from file browser selection */
#define SNES_CMD_SET_AUTOBOOT_FAV    (0x18) /* set autoboot from favorites list (index in MCU_PARAM) */
#define SNES_CMD_SET_AUTOBOOT_RECENT (0x19) /* set autoboot from recent games list (index in MCU_PARAM) */
#define SNES_CMD_CLR_AUTOBOOT_ROM    (0x1a) /* clear autoboot ROM setting */
#define SNES_CMD_LOAD_AUTOBOOT       (0x1b) /* boot into the stored autoboot ROM */
#define SNES_CMD_LOAD_COVER          (0x1c) /* stage sibling .cov for the highlighted ROM */
#define SNES_CMD_QUERY_IPS_PATCHES   (0x1d) /* find IPS patches for selected ROM */
#define SNES_CMD_LOAD_MENU_SPC       (0x1e) /* stage /sd2snes/menu.spc for background menu music */
#define SNES_CMD_LOAD_COVER_RECENT   (0x1f) /* stage downscaled .cov for recent game (index in MCU_PARAM) */
#define SNES_CMD_LOAD_COVER_FAVORITE (0x20) /* stage downscaled .cov for favorite game (index in MCU_PARAM) */
#define SNES_CMD_DELETE_FILE         (0x21) /* delete selected file */
#define SNES_CMD_DELETE_SRM          (0x22) /* delete SRM save file for selected ROM */
#define SNES_CMD_TOGGLE_CHT          (0x23) /* MCU_PARAM low byte: cheat index. XORs flag bit in PSRAM record. */
#define SNES_CMD_LOAD_CHT_FAV        (0x24) /* MCU_PARAM low byte: favorite index. Resolve path via cfg_get_listed_game(FAVORITES_FILE, ...) then cheat_yaml_load. */
#define SNES_CMD_SAVE_CHT_FAV        (0x25) /* MCU_PARAM low byte: favorite index. Resolve path via cfg_get_listed_game(FAVORITES_FILE, ...) then cheat_yaml_save. */
#define SNES_CMD_DELETE_FILE_FAV     (0x26) /* MCU_PARAM low byte: favorite index. Resolve path via FAVORITES_FILE, delete the ROM, then drop the list entry. */
#define SNES_CMD_DELETE_SRM_FAV      (0x27) /* MCU_PARAM low byte: favorite index. Resolve path via FAVORITES_FILE, delete only the .srm (ROM stays in favorites). */
#define SNES_CMD_DELETE_FILE_RECENT  (0x28) /* MCU_PARAM low byte: recent index. Resolve path via LAST_FILE, delete the ROM, then drop the list entry. */
#define SNES_CMD_DELETE_SRM_RECENT   (0x29) /* MCU_PARAM low byte: recent index. Resolve path via LAST_FILE, delete only the .srm (ROM stays in recents). */
#define SNES_CMD_LOAD_CHT_RECENT     (0x2a) /* MCU_PARAM low byte: recent index. Resolve path via LAST_FILE then cheat_yaml_load. */
#define SNES_CMD_SAVE_CHT_RECENT     (0x2b) /* MCU_PARAM low byte: recent index. Resolve path via LAST_FILE then cheat_yaml_save. */
#define SNES_CMD_SET_THEME           (0x2c) /* selected .thm (any visible folder): get_selected_name -> store full path in CFG.skin_name, then reload menu */
#define SNES_CMD_CLR_THEME           (0x2d) /* clear the menu theme back to the baked default, then reload menu */
#define SNES_CMD_SET_MENU_SPC        (0x2e) /* selected .spc (any visible folder): get_selected_name -> store full path in CFG.bgm_name, enable music, then reload menu (in-place restart black-screened; see main.c) */
#define SNES_CMD_CLR_MENU_SPC        (0x2f) /* clear CFG.bgm_name -> revert menu BGM to the /sd2snes/menu.spc fallback */
/* WiFi-in-menu commands (bridged to the ESP via uart_proto WIFI_* opcodes).
   RESERVED here so the command map stays lockstep with feat-esp32companion: the
   Companion port owns 0x30-0x33, which is why game-info lives at 0x34-0x37 below.
   No MCU handler in this branch (no ESP link yet) -- adding WiFi later is purely
   additive (fill in the cases), with zero command renumbering. */
#define SNES_CMD_WIFI_SCAN           (0x30) /* RESERVED (Companion): queue an AP scan on the ESP */
#define SNES_CMD_WIFI_GET            (0x31) /* RESERVED (Companion): write current status + scan list to SRAM (WIFI_BLK) */
#define SNES_CMD_WIFI_CONNECT        (0x32) /* RESERVED (Companion): connect using ssid/pass the menu wrote to SRAM */
#define SNES_CMD_WIFI_FORGET         (0x33) /* RESERVED (Companion): forget the saved network */
/* game-info commands live at 0x34-0x37 (WiFi owns 0x30-0x33) -- see reservation note above */
#define SNES_CMD_GAME_INFO           (0x34) /* parse /sd2snes/info/<rom>.yml + stage cover/screenshot for the pre-boot info screen (non-booting; like LOAD_COVER) */
#define SNES_CMD_GAME_INFO_RECENT    (0x35) /* like GAME_INFO but for the recent game at the index in MCU_PARAM (resolved via LAST_FILE) */
#define SNES_CMD_GAME_INFO_FAVORITE  (0x36) /* like GAME_INFO but for the favorite game at the index in MCU_PARAM (resolved via FAVORITES_FILE) */
#define SNES_CMD_FMV_NEXT            (0x37) /* pre-boot info screen FMV pump: stream the next <rom>.fmv frame into the band tile bank ($CA0000) for the menu to re-DMA (gameinfo_fmv_next). Non-booting. */
#define SNES_CMD_GI_DESC_FULL        (0x38) /* info-screen "full description" (Y): re-scan the .yml with a streaming reader and stage the COMPLETE (untruncated) description into SRAM_GAMEINFO_DESCEXT_ADDR ($FF7600). Non-booting (gameinfo_desc_full). */
#define SNES_CMD_RESTORE_CLASSIC     (0x39) /* apply the classic (pre-2.16) look from the fixed path THEME_CLASSIC (/sd2snes/classic.thm), then reload menu. NACKs without touching CFG.skin_name when the file is missing (see main.c) */
#define SNES_CMD_EXPORT_PATCHED_ROM  (0x3a) /* patch selector context menu: load the ROM, apply the patch at MCU_PARAM+7 (1..IPS_MAX_PATCHES) and write a .sfc next to it named after the PATCH FILE ("Foo (USA) - [BR].ips" -> "Foo (USA) - [BR].sfc"), then reload the menu. Refused up front (PATCH_EXPORT_EXISTS) when that .sfc already exists. Takes tens of seconds; the SNES stays in reset throughout. */
#define SNES_CMD_PATCH_META_SAVE     (0x3b) /* patch selector: rewrite /sd2snes/patches/<BB>/<stem>.yml from the flags bytes the menu edited (IPS_FLAGS_BASE) plus the last live scan, pruning entries whose patch is gone. MCU_PARAM holds cwd+direntry like LOADROM. Non-booting. Was 0x39 while this branch was developed in parallel with RESTORE_CLASSIC, which took that opcode first. */
#define SNES_CMD_EXPORT_CHECK        (0x3c) /* pre-flight for EXPORT_PATCHED_ROM: same MCU_PARAM contract (+7 = patch index), but only answers "would the .sfc collide?" -- writes PATCH_EXPORT_NONE (free) or PATCH_EXPORT_EXISTS (taken; the existing path is staged in SRAM_EXPORT_PATH_ADDR) to SRAM_EXPORT_RESULT_ADDR, then ACKs $55. The answer rides the persistent byte, NOT an $aa on SNES_CMD -- the menu loop re-arms $55 right after, the same race SRAM_LOAD_NACK_ADDR exists for. Non-booting: lets the menu refuse IN PLACE (modal over the live patch dialog) instead of tearing the screen down and cold-booting just to say no. */

/* WiFi SRAM block layout (base = SRAM_WIFI_ADDR; menu side = WIFI_BLK $FF4000).
   RESERVED for the Companion port -- its own dedicated 437-byte block ($FF4000..$FF41B5),
   no longer aliases the sysinfo block (so the two can coexist). Kept here so the future
   WiFi code uses these offsets. */
#define WIFI_OFF_CONNECTED  0    /* u8  */
#define WIFI_OFF_RSSI       1    /* i8  */
#define WIFI_OFF_SSID       2    /* char[33] */
#define WIFI_OFF_IP         35   /* char[16] */
#define WIFI_OFF_SCAN_CNT   51   /* u8  */
#define WIFI_OFF_SCAN_SEQ   52   /* u8  */
#define WIFI_OFF_APS        53   /* scan_cnt * { i8 rssi, u8 enc, char ssid[33] } = 35B each */
#define WIFI_AP_STRIDE      35
#define WIFI_OFF_REQ_SSID   340  /* char[33] - menu writes (CONNECT) */
#define WIFI_OFF_REQ_PASS   373  /* char[64] - menu writes (CONNECT) */

#define SNES_CMD_SAVESTATE           (0x40)
#define SNES_CMD_LOADSTATE           (0x41)
#define SNES_CMD_CHEAT_REPROGRAM     (0x42) /* in-game cheat overlay: reconcile BSRAM flag mirror ($FF0500) into the canonical PSRAM records and re-deploy all cheats live (no reboot) */
#define SNES_CMD_SET_SRM_SLOT        (0x44) /* in-game SAVES tab: persist the selected battery-SRAM slot (MCU_PARAM low byte = 0..3) to the /sd2snes/saves/<stem>.slot sidecar; refreshes SAVEINFO+42/SRM_SLOT_STATUS $FF0717. NEVER changes the live session slot (applies on the next game load). ACK = snes_set_mcu_cmd(0). Bounded (one f_write + 4 f_stat); lockstep with CMD_SET_SRM_SLOT in snes/memmap.i65 */
#define SNES_CMD_MANUAL_S1PAGE       (0x46) /* in-game guides viewer, SCROLLABLE 1x: stage one whole scale-1 page (256px wide, 4bpp) into PSRAM $C30000/$C4B000/$C4C000 so the 1x view pans continuously over the page instead of jumping band to band. MCU_PARAM[0] = compacted guide, [1..2] = page u16 LE. Its PSRAM region is separate from the 2x page, so both stay resident and toggling is instant. Lockstep with CMD_MANUAL_S1PAGE in snes/memmap.i65 */
#define SNES_CMD_MANUAL_ZPAGE        (0x45) /* in-game guides viewer, scrollable 2x zoom: stage ONE whole 2x page (tiles split BG1/BG2 halves, prebuilt tilemap words, 128-colour palette) into PSRAM $C5/$C6. MCU_PARAM[0] = compacted guide (0..7), [1] = zoom page (== 1x block index), [2..3] rsvd; ACK = the standard snes_set_mcu_cmd(0) clear. Once staged, panning is pure PSRAM->VRAM DMA with NO further MCU traffic. Bounded (sequential sector-aligned reads + retry); lockstep with CMD_MANUAL_ZPAGE in snes/memmap.i65 */
#define SNES_CMD_CHEAT_NAMES_WINDOW  (0x47) /* in-game cheat overlay: stage a sliding 64-name window into SRAM_CHEAT_NAMES_ADDR so ALL cheats (up to CHEAT_RECORD_MAX) can be listed without growing the game-load staging. MCU_PARAM low 16 = absolute base index; the MCU reads the descriptions from the canonical PSRAM records ($D00000+512*i+1 -- the same frozen-SNES $D0 read cheat_reprogram_from_mirror already does) and publishes the new base to SRAM_CHEAT_WIN_BASE_ADDR. ACK = snes_set_mcu_cmd(0). Bounded (64 fixed reads, no SD); lockstep with CMD_CHEAT_NAMES_WINDOW in snes/memmap.i65 */

#define SNES_CMD_RESET               (0x80)
#define SNES_CMD_RESET_TO_MENU       (0x81)
#define SNES_CMD_ENABLE_CHEATS       (0x82)
#define SNES_CMD_DISABLE_CHEATS      (0x83)
#define SNES_CMD_KILL_NMIHOOK        (0x84)
#define SNES_CMD_TEMP_KILL_NMIHOOK   (0x85)
#define SNES_CMD_RESET_LOOP_FAIL     (0x88)
#define SNES_CMD_RESET_LOOP_PASS     (0x89)
#define SNES_CMD_RESET_LOOP_TIMEOUT  (0x8a)
#define SNES_CMD_COMBO_TRANSITION    (0x90)

#define SNES_CMD_GAMELOOP            (0xff)

#define MCU_CMD_RDY                  (0x55)
#define MCU_CMD_ERR                  (0xaa)

#define MENU_ERR_OK        (0x0)
#define MENU_ERR_FS        (0x1)
#define MENU_ERR_SUPPLFILE (0x2)
#define MENU_ERR_NOIMPL    (0x3)
#define MENU_ERR_CARDWP    (0x4)
#define MENU_ERR_NOHW      (0x5) /* file type needs hardware this unit lacks (mk3-only cores on mk2) */

#define SNES_RELEASE_RESET_DELAY_US (2)
#define SNES_RESET_PULSELEN_MS (5)
#define SNES_RESET_LOOP_TIMEOUT (20) // 10ms steps x20 = 200ms

#define SNES_BOOL_TRUE  (0x01)
#define SNES_BOOL_FALSE (0x00)
#define SNES_BOOL_UNDEF (0xff)

#define SNESCMD_MCU_CMD              (0x2a00)
#define SNESCMD_SNES_CMD             (0x2a02)
#define SNESCMD_MCU_PARAM            (0x2a04)
#define SNESCMD_SFX_MAILBOX          (0x2be0) /* menu sound effects: effect+1 (1-4), 0 = consumed. Dedicated byte in the unreferenced $2BB4-$2BEF gap. NOT 0x2a08: MCU_PARAM is a 12-byte region (0x2a04-0x2a0f; settime uses +11) - parking the mailbox inside it corrupted cover request params and saved garbled favorites names. */
#define SNESCMD_INGAME_HOOK          (0x2a10)
#define SNESCMD_RESET_HOOK           (0x2a7d)
#define SNESCMD_WRAM_CHEATS          (0x2ad8)
#define SNESCMD_NMI_RESET            (0x2ba0)
#define SNESCMD_NMI_RESET_TO_MENU    (0x2ba2)
#define SNESCMD_NMI_ENABLE_CHEATS    (0x2ba4)
#define SNESCMD_NMI_DISABLE_CHEATS   (0x2ba6)
#define SNESCMD_NMI_KILL_NMIHOOK     (0x2ba8)
#define SNESCMD_NMI_TMP_KILL_NMIHOOK (0x2baa)
#define SNESCMD_COMBO_VERSION        (0x2bb0)
#define SNESCMD_MAP                  (0x2bb2)
#define SNESCMD_NMI_ENABLE_BUTTONS   (0x2bfc)
#define SNESCMD_NMI_DISABLE_WRAM     (0x2bfe)
#define SNESCMD_NMI_WRAM_PATCH_COUNT (0x2bff)
#define SNESCMD_EXE                  (0x2c00)

#define ASM_LDA_IMM      (0xa9)
#define ASM_LDA_ABSLONG  (0xaf)
#define ASM_STA_ABSLONG  (0x8f)
#define ASM_ORA_IMM      (0x09)
#define ASM_AND_IMM      (0x29)
#define ASM_EOR_IMM      (0x49)
#define ASM_RTS          (0x60)
#define ASM_RTL          (0x6b)

#define SNES_BUTTON_LRET (0x3030)
#define SNES_BUTTON_LREX (0x2070)
#define SNES_BUTTON_LRSA (0x10b0)
#define SNES_BUTTON_LRSB (0x9030)
#define SNES_BUTTON_LRSY (0x5030)
#define SNES_BUTTON_LRSX (0x1070)

#define SRAM_REGION_SIZE (0x10000)

#define COMBO_VERSION    (0x1)

#define SNES_NUM_BUTTONS (12)

#define SNES_BOOTPRINT_MAX_LINES (24)

enum snes_button_bits {
  SNES_BUTTON_B       = 0x8000,
  SNES_BUTTON_Y       = 0x4000,
  SNES_BUTTON_SELECT  = 0x2000,
  SNES_BUTTON_START   = 0x1000,
  SNES_BUTTON_UP      = 0x0800,
  SNES_BUTTON_DOWN    = 0x0400,
  SNES_BUTTON_LEFT    = 0x0200,
  SNES_BUTTON_RIGHT   = 0x0100,
  SNES_BUTTON_A       = 0x0080,
  SNES_BUTTON_X       = 0x0040,
  SNES_BUTTON_L       = 0x0020,
  SNES_BUTTON_R       = 0x0010
};

enum snes_reset_state { SNES_RESET_NONE = 0, SNES_RESET_SHORT, SNES_RESET_LONG };

typedef struct __attribute__ ((__packed__)) _mcu_status {
  uint8_t rtc_valid;
  uint8_t num_recent_games;
  uint8_t pairmode;
  uint8_t num_favorite_games;
  uint8_t autoboot_enabled;        /* 1 if an autoboot ROM is configured */
  uint8_t reset_to_menu_active;    /* 1 if this boot is a reset-to-menu (not cold power-on) */
  uint8_t favorites_full;          /* 1 if the last "add favorite" was refused (list at MAX_FAVORITE_GAMES) */
  /* ORDER IS THE WIRE FORMAT: this struct is copied verbatim to ST_MCU_ADDR and the
     menu reads it by fixed offset (snes/memmap.i65).  Append only, and update the
     matching ST_* define in the same commit. */
  uint8_t restore_browser;         /* +7. 1 if this menu boot follows a theme/BGM change: reopen SRAM_BROWSER_DIR_ADDR
                                      and put the cursor on SRAM_BROWSER_FILE_ADDR (see browser_pos_save). One-shot,
                                      cleared in RAM right after status_load_to_menu() publishes it. */
  uint8_t is_mk2;                  /* +8. 1 on Mk.II (LPC1754). Board identity, published on
                                      every boot: m3nu.bin is ONE binary shared across configs,
                                      so anything the mk2 firmware cannot carry has to be gated
                                      at RUNTIME from here. No menu code gates on it today.
                                      Lockstep with ST_IS_MK2. */
} mcu_status_t;

typedef struct __attribute__ ((__packed__)) _snes_status {
  uint8_t is_u16;
  uint8_t u16_cfg;
  uint8_t has_satellaview;
} snes_status_t;

extern uint8_t crc_valid;
extern uint8_t resetButtonState;

void prepare_reset(void);
void snes_init(void);
void snes_reset_pulse(void);
void snes_reset(int state);
uint8_t get_snes_reset(void);
uint8_t get_snes_reset_state(void);
uint8_t snes_reset_loop(void);
uint8_t snes_main_loop(void);
/* serves the in-game shell / overlay commands shared by BOTH game loops (main.c and
   msu1_loop); returns 0 for commands the caller must handle itself. See snes.c. */
uint8_t game_cmd_serve(uint8_t cmd);
uint8_t menu_main_loop(void);
void get_selected_name(uint8_t* lfn);
void snes_bootprint(int line, void* fmt, ...);
void snes_bootclear(void);
void snes_bootprint_version(void);
void snes_bootprint_center(int line, void *fmt, ...);
void snes_menu_errmsg(int err, void* msg);
uint8_t snes_get_last_game_index(void);
uint8_t snes_get_mcu_cmd(void);
void snes_set_mcu_cmd(uint8_t cmd);
uint8_t snes_get_snes_cmd(void);
void snes_set_snes_cmd(uint8_t cmd);
void echo_mcu_cmd(void);
uint32_t snes_get_mcu_param(void);
void snescmd_writeshort(uint16_t val, uint16_t addr);
void snescmd_writebyte(uint8_t val, uint16_t addr);
uint16_t snescmd_writeblock(void *buf, uint16_t addr, uint16_t size);
uint16_t snescmd_readshort(uint16_t addr);
uint8_t snescmd_readbyte(uint16_t addr);
uint32_t snescmd_readlong(uint16_t addr);
uint16_t snescmd_readblock(void *buf, uint16_t addr, uint16_t size);
uint64_t snescmd_gettime(void);
uint16_t snescmd_readstrn(void *buf, uint16_t addr, uint16_t size);
void snescmd_prepare_nmihook(void);
void snes_get_filepath(uint8_t *buffer, uint16_t length);
void status_load_to_menu(void);
void status_save_from_menu(void);
void recalculate_sram_range(void);
#endif
