/*

   esplink.h: arch-neutral byte transport for the ESP32 companion link.

   Implemented per-MCU:
     - LPC1754/LPC1756 (mk2/mk3): src/lpc175x/uart0.c  -> UART0 on P0.2/P0.3 (pins 79/80)
     - STM32F401 (FXPAK PRO STM32): src/stm32f4xx/usart_stm32.c -> USART6 on PC6/PC7
   The protocol/file-server layer (uart_proto.c) uses ONLY this API, so it is the
   same on every MCU.

   ESPLINK_ENABLE gates whether the link is actually brought up & serviced:
     - LPC: 1 (active - verified on mk3 hardware).
     - STM32: 0 by default - the USART6/PC6-PC7 driver is BUILT (compiles) but
       OFF, because those pins are NOT yet confirmed accessible on any FXPAK PRO
       STM32 PCB and there's no STM32 hardware to test. Flip to 1 once a board
       with PC6/PC7 broken out is verified.

*/

#ifndef ESPLINK_H
#define ESPLINK_H

#include <stdint.h>
#include "config.h"

#ifndef ESPLINK_ENABLE
#  ifdef CONFIG_MK3_STM32
#    define ESPLINK_ENABLE 0   /* STM32 USART6/PC6-PC7: ready but UNTESTED (pins unconfirmed) */
#  else
#    define ESPLINK_ENABLE 1   /* LPC UART0 (P0.2/P0.3, pins 79/80) */
#  endif
#endif

void esplink_init(void);                  /* pins, baud, FIFO/IRQ */
int  esplink_rx_avail(void);              /* bytes ready in the RX ring */
int  esplink_getbyte(void);               /* one RX byte, or -1 if empty */
int  esplink_read(uint8_t *dst, int max); /* drain up to max bytes, returns count */
int  esplink_tx_space(void);              /* free bytes in the TX ring */
int  esplink_write(const uint8_t *src, int len); /* queue up to tx_space, returns count */

#endif
