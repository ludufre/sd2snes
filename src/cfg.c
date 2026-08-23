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
#include "util.h"

/* The SNES menu pokes config bytes by hard-coded offset (snes/memmap.i65,
   CFG_*=CFG_ADDR+$nn).  Pinning the C layout to that map is what stops an
   inserted or resized field from silently desyncing menu and firmware: without
   it the menu keeps writing the old offsets, which is a wrong value at runtime,
   not a build failure.  EVERY CFG_* in memmap.i65 has a line here and every
   field has a CFG_*; add a field, add a line. */
_Static_assert(offsetof(cfg_t, vidmode_menu) == 0x00, "cfg_t.vidmode_menu must stay at CFG_ADDR+$00");
_Static_assert(offsetof(cfg_t, vidmode_game) == 0x01, "cfg_t.vidmode_game must stay at CFG_ADDR+$01");
_Static_assert(offsetof(cfg_t, pair_mode_allowed) == 0x02, "cfg_t.pair_mode_allowed must stay at CFG_ADDR+$02");
_Static_assert(offsetof(cfg_t, bsx_use_usertime) == 0x03, "cfg_t.bsx_use_usertime must stay at CFG_ADDR+$03");
_Static_assert(offsetof(cfg_t, bsx_time) == 0x04, "cfg_t.bsx_time must stay at CFG_ADDR+$04");
_Static_assert(offsetof(cfg_t, r213f_override) == 0x10, "cfg_t.r213f_override must stay at CFG_ADDR+$10");
_Static_assert(offsetof(cfg_t, enable_ingame_hook) == 0x11, "cfg_t.enable_ingame_hook must stay at CFG_ADDR+$11");
_Static_assert(offsetof(cfg_t, enable_ingame_buttons) == 0x12, "cfg_t.enable_ingame_buttons must stay at CFG_ADDR+$12");
_Static_assert(offsetof(cfg_t, enable_hook_holdoff) == 0x13, "cfg_t.enable_hook_holdoff must stay at CFG_ADDR+$13");
_Static_assert(offsetof(cfg_t, enable_screensaver) == 0x14, "cfg_t.enable_screensaver must stay at CFG_ADDR+$14");
_Static_assert(offsetof(cfg_t, screensaver_timeout) == 0x15, "cfg_t.screensaver_timeout must stay at CFG_ADDR+$15");
_Static_assert(offsetof(cfg_t, sort_directories) == 0x17, "cfg_t.sort_directories must stay at CFG_ADDR+$17");
_Static_assert(offsetof(cfg_t, hide_extensions) == 0x18, "cfg_t.hide_extensions must stay at CFG_ADDR+$18");
_Static_assert(offsetof(cfg_t, cx4_speed) == 0x19, "cfg_t.cx4_speed must stay at CFG_ADDR+$19");
_Static_assert(offsetof(cfg_t, skin_name) == 0x1A, "cfg_t.skin_name must stay at CFG_ADDR+$1A");
_Static_assert(offsetof(cfg_t, control_type) == 0x9A, "cfg_t.control_type must stay at CFG_ADDR+$9A");
_Static_assert(offsetof(cfg_t, msu_volume_boost) == 0x9B, "cfg_t.msu_volume_boost must stay at CFG_ADDR+$9B");
_Static_assert(offsetof(cfg_t, onechip_transient_fixes) == 0x9C, "cfg_t.onechip_transient_fixes must stay at CFG_ADDR+$9C");
_Static_assert(offsetof(cfg_t, brightness_limit) == 0x9D, "cfg_t.brightness_limit must stay at CFG_ADDR+$9D");
_Static_assert(offsetof(cfg_t, gsu_speed) == 0x9E, "cfg_t.gsu_speed must stay at CFG_ADDR+$9E");
_Static_assert(offsetof(cfg_t, reset_to_menu) == 0x9F, "cfg_t.reset_to_menu must stay at CFG_ADDR+$9F");
_Static_assert(offsetof(cfg_t, led_brightness) == 0xA0, "cfg_t.led_brightness must stay at CFG_ADDR+$A0");
_Static_assert(offsetof(cfg_t, enable_cheats) == 0xA1, "cfg_t.enable_cheats must stay at CFG_ADDR+$A1");
_Static_assert(offsetof(cfg_t, reset_patch) == 0xA2, "cfg_t.reset_patch must stay at CFG_ADDR+$A2");
_Static_assert(offsetof(cfg_t, enable_ingame_savestate) == 0xA3, "cfg_t.enable_ingame_savestate must stay at CFG_ADDR+$A3");
_Static_assert(offsetof(cfg_t, loadstate_delay) == 0xA4, "cfg_t.loadstate_delay must stay at CFG_ADDR+$A4");
_Static_assert(offsetof(cfg_t, enable_savestate_slots) == 0xA5, "cfg_t.enable_savestate_slots must stay at CFG_ADDR+$A5");
_Static_assert(offsetof(cfg_t, ingame_buttons_savestate) == 0xA6, "cfg_t.ingame_buttons_savestate must stay at CFG_ADDR+$A6");
_Static_assert(offsetof(cfg_t, ingame_buttons_loadstate) == 0xA8, "cfg_t.ingame_buttons_loadstate must stay at CFG_ADDR+$A8");
_Static_assert(offsetof(cfg_t, ingame_buttons_changestate) == 0xAA, "cfg_t.ingame_buttons_changestate must stay at CFG_ADDR+$AA");
_Static_assert(offsetof(cfg_t, sgb_enable_ingame_hook) == 0xAC, "cfg_t.sgb_enable_ingame_hook must stay at CFG_ADDR+$AC");
_Static_assert(offsetof(cfg_t, sgb_enable_state) == 0xAD, "cfg_t.sgb_enable_state must stay at CFG_ADDR+$AD");
_Static_assert(offsetof(cfg_t, sgb_volume_boost) == 0xAE, "cfg_t.sgb_volume_boost must stay at CFG_ADDR+$AE");
_Static_assert(offsetof(cfg_t, sgb_enh_override) == 0xAF, "cfg_t.sgb_enh_override must stay at CFG_ADDR+$AF");
_Static_assert(offsetof(cfg_t, sgb_spr_increase) == 0xB0, "cfg_t.sgb_spr_increase must stay at CFG_ADDR+$B0");
_Static_assert(offsetof(cfg_t, sgb_clock_fix) == 0xB1, "cfg_t.sgb_clock_fix must stay at CFG_ADDR+$B1");
_Static_assert(offsetof(cfg_t, sgb_bios_version) == 0xB2, "cfg_t.sgb_bios_version must stay at CFG_ADDR+$B2");
_Static_assert(offsetof(cfg_t, show_tribute) == 0xB3, "cfg_t.show_tribute must stay at CFG_ADDR+$B3");
_Static_assert(offsetof(cfg_t, enable_autosave) == 0xB4, "cfg_t.enable_autosave must stay at CFG_ADDR+$B4");
_Static_assert(offsetof(cfg_t, enable_autosave_msu1) == 0xB5, "cfg_t.enable_autosave_msu1 must stay at CFG_ADDR+$B5");
_Static_assert(offsetof(cfg_t, show_covers) == 0xB6, "cfg_t.show_covers must stay at CFG_ADDR+$B6");
_Static_assert(offsetof(cfg_t, language) == 0xB7, "cfg_t.language must stay at CFG_ADDR+$B7");
_Static_assert(offsetof(cfg_t, patch_verify_integrity) == 0xB8, "cfg_t.patch_verify_integrity must stay at CFG_ADDR+$B8");
_Static_assert(offsetof(cfg_t, enable_menu_music) == 0xB9, "cfg_t.enable_menu_music must stay at CFG_ADDR+$B9");
_Static_assert(offsetof(cfg_t, covers_in_lists) == 0xBA, "cfg_t.covers_in_lists must stay at CFG_ADDR+$BA");
_Static_assert(offsetof(cfg_t, enable_menu_sfx) == 0xBB, "cfg_t.enable_menu_sfx must stay at CFG_ADDR+$BB");
_Static_assert(offsetof(cfg_t, bgm_name) == 0xBC, "cfg_t.bgm_name must stay at CFG_ADDR+$BC");
_Static_assert(offsetof(cfg_t, sort_favorites) == 0x13C, "cfg_t.sort_favorites must stay at CFG_ADDR+$13C");
_Static_assert(offsetof(cfg_t, enable_cheat_overlay) == 0x13D, "cfg_t.enable_cheat_overlay must stay at CFG_ADDR+$13D");
_Static_assert(offsetof(cfg_t, show_game_info) == 0x13E, "cfg_t.show_game_info must stay at CFG_ADDR+$13E");
_Static_assert(offsetof(cfg_t, enable_wifi) == 0x13F, "cfg_t.enable_wifi must stay at CFG_ADDR+$13F (RESERVED for the Companion port; NOT $BD which overlapped bgm_name @ $BC)");
_Static_assert(offsetof(cfg_t, game_info_video) == 0x140, "cfg_t.game_info_video must stay at CFG_ADDR+$140");
_Static_assert(offsetof(cfg_t, game_info_music) == 0x141, "cfg_t.game_info_music must stay at CFG_ADDR+$141");
_Static_assert(offsetof(cfg_t, enable_bps_copier) == 0x142, "cfg_t.enable_bps_copier must stay at CFG_ADDR+$142");
_Static_assert(offsetof(cfg_t, clear_ppu_on_boot) == 0x143, "cfg_t.clear_ppu_on_boot must stay at CFG_ADDR+$143");
_Static_assert(offsetof(cfg_t, bus_compat) == 0x144, "cfg_t.bus_compat must stay at CFG_ADDR+$144");
_Static_assert(offsetof(cfg_t, enable_game_manual) == 0x145, "cfg_t.enable_game_manual must stay at CFG_ADDR+$145");
_Static_assert(offsetof(cfg_t, enable_sram_slots) == 0x146, "cfg_t.enable_sram_slots must stay at CFG_ADDR+$146");
_Static_assert(offsetof(cfg_t, ingame_buttons_menu) == 0x147, "cfg_t.ingame_buttons_menu must stay at CFG_ADDR+$147");
_Static_assert(offsetof(cfg_t, a26_video_width) == 0x149, "cfg_t.a26_video_width must stay at CFG_ADDR+$149");

const cfg_t CFG_DEFAULT = {
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
  .sort_favorites = 1,
  .enable_cheat_overlay = 1,
  .show_game_info = 1,
  .enable_wifi = 0,
  .game_info_video = 1,
  .game_info_music = 1,
  .enable_bps_copier = 1,
  .clear_ppu_on_boot = 0,
  .bus_compat = 0,
  .enable_game_manual = 1,
  .enable_sram_slots = 1,
  .ingame_buttons_menu = SNES_BUTTON_L | SNES_BUTTON_R | SNES_BUTTON_Y | SNES_BUTTON_LEFT,
  .a26_video_width = 0
};

cfg_t CFG;
extern mcu_status_t STM;

static const char button_names[] = "BYsSudlrAXLR";

/* Gestures the FPGA decodes by exact equality of the pad word (see any core's cheat.v). */
static const uint16_t cfg_reserved_gestures[] = {
  SNES_BUTTON_LRET, SNES_BUTTON_LREX, SNES_BUTTON_LRSA,
  SNES_BUTTON_LRSB, SNES_BUTTON_LRSY, SNES_BUTTON_LRSX
};

static uint8_t cfg_bitcount16(uint16_t v) {
  uint8_t n = 0;
  while(v) { n += v & 1; v >>= 1; }
  return n;
}

/* A combo that is a subset of a reserved gesture opens the menu a frame before that gesture
   completes, leaving the reset to land on an already frozen game. */
static uint16_t cfg_check_menu_combo(uint16_t combo) {
  const char *why = NULL;
  if(!combo) {
    why = "empty";
  } else if(cfg_bitcount16(combo) < CFG_MENU_COMBO_MIN_BUTTONS) {
    why = "too few buttons";
  } else {
    for(unsigned i = 0; i < sizeof(cfg_reserved_gestures)/sizeof(cfg_reserved_gestures[0]); i++) {
      if((cfg_reserved_gestures[i] & combo) == combo) {
        why = "subset of a reserved gesture";
        break;
      }
    }
  }
  if(why) {
    printf("cfg: menu combo %04X rejected (%s), using %04X\n",
           combo, why, CFG_DEFAULT.ingame_buttons_menu);
    return CFG_DEFAULT.ingame_buttons_menu;
  }
  if((CFG.ingame_buttons_savestate   & combo) == combo
  || (CFG.ingame_buttons_loadstate   & combo) == combo
  || (CFG.ingame_buttons_changestate & combo) == combo) {
    printf("cfg: menu combo %04X shadows a savestate combo (menu probe runs first)\n", combo);
  }
  return combo;
}

/* ---- config.yml serializer ----------------------------------------------
   config.yml is a flat list of "Key: value" lines; per setting only the byte's
   place in cfg_t and the spelling of its scalar vary, so one table drives both
   directions.  cfg_save walks it in order -- the table order IS the file layout,
   pinned by tests/host/run_cfg.sh -- while cfg_load looks each key up
   independently (yaml_get_itemvalue rewinds, so lookup order is free).
   The file carries no prose: the documentation belongs with the menu entry that
   sets the value, not on the card. */
typedef enum {
  CK_BOOL = 0,   /* uint8_t 0/1        <-> true / false */
  CK_NUM,        /* uint8_t            <-> decimal, optional clamp (see below) */
  CK_NIB,        /* uint8_t            <-> decimal, masked to 4 bits on load */
  CK_STR,        /* uint8_t[CFG_STR_LEN] <-> bare scalar */
  CK_BUTTONS,    /* uint16_t pad mask  <-> "BYsSudlrAXLR" letters */
  CK_BSXTIME     /* uint8_t[12] S-RTC  <-> 14 BCD hex digits */
} cfg_kind_t;

/* clamp = (max << 4) | replacement, 0 = take whatever the file says.  Every max
   and replacement in use fits in a nibble, and the replacement is not always zero:
   an out-of-range LEDBrightness means "maximum", an out-of-range ShowCovers means
   "the default, on".

   RANGE-CHECKED ON THE long, BOTH WAYS, before anything is narrowed (see cfg_load).
   The file is text and the destination is a uint8_t, so a test made after the
   narrowing has a blind spot on one side: "> max" alone lets -1 through and stores
   255, truncating first lets 256 through because it narrows to 0.  Either way the
   card ships a byte the menu cannot name.  The cfg_clamped and cfg_over golden
   fixtures pin one side each. */
typedef struct {
  const char *key;
  uint16_t    off;    /* offsetof() into cfg_t -- the menu's CFG map, indirectly */
  uint8_t     kind;
  uint8_t     clamp;
} cfg_item_t;

#define CFG_STR_LEN (sizeof(CFG_DEFAULT.skin_name))
_Static_assert(sizeof(CFG_DEFAULT.bgm_name) == CFG_STR_LEN,
               "both CK_STR fields must share one length");

/* The favorites mirror is 20 x 256 bytes and the game info block starts right after
   it: raising a cap on one side only lets the SNES overwrite the other from
   underneath (memory.h <-> snes/memmap.i65 are in lockstep). */
_Static_assert(SRAM_FAVORITEGAMES_ADDR + MAX_FAVORITE_GAMES * 256L <= SRAM_GAMEINFO_ADDR,
               "the favorites SRAM mirror runs into SRAM_GAMEINFO_ADDR");

#define CFGI(k, field, kind, clamp) { k, offsetof(cfg_t, field), kind, clamp }
static const cfg_item_t cfg_items[] = {
  CFGI(CFG_PAIR_MODE_ALLOWED,           pair_mode_allowed,          CK_BOOL,    0),
  CFGI(CFG_VIDMODE_MENU,                vidmode_menu,               CK_NUM,     0),
  CFGI(CFG_VIDMODE_GAME,                vidmode_game,               CK_NUM,     0),
  CFGI(CFG_BSX_USE_USERTIME,            bsx_use_usertime,           CK_BOOL,    0),
  CFGI(CFG_BSX_TIME,                    bsx_time,                   CK_BSXTIME, 0),
  CFGI(CFG_R213F_OVERRIDE,              r213f_override,             CK_BOOL,    0),
  CFGI(CFG_1CHIP_TRANSIENT_FIXES,       onechip_transient_fixes,    CK_BOOL,    0),
  CFGI(CFG_BRIGHTNESS_LIMIT,            brightness_limit,           CK_NIB,     0),
  CFGI(CFG_ENABLE_RST_TO_MENU,          reset_to_menu,              CK_NUM,     0x31),
  CFGI(CFG_ENABLE_CHEATS,               enable_cheats,              CK_BOOL,    0),
  CFGI(CFG_ENABLE_INGAME_HOOK,          enable_ingame_hook,         CK_BOOL,    0),
  CFGI(CFG_ENABLE_INGAME_BUTTONS,       enable_ingame_buttons,      CK_BOOL,    0),
  CFGI(CFG_ENABLE_HOOK_HOLDOFF,         enable_hook_holdoff,        CK_BOOL,    0),
  CFGI(CFG_RESET_PATCH,                 reset_patch,                CK_BOOL,    0),
  CFGI(CFG_ENABLE_INGAME_SAVESTATE,     enable_ingame_savestate,    CK_NUM,     0),
  CFGI(CFG_LOADSTATE_DELAY,             loadstate_delay,            CK_NUM,     0),
  CFGI(CFG_ENABLE_SAVESTATE_SLOTS,      enable_savestate_slots,     CK_BOOL,    0),
  CFGI(CFG_INGAME_BUTTONS_SAVE_STATE,   ingame_buttons_savestate,   CK_BUTTONS, 0),
  CFGI(CFG_INGAME_BUTTONS_LOAD_STATE,   ingame_buttons_loadstate,   CK_BUTTONS, 0),
  CFGI(CFG_INGAME_BUTTONS_CHANGE_STATE, ingame_buttons_changestate, CK_BUTTONS, 0),
  CFGI(CFG_SGB_ENABLE_INGAME_HOOK,      sgb_enable_ingame_hook,     CK_BOOL,    0),
  CFGI(CFG_SGB_ENABLE_STATE,            sgb_enable_state,           CK_BOOL,    0),
  CFGI(CFG_SGB_VOLUME_BOOST,            sgb_volume_boost,           CK_NUM,     0),
  CFGI(CFG_SGB_ENH_OVERRIDE,            sgb_enh_override,           CK_BOOL,    0),
#ifdef CONFIG_MK3
  /* The one key the mk2 and mk3 firmwares genuinely disagree about. */
  CFGI(CFG_SGB_SPR_INCREASE,            sgb_spr_increase,           CK_BOOL,    0),
#endif
  CFGI(CFG_SGB_CLOCK_FIX,               sgb_clock_fix,              CK_BOOL,    0),
  CFGI(CFG_SGB_BIOS_VERSION,            sgb_bios_version,           CK_NUM,     0),
  CFGI(CFG_ENABLE_SCREENSAVER,          enable_screensaver,         CK_BOOL,    0),
  CFGI(CFG_SORT_DIRECTORIES,            sort_directories,           CK_BOOL,    0),
  CFGI(CFG_HIDE_EXTENSIONS,             hide_extensions,            CK_BOOL,    0),
  CFGI(CFG_LED_BRIGHTNESS,              led_brightness,             CK_NUM,     0xFF),
  CFGI(CFG_CX4_SPEED,                   cx4_speed,                  CK_NUM,     0),
  CFGI(CFG_GSU_SPEED,                   gsu_speed,                  CK_NUM,     0),
  CFGI(CFG_MSU_VOLUME_BOOST,            msu_volume_boost,           CK_NUM,     0),
  CFGI(CFG_ENABLE_AUTOSAVE,             enable_autosave,            CK_BOOL,    0),
  CFGI(CFG_ENABLE_AUTOSAVE_MSU1,        enable_autosave_msu1,       CK_BOOL,    0),
  CFGI(CFG_SHOW_COVERS,                 show_covers,                CK_NUM,     0x21),
  CFGI(CFG_COVERS_IN_LISTS,             covers_in_lists,            CK_BOOL,    0),
  /* An unclamped value leaves cur_lang past the last column of every dispatch
     table in the menu. */
  CFGI(CFG_LANGUAGE,                    language,                   CK_NUM,     0x50),
  CFGI(CFG_PATCH_VERIFY_INTEGRITY,      patch_verify_integrity,     CK_BOOL,    0),
  CFGI(CFG_ENABLE_MENU_MUSIC,           enable_menu_music,          CK_BOOL,    0),
  CFGI(CFG_ENABLE_MENU_SFX,             enable_menu_sfx,            CK_BOOL,    0),
  CFGI(CFG_SORT_FAVORITES,              sort_favorites,             CK_BOOL,    0),
  CFGI(CFG_ENABLE_CHEAT_OVERLAY,        enable_cheat_overlay,       CK_BOOL,    0),
  CFGI(CFG_INGAME_BUTTONS_MENU,         ingame_buttons_menu,        CK_BUTTONS, 0),
  CFGI(CFG_ENABLE_BPS_COPIER,           enable_bps_copier,          CK_BOOL,    0),
  CFGI(CFG_CLEAR_PPU_ON_BOOT,           clear_ppu_on_boot,          CK_BOOL,    0),
  CFGI(CFG_BUS_COMPAT,                  bus_compat,                 CK_BOOL,    0),
  CFGI(CFG_ENABLE_GAME_MANUAL,          enable_game_manual,         CK_BOOL,    0),
  CFGI(CFG_A26_VIDEO_WIDTH,             a26_video_width,            CK_NUM,     0x10),
  CFGI(CFG_SKIN_NAME,                   skin_name,                  CK_STR,     0),
  CFGI(CFG_MENU_MUSIC_FILE,             bgm_name,                   CK_STR,     0),
  CFGI(CFG_SHOW_GAME_INFO,              show_game_info,             CK_NUM,     0x21),
  CFGI(CFG_GAME_INFO_VIDEO,             game_info_video,            CK_BOOL,    0),
  CFGI(CFG_GAME_INFO_MUSIC,             game_info_music,            CK_BOOL,    0),
  CFGI(CFG_ENABLE_WIFI,                 enable_wifi,                CK_BOOL,    0)
};
#undef CFGI

#define CFG_NITEMS (sizeof(cfg_items) / sizeof(cfg_items[0]))

int cfg_save() {
  char buttons[13];

  file_open((uint8_t*)CFG_FILE, FA_CREATE_ALWAYS | FA_WRITE);
  f_puts("---\n", &file_handle);
  for(unsigned i = 0; i < CFG_NITEMS; i++) {
    const cfg_item_t *it = &cfg_items[i];
    const uint8_t *p = (const uint8_t*)&CFG + it->off;
    const char *str;

    switch(it->kind) {
      case CK_NUM:
      case CK_NIB:
        f_printf(&file_handle, "%s: %d\n", it->key, *p);
        continue;
      case CK_BSXTIME: {
        uint64_t bcdtime = srtctime2bcdtime((uint8_t*)p);
        f_printf(&file_handle, "%s: %06lX%08lX\n", it->key,
                 (uint32_t)(bcdtime >> 32), (uint32_t)(bcdtime & 0xffffffffLL));
        continue;
      }
      case CK_BUTTONS: {
        /* uint16_t at an odd offset in a packed struct: copy it out first. */
        uint16_t bits;
        memcpy(&bits, p, sizeof(bits));
        cfg_buttons_bits2string(bits, buttons);
        str = buttons;
        break;
      }
      case CK_STR:
        str = (const char*)p;
        break;
      case CK_BOOL:
        str = *p ? "true" : "false";
        break;
      default:  /* a kind without a writer: emit nothing rather than guess */
        continue;
    }
    f_printf(&file_handle, "%s: %s\n", it->key, str);
  }
  file_close();
  return 0;
}

int cfg_load() {
  int err = 0;
  /* pre-load defaults: a key missing from the file keeps its CFG_DEFAULT value */
  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));
  yaml_file_open(CFG_FILE, FA_READ);
  if(file_res) {
    err = file_res;
  }
  if(!err) {
    yaml_token_t tok;
    for(unsigned i = 0; i < CFG_NITEMS; i++) {
      const cfg_item_t *it = &cfg_items[i];
      uint8_t *p = (uint8_t*)&CFG + it->off;
      long v;

      if(!yaml_get_itemvalue(it->key, &tok)) continue;
      switch(it->kind) {
        case CK_STR:
          /* strncpy, NOT strlcpy_nul: the WHOLE cfg_t is blitted into the shared
             BSRAM (cfg_load_to_menu), so a string field's tail travels with the
             struct and the zero padding keeps a longer previous value out of that
             window. */
          strncpy((char*)p, tok.stringvalue, CFG_STR_LEN - 1);
          p[CFG_STR_LEN - 1] = 0;
          break;
        case CK_BUTTONS: {
          uint16_t bits = cfg_buttons_string2bits(tok.stringvalue);
          memcpy(p, &bits, sizeof(bits));
          break;
        }
        case CK_BSXTIME:
          bcdtime2srtctime(strtoll(tok.stringvalue, NULL, 16), p);
          break;
        default:
          /* Numeric and boolean scalars.  The type check is MANDATORY: one token is
             reused for every key and yaml_detect_value fills only the field matching
             the type it detected, so reading the other one picks up a stale value from
             an earlier key.  A number on a boolean key (and vice versa) is accepted --
             non-zero is true, true is one -- anything else keeps the default. */
          if(tok.type == YAML_BOOL)      v = tok.boolvalue ? 1 : 0;
          else if(tok.type == YAML_LONG) v = tok.longvalue;
          else break;
          if(it->kind == CK_BOOL)     v = v ? 1 : 0;
          else if(it->kind == CK_NIB) v &= 0xf;
          /* Range check on the LONG, both ends, before the narrowing below: either
             half alone has a blind spot (see the clamp note above). */
          else if(it->clamp && (v < 0 || v > (long)(it->clamp >> 4))) v = it->clamp & 0xf;
          *p = (uint8_t)v;
          break;
      }
    }
  }
  yaml_file_close();
  CFG.ingame_buttons_menu = cfg_check_menu_combo(CFG.ingame_buttons_menu);
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
  strlcpy_nul(tmp, entry, sizeof(tmp));
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
  strlcpy_nul(out, disp, 256);
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
  strlcpy_nul(out, (const char*)listfilename, outsz - 4);
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
    strlcpy_nul(base, entry, sizeof(base));
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
      strlcpy_nul(base, entry, sizeof(base));
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
    strlcpy_nul(fqfn, (const char*)file_path, sizeof(fqfn));
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
          strlcpy_nul(keys[count], dirtmp, LISTED_DISP_KEYLEN);
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
    strlcpy_nul(fqfn, (const char*)file_path, sizeof(fqfn));
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
    strlcpy_nul(target, tok.stringvalue, count);
  } else if(count) {
    target[0] = 0;
  }
  yaml_file_close();
  return found;
}
