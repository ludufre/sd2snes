/* Host shim for src/crc32.h (standard reflected CRC-32, same as BPS/zlib). */
#ifndef HOST_CRC32_H
#define HOST_CRC32_H

#include <stdint.h>

uint32_t crc32_init(void);
uint32_t crc32_update(uint32_t crc, uint8_t b);
uint32_t crc32_finalize(uint32_t crc);

#endif
