/* bootimg: write the SNES boot program (the RLE image kept in src/snesboot.h) to a file,
   for the build to expand (rled) and re-pack (zx0).

   usage: bootimg <out.rle> */
#include <stdio.h>
#include <stdint.h>
#include "../snesboot.h"

int main(int argc, char **argv) {
  FILE *out;
  if(argc < 2) {
    fprintf(stderr, "usage: %s <out.rle>\n", argv[0]);
    return 1;
  }
  if((out = fopen(argv[1], "wb")) == NULL || fwrite(bootrle, 1, sizeof(bootrle), out) != sizeof(bootrle) || fclose(out)) {
    perror(argv[1]);
    return 1;
  }
  return 0;
}
