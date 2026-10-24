/* crc32_update() (src/crc32.c, REAL) against the check value of CRC-32/ISO-HDLC and a bitwise
   reference over every byte value and a long pseudo-random run (the firmware CRCs SRAM for the
   autosave, ROMs for the game info and its card modules with it). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "crc32.h"

static uint32_t ref(uint32_t crc, uint8_t b) {
  crc ^= b;
  for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  return crc;
}

int main(void) {
  int fails = 0;
  uint32_t c = crc32_init();
  for(const char *p = "123456789"; *p; p++) c = crc32_update(c, (unsigned char)*p);
  if(crc32_finalize(c) != 0xcbf43926u) { printf("  FAIL check value %08x\n", crc32_finalize(c)); fails++; }
  for(unsigned s = 0; s < 256 && !fails; s++)
    for(unsigned b = 0; b < 256; b++)
      if(crc32_update(s * 0x01010101u, b) != ref(s * 0x01010101u, b)) {
        printf("  FAIL crc %08x byte %02x\n", s * 0x01010101u, b); fails++; break;
      }
  uint32_t x = 1, a = crc32_init(), r = 0xffffffffu;
  for(unsigned i = 0; i < (1u << 20); i++) {
    x = x * 1103515245u + 12345u;
    a = crc32_update(a, x >> 16);
    r = ref(r, x >> 16);
  }
  if(a != r) { printf("  FAIL 1 MB run %08x != %08x\n", a, r); fails++; }
  printf("crc32: %s\n", fails ? "FAIL" : "ok");
  return fails != 0;
}
