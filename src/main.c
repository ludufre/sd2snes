#include <string.h>
#include "config.h"
#include "version.h"
#include "clock.h"
#include "uart.h"
#include "bits.h"
#include "power.h"
#include "timer.h"
#include "ff.h"
#include "diskio.h"
#include "spi.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "filetypes.h"
#include "memory.h"
#include "snes.h"
#include "cover.h"
#include "gameinfo.h"
#include "led.h"
#include "sort.h"
#include "cic.h"
#include "tests.h"
#include "cli.h"
#include "sdnative.h"
#include "crc.h"
#include "smc.h"
#include "msu1.h"
#include "rtc.h"
#include "sysinfo.h"
#include "cfg.h"
#include "savestate.h"
#include "patch.h"
#include "patchmeta.h"
#include "cheat.h"
#include "theme.h"
#include "manual.h"
#include "nes.h"

//usb
#include "usb.h"
#include "usbhw.h"
#include "cdcuser.h"
#include "usbinterface.h"

int sd_offload = 0, ff_sd_offload = 0, sd_offload_tgt = 0;
int sd_offload_partial = 0;
int sd_offload_start_mid = 0;
int sd_offload_end_mid = 0;
uint16_t sd_offload_partial_start = 0;
uint16_t sd_offload_partial_end = 0;

uint16_t current_features = 0;

int snes_boot_configured, firstboot;
extern const uint8_t *fpga_config;

volatile enum diskstates disk_state;
extern volatile tick_t ticks;
extern snes_romprops_t romprops;
extern volatile int reset_changed;

extern volatile cfg_t CFG;
extern volatile mcu_status_t STM;
extern volatile snes_status_t STS;

/* Firmware-header placeholder: never referenced by code (filled post-build via
   objcopy --update-section .fwhdr). The linker KEEP()s it, but under -flto the
   whole-program optimizer would drop the unreferenced symbol BEFORE the linker
   sees it, so `used` is required to make LTO keep .fwhdr alive. */
const char fwhdr[CONFIG_FW_HEADERSIZE] __attribute__ ((used, section(".fwhdr")));

/* Drop any Recent/Favorite list entries whose ROM file no longer exists, then
   re-publish both lists + status to the SNES.  Called after every ROM delete
   (browser / favorites / recents) so a now-missing game can't linger in those
   lists and hang the loader when opened.  The boot-time check (main) covers
   files removed on a PC between sessions.  cfg_validity_check rewrites a .cfg
   (keeping a .bak) only when something actually changed. */
static void revalidate_game_lists(void) {
  cfg_validity_check_listed_games(LAST_FILE);
  STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
  cfg_validity_check_listed_games(FAVORITES_FILE);
  STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
  status_load_to_menu();
}

/* Patch-aware Recents/Favorites: a list entry may be "<rom>\t<patch_basename>".
   Given such a raw entry, recover the patch, stage it where load_rom() expects
   it and arm ips_pending_index so the relaunch re-applies the patch (reusing the
   same path as a normal LOADROM after ips_find_patches).  `entry` is truncated to
   the bare base ROM path on return (ready to hand to load_rom).  A patch deleted
   since launch degrades to a clean vanilla boot instead of failing. */
static void stage_patch_from_entry(char *entry) {
  char patchpath[256];
  const char *pbase;
  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  if(!cfg_parse_patch_entry(entry, patchpath, sizeof(patchpath))) {
    return; /* no patch tag -> plain base ROM */
  }
  FILINFO fno;
  fno.lfname = NULL;
  if(f_stat((TCHAR*)patchpath, &fno) != FR_OK) {
    printf("stage_patch: patch gone (%s) -> vanilla\n", patchpath);
    return; /* patch deleted -> boot base vanilla */
  }
  /* A path that does not fit the staging slot would be truncated and then opened
     as some OTHER file, so refuse it the same way the directory scan does. */
  if(strlen(patchpath) >= IPS_PATH_LEN) {
    printf("stage_patch: path too long (%s) -> vanilla\n", patchpath);
    return;
  }
  strncpy((char*)current_ips_srm_source, patchpath, sizeof(current_ips_srm_source) - 1);
  current_ips_srm_source[sizeof(current_ips_srm_source) - 1] = '\0';
  /* Stage the patch path where patch_apply()/bps_probe_header() read it, exactly
     as ips_find_patches() would have for a normal LOADROM (slot index 1).  No
     directory scan runs on this path, so drop whatever scan was live: its scratch
     describes some other ROM, and CMD_PATCH_META_SAVE reads basenames from it. */
  ips_scan_count = 0;
  sram_writeblock(current_ips_srm_source, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE,
                  (uint16_t)(strlen((char*)current_ips_srm_source) + 1));
  /* No directory scan happens on this path, so the header-mode override has to
     come straight from the sidecar -- `entry` is the base ROM path by now, which
     is exactly the key the sidecar is filed under. */
  pbase = strrchr(patchpath, '/');
  pbase = pbase ? pbase + 1 : patchpath;
  current_ips_flags = patchmeta_flags_for((const uint8_t*)entry, pbase,
                                          patch_ext_type(pbase) == PATCH_TYPE_BPS
                                            ? PATCH_TYPE_BPS : PATCH_TYPE_IPS);
  sram_writebyte(current_ips_flags, SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE);
  ips_pending_index = 1;
}

/* Resolve the Recents/Favorites entry at the index in MCU_PARAM (low byte) to
   the name its sidecar files (.srm/.yml) are keyed off: the PATCH for a
   patched entry (consistent with in-game current_ips_srm_source), the base
   ROM otherwise.  Raw entry lands in file_lfn; patchpath is caller scratch. */
static char *listed_game_sidecar_source(const uint8_t *listfile) {
  /* The result is returned in the global file_lfn (not a caller stack buffer) and
     the scratch patchpath is OUR local, popped on return.  This matters: the cheat
     menu reaches cheat_yaml_load()/cheat_yaml_save() through this on the recents/
     favorites path, and those carry a large frame (cheat_record_t + yaml_token_t).
     Keeping a 256-byte buffer alive in the *caller* across that call added enough
     depth to overflow the LPC1756's ~4 KB stack into .bss (recents cheats showed
     "no cheats"/hung; the shallower browser path was fine).  Resolving here and
     returning a global keeps the cheat-load stack as shallow as the browser. */
  char patchpath[256];
  cfg_get_listed_game_raw(listfile, file_lfn,
                          listed_game_resolve_index(listfile, snes_get_mcu_param() & 0xff));
  if(cfg_parse_patch_entry((char*)file_lfn, patchpath, sizeof(patchpath))) {
    /* patched entry: the sidecar key is the patch path -- move it into file_lfn */
    strncpy((char*)file_lfn, patchpath, sizeof(file_lfn) - 1);
    file_lfn[sizeof(file_lfn) - 1] = 0;
  }
  return (char*)file_lfn;
}

/* DELETE_FILE_{FAV,RECENT}: for a PATCHED entry, delete only the patch
   (.ips/.bps) and drop the list entry by index — the base ROM is shared and
   must stay.  For a plain entry, delete the ROM; revalidate_game_lists() then
   drops it from BOTH lists by file existence. */
static void delete_listed_game_file(const uint8_t *listfile, const char *what) {
  uint8_t idx = listed_game_resolve_index(listfile, snes_get_mcu_param() & 0xff);
  char patchpath[256];
  cfg_get_listed_game_raw(listfile, file_lfn, idx);
  if(cfg_parse_patch_entry((char*)file_lfn, patchpath, sizeof(patchpath))) {
    printf("Delete %s patch: %s\n", what, patchpath);
    if(f_unlink((TCHAR*)patchpath) != FR_OK) {
      snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
    }
    cfg_remove_listed_game(listfile, idx);
  } else {
    printf("Delete %s file: %s\n", what, file_lfn);
    if(f_unlink((TCHAR*)file_lfn) != FR_OK) {
      snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
    }
  }
  revalidate_game_lists();
}

/* DELETE_SRM_{FAV,RECENT}: delete the battery-SRAM save(s) for a list entry; the
   ROM/patch (and the list entry itself) stay in place.  Wipes ALL slots + the
   .slot sidecar (no slot UI in the list). Slot 0 (legacy <stem>.srm) drives the
   NACK exactly as before; slots 2-4 and the sidecar are best-effort. */
static void delete_listed_game_srm(const uint8_t *listfile, const char *what) {
  char *srmsrc = listed_game_sidecar_source(listfile);
  printf("Delete SRM for %s: %s\n", what, srmsrc);
  for(uint8_t s = 0; s < SRM_SLOT_COUNT; s++) {
    uint8_t srmfile[256];
    char ext[8];
    srm_slot_ext(ext, sizeof(ext), s);
    /* path_asset DIRECT, not memory.c's append_save_basename: that one re-applies
       current_ips_srm_source, which survives from the last LOADED game into the menu loop and
       would make this delete the previous game's patch save instead of the selected entry. */
    /* A name too long to fit leaves the buffer EMPTY; unlinking that would either fail oddly or,
       worse on some paths, act on the wrong name. Treat it as "nothing deleted" and NACK slot 0,
       same as a failed unlink -- the menu already knows how to report that. */
    if(path_asset((char*)srmfile, sizeof(srmfile), SAVE_BASEDIR, srmsrc, ext) < 0) {
      printf("SRM path too long for %s\n", srmsrc);
      if(s == 0) snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
      continue;
    }
    printf("SRM path: %s\n", srmfile);
    FRESULT r = f_unlink((TCHAR*)srmfile);
    if(s == 0 && r != FR_OK) {
      snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
    }
  }
  { uint8_t scfile[256];
    if(path_asset((char*)scfile, sizeof(scfile), SAVE_BASEDIR, srmsrc, ".slot") >= 0)
      f_unlink((TCHAR*)scfile);
  }
}

/* {LOAD,SAVE}_CHT_{FAV,RECENT}: load/save the cheat YAML for a list entry.
   The SNES side rewrites MCU_PARAM with the list index right before sending
   these, because the cheat toggle handler clobbers it. */
static void listed_game_cheats(const uint8_t *listfile, const char *what,
                               int save) {
  /* No 256-byte buffer in this frame: listed_game_sidecar_source resolves into
     the global file_lfn so the cheat_yaml_load/save call below stays as shallow
     as the browser path (LPC1756 ~4 KB stack -- see the note there). */
  char *src = listed_game_sidecar_source(listfile);
  printf("%s cheats for %s: %s\n", save ? "Save" : "Load", what, src);
  if(save) cheat_yaml_save((uint8_t*)src);
  else     cheat_yaml_load((uint8_t*)src);
}

/* Stage where the browser should reopen after a menu reload, and arm the one-shot
   ST_RESTORE_BROWSER flag the SNES reads on the next boot.

   Every theme/BGM action cold-boots the menu (menu_reload -> the outer loop reloads
   m3nu.bin and resets the SNES), and clear_wram fills $7E/$7F with $55, so the menu's
   own dirlog/filesel_sel cannot survive -- only strings in BSRAM do. filesel_nav_restore
   re-navigates the path component by component from these, the same way
   filesel_nav_last does for reset-to-menu.

   `path` = full SD path of the item that was acted on (browser selections), or NULL when
   the action came from the config menu, in which case we reopen the folder the browser is
   sitting in (FILESEL_CWD, which the menu keeps at SRAM_MENU_FILEPATH_ADDR) with no item
   pre-selected. Never reuse SRAM_LASTGAME_DIR/FILE for this: those are rewritten on every
   menu boot by cfg_dump_listed_games_for_snes. */
static void browser_pos_save(const char *path) {
  char dir[256];
  const char *slash = path ? strrchr(path, '/') : NULL;
  if(slash) {
    size_t dir_len = slash - path;
    if(dir_len == 0 || dir_len >= sizeof(dir)) {
      strcpy(dir, "/");
    } else {
      memcpy(dir, path, dir_len);
      dir[dir_len] = 0;
    }
    sram_writestrn((uint8_t*)dir, SRAM_BROWSER_DIR_ADDR, sizeof(dir));
    sram_writestrn((uint8_t*)(slash + 1), SRAM_BROWSER_FILE_ADDR, 256);
  } else {
    /* no path: reopen the browser's current folder, cursor left at the top */
    sram_readstrn(dir, SRAM_MENU_FILEPATH_ADDR, sizeof(dir));
    if(dir[0] != '/') strcpy(dir, "/");
    sram_writestrn((uint8_t*)dir, SRAM_BROWSER_DIR_ADDR, sizeof(dir));
    sram_writestrn((uint8_t*)"", SRAM_BROWSER_FILE_ADDR, 1);
  }
  STM.restore_browser = 1;
}

void menu_cmd_readdir(void) {
  uint8_t path[256];
  SNES_FTYPE filetypes[16];
  snes_get_filepath(path, 256);
  snescmd_readstrn(filetypes, SNESCMD_MCU_PARAM + 8, sizeof(filetypes));
  uint32_t tgt_addr = snescmd_readlong(SNESCMD_MCU_PARAM + 4) & 0xffffff;
printf("path=%s tgt=%06lx types=", path, tgt_addr);
uart_puts_hex((char*)filetypes);
uart_putc('\n');
  uint16_t n = scan_dir(path, tgt_addr, filetypes);
  /* scan_dir grows the file-STRING table from SRAM_MANUAL_S1TILES_ADDR ($C30000) up to $C80000,
     i.e. straight through BOTH manual staging regions. Drop the "page already resident" memo or
     the next SNES_CMD_MANUAL_S1PAGE/_ZPAGE for the same page takes its skip branch and the viewer
     DMAs these filenames into VRAM as tiles. (The menu-side viewer's exit fires exactly this
     READDIR to rebuild the table -- see snes/manhost.a65.) */
  manual_invalidate_resident();
  /* Hand the authoritative entry count back to the menu through the snescmd
     param region (BRAM-backed, reliable to read from the SNES immediately).
     The menu sets dirend_addr = n*4 from this instead of scanning the SDRAM dir
     table at $C2 (SRAM_DIR_ADDR) itself, which can read a stale/partial buffer in the short
     window right after this write -> bogus short dirend -> broken pagination. */
  snescmd_writeshort(n, SNESCMD_MCU_PARAM);
}

#ifndef CONFIG_MK2
/* "Create patched ROM" (SNES_CMD_EXPORT_PATCHED_ROM).
 *
 * Lifted out of the command switch because it is a whole procedure rather than a
 * dispatch: it halts the SNES, drives a full load_rom + patch, streams the image
 * out and drags every sidecar across.
 *
 * The ROM path comes in through file_lfn, which the CALLER fills via
 * get_selected_name -- and it must do so only AFTER reading the index out of
 * MCU_PARAM, because get_selected_name goes through set_mcu_addr, which preserves
 * only the low 24 bits.
 *
 * Leaves the SNES HELD IN RESET on every path.  That is deliberate: PSRAM at
 * 0x000000 now holds the exported GAME, so releasing the CPU would boot it and let
 * it run -- reprogramming the PPU and trampling WRAM -- throughout the menu
 * teardown that follows (RTC probe, cfg_save, ...), until the reload's reset pulse
 * cut it off mid-stride.  The caller's menu_reload owns the reset from here: it
 * restores the base core, loads the menu image and issues snes_reset(1)/(0) itself.
 *
 * Returns the PATCH_EXPORT_* code to park for the reloaded browser to report.
 */
static uint8_t patch_export_command(uint8_t xidx) {
  uint8_t result = PATCH_EXPORT_FAILED;

  /* A Game Boy ROM is staged behind the SGB BIOS at a different base, so
     exporting it would write out the BIOS, not the game. */
  if(xidx >= 1 && xidx <= IPS_MAX_PATCHES && !path_is_gb((char*)file_lfn)) {
    uint32_t rom_bytes;

    ips_pending_index = xidx;
    sram_readstrn(current_ips_srm_source,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                  + (uint32_t)(xidx - 1) * IPS_PATH_LEN,
                  sizeof(current_ips_srm_source));

    /* Refuse a name collision HERE, before the SNES is halted and before the
       multi-second load_rom+patch: re-running an export is almost always an
       accident, and the old answer -- silently minting "<...> 2.sfc" -- duplicated
       a multi-MB ROM and split its saves off under the new stem.  The SNES is
       still alive spinning in pcr_wait; the menu reload the caller issues either
       way is what recovers it, and the reloaded browser reports EXISTS.
       (PSRAM reads with the menu live are the norm; nothing here remaps.) */
    if(patch_export_exists(current_ips_srm_source)) {
      result = PATCH_EXPORT_EXISTS;
    } else {
      /* STOP THE SNES FIRST.  It is spinning inside patch_create_rom, executing the
         MENU out of PSRAM -- and load_rom reprograms the cartridge mapping for the
         game (set_rom_mask, set_mapper) LONG before its own assert_reset.  The moment
         that remap lands, the code under the spinning CPU changes and it runs off
         into whatever the new mapping exposes, scribbling over the PPU (the shredded
         header rows).  The normal boot path is immune because the SNES waits in the
         WRAM trampoline at $7EF000, off the cartridge entirely; the export has no
         trampoline, so it must halt the CPU. */
      assert_reset();

      current_ips_flags = sram_readbyte(SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE + xidx - 1);
      patch_export_size = 0;
      patch_last_ok = 0;
      rom_export_active = 1;   /* keep the SNES in reset across the dump */
      rom_bytes = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_RESET);
      rom_export_active = 0;

      /* load_rom boots on through a failed patch by design; the export must not.
         Writing the .sfc from an unpatched (or half-patched) image hands the user a
         file that lies about what it contains -- and for a failed BPS
         patch_export_size is 0, so it would be the pristine ROM verbatim. */
      if(rom_bytes && patch_last_ok
         && patch_export_write(current_ips_srm_source,
                               SRAM_ROM_ADDR + romprops.load_address,
                               patch_export_size ? patch_export_size
                                                 : romprops.romsize_bytes)) {
        /* Bring the cover, info screen, guides, cheats and saves along, so the new
           entry is not a bare ROM the user has to re-decorate by hand.  MUST run
           before current_ips_srm_source is cleared below: the saves and states of a
           patched session are filed under the PATCH's name.
           current_filename, NOT file_lfn: load_rom's savestate slot scan calls
           cfg_get_listed_game(LAST_FILE, file_lfn, 0) (savestate.c:32), which
           overwrites that global with the TOP OF RECENTS -- so by now file_lfn
           names whatever was played last, not the ROM just loaded.  That is how a
           correctly patched Chrono Trigger once got its sidecars copied from
           another game's name.  load_rom stashes the real path in
           current_filename and nothing else touches it. */
        result = patch_export_copy_assets((uint8_t*)current_filename,
                                          current_ips_srm_source)
               ? PATCH_EXPORT_OK : PATCH_EXPORT_PARTIAL;
      }
    }
  }

  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  return result;
}
#endif /* !CONFIG_MK2 */

/* Commands issued FROM the pre-boot info screen, i.e. the ones that must NOT stop a running FMV.
   Everything else means the SNES left that screen (see the call site in the menu loop).
     - FMV_NEXT                        : the pump itself
     - GAME_INFO / _RECENT / _FAVORITE : a different game selected while the screen stays up
     - GI_DESC_FULL                    : Y = full description, drawn OVER the still-playing clip.
                                         Stopping here killed the video AND its audio for good
                                         (nothing ever re-opens the .fmv) -- that was the bug.
     - MANUAL_S1PAGE / MANUAL_ZPAGE    : the manual viewer opened from the info screen; the same
                                         reasoning applies on the way back out of it. */
static int cmd_keeps_fmv(uint8_t cmd) {
  return cmd == SNES_CMD_FMV_NEXT
      || cmd == SNES_CMD_GAME_INFO
      || cmd == SNES_CMD_GAME_INFO_RECENT
      || cmd == SNES_CMD_GAME_INFO_FAVORITE
      || cmd == SNES_CMD_GI_DESC_FULL
      || cmd == SNES_CMD_MANUAL_S1PAGE
      || cmd == SNES_CMD_MANUAL_ZPAGE;
}

int main(void) {
  power_init();
  GPIO_MODE_OUT(SNES_CIC_PAIR_REG, SNES_CIC_PAIR_BIT);
  SET_BIT(SNES_CIC_PAIR_REG, SNES_CIC_PAIR_BIT);
  GPIO_MODE_OUT(FPGA_SSREG, FPGA_SSBIT);

#ifdef DAC_DEMREG
  BITBAND(DAC_DEMREG->FIODIR, DAC_DEMBIT) = 1;
  BITBAND(DAC_DEMREG->FIOSET, DAC_DEMBIT) = 1;
#endif
  /* pull-down CIC data lines */
  GPIO_PULLDOWN(SNES_CIC_D0_REG, SNES_CIC_D0_BIT);
  GPIO_PULLDOWN(SNES_CIC_D1_REG, SNES_CIC_D1_BIT);

  /* pull-up SuperCIC status line so missing CIC clock doesn't result in lockup */
  GPIO_PULLUP(SNES_CIC_STATUS_REG, SNES_CIC_STATUS_BIT);

 /* PCLKSEL settings applied by above peripheral inits may be ineffective after
    PLL0 has been connected, so first disconnect PLL0, then do peripheral setup
    Erratum ES_LPC175x - PCLKSELx.1 */
  clock_disconnect();
  snes_init();
  snes_reset(1);
  timer_init();
  uart_init();
  fpga_spi_init();
  spi_preinit();
  led_init();
  led_std();
 /* and setup & connect PLL0 again */
  clock_init();

  led_std();
  sdn_init();

 /* USB initialization. Not affected by PCLKSELx.1 erratum */
  USB_Init ();
  CDC_Init (0x00);
  USB_Connect (1);

  printf("\n\n" DEVICE_NAME "\n===============\nfw ver.: " CONFIG_VERSION "\ncpu clock: %d Hz\n", CONFIG_CPU_FREQUENCY);
#ifdef CONFIG_MK3_STM32
  printf("AHB1ENR=%lx\n", RCC->AHB1ENR);
  printf("AHB2ENR=%lx\n", RCC->AHB2ENR);
  printf("APB1ENR=%lx\n", RCC->APB1ENR);
  printf("APB2ENR=%lx\n", RCC->APB2ENR);
#else
  printf("PCONP=%lx\n", LPC_SC->PCONP);
#endif
  file_init();

  cic_preinit();
  cic_init(0);

  fpga_init();
  firstboot = 1;
  while(1) {
    snes_boot_configured = 0;
    while(get_cic_state() == CIC_FAIL) {
      rdyled(0);
      readled(0);
      writeled(0);
      delay_ms(500);
      rdyled(1);
      readled(1);
      writeled(1);
      delay_ms(500);
    }
    /* some sanity checks */
    uint8_t card_go = 0;
    while(!card_go) {
      if(disk_status(0) & (STA_NODISK)) {
        snes_bootclear();
        delay_ms(50);
        snes_bootprint_version();
        snes_bootprint_center( 8, "No SD Card found!");
        snes_bootprint_center( 9, "\x12\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x11");
        snes_bootprint_center(11, "Please insert SD Card and");
        snes_bootprint_center(13, "make sure it is seated");
        snes_bootprint_center(15, "properly.");
        cli_entrycheck();
        while(disk_status(0) & (STA_NODISK));
        snes_bootprint_center(17, "SD Card inserted!");
        delay_ms(200);
      }
      file_open((uint8_t*)MENU_FILENAME, FA_READ);
      if(file_status != FILE_OK) {
        char *errorname;
        errorname = get_fresult_friendlyname(file_res);
        snes_bootclear();
        delay_ms(50);
        snes_bootprint_version();
        snes_bootprint_center( 5, "Could not load menu ROM!");
        snes_bootprint_center( 6, "\x12\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x13\x11");
        snes_bootprint_center( 9, "Error: %s", errorname);
        snes_bootprint_center(12, "Check that your card is wor-");
        snes_bootprint_center(14, "king, formatted correctly");
        snes_bootprint_center(16, "(MBR+FAT32), and that the");
        snes_bootprint_center(18, "file " MENU_FILENAME);
        snes_bootprint_center(20, "exists.");
        cli_entrycheck();
        while((disk_status(0) & ~STA_PROTECT) == 0);
      } else {
        card_go = 1;
      }
      file_close();
    }
    if(fpga_config == FPGA_ROM) {
      snes_bootclear();
      snes_bootprint_version();
      snes_bootprint_center(12, "Loading ...");
    }
    led_pwm();
    rdyled(1);
    readled(0);
    writeled(0);

    cic_init(0);

    if(firstboot) {
      cfg_load();
      cfg_save();
      cfg_validity_check_listed_games(LAST_FILE);
      cfg_validity_check_listed_games(FAVORITES_FILE);
    }
    if(fpga_config != FPGA_BASE) fpga_pgm((uint8_t*)FPGA_BASE);
    STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
    STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
#ifdef CONFIG_MK2
    STM.is_mk2 = 1;   /* greys + refuses "create patched ROM" only; see snes.h */
#else
    STM.is_mk2 = 0;
#endif
    /* A just-finished "create patched ROM" wants the browser to open ON the new file.
       This has to run AFTER the dump above, which rewrites both of those from the
       recents list.  Reuses the reset-to-menu navigation (filesel_nav_last), which
       walks LASTGAME_DIR component by component and then selects LASTGAME_FILE. */
    /* PSRAM comes up with whatever the last power-on left in it -- nothing zeroes
       $FF07xx -- so clear this byte ONCE, or garbage in it pops "Cannot write
       patched ROM" on a cold start with no export in sight.
       It has to be HERE, not up next to fpga_init(): sram_writebyte drives the FPGA
       memory window and blocks in an UNBOUNDED FPGA_WAIT_RDY, so before fpga_pgm()
       above has configured the FPGA that call never returns -- no menu, no USB, dead
       console.  And it has to be gated on firstboot: a real export writes this byte
       and then asks for a menu reload, which re-enters this very loop, and the
       reloaded browser is exactly who consumes it. */
    if(firstboot) sram_writebyte(PATCH_EXPORT_NONE, SRAM_EXPORT_RESULT_ADDR);
    /* OK *and* PARTIAL: a partial export still WROTE the .sfc -- only some sidecar
       failed to copy -- and export_result_check (snes/patch.a65) navigates the
       browser to it in both cases.  Testing == PATCH_EXPORT_OK here left LASTGAME_
       DIR/FILE pointing at the previous entry, so a partial export walked the user
       to the top of Recents instead of to the file just created.
       EXISTS too (the belt-and-braces refusal inside the export, when a file
       appeared between the pre-flight CMD_EXPORT_CHECK and the run): there the
       staged path is the EXISTING .sfc, and landing on it beats dumping the user
       at the top of a fresh browser. */
    uint8_t xres = sram_readbyte(SRAM_EXPORT_RESULT_ADDR);
    if(xres == PATCH_EXPORT_OK || xres == PATCH_EXPORT_PARTIAL
       || xres == PATCH_EXPORT_EXISTS) {
      uint8_t xpath[256];
      sram_readstrn(xpath, SRAM_EXPORT_PATH_ADDR, sizeof(xpath));
      char *xslash = strrchr((char*)xpath, '/');
      if(xslash) {
        sram_writestrn((uint8_t*)(xslash + 1), SRAM_LASTGAME_FILE_ADDR, 256);
        if(xslash == (char*)xpath) {
          sram_writestrn((uint8_t*)"/", SRAM_LASTGAME_DIR_ADDR, 256);
        } else {
          *xslash = 0;
          sram_writestrn(xpath, SRAM_LASTGAME_DIR_ADDR, 256);
        }
      }
    }
    led_set_brightness(CFG.led_brightness);

    /* DEBUG: boot-time self-test of the MCU-driven copier in fpga_base (SNES in
       reset here).  Writes 0x01 to $FF0726 if the copier works, 0x00 if not, for a
       USB read -- proves the FPGA change independent of any patch / chip core. */
    { extern int patch_copier_available(void);
      sram_writebyte(patch_copier_available() ? 0x01 : 0x00, 0xFF0726L); }

    /* load menu */
    sram_writelong(0x12345678, SRAM_SCRATCHPAD);
    fpga_dspx_reset(1);
    uart_putc('(');
    load_rom((uint8_t*)MENU_FILENAME, SRAM_MENU_ADDR, 0);
    /* apply the selected menu theme (if any) by patching the gfxptr regions of
       the just-loaded menu image in PSRAM, before the SNES runs setup_gfx.
       Fail-safe: a missing/bad theme leaves the baked menu untouched. */
    theme_apply();
    /* force memory size + mapper */
    set_rom_mask(0x3fffff);
    set_mapper(0x7);
    /* disable all cheats+hooks */
    fpga_write_cheat(7, 0x3f00);
    /* reset DAC */
    dac_pause();
    dac_reset(0);
    uart_putc(')');
    uart_putcrlf();

    sram_writebyte(0, SRAM_CMD_ADDR);
    /* menu sound effects: start with an empty SFX mailbox (dedicated byte,
       outside the command handshake - see snes.c menu_main_loop) */
    snescmd_writebyte(0, SNESCMD_SFX_MAILBOX);

    if((rtc_state = rtc_isvalid()) != RTC_OK) {
      printf("RTC invalid!\n");
      STM.rtc_valid = 0xff;
      set_bcdtime(0x20120701000000LL);
      set_fpga_time(0x20120701000000LL);
      invalidate_rtc();
    } else {
      printf("RTC valid!\n");
      STM.rtc_valid = 0;
      set_fpga_time(get_bcdtime());
    }
    sram_memset(SRAM_SYSINFO_ADDR, 13*40, 0x20);
    printf("SNES GO!\n");
    snes_reset(1);
    fpga_reset_srtc_state();
    if(!firstboot) {
      if(STS.is_u16 && (STS.u16_cfg & 0x01)) {
        delay_ms(59*SNES_RESET_PULSELEN_MS);
      }
    }
    firstboot = 0;
    delay_ms(SNES_RESET_PULSELEN_MS);
    sram_writebyte(32, SRAM_CMD_ADDR);

    fpga_set_dac_boost(CFG.msu_volume_boost);
    cfg_load_to_menu();
    cfg_save();
    snes_reset(0);

/* Since the Super Nt workaround requires pair mode to be disabled during reset
   (or the Super Nt doesn't boot), pair mode can only be enabled after reset,
   so we need to get the CIC state later to actually detect pair mode.
   A delay is required so the CICs can settle before getting the state. */
    delay_ms(100);
    enum cicstates cic_state = get_cic_state();
    switch(cic_state) {
      case CIC_PAIR:
        STM.pairmode = 1;
        printf("PAIR MODE ENGAGED!\n");
        cic_pair(CFG.vidmode_menu, CFG.vidmode_menu);
        break;
      case CIC_SCIC:
        STM.pairmode = 1;
        break;
      default:
        STM.pairmode = 0;
    }
    STM.autoboot_enabled = cfg_is_autoboot_enabled();
    status_load_to_menu();
    STM.reset_to_menu_active = 0;  /* SRAM now holds the flag for the SNES; zero in RAM so later status_load_to_menu() calls don't re-broadcast it */
    STM.restore_browser = 0;       /* same one-shot contract: this boot consumes it (see browser_pos_save) */

    uint8_t cmd = 0;
    uint8_t menu_reload = 0;
    uint64_t btime = 0;
    uint32_t filesize=0;
    printf("test sram\n");
    while(!sram_reliable()) cli_entrycheck();
    printf("ok\n");
//while(1) {
//  delay_ms(1000);
//  printf("Estimated SNES master clock: %ld Hz\n", get_snes_sysclk());
//}
  //sram_hexdump(SRAM_DB_ADDR, 0x200);
  //sram_hexdump(SRAM_MENU_ADDR, 0x400);
    while(!cmd) {
      /* tell the menu we're ready to accept commands */
      snescmd_writebyte(MCU_CMD_RDY, SNESCMD_SNES_CMD);
      cmd=menu_main_loop();
      /* acknowledge command */
      echo_mcu_cmd();
      printf("cmd: %d\n", cmd);
      status_save_from_menu();
      uart_putc('-');
      /* FMV plays only while the info screen is up. Any command the info screen itself does NOT
         issue (see cmd_keeps_fmv) means the SNES left the screen -> stop it (close the file + the
         DAC clip) so the DAC frees up for the browser's nav SFX. No-op if no FMV is active.
         (Returning to the Favorites/Recents list issues NO command -> the idle watchdog in snes.c
         covers it.) */
      if(!cmd_keeps_fmv(cmd))
        gameinfo_fmv_stop();
      switch(cmd) {
        case SNES_CMD_LOADROM:
          /* Read the IPS patch index BEFORE get_selected_name so that the
             MCU_PARAM+7 byte is not overwritten.  set_mcu_addr() only uses
             the lower 24 bits, so the index byte at offset +7 is safe. */
          ips_pending_index = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
          get_selected_name(file_lfn);
          printf("Selected name: %s (patch idx=%d)\n", file_lfn, ips_pending_index);
          /* Build the SRM-override path from the IPS file's full SD path. */
          current_ips_srm_source[0] = '\0';
          current_ips_flags = 0;
          if(ips_pending_index > 0 && ips_pending_index <= IPS_MAX_PATCHES) {
            sram_readstrn(current_ips_srm_source,
                          SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                          + (uint32_t)(ips_pending_index - 1) * IPS_PATH_LEN,
                          sizeof(current_ips_srm_source));
            /* Kept in MCU RAM so a recore reload can re-stage it (see memory.c). */
            current_ips_flags = sram_readbyte(SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE
                                              + (uint32_t)(ips_pending_index - 1));
            printf("Patch SRM source: %s (flags %02x)\n", current_ips_srm_source,
                   current_ips_flags);
          }
          /* Record into Recents BEFORE the load.  This DOES sit on the critical
             path -- it is SD write traffic (stream the list, write a .tmp, unlink,
             rename) and the SNES is parked in game_handshake until load_rom hands
             it the 0x55 -- but moving it after the load broke it: Recents stopped
             updating and, with it, the reset-to-menu "return to the last ROM"
             navigation that reads the same list.  The stall it costs is hidden the
             right way instead, by starting the iris as soon as the command is sent
             (snes/main.a65) rather than by shortening the MCU's work.
             For a patched launch, store "<rom>\t<patch_basename>" so the list
             shows/relaunches the patch (cfg_add_listed_game_patched appends the tag
             with bounded strncat and dedups on the whole string; cwd qualification
             still applies to the leading ROM part, and an over-long entry degrades
             to base-only inside the helper). */
          if(current_ips_srm_source[0]) {
            const char *pbase = strrchr((char*)current_ips_srm_source, '/');
            pbase = pbase ? pbase + 1 : (char*)current_ips_srm_source;
            cfg_add_listed_game_patched(LAST_FILE, file_lfn, pbase, true);
          } else {
            cfg_add_listed_game(LAST_FILE, file_lfn, true);
          }
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
          if(filesize) break; /* ROM loaded and SNES reset, exit menu loop */
          /* load aborted (missing chip BIOS etc.): NACK so game_handshake_error
             shows the message and returns to the browser; stay in the menu loop
             (else the MCU drops into in-game mode while the SNES is still in menu). */
          file_res = FR_OK;
          snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          cmd=0;
          break;
        case SNES_CMD_QUERY_IPS_PATCHES: {
          /* MUST be its own buffer, never file_lfn: patch_scan_dir points the
             FatFs long-name buffer AT file_lfn (fno.lfname), so the first
             f_readdir would overwrite the ROM path we are scanning for -- the
             stem stops matching, the scan returns 0 patches, and the dialog
             silently never opens.  patch_publish/patchmeta_apply read the same
             pointer afterwards and would key the sidecar off whatever directory
             entry happened to be read last. */
          uint8_t qpath[256];
          get_selected_name(qpath);
          current_ips_srm_source[0] = '\0';
          current_ips_flags = 0;
          ips_find_patches(qpath, SRAM_IPS_LIST_ADDR);
          cmd = 0; /* stay in menu loop */
          break;
        }
        case SNES_CMD_PATCH_META_SAVE:
          /* The menu edited the header-mode field of the staged flags bytes; rewrite
             the sidecar from the scan we still hold (which also prunes entries whose
             patch has since been deleted).  patchmeta_save pairs each staged flags
             byte with its basename through patch_basename_at, so nothing has to be
             folded back by hand here.  Guard on the scan being live so a stale one
             can never be written out under some other ROM's name. */
          if(ips_scan_count) {
            get_selected_name(file_lfn);
            patchmeta_save(file_lfn, SRAM_IPS_LIST_ADDR, ips_scan_count);
          }
          snescmd_writebyte(0x55, SNESCMD_SNES_CMD);
          cmd = 0; /* stay in menu loop */
          break;
#ifndef CONFIG_MK2
        /* Not built on the mk2: its 122624-byte flash is full, and this handler plus
           patch_export_write and the sidecar copier do not fit.  The menu greys the
           "create patched ROM" entry there and patch_create_rom refuses it outright
           (ST_IS_MK2), so neither command is ever sent. The REST of that context menu
           does run on the mk2 -- CMD_PATCH_META_SAVE above is built for every config. */
        case SNES_CMD_EXPORT_CHECK: {
          /* Pre-flight for the export below: answer "would it collide?" while the
             menu is still fully alive, so a refusal is a modal over the live patch
             dialog instead of a screen teardown + cold boot.  Index read BEFORE
             get_selected_name for the same set_mcu_addr reason as the export. */
          uint8_t cidx = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
          uint8_t cres = PATCH_EXPORT_NONE;
          get_selected_name(file_lfn);
          if(cidx >= 1 && cidx <= IPS_MAX_PATCHES && !path_is_gb((char*)file_lfn)) {
            sram_readstrn(current_ips_srm_source,
                          SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                          + (uint32_t)(cidx - 1) * IPS_PATH_LEN,
                          sizeof(current_ips_srm_source));
            /* patch_export_exists also stages the existing path in
               SRAM_EXPORT_PATH_ADDR; unread on this path, needed on the fallback. */
            if(patch_export_exists(current_ips_srm_source))
              cres = PATCH_EXPORT_EXISTS;
            current_ips_srm_source[0] = '\0';
          }
          /* Persistent answer first, $55 after: the menu waits on the $55 and then
             reads EXPORT_RESULT (and one-shots it) -- no transient-NACK race. */
          sram_writebyte(cres, SRAM_EXPORT_RESULT_ADDR);
          snescmd_writebyte(0x55, SNESCMD_SNES_CMD);
          cmd = 0; /* stay in menu loop */
          break;
        }
        case SNES_CMD_EXPORT_PATCHED_ROM: {
          /* Read the index BEFORE get_selected_name, same as LOADROM: that call
             goes through set_mcu_addr, which only preserves the low 24 bits.
             It has to be its own statement, not an argument alongside the call --
             C does not order argument evaluation. */
          uint8_t xidx = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
          get_selected_name(file_lfn);
          /* The SNES is in reset and cannot read a handshake byte, so the outcome is
             parked here and the reloaded browser pops a modal for it -- the same
             modal machinery the missing-chip-BIOS message uses. */
          sram_writebyte(patch_export_command(xidx), SRAM_EXPORT_RESULT_ADDR);
          /* The menu image in PSRAM was overwritten by the game load either way,
             so the menu has to be cold-booted back; that is also what makes the
             new file show up in the browser listing.  NOTE: no cmd = 0 here --
             menu_reload only takes effect once this loop exits (see SET_THEME). */
          menu_reload = 1;
          break;
        }
#endif /* !CONFIG_MK2 */
        case SNES_CMD_SETRTC:
          /* get time from RAM */
          btime = snescmd_gettime();
          /* set RTC */
          set_bcdtime(btime);
          set_fpga_time(btime);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SYSINFO:
          /* go to sysinfo loop */
          sysinfo_loop();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOADSPC:
          /* load SPC file */
          get_selected_name(file_lfn);
          printf("Selected name: %s\n", file_lfn);
          filesize = load_spc(file_lfn, SRAM_SPC_DATA_ADDR, SRAM_SPC_HEADER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_MENU_SPC:
          /* stage background menu music. Use the user-chosen .spc (CFG.bgm_name, a
             full SD path set via SNES_CMD_SET_MENU_SPC) when present, otherwise fall
             back to the fixed /sd2snes/menu.spc. load_spc is graceful: a missing/
             too-small file zeroes the SPC header, which the menu detects and skips. */
          filesize = load_spc((uint8_t*)(CFG.bgm_name[0] == '/' ? CFG.bgm_name : (uint8_t*)"/sd2snes/menu.spc"),
                              SRAM_SPC_DATA_ADDR, SRAM_SPC_HEADER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_RESET:
          /* process RESET request from SNES */
          printf("RESET requested by SNES\n");
          snes_reset_pulse();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOADLAST:
          /* read RAW so the "<rom>\t<patch>" tag survives the move-to-top, then
             stage the patch (re-applied by load_rom) and load the base ROM. */
          cfg_get_listed_game_raw(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name: %s\n", file_lfn);
          cfg_add_listed_game(LAST_FILE, file_lfn, true);
          stage_patch_from_entry((char*)file_lfn);
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
          if(filesize) break; /* booted, exit menu loop */
          file_res = FR_OK;
          snescmd_writebyte(0xaa, SNESCMD_SNES_CMD); /* NACK -> error popup, stay in menu */
          cmd=0;
          break;
        case SNES_CMD_LOADFAVORITE:
          cfg_get_listed_game_raw(FAVORITES_FILE, file_lfn,
                                  listed_game_resolve_index(FAVORITES_FILE, snes_get_mcu_param() & 0xff));
          printf("Selected name: %s\n", file_lfn);
          cfg_add_listed_game(LAST_FILE, file_lfn, true);   /* lands in recents too, tag intact */
          stage_patch_from_entry((char*)file_lfn);
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
          if(filesize) break; /* booted, exit menu loop */
          file_res = FR_OK;
          snescmd_writebyte(0xaa, SNESCMD_SNES_CMD); /* NACK -> error popup, stay in menu */
          cmd=0;
          break;
/*        case SNES_CMD_SET_ALLOW_PAIR:
          cfg_set_pair_mode_allowed(snes_get_mcu_param() & 0xff);
          break;
        case SNES_CMD_SELECT_FILE:
          menu_cmd_select_file();
          cmd=0;
          break;
        case SNES_CMD_SELECT_LAST_FILE:
          menu_cmd_select_last_file();
          cmd=0;
          break;*/
        case SNES_CMD_READDIR:
          menu_cmd_readdir();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_GAMELOOP:
          /* enter game loop immediately */
          break;
        case SNES_CMD_SAVE_CFG:
          /* save config */
          cfg_get_from_menu();
          cic_init(CFG.pair_mode_allowed);
          if(CFG.pair_mode_allowed && cic_state == CIC_SCIC) {
            delay_ms(100);
            if(get_cic_state() == CIC_PAIR) {
              cic_pair(CFG.vidmode_menu, CFG.vidmode_menu);
            }
          }
          cic_videomode(CFG.vidmode_menu);
          fpga_set_dac_boost(CFG.msu_volume_boost);
          cfg_save();
          /* re-dump favorites so a just-toggled SortFavorites takes effect the next
             time the list opens (the dump honors CFG.sort_favorites). */
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LED_BRIGHTNESS:
          cfg_get_from_menu();
          led_set_brightness(CFG.led_brightness);
          cmd=0;
          break;
        case SNES_CMD_ADD_FAVORITE_ROM:
          get_selected_name(file_lfn);
          printf("Selected name: %s\n", file_lfn);
          /* returns 1 ONLY when the list is full and the game is not already in it
             (0 = added, <0 = write error).  Report just the full case to the menu so
             it can show a "list full" popup; the dump+status sync below carry it. */
          STM.favorites_full = (cfg_add_listed_game(FAVORITES_FILE, file_lfn, false) == 1);
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_ADD_FAVORITE_RECENT:
          /* RAW so a patched recent carries its "<rom>\t<patch>" tag into Favorites. */
          cfg_get_listed_game_raw(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name from recent: %s\n", file_lfn);
          STM.favorites_full = (cfg_add_listed_game(FAVORITES_FILE, file_lfn, false) == 1);
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0;
          break;
        case SNES_CMD_REMOVE_RECENT_ROM:
          cfg_remove_listed_game(LAST_FILE, snes_get_mcu_param() & 0xff);
          STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
          status_load_to_menu();
          cmd=0;
          break;
        case SNES_CMD_REMOVE_FAVORITE_ROM:
          cfg_remove_listed_game(FAVORITES_FILE,
                                 listed_game_resolve_index(FAVORITES_FILE, snes_get_mcu_param() & 0xff));
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SET_AUTOBOOT_ROM:
          get_selected_name(file_lfn);
          printf("Set autoboot ROM: %s\n", file_lfn);
          cfg_set_autoboot_rom(file_lfn);
          STM.autoboot_enabled = 1;
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SET_AUTOBOOT_FAV:
          /* RAW so the patch tag is stored in autoboot.cfg (round-trips on NUL);
             SNES_CMD_LOAD_AUTOBOOT re-applies the patch at boot. */
          cfg_get_listed_game_raw(FAVORITES_FILE, file_lfn,
                                  listed_game_resolve_index(FAVORITES_FILE, snes_get_mcu_param() & 0xff));
          printf("Set autoboot from favorite: %s\n", file_lfn);
          cfg_set_autoboot_rom(file_lfn);
          STM.autoboot_enabled = 1;
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SET_AUTOBOOT_RECENT:
          cfg_get_listed_game_raw(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name: %s\n", file_lfn);
          cfg_set_autoboot_rom(file_lfn);
          STM.autoboot_enabled = 1;
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_CLR_AUTOBOOT_ROM:
          printf("Clear autoboot ROM\n");
          cfg_clr_autoboot_rom();
          STM.autoboot_enabled = 0;
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_AUTOBOOT:
          ips_pending_index = 0;
          current_ips_srm_source[0] = '\0';
          cfg_get_autoboot_rom(file_lfn);
          printf("Autobooting: %s\n", file_lfn);
          if(file_lfn[0]) {
            cfg_add_listed_game(LAST_FILE, file_lfn, true);   /* keep the tag in recents */
            stage_patch_from_entry((char*)file_lfn);          /* re-apply patch; truncates to base */
            filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
            if(filesize) break; /* ROM loaded and SNES reset, exit menu loop */
          }
          /* clear file error state from any potential cause of failure
             (prevent LED blinking) */
          file_res = FR_OK;
          /* NACK so autoboot_cmd_handshake returns cleanly to menu */
          snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          cmd=0;
          break;
        case SNES_CMD_DELETE_FILE:
          get_selected_name(file_lfn);
          printf("Delete file: %s\n", file_lfn);
          if(f_unlink((TCHAR*)file_lfn) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          /* the deleted ROM may also be in Recents/Favorites -> drop any now-dead
             entries so they can't hang the loader when opened later. */
          revalidate_game_lists();
          cmd=0;
          break;
        case SNES_CMD_DELETE_SRM: {
          get_selected_name(file_lfn);
          printf("Delete SRM for: %s\n", file_lfn);
          /* Wipe ALL battery-SRAM slots + the .slot sidecar (the browser has no
             slot UI). Slot 0 (legacy <stem>.srm) drives the NACK exactly as before;
             slots 2-4 and the sidecar are best-effort (FR_NO_FILE = nothing there). */
          for(uint8_t s = 0; s < SRM_SLOT_COUNT; s++) {
            uint8_t srmfile[256];
            char ext[8];
            srm_slot_ext(ext, sizeof(ext), s);
            /* path_asset DIRECT -- see the note in delete_listed_game_srm above. Same -1 handling:
               a name that does not fit deletes nothing and NACKs slot 0. */
            if(path_asset((char*)srmfile, sizeof(srmfile), SAVE_BASEDIR, (const char*)file_lfn, ext) < 0) {
              printf("SRM path too long for %s\n", file_lfn);
              if(s == 0) snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
              continue;
            }
            printf("SRM path: %s\n", srmfile);
            FRESULT r = f_unlink((TCHAR*)srmfile);
            if(s == 0 && r != FR_OK) {
              snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
            }
          }
          { uint8_t scfile[256];
            if(path_asset((char*)scfile, sizeof(scfile), SAVE_BASEDIR, (const char*)file_lfn, ".slot") >= 0)
              f_unlink((TCHAR*)scfile);
          }
          cmd=0;
          break;
        }
        case SNES_CMD_DELETE_FILE_FAV:
          delete_listed_game_file(FAVORITES_FILE, "favorite");
          cmd=0;
          break;
        case SNES_CMD_DELETE_SRM_FAV:
          delete_listed_game_srm(FAVORITES_FILE, "favorite");
          cmd=0;
          break;
        case SNES_CMD_DELETE_FILE_RECENT:
          delete_listed_game_file(LAST_FILE, "recent");
          cmd=0;
          break;
        case SNES_CMD_DELETE_SRM_RECENT:
          delete_listed_game_srm(LAST_FILE, "recent");
          cmd=0;
          break;
        case SNES_CMD_LOAD_COVER:
          /* MCU_PARAM was filled by the menu (cover_fill_param_for_current_sel)
             to look exactly like LOADROM's params, so get_selected_name works.
             load_cover is bounded + fail-safe: it never hangs the menu loop. */
          get_selected_name(file_lfn);
          load_cover(file_lfn, SRAM_COVER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_COVER_RECENT:
          /* small (downscaled) cover for the highlighted RECENT game; the menu
             puts the list index in MCU_PARAM (resolved like LOADLAST). Bounded
             + fail-safe like load_cover; reuses the same C9 staging area. */
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          load_cover(file_lfn, SRAM_COVER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_COVER_FAVORITE:
          /* small (downscaled) cover for the highlighted FAVORITE game */
          cfg_get_listed_game(FAVORITES_FILE, file_lfn,
                              listed_game_resolve_index(FAVORITES_FILE, snes_get_mcu_param() & 0xff));
          load_cover(file_lfn, SRAM_COVER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_GAME_INFO:
          /* parse /sd2snes/info/<rom>.yml + stage cover/screenshot for the pre-boot
             info screen. MCU_PARAM was filled like LOADROM (cover_fill_param_for_current_sel)
             so get_selected_name yields the ROM path. Bounded + fail-safe; does NOT
             boot, so no NACK -- the menu polls GAMEINFO status in $FF6000. */
          get_selected_name(file_lfn);
          gameinfo_load(file_lfn);
          /* publish MANUAL_GUIDES/MANUAL_META for this game so the info screen can show the
             "X: Guides" footer and open the viewer before booting. Cached on the path: the
             screen restages on every Up/Down and a full probe is a directory pass. */
          manual_stage_meta_cached(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_GAME_INFO_RECENT:
          /* pre-boot info screen for the recent game at the MCU_PARAM index (resolved
             via LAST_FILE, like LOAD_COVER_RECENT). Non-booting; menu polls $FF6000. */
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          gameinfo_load(file_lfn);
          manual_stage_meta_cached(file_lfn);   /* guides list/meta for the footer + viewer */
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_GAME_INFO_FAVORITE:
          /* pre-boot info screen for the favorite game at the MCU_PARAM index (resolved
             via FAVORITES_FILE, like LOAD_COVER_FAVORITE). Non-booting. */
          cfg_get_listed_game(FAVORITES_FILE, file_lfn,
                              listed_game_resolve_index(FAVORITES_FILE, snes_get_mcu_param() & 0xff));
          gameinfo_load(file_lfn);
          manual_stage_meta_cached(file_lfn);   /* guides list/meta for the footer + viewer */
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_FMV_NEXT:
          /* info-screen FMV pump: stream the next <rom>.fmv frame into the band tile
             bank ($CA0000). Bounded + fail-safe (no-op if no .fmv open); does NOT boot. */
          gameinfo_fmv_next();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_GI_DESC_FULL:
          /* "full description" (Y) on the info screen: re-scan the last-loaded .yml and
             stage the COMPLETE (untruncated) description into $FF7600. Bounded + fail-safe
             (on any error the region stays invalid -> menu keeps the 256-char copy); no boot. */
          gameinfo_desc_full();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_MANUAL_ZPAGE:
          /* manual viewer opened from the PRE-BOOT info screen -- the very same command the
             in-game GUIDES tab uses. Scrollable 2x zoom: stage ONE WHOLE 2x page (<=119KB) into
             PSRAM $C5/$C6. MCU_PARAM: [0] = compacted guide (0..7), [1] = zoom page (== the 1x
             block index), [3] = mode. After this the viewer pans with pure PSRAM->VRAM DMA and
             issues NO further commands until it turns the page, which is exactly why the pan
             cannot stall. Bounded + fail-safe; does NOT boot.
             ACK: unlike the in-game path there is no snes_set_mcu_cmd(0) here -- in the menu the
             ACK is the MCU_CMD clear menu_main_loop() does at the TOP of the next iteration, which
             is what the viewer's bounded "spin until MCU_CMD == 0" waits for. So this case must
             just drop back into the menu loop (cmd = 0). */
          { uint32_t p = snes_get_mcu_param();
            manual_stage_zpage((uint8_t)(p & 0xff),              /* guide (compacted) */
                               (uint16_t)((p >> 8) & 0xffff),    /* block or zoom page */
                               (uint8_t)((p >> 24) & 0xff));     /* mode: bit0 = page */
          }
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_MANUAL_S1PAGE:
          /* same viewer, scale-1 view: stage one whole 1x page so it pans over the page instead
             of jumping band to band. Its PSRAM region is separate from the 2x page, so both stay
             resident and toggling is instant. MCU_PARAM: [0] = guide, [1..2] = page. Bounded +
             fail-safe; does NOT boot. Same ACK contract as MANUAL_ZPAGE above. */
          { uint32_t p = snes_get_mcu_param();
            manual_stage_s1page((uint8_t)(p & 0xff),
                                (uint16_t)((p >> 8) & 0xffff));
          }
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SET_THEME:
          /* a .thm was picked in the browser (any visible folder). MCU_PARAM was
             set up like LOADROM (cwd + selected entry) so get_selected_name
             yields the full SD path; store it and reload the menu so theme_apply
             patches the gfxptr regions of the fresh menu image. */
          get_selected_name(file_lfn);
          theme_select((char*)file_lfn);
          browser_pos_save((char*)file_lfn); /* come back to this .thm, not to the root */
          menu_reload = 1; /* leave loop -> outer loop reloads + themes the menu */
          break;
        case SNES_CMD_CLR_THEME:
          /* revert to the baked-in default look */
          theme_select(NULL);
          browser_pos_save(NULL);
          menu_reload = 1;
          break;
        case SNES_CMD_RESTORE_CLASSIC:
          /* "Restore classic theme": apply the pre-2.16 look, which now ships as a
             regular theme file instead of being the baked default.
             f_stat first: the common update path is "copy firmware + m3nu.bin" without
             refreshing /sd2snes, and writing a skin_name that points at a missing file
             would persist in config.yml while the menu reloaded to no visible effect.
             On miss: leave CFG.skin_name alone, NACK, and stay in the menu loop so the
             SNES pops an error instead of reloading. */
          if(f_stat((const TCHAR*)THEME_CLASSIC, NULL) == FR_OK) {
            theme_select(THEME_CLASSIC);
            browser_pos_save(NULL);
            menu_reload = 1;
          } else {
            printf("theme: %s not found on card\n", THEME_CLASSIC);
            snes_menu_errmsg(MENU_ERR_SUPPLFILE, THEME_CLASSIC);
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
            cmd = 0;
          }
          break;
        case SNES_CMD_SET_MENU_SPC:
          /* a .spc was picked in the browser (any visible folder) to become the menu
             background music. MCU_PARAM was set up like LOADROM (cwd + selected entry)
             so get_selected_name yields the full SD path; store it, enable music, and
             persist, then reload the menu (like SET_THEME). The cold reload re-syncs
             SRAM via cfg_load_to_menu and starts the new BGM cleanly on boot -- the
             only reliable way to (re)start the S-SMP (the in-place warm-reset path
             black-screened: the warm boot leaves NMI off). */
          get_selected_name(file_lfn);
          strncpy((char*)CFG.bgm_name, (char*)file_lfn, sizeof(CFG.bgm_name) - 1);
          CFG.bgm_name[sizeof(CFG.bgm_name) - 1] = 0;
          CFG.enable_menu_music = 1;
          cfg_save();
          browser_pos_save((char*)file_lfn); /* come back to this .spc */
          menu_reload = 1; /* leave loop -> outer loop reloads, boots into the new BGM */
          break;
        case SNES_CMD_CLR_MENU_SPC:
          /* "Restore music": drop the chosen .spc so the BGM falls back to
             /sd2snes/menu.spc, then reload the menu (like CLR_THEME). */
          CFG.bgm_name[0] = 0;
          cfg_save();
          browser_pos_save(NULL);
          menu_reload = 1;
          break;
        case SNES_CMD_LOAD_CHT:
          /* load cheats from YAML file into PSRAM for the menu to edit.
             Filename is provided by the menu via MCU_PARAM (path) plus
             the selected directory entry, the same way the favorites
             and autoboot handlers retrieve it. */
          get_selected_name(file_lfn);
          printf("Load cheats for: %s\n", file_lfn);
          cheat_yaml_load(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SAVE_CHT:
          /* save the (possibly edited) cheat records from PSRAM back
             to the YAML file on the SD card. */
          get_selected_name(file_lfn);
          printf("Save cheats for: %s\n", file_lfn);
          cheat_yaml_save(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_CHT_FAV:
          listed_game_cheats(FAVORITES_FILE, "favorite", 0);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SAVE_CHT_FAV:
          listed_game_cheats(FAVORITES_FILE, "favorite", 1);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_CHT_RECENT:
          listed_game_cheats(LAST_FILE, "recent", 0);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SAVE_CHT_RECENT:
          listed_game_cheats(LAST_FILE, "recent", 1);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_TOGGLE_CHT: {
          /* toggle the enabled flag for the cheat at the index passed
             in MCU_PARAM low two bytes (16-bit index, supports 0..511).
             The MCU does the bit flip directly in the PSRAM cheat
             record at $D00000+512*idx because the SNES menu mapper
             makes that region read-only. */
          uint32_t idx = snes_get_mcu_param() & 0xffff;
          printf("Toggle cheat idx=%lu\n", (unsigned long)idx);
          cheat_toggle_flag((int)idx);
          cmd=0; /* stay in menu loop */
          break;
        }
        case SNES_CMD_RESET_TO_MENU:
          /* USB-triggered menu reload: leave the menu loop so the outer loop
             re-runs load_rom(MENU_FILENAME) and reboots into the fresh menu.bin.
             Lets menu.bin be updated over USB without a physical power-cycle. */
          menu_reload = 1;
          break;
        default:
          printf("unknown cmd: %d\n", cmd);
          cmd=0; /* unknown cmd: stay in loop */
          break;
      }
    }
    if(menu_reload) continue; /* reload menu.bin from SD (outer loop) */
    printf("loaded %lu bytes\n", filesize);
    printf("cmd was %x, going to snes main loop\n", cmd);

    /* clear SNES cmd */
    snes_set_mcu_cmd(0);

    if(romprops.has_msu1) {
      while(!msu1_loop());
      /* An MSU-1 game runs its own loop instead of the one below, so the
         reset-to-menu flag has to be raised HERE too: msu1_loop only ever
         returns 1 for a long reset or the $81 combo, i.e. exactly the two
         cases the normal loop flags. Without it the menu boots with
         ST_RESET_TO_MENU_ACTIVE = 0 and never runs filesel_nav_last, so
         "Reset to menu" Folder/ROM silently degrades to the root folder on
         every MSU-1 title. */
      STM.reset_to_menu_active = (CFG.reset_to_menu >= 2) ? 1 : 0;
      prepare_reset();
      continue;
    }

    cmd=0;
    int loop_ticks = getticks();
    uint8_t usb_cmd = 0;
// uint8_t snes_res;
    while(fpga_test() == FPGA_TEST_TOKEN) {
      cli_entrycheck();
      //usb upload/boot/lock
      usb_cmd |= usbint_handler();
      if (usb_cmd == SNES_CMD_GAMELOOP) usb_cmd = 0;

//        sleep_ms(250);
      sram_reliable();
      /* NES in-game debug snapshot ("NDBG" @ PSRAM 0x400100): PC/regs do
         6502 + contadores da bridge, lidos da config-bus (grupo 0x04) e
         publicados 1x/iteracao.  No-op sem .nes; bounded (ver nes.c). */
      nes_dbg_publish();
      
      // loop if we are in the middle of a reset
      if (usbint_server_reset()) continue;
      
      if(reset_changed) {
        printf("reset\n");
        reset_changed = 0;
// TODO have FPGA automatically reset SRTC on detected reset
        fpga_reset_srtc_state();
      }
      uint8_t resetState = get_snes_reset_state();
      if(resetState == SNES_RESET_LONG) {
        STM.reset_to_menu_active = (CFG.reset_to_menu >= 2) ? 1 : 0;
        prepare_reset();
        break;
      } else {
        if (resetState == SNES_RESET_SHORT) resetButtonState = 1;
        
        if(getticks() > loop_ticks + 25) {
          loop_ticks = getticks();
 //         sram_reliable();
          printf("%s ", get_cic_statename(get_cic_state()));
          cmd=snes_main_loop();
          if (usb_cmd && !cmd) cmd = usb_cmd;
          if(cmd) {
            printf("snes loop cmd=%02x\n", cmd);
            /* in-game shell / overlay commands are served by the shared dispatcher
               (snes.c), which the parallel MSU-1 loop calls too -- one body, so the
               two loops cannot drift. Only loop-specific arms remain below. */
            if(game_cmd_serve(cmd)) usb_cmd = 0;
            else switch(cmd) {
              case SNES_CMD_RESET_LOOP_PASS:
              case SNES_CMD_RESET_LOOP_FAIL:
                usb_cmd = 0;
                snes_reset_loop();
                break;
              case SNES_CMD_RESET:
                usb_cmd = 0;
                // also force full ROM reset if we used button combination
                resetButtonState = 1;
                snes_reset_pulse();
                break;
              case SNES_CMD_RESET_TO_MENU:
                usb_cmd = 0;
                STM.reset_to_menu_active = (CFG.reset_to_menu >= 2) ? 1 : 0;
                prepare_reset();
                goto snes_loop_out;
              case SNES_CMD_COMBO_TRANSITION:
                usb_cmd = 0;
                load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_COMBO | LOADROM_WITH_RESET);
                break;
              default:
                printf("unknown cmd: %02x\n", cmd);
                break;
            }
            snes_set_mcu_cmd(0);
          }
        }
      }
    }
    /* fpga test fail: panic */
    snes_loop_out:
    if(fpga_test() != FPGA_TEST_TOKEN){
      led_panic(LED_PANIC_FPGA_DEAD);
    }
    /* else reset */
  }
}
