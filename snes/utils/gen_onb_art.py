#!/usr/bin/env python3
"""Draw the onboarding tour's DEMO pictures (onboarding/demo/<id>.png).

Only the cards whose answer CHANGES THE SCREEN get a picture, and the picture is
that screen: a faithful miniature of the menu at half scale (256x224 -> 128x112),
with the real logo, gradient and layout measured from the real menu, and the text
as word bars (at half scale the menu's 4-pixel glyphs cannot be read anyway, the
layout can). The language card shows the flags. The other cards have no picture.

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
    def f():
        scr = Screen(default_look())
        if mode == 1:       # the game info card
            scr.cover((29, 0, 121, 126), cover_art())
            scr.words(8, 138, "Cosmic Quest (Europe)", FILES)
            scr.words(8, 152, "Publisher   Nova Soft", DIRS)
            scr.words(8, 160, "Developer   Nova Soft", DIRS)
            scr.words(8, 168, "Year 1994  Genre RPG     Players 1", DIRS)
            for k, s in enumerate(("A space opera across nine planets, with",
                                   "turn-based battles and a crew of heroes.")):
                scr.words(8, 180 + 8 * k, s, DIM)
            scr.footer("A:Play B:Back Y:Desc Up/Down:Game")
        else:               # the Y context menu, "Game info" on top
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
        elif kind == "cheats":
            browser(scr, sel=-1)
            scr.window(24, 74, 208, 128, "Cheats for Cosmic Quest")
            scr.words(32, 86, "Description", FILES)
            scr.words(196, 86, "Enabled", FILES)
            for k, (s, on) in enumerate((("Infinite lives", 1), ("Start with 99 coins", 0), ("Moon jump", 0),
                                         ("Invincible", 1), ("Max health", 0), ("All levels open", 0),
                                         ("Slow motion", 0), ("Walk through walls", 0))):
                y = 102 + 8 * k
                if k == 0:
                    scr.d.rectangle([28, y - 1, 228, y + 6], fill=BAR)
                scr.words(32, y, s, FILES)
                scr.words(212, y, "Yes" if on else "No", FILES)
            scr.words(90, 194, "A:On/Off Y:Edit Sel:Add B:Exit", FILES)
        elif kind == "consoles":
            scr.band_logo()
            rows = ["../", "Block Puzzle.a26", "Cave Runner.nes", "Dot Hunter.gbc", "Moon Base.sms",
                    "Neon Racer.nes", "Pocket Quest.gbc", "River Raid 2.a26", "Sea Diver.sms", "Star Fort.nes"]
            for r, name in enumerate(rows):
                d = name.endswith("/")
                scr.row(r, name, DIRS if d else FILES, "<dir>" if d else "40K", sel=(r == 2))
            scr.footer(FOOTER)
        elif kind == "chips":
            browser(scr, sel=-1)
            scr.window(30, 68, 60, 110, "Main Menu", border=DIM)
            scr.window(36, 76, 60, 96, "Configuration", border=DIM)
            scr.window(40, 90, 180, 56, "Chip Options")
            for k, (s, v) in enumerate((("CX4 speed", "Normal"), ("SuperFX speed", "Normal"),
                                        ("MSU-1 volume boost", "Off"), ("Atari 2600 video width", "160"),
                                        ("Competition Cart timer (min)", "6"))):
                y = 98 + 8 * k
                if k == 0:
                    scr.d.rectangle([44, y - 1, 216, y + 6], fill=BAR)
                scr.words(48, y, s, FILES)
                scr.words(212 - 4 * len(v), y, v, DIRS)
            scr.window(40, 152, 120, 14, border=DESC_BORDER)
            scr.words(46, 156, "Set speed of CX4 soft core", FILES)
        elif kind == "memtest":
            browser(scr, sel=-1)
            scr.window(40, 58, 172, 136, "Memory test")
            scr.words(48, 70, "No test has been run yet.", FILES)
            scr.words(48, 176, "A: Wiring test  X: Full test  B: Close", FILES)
            scr.words(48, 184, "Both reset the console. X takes 30s.", FILES)
        return scr.done()
    return f


def theme():
    scr = Screen(theme_look(os.path.join(SNES, "..", "misc", "classic.thm")))
    browser(scr)
    scr.cover((161, 0, 255, 128), cover_art())
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
    """The browser with a type icon at the start of every row (snes/diricon.a65)."""
    scr = Screen(default_look())
    scr.band_logo()
    # (name, row colour, icon colour, icon shape): the shapes are the glyphs' rough silhouettes
    rows = [("../", DIRS, DIRS, "up"), ("Homebrew/", DIRS, DIRS, "folder"),
            ("Hero Quest (MSU-1)/", DIRS, DIRS, "pad"), ("Astro Blaster (USA).sfc", FILES, LIGHT, "pad"),
            ("Battle Theme.spc", PCM, PCM, "note"), ("Cave Runner.nes", FILES, (230, 90, 80), "cart"),
            ("Dot Hunter.gbc", FILES, (130, 200, 255), "gb"), ("Midnight.thm", FILES, LIGHT, "pal"),
            ("Moon Base.sms", FILES, (100, 160, 255), "cart"), ("Pocket Quest.gb", FILES, (170, 170, 170), "gb"),
            ("River Raid 2.a26", FILES, (240, 190, 60), "stick"), ("Boss Theme.pcm", PCM, PCM, "wave")]
    for r, (name, col, icol, shape) in enumerate(rows):
        y = 57 + 8 * r
        if r == 2:
            scr.d.rectangle([2, y - 1, 252, y + 6], fill=BAR)
        d = scr.d
        if shape == "folder":
            d.rectangle([8, y + 1, 17, y + 6], fill=icol); d.rectangle([8, y, 12, y + 1], fill=icol)
        elif shape == "up":
            d.polygon([(8, y + 3), (12, y), (12, y + 6)], fill=icol); d.rectangle([12, y + 2, 17, y + 4], fill=icol)
        elif shape == "pad":
            d.rounded_rectangle([7, y + 1, 18, y + 6], 2, fill=icol)
            d.rectangle([9, y + 3, 11, y + 4], fill=(40, 40, 60)); d.rectangle([14, y + 2, 16, y + 5], fill=(200, 60, 80))
        elif shape == "note":
            d.ellipse([8, y + 3, 12, y + 7], fill=icol); d.rectangle([11, y, 12, y + 5], fill=icol)
            d.rectangle([11, y, 16, y + 1], fill=icol)
        elif shape == "cart":
            d.rectangle([8, y, 17, y + 6], fill=icol); d.rectangle([10, y + 2, 15, y + 4], fill=(40, 40, 60))
        elif shape == "gb":
            d.rectangle([9, y - 1, 16, y + 7], fill=icol); d.rectangle([10, y, 15, y + 3], fill=(60, 120, 60))
        elif shape == "pal":
            d.ellipse([8, y, 17, y + 7], fill=icol)
            for k, c in enumerate(((220, 60, 60), (60, 160, 230), (240, 200, 60))):
                d.rectangle([10 + 2 * k, y + 2, 11 + 2 * k, y + 3], fill=c)
        elif shape == "stick":
            d.rectangle([9, y + 4, 16, y + 7], fill=(60, 60, 70)); d.rectangle([12, y, 13, y + 4], fill=icol)
            d.ellipse([11, y - 1, 14, y + 2], fill=(220, 60, 60))
        elif shape == "wave":
            for k, hgt in enumerate((2, 5, 3, 6, 2)):
                d.rectangle([8 + 2 * k, y + 6 - hgt, 9 + 2 * k, y + 6], fill=icol)
        scr.words(22, y, name, col)
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

PICTURES = {
    **{"lang_" + c: flags(k) for k, c in enumerate(LANGS)},
    "covers_off": covers(0), "covers_large": covers(1), "covers_small": covers(2),
    "gameinfo_on": gameinfo(1), "gameinfo_ctx": gameinfo(2),
    "music_on": icon_music, "music_off": dim(icon_music),
    "random_on": icon_random, "random_off": dim(icon_random),
    "sfx_on": icon_sounds, "sfx_off": dim(icon_sounds),
    "igm_on": igm, "igm_off": dim(igm),
    "savestates_on": savestates, "savestates_off": dim(savestates),
    "pad2_on": icon_pad2(True), "pad2_off": icon_pad2(False),
    "msu_on": msu(True), "msu_off": msu(False),
    **{"reset_%d" % k: reset(k) for k in range(5)},
    "theme": theme, "cheats": more("cheats"), "patches": more("patches"),
    "consoles": more("consoles"), "chips": more("chips"), "memtest": more("memtest"),
    "saves": saves_tab, "pcm": pcm_player, "bsx": icon_bsx, "delete": delete_ctx,
    "bios": bios_popup, "desc": option_desc,
    "section_217": section_217, "gbc": icon_gbc, "shortcuts": shortcut_list,
    "carts_seta": icon_carts_seta, "carts_more": carts_more, "icons": browser_icons,
    "gameinfo_cheats": gameinfo_cheats,
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
