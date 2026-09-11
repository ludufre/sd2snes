/* Host shim for src/memory.h as src/sort.c sees it: the menu window base and the five
 * PSRAM accessors the sorter uses, implemented in sort_cli.c over a 16 MB array.
 * Kept apart from shim/memory.h, whose accessor signatures follow patch.c's usage. */
#ifndef HOST_SORT_MEMORY_H
#define HOST_SORT_MEMORY_H

#include <stdint.h>

#define SRAM_MENU_ADDR      (0xC00000L)   /* mirrors src/memmap.h */
#define SRAM_DIR_ADDR       (0xDB0000L)   /* mirrors src/memmap.h */

uint8_t  sram_readbyte(uint32_t addr);   /* only the unused *_old helper references it */
uint32_t sram_readlong(uint32_t addr);
void     sram_writelong(uint32_t val, uint32_t addr);
uint16_t sram_readblock(void *buf, uint32_t addr, uint16_t size);
uint16_t sram_writeblock(void *buf, uint32_t addr, uint16_t size);
uint16_t sram_readstrn(void *buf, uint32_t addr, uint16_t size);

#endif
