#ifndef ZX0_H
#define ZX0_H
#include <stdint.h>
/* ZX0 stream decoder (format by Einar Saukas; the compressor is a host tool).  The output
   goes out a byte at a time through out(); back-references are served from ring[], a
   power-of-two window (rmask = size - 1) at least as large as the compressor's offset
   limit. */
void zx0_stream(const uint8_t *src, uint8_t *ring, unsigned rmask, void (*out)(uint8_t));
#endif
