#!/usr/bin/env python3
"""The "- ludufre - Presents" screen the menu shows once after the console is switched on.

It follows Super Mario World's "- Nintendo - Presents" screen: the same layout, pixel
style, timing and sound, with "ludufre" in place of the publisher. Nothing is copied
from the game: the letters are drawn here in the same bitmap style, and the chime is a
sine the S-DSP plays with the measured notes and envelope.

Writes snes/bootintro_data.i65 (committed: the build never runs this, it needs Pillow),
the data snes/bootintro.a65 includes:
  - the picture: BG1 tiles (mode 1, 4bpp), a 16-colour palette and the used rectangle
    of the tilemap;
  - the chime: an SPC700 program, its sample directory, one BRR sample and the S-DSP
    register lists it plays from.

    python3 utils/gen_boot_intro.py                # write the include
    python3 utils/gen_boot_intro.py --preview DIR  # plus the screen (PNG, 1x and 4x)
                                                   # and an approximate render of the
                                                   # chime (WAV)

THE REFERENCE (Super Mario World (USA), measured in an emulator: register writes, the
S-DSP registers frame by frame, screenshots)
  picture  pure white on black, nothing else. Line 1, y 112..120: "- Nintendo -" in a
           bold bitmap face (2-pixel stems, x-height 6, ascenders 9), centred; line 2,
           y 122..127: "Presents" in a thin 6-line face, x 109..146.
  timing   (T0 = the frame the picture appears, at full brightness, no fade in)
           T0+3 first note, T0+6 key off, T0+7 second note, T0+26 key off;
           T0+108 the fade out starts, one brightness step every 2 frames (0 at T0+136).
  sound    one voice, a sine-like sample (period 24 samples, ~1.3 kHz at pitch $1000)
           played at pitch $17BB then $1FB9 (B6, then E7), volume $46/$46, ADSR $FE/$11
           (attack 14, decay 7, sustain level 0, sustain rate 17), master volume $7F,
           no echo, no noise, no pitch modulation.

CHIME PROGRAM
Uploaded through the IPL at $0300 (the sample directory, DIR = $03). The SNES drives it
through port 1 (the program acknowledges each command by echoing it back on port 1):
  1  first note     2  key off     3  second note
  4  exit: key off, mute, and back to the IPL ($FFC0), so the menu's own apu_ram_init
     finds the S-SMP exactly as a console reset leaves it.
Echo writes stay off the whole time (FLG bit 5), so no ARAM beyond the upload is touched.
"""
import math
import os
import struct
import sys
import wave

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'bootintro_data.i65')

W, H = 256, 224

# ------------------------------------------------------------------ the picture

# The bold face of the first line: 9 rows (0 = ascender top, 3 = x-height top, 8 =
# baseline row), 2-pixel stems. d and e are the reference's; u is its n turned round,
# r its n without the right stem below the shoulder, l the stem of its i at full
# height, f the stem of its t with a hook.
BOLD = {
    'l': ["##",
          "##",
          "##",
          "##",
          "##",
          "##",
          "##",
          "##",
          "##"],
    'u': ["......",
          "......",
          "......",
          "##..##",
          "##..##",
          "##..##",
          "##..##",
          "##..##",
          ".#####"],
    'd': ["....##",
          "....##",
          "....##",
          ".#####",
          "##..##",
          "##..##",
          "##..##",
          "##..##",
          ".#####"],
    'f': ["..###",
          ".##..",
          ".##..",
          "####.",
          ".##..",
          ".##..",
          ".##..",
          ".##..",
          ".##.."],
    'r': ["......",
          "......",
          "......",
          "#####.",
          "##..##",
          "##....",
          "##....",
          "##....",
          "##...."],
    'e': ["......",
          "......",
          "......",
          ".####.",
          "##..##",
          "######",
          "##....",
          "##..##",
          ".####."],
    '-': ["....",
          "....",
          "....",
          "....",
          "....",
          "####",
          "....",
          "....",
          "...."],
}
WORD = "ludufre"
LINE1_Y = 112
DASH_GAP = 3                    # blank columns between a dash and the word
LETTER_GAP = 1

# "Presents": the thin face, as the reference draws it (x 109..146, y 122..127)
PRESENTS = [
    "####..........................#.......",
    "#..#.#....##...###..##..###..####..###",
    "####.###.#..#.#....#..#.#..#..#...#...",
    "#....#...####..##..####.#..#..#....##.",
    "#....#...#.......#.#....#..#..#......#",
    "#....#....###.####..###.#..#..#...####",
]
PRESENTS_X, PRESENTS_Y = 109, 122
WHITE = (255, 255, 255)


def glyph_row(parts):
    """Glyphs side by side -> list of 9 strings."""
    rows = [""] * 9
    for g, gap in parts:
        for r in range(9):
            rows[r] += g[r] + "." * gap
    return rows


def picture():
    """The screen as RGB: white pixels on black."""
    parts = [(BOLD['-'], DASH_GAP)]
    for i, ch in enumerate(WORD):
        parts.append((BOLD[ch], DASH_GAP if i == len(WORD) - 1 else LETTER_GAP))
    parts.append((BOLD['-'], 0))
    rows = glyph_row(parts)
    width = len(rows[0])
    x0 = 128 - (width + 1) // 2
    img = Image.new('RGB', (W, H), (0, 0, 0))
    px = img.load()
    for r, row in enumerate(rows):
        for c, v in enumerate(row):
            if v == '#':
                px[x0 + c, LINE1_Y + r] = WHITE
    for r, row in enumerate(PRESENTS):
        for c, v in enumerate(row):
            if v == '#':
                px[PRESENTS_X + c, PRESENTS_Y + r] = WHITE
    return img


def indexed(img):
    """-> (index image: 0 black, 1 white; the 16 palette colours)."""
    src = img.convert('RGB').load()
    out = Image.new('P', img.size)
    dst = out.load()
    for y in range(img.height):
        for x in range(img.width):
            dst[x, y] = 1 if src[x, y] != (0, 0, 0) else 0
    return out, [(0, 0, 0), WHITE] + [(0, 0, 0)] * 14


def bgr555(c):
    return (c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10


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


def tiles(q):
    """-> (tile bytes, map rect (x, y, w, h), map words)."""
    data = q.load()
    grid = {}
    for ty in range(H // 8):
        for tx in range(W // 8):
            t = tuple(data[tx * 8 + c, ty * 8 + r] for r in range(8) for c in range(8))
            if any(t):
                grid[tx, ty] = t
    xs = [k[0] for k in grid]
    ys = [k[1] for k in grid]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    uniq = [tuple([0] * 64)]
    words = []
    for ty in range(y0, y1 + 1):
        for tx in range(x0, x1 + 1):
            t = grid.get((tx, ty), uniq[0])
            if t not in uniq:
                uniq.append(t)
            words.append(uniq.index(t))
    blob = []
    for t in uniq:
        blob += planar(t)
    return blob, (x0, y0, x1 - x0 + 1, y1 - y0 + 1), words


# ------------------------------------------------------------------ the chime (S-DSP)

DSP_RATE = 32000
PERIOD = 24                     # samples per cycle of the sample (the reference's)
HEAD_BLOCKS = 33                # the attack: a sine whose amplitude settles...
LOOP_BLOCKS = 3                 # ...into this looped tail (2 cycles = 48 samples)
# the attack, fitted to the reference's RMS per 48 samples: silent for 22 samples, up
# to PEAK by sample 60, held to 72, then straight down to STEADY at sample 400
SILENT, RISE, HOLD, SETTLE = 22, 60, 72, 400
PEAK, STEADY = 16660, 5870      # amplitude, output units
PITCH1, PITCH2 = 0x17BB, 0x1FB9 # B6, E7
VOL = 0x46
ADSR1, ADSR2 = 0xFE, 0x11
MVOL = 0x7F
BASE = 0x0300                   # upload address = the sample directory (DIR = $03)


def sample_shape():
    out = []
    for i in range((HEAD_BLOCKS + LOOP_BLOCKS) * 16):
        if i < SILENT:
            a = 0.0
        elif i < RISE:
            a = PEAK * (i - SILENT) / (RISE - SILENT)
        elif i < HOLD:
            a = PEAK
        elif i < SETTLE:
            a = PEAK + (STEADY - PEAK) * (i - HOLD) / (SETTLE - HOLD)
        else:
            a = STEADY
        out.append(a * math.sin(2 * math.pi * (i - SILENT) / PERIOD))
    return out


def clamp16(v):
    return max(-32768, min(32767, v))


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def brr_decode_block(hdr, nib, p1, p2):
    """blargg's S-DSP BRR decode (snes_spc). p1/p2 = the two previous OUTPUT samples."""
    shift = hdr >> 4
    filt = hdr & 0x0c
    out = []
    for n in nib:
        s = n - 16 if n & 8 else n
        s = (s << shift) >> 1 if shift <= 12 else (-2048 if s < 0 else 0)
        q1, q2 = p1, p2 >> 1
        if filt >= 8:
            s += q1
            s -= q2
            if filt == 8:
                s += q2 >> 4
                s += (q1 * -3) >> 6
            else:
                s += (q1 * -13) >> 7
                s += (q2 * 3) >> 4
        elif filt:
            s += q1 >> 1
            s += (-q1) >> 5
        s = s16(clamp16(s) * 2)
        out.append(s)
        p1, p2 = s, p1
    return out, p1, p2


def brr_encode_block(target, p1, p2, flags):
    best = None
    for filt in (0, 4, 8, 12):
        for shift in range(13):
            hdr = shift << 4 | filt | flags
            q1, q2 = p1, p2
            nib, err = [], 0
            for t in target:
                cands = []
                for n in range(-8, 8):
                    o, _, _ = brr_decode_block(hdr, [n & 15], q1, q2)
                    cands.append(((o[0] - t) ** 2, n, o[0]))
                e, n, o = min(cands)
                err += e
                nib.append(n & 15)
                q1, q2 = o, q1
            if best is None or err < best[0]:
                best = (err, hdr, nib)
    return best[1], best[2]


def brr_play(blocks, loop_block, n_out):
    """Decode as the S-DSP plays it from key on (zero history), looping."""
    q1 = q2 = 0
    out = []
    i = 0
    while len(out) < n_out:
        hdr, nib = blocks[i]
        o, q1, q2 = brr_decode_block(hdr, nib, q1, q2)
        out += o
        i = loop_block if hdr & 1 else i + 1
    return out


def brr_encode_sample(target):
    """Head encoded straight through; the looped tail re-encoded against the history
    its own end leaves (the greedy nibbles keep it jittering by a few LSBs, so the best
    of 16 passes is kept, judged by playing the whole sample 40 loops long)."""
    nb = len(target) // 16
    head = []
    q1 = q2 = 0
    for bi in range(HEAD_BLOCKS):
        hdr, nib = brr_encode_block(target[bi * 16:bi * 16 + 16], q1, q2, 0)
        _, q1, q2 = brr_decode_block(hdr, nib, q1, q2)
        head.append((hdr, nib))
    h1, h2 = q1, q2
    best = None
    n_loop = LOOP_BLOCKS * 16
    ref = target[HEAD_BLOCKS * 16:]
    for _ in range(16):
        loop = []
        q1, q2 = h1, h2
        for bi in range(HEAD_BLOCKS, nb):
            flags = 2 | (1 if bi == nb - 1 else 0)
            hdr, nib = brr_encode_block(target[bi * 16:bi * 16 + 16], q1, q2, flags)
            _, q1, q2 = brr_decode_block(hdr, nib, q1, q2)
            loop.append((hdr, nib))
        h1, h2 = q1, q2
        blocks = head + loop
        tail = brr_play(blocks, HEAD_BLOCKS, HEAD_BLOCKS * 16 + n_loop * 40)[HEAD_BLOCKS * 16:]
        err = max(abs(v - ref[i % n_loop]) for i, v in enumerate(tail))
        if best is None or err < best[0]:
            best = (err, blocks)
    err, blocks = best
    assert err < 0.08 * STEADY, f"BRR loop error {err}"
    blob = []
    for hdr, nib in blocks:
        blob.append(hdr)
        for i in range(0, 16, 2):
            blob.append(nib[i] << 4 | nib[i + 1])
    return blob, blocks


class Spc:
    """A few SPC700 instructions, enough for the chime program."""

    def __init__(self, org):
        self.org = org
        self.b = []
        self.labels = {}
        self.fix = []

    def pc(self):
        return self.org + len(self.b)

    def label(self, n):
        self.labels[n] = self.pc()

    def emit(self, *bs):
        self.b += list(bs)

    def rel(self, op, lab):
        self.emit(op, 0)
        self.fix.append(('rel', len(self.b) - 1, lab))

    def abs16(self, op, lab, extra=0):
        self.emit(op, 0, 0)
        self.fix.append(('abs', len(self.b) - 2, lab, extra))

    def resolve(self):
        for f in self.fix:
            if f[0] == 'rel':
                _, at, lab = f
                d = self.labels[lab] - (self.org + at + 1)
                assert -128 <= d < 128, lab
                self.b[at] = d & 0xff
            else:
                _, at, lab, extra = f
                a = (self.labels[lab] if isinstance(lab, str) else lab) + extra
                self.b[at] = a & 0xff
                self.b[at + 1] = a >> 8
        return self.b


CMD_NOTE1, CMD_OFF, CMD_NOTE2, CMD_EXIT = 1, 2, 3, 4
LAST = 0x20                     # direct page: the last command seen


def chime_program():
    """-> (blob uploaded at BASE, entry address, BRR blocks for the preview)."""
    brr, blocks = brr_encode_sample([round(v) for v in sample_shape()])
    at = BASE + 4
    loop_at = at + HEAD_BLOCKS * 9
    blob = [at & 0xff, at >> 8, loop_at & 0xff, loop_at >> 8] + brr

    # register lists: (reg, value)..., $ff
    init = [(0x6c, 0x60),                       # FLG: mute, echo writes off
            (0x5c, 0xff), (0x4c, 0x00),         # key every voice off
            (0x4d, 0x00), (0x2c, 0x00), (0x3c, 0x00), (0x0d, 0x00),
            (0x2d, 0x00), (0x3d, 0x00)]
    init += [(v << 4 | k, 0x00) for v in range(8) for k in (0, 1)]
    init += [(0x5d, BASE >> 8), (0x0c, MVOL), (0x1c, MVOL),
             (0x04, 0), (0x05, ADSR1), (0x06, ADSR2), (0x07, 0x00),
             (0x6c, 0x20)]                      # unmute (every voice at volume 0), echo writes off
    note1 = [(0x00, VOL), (0x01, VOL), (0x02, PITCH1 & 0xff), (0x03, PITCH1 >> 8),
             (0x5c, 0x00), (0x4c, 0x01)]
    off = [(0x5c, 0x01)]
    note2 = [(0x02, PITCH2 & 0xff), (0x03, PITCH2 >> 8), (0x5c, 0x00), (0x4c, 0x01)]
    leave = [(0x5c, 0xff), (0x6c, 0x60)]        # key off + mute, echo writes off: the IPL's state

    tables = []
    offs = {}
    for name, t in (('init', init), ('note1', note1), ('off', off), ('note2', note2),
                    ('exit', leave)):
        offs[name] = len(tables)
        for r, v in t:
            tables += [r, v]
        tables.append(0xff)
    assert len(tables) <= 256
    cmdtab = [0, offs['note1'], offs['off'], offs['note2'], offs['exit']]

    tab_at = BASE + len(blob)
    blob += tables
    cmd_at = BASE + len(blob)
    blob += cmdtab
    a = Spc(BASE + len(blob))
    a.label('entry')
    a.emit(0xcd, offs['init'])                  # mov x,#init
    a.abs16(0x3f, 'runtab')                     # call runtab
    a.emit(0x8f, 0x00, LAST)                    # mov LAST,#0
    a.label('loop')
    a.emit(0xe4, 0xf5)                          # mov a,$f5      (port 1 from the SNES)
    a.emit(0x64, LAST)                          # cmp a,LAST
    a.rel(0xf0, 'loop')                         # beq loop
    a.emit(0xc4, LAST)                          # mov LAST,a
    a.emit(0xc4, 0xf5)                          # mov $f5,a      (acknowledge)
    a.emit(0x68, len(cmdtab))                   # cmp a,#5
    a.rel(0xb0, 'loop')                         # bcs loop       (unknown command)
    a.emit(0xfd)                                # mov y,a
    a.abs16(0xf6, cmd_at)                       # mov a,!cmdtab+y
    a.emit(0x5d)                                # mov x,a
    a.abs16(0x3f, 'runtab')                     # call runtab
    a.emit(0xe4, LAST)                          # mov a,LAST
    a.emit(0x68, CMD_EXIT)                      # cmp a,#EXIT
    a.rel(0xd0, 'loop')                         # bne loop
    a.abs16(0x5f, 0xffc0)                       # jmp $ffc0      (back to the IPL)
    a.label('runtab')
    a.abs16(0xf5, tab_at)                       # mov a,!tables+x
    a.emit(0x68, 0xff)                          # cmp a,#$ff
    a.rel(0xf0, 'rt_done')                      # beq rt_done
    a.emit(0xc4, 0xf2)                          # mov $f2,a      (DSP address)
    a.emit(0x3d)                                # inc x
    a.abs16(0xf5, tab_at)                       # mov a,!tables+x
    a.emit(0xc4, 0xf3)                          # mov $f3,a      (DSP data)
    a.emit(0x3d)                                # inc x
    a.rel(0x2f, 'runtab')                       # bra runtab
    a.label('rt_done')
    a.emit(0x6f)                                # ret
    entry = a.labels['entry']
    blob += a.resolve()
    # the IPL protocol: the "execute" write is port0 = last index + 2; it must not be
    # 0, and it must not be $CC either -- the value stays on port 0 when the program
    # returns to the IPL, which would read it as the start of a new transfer.
    n = len(blob)
    assert (n + 1) & 0xff not in (0x00, 0xcc), n
    return blob, entry, blocks


# frames from the picture appearing (T0), as the reference; bootintro.a65 follows them
T_NOTE1, T_OFF1, T_NOTE2, T_OFF2, T_FADE = 3, 6, 7, 26, 108


def render_chime(blocks, path):
    """Approximate the S-DSP (linear interpolation instead of the gaussian, the real
    ADSR rates) to a 32 kHz WAV, for a listen before hardware."""
    rates = [0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96,
             80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1]
    spf = DSP_RATE / 60.0
    src = brr_play(blocks, HEAD_BLOCKS, 400000)
    events = [(round(T_NOTE1 * spf), 'on', PITCH1), (round(T_OFF1 * spf), 'off', 0),
              (round(T_NOTE2 * spf), 'on', PITCH2), (round(T_OFF2 * spf), 'off', 0)]
    total = round((T_FADE + 30) * spf)
    ar, dr, sl, sr = ADSR1 & 15, (ADSR1 >> 4) & 7, ADSR2 >> 5, ADSR2 & 31
    env, mode, ctr, pos, pit = 0, 'off', 0, 0.0, 0
    out = []
    ev = 0
    for n in range(total):
        while ev < len(events) and events[ev][0] == n:
            _, kind, p = events[ev]
            if kind == 'on':
                env, mode, ctr, pos, pit = 0, 'attack', 0, 0.0, p
            else:
                mode = 'release'
            ev += 1
        smp = 0.0
        if mode != 'off':
            i = int(pos)
            fr = pos - i
            smp = src[i] * (1 - fr) + src[i + 1] * fr
            pos += pit / 4096
            ctr += 1
            if mode == 'attack':
                if ctr >= rates[ar * 2 + 1]:
                    ctr = 0
                    env += 1024 if ar == 15 else 32
                    if env >= 0x7ff:
                        env, mode = 0x7ff, 'decay'
            elif mode == 'decay':
                if ctr >= rates[dr * 2 + 16]:
                    ctr = 0
                    env -= ((env - 1) >> 8) + 1
                if env >> 8 <= sl:
                    mode = 'sustain'
            elif mode == 'sustain':
                if sr and ctr >= rates[sr]:
                    ctr = 0
                    env -= ((env - 1) >> 8) + 1
            else:
                env -= 8
                if env <= 0:
                    env, mode = 0, 'off'
            env = max(0, env)
        o = smp * env / 0x800 * VOL / 128 * MVOL / 128
        v = max(-32768, min(32767, round(o)))
        out.append((v, v))
    with wave.open(path, 'wb') as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(DSP_RATE)
        wf.writeframes(b''.join(struct.pack('<hh', *s) for s in out))
    peak = max(abs(a) for a, _ in out)
    print(f"  chime preview: {path} (peak {peak / 32768:.2f} of full scale)")


# ------------------------------------------------------------------ output

def fmt_bytes(label, data, per=16):
    lines = [f"{label}:"]
    for i in range(0, len(data), per):
        lines.append("  .byt " + ", ".join(f"${b:02x}" for b in data[i:i + per]))
    return "\n".join(lines)


def main():
    preview = None
    if '--preview' in sys.argv:
        preview = sys.argv[sys.argv.index('--preview') + 1]
        os.makedirs(preview, exist_ok=True)
    img = picture()
    q, cols = indexed(img)
    tile_bytes, (mx, my, mw, mh), words = tiles(q)
    pal = []
    for c in cols:
        v = bgr555(c)
        pal += [v & 0xff, v >> 8]
    mapb = []
    for wd in words:
        mapb += [wd & 0xff, wd >> 8]          # palette 0, priority 0, no flip
    spc, entry, blocks = chime_program()

    src = [
        "// GENERATED by utils/gen_boot_intro.py -- do not edit. The picture and the chime",
        "// of the power-on screen (snes/bootintro.a65).",
        f"#define BI_TILE_BYTES   {len(tile_bytes)}",
        f"#define BI_MAP_X        {mx}",
        f"#define BI_MAP_Y        {my}",
        f"#define BI_MAP_W        {mw}",
        f"#define BI_MAP_H        {mh}",
        f"#define BI_MAP_ROWBYTES {mw * 2}",
        f"#define BI_MAP_VADDR    ${0x7c00 + my * 32 + mx:04x}   // BI_VRAM_MAP + y*32 + x",
        f"#define BI_SPC_ADDR     ${BASE:04x}",
        f"#define BI_SPC_LEN      {len(spc)}",
        f"#define BI_SPC_ENTRY    ${entry:04x}",
        f"#define BI_CMD_NOTE1    {CMD_NOTE1}",
        f"#define BI_CMD_OFF      {CMD_OFF}",
        f"#define BI_CMD_NOTE2    {CMD_NOTE2}",
        f"#define BI_CMD_EXIT     {CMD_EXIT}",
        f"#define BI_T_NOTE1      {T_NOTE1}",
        f"#define BI_T_OFF1       {T_OFF1}",
        f"#define BI_T_NOTE2      {T_NOTE2}",
        f"#define BI_T_OFF2       {T_OFF2}",
        f"#define BI_T_FADE       {T_FADE}",
        "",
        fmt_bytes("bi_pal", pal),
        "",
        fmt_bytes("bi_map", mapb),
        "",
        fmt_bytes("bi_tiles", tile_bytes),
        "",
        fmt_bytes("bi_spc", spc),
        "",
    ]
    with open(OUT, 'w') as fh:
        fh.write("\n".join(src))
    print(f"{os.path.relpath(OUT)}: {len(tile_bytes) // 32} tiles, map {mw}x{mh} at "
          f"({mx},{my}), SPC {len(spc)} B (entry ${entry:04x}), "
          f"{len(tile_bytes) + len(mapb) + len(pal) + len(spc)} B of data")

    if preview:
        img.save(os.path.join(preview, 'bootintro.png'))
        img.resize((W * 4, H * 4), Image.NEAREST).save(os.path.join(preview, 'bootintro_4x.png'))
        render_chime(blocks, os.path.join(preview, 'bootintro_chime.wav'))


if __name__ == '__main__':
    main()
