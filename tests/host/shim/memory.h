/* Host shim for src/memory.h: the real fork memory-map constants (so region
 * bounds in the patcher are exercised with shipping values) plus the sram_*
 * accessors patch.c uses, implemented in shim.c over a 16 MB array. */
#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H

#include <stdint.h>

/* Values mirror src/memory.h (the real fork map). */
#define SRAM_ROM_ADDR       (0x000000L)
#define SRAM_MENU_ADDR      (0xC00000L)
#define SRAM_SAVE_ADDR      (0xE00000L)
#define SRAM_IPS_LIST_ADDR  (0xFF5000L)

void     sram_writebyte(uint8_t val, uint32_t addr);
uint8_t  sram_readbyte(uint32_t addr);
void     sram_writeblock(const void *buf, uint32_t addr, uint16_t size);
void     sram_readblock(void *buf, uint32_t addr, uint16_t size);

#endif
