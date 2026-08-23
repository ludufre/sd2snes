/*

   uart.h: Definitions for the UART access routines

*/

#ifndef UART_H
#define UART_H

#include <stdio.h>
#include <stdint.h>
#include "config.h"

/* A few symbols to make this code work for all four UARTs */
#if defined CONFIG_MK3_STM32
#  if defined(CONFIG_UART_NUM) && CONFIG_UART_NUM == 1
#    define UART_PCONBIT 3
#    define UART_PCLKREG PCLKSEL0
#    define UART_PCLKBIT 6
#    define UART_REGS    LPC_UART0
#    define UART_HANDLER UART0_IRQHandler
#    define UART_IRQ     USART1_IRQn
#  elif CONFIG_UART_NUM == 2
#  elif CONFIG_UART_NUM == 6
#  else
#    error CONFIG_UART_NUM is not set or has an invalid value!
#  endif
#else
#  if defined(CONFIG_UART_NUM) && CONFIG_UART_NUM == 0
#    define UART_PCONBIT 3
#    define UART_PCLKREG PCLKSEL0
#    define UART_PCLKBIT 6
#    define UART_REGS    LPC_UART0
#    define UART_HANDLER UART0_IRQHandler
#    define UART_IRQ     UART0_IRQn
#  elif CONFIG_UART_NUM == 1
#    define UART_PCONBIT 4
#    define UART_PCLKREG PCLKSEL0
#    define UART_PCLKBIT 8
#    define UART_REGS    LPC_UART1
#    define UART_HANDLER UART1_IRQHandler
#    define UART_IRQ     UART1_IRQn
#  elif CONFIG_UART_NUM == 2
#    define UART_PCONBIT 24
#    define UART_PCLKREG PCLKSEL1
#    define UART_PCLKBIT 16
#    define UART_REGS    LPC_UART2
#    define UART_HANDLER UART2_IRQHandler
#    define UART_IRQ     UART2_IRQn
#  elif CONFIG_UART_NUM == 3
#    define UART_PCONBIT 25
#    define UART_PCLKREG PCLKSEL1
#    define UART_PCLKBIT 18
#    define UART_REGS    LPC_UART3
#    define UART_HANDLER UART3_IRQHandler
#    define UART_IRQ     UART3_IRQn
#  else
#    error CONFIG_UART_NUM is not set or has an invalid value!
#  endif
#endif

/* snprintf()/vsnprintf() are NOT diagnostics -- they render into a caller-supplied
   buffer and stay compiled in for every config (see printf.c). Only the UART side of
   printf.c follows CONFIG_UART_DEBUG. */
int  snprintf(char *str, size_t size, const char *format, ...);

#ifdef CONFIG_UART_DEBUG

#ifdef __AVR__
#  include <avr/pgmspace.h>
  void uart_puts_P(prog_char *text);
#else
#  define uart_puts_P(str) uart_puts(str)
#endif

void uart_init(void);
unsigned char uart_getc(void);
unsigned char uart_gotc(void);
void uart_putc(char c);
void uart_puts(const char *str);
void uart_puthex(uint8_t num);
void uart_puts_hex(const char *text);
void uart_trace(void *ptr, uint32_t start, uint32_t len);
void uart_flush(void);
int  printf(const char *fmt, ...);
#define uart_putcrlf() uart_putc('\n')

#else

/* No serial console in this config: printf() and every uart_*() entry point compile
   away, which also strands cli_entrycheck() -> uart_gotc() at a constant 0, so the CLI
   and its YMODEM transfer drop out under --gc-sections/LTO.  The dead `if (0)` keeps
   the arguments syntactically live for -Wformat without emitting the call or its
   literals.  printf_real() is declared and NEVER defined, so a reference that is not
   this macro fails at link time instead of pulling the diagnostics back in.

   Two rules follow.  (1) The arguments are NOT evaluated here -- a side effect inside
   a printf() argument silently does not happen on the Mk.II.  (2) Every .c that calls
   printf() must see THIS header; including only <stdio.h> binds newlib's printf and
   links the whole stdio/syscall chain back in (mcu-check greps the Mk.II image for
   _printf_r/_write). */
#if defined(DEBUG_USB) || defined(DEBUG_USBHW) || defined(CONFIG_CDC_DEBUG)
#  error "DEBUG_USB / DEBUG_USBHW / CONFIG_CDC_DEBUG need CONFIG_UART_DEBUG: there is no console to print to"
#endif
int printf_real(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define printf(...)      do { if (0) printf_real(__VA_ARGS__); } while (0)
#define vprintf(fmt, ap) do { (void)(fmt); (void)(ap); } while (0)

#define uart_init()       do {} while(0)
#define uart_getc()       0
#define uart_gotc()       0
#define uart_putc(x)      do { (void)(x); } while(0)
#define uart_puthex(x)    do { (void)(x); } while(0)
#define uart_flush()      do {} while(0)
#define uart_puts_P(x)    do { (void)(x); } while(0)
#define uart_puts(x)      do { (void)(x); } while(0)
#define uart_puts_hex(x)  do { (void)(x); } while(0)
#define uart_putcrlf()    do {} while(0)
#define uart_trace(a,b,c) do { (void)(a); (void)(b); (void)(c); } while(0)

#endif

#endif
