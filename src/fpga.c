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

   fpga.c: FPGA (re)configuration
*/


#include "bits.h"

#include "fpga.h"
#include "fpga_spi.h"
#include "config.h"
#include "uart.h"
#include "diskio.h"
#include "integer.h"
#include "ff.h"
#include "fileops.h"
#include "spi.h"
#include "led.h"
#include "timer.h"
#include "rle.h"
#ifndef CONFIG_MK2
/* mk2 loads the boot bitstream from SD (see fpga_rompgm), so it does NOT bake the
   ~21 KB cfgware blob into the firmware. mk3/mk3-stm32 keep it embedded. */
#include "cfgware.h"
#endif

uint8_t SPI_OFFLOAD;

const uint8_t *fpga_config;
/* Boot-display failure signal for the LEDs (read by led_error on SysTick):
   0 = ok, 1 = no SD card, 2 = fpga_mini missing/unreadable. */
uint8_t fpga_boot_led = 0;
void fpga_set_prog_b(uint8_t val) {
  OUT_BIT(FPGA_PROGBREG, FPGA_PROGBBIT, val);
}

void fpga_set_cclk(uint8_t val) {
  OUT_BIT(FPGA_CCLKREG, FPGA_CCLKBIT, val);
}

int fpga_get_initb() {
  return BITBAND(FPGA_INITBREG->GPIO_I, FPGA_INITBBIT);
}

void fpga_init() {
/* mainly GPIO directions */
  GPIO_MODE_OUT(FPGA_CCLKREG, FPGA_CCLKBIT);   /* CCLK */
  GPIO_MODE_IN(FPGA_DONEREG, FPGA_DONEBIT);    /* DONE */
  GPIO_MODE_OUT(FPGA_PROGBREG, FPGA_PROGBBIT); /* PROG_B */
  GPIO_MODE_OUT(FPGA_DINREG, FPGA_DINBIT);     /* DIN */
  GPIO_MODE_IN(FPGA_INITBREG, FPGA_INITBBIT);  /* INIT_B */

/* pullup inputs */
  GPIO_PULLUP(FPGA_DONEREG, FPGA_DONEBIT);
  GPIO_PULLUP(FPGA_INITBREG, FPGA_INITBBIT);

  SPI_OFFLOAD=0;
  fpga_set_cclk(0);    /* initial clk=0 */
}

int fpga_get_done(void) {
  return BITBAND(FPGA_DONEREG->GPIO_I, FPGA_DONEBIT);
}

void fpga_postinit() {
  GPIO_MODE_IN(FPGA_DINREG, FPGA_DINBIT); /* DATA0 -> MCU_RDY */
}

void fpga_pgm(uint8_t* filename) {
  int MAXRETRIES = 10;
  int retries = MAXRETRIES;
  uint8_t data;
  int i;
  tick_t timeout;
  /* open configware file */
  file_open(filename, FA_READ);
  if(file_res) {
    uart_putc('?');
    uart_putc(0x30+file_res);
    return;
  }
  fpga_init();
  do {
    printf("fpga_pgm: configuring FPGA, attempts left: %d\n", retries);
    i=0;
    timeout = getticks() + 1;
    fpga_set_prog_b(0);
    while(BITBAND(FPGA_PROGBREG->GPIO_I, FPGA_PROGBBIT)) {
      if(getticks() > timeout) {
        printf("fpga_pgm: PROGB is stuck high!\n");
        led_panic(LED_PANIC_FPGA_PROGB_STUCK);
      }
    }
    timeout = getticks() + 100;
    uart_putc('P');
    fpga_set_prog_b(1);
    while(!fpga_get_initb()){
      if(getticks() > timeout) {
        printf("fpga_pgm: no response from FPGA trying to initiate configuration!\n");
        led_panic(LED_PANIC_FPGA_NO_INITB);
      }
    };
    timeout = getticks() + 100;
    while(fpga_get_done()) {
      if(getticks() > timeout) {
        printf("fpga_pgm: DONE is stuck high!\n");
        led_panic(LED_PANIC_FPGA_DONE_STUCK);
      }
    }
    uart_putc('p');

    uart_putc('C');
    FPGA_DIN_MASK();
    for (;;) {
      data = rle_file_getc();
      i++;
      if (file_status || file_res) break;   /* error or eof */
      FPGA_SEND_BYTE_SERIAL(data);
    }
    FPGA_DIN_UNMASK();
    uart_putc('c');
    file_close();
    printf("fpga_pgm: %d bytes programmed\n", i);
    timeout = getticks() + 100;
    while(!fpga_get_done()) {
      if(getticks() > timeout) {
        printf("fpga_pgm: no DONE from FPGA! Retrying\n");
        break;
      }
    }
    CCLK(); CCLK(); CCLK();
    delay_ms(1);
  } while (!fpga_get_done() && retries--);
  if(!fpga_get_done()) {
    printf("fpga_pgm: FPGA failed to configure after %d tries.\n", MAXRETRIES);
    led_panic(LED_PANIC_FPGA_NOCONF);
  }
  
  printf("FPGA configured\n");
  fpga_config = filename;
  fpga_boot_led = 0; /* any successful config clears the boot-failure LED signal */
  fpga_postinit();
}

void fpga_rompgm() {
#ifdef CONFIG_MK2
  /* mk2: the boot-display config "fpga_mini" lives on the SD card (/sd2snes/
     fpga_mini.bit) instead of being baked into the 128 KB firmware, reclaiming
     ~21 KB.  fpga_mini is ONLY used to render boot-diagnostic screens (No SD /
     menu-load error / fw-update progress) via the lazy bootstrap in snes.c; a
     normal boot loads fpga_base from SD directly (main.c) and never calls this.
     fpga_pgm() already does the full PROG_B/INIT_B/DONE handshake + retries and
     returns cleanly if the file is absent (no hang, no led_panic on open-fail).
     Trade-off: with a missing/unreadable SD there is no config to draw the
     "insert card" message (blank screen) -- but USB is up and the bootldr can
     still reflash from SD, so this is a recoverable no-boot, not a brick. */
  /* Probe why the boot bitstream might be unavailable, for the LED signal:
     no card (FR_NOT_READY) vs fpga_mini file missing/unreadable (other error). */
  file_open((uint8_t*)FPGA_MINI, FA_READ);
  if(file_res == FR_NOT_READY)   fpga_boot_led = 1;       /* no SD card */
  else if(file_res != FR_OK)     fpga_boot_led = 2;       /* fpga_mini missing/unreadable */
  else { fpga_boot_led = 0; file_close(); }               /* present (fpga_pgm reopens it) */
  fpga_pgm((uint8_t*)FPGA_MINI);
  fpga_config = FPGA_ROM; /* keep the boot-display marker (main.c FPGA_ROM check) */
#else
  int MAXRETRIES = 10;
  int retries = MAXRETRIES;
  uint8_t data;
  int i;
  tick_t timeout;
  fpga_init();
  do {
    i=0;
    timeout = getticks() + 100;
    fpga_set_prog_b(0);
    uart_putc('P');
    fpga_set_prog_b(1);
    while(!fpga_get_initb()){
      if(getticks() > timeout) {
        printf("no response from FPGA trying to initiate configuration!\n");
        led_panic(LED_PANIC_FPGA_NO_INITB);
      }
    };
    timeout = getticks() + 100;
    while(fpga_get_done()) {
      if(getticks() > timeout) {
        printf("DONE is stuck high!\n");
        led_panic(LED_PANIC_FPGA_DONE_STUCK);
      }
    }
    uart_putc('p');

    /* open configware file */
    rle_mem_init(cfgware, sizeof(cfgware));
    printf("sizeof(cfgware) = %d\n", sizeof(cfgware));
    FPGA_DIN_MASK();
    for (;;) {
      data = rle_mem_getc();
      if(rle_state) break;
      i++;
      FPGA_SEND_BYTE_SERIAL(data);
    }
    FPGA_DIN_UNMASK();
    uart_putc('c');
    printf("fpga_rompgm: %d bytes programmed\n", i);
    delay_ms(1);
  } while (!fpga_get_done() && retries--);
  if(!fpga_get_done()) {
    printf("FPGA failed to configure after %d tries.\n", MAXRETRIES);
    led_panic(LED_PANIC_FPGA_NOCONF);
  }
  printf("FPGA configured\n");
  fpga_config = FPGA_ROM;
  fpga_postinit();
#endif
}

