#ifndef RLE_H
#define RLE_H

#include <stdint.h>

#define RLE_ESC     (0x9b)
#define RLE_RUN     (0x5b)
#define RLE_RUNLONG (0x77)

uint8_t rle_file_getc(void);

#endif
