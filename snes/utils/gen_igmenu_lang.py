#!/usr/bin/env python3
"""Generate igmenu_lang.i65 -- the in-game HELP tab's font-encoded i18n tables.

The in-game TAB menu shell (snes/igmenu.a65 -> igmenu.bin, bank $C8) is a SEPARATE
link from m3nu.bin, so it cannot reach the menu's $C1 string pool (that lives in a
different binary). Instead, this generator re-reads the English base (snes/const.a65)
and the SAME per-language dicts as build_const.py and emits a bank-$C8 include with a
flat 2D dispatch table:

    igm_help_tbl[line*IGM_LANG_COUNT + lang] -> 16-bit offset (within bank $C8) of a
    NUL-terminated, ALREADY font-encoded string.

igmenu.a65 reads CFG.language ($FF01B7) and indexes it. The strings are pre-encoded to
the font glyph codes here by REUSING build_const's ACCENTS/encode_string (single source
of the accent map -- the i18n parity test guards ACCENTS itself), so the shell never
touches the accent map.

Guards (exit 1):
  - a text_igm_* label in const.a65 missing from any lang dict (parity)
  - a text_igm_* dict key not present in const.a65 (parity, both directions)
  - a HELP_LABELS entry missing from const.a65
  - an encoded line wider than IGM_WIDTH_MAX (the overlay is 64 hi-res columns; a
    prudent cap of 60 -- build_const's default 56 is stricter and also applies)

Usage:
    gen_igmenu_lang.py <const.a65> <lang_ptbr.py> <lang_es.py> <lang_de.py> <lang_fr.py> -o <igmenu_lang.i65>
"""
import sys
from pathlib import Path

import build_const as bc

IGM_PREFIX = "text_igm_"
IGM_WIDTH_MAX = 60

# The HELP tab lines, IN RENDER ORDER. Lockstep with igm_help_rows / igm_help_cols in
# snes/igmenu.a65 (SAME count + order) and with the EN base labels in snes/const.a65.
HELP_LABELS = [
    "text_igm_hdr_controls",
    "text_igm_open",
    "text_igm_save",
    "text_igm_load",
    "text_igm_slot",
    "text_igm_reset",
    "text_igm_hdr_keys",
    "text_igm_tab",
    "text_igm_page",
    "text_igm_nav",
    "text_igm_act",
    "text_igm_close",
    "text_igm_note",
]

# The STATES tab (Phase 3) strings, IN INDEX ORDER. Lockstep with the IGM_ST_* indices
# in snes/igmenu.a65 (SAME count + order) and with the EN base labels in const.a65.
STATES_LABELS = [
    "text_igm_st_title",     # 0 header
    "text_igm_st_slot",      # 1 slot word ("SLOT")
    "text_igm_st_full",      # 2 occupied
    "text_igm_st_empty",     # 3 empty
    "text_igm_st_hint",      # 4 save/load hint
    "text_igm_st_disabled",  # 5 slots-disabled message
]

# Column layout in igmenu.a65 draws the slot word at col 24 with the digit at ~col 31,
# so the slot word must stay <= 8 columns; guard it here (build fails otherwise).
STATES_LABEL_MAX = {"text_igm_st_slot": 8}

# Words per table row. MUST be a power of two and MUST match the number of `asl`
# in the nine row-index sites of snes/igmenu.a65 (search IGM_LANG_SHIFT there).
# See the stride note in main() for why this is fixed rather than derived.
IGM_LANG_STRIDE = 8
IGM_LANG_SHIFT = 3   # log2(IGM_LANG_STRIDE)

# The SAVES tab (Phase 4) strings, IN INDEX ORDER. Lockstep with the IGM_SV_* indices in
# snes/igmenu.a65 (SAME count + order) and with the EN base labels in const.a65.
SAVES_LABELS = [
    "text_igm_sv_title",     # 0 header (centered)
    "text_igm_sv_nosram",    # 1 "no battery save" message (centered)
    "text_igm_sv_file",      # 2 ".srm file:" label
    "text_igm_sv_exists",    # 3 EXISTS token
    "text_igm_sv_missing",   # 4 MISSING token
    "text_igm_sv_size",      # 5 "Size:" label
    "text_igm_sv_write",     # 6 "Last write:" label
    "text_igm_sv_autosave",  # 7 "Autosave:" label
    "text_igm_sv_on",        # 8 ON token
    "text_igm_sv_off",       # 9 OFF token
    "text_igm_sv_hint_on",   # 10 footer hint, autosave on (centered)
    "text_igm_sv_hint_off",  # 11 footer hint, autosave off (centered)
    "text_igm_sv_slottitle", # 12 SRAM slot selector header (centered)
    "text_igm_sv_slot",      # 13 "SLOT" word (<= 8 cols)
    "text_igm_sv_slothint",  # 14 slot selector footer hint (centered)
]

# SAVES layout in igmenu.a65 puts the left labels at col 16 and their values at col 36,
# so the field labels must stay <= 14 cols (label + value never collide). The status
# tokens sit at col 36 (room 28) -> cap at 24. Centered strings use the overlay width.
SAVES_LABEL_MAX = {
    "text_igm_sv_file": 14,
    "text_igm_sv_size": 14,
    "text_igm_sv_write": 14,
    "text_igm_sv_autosave": 14,
    "text_igm_sv_exists": 24,
    "text_igm_sv_missing": 24,
    "text_igm_sv_on": 24,
    "text_igm_sv_off": 24,
    "text_igm_sv_slot": 8,   # slot word drawn at col 24 with the digit at ~col 31 (like STATES)
}

# The tab-bar labels, IN TAB ORDER (0=Cheats..4=Help). Lockstep with the tab dispatch
# order in snes/igmenu.a65 and with the EN base labels in const.a65.
TAB_LABELS = [
    "text_igm_tab_cheats",   # 0 CHEATS
    "text_igm_tab_states",   # 1 SAVESTATES
    "text_igm_tab_saves",    # 2 SAVES
    "text_igm_tab_manual",   # 3 GUIDES
    "text_igm_tab_help",     # 4 HELP
]

# The MANUAL tab (Phase 5) strings, IN INDEX ORDER. Lockstep with the IGM_MN_* indices in
# snes/igmenu.a65 (SAME count + order) and with the EN base labels in const.a65. All are
# centered lines except "pages" (a word composed with the page count at runtime).
MANUAL_LABELS = [
    "text_igm_mn_title",     # 0 tab-body title (centered)
    "text_igm_mn_read",      # 1 "A: Read the manual" prompt (centered)
    "text_igm_mn_pages",     # 2 "pages" word ("<N> pages", centered with the count)
    "text_igm_mn_notfound",  # 3 "Manual not found" message (centered)
    "text_igm_mn_error",     # 4 "Error loading manual" message (centered)
    "text_igm_mn_guide",     # 5 fallback title word ("GUIDE N" when a guide has no title)
    "text_igm_mn_select",    # 6 guide-list header (centered, count>1)
    "text_igm_mn_keys1",     # 7 control legend, line 1 (zoom / page turn)
    "text_igm_mn_keys2",     # 8 control legend, line 2 (pan / close)
    # document-type labels — index = IGM_MN_SLUG_MANUAL + (slug-1); the `.man` header carries the slug
    # (title[0]=1..5) and the firmware renders the label in the user's language (lockstep IGM_MN_SLUG_*).
    "text_igm_mn_slug_manual",  # 9  slug 1 -> Manual
    "text_igm_mn_slug_guide",   # 10 slug 2 -> Guide
    "text_igm_mn_slug_map",     # 11 slug 3 -> Map
    "text_igm_mn_slug_insert",  # 12 slug 4 -> Insert
    "text_igm_mn_slug_other",   # 13 slug 5 -> Other
]

# The CHEATS tab strings, IN INDEX ORDER. Unlike the other tabs there is no index enum
# in snes/igmenu.a65: igm_fill_noname indexes line 0 directly (igm_cheats_tbl[lang]), so
# a second entry here would need that routine to take a line index first.
CHEATS_LABELS = [
    "text_igm_cheat_noname",  # 0 placeholder for a cheat whose YAML carries no name
]

# The placeholder is copied into OVL_NONAME_BUF (32 B, see memmap.i65) and drawn in the
# list's name column; cap it well inside both.
CHEATS_LABEL_MAX = {"text_igm_cheat_noname": 24}

# The tab bar centers each label in a 10-column field (IGM_TAB_FIELD in igmenu.a65), so
# every translated tab label must stay <= 10 encoded columns; guard it here.
TAB_LABEL_MAX = 10


def cap_for(label):
    """Per-label encoded-width cap (build fails if a translation exceeds it)."""
    if label in TAB_LABELS:
        return TAB_LABEL_MAX
    if label in SAVES_LABEL_MAX:
        return SAVES_LABEL_MAX[label]
    if label in CHEATS_LABEL_MAX:
        return CHEATS_LABEL_MAX[label]
    return STATES_LABEL_MAX.get(label, IGM_WIDTH_MAX)


def lang_code(path):
    stem = Path(path).stem
    return stem[5:] if stem.startswith("lang_") else stem


def encoded_len(args):
    """Encoded byte count of a `.byt` argument list (quoted runs + raw accent/
    placeholder bytes, excluding the trailing 0 terminator)."""
    n = 0
    for p in bc.split_args(args):
        if p.startswith('"'):
            n += len(p) - 2      # quoted run -> 1 byte per char
        elif p != "0":
            n += 1               # raw byte (accent / {NNN} placeholder)
    return n


def main():
    base = Path(sys.argv[1])
    out_i = sys.argv.index("-o")
    lang_paths = [p for p in sys.argv[2:out_i] if p.endswith(".py")]
    out_path = Path(sys.argv[out_i + 1])
    langs = [(lang_code(p), bc.load_dict(p)) for p in lang_paths]

    _, en_args = bc.parse_base(base)

    # --- parity: every text_igm_* in const.a65 must be in every dict, and every
    #     text_igm_* dict key must be in const.a65 (both directions) ---
    const_labels = {k for k in en_args if k.startswith(IGM_PREFIX)}
    problems = []
    for name, d in langs:
        dl = {k for k in d if k.startswith(IGM_PREFIX)}
        for lbl in sorted(const_labels - dl):
            problems.append(f"{lbl}: missing from lang_{name}.py")
        for lbl in sorted(dl - const_labels):
            problems.append(f"{lbl}: in lang_{name}.py but not in {base.name}")
    for lbl in (HELP_LABELS + STATES_LABELS + SAVES_LABELS + TAB_LABELS + MANUAL_LABELS
                + CHEATS_LABELS):
        if lbl not in en_args:
            problems.append(f"{lbl}: listed in a *_LABELS table but not in {base.name}")
    if problems:
        sys.exit("gen_igmenu_lang.py: i18n parity error(s):\n  " + "\n  ".join(problems))

    lang_order = ["en"] + [name for name, _ in langs]   # EN first, then dict order
    nlang = len(lang_order)
    dict_by_code = dict(langs)

    def args_for(label, lang):
        """`.byt` args (font-encoded, ends with 0) for (label, lang). EN comes from
        const.a65 verbatim; others encode_string(dict[label]); parity above forbids a
        missing text_igm_* translation, so the EN fallback is just belt-and-braces."""
        if lang == "en":
            return en_args[label]
        text = dict_by_code[lang].get(label)
        return bc.encode_string(text) if text else en_args[label]

    # --- width guard (overlay width, plus tighter per-label caps for column layout) ---
    wide = []
    for lbl in (HELP_LABELS + STATES_LABELS + SAVES_LABELS + TAB_LABELS + MANUAL_LABELS
                + CHEATS_LABELS):
        cap = cap_for(lbl)
        for lang in lang_order:
            n = encoded_len(args_for(lbl, lang))
            if n > cap:
                wide.append(f"{lbl} [{lang}]: {n} > {cap} bytes")
    if wide:
        sys.exit("gen_igmenu_lang.py: line(s) exceed the overlay width:\n  "
                 + "\n  ".join(wide))

    # --- row stride ---------------------------------------------------------
    # The tables are indexed as `line*STRIDE + lang`, and igmenu.a65 computes that
    # offset in 65816. A stride that is a power of two makes the row index a plain
    # shift (`asl : asl : asl`), so the nine index sites in igmenu.a65 never have to
    # change when a language is added -- previously each one open-coded `*5` as
    # `asl : asl : adc idx`, which the assembler happily accepted after a 6th
    # language landed and shipped a silently mis-indexed binary.
    # The stride is FIXED, not derived from nlang: deriving it would silently
    # re-break the same sites the moment the language count crossed a power of two.
    # Columns past nlang are dead (every reader clamps to IGM_LANG_COUNT) but still
    # point at the EN string, so a stray index reads valid text instead of garbage.
    if nlang > IGM_LANG_STRIDE:
        sys.exit(f"gen_igmenu_lang.py: {nlang} languages exceed IGM_LANG_STRIDE="
                 f"{IGM_LANG_STRIDE}.\nRaise IGM_LANG_STRIDE to the next power of two "
                 f"AND update the {IGM_LANG_SHIFT} `asl` in each of the nine row-index "
                 f"sites in snes/igmenu.a65 (grep IGM_LANG_SHIFT).")
    padded_order = lang_order + ["en"] * (IGM_LANG_STRIDE - nlang)

    def emit_table(table_name, str_prefix, labels):
        """Emit `<table_name>[line*IGM_LANG_STRIDE + lang] -> 16-bit $C8 offset` plus the
        NUL-terminated font-encoded source strings (labels `<str_prefix><li>_<lang>`)."""
        lines = [
            f"// {table_name}[line*IGM_LANG_STRIDE + lang] = 16-bit offset within bank $C8",
            "// of the NUL-terminated font-encoded string. igmenu.a65: lda @<tbl>,x : tax",
            "// then lda @$c80000,x walks the source bytes (long,X, bank $C8).",
            f"// Row is {IGM_LANG_STRIDE} words wide; columns {nlang}..{IGM_LANG_STRIDE - 1} "
            "are dead padding (aliased to EN) that keeps the row index a pure shift.",
            f"{table_name}:",
        ]
        for li in range(len(labels)):
            row = " : ".join(f".word {str_prefix}{li}_{lang}" for lang in padded_order)
            lines.append(f"  {row}")
        lines.append("")
        for li, lbl in enumerate(labels):
            for lang in lang_order:
                lines.append(f"{str_prefix}{li}_{lang} .byt {args_for(lbl, lang)}")
        lines.append("")
        return lines

    out = [
        "// igmenu_lang.i65 -- GENERATED by utils/gen_igmenu_lang.py. DO NOT EDIT.",
        f"// In-game TAB menu i18n string tables, font-encoded ({nlang} langs: "
        f"{', '.join(lang_order)}).",
        "// Bank $C8 data (inherits igmenu.a65's .link; jumped over, never executed).",
        "// snescom preprocessing: use // comments, never ; inside an included file.",
        "",
        f"#define IGM_HELP_NLINES {len(HELP_LABELS)}",
        f"#define IGM_STATES_NLINES {len(STATES_LABELS)}",
        f"#define IGM_SAVES_NLINES {len(SAVES_LABELS)}",
        f"#define IGM_TAB_NLINES {len(TAB_LABELS)}",
        f"#define IGM_MANUAL_NLINES {len(MANUAL_LABELS)}",
        f"#define IGM_CHEATS_NLINES {len(CHEATS_LABELS)}",
        f"#define IGM_LANG_COUNT {nlang}",
        f"#define IGM_LANG_STRIDE {IGM_LANG_STRIDE}",
        f"#define IGM_LANG_SHIFT {IGM_LANG_SHIFT}",
        "",
    ]
    out += emit_table("igm_help_tbl", "igm_s", HELP_LABELS)
    out += emit_table("igm_states_tbl", "igm_sts", STATES_LABELS)
    out += emit_table("igm_saves_tbl", "igm_sv", SAVES_LABELS)
    out += emit_table("igm_tab_tbl", "igm_tb", TAB_LABELS)
    out += emit_table("igm_manual_tbl", "igm_mn", MANUAL_LABELS)
    out += emit_table("igm_cheats_tbl", "igm_ch", CHEATS_LABELS)

    out_path.write_text("\n".join(out) + "\n")
    print(f"generated {out_path}: HELP {len(HELP_LABELS)} + STATES {len(STATES_LABELS)} "
          f"+ SAVES {len(SAVES_LABELS)} + TAB {len(TAB_LABELS)} + MANUAL {len(MANUAL_LABELS)} "
          f"+ CHEATS {len(CHEATS_LABELS)} "
          f"lines x {nlang} langs ({', '.join(lang_order)})")


if __name__ == "__main__":
    main()
