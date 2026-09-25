#include "config.h"
#include "uart.h"
#include "wdiag.h"

#ifdef GBC_WEDGE_DIAG
/* Wedge diagnostic build: hand the stacked frame to wd_hardfault (wdiag.c),
   which records PC/LR/HFSR/CFSR in AHB RAM and resets the MCU. */
void __attribute__((naked)) HardFault_Handler(void) {
  __asm volatile(
    "tst lr, #4    \n"
    "ite eq        \n"
    "mrseq r0, msp \n"
    "mrsne r0, psp \n"
    "b wd_hardfault\n");
}
#else
void HardFault_Handler(void) {
  printf("HFSR: %lx\n", SCB->HFSR);
  while (1) ;
}
#endif

void MemManage_Handler(void) {
  printf("MemManage - CFSR: %lx; MMFAR: %lx\n", SCB->CFSR, SCB->MMFAR);
}

void BusFault_Handler(void) {
  printf("BusFault - CFSR: %lx; BFAR: %lx\n", SCB->CFSR, SCB->BFAR);
}

void UsageFault_Handler(void) {
  printf("UsageFault - CFSR: %lx; BFAR: %lx\n", SCB->CFSR, SCB->BFAR);
}

