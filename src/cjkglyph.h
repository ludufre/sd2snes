#ifndef CJKGLYPH_H
#define CJKGLYPH_H

#include <stdint.h>

/* An 8x8 1bpp glyph (rows top-down, bit 7 = leftmost pixel) -> the 64 bytes the menu's glyph
 * cache uploads for it (snes/cjk.a65): its 16-px mode-5 cell as two 4bpp tiles, each source
 * pixel two hires pixels wide, colour 1 = body and colour 2 = the dark contour the menu font
 * draws around its letters (orthogonal neighbours only). Byte-for-byte the same as
 * cjk_glyph_tiles() in snes/utils/build_const.py, which renders the menu's own strings; the
 * firmware renders text the menu cannot know in advance (a game's Japanese description). */
void cjk_render_glyph(const uint8_t rows[8], uint8_t out[64]);

#endif
