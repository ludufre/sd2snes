/* ZX0 (v2 format) stream decoder, written for the firmware: the embedded boot images are
   ZX0 streams made at build time by utils/zx0c (Makefile).  Elias-gamma lengths and
   offsets, literals copied through; the window is a ring the caller owns, so the stream
   must not reference further back than its size (zx0c -w). */
#include "zx0.h"
typedef struct { const uint8_t *p; uint8_t mask, val, last, back; } bs_t;
static int rb(bs_t *s) {
  if(s->back) { s->back = 0; return s->last & 1; }
  if(!(s->mask >>= 1)) { s->mask = 128; s->val = *s->p++; }
  return (s->val & s->mask) != 0;
}
static unsigned gam(bs_t *s, int inv) {
  unsigned v = 1;
  while(!rb(s)) v = (v << 1) | (rb(s) ^ inv);
  return v;
}
void zx0_stream(const uint8_t *src, uint8_t *ring, unsigned rmask, void (*out)(uint8_t)) {
  bs_t s = { src, 0, 0, 0, 0 };
  unsigned pos = 0, off = 1, len;
  uint8_t b;
lit:
  len = gam(&s, 0);
  while(len--) { b = *s.p++; ring[pos++ & rmask] = b; out(b); }
  if(rb(&s)) goto newoff;
  len = gam(&s, 0);
  goto copy;
newoff:
  off = gam(&s, 1);
  if(off == 256) return;
  s.last = *s.p++;
  off = off * 128 - (s.last >> 1);
  s.back = 1;
  len = gam(&s, 0) + 1;
copy:
  while(len--) { b = ring[(pos - off) & rmask]; ring[pos++ & rmask] = b; out(b); }
  if(rb(&s)) goto newoff;
  goto lit;
}
