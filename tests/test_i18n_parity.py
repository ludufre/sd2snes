#!/usr/bin/env python3
"""Parity tests for the menu i18n accent tables.

Three copies of the accent contract exist and must agree:
  - snes/utils/build_const.py ACCENTS   (encodes translations at build time)
  - snes/utils/fontedit.py    ACCENT_MAP (edits/regenerates the glyph tiles)
  - snes/font.a65             the glyph tiles themselves

A drift ships menu text whose accent bytes point at blank/wrong glyphs, found
only on real hardware. Run standalone (python3 tests/test_i18n_parity.py) or
via pytest.
"""
import importlib.util
import sys
from pathlib import Path

UTILS = Path(__file__).resolve().parent.parent / "snes" / "utils"


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


if __name__ == "__main__":
    failed = 0
    for fn in (test_accents_match_accent_map, test_accent_codes_have_glyphs,
               test_homoglyphs_match_and_stay_out_of_accents,
               test_homoglyph_codes_point_at_a_real_glyph,
               test_accent_tiles_are_distinct,
               test_cyrillic_table_matches_font):
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except AssertionError as e:
            print(f"FAIL {fn.__name__}: {e}")
            failed = 1
    sys.exit(failed)
