/* rled: expand an image made by utils/rle exactly as the firmware's in-memory RLE reader
   did (rle_mem_getc() and the "stop when rle_state is set" loop around it), byte for byte.
   That reader ends as soon as its input pointer reaches the end of the blob, so its output
   -- what the FPGA and the SNES actually received from the embedded RLE images -- is not
   always what utils/derle gives.  The build uses this to re-pack those images (the boot
   bitstream, the SNES boot program) without changing a byte of what goes out on the wire.

   usage: rled <in.rle> <out.bin> */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define RLE_ESC     (0x9b)
#define RLE_RUN     (0x5b)
#define RLE_RUNLONG (0x77)

int main(int argc, char **argv) {
  FILE *in, *out;
  uint8_t *buf, *p, *end;
  long len;
  uint16_t filled = 0;
  uint8_t data = 0;

  if(argc < 3) {
    fprintf(stderr, "usage: %s <in.rle> <out.bin>\n", argv[0]);
    return 1;
  }
  if((in = fopen(argv[1], "rb")) == NULL) {
    perror(argv[1]);
    return 1;
  }
  fseek(in, 0, SEEK_END);
  len = ftell(in);
  fseek(in, 0, SEEK_SET);
  if(len <= 0 || (buf = malloc(len)) == NULL || fread(buf, 1, len, in) != (size_t)len) {
    fprintf(stderr, "%s: cannot read\n", argv[1]);
    return 1;
  }
  fclose(in);
  if((out = fopen(argv[2], "wb")) == NULL) {
    perror(argv[2]);
    return 1;
  }
  p = buf;
  end = buf + len;
  for(;;) {
    if(!filled) {
      data = *p++;
      switch(data) {
        case RLE_RUN:
          data = *p++;
          filled = *p++ - 1;
          break;
        case RLE_RUNLONG:
          data = *p++;
          filled = *p++;
          filled |= *p++ << 8;
          filled--;
          break;
        case RLE_ESC:
          data = *p++;
          break;
      }
    } else {
      filled--;
    }
    if(p >= end) break;
    fputc(data, out);
  }
  if(fclose(out)) {
    perror(argv[2]);
    return 1;
  }
  free(buf);
  return 0;
}
