
#include "rle.h"
#include "fileops.h"

uint8_t rle_file_getc() {
  static uint16_t rle_filled = 0;
  static uint8_t data;
  if(!rle_filled) {
    data = file_getc();
    switch(data) {
      case RLE_RUN:
        data = file_getc();
        rle_filled = file_getc()-1;
        break;
      case RLE_RUNLONG:
        data = file_getc();
        rle_filled = file_getc();
        rle_filled |= file_getc() << 8;
        rle_filled--;
        break;
      case RLE_ESC:
        data = file_getc();
        break;
    }
  } else {
    rle_filled--;
  }
  if(file_status || file_res) rle_filled = 0;
  return data;
}
