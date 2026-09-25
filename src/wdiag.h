/* sd2snes - SD card based universal cartridge for the SNES
   uC firmware portion

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   wdiag.h: "wedge" diagnostic build (GBC_WEDGE_DIAG).

   A Mk.III-only instrument for finding WHERE the in-game loop stops.  It is
   compiled in only by a private diagnostic config (a copy of config-mk3 with
   `GBC_WEDGE_DIAG = y`); without the define every hook below expands to
   nothing and the firmware is byte-identical.

   With the define:
   - every FPGA_MCU_RDY wait (FPGA_WAIT_RDY and the *_INLINE form) is bounded
     (WD_RDY_LIMIT_US) and, on timeout, counted against the current "site";
   - the in-game loop tags each step with a site number and bumps a heartbeat;
   - the UART deadlockable spin is bounded and counted;
   - the USB INFO reply carries the counters in bytes 388..511;
   - SysTick watches the heartbeat: a stalled in-game loop lights a LED code
     after WD_STALL_LED_TICKS and, after WD_STALL_RESET_TICKS, records the
     interrupted PC/LR in AHB RAM (not cleared by reset) and resets the MCU;
     a HardFault does the same.  The next boot reports that record in INFO.
*/

#ifndef WDIAG_H
#define WDIAG_H

#include <stdint.h>
#include "config.h"

#ifdef GBC_WEDGE_DIAG

#if defined(CONFIG_MK2) || !defined(CONFIG_MK3) || defined(CONFIG_MK3_STM32)
#error "GBC_WEDGE_DIAG is an LPC1756 (Mk.III) instrument only"
#endif

/* site numbers -- keep in step with the table in usb_info.py --diag */
#define WD_SITE_NONE        0
#define WD_SITE_LOOP_TEST   1   /* fpga_test() in the in-game while() condition */
#define WD_SITE_USBINT      2   /* usbint_handler() (INFO, BOOT, RESET ...) */
#define WD_SITE_SRAM_REL    3   /* sram_reliable(): 4 x sram_readlong(0xFFFF00) */
#define WD_SITE_NES_DBG     4   /* nes_dbg_publish() */
#define WD_SITE_RESET       5   /* reset_changed/SRTC + get_snes_reset_state() */
#define WD_SITE_SNES_LOOP   6   /* snes_main_loop() body: gtc/sufami/spc7110/SRAM CRC/BS */
#define WD_SITE_GET_CMD     7   /* snes_get_mcu_cmd(): snescmd $2A00 read */
#define WD_SITE_CMD_SERVE   8   /* game_cmd_serve()/switch arm for a nonzero cmd */
#define WD_SITE_ACK_CMD     9   /* snes_set_mcu_cmd(0) after a cmd */
#define WD_SITE_USB_LOCK   10   /* usbint_handler_cmd lock spin (GET/PUT data phase) */
#define WD_SITE_USB_SEND   11   /* usbint_send_block spin (bulk IN occupied) */
#define WD_SITE_UART       12   /* uart_putc full-buffer spin (counter only) */
#define WD_SITE_CIC_PRINT  13   /* get_cic_statename printf, 1x per 25 ticks */
#define WD_SITE_OUTSIDE    14   /* menu / load / anything outside the in-game loop */
#define WD_SITE_IRQ        15   /* an MCU_RDY wait taken from interrupt context */
#define WD_NSITES          16

/* bounds */
#define WD_RDY_LIMIT_US        20000UL  /* one MCU_RDY wait; legit worst case is ~1 us */
#define WD_UART_LIMIT_US       20000UL  /* 256 B drain at 921600 baud is ~2.8 ms */
#define WD_USB_SPIN_TICKS        200    /* a USB spin longer than 2 s is counted */
#define WD_STALL_LED_TICKS       200    /* heartbeat stale 2 s  -> LED code */
#define WD_STALL_RESET_TICKS    1500    /* heartbeat stale 15 s -> record + reset */
#define WD_USBOFF_RESET_TICKS    500    /* USB IRQ masked 5 s in-game -> record + reset */

/* persisted record kinds */
#define WD_KIND_NONE      0
#define WD_KIND_STALL     1
#define WD_KIND_HARDFAULT 2
#define WD_KIND_USBOFF    3

extern volatile uint8_t wd_site;

#define WD_SITE(n)         do { wd_site = (n); } while (0)
#define WD_SITE_SAVE(v)    uint8_t v = wd_site
#define WD_SITE_RESTORE(v) do { wd_site = (v); } while (0)

void wd_init(void);
void wd_loop_enter(void);
void wd_loop_exit(void);
void wd_heartbeat(void);
void wd_cmd(uint8_t cmd);
void wd_set_addr(uint32_t addr);
void wd_rdy_timeout(uint32_t cycles);
void wd_rdy_waited(uint32_t cycles);
void wd_sentinel_bad(uint32_t val);
void wd_fpga_test_bad(uint8_t val);
void wd_spi_select(void);
void wd_usb_spin(uint8_t site, uint32_t ticks);
void wd_uart_timeout(uint8_t thre);
void wd_info_fill(volatile uint8_t *resp);
uint32_t wd_cycles(void);

#define WD_CYC_PER_US (CONFIG_CPU_FREQUENCY / 1000000UL)

#else  /* !GBC_WEDGE_DIAG: every hook is a no-op */

#define WD_SITE(n)         do { } while (0)
#define WD_SITE_SAVE(v)    do { } while (0)
#define WD_SITE_RESTORE(v) do { } while (0)

#endif /* GBC_WEDGE_DIAG */

#endif /* WDIAG_H */
