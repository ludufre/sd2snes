/* zx0_cli: expand a ZX0 stream with the firmware's decoder (src/zx0.c) through a ring of the
   given size, exactly as fpga_rompgm()/load_bootrle() do, and write the bytes it sends out.

   usage: zx0_cli <in.zx0> <out.bin> <ring size, power of two> */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "zx0.h"

static FILE *out;
static unsigned long count;
static void put(uint8_t b) { fputc(b, out); count++; }

int main(int argc, char **argv) {
  FILE *in;
  long len;
  uint8_t *src, *ring;
  unsigned rsize;
  if(argc < 4) { fprintf(stderr, "usage: %s <in.zx0> <out.bin> <ring>\n", argv[0]); return 2; }
  rsize = strtoul(argv[3], NULL, 0);
  if(!rsize || (rsize & (rsize - 1))) { fprintf(stderr, "ring must be a power of two\n"); return 2; }
  if(!(in = fopen(argv[1], "rb"))) { perror(argv[1]); return 2; }
  fseek(in, 0, SEEK_END); len = ftell(in); fseek(in, 0, SEEK_SET);
  /* exact-size buffers: ASan owns the bounds of both the stream and the ring */
  src = malloc(len); ring = malloc(rsize);
  if(!src || !ring || fread(src, 1, len, in) != (size_t)len) { fprintf(stderr, "read failed\n"); return 2; }
  fclose(in);
  if(!(out = fopen(argv[2], "wb"))) { perror(argv[2]); return 2; }
  zx0_stream(src, ring, rsize - 1, put);
  fclose(out);
  free(src); free(ring);
  return 0;
}
