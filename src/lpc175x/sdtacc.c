/* SD access time measurement, the System Information screen's only (card overlay code,
   see ovl.h): out of sdnative.c so that it goes to the card with the screen. */
#include <stdio.h>
#include "config.h"
#include "crc.h"
#include "crc16.h"
#include "diskio.h"
#include "spi.h"
#include "timer.h"
#include "uart.h"
#include "led.h"
#include "sdnative.h"
#include "fileops.h"
#include "bits.h"
#include "fpga_spi.h"
#include "memory.h"
#include "snes.h"
#include "usbinterface.h"

extern diskinfo0_t di;
void sdn_checkinit(BYTE drv);      /* sdnative.c */

/* measure sd access time */
void sdn_gettacc(uint32_t *tacc_max, uint32_t *tacc_avg) {
  uint32_t sec1 = 0;
  uint32_t sec2 = 0;
  uint32_t time, time_max = 0;
  uint32_t time_avg = 0LL;
  uint32_t numread = 16384;
  int i;
  int sec_step = di.sectorcount / numread - 1;
  if(disk_state == DISK_REMOVED || usbint_server_busy()) return;
  sdn_checkinit(0);
  for (i=0; i < 128; i++) {
    sd_offload_tgt=2;
    sd_offload=1;
    sdn_read(0, NULL, 0, 1);
    sd_offload_tgt=2;
    sd_offload=1;
    sdn_read(0, NULL, i*sec_step, 1);
  }
  for (i=0; i < numread && (snes_get_mcu_cmd() == SNES_CMD_SYSINFO) && disk_state != DISK_REMOVED && !usbint_server_busy(); i++) {
  /* reset timer */
    LPC_RIT->RICTRL = 0;
    sd_offload_tgt=2;
    sd_offload=1;
    sdn_read(0, NULL, sec1, 2);
    sec1 += 2;
  /* start timer */
    LPC_RIT->RICOUNTER = 0;
    LPC_RIT->RICTRL = BV(RITEN);
    sd_offload_tgt=2;
    sd_offload=1;
    sdn_read(0, NULL, sec2, 1);
  /* read timer */
    time = LPC_RIT->RICOUNTER;
/*    sd_offload_tgt=2;
    sd_offload=1;
    sdn_read(0, NULL, sec2, 15);*/
    time_avg += time/16;
    if(time > time_max) {
      time_max = time;
    }
    sec2 += sec_step;
  }
  time_avg = time_avg / (i+1) * 16;
  sd_offload=0;
  LPC_RIT->RICTRL = 0;
  if(disk_state != DISK_REMOVED && !usbint_server_busy()) {
    *tacc_max = time_max/(CONFIG_CPU_FREQUENCY / 1000000)-114;
    *tacc_avg = time_avg/(CONFIG_CPU_FREQUENCY / 1000000)-114;
  }
}
