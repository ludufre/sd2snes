#!/usr/bin/env python3
"""snesgfx.py - SNES tile/palette codec helpers for the theme tooling.

Covers exactly what pack_theme.py needs:
  * 8bpp planar tile decode/encode (64 bytes/tile, planes 0&1,2&3,4&5,6&7)
  * BGR555 <-> RGB
  * decode a linear (no tilemap) logo grid -> RGBA PIL image
  * refit an RGBA image to the fork logo format (16x7 8bpp tiles, 32-colour
    palette loaded at CGRAM 64, pixel value = 64+index, 0 = transparent)
"""
import struct

import numpy as np
from PIL import Image

FORK_LOGO_COLS = 32          # fork logo is 32x7 tiles = 256x56 px (full-width)
FORK_LOGO_ROWS = 7
FORK_LOGO_CGBASE = 64        # logo palette lives at CGRAM 64..95
FORK_LOGO_NCOLORS = 32       # 32-colour logo palette (64 bytes)


# ---- HDMA background gradient (hdma_pal_src): [count, colorLo, colorHi]* , 0 ----
def parse_hdma(d, off, maxn=130):
    """Parse a single-write colour-ramp HDMA table -> [(count, color15)]."""
    pos = off
    out = []
    for _ in range(maxn):
        if pos + 3 > len(d):
            break
        c = d[pos]
        if c == 0 or (c & 0x80):
            break
        col = struct.unpack("<H", d[pos + 1:pos + 3])[0]
        if col >= 0x8000:
            break
        out.append((c & 0x7f, col))
        pos += 3
    return out


def encode_hdma(stops):
    out = bytearray()
    for c, col in stops:
        out += bytes([c & 0x7f]) + struct.pack("<H", col & 0x7fff)
    out += b"\x00"
    return bytes(out)


def _hdma_is_ramp(stops):
    if len(stops) < 8:
        return False
    total = sum(c for c, _ in stops)
    if not (150 <= total <= 245):            # must cover ~one screen of scanlines
        return False
    cols = [c for _, c in stops]
    if len(set(cols)) < 4:                   # must vary (reject flat text regions)
        return False
    ch = lambda w, s: (w >> s) & 31
    smooth = sum(1 for a, b in zip(cols, cols[1:])
                 if all(abs(ch(a, s) - ch(b, s)) <= 7 for s in (0, 5, 10)))
    return smooth >= 1


def find_bg_gradient(d, gfxptr_slot5=None):
    """Locate the theme's background-gradient stops. v1.11.0 carries it at
    _GFXPTR_ slot 5 (authoritative); older builds (no table) are scanned for a
    colour ramp that spans ~one screen. Returns stops or None."""
    if gfxptr_slot5:
        st = parse_hdma(d, gfxptr_slot5)
        if st and 120 <= sum(c for c, _ in st) <= 245:
            return st
    best = None
    for o in range(0x1700, 0x2000):
        st = parse_hdma(d, o)
        if _hdma_is_ramp(st) and (best is None or len(st) > len(best)):
            best = st
    return best


def resample_gradient(theme_stops, fork_stops):
    """Recolour the fork's gradient (keep its per-scanline COUNTS, i.e. the fork
    menu's vertical band layout) using colours sampled from the theme gradient by
    vertical position. Returns stops the same length/shape as fork_stops."""
    theme_px = []
    for c, col in theme_stops:
        theme_px += [col] * max(c, 1)
    if not theme_px:
        return list(fork_stops)
    total_f = sum(c for c, _ in fork_stops)
    out = []
    cum = 0
    for c, _col in fork_stops:
        f = cum / max(total_f - 1, 1)
        ti = min(int(f * (len(theme_px) - 1)), len(theme_px) - 1)
        out.append((c, theme_px[ti]))
        cum += c
    return out


# ---- colour ----
def bgr555_to_rgb(w):
    return ((w & 31) * 255 // 31, ((w >> 5) & 31) * 255 // 31, ((w >> 10) & 31) * 255 // 31)


def rgb_to_bgr555(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def palette_rgb(pal_bytes):
    n = len(pal_bytes) // 2
    return [bgr555_to_rgb(struct.unpack("<H", pal_bytes[i * 2:i * 2 + 2])[0]) for i in range(n)]


# ---- 8bpp planar tile <-> 8x8 index array ----
def decode_tile_8bpp(t):
    px = np.zeros((8, 8), np.uint8)
    for y in range(8):
        for x in range(8):
            b = 7 - x
            v = 0
            for pl in range(4):
                v |= ((t[pl * 16 + 2 * y] >> b) & 1) << (pl * 2)
                v |= ((t[pl * 16 + 2 * y + 1] >> b) & 1) << (pl * 2 + 1)
            px[y, x] = v
    return px


def encode_tile_8bpp(px):
    """px: 8x8 uint8 index array -> 64 planar bytes."""
    t = bytearray(64)
    for y in range(8):
        for x in range(8):
            v = int(px[y, x])
            b = 7 - x
            for pl in range(4):
                if (v >> (pl * 2)) & 1:
                    t[pl * 16 + 2 * y] |= (1 << b)
                if (v >> (pl * 2 + 1)) & 1:
                    t[pl * 16 + 2 * y + 1] |= (1 << b)
    return bytes(t)


# ---- decode a linear logo grid (no tilemap) -> RGBA image ----
def decode_logo(data, off, cols, rows, pal_bytes, cgbase=0, transparent_index=0):
    """Decode `cols`x`rows` 8bpp tiles laid out row-major starting at `off`.
    Palette index (pixel - cgbase) selects a colour from pal_bytes; the
    `transparent_index` (after cgbase subtraction) maps to alpha 0."""
    pal = palette_rgb(pal_bytes)
    img = np.zeros((rows * 8, cols * 8, 4), np.uint8)
    for ti in range(cols * rows):
        t = data[off + ti * 64: off + ti * 64 + 64]
        if len(t) < 64:
            break
        px = decode_tile_8bpp(t)
        cy, cx = divmod(ti, cols)
        for y in range(8):
            for x in range(8):
                pidx = int(px[y, x]) - cgbase
                if pidx == transparent_index:
                    img[cy * 8 + y, cx * 8 + x] = (0, 0, 0, 0)
                elif 0 <= pidx < len(pal):
                    r, g, b = pal[pidx]
                    img[cy * 8 + y, cx * 8 + x] = (r, g, b, 255)
    return Image.fromarray(img, "RGBA")


# ---- refit an image to the fork logo format ----
def encode_fork_logo(img, keep_alpha=True):
    """img: PIL image (any size/mode). Resized to 256x56 and quantised to a
    32-colour palette. Returns (logo_pal_bytes[64], logo_tiles_bytes[14336]).

    Pixel encoding matches the fork: opaque pixel -> value 64+palidx, fully
    transparent pixel (alpha<128) -> value 0 (shows the menu background)."""
    W, H = FORK_LOGO_COLS * 8, FORK_LOGO_ROWS * 8   # 256 x 56
    img = img.convert("RGBA").resize((W, H), Image.LANCZOS)
    rgba = np.asarray(img)
    alpha = rgba[:, :, 3]
    opaque = (alpha >= 128) if keep_alpha else np.ones((H, W), bool)

    rgb = Image.fromarray(rgba[:, :, :3], "RGB")
    # quantise the whole frame to 32 colours, then build the index map
    # No dither: the header is a clean logo/wordmark; FS dither only grains it.
    q = rgb.quantize(colors=FORK_LOGO_NCOLORS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.asarray(q, np.uint8)                    # HxW, values 0..31
    qpal = q.getpalette() or []                      # RGB triples
    qpal = (qpal + [0] * (FORK_LOGO_NCOLORS * 3))[:FORK_LOGO_NCOLORS * 3]  # pad: image may have <32 colours

    logo_pal = bytearray()
    for i in range(FORK_LOGO_NCOLORS):
        r, g, b = qpal[i * 3], qpal[i * 3 + 1], qpal[i * 3 + 2]
        logo_pal += struct.pack("<H", rgb_to_bgr555(r, g, b))

    # build tile value map: transparent -> 0, opaque -> 64 + palidx
    val = np.where(opaque, idx.astype(np.uint16) + FORK_LOGO_CGBASE, 0).astype(np.uint8)

    tiles = bytearray()
    for ty in range(FORK_LOGO_ROWS):
        for tx in range(FORK_LOGO_COLS):
            block = val[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
            tiles += encode_tile_8bpp(block)
    return bytes(logo_pal), bytes(tiles)
