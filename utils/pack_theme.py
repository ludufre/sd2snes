#!/usr/bin/env python3
"""pack_theme.py - build/convert sd2snes menu THEME files (.thm) for the ludufre fork.

A "theme" overrides the menu's gfxptr-marked regions (logo, colours, background).
The fork applies a theme by locating the menu's `_GFXPTR_` table at runtime and
overwriting the referenced regions in PSRAM right after the menu is loaded
(see src/theme.c). This tool produces the compact on-SD `.thm` payload.

.thm format (little-endian):
    off  size  field
    0    8     magic  "FXTHEME1"
    8    1     version (=1)
    9    1     N = number of regions
    10   2     flags  (bit0 = has logo)
    12   4     reserved (0)
    16   N*4   TOC: per region { u8 slot ; u8 _rsv ; u16 length }
    ...        payload: region blobs concatenated in TOC order

`slot` indexes the fork's _GFXPTR_ table (see SLOT_* below). The MCU resolves
each slot to a PSRAM address from the live menu image, so a .thm is robust
across menu rebuilds; only the per-slot byte cap (SLOT_MAX) is build-coupled.

Library conversion (`convert-library`): the official theme editor / SmokeMonster
packs produce full 64KB menu binaries for older firmwares whose logo/HDMA layout
is NOT compatible with the fork (different geometry). Only the flat 256-colour
CGRAM palette ports 1:1, so converted themes are PALETTE-ONLY: the fork keeps its
own logo and background gradient, the palette recolours text/list/UI per theme.
The palette is located directly in each editor binary via the _GFXPTR_ table
(range-checked) with known per-build offsets as fallback (find_palette_offset).

Usage:
    pack_theme.py convert-library [--out dist/theme] [--custom menu/custom]
    pack_theme.py extract <source_menu.bin> -o <name.thm>
    pack_theme.py pack -o <name.thm> [--palette pal.bin] [--logo-pal lp.bin] [--logo-tiles lt.bin] ...
    pack_theme.py verify <fork_m3nu.bin> <theme.thm> [theme.thm ...]
"""
import argparse
import os
import re
import struct
import sys

import snesgfx

MAGIC = b"FXTHEME1"
VERSION = 1

# _GFXPTR_ slot indices (fork order: see snes/const.a65 gfxptr_info)
SLOT_FONT = 0
SLOT_LOGO_PAL = 1
SLOT_HDMA_MATH = 2
SLOT_HDMA_BAR = 3
SLOT_HDMA_PAL = 4
SLOT_OAM_L = 5
SLOT_OAM_H = 6
SLOT_PALETTE = 7
SLOT_LOGO_TILES = 8

# Per-slot byte cap == the region size in the fork build. The MCU clamps a
# theme's declared length to this so a bad/oversized .thm can never write past a
# region into adjacent menu code. Keep in sync with src/theme.c THEME_SLOT_MAX.
SLOT_MAX = {
    SLOT_LOGO_PAL: 64,
    SLOT_HDMA_MATH: 19,
    SLOT_HDMA_BAR: 8,
    SLOT_HDMA_PAL: 55,
    SLOT_OAM_L: 96,
    SLOT_OAM_H: 9,
    SLOT_PALETTE: 512,
    SLOT_LOGO_TILES: 14336,
}
SLOT_NAME = {
    SLOT_FONT: "font", SLOT_LOGO_PAL: "logo_pal", SLOT_HDMA_MATH: "hdma_math_src",
    SLOT_HDMA_BAR: "hdma_bar_color_src", SLOT_HDMA_PAL: "hdma_pal_src",
    SLOT_OAM_L: "oam_data_l", SLOT_OAM_H: "oam_data_h", SLOT_PALETTE: "palette",
    SLOT_LOGO_TILES: "logo_tiles",
}

PALETTE_SIZE = 512
FLAG_HAS_LOGO = 0x0001

GFXPTR_MAGIC = b"_GFXPTR_"


def _firmware_slot_caps(theme_c=None):
    """Parse theme_slot_max[] out of src/theme.c (the MCU-side copy of
    SLOT_MAX). Returns {slot: cap}. Raises when the file or the table can't be
    found -- silently skipping would defeat the parity check below."""
    if theme_c is None:
        theme_c = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "src", "theme.c")
    with open(theme_c) as fh:
        src = fh.read()
    m = re.search(r"theme_slot_max\s*\[[^\]]*\]\s*=\s*\{(.*?)\}", src, re.S)
    if not m:
        raise RuntimeError("theme_slot_max[] initializer not found in %s" % theme_c)
    caps = [int(v) for v in re.findall(r"^\s*(\d+)\s*,", m.group(1), re.M)]
    if not caps:
        raise RuntimeError("could not parse theme_slot_max[] entries in %s" % theme_c)
    return dict(enumerate(caps))


def check_firmware_caps():
    """Fail loudly when SLOT_MAX here drifts from src/theme.c theme_slot_max[]
    (the 'keep in sync' contract was comment-only). A desync either silently
    truncates a region on the MCU (cap here too big) or rejects valid themes at
    pack time (cap here too small) -- both break the published gallery."""
    fw = _firmware_slot_caps()
    bad = []
    for slot in sorted(set(fw) | set(SLOT_MAX)):
        ours = SLOT_MAX.get(slot, 0)   # omitted here == not packable == 0 there
        theirs = fw.get(slot, 0)
        if ours != theirs:
            bad.append("slot %d (%s): pack_theme.py=%d src/theme.c=%d"
                       % (slot, SLOT_NAME.get(slot, "?"), ours, theirs))
    if bad:
        sys.exit("SLOT_MAX out of sync with src/theme.c theme_slot_max[]:\n  "
                 + "\n  ".join(bad))


# ---------------------------------------------------------------------------
# .thm build / parse
# ---------------------------------------------------------------------------
def build_thm(regions, flags=0):
    """regions: list of (slot, bytes). Returns the .thm bytes."""
    n = len(regions)
    hdr = MAGIC + bytes([VERSION, n]) + struct.pack("<H", flags) + b"\x00\x00\x00\x00"
    toc = b""
    payload = b""
    for slot, blob in regions:
        cap = SLOT_MAX.get(slot)
        if cap is not None and len(blob) > cap:
            raise ValueError("slot %d (%s) blob %d > cap %d"
                             % (slot, SLOT_NAME.get(slot, "?"), len(blob), cap))
        toc += bytes([slot, 0]) + struct.pack("<H", len(blob))
        payload += blob
    return hdr + toc + payload


def parse_thm(data):
    if data[:8] != MAGIC:
        raise ValueError("bad magic")
    ver, n = data[8], data[9]
    flags = struct.unpack("<H", data[10:12])[0]
    off = 16
    regions = []
    poff = 16 + n * 4
    for _ in range(n):
        slot, _rsv, length = data[off], data[off + 1], struct.unpack("<H", data[off + 2:off + 4])[0]
        regions.append((slot, data[poff:poff + length]))
        poff += length
        off += 4
    return ver, flags, regions


# ---------------------------------------------------------------------------
# palette location in arbitrary editor/stock menu binaries
# ---------------------------------------------------------------------------
def gfxptr_words(d, count=12):
    """Words following the _GFXPTR_ magic. Stops at EOF instead of crashing on
    a truncated binary. NOTE: the table has no length marker, so entries past
    the real table (9 in both stock and fork menus) are unrelated code/data --
    every caller must range/sanity-check the words it uses (see
    find_palette_offset for the mispick this once caused)."""
    i = d.find(GFXPTR_MAGIC)
    if i < 0:
        return None
    base = i + 8
    out = []
    for k in range(count):
        o = base + k * 2
        if o + 2 > len(d):
            break
        out.append(struct.unpack("<H", d[o:o + 2])[0])
    return out


def detect_version(d):
    i = d.find(b"SD2SNES MENU v")
    if i < 0:
        return None
    return d[i + 14:i + 40].split(b" ")[0].decode("ascii", "ignore").strip()


def _is_real_palette(d, o):
    """A live 256-colour menu palette: 512B that fits, not mostly black, plenty of
    distinct colours. Mask bit15 (the SmokeMonster 'Sho' build sets it on ~half its
    palette entries -- harmless to the PPU) so do NOT require 15-bit purity."""
    if o < 0 or o + PALETTE_SIZE > len(d):
        return False
    words = [struct.unpack("<H", d[o + i * 2:o + i * 2 + 2])[0] & 0x7fff for i in range(256)]
    return sum(1 for w in words if w) >= 128 and len(set(words)) >= 40


# Palette offset across the editor builds we ship from: v1.11.0 release AND the
# SmokeMonster 'Sho' v1.10.3 beta both use 0x19E1. The OFFICIAL v1.10.3 release
# uses 0x19D2 (last-ditch fallback only).
PALETTE_OFFSETS = (0x19E1, 0x19D2)


def find_palette_offset(d):
    """Locate the 256-colour palette directly in an editor/stock menu binary.
    WARNING: the stock 9-entry _GFXPTR_ table has the palette LAST, so reverse-
    scan; do not use on the fork menu (its trailing entry is logo_tiles)."""
    # The flat CGRAM palette always lives in the menu's data region around 0x19xx;
    # range-check so we never latch onto junk read past the table (gfxptr_words
    # reads a fixed count, and bytes after the table can pass _is_real_palette --
    # e.g. SD2SNES binaries have 0x03FF there, which mispicked vs FXPAK).
    PAL_LO, PAL_HI = 0x1700, 0x1E00
    ws = gfxptr_words(d)
    if ws:
        for w in reversed(ws):
            if w and PAL_LO <= w <= PAL_HI and _is_real_palette(d, w):
                return w
    for o in PALETTE_OFFSETS:           # builds without the table (Sho)
        if _is_real_palette(d, o):
            return o
    return None


def extract_palette(menu_bin):
    """Extract the 256-colour palette (512B) from an editor/stock menu binary."""
    version = detect_version(menu_bin)
    off = find_palette_offset(menu_bin)
    if off is None:
        raise ValueError("could not locate palette (v%s)" % version)
    return version, off, menu_bin[off:off + PALETTE_SIZE]


# ---------------------------------------------------------------------------
# logo location & extraction
# ---------------------------------------------------------------------------
# The menu logo is a linear 32x7 grid of 8bpp tiles (256x56 px) indexing the
# main 256-colour palette. Its file offset differs per firmware: stock v1.11.0
# carries a 9-entry _GFXPTR_ table whose word[1] is the logo (word[2]-word[1] =
# size); older builds (v1.10.3 / SmokeMonster) have no table, so use a known
# per-version offset.
LOGO_COLS, LOGO_ROWS = 32, 7
LOGO_SIZE = LOGO_COLS * LOGO_ROWS * 64        # 14336
LOGO_VER_OFFSET = {"1.10.3": 0x2D0A}


def locate_logo(d, version):
    """Return (offset, cols, rows) of the menu logo grid, or None."""
    ws = gfxptr_words(d)
    if ws and ws[1] and ws[2] and 0 < (ws[2] - ws[1]) <= 0x4000:
        size = ws[2] - ws[1]
        if size % (LOGO_COLS * 64) == 0:
            return ws[1], LOGO_COLS, size // (LOGO_COLS * 64)
    off = LOGO_VER_OFFSET.get(version)
    if off is not None:
        return off, LOGO_COLS, LOGO_ROWS
    return None


def extract_logo_image(d, version, palette_bytes):
    """Decode the menu logo to a full-resolution opaque RGBA PIL image."""
    loc = locate_logo(d, version)
    if loc is None:
        return None
    off, cols, rows = loc
    # opaque (transparent_index=None): show the banner exactly as stored
    return snesgfx.decode_logo(d, off, cols, rows, palette_bytes, cgbase=0,
                               transparent_index=None)


# ---------------------------------------------------------------------------
# library conversion
# ---------------------------------------------------------------------------
# Sources whose theme NAME collides across packs (FXPAK Pro vs SD2SNES carry the
# same 7 names but DIFFERENT logos) get a brand suffix so both survive on the SD.
BRAND_SUFFIX = {"FXPAK": " (FXPAK PRO)", "SD2SNES": " (SD2SNES)", "Smoke": ""}


def iter_custom_sources(custom_dir):
    """Yield (brand, theme_name, path) for every theme binary under menu/custom."""
    out = []
    for sub, brand in (("FXPAK Pro", "FXPAK"), ("SD2SNES", "SD2SNES")):
        d = os.path.join(custom_dir, sub)
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.lower().endswith(".bin"):
                    out.append((brand, os.path.splitext(f)[0], os.path.join(d, f)))
    # SmokeMonster pack: theme name is the FOLDER name, menu in <folder>/m3nu.bin
    for f in sorted(os.listdir(custom_dir)):
        p = os.path.join(custom_dir, f)
        if os.path.isdir(p) and "SmokeMonster" in f:
            for t in sorted(os.listdir(p)):
                tp = os.path.join(p, t, "m3nu.bin")
                if os.path.isdir(os.path.join(p, t)) and not t.startswith("_") and os.path.exists(tp):
                    out.append(("Smoke", t, tp))
    return out


def _fork_hdma_pal_stops(fork_menu):
    """Read the fork menu's own hdma_pal_src structure (the per-scanline COUNTS
    that define the menu's vertical gradient bands) to recolour per theme."""
    import snesgfx
    d = open(fork_menu, "rb").read()
    ws = gfxptr_words(d)
    if not ws:
        return None
    return snesgfx.parse_hdma(d, ws[SLOT_HDMA_PAL])


# Colour-math HDMA bytes that hold the selection-bar colour (ui.a65 set_bar_color
# writes hdma_bar_color_src -> hdma_math[2,5,8,11]; hdma_math is copied from
# hdma_math_src at boot).
BAR_MATH_OFFSETS = (2, 5, 8, 11)


def _fork_hdma_math(fork_menu):
    """The fork's hdma_math_src (19B) - the boot-time colour-math table (bar
    position + default bar colour)."""
    d = open(fork_menu, "rb").read()
    ws = gfxptr_words(d)
    if not ws:
        return None
    o = ws[SLOT_HDMA_MATH]
    return d[o:o + SLOT_MAX[SLOT_HDMA_MATH]]


def convert_library(custom_dir, out_dir, png_dir=None, with_logo=True,
                    with_bg=True, fork_menu="bin/m3nu.bin"):
    import snesgfx
    os.makedirs(out_dir, exist_ok=True)
    fork_stops = _fork_hdma_pal_stops(fork_menu) if with_bg else None
    fork_math = _fork_hdma_math(fork_menu)
    written, failed, no_bg = 0, [], []
    for brand, name, path in iter_custom_sources(custom_dir):
        disp = name + BRAND_SUFFIX.get(brand, "")
        try:
            d = open(path, "rb").read()
            version, off, pal = extract_palette(d)
            regions = [(SLOT_PALETTE, pal)]
            flags = 0
            # background gradient: recolour the fork's own gradient with the
            # theme's colours (the stock HDMA table is a different size/layout,
            # so we resample, not copy). Themes without a simple gradient
            # (image/scene backgrounds) keep the fork default.
            ws = gfxptr_words(d)
            if fork_stops:
                grad = snesgfx.find_bg_gradient(d, ws[5] if ws else None)
                if grad:
                    blob = snesgfx.encode_hdma(snesgfx.resample_gradient(grad, fork_stops))
                    regions.append((SLOT_HDMA_PAL, blob))
                else:
                    no_bg.append(disp)
            # selection-bar colour (hdma_bar_color_src, 8B, same format as the
            # fork): stock _GFXPTR_ slot 4. ui.a65 feeds it into the colour-math
            # HDMA at runtime, so a direct copy recolours the highlight bar.
            if ws and ws[4]:
                bar = d[ws[4]:ws[4] + SLOT_MAX[SLOT_HDMA_BAR]]
                if len(bar) == SLOT_MAX[SLOT_HDMA_BAR]:
                    regions.append((SLOT_HDMA_BAR, bar))
                    # also recolour the boot-time bar (browser): patch only the
                    # colour bytes of the fork's hdma_math_src with the theme's
                    # NORMAL bar colour, keeping the fork's bar position.
                    if fork_math:
                        m = bytearray(fork_math)
                        for i, mo in enumerate(BAR_MATH_OFFSETS):
                            if mo < len(m):
                                m[mo] = bar[i]
                        regions.append((SLOT_HDMA_MATH, bytes(m)))
            if with_logo:
                from PIL import Image
                # Prefer a hand-edited logo: <png_dir>/<brand>/<name>_SMALL.png
                # (128x56 RGBA, honours transparency). Falls back to the auto
                # refit extracted from the editor binary.
                small = (os.path.join(png_dir, brand, name + "_SMALL.png")
                         if png_dir else None)
                if small and os.path.exists(small):
                    logo_pal, logo_tiles = snesgfx.encode_fork_logo(
                        Image.open(small), keep_alpha=True)
                    regions += [(SLOT_LOGO_PAL, logo_pal), (SLOT_LOGO_TILES, logo_tiles)]
                    flags |= FLAG_HAS_LOGO
                else:
                    logo_img = extract_logo_image(d, version, pal)
                    if logo_img is not None:
                        if png_dir:  # full-res PNG for manual (Photoshop) editing
                            bdir = os.path.join(png_dir, brand)
                            os.makedirs(bdir, exist_ok=True)
                            logo_img.save(os.path.join(bdir, name + ".png"))
                        logo_pal, logo_tiles = snesgfx.encode_fork_logo(
                            logo_img, keep_alpha=False)
                        regions += [(SLOT_LOGO_PAL, logo_pal), (SLOT_LOGO_TILES, logo_tiles)]
                        flags |= FLAG_HAS_LOGO
        except Exception as e:  # noqa: BLE001 - report and continue
            failed.append((disp, str(e)))
            continue
        thm = build_thm(regions, flags)
        with open(os.path.join(out_dir, disp + ".thm"), "wb") as fh:
            fh.write(thm)
        written += 1
        print("  + %-46s v%-7s %d region(s)  %d B%s"
              % (disp, version, len(regions), len(thm), "  +logo" if flags else ""))
    print("\nconverted %d themes -> %s  (failed %d)" % (written, out_dir, len(failed)))
    if no_bg:
        print("kept fork background (no simple gradient) for %d: %s"
              % (len(no_bg), ", ".join(no_bg)))
    if png_dir:
        print("logo PNGs (full-res, for manual editing) -> %s" % png_dir)
    for name, err in failed:
        print("  FAILED %s: %s" % (name, err))
    return written, failed


# ---------------------------------------------------------------------------
# verify against a fork menu image
# ---------------------------------------------------------------------------
def verify(fork_menu_path, thm_paths):
    d = open(fork_menu_path, "rb").read()
    ws = gfxptr_words(d)
    if ws is None:
        print("FAIL: no _GFXPTR_ in %s" % fork_menu_path)
        return 1
    print("fork _GFXPTR_ slots:")
    for k in range(9):
        print("  slot %d %-18s -> 0x%04X" % (k, SLOT_NAME.get(k, "?"), ws[k]))
    rc = 0
    for tp in thm_paths:
        try:
            ver, flags, regions = parse_thm(open(tp, "rb").read())
        except Exception as e:  # noqa: BLE001
            print("FAIL %s: %s" % (tp, e))
            rc = 1
            continue
        ok = True
        for slot, blob in regions:
            cap = SLOT_MAX.get(slot)
            dest = ws[slot] if slot < len(ws) else 0
            if cap is not None and len(blob) > cap:
                ok = False
                print("  %s: slot %d len %d > cap %d" % (tp, slot, len(blob), cap))
            if dest == 0:
                ok = False
                print("  %s: slot %d resolves to 0 in fork menu" % (tp, slot))
        print("%-50s v%d %d region(s) %s"
              % (os.path.basename(tp), ver, len(regions), "OK" if ok else "BAD"))
        rc = rc or (0 if ok else 1)
    return rc


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert-library", help="convert menu/custom/* -> dist/theme/*.thm")
    c.add_argument("--custom", default="menu/custom")
    c.add_argument("--out", default="dist/theme")
    c.add_argument("--png-out", dest="png_out", default="dist/theme_png",
                   help="dir for full-res logo PNGs (manual editing); '' to skip")
    c.add_argument("--no-logo", dest="no_logo", action="store_true",
                   help="palette-only, skip logo extraction/baking")

    e = sub.add_parser("extract", help="extract palette from one menu binary -> .thm")
    e.add_argument("source")
    e.add_argument("-o", "--out", required=True)

    p = sub.add_parser("pack", help="build a .thm from explicit region blobs / an edited logo PNG")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--palette", help="raw 512-byte palette blob")
    p.add_argument("--colors-from", dest="colors_from",
                   help="pull the palette from this editor menu.bin")
    p.add_argument("--logo-png", dest="logo_png",
                   help="an edited logo image; refit to the fork format (128x56, 32 colours)")
    p.add_argument("--logo-pal", dest="logo_pal")
    p.add_argument("--logo-tiles", dest="logo_tiles")
    p.add_argument("--hdma-pal", dest="hdma_pal")
    p.add_argument("--hdma-math", dest="hdma_math")
    p.add_argument("--hdma-bar", dest="hdma_bar")
    p.add_argument("--oam-l", dest="oam_l")
    p.add_argument("--oam-h", dest="oam_h")

    v = sub.add_parser("verify", help="check .thm slots/sizes against a fork m3nu.bin")
    v.add_argument("fork_menu")
    v.add_argument("thm", nargs="+")

    args = ap.parse_args()

    # Every subcommand produces or validates .thm files against SLOT_MAX, so
    # always prove SLOT_MAX still matches the firmware before doing anything.
    check_firmware_caps()

    if args.cmd == "convert-library":
        _, failed = convert_library(args.custom, args.out,
                                    png_dir=(args.png_out or None),
                                    with_logo=not args.no_logo)
        return 1 if failed else 0

    if args.cmd == "extract":
        d = open(args.source, "rb").read()
        version, off, pal = extract_palette(d)
        with open(args.out, "wb") as fh:
            fh.write(build_thm([(SLOT_PALETTE, pal)]))
        print("extracted palette v%s @0x%04X -> %s" % (version, off, args.out))
        return 0

    if args.cmd == "pack":
        import snesgfx
        from PIL import Image
        regions = []
        flags = 0
        # palette: explicit blob or pulled from an editor menu.bin
        if args.palette:
            regions.append((SLOT_PALETTE, open(args.palette, "rb").read()))
        elif args.colors_from:
            _, _, pal = extract_palette(open(args.colors_from, "rb").read())
            regions.append((SLOT_PALETTE, pal))
        # logo: refit an edited PNG, or take raw logo_pal/logo_tiles blobs
        if args.logo_png:
            lp, lt = snesgfx.encode_fork_logo(Image.open(args.logo_png))
            regions += [(SLOT_LOGO_PAL, lp), (SLOT_LOGO_TILES, lt)]
            flags |= FLAG_HAS_LOGO
        slot_args = [
            (SLOT_LOGO_PAL, args.logo_pal), (SLOT_LOGO_TILES, args.logo_tiles),
            (SLOT_HDMA_PAL, args.hdma_pal), (SLOT_HDMA_MATH, args.hdma_math),
            (SLOT_HDMA_BAR, args.hdma_bar), (SLOT_OAM_L, args.oam_l), (SLOT_OAM_H, args.oam_h),
        ]
        for slot, path in slot_args:
            if path:
                regions.append((slot, open(path, "rb").read()))
                if slot in (SLOT_LOGO_PAL, SLOT_LOGO_TILES):
                    flags |= FLAG_HAS_LOGO
        if not regions:
            ap.error("pack: at least one region required")
        with open(args.out, "wb") as fh:
            fh.write(build_thm(regions, flags))
        print("packed %d region(s) -> %s" % (len(regions), args.out))
        return 0

    if args.cmd == "verify":
        return verify(args.fork_menu, args.thm)

    return 0


if __name__ == "__main__":
    sys.exit(main())
