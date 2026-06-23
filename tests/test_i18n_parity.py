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


if __name__ == "__main__":
    failed = 0
    for fn in (test_accents_match_accent_map, test_accent_codes_have_glyphs):
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except AssertionError as e:
            print(f"FAIL {fn.__name__}: {e}")
            failed = 1
    sys.exit(failed)
