#!/usr/bin/env python3
"""Draw the onboarding tour's DEMO pictures (onboarding/demo/<id>.png).

Every card has a picture. Where the answer CHANGES THE SCREEN the picture is that
screen: a faithful miniature of the menu at half scale (256x224 -> 128x112), with the
real logo, gradient and layout measured from the real menu, and the text as word bars
(at half scale the menu's 4-pixel glyphs cannot be read anyway, the layout can); the
font-edge cards show the menu font itself, enlarged. The language card shows the
flags. Where nothing changes on screen: one clean icon. A "No" is its picture greyed,
unless the "No" takes something off a real screen: then it is that screen without it
(covers in the lists: the list with no cover; the clip: the card with its still screenshot).

The PNGs (RGBA, 128x112) are the committed source art: gen_onb_demo.py turns them
into sprites 1:1. Redraw one by hand and gen_onb_demo.py takes it as is.

    python3 utils/gen_onb_art.py                  # every picture
    python3 utils/gen_onb_art.py --sheet out.png  # plus a contact sheet
"""
import os
import re
import struct
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SNES = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import fontedit      # noqa: E402  the menu font (font.a65), for the section card's version
import build_const   # noqa: E402  the menu's char -> glyph table
OUT = os.environ.get("ONB_ART_OUT") or os.path.join(SNES, "onboarding", "demo")
W, H = 128, 112

# the menu's colours (sampled from the real screen)
BAR = (82, 8, 255)
DIRS = (255, 255, 132)
FILES = (255, 255, 255)
PCM = (132, 255, 132)
BORDER = (140, 239, 255)
WIN_BG = (0, 16, 41)
DESC_BORDER = (200, 190, 170)
DIM = (165, 165, 165)


# ------------------------------------------------------------------ menu data

def bytes_after(path, label):
    """The .byt values that follow `label` in an .a65, up to the next label."""
    out, on = [], False
    for line in open(path):
        if re.match(r"^%s\b" % re.escape(label), line):
            on = True
            continue
        if on:
            if re.match(r"^[A-Za-z_]", line):
                break
            body = line.split(";")[0].replace(".byt", "")
            out += [int(v[1:], 16) if v.startswith("$") else int(v)
                    for v in re.findall(r"\$[0-9a-fA-F]+|\b\d+\b", body)]
    return out


def bgr(w):
    return ((w & 31) * 255 // 31, (w >> 5 & 31) * 255 // 31, (w >> 10 & 31) * 255 // 31)


def gradient_lines(hdma):
    """hdma_pal_src ([count, lo, hi]..., 0) -> the colour of each of the 224 lines."""
    lines, i = [], 0
    while i + 2 < len(hdma) and hdma[i] and len(lines) < 224:
        lines += [bgr(hdma[i + 1] | hdma[i + 2] << 8)] * (hdma[i] & 0x7f)
        i += 3
    return (lines + [lines[-1]] * 224)[:224]


def logo_image(pal, tiles):
    """The 8bpp header logo (row-major tiles, colours at CGRAM 64) -> RGBA 256x56;
    a 16-column (128 px) logo is left-anchored, as the menu shows it."""
    cols = 32 if len(tiles) >= 32 * 7 * 64 else 16
    im = Image.new("RGBA", (256, 56), (0, 0, 0, 0))
    px = im.load()
    for ti in range(cols * 7):
        t = tiles[ti * 64:ti * 64 + 64]
        if len(t) < 64:
            break
        cy, cx = divmod(ti, cols)
        for y in range(8):
            for x in range(8):
                b = 7 - x
                v = 0
                for pl in range(4):
                    v |= (t[pl * 16 + 2 * y] >> b & 1) << (pl * 2)
                    v |= (t[pl * 16 + 2 * y + 1] >> b & 1) << (pl * 2 + 1)
                k = v - 64
                if v and 0 <= k < len(pal) // 2:
                    px[cx * 8 + x, cy * 8 + y] = bgr(pal[k * 2] | pal[k * 2 + 1] << 8) + (255,)
    return im


def default_look():
    return (gradient_lines(bytes_after(os.path.join(SNES, "const.a65"), "hdma_pal_src")),
            logo_image(bytes_after(os.path.join(SNES, "logo.a65"), "logo_pal"),
                       bytes_after(os.path.join(SNES, "logo.a65"), "logo_tiles")))


def theme_look(path):
    """Gradient + logo of a .thm (FXTHEME1: TOC of slot/length; slot 1 logo palette,
    4 gradient, 8 logo tiles), over the default for what it leaves out."""
    d = open(path, "rb").read()
    n = d[9]
    regions, poff = {}, 16 + n * 4
    for k in range(n):
        slot, _, length = struct.unpack("<BBH", d[16 + k * 4:20 + k * 4])
        regions[slot] = list(d[poff:poff + length])
        poff += length
    grad, logo = default_look()
    if 4 in regions:
        grad = gradient_lines(regions[4])
    if 1 in regions and 8 in regions:
        logo = logo_image(regions[1], regions[8])
    return grad, logo


# ------------------------------------------------------------------ the miniature

class Screen:
    """A 256x224 menu screen, drawn at full (lowres) size and reduced by half."""

    def __init__(self, look):
        grad, logo = look
        self.im = Image.new("RGB", (256, 224))
        for y in range(224):
            self.im.paste(grad[y], (0, y, 256, y + 1))
        self.logo = logo
        self.d = ImageDraw.Draw(self.im)

    def band_logo(self):
        self.im.paste(self.logo, (0, 0), self.logo)

    def words(self, x, y, s, col, shadow=True):
        """Text as word bars: 4 px per hires character, drawn as the thin middle of
        the glyph row (a text line reads as a line, not as a block), a little dimmer
        than the glyphs since letters only fill part of their cell."""
        for m in re.finditer(r"\S+", s):
            x0, x1 = x + m.start() * 4, x + m.end() * 4 - 2
            bg = self.im.getpixel((x0, y + 2))
            c = tuple(round(a * 0.8 + b * 0.2) for a, b in zip(col, bg))
            self.d.rectangle([x0, y + 1, x1, y + 4], fill=c)

    def row(self, r, text, col, right=None, sel=False):
        """Browser row r (0 = the first under the logo): lines 57 + 8r."""
        y = 57 + 8 * r
        if sel:
            self.d.rectangle([2, y - 1, 252, y + 6], fill=BAR)
        self.words(8, y, text, col)
        if right:
            self.words(248 - 4 * len(right), y, right, col)

    def window(self, x, y, w, h, title=None, border=BORDER):
        self.d.rectangle([x, y, x + w, y + h], fill=WIN_BG, outline=border)
        if title:
            tw = 4 * len(title)
            self.d.rectangle([x + 6, y - 3, x + 10 + tw, y + 3], fill=WIN_BG)
            self.words(x + 8, y - 3, title, FILES, shadow=False)

    def footer(self, s):
        self.words(8, 208, s, FILES)

    def cover(self, box, art):
        x0, y0, x1, y1 = box
        self.im.paste(art.resize((x1 - x0, y1 - y0), Image.LANCZOS), (x0, y0))

    def done(self):
        im = self.im.resize((W, H), Image.BOX).convert("RGBA")
        ImageDraw.Draw(im).rectangle([0, 0, W - 1, H - 1], outline=(90, 100, 130, 255))
        return im


def cover_art(w=128, h=176):
    """A made-up box art (no real game): a sky, a planet and a title band."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=(round(30 + 80 * t), round(20 + 30 * t), round(90 + 120 * t)))
    d.ellipse([w * 0.18, h * 0.3, w * 0.82, h * 0.77], fill=(255, 70, 160))
    d.ellipse([w * 0.3, h * 0.36, w * 0.62, h * 0.52], fill=(255, 150, 200))
    for sx, sy in ((0.12, 0.2), (0.8, 0.15), (0.7, 0.85), (0.25, 0.9), (0.9, 0.6)):
        d.rectangle([w * sx, h * sy, w * sx + 3, h * sy + 3], fill=(255, 255, 200))
    d.rectangle([0, 0, w - 1, h * 0.17], fill=(250, 210, 60))
    d.rectangle([w * 0.1, h * 0.05, w * 0.9, h * 0.12], fill=(200, 40, 40))
    d.rectangle([0, 0, w - 1, h - 1], outline=(255, 255, 255), width=3)
    return im


GAMES = ["Astro Blaster (USA)", "Cosmic Quest (Europe)", "Dragon Crest (Japan)",
         "Galaxy Racer (USA)", "Knight Tale (USA)", "Mega Drift (USA)", "Ninja Cats (USA)",
         "Pixel Island (USA)", "Puzzle Tower (USA)", "Sky Pirates (Europe)", "Star Voyager (USA)",
         "Super Soccer 94 (USA)", "Test Game (USA)", "Wizard Keep (USA)"]
FOOTER = "A:Select B:Back X:Menu Y:Context   01/02/2026 03:04:05"


def browser(scr, sel=6):
    scr.band_logo()
    rows = ["Homebrew/", "MSU-1/", "Patches/"] + GAMES
    for r, name in enumerate(rows[:18]):
        d = name.endswith("/")
        scr.row(r, name, DIRS if d else FILES, "<dir>" if d else "1024K", sel=(r == sel))
    scr.footer(FOOTER)


def covers(mode):
    def f():
        scr = Screen(default_look())
        browser(scr)
        if mode == 1:
            scr.cover((161, 0, 255, 128), cover_art())
        elif mode == 2:
            scr.cover((209, 0, 255, 64), cover_art())
        return scr.done()
    return f


def gameinfo(mode):
    """The Y context menu with "Game info" on top (mode 2; the card itself is ficha())."""
    def f():
        scr = Screen(default_look())
        browser(scr, sel=4)
        scr.window(24, 74, 80, 58, "Selected file")
        for k, s in enumerate(("Game info", "Add to favorites", "Cheats", "Set as autoboot",
                               "Delete", "Delete save file")):
            y = 80 + 8 * k
            if k == 0:
                scr.d.rectangle([28, y - 1, 100, y + 6], fill=BAR)
            scr.words(30, y, s, FILES)
        scr.window(24, 150, 212, 22, border=DESC_BORDER)
        scr.words(30, 154, "Show this ROM's info screen (cover,", FILES)
        scr.words(30, 162, "screenshot and metadata) before playing", FILES)
        return scr.done()
    return f


def msu(open_as_game):
    def f():
        scr = Screen(default_look())
        scr.band_logo()
        if open_as_game:    # the folder is listed like a game, with its cover
            rows = ["Homebrew/", "MSU-1 Adventure/", "Patches/"] + GAMES[:12]
            for r, name in enumerate(rows):
                d = name.endswith("/")
                scr.row(r, name, DIRS if d else FILES, "<dir>" if d else "1024K", sel=(r == 1))
            scr.cover((161, 0, 255, 128), cover_art())
        else:               # the folder's inside: the game and its tracks
            rows = [("../", DIRS, "<dir>"), ("msu1adventure.msu", FILES, "16"),
                    ("msu1adventure.sfc", FILES, "2048K")]
            rows += [("msu1adventure-%d.pcm" % k, PCM, "%dM" % (8 + k % 5)) for k in range(1, 12)]
            for r, (name, col, right) in enumerate(rows):
                scr.row(r, name, col, right, sel=(r == 0))
        scr.footer(FOOTER)
        return scr.done()
    return f


def igm_tabs(scr, active):
    """The in-game menu's tab bar (it covers the whole screen, over the game)."""
    x = 6
    for k, t in enumerate(("CHEATS", "SAVESTATES", "SAVES", "GUIDES", "TRAINER")):
        w = 4 * len(t) + 12
        scr.d.rectangle([x, 4, x + w, 18], fill=BAR if k == active else WIN_BG, outline=BORDER)
        scr.words(x + 6, 8, t, FILES, shadow=False)
        x += w + 4
    scr.words(56, 208, "<>:TAB L/R:ENDS A:OPEN B:CLOSE", FILES)


def igm():
    """The in-game menu on its cheats tab."""
    scr = Screen(default_look())
    igm_tabs(scr, 0)
    scr.words(108, 26, "CHEATS: ON", FILES)
    cheats = ["Infinite lives", "Start with 99 coins", "Moon jump", "Invincible",
              "Max health", "All levels open", "Slow motion", "Walk through walls",
              "Infinite time", "Always big", "Super speed", "Low gravity", "Infinite ammo",
              "Unlock all items", "Debug menu", "Skip intro", "Hard mode", "Rainbow colours"]
    for k, s in enumerate(cheats):
        y = 38 + 8 * k
        if k == 2:
            scr.d.rectangle([6, y - 1, 250, y + 6], fill=BAR)
        scr.words(10, y, s, FILES)
        scr.words(230, y, "ON" if k in (0, 2) else "OFF", FILES)
    return scr.done()


def savestates():
    """The in-game menu's savestates tab: the four slots."""
    scr = Screen(default_look())
    igm_tabs(scr, 1)
    scr.window(52, 40, 152, 134)
    scr.words(78, 50, "SELECT SAVESTATE SLOT", DIRS)
    for k in range(4):
        y = 72 + 16 * k
        if k == 0:
            scr.d.rectangle([60, y - 2, 196, y + 7], fill=BAR)
        scr.words(72, y, "SLOT %d" % (k + 1), FILES)
        scr.words(128, y, "OCCUPIED" if k < 2 else "EMPTY", PCM if k < 2 else DIM)
        scr.words(128, y, "", FILES)
    scr.words(64, 156, "Start+R save   Start+L load", FILES)
    return scr.done()


FOLDER = ["../", "Astro Blaster (USA).sfc", "Cosmic Quest (Europe).sfc", "Dragon Crest (Japan).sfc",
          "Galaxy Racer (USA).sfc", "Knight Tale (USA).sfc", "Mega Drift (USA).sfc", "Ninja Cats (USA).sfc",
          "Pixel Island (USA).sfc", "Sky Pirates (Europe).sfc", "Star Voyager (USA).sfc"]


def folder_view(sel):
    scr = Screen(default_look())
    scr.band_logo()
    for r, name in enumerate(FOLDER):
        d = name.endswith("/")
        scr.row(r, name, DIRS if d else FILES, "<dir>" if d else "2048K", sel=(r == sel))
    scr.footer(FOOTER)
    return scr


def reset(mode):
    """Where the reset button takes you: 1 the menu, 2 the game's folder, 3 the same
    folder with the game selected, 4 the same, holding the button (a tap resets the
    game). 0 (off) is an icon: the game just restarts."""
    def f():
        if mode == 0:
            return icon_restart()
        if mode == 1:
            scr = Screen(default_look())
            browser(scr, sel=0)
            return scr.done()
        scr = folder_view(0 if mode == 2 else 7)
        im = scr.done()
        if mode == 4:
            clock_badge(im)
        return im
    return f


def more(kind):
    """The "and more" card: each item's own screen."""
    def f():
        scr = Screen(default_look())
        if kind == "patches":
            browser(scr, sel=-1)
            scr.window(8, 66, 240, 56, "Patches")
            rows = [("Cosmic Quest (Europe).sfc", FILES, None), ("[No patch]", FILES, None),
                    ("Translation", PCM, "IPS auto"), ("Hard mode", PCM, "BPS")]
            for k, (s, col, right) in enumerate(rows):
                y = 72 + 8 * k
                if k == 1:
                    scr.d.rectangle([12, y - 1, 244, y + 6], fill=BAR)
                scr.words(16, y, s, col)
                if right:
                    scr.words(168, y, right, FILES)
            scr.words(16, 108, "A:Play Y:More B:Back <>:Page LR:Ends", FILES)
        elif kind == "memtest":
            browser(scr, sel=-1)
            scr.window(40, 58, 172, 136, "Memory test")
            scr.words(48, 70, "No test has been run yet.", FILES)
            scr.words(48, 176, "A: Wiring test  X: Full test  B: Close", FILES)
            scr.words(48, 184, "Both reset the console. X takes 30s.", FILES)
        return scr.done()
    return f


def trainer():
    """The in-game menu's TRAINER tab on a found address (snes/trainer.i65 tr_draw_detail):
    the frame of ig_frame_geom (cols 5..58, rows 4..21), labels at col 16, values at
    col 34, the actions centred, the bar on FREEZE."""
    scr = Screen(default_look())
    igm_tabs(scr, 4)
    scr.window(20, 32, 216, 144)
    scr.words(128 - 2 * len("RAM TRAINER"), 40, "RAM TRAINER", DIRS)
    for row, label, value in ((7, "ADDRESS", "7E0DBE"), (8, "CURRENT", "$04    (4)"),
                              (11, "Value:", "$63    (99)")):
        scr.words(64, row * 8, label, FILES)
        scr.words(136, row * 8, value, FILES)
    for row, s in ((13, "SET VALUE"), (14, "FREEZE"), (15, "SAVE CHEAT")):
        y = row * 8
        if s == "FREEZE":
            scr.d.rectangle([56, y - 1, 200, y + 6], fill=BAR)
        scr.words(128 - 2 * len(s), y, s, FILES)
    scr.words(128 - 2 * len("A: inspect   B: back"), 160, "A: inspect   B: back", FILES)
    return scr.done()


def saves_tab():
    """The in-game menu's SAVES tab: the 4 battery-save slots."""
    scr = Screen(default_look())
    igm_tabs(scr, 2)
    scr.window(52, 40, 152, 134)
    scr.words(86, 50, "SELECT SRAM SLOT", DIRS)
    for k in range(4):
        y = 72 + 16 * k
        if k == 1:
            scr.d.rectangle([60, y - 2, 196, y + 7], fill=BAR)
        scr.words(72, y, "SLOT %d" % (k + 1), FILES)
    scr.words(64, 156, "Applies on the next game boot", DIM)
    return scr.done()


def pcm_player():
    """The menu's MSU-1 track player over the file list."""
    scr = Screen(default_look())
    scr.band_logo()
    rows = [("Homebrew/", DIRS, "<dir>"), ("MSU-1 Adventure/", DIRS, "<dir>"), ("msu1adventure.sfc", FILES, "2048K")]
    rows += [("msu1adventure-%d.pcm" % k, PCM, "%dM" % (8 + k % 5)) for k in range(1, 9)]
    for r, (name, col, right) in enumerate(rows):
        scr.row(r, name, col, right, sel=(r == 4))
    scr.window(40, 90, 176, 72, "PCM Player")
    scr.words(48, 100, "msu1adventure-2.pcm", FILES)
    scr.d.rectangle([64, 122, 188, 123], fill=FILES)
    scr.d.rectangle([64, 121, 110, 124], fill=PCM)
    scr.words(64, 132, "0:48 / 2:15", FILES)
    scr.words(48, 150, "A: Play/Pause   B: Close", FILES)
    scr.footer(FOOTER)
    return scr.done()


def delete_ctx():
    """The Y menu of the file list on its "Delete save file" row."""
    scr = Screen(default_look())
    browser(scr, sel=4)
    scr.window(24, 74, 80, 58, "Selected file")
    for k, s in enumerate(("Game info", "Add to favorites", "Cheats", "Set as autoboot",
                           "Delete", "Delete save file")):
        y = 80 + 8 * k
        if k == 5:
            scr.d.rectangle([28, y - 1, 100, y + 6], fill=BAR)
        scr.words(30, y, s, FILES)
    scr.window(24, 150, 212, 14, border=DESC_BORDER)
    scr.words(30, 154, "Delete the SRM save file for this ROM", FILES)
    return scr.done()


def bios_popup():
    """The missing-BIOS popup: the menu says which file and stays up."""
    scr = Screen(default_look())
    scr.band_logo()
    scr.window(72, 82, 112, 40)
    scr.words(80, 90, "Required file not found:", FILES)
    scr.words(104, 102, "dsp1.bin", DIRS)
    scr.footer(FOOTER)
    return scr.done()


def option_desc():
    """A settings screen: the description box over the focused option."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    scr.window(30, 76, 60, 120, "Main Menu", border=DIM)
    scr.window(36, 84, 60, 108, "Configuration", border=DIM)
    scr.window(40, 98, 170, 100, "Browser Settings")
    for k, (s, v) in enumerate((("Sort directories", "Yes"), ("Hide file extensions", "No"),
                                ("Open MSU-1 folders as games", "On"), ("Show sd2snes folder", "Off"),
                                ("Screensaver", "On"), ("LED brightness", "15"), ("Show covers", "Large"),
                                ("Favorites/Recent", "Yes"), ("Menu music", "On"), ("Menu sounds", "On"))):
        y = 106 + 8 * k
        if k == 1:
            scr.d.rectangle([44, y - 1, 206, y + 6], fill=BAR)
        scr.words(48, y, s, FILES)
        scr.words(204 - 4 * len(v), y, v, DIRS)
    scr.window(40, 62, 170, 14, border=DESC_BORDER)
    scr.words(46, 66, "Choose whether to hide file extensions", FILES)
    return scr.done()


def shortcut_list():
    """The in-game menu's shortcut list (SELECT on the tab bar)."""
    scr = Screen(default_look())
    igm_tabs(scr, -1)
    scr.words(92, 26, "IN-GAME SHORTCUTS", DIRS)
    rows = (("Open this menu", "L+R+Y+Left"), ("Save state", "Start+R"), ("Load state", "Start+L"),
            ("Choose slot", "Select+R"), ("Reset game", "L+R+Sel+Start"), ("Reset to menu", "L+R+Sel+X"),
            ("Cheats on", "L+R+Sel+A"), ("Cheats off", "L+R+Sel+B"))
    for k, (a, b) in enumerate(rows):
        y = 40 + 10 * k
        scr.words(24, y, a, FILES)
        scr.words(140, y, b, PCM)
    scr.words(100, 128, "MENU KEYS", DIRS)
    scr.words(24, 142, "Left/Right: tab   L/R: first/last   A: open", FILES)
    scr.words(24, 152, "B: back   START: close   SELECT: this help", FILES)
    return scr.done()


def section_217():
    """The 2.17 section's opening card: the sd2snes+ logo and the version."""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    grad, logo = default_look()
    lg = logo.resize((128, 28), Image.LANCZOS)
    im.paste(lg, (0, 14), lg)
    draw_menu_text(im, 64, 56, "2.17", 3, 5, (248, 208, 56))
    return im


_, _FONT = fontedit.load_font()


def draw_menu_text(im, cx, y, s, xs, ys, body):
    """The menu font, xs x ys per pixel, centred on cx (white body swapped for `body`,
    the dark outline kept) -- crisp, drawn after any reduction."""
    px = im.load()
    x0 = round(cx - len(s) * 8 * xs / 2)
    for i, ch in enumerate(s):
        g = fontedit.tile_to_pixels(_FONT[build_const.ENCODE.get(ch, ord(ch))])
        for gy in range(8):
            for gx in range(8):
                c = g[gy][gx]
                if not c:
                    continue
                rgb = {1: body, 2: (18, 22, 52), 3: tuple(v // 2 for v in body)}[c]
                for dy in range(ys):
                    for dx in range(xs):
                        X, Y = x0 + (i * 8 + gx) * xs + dx, y + gy * ys + dy
                        if 0 <= X < W and 0 <= Y < H:
                            px[X, Y] = rgb + (255,)


# ------------------------------------------------------------------ flags

S = 4


# a language spoken in more than one big country shows two flags, cut on the diagonal
# (first top-left); each flag's emblem is moved into its visible half: (cx, cy, scale)
# (Portuguese is the fork's pt-BR: Brazil's flag alone)
SPLIT = {"en": ("us", "uk"), "es": ("es", "mx")}
EMBLEM = {("mx", 1): (0.5, 0.7, 1.0)}


def flag(code, w, h):
    """A language's flag at w x h: one country, or two cut on the diagonal (the first
    top-left)."""
    if code not in SPLIT:
        return country_flag({"ptbr": "br"}.get(code, code), w, h)
    a, b = (country_flag(c, w, h, *EMBLEM.get((c, k), (0.5, 0.5, 1.0)))
            for k, c in enumerate(SPLIT[code]))
    mask = Image.new("L", (w * S, h * S), 0)
    ImageDraw.Draw(mask).polygon([(0, 0), (w * S, 0), (0, h * S)], fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)
    b.paste(a, (0, 0), mask)
    return b


def country_flag(code, w, h, ex=0.5, ey=0.5, es=1.0):
    """A country's flag drawn at 4x and reduced; (ex, ey, es) place and scale its
    emblem (Brazil, Mexico, Portugal) for a split flag."""
    im = Image.new("RGB", (w * S, h * S))
    d = ImageDraw.Draw(im)
    W4, H4 = w * S, h * S

    def tri(cols, vertical=True):
        for k, c in enumerate(cols):
            d.rectangle([W4 * k // 3, 0, W4 * (k + 1) // 3, H4] if vertical
                        else [0, H4 * k // 3, W4, H4 * (k + 1) // 3], fill=c)

    if code == "de":
        tri([(0, 0, 0), (221, 0, 0), (255, 206, 0)], False)
    elif code == "fr":
        tri([(0, 85, 164), (255, 255, 255), (239, 65, 53)])
    elif code == "it":
        tri([(0, 146, 70), (255, 255, 255), (206, 43, 55)])
    elif code == "ru":
        tri([(255, 255, 255), (0, 57, 166), (213, 43, 30)], False)
    elif code == "nl":
        tri([(174, 28, 40), (255, 255, 255), (33, 70, 139)], False)
    elif code == "es":
        d.rectangle([0, 0, W4, H4], fill=(170, 21, 27))
        d.rectangle([0, H4 // 4, W4, H4 * 3 // 4], fill=(241, 191, 0))
    elif code == "mx":
        tri([(0, 104, 71), (255, 255, 255), (206, 17, 38)])
        r = H4 * 0.16 * es
        d.ellipse([W4 * ex - r, H4 * ey - r, W4 * ex + r, H4 * ey + r], fill=(140, 90, 40))
    elif code == "us":
        red, white, blue = (178, 34, 52), (255, 255, 255), (60, 59, 110)
        for k in range(13):
            d.rectangle([0, H4 * k / 13, W4, H4 * (k + 1) / 13], fill=red if k % 2 == 0 else white)
        d.rectangle([0, 0, W4 * 0.4, H4 * 7 / 13], fill=blue)
        for ry in range(4):
            for rx in range(5):
                x, y = W4 * 0.4 * (rx + 0.5) / 5, H4 * 7 / 13 * (ry + 0.5) / 4
                d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=white)
    elif code == "br":
        d.rectangle([0, 0, W4, H4], fill=(0, 156, 59))
        cx, cy = W4 * ex, H4 * ey
        hw, hh = (W4 / 2 - H4 * 0.17) * es, (H4 / 2 - H4 * 0.12) * es
        d.polygon([(cx, cy - hh), (cx + hw, cy), (cx, cy + hh), (cx - hw, cy)], fill=(255, 223, 0))
        r = H4 * 0.26 * es
        # the globe, its white band and stars drawn on their own layer, clipped to the disc
        globe = Image.new("RGB", (W4, H4), (0, 39, 118))
        g = ImageDraw.Draw(globe)
        # the band, by the flag's official construction (Lei 5.700, in modules of r/3.5):
        # arcs of radius 8 and 8.5 centred on the flag's bottom edge (7 below the globe's
        # centre), 2 to the left of its vertical diameter -- so it crosses the globe just
        # above the middle, higher on the left
        m = r / 3.5
        bx, by = cx - 2 * m, cy + 7 * m
        g.ellipse([bx - 8.5 * m, by - 8.5 * m, bx + 8.5 * m, by + 8.5 * m], fill=(255, 255, 255))
        g.ellipse([bx - 8 * m, by - 8 * m, bx + 8 * m, by + 8 * m], fill=(0, 39, 118))
        # a few stars under the band
        for sx, sy, sr in ((-0.45, 0.25, 0.07), (-0.15, 0.45, 0.06), (0.2, 0.3, 0.07), (0.45, 0.05, 0.05),
                           (0.05, 0.62, 0.05), (0.35, 0.55, 0.05), (-0.5, 0.55, 0.05), (0.6, -0.2, 0.05)):
            x, y, q = cx + sx * r, cy + sy * r, sr * r
            g.ellipse([x - q, y - q, x + q, y + q], fill=(255, 255, 255))
        mask = Image.new("L", (W4, H4), 0)
        ImageDraw.Draw(mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        im.paste(globe, (0, 0), mask)
    elif code == "uk":
        blue, red, white = (1, 33, 105), (200, 16, 46), (255, 255, 255)
        d.rectangle([0, 0, W4, H4], fill=blue)
        t = H4 * 0.2
        d.line([(0, 0), (W4, H4)], fill=white, width=round(t))
        d.line([(0, H4), (W4, 0)], fill=white, width=round(t))
        d.line([(0, 0), (W4, H4)], fill=red, width=round(t / 3))
        d.line([(0, H4), (W4, 0)], fill=red, width=round(t / 3))
        d.rectangle([W4 / 2 - t * 0.85, 0, W4 / 2 + t * 0.85, H4], fill=white)
        d.rectangle([0, H4 / 2 - t * 0.85, W4, H4 / 2 + t * 0.85], fill=white)
        d.rectangle([W4 / 2 - t / 2, 0, W4 / 2 + t / 2, H4], fill=red)
        d.rectangle([0, H4 / 2 - t / 2, W4, H4 / 2 + t / 2], fill=red)
    return im.resize((w, h), Image.LANCZOS)


LANGS = ["en", "ptbr", "es", "de", "fr", "it", "ru", "nl"]


def flags(focus):
    """The focused language's flag large, all eight in a strip below, the focused one lit."""
    def f():
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rectangle([20, 8, 107, 67], fill=(18, 22, 52, 255))
        im.paste(flag(LANGS[focus], 84, 56), (22, 10))
        d.rectangle([21, 9, 106, 66], outline=(255, 255, 255, 255))
        for k, c in enumerate(LANGS):
            x, y = 3 + (k % 4) * 31, 76 + (k // 4) * 18
            small = flag(c, 26, 14)
            if k != focus:
                small = Image.blend(small, Image.new("RGB", small.size, (10, 20, 50)), 0.55)
            d.rectangle([x - 1, y - 1, x + 26, y + 14], fill=(18, 22, 52, 255))
            im.paste(small, (x, y))
            if k == focus:
                d.rectangle([x - 2, y - 2, x + 27, y + 15], outline=BORDER + (255,))
        return im
    return f


# ------------------------------------------------------------------ icons
# (the cards whose answer changes no screen: a sound, a button)

INK = (18, 22, 52)
CYAN = (72, 216, 248)
PINK = (248, 64, 150)
YELLOW = (248, 208, 56)
WHITE = (248, 248, 248)
LIGHT = (196, 206, 232)


class Icon:
    """A drawing in 128x112 coordinates, made at 4x and reduced (smooth edges)."""

    def __init__(self):
        self.im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    def P(self, pts):
        return [(x * S, y * S) for x, y in pts]

    def poly(self, pts, fill, out=INK, w=2.0):
        self.d.polygon(self.P(pts), fill=fill, outline=out, width=round(w * S))

    def line(self, pts, col, w):
        self.d.line(self.P(pts), fill=col, width=round(w * S), joint="curve")
        for x, y in (pts[0], pts[-1]):
            r = w * S / 2
            self.d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=col)

    def arc(self, cx, cy, r, a0, a1, col, w):
        self.d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], a0, a1, fill=col, width=round(w * S))

    def ellipse(self, box, fill, out=INK, w=2.0):
        self.d.ellipse([v * S for v in box], fill=fill, outline=out, width=round(w * S))

    def rrect(self, box, r, fill, out=INK, w=2.0):
        self.d.rounded_rectangle([v * S for v in box], r * S, fill=fill, outline=out, width=round(w * S))

    def arrow(self, pts, col, w=5, head=9):
        import math
        self.line(pts, col, w)
        (xa, ya), (xb, yb) = pts[-2], pts[-1]
        a = math.atan2(yb - ya, xb - xa)
        self.d.polygon(self.P([(xb + math.cos(a) * head * 0.6, yb + math.sin(a) * head * 0.6),
                               (xb + math.cos(a + 2.4) * head, yb + math.sin(a + 2.4) * head),
                               (xb + math.cos(a - 2.4) * head, yb + math.sin(a - 2.4) * head)]), fill=col)

    def note(self, x, y, col, k=1.0):
        self.ellipse((x - 7 * k, y - 5 * k, x + 5 * k, y + 5 * k), col, w=1.5)
        self.rrect((x + 2.5 * k, y - 28 * k, x + 5.5 * k, y), 1, col, w=1.2)
        self.poly([(x + 3 * k, y - 28 * k), (x + 15 * k, y - 22 * k), (x + 15 * k, y - 15 * k),
                   (x + 4 * k, y - 20 * k)], col, w=1.2)

    def done(self):
        return self.im.resize((W, H), Image.LANCZOS)


def icon_music():
    ic = Icon()
    ic.poly([(18, 44), (34, 44), (56, 24), (56, 88), (34, 68), (18, 68)], CYAN, w=2.5)
    for r in (14, 24):
        ic.arc(58, 56, r, -50, 50, WHITE, 4)
    ic.note(96, 50, YELLOW)
    ic.note(106, 88, PINK, 0.8)
    return ic.done()


def icon_random():
    ic = Icon()
    ic.arrow([(14, 36), (40, 36), (80, 78), (110, 78)], YELLOW)
    ic.arrow([(14, 78), (40, 78), (80, 36), (110, 36)], CYAN)
    ic.note(58, 104, PINK, 0.6)
    return ic.done()


def icon_sounds():
    ic = Icon()
    ic.poly([(30, 22), (30, 86), (46, 72), (58, 98), (68, 94), (56, 68), (78, 68)], WHITE, w=2.5)
    for r in (12, 22, 32):
        ic.arc(78, 56, r, -45, 45, YELLOW, 4)
    return ic.done()


def snes_pad(ic, cx, cy, grey=False):
    """A Super Nintendo controller seen from above (the Super Famicom / PAL colours of
    its four buttons), 100 x 44, centred on (cx, cy)."""
    def c(rgb):
        if not grey:
            return rgb
        l = round((rgb[0] * 3 + rgb[1] * 4 + rgb[2] * 2) / 9 * 0.5)
        return (l, l, round(l * 1.15))
    body, shade, dark = c((206, 206, 214)), c((168, 168, 180)), c((52, 52, 62))
    x0, x1, y0, y1 = cx - 50, cx + 50, cy - 22, cy + 22
    # L / R shoulders peeking out above the body
    ic.rrect((x0 + 8, y0 - 4, x0 + 38, y0 + 10), 6, shade, w=1.5)
    ic.rrect((x1 - 38, y0 - 4, x1 - 8, y0 + 10), 6, shade, w=1.5)
    ic.rrect((x0, y0, x1, y1), 22, body, w=2)
    # d-pad in its round well
    ic.ellipse((cx - 44, cy - 15, cx - 14, cy + 15), shade, out=None)
    k = 4.2
    ic.poly([(cx - 29 - k, cy - 12), (cx - 29 + k, cy - 12), (cx - 29 + k, cy - k), (cx - 17, cy - k),
             (cx - 17, cy + k), (cx - 29 + k, cy + k), (cx - 29 + k, cy + 12), (cx - 29 - k, cy + 12),
             (cx - 29 - k, cy + k), (cx - 41, cy + k), (cx - 41, cy - k), (cx - 29 - k, cy - k)], dark, w=1)
    # select / start, slanted
    for sx in (-10, 3):
        ic.poly([(cx + sx, cy + 2), (cx + sx + 8, cy - 3), (cx + sx + 10, cy), (cx + sx + 2, cy + 5)],
                dark, out=None)
    # the four buttons in their tilted well: X top, A right, B bottom, Y left
    ic.ellipse((cx + 12, cy - 18, cx + 48, cy + 18), shade, out=None)
    bx, by = cx + 30, cy
    for (dx, dy), col in (((0, -9), (60, 90, 220)), ((9, 0), (220, 40, 50)),
                          ((0, 9), (240, 200, 40)), ((-9, 0), (40, 170, 80))):
        ic.ellipse((bx + dx - 5.5, by + dy - 5.5, bx + dx + 5.5, by + dy + 5.5), c(col), w=1.2)


def icon_pad2(on):
    """Two controllers: player 2's lit (it can use the in-game shortcuts) or greyed."""
    def f():
        ic = Icon()
        snes_pad(ic, 64, 30)
        snes_pad(ic, 64, 82, grey=not on)
        return ic.done()
    return f


def icon_gbc():
    """A colour handheld: purple body, a lit colour screen, d-pad and two buttons."""
    ic = Icon()
    body, dark = (118, 72, 190), (60, 34, 110)
    ic.rrect((34, 4, 94, 108), 10, body, w=2.5)
    ic.rrect((40, 10, 88, 54), 5, (70, 72, 86), w=1.5)
    # the screen: sky, sun, hills -- colour is the point; drawn apart, then set in the bezel
    sw, sh = 36 * S, 32 * S
    scr = Image.new("RGBA", (sw, sh), (120, 190, 255, 255))
    sd = ImageDraw.Draw(scr)
    sd.ellipse([26 * S, 3 * S, 33 * S, 10 * S], fill=(255, 214, 60))
    sd.ellipse([-8 * S, 18 * S, 22 * S, 44 * S], fill=(70, 186, 92))
    sd.ellipse([12 * S, 20 * S, 46 * S, 46 * S], fill=(40, 150, 80))
    sd.rectangle([0, 27 * S, sw, sh], fill=(196, 104, 56))
    ic.im.paste(scr, (46 * S, 16 * S))
    ic.rrect((38, 70, 58, 76), 1.5, dark, out=None)
    ic.rrect((45, 63, 51, 83), 1.5, dark, out=None)
    ic.ellipse((70, 72, 80, 82), (220, 40, 90), w=1.2)
    ic.ellipse((80, 64, 90, 74), (220, 40, 90), w=1.2)
    for k in range(4):
        ic.line([(66 + k * 5, 98), (72 + k * 5, 90)], dark, 1.6)
    return ic.done()


def icon_bsx():
    """A cartridge with a memory pack plugged in its top slot."""
    ic = Icon()
    grey, dgrey = (190, 190, 200), (120, 120, 134)
    ic.rrect((24, 34, 104, 104), 4, grey, w=2.5)
    ic.rrect((34, 58, 94, 98), 2, (230, 230, 236), w=1.5)
    ic.d.rectangle([36 * S, 62 * S, 92 * S, 94 * S], fill=(90, 40, 170))
    ic.ellipse((52, 66, 76, 90), (255, 214, 60), out=None)
    for k in range(5):
        ic.line([(34, 40 + k * 3), (94, 40 + k * 3)], dgrey, 1)
    # the pack, standing in the slot on top
    ic.rrect((44, 6, 84, 44), 3, (40, 60, 150), w=2)
    ic.rrect((50, 12, 78, 30), 2, (120, 190, 255), w=1)
    ic.d.rectangle([46 * S, 36 * S, 82 * S, 42 * S], fill=(200, 170, 60))
    return ic.done()


def icon_restart():
    ic = Icon()
    ic.arc(64, 56, 30, 60, 350, WHITE, 7)
    ic.d.polygon(ic.P([(94, 36), (100, 60), (78, 52)]), fill=WHITE)
    ic.d.polygon(ic.P([(56, 44), (56, 68), (76, 56)]), fill=CYAN)
    return ic.done()


def clock_badge(im):
    """A small clock on a miniature (hold the button)."""
    ic = Icon()
    ic.ellipse((96, 80, 122, 106), WHITE, w=2.5)
    ic.line([(109, 93), (109, 85)], INK, 2.5)
    ic.line([(109, 93), (115, 96)], INK, 2.5)
    b = ic.done()
    im.paste(b, (0, 0), b)


def dim(pic):
    """The "No" version of a picture: the same, greyed and dark."""
    def f():
        im = pic()
        px = im.load()
        for y in range(H):
            for x in range(W):
                r, g, b, a = px[x, y]
                l = round((r * 3 + g * 4 + b * 2) / 9 * 0.45)
                px[x, y] = (l, l, round(l * 1.15), a)
        return im
    return f



# ---- 2.17: the community cartridges, the browser icons, cheats from the game info card

def icon_carts_seta():
    """A Seta cartridge with its coprocessor on the board, and an opened padlock: the
    copy-protected bootlegs that now boot from the untouched dump."""
    ic = Icon()
    grey, dgrey = (190, 190, 200), (120, 120, 134)
    ic.rrect((10, 18, 78, 100), 4, grey, w=2.5)
    for k in range(5):
        ic.line([(18, 24 + k * 3), (70, 24 + k * 3)], dgrey, 1)
    ic.rrect((18, 44, 70, 92), 2, (40, 120, 70), w=1.5)          # the board
    ic.rrect((28, 56, 60, 80), 2, (36, 36, 44), w=1.2)           # the chip
    for k in range(5):
        ic.line([(31 + k * 7, 52), (31 + k * 7, 56)], (220, 200, 120), 1.4)
        ic.line([(31 + k * 7, 80), (31 + k * 7, 84)], (220, 200, 120), 1.4)
    # padlock, opened
    ic.arc(103, 52, 12, 180, 330, LIGHT, 5)
    ic.rrect((86, 52, 120, 84), 4, YELLOW, w=2)
    ic.ellipse((99, 62, 107, 70), INK, out=None)
    ic.rrect((101.5, 66, 104.5, 76), 1, INK, out=None)
    im = ic.done()
    draw_menu_text(im, 44, 60, "ST018", 1, 1, (248, 248, 248))
    return im


def carts_more():
    """The Super 20 in 1 multicart and a file from the SNES Classic (.sfrom)."""
    ic = Icon()
    grey, dgrey = (190, 190, 200), (120, 120, 134)
    ic.rrect((2, 18, 62, 100), 4, grey, w=2.5)
    for k in range(5):
        ic.line([(10, 24 + k * 3), (54, 24 + k * 3)], dgrey, 1)
    ic.rrect((10, 44, 54, 92), 2, (200, 60, 70), w=1.5)          # the label
    # a document with a folded corner: the .sfrom file
    ic.poly([(70, 30), (112, 30), (126, 44), (126, 100), (70, 100)], LIGHT, w=2)
    ic.poly([(112, 30), (112, 44), (126, 44)], (170, 170, 180), w=1.5)
    im = ic.done()
    draw_menu_text(im, 32, 56, "20", 1, 1, (248, 248, 248))
    draw_menu_text(im, 32, 70, "in 1", 1, 1, (248, 248, 248))
    draw_menu_text(im, 97, 64, "sfrom", 1, 1, INK)
    return im


def browser_icons():
    """The browser with a type icon at the start of every row: the real glyphs in their real
    print palettes (snes/diricon.a65 di_str / di_pal, drawn by dir_icon like consoles_icons)."""
    scr = Screen(default_look())
    scr.band_logo()
    rows = [("../", DIRS, "parent"), ("Homebrew/", DIRS, "folder"),
            ("Hero Quest (MSU-1)/", DIRS, "msu"), ("Astro Blaster (USA).sfc", FILES, "snes"),
            ("Battle Theme.spc", PCM, "spc"), ("Cave Runner.nes", FILES, "nes"),
            ("Dot Hunter.gbc", FILES, "gbc"), ("Midnight.thm", FILES, "theme"),
            ("Moon Base.sms", FILES, "sms"), ("Pocket Quest.gb", FILES, "gb"),
            ("River Raid 2.a26", FILES, "a26"), ("Boss Theme.pcm", PCM, "pcm")]
    for r, (name, col, kind) in enumerate(rows):
        y = 57 + 8 * r
        if r == 2:
            scr.d.rectangle([2, y - 1, 252, y + 6], fill=BAR)
        dir_icon(scr, 8, y - 1, kind)
        scr.words(20, y, name, col)
        right = "<dir>" if name.endswith("/") else "1024K"
        scr.words(248 - 4 * len(right), y, right, col)
    scr.words(8, 208, "A:Select B:Back X:Menu Y:Context", FILES)
    return scr.done()


def gameinfo_cheats():
    """The game info card with the cheat list SELECT opens over it."""
    scr = Screen(default_look())
    scr.cover((29, 0, 121, 126), cover_art())
    scr.words(8, 138, "Cosmic Quest (Europe)", FILES)
    scr.words(8, 152, "Publisher   Nova Soft", DIRS)
    scr.footer("A:Play B:Back Y:Desc Sel:Cheats")
    scr.window(56, 50, 176, 110, "Cheats for Cosmic Quest")
    for k, (name, on) in enumerate((("Infinite lives", 1), ("Start with 99 coins", 0), ("Moon jump", 0),
                                    ("Invincible", 1), ("Max health", 0), ("All levels open", 0))):
        y = 64 + 10 * k
        if k == 1:
            scr.d.rectangle([60, y - 1, 228, y + 6], fill=BAR)
        scr.words(64, y, name, FILES)
        scr.words(208, y, "Yes" if on else "No", PCM if on else FILES)
    scr.words(64, 148, "A:On/Off Y:Edit Sel:Add", FILES)
    return scr.done()

# ---- the base tour's later cards: theme restore, the font edges, covers in the lists, the
#      game info video and its music, the cheat list, a patch's Y menu, Sufami Turbo, the
#      other consoles' buttons, the Competition Cart round, the card's folders, the sd2snes
#      folder, the Mk.II LED; and four more "and more" items

def settings_window(scr, title, rows, sel, desc=None, x=40, y=98, w=170, parents=True):
    """A Configuration submenu over the browser, the way option_desc draws it: the main
    menu and Configuration behind, `rows` of (label, value), the bar on `sel`."""
    if parents:
        scr.window(x - 10, y - 22, 60, 8 * len(rows) + 20, "Main Menu", border=DIM)
        scr.window(x - 4, y - 14, 60, 8 * len(rows) + 8, "Configuration", border=DIM)
    scr.window(x, y, w, 8 * len(rows) + 8, title)
    for k, (s, v) in enumerate(rows):
        yy = y + 8 + 8 * k
        if k == sel:
            scr.d.rectangle([x + 4, yy - 1, x + w - 4, yy + 6], fill=BAR)
        scr.words(x + 8, yy, s, FILES)
        if v:
            scr.words(x + w - 6 - 4 * len(v), yy, v, DIRS)
    if desc:
        scr.window(x, y - 36, w, 14, border=DESC_BORDER)
        scr.words(x + 6, y - 32, desc, FILES)


def theme_restore():
    """Browser Settings on "Restore classic theme", over the classic look it brings back."""
    scr = Screen(theme_look(os.path.join(SNES, "..", "misc", "classic.thm")))
    browser(scr, sel=-1)
    settings_window(scr, "Browser Settings",
                    (("Show covers", "Large"), ("Menu music", "On"), ("Menu sounds", "On"),
                     ("Text outline", "Theme"), ("Text anti-aliasing", "Theme"),
                     ("Restore theme", ""), ("Restore classic theme", ""), ("Restore music", "")),
                    6, "Bring back the classic teal theme")
    return scr.done()




def recent_list(covers_on):
    """The Recent games list over the browser, with the box art of the game under the bar.
    Geometry of the real window (last_win_x/y/w = 2/12/60, height = entries + 2, measured on
    the menu): the full width of the screen, frame from line 84, rows from line 90 every 8;
    the large cover is a sprite, so it sits OVER the right end of the window."""
    def f():
        scr = Screen(default_look())
        browser(scr, sel=-1)
        n = 10                                  # MAX_RECENT_GAMES
        scr.window(9, 84, 237, 8 * n + 10, "Recent games")
        for k, s in enumerate(GAMES[:n]):
            y = 90 + 8 * k
            if k == 0:
                scr.d.rectangle([14, y - 1, 240, y + 6], fill=BAR)
            scr.words(16, y, s, FILES)
        if covers_on:
            scr.cover((161, 0, 255, 128), cover_art())
        return scr.done()
    return f


def screenshot_art(w, h):
    """A made-up game screen (no real game): sky, hills, a hero and a coin row."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=(round(90 + 60 * t), round(150 + 50 * t), 255))
    d.ellipse([-w * 0.2, h * 0.55, w * 0.6, h * 1.3], fill=(70, 186, 92))
    d.ellipse([w * 0.4, h * 0.6, w * 1.3, h * 1.4], fill=(40, 150, 80))
    d.rectangle([0, h * 0.82, w, h], fill=(196, 104, 56))
    d.rectangle([w * 0.3, h * 0.55, w * 0.38, h * 0.82], fill=(230, 60, 60))
    d.ellipse([w * 0.29, h * 0.44, w * 0.39, h * 0.56], fill=(255, 210, 170))
    for k in range(4):
        x = w * (0.55 + 0.1 * k)
        d.ellipse([x, h * 0.3, x + w * 0.06, h * 0.38], fill=(255, 214, 60))
    return im



def icon_clip_music():
    """A film frame with a note: the clip's soundtrack."""
    ic = Icon()
    ic.rrect((10, 22, 80, 92), 4, (36, 36, 44), w=2)
    for y in range(26, 90, 10):
        ic.rrect((13, y, 19, y + 5), 1, WHITE, out=None)
        ic.rrect((71, y, 77, y + 5), 1, WHITE, out=None)
    ic.rrect((22, 30, 68, 84), 2, (120, 190, 255), w=1.2)
    ic.poly([(38, 46), (38, 68), (56, 57)], WHITE, w=1.2)
    for r in (10, 18):
        ic.arc(86, 56, r, -50, 50, WHITE, 4)
    ic.note(108, 50, YELLOW)
    ic.note(116, 88, PINK, 0.7)
    return ic.done()


def cheat_list_paged():
    """The menu's cheat list: a page of codes, the long name of the selected one scrolled
    and the page counter."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    scr.window(16, 62, 224, 132, "Cheats for Cosmic Quest")
    scr.words(24, 74, "Description", FILES)
    scr.words(200, 74, "Enabled", FILES)
    rows = (("Infinite lives", 1), ("es with 99 coins and a key", 0), ("Moon jump", 0),
            ("Invincible", 1), ("Max health", 0), ("All levels open", 0), ("Slow motion", 0),
            ("Walk through walls", 1), ("Infinite time", 0), ("Always big", 0))
    for k, (s, on) in enumerate(rows):
        y = 90 + 8 * k
        if k == 1:
            scr.d.rectangle([20, y - 1, 236, y + 6], fill=BAR)
        scr.words(24, y, s, FILES)
        scr.words(212, y, "Yes" if on else "No", PCM if on else FILES)
    scr.words(108, 172, "< 1/3 >", DIRS)
    scr.words(32, 184, "A:On/Off  Y:Edit  Sel:Add  B:Exit", FILES)
    return scr.done()


def patch_y_menu():
    """The patch list with [Y] on a patch: Header mode and Create patched ROM."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    scr.window(8, 66, 240, 56, "Patches")
    rows = [("Cosmic Quest (Europe).sfc", FILES, None), ("[No patch]", FILES, None),
            ("Translation", PCM, "IPS auto"), ("Hard mode", PCM, "BPS")]
    for k, (s, col, right) in enumerate(rows):
        y = 72 + 8 * k
        if k == 2:
            scr.d.rectangle([12, y - 1, 244, y + 6], fill=(40, 30, 110))
        scr.words(16, y, s, col)
        if right:
            scr.words(168, y, right, FILES)
    scr.window(40, 112, 176, 30, "Selected file")
    for k, (s, v) in enumerate((("Header mode", "Auto (detect)"), ("Create patched ROM", ""))):
        y = 120 + 8 * k
        if k == 1:
            scr.d.rectangle([44, y - 1, 212, y + 6], fill=BAR)
        scr.words(48, y, s, FILES)
        if v:
            scr.words(210 - 4 * len(v), y, v, DIRS)
    scr.window(40, 152, 176, 22, border=DESC_BORDER)
    scr.words(46, 156, "Save a patched copy next to the ROM,", FILES)
    scr.words(46, 164, "with cover, saves, cheats and guides", FILES)
    return scr.done()


def sufami_slot_b():
    """The Slot B picker: the other minicarts of the folder, or none."""
    scr = Screen(default_look())
    scr.band_logo()
    rows = ["../", "Moon Derby (Japan).st", "Puzzle Pals (Japan).st", "Tiny Farm (Japan).st",
            "Star Kids (Japan).st"]
    for r, name in enumerate(rows):
        scr.row(r, name, DIRS if name.endswith("/") else FILES, "<dir>" if r == 0 else "1024K", sel=(r == 1))
    scr.window(16, 106, 224, 64, "Sufami Turbo - Slot B")
    for k, s in enumerate(("[No cart in Slot B]", "Puzzle Pals (Japan).st", "Tiny Farm (Japan).st",
                           "Star Kids (Japan).st")):
        y = 116 + 10 * k
        if k == 1:
            scr.d.rectangle([20, y - 1, 236, y + 6], fill=BAR)
        scr.words(26, y, s, FILES)
    scr.footer(FOOTER)
    return scr.done()


def icon_atari_controls():
    """The Atari 2600's console switches, each labelled with the SNES button that works it."""
    ic = Icon()
    ic.poly([(6, 30), (122, 30), (126, 66), (2, 66)], (36, 32, 30), w=2)           # the black top
    ic.d.rectangle([2 * S, 66 * S, 126 * S, 96 * S], fill=(120, 72, 36))             # woodgrain front
    for k in range(4):
        ic.line([(4, 72 + k * 6), (124, 72 + k * 6)], (96, 56, 28), 1)
    ic.rrect((2, 66, 126, 96), 2, None, w=2)
    xs = (16, 50, 84, 112)
    for x in xs:
        ic.rrect((x - 6, 38, x + 6, 58), 2, (200, 200, 206), w=1.5)               # a switch
        ic.rrect((x - 3, 34, x + 3, 46), 1.5, (240, 240, 244), w=1.2)
    im = ic.done()
    # only the SNES buttons are written: the switches' own names would be English in
    # every language (the card's text names them)
    for x, s in zip(xs, ("X", "L R", "SEL", "ST")):
        draw_menu_text(im, x, 6, s, 1, 2, YELLOW)
    return im


def icon_round_timer():
    """A stopwatch at 6:00, the events' round."""
    ic = Icon()
    ic.rrect((56, 4, 72, 14), 2, LIGHT, w=2)
    ic.line([(98, 22), (106, 14)], LIGHT, 5)
    ic.ellipse((20, 14, 108, 102), WHITE, w=3)
    ic.ellipse((28, 22, 100, 94), (230, 236, 248), w=1.5)
    for k in range(12):
        import math
        a = k * math.pi / 6
        ic.line([(64 + math.sin(a) * 30, 58 - math.cos(a) * 30), (64 + math.sin(a) * 34, 58 - math.cos(a) * 34)], INK, 2)
    im = ic.done()
    draw_menu_text(im, 64, 48, "6:00", 2, 2, (220, 40, 50))
    return im


def card_folders():
    """The card's tree: /sd2snes/saves/nes/AS/<game>.srm, beside states and cheats."""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rows = ((0, "sd2snes", 1), (1, "saves", 1), (2, "nes", 1), (3, "AS", 1), (4, "Astro.srm", 0),
            (1, "states", 1), (1, "cheats", 1))
    d = ImageDraw.Draw(im)
    for k, (lvl, name, folder) in enumerate(rows):
        x, y = 4 + lvl * 8, 4 + k * 15
        if lvl:
            d.line([(x - 6, y - 7), (x - 6, y + 4), (x - 1, y + 4)], fill=LIGHT + (255,), width=1)
        if folder:
            d.rectangle([x, y + 1, x + 11, y + 9], fill=DIRS + (255,), outline=INK + (255,))
            d.rectangle([x, y - 1, x + 5, y + 1], fill=DIRS + (255,), outline=INK + (255,))
        else:
            d.polygon([(x + 1, y - 1), (x + 8, y - 1), (x + 11, y + 2), (x + 11, y + 10), (x + 1, y + 10)],
                      fill=WHITE + (255,), outline=INK + (255,))
        tw = len(name) * 8
        draw_menu_text(im, x + 14 + tw // 2, y + 1, name, 1, 1, DIRS if folder else WHITE)
    return im


def sd2snes_dir():
    """The root of the card with the sd2snes folder listed (and selected)."""
    scr = Screen(default_look())
    scr.band_logo()
    rows = ["Homebrew/", "MSU-1/", "Patches/", "sd2snes/"] + GAMES[:12]
    for r, name in enumerate(rows):
        d = name.endswith("/")
        scr.row(r, name, DIRS if d else FILES, "<dir>" if d else "1024K", sel=(r == 3))
    scr.footer(FOOTER)
    return scr.done()



def clear_ppu():
    """A patched intro before and after: the menu's leftovers as garbage, then clean."""
    import random
    rnd = random.Random(7)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for x0, clean in ((2, False), (68, True)):
        d.rectangle([x0, 20, x0 + 57, 72], fill=(10, 12, 30, 255), outline=LIGHT + (255,))
        d.rectangle([x0 + 8, 36, x0 + 49, 44], fill=(240, 200, 60, 255))
        d.rectangle([x0 + 14, 54, x0 + 43, 58], fill=WHITE + (255,))
        if not clean:
            for _ in range(70):
                x, y = rnd.randrange(x0 + 2, x0 + 52), rnd.randrange(22, 66)
                c = rnd.choice(((82, 8, 255), (255, 255, 132), (140, 239, 255), (248, 248, 248), (60, 60, 90)))
                d.rectangle([x, y, x + 3, y + 3], fill=c + (255,))
    d.polygon([(60, 42), (66, 46), (60, 50)], fill=WHITE + (255,))
    # a cross under the garbage and a check under the clean one, no words (the
    # picture is the same in every language)
    ic = Icon()
    ic.line([(22, 80), (42, 100)], (230, 80, 80), 5)
    ic.line([(42, 80), (22, 100)], (230, 80, 80), 5)
    ic.line([(84, 91), (92, 99), (108, 81)], (90, 220, 110), 5)
    return Image.alpha_composite(im, ic.done())


def bus_compat():
    """In-game Settings on "Bus timing compat"."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    settings_window(scr, "In-game Settings",
                    (("In-game hook", "On"), ("In-game buttons", "On"), ("Savestates", "On"),
                     ("Reset to menu", "ROM"), ("1CHIP transient fixes", "Off"),
                     ("Bus timing compat", "On"), ("Brightness limit", "15")),
                    5, "Release the cart bus earlier (1.11.0)")
    return scr.done()


def sys_info():
    """System Information, the model line lit."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    scr.window(16, 60, 224, 140, "System Information")
    lines = ("Firmware version: 1.11.2-br-2.17", "", "SD card: SDHC 32GB  FAT32",
             "Free: 18.2GB of 29.7GB", "", "CIC state: Pair mode", "Last game: Cosmic Quest",
             "Companion: none", "", "Temperature: 32C", "Clock: 01/02/2026 03:04:05")
    scr.words(24, 70, "Model: sd2snes Mk.III", YELLOW)
    for k, s in enumerate(lines):
        scr.words(24, 82 + 9 * k, s, FILES)
    return scr.done()


def clock_prompt():
    """The time prompt at start: the date in the language's order, the bar on the day."""
    scr = Screen(default_look())
    browser(scr, sel=-1)
    scr.window(40, 84, 176, 56, "Please set the time")
    scr.d.rectangle([62, 103, 74, 111], fill=BAR)
    scr.words(64, 105, "01/02/2026  03:04:05", DIRS)
    scr.words(56, 124, "A: Set  B: Later", FILES)
    return scr.done()


# ---- the font-edge cards, redrawn so the edge reads: the menu's real text colours (palette.a65:
#      white body, dark grey outline, light grey half-tone; yellow for folders), enlarged, over
#      the menu's selection bar (where the outline matters most) and over the gradient

def menu_palettes():
    """palette.a65's BG palettes as [(c0, c1, c2, c3)...] (the label's own line included)."""
    vals, on = [], False
    for line in open(os.path.join(SNES, "palette.a65")):
        if re.match(r"^palette\b", line):
            on = True
        elif on and re.match(r"^[A-Za-z_]", line):
            break
        if on:
            body = line.split(";")[0].replace("palette", "").replace(".byt", "")
            vals += [int(v[1:], 16) if v.startswith("$") else int(v)
                     for v in re.findall(r"\$[0-9a-fA-F]+|\b\d+\b", body)]
    words = [vals[k] | vals[k + 1] << 8 for k in range(0, len(vals) - 1, 2)]
    return [tuple(bgr(w) for w in words[k:k + 4]) for k in range(0, len(words) - 3, 4)]


def draw_edge_text(im, x0, y, s, xs, ys, pal, mode):
    """The menu font as the firmware's remap leaves it (src/theme.c), xs x ys per pixel, from
    x0, in palette `pal` (body, outline, half-tone): "full" all three; "nooutline" the outline
    ring gone (whatever is below shows through); "noaa" the half-tone folded into the body."""
    px = im.load()
    cols = {1: pal[1], 2: pal[2], 3: pal[3]}
    if mode == "nooutline":
        cols[2] = None
    elif mode == "noaa":
        cols[3] = pal[1]
    for i, ch in enumerate(s):
        g = fontedit.tile_to_pixels(_FONT[build_const.ENCODE.get(ch, ord(ch))])
        for gy in range(8):
            for gx in range(8):
                rgb = cols.get(g[gy][gx])
                if not g[gy][gx] or rgb is None:
                    continue
                for dy in range(ys):
                    for dx in range(xs):
                        X, Y = x0 + (i * 8 + gx) * xs + dx, y + gy * ys + dy
                        if 0 <= X < W and 0 <= Y < H:
                            px[X, Y] = rgb + (255,)


def theme_badge(im):
    """"Theme": a palette (the .thm icon) with the red plus of the sd2snes+ logo -- with the
    default theme, following the theme gives the same letters as On."""
    ic = Icon()
    ic.ellipse((101, 3, 126, 28), LIGHT, w=2)
    for k, c in enumerate(((220, 60, 60), (60, 160, 230), (240, 200, 60))):
        x, y = (106, 108, 115)[k], (7, 16, 8)[k]
        ic.ellipse((x, y, x + 6, y + 6), c, out=None)
    ic.d.rectangle([115 * S, 18 * S, 125 * S, 22 * S], fill=(232, 40, 56))
    ic.d.rectangle([118 * S, 15 * S, 122 * S, 25 * S], fill=(232, 40, 56))
    b = ic.done()
    im.paste(b, (0, 0), b)


def text_edges(mode, badge=False):
    """"So" at 6x over the selection bar, then three list rows at 2x: one over the gradient,
    one over the bar, a folder (yellow) over the gradient."""
    def f():
        pals = menu_palettes()
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, W - 1, 53], fill=BAR + (255,))
        d.rectangle([0, 76, W - 1, 93], fill=BAR + (255,))
        draw_edge_text(im, 10, 6, "So", 6, 6, pals[0], mode)
        draw_edge_text(im, 4, 58, "Zeltro.sfc", 1, 2, pals[0], mode)
        draw_edge_text(im, 4, 77, "Vorkan.sfc", 1, 2, pals[0], mode)
        draw_edge_text(im, 4, 96, "MSU-1/", 1, 2, pals[1], mode)
        if badge:
            theme_badge(im)
        return im
    return f


# ---- the game info card, with the band's real geometry (snes/gameinfo.a65): the cover
#      letterboxed in the 128x128 box at x 8..136, the screenshot / clip box 96x72 at (152, 24)
#      (GI_FMV_COL0/ROW0), title row 17, Publisher 19, Developer 20, Year|Genre|Players 21,
#      description 23-24, footer 26; labels white, values green (print palette 2)

def ficha(shot=True, play=False):
    def f():
        scr = Screen(default_look())
        art = cover_art()
        cw = round(128 * art.width / art.height)
        scr.cover((8 + (128 - cw) // 2, 0, 8 + (128 - cw) // 2 + cw, 128), art)
        if shot:
            box = (152, 24, 248, 96)
            scr.im.paste(screenshot_art(96, 72), box[:2])
            if play:   # the clip: a play badge over its box
                cx, cy = 200, 60
                scr.d.ellipse([cx - 15, cy - 15, cx + 15, cy + 15], fill=INK, outline=WHITE, width=3)
                scr.d.polygon([(cx - 6, cy - 9), (cx - 6, cy + 9), (cx + 10, cy)], fill=WHITE)
        scr.words(8, 136, "Cosmic Quest (Europe)", FILES)
        for y, lbl, val in ((152, "Publisher", "Nova Soft"), (160, "Developer", "Nova Soft")):
            scr.words(8, y, lbl, FILES)
            scr.words(72, y, val, PCM)
        scr.words(176, 160, "DSP-1", PCM)
        for x, s, col in ((8, "Year", FILES), (28, "1994", PCM), (60, "Genre", FILES), (88, "RPG", PCM),
                          (160, "Players", FILES), (200, "1", PCM)):
            scr.words(x, 168, s, col)
        for k, s in enumerate(("A space opera across nine planets, with",
                               "turn-based battles and a crew of heroes.")):
            scr.words(8, 184 + 8 * k, s, FILES)
        scr.footer("A:Play B:Back Y:Desc Up/Down:Game")
        return scr.done()
    return f


# ---- the other consoles: their ROMs in the list with the 2.17 type icons (snes/diricon.a65:
#      the two glyphs of each icon from the font, in the icon's print palette)

DI_GLYPHS = {"parent": (162, 163, 1), "folder": (160, 161, 1), "snes": (164, 165, 0),
             "nes": (166, 167, 6), "sms": (168, 169, 3), "gb": (170, 171, 6), "gbc": (170, 171, 3),
             "a26": (172, 173, 1), "spc": (174, 175, 2), "pcm": (244, 245, 2),
             "theme": (246, 247, 0), "file": (248, 249, 6), "msu": (164, 165, 1)}


def dir_icon(scr, x, y, kind):
    """A browser icon at lowres (x, y): two hires glyphs = 8 x 8 lowres, each pair of hires
    pixels averaged into one."""
    a, b, p = DI_GLYPHS[kind]
    pal = menu_palettes()[p]
    for k, code in enumerate((a, b)):
        g = fontedit.tile_to_pixels(_FONT[code])
        for gy in range(8):
            for lx in range(4):
                v = [g[gy][lx * 2], g[gy][lx * 2 + 1]]
                cs = [pal[c] for c in v if c]
                if not cs:
                    continue
                bg = scr.im.getpixel((x + k * 4 + lx, y + gy))
                cs += [bg] * (2 - len(cs))
                scr.im.putpixel((x + k * 4 + lx, y + gy), tuple((c1 + c2) // 2 for c1, c2 in zip(*cs)))


def consoles_icons():
    scr = Screen(default_look())
    scr.band_logo()
    rows = [("../", "parent"), ("Block Puzzle.a26", "a26"), ("Cave Runner.nes", "nes"),
            ("Dot Hunter.gbc", "gbc"), ("Moon Base.sms", "sms"), ("Neon Racer.nes", "nes"),
            ("Pocket Quest.gb", "gb"), ("River Raid 2.a26", "a26"), ("Sea Diver.sms", "sms"),
            ("Star Fort.nes", "nes")]
    for r, (name, kind) in enumerate(rows):
        y = 57 + 8 * r
        d = name.endswith("/")
        if r == 2:
            scr.d.rectangle([2, y - 1, 252, y + 6], fill=BAR)
        dir_icon(scr, 8, y - 1, kind)
        scr.words(20, y, name, DIRS if d else FILES)
        right = "<dir>" if d else "40K"
        scr.words(248 - 4 * len(right), y, right, DIRS if d else FILES)
    scr.footer(FOOTER)
    return scr.done()


# ---- more special chips: SPC7110 with its clock, Super FX 3, Sufami Turbo, the competition carts

def chip_ic(ic, x0, y0, x1, y1):
    """A black chip with gold pins on its long sides."""
    n = int((x1 - x0) // 7)
    for k in range(n):
        x = x0 + 4 + k * (x1 - x0 - 8) / max(1, n - 1)
        ic.line([(x, y0 - 4), (x, y0)], (220, 200, 120), 2)
        ic.line([(x, y1), (x, y1 + 4)], (220, 200, 120), 2)
    ic.rrect((x0, y0, x1, y1), 2, (52, 52, 64), out=(150, 156, 180), w=1.5)


def icon_chips_more():
    ic = Icon()
    # SPC7110 and its real-time clock
    chip_ic(ic, 4, 12, 58, 40)
    ic.ellipse((44, 30, 62, 48), WHITE, w=2)
    ic.line([(53, 39), (53, 33)], INK, 2)
    ic.line([(53, 39), (58, 41)], INK, 2)
    # Super FX 3
    chip_ic(ic, 70, 12, 124, 40)
    # Sufami Turbo: the base with its two minicart slots, two minicarts in it
    ic.rrect((6, 82, 60, 106), 4, (200, 200, 210), w=2)
    for x, col in ((10, (230, 80, 90)), (34, (80, 140, 230))):
        ic.rrect((x, 60, x + 22, 88), 2, col, w=1.5)
        ic.rrect((x + 4, 64, x + 18, 76), 1, (240, 240, 244), out=None)
    # the competition carts: a trophy
    ic.d.pieslice([74 * S, 50 * S, 114 * S, 92 * S], 0, 180, fill=YELLOW, outline=INK, width=2 * S)
    ic.rrect((74, 54, 114, 72), 2, YELLOW, out=None)
    ic.line([(74, 54), (74, 72)], INK, 2)
    ic.line([(114, 54), (114, 72)], INK, 2)
    ic.line([(74, 54), (114, 54)], INK, 2)
    for cx, a0, a1 in ((74, 90, 270), (114, 270, 90)):
        ic.arc(cx, 64, 7, a0, a1, YELLOW, 3)
    ic.rrect((90, 90, 98, 98), 1, YELLOW, w=1.5)
    ic.rrect((80, 98, 108, 106), 2, (150, 100, 40), w=1.5)
    im = ic.done()
    draw_menu_text(im, 25, 22, "7110", 1, 1, WHITE)
    draw_menu_text(im, 97, 22, "FX3", 1, 1, WHITE)
    return im


# ---- the Mk.II LED codes: the cart's three LEDs (green ready, yellow read, red write) in the two
#      states the siren swaps between, and what is missing; no words (the same in every language)

LED_COLS = {"g": (60, 220, 90), "y": (250, 210, 50), "r": (235, 50, 50)}


def led_trio(ic, x, y, lit):
    for k, c in enumerate("gyr"):
        col = LED_COLS[c] if c in lit else tuple(v // 4 + 16 for v in LED_COLS[c])
        cx = x + 6 + k * 13
        if c in lit:      # a halo: the colour half over the tour's dark background
            ic.ellipse((cx - 8, y - 8, cx + 8, y + 8), tuple((v + b) // 2 for v, b in zip(col, (0, 24, 48))),
                       out=None)
        ic.ellipse((cx - 5, y - 5, cx + 5, y + 5), col, w=1.5)


def swap_arrows(ic, x, y):
    ic.arrow([(x, y - 4), (x + 12, y - 4)], WHITE, w=2, head=5)
    ic.arrow([(x + 12, y + 4), (x, y + 4)], WHITE, w=2, head=5)


def sd_card(ic, x, y):
    """An SD card: dark blue, cut corner, gold contacts (the file beside it is white)."""
    ic.poly([(x, y), (x + 12, y), (x + 17, y + 5), (x + 17, y + 22), (x, y + 22)], (50, 80, 190), out=LIGHT, w=1.5)
    for k in range(3):
        ic.line([(x + 3 + k * 4, y + 3), (x + 3 + k * 4, y + 7)], (230, 200, 90), 1.6)


def doc_file(ic, x, y):
    ic.poly([(x, y), (x + 11, y), (x + 17, y + 6), (x + 17, y + 22), (x, y + 22)], WHITE, w=1.5)
    ic.poly([(x + 11, y), (x + 11, y + 6), (x + 17, y + 6)], (170, 170, 180), w=1)
    for k in range(3):
        ic.line([(x + 3, y + 11 + k * 4), (x + 13, y + 11 + k * 4)], (120, 126, 150), 1.2)


def red_cross(ic, x, y, r=7):
    ic.line([(x - r, y - r), (x + r, y + r)], (240, 60, 60), 3.5)
    ic.line([(x + r, y - r), (x - r, y + r)], (240, 60, 60), 3.5)


def led_row(ic, y, a, b, cause):
    led_trio(ic, 2, y, a)
    swap_arrows(ic, 46, y)
    led_trio(ic, 62, y, b)
    if cause == "sd":
        sd_card(ic, 104, y - 11)
        red_cross(ic, 113, y)
    elif cause == "file":
        doc_file(ic, 104, y - 11)
        red_cross(ic, 113, y)


def mk2_cart(ic):
    """The sd2snes Mk.II from the front, its three LEDs on the label."""
    grey, dgrey = (190, 190, 200), (120, 120, 134)
    ic.rrect((34, 2, 94, 44), 4, grey, w=2)
    for k in range(4):
        ic.line([(42, 7 + k * 3), (86, 7 + k * 3)], dgrey, 1)
    ic.rrect((42, 20, 86, 40), 2, (40, 60, 150), w=1.5)
    for k, c in enumerate("gyr"):
        ic.ellipse((49 + k * 11, 26, 57 + k * 11, 34), LED_COLS[c], w=1)


def icon_led2():
    """Mk.II on top; green <-> red = no SD card, green <-> yellow = /sd2snes/fpga_mini.bit missing."""
    ic = Icon()
    mk2_cart(ic)
    led_row(ic, 55, "g", "r", "sd")      # rows centred inside a sprite row (32..63, 64..95)
    led_row(ic, 87, "g", "y", "file")
    return ic.done()


PICTURES = {
    **{"lang_" + c: flags(k) for k, c in enumerate(LANGS)},
    "covers_off": covers(0), "covers_large": covers(1), "covers_small": covers(2),
    "gameinfo_on": ficha(True), "gameinfo_ctx": gameinfo(2),
    "music_on": icon_music, "music_off": dim(icon_music),
    "random_on": icon_random, "random_off": dim(icon_random),
    "sfx_on": icon_sounds, "sfx_off": dim(icon_sounds),
    "igm_on": igm, "igm_off": dim(igm),
    "savestates_on": savestates, "savestates_off": dim(savestates),
    "pad2_on": icon_pad2(True), "pad2_off": icon_pad2(False),
    "msu_on": msu(True), "msu_off": msu(False),
    **{"reset_%d" % k: reset(k) for k in range(5)},
    "patches": more("patches"),
    "consoles": consoles_icons, "memtest": more("memtest"),
    "saves": saves_tab, "pcm": pcm_player, "bsx": icon_bsx, "delete": delete_ctx,
    "bios": bios_popup, "desc": option_desc,
    "section_217": section_217, "gbc": icon_gbc, "shortcuts": shortcut_list,
    "carts_seta": icon_carts_seta, "carts_more": carts_more, "icons": browser_icons,
    "gameinfo_cheats": gameinfo_cheats,
    "theme_restore": theme_restore,
    "text_theme": text_edges("full", badge=True), "text_full": text_edges("full"),
    "text_nooutline": text_edges("nooutline"), "text_noaa": text_edges("noaa"),
    "lists_on": recent_list(True), "lists_off": recent_list(False),
    "video_on": ficha(True, True), "video_off": ficha(True),
    "clipmusic_on": icon_clip_music, "clipmusic_off": dim(icon_clip_music),
    "cheatlist": cheat_list_paged, "patchmenu": patch_y_menu, "sufami": sufami_slot_b,
    "atari": icon_atari_controls, "cctime": icon_round_timer,
    "folders": card_folders, "sd2snesdir_on": sd2snes_dir, "sd2snesdir_off": dim(sd2snes_dir),
    "led": icon_led2, "clearppu": clear_ppu, "buscompat": bus_compat, "sysinfo": sys_info,
    "clock": clock_prompt, "trainer": trainer,
    "chips_more": icon_chips_more,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in PICTURES.items():
        fn().save(os.path.join(OUT, name + ".png"))
    print("drew %d pictures in %s" % (len(PICTURES), OUT))
    if "--sheet" in sys.argv:
        dst = sys.argv[sys.argv.index("--sheet") + 1]
        cols = 6
        names = list(PICTURES)
        rows = (len(names) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (W + 4) * 2, rows * (H + 4) * 2), (60, 60, 60))
        for i, n in enumerate(names):
            bg = Image.new("RGB", (W, H), (0, 40, 80))
            art = Image.open(os.path.join(OUT, n + ".png"))
            bg.paste(art, (0, 0), art)
            sheet.paste(bg.resize((W * 2, H * 2), Image.NEAREST),
                        ((i % cols) * (W + 4) * 2, (i // cols) * (H + 4) * 2))
        sheet.save(dst)


if __name__ == "__main__":
    main()
