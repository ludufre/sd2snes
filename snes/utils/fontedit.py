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
    python3 fontedit.py addfrench          Insert French chars (è ù î ï ë û)
    python3 fontedit.py additalian         Insert Italian chars (ì ò È Ì Ò Ù)
    python3 fontedit.py addscrollbar       Write scrollbar glyphs (codes 16,17)
    python3 fontedit.py addprogressbar    Write progress-bar glyphs (codes 18,19)
    python3 fontedit.py export [opts]      Dump the font to an editable PNG
    python3 fontedit.py import <png> [opts]  Read a PNG sheet back into font.a65
    python3 fontedit.py freeslots          List slots with no glyph in them

The export/import pair is the pixel-editor round trip (Paint, Photoshop,
Aseprite, GIMP): `export` writes a 16x16 tile sheet plus an annotated guide,
`import` reads the edited sheet back. See the PNG SHEET section below for the
palette and the write-scope rules.

The PT-BR insertion writes 24 new tiles into empty slots and prints the
codepoint -> char mapping so const.a65 can reference them.

`addfrench` adds the DIAERESIS mark (ë ï) alongside the existing GRAVE/CIRC
marks and writes ONLY the 6 new slots (224-229), preserving every other tile
(so the hand-made Spanish glyphs and the art in 161-223 are untouched). At 8x8
the diaeresis and circumflex collapse to the same two-dot mark, so ë==ê and
ï==î byte-for-byte (see DIAERESIS below).

`additalian` follows the same targeted pattern for codes 230-235 and introduces
no new mark: every Italian glyph is the existing GRAVE over its base letter.

PNG SHEET
---------
`export` writes two files:

  font_sheet.png   the round-trip file: 16x16 tiles, no gutters, one image
                   pixel per font pixel (times --scale). THIS is the file to
                   edit and hand back to `import`. It mirrors font.a65 as it
                   stands, so a slot that still holds an old glyph shows it;
                   --blank hands over empty cells instead (PNG only, the .a65
                   changes on import).
  font_guide.png   read-only reference: same grid, but blown up and annotated
                   with the code of every cell, so it is obvious which cell is
                   which. Never imported.

The sheet is a 4-colour image and those four RGB values are the whole
contract; `import` maps every pixel back to a colour index by nearest match
and reports how many pixels were not an exact hit (antialiasing, a soft brush
or a JPEG round trip show up there):

  0 = magenta #ff00ff  transparent
  1 = white   #ffffff  glyph body
  2 = grey    #393939  outline / counter
  3 = grey    #a5a5a5  mid tone

Keep the pencil hard-edged and anti-aliasing off. Any indexed/RGB/RGBA PNG is
accepted on the way back, at any integer scale.

How the existing glyphs are shaded, worth copying for a new alphabet: the
letter itself is a fat white (1) shape, every side of it wrapped in the dark
outline (2) -- including the counters, so the hole in a 'B' is outline grey,
not transparent -- with the mid tone (3) dropped on single pixels where a
corner or a curve would otherwise look jagged. Transparent (0) only survives
in the outer corners and along the bottom row, which stays empty so stacked
menu rows do not touch. Copying an existing letter and reshaping its strokes
keeps a new script consistent far more easily than drawing one from scratch.

`import` defaults to --only 162-175,178-223,236-255 -- the free tail of the
table plus the dead katakana block (see RECYCLABLE_CODES: it is JIS X 0201
left over from the CP932 era, unreachable since 2010). A full-sheet write is
not the default because it would silently repaint the hand-made Spanish glyphs
and the window art whenever an editor shifted a colour. Widen it deliberately
(--only 130-159, --all) once the diff printed by --dry-run looks right.

That leaves 80 slots for a new script, comfortably more than a full Cyrillic
alphabet in both cases; `freeslots` prints the current tally.
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
    # French additions (codes 160-165 are NOT free -- 161-223 hold other art,
    # gameinfo reuses 160/161/176/177 for the chip icon OBJ. 224-255 are blank
    # and unreferenced, so the French block lives there):
    "è": 224, "ù": 225, "î": 226, "ï": 227, "ë": 228, "û": 229,
    # Italian additions. The lowercase graves the earlier blocks never needed
    # (à/è/ù already exist), plus the uppercase graves: Italian headers are drawn
    # in caps by the in-game menu and "E'" is not an acceptable stand-in for "È",
    # which opens a large share of sentences:
    "ì": 230, "ò": 231, "È": 232, "Ì": 233, "Ò": 234, "Ù": 235,
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
# Diaeresis / trema (ë ï, and the hand-made ü=156): two dots on the top row.
# At 8x8 the accent shares the letter's top row, so the diaeresis reduces to the
# same two-dot mark as the circumflex (cols 2 and 5) -- this matches the existing
# ü precedent (overlay "..#..#.." on 'u' reproduces tile 156 exactly). Consequence:
# ë is byte-identical to ê, and ï to î; there is no room to distinguish them.
DIAERESIS = "..#..#.."


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


# -- French accents (targeted: writes ONLY the 6 new slots) -----------------
# Unlike add_accents (which regenerates every ACCENT_MAP slot and would clobber
# the hand-made Spanish glyphs), add_french touches ONLY codes 224-229. The base
# letter is copied verbatim and a proven mark painted on row 0; every other tile
# in font.a65 is preserved byte-for-byte.
FRENCH_BASE = {
    "è": ("e", GRAVE),  "ù": ("u", GRAVE),
    "î": ("i", CIRC),   "û": ("u", CIRC),
    "ï": ("i", DIAERESIS), "ë": ("e", DIAERESIS),
}


def add_french():
    header, tiles = load_font()
    new_tiles = dict(enumerate(tiles))  # code -> tile (start from current file)
    for ch, (base_letter, mark) in FRENCH_BASE.items():
        code = ACCENT_MAP[ch]
        new_tiles[code] = pixels_to_tile(with_accent(base_letter, mark, tiles))

    out_lines = list(header)
    total = max(len(tiles), max(new_tiles) + 1)
    for code in range(total):
        tile = new_tiles.get(code, [0] * 16)
        label = "font" if code == 0 else None
        out_lines.extend(encode_tile_lines(tile, label=label))
    FONT.write_text("\n".join(out_lines) + "\n")
    print(f"Updated {FONT}")
    for ch, (base_letter, mark) in FRENCH_BASE.items():
        code = ACCENT_MAP[ch]
        print(f"  {ch} = {code} (base {base_letter!r})")
        print(render_ascii(tile_to_pixels(new_tiles[code])))


# -- Italian accents (targeted: writes ONLY the 6 new slots) ----------------
# Same discipline as add_french: touches ONLY codes 230-235, so the hand-made
# Spanish glyphs, the art in 161-223 and the French block stay byte-for-byte.
# Every mark here is the existing GRAVE, so no new accent art is introduced --
# the uppercase forms reuse the proven À (143) construction on their own base.
ITALIAN_BASE = {
    "ì": ("i", GRAVE), "ò": ("o", GRAVE),
    "È": ("E", GRAVE), "Ì": ("I", GRAVE),
    "Ò": ("O", GRAVE), "Ù": ("U", GRAVE),
}


def add_italian():
    header, tiles = load_font()
    new_tiles = dict(enumerate(tiles))  # code -> tile (start from current file)
    for ch, (base_letter, mark) in ITALIAN_BASE.items():
        code = ACCENT_MAP[ch]
        new_tiles[code] = pixels_to_tile(with_accent(base_letter, mark, tiles))

    out_lines = list(header)
    total = max(len(tiles), max(new_tiles) + 1)
    for code in range(total):
        tile = new_tiles.get(code, [0] * 16)
        label = "font" if code == 0 else None
        out_lines.extend(encode_tile_lines(tile, label=label))
    FONT.write_text("\n".join(out_lines) + "\n")
    print(f"Updated {FONT}")
    for ch, (base_letter, mark) in ITALIAN_BASE.items():
        code = ACCENT_MAP[ch]
        print(f"  {ch} = {code} (base {base_letter!r})")
        print(render_ascii(tile_to_pixels(new_tiles[code])))


# -- Scrollbar glyphs (Y-mode game-info full description) --------------------
# Two solid vertical bars used by gi_desc_scrollbar in gameinfo.a65. Both fill
# all 8 rows so adjacent cells join into a continuous bar. Codes 16/17 are in
# the 0..31 blank range, so no printable glyph is displaced.
#   code 16 = track: a thin 2px-wide line, color 1 (discreet).
#   code 17 = thumb: a 4px-wide bar,       color 3 (same as normal text).
SCROLLBAR_GLYPHS = {
    16: ("00011000", 1),
    17: ("00111100", 3),
}


def _bar_tile(pattern, color):
    """Build an 8x8 tile: the same horizontal `pattern` on every row, its set
    pixels painted `color`."""
    row = [color if ch == "1" else 0 for ch in pattern]
    return pixels_to_tile([row[:] for _ in range(8)])


# -- Progress bar glyphs (menu PCM player) -----------------------------------
# The scrollbar glyphs above are VERTICAL bars: the same pattern on all 8 rows,
# so cells stacked downwards join up. Laid out sideways they would read as dots,
# which is why a horizontal bar needs its own pair. These fill all 8 COLUMNS of
# a few centre rows instead, so cells placed side by side join into one line.
# Codes 18/19 continue in the same 0..31 blank range (nothing printable moves).
#   code 18 = track: 2 centre rows, color 1 (discreet).
#   code 19 = filled: 4 centre rows, color 3 (same as normal text).
PROGRESSBAR_GLYPHS = {
    18: (2, 1),
    19: (4, 3),
}


def _hbar_tile(thickness, color):
    """Build an 8x8 tile: `thickness` fully-lit centre rows, painted `color`."""
    top = (8 - thickness) // 2
    rows = []
    for y in range(8):
        lit = top <= y < top + thickness
        rows.append([color if lit else 0 for _ in range(8)])
    return pixels_to_tile(rows)


def add_progressbar():
    header, tiles = load_font()
    new_tiles = dict(enumerate(tiles))  # code -> tile
    for code, (thickness, color) in PROGRESSBAR_GLYPHS.items():
        new_tiles[code] = _hbar_tile(thickness, color)

    out_lines = list(header)
    total = max(len(tiles), max(new_tiles) + 1)
    for code in range(total):
        tile = new_tiles.get(code, [0] * 16)
        label = "font" if code == 0 else None
        out_lines.extend(encode_tile_lines(tile, label=label))
    FONT.write_text("\n".join(out_lines) + "\n")
    print(f"Updated {FONT}")
    for code in PROGRESSBAR_GLYPHS:
        print(f"  progress-bar glyph {code}:")
        print(render_ascii(tile_to_pixels(new_tiles[code])))


def add_scrollbar():
    header, tiles = load_font()
    new_tiles = dict(enumerate(tiles))  # code -> tile
    for code, (pattern, color) in SCROLLBAR_GLYPHS.items():
        new_tiles[code] = _bar_tile(pattern, color)

    out_lines = list(header)
    total = max(len(tiles), max(new_tiles) + 1)
    for code in range(total):
        tile = new_tiles.get(code, [0] * 16)
        label = "font" if code == 0 else None
        out_lines.extend(encode_tile_lines(tile, label=label))
    FONT.write_text("\n".join(out_lines) + "\n")
    print(f"Updated {FONT}")
    for code in SCROLLBAR_GLYPHS:
        print(f"  scrollbar glyph {code}:")
        print(render_ascii(tile_to_pixels(new_tiles[code])))


# -- PNG sheet round trip ---------------------------------------------------
# The font is 256 tiles laid out as a 16x16 grid, row-major, so cell (col,row)
# holds code row*16+col. The sheet has no gutters and no scaling by default:
# one image pixel IS one font pixel, which is what a pixel editor wants.

SHEET_COLS = 16
SHEET_ROWS = 16
TILE_PX = 8
SHEET_W = SHEET_COLS * TILE_PX
SHEET_H = SHEET_ROWS * TILE_PX

# Colour index -> RGB, matching menu palette 0 (palette.a65, BGR555) so the
# sheet looks like the real thing, except index 0 which is drawn as magenta:
# on screen it is transparent, and a chroma-key colour is the one thing an
# editor will never blend into. Import resolves each pixel by nearest match.
SHEET_PALETTE = [
    (0xff, 0x00, 0xff),  # 0 transparent
    (0xff, 0xff, 0xff),  # 1 glyph body
    (0x39, 0x39, 0x39),  # 2 outline / counter (the dark shape around the body)
    (0xa5, 0xa5, 0xa5),  # 3 mid tone, softens corners and curves
]

# Codes that already have a job and must not be recycled as letter slots:
# 0/1 terminate a string, 2..22 ($02..$16) are the sysinfo placeholder bytes,
# 16/17 are the scrollbar glyphs, and 32 is the space (blank by design).
#
# 160/161/176/177 are a different kind of taken: gameinfo does not draw those
# glyphs, it claims the VRAM they occupy. The chip icon is a 16x16 OBJ DMA'd
# straight over their tile slots (GI_CHIP_VRAM0 $4A00 = 160/161 top row,
# GI_CHIP_VRAM1 $4B00 = 176/177 bottom row, gameinfo.a65). A letter parked
# there would survive in font.a65 and still be destroyed on screen the moment
# a game info screen opened, so they stay out of the alphabet.
RESERVED_CODES = set(range(0, 33)) | {160, 161, 176, 177}

# Dead weight, free to overwrite: 161-223 is JIS X 0201 half-width katakana at
# its exact encoding positions ($A1-$DF), drawn in 2009 back when FatFs here
# was set to CP932 so Japanese file names would render in the browser. ikari
# moved _CODE_PAGE to 1252 in 2010 and it never went back, so nothing can emit
# these any more: a Japanese LFN no longer converts and FatFs substitutes '?'
# (ff.c), the internal ROM header title -- the other JIS X 0201 source -- is
# never displayed (smc.c only pattern-matches it), and no menu string uses the
# range. 176/177 stay out of it: gameinfo reuses them for the chip icon.
#
# The range is not inert, it is actively wrong: under CP1252 an accented file
# name lands right here with no translation to our own accent slots, so
# "Cafe" with an acute renders tile 233 and an A-grave renders katakana. A new
# alphabet drawn over it replaces garbage with garbage in that case.
#
# 162-175 is the same block and equally dead -- the JIS punctuation and the
# small kana -- and nothing references them either (the near hits are the $aa
# handshake NACK, an X coordinate of 172 in the sprite table, and the igmenu
# $a5 magic; none is a tile index). Window borders are tiles 20-27, not these.
RECYCLABLE_CODES = set(range(162, 176)) | set(range(178, 224))

# What a sheet import may write unasked: the free tail plus the katakana
# block. Everything else -- the hand-made Spanish glyphs, the window art --
# needs an explicit --only/--all, so an editor shifting a colour cannot
# quietly repaint them.
DEFAULT_IMPORT_RANGE = "162-175,178-223,236-255"

DEFAULT_SHEET = "font_sheet.png"
DEFAULT_GUIDE = "font_guide.png"

# Above this, an import prints the code list instead of every glyph.
PREVIEW_MAX_TILES = 12


def _require_pil():
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401
    except ImportError:
        sys.exit("Pillow is required for the PNG sheet: pip3 install Pillow")
    from PIL import Image, ImageDraw, ImageFont
    return Image, ImageDraw, ImageFont


def write_font(header, tiles):
    """Write `tiles` (code -> 16 bytes) back out in font.a65 style, keeping the
    header verbatim and the `font` label on tile 0."""
    out_lines = list(header)
    total = max(256, max(tiles) + 1) if isinstance(tiles, dict) else len(tiles)
    get = tiles.get if isinstance(tiles, dict) else (lambda c, d=None: tiles[c])
    for code in range(total):
        tile = get(code, None) or [0] * 16
        out_lines.extend(encode_tile_lines(tile, label="font" if code == 0 else None))
    FONT.write_text("\n".join(out_lines) + "\n")


def parse_ranges(spec):
    """'236-255,130' -> {130, 236..255}. 'all' -> every code."""
    if spec.strip().lower() == "all":
        return set(range(256))
    codes = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            lo, hi = int(lo), int(hi)
            if lo > hi:
                lo, hi = hi, lo
            codes.update(range(lo, hi + 1))
        else:
            codes.add(int(part))
    bad = [c for c in codes if not 0 <= c <= 255]
    if bad:
        sys.exit(f"code out of range 0-255: {sorted(bad)}")
    return codes


def slot_status(code, tile):
    """'reserved' | 'free' | 'recyclable' | 'used' -- what the guide colours
    the cell by, and what freeslots counts.

    'recyclable' means the dead glyph is still sitting there: draw over it.
    Once the cell is empty (--blank, or an import that cleared it) it is
    plain 'free', because at that point it is indistinguishable from a slot
    that was never used -- two colours for one state only confuse."""
    if code in RESERVED_CODES:
        return "reserved"
    if code in RECYCLABLE_CODES:
        return "recyclable" if any(tile) else "free"
    return "used" if any(tile) else "free"


def tile_image(Image, tile, bg=None):
    """8x8 RGB image of one tile; colour 0 takes `bg` (default palette 0)."""
    pixels = tile_to_pixels(tile)
    bg = bg or SHEET_PALETTE[0]
    im = Image.new("RGB", (TILE_PX, TILE_PX))
    im.putdata([
        bg if c == 0 else SHEET_PALETTE[c]
        for row in pixels for c in row
    ])
    return im


def export_sheet(sheet_path, guide_path, scale, blank=()):
    Image, ImageDraw, ImageFont = _require_pil()
    _, tiles = load_font()
    tiles = (tiles + [[0] * 16] * 256)[:256]
    # --blank empties cells in the PNG only; font.a65 is not touched until an
    # import runs. Handing over a clean cell beats making the artist erase the
    # old glyph first.
    for code in blank:
        tiles[code] = [0] * 16

    sheet = Image.new("RGB", (SHEET_W, SHEET_H), SHEET_PALETTE[0])
    for code in range(256):
        col, row = code % SHEET_COLS, code // SHEET_COLS
        sheet.paste(tile_image(Image, tiles[code]), (col * TILE_PX, row * TILE_PX))
    if scale > 1:
        sheet = sheet.resize((SHEET_W * scale, SHEET_H * scale), Image.NEAREST)
    sheet.save(sheet_path)
    print(f"wrote {sheet_path} ({sheet.width}x{sheet.height}, scale {scale})")

    if guide_path:
        build_guide(Image, ImageDraw, ImageFont, tiles, guide_path)
        print(f"wrote {guide_path} (reference only -- do not import)")

    counts = {"reserved": 0, "free": 0, "recyclable": 0, "used": 0}
    for code in range(256):
        counts[slot_status(code, tiles[code])] += 1
    avail = counts["free"] + counts["recyclable"]
    print(f"slots: {counts['used']} used, {counts['reserved']} reserved, "
          f"{avail} available to draw in"
          + (f" ({counts['recyclable']} still holding a dead glyph)"
             if counts["recyclable"] else ""))
    if blank:
        print(f"blanked {len(blank)} cell(s) in the PNG only -- importing the "
              f"sheet as-is clears them in font.a65 too")
    print(f"import writes {DEFAULT_IMPORT_RANGE} unless --only/--all says otherwise")


# Guide geometry: a 4x blow-up of each tile plus a caption line under it.
GUIDE_SCALE = 4
GUIDE_PAD = 3
GUIDE_CAPTION = 13
GUIDE_MARGIN = 10
GUIDE_HEADER = 32
STATUS_BG = {
    "reserved": (72, 20, 20),
    "free": (16, 66, 24),
    "recyclable": (72, 60, 12),
    "used": (34, 34, 40),
}


def _guide_font(ImageFont, size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:      # Pillow < 10 has no size argument
        return ImageFont.load_default()


def _caption(code):
    """Short label: the code, plus the character it stands for when there is
    one (ASCII, or an entry of ACCENT_MAP)."""
    accents = {v: k for k, v in ACCENT_MAP.items()}
    if 33 <= code < 127:
        return f"{code} {chr(code)}"
    if code in accents:
        return f"{code} {accents[code]}"
    return str(code)


def build_guide(Image, ImageDraw, ImageFont, tiles, path):
    cell_w = TILE_PX * GUIDE_SCALE + GUIDE_PAD * 2
    cell_h = TILE_PX * GUIDE_SCALE + GUIDE_PAD * 2 + GUIDE_CAPTION
    w = GUIDE_MARGIN * 2 + cell_w * SHEET_COLS
    h = GUIDE_MARGIN * 2 + GUIDE_HEADER + cell_h * SHEET_ROWS
    im = Image.new("RGB", (w, h), (12, 12, 14))
    draw = ImageDraw.Draw(im)
    font = _guide_font(ImageFont, 11)

    draw.text((GUIDE_MARGIN, GUIDE_MARGIN),
              "font.a65 slot map -- reference only, draw in font_sheet.png",
              fill=(200, 200, 200), font=font)
    draw.text((GUIDE_MARGIN, GUIDE_MARGIN + 14),
              "green: draw here    olive: dead glyph, draw over it    "
              "red: reserved    grey: in use",
              fill=(150, 150, 150), font=font)

    for code in range(256):
        col, row = code % SHEET_COLS, code // SHEET_COLS
        x = GUIDE_MARGIN + col * cell_w
        y = GUIDE_MARGIN + GUIDE_HEADER + row * cell_h
        status = slot_status(code, tiles[code])
        bg = STATUS_BG[status]
        draw.rectangle([x, y, x + cell_w - 2, y + cell_h - 2], fill=bg)
        glyph = tile_image(Image, tiles[code], bg=bg).resize(
            (TILE_PX * GUIDE_SCALE, TILE_PX * GUIDE_SCALE), Image.NEAREST)
        im.paste(glyph, (x + GUIDE_PAD, y + GUIDE_PAD))
        draw.text((x + GUIDE_PAD, y + GUIDE_PAD + TILE_PX * GUIDE_SCALE),
                  _caption(code), fill=(210, 210, 210), font=font)
    im.save(path)


def sheet_to_tiles(path):
    """Read a sheet PNG back into 256 tiles. Returns (tiles, inexact_pixels)."""
    Image, _, _ = _require_pil()
    im = Image.open(path)
    if im.mode not in ("RGB", "RGBA"):
        im = im.convert("RGBA")
    w, h = im.size
    if w % SHEET_W or h % SHEET_H or w // SHEET_W != h // SHEET_H:
        sys.exit(f"{path}: expected a {SHEET_W}x{SHEET_H} sheet at an integer "
                 f"scale (128x128, 256x256, 1024x1024, ...), got {w}x{h}")
    scale = w // SHEET_W

    alpha = im.getchannel("A").tobytes() if im.mode == "RGBA" else None
    rgb = im.convert("RGB").tobytes()
    cache = {}
    inexact = 0
    indices = []
    for n in range(w * h):
        if alpha is not None and alpha[n] < 128:
            indices.append(0)            # transparent counts as background
            continue
        p = rgb[n * 3:n * 3 + 3]
        hit = cache.get(p)
        if hit is None:
            d, i = min(
                ((p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2, i)
                for i, c in enumerate(SHEET_PALETTE)
            )
            hit = (i, d == 0)
            cache[p] = hit
        indices.append(hit[0])
        if not hit[1]:
            inexact += 1

    tiles = []
    for code in range(256):
        col, row = code % SHEET_COLS, code // SHEET_COLS
        pixels = []
        for py in range(TILE_PX):
            line = []
            for px in range(TILE_PX):
                x0 = (col * TILE_PX + px) * scale
                y0 = (row * TILE_PX + py) * scale
                if scale == 1:
                    line.append(indices[y0 * w + x0])
                    continue
                # majority vote over the block, so a soft edge inside one font
                # pixel cannot flip it
                votes = {}
                for yy in range(y0, y0 + scale):
                    base = yy * w
                    for xx in range(x0, x0 + scale):
                        v = indices[base + xx]
                        votes[v] = votes.get(v, 0) + 1
                line.append(max(votes.items(), key=lambda kv: (kv[1], -kv[0]))[0])
            pixels.append(line)
        tiles.append(pixels_to_tile(pixels))
    return tiles, inexact


def import_sheet(path, scope, dry_run):
    header, current = load_font()
    current = (current + [[0] * 16] * 256)[:256]
    new, inexact = sheet_to_tiles(path)

    changed = [c for c in range(256) if new[c] != current[c]]
    in_scope = [c for c in changed if c in scope]
    out_scope = [c for c in changed if c not in scope]

    if inexact:
        print(f"note: {inexact} pixel(s) were not an exact palette colour and "
              f"were snapped to the nearest one (anti-aliasing?)")
    if out_scope:
        print(f"skipped {len(out_scope)} changed tile(s) outside the write "
              f"scope: {out_scope}")
        print("       widen it with --only/--all if those edits are intended")
    if not in_scope:
        print("no in-scope tile changed; font.a65 left alone")
        return

    reserved_hits = [c for c in in_scope if c in RESERVED_CODES]
    if reserved_hits:
        print(f"WARNING: writing reserved code(s) {reserved_hits} -- "
              f"see RESERVED_CODES")

    print(f"{len(in_scope)} tile(s) to write: {in_scope}")
    if len(in_scope) <= PREVIEW_MAX_TILES:
        for code in in_scope:
            print(f"  {_caption(code)}:")
            print(render_ascii(tile_to_pixels(new[code])))
    else:
        blanked = [c for c in in_scope if not any(new[c])]
        if blanked:
            print(f"  ({len(blanked)} of them are being cleared: {blanked})")
        print(f"  (too many to preview; `show <code>` renders one)")

    if dry_run:
        print("--dry-run: nothing written")
        return
    merged = dict(enumerate(current))
    for code in in_scope:
        merged[code] = new[code]
    write_font(header, merged)
    print(f"Updated {FONT}")


def _runs(codes):
    runs = []
    for c in codes:
        if runs and runs[-1][1] == c - 1:
            runs[-1][1] = c
        else:
            runs.append([c, c])
    return runs


def free_slots():
    _, tiles = load_font()
    tiles = (tiles + [[0] * 16] * 256)[:256]
    total = 0
    for status, label in (("free", "empty"),
                          ("recyclable", "reusable (dead katakana)")):
        codes = [c for c in range(256) if slot_status(c, tiles[c]) == status]
        total += len(codes)
        print(f"{len(codes)} {label} slot(s):")
        for lo, hi in _runs(codes):
            span = f"{lo}-{hi}" if hi > lo else f"{lo}"
            print(f"  {span}  ({hi - lo + 1} slot(s))")
    print(f"{total} slot(s) available in total")


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "show":
        show(int(sys.argv[2]))
    elif cmd == "addaccents":
        add_accents()
    elif cmd == "addfrench":
        add_french()
    elif cmd == "additalian":
        add_italian()
    elif cmd == "addscrollbar":
        add_scrollbar()
    elif cmd == "addprogressbar":
        add_progressbar()
    elif cmd == "export":
        import argparse
        ap = argparse.ArgumentParser(prog="fontedit.py export")
        ap.add_argument("-o", "--out", default=DEFAULT_SHEET,
                        help=f"sheet PNG to write (default {DEFAULT_SHEET})")
        ap.add_argument("-g", "--guide", default=DEFAULT_GUIDE,
                        help=f"annotated reference PNG (default {DEFAULT_GUIDE}; "
                             f"pass '' to skip)")
        ap.add_argument("-s", "--scale", type=int, default=1,
                        help="image pixels per font pixel (default 1)")
        ap.add_argument("-b", "--blank", metavar="RANGES",
                        help="empty these cells in the PNG (not in font.a65), "
                             "e.g. '178-223' or the keyword 'recyclable' for "
                             "the dead katakana block")
        a = ap.parse_args(sys.argv[2:])
        if a.scale < 1:
            sys.exit("--scale must be >= 1")
        if a.blank and a.blank.strip().lower() == "recyclable":
            blank = sorted(RECYCLABLE_CODES)
        else:
            blank = sorted(parse_ranges(a.blank)) if a.blank else []
        export_sheet(a.out, a.guide or None, a.scale, blank)
    elif cmd == "import":
        import argparse
        ap = argparse.ArgumentParser(prog="fontedit.py import")
        ap.add_argument("png")
        ap.add_argument("--only", default=DEFAULT_IMPORT_RANGE,
                        help=f"codes this import may write, e.g. '236-255,130' "
                             f"(default {DEFAULT_IMPORT_RANGE})")
        ap.add_argument("--all", action="store_true",
                        help="write every changed tile (shorthand for --only all)")
        ap.add_argument("-n", "--dry-run", action="store_true",
                        help="show the diff, write nothing")
        a = ap.parse_args(sys.argv[2:])
        import_sheet(a.png, parse_ranges("all" if a.all else a.only), a.dry_run)
    elif cmd == "freeslots":
        free_slots()
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
