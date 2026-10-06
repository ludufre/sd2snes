#include <string.h>
#include "cjkglyph.h"

/* pixel (x, y) of the 16x8 cell is body: source column x/2 of row y */
static int cjk_body(const uint8_t rows[8], int x, int y) {
  if(x < 0 || x > 15 || y < 0 || y > 7) return 0;
  return (rows[y] >> (7 - x / 2)) & 1;
}

void cjk_render_glyph(const uint8_t rows[8], uint8_t out[64]) {
  memset(out, 0, 64);
  for(int half = 0; half < 2; half++) {
    for(int y = 0; y < 8; y++) {
      uint8_t p0 = 0, p1 = 0;
      for(int x = 0; x < 8; x++) {
        int hx = half * 8 + x;
        int c = 0;
        if(cjk_body(rows, hx, y)) c = 1;
        else if(cjk_body(rows, hx + 1, y) || cjk_body(rows, hx - 1, y)
                || cjk_body(rows, hx, y + 1) || cjk_body(rows, hx, y - 1)) c = 2;
        p0 |= (uint8_t)((c & 1) << (7 - x));
        p1 |= (uint8_t)(((c >> 1) & 1) << (7 - x));
      }
      out[half * 32 + y * 2] = p0;          /* planes 0/1, row-interleaved; 2/3 stay 0 */
      out[half * 32 + y * 2 + 1] = p1;
    }
  }
}
