/* Host CLI wrapping the REAL src/nes_chr.c tile-conversion functions,
   mirroring utils/nes_chr_convert.py's CLI 1:1 so run_nes_chr.sh can diff
   their outputs byte-for-byte over the same corpus. This is the golden
   test for the Fase 1c CHR conversion contract -- see NES-CORE-CONTRACT.md
   Sec. 10.5 and src/nes_chr.h. Zero shims needed: nes_chr.c has no
   hardware/firmware header dependency (unlike src/patch.c's host harness,
   which needs tests/host/shim/). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_chr.h"

static uint8_t *read_file(const char *path, long *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
  if (!buf) { fprintf(stderr, "out of memory\n"); exit(1); }
  if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
    fprintf(stderr, "short read: %s\n", path);
    exit(1);
  }
  fclose(f);
  *out_len = len;
  return buf;
}

/* Mirrors chr_from_ines() in utils/nes_chr_convert.py byte-for-byte
   (magic check, trainer skip, prg/chr size derivation). */
static uint8_t *chr_from_ines(uint8_t *rom, long romlen, long *out_len) {
  if (romlen < 16 || memcmp(rom, "NES\x1a", 4) != 0) {
    fprintf(stderr, "not an iNES file\n");
    exit(1);
  }
  long prg = (long)rom[4] * 16384L;
  long chrsize = (long)rom[5] * 8192L;
  long off = 16 + ((rom[6] & 0x04) ? 512 : 0) + prg;
  if (chrsize == 0) {
    fprintf(stderr, "ROM uses CHR-RAM (chr=0), nothing to convert\n");
    exit(1);
  }
  if (off < 0 || off + chrsize > romlen) {
    fprintf(stderr, "CHR window out of bounds (off=%ld chrsize=%ld romlen=%ld)\n",
            off, chrsize, romlen);
    exit(1);
  }
  *out_len = chrsize;
  return rom + off;
}

int main(int argc, char **argv) {
  const char *input = NULL, *output = NULL;
  int bpp = 0, from_ines = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bpp") && i + 1 < argc) {
      bpp = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--from-ines")) {
      from_ines = 1;
    } else if (!input) {
      input = argv[i];
    } else if (!output) {
      output = argv[i];
    }
  }
  if (!input || !output || (bpp != 2 && bpp != 4)) {
    fprintf(stderr, "usage: %s input output --bpp 2|4 [--from-ines]\n", argv[0]);
    return 1;
  }

  long romlen;
  uint8_t *data = read_file(input, &romlen);
  uint8_t *chr = data;
  long chrlen = romlen;
  if (from_ines) {
    chr = chr_from_ines(data, romlen, &chrlen);
  }
  if (chrlen % NES_CHR_TILE_BYTES != 0) {
    fprintf(stderr, "CHR of %ld bytes is not a multiple of %d\n", chrlen, NES_CHR_TILE_BYTES);
    return 1;
  }

  long tiles = chrlen / NES_CHR_TILE_BYTES;
  size_t outsz = (bpp == 2) ? NES_CHR_TILE_SNES2_BYTES : NES_CHR_TILE_SNES4_BYTES;
  uint8_t *out = malloc((size_t)tiles * outsz > 0 ? (size_t)tiles * outsz : 1);
  if (!out) { fprintf(stderr, "out of memory\n"); return 1; }

  for (long t = 0; t < tiles; t++) {
    const uint8_t *tile = chr + t * NES_CHR_TILE_BYTES;
    uint8_t *dst = out + (size_t)t * outsz;
    if (bpp == 2) {
      nes_chr_tile_to_snes2(tile, dst);
    } else {
      nes_chr_tile_to_snes4(tile, dst);
    }
  }

  FILE *of = fopen(output, "wb");
  if (!of) { perror(output); return 1; }
  size_t total = (size_t)tiles * outsz;
  if (total > 0 && fwrite(out, 1, total, of) != total) {
    fprintf(stderr, "short write: %s\n", output);
    fclose(of);
    return 1;
  }
  fclose(of);
  printf("%ld B CHR NES -> %zu B SNES %dbpp\n", chrlen, total, bpp);

  free(data);
  free(out);
  return 0;
}
