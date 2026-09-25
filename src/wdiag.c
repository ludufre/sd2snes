/* sd2snes - SD card based universal cartridge for the SNES
   uC firmware portion

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   wdiag.c: "wedge" diagnostic build (GBC_WEDGE_DIAG) -- see wdiag.h.
   Empty unless the private diagnostic config defines GBC_WEDGE_DIAG.
*/

#include "config.h"
#include "wdiag.h"

#ifdef GBC_WEDGE_DIAG

#include <string.h>
#include "bits.h"
#include "timer.h"
#include "led.h"

#define WD_MAGIC 0x31474457UL   /* "WDG1" */

/* One record that survives NVIC_SystemReset: .ahbram is NOLOAD, so the next
   boot finds it untouched unless the bootloader reused the memory -- magic +
   checksum make that case read as "no record" instead of garbage. */
typedef struct {
  uint32_t magic;
  uint8_t  kind, site, last_cmd, last_to_site;
  uint32_t pc, lr, info1, info2;
  uint16_t test_bad;
  uint8_t  test_bad_val, flags;
  uint32_t check;
} wd_rec_t;

typedef struct {
  uint32_t heartbeat;
  uint32_t to_total;
  uint32_t to_addr;      /* PSRAM address of the last set_mcu_addr before the last timeout */
  uint32_t to_ticks;
  uint32_t last_addr;
  uint32_t sent_bad_val; /* last value sram_reliable() read instead of 0x12345678 */
  uint32_t hb_seen, hb_ticks;
  uint16_t to_cnt[WD_NSITES];
  uint16_t max_us[WD_NSITES];
  uint16_t cmd_count, sent_bad, usboff;
  uint8_t  ingame, last_to_site, last_cmd, uart_thre, led_on;
  wd_rec_t prev;
} wd_state_t;

static wd_state_t wd IN_AHBRAM;
static wd_rec_t wd_persist IN_AHBRAM;
volatile uint8_t wd_site IN_AHBRAM;

extern uint32_t __stack;

static uint32_t wd_rec_sum(const wd_rec_t *r) {
  const uint32_t *p = (const uint32_t *)r;
  uint32_t s = 0x5A5A5A5AUL;
  for(unsigned i = 0; i < (sizeof(wd_rec_t) / 4) - 1; i++) s = (s << 5 | s >> 27) ^ p[i];
  return s;
}

uint32_t wd_cycles(void) {
  return DWT->CYCCNT;
}

void wd_init(void) {
  wd_rec_t r = wd_persist;
  memset(&wd, 0, sizeof(wd));
  if(r.magic == WD_MAGIC && r.check == wd_rec_sum(&r)) wd.prev = r;
  memset(&wd_persist, 0, sizeof(wd_persist));
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  wd_site = WD_SITE_OUTSIDE;
}

void wd_loop_enter(void) {
  wd.hb_seen = wd.heartbeat;
  wd.hb_ticks = getticks();
  wd.ingame = 1;
}

void wd_loop_exit(void) {
  wd.ingame = 0;
  wd_site = WD_SITE_OUTSIDE;
  if(wd.led_on) {
    wd.led_on = 0;
    writeled(0);
    readled(0);
  }
}

void wd_heartbeat(void) {
  wd.heartbeat++;
}

void wd_cmd(uint8_t cmd) {
  wd.last_cmd = cmd;
  if(wd.cmd_count != 0xffff) wd.cmd_count++;
}

void wd_set_addr(uint32_t addr) {
  wd.last_addr = addr;
}

static inline uint8_t wd_cur_site(void) {
  return __get_IPSR() ? WD_SITE_IRQ : (wd_site & (WD_NSITES - 1));
}

void wd_rdy_waited(uint32_t cycles) {
  uint8_t s = wd_cur_site();
  uint32_t us = cycles / WD_CYC_PER_US;
  if(us > 0xffff) us = 0xffff;
  if(us > wd.max_us[s]) wd.max_us[s] = us;
}

void wd_rdy_timeout(uint32_t cycles) {
  uint8_t s = wd_cur_site();
  if(wd.to_cnt[s] != 0xffff) wd.to_cnt[s]++;
  wd.to_total++;
  wd.last_to_site = s;
  wd.to_addr = wd.last_addr;
  wd.to_ticks = getticks();
  wd_rdy_waited(cycles);
}

void wd_sentinel_bad(uint32_t val) {
  if(wd.sent_bad != 0xffff) wd.sent_bad++;
  wd.sent_bad_val = val;
}

/* fpga_test() carries no MCU_RDY wait, so slot 1 of the tables is free to
   count bad test tokens (max_us[1] keeps the last bad token). */
void wd_fpga_test_bad(uint8_t val) {
  if(wd.to_cnt[WD_SITE_LOOP_TEST] != 0xffff) wd.to_cnt[WD_SITE_LOOP_TEST]++;
  wd.max_us[WD_SITE_LOOP_TEST] = val;
}

/* Slot 0 counts FPGA selects issued from interrupt context while the FPGA
   chip select was already low: an IRQ-side transaction cutting into one the
   main thread had in flight. */
void wd_spi_select(void) {
  if(__get_IPSR() && !BITBAND(FPGA_SSREG->FIOPIN, FPGA_SSBIT)) {
    if(wd.to_cnt[WD_SITE_NONE] != 0xffff) wd.to_cnt[WD_SITE_NONE]++;
  }
}

/* USB spins: to_cnt = spins longer than WD_USB_SPIN_TICKS, max_us = longest
   spin in 10 ms ticks (NOT microseconds for these two slots). */
void wd_usb_spin(uint8_t site, uint32_t t) {
  if(t > 0xffff) t = 0xffff;
  if(t > wd.max_us[site]) wd.max_us[site] = t;
  if(t > WD_USB_SPIN_TICKS && wd.to_cnt[site] != 0xffff) wd.to_cnt[site]++;
}

/* Called by uart_putc when its full-buffer spin passed WD_UART_LIMIT_US.
   thre = LSR.THRE at that moment: 1 means the transmitter sat idle with a full
   buffer, i.e. the THRE interrupt chain was lost (the deadlock the config
   calls CONFIG_UART_DEADLOCKABLE). */
void wd_uart_timeout(uint8_t thre) {
  if(wd.to_cnt[WD_SITE_UART] != 0xffff) wd.to_cnt[WD_SITE_UART]++;
  if(thre) wd.uart_thre = 1;
}

static uint8_t wd_flags(void) {
  uint8_t f = 0;
  if(wd.ingame) f |= 0x01;
  if(BITBAND(FPGA_MCU_RDY_REG->FIOPIN, FPGA_MCU_RDY_BIT)) f |= 0x02;
  if(NVIC_GetEnableIRQ(USB_IRQn)) f |= 0x04;
  if(wd.uart_thre) f |= 0x08;
  if(wd.prev.magic == WD_MAGIC) f |= 0x10;
  return f;
}

static void wd_put32(volatile uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void wd_put16(volatile uint8_t *p, uint16_t v) {
  p[0] = v; p[1] = v >> 8;
}

/* INFO reply bytes 388..511 (everything little-endian); layout mirrored by
   usb_info.py --diag. */
void wd_info_fill(volatile uint8_t *resp) {
  volatile uint8_t *b = resp + 388;
  b[0] = 'W'; b[1] = 'D'; b[2] = 'G'; b[3] = '1';
  wd_put32(b + 4, wd.heartbeat);
  wd_put32(b + 8, getticks());
  b[12] = wd_site; b[13] = wd.last_to_site; b[14] = wd.last_cmd; b[15] = wd_flags();
  wd_put32(b + 16, wd.to_total);
  wd_put32(b + 20, wd.to_addr);
  wd_put32(b + 24, wd.to_ticks);
  wd_put16(b + 28, wd.cmd_count);
  wd_put16(b + 30, wd.sent_bad);
  wd_put32(b + 32, wd.sent_bad_val);
  for(int i = 0; i < WD_NSITES; i++) {
    wd_put16(b + 36 + 2 * i, wd.to_cnt[i]);
    wd_put16(b + 68 + 2 * i, wd.max_us[i]);
  }
  /* 488..511: record left by the previous boot (zero = none) */
  b = resp + 488;
  b[0] = wd.prev.kind; b[1] = wd.prev.site; b[2] = wd.prev.last_cmd; b[3] = wd.prev.last_to_site;
  wd_put32(b + 4, wd.prev.pc);
  wd_put32(b + 8, wd.prev.lr);
  wd_put32(b + 12, wd.prev.info1);
  wd_put32(b + 16, wd.prev.info2);
  wd_put16(b + 20, wd.prev.test_bad);
  b[22] = wd.prev.test_bad_val; b[23] = wd.prev.flags;
}

static void __attribute__((noreturn)) wd_record_reset(uint8_t kind, uint32_t pc, uint32_t lr,
                                                     uint32_t info1, uint32_t info2) {
  wd_rec_t r;
  r.magic = WD_MAGIC;
  r.kind = kind;
  r.site = wd_site;
  r.last_cmd = wd.last_cmd;
  r.last_to_site = wd.last_to_site;
  r.pc = pc; r.lr = lr; r.info1 = info1; r.info2 = info2;
  r.test_bad = wd.to_cnt[WD_SITE_LOOP_TEST];
  r.test_bad_val = wd.max_us[WD_SITE_LOOP_TEST];
  r.flags = wd_flags();
  r.check = wd_rec_sum(&r);
  wd_persist = r;
  __DSB();
  NVIC_SystemReset();
  while(1);
}

/* Find the exception frame of the thread SysTick interrupted: the handler
   chain pushed EXC_RETURN 0xFFFFFFF9 (return to thread mode, MSP) right below
   the 8-word hardware frame.  The frame is accepted only if xPSR has the Thumb
   bit and PC lies in the firmware flash window, so a stray 0xFFFFFFF9 local
   cannot be mistaken for it. */
static uint32_t wd_thread_pc(uint32_t *lr) {
  uint32_t *sp = (uint32_t *)__get_MSP();
  uint32_t *top = &__stack;
  for(int i = 0; i < 128 && &sp[i + 9] <= top; i++) {
    if(sp[i] == 0xFFFFFFF9UL) {
      uint32_t *f = &sp[i + 1];
      if((f[7] & (1UL << 24)) && f[6] >= CONFIG_FW_START && f[6] < CONFIG_FLASH_SIZE) {
        *lr = f[5];
        return f[6];
      }
    }
  }
  *lr = 0;
  return 0;
}

/* HardFault: called from the naked HardFault_Handler with the stacked frame. */
void __attribute__((used, noinline, noreturn)) wd_hardfault(uint32_t *frame) {
  uint32_t pc = 0, lr = 0;
  if((uint32_t)frame >= 0x10000000UL && (uint32_t)frame + 32 <= (uint32_t)&__stack) {
    pc = frame[6];
    lr = frame[5];
  }
  wd_record_reset(WD_KIND_HARDFAULT, pc, lr, SCB->HFSR, SCB->CFSR);
}

/* stall LED code: red on, yellow blinks <site> times (150 ms on/off), 0.8 s pause */
static void wd_led_code(uint32_t stale) {
  uint8_t s = wd_site & (WD_NSITES - 1);
  uint32_t period = s * 30 + 80;
  uint32_t p = stale % period;
  wd.led_on = 1;
  writeled(1);
  rdyled(0);
  readled(p < (uint32_t)s * 30 && (p % 30) < 15);
}

/* SysTick (10 ms) watchdog: only armed inside the in-game loop. */
void SysTick_Hook(void) {
  tick_t now = getticks();
  if(!wd.ingame) {
    wd.hb_ticks = now;
    wd.usboff = 0;
    return;
  }
  if(wd.heartbeat != wd.hb_seen) {
    wd.hb_seen = wd.heartbeat;
    wd.hb_ticks = now;
    if(wd.led_on) {
      wd.led_on = 0;
      writeled(0);
      readled(0);
    }
  }
  uint32_t stale = now - wd.hb_ticks;
  if(stale >= WD_STALL_RESET_TICKS) {
    uint32_t lr, pc = wd_thread_pc(&lr);
    wd_record_reset(WD_KIND_STALL, pc, lr, wd.heartbeat, stale);
  } else if(stale >= WD_STALL_LED_TICKS) {
    wd_led_code(stale);
  }
  if(!NVIC_GetEnableIRQ(USB_IRQn)) {
    if(++wd.usboff >= WD_USBOFF_RESET_TICKS) {
      uint32_t lr, pc = wd_thread_pc(&lr);
      wd_record_reset(WD_KIND_USBOFF, pc, lr, wd.heartbeat, wd.usboff);
    }
  } else {
    wd.usboff = 0;
  }
}

#endif /* GBC_WEDGE_DIAG */
