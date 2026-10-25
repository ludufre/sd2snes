/* sdcrc64.h (the four DATx CRC16s of a 4-bit SD block as one 64-bit CRC) against a plain
   bit-by-bit CRC16-CCITT per data line, and against the SD spec's own example. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "sdcrc64.h"

/* CRC16-CCITT (x^16 + x^12 + x^5 + 1, init 0) of the bits line k carries */
static uint16_t line_crc(const uint8_t *b, int k) {
  uint16_t c = 0;
  for(int i = 0; i < 1024; i++) {
    uint8_t nib = (i & 1) ? (b[i >> 1] & 15) : (b[i >> 1] >> 4);
    int bit = (nib >> k) & 1;
    int fb = ((c >> 15) & 1) ^ bit;
    c = (uint16_t)(c << 1);
    if(fb) c ^= 0x1021;
  }
  return c;
}

/* what the card expects on the 16 CRC clocks, DAT3 = bit 3 of each nibble */
static uint64_t expect(const uint8_t *b) {
  uint16_t c[4];
  uint64_t e = 0;
  for(int k = 0; k < 4; k++) c[k] = line_crc(b, k);
  for(int s = 15; s >= 0; s--)
    for(int k = 3; k >= 0; k--) e = e << 1 | ((c[k] >> s) & 1);
  return e;
}

static uint64_t table(const uint8_t *b) {
  uint64_t crc = 0;
  for(int i = 0; i < 512; i++) { SD_CRC64_NIB(crc, b[i] >> 4); SD_CRC64_NIB(crc, b[i] & 15); }
  return crc;
}

int main(void) {
  uint8_t b[512];
  int fails = 0;
  /* SD Physical Layer spec: 512 bytes of 0xFF (4096 one bits, 1-bit mode) give CRC16 0x7FA1 */
  {
    uint16_t c = 0;
    for(int i = 0; i < 4096; i++) { int fb = ((c >> 15) & 1) ^ 1; c = (uint16_t)(c << 1); if(fb) c ^= 0x1021; }
    if(c != 0x7fa1) { printf("reference CRC16 is wrong: %04x\n", c); return 1; }
  }
  for(int i = 0; i < 512; i++) b[i] = 0xff;
  if(table(b) != expect(b)) { printf("FAIL: 0xFF block\n"); fails++; }
  srand(1);
  for(int t = 0; t < 2000; t++) {
    for(int i = 0; i < 512; i++) b[i] = (uint8_t)rand();
    if(t < 256) for(int i = 0; i < 512; i++) b[i] = i == t ? 1 : 0;   /* single bits */
    if(table(b) != expect(b)) { printf("FAIL: block %d\n", t); if(++fails > 5) break; }
  }
  printf(fails ? "FAILED\n" : "sdcrc64: all blocks match\n");
  return fails != 0;
}
