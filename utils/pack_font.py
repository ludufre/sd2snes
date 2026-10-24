#!/usr/bin/env python3
"""Pack a menu font into a .sfn, the font file the browser applies like a theme.

    pack_font.py <font.a65 | sheet.png> <out.sfn> [--only CODES | --all] [--dry-run]

The input is a font in snes/font.a65 form (256 tiles, 2bpp, 16 bytes each) or a
sheet from `snes/utils/fontedit.py export` (same palette: 0 transparent, 1 fill,
2 outline, 3 mid-tone).

.sfn format (little-endian), read by theme_font_file() in src/theme.c:
    off  size  field
    0    8     magic "SD2SFNT1"
    8    1     version (1)
    9    7     reserved (0)
    16   32    mask: bit (t & 7) of byte (t >> 3) set = the file brings tile t
    48   4096  the 256 tiles, in font.a65 order

The firmware writes only the tiles the mask names over the font of the menu it
loaded, so a font drawn against an older menu cannot repaint icons that moved.
The default mask is the text: 33-159 (ASCII and the Latin accents) and 177-240
(Cyrillic and the French/Italian/German accents). Left out by default are the
control codes and window borders (0-32), the browser icons and the mouse
(160-176) and the tour glyphs, icons and mouse (241-255); --only/--all widen it.
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "snes", "utils"))
import fontedit  # noqa: E402

MAGIC = b"SD2SFNT1"
VERSION = 1
NTILES = 256
TILE_LEN = 16
TEXT_CODES = set(range(33, 160)) | set(range(177, 241))


def read_a65(path):
    tiles, pending = [], []
    with open(path) as fh:
        for line in fh:
            if ".byt" not in line:
                continue
            pending.extend(int(b, 16) for b in fontedit.BYTE_RE.findall(line.split(";")[0]))
            while len(pending) >= TILE_LEN:
                tiles.append(pending[:TILE_LEN])
                pending = pending[TILE_LEN:]
    if len(tiles) != NTILES or pending:
        sys.exit(f"{path}: expected {NTILES} tiles of {TILE_LEN} bytes, got {len(tiles)}")
    return tiles


def read_font(path):
    if path.lower().endswith(".png"):
        tiles, inexact = fontedit.sheet_to_tiles(path)
        if inexact:
            print(f"note: {inexact} pixel(s) snapped to the nearest palette colour")
        return tiles
    return read_a65(path)


def pack(tiles, codes):
    mask = bytearray(32)
    for c in codes:
        mask[c >> 3] |= 1 << (c & 7)
    body = b"".join(bytes(t) for t in tiles)
    return MAGIC + bytes([VERSION]) + bytes(7) + bytes(mask) + body


def main():
    ap = argparse.ArgumentParser(description="Pack a menu font into a .sfn")
    ap.add_argument("font", help="font.a65 or a fontedit.py sheet (.png)")
    ap.add_argument("out", help="output .sfn")
    scope = ap.add_mutually_exclusive_group()
    scope.add_argument("--only", help="tile codes to bring, e.g. '33-159,177-240,244-247'")
    scope.add_argument("--all", action="store_true", help="bring all 256 tiles")
    ap.add_argument("--dry-run", action="store_true", help="list what differs from snes/font.a65")
    a = ap.parse_args()

    tiles = read_font(a.font)
    codes = set(range(NTILES)) if a.all else (fontedit.parse_ranges(a.only) if a.only else TEXT_CODES)

    stock = read_a65(str(fontedit.FONT))
    changed = [c for c in range(NTILES) if tiles[c] != stock[c]]
    kept = [c for c in changed if c in codes]
    left = [c for c in changed if c not in codes]
    print(f"{len(kept)} changed tile(s) in the file: {kept}")
    if left:
        print(f"{len(left)} changed tile(s) outside the mask, left to the menu: {left}")
        print("       widen it with --only/--all if those edits are intended")
    reserved = sorted(codes & fontedit.RESERVED_CODES)
    if reserved:
        print(f"WARNING: the mask includes reserved code(s) {reserved}")
    if a.dry_run:
        print("--dry-run: nothing written")
        return
    data = pack(tiles, codes)
    with open(a.out, "wb") as fh:
        fh.write(data)
    print(f"wrote {a.out} ({len(data)} bytes, {len(codes)} tile(s) in the mask)")


if __name__ == "__main__":
    main()
