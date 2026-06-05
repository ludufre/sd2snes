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
#include "lang.h"
#include "esplink.h"
#include "uart_proto.h"

extern snes_status_t STS;
extern cfg_t CFG;

static uint32_t sd_tacc_max, sd_tacc_avg;

/* Companion (ESP) info snapshot, captured on entry (while the MCU is still free,
   coming from the menu loop). The SD access-time measurement later freezes the
   MCU for ~20s, so we must NOT re-query the live link during the sysinfo screen. */
static uint8_t esp_snap_present;
static char    esp_snap_str[40];

void sysinfo_loop() {
  sd_tacc_max = 0;
  sd_tacc_avg = 0;
  int sd_measured = 0;
  /* snapshot the companion now, before the SD measurement can freeze the MCU */
  esp_snap_present = uart_esp_present();
  strncpy(esp_snap_str, uart_esp_string(), sizeof(esp_snap_str) - 1);
  esp_snap_str[sizeof(esp_snap_str) - 1] = 0;
  echo_mcu_cmd();
  while(snes_get_mcu_cmd() == SNES_CMD_SYSINFO) {
    sd_measured = write_sysinfo(sd_measured);
    delay_ms(100);
    usbint_handler();
#if ESPLINK_ENABLE
    uart_proto_poll();   /* keep servicing the ESP so the companion line stays live */
#endif
  }
  echo_mcu_cmd();
}

int write_sysinfo(int sd_measured) {
  uint32_t sram_addr = SRAM_SYSINFO_ADDR;
  char linebuf[40];
  int len;
  int sd_ok = 0;
  uint8_t *sd_cid;
  uint32_t sd_tacc_max_int = sd_tacc_max / 1000;
  uint32_t sd_tacc_max_frac = sd_tacc_max - (sd_tacc_max_int * 1000);
  uint32_t sd_tacc_avg_int = sd_tacc_avg / 1000;
  uint32_t sd_tacc_avg_frac = sd_tacc_avg - (sd_tacc_avg_int * 1000);
  int32_t sysclk = get_snes_sysclk();
  uint32_t fssize;
  uint32_t fsfree;
  FATFS *ffs = &fatfs;
  status_save_from_menu();

  if (!sd_measured) {
    sram_writeblock((char *)sysinfo_msg[lang_idx()][SI_BUSY_DISK], sram_addr, 40);
  }
  /* remount before sdn_getcid so fatfs registers the disk state change first */
  f_getfree("0:", &fsfree, &ffs);
  sd_cid = sdn_getcid();

  fssize = ((uint64_t)fatfs.n_fatent - 2LL) * (uint64_t)fatfs.csize * 512LL / 1048576LL;
  fsfree = ((uint64_t)fsfree) * (uint64_t)fatfs.csize * 512LL / 1048576LL;

  len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_FW_VERSION], CONFIG_VERSION);
  memset(linebuf+len, 0x20, 40-len);
  sram_writeblock(linebuf, sram_addr, 40);
  sram_addr += 40;
  /* companion (ESP) version (snapshot from entry), or "not detected" */
  {
    const char *cs = esp_snap_present ? esp_snap_str
                                      : sysinfo_msg[lang_idx()][SI_NOT_DETECTED];
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_COMPANION], cs);
  }
  memset(linebuf+len, 0x20, 40-len);
  sram_writeblock(linebuf, sram_addr, 40);
  sram_addr += 40;
  if(disk_state == DISK_REMOVED || usbint_server_busy()) {
    sd_measured = 0;
    sd_tacc_max = 0;
    sd_tacc_avg = 0;
    sram_memset(sram_addr, 40, 0x20);
    sram_addr += 40;
    sram_memset(sram_addr, 40, 0x20);
    sram_addr += 40;
    sram_writestrn((char *)sysinfo_msg[lang_idx()][SI_SD_REMOVED], sram_addr, 40);
    sram_addr += 40;
    sram_memset(sram_addr, 40, 0x20);
    sram_addr += 40;
    sram_memset(sram_addr, 40, 0x20);
    sram_addr += 40;
    sd_ok = 0;
  } else {
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SD_MAKER], sd_cid[1], sd_cid[2], sd_cid[3]);
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SD_PRODUCT], sd_cid[4], sd_cid[5], sd_cid[6], sd_cid[7], sd_cid[8], sd_cid[9]>>4, sd_cid[9]&15);
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SD_SERIAL], sd_cid[10], sd_cid[11], sd_cid[12], sd_cid[13], 2000+((sd_cid[14]&15)<<4)+(sd_cid[15]>>4), sd_cid[15]&15);
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;
    if(sd_tacc_max) {
      len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SD_ACC_TIME], sd_tacc_avg_int, sd_tacc_avg_frac, sd_tacc_max_int, sd_tacc_max_frac);
    } else {
      len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SD_ACC_MEASURING]);
    }
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_CARD_USAGE], fssize-fsfree, fssize);
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;

    static uint8_t sgb_state = SGB_BIOS_CHECK;
    if(sgb_state == SGB_BIOS_CHECK) {
      sgb_state = sgb_bios_state();
    }
    len = snprintf(linebuf, sizeof(linebuf), "sgb%d_boot.bin/sgb%d_snes.bin: %s", CFG.sgb_bios_version, CFG.sgb_bios_version, (
      sgb_state == SGB_BIOS_MISSING ? sgb_state_l[lang_idx()][SGB_W_MISSING]
      : sgb_state == SGB_BIOS_MISMATCH ? sgb_state_l[lang_idx()][SGB_W_MISMATCH]
      : sgb_state == SGB_BIOS_OK ? sgb_state_l[lang_idx()][SGB_W_OK]
      : sgb_state_l[lang_idx()][SGB_W_CHECKING]
    ));
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
    sram_addr += 40;
    sd_ok = 1;
  }
  sram_memset(sram_addr, 40, 0x20);
  sram_addr += 40;
  len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_CIC_STATE], get_cic_statefriendlyname(get_cic_state()));
  memset(linebuf+len, 0x20, 40-len);
  sram_writeblock(linebuf, sram_addr, 40);
  sram_addr += 40;
  if(sysclk == -1)
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SNES_CLK_MEASURING]);
  else
    len = snprintf(linebuf, sizeof(linebuf), sysinfo_msg[lang_idx()][SI_SNES_CLK], get_snes_sysclk());
  memset(linebuf+len, 0x20, 40-len);
  sram_writeblock(linebuf, sram_addr, 40);
  sram_addr += 40;
  if(STS.is_u16) {
    if(STS.u16_cfg & 0x01) {
      len = snprintf(linebuf, sizeof(linebuf), "Ultra16 serial no. %d (Autoboot On)", STS.is_u16);
    } else {
      len = snprintf(linebuf, sizeof(linebuf), "Ultra16 serial no. %d (Autoboot Off)", STS.is_u16);
    }
    memset(linebuf+len, 0x20, 40-len);
    sram_writeblock(linebuf, sram_addr, 40);
  } else {
    sram_memset(sram_addr, 40, 0x20);
  }

  sram_hexdump(SRAM_SYSINFO_ADDR, 13*40);
  if(sysclk != -1 && sd_ok && !sd_measured){
    sdn_gettacc(&sd_tacc_max, &sd_tacc_avg);
    if (sd_tacc_max && sd_tacc_avg) sd_measured = 1;
  }
  return sd_measured;
}
