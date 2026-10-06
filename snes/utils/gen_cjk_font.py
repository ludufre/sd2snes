#!/usr/bin/env python3
"""snes/fonts/<font>.hex -> lang_<code>.fnt, the font the FIRMWARE reads to draw CJK text the
menu cannot know in advance (a game's Japanese title/description, src/gameinfo.c). The menu's
own strings get their glyphs from build_const.py; this is the whole font, so any character
of the language can show up.
Layout: "SDF1", record count (u16 LE), 2 bytes 0; then one 10-byte record per glyph, sorted by
code point: code point (u16 LE), 8 rows top-down (bit 7 = leftmost pixel).
  gen_cjk_font.py <font.hex> -o <out.fnt>"""
import sys
from pathlib import Path

src = Path(sys.argv[1])
out = Path(sys.argv[sys.argv.index("-o") + 1])
recs = []
for line in src.read_text().splitlines():
    if ":" in line:
        cp, rows = line.split(":")
        cp = int(cp, 16)
        if cp <= 0xFFFF:
            recs.append((cp, bytes.fromhex(rows)))
recs.sort()
data = bytearray(b"SDF1" + len(recs).to_bytes(2, "little") + bytes(2))
for cp, rows in recs:
    data += cp.to_bytes(2, "little") + rows
out.write_bytes(data)
print(f"{out}: {len(recs)} glyphs, {len(data)} bytes")
