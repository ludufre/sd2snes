/* host stand-in for src/memory.h: the REAL address map plus the PSRAM accessors the
   harness implements over a flat 16 MB array. */
#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H
#include <stdint.h>
#include "memmap.h"

void     sram_writebyte(uint8_t val, uint32_t addr);
uint8_t  sram_readbyte(uint32_t addr);
void     sram_writeshort(uint16_t val, uint32_t addr);
uint16_t sram_readshort(uint32_t addr);
uint16_t sram_writeblock(void *buf, uint32_t addr, uint16_t size);
uint16_t sram_readblock(void *buf, uint32_t addr, uint16_t size);
void     sram_memset(uint32_t base_addr, uint32_t len, uint8_t val);
#endif
