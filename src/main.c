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
#include "cheat.h"

//usb
#include "usb.h"
#include "usbhw.h"
#include "cdcuser.h"
#include "usbinterface.h"

int i;

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

const char fwhdr[CONFIG_FW_HEADERSIZE] __attribute__ ((section(".fwhdr")));

void menu_cmd_readdir(void) {
  uint8_t path[256];
  SNES_FTYPE filetypes[16];
  snes_get_filepath(path, 256);
  snescmd_readstrn(filetypes, SNESCMD_MCU_PARAM + 8, sizeof(filetypes));
  uint32_t tgt_addr = snescmd_readlong(SNESCMD_MCU_PARAM + 4) & 0xffffff;
printf("path=%s tgt=%06lx types=", path, tgt_addr);
uart_puts_hex((char*)filetypes);
uart_putc('\n');
  scan_dir(path, tgt_addr, filetypes);
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
    led_set_brightness(CFG.led_brightness);

    /* load menu */
    sram_writelong(0x12345678, SRAM_SCRATCHPAD);
    fpga_dspx_reset(1);
    uart_putc('(');
    load_rom((uint8_t*)MENU_FILENAME, SRAM_MENU_ADDR, 0);
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
      switch(cmd) {
        case SNES_CMD_LOADROM:
          /* Read the IPS patch index BEFORE get_selected_name so that the
             MCU_PARAM+7 byte is not overwritten.  set_mcu_addr() only uses
             the lower 24 bits, so the index byte at offset +7 is safe. */
          ips_pending_index = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
          get_selected_name(file_lfn);
          printf("Selected name: %s (patch idx=%d)\n", file_lfn, ips_pending_index);
          cfg_add_listed_game(LAST_FILE, file_lfn, true);
          /* Build the SRM-override path from the IPS file's full SD path. */
          current_ips_srm_source[0] = '\0';
          if(ips_pending_index > 0 && ips_pending_index <= IPS_MAX_PATCHES) {
            sram_readstrn(current_ips_srm_source,
                          SRAM_IPS_LIST_ADDR + 512
                          + (uint32_t)(ips_pending_index - 1) * IPS_PATH_LEN,
                          sizeof(current_ips_srm_source));
            printf("Patch SRM source: %s\n", current_ips_srm_source);
          }
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
          break;
        case SNES_CMD_QUERY_IPS_PATCHES: {
          uint8_t qpath[256];
          get_selected_name(qpath);
          current_ips_srm_source[0] = '\0';
          ips_find_patches(qpath, SRAM_IPS_LIST_ADDR);
          cmd = 0; /* stay in menu loop */
          break;
        }
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
          /* stage background menu music from a fixed path (no browser selection).
             load_spc is graceful: a missing/too-small file zeroes the SPC header,
             which the menu detects and skips. */
          filesize = load_spc((uint8_t*)"/sd2snes/menu.spc", SRAM_SPC_DATA_ADDR, SRAM_SPC_HEADER_ADDR);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_RESET:
          /* process RESET request from SNES */
          printf("RESET requested by SNES\n");
          snes_reset_pulse();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOADLAST:
          ips_pending_index = 0;
          current_ips_srm_source[0] = '\0';
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name: %s\n", file_lfn);
          cfg_add_listed_game(LAST_FILE, file_lfn, true);
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
          break;
        case SNES_CMD_LOADFAVORITE:
          ips_pending_index = 0;
          current_ips_srm_source[0] = '\0';
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name: %s\n", file_lfn);
          cfg_add_listed_game(LAST_FILE, file_lfn, true);
          filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
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
          cfg_add_listed_game(FAVORITES_FILE, file_lfn, false);
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_ADD_FAVORITE_RECENT:
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Selected name from recent: %s\n", file_lfn);
          cfg_add_listed_game(FAVORITES_FILE, file_lfn, true);
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
          cfg_remove_listed_game(FAVORITES_FILE, snes_get_mcu_param() & 0xff);
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
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Set autoboot from favorite: %s\n", file_lfn);
          cfg_set_autoboot_rom(file_lfn);
          STM.autoboot_enabled = 1;
          status_load_to_menu();
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SET_AUTOBOOT_RECENT:
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
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
            cfg_add_listed_game(LAST_FILE, file_lfn, true);
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
          cmd=0;
          break;
        case SNES_CMD_DELETE_SRM: {
          uint8_t srmfile[256] = SAVE_BASEDIR;
          get_selected_name(file_lfn);
          printf("Delete SRM for: %s\n", file_lfn);
          append_file_basename((char*)srmfile, (char*)file_lfn, ".srm", sizeof(srmfile));
          printf("SRM path: %s\n", srmfile);
          if(f_unlink((TCHAR*)srmfile) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          cmd=0;
          break;
        }
        case SNES_CMD_DELETE_FILE_FAV: {
          /* delete the ROM behind a favorites entry (index in MCU_PARAM),
             then drop the now-dangling list entry so it disappears from
             the favorites window the same way REMOVE_FAVORITE_ROM does. */
          uint8_t idx = snes_get_mcu_param() & 0xff;
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, idx);
          printf("Delete favorite file: %s\n", file_lfn);
          if(f_unlink((TCHAR*)file_lfn) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          cfg_remove_listed_game(FAVORITES_FILE, idx);
          STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
          status_load_to_menu();
          cmd=0;
          break;
        }
        case SNES_CMD_DELETE_SRM_FAV: {
          /* delete only the .srm for a favorites entry; the ROM (and the
             favorites entry itself) stay in place. */
          uint8_t srmfile[256] = SAVE_BASEDIR;
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Delete SRM for favorite: %s\n", file_lfn);
          append_file_basename((char*)srmfile, (char*)file_lfn, ".srm", sizeof(srmfile));
          printf("SRM path: %s\n", srmfile);
          if(f_unlink((TCHAR*)srmfile) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          cmd=0;
          break;
        }
        case SNES_CMD_DELETE_FILE_RECENT: {
          /* delete the ROM behind a recents entry (index in MCU_PARAM),
             then drop the list entry like REMOVE_RECENT_ROM. */
          uint8_t idx = snes_get_mcu_param() & 0xff;
          cfg_get_listed_game(LAST_FILE, file_lfn, idx);
          printf("Delete recent file: %s\n", file_lfn);
          if(f_unlink((TCHAR*)file_lfn) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          cfg_remove_listed_game(LAST_FILE, idx);
          STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
          status_load_to_menu();
          cmd=0;
          break;
        }
        case SNES_CMD_DELETE_SRM_RECENT: {
          /* delete only the .srm for a recents entry; the ROM (and the
             recents entry itself) stay in place. */
          uint8_t srmfile[256] = SAVE_BASEDIR;
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Delete SRM for recent: %s\n", file_lfn);
          append_file_basename((char*)srmfile, (char*)file_lfn, ".srm", sizeof(srmfile));
          printf("SRM path: %s\n", srmfile);
          if(f_unlink((TCHAR*)srmfile) != FR_OK) {
            snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
          }
          cmd=0;
          break;
        }
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
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          load_cover(file_lfn, SRAM_COVER_ADDR);
          cmd=0; /* stay in menu loop */
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
          /* load cheats for a favorite-list entry. MCU_PARAM low byte
             holds the favorite index; we resolve the path the same
             way LOADFAVORITE / SET_AUTOBOOT_FAV do, then reuse the
             standard cheat_yaml_load flow. */
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Load cheats for favorite: %s\n", file_lfn);
          cheat_yaml_load(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SAVE_CHT_FAV:
          /* save cheats for a favorite-list entry. Same lookup as
             LOAD_CHT_FAV; the SNES side rewrites MCU_PARAM with the
             favorite index before sending this command, because the
             toggle handler clobbers it during normal menu use. */
          cfg_get_listed_game(FAVORITES_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Save cheats for favorite: %s\n", file_lfn);
          cheat_yaml_save(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_LOAD_CHT_RECENT:
          /* load cheats for a recent-list entry. MCU_PARAM low byte holds
             the recent index; resolve the path via LAST_FILE like LOADLAST,
             then reuse the standard cheat_yaml_load flow. */
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Load cheats for recent: %s\n", file_lfn);
          cheat_yaml_load(file_lfn);
          cmd=0; /* stay in menu loop */
          break;
        case SNES_CMD_SAVE_CHT_RECENT:
          /* save cheats for a recent-list entry. Same lookup as
             LOAD_CHT_RECENT; the SNES side rewrites MCU_PARAM with the
             recent index before sending this command. */
          cfg_get_listed_game(LAST_FILE, file_lfn, snes_get_mcu_param() & 0xff);
          printf("Save cheats for recent: %s\n", file_lfn);
          cheat_yaml_save(file_lfn);
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
            switch(cmd) {
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
              case SNES_CMD_SAVESTATE:
                usb_cmd = 0;
                save_backup_state();
                break;
              case SNES_CMD_LOADSTATE:
                usb_cmd = 0;
                load_backup_state();
                break;
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
