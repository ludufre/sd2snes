#include <arm/bits.h>
#include <string.h>
#include "config.h"
#include "version.h"
#include "diskio.h"
#include "ff.h"
#include "timer.h"
#include "uart.h"
#include "fileops.h"
#include "memory.h"
#include "snes.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "cic.h"
#include "sdnative.h"
#include "sysinfo.h"
#include "usbinterface.h"
#include "cfg.h"
#include "sgb.h"
#include "msu1.h"   /* menu_sfx_pump/active: menu sound effects via the MSU-1 DAC */

extern snes_status_t STS;
extern cfg_t CFG;

static uint32_t sd_tacc_max, sd_tacc_avg;

static int write_sysinfo(int sd_measured);
static void write_sysinfo_legacy(void);

void sysinfo_loop() {
  uint8_t caps[4];
  int v2;
  sd_tacc_max = 0;
  sd_tacc_avg = 0;
  int sd_measured = 0;
  echo_mcu_cmd();
  /* sysinfo measures raw SD access times, which races a playing effect's
     sd_offload streaming (the DAC then loops stale buffer content - a stuck
     tone for the whole measurement). Cut any effect cleanly on entry; the
     screen has no sounds of its own. */
  menu_sfx_stop();
  /* Which screen is on the other side? A menu that carries the capability marker reads the
     binary sysinfo_blk_t; anything older still expects 13 formatted text lines it cannot get
     any more. Probed once here, not per pass: the menu image cannot change while the screen
     is open. */
  sram_readblock(caps, SRAM_MENU_ADDR + MENU_ADDR_SYSINFO_CAPS, sizeof(caps));
  v2 = (caps[0] == MENU_CAP_MAGIC0 && caps[1] == MENU_CAP_MAGIC1
        && caps[2] >= MENU_CAP_SYSINFO_VER);
  if(!v2) {
    write_sysinfo_legacy();
  }
  while(snes_get_mcu_cmd() == SNES_CMD_SYSINFO) {
    if(v2) {
      sd_measured = write_sysinfo(sd_measured);
    }
    delay_ms(100);
    usbint_handler();
  }
  echo_mcu_cmd();
}

/* Firmware newer than the menu: the old screen renders 13 fixed 40-char lines straight out of
   this region, so hand it one English line asking for a menu update and blank the rest. Called
   once - the screen is static from then on, but the 100 ms loop keeps running so USB and the
   command handshake stay alive. */
static void write_sysinfo_legacy(void) {
  static const char msg[] = "Update menu (m3nu.bin)";
  char linebuf[40];
  memset(linebuf, 0x20, sizeof(linebuf));
  memcpy(linebuf, msg, sizeof(msg) - 1);
  sram_writeblock(linebuf, SRAM_SYSINFO_ADDR, sizeof(linebuf));
  sram_memset(SRAM_SYSINFO_ADDR + sizeof(linebuf), 12 * sizeof(linebuf), 0x20);
}

/* Copy a NUL-terminated string into one of the block's fixed-size text fields. The bound is a
   runtime argument, so the terminator is always written by hand: the firmware's snprintf
   returns the length it WOULD have written (not the C99-clamped one), which makes it useless
   for building bounded strings. */
static void si_setstr(char *dst, size_t cap, const char *src) {
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

/* sgb.h orders its enum CHECK/OK/MISMATCH/MISSING; the block orders by severity so the menu can
   index a word table with it. Neither side may assume the other's order. */
static uint8_t si_sgb_canon(uint8_t state) {
  switch(state) {
    case SGB_BIOS_MISSING:  return SYSINFO_SGB_MISSING;
    case SGB_BIOS_MISMATCH: return SYSINFO_SGB_MISMATCH;
    case SGB_BIOS_OK:       return SYSINFO_SGB_OK;
    default:                return SYSINFO_SGB_CHECKING;
  }
}

static int write_sysinfo(int sd_measured) {
  sysinfo_blk_t si;
  int sd_ok = 0;
  uint8_t *sd_cid;
  uint32_t tacc_int;
  int32_t sysclk = get_snes_sysclk();
  uint32_t fssize;
  uint32_t fsfree;
  FATFS *ffs = &fatfs;
  /* Result of the SGB BIOS probe, cached for the whole session. The probe opens files on the
     card, so it may only run on the card-present path below -- probing with the card out would
     latch MISSING for good. */
  static uint8_t sgb_state = SGB_BIOS_CHECK;
  status_save_from_menu();

  memset(&si, 0, sizeof(si));
  si.magic[0] = SYSINFO_MAGIC0;
  si.magic[1] = SYSINFO_MAGIC1;
  si.version = SYSINFO_VERSION;
  si_setstr(si.fw_str, sizeof(si.fw_str), CONFIG_VERSION);
  /* hardware model: DEVICE_NAME, compile-time -- the same string the USB INFO reports */
  si_setstr(si.model_str, sizeof(si.model_str), DEVICE_NAME);
  si.sgb_ver = CFG.sgb_bios_version;
  si.sgb_state = si_sgb_canon(sgb_state);
  si.cic_state = get_cic_state();
  si_setstr(si.cic_str, sizeof(si.cic_str), get_cic_statefriendlyname(si.cic_state));
  if(sysclk == -1) {
    si.flags |= SYSINFO_FLAG_CLK_MEASURING;
  } else {
    si.snes_clk_hz = (uint32_t)sysclk;
  }
  if(STS.is_u16) {
    si.flags |= SYSINFO_FLAG_U16;
    si.u16_serial = STS.is_u16;
    if(STS.u16_cfg & 0x01) {
      si.flags |= SYSINFO_FLAG_U16_AUTOBOOT;
    }
  }

  if (!sd_measured) {
    /* Publish everything that does not depend on the card BEFORE the slow f_getfree, so the
       screen shows live values while the free space is being counted instead of sitting on the
       previous pass' block. Idempotent: the magic is already in place, so the menu never sees a
       header-less block. Both "not valid yet" flags go out with it -- every card field is still
       zero at this point, and zeros here read as real data (0MB card, serial 00000000), not as
       an obvious placeholder. Cleared again straight afterwards so the final block only carries
       whichever of them the real data still justifies. */
    si.flags |= SYSINFO_FLAG_FS_BUSY | SYSINFO_FLAG_ACC_MEASURING;
    sram_writeblock(&si, SRAM_SYSINFO_ADDR, sizeof(si));
    si.flags &= (uint8_t)~(SYSINFO_FLAG_FS_BUSY | SYSINFO_FLAG_ACC_MEASURING);
  }
  /* remount before sdn_getcid so fatfs registers the disk state change first */
  f_getfree("0:", &fsfree, &ffs);
  sd_cid = sdn_getcid();

  fssize = ((uint64_t)fatfs.n_fatent - 2LL) * (uint64_t)fatfs.csize * 512LL / 1048576LL;
  fsfree = ((uint64_t)fsfree) * (uint64_t)fatfs.csize * 512LL / 1048576LL;

  if(disk_state == DISK_REMOVED || usbint_server_busy()) {
    /* every card-derived field stays zero from the memset; the flag tells the menu to blank
       those lines rather than print zeros */
    si.flags |= SYSINFO_FLAG_SD_GONE;
    sd_measured = 0;
    sd_tacc_max = 0;
    sd_tacc_avg = 0;
    sd_ok = 0;
  } else {
    si.sd_maker = sd_cid[1];
    si.sd_oem[0] = sd_cid[2];
    si.sd_oem[1] = sd_cid[3];
    si.sd_product[0] = sd_cid[4];
    si.sd_product[1] = sd_cid[5];
    si.sd_product[2] = sd_cid[6];
    si.sd_product[3] = sd_cid[7];
    si.sd_product[4] = sd_cid[8];
    si.sd_rev_maj = sd_cid[9] >> 4;
    si.sd_rev_min = sd_cid[9] & 15;
    si.sd_serial[0] = sd_cid[10];
    si.sd_serial[1] = sd_cid[11];
    si.sd_serial[2] = sd_cid[12];
    si.sd_serial[3] = sd_cid[13];
    si.sd_mfd_year = 2000 + ((sd_cid[14] & 15) << 4) + (sd_cid[15] >> 4);
    si.sd_mfd_month = sd_cid[15] & 15;
    if(sd_tacc_max) {
      tacc_int = sd_tacc_avg / 1000;
      si.tacc_avg_int = tacc_int;
      si.tacc_avg_frac = sd_tacc_avg - (tacc_int * 1000);
      tacc_int = sd_tacc_max / 1000;
      si.tacc_max_int = tacc_int;
      si.tacc_max_frac = sd_tacc_max - (tacc_int * 1000);
    } else {
      si.flags |= SYSINFO_FLAG_ACC_MEASURING;
    }
    si.card_total_mb = fssize;
    si.card_used_mb = fssize - fsfree;
    if(sgb_state == SGB_BIOS_CHECK) {
      sgb_state = sgb_bios_state();
    }
    si.sgb_state = si_sgb_canon(sgb_state);
    sd_ok = 1;
  }
  sram_writeblock(&si, SRAM_SYSINFO_ADDR, sizeof(si));

  sram_hexdump(SRAM_SYSINFO_ADDR, 13*40);
  if(sysclk != -1 && sd_ok && !sd_measured){
    sdn_gettacc(&sd_tacc_max, &sd_tacc_avg);
    if (sd_tacc_max && sd_tacc_avg) sd_measured = 1;
  }
  return sd_measured;
}
