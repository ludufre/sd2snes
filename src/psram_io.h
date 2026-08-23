/* sd2snes - bounded SD file -> PSRAM streaming */

#ifndef PSRAM_IO_H
#define PSRAM_IO_H

#include <stdint.h>
#include "ff.h"

/* Copy `size` bytes from the current position of `fp` into PSRAM at `addr`, in chunks
 * bounded by `buf`/`bufsz` -- never a whole-region RAM buffer, so the frame stays small
 * and the MCU cannot stall on one giant read. `pump`, when non-NULL, is called after
 * every chunk so a playing DAC effect stays fed. Returns 1 when the full `size` was
 * written, 0 on read error / short read; file_res receives f_read's result for that
 * chunk (FR_OK on a short read).
 *
 * `buf` may be file_buf (the SD DMA scratch) only where no other stream owns it --
 * manual.c passes its own man_buf for that reason. */
int psram_stream_buf(FIL *fp, uint32_t addr, uint32_t size, void *buf,
                     uint16_t bufsz, void (*pump)(void));

/* psram_stream_buf over the shared file_buf. */
int psram_stream(FIL *fp, uint32_t addr, uint32_t size, void (*pump)(void));

#endif
