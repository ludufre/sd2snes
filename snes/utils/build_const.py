#!/usr/bin/env python3
"""Generate a multilingual const_lang.a65 from the English base snes/const.a65.

The menu renders strings by label. To support a runtime language switch in a
single binary, every *localized* label (one that appears in any lang_*.py)
is expanded into:

    <label>_en   .byt <english>, 0
    <label>_<code> .byt <translation>, 0    ; one per lang_<code>.py given on argv
    <label>:                              ; dispatch table (the public label)
      .word !<label>_en
      .word !<label>_<code>                ; ... one word per language, in argv order

All dispatch tables are emitted contiguously between the exported labels
`strtab_lo` and `strtab_hi`. The menu's `resolve_str` recognises a dispatch
table purely by address range: a pointer inside [strtab_lo, strtab_hi) is
indexed by the active language (cur_lang * 2); any other pointer (a plain,
language-neutral string such as "50Hz") passes through unchanged. This keeps
menudata.a65 and every existing `!label`/`^label` reference untouched.

Non-localized lines (data tables, window geometry, neutral strings) are copied
verbatim and keep their original positions.

Usage:
    build_const.py <const.a65> <lang_*.py> [<lang_*.py> ...] -o <const_lang.a65>
    build_const.py <const.a65> --dump-en <out.py>   # English scaffold for a new lang
"""
import re
import sys
import importlib.util
from pathlib import Path

# Font byte codes for accented glyphs. MUST match snes/font.a65 / fontedit.py.
ACCENTS = {
    "á": 130, "à": 131, "â": 132, "ã": 133, "é": 134, "ê": 135,
    "í": 136, "ó": 137, "ô": 138, "õ": 139, "ú": 140, "ç": 141,
    "Á": 142, "À": 143, "Â": 144, "Ã": 145, "É": 146, "Ê": 147,
    "Í": 148, "Ó": 149, "Ô": 150, "Õ": 151, "Ú": 152, "Ç": 153,
    # Spanish additions:
    "ñ": 154, "Ñ": 155, "ü": 156, "Ü": 157, "¿": 158, "¡": 159,
    # French additions. When they went in, 160-223 was not free (katakana art in
    # 161-223, and a game info chip icon since removed was drawn over the VRAM
    # of 160/161/176/177), so the block went to the blank tail at 224-255:
    "è": 224, "ù": 225, "î": 226, "ï": 227, "ë": 228, "û": 229,
    # Italian additions. The lowercase graves the earlier blocks never needed
    # (à/è/ù already exist), plus the uppercase graves: Italian headers are drawn
    # in caps by the in-game menu and "E'" is not an acceptable stand-in for "È",
    # which opens a large share of sentences:
    "ì": 230, "ò": 231, "È": 232, "Ì": 233, "Ò": 234, "Ù": 235,
    # German additions. Without them ä/ö/ß fall through as literal UTF-8 and
    # each one renders as two tiles of katakana art. ä/ö/Ä/Ö are the diaeresis
    # over the same bases as ü/Ü; ß is hand-drawn (it has no base letter):
    "ä": 236, "ö": 237, "ß": 238, "Ä": 239, "Ö": 240,
    # Cyrillic, drawn over the dead katakana block (fontedit.py CYRILLIC). Only
    # the 47 letters that need a tile of their own are here; the 19 that reuse
    # an existing tile are in HOMOGLYPHS below. Uppercase then lowercase, each
    # in alphabet order, with У last:
    "Б": 178, "Г": 179, "Д": 180, "Ё": 181, "Ж": 182, "З": 183,
    "И": 184, "Й": 185, "Л": 186, "П": 187, "Ф": 188, "Ц": 189,
    "Ч": 190, "Ш": 191, "Щ": 192, "Ъ": 193, "Ы": 194, "Ь": 195,
    "Э": 196, "Ю": 197, "Я": 198, "б": 199, "в": 200, "г": 201,
    "д": 202, "ж": 203, "з": 204, "и": 205, "й": 206, "к": 207,
    "л": 208, "м": 209, "н": 210, "п": 211, "т": 212, "ф": 213,
    "ц": 214, "ч": 215, "ш": 216, "щ": 217, "ъ": 218, "ы": 219,
    "ь": 220, "э": 221, "ю": 222, "я": 223,
    # У sits just below the block. It shared the Latin Y tile until that one was
    # redrawn with a straight stem; У keeps the old tailed shape, byte for byte:
    "У": 177,
}

# Cyrillic letters an existing tile already draws: 11 uppercase and 7 lowercase
# Latin homoglyphs, plus ё, which IS the French ë (228). ENCODE-ONLY -- putting
# them in ACCENTS would give a code two owners and DECODE would pick the wrong
# one, handing back 'А' for a Latin 'A' and 'ё' for a French ë.
HOMOGLYPHS = {
    "А": ord("A"), "В": ord("B"), "Е": ord("E"), "К": ord("K"), "М": ord("M"),
    "Н": ord("H"), "О": ord("O"), "Р": ord("P"), "С": ord("C"), "Т": ord("T"),
    "Х": ord("X"),
    "а": ord("a"), "е": ord("e"), "о": ord("o"), "р": ord("p"), "с": ord("c"),
    "у": ord("y"), "х": ord("x"),
    "ё": 228,
}
# What encode_string may translate; DECODE stays keyed on ACCENTS alone.
ENCODE = {**ACCENTS, **HOMOGLYPHS}
DECODE = {v: k for k, v in ACCENTS.items()}

# `LABEL  .byt  <args>` (args may contain quoted strings and raw byte values).
LINE_RE = re.compile(r'^(\s*)(\S+)(\s+\.byt\s+)(.*)$')

# CJK (Japanese/Chinese). The 256-tile font cannot hold ideographs, so they are drawn from
# a glyph cache in VRAM (snes/cjk.a65). A CJK character is encoded as TWO bytes,
#   lead  = CJK_LEAD0 + index // 128      ($FA..$FF: font codes no Latin string uses)
#   trail = $80 + index % 128             (never a space, a terminator or a marker)
# where index is the glyph's position in the sheet build_const emits (cjk_sheet). Two bytes
# for a glyph two columns wide keeps "one byte = one column", so strlen, window widths and
# print_count stay right with no change. The glyph is a 16-px cell of BG1, which only owns the
# ODD columns: every lead is placed at an EVEN byte offset of its string (a space is inserted
# after an odd-length run of single-byte text), so all of a string's glyphs share a parity and
# the printer fixes that parity once, at the CJK_MARK the string starts with.
CJK_LEAD0 = 0xFA
CJK_MAX = (0x100 - CJK_LEAD0) * 128
# Every string that has a glyph STARTS with this byte (a free font code). It is one column of
# the string's width that the printer fills with a blank when the string starts on a BG2
# column and skips when it starts on a BG1 one: the parity fix happens once, up front, so a
# list of labels stays aligned and the text inside a string keeps its spacing.
CJK_MARK = 176
CJK_FIRST_CP = 0x2E80          # CJK radicals onwards: kana, ideographs, full-width forms
CJK = None                     # char -> glyph index of the CURRENT encoding context (main())
# Pad a lead that would land on an odd offset after the marker with a blank. The tour needs it
# (its glyphs go on BG2 cells only, gen_onb_lang.py); the menu does not (snes/cjk.a65 puts a
# glyph on a BG1 or a BG2 cell, whichever column it starts on), so main() turns it off: the
# pad was a blank column the ASCII rows around the string did not have.
CJK_PAD = True

# Two glyph ranges. Indices 0..127 (lead $FA) are the RESIDENT sheet, linked into the menu:
# the glyphs of text every language shows, i.e. the languages' own names in the language list.
# Indices 128..767 (leads $FB-$FF) belong to the ACTIVE CJK language: its glyph sheet and its
# whole string pool are a file on the card (lang_<code>.bin, /sd2snes/lang/) that the menu
# copies into WRAM bank $7F when that language is selected (cjk.a65 cjk_lang_sync). A CJK
# language column therefore costs the menu banks only its dispatch-table words.
CJK_COMMON_MAX = 128
CJK_LANG_FIRST = 128
CJK_LANG_LAST = 640            # 640..767 (lead $FF) stay free for glyphs the firmware supplies
CJK_LANGS = {"ja": "misaki_gothic_2nd.hex", "zh": "fusion8_zh_hans.hex"}   # code -> font
CJK_FONT_DEFAULT = "misaki_gothic_2nd.hex"
CJK_BASE = 0x2000              # the file's place in bank $7F (memmap.i65 CJK_LANG_BASE)
CJK_HDR = 16                   # "SDL1", column, version, pool_len, sheet_addr, nglyphs, total
CJK_FILE_MAX = 0xDF00          # $7F2000..$7FFEFF; the menu's launch trampoline sits at $7FFFE0


def is_cjk(ch):
    return ord(ch) >= CJK_FIRST_CP


def encode_string(text, zero_width=()):
    """UTF-8 text (with {NNN} raw-byte placeholders) -> `.byt` argument string.
    zero_width: raw byte values that take no column (the tour's BTN_TOGGLE), left out of the
    offsets the CJK alignment counts."""
    pieces, cur, i, off = [], "", 0, 0
    if CJK_PAD and any(is_cjk(ch) for ch in text):
        pieces.append(str(CJK_MARK))   # offsets below count from after the marker (the tour)
    while i < len(text):
        ch = text[i]
        if ch == "{":
            end = text.index("}", i)
            if cur:
                pieces.append(f'"{cur}"'); cur = ""
            pieces.append(text[i + 1:end])
            if not (text[i + 1:end].isdigit() and int(text[i + 1:end]) in zero_width):
                off += 1
            i = end + 1
            continue
        if ch in ENCODE:
            if cur:
                pieces.append(f'"{cur}"'); cur = ""
            pieces.append(str(ENCODE[ch]))
        elif is_cjk(ch):
            if CJK is None or ch not in CJK:
                sys.exit(f"build_const.py: CJK character {ch!r} (U+{ord(ch):04X}) in {text!r} "
                         f"has no glyph cache entry (only build_const's menu strings support CJK)")
            if off & 1 and CJK_PAD:                # keep every lead at an even offset:
                sp = cur.rfind(" ")                # widen a space of the run before it
                if sp >= 0:                        # ("A  B:" reads better than "B: X")
                    cur = cur[:sp] + " " + cur[sp:]
                else:
                    if cur:
                        pieces.append(f'"{cur}"'); cur = ""
                    pieces.append("32")
                off += 1
            if cur:
                pieces.append(f'"{cur}"'); cur = ""
            idx = CJK[ch]
            pieces.append(str(CJK_LEAD0 + idx // 128))
            pieces.append(str(0x80 + idx % 128))
            off += 1                               # (+1 below)
        else:
            cur += ch
        off += 1
        i += 1
    if cur:
        pieces.append(f'"{cur}"')
    pieces.append("0")
    return ", ".join(pieces)


def load_cjk_font(path):
    """snes/fonts/*.hex: one `CODEPOINT:16 hex digits` line per glyph, 8 rows top-down, bit 7
    = leftmost pixel, row 7 and column 7 blank (the cell's spacing)."""
    font = {}
    for line in Path(path).read_text().splitlines():
        if ":" in line:
            cp, rows = line.split(":")
            font[chr(int(cp, 16))] = bytes.fromhex(rows)
    return font


def cjk_glyph_tiles(rows):
    """8x8 1bpp glyph -> the 64 bytes of its 16-px mode-5 cell: left 8x8 tile then right 8x8
    tile, 4bpp (planes 0/1 row-interleaved, then planes 2/3 = 0). Each source pixel is two hires pixels wide.
    Colour 1 = body, colour 2 = the dark contour the menu font draws around its letters
    (orthogonal neighbours only: diagonal-only gaps stay transparent, as in the font)."""
    body = [[(rows[y] >> (7 - x // 2)) & 1 for x in range(16)] for y in range(8)]
    px = [[0] * 16 for _ in range(8)]
    for y in range(8):
        for x in range(16):
            if body[y][x]:
                px[y][x] = 1
            elif any(0 <= y + dy < 8 and 0 <= x + dx < 16 and body[y + dy][x + dx]
                     for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0))):
                px[y][x] = 2
    out = bytearray()
    for half in (0, 8):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                c = px[y][half + x]
                p0 |= (c & 1) << (7 - x)
                p1 |= ((c >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
        out += bytes(16)                   # planes 2/3: BG1 is 4bpp; one DMA writes all of it
    return bytes(out)


def split_args(args):
    """Split a `.byt` argument list into top-level pieces (respect quotes)."""
    pieces, cur, in_str = [], "", False
    for ch in args:
        if ch == '"':
            in_str = not in_str
            cur += ch
        elif ch == "," and not in_str:
            pieces.append(cur.strip()); cur = ""
        else:
            cur += ch
    if cur.strip():
        pieces.append(cur.strip())
    return pieces


def decode_args(args):
    """Inverse of encode_string: `.byt` args -> UTF-8 text with {NNN} placeholders.
    The trailing 0 terminator is dropped."""
    text = ""
    for p in split_args(args):
        if p.startswith('"') and p.endswith('"'):
            text += p[1:-1]
        else:
            try:
                n = int(p, 0)
            except ValueError:
                text += "{" + p + "}"
                continue
            if n == 0:
                continue
            if n in DECODE:
                text += DECODE[n]
            else:
                text += "{" + str(n) + "}"
    return text


def args_to_bytes(args):
    """`.byt` argument string (encode_string's output) -> the bytes it assembles to."""
    out = bytearray()
    for p in split_args(args):
        if p.startswith('"') and p.endswith('"'):
            out += p[1:-1].encode("ascii")
        else:
            out.append(int(p, 0))
    return bytes(out)


def load_dict(path):
    spec = importlib.util.spec_from_file_location(Path(path).stem, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return dict(getattr(mod, "TRANSLATIONS", {}))


def parse_base(base_path):
    """Return (lines, en_args). lines preserves order; en_args maps label->args."""
    lines = base_path.read_text().splitlines()
    en_args = {}
    for line in lines:
        m = LINE_RE.match(line)
        if m:
            en_args[m.group(2)] = m.group(4)
    return lines, en_args


def dump_en_scaffold(base_path, out_path, keys):
    """Write a translation scaffold (label -> decoded English text) for `keys`."""
    _, en_args = parse_base(base_path)
    out = ['"""Translated strings for the sd2snes menu. Translate each value.',
           '',
           'Pre-seeded with the English source. Any label left in English (or removed)',
           'falls back to English at build time. {129}=submenu icon, {127}{128}=ellipsis.',
           'Accented chars (á é í ó ú ñ ü ¿ ¡ ...) may be written as real UTF-8.',
           '"""',
           '',
           'TRANSLATIONS = {']
    for k in keys:
        if k in en_args:
            out.append(f"    {k!r}: {decode_args(en_args[k])!r},")
    out.append('}')
    Path(out_path).write_text("\n".join(out) + "\n")
    print(f"wrote scaffold {out_path}: {sum(1 for k in keys if k in en_args)} entries")


def lang_code(path):
    """Return the dispatch-table code for utils/lang_<code>.py."""
    stem = Path(path).stem
    if stem.startswith("lang_"):
        return stem[5:]
    return stem


def main():
    base = Path(sys.argv[1])

    if "--dump-en" in sys.argv:
        # build_const.py <const.a65> <lang_ref.py> --dump-en <out.py>
        ref = load_dict(sys.argv[2])
        out_py = sys.argv[sys.argv.index("--dump-en") + 1]
        dump_en_scaffold(base, out_py, list(ref.keys()))
        return

    out_i = sys.argv.index("-o")
    lang_paths = [p for p in sys.argv[2:out_i] if p.endswith(".py")]
    langs = [(lang_code(p), load_dict(p)) for p in lang_paths]
    out_path = Path(sys.argv[sys.argv.index("-o") + 1])

    # CJK glyph registries (see CJK_LANGS). The resident range holds what the menu shows in
    # every language: CJK in const.a65's neutral lines (a language's own name, drawn in that
    # language's font) and in non-CJK columns. Each CJK language gets its own range, indexed
    # in code point order so a sheet is reproducible. Built before anything is encoded.
    global CJK, CJK_PAD
    CJK_PAD = False
    fonts_dir = Path(__file__).resolve().parent.parent / "fonts"
    font_cache = {}

    def font(name):
        if name not in font_cache:
            font_cache[name] = load_cjk_font(fonts_dir / name)
        return font_cache[name]

    cjk_codes = [code for code, _ in langs if code in CJK_LANGS]
    if [code for code, _ in langs][len(langs) - len(cjk_codes):] != cjk_codes:
        sys.exit("build_const.py: the CJK languages (" + ", ".join(CJK_LANGS) + ") must be the "
                 "LAST dicts on the command line (the in-game readers stop before them)")
    common = {}                                  # char -> font file
    for line in base.read_text().splitlines():
        m = LINE_RE.match(line)
        if m and any(is_cjk(ch) for ch in m.group(4)):
            code = m.group(2)[len("text_lang_"):] if m.group(2).startswith("text_lang_") else ""
            for ch in m.group(4):
                if is_cjk(ch):
                    common.setdefault(ch, CJK_LANGS.get(code, CJK_FONT_DEFAULT))
    for code, d in langs:
        if code not in CJK_LANGS:
            for text in d.values():
                for ch in str(text):
                    if is_cjk(ch):
                        common.setdefault(ch, CJK_FONT_DEFAULT)
    if len(common) > CJK_COMMON_MAX:
        sys.exit(f"build_const.py: {len(common)} resident CJK glyphs, room for {CJK_COMMON_MAX}")
    common_map = {ch: i for i, ch in enumerate(sorted(common))}
    lang_maps = {}
    for code, d in langs:
        if code in CJK_LANGS:
            used = {ch for text in d.values() for ch in str(text) if is_cjk(ch)}
            if len(used) > CJK_LANG_LAST - CJK_LANG_FIRST:
                sys.exit(f"build_const.py: [{code}] {len(used)} CJK glyphs, room for "
                         f"{CJK_LANG_LAST - CJK_LANG_FIRST}")
            lang_maps[code] = {ch: CJK_LANG_FIRST + i for i, ch in enumerate(sorted(used))}
    for name, chars in [(f, [c for c, ff in common.items() if ff == f]) for f in set(common.values())] + \
                       [(CJK_LANGS[c], list(m)) for c, m in lang_maps.items()]:
        missing = sorted(ch for ch in chars if ch not in font(name))
        if missing:
            sys.exit(f"build_const.py: no glyph in {name} for: "
                     + " ".join(f"{c} (U+{ord(c):04X})" for c in missing))

    def use(code):
        """Select the glyph range the next encode_string calls draw from."""
        global CJK
        CJK = lang_maps[code] if code in lang_maps else common_map

    use(None)

    # Item descriptions (mdesc_*) ARE rendered now (the menu draws the selected
    # entry's description), so they get localized too. The localized string pool
    # + dispatch tables are emitted into a SEPARATE bank ($C2, see below) so the
    # menu spans banks $C0-$C2 (m3nu.bin = 192K) instead of overflowing.  $C2 is
    # RESIDENT in-game too (igmenu moved to $C8), so the overlay's pool reads keep working.
    #
    # text_igm_* labels are consumed ONLY by gen_igmenu_lang.py, which parses
    # const.a65 directly and emits them into the igmenu's own bank ($C8, a
    # separate link) -- nothing in the m3nu link references them, so emitting
    # them here would waste bytes in the full $C0 bank (neutral labels) and the
    # $C2 pool (dispatch tables). Drop them from BOTH the verbatim copy and the
    # localized pool. They must stay in const.a65 itself: it is the single
    # source the igmenu generator reads.
    DROP_PREFIXES = ("text_igm_",)
    localized = {k for _, d in langs for k in d
                 if not k.startswith(DROP_PREFIXES)}
    lines, en_args = parse_base(base)

    out, order = [], []
    for line in lines:
        m = LINE_RE.match(line)
        if m and m.group(2).startswith(DROP_PREFIXES):
            continue                            # igmenu-only label, dead in this link
        if m and m.group(2) in localized:
            order.append(m.group(2))            # moved to the localized block below
        elif m and any(is_cjk(ch) for ch in m.group(4)):
            # a neutral label written in CJK (a language's own name): encode its glyphs
            out.append(f"{m.group(1)}{m.group(2)}{m.group(3)}"
                       f"{encode_string(decode_args(m.group(4)))}")
        else:
            out.append(line)

    # A dict key with no matching label means a label was renamed/removed in
    # const.a65 without updating the dicts -- from that point on the menu would
    # silently ship the new label in English. Fail the build instead.
    missing = sorted(l for l in localized if l not in en_args)
    if missing:
        sys.exit(f"build_const.py: {len(missing)} translated label(s) not found "
                 f"in {base}: {', '.join(missing)}\n"
                 f"(label renamed/removed in const.a65? update every lang_*.py to "
                 f"match, or the translation silently ships as English)")

    # Render budgets (encoded bytes, excluding the NUL terminator). A
    # translation longer than its UI slot overruns a popup border or wraps the
    # 64-tile row, and nothing at runtime guards that -- enforce it here.
    # Budgets derived from the render sites:
    #   text_no_*       show_empty_msg box: window_w=24 -> interior 22
    #                   (filesel.a65)
    #   cheat_tab_head  fixed cheat-table column layout, 48 cols incl. the
    #                   Enabled column (cheatmenu.a65)
    #   mtext_*         options window is COMPUTED from content: max_label +
    #                   max_value + 7 must fit the 64-tile screen (menu.a65
    #                   menu_open) -> keep labels <= 40
    #   default         hiprint row budget (print_count = 56)
    #   mdesc_          word-wrapped across several lines by the description box;
    #                   the runtime truncates with an ellipsis, so this is just a
    #                   sanity cap to keep a translation from bloating the ROM.
    #   text_err_*      show_error_msg box: window_w=28 -> interior ~26
    #                   (game-load error popup, filesel.a65)
    #   text_si_*       System Information line: 40 columns, hard-clipped by
    #                   sysinfo_render.a65. Note this counts the TEMPLATE, so a long
    #                   substituted value can still be clipped at runtime.
    #   text_cheat_noname  drawn in the cheat list's name column, CHEAT_NAME_WIDTH = 42
    #                   (cheatmenu.a65); hiprint truncates past that
    # First matching prefix wins, so text_mtl_ (whole lines) must precede text_mt_
    # (fragments printed AFTER a "U501: " chip prefix, hence 8 columns less).
    #   text_statusbar_keys  browser statusbar, from column 2 up to the clock: 41 columns
    #   text_gi_year/genre/players  the game-info metadata row: 5/7/10-column fields
    WIDTH_LIMITS = (("text_gi_year", 5), ("text_gi_genre", 7), ("text_gi_players", 10),
                    ("text_statusbar_keys", 41), ("text_si_", 40), ("text_cheat_noname", 42), ("text_cheat_flag_", 4),
                    ("text_no_", 22), ("cheat_tab_head", 48),
                    ("text_mtl_", 40), ("text_mt_", 32), ("text_pcm_", 40),
                    ("mtext_", 40),
                    ("mdesc_", 160), ("text_err_", 26), ("text_ce_", 6))
    WIDTH_DEFAULT = 56

    def encoded_len(text):
        n = 0
        for p in split_args(encode_string(text)):
            if p.startswith('"'):
                n += len(p) - 2      # quoted run -> 1 byte per char
            elif p != "0":
                n += 1               # raw byte (accent / {NNN} placeholder)
        return n

    def budget_for(label):
        for prefix, lim in WIDTH_LIMITS:
            if label.startswith(prefix):
                return lim
        return WIDTH_DEFAULT

    too_wide = []
    for lang_name, d in langs:   # every loaded translation: validate each
        use(lang_name)
        for label in order:
            text = d.get(label)
            if not text:
                continue
            n, lim = encoded_len(text), budget_for(label)
            if n > lim:
                too_wide.append(f"{label} [{lang_name}]: {n} > {lim} bytes: {text!r}")
    use(None)
    if too_wide:
        sys.exit("build_const.py: translation(s) exceed their UI slot:\n  "
                 + "\n  ".join(too_wide))

    # The System Information templates (text_si_*) carry raw bytes $02..$1F that the
    # menu replaces with values from the firmware's binary block. A translation that
    # drops one silently loses a field; one that adds or repeats one prints a field
    # twice or, worse, formats an unrelated one. Neither shows up as a build error
    # anywhere else, so require the exact same MULTISET of placeholder bytes as the
    # English base (multiset, not set: the SGB line legitimately uses {17} twice).
    def placeholders(args):
        counts = {}
        for p in split_args(args):
            if p.startswith('"'):
                continue
            try:
                n = int(p, 0)
            except ValueError:
                continue
            if 2 <= n <= 31:
                counts[n] = counts.get(n, 0) + 1
        return counts

    def fmt_counts(counts):
        return ", ".join(f"{{{n}}}x{c}" for n, c in sorted(counts.items())) or "(none)"

    bad_ph = []
    for lang_name, d in langs:
        use(lang_name)
        for label in order:
            if not label.startswith("text_si_"):
                continue
            text = d.get(label)
            if not text:
                continue
            want = placeholders(en_args[label])
            got = placeholders(encode_string(text))
            if want != got:
                bad_ph.append(f"{label} [{lang_name}]: has {fmt_counts(got)}, "
                              f"English base has {fmt_counts(want)}")
    use(None)
    if bad_ph:
        sys.exit("build_const.py: sysinfo template(s) with mismatched placeholders:\n  "
                 + "\n  ".join(bad_ph))

    # Intern identical strings so a label whose translations coincide (e.g. an
    # untranslated language that falls back to English) stores each unique byte
    # sequence only once.
    # TWO pools, chosen per LANGUAGE: one bank cannot hold eight columns. Each
    # column's strings live entirely in one pool, so the reader needs one bank byte
    # per language (strpool_bank, emitted with the tables) instead of one per entry.
    # Pool A = the const_lang_str object ($C2), pool B = appended to const_lang_tab
    # ($C1, which has tens of KB free). The biggest columns go to pool B. A string
    # shared by columns in different pools is stored once in EACH pool.
    POOL_B_LANGS = ("ru", "nl")
    pools = ({}, {})          # `.byt` args -> shared label, one dict per pool
    pool_orders = ([], [])    # preserve emission order
    pool_prefix = ("strpool_", "strpoolb_")

    def intern(args, which):
        pool = pools[which]
        if args not in pool:
            pool[args] = f"{pool_prefix[which]}{len(pool)}"
            pool_orders[which].append(args)
        return pool[args]

    col_pool = [0] + [1 if code in POOL_B_LANGS else 0 for code, _ in langs]
    col_code = [None] + [code for code, _ in langs]

    # A CJK column's strings go to that language's own pool, shipped in lang_<code>.bin and
    # read from WRAM bank $7F: its table words are absolute addresses there.
    ext = {code: ({}, bytearray()) for code in lang_maps}

    def intern_ext(code, args):
        idx, data = ext[code]
        if args not in idx:
            idx[args] = CJK_BASE + CJK_HDR + len(data)
            data += args_to_bytes(args)
        return f"${idx[args]:04x}"

    tabledefs = []
    plaindefs = []   # labels whose languages coincide -> plain string, no table
    for label in order:
        en = en_args[label]
        # Language-neutral? Compare via normalized decode so a translation that
        # merely repeats the English (or an untranslated language that falls back to
        # English) collapses to a single plain string with NO dispatch
        # table -- resolve_str passes any pointer outside [strtab_lo,strtab_hi)
        # straight through. This keeps the menu inside one 64K bank.
        use(None)
        en_norm = encode_string(decode_args(en))
        norms = [en_norm]
        strings = [en]
        for code, d in langs:
            use(code)
            text = d.get(label)
            args = encode_string(text) if text else en
            norms.append(encode_string(text) if text else en_norm)
            strings.append(args)
        use(None)
        if all(n == en_norm for n in norms):
            plaindefs.append((label, en))
            continue
        tabledefs.append((label, [intern_ext(col_code[c], args) if col_code[c] in ext
                                  else "!" + intern(args, col_pool[c])
                                  for c, args in enumerate(strings)]))

    if plaindefs:
        out += ["", "; ==== language-neutral labels (same in all langs): no table ===="]
        for label, args in plaindefs:
            out.append(f"{label} .byt {args}")

    # The interned pool and the dispatch tables live in SEPARATE banks so the menu
    # can grow past 128K AND the scarce pool bank is not also paying for the tables:
    # the pool -> <out>_str.a65 at $C2, the tables -> <out>_tab.a65 at $C1 (tens of KB
    # free). menudata reaches each dispatch table via ^label, so the split is
    # transparent there, but resolve_str (ui.a65) and ovl_fill_noname
    # (sysinfo_render.a65) MUST use ^strtab_lo for the table and strpool_bank[lang] for
    # the string it names -- getting one of the two wrong assembles clean and renders junk.
    strout = [".link page $c2", "",
              "; ==== interned language string pool A (deduplicated) ====",
              "; strpool_lo: the bank of THIS label is the bank of every string of the",
              "; columns strpool_bank maps here (resolve_str / ovl_fill_noname).",
              "strpool_lo"]
    for args in pool_orders[0]:
        strout.append(f"{pools[0][args]} .byt {args}")

    # Number of language COLUMNS actually present. Trailing columns whose every
    # entry just repeats English (e.g. an unfilled scaffold) are dropped
    # so they cost no table space; resolve_str maps cur_lang >= strtab_nlang back
    # to English (column 0). The Makefile argument order fixes the language order:
    # EN(0), then each lang_*.py in order. A column is kept only if it or a later
    # one carries a real translation.
    def differs(text, lbl):
        return bool(text) and (encode_string(text)
                               != encode_string(decode_args(en_args.get(lbl, ""))))
    nlang = 1
    for idx, (code, d) in enumerate(langs, start=1):
        use(code)
        if any(differs(d.get(l), l) for l in order):
            nlang = idx + 1
    use(None)
    # The in-game readers (ovl_resolve_str) cannot reach a CJK pool: it is in WRAM, which
    # in game belongs to the game. They clamp to the columns before the first CJK one.
    nlatin = next((c for c in range(1, nlang) if col_code[c] in ext), nlang)

    lang_names = ", ".join(["EN"] + [code for code, _ in langs])
    tabout = [".link page $c1", "",
              f"; ==== dispatch tables: resolve_str range [strtab_lo, strtab_hi) ====",
              f"; each table = {nlang} x 16-bit address ({lang_names})[:{nlang}]; NO bank byte:",
              "; the strings live in a pool of ANOTHER object, so a reader takes the table",
              "; with ^strtab_lo and the string it names with strpool_bank[lang].",
              "; cur_lang >= strtab_nlang -> EN."]
    tabout.append(f"strtab_nlang .byt {nlang}")
    tabout.append(f"strtab_nlang_latin .byt {nlatin}   ; columns without a CJK pool (in-game clamp)")
    tabout.append("; bank of each language's pool, indexed by the (clamped) language")
    tabout.append("strpool_bank .byt " + ", ".join(
        "$7f" if col_code[c] in ext else ("^strpoolb_lo" if col_pool[c] else "^strpool_lo")
        for c in range(nlang)))
    tabout.append("; per column: 0, or the two letters of a CJK language whose pool+glyphs are")
    tabout.append("; /sd2snes/lang/<code>.bin (cjk.a65 cjk_lang_sync), little-endian")
    tabout.append("cjk_lang_code .word " + ", ".join(
        f"${ord(col_code[c][1]) << 8 | ord(col_code[c][0]):04x}" if col_code[c] in ext else "$0000"
        for c in range(nlang)))
    tabout.append("strtab_lo")
    for label, labels in tabledefs:
        cols = labels[:nlang]
        # `label .word ...` (no colon) matches the proven `label .byt ...` style.
        tabout.append(f"{label} " + " : ".join(f".word {c}" for c in cols))
    tabout.append("strtab_hi")
    # Pool B sits AFTER strtab_hi: resolve_str treats [strtab_lo, strtab_hi) as tables.
    tabout += ["", "; ==== interned language string pool B (" +
               ", ".join(POOL_B_LANGS) + ") ====", "strpoolb_lo"]
    for args in pool_orders[1]:
        tabout.append(f"{pools[1][args]} .byt {args}")

    # The RESIDENT glyph sheet (snes/cjk.a65): 32 bytes per glyph, in index order, the
    # glyphs every language shows (the languages' own names). Emitted even when empty so the
    # link list is fixed. The CJK languages' sheets go into their lang_<code>.bin.
    cjkout = [".link page $c1", "",
              "; ==== resident CJK glyph sheet (build_const.py; fonts: snes/fonts/) ====",
              "; 64 bytes per glyph: left 8x8 tile then right 8x8 tile of its 16-px cell, 4bpp.",
              "; Glyph i is the byte pair {$%02X + i/128, $80 + i%%128} in a string." % CJK_LEAD0,
              f"cjk_nglyphs .word {len(common_map)}",
              "cjk_sheet"]
    for ch, idx in common_map.items():
        tiles = cjk_glyph_tiles(font(common[ch])[ch])
        cjkout.append(f" .byt {', '.join(f'${b:02x}' for b in tiles)}  ; {idx} U+{ord(ch):04X}")
    cjkout.append(" .byt 0")                    # keeps the label valid when the sheet is empty

    # lang_<code>.bin: header, the column's string pool, its glyph sheet. Loaded to
    # $7F:CJK_BASE; every address in it (table words, sheet_addr) is absolute in bank $7F.
    for code, (idx, data) in ext.items():
        col = col_code.index(code)
        if col >= nlang:
            continue
        lmap = lang_maps[code]
        sheet = b"".join(cjk_glyph_tiles(font(CJK_LANGS[code])[ch]) for ch in lmap)
        sheet_addr = CJK_BASE + CJK_HDR + len(data)
        total = CJK_HDR + len(data) + len(sheet)
        if total > CJK_FILE_MAX:
            sys.exit(f"build_const.py: lang_{code}.bin is {total} bytes, $7F holds {CJK_FILE_MAX}")
        hdr = (b"SDL1" + bytes((col, 1)) + len(data).to_bytes(2, "little")
               + sheet_addr.to_bytes(2, "little") + len(lmap).to_bytes(2, "little")
               + total.to_bytes(2, "little") + bytes(2))
        out_path.with_name(f"lang_{code}.bin").write_bytes(hdr + bytes(data) + sheet)
        print(f"  lang_{code}.bin: column {col}, {len(idx)} strings ({len(data)} B), "
              f"{len(lmap)} glyphs ({len(sheet)} B), {total} B of {CJK_FILE_MAX}")

    out_path.write_text("\n".join(out) + "\n")
    cjk_path = out_path.with_name(out_path.stem + "_cjk" + out_path.suffix)
    cjk_path.write_text("\n".join(cjkout) + "\n")
    str_path = out_path.with_name(out_path.stem + "_str" + out_path.suffix)
    str_path.write_text("\n".join(strout) + "\n")
    tab_path = out_path.with_name(out_path.stem + "_tab" + out_path.suffix)
    tab_path.write_text("\n".join(tabout) + "\n")
    print(f"generated {out_path} + {str_path} + {tab_path}: {len(order)} localized labels "
          f"({len(pool_orders[0])} pooled strings in bank $C2, "
          f"{len(pool_orders[1])} in bank $C1, "
          f"{len(tabledefs)} dispatch tables in bank $C1), {nlang} language column(s)")


if __name__ == "__main__":
    main()
