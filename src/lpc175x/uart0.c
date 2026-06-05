/*

   uart0.c: ESP32-link byte transport for lpc175x (LPC1754/LPC1756) - UART0 on
   P0.2 (TXD0) / P0.3 (RXD0) = LQFP80 physical pins 79/80.  Implements the
   arch-neutral esplink.h API.  Interrupt-driven, own TX/RX rings, independent
   of the UART3 debug console (uart.c).  Pattern mirrors uart.c's UART_HANDLER,
   adding the RX path it omits.  Never touches the UART_* macros (they bind to
   UART3 in config-mk3).

*/

#include <stddef.h>     /* offsetof (used by the GPIO_MODE_AF / PINSEL macros) */

#include "bits.h"
#include "config.h"
#include "esplink.h"

/* Power-of-two rings (mask with size-1). 512B holds a full 264B frame + slack. */
#define RX_SHIFT 9
#define TX_SHIFT 9
#define RX_SIZE  (1u << RX_SHIFT)
#define TX_SIZE  (1u << TX_SHIFT)
#define RX_MASK  (RX_SIZE - 1u)
#define TX_MASK  (TX_SIZE - 1u)

/* Big rings in AHB SRAM (IN_AHBRAM) to spare the tiny 16KB main SRAM / stack. */
static IN_AHBRAM volatile uint8_t rxbuf[RX_SIZE];
static IN_AHBRAM volatile uint8_t txbuf[TX_SIZE];
static volatile unsigned int rx_ri, rx_wi;   /* ISR writes wi, main reads ri */
static volatile unsigned int tx_ri, tx_wi;   /* main writes wi, ISR reads ri */

/* 921600 @ 96MHz PCLK: DL=4, FDR=0x85 (FR=1.625) -> 923077 baud (+0.16%).
   Reliable sweet spot for this wiring. Baud ladder tested (1MB): 921600=25s clean;
   1.0M=25.8s clean but no faster (round-trip-bound); 1.2M=83s & 1.5M=99s (CRC
   retries); 2M truncates. So higher baud gives nothing here. */
#define UART0_DLL  4
#define UART0_DLM  0
#define UART0_FDR  0x85

void UART0_IRQHandler(void) {
  uint32_t iir = LPC_UART0->IIR;
  if (iir & 1) return;                 /* no interrupt pending */

  switch (iir & 0x0e) {
  case 0x04: /* RDA - RX FIFO at trigger level */
  case 0x0c: /* CTI - RX character timeout */
    while (BITBAND(LPC_UART0->LSR, 0)) {        /* RDR: receive data ready */
      uint8_t c = LPC_UART0->RBR;
      unsigned int nwi = (rx_wi + 1) & RX_MASK;
      if (nwi != rx_ri) { rxbuf[rx_wi] = c; rx_wi = nwi; }   /* drop on overflow */
    }
    break;

  case 0x02: /* THRE - transmit holding/FIFO empty */
    {
      int maxchars = 16;                        /* TX FIFO depth */
      while (tx_ri != tx_wi && --maxchars >= 0) {
        LPC_UART0->THR = txbuf[tx_ri];
        tx_ri = (tx_ri + 1) & TX_MASK;
      }
      if (tx_ri == tx_wi) BITBAND(LPC_UART0->IER, 1) = 0;    /* nothing left */
    }
    break;

  case 0x06: /* RLS - line status (errors) */
    (void) LPC_UART0->LSR;
    break;

  default: break;
  }
}

void esplink_init(void) {
  /* P0.2 -> TXD0, P0.3 -> RXD0  (PINSEL function 01) */
  GPIO_MODE_AF(LPC_GPIO0, 2, 1);
  GPIO_MODE_AF(LPC_GPIO0, 3, 1);

  BITBAND(LPC_SC->PCONP, 3) = 1;       /* power UART0 (PCUART0) */

  /* UART0 PCLK = CCLK/1 (PCLKSEL0 bits 7:6 = 01) -> 96MHz */
  BITBAND(LPC_SC->PCLKSEL0, 6) = 1;
  BITBAND(LPC_SC->PCLKSEL0, 7) = 0;

  LPC_UART0->LCR = BV(7) | 3;          /* DLAB=1, 8N1 */
  LPC_UART0->DLL = UART0_DLL;
  LPC_UART0->DLM = UART0_DLM;
  LPC_UART0->FDR = UART0_FDR;
  BITBAND(LPC_UART0->LCR, 7) = 0;      /* DLAB=0 */

  LPC_UART0->FCR = BV(0) | BV(1) | BV(2) | (2u << 6);  /* FIFO en+reset, RX trig 8 */

  rx_ri = rx_wi = tx_ri = tx_wi = 0;
  LPC_UART0->IER = BV(0);              /* RX data + timeout int; THRE on demand */
  NVIC_EnableIRQ(UART0_IRQn);
}

int esplink_rx_avail(void) { return (int)((rx_wi - rx_ri) & RX_MASK); }

int esplink_getbyte(void) {
  if (rx_ri == rx_wi) return -1;
  uint8_t c = rxbuf[rx_ri];
  rx_ri = (rx_ri + 1) & RX_MASK;
  return (int)c;
}

int esplink_read(uint8_t *dst, int max) {
  int n = 0;
  while (n < max && rx_ri != rx_wi) { dst[n++] = rxbuf[rx_ri]; rx_ri = (rx_ri + 1) & RX_MASK; }
  return n;
}

int esplink_tx_space(void) { return (int)(TX_MASK - ((tx_wi - tx_ri) & TX_MASK)); }

int esplink_write(const uint8_t *src, int len) {
  int n = 0;
  BITBAND(LPC_UART0->IER, 1) = 0;      /* mask THRE int while we prime (protect tx_ri) */
  while (n < len) {
    unsigned int nwi = (tx_wi + 1) & TX_MASK;
    if (nwi == tx_ri) break;           /* ring full */
    txbuf[tx_wi] = src[n++];
    tx_wi = nwi;
  }
  if (BITBAND(LPC_UART0->LSR, 5)) {    /* THR/FIFO empty -> jump-start directly */
    int maxchars = 16;
    while (tx_ri != tx_wi && --maxchars >= 0) {
      LPC_UART0->THR = txbuf[tx_ri];
      tx_ri = (tx_ri + 1) & TX_MASK;
    }
  }
  if (tx_ri != tx_wi) BITBAND(LPC_UART0->IER, 1) = 1;
  return n;
}
