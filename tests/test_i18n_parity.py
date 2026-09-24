#!/usr/bin/env python3
"""Parity tests for the menu i18n accent tables.

Four copies of the accent contract exist and must agree:
  - snes/utils/build_const.py ACCENTS   (encodes translations at build time)
  - snes/utils/fontedit.py    ACCENT_MAP (edits/regenerates the glyph tiles)
  - snes/font.a65             the glyph tiles themselves
  - src/gameinfo.c            the MCU's UTF-8 -> font transcoder for game info

A drift ships menu text whose accent bytes point at blank/wrong glyphs, found
only on real hardware. Run standalone (python3 tests/test_i18n_parity.py) or
via pytest.
"""
import importlib.util
import re
import sys
from pathlib import Path

UTILS = Path(__file__).resolve().parent.parent / "snes" / "utils"
GAMEINFO_C = Path(__file__).resolve().parent.parent / "src" / "gameinfo.c"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, UTILS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


build_const = _load("build_const")
fontedit = _load("fontedit")


def test_accents_match_accent_map():
    """build_const.ACCENTS and fontedit.ACCENT_MAP must be the same table."""
    assert build_const.ACCENTS == fontedit.ACCENT_MAP, (
        "ACCENTS (build_const.py) != ACCENT_MAP (fontedit.py):\n"
        f"  only in build_const: {sorted(set(build_const.ACCENTS) - set(fontedit.ACCENT_MAP))}\n"
        f"  only in fontedit:    {sorted(set(fontedit.ACCENT_MAP) - set(build_const.ACCENTS))}\n"
        f"  value mismatches:    "
        f"{sorted(k for k in set(build_const.ACCENTS) & set(fontedit.ACCENT_MAP) if build_const.ACCENTS[k] != fontedit.ACCENT_MAP[k])}"
    )


def test_accent_codes_have_glyphs():
    """Every accent code must have a non-blank 2bpp tile in snes/font.a65."""
    _, tiles = fontedit.load_font()
    missing = []
    for ch, code in sorted(build_const.ACCENTS.items(), key=lambda kv: kv[1]):
        if code >= len(tiles) or not any(tiles[code]):
            missing.append(f"{ch!r} -> {code}")
    assert not missing, f"accent codes with no glyph tile in font.a65: {missing}"


def test_homoglyphs_match_and_stay_out_of_accents():
    """HOMOGLYPHS is the encode-only half of the table: a Cyrillic letter drawn
    by a tile another character already owns. The two copies must agree, and no
    homoglyph may also sit in ACCENTS -- that would give one code two owners and
    the decode direction would start handing back the wrong letter."""
    assert build_const.HOMOGLYPHS == fontedit.HOMOGLYPHS, (
        "HOMOGLYPHS (build_const.py) != HOMOGLYPHS (fontedit.py):\n"
        f"  only in build_const: {sorted(set(build_const.HOMOGLYPHS) - set(fontedit.HOMOGLYPHS))}\n"
        f"  only in fontedit:    {sorted(set(fontedit.HOMOGLYPHS) - set(build_const.HOMOGLYPHS))}"
    )
    both = sorted(set(build_const.HOMOGLYPHS) & set(build_const.ACCENTS))
    assert not both, f"characters in BOTH ACCENTS and HOMOGLYPHS: {both}"


def test_homoglyph_codes_point_at_a_real_glyph():
    """Every homoglyph must land on a tile that exists and is drawn -- it has no
    tile of its own, so a wrong code is invisible until it reaches a screen."""
    _, tiles = fontedit.load_font()
    bad = [f"{ch!r} -> {code}" for ch, code in sorted(build_const.HOMOGLYPHS.items(),
                                                      key=lambda kv: kv[1])
           if code >= len(tiles) or not any(tiles[code])]
    assert not bad, f"homoglyphs pointing at a blank/missing tile: {bad}"


def test_accent_tiles_are_distinct():
    """No two accented letters may share a tile. A byte-identical pair means one
    of them is drawn with the wrong mark and the reader sees the other letter:
    the circumflex used to be two dots, which made ê==ë, î==ï and û==ü, and left
    no shape for ä/ö to take."""
    _, tiles = fontedit.load_font()
    seen = {}
    clashes = []
    for ch, code in sorted(build_const.ACCENTS.items(), key=lambda kv: kv[1]):
        key = tuple(tiles[code])
        if key in seen:
            other_ch, other_code = seen[key]
            clashes.append(f"{other_ch!r}({other_code}) == {ch!r}({code})")
        seen[key] = (ch, code)
    assert not clashes, f"accent tiles that are byte-identical: {clashes}"


def test_cyrillic_table_matches_font():
    """fontedit.CYRILLIC is the source of the Cyrillic tiles, so `addrussian`
    must write font.a65 back unchanged. The review once redrew the glyphs
    straight in font.a65 while the table kept the first pass, and rerunning
    the command would have reverted that work without a word."""
    _, tiles = fontedit.load_font()
    generated = fontedit.russian_tiles()
    uncovered = sorted(ch for ch, code in build_const.ACCENTS.items()
                       if 177 <= code <= 223 and ch not in fontedit.CYRILLIC)
    drift = [f"{ch!r}({fontedit.ACCENT_MAP[ch]})" for ch in fontedit.CYRILLIC
             if generated[fontedit.ACCENT_MAP[ch]] != tiles[fontedit.ACCENT_MAP[ch]]]
    assert not uncovered, f"Cyrillic tiles with no art in fontedit.CYRILLIC: {uncovered}"
    assert not drift, f"font.a65 tiles that differ from fontedit.CYRILLIC: {drift}"


def _gameinfo_table(name):
    """(base codepoint, [font bytes]) of one lookup table in src/gameinfo.c."""
    src = GAMEINFO_C.read_text()
    base = int(re.search(rf"#define GI_FONT_{name.upper()}_BASE\s+(0x[0-9A-Fa-f]+)", src).group(1), 16)
    body = re.search(rf"gi_font_{name}\[\d+\] = \{{(.*?)\}};", src, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return base, [int(v) for v in body.replace(",", " ").split()]


def test_gameinfo_transcoder_matches_encode():
    """The MCU re-encodes the game-info .yml (UTF-8) with its own copy of the table,
    in src/gameinfo.c. It once stopped at the Portuguese/Spanish block (130..159),
    so every French/Italian/German accent and every Cyrillic letter in a description
    printed as '?'. Every non-ASCII ENCODE entry must map to the same byte there;
    anything else mapped in these tables must be a plain-ASCII stand-in (« -> '"'),
    never a glyph code the font assigns to some other letter."""
    mcu = {}
    for name in ("latin1", "cyrillic"):
        base, vals = _gameinfo_table(name)
        mcu.update({chr(base + i): v for i, v in enumerate(vals) if v})
    want = {ch: code for ch, code in build_const.ENCODE.items() if ord(ch) >= 0x80}
    missing = sorted(ch for ch in want if ch not in mcu)
    extra = sorted(f"{ch!r}->{mcu[ch]}" for ch in mcu if ch not in want and not 32 <= mcu[ch] <= 126)
    wrong = sorted(f"{ch!r}: {mcu[ch]} != {want[ch]}" for ch in want if ch in mcu and mcu[ch] != want[ch])
    assert not (missing or extra or wrong), (
        "src/gameinfo.c transcoder != build_const.ENCODE:\n"
        f"  missing: {missing}\n  extra: {extra}\n  wrong: {wrong}")


def _gameinfo_fallback():
    """(codepoints, stand-in strings) of gi_fb_cp / gi_fb_str in src/gameinfo.c."""
    src = GAMEINFO_C.read_text()
    cps = re.search(r"gi_fb_cp\[\] = \{(.*?)\};", src, re.S).group(1)
    strs = re.search(r"gi_fb_str\[\]\[3\] = \{(.*?)\};", src, re.S).group(1)
    cps = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", re.sub(r"/\*.*?\*/", "", cps, flags=re.S))]
    strs = [bytes(v, "ascii").decode("unicode_escape")
            for v in re.findall(r'"((?:\\.|[^"\\])*)"', re.sub(r"/\*.*?\*/", "", strs, flags=re.S))]
    return cps, strs


def test_gameinfo_fallback_table():
    """gi_fb_cp / gi_fb_str spell typography the font has no glyph for (the em dash, curly
    quotes, the ellipsis) with glyphs it has. Parallel arrays, so the lengths must agree;
    ordered, so a duplicate or a misplaced entry shows; each stand-in at most 3 printable
    ASCII bytes (the transcoder's output buffers hold 3). An entry for a character the
    font DOES draw would hide its glyph, and one whose codepoint the direct tables already
    map would never be reached."""
    cps, strs = _gameinfo_fallback()
    assert len(cps) == len(strs), f"gi_fb_cp has {len(cps)} entries, gi_fb_str {len(strs)}"
    assert cps == sorted(set(cps)), "gi_fb_cp must be strictly ascending"
    bad = [f"U+{c:04X}={s!r}" for c, s in zip(cps, strs)
           if len(s) > 3 or any(not 32 <= ord(ch) <= 126 for ch in s)]
    assert not bad, f"stand-ins longer than 3 bytes or not printable ASCII: {bad}"
    glyph = sorted(f"U+{c:04X}" for c in cps if chr(c) in build_const.ENCODE)
    assert not glyph, f"fallback entries for characters the font draws: {glyph}"
    direct = {}
    for name in ("latin1", "cyrillic"):
        base, vals = _gameinfo_table(name)
        direct.update({base + i: v for i, v in enumerate(vals)})
    dead = sorted(f"U+{c:04X}" for c in cps if direct.get(c))
    assert not dead, f"fallback entries shadowed by the direct tables: {dead}"


if __name__ == "__main__":
    failed = 0
    for fn in (test_accents_match_accent_map, test_accent_codes_have_glyphs,
               test_homoglyphs_match_and_stay_out_of_accents,
               test_homoglyph_codes_point_at_a_real_glyph,
               test_accent_tiles_are_distinct,
               test_cyrillic_table_matches_font,
               test_gameinfo_transcoder_matches_encode,
               test_gameinfo_fallback_table):
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except AssertionError as e:
            print(f"FAIL {fn.__name__}: {e}")
            failed = 1
    sys.exit(failed)
