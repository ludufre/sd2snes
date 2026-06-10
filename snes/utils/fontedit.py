#!/usr/bin/env python3
"""
Decode/encode the 8x8 2bpp SNES tile font used by the sd2snes menu.

Each character in font.a65 is 16 bytes: 8 rows, each row being two bytes
(low plane, high plane). Pixel color = (high_bit << 1) | low_bit, with
MSB-first within each byte. Colors 1-3 are all visible foreground in the
menu palette.

Usage:
    python3 fontedit.py show <code>        Render a single char
    python3 fontedit.py addaccents         Insert PT-BR accented chars

The PT-BR insertion writes 24 new tiles into empty slots and prints the
codepoint -> char mapping so const.a65 can reference them.
"""

import re
import sys
from pathlib import Path

FONT = Path(__file__).resolve().parent.parent / "font.a65"

# Codepoints reserved for accented letters. MUST match build_const.py ACCENTS
# (enforced by tests/test_i18n_parity.py) and the glyph tiles in font.a65.
ACCENT_MAP = {
    "á": 130, "à": 131, "â": 132, "ã": 133, "é": 134, "ê": 135,
    "í": 136, "ó": 137, "ô": 138, "õ": 139, "ú": 140, "ç": 141,
    "Á": 142, "À": 143, "Â": 144, "Ã": 145, "É": 146, "Ê": 147,
    "Í": 148, "Ó": 149, "Ô": 150, "Õ": 151, "Ú": 152, "Ç": 153,
    # Spanish additions:
    "ñ": 154, "Ñ": 155, "ü": 156, "Ü": 157, "¿": 158, "¡": 159,
}

BYTE_RE = re.compile(r"\$([0-9a-fA-F]{2})")


def load_font():
    """Return (header_lines, list_of_tiles) where each tile is a list of
    16 ints."""
    lines = FONT.read_text().splitlines()
    header = []
    tiles = []
    i = 0
    # Header (everything before first .byt line)
    while i < len(lines) and ".byt" not in lines[i]:
        header.append(lines[i])
        i += 1
    # Tile data: 2 .byt lines per tile, 16 bytes total. Skip non-.byt lines.
    pending = []
    while i < len(lines):
        line = lines[i]
        if ".byt" in line:
            bytes_ = BYTE_RE.findall(line)
            pending.extend(int(b, 16) for b in bytes_)
            while len(pending) >= 16:
                tiles.append(pending[:16])
                pending = pending[16:]
        i += 1
    return header, tiles


def tile_to_pixels(tile):
    """Return an 8x8 list of color indices (0-3)."""
    pixels = []
    for row in range(8):
        low = tile[row * 2]
        high = tile[row * 2 + 1]
        line = []
        for px in range(8):
            bit = 7 - px
            c = ((high >> bit) & 1) << 1 | ((low >> bit) & 1)
            line.append(c)
        pixels.append(line)
    return pixels


def pixels_to_tile(pixels):
    """Inverse of tile_to_pixels."""
    out = []
    for row in range(8):
        low = 0
        high = 0
        for px in range(8):
            c = pixels[row][px]
            bit = 7 - px
            low |= (c & 1) << bit
            high |= ((c >> 1) & 1) << bit
        out.append(low)
        out.append(high)
    return out


def render_ascii(pixels):
    glyphs = {0: ".", 1: "x", 2: "X", 3: "#"}
    return "\n".join("".join(glyphs[c] for c in row) for row in pixels)


def show(code):
    _, tiles = load_font()
    tile = tiles[code]
    print(f"char {code} ({chr(code) if 32 <= code < 127 else '?'}):")
    print(render_ascii(tile_to_pixels(tile)))


# -- Accent designs ---------------------------------------------------------
# The menu font uses nearly the full 8x8 tile: most letters occupy rows 0-6
# and row 7 is the only reliable free row. Drawing accents in the tile above
# collides with adjacent menu rows, while compressing the letter visibly
# damages it. These marks therefore never delete or move letter pixels. All
# top accents are compact marks on the original top row; tilde is reduced to a
# dash for readability. Cedilla keeps the old attached two-row shape.

ACUTE = "....###."
GRAVE = ".###...."
CIRC = "..#..#.."
TILDE = ".#####.."
CEDILLA_TOP = "...##..."
CEDILLA_BOTTOM = "..####.."


def overlay_row(pixels, art_row, row, color=3):
    for c, ch in enumerate(art_row):
        if ch in ("x", "X", "#"):
            pixels[row][c] = color


def with_accent(base_letter, accent_row, tiles):
    """Keep the base letter in place and paint a compact mark on row 0."""
    base = tile_to_pixels(tiles[ord(base_letter)])
    out = [row[:] for row in base]
    overlay_row(out, accent_row, row=0)
    return out


def with_cedilla(base_letter, tiles):
    """Copy the base letter and paint the established two-row cedilla."""
    base = tile_to_pixels(tiles[ord(base_letter)])
    out = [row[:] for row in base]
    overlay_row(out, CEDILLA_TOP, row=6)
    overlay_row(out, CEDILLA_BOTTOM, row=7)
    return out


def encode_tile_lines(tile, label=None):
    """Format 16 bytes as two .byt lines matching font.a65 style. The
    optional `label` is placed at the start of the first line (used for
    the `font` symbol on tile 0)."""
    fmt = lambda b: ", ".join(f"${x:02x}" for x in b)
    first_prefix = f"{label}    .byt  " if label else "        .byt  "
    return [
        first_prefix + fmt(tile[:8]),
        "        .byt  " + fmt(tile[8:16]),
    ]


def add_accents():
    header, tiles = load_font()
    # Build new tiles.
    base = {
        "á": ("a", ACUTE),  "à": ("a", GRAVE),  "â": ("a", CIRC),  "ã": ("a", TILDE),
        "é": ("e", ACUTE),  "ê": ("e", CIRC),
        "í": ("i", ACUTE),
        "ó": ("o", ACUTE),  "ô": ("o", CIRC),  "õ": ("o", TILDE),
        "ú": ("u", ACUTE),
        "Á": ("A", ACUTE),  "À": ("A", GRAVE),  "Â": ("A", CIRC),  "Ã": ("A", TILDE),
        "É": ("E", ACUTE),  "Ê": ("E", CIRC),
        "Í": ("I", ACUTE),
        "Ó": ("O", ACUTE),  "Ô": ("O", CIRC),  "Õ": ("O", TILDE),
        "Ú": ("U", ACUTE),
    }
    new_tiles = dict(enumerate(tiles))  # code -> tile
    for accented, code in ACCENT_MAP.items():
        if accented == "ç":
            new_tiles[code] = pixels_to_tile(with_cedilla("c", tiles))
        elif accented == "Ç":
            new_tiles[code] = pixels_to_tile(with_cedilla("C", tiles))
        else:
            base_letter, accent_rows = base[accented]
            new_pixels = with_accent(base_letter, accent_rows, tiles)
            new_tiles[code] = pixels_to_tile(new_pixels)

    # Reassemble file. Preserve the `font` label on the very first tile so
    # the assembler can still resolve references to it.
    out_lines = list(header)
    total = max(len(tiles), max(new_tiles) + 1)
    for code in range(total):
        tile = new_tiles.get(code, [0] * 16)
        label = "font" if code == 0 else None
        out_lines.extend(encode_tile_lines(tile, label=label))
    FONT.write_text("\n".join(out_lines) + "\n")
    print(f"Updated {FONT}")
    print("Accent codepoints:")
    for accented, code in ACCENT_MAP.items():
        print(f"  {accented} = {code}")


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "show":
        show(int(sys.argv[2]))
    elif cmd == "addaccents":
        add_accents()
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
