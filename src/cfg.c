#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "cfg.h"
#include "config.h"
#include "uart.h"
#include "fileops.h"
#include "memory.h"
#include "yaml.h"
#include "rtc.h"
#include "snes.h"

/* The SNES menu pokes config bytes by hard-coded offset (snes/memmap.i65,
   CFG_*=CFG_ADDR+$nn).  Pin the C struct layout to that map so inserting or
   resizing a field can never silently desync the menu from the firmware. */
_Static_assert(offsetof(cfg_t, language) == 0xB7, "cfg_t.language must stay at CFG_ADDR+$B7");
_Static_assert(offsetof(cfg_t, patch_verify_integrity) == 0xB8, "cfg_t.patch_verify_integrity must stay at CFG_ADDR+$B8");
_Static_assert(offsetof(cfg_t, covers_in_lists) == 0xBA, "cfg_t.covers_in_lists must stay at CFG_ADDR+$BA");
_Static_assert(offsetof(cfg_t, enable_menu_sfx) == 0xBB, "cfg_t.enable_menu_sfx must stay at CFG_ADDR+$BB");
_Static_assert(offsetof(cfg_t, bgm_name) == 0xBC, "cfg_t.bgm_name must stay at CFG_ADDR+$BC");
_Static_assert(offsetof(cfg_t, sort_favorites) == 0x13C, "cfg_t.sort_favorites must stay at CFG_ADDR+$13C");
_Static_assert(offsetof(cfg_t, enable_cheat_overlay) == 0x13D, "cfg_t.enable_cheat_overlay must stay at CFG_ADDR+$13D");
_Static_assert(offsetof(cfg_t, show_game_info) == 0x13E, "cfg_t.show_game_info must stay at CFG_ADDR+$13E");
_Static_assert(offsetof(cfg_t, enable_wifi) == 0x13F, "cfg_t.enable_wifi must stay at CFG_ADDR+$13F (relocated from $BD: overlapped bgm_name @ $BC in the esp32companion merge)");

cfg_t CFG_DEFAULT = {
  .vidmode_menu = VIDMODE_60,
  .vidmode_game = VIDMODE_AUTO,
  .pair_mode_allowed = 0,
  .bsx_use_usertime = 0,
  .bsx_time = {0x0, 0x3, 0x5, 0x0, 0x8, 0x1, 0x1, 0x0, 0x3, 0x7, 0x9, 0x9},
  .r213f_override = 1,
  .enable_ingame_hook = 0,
  .enable_ingame_buttons = 1,
  .enable_hook_holdoff = 1,
  .enable_screensaver = 1,
  .screensaver_timeout = 600,
  .sort_directories = 1,
  .hide_extensions = 0,
  .cx4_speed = 0,
  .skin_name = "sd2snes.skin",
  .control_type = 0,
  .msu_volume_boost = 0,
  .onechip_transient_fixes = 0,
  .brightness_limit = 15,
  .gsu_speed = 0,
  .reset_to_menu = 0,
  .led_brightness = 15,
  .enable_cheats = 1,
  .reset_patch = 1,
  .enable_ingame_savestate = 0,
  .loadstate_delay = 10,
  .enable_savestate_slots = 1,
  .ingame_buttons_savestate = SNES_BUTTON_START | SNES_BUTTON_R,
  .ingame_buttons_loadstate = SNES_BUTTON_START | SNES_BUTTON_L,
  .ingame_buttons_changestate = SNES_BUTTON_SELECT,
  .sgb_enable_ingame_hook = 0,
  .sgb_enable_state = 0,
  .sgb_volume_boost = 0,
  .sgb_enh_override = 0,
  .sgb_clock_fix = 1,
  .sgb_bios_version = 2,
  .show_tribute = 0,
  .enable_autosave = 1,
  .enable_autosave_msu1 = 1,
  .show_covers = 1,
  .language = 0,
  .patch_verify_integrity = 0,
  .enable_menu_music = 1,
  .covers_in_lists = 1,
  .enable_menu_sfx = 1,
  .bgm_name = "",
  .enable_wifi = 0,
  .sort_favorites = 0,
  .enable_cheat_overlay = 1,
  .show_game_info = 1,
};

cfg_t CFG;
extern mcu_status_t STM;

char *button_names = "BYsSudlrAXLR";

int cfg_save() {
  int err = 0;
  uint64_t bcdtime = srtctime2bcdtime(CFG.bsx_time);
  file_open((uint8_t*)CFG_FILE, FA_CREATE_ALWAYS | FA_WRITE);
  f_puts("---\n", &file_handle);
  f_puts("##############################\n", &file_handle);
  f_puts("# sd2snes configuration file #\n", &file_handle);
  f_puts("##############################\n\n", &file_handle);
  f_puts("# Allow SuperCIC Pair Mode (required for video mode setting)\n", &file_handle);
  f_printf(&file_handle, "%s: %s\n", CFG_PAIR_MODE_ALLOWED, CFG.pair_mode_allowed ? "true" : "false");
  f_printf(&file_handle, "\n# Video mode (%d = 60Hz, %d = 50Hz, %d = Auto (game only))\n", VIDMODE_60, VIDMODE_50, VIDMODE_AUTO);
  f_printf(&file_handle, "%s: %d\n", CFG_VIDMODE_MENU, CFG.vidmode_menu);
  f_printf(&file_handle, "%s: %d\n", CFG_VIDMODE_GAME, CFG.vidmode_game);
  f_printf(&file_handle, "\n# Satellaview Settings\n#  %s: use user defined time instead of real time\n", CFG_BSX_USE_USERTIME);
  f_printf(&file_handle, "#  %s: user defined Satellaview broadcast time (format: YYYYMMDDhhmmss)\n", CFG_BSX_TIME);
  f_printf(&file_handle, "%s: %s\n", CFG_BSX_USE_USERTIME, CFG.bsx_use_usertime ? "true" : "false");
  f_printf(&file_handle, "%s: %06lX%08lX\n", CFG_BSX_TIME, (uint32_t)(bcdtime>>32), (uint32_t)(bcdtime & 0xffffffffLL));
  f_puts("\n# Enable PPU region flag patching\n", &file_handle);
  f_printf(&file_handle, "%s: %s\n", CFG_R213F_OVERRIDE, CFG.r213f_override ? "true" : "false");
  f_puts("\n# Enable 1CHIP transient fixes (experimental) - Fix some 1CHIP related graphical issues\n", &file_handle);
  f_printf(&file_handle, "%s: %s\n", CFG_1CHIP_TRANSIENT_FIXES, CFG.onechip_transient_fixes ? "true" : "false");
  f_puts("\n# Brightness limit - can be used to limit RGB output levels on S-CPUN based consoles\n", &file_handle);
  f_printf(&file_handle, "%s: %d\n", CFG_BRIGHTNESS_LIMIT, CFG.brightness_limit);
  f_puts("\n# Reset to menu on short reset (0: off, 1: on, 2: on+return to last folder, 3: on+return to folder and pre-select ROM)\n", &file_handle);
  f_printf(&file_handle, "%s: %d\n", CFG_ENABLE_RST_TO_MENU, CFG.reset_to_menu);
  f_puts("\n# Initial cheats state when loading a game (true: enabled, false: disabled)\n", &file_handle);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_CHEATS, CFG.enable_cheats ? "true" : "false");
  f_puts("\n\n# IRQ hook related settings\n", &file_handle);
  f_printf(&file_handle, "#  %s: Overall enable IRQ hooks (required for in-game buttons & WRAM cheats)\n", CFG_ENABLE_INGAME_HOOK);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_INGAME_HOOK, CFG.enable_ingame_hook ? "true" : "false");
  f_printf(&file_handle, "#  %s: Enable in-game buttons (en/disable cheats, reset sd2snes...)\n", CFG_ENABLE_INGAME_BUTTONS);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_INGAME_BUTTONS, CFG.enable_ingame_buttons ? "true" : "false");
  f_printf(&file_handle, "#  %s: Enable 10s grace period after reset before enabling in-game hooks\n", CFG_ENABLE_HOOK_HOLDOFF);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_HOOK_HOLDOFF, CFG.enable_hook_holdoff ? "true" : "false");
  f_printf(&file_handle, "%s: %s\n", CFG_RESET_PATCH, CFG.reset_patch ? "true" : "false");
  f_puts("\n", &file_handle);
  f_printf(&file_handle, "#  %s: Enable in-game savestate\n", CFG_ENABLE_INGAME_SAVESTATE);
  f_printf(&file_handle, "%s: %d\n", CFG_ENABLE_INGAME_SAVESTATE, CFG.enable_ingame_savestate);
  f_printf(&file_handle, "#  %s: Load state delay (frames),\n", CFG_LOADSTATE_DELAY);
  f_printf(&file_handle, "%s: %d\n", CFG_LOADSTATE_DELAY, CFG.loadstate_delay);
  f_printf(&file_handle, "#  %s: Enable in-game savestate (0: disabled, 1: enabled)\n", CFG_ENABLE_INGAME_SAVESTATE);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_SAVESTATE_SLOTS, CFG.enable_savestate_slots ? "true" : "false");
  char buttons[13];
  cfg_buttons_bits2string(CFG.ingame_buttons_savestate, buttons);
  f_printf(&file_handle, "#  %s: In-game save state buttons (buttons: BYsSudlrAXLR, default: start+r (SR)),\n", CFG_INGAME_BUTTONS_SAVE_STATE);
  f_printf(&file_handle, "%s: %s\n", CFG_INGAME_BUTTONS_SAVE_STATE, buttons);
  cfg_buttons_bits2string(CFG.ingame_buttons_loadstate, buttons);
  f_printf(&file_handle, "#  %s: In-game load state buttons (buttons: BYsSudlrAXLR, default: start+l (SL)),\n", CFG_INGAME_BUTTONS_LOAD_STATE);
  f_printf(&file_handle, "%s: %s\n", CFG_INGAME_BUTTONS_LOAD_STATE, buttons);
  cfg_buttons_bits2string(CFG.ingame_buttons_changestate, buttons);
  f_printf(&file_handle, "#  %s: In-game change state slot buttons (buttons: BYsSudlrAXLR, don't use dpad buttons, default: select (s)),\n", CFG_INGAME_BUTTONS_CHANGE_STATE);
  f_printf(&file_handle, "%s: %s\n", CFG_INGAME_BUTTONS_CHANGE_STATE, buttons);
  f_puts("\n", &file_handle);
  f_printf(&file_handle, "#  %s: SGB enable hooks (%s or %s enables SGB hooks.  zero overhead.)\n", CFG_SGB_ENABLE_INGAME_HOOK, CFG_SGB_ENABLE_INGAME_HOOK, CFG_ENABLE_INGAME_HOOK);
  f_printf(&file_handle, "%s: %s\n", CFG_SGB_ENABLE_INGAME_HOOK, CFG.sgb_enable_ingame_hook ? "true" : "false");
  f_printf(&file_handle, "#  %s: SGB enable save states (only works with ingame hooks when supported boot and bios/snes is used)\n", CFG_SGB_ENABLE_STATE);
  f_printf(&file_handle, "%s: %s\n", CFG_SGB_ENABLE_STATE, CFG.sgb_enable_state ? "true" : "false");
  f_printf(&file_handle, "#  %s: SGB audio volume boost\n#    (0: none; 1: +3.5dBFS; 2: +6dBFS; 3: +9.5dBFS; 4: +12dBFS)\n", CFG_SGB_VOLUME_BOOST);
  f_printf(&file_handle, "%s: %d\n", CFG_SGB_VOLUME_BOOST, CFG.sgb_volume_boost);
  f_printf(&file_handle, "#  %s: SGB enhancements override (false: enhancements enabled; true: enhancements disabled)\n", CFG_SGB_ENH_OVERRIDE);
  f_printf(&file_handle, "%s: %s\n", CFG_SGB_ENH_OVERRIDE, CFG.sgb_enh_override ? "true" : "false");
#ifdef CONFIG_MK3
  f_printf(&file_handle, "#  %s: SGB sprite increase per scanline.  not compatible with all games  (false: 10 sprites (default); true: 16 sprites)\n", CFG_SGB_SPR_INCREASE);
  f_printf(&file_handle, "%s: %s\n", CFG_SGB_SPR_INCREASE, CFG.sgb_spr_increase ? "true" : "false");
#endif
  f_printf(&file_handle, "#  %s: SGB timing/clock (true: original/sgb2, false: snes/sgb1)\n", CFG_SGB_CLOCK_FIX);
  f_printf(&file_handle, "%s: %s\n", CFG_SGB_CLOCK_FIX, CFG.sgb_clock_fix ? "true" : "false");
  f_printf(&file_handle, "#  %s: SGB bios firmware version (defined number loads: sgbX_boot.bin and sgbX_snes.bin)\n", CFG_SGB_BIOS_VERSION);
  f_printf(&file_handle, "%s: %d\n", CFG_SGB_BIOS_VERSION, CFG.sgb_bios_version);

  f_puts("\n# Screensaver settings\n", &file_handle);
  f_printf(&file_handle, "#  %s: Enable screensaver\n", CFG_ENABLE_SCREENSAVER);
//  f_printf(&file_handle, "#  %s: Dim screen after n seconds\n", CFG_SCREENSAVER_TIMEOUT);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_SCREENSAVER, CFG.enable_screensaver ? "true" : "false");
//  f_printf(&file_handle, "%s: %d\n", CFG_SCREENSAVER_TIMEOUT, CFG.screensaver_timeout);
  f_puts("\n# UI related settings\n", &file_handle);
  f_printf(&file_handle, "#  %s: Sort directories (slower but files are guaranteed to be in order)\n", CFG_SORT_DIRECTORIES);
  f_printf(&file_handle, "#  %s: Hide file extensions\n", CFG_HIDE_EXTENSIONS);
  f_printf(&file_handle, "#  %s: LED brightness (0: minimum, 15: maximum)\n", CFG_LED_BRIGHTNESS);
  f_printf(&file_handle, "%s: %s\n", CFG_SORT_DIRECTORIES, CFG.sort_directories ? "true" : "false");
  f_printf(&file_handle, "%s: %s\n", CFG_HIDE_EXTENSIONS, CFG.hide_extensions ? "true" : "false");
  f_printf(&file_handle, "%s: %d\n", CFG_LED_BRIGHTNESS, CFG.led_brightness);
  f_puts("\n# Enhancement chip settings\n", &file_handle);
  f_printf(&file_handle, "#  %s: Cx4 core speed (0: original, 1: fast, all instructions are single cycle)\n", CFG_CX4_SPEED);
  f_printf(&file_handle, "%s: %d\n", CFG_CX4_SPEED, CFG.cx4_speed);
  f_printf(&file_handle, "#  %s: SuperFX core speed (0: original, 1: fast, instructions execute as fast as the implementation allows)\n", CFG_GSU_SPEED);
  f_printf(&file_handle, "%s: %d\n", CFG_GSU_SPEED, CFG.gsu_speed);
  f_printf(&file_handle, "#  %s: MSU audio volume boost\n#    (0: none; 1: +3.5dBFS; 2: +6dBFS; 3: +9.5dBFS; 4: +12dBFS)\n", CFG_MSU_VOLUME_BOOST);
  f_printf(&file_handle, "%s: %d\n", CFG_MSU_VOLUME_BOOST, CFG.msu_volume_boost);
  f_puts("\n# Autosave (save SRAM contents to card when changes are detected)\n", &file_handle);
  f_printf(&file_handle, "#  %s: Autosave for everything except MSU-1 games\n", CFG_ENABLE_AUTOSAVE);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_AUTOSAVE, CFG.enable_autosave ? "true" : "false");
  f_printf(&file_handle, "#  %s: Opportunistic Autosave for MSU-1 games\n", CFG_ENABLE_AUTOSAVE_MSU1);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_AUTOSAVE_MSU1, CFG.enable_autosave_msu1 ? "true" : "false");
  f_printf(&file_handle, "\n#  %s: Show per-ROM cover preview (Game.cov) in the file browser (0: off, 1: large, 2: small)\n", CFG_SHOW_COVERS);
  f_printf(&file_handle, "%s: %d\n", CFG_SHOW_COVERS, CFG.show_covers);
  f_printf(&file_handle, "#  %s: Also show covers in the Recent and Favorite lists (requires ShowCovers)\n", CFG_COVERS_IN_LISTS);
  f_printf(&file_handle, "%s: %s\n", CFG_COVERS_IN_LISTS, CFG.covers_in_lists ? "true" : "false");
  f_printf(&file_handle, "\n#  %s: Menu/firmware language (0: English, 1: Portugues BR, 2: Spanish, 3: German)\n", CFG_LANGUAGE);
  f_printf(&file_handle, "%s: %d\n", CFG_LANGUAGE, CFG.language);
  f_printf(&file_handle, "\n#  %s: Re-read and CRC-check the ROM after applying an IPS/BPS patch (slow; ~23s for a 4MB BPS)\n", CFG_PATCH_VERIFY_INTEGRITY);
  f_printf(&file_handle, "%s: %s\n", CFG_PATCH_VERIFY_INTEGRITY, CFG.patch_verify_integrity ? "true" : "false");
  f_printf(&file_handle, "\n#  %s: Play background music (/sd2snes/menu.spc) in the menu\n", CFG_ENABLE_MENU_MUSIC);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_MENU_MUSIC, CFG.enable_menu_music ? "true" : "false");
  f_printf(&file_handle, "#  %s: Play menu navigation sound effects (cursor/confirm/back/error)\n", CFG_ENABLE_MENU_SFX);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_MENU_SFX, CFG.enable_menu_sfx ? "true" : "false");
  f_printf(&file_handle, "\n#  %s: Enable the WiFi companion (false: no access point, no WebUI)\n", CFG_ENABLE_WIFI);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_WIFI, CFG.enable_wifi ? "true" : "false");
  f_printf(&file_handle, "#  %s: Show the Favorites list in alphabetical order (display only)\n", CFG_SORT_FAVORITES);
  f_printf(&file_handle, "%s: %s\n", CFG_SORT_FAVORITES, CFG.sort_favorites ? "true" : "false");
  f_printf(&file_handle, "#  %s: In-game cheat overlay (pause via L+R+Y+Left to toggle cheats). Off on special-chip games.\n", CFG_ENABLE_CHEAT_OVERLAY);
  f_printf(&file_handle, "%s: %s\n", CFG_ENABLE_CHEAT_OVERLAY, CFG.enable_cheat_overlay ? "true" : "false");
  f_printf(&file_handle, "\n#  %s: Selected menu theme file in /sd2snes/theme (\"%s\" = baked-in default)\n", CFG_SKIN_NAME, "sd2snes.skin");
  f_printf(&file_handle, "%s: %s\n", CFG_SKIN_NAME, (char*)CFG.skin_name);
  f_printf(&file_handle, "\n#  %s: Full path of the chosen menu background-music .spc (\"\" = /sd2snes/menu.spc fallback)\n", CFG_MENU_MUSIC_FILE);
  f_printf(&file_handle, "%s: %s\n", CFG_MENU_MUSIC_FILE, (char*)CFG.bgm_name);
  f_printf(&file_handle, "\n#  %s: Show the game info screen (cover/screenshot/metadata) before booting a ROM that has a /sd2snes/info entry\n", CFG_SHOW_GAME_INFO);
  f_printf(&file_handle, "%s: %s\n", CFG_SHOW_GAME_INFO, CFG.show_game_info ? "true" : "false");
  file_close();
  return err;
}

int cfg_load() {
  int err = 0;
  /* pre-load defaults */
  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));
  yaml_file_open(CFG_FILE, FA_READ);
  if(file_res) {
    err = file_res;
  }
  if(!err) {
    yaml_token_t tok;
    /* get config entries */
    if(yaml_get_itemvalue(CFG_VIDMODE_MENU, &tok)) {
      CFG.vidmode_menu = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_VIDMODE_GAME, &tok)) {
      CFG.vidmode_game = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_PAIR_MODE_ALLOWED, &tok)) {
      CFG.pair_mode_allowed = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_BSX_USE_USERTIME, &tok)) {
      CFG.bsx_use_usertime = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_BSX_TIME, &tok)) {
      uint64_t bcdtime = strtoll(tok.stringvalue, NULL, 16);
      bcdtime2srtctime(bcdtime, CFG.bsx_time);
    }
    if(yaml_get_itemvalue(CFG_R213F_OVERRIDE, &tok)) {
      CFG.r213f_override = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_INGAME_HOOK, &tok)) {
      CFG.enable_ingame_hook = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_INGAME_BUTTONS, &tok)) {
      CFG.enable_ingame_buttons = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_HOOK_HOLDOFF, &tok)) {
      CFG.enable_hook_holdoff = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_SCREENSAVER, &tok)) {
      CFG.enable_screensaver = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SORT_DIRECTORIES, &tok)) {
      CFG.sort_directories = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_HIDE_EXTENSIONS, &tok)) {
      CFG.hide_extensions = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_CX4_SPEED, &tok)) {
      CFG.cx4_speed = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_GSU_SPEED, &tok)) {
      CFG.gsu_speed = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_MSU_VOLUME_BOOST, &tok)) {
      CFG.msu_volume_boost = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_1CHIP_TRANSIENT_FIXES, &tok)) {
      CFG.onechip_transient_fixes = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_BRIGHTNESS_LIMIT, &tok)) {
      CFG.brightness_limit = tok.longvalue & 0xf;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_RST_TO_MENU, &tok)) {
      if(tok.type == YAML_BOOL) {
        CFG.reset_to_menu = tok.boolvalue ? 1 : 0;
      } else {
        CFG.reset_to_menu = tok.longvalue > 3 ? 1 : (uint8_t)tok.longvalue;
      }
    }
    if(yaml_get_itemvalue(CFG_LED_BRIGHTNESS, &tok)) {
      CFG.led_brightness = tok.longvalue;
      if(CFG.led_brightness > 15) {
        CFG.led_brightness = 15;
      }
    }
    if(yaml_get_itemvalue(CFG_ENABLE_CHEATS, &tok)) {
      CFG.enable_cheats = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_RESET_PATCH, &tok)) {
      CFG.reset_patch = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_INGAME_SAVESTATE, &tok)) {
      CFG.enable_ingame_savestate = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_LOADSTATE_DELAY, &tok)) {
      CFG.loadstate_delay = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_SAVESTATE_SLOTS, &tok)) {
      CFG.enable_savestate_slots = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_INGAME_BUTTONS_SAVE_STATE, &tok)) {
      CFG.ingame_buttons_savestate = cfg_buttons_string2bits(tok.stringvalue);
    }
    if(yaml_get_itemvalue(CFG_INGAME_BUTTONS_LOAD_STATE, &tok)) {
      CFG.ingame_buttons_loadstate = cfg_buttons_string2bits(tok.stringvalue);
    }
    if(yaml_get_itemvalue(CFG_INGAME_BUTTONS_CHANGE_STATE, &tok)) {
      CFG.ingame_buttons_changestate = cfg_buttons_string2bits(tok.stringvalue);
    }
    if(yaml_get_itemvalue(CFG_SGB_ENABLE_INGAME_HOOK, &tok)) {
      CFG.sgb_enable_ingame_hook = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SGB_ENABLE_STATE, &tok)) {
      CFG.sgb_enable_state = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SGB_VOLUME_BOOST, &tok)) {
      CFG.sgb_volume_boost = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_SGB_ENH_OVERRIDE, &tok)) {
      CFG.sgb_enh_override = tok.boolvalue ? 1 : 0;
    }
#ifdef CONFIG_MK3
    if(yaml_get_itemvalue(CFG_SGB_SPR_INCREASE, &tok)) {
      CFG.sgb_spr_increase = tok.boolvalue ? 1 : 0;
    }
#endif
    if(yaml_get_itemvalue(CFG_SGB_CLOCK_FIX, &tok)) {
      CFG.sgb_clock_fix = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SGB_BIOS_VERSION, &tok)) {
      CFG.sgb_bios_version = tok.longvalue;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_AUTOSAVE, &tok)) {
      CFG.enable_autosave = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_AUTOSAVE_MSU1, &tok)) {
      CFG.enable_autosave_msu1 = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SHOW_COVERS, &tok)) {
      if(tok.type == YAML_BOOL) {
        CFG.show_covers = tok.boolvalue ? 1 : 0;   /* legacy true -> large, false -> off */
      } else {
        CFG.show_covers = tok.longvalue > 2 ? 1 : (uint8_t)tok.longvalue;  /* 0: off, 1: large, 2: small */
      }
    }
    if(yaml_get_itemvalue(CFG_COVERS_IN_LISTS, &tok)) {
      CFG.covers_in_lists = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_LANGUAGE, &tok)) {
      CFG.language = tok.longvalue;
      if(CFG.language > 3) CFG.language = 0;
    }
    if(yaml_get_itemvalue(CFG_PATCH_VERIFY_INTEGRITY, &tok)) {
      CFG.patch_verify_integrity = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_MENU_MUSIC, &tok)) {
      CFG.enable_menu_music = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_MENU_SFX, &tok)) {
      CFG.enable_menu_sfx = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_WIFI, &tok)) {
      CFG.enable_wifi = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SORT_FAVORITES, &tok)) {
      CFG.sort_favorites = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_ENABLE_CHEAT_OVERLAY, &tok)) {
      CFG.enable_cheat_overlay = tok.boolvalue ? 1 : 0;
    }
    if(yaml_get_itemvalue(CFG_SKIN_NAME, &tok)) {
      strncpy((char*)CFG.skin_name, tok.stringvalue, sizeof(CFG.skin_name) - 1);
      CFG.skin_name[sizeof(CFG.skin_name) - 1] = 0;
    }
    if(yaml_get_itemvalue(CFG_MENU_MUSIC_FILE, &tok)) {
      strncpy((char*)CFG.bgm_name, tok.stringvalue, sizeof(CFG.bgm_name) - 1);
      CFG.bgm_name[sizeof(CFG.bgm_name) - 1] = 0;
    }
    if(yaml_get_itemvalue(CFG_SHOW_GAME_INFO, &tok)) {
      CFG.show_game_info = tok.boolvalue ? 1 : 0;
    }
  }
  yaml_file_close();
  return err;
}

/* Cap for a given list file.  Favorites holds more entries than Recents; the
   shared list functions below pick the right cap by filename.  Compare by
   CONTENT (not pointer): FAVORITES_FILE expands to a string literal whose
   merging across call sites is not guaranteed. */
static int listed_game_cap(const uint8_t *listfilename) {
  return !strcmp((const char*)listfilename, (const char*)FAVORITES_FILE)
       ? MAX_FAVORITE_GAMES : MAX_RECENT_GAMES;
}

/* Favorites display-time sort permutation: favorite_sort_map[displayed position] =
   index of that entry in favorites.cfg (insertion order).  Rebuilt by every
   favorites dump (cfg_dump_listed_games_for_snes); favorite_sort_count = its length
   (0 when the toggle is off, so the mapping is identity).  The file is never
   reordered, so turning the toggle off restores the original insertion order. */
static uint8_t favorite_sort_map[MAX_FAVORITE_GAMES];
static uint8_t favorite_sort_count = 0;

uint8_t listed_game_resolve_index(const uint8_t *listfile, uint8_t menu_idx) {
  if(CFG.sort_favorites
     && !strcmp((const char*)listfile, (const char*)FAVORITES_FILE)
     && menu_idx < favorite_sort_count) {
    return favorite_sort_map[menu_idx];
  }
  return menu_idx;
}

/* The on-screen display name of a list entry, written to out (>= 256 bytes).
   Mirrors what cfg_dump_listed_games_for_snes shows: a patch-aware
   "<rom>\t<patch>" entry displays the patch basename without extension; a plain
   entry displays the ROM basename.  Used both for the SNES dump and to derive
   the sort key, so the two never diverge. */
static void listed_game_display_key(const TCHAR *entry, TCHAR *out) {
  TCHAR tmp[256];
  strncpy(tmp, entry, 255);
  tmp[255] = 0;
  char *tab = strchr(tmp, '\t');
  char *disp;
  if(tab) {
    disp = tab + 1;
    char *dot = strrchr(disp, '.');
    if(dot) *dot = 0;
  } else {
    char *slash = strrchr(tmp, '/');
    disp = slash ? slash + 1 : tmp;
  }
  strncpy(out, disp, 255);
  out[255] = 0;
}

/* The recent/favorite list functions below are STREAMING: they process one
   entry at a time through a single 256-byte buffer and rewrite the list into a
   temp file (a 2nd FIL) that atomically replaces it.  They must NEVER buffer the
   whole list into a TCHAR[cap][256] array: at cap=20 that is a 5 KB stack frame,
   and the mk2/mk3-LPC1756 MCUs only have ~4 KB of stack (16 KB SRAM minus
   globals) -- the frame overran into .bss and corrupted the lists (phantom
   "0x0B" entries).  Keep these allocation-light.  As a side benefit, entries that
   are not '/'-rooted (empty/garbage) are dropped on any rewrite, self-healing a
   list that an older firmware corrupted. */

/* Build "<listfilename>.tmp" into out (out must be >= strlen(list)+5). */
static void listed_game_tmp_path(const uint8_t *listfilename, TCHAR *out, size_t outsz) {
  strncpy(out, (const char*)listfilename, outsz - 5);
  out[outsz - 5] = 0;
  strncat(out, ".tmp", 5);
}

/* Finish a stream-rewrite: close the temp `dst`, then on success swap it in for
   `listfilename` (drop original, rename temp -> original).  On a write/close
   error the original is left untouched and the temp discarded.  Returns 0 / <0. */
static int listed_game_commit(const uint8_t *listfilename, const TCHAR *tmppath, FIL *dst) {
  if(f_close(dst) != FR_OK) {
    f_unlink(tmppath);
    return -1;
  }
  f_unlink((const TCHAR*)listfilename);
  return f_rename(tmppath, (const TCHAR*)listfilename) == FR_OK ? 0 : -1;
}

int cfg_validity_check_listed_games(const uint8_t *listfilename) {
  int cap = listed_game_cap(listfilename);
  int seen = 0, written = 0, bad = 0;
  TCHAR entry[256];
  TCHAR base[256];
  TCHAR tmppath[80];
  FIL dst;
  char *tab;

  /* Pass 1 (read-only): is a rewrite needed?  Triggered by any garbage entry
     (not '/'-rooted) or any entry whose base ROM no longer exists.  Patch-aware
     entries are "<rom>\t<patch>": validate the base ROM ONLY (a temporarily-
     missing patch must not evict the entry), stripping a COPY at the tab. */
  file_open(listfilename, FA_READ);
  if(file_status == FILE_ERR) {
    return 0;
  }
  while(seen < cap) {
    f_gets(entry, 255, &file_handle);
    if(*entry == 0) break;
    seen++;
    if(*entry != '/') { bad = 1; break; }
    strncpy(base, entry, 255);
    base[255] = 0;
    tab = strchr(base, '\t');
    if(tab) *tab = 0;
    if(f_stat((const TCHAR*)base, NULL) != FR_OK) { bad = 1; break; }
  }
  file_close();
  if(!bad) {
    return 0;
  }

  /* Pass 2: stream the surviving entries into a temp file, then swap it in. */
  listed_game_tmp_path(listfilename, tmppath, sizeof(tmppath));
  if(f_open(&dst, tmppath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    return 0;
  }
  file_open(listfilename, FA_READ);
  if(file_status != FILE_ERR) {
    while(written < cap) {
      f_gets(entry, 255, &file_handle);
      if(*entry == 0) break;
      if(*entry != '/') continue;
      strncpy(base, entry, 255);
      base[255] = 0;
      tab = strchr(base, '\t');
      if(tab) *tab = 0;
      if(f_stat((const TCHAR*)base, NULL) != FR_OK) continue;
      f_puts(entry, &dst);
      f_putc(0, &dst);
      written++;
    }
  }
  file_close();
  return listed_game_commit(listfilename, tmppath, &dst);
}

int cfg_add_listed_game_patched(const uint8_t *listfilename, uint8_t *fn,
                                const char *patch_basename, bool evict_oldest) {
  int cap = listed_game_cap(listfilename);
  int count = 0, found = 0, written = 0;
  TCHAR fqfn[256];
  TCHAR entry[256];
  TCHAR tmppath[80];
  FIL dst;
  fqfn[0] = 0;
  if(fn[0] !=  '/') {
    strncpy(fqfn, (const char*)file_path, 256);
    fqfn[255] = 0;
  }
  strncat(fqfn, (const char*)fn, 256 - strlen(fqfn) - 1);
  /* Patch-aware: append "\t<patch_basename>" when it fits the 255-char entry cap
     (graceful degrade to base-only otherwise; bounded strncat, never snprintf —
     keeps -Werror=format-truncation happy).  The dedup/write below operate on the
     whole fqfn, so a patched entry stays distinct from the plain ROM. */
  if(patch_basename && patch_basename[0]) {
    size_t cur = strlen(fqfn);
    if(cur + 1 + strlen(patch_basename) < 255) {
      strncat(fqfn, "\t", 256 - cur - 1);
      strncat(fqfn, patch_basename, 256 - strlen(fqfn) - 1);
    }
  }
  /* Pass 1: count valid entries + detect an existing copy (dedup), streaming one
     entry at a time -- see the note above on why we never buffer the whole list.
     A missing list file (FILE_ERR) is an empty list: count stays 0.  Garbage
     entries (not '/'-rooted) are ignored so a corrupted list self-heals. */
  file_open(listfilename, FA_READ);
  if(file_status != FILE_ERR) {
    while(count < cap) {
      f_gets(entry, 255, &file_handle);
      if(*entry == 0) break;
      if(*entry != '/') continue;
      if(!strncasecmp((TCHAR*)fqfn, entry, 255)) found = 1;
      count++;
    }
  }
  file_close();

  if(!evict_oldest && count > (cap - 1) + found) {
    /* List is full and game is not already in list, refuse to add it. */
    return 1;
  }

  /* Pass 2: stream-rewrite -- new entry on top, then the old entries copied
     through one buffer (dropping the moved/duplicate copy and any garbage,
     capped at `cap`) into a temp file that atomically replaces the list. */
  listed_game_tmp_path(listfilename, tmppath, sizeof(tmppath));
  if(f_open(&dst, tmppath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
  f_puts((const TCHAR*)fqfn, &dst);
  f_putc(0, &dst);
  written = 1;
  file_open(listfilename, FA_READ);
  if(file_status != FILE_ERR) {
    while(written < cap) {
      f_gets(entry, 255, &file_handle);
      if(*entry == 0) break;
      if(*entry != '/') continue;                          /* skip garbage */
      if(!strncasecmp((TCHAR*)fqfn, entry, 255)) continue; /* dedup / move-to-top */
      f_puts(entry, &dst);
      f_putc(0, &dst);
      written++;
    }
  }
  file_close();
  /* Contract: 1 == list full (refused above); 0 == added OK; <0 == write error. */
  return listed_game_commit(listfilename, tmppath, &dst);
}

int cfg_add_listed_game(const uint8_t *listfilename, uint8_t *fn, bool evict_oldest) {
  return cfg_add_listed_game_patched(listfilename, fn, NULL, evict_oldest);
}

int cfg_remove_listed_game(const uint8_t *listfilename, uint8_t index_to_remove) {
  int cap = listed_game_cap(listfilename);
  int index = 0, written = 0;
  TCHAR entry[256];
  TCHAR tmppath[80];
  FIL dst;

  /* Stream all valid entries except the one at file index `index_to_remove`
     (the caller already mapped any displayed/sorted position to the file index
     via listed_game_resolve_index) into a temp file, then swap it in.  Garbage
     entries are dropped (self-heal). */
  listed_game_tmp_path(listfilename, tmppath, sizeof(tmppath));
  if(f_open(&dst, tmppath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
  file_open(listfilename, FA_READ);
  if(file_status != FILE_ERR) {
    while(written < cap) {
      f_gets(entry, 255, &file_handle);
      if(*entry == 0) break;
      if(*entry != '/') continue;                 /* skip garbage */
      if(index++ == index_to_remove) continue;    /* drop the entry being removed */
      f_puts(entry, &dst);
      f_putc(0, &dst);
      written++;
    }
  }
  file_close();
  return listed_game_commit(listfilename, tmppath, &dst);
}

int cfg_get_listed_game_raw(const uint8_t *listfilename, uint8_t *fn, uint8_t index) {
  int err = 0;
  fn[0] = 0;
  file_open(listfilename, FA_READ);
  do {
    f_gets((TCHAR*)fn, 255, &file_handle);
    if(fn[0] == 0) break;   /* stop at EOF: an out-of-range index must not loop up to 256 reads */
  } while (index--);
  file_close();
  return err;
}

int cfg_get_listed_game(const uint8_t *listfilename, uint8_t *fn, uint8_t index) {
  int err = cfg_get_listed_game_raw(listfilename, fn, index);
  /* List entries may carry a "<rom>\t<patch>" tag (patch-aware Recents/
     Favorites).  Callers that consume the entry as a plain ROM path get just
     the base ROM here; patch-aware callers use cfg_get_listed_game_raw +
     cfg_parse_patch_entry to recover the patch. */
  char *tab = strchr((char*)fn, '\t');
  if(tab) *tab = 0;
  return err;
}

/* Split a raw list entry of the form "<rom_path>\t<patch_basename>" in place.
   Truncates `entry` at the tab so it becomes the bare base ROM path, and builds
   the patch's full SD path into `patchpath` (= the ROM's directory + the stored
   patch basename; patches always live alongside their ROM, see ips_find_patches).
   Returns 1 when a patch tag was present, 0 otherwise (entry left untouched). */
int cfg_parse_patch_entry(char *entry, char *patchpath, int size) {
  char *tab = strchr(entry, '\t');
  if(!tab) {
    if(size) patchpath[0] = 0;
    return 0;
  }
  *tab = 0;                            /* entry -> base ROM path */
  const char *patch_basename = tab + 1;
  int n = 0;
  char *slash = strrchr(entry, '/');
  if(slash) {
    int dirlen = (int)(slash - entry) + 1;   /* keep the trailing '/' */
    for(int i = 0; i < dirlen && n < size - 1; i++) patchpath[n++] = entry[i];
  } else if(n < size - 1) {
    patchpath[n++] = '/';
  }
  for(const char *p = patch_basename; *p && n < size - 1; p++) patchpath[n++] = *p;
  patchpath[n] = 0;
  return 1;
}

/**
 * @brief Reads file names from a list file and makes them accessible inside
 *        SNES address space.
 *
 * @param listfilename The file to read the list entries from.
 * @param address The address in SNES address space where the list entries are
 *                to be made accessible.
 * @return uint8_t Number of list entries.
 */
/* Max display-key length buffered when sorting favorites (kept small: up to 20
   keys live on the stack at once -- buffering the full [cap][256] paths was a
   5 KB frame that overran the 16 KB MCUs' stack). Names longer than this only
   truncate in the sorted view; the menu shows far fewer columns anyway. */
#define LISTED_DISP_KEYLEN 64
uint8_t cfg_dump_listed_games_for_snes(const uint8_t *listfilename, uint32_t address, uint8_t write_lastdir) {
  TCHAR fntmp[256];
  TCHAR dirtmp[256];
  int index;
  /* LAST_GAME_DIR (used by reset_to_menu Folder/ROM navigation) belongs ONLY
     to the recent-games list. The favorites dump shares this function but must
     NOT touch LAST_GAME_DIR — otherwise it clobbers the recent game's folder
     with the favorite index-0 folder (e.g. a root favorite resets it to "/"),
     and the menu never returns to a sub-folder game. */
  if(write_lastdir) {
    sram_writebyte(0, SRAM_LASTGAME_DIR_ADDR);  /* default: empty dir path */
    sram_writebyte(0, SRAM_LASTGAME_FILE_ADDR); /* default: empty pre-select name */
  }
  int cap = listed_game_cap(listfilename);
  /* Favorites alphabetical sort is DISPLAY-ONLY: the file keeps its insertion order
     (so turning the toggle off restores it).  Sort an index permutation, dump the
     names in that order, and record favorite_sort_map so the by-index ops
     (cover/play/remove/delete/cheats/autoboot) resolve the entry the user sees via
     listed_game_resolve_index.  Recents are never sorted. */
  if(!strcmp((const char*)listfilename, (const char*)FAVORITES_FILE)) {
    if(CFG.sort_favorites) {
      /* Sort favorites for DISPLAY by their display key.  Buffer only the short
         keys (never the full [cap][256] paths -- that 5 KB frame overran the
         stack), reusing fntmp/dirtmp as the read / display-key scratch.  Garbage
         entries are skipped.  favorite_sort_map maps displayed pos -> file index. */
      TCHAR keys[MAX_LISTED_GAMES][LISTED_DISP_KEYLEN];
      uint8_t order[MAX_LISTED_GAMES];
      int count = 0, i, j;
      file_open(listfilename, FA_READ);
      if(file_status != FILE_ERR) {
        while(count < cap) {
          f_gets(fntmp, 255, &file_handle);
          if(*fntmp == 0) break;
          if(*fntmp != '/') continue;
          listed_game_display_key(fntmp, dirtmp);
          strncpy(keys[count], dirtmp, LISTED_DISP_KEYLEN - 1);
          keys[count][LISTED_DISP_KEYLEN - 1] = 0;
          count++;
        }
      }
      file_close();
      for(i = 0; i < count; i++) order[i] = (uint8_t) i;
      /* insertion sort the permutation by display key (case-insensitive) */
      for(i = 1; i < count; i++) {
        uint8_t curi = order[i];
        for(j = i - 1; j >= 0; j--) {
          if(strncasecmp(keys[order[j]], keys[curi], LISTED_DISP_KEYLEN) <= 0) {
            break;
          }
          order[j + 1] = order[j];
        }
        order[j + 1] = curi;
      }
      for(i = 0; i < count; i++) {
        sram_writestrn((uint8_t*)keys[order[i]], address + 256 * i, 256);
        favorite_sort_map[i] = order[i];
      }
      favorite_sort_count = (uint8_t) count;
      return (uint8_t) count;
    }
    favorite_sort_count = 0; /* toggle off -> identity (display == file order) */
  }
  file_open(listfilename, FA_READ);
  index = 0;
  while(file_status != FILE_ERR && index < cap) {
    f_gets(fntmp, 255, &file_handle);
    if(*fntmp == 0) break;        /* end of list */
    if(*fntmp != '/') continue;   /* skip empty/garbage entries (self-heal) */
    /* Patch-aware entries are "<rom>\t<patch_basename>": display the patch name
       (without the .ips/.bps extension); plain entries display the ROM basename.
       listed_game_display_key works on a COPY, so fntmp stays intact for the
       LAST_GAME_DIR block below (which scans the base part for the last '/'). */
    TCHAR disp[256];
    listed_game_display_key(fntmp, disp);
    sram_writestrn((uint8_t*)disp, address+256*index, 256);
    if(write_lastdir && index == 0) {
      /* write directory + base ROM basename of the most recent game for
         reset_to_menu >= 2 (Folder/Rom) navigation. For a patch-aware
         "<rom>\t<patch>" entry the navigation must target the BASE ROM (the
         part before the tab), not the patch display name written above — so
         temporarily terminate fntmp at the tab while scanning. */
      char *tab = strchr((char*)fntmp, '\t');
      char *base_end = tab ? tab : (fntmp + strlen((const char*)fntmp));
      char base_saved = *base_end;
      *base_end = '\0';
      char *slash = strrchr((const char*)fntmp, '/');
      if(slash != NULL) {
        size_t dir_len = slash - fntmp;
        if(dir_len == 0) {
          sram_writestrn((uint8_t*)"/", SRAM_LASTGAME_DIR_ADDR, 256);
        } else {
          strncpy(dirtmp, fntmp, dir_len);
          dirtmp[dir_len] = '\0';
          sram_writestrn((uint8_t*)dirtmp, SRAM_LASTGAME_DIR_ADDR, 256);
        }
        /* base ROM basename → reset_to_menu==3 pre-select target */
        sram_writestrn((uint8_t*)(slash + 1), SRAM_LASTGAME_FILE_ADDR, 256);
      } else {
        /* bare filename with no directory: folder nav bails (dir empty), but
           still record the name for completeness */
        sram_writestrn((uint8_t*)fntmp, SRAM_LASTGAME_FILE_ADDR, 256);
      }
      *base_end = base_saved;
    }
    index++;
  }
  file_close();
  return (uint8_t) index;
}

/* ---- Autoboot ROM functions ---- */

uint8_t cfg_is_autoboot_enabled() {
  uint8_t fn[4];
  fn[0] = 0;
  file_open(AUTOBOOT_FILE, FA_READ);
  if(file_status != FILE_OK) {
    if(file_res == FR_NO_FILE || file_res == FR_NO_PATH) {
      file_res = FR_OK;
    }
    return 0;
  }
  f_gets((TCHAR*)fn, sizeof(fn), &file_handle);
  file_close();
  return fn[0] != 0;
}

int cfg_get_autoboot_rom(uint8_t *fn) {
  fn[0] = 0;
  file_open(AUTOBOOT_FILE, FA_READ);
  if(file_status != FILE_OK) {
    if(file_res == FR_NO_FILE || file_res == FR_NO_PATH) {
      file_res = FR_OK;
    }
    return 1;
  }
  f_gets((TCHAR*)fn, 255, &file_handle);
  file_close();
  return (fn[0] == 0) ? 1 : 0;
}

int cfg_set_autoboot_rom(const uint8_t *fn) {
  int err = 0;
  TCHAR fqfn[256];
  fqfn[0] = 0;
  if(fn[0] != '/') {
    strncpy(fqfn, (const char*)file_path, 256);
    fqfn[255] = 0;
  }
  strncat(fqfn, (const char*)fn, 256 - strlen(fqfn) - 1);
  file_open(AUTOBOOT_FILE, FA_CREATE_ALWAYS | FA_WRITE);
  err = f_puts((const TCHAR*)fqfn, &file_handle);
  err |= (f_putc(0, &file_handle) == EOF) ? 1 : 0;
  file_close();
  return err;
}

int cfg_clr_autoboot_rom() {
  f_unlink((TCHAR*)AUTOBOOT_FILE);
  if(file_res == FR_NO_FILE || file_res == FR_NO_PATH) {
    file_res = FR_OK;
  }
  return 0;
}

/* make binary config available to menu */
void cfg_load_to_menu() {
  sram_writeblock(&CFG, SRAM_MENU_CFG_ADDR, sizeof(cfg_t));
}

/* dump binary config from menu */
void cfg_get_from_menu() {
  sram_readblock(&CFG, SRAM_MENU_CFG_ADDR, sizeof(cfg_t));
}

void cfg_set_pair_mode_allowed(uint8_t allowed) {
  CFG.pair_mode_allowed = allowed;
}
uint8_t cfg_is_pair_mode_allowed() {
  return CFG.pair_mode_allowed;
}

void cfg_set_r213f_override(uint8_t enable) {
  CFG.r213f_override = enable;
}
uint8_t cfg_is_r213f_override_enabled() {
  return CFG.r213f_override;
}

void cfg_set_onechip_transient_fixes(uint8_t enable) {
  CFG.onechip_transient_fixes = enable;
}
uint8_t cfg_is_onechip_transient_fixes() {
  return CFG.onechip_transient_fixes;
}

void cfg_set_brightness_limit(uint8_t limit) {
  CFG.brightness_limit = limit;
}

uint8_t cfg_get_brightness_limit() {
  return CFG.brightness_limit;
}

void cfg_set_reset_to_menu(uint8_t enable) {
  CFG.reset_to_menu = enable;
}
uint8_t cfg_is_reset_to_menu() {
  return CFG.reset_to_menu;
}

void cfg_set_vidmode_game(cfg_vidmode_t vidmode) {
  CFG.vidmode_game = vidmode;
}

cfg_vidmode_t cfg_get_vidmode_game() {
  return CFG.vidmode_game;
}

void cfg_set_vidmode_menu(cfg_vidmode_t vidmode) {
  CFG.vidmode_menu = vidmode;
}

cfg_vidmode_t cfg_get_vidmode_menu() {
  return CFG.vidmode_menu;
}

/* convert a controller input bit field (16 bits) to config string
   target string *out must have enough space for 12 characters */
void cfg_buttons_bits2string(uint16_t bits, char *out) {
  int j = 0;
//  printf("converted button bits %04X ", bits);
  for(uint8_t i=0; i < 12; i++) {
    if(bits & 0x8000) {
      out[j++] = button_names[i];
    }
    bits <<= 1;
  }
  out[j] = 0;
//  printf(" to string: %s\n", out);
}

/* convert a config buttons string to controller input bit field (16 bits) */
uint16_t cfg_buttons_string2bits(char *str) {
  uint16_t input = 0;
  for(uint8_t x=0; x < SNES_NUM_BUTTONS && str[x]; x++){
    char *p = strchr(button_names, str[x]);
    if(p) input |= 1 << (0xF - (p - button_names));   /* ignore chars not in button_names */
  }
//  printf("converted button string %s to bits: %04X\n", str, input);
  return input;
}

uint8_t cfg_is_msu1_autosave_enabled() {
  return CFG.enable_autosave && CFG.enable_autosave_msu1;
}

int cfg_get_stringvalue(const char *key, char *target, size_t count) {
  yaml_token_t tok;
  int found = 0;
  yaml_file_open(CFG_FILE, FA_READ);
  found = yaml_get_itemvalue(key, &tok);
  if(found) {
    strncpy(target, tok.stringvalue, count);
    if(count) target[count-1] = 0;   /* strncpy may not NUL-terminate */
  } else if(count) {
    target[0] = 0;
  }
  yaml_file_close();
  return found;
}
