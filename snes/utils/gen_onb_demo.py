#!/usr/bin/env python3
"""Convert the onboarding tour's DEMO pictures (onboarding/demo/<id>.png) into
onboarding/onb_demo_{a..j}.a65 (banks $C2-$CB).

Each picture is a 128x112 RGBA PNG (drawn by utils/gen_onb_art.py, or by hand),
shown 1:1 by 4x4 sprites of 32x32 in the tour's left column. On the SNES side a
picture is:
  - its characters: the top 14 rows of a 16x16-character OBJ name table of 4bpp
    (7168 bytes; rows 14..15 of both VRAM slots stay blank);
  - 4 palettes of 15 colours (128 bytes): each 32x32 sprite uses the one that fits
    it best, so a picture has up to 60 colours;
  - its 16 OAM entries twice (64 bytes each), one per VRAM slot: slot 0 is name
    table 0 with OBJ palettes 0-3, slot 1 is name table 1 with palettes 4-7. A
    sprite with nothing to show sits below the screen.
The 256 bytes at onb_demo_<id>_pal are palettes + OAM of slot 0 + OAM of slot 1,
which is all the tour's onb_demo_show copies.

Transparency: alpha < 50% is transparent; anything else is opaque, its edge blended
with the tour's background gradient (so the smooth edges keep their look).

Pictures are numbered in DEMOS order (the tour's onb_demo_map uses the numbers).
onb_demo_a.a65 carries the index table; PER_BANK pictures per object, one bank each.

The .a65 files are COMMITTED: the build never runs this (it needs Pillow).
    python3 utils/gen_onb_demo.py
"""
import functools
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "onboarding", "demo")
OUT = os.path.join(HERE, "..", "onboarding")

# ids in this order; the tour's onb_demo_map (onboarding_const.a65) uses them
DEMOS = ["lang_en", "lang_ptbr", "lang_es", "lang_de", "lang_fr", "lang_it", "lang_ru", "lang_nl",
         "covers_off", "covers_large", "covers_small", "gameinfo_on", "gameinfo_ctx",
         "music_on", "music_off", "random_on", "random_off", "sfx_on", "sfx_off",
         "igm_on", "igm_off", "savestates_on", "savestates_off",
         "pad2_on", "pad2_off", "msu_on", "msu_off",
         "reset_0", "reset_1", "reset_2", "reset_3", "reset_4",
         "patches", "consoles", "memtest",
         "saves", "pcm", "bsx", "delete", "bios", "desc", "section_217", "gbc", "shortcuts",
         "carts_seta", "carts_more", "icons", "gameinfo_cheats",
         "theme_restore", "text_theme", "text_full", "text_nooutline", "text_noaa",
         "lists_on", "lists_off", "video_on", "video_off", "clipmusic_on", "clipmusic_off",
         "cheatlist", "patchmenu", "sufami", "atari", "cctime", "folders",
         "sd2snesdir_on", "sd2snesdir_off", "led", "clearppu", "buscompat", "sysinfo", "clock",
         "trainer", "chips_more", "lang_ja", "lang_zh"]
PER_BANK = 8                   # 8 x (7168 + 256) bytes = 59392, a bank holds 65536
BANKS = ("$c2", "$c3", "$c4", "$c5", "$c6", "$c7", "$c8", "$c9", "$ca", "$cb")
SUFFIX = "abcdefghij"          # onb_demo_<suffix>.a65, one per bank (the Makefile lists them all)
W, H = 128, 112
ROWS, COLS = 14, 16
NPAL = 4


@functools.lru_cache(None)
def demo_xy():
    """ONB_DEMO_X / ONB_DEMO_Y from the tour's memmap (the OAM tables carry them)."""
    s = open(os.path.join(OUT, "onb_memmap.i65")).read()
    get = lambda n: int(re.search(r"#define\s+%s\s+(\d+)" % n, s).group(1))
    return get("ONB_DEMO_X"), get("ONB_DEMO_Y")


def gradient(y):
    """The tour's background at picture line y (roughly the menu gradient)."""
    t = (y + demo_xy()[1]) / 224
    return (0, round(8 + 60 * t), round(16 + 110 * t))


def snes(c):
    """RGB -> the 5-bit colour the SNES shows, back as RGB (what the error is measured on)."""
    return tuple((v >> 3) * 255 // 31 for v in c)


def load(name):
    im = Image.open(os.path.join(SRC, name + ".png")).convert("RGBA")
    if im.size != (W, H):
        sys.exit("gen_onb_demo: %s.png is %dx%d, not %dx%d" % (name, im.width, im.height, W, H))
    px = [[None] * W for _ in range(H)]
    for y in range(H):
        bg = gradient(y)
        for x in range(W):
            r, g, b, a = im.getpixel((x, y))
            if a >= 128:
                k = a / 255
                px[y][x] = snes(tuple(round(c * k + d * (1 - k)) for c, d in zip((r, g, b), bg)))
    return px


def blocks(px):
    """The 16 sprites' opaque pixels, [(sx, sy, [(x, y, rgb)...])]."""
    out = []
    for sy in range(4):
        for sx in range(4):
            pts = [(x, y, px[y][x]) for y in range(sy * 32, min(H, sy * 32 + 32))
                   for x in range(sx * 32, sx * 32 + 32) if px[y][x] is not None]
            out.append((sx, sy, pts))
    return out


def d2(a, b):
    return (a[0] - b[0]) ** 2 * 3 + (a[1] - b[1]) ** 2 * 4 + (a[2] - b[2]) ** 2 * 2


def palette_of(colors, n=15):
    """Up to n colours for a list of RGB (median cut on a strip image)."""
    uniq = list(dict.fromkeys(colors))
    if len(uniq) <= n:
        return uniq
    strip = Image.new("RGB", (len(colors), 1))
    strip.putdata(colors)
    q = strip.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:n * 3]
    return list(dict.fromkeys(snes(tuple(pal[i:i + 3])) for i in range(0, len(pal), 3)))


def err(pts, pal):
    return sum(min(d2(c, p) for p in pal) for _, _, c in pts)


def fit(blks):
    """4 palettes and a palette per sprite: k-means over the sprites."""
    used = [i for i, b in enumerate(blks) if b[2]]
    mean = lambda pts: sum(sum(c) for _, _, c in pts) / len(pts)
    order = sorted(used, key=lambda i: mean(blks[i][2]))       # seed: by brightness, cut in 4
    assign = {i: min(NPAL - 1, k * NPAL // max(1, len(order))) for k, i in enumerate(order)}
    pals = [[] for _ in range(NPAL)]
    for _ in range(8):
        for g in range(NPAL):
            cols = [c for i in used if assign[i] == g for _, _, c in blks[i][2]]
            pals[g] = palette_of(cols) if cols else []
        new = {i: min((g for g in range(NPAL) if pals[g]), key=lambda g: err(blks[i][2], pals[g]))
               for i in used}
        if new == assign:
            break
        assign = new
    for g in range(NPAL):                                       # palettes for the final groups
        cols = [c for i in used if assign[i] == g for _, _, c in blks[i][2]]
        pals[g] = palette_of(cols) if cols else []
    return pals, assign


# ---- the refined fitter (pictures in REFINED). The fit above takes each group's palette by a
#      plain median cut over every pixel, so a small but salient colour (a lit LED, a highlight)
#      loses to the shades of a large gradient. This one starts from the same groups, refines
#      each palette by weighted k-means over the distinct colours (weight = sqrt(count): rare
#      colours still count) and keeps the grouping of the lowest total error out of a few seeds.
#      The older pictures stay on fit() so their bytes do not move; a picture joins REFINED
#      when its art is redrawn.

REFINED = {"text_theme", "text_full", "text_nooutline", "text_noaa", "lists_off", "gameinfo_on",
           "video_on", "video_off", "consoles", "led", "chips_more", "lists_on",
           "icons"}


def counts_of(pts):
    out = {}
    for _, _, c in pts:
        out[c] = out.get(c, 0) + 1
    return out


def cerr(cnt, pal):
    return sum(k * min(d2(c, p) for p in pal) for c, k in cnt.items())


def palette_refined(cnt, n=15):
    uniq = list(cnt)
    if len(uniq) <= n:
        return uniq
    pal = palette_of([c for c, k in cnt.items() for _ in range(k)], n)
    for _ in range(12):
        near = {c: min(range(len(pal)), key=lambda j: d2(c, pal[j])) for c in uniq}
        new = []
        for j in range(len(pal)):
            mem = [(c, cnt[c] ** 0.5) for c in uniq if near[c] == j]
            if not mem:
                continue
            tw = sum(w for _, w in mem)
            new.append(snes(tuple(round(sum(c[ch] * w for c, w in mem) / tw) for ch in range(3))))
        new = list(dict.fromkeys(new))
        while len(new) < n:                 # a free entry: the colour paying the most error
            worst = max(uniq, key=lambda c: cnt[c] ** 0.5 * min(d2(c, p) for p in new))
            if min(d2(worst, p) for p in new) == 0:
                break
            new.append(worst)
        if new == pal:
            break
        pal = new
    return pal


def fit_refined(blks):
    used = [i for i, b in enumerate(blks) if b[2]]
    cnts = {i: counts_of(blks[i][2]) for i in used}

    def run(assign):
        pals = [[] for _ in range(NPAL)]
        for _ in range(8):
            for g in range(NPAL):
                cnt = {}
                for i in used:
                    if assign[i] == g:
                        for c, k in cnts[i].items():
                            cnt[c] = cnt.get(c, 0) + k
                pals[g] = palette_refined(cnt) if cnt else []
            new = {i: min((g for g in range(NPAL) if pals[g]), key=lambda g: cerr(cnts[i], pals[g]))
                   for i in used}
            if new == assign:
                break
            assign = new
        for g in range(NPAL):
            cnt = {}
            for i in used:
                if assign[i] == g:
                    for c, k in cnts[i].items():
                        cnt[c] = cnt.get(c, 0) + k
            pals[g] = palette_refined(cnt) if cnt else []
        return sum(cerr(cnts[i], pals[assign[i]]) for i in used), pals, assign

    seeds = [fit(blks)[1]]
    for ch in (None, 0, 1, 2):
        key = (lambda i: sum(sum(c) * k for c, k in cnts[i].items()) / sum(cnts[i].values())) if ch is None \
            else (lambda i, ch=ch: sum(c[ch] * k for c, k in cnts[i].items()) / sum(cnts[i].values()))
        order = sorted(used, key=key)
        seeds.append({i: min(NPAL - 1, k * NPAL // max(1, len(order))) for k, i in enumerate(order)})
    best = min((run(dict(sd)) for sd in seeds), key=lambda r: r[0])
    return best[1], best[2]


def picture(name, xy):
    px = load(name)
    blks = blocks(px)
    pals, assign = (fit_refined if name in REFINED else fit)(blks)
    idx = [[0] * (COLS * 8) for _ in range(ROWS * 8)]
    for i, (sx, sy, pts) in enumerate(blks):
        pal = pals[assign[i]] if pts else []
        for x, y, c in pts:
            idx[y][x] = 1 + min(range(len(pal)), key=lambda k: d2(c, pal[k]))
    cg = []
    for pal in pals:
        for c in [(0, 0, 0)] + pal + [(0, 0, 0)] * (15 - len(pal)):
            v = (c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10
            cg += [v & 0xff, v >> 8]
    oam = []
    x0, y0 = xy
    for slot in (0, 1):
        for i, (sx, sy, pts) in enumerate(blks):
            if pts:
                attr = 0x30 | ((slot * NPAL + assign[i]) << 1) | slot
                oam += [x0 + sx * 32, y0 + sy * 32, sy * 64 + sx * 4, attr]
            else:
                oam += [0, 224, 0, 0x30]        # below the screen; a 32x32 at 224 never wraps
    return idx, cg + oam


def preview(name, xy):
    """What the SNES will show (for checking the palettes), over the gradient."""
    idx, extra = picture(name, xy)
    cg = extra[:128]
    col = lambda p, k: tuple(((cg[p * 32 + k * 2] | cg[p * 32 + k * 2 + 1] << 8) >> s & 31) * 255 // 31
                             for s in (0, 5, 10))
    oam = extra[128:192]
    im = Image.new("RGB", (W, H))
    for y in range(H):
        for x in range(W):
            im.putpixel((x, y), gradient(y))
    for i in range(16):
        sx, sy = i % 4, i // 4
        pal = (oam[i * 4 + 3] >> 1) & 7
        for y in range(sy * 32, min(H, sy * 32 + 32)):
            for x in range(sx * 32, sx * 32 + 32):
                if idx[y][x]:
                    im.putpixel((x, y), col(pal, idx[y][x]))
    return im


def tile(px, tx, ty):
    lo, hi = [], []
    for r in range(8):
        p = [0, 0, 0, 0]
        for c in range(8):
            v = px[ty * 8 + r][tx * 8 + c]
            for k in range(4):
                if v >> k & 1:
                    p[k] |= 0x80 >> c
        lo += p[0:2]
        hi += p[2:4]
    return lo + hi


def emit(lines, label, data):
    lines.append(label + ":")
    for i in range(0, len(data), 16):
        lines.append("  .byt " + ", ".join("$%02x" % b for b in data[i:i + 16]))


def main():
    xy = demo_xy()
    if "--preview" in sys.argv:                       # what the SNES will show, 2x, to look at
        out = sys.argv[sys.argv.index("--preview") + 1]
        os.makedirs(out, exist_ok=True)
        for n in DEMOS:
            preview(n, xy).resize((W * 2, H * 2), Image.NEAREST).save(os.path.join(out, n + ".png"))
        return
    groups = [DEMOS[k:k + PER_BANK] for k in range(0, len(DEMOS), PER_BANK)]
    if len(groups) > len(BANKS):
        sys.exit("gen_onb_demo: %d pictures need more than %d banks" % (len(DEMOS), len(BANKS)))
    for part, names in enumerate(groups):
        suffix = SUFFIX[part]
        L = ["; AUTO-GENERATED by utils/gen_onb_demo.py from the PNGs in onboarding/demo -- DO NOT EDIT.",
             ".link page " + BANKS[part], ""]
        if part == 0:
            L.append("; per picture id: characters addr, bank, palettes+OAM addr, bank")
            L.append("onb_demo_tbl:")
            for n in DEMOS:
                L.append("  .word !onb_demo_%s : .byt ^onb_demo_%s : .word !onb_demo_%s_pal : .byt ^onb_demo_%s_pal"
                         % (n, n, n, n))
            L.append("")
        for n in names:
            px, extra = picture(n, xy)
            data = []
            for ty in range(ROWS):
                for tx in range(COLS):
                    data += tile(px, tx, ty)
            emit(L, "onb_demo_%s_pal" % n, extra)
            emit(L, "onb_demo_%s" % n, data)
            L.append("")
        with open(os.path.join(OUT, "onb_demo_%s.a65" % suffix), "w") as f:
            f.write("\n".join(L) + "\n")
    for part in range(len(groups), len(BANKS)):          # keep the Makefile's list whole
        with open(os.path.join(OUT, "onb_demo_%s.a65" % SUFFIX[part]), "w") as f:
            f.write("; AUTO-GENERATED by utils/gen_onb_demo.py -- no pictures in this bank.\n"
                    ".link page %s\n" % BANKS[part])
    print("generated onb_demo_a..%s.a65: %d pictures" % (SUFFIX[len(BANKS) - 1], len(DEMOS)))


if __name__ == "__main__":
    main()
