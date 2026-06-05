/*

   usart_stm32.c: ESP32-link byte transport for STM32F401 (FXPAK PRO STM32).

   Implements the arch-neutral esplink.h API on **USART6** (the only USART whose
   pins are free in config-mk3-stm32):
       PC6 = USART6_TX (AF8),  PC7 = USART6_RX (AF8),  APB2 = 84 MHz.
   USART1/2 are unavailable: USART2 (PA2/PA3) is the debug console, and every
   USART1 pin (PA9 USB_VBUS, PA10 HWREV7, PB6 HWREV0, PB7 FPGA_INIT_B) is taken.

   *** UNTESTED / pins UNCONFIRMED. ***  This file compiles so it's ready, but
   ESPLINK_ENABLE defaults to 0 on STM32 (see esplink.h): PC6/PC7 are not known
   to be broken out on any FXPAK PRO STM32 PCB, and there's no STM32 hardware to
   test on. Confirm the pins, then flip ESPLINK_ENABLE to 1 and verify the BRR
   on a scope before trusting it.

   STM32 has ~64KB contiguous RAM (no .ahbram), so the rings live in plain .bss.

*/

#include "bits.h"
#include "config.h"
#include "esplink.h"

#define RX_SHIFT 9
#define TX_SHIFT 9
#define RX_SIZE  (1u << RX_SHIFT)
#define TX_SIZE  (1u << TX_SHIFT)
#define RX_MASK  (RX_SIZE - 1u)
#define TX_MASK  (TX_SIZE - 1u)

static volatile uint8_t rxbuf[RX_SIZE];
static volatile uint8_t txbuf[TX_SIZE];
static volatile unsigned int rx_ri, rx_wi;   /* ISR writes wi, main reads ri */
static volatile unsigned int tx_ri, tx_wi;   /* main writes wi, ISR reads ri */

void USART6_IRQHandler(void) {
  uint32_t sr = USART6->SR;

  if (sr & USART_SR_RXNE) {
    uint8_t c = (uint8_t)USART6->DR;            /* reading DR clears RXNE (and ORE) */
    unsigned int nwi = (rx_wi + 1) & RX_MASK;
    if (nwi != rx_ri) { rxbuf[rx_wi] = c; rx_wi = nwi; }
  } else if (sr & USART_SR_ORE) {
    (void) USART6->DR;                          /* clear overrun (SR read above + DR read) */
  }

  if (BITBAND(USART6->CR1, USART_CR1_TXEIE_Pos) && (sr & USART_SR_TXE)) {
    if (tx_ri != tx_wi) {
      USART6->DR = txbuf[tx_ri];
      tx_ri = (tx_ri + 1) & TX_MASK;
    }
    if (tx_ri == tx_wi) BITBAND(USART6->CR1, USART_CR1_TXEIE_Pos) = 0;
  }
}

void esplink_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
  RCC->APB2ENR |= RCC_APB2ENR_USART6EN;

  /* PC6 = USART6_TX, PC7 = USART6_RX, alternate function 8 */
  GPIO_SEL_AF(GPIOC, 6, 8);
  GPIO_SEL_AF(GPIOC, 7, 8);
  GPIO_MODE_AF(GPIOC, 6);
  GPIO_MODE_AF(GPIOC, 7);
  GPIO_SPEED(GPIOC, 6, IO_SPEED_H);
  GPIO_SPEED(GPIOC, 7, IO_SPEED_H);
  GPIO_PULLUP(GPIOC, 7);                         /* RX idle high */

  USART6->CR1 = 0;
  USART6->CR3 = USART_CR3_ONEBIT;

  /* BRR for 921600 8N1, 16x oversampling, APB2 = CONFIG_CPU_FREQUENCY (84MHz).
     84e6/16/921600 = 5.6966 -> mantissa 5, fraction 11 -> BRR 0x5B (+0.16%). */
  {
    float f = (float)CONFIG_CPU_FREQUENCY / 16.0f / (float)CONFIG_UART_BAUDRATE;
    uint32_t mant = (uint32_t)f;
    uint32_t frac = (uint32_t)(16.0f * (f - mant) + 0.5f);
    if (frac >= 16) { mant++; frac = 0; }
    USART6->BRR = (mant << 4) | (frac & 0xf);
  }

  rx_ri = rx_wi = tx_ri = tx_wi = 0;
  USART6->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
  NVIC_EnableIRQ(USART6_IRQn);
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
  while (n < len) {
    unsigned int nwi = (tx_wi + 1) & TX_MASK;
    if (nwi == tx_ri) break;                     /* ring full */
    txbuf[tx_wi] = src[n++];
    tx_wi = nwi;
  }
  if (n) BITBAND(USART6->CR1, USART_CR1_TXEIE_Pos) = 1;   /* TXE is level -> fires now */
  return n;
}
