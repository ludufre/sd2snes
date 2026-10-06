#!/usr/bin/env bash
# The firmware's CJK glyph renderer (src/cjkglyph.c) against the menu generator's
# (snes/utils/build_const.py cjk_glyph_tiles): every glyph of both CJK fonts, byte for byte.
# The menu draws its own strings with the generator's tiles and a game's description with the
# firmware's; a divergence would show the same character two ways on one screen.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tests/host/sanitizers.sh
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/cli.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include "cjkglyph.h"
int main(void) {             /* stdin: 16 hex digits per line -> stdout: 128 hex digits */
  char line[64];
  while(fgets(line, sizeof line, stdin)) {
    unsigned char rows[8], out[64];
    for(int i = 0; i < 8; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); rows[i] = (unsigned char)v; }
    cjk_render_glyph(rows, out);
    for(int i = 0; i < 64; i++) printf("%02x", out[i]);
    printf("\n");
  }
  return 0;
}
C
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc "$OUT/cli.c" src/cjkglyph.c -o "$OUT/cli"
fail=0
for font in snes/fonts/misaki_gothic_2nd.hex snes/fonts/fusion8_zh_hans.hex; do
  cut -d: -f2 "$font" > "$OUT/in.txt"
  "$OUT/cli" < "$OUT/in.txt" > "$OUT/c.txt"
  python3 - "$OUT/in.txt" > "$OUT/py.txt" <<'P'
import sys; sys.path.insert(0, "snes/utils")
import build_const as b
for l in open(sys.argv[1]):
    print(b.cjk_glyph_tiles(bytes.fromhex(l.strip())).hex())
P
  if cmp -s "$OUT/c.txt" "$OUT/py.txt"; then echo "PASS  $(wc -l < "$OUT/in.txt" | tr -d ' ') glyphs of $(basename "$font")"
  else echo "FAIL  $(basename "$font"): first difference at line $(cmp "$OUT/c.txt" "$OUT/py.txt" | awk '{print $NF}')"; fail=1; fi
done
exit $fail
