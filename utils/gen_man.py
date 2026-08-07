#!/usr/bin/env python3
"""Generate a .man in-game Manual/Guide asset for the sd2snes+ Manual viewer.

CICLO 2 CONTRATO RECONCILIADO (IN-GAME-MENU-PLANO.md): version byte stays 1
FOREVER pre-release (the LAYOUT is free to change -- it's just regenerated
assets, never a "v2" in name/byte). This encodes the reconciled format:
  - header 40B (was 16B): adds flags (zoom bit), zoom_nblocks, and a 24B
    FONT-ENCODED title (ACCENTS scheme, mirrors snes/utils/build_const.py).
  - index entries keep 8B but the reserved byte becomes `zflags` (zoom
    metadata: bit0=is-zoom bit1=column bit2=row).
  - optional --zoom: a SEAM-LOCKED re-render of each band at 512-wide (same cut
    points, doubled) so the viewer can offer a legible 2x zoom without
    re-deciding line breaks. The zoom section is a SCROLLABLE 4bpp page (see
    below), not a grid of screens.
  - multiple guides per ROM are just multiple .man files side by side
    (<stem>.man + <stem>.NN.man, NN 02..08) -- out of scope for this
    single-file encoder; the title lives in EACH file's header.

The Manual viewer (in-game overlay, GUIDES tab) is a mode-3 8bpp PALETTED
image viewer, streamed block-by-block from the SD card while the game is
frozen. This host converter renders a PDF manual FIT-TO-WIDTH 256px (the
geometry proven legible in the design experiments) and slices each page into
224px-tall "bands", each band a self-contained fixed-size block ready for a
straight DMA to VRAM.

Pipeline (encode):
  pdftoppm -png -scale-to-x 256 -scale-to-y -1 <pdf>  ->  one 256xH image / page.
  Per page: cut into 224px bands.  The band boundary is SMART-SEAMed: the cut is
    snapped to the cleanest (fewest dark pixels) scanline in [224-16, 224], so a
    text line is never split across two bands.  Last band of a page is partial.
  Per band: PIL median-cut to 256 colors (no dither), snap the 256 palette
    entries to 15-bit BGR555, pad the band to 224px with the dominant background
    color, emit 896 tiles (32x28) of 8bpp indices in TILE ORDER.
  --zoom (opt-in): ALSO render at 512-wide (2x). The zoom image is the WHOLE
    PDF PAGE, NOT a band: the viewer pans continuously over all of it with
    hardware scroll. (Cutting zoom at band boundaries made the view jump at the
    half-way point of every page -- do not reintroduce that.) A page taller than
    the resident budget is split into several zoom pages.
  --spread auto|on|off (default auto): a PDF page whose aspect (w/h) crosses
    SPREAD_AR is a 2-page spread scan laid flat; fit-width would squeeze each
    printed page into HALF the pixel budget (illegible after quantization).
    Split pages are re-rendered at double width (512/1024) and emitted as TWO
    pages (left, right) -- each half IS one printed page, so the emitted
    sequence stays the book's own pagination and each gets the full budget.

Block layout (fixed 57856 B = 113 * 512, sector-aligned; 1x and zoom blocks
are byte-for-byte the SAME shape, so the viewer's block-DMA path is unchanged
by zoom):
  palette  256 * 2B  BGR555 little-endian   (512 B)
  tiles    896 * 64B 8bpp tile-order         (57344 B)

Container (.man), header 40B:
  off  size  field
  +0   4     "MANL"                      magic
  +4   1     ver = 1                     (INALTERADO -- maintainer rule)
  +5   1     bpp = 8                     (INALTERADO)
  +6   1     npages                      pages in the PDF (1..255)
  +7   1     flags                       bit0 = LEGACY quadrant zoom (never set
                                         by this encoder any more -- always 0)
                                         bit1 = scrollable zoom section present
  +8   2     page_w = 256  (LE)
  +10  2     band_h = 224  (LE)
  +12  2     nblocks       (LE)          1x blocks (page/band order)
  +14  2     rsvd = 0      (LE)          (was zoom_nblocks, quadrant scheme)
  +16  24    title[24]                   FONT-ENCODED (ACCENTS), NUL-terminated

index (8B/entry, starts at +40, entries 0..nblocks-1 = the 1x stream in
page/band order). UNCHANGED in shape and content -- see compat note below.
  offset:u32 page:u8 block:u8 content_rows:u8 rsvd:u8
Index padded so the first block starts at a 512-byte boundary.

Zoom section (scrollable). The viewer holds ONE zoom page resident in PSRAM and
pans over ALL of it with hardware scroll. A zoom page is a WHOLE PDF PAGE at 2x
(512 x 2*page_h), cut into 8px tile ROWS -- it is NOT a 1x band. Only a page
taller than ZMAX_ROWS*8 px is split into consecutive zoom pages.

  Z0 = 40 + nblocks*8
  zoom sub-header (32B @ Z0):
    +0  4  "ZOOM"        +4  1  zbpp = 4        +5  1  zpal_count = 8
    +6  1  ztiles_per_row = 64                  +7  1  zattr_stride = 64
    +8  2  zrow_bytes = 2048 (LE)               +10 2  zpal_bytes = 256 (LE)
    +12 4  nzrows (LE)   total tile rows in the document
    +16 2  zmax_rows     max nrows over all pages (viewer/MCU clamp)
    +18 2  rsvd = 0      +20 4  pagedir_ofs (absolute)
    +24 2  zpages        number of zoom pages   +26 2 rsvd
    +28 4  blockmap_ofs (absolute)
  page directory (20B/entry, zpages entries @ pagedir_ofs), offsets ABSOLUTE:
    +0 4 tile_ofs  +4 4 attr_ofs  +8 4 pal_ofs  +12 2 nrows  +14 2 pix_h
    +16 2 first_block   the earliest 1x block showing this page's content (the
                        viewer returns to it when leaving zoom after page turns)
    +18 1 pdf_page      0-based PDF page, for the "Pg X/N" HUD
    +19 1 nblk_in_page  how many 1x blocks cover this page, so leaving zoom can return to the
                        band you were actually looking at instead of always the first one
  block map (4B/entry, nblocks entries @ blockmap_ofs) -- the 1x -> 2x entry
  point, i.e. where panning should start when you press Y on 1x block b:
    +0 2 zoom page      +2 2 Y within that page, in 2x pixels

Per page, every base sector-aligned:
  palette  8*16 BGR555 LE = 256B (padded to 512)
  attrs    nrows * 64B -- ONE BYTE PER TILE, value = palette << 2 (pre-shifted;
           the viewer must rewrite the tilemap row on every row load anyway
           because the palette bits change when a ring slot is reused, so a
           pre-shifted byte makes that work cheap instead of a bit-unpack)
  tiles    nrows * 2048B. Each row is PRE-SPLIT: 32 tiles (cols 0-31, 1024B)
           then 32 tiles (cols 32-63, 1024B). The viewer's 512px width does not
           fit one BG layer (a tilemap entry's character field is 10 bits = 1024
           tiles max), so the halves land on BG1 and BG2 and are joined by a
           window -- pre-splitting makes each half one contiguous MCU copy.

Compat (both directions clean, which is why bit0 is retired rather than reused):
  old firmware + new file -> bit0=0 and +14=0 -> "no zoom"; the 1x index stride
    and block format are byte-identical, so 1x viewing still works fully.
  new firmware + old file -> bit1=0 -> no zoom; 1x works. New firmware must
    ignore bit0 entirely so it never feeds legacy 8bpp quadrants to the new path.

Usage:
  gen_man.py <input.pdf> -o <out.man> [--title "Texto"] [--zoom]
                                      [--spread auto|on|off] [--no-sharpen]
  gen_man.py --verify <man>                        structural audit (no -o needed)
  gen_man.py --verify <man> --block N -o out.png   round-trip a 1x block to PNG
  gen_man.py --verify <man> --zoom-page P -o out.png   round-trip a zoom page
  gen_man.py --verify <man> --zoom-row R -o out.png    round-trip one 512x8 row
"""
import os, re, sys, struct, argparse, subprocess, tempfile, glob
from pathlib import Path
from PIL import Image, ImageFilter
import numpy as np
# reuse the fork's canonical 8bpp PLANAR tile codec (bitplanes interleaved, MSB-first)
# so the .man tiles match what the mode-3 BG1 8bpp viewer DMAs straight to VRAM.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snesgfx import (encode_tile_8bpp, decode_tile_8bpp,
                     encode_tiles_4bpp, decode_tiles_4bpp)
# canonical font glyph map (accented characters) -- SINGLE SOURCE OF TRUTH for
# every host tool that font-encodes text for the in-game font (menu i18n, and
# now the .man title). Import, never reimplement/duplicate this table.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "snes", "utils"))
from build_const import ACCENTS, DECODE

PAGE_W      = 256
BAND_H      = 224
TILES_W     = PAGE_W // 8            # 32
TILES_H     = BAND_H // 8            # 28
NTILES      = TILES_W * TILES_H      # 896
TILE_BYTES  = 64
PAL_BYTES   = 256 * 2               # 512
BLOCK_BYTES = PAL_BYTES + NTILES * TILE_BYTES   # 57856
HDR_BYTES   = 40
TITLE_OFS   = 16
TITLE_BYTES = 24
HDR_STRUCT  = "<4sBBBBHHHH%ds" % TITLE_BYTES   # magic,ver,bpp,npages,flags,page_w,band_h,nblocks,zoom_nblocks,title
IDX_ENTRY   = 8
SECTOR      = 512
SEAM_WINDOW = 16                    # snap only earlier (keeps band <= 224)
DARK_THRESH = 128                   # grayscale below this = "ink"
# The 1x view is mode 3 (BG1 8bpp), where the BG reaches ALL 256 CGRAM entries -- there is no
# spare colour for the HUD, so the viewer pokes the top two entries with its ink/box colours.
# Quantise to 254 and leave 254/255 UNUSED so that poke is free instead of destructive: with 256
# the median cut happily put pure black at index 255 (22250 px of a cover page), and the HUD
# turned the whole black background grey and speckled the artwork edges white. 254 vs 256 colours
# is imperceptible; losing the background colour is not. (The 2x view has no such problem: its
# 4bpp BG only reaches CGRAM 0-127 and the HUD lives in the OBJ palettes at 128+.)
NCOLORS_1X  = 254
HUD_CG_INK  = 254                   # lockstep with MN_HUD_CG_INK / MN_HUD_CG_BOX in igmenu.a65
HUD_CG_BOX  = 255
FLAG_ZOOM   = 0x01                  # LEGACY quadrant zoom -- retired, never set
FLAG_ZOOM2  = 0x02                  # scrollable 4bpp zoom section present

# ---- scrollable zoom section ----
ZOOM_W       = PAGE_W * 2                       # 512
ZTILES_W     = ZOOM_W // 8                      # 64 tiles across
ZTILE_BYTES  = 32                               # 4bpp
ZROW_BYTES   = ZTILES_W * ZTILE_BYTES           # 2048 = exactly 4 sectors
ZATTR_STRIDE = ZTILES_W                         # 64 attr bytes / tile row
ZPAL_COUNT   = 8                                # 8 palettes...
ZPAL_SIZE    = 16                               # ...of 16 colours = 128
ZPAL_BYTES   = ZPAL_COUNT * ZPAL_SIZE * 2       # 256
ZPAL_STRIDE  = SECTOR                           # padded to one sector
# A zoom page is a WHOLE PDF PAGE at 2x, NOT a 1x band. Panning must be continuous over the
# entire page -- cutting it at band boundaries is what made the view jump at the half-way point.
# The cap is the PSRAM budget: tiles A ($C50000, 96*1024) + tiles B ($C68000, 96*1024) exactly
# fill $C50000..$C7FFFF, with the tilemap and palette in the $C4 hole. 96 rows = 768px of 2x
# content, which covers Letter (83) and A4 (91). A taller page is SPLIT into several zoom pages
# (the old jump, but only for pathological scans instead of every page).
ZMAX_ROWS    = 96
ZCHUNK_PX    = ZMAX_ROWS * 8                    # 2x pixels per zoom page at most
ZHDR_BYTES   = 32
ZHDR_STRUCT  = "<4sBBBBHHIHHIHHI"               # ...+ zpages, rsvd, blockmap_ofs
ZDIR_ENTRY   = 20
ZDIR_STRUCT  = "<IIIHHHBB"                      # tile_ofs, attr_ofs, pal_ofs, nrows, pix_h,
                                                # first_block, pdf_page, nblk_in_page
ZMAP_ENTRY   = 4
ZMAP_STRUCT  = "<HH"                            # per 1x block: zoom page, Y within it (2x px)
# --- scale-1 scrollable section (the 1x view, same machinery at 256px wide) ---------------------
# 32 tiles per row means a 29-row ring is 928 tiles, so the 1x fits on ONE BG layer -- no split,
# no window. Rows are 1024B and a whole page (<=64 rows) fits in PSRAM bank $C3, which the retired
# 8bpp block used to occupy. 64 rows = 512px at 1x, covering Letter (47), A4 (46) and Legal (53).
S1TILES_W    = PAGE_W // 8                      # 32
S1ROW_BYTES  = S1TILES_W * ZTILE_BYTES          # 1024 = 2 sectors
S1ATTR_STRIDE = S1TILES_W                       # 32 attr bytes / tile row
S1MAX_ROWS   = 64                               # PSRAM capacity clamp (bank $C3), NOT the cut size
# SCL1 chunk height: 48 rows = 384 1x px = HALF a zoom chunk (768 2x px). The viewer pairs 1x page p
# with zoom page p (Y2 = 2*Y1) with NO cross-count fallback, so both sections MUST cut into the same
# number of chunks for any page height: ceil(H1/384) == ceil(2*H1/768). Cutting at the 64-row
# capacity instead breaks the pairing for tall pages (H1 in (384,512]: 1 SCL1 chunk vs 2 zoom
# chunks -> the A toggle on the 2nd zoom page has no 1x page to return to).
S1CHUNK_ROWS = 48
S1HDR_MAGIC  = b"SCL1"
# A page wider than SPREAD_AR * height is a 2-page spread scan laid flat (see --spread). 1.6 catches
# real spreads (2 landscape pages ~2.8, 2 squarish ~1.8) while a single landscape A4 (1.414) stays
# whole; 2 portrait pages side by side also land at ~1.414 -- that false negative is what
# --spread on is for. Splitting is RECURSIVE: a piece still at/over the threshold is halved again
# (cover WRAPS scan at ~4.8:1 and fold-outs at ~3.3:1 -- one halving leaves them at 2.4/1.65, still
# a sliver on screen), capped at SPREAD_MAX_SPLITS halvings (4 pieces covers aspect < 6.4).
SPREAD_AR    = 1.6
SPREAD_MAX_SPLITS = 2


def spread_split_factor(width, height):
    """Number of halvings (0..SPREAD_MAX_SPLITS) so every emitted piece is < SPREAD_AR."""
    k = 0
    ar = width / height if height else 0.0
    while k < SPREAD_MAX_SPLITS and ar >= SPREAD_AR:
        ar /= 2.0
        k += 1
    return k
ZLLOYD_ITERS = 6                                # fixed count -> deterministic output.
# Measured on a scanned 32-page manual (the hard case): 1 iter = 26.08 dB mean over the 5 worst
# pages, 3 = 26.37, 6 = 26.69, 12 = 27.04, at 0.07 / 0.08 / 0.10 / 0.13 s per page. Returns are
# diminishing AND non-monotonic per page (Lloyd on a discrete assignment can trade one page for
# another), so this is not a knob to keep turning -- 6 is where it stops being free.

assert BLOCK_BYTES == 57856 and BLOCK_BYTES % SECTOR == 0
assert HDR_BYTES == TITLE_OFS + TITLE_BYTES == 40
assert struct.calcsize(HDR_STRUCT) == HDR_BYTES
assert struct.calcsize(ZHDR_STRUCT) == ZHDR_BYTES
assert struct.calcsize(ZDIR_STRUCT) == ZDIR_ENTRY
assert ZROW_BYTES % SECTOR == 0 and ZPAL_BYTES <= ZPAL_STRIDE

# ---------------------------------------------------------------- title font encode ----
def font_encode_title(text):
    """UTF-8 text -> up to TITLE_BYTES-1 font-code bytes, NUL-padded to TITLE_BYTES.
    ASCII passthrough + the canonical ACCENTS map (imported from build_const.py --
    NOT reimplemented here). A character with no font mapping is a hard error
    (guard): silently dropping it would ship a title with an invisible hole,
    indistinguishable from truncation."""
    out = bytearray()
    truncated = False
    for ch in text:
        if len(out) >= TITLE_BYTES - 1:
            truncated = True
            break
        if ch in ACCENTS:
            out.append(ACCENTS[ch])
        elif 0x20 <= ord(ch) < 0x7f:
            out.append(ord(ch))
        else:
            raise SystemExit(f"--title: character {ch!r} (U+{ord(ch):04X}) has no font "
                              f"mapping (see snes/utils/build_const.py ACCENTS)")
    if truncated:
        print(f"warning: title truncated to {TITLE_BYTES - 1} glyphs: {text!r}", file=sys.stderr)
    out += b"\x00" * (TITLE_BYTES - len(out))
    return bytes(out)


def font_decode_title(blob24):
    """Inverse of font_encode_title (best-effort, for --verify display)."""
    out = ""
    for b in blob24:
        if b == 0:
            break
        out += DECODE.get(b, chr(b) if 0x20 <= b < 0x7f else "?")
    return out


def derive_title(out_path):
    """Default title when --title is omitted: Title-Case of the output stem, with
    a trailing 2-digit multi-guide suffix (<stem>.NN.man) stripped first."""
    stem = Path(out_path).stem  # strips ".man"
    m = re.match(r"^(.*)\.(\d{2})$", stem)
    if m:
        stem = m.group(1)
    words = re.split(r"[_\-]+", stem)
    return " ".join(w.capitalize() for w in words if w)


# ---------------------------------------------------------------- encode ----
def render_pdf(pdf_path, workdir, width):
    """pdftoppm fit-to-width `width` -> sorted list of RGB numpy arrays (one/page)."""
    prefix = os.path.join(workdir, f"pg{width}")
    subprocess.run(["pdftoppm", "-png", "-scale-to-x", str(width), "-scale-to-y", "-1",
                    pdf_path, prefix], check=True)
    pages = []
    for p in sorted(glob.glob(prefix + "-*.png")):
        im = Image.open(p).convert("RGB")
        a = np.asarray(im, dtype=np.uint8)
        # pdftoppm can round to width+/-1; force exactly `width` columns.
        if a.shape[1] != width:
            a = np.asarray(im.resize((width, im.height), Image.LANCZOS), dtype=np.uint8)
        pages.append(a)
    return pages


def find_smart_seam(page_gray, y0):
    """Return the cut row in [y0+BAND_H-SEAM_WINDOW, y0+BAND_H] (never later, so
    band0 stays <= BAND_H tall) with the fewest ink pixels, nearest to y0+BAND_H."""
    target = y0 + BAND_H
    lo = max(y0 + 1, target - SEAM_WINDOW)
    rows = page_gray[lo:target + 1]                       # inclusive of target
    ink = (rows < DARK_THRESH).sum(axis=1)                # dark-pixel count / row
    best, best_key = target, None
    for i, cnt in enumerate(ink):
        y = lo + i
        key = (int(cnt), abs(y - target))                # cleanest, then nearest
        if best_key is None or key < best_key:
            best_key, best = key, y
    return best


def cut_bands_with_bounds(page):
    """Yield (band_rgb[content_h,256,3], content_h, y0, y1) top-to-bottom for one
    page. (y0,y1) are the 256-space row bounds actually consumed -- the seam-lock
    for --zoom doubles these to slice the 512-wide render at the SAME cut points."""
    H = page.shape[0]
    gray = page.astype(np.uint16).sum(axis=2) // 3       # luma-ish
    y0 = 0
    while y0 < H:
        if H - y0 <= BAND_H:
            yield page[y0:H], H - y0, y0, H
            return
        seam = find_smart_seam(gray, y0)
        yield page[y0:seam], seam - y0, y0, seam
        y0 = seam


def cut_bands(page):
    for band, content_h, _y0, _y1 in cut_bands_with_bounds(page):
        yield band, content_h


def quantize_band(band_rgb, content_h):
    """median-cut -> (idx[224,256] uint8, pal555[256] uint16 LE-value list).
    Palette is snapped to BGR555 AFTER median-cut (better fidelity than pre-snap:
    the cut picks 256 optimal full-precision reps, then each is snapped once).
    content_h==0 (a fully-padding zoom cell -- e.g. the bottom row of a 2x2
    grid when the source band was shorter than BAND_H) short-circuits to a
    plain white block: there is no real content to quantize, and depending on
    PIL's behaviour for a 0-row image is not something to rely on."""
    if content_h == 0:
        idx = np.zeros((BAND_H, PAGE_W), dtype=np.uint8)
        pal555 = np.zeros(256, dtype=np.uint16)
        pal555[0] = 0x7FFF                                  # white, index 0
        return idx, pal555, 0

    im = Image.fromarray(band_rgb, "RGB")
    q = im.quantize(colors=NCOLORS_1X, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx_content = np.asarray(q, dtype=np.uint8)          # [content_h, 256]

    raw = q.getpalette() or []
    raw = raw + [0] * (768 - len(raw))
    pal_rgb = np.array(raw[:768], dtype=np.uint8).reshape(256, 3)
    r5 = (pal_rgb[:, 0] >> 3).astype(np.uint16)
    g5 = (pal_rgb[:, 1] >> 3).astype(np.uint16)
    b5 = (pal_rgb[:, 2] >> 3).astype(np.uint16)
    pal555 = (b5 << 10) | (g5 << 5) | r5                 # 0BBBBBGGGGGRRRRR

    # dominant background index = most common index in the real content.
    bg = int(np.bincount(idx_content.reshape(-1), minlength=256).argmax())

    idx = np.full((BAND_H, PAGE_W), bg, dtype=np.uint8)
    idx[:content_h] = idx_content
    return idx, pal555, bg


def decode_tiles_max_index(tiles):
    """Highest 8bpp palette index present in a planar tile blob (audit helper)."""
    a = np.frombuffer(tiles, dtype=np.uint8).reshape(-1, TILE_BYTES)
    n = a.shape[0]
    px = np.zeros((n, 8, 8), dtype=np.uint8)
    sh = np.arange(7, -1, -1, dtype=np.uint8)
    for pl in range(8):
        base = (pl // 2) * 16 + (pl % 2)      # planes 0&1 @0-15, 2&3 @16-31, 4&5 @32-47, 6&7 @48-63
        by = a[:, base:base + 16:2]           # one byte per row for this plane
        px |= (((by[:, :, None] >> sh) & 1) << pl).astype(np.uint8)
    return int(px.max())


def emit_block(idx, pal555):
    """palette (512B) + tiles (57344B, tile-order, 8bpp PLANAR) -> 57856B.
    Tiles are emitted with the fork's snesgfx planar encoder (bitplanes 0-7
    interleaved, MSB-first) so the block DMAs straight into mode-3 BG1 8bpp VRAM."""
    pal = pal555.astype("<u2").tobytes()
    assert len(pal) == PAL_BYTES
    out = bytearray(pal)
    for tr in range(TILES_H):
        for tc in range(TILES_W):
            px = idx[tr * 8:tr * 8 + 8, tc * 8:tc * 8 + 8]
            out += encode_tile_8bpp(px)
    blk = bytes(out)
    assert len(blk) == BLOCK_BYTES
    return blk


# ------------------------------------------------------- zoom (scrollable 4bpp) ----
def _rgb_to_555(pal_rgb):
    """[N,3] uint8/int -> [N] uint16 BGR555 (0BBBBBGGGGGRRRRR)."""
    a = np.asarray(pal_rgb)
    r5 = (a[:, 0].astype(np.uint16) >> 3)
    g5 = (a[:, 1].astype(np.uint16) >> 3)
    b5 = (a[:, 2].astype(np.uint16) >> 3)
    return ((b5 << 10) | (g5 << 5) | r5).astype(np.uint16)


def _555_to_rgb(v):
    """[N] uint16 BGR555 -> [N,3] uint8 (5->8 bit replicate, what the PPU shows)."""
    v = np.asarray(v, dtype=np.uint16)
    exp = lambda c: ((c << 3) | (c >> 2)).astype(np.uint8)
    return np.stack([exp(v & 31), exp((v >> 5) & 31), exp((v >> 10) & 31)], axis=1)


def _snap555(rgb_f):
    """Round a float RGB array onto the BGR555 grid and back -> float. Every
    palette the quantiser scores against is snapped first, so the distances it
    minimises are the ones the PPU will actually display."""
    a = np.clip(np.rint(rgb_f), 0, 255).astype(np.uint8)
    return _555_to_rgb(_rgb_to_555(a)).astype(np.float64)


def _weighted_median_cut(pts, w, k):
    """pts [M,3] float, w [M] float -> [k,3] float representatives.
    Deterministic: splits the box with the largest (weight x longest extent),
    at the weighted median of its longest channel; ties resolved by first index."""
    live = np.nonzero(w > 0)[0]
    if live.size == 0:
        return np.zeros((k, 3), dtype=np.float64)
    boxes = [live]
    while len(boxes) < k:
        best_i, best_score = -1, -1.0
        for i, b in enumerate(boxes):
            if b.size < 2:
                continue
            p = pts[b]
            score = float(w[b].sum()) * float((p.max(0) - p.min(0)).max())
            if score > best_score:
                best_score, best_i = score, i
        if best_i < 0:
            break                                   # nothing splittable left
        b = boxes.pop(best_i)
        p = pts[b]
        ch = int(np.argmax(p.max(0) - p.min(0)))
        order = b[np.argsort(p[:, ch], kind="stable")]
        cw = np.cumsum(w[order])
        split = int(np.searchsorted(cw, cw[-1] / 2.0)) + 1
        split = max(1, min(split, order.size - 1))
        boxes.append(order[:split])
        boxes.append(order[split:])
    reps = []
    for b in boxes:
        ww = w[b]
        reps.append((pts[b] * ww[:, None]).sum(0) / max(float(ww.sum()), 1e-9))
    while len(reps) < k:                            # fewer distinct colours than slots
        reps.append(reps[-1] if reps else np.zeros(3))
    return np.asarray(reps[:k], dtype=np.float64)


def quantize_zoom_page(page2x, tiles_w=None):
    """[H,512,3] uint8 -> (idx4 [T,8,8] uint8 0..15, attr [T] uint8 = pal<<2,
    pal555 [128] uint16, nrows, pix_h) for one scrollable zoom page.

    Method -- collapse to a 128-colour MASTER space first, then every per-tile
    decision is small matrix algebra over 128 colours instead of a per-pixel
    nearest-colour search. That is what makes 8-palettes-of-16 affordable:
    the naive form is ~100x slower for the same answer.

      1. master median-cut to 128 colours -> m_idx[H,512], m_rgb[128,3]
      2. hist[t][m] = per-tile histogram over the master colours
      3. seed the 8 palettes from luma-sorted, pixel-count-weighted runs
      4. ZLLOYD_ITERS fixed iterations of:
           cost[m][g] = min_c dist(m_rgb[m], pal[g][c])
           grp        = argmin_g (hist @ cost)      <- ONE matmul, all tiles
           pal[g]     = weighted median cut of the master colours g's tiles use
      5. emit through a pure LUT

    Step 4's re-assignment is the quality lever: every tile independently
    re-picks the best of all 8 palettes each iteration, so the seed only has to
    be reasonable, not good. No RNG anywhere and a fixed iteration count, so the
    same PDF always produces the same bytes.

    Entry 0 of EVERY palette is forced to the page background. In mode 1, colour
    index 0 of a BG palette is transparent and would show the backdrop -- pinning
    background there (and setting CGRAM[0] to the same colour) makes that
    invisible and makes the bottom-of-page padding uniform."""
    if tiles_w is None:
        tiles_w = ZTILES_W
    W = tiles_w * 8
    H = page2x.shape[0]
    assert page2x.shape[1] == W, f"scroll page must be {W}px wide"
    pix_h = H
    nrows = max(1, min((H + 7) // 8, ZMAX_ROWS if tiles_w == ZTILES_W else S1MAX_ROWS))
    Hpad = nrows * 8

    # --- 1. master palette (128) ---------------------------------------------
    im = Image.fromarray(page2x, "RGB")
    q = im.quantize(colors=ZPAL_COUNT * ZPAL_SIZE,
                    method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    m_idx = np.asarray(q, dtype=np.uint8)                       # [H,512]
    raw = (q.getpalette() or []) + [0] * (128 * 3)
    m_rgb = _snap555(np.array(raw[:384], dtype=np.uint8).reshape(128, 3).astype(np.float64))

    # --- 2. pad to a whole tile row with the modal (background) colour --------
    counts_content = np.bincount(m_idx.reshape(-1), minlength=128)
    bg_m = int(counts_content.argmax())
    bg_rgb = m_rgb[bg_m]
    if Hpad > H:
        m_idx = np.vstack([m_idx, np.full((Hpad - H, W), bg_m, dtype=np.uint8)])
    elif Hpad < H:
        m_idx = m_idx[:Hpad]        # nrows hit ZMAX_ROWS (can't happen from a <=224px
                                    # 1x band, but never let it mis-shape the reshape)

    # tiles in row-major (tile-row, tile-col) order -- so the encoded byte stream
    # is already "row r: cols 0-31 then cols 32-63", the MCU's pre-split layout.
    T = nrows * tiles_w
    t_px = (m_idx.reshape(nrows, 8, tiles_w, 8)
                 .transpose(0, 2, 1, 3)
                 .reshape(T, 8, 8))
    flat = t_px.reshape(T, 64).astype(np.int64)
    hist = np.bincount((np.arange(T)[:, None] * 128 + flat).ravel(),
                       minlength=T * 128).reshape(T, 128).astype(np.float64)

    # --- 3. deterministic seed: luma-sorted runs weighted by pixel count -------
    counts = hist.sum(axis=0)                                   # [128] incl. padding
    luma = m_rgb @ np.array([0.299, 0.587, 0.114])
    order = np.lexsort((_rgb_to_555(m_rgb.astype(np.uint8)).astype(np.int64), luma))
    cum = np.cumsum(counts[order])
    total = max(float(cum[-1]), 1.0)
    pal = np.zeros((ZPAL_COUNT, ZPAL_SIZE, 3), dtype=np.float64)
    for g in range(ZPAL_COUNT):
        lo = np.searchsorted(cum, total * g / ZPAL_COUNT, side="left")
        hi = np.searchsorted(cum, total * (g + 1) / ZPAL_COUNT, side="left") + 1
        run = order[lo:min(hi, 128)]
        w = counts[run].copy()
        w[run == bg_m] = 0.0                    # bg already owns entry 0
        pal[g, 0] = bg_rgb
        pal[g, 1:] = _weighted_median_cut(m_rgb[run], w, ZPAL_SIZE - 1) if run.size \
                     else bg_rgb
        pal[g] = _snap555(pal[g])

    # --- 4. Lloyd: re-assign every tile, then refit every palette -------------
    grp = np.zeros(T, dtype=np.int64)
    for _ in range(ZLLOYD_ITERS):
        d = ((m_rgb[None, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)   # [8,128,16]
        cost = d.min(-1).T                                                  # [128,8]
        tcost = hist @ cost                                                 # [T,8]
        grp = np.argmin(tcost, axis=1)                                      # [T]
        # Revive DEAD palettes on the worst-served tiles. A luma-run seed
        # collapses on a page dominated by one background (i.e. most manual
        # pages): several seeds come out near-identical, never win a tile, and
        # 15 colours each are wasted. Handing each dead palette the tile the
        # live ones fit worst is what puts those colours on the figures/photos
        # that actually need them.
        served = tcost[np.arange(T), grp].copy()
        for g in range(ZPAL_COUNT):
            if (grp == g).any():
                continue
            t = int(np.argmax(served))
            if served[t] <= 0.0:
                break                           # page is already exact; nothing to gain
            grp[t] = g
            served[t] = 0.0                     # next dead palette takes a different tile
        for g in range(ZPAL_COUNT):
            sel = (grp == g)
            if not sel.any():
                continue                        # still empty -> keeps its palette
            w = hist[sel].sum(axis=0)
            w[bg_m] = 0.0
            pal[g, 0] = bg_rgb
            pal[g, 1:] = _weighted_median_cut(m_rgb, w, ZPAL_SIZE - 1)
            pal[g] = _snap555(pal[g])

    # --- 5. emit through a pure LUT ------------------------------------------
    d = ((m_rgb[None, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)       # [8,128,16]
    grp = np.argmin(hist @ d.min(-1).T, axis=1)   # final re-assign against the final palettes
    lut = d.argmin(-1).astype(np.uint8)                                     # [8,128]
    idx4 = np.take_along_axis(lut[grp], flat, axis=1).reshape(T, 8, 8)
    attr = (grp.astype(np.uint8) << 2)
    pal555 = _rgb_to_555(np.clip(np.rint(pal.reshape(-1, 3)), 0, 255).astype(np.uint8))
    return idx4, attr, pal555, nrows, pix_h


def emit_zoom_page(idx4, attr, pal555, nrows, tiles_w=None):
    """-> (pal_bytes 256B, attr_bytes nrows*64, tile_bytes nrows*2048).
    Tile order is already (row, col), so the stream is exactly the pre-split
    per-row layout: 1024B of cols 0-31 then 1024B of cols 32-63."""
    if tiles_w is None:
        tiles_w = ZTILES_W
    pal_b = pal555.astype("<u2").tobytes()
    attr_b = attr.astype(np.uint8).tobytes()
    tile_b = encode_tiles_4bpp(idx4)
    assert len(pal_b) == ZPAL_BYTES
    assert len(attr_b) == nrows * tiles_w
    assert len(tile_b) == nrows * tiles_w * ZTILE_BYTES
    return pal_b, attr_b, tile_b


def zoom_band(page2x, y0, y1):
    """The seam-locked 2x region of one 1x band: rows [2*y0, 2*y1) of the 2x
    render, full 512 width. Page == band, so there is no quadrant enumeration
    and no seam arithmetic on the viewer side."""
    return page2x[2 * y0:min(2 * y1, page2x.shape[0])]


def _unsharp(a):
    """Pre-quantize sharpen (mild unsharp mask). Applied to the QUANTIZER INPUTS
    only -- the seam finder keeps reading the original page, so the HEAD region
    (header/index/dirs) is identical with sharpen on or off."""
    im = Image.fromarray(np.ascontiguousarray(a), "RGB")
    return np.asarray(im.filter(ImageFilter.UnsharpMask(radius=1, percent=80,
                                                        threshold=0)), dtype=np.uint8)


def encode(pdf_path, out_path, title=None, zoom=False, spread="auto", sharpen=True):
    with tempfile.TemporaryDirectory() as wd:
        p256 = render_pdf(pdf_path, wd, PAGE_W)
        if not p256:
            raise SystemExit("no pages rendered from PDF")
        if len(p256) > 255:
            raise SystemExit("PDF has >255 pages (npages is a u8)")
        if spread == "off":
            ks = [0] * len(p256)
        else:
            ks = [spread_split_factor(p.shape[1], p.shape[0]) for p in p256]
            if spread == "on":
                ks = [max(1, k) for k in ks]
        # one whole-document render per distinct source width: piece k needs the page at
        # 256*2^k (1x) and 512*2^k (2x), sliced into 2^k equal columns of 256/512.
        widths = set()
        for k in ks:
            if k > 0:
                widths.add(PAGE_W << k)      # k=0 reuses the base 256 render
            if zoom:
                widths.add((PAGE_W * 2) << k)
        renders = {w: render_pdf(pdf_path, wd, w) for w in sorted(widths)}
        for w, pl in renders.items():
            if len(pl) != len(p256):
                raise SystemExit(f"render page count mismatch at {w}px vs 1x")

        def slices(arr, k):
            n = 1 << k
            cw = arr.shape[1] // n
            return [np.ascontiguousarray(arr[:, j * cw:(j + 1) * cw]) for j in range(n)]

        # Assemble in reading order: a split spread contributes its pieces left-to-right --
        # each piece IS (a column of) one printed page, so the emitted sequence follows the
        # book's own pagination (and "Pg X/N" counts emitted pages).
        pages = []
        pages2x = [] if zoom else None
        for i, k in enumerate(ks):
            if k == 0:
                pages.append(p256[i])
                if zoom:
                    pages2x.append(renders[PAGE_W * 2][i])
            else:
                pages.extend(slices(renders[PAGE_W << k][i], k))
                if zoom:
                    pages2x.extend(slices(renders[(PAGE_W * 2) << k][i], k))
        if len(pages) > 255:
            raise SystemExit(f"spread split produces {len(pages)} pages (npages is a "
                             f"u8, max 255) -- use --spread off or trim the PDF")
        if zoom:
            # Seam-lock invariant: force each 2x page to be EXACTLY double the
            # matching 1x page's shape. pdftoppm scales each render (-scale-to-x
            # 256 vs 512) independently, so its internal height rounding could in
            # principle drift by a row between the two -- the zoom cutter below
            # multiplies 1x seam coordinates by 2, so it needs an exact 2x source
            # to land precisely on the same content, not an approximately-2x one.
            for i, p1 in enumerate(pages):
                target_h, target_w = p1.shape[0] * 2, p1.shape[1] * 2
                if pages2x[i].shape[0] != target_h or pages2x[i].shape[1] != target_w:
                    im = Image.fromarray(pages2x[i], "RGB").resize(
                        (target_w, target_h), Image.LANCZOS)
                    pages2x[i] = np.asarray(im, dtype=np.uint8)
    npages = len(pages)

    blocks, index = [], []              # 1x blocks: legacy 8bpp stream (still emitted; the viewer
                                        #   now reads the scale-1 SCROLLABLE section instead)
    zpages, zmap = [], []               # 2x: whole-PDF-page images + a per-1x-block entry point
    s1pages = []                        # 1x: the same pages at 256px, scrollable
    for pi, page in enumerate(pages):
        page2x = pages2x[pi] if zoom else None
        bands = list(cut_bands_with_bounds(page))
        gb0 = len(blocks)                                # global index of this page's first band
        for bi, (band, content_h, y0, y1) in enumerate(bands):
            # The legacy 8bpp block stream is RETIRED: the 1x view is now the scale-1 scrollable
            # page (which pans, and whose HUD lives in OBJ so it cannot steal image colours).
            # The index is still emitted -- it is what locates Z0 and what old firmware strides --
            # but the 57856-byte bodies are not, which is ~3.5 MB off a 32-page manual.
            content_rows = (content_h + 7) // 8          # tile rows with content
            index.append((pi, bi, content_rows, 0))
            blocks.append(None)
        if zoom:
            sq1 = _unsharp(page) if sharpen else page       # quantizer inputs only
            sq2 = _unsharp(page2x) if sharpen else page2x
            # The zoom image is the WHOLE 2x page, split only if it exceeds the resident budget.
            H2 = page2x.shape[0]
            nchunks = max(1, (H2 + ZCHUNK_PX - 1) // ZCHUNK_PX)
            zp0 = len(zpages)
            for j in range(nchunks):
                y0c, y1c = j * ZCHUNK_PX, min((j + 1) * ZCHUNK_PX, H2)
                zq = quantize_zoom_page(sq2[y0c:y1c])
                zpages.append(emit_zoom_page(*zq[:3], zq[3]) + (zq[3], zq[4], pi, None))
            # every 1x band maps to the chunk holding its top scanline, doubled
            for bi, (_b, _c, y0, _y1) in enumerate(bands):
                y2 = 2 * y0
                j = min(y2 // ZCHUNK_PX, nchunks - 1)
                zmap.append((zp0 + j, y2 - j * ZCHUNK_PX))
                # first_block: the earliest 1x block that lands in this chunk (used when leaving
                # zoom after page turns, so we return to a block that actually shows this content)
                pg = zpages[zp0 + j]
                if pg[6] is None:
                    zpages[zp0 + j] = pg[:6] + (gb0 + bi,)
            for j in range(nchunks):                     # a chunk no band starts in (tall page)
                if zpages[zp0 + j][6] is None:
                    zpages[zp0 + j] = zpages[zp0 + j][:6] + (gb0,)
            # how many 1x bands each chunk covers (>=1; used when leaving zoom)
            for j in range(nchunks):
                n = sum(1 for (zp, _zy) in zmap[-len(bands):] if zp == zp0 + j)
                zpages[zp0 + j] = zpages[zp0 + j] + (max(1, n),)
            # --- scale 1: the SAME page at 256px, cut into S1CHUNK_ROWS-tall chunks -- HALF a
            #     zoom chunk, so both sections always yield the SAME page count and page p at 1x
            #     and page p at 2x are the same content (toggling is a pure coordinate scale x2). ---
            H1 = page.shape[0]
            c1 = max(1, (H1 + S1CHUNK_ROWS * 8 - 1) // (S1CHUNK_ROWS * 8))
            for j in range(c1):
                y0c, y1c = j * S1CHUNK_ROWS * 8, min((j + 1) * S1CHUNK_ROWS * 8, H1)
                q1 = quantize_zoom_page(sq1[y0c:y1c], S1TILES_W)
                s1pages.append(emit_zoom_page(*q1[:3], q1[3], S1TILES_W) + (q1[3], q1[4], pi))

    nblocks = len(blocks)
    if nblocks > 0xFFFF:
        raise SystemExit("too many blocks (nblocks is a u16)")
    if zoom and len(zmap) != nblocks:
        raise SystemExit("internal error: zoom block map != nblocks")

    title_bytes = font_encode_title(title if title is not None else derive_title(out_path))

    # --- layout ---------------------------------------------------------------
    # header | 1x index | [zoom sub-header | page dir] | pad | 1x blocks |
    # [per page: palette (sector), attrs (sector-padded), tiles]
    # Everything past the 1x blocks is reached through ABSOLUTE offsets in the
    # page directory, so nothing downstream re-derives a base by arithmetic.
    idx_len = HDR_BYTES + nblocks * IDX_ENTRY
    z0 = idx_len
    nzp, ns1 = len(zpages), len(s1pages)
    head_len = idx_len + (2 * ZHDR_BYTES + (nzp + ns1) * ZDIR_ENTRY
                          + nblocks * ZMAP_ENTRY if zoom else 0)
    body_start = (head_len + SECTOR - 1) // SECTOR * SECTOR   # 512-aligned
    pad = body_start - head_len

    idx_bytes = bytearray()
    off = body_start
    for (pi, bi, rows, rsvd) in index:
        idx_bytes += struct.pack("<IBBBB", 0, pi, bi, rows, rsvd)   # 0 = no legacy block body
    # `off` now points just past the last 1x block == the zoom body base.

    zhdr_bytes = s1hdr_bytes = zdir_bytes = zmap_bytes = s1dir_bytes = b""
    nzrows = 0
    if zoom:
        zdir = bytearray()
        for (pal_b, attr_b, tile_b, nrows, pix_h, pdf_page, first_block, nblk) in zpages:
            pal_ofs = off
            off += ZPAL_STRIDE                                   # palette, sector-padded
            attr_ofs = off
            off += (len(attr_b) + SECTOR - 1) // SECTOR * SECTOR  # attrs, sector-padded
            tile_ofs = off
            off += len(tile_b)                                   # tiles (already 4-sector rows)
            zdir += struct.pack(ZDIR_STRUCT, tile_ofs, attr_ofs, pal_ofs, nrows, pix_h,
                                first_block, pdf_page & 0xFF, nblk & 0xFF)
            nzrows += nrows
        zdir_bytes = bytes(zdir)
        zmap_bytes = b"".join(struct.pack(ZMAP_STRUCT, zp, zy) for (zp, zy) in zmap)
        s1dir = bytearray()
        for (pal_b, attr_b, tile_b, nrows, pix_h, pdf_page) in s1pages:
            pal_ofs = off;  off += ZPAL_STRIDE
            attr_ofs = off; off += (len(attr_b) + SECTOR - 1) // SECTOR * SECTOR
            tile_ofs = off; off += len(tile_b)
            s1dir += struct.pack(ZDIR_STRUCT, tile_ofs, attr_ofs, pal_ofs, nrows, pix_h,
                                 0, pdf_page & 0xFF, 1)
        s1dir_bytes = bytes(s1dir)
        zhdr_bytes = struct.pack(ZHDR_STRUCT, b"ZOOM", 4, ZPAL_COUNT, ZTILES_W,
                                 ZATTR_STRIDE, ZROW_BYTES, ZPAL_BYTES, nzrows,
                                 max((p[3] for p in zpages), default=0), 0,
                                 z0 + 2 * ZHDR_BYTES, nzp, 0,
                                 z0 + 2 * ZHDR_BYTES + nzp * ZDIR_ENTRY)
        s1hdr_bytes = struct.pack(ZHDR_STRUCT, S1HDR_MAGIC, 4, ZPAL_COUNT, S1TILES_W,
                                  S1ATTR_STRIDE, S1ROW_BYTES, ZPAL_BYTES,
                                  sum(p[3] for p in s1pages),
                                  max((p[3] for p in s1pages), default=0), 0,
                                  z0 + 2 * ZHDR_BYTES + nzp * ZDIR_ENTRY
                                     + nblocks * ZMAP_ENTRY, ns1, 0, 0)
        assert len(zhdr_bytes) == ZHDR_BYTES and len(s1hdr_bytes) == ZHDR_BYTES

    flags = FLAG_ZOOM2 if zoom else 0
    header = struct.pack(HDR_STRUCT, b"MANL", 1, 8, npages, flags,
                          PAGE_W, BAND_H, nblocks, 0, title_bytes)
    assert len(header) == HDR_BYTES

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(idx_bytes)
        f.write(zhdr_bytes)
        f.write(s1hdr_bytes)
        f.write(zdir_bytes)
        f.write(zmap_bytes)
        f.write(s1dir_bytes)
        f.write(b"\x00" * pad)
        for pg in zpages + s1pages:
            pal_b, attr_b, tile_b = pg[0], pg[1], pg[2]
            f.write(pal_b + b"\x00" * (ZPAL_STRIDE - len(pal_b)))
            apad = (len(attr_b) + SECTOR - 1) // SECTOR * SECTOR - len(attr_b)
            f.write(attr_b + b"\x00" * apad)
            f.write(tile_b)

    assert off == os.path.getsize(out_path), "layout drift: computed offsets vs file size"
    return npages, nblocks, nzrows, off


# ---------------------------------------------------------------- verify ----
def _read_header(f):
    magic, ver, bpp, npages, flags, page_w, band_h, nblocks, rsvd, title_bytes = \
        struct.unpack(HDR_STRUCT, f.read(HDR_BYTES))
    if magic != b"MANL":
        raise SystemExit("bad magic (not a .man)")
    return dict(ver=ver, bpp=bpp, npages=npages, flags=flags, page_w=page_w,
                band_h=band_h, nblocks=nblocks, rsvd=rsvd,
                title=font_decode_title(title_bytes))


def _read_zoom(f, h):
    """-> (zoom sub-header dict, page directory list) or (None, None) if absent.
    Validates the same fields the MCU validates, in the same order, so a file
    this rejects is a file the firmware would also reject."""
    if not (h["flags"] & FLAG_ZOOM2):
        return None, None
    z0 = HDR_BYTES + h["nblocks"] * IDX_ENTRY
    f.seek(z0)
    raw = f.read(ZHDR_BYTES)
    if len(raw) != ZHDR_BYTES:
        raise SystemExit("truncated zoom sub-header")
    (magic, zbpp, zpal_count, ztiles_per_row, zattr_stride, zrow_bytes,
     zpal_bytes, nzrows, zmax_rows, _r0, pagedir_ofs, zpages, _r1,
     blockmap_ofs) = struct.unpack(ZHDR_STRUCT, raw)
    if magic != b"ZOOM":
        raise SystemExit("bad zoom magic")
    if (zbpp, zpal_count, ztiles_per_row, zattr_stride, zrow_bytes, zpal_bytes) != \
       (4, ZPAL_COUNT, ZTILES_W, ZATTR_STRIDE, ZROW_BYTES, ZPAL_BYTES):
        raise SystemExit("zoom sub-header field mismatch (stale/foreign encoder?)")
    z = dict(zbpp=zbpp, nzrows=nzrows, zmax_rows=zmax_rows, pagedir_ofs=pagedir_ofs,
             zpages=zpages, blockmap_ofs=blockmap_ofs)
    f.seek(pagedir_ofs)
    d = f.read(zpages * ZDIR_ENTRY)
    if len(d) != zpages * ZDIR_ENTRY:
        raise SystemExit("truncated zoom page directory")
    pdir = [struct.unpack_from(ZDIR_STRUCT, d, i * ZDIR_ENTRY) for i in range(zpages)]
    f.seek(blockmap_ofs)
    m = f.read(h["nblocks"] * ZMAP_ENTRY)
    if len(m) != h["nblocks"] * ZMAP_ENTRY:
        raise SystemExit("truncated zoom block map")
    z["blockmap"] = [struct.unpack_from(ZMAP_STRUCT, m, i * ZMAP_ENTRY)
                     for i in range(h["nblocks"])]
    return z, pdir


def audit(man_path):
    """Structural audit: every invariant the viewer/MCU depends on, checked
    without decoding pixels. This is the cheap net for LAYOUT drift -- the class
    of bug that otherwise only shows up as garbage on hardware."""
    size = os.path.getsize(man_path)
    errs = []
    with open(man_path, "rb") as f:
        h = _read_header(f)
        print(f"{man_path}: title={h['title']!r} ver={h['ver']} bpp={h['bpp']} "
              f"npages={h['npages']} flags=0x{h['flags']:02x} nblocks={h['nblocks']} "
              f"size={size} ({size/1024/1024:.2f} MB)")
        if h["flags"] & FLAG_ZOOM:
            errs.append("flags bit0 (legacy quadrant zoom) must never be set")
        if h["rsvd"]:
            errs.append(f"header +14 must be 0, got {h['rsvd']}")
        f.seek(HDR_BYTES)
        idx_entries = [struct.unpack("<IBBBB", f.read(IDX_ENTRY)) for _ in range(h["nblocks"])]
        for i, (off, page, blk, rows, rsvd) in enumerate(idx_entries):
            if off:
                errs.append(f"1x block {i}: legacy block bodies are retired, offset must be 0")
            if rsvd:
                errs.append(f"1x block {i}: rsvd byte must be 0, got {rsvd}")

        z, pdir = _read_zoom(f, h)
        if z is None:
            print("  zoom: absent")
        else:
            tot = 0
            for i, (tile_ofs, attr_ofs, pal_ofs, nrows, pix_h, first_block,
                    pdf_page, nblk) in enumerate(pdir):
                tot += nrows
                for nm, o in (("tile", tile_ofs), ("attr", attr_ofs), ("pal", pal_ofs)):
                    if o % SECTOR:
                        errs.append(f"zoom page {i}: {nm}_ofs {o} not sector-aligned")
                if not 1 <= nrows <= ZMAX_ROWS:
                    errs.append(f"zoom page {i}: nrows {nrows} outside 1..{ZMAX_ROWS}")
                if nrows > z["zmax_rows"]:
                    errs.append(f"zoom page {i}: nrows {nrows} > zmax_rows {z['zmax_rows']}")
                if not (nrows - 1) * 8 < pix_h <= nrows * 8:
                    errs.append(f"zoom page {i}: pix_h {pix_h} inconsistent with nrows {nrows}")
                if tile_ofs + nrows * ZROW_BYTES > size:
                    errs.append(f"zoom page {i}: tiles run past EOF")
                if attr_ofs + nrows * ZATTR_STRIDE > size:
                    errs.append(f"zoom page {i}: attrs run past EOF")
                if pal_ofs + ZPAL_BYTES > size:
                    errs.append(f"zoom page {i}: palette runs past EOF")
                if first_block >= h["nblocks"]:
                    errs.append(f"zoom page {i}: first_block {first_block} out of range")
                if pdf_page >= h["npages"]:
                    errs.append(f"zoom page {i}: pdf_page {pdf_page} out of range")
                if not 1 <= nblk or first_block + nblk > h["nblocks"]:
                    errs.append(f"zoom page {i}: nblk_in_page {nblk} inconsistent with "
                                f"first_block {first_block}")
            for b, (zp, zy) in enumerate(z["blockmap"]):
                if zp >= len(pdir):
                    errs.append(f"block {b}: maps to zoom page {zp}, only {len(pdir)} exist")
                elif zy >= pdir[zp][4]:
                    errs.append(f"block {b}: entry Y {zy} past page {zp} height {pdir[zp][4]}")
            if tot != z["nzrows"]:
                errs.append(f"sum(nrows)={tot} != nzrows={z['nzrows']}")
            print(f"  zoom: {len(pdir)} pages ({h['npages']} PDF pages), {z['nzrows']} tile "
                  f"rows, zmax_rows={z['zmax_rows']}, blockmap@{z['blockmap_ofs']}")
            tall = sum(1 for e in pdir if e[3] >= ZMAX_ROWS)
            print(f"  zoom: {len(pdir) - h['npages']} extra page(s) from splitting"
                  f"{f' ({tall} at the {ZMAX_ROWS}-row cap)' if tall else ''}")
    if errs:
        for e in errs:
            print(f"  ERROR: {e}", file=sys.stderr)
        raise SystemExit(f"{len(errs)} structural error(s)")
    print("  structure OK")


def _zoom_page_rgb(f, h, z, pdir, p):
    """Reassemble zoom page p exactly as the PPU would: pixel = pal[attr>>2][idx]."""
    tile_ofs, attr_ofs, pal_ofs, nrows, pix_h = pdir[p][:5]
    f.seek(pal_ofs)
    pal555 = np.frombuffer(f.read(ZPAL_BYTES), dtype="<u2")
    pal_rgb = _555_to_rgb(pal555).reshape(ZPAL_COUNT, ZPAL_SIZE, 3)
    f.seek(attr_ofs)
    attr = np.frombuffer(f.read(nrows * ZATTR_STRIDE), dtype=np.uint8)
    f.seek(tile_ofs)
    T = nrows * ZTILES_W
    px = decode_tiles_4bpp(f.read(nrows * ZROW_BYTES), T)          # [T,8,8]
    g = (attr >> 2).astype(np.int64)
    if (attr & 0x03).any():
        raise SystemExit(f"zoom page {p}: attr low bits must be 0 (value is palette<<2)")
    rec = pal_rgb[g[:, None, None].repeat(8, 1).repeat(8, 2), px]  # [T,8,8,3]
    return (rec.reshape(nrows, ZTILES_W, 8, 8, 3)
               .transpose(0, 2, 1, 3, 4)
               .reshape(nrows * 8, ZOOM_W, 3)[:pix_h]), nrows, pix_h


def verify_zoom(man_path, page_n, row_n, out_png):
    with open(man_path, "rb") as f:
        h = _read_header(f)
        z, pdir = _read_zoom(f, h)
        if z is None:
            raise SystemExit("this .man has no zoom section")
        if page_n is None:                       # --zoom-row R: find its page
            acc = 0
            for i, e in enumerate(pdir):
                if row_n < acc + e[3]:
                    page_n, row_n = i, row_n - acc
                    break
                acc += e[3]
            else:
                raise SystemExit(f"--zoom-row {row_n} out of range 0..{z['nzrows']-1}")
        if not 0 <= page_n < len(pdir):
            raise SystemExit(f"--zoom-page {page_n} out of range 0..{len(pdir)-1}")
        rgb, nrows, pix_h = _zoom_page_rgb(f, h, z, pdir, page_n)
    if row_n is not None:
        if not 0 <= row_n < nrows:
            raise SystemExit(f"row {row_n} out of range 0..{nrows-1}")
        rgb = rgb[row_n * 8:row_n * 8 + 8]
    Image.fromarray(rgb, "RGB").save(out_png)
    print(f"zoom page {page_n}: {ZOOM_W}x{pix_h} ({nrows} tile rows)"
          f"{f' row {row_n}' if row_n is not None else ''} "
          f"title={h['title']!r} -> {out_png}")


def verify(man_path, block_n, out_png):
    with open(man_path, "rb") as f:
        h = _read_header(f)
        if not 0 <= block_n < h["nblocks"]:
            raise SystemExit(f"--block {block_n} out of range 0..{h['nblocks']-1}")
        f.seek(HDR_BYTES + block_n * IDX_ENTRY)
        off, page, block, content_rows, zflags = struct.unpack("<IBBBB", f.read(IDX_ENTRY))
        f.seek(off)
        blk = f.read(BLOCK_BYTES)
    if len(blk) != BLOCK_BYTES:
        raise SystemExit("truncated block")

    pal555 = np.frombuffer(blk[:PAL_BYTES], dtype="<u2")
    r5 = (pal555 & 0x1F); g5 = (pal555 >> 5) & 0x1F; b5 = (pal555 >> 10) & 0x1F
    exp = lambda c: ((c << 3) | (c >> 2)).astype(np.uint8)     # 5->8 bit replicate
    pal_rgb = np.stack([exp(r5), exp(g5), exp(b5)], axis=1)    # [256,3]

    tiles = blk[PAL_BYTES:]                                    # 8bpp PLANAR, tile-order
    idx = np.zeros((BAND_H, PAGE_W), dtype=np.uint8)
    ti = 0
    for tr in range(TILES_H):
        for tc in range(TILES_W):
            px = decode_tile_8bpp(tiles[ti * TILE_BYTES:ti * TILE_BYTES + TILE_BYTES])
            idx[tr * 8:tr * 8 + 8, tc * 8:tc * 8 + 8] = px
            ti += 1
    raster = pal_rgb[idx]                                      # [224,256,3]
    Image.fromarray(raster, "RGB").save(out_png)
    print(f"block {block_n}: page={page} block-in-page={block} content_rows={content_rows}"
          f" offset={off} (sector {off // SECTOR}) title={h['title']!r} -> {out_png}")


# ------------------------------------------------------------------ cli -----
def main(argv):
    ap = argparse.ArgumentParser(description="Generate/inspect a .man Manual/Guide asset")
    ap.add_argument("input", nargs="?", help="input .pdf (encode) or .man (--verify)")
    ap.add_argument("-o", "--out", help="output .man (encode) or .png (--verify with a target)")
    ap.add_argument("--title", help="guide title (UTF-8; font-encoded, cap 23 glyphs). "
                                     "Default: derived from the output filename stem.")
    ap.add_argument("--zoom", action="store_true", help="also emit a seam-locked 2x zoom section")
    ap.add_argument("--spread", choices=("auto", "on", "off"), default="auto",
                    help="split 2-page spread scans into one emitted page per half "
                         "(auto = aspect w/h >= %.1f; default auto)" % SPREAD_AR)
    ap.add_argument("--no-sharpen", action="store_true",
                    help="skip the pre-quantize unsharp mask (body bytes only)")
    ap.add_argument("--verify", metavar="MAN", help="structural audit of MAN; with a target "
                                                    "below, also round-trip it back to PNG")
    ap.add_argument("--block", type=int, help="1x block index to round-trip")
    ap.add_argument("--zoom-page", type=int, help="zoom page index to round-trip")
    ap.add_argument("--zoom-row", type=int, help="global zoom tile row to round-trip (512x8)")
    args = ap.parse_args(argv)

    if args.verify:
        targets = sum(x is not None for x in (args.block, args.zoom_page, args.zoom_row))
        if targets > 1:
            ap.error("--block / --zoom-page / --zoom-row are mutually exclusive")
        if targets == 0:
            audit(args.verify)                       # audit-only: -o not required
            return 0
        if not args.out:
            ap.error("-o is required when round-tripping to PNG")
        if args.block is not None:
            verify(args.verify, args.block, args.out)
        else:
            verify_zoom(args.verify, args.zoom_page, args.zoom_row, args.out)
        return 0

    if not args.input:
        ap.error("input PDF required (or use --verify)")
    if not args.out:
        ap.error("-o output .man required")
    npages, nblocks, nzrows, total = encode(args.input, args.out, args.title, args.zoom,
                                            spread=args.spread, sharpen=not args.no_sharpen)
    print(f"{args.out}: {npages} pages, {nblocks} blocks"
          f"{f' + {nzrows} zoom tile rows' if nzrows else ''}, {total} bytes "
          f"({total/1024/1024:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
