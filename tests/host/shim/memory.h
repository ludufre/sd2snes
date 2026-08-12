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
#define SRAM_CHEAT_ADDR     (0xD00000L)
#define SRAM_IPS_LIST_ADDR  (0xFF5000L)
#define SRAM_IPS_TEXT_ADDR  (0xD90000L)
#define SRAM_IPS_SCRATCH_ADDR (0xDA0000L)
#define SRAM_PATCH_TOP      (SRAM_CHEAT_ADDR)
#define SRAM_EXPORT_PATH_ADDR (0xFF4D00L)
#define SAVE_BASEDIR    ("/sd2snes/saves/")
#define SRM_SLOT_COUNT  4

void     sram_writebyte(uint8_t val, uint32_t addr);
uint8_t  sram_readbyte(uint32_t addr);
void     sram_writeblock(const void *buf, uint32_t addr, uint16_t size);
void     sram_readblock(void *buf, uint32_t addr, uint16_t size);
void     sram_readstrn(void *buf, uint32_t addr, uint16_t size);
int      save_sram(uint8_t *filename, uint32_t sram_size, uint32_t base_addr);
void     sram_writestrn(void *buf, uint32_t addr, uint16_t size);

#endif
