#!/usr/bin/env python3
"""The welcome clip the first-boot tour plays once the language is picked.

Writes, both committed (the build never runs this: it needs Pillow):
  misc/welcome.fmv  the video, full screen 256x224 at 20 fps, plus the "Welcome"
                    line in every language, which the SNES lays over the video
  misc/welcome.pcm  the jingle (MSU-1 PCM, 44.1 kHz 16-bit stereo), then a stretch
                    of silence its loop point sits on: the firmware streams it looped
                    until the tour stops it

    python3 utils/gen_welcome.py                 # write both files
    python3 utils/gen_welcome.py --preview DIR   # plus what the SNES shows, frame by
                                                 # frame (PNG + GIF) and the jingle (WAV)

The SNES side is snes/onboarding/onb_welcome.a65; the firmware stages the video in
PSRAM and streams the jingle (src/menucmd.c, SNES_CMD_ONB_WELCOME).

VIDEO (mode 1, BG1 4bpp, 8 palettes of 15 colours, the backdrop a gradient by HDMA)
A frame is not stored whole: the VRAM holds a pool of 1024 tiles and two tilemaps,
and each frame sends only the tiles the pool lacks (into slots the frame on screen
does not use) and the tilemap words that differ from what the hidden tilemap holds.
The SNES shows frame n while the next three VBlanks (two or three on PAL) receive
frame n+1, then flips the tilemap at the frame's deadline. A frame that would not fit
those VBlanks keeps some tiles of the frame before (the ones that change least);
nmis_needed() replays the player's own rule to decide it, and simulate() replays
the whole file on NTSC and PAL before it is written.

File layout (offsets from the file start, which the firmware puts at $D00000):
  +0   "SWV1"
  +4   u16 frames
  +6   u16 stream offset (the stream starts in the first bank)
  +8   u16 HDMA backdrop table offset ([count, lo, hi]..., 0; CGDATA write-twice)
  +10  u8  overlay line (screen y of the "Welcome" sprites' top)
  +11  u8  languages (8)
  +12  u16 overlay offset per language
  overlay block: u8 sprites, u8 0, u16 tile bytes, sprites x (x, y) u8, tile bytes
                 (to OBJ name table 0; sprite k uses characters (k/8)*32 + (k%8)*2)
  stream: commands, the first byte the opcode
    $00             end of the clip
    $01 a:u16 n:u16 data[n]   VRAM, word address a
    $02 i:u8  n:u16 data[n]   CGRAM, from colour i
    $03 f:u8  b:u8            end of a frame: f bit0 = tilemap buffer ($4000/$4400),
                              bit1 = the overlay shown; b = brightness 0..15
    $04                       the rest of this bank is padding: go on at the next one
  No command crosses a bank (a DMA source does not either).
"""
import math
import os
import random
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_const   # noqa: E402  the menu's char -> glyph table
import fontedit      # noqa: E402  the menu font
import gen_onb_art   # noqa: E402  the real logo

REPO = os.path.join(HERE, "..", "..")
OUT_FMV = os.path.join(REPO, "misc", "welcome.fmv")
OUT_PCM = os.path.join(REPO, "misc", "welcome.pcm")

W, H = 256, 224
TW, TH = 32, 28
FPS = 20
NFRAMES = 110                      # 5.5 s
SLOTS = 1024                       # the VRAM tile pool (words $0000-$3FFF)
TMAP = (0x4000, 0x4400)            # the two tilemaps (words)
NMI_BUDGET = 3584                  # lockstep with WEL_BUDGET_NTSC (onb_welcome.a65): the busiest
                                   # VBlank of the clip ends on line 249 (measured), the VBlank on 261
NMI_BUDGET_PAL = 7168              # lockstep with WEL_BUDGET_PAL
CMD_COST = 256                     # what the player counts per command on top of its bytes: its
                                   # CPU time, ~2100 master cycles = 256 bytes of DMA (measured)
CHUNK = 1024                       # largest command (a VBlank leaves up to one unused)
OAM_COST = 608                     # the flip that shows/hides the overlay DMAs the OAM
NMIS = 3                           # VBlanks per frame on NTSC
BANK = 0x10000
FMV_CAP = 0x100000                 # $D00000-$DFFFFF (src/menucmd.c)
OVERLAY_Y = 140
RATE = 44100
TAIL = RATE // 2                   # the silence after the jingle (its loop)

# the timeline, in frames (the jingle follows the same marks: frame / 20 s)
F_FADE_IN = 8
F_WARP_END = 26                    # the stars rush in, the comets arrive
F_IMPACT = 26
F_SETTLE = 36
F_SHINE = (42, 56)
F_TEXT = (52, 66)                  # the "Welcome" line fades in
F_SPARKLE = (58, 98)
F_FADE_OUT = 100

CX, CY = 128, 90                   # where it all happens: the logo's centre
LOGO_SCALE = 1.75

WELCOME = ["Welcome!", "Bem-vindo!", "¡Bienvenido!", "Willkommen!", "Bienvenue !",
           "Benvenuto!", "Добро пожаловать!",
           "Welkom!"]


def s5(c):
    """RGB -> the 5-bit colour the SNES shows, back as RGB."""
    return tuple((int(v) >> 3) * 255 // 31 for v in c[:3])


def bgr555(c):
    return (c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def ease_out(t):
    return 1 - (1 - t) ** 3


def clamp01(t):
    return max(0.0, min(1.0, t))


# ------------------------------------------------------------------ backdrop

STOPS = [(0, (0, 0, 10)), (70, (2, 10, 44)), (150, (20, 18, 82)), (223, (52, 22, 96))]


def gradient():
    """The backdrop colour of each line (an HDMA table: it costs nothing to vary it
    line by line, and the three channels then step on different lines)."""
    out = []
    for y in range(H):
        for (y0, c0), (y1, c1) in zip(STOPS, STOPS[1:]):
            if y0 <= y <= y1:
                out.append(s5(lerp(c0, c1, (y - y0) / (y1 - y0))))
                break
    return out


GRAD = gradient()


def hdma_table():
    t, y = [], 0
    while y < H:
        n = 1
        while y + n < H and GRAD[y + n] == GRAD[y] and n < 127:
            n += 1
        v = bgr555(GRAD[y])
        t += [n, v & 0xff, v >> 8]
        y += n
    return t + [0]


# ------------------------------------------------------------------ the animation

def logo_art():
    _, logo = gen_onb_art.default_look()
    return logo.crop(logo.getbbox())


LOGO = logo_art()


def scaled_logo(s):
    w, h = LOGO.size
    big = LOGO.resize((w * 4, h * 4), Image.NEAREST)
    return big.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)


class Stars:
    def __init__(self, n=64, seed=7):
        r = random.Random(seed)
        self.s = []
        for _ in range(n):
            self.s.append({"a": r.uniform(0, 2 * math.pi), "d": r.uniform(8, 190),
                           "v": r.uniform(0.6, 1.4), "ph": r.uniform(0, 6.28),
                           "w": r.uniform(0.25, 0.6), "big": r.random() < 0.22,
                           "c": r.choice([(210, 225, 255), (255, 240, 190), (160, 210, 255)])})
        self.r = r

    def speed(self, f):
        if f < F_WARP_END:
            return 0.4 + 6.0 * (f / F_WARP_END) ** 2
        return 0.25

    def step(self, f):
        v = self.speed(f)
        for s in self.s:
            s["prev"] = s["d"]
            s["d"] += v * s["v"] * (0.3 + s["d"] / 90)
            if s["d"] > 200:
                s["d"] = self.r.uniform(6, 30)
                s["a"] = self.r.uniform(0, 2 * math.pi)
                s["prev"] = s["d"]

    def draw(self, im, f, dim):
        d = ImageDraw.Draw(im)
        v = self.speed(f)
        for s in self.s:
            tw = 0.6 + 0.4 * math.sin(s["ph"] + f * s["w"])
            a = clamp01(tw * dim * min(1.0, s["d"] / 30))
            if a < 0.08:
                continue
            ca, sa = math.cos(s["a"]), math.sin(s["a"]) * 0.8
            x, y = CX + ca * s["d"], CY + sa * s["d"]
            col = s["c"] + (round(255 * a),)
            if v > 1.5:                               # warp: a streak back toward the centre
                L = min(26, v * 2.5 * s["v"] * (0.3 + s["d"] / 90))
                d.line([(x - ca * L, y - sa * L), (x, y)], fill=s["c"] + (round(150 * a),), width=1)
            d.point((x, y), fill=col)
            if s["big"]:
                c2 = s["c"] + (round(140 * a),)
                d.point([(x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)], fill=c2)


def comets(im, f):
    """Eight comets closing in on the centre, arriving on the impact."""
    if not (6 <= f <= F_IMPACT):
        return
    d = ImageDraw.Draw(im)
    t = (f - 6) / (F_IMPACT - 6)
    for k in range(8):
        ang = k * math.pi / 4 + 0.35
        dist = 190 * (1 - t) ** 1.6
        x, y = CX + math.cos(ang) * dist, CY + math.sin(ang) * dist * 0.7
        tx, ty = math.cos(ang), math.sin(ang) * 0.7
        L = 18 + 40 * t
        for i in range(10, 0, -1):                    # the tail, fading out
            a = (1 - i / 10) ** 1.5
            x0, y0 = x + tx * L * i / 10, y + ty * L * i / 10
            x1, y1 = x + tx * L * (i - 1) / 10, y + ty * L * (i - 1) / 10
            d.line([(x0, y0), (x1, y1)], fill=(150, 220, 255, round(220 * a)), width=2)
        d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(255, 255, 255, 255))


def impact(im, f):
    k = f - F_IMPACT
    if not (0 <= k < 9):
        return
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    r = 8 + k * 20
    a = round(255 * (1 - k / 9) ** 1.3)
    for w, aa in ((7, 0.35), (3, 1.0)):
        d.ellipse([CX - r, CY - r * 0.62, CX + r, CY + r * 0.62], outline=(170, 230, 255, round(a * aa)), width=w)
    layer = layer.filter(ImageFilter.GaussianBlur(1.2))
    im.alpha_composite(layer)


def flash(im, f):
    """The burst at the centre, OVER the logo: white, gone in four frames."""
    k = f - F_IMPACT
    if not (0 <= k < 4):
        return
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = 44 - k * 10
    a = [235, 170, 100, 45][k]
    ImageDraw.Draw(layer).ellipse([CX - g, CY - g * 0.45, CX + g, CY + g * 0.45], fill=(255, 255, 255, a))
    im.alpha_composite(layer.filter(ImageFilter.GaussianBlur(5)))


def logo_scale(f):
    if f < F_IMPACT:
        return 0
    keys = [(F_IMPACT, 0.35), (F_IMPACT + 3, 1.6), (F_IMPACT + 5, 2.02), (F_IMPACT + 7, 1.68),
            (F_IMPACT + 9, 1.8), (F_SETTLE, LOGO_SCALE)]
    for (f0, s0), (f1, s1) in zip(keys, keys[1:]):
        if f0 <= f <= f1:
            return s0 + (s1 - s0) * ease_out((f - f0) / (f1 - f0))
    return LOGO_SCALE


def logo(im, f):
    s = logo_scale(f)
    if not s:
        return
    lg = scaled_logo(s)
    if F_SHINE[0] <= f <= F_SHINE[1]:                 # a diagonal shine across it
        t = (f - F_SHINE[0]) / (F_SHINE[1] - F_SHINE[0])
        w, h = lg.size
        band = Image.new("L", lg.size, 0)
        bx = -20 + (w + 50) * t
        ImageDraw.Draw(band).polygon([(bx, 0), (bx + 7, 0), (bx + 7 - h * 0.5, h), (bx - h * 0.5, h)], fill=235)
        ImageDraw.Draw(band).polygon([(bx + 11, 0), (bx + 13, 0), (bx + 13 - h * 0.5, h), (bx + 11 - h * 0.5, h)], fill=150)
        band = band.filter(ImageFilter.GaussianBlur(0.8))
        from PIL import ImageChops
        mask = ImageChops.multiply(band, lg.getchannel("A"))
        white = Image.new("RGBA", lg.size, (255, 255, 255, 0))
        white.putalpha(mask)
        lg = lg.copy()
        lg.alpha_composite(white)
    im.alpha_composite(lg, (round(CX - lg.width / 2), round(CY - lg.height / 2)))


SPARKS = []
_r = random.Random(11)
for _f in range(F_SPARKLE[0], F_SPARKLE[1], 3):
    SPARKS.append((_f, _r.uniform(24, 232), _r.choice([_r.uniform(52, 76), _r.uniform(104, 128), _r.uniform(168, 196)])))


def sparkles(im, f):
    d = ImageDraw.Draw(im)
    for f0, x, y in SPARKS:
        k = f - f0
        if not (0 <= k < 10):
            continue
        s = [1, 2, 4, 5, 6, 5, 4, 3, 2, 1][k]
        a = [150, 220, 255, 255, 255, 230, 200, 160, 110, 70][k]
        d.line([(x - s, y), (x + s, y)], fill=(255, 250, 220, a))
        d.line([(x, y - s), (x, y + s)], fill=(255, 250, 220, a))
        if s >= 4:
            d.point([(x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1), (x + 1, y + 1)], fill=(255, 240, 180, a // 2))


def glint(im, f):
    """Where the shine leaves the logo: one big four-point star, on the "+"."""
    k = f - (F_SHINE[1] - 1)
    if not (0 <= k < 9):
        return
    w, h = LOGO.size
    x, y = CX + w * LOGO_SCALE / 2 - 13, CY - h * LOGO_SCALE / 2 + 5
    s = [3, 7, 11, 12, 10, 8, 6, 4, 2][k]
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.polygon([(x - s, y), (x, y - 1.2), (x + s, y), (x, y + 1.2)], fill=(255, 255, 240, 255))
    d.polygon([(x, y - s), (x + 1.2, y), (x, y + s), (x - 1.2, y)], fill=(255, 255, 240, 255))
    d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(255, 255, 255, 255))
    im.alpha_composite(layer.filter(ImageFilter.GaussianBlur(0.5)))


def brightness(f):
    if f < F_FADE_IN:
        return min(15, round(15 * f / F_FADE_IN))
    if f >= F_FADE_OUT:
        return max(0, round(15 * (NFRAMES - 1 - f) / (NFRAMES - 1 - F_FADE_OUT)))
    return 15


def render_frames():
    stars = Stars()
    frames = []
    for f in range(NFRAMES):
        stars.step(f)
        fg = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        stars.draw(fg, f, 1.0 if f < F_IMPACT else 0.7)
        comets(fg, f)
        impact(fg, f)
        logo(fg, f)
        flash(fg, f)
        glint(fg, f)
        sparkles(fg, f)
        frames.append(fg)
    return frames


def composite(fg):
    """The foreground over the backdrop, SNES colours; None where the backdrop shows."""
    px = fg.load()
    out = []
    for y in range(H):
        bg = GRAD[y]
        row = []
        for x in range(W):
            r, g, b, a = px[x, y]
            if a < 16:
                row.append(None)
            else:
                k = a / 255
                row.append(s5((r * k + bg[0] * (1 - k), g * k + bg[1] * (1 - k), b * k + bg[2] * (1 - k))))
        out.append(row)
    return out


# ------------------------------------------------------------------ palettes + tiles

def tiles_of(px):
    """896 tile keys: tuple of 64 colours / None, or None for an empty tile."""
    keys = []
    for ty in range(TH):
        rows = px[ty * 8:ty * 8 + 8]
        for tx in range(TW):
            t = tuple(c for r in rows for c in r[tx * 8:tx * 8 + 8])
            keys.append(None if all(c is None for c in t) else t)
    return keys


def d2(a, b):
    return (a[0] - b[0]) ** 2 * 3 + (a[1] - b[1]) ** 2 * 4 + (a[2] - b[2]) ** 2 * 2


def median_cut(weighted, n=15):
    """Up to n colours for {colour: weight}."""
    cols = list(weighted)
    if len(cols) <= n:
        return cols
    strip = []
    for c, w in weighted.items():
        strip += [c] * max(1, min(64, round(w)))
    im = Image.new("RGB", (len(strip), 1))
    im.putdata(strip)
    q = im.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:n * 3]
    return list(dict.fromkeys(s5(tuple(pal[i:i + 3])) for i in range(0, len(pal), 3)))


def build_palettes(uses, npal=8, iters=6):
    """uses: {tile key: occurrences}. 8 palettes of 15 by k-means over the tiles."""
    tiles = list(uses)
    hist = []
    for t in tiles:
        h = {}
        for c in t:
            if c is not None:
                h[c] = h.get(c, 0) + 1
        hist.append(h)
    weight = [uses[t] ** 0.5 for t in tiles]
    mean = [tuple(sum(c[i] * n for c, n in h.items()) / sum(h.values()) for i in range(3)) for h in hist]
    order = sorted(range(len(tiles)), key=lambda i: (mean[i][0] + mean[i][1] + mean[i][2], mean[i][2] - mean[i][0]))
    assign = [0] * len(tiles)
    for k, i in enumerate(order):
        assign[i] = k * npal // len(order)
    pals = []
    for _ in range(iters):
        pals = []
        for g in range(npal):
            acc = {}
            for i in range(len(tiles)):
                if assign[i] == g:
                    for c, n in hist[i].items():
                        acc[c] = acc.get(c, 0) + n * weight[i]
            pals.append(median_cut(acc) if acc else [(0, 0, 0)])
        allc = {c for h in hist for c in h}
        near = [{c: min(d2(c, p) for p in pal) for c in allc} for pal in pals]
        new = [min(range(npal), key=lambda g: sum(near[g][c] * n for c, n in hist[i].items()))
               for i in range(len(tiles))]
        if new == assign:
            break
        assign = new
    return pals


class Quant:
    """Tile key -> (palette, 32 bytes of 4bpp, the colours the SNES shows)."""

    def __init__(self, pals):
        self.pals = pals
        self.cache = {}
        self.near = [{} for _ in pals]

    def nearest(self, g, c):
        m = self.near[g]
        if c not in m:
            pal = self.pals[g]
            m[c] = min(range(len(pal)), key=lambda k: d2(c, pal[k]))
        return m[c]

    def err(self, g, t):
        pal = self.pals[g]
        return sum(d2(c, pal[self.nearest(g, c)]) for c in t if c is not None)

    def __call__(self, t):
        if t in self.cache:
            return self.cache[t]
        g = min(range(len(self.pals)), key=lambda g: self.err(g, t))
        idx = [0 if c is None else 1 + self.nearest(g, c) for c in t]
        shown = tuple(None if not i else self.pals[g][i - 1] for i in idx)
        r = (g, bytes(planar(idx)), shown)
        self.cache[t] = r
        return r


def planar(idx):
    lo, hi = [], []
    for r in range(8):
        p = [0, 0, 0, 0]
        for c in range(8):
            v = idx[r * 8 + c]
            for k in range(4):
                if v >> k & 1:
                    p[k] |= 0x80 >> c
        lo += p[0:2]
        hi += p[2:4]
    return lo + hi


def cgram(pals):
    out = []
    for pal in pals:
        for c in [(0, 0, 0)] + pal + [(0, 0, 0)] * (15 - len(pal)):
            v = bgr555(c)
            out += [v & 0xff, v >> 8]
    return bytes(out)


# ------------------------------------------------------------------ the overlay

def welcome_text(s, body_top=(255, 236, 150), body_bot=(255, 255, 255), outline=(14, 16, 44)):
    """The menu font at 3x (hires pixels: 1.5 x 3 lowres), gold to white, dark outline."""
    _, font = fontedit.load_font()
    k = 6                                      # draw at 6 px per hires pixel, then halve x
    im = Image.new("RGBA", (len(s) * 8 * k, 8 * k), (0, 0, 0, 0))
    px = im.load()
    for i, ch in enumerate(s):
        code = build_const.ENCODE.get(ch, ord(ch))
        if code >= 256 or (ch != " " and code == 32):
            sys.exit("gen_welcome: no glyph for %r" % ch)
        g = fontedit.tile_to_pixels(font[code])
        for gy in range(8):
            for gx in range(8):
                c = g[gy][gx]
                if not c:
                    continue
                body = lerp(body_top, body_bot, gy / 7)
                rgb = {1: body, 2: outline, 3: lerp(body, outline, 0.5)}[c]
                for dy in range(k):
                    for dx in range(k):
                        px[(i * 8 + gx) * k + dx, gy * k + dy] = rgb + (255,)
    return im.resize((len(s) * 8 * 3 // 2 * 1, 8 * 3), Image.LANCZOS)


def overlay_blocks():
    """Per language: the sprites (16x16) covering the line, and one OBJ palette for all."""
    ims = [welcome_text(s) for s in WELCOME]
    acc = {}
    for im in ims:
        px = im.load()
        for r, g, b, a in (px[x, y] for y in range(im.height) for x in range(im.width)):
            if a >= 128:
                c = s5((r, g, b))
                acc[c] = acc.get(c, 0) + 1
    pal = median_cut(acc)
    blocks = []
    for im in ims:
        if im.width > 240:
            sys.exit("gen_welcome: %d px is too wide" % im.width)
        x0 = (W - im.width) // 2
        cols = (im.width + 15) // 16
        sprites, tiles = [], {}
        for sy in range(2):
            for sx in range(cols):
                cell = []
                for y in range(16):
                    for x in range(16):
                        X, Y = sx * 16 + x, sy * 16 + y
                        if X < im.width and Y < im.height:
                            r, g, b, a = im.getpixel((X, Y))
                            cell.append(0 if a < 128 else 1 + min(range(len(pal)), key=lambda k: d2(s5((r, g, b)), pal[k])))
                        else:
                            cell.append(0)
                if any(cell):
                    k = len(sprites)
                    sprites.append((x0 + sx * 16, OVERLAY_Y + sy * 16))
                    base = (k // 8) * 32 + (k % 8) * 2
                    for q, (ox, oy) in enumerate(((0, 0), (8, 0), (0, 8), (8, 8))):
                        idx = [cell[(oy + y) * 16 + ox + x] for y in range(8) for x in range(8)]
                        tiles[base + (q & 1) + (q >> 1) * 16] = bytes(planar(idx))
        n = max(tiles) + 1 if tiles else 0
        data = b"".join(tiles.get(i, bytes(32)) for i in range(n))
        blocks.append((sprites, data))
    return pal, blocks


def overlay_bg():
    ys = range(OVERLAY_Y, OVERLAY_Y + 24)
    return tuple(round(sum(GRAD[y][i] for y in ys) / len(ys)) for i in range(3))


# ------------------------------------------------------------------ the stream

class Stream:
    def __init__(self, start):
        self.buf = bytearray()
        self.start = start

    def pos(self):
        return self.start + len(self.buf)

    def room(self, n):
        """Pad to the next bank when n bytes would cross it."""
        left = BANK - self.pos() % BANK
        if n > left:
            self.buf.append(0x04)
            self.buf += bytes(left - 1)

    def vram(self, addr, data):
        for i in range(0, len(data), CHUNK):
            part = data[i:i + CHUNK]
            self.room(5 + len(part) + 1)
            self.buf += struct.pack("<BHH", 1, addr + i // 2, len(part)) + part

    def cgram(self, index, data):
        self.room(4 + len(data) + 1)
        self.buf += struct.pack("<BBH", 2, index, len(data)) + data

    def frame(self, flags, bright):
        self.room(3 + 1)
        self.buf += bytes([3, flags, bright])

    def end(self):
        self.room(1)
        self.buf.append(0)


def cost(n):
    return n + CMD_COST


def runs(items):
    """Sorted (slot, data) -> [(first slot, bytes)] of consecutive slots."""
    out = []
    for s, d in sorted(items):
        if out and out[-1][0] + len(out[-1][1]) // 32 == s:
            out[-1] = (out[-1][0], out[-1][1] + d)
        else:
            out.append((s, d))
    return out


def upload_cost(rs):
    return sum(cost(len(d[i:i + CHUNK])) for _, d in rs for i in range(0, len(d), CHUNK))


def chunk_costs(blobs):
    return [cost(len(d[i:i + CHUNK])) for d in blobs for i in range(0, len(d), CHUNK)]


def nmis_needed(costs, first_budget):
    """VBlanks the player takes for these commands: each VBlank runs commands while
    they fit what is left of its budget (the first one always runs)."""
    n, b, any_ = 1, first_budget, False
    for c in costs:
        if any_ and c > b:
            n, b, any_ = n + 1, NMI_BUDGET, False
        b -= c
        any_ = True
    return n


def spans(old, new, gap=CMD_COST // 2):
    """Changed stretches of a tilemap: [(first index, [words])]."""
    out, i = [], 0
    while i < len(new):
        if old[i] == new[i]:
            i += 1
            continue
        j = i + 1
        last = i
        while j < len(new) and j - last <= gap:
            if old[j] != new[j]:
                last = j
            j += 1
        out.append((i, new[i:last + 1]))
        i = last + 1
    return out


def spans_cost(sp):
    return sum(cost(2 * len(w)) for _, w in sp)


def encode(keys_per_frame, quant, extra_per_frame):
    """The tile/tilemap stream. extra_per_frame[f] = (cgram commands, overlay flag)."""
    slot_key = [None] * SLOTS                  # what each VRAM slot holds (slot 0: empty)
    key_slot = {None: 0}
    bufmap = [[0] * (TW * TH), [0] * (TW * TH)]
    shown = [0] * (TW * TH)
    ring = 1
    out = []                                   # per frame: (tile runs, tilemap spans, buffer)
    late = 0
    for f, keys in enumerate(keys_per_frame):
        b = f & 1
        want = []
        for k in keys:
            if k is None:
                want.append(None)
            else:
                want.append(k)
        in_use = {e & 0x3ff for e in shown}
        first_budget = NMI_BUDGET - (OAM_COST if f >= 2 and extra_per_frame[f - 1][1] != extra_per_frame[f - 2][1] else 0)
        pre = [cost(len(d)) for _, d in extra_per_frame[f][0]]
        postponed = set()
        while True:
            entries = []
            new_keys, reuse = {}, set()
            for p, k in enumerate(want):
                if p in postponed:
                    entries.append(shown[p])
                    continue
                if k is None:
                    entries.append(0)
                    continue
                g, _, _ = quant(k)
                qk = quant(k)[1] + bytes([g])
                if qk in key_slot:
                    reuse.add(key_slot[qk])
                    entries.append(("r", qk, g))
                else:
                    new_keys.setdefault(qk, g)
                    entries.append(("n", qk, g))
            free = [s for s in range(1, SLOTS) if s not in in_use and s not in reuse]
            if len(free) < len(new_keys):
                sys.exit("gen_welcome: frame %d needs %d new tiles, %d slots free" % (f, len(new_keys), len(free)))
            # allocate from the ring pointer on: long runs of consecutive slots
            free.sort(key=lambda s: (s - ring) % SLOTS)
            alloc = dict(zip(new_keys, free))
            rs = runs([(alloc[k], k[:32]) for k in new_keys])
            final = []
            for e in entries:
                if isinstance(e, tuple):
                    _, qk, g = e
                    s = key_slot[qk] if e[0] == "r" else alloc[qk]
                    final.append(s | g << 10)
                else:
                    final.append(e)
            sp = spans(bufmap[b], final)
            costs = pre + chunk_costs([d for _, d in rs]) + chunk_costs([b"\0" * (2 * len(w)) for _, w in sp])
            c = sum(costs)
            if f == 0 or nmis_needed(costs, first_budget) <= NMIS:
                break
            # too much: keep the old tile where the change is smallest
            cand = [p for p, e in enumerate(entries) if isinstance(e, tuple) and e[0] == "n" and p not in postponed]
            if not cand:
                cand = [p for p in range(len(want)) if p not in postponed and final[p] != shown[p]]
            cand.sort(key=lambda p: change(quant, keys_per_frame[f][p], slot_key, shown[p]))
            for p in cand[:max(1, len(cand) // 8)]:
                postponed.add(p)
            late = max(late, len(postponed))
        for k, s in alloc.items():
            old = slot_key[s]
            if old is not None and key_slot.get(old) == s:
                del key_slot[old]
            slot_key[s] = k
            key_slot[k] = s
        if new_keys:
            ring = (max(alloc.values()) + 1) % SLOTS or 1
        out.append((rs, sp, b, c, len(postponed)))
        bufmap[b] = final
        shown = final
    return out, late


def change(quant, key, slot_key, entry):
    """How much showing the old tile instead of `key` costs (smaller = postpone first)."""
    old = slot_key[entry & 0x3ff]
    new = quant(key)[2] if key is not None else (None,) * 64
    if old is None:
        oldc = (None,) * 64
    else:
        oldc = quant_shown(old)
    return sum(d2(a or (0, 0, 0), b or (0, 0, 0)) for a, b in zip(new, oldc))


_SHOWN = {}


def quant_shown(qk):
    return _SHOWN[qk]


# ------------------------------------------------------------------ the SNES player, simulated

def simulate(data, nmi_budget, step_of):
    """Replay the stream the way onb_welcome.a65 does. Returns [(nmi of the flip,
    VRAM tilemap buffer, brightness, overlay)] per frame, and the VRAM at each flip."""
    hdr = data[:16]
    so = struct.unpack_from("<H", hdr, 6)[0]
    vram = bytearray(0x10000)
    cg = bytearray(512)
    p = so
    flips = []
    shots = []

    def run(budget, first):
        nonlocal p
        done_any = False
        while True:
            op = data[p]
            if op == 0:
                return "end"
            if op == 4:
                p = (p // BANK + 1) * BANK
                continue
            if op == 3:
                fl, br = data[p + 1], data[p + 2]
                p += 3
                return ("frame", fl, br)
            if op == 1:
                a, n = struct.unpack_from("<HH", data, p + 1)
                if not first and done_any and cost(n) > budget:
                    return None
                vram[a * 2:a * 2 + n] = data[p + 5:p + 5 + n]
                p += 5 + n
            elif op == 2:
                i, n = struct.unpack_from("<BH", data, p + 1)
                if not first and done_any and cost(n) > budget:
                    return None
                cg[i * 2:i * 2 + n] = data[p + 4:p + 4 + n]
                p += 4 + n
            else:
                raise ValueError("bad opcode %02x at %x" % (op, p))
            budget -= cost(n)
            done_any = True

    r = run(0, True)
    nmi = 0
    deadline = 0
    pending = r
    fi = 0
    ov = 0
    while True:
        extra = 0
        if pending and pending != "end" and nmi >= deadline:
            if (pending[1] >> 1 & 1) != ov:
                ov = pending[1] >> 1 & 1
                extra = OAM_COST
            flips.append((nmi, pending[1] & 1, pending[2], pending[1] >> 1 & 1))
            shots.append((bytes(vram), bytes(cg)))
            deadline = (deadline if fi else nmi) + step_of(fi)
            fi += 1
            pending = None
        if pending == "end":
            break
        if pending is None:
            pending = run(nmi_budget - extra, False)
        nmi += 1
        if nmi > 100000:
            raise RuntimeError("runaway")
    return flips, shots


def picture(vram, cg, buf, over=None):
    col = lambda i: (lambda v: ((v & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v >> 10 & 31) * 255 // 31))(cg[i * 2] | cg[i * 2 + 1] << 8)
    im = Image.new("RGB", (W, H))
    px = im.load()
    base = TMAP[buf] * 2
    for ty in range(TH):
        for tx in range(TW):
            e = vram[base + (ty * 32 + tx) * 2] | vram[base + (ty * 32 + tx) * 2 + 1] << 8
            t = vram[(e & 0x3ff) * 32:(e & 0x3ff) * 32 + 32]
            pal = (e >> 10) & 7
            for y in range(8):
                for x in range(8):
                    b = 7 - x
                    v = (t[y * 2] >> b & 1) | (t[y * 2 + 1] >> b & 1) << 1 | (t[16 + y * 2] >> b & 1) << 2 | (t[17 + y * 2] >> b & 1) << 3
                    Y, X = ty * 8 + y, tx * 8 + x
                    px[X, Y] = col(pal * 16 + v) if v else GRAD[Y]
    if over:
        sprites, tiles, ocg = over
        for k, (sx, sy) in enumerate(sprites):
            base = (k // 8) * 32 + (k % 8) * 2
            for q in range(4):
                ch = base + (q & 1) + (q >> 1) * 16
                t = tiles[ch * 32:ch * 32 + 32]
                if len(t) < 32:
                    continue
                for y in range(8):
                    for x in range(8):
                        b = 7 - x
                        v = (t[y * 2] >> b & 1) | (t[y * 2 + 1] >> b & 1) << 1 | (t[16 + y * 2] >> b & 1) << 2 | (t[17 + y * 2] >> b & 1) << 3
                        if v:
                            X, Y = sx + (q & 1) * 8 + x, sy + (q >> 1) * 8 + y
                            if X < W and Y < H:
                                px[X, Y] = col(128 + v) if ocg is None else ocg[v]
    return im


# ------------------------------------------------------------------ the jingle

def synth():
    n = int(NFRAMES / FPS * RATE)
    L = [0.0] * n
    R = [0.0] * n
    sec = lambda f: f / FPS

    def add(t0, dur, fn, pan=0.0, gain=1.0):
        i0 = int(t0 * RATE)
        gl, gr = gain * math.cos((pan + 1) * math.pi / 4), gain * math.sin((pan + 1) * math.pi / 4)
        for i in range(max(0, i0), min(n, i0 + int(dur * RATE))):
            v = fn((i - i0) / RATE)
            L[i] += v * gl
            R[i] += v * gr

    def bell(f0, decay=1.2, bright=2.0):
        def fn(t):
            env = math.exp(-t / decay) * min(1.0, t / 0.004)
            mod = bright * math.exp(-t / (decay * 0.35)) * math.sin(2 * math.pi * f0 * 3.5 * t)
            return env * math.sin(2 * math.pi * f0 * t + mod)
        return fn

    def pad(f0, t_in, t_out, total):
        def fn(t):
            env = min(1.0, t / t_in) * min(1.0, max(0.0, (total - t) / t_out))
            v = 0.0
            for det, g in ((1.0, 1.0), (1.004, 0.7), (0.996, 0.7)):
                ph = 2 * math.pi * f0 * det * t
                v += g * (math.sin(ph) + 0.25 * math.sin(2 * ph) + 0.1 * math.sin(3 * ph))
            return env * v / 2.4
        return fn

    note = lambda m: 440.0 * 2 ** ((m - 69) / 12)
    total = n / RATE
    # the pad: Cmaj9 under everything, a little louder once the logo is there
    for m, pan in ((48, -0.3), (55, 0.3), (64, -0.2), (71, 0.2), (74, 0.0)):
        add(0, total, pad(note(m), 1.2, 0.9, total), pan, 0.06)
    for m, pan in ((60, -0.4), (67, 0.4), (76, 0.0)):
        add(sec(F_IMPACT), total - sec(F_IMPACT), pad(note(m), 0.05, 0.9, total - sec(F_IMPACT)), pan, 0.05)
    # the stars twinkling
    r = random.Random(3)
    penta = [84, 86, 88, 91, 93, 96]
    for k in range(14):
        add(0.1 + k * 0.085 + r.uniform(0, 0.03), 0.5, bell(note(r.choice(penta)), 0.18, 1.2), r.uniform(-0.8, 0.8), 0.07)
    # the rush: noise swelling into the impact, and a rising tone
    rise0, rise1 = 0.35, sec(F_IMPACT)
    state = [0.0]
    rn = random.Random(5)

    def riser(t):
        u = t / (rise1 - rise0)
        a = 0.02 + 0.3 * u ** 2
        state[0] += a * (rn.uniform(-1, 1) - state[0])
        return state[0] * u ** 1.5
    add(rise0, rise1 - rise0, riser, 0.0, 0.5)
    add(rise0, rise1 - rise0, lambda t: math.sin(2 * math.pi * (180 * t + 260 * t * t / (rise1 - rise0))) * (t / (rise1 - rise0)) ** 2, 0.0, 0.08)
    # the impact: a bright chord, a low boom and a burst
    t0 = sec(F_IMPACT)
    for m, pan in ((72, -0.35), (76, 0.35), (79, -0.1), (84, 0.1)):
        add(t0, 2.2, bell(note(m), 0.9, 2.2), pan, 0.13)
    add(t0, 0.6, lambda t: math.sin(2 * math.pi * (85 * t - 30 * t * t)) * math.exp(-t / 0.18), 0.0, 0.5)
    nb = random.Random(9)
    add(t0, 0.25, lambda t: nb.uniform(-1, 1) * math.exp(-t / 0.05), 0.0, 0.25)
    # the shine: a quick glassy sweep, left to right
    s0, s1 = sec(F_SHINE[0]), sec(F_SHINE[1])
    for k in range(8):
        tk = s0 + (s1 - s0) * k / 8
        add(tk, 0.35, bell(note(96 + [0, 2, 4, 7, 9, 12, 14, 16][k]), 0.12, 1.0), -0.8 + 1.6 * k / 7, 0.05)
    # "Welcome": an ascending chime, then its chord ringing out
    w0 = sec(F_TEXT[0])
    for k, m in enumerate((79, 84, 88, 91)):
        add(w0 + k * 0.12, 2.4, bell(note(m), 1.1, 1.6), [-0.4, -0.1, 0.1, 0.4][k], 0.14)
    # sparkles
    for f0, x, _y in SPARKS:
        add(sec(f0), 0.4, bell(note(r.choice(penta) + 12), 0.1, 0.8), (x - 128) / 128, 0.035)
    peak = max(max(abs(v) for v in L), max(abs(v) for v in R)) or 1
    g = 0.7 / peak
    fade = int(0.5 * RATE)
    pcm = bytearray()
    for i in range(n):
        e = min(1.0, (n - 1 - i) / fade)
        for v in (L[i], R[i]):
            s = max(-32767, min(32767, round(v * g * e * 32767)))
            pcm += struct.pack("<h", s)
    return pcm


# ------------------------------------------------------------------ main

def main():
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    print("rendering %d frames..." % NFRAMES)
    frames = [composite(fg) for fg in render_frames()]
    keys = [tiles_of(px) for px in frames]
    uses = {}
    for ks in keys:
        for k in ks:
            if k is not None:
                uses[k] = uses.get(k, 0) + 1
    print("%d distinct tiles; palettes..." % len(uses))
    pals = build_palettes(uses)
    quant = Quant(pals)
    for k in uses:
        g, data, shown = quant(k)
        _SHOWN[data + bytes([g])] = shown
    opal, blocks = overlay_blocks()
    obg = overlay_bg()

    def opal_at(t):
        cols = [lerp(obg, c, t) for c in opal]
        return b"".join(struct.pack("<H", bgr555(c)) for c in [(0, 0, 0)] + cols + [(0, 0, 0)] * (15 - len(cols)))

    extra = []
    for f in range(NFRAMES):
        cmds, shown_ov = [], 0
        if f == 0:
            cmds.append((0, cgram(pals)))
            cmds.append((128, opal_at(0)))
        if F_TEXT[0] <= f <= F_TEXT[1]:
            cmds.append((128, opal_at((f - F_TEXT[0]) / (F_TEXT[1] - F_TEXT[0]))))
        if f >= F_TEXT[0]:
            shown_ov = 1
        extra.append((cmds, shown_ov))
    print("encoding...")
    enc, late = encode(keys, quant, extra)

    # the file: header, overlay blocks, the HDMA table, the stream
    head = bytearray(b"SWV1") + struct.pack("<H", NFRAMES)
    ofs = 12 + 2 * len(blocks)
    over = bytearray()
    offsets = []
    for sprites, data in blocks:
        offsets.append(ofs + len(over))
        over += struct.pack("<BBH", len(sprites), 0, len(data))
        for x, y in sprites:
            over += bytes([x, y])
        over += data
    hd = bytes(hdma_table())
    hd_ofs = ofs + len(over)
    stream_ofs = (hd_ofs + len(hd) + 15) & ~15
    head += struct.pack("<HHBB", stream_ofs, hd_ofs, OVERLAY_Y, len(blocks))
    for o in offsets:
        head += struct.pack("<H", o)
    body = bytes(head) + bytes(over) + hd
    body += bytes(stream_ofs - len(body))
    if stream_ofs > 0x8000:
        sys.exit("gen_welcome: the header does not fit")
    st = Stream(stream_ofs)
    for f, (rs, sp, b, c, post) in enumerate(enc):
        for i, d in extra[f][0]:
            st.cgram(i, d)
        for s, d in rs:
            st.vram(s * 16, d)
        for i, w in sp:
            st.vram(TMAP[b] + i, b"".join(struct.pack("<H", v) for v in w))
        st.frame(b | extra[f][1] << 1, brightness(f))
    st.end()
    data = body + bytes(st.buf)
    if len(data) > FMV_CAP:
        sys.exit("gen_welcome: %d bytes, the PSRAM window holds %d" % (len(data), FMV_CAP))

    # check it plays on time: NTSC (3 VBlanks a frame) and PAL (2 and 3 in turn)
    for name, budget, step in (("NTSC", NMI_BUDGET, lambda i: 3), ("PAL", NMI_BUDGET_PAL, lambda i: 2 + (i & 1))):
        flips, _ = simulate(data, budget, step)
        want = [sum(step(j) for j in range(i)) for i in range(len(flips))]
        lat = max(fl[0] - (flips[0][0] + w) for fl, w in zip(flips, want))
        print("%s: %d frames, worst lateness %d VBlanks" % (name, len(flips), lat))
        if len(flips) != NFRAMES or lat > 0:
            sys.exit("gen_welcome: the clip does not keep time on %s" % name)
    worst = max(e[3] for e in enc[1:])
    print("video: %d bytes, largest frame %d bytes, tiles held back at most %d" % (len(data), worst, late))

    pcm = synth()
    loop = len(pcm) // 4
    with open(OUT_FMV, "wb") as f:
        f.write(data)
    with open(OUT_PCM, "wb") as f:
        f.write(b"MSU1" + struct.pack("<I", loop) + pcm + bytes(4 * TAIL))
    print("wrote %s (%d) and %s (%d)" % (OUT_FMV, len(data), OUT_PCM, 8 + len(pcm) + 4 * TAIL))

    if preview:
        os.makedirs(preview, exist_ok=True)
        _, shots = simulate(data, NMI_BUDGET, lambda i: 3)
        flips, _ = simulate(data, NMI_BUDGET, lambda i: 3)
        sprites, tiles = blocks[1]
        gif = []
        for i, ((vram, cg), fl) in enumerate(zip(shots, flips)):
            ov = (sprites, tiles, None) if fl[3] else None
            im = picture(vram, cg, fl[1], ov)
            k = fl[2] / 15
            im = im.point(lambda v: round(v * k))
            im.save(os.path.join(preview, "f%03d.png" % i))
            gif.append(im.resize((W * 2, H * 2), Image.NEAREST))
        gif[0].save(os.path.join(preview, "welcome.gif"), save_all=True, append_images=gif[1:], duration=50, loop=0)
        import wave
        with wave.open(os.path.join(preview, "welcome.wav"), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(bytes(pcm))
        print("preview in", preview)


if __name__ == "__main__":
    main()
