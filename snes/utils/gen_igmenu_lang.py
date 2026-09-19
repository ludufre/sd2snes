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

# The HELP screen strings, IN INDEX ORDER. Lockstep with the IGM_HP_* indices in
# snes/ighelp.i65 (SAME count + order) and with the EN base labels in snes/const.a65.
# The shortcut lines are the ACTION only; the combo beside each one is rendered at run
# time from the pad word (ig_combo_draw), never baked into a translated string.
HELP_LABELS = [
    "text_igm_hdr_controls",  # 0  header (centered)
    "text_igm_open",          # 1  action: open the in-game menu
    "text_igm_save",          # 2  action: save state
    "text_igm_load",          # 3  action: load state
    "text_igm_slot",          # 4  action: choose slot
    "text_igm_reset_game",    # 5  action: reset the game
    "text_igm_reset",         # 6  action: reset to the menu
    "text_igm_cheats_on",     # 7  action: cheats on
    "text_igm_cheats_off",    # 8  action: cheats off
    "text_igm_hooks_off",     # 9  action: disable the in-game hooks
    "text_igm_hooks_10s",     # 10 action: disable them for 10 seconds
    "text_igm_pergame",       # 11 footnote for a per-game savestate combo (centered)
    "text_igm_hdr_keys",      # 12 header (centered)
    "text_igm_keys1",         # 13 menu keys, line 1 (centered)
    "text_igm_keys2",         # 14 menu keys, line 2 (centered)
    "text_igm_unavail",       # 15 value-column text when savestates are off for this game
]

# HELP geometry (ighelp.i65): action labels start at col 6 and the combo column at 32, so
# an action may not exceed 24 columns; the "unavailable" text sits IN the combo column
# (32..59). Headers and the centered lines only need to stay inside the frame interior.
HELP_LABEL_MAX = {lbl: 24 for lbl in HELP_LABELS[1:11]}
HELP_LABEL_MAX.update({
    "text_igm_hdr_controls": 40, "text_igm_hdr_keys": 40,
    "text_igm_pergame": 56, "text_igm_keys1": 56, "text_igm_keys2": 56,
    "text_igm_unavail": 28,
})

# D-pad direction names printed INSIDE a rendered combo, IN PAD-BIT ORDER
# ($0800 Up, $0400 Down, $0200 Left, $0100 Right). Lockstep with igc_tok_tbl in
# snes/ighelp.i65. The other button names are neutral ASCII over there.
KEYS_LABELS = [
    "text_igm_key_up",
    "text_igm_key_down",
    "text_igm_key_left",
    "text_igm_key_right",
]
KEYS_LABEL_MAX = {lbl: 8 for lbl in KEYS_LABELS}

# The STATES tab (Phase 3) strings, IN INDEX ORDER. Lockstep with the IGM_ST_* indices
# in snes/igmenu.a65 (SAME count + order) and with the EN base labels in const.a65.
STATES_LABELS = [
    "text_igm_st_title",     # 0 header
    "text_igm_st_slot",      # 1 slot word ("SLOT")
    "text_igm_st_full",      # 2 occupied
    "text_igm_st_empty",     # 3 empty
    "text_igm_st_hint_save", # 4 hint word after the save combo ("<combo> save")
    "text_igm_st_disabled",  # 5 slots-disabled message
    "text_igm_st_hint_load", # 6 hint word after the load combo
    "text_igm_st_unavail",   # 7 savestates not available for this game (centered)
]

# Column layout in igmenu.a65 draws the slot word at col 24 with the digit at ~col 31,
# so the slot word must stay <= 8 columns; guard it here (build fails otherwise).
# The hint line is COMPOSED at run time ("<save combo> <word>   <load combo> <word>"), so
# the two words stay short enough for two four-button combos to share the row with them.
STATES_LABEL_MAX = {"text_igm_st_slot": 8, "text_igm_st_hint_save": 12,
                    "text_igm_st_hint_load": 12, "text_igm_st_unavail": 50}

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

# The tab-bar labels, IN TAB ORDER (0=Cheats..4=Trainer, 5=Help). Lockstep with the tab dispatch
# order in snes/igmenu.a65 and with the EN base labels in const.a65.
TAB_LABELS = [
    "text_igm_tab_cheats",   # 0 CHEATS
    "text_igm_tab_states",   # 1 SAVESTATES
    "text_igm_tab_saves",    # 2 SAVES
    "text_igm_tab_manual",   # 3 GUIDES
    "text_igm_tab_trainer",  # 4 TRAINER
    "text_igm_tab_help",     # 5 HELP -- off the bar (IGMENU_TABS = 5): SELECT opens it
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

# The CHEATS tab strings, IN INDEX ORDER. Lockstep with the IGM_CH_* indices in
# snes/igmenu.a65. Line 0 stays first because igm_fill_noname indexes it directly as
# igm_cheats_tbl[lang] (line 0 is at offset 0 whatever comes after), while line 1 is
# read through ch_copy/ch_strlen, which do take a line index.
CHEATS_LABELS = [
    "text_igm_cheat_noname",  # 0 placeholder for a cheat whose YAML carries no name
    "text_igm_ch_none",       # 1 centered message when the ROM has no cheats at all
]

# The placeholder is copied into OVL_NONAME_BUF (32 B, see memmap.i65) and drawn in the
# list's name column; cap it well inside both. The empty-list message is centered on a
# 64-column row by ch_draw_centered, so it only needs to stay well short of that.
CHEATS_LABEL_MAX = {"text_igm_cheat_noname": 24, "text_igm_ch_none": 40}

# Shell chrome shared by every tab, IN INDEX ORDER. Lockstep with the IGM_SH_* indices
# in snes/igmenu.a65. These were hard-coded ASCII in igmenu.a65 until the Russian
# translation landed: a Cyrillic menu with a Latin footer reads as broken, so the
# "Cheats stays English" convention gets an explicit exception for the in-game chrome.
SHELL_LABELS = [
    "text_igm_ft_tab",        # 0 footer, tab-bar focus
    "text_igm_ft_cheats",     # 1 footer, CHEATS content focus
    "text_igm_ft_generic",    # 2 footer, any other content focus
    "text_igm_master_on",     # 3 master cheat switch, enabled
    "text_igm_master_off",    # 4 master cheat switch, disabled
    "text_igm_ft_ce_edit",    # 5 footer, cheat editor (item list / confirm)
    "text_igm_ft_ce_kbd",     # 6 footer, cheat editor keyboard
    "text_igm_ft_help",       # 7 footer, HELP screen
]

# The cheat EDITOR (CHEATS tab: add / edit / delete a cheat), IN INDEX ORDER. Lockstep with
# the IGM_CE_* indices in snes/cheatedit_defs.i65 (SAME count + order) and with the EN base
# labels in const.a65.
CHEATEDIT_LABELS = [
    "text_igm_ce_title_new",    # 0 title while adding (centered in the 52-col interior)
    "text_igm_ce_title_edit",   # 1 title while editing
    "text_igm_ce_name",         # 2 "Name:" label at col 8 -- the value starts at col 16
    "text_igm_ce_save",         # 3 SAVE item
    "text_igm_ce_delete",       # 4 DELETE item
    "text_igm_ce_confirm",      # 5 "Delete this cheat?" (centered)
    "text_igm_ce_confirm_keys", # 6 "A: yes  B: no" (centered)
    "text_igm_ce_nosup",        # 7 old firmware (centered)
    "text_igm_ce_full",         # 8 512 records already (centered)
    "text_igm_ce_badcode",      # 9 a code the firmware refused (centered)
]

# Editor geometry (cheatedit_defs.i65): interior 52 columns; the Name label runs from col 8
# and its value from col 16, so the label may not exceed 7 encoded columns.
CHEATEDIT_LABEL_MAX = {lbl: 52 for lbl in CHEATEDIT_LABELS}
CHEATEDIT_LABEL_MAX["text_igm_ce_name"] = 7

# All five are centered on a 64-column row by sh_draw_centered, so the only real
# limit is the row itself; keep a margin so a long translation cannot touch the edges.
SHELL_LABEL_MAX = {lbl: 56 for lbl in SHELL_LABELS}

# The TRAINER tab strings, IN INDEX ORDER. Lockstep with the IGM_TR_* indices in
# snes/trainer.i65 (SAME count + order) and with the EN base labels in const.a65.
TRAINER_LABELS = [
    "text_igm_tr_title",        # 0  window title (centered)
    "text_igm_tr_searchtype",   # 1  field label
    "text_igm_tr_datatype",     # 2  field label
    "text_igm_tr_value",        # 3  field label
    "text_igm_tr_candidates",   # 4  field label
    "text_igm_tr_compare",      # 5  field label
    "text_igm_tr_exact",        # 6  search type value
    "text_igm_tr_unknown",      # 7  search type value
    "text_igm_tr_8bit",         # 8  data type value
    "text_igm_tr_16bit",        # 9  data type value
    "text_igm_tr_eq",           # 10 compare value -- ORDER IS TR_MODE_EQ..TR_MODE_LT
    "text_igm_tr_ne",           # 11
    "text_igm_tr_changed",      # 12
    "text_igm_tr_unchanged",    # 13
    "text_igm_tr_increased",    # 14
    "text_igm_tr_decreased",    # 15
    "text_igm_tr_gt",           # 16
    "text_igm_tr_lt",           # 17
    "text_igm_tr_newsearch",    # 18 action
    "text_igm_tr_filter",       # 19 action
    "text_igm_tr_results",      # 20 action
    "text_igm_tr_reset",        # 21 action
    "text_igm_tr_address",      # 22 detail label
    "text_igm_tr_current",      # 23 detail label
    "text_igm_tr_setvalue",     # 24 action
    "text_igm_tr_freeze",       # 25 action
    "text_igm_tr_unfreeze",     # 26 action
    "text_igm_tr_savecheat",    # 27 action
    "text_igm_tr_frozen",       # 28 status token
    "text_igm_tr_searching",    # 29 message (centered)
    "text_igm_tr_noresults",    # 30 message (centered)
    "text_igm_tr_toomany",      # 31 message (centered)
    "text_igm_tr_dropped_rst",  # 32 message (centered)
    "text_igm_tr_nofreeze",     # 33 message (centered)
    "text_igm_tr_hint_setup",   # 34 footer hint (centered)
    "text_igm_tr_hint_edit",    # 35 footer hint (centered)
    "text_igm_tr_hint_list",    # 36 footer hint (centered)
    "text_igm_tr_master_off",   # 37 warning: a freeze exists but the master switch is off
    "text_igm_tr_saved",        # 38 SAVE CHEAT result (centered)
    "text_igm_tr_savefail",     # 39 SAVE CHEAT result (centered)
    "text_igm_tr_unpin",        # 40 action: take a set/frozen address off the results
]

# The TRAINER body is a 36-column window (ig_frame_geom x=13 w=38 -> interior cols 14..49):
# labels start at col 16 and their values at col 34, so a label may not exceed 16 columns
# and a value 14, or the two collide. Actions and messages are centered inside the interior.
TRAINER_LABEL_MAX = {
    "text_igm_tr_searchtype": 16, "text_igm_tr_datatype": 16, "text_igm_tr_value": 16,
    "text_igm_tr_candidates": 16, "text_igm_tr_compare": 16,
    "text_igm_tr_exact": 14, "text_igm_tr_unknown": 14,
    "text_igm_tr_8bit": 14, "text_igm_tr_16bit": 14,
    "text_igm_tr_eq": 14, "text_igm_tr_ne": 14, "text_igm_tr_changed": 14,
    "text_igm_tr_unchanged": 14, "text_igm_tr_increased": 14, "text_igm_tr_decreased": 14,
    "text_igm_tr_gt": 14, "text_igm_tr_lt": 14,
    "text_igm_tr_newsearch": 22, "text_igm_tr_filter": 22, "text_igm_tr_results": 22,
    "text_igm_tr_reset": 22, "text_igm_tr_setvalue": 22, "text_igm_tr_freeze": 22,
    "text_igm_tr_unfreeze": 22, "text_igm_tr_savecheat": 22, "text_igm_tr_unpin": 22,
    "text_igm_tr_address": 14, "text_igm_tr_current": 14, "text_igm_tr_frozen": 14,
}
TRAINER_MSG_MAX = 36   # centered lines inside the 36-column interior

# The tab bar centers each label in a 10-column field (IGM_TAB_FIELD in igmenu.a65), so
# every translated tab label must stay <= 10 encoded columns; guard it here.
TAB_LABEL_MAX = 10


def cap_for(label):
    """Per-label encoded-width cap (build fails if a translation exceeds it)."""
    if label in TAB_LABELS:
        return TAB_LABEL_MAX
    if label in HELP_LABEL_MAX:
        return HELP_LABEL_MAX[label]
    if label in KEYS_LABEL_MAX:
        return KEYS_LABEL_MAX[label]
    if label in SAVES_LABEL_MAX:
        return SAVES_LABEL_MAX[label]
    if label in CHEATS_LABEL_MAX:
        return CHEATS_LABEL_MAX[label]
    if label in SHELL_LABEL_MAX:
        return SHELL_LABEL_MAX[label]
    if label in CHEATEDIT_LABEL_MAX:
        return CHEATEDIT_LABEL_MAX[label]
    if label in TRAINER_LABEL_MAX:
        return TRAINER_LABEL_MAX[label]
    if label in TRAINER_LABELS:
        return TRAINER_MSG_MAX
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
    all_labels = (HELP_LABELS + KEYS_LABELS + STATES_LABELS + SAVES_LABELS + TAB_LABELS
                  + MANUAL_LABELS + CHEATS_LABELS + SHELL_LABELS + TRAINER_LABELS
                  + CHEATEDIT_LABELS)
    for lbl in all_labels:
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
    for lbl in all_labels:
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
        f"#define IGM_KEYS_NLINES {len(KEYS_LABELS)}",
        f"#define IGM_STATES_NLINES {len(STATES_LABELS)}",
        f"#define IGM_SAVES_NLINES {len(SAVES_LABELS)}",
        f"#define IGM_TAB_NLINES {len(TAB_LABELS)}",
        f"#define IGM_MANUAL_NLINES {len(MANUAL_LABELS)}",
        f"#define IGM_CHEATS_NLINES {len(CHEATS_LABELS)}",
        f"#define IGM_TRAINER_NLINES {len(TRAINER_LABELS)}",
        f"#define IGM_CHEATEDIT_NLINES {len(CHEATEDIT_LABELS)}",
        f"#define IGM_LANG_COUNT {nlang}",
        f"#define IGM_LANG_STRIDE {IGM_LANG_STRIDE}",
        f"#define IGM_LANG_SHIFT {IGM_LANG_SHIFT}",
        "",
    ]
    out += emit_table("igm_help_tbl", "igm_s", HELP_LABELS)
    out += emit_table("igm_keys_tbl", "igm_k", KEYS_LABELS)
    out += emit_table("igm_states_tbl", "igm_sts", STATES_LABELS)
    out += emit_table("igm_saves_tbl", "igm_sv", SAVES_LABELS)
    out += emit_table("igm_tab_tbl", "igm_tb", TAB_LABELS)
    out += emit_table("igm_manual_tbl", "igm_mn", MANUAL_LABELS)
    out += emit_table("igm_cheats_tbl", "igm_ch", CHEATS_LABELS)
    out += emit_table("igm_shell_tbl", "igm_sh", SHELL_LABELS)
    out += emit_table("igm_trainer_tbl", "igm_tr", TRAINER_LABELS)
    out += emit_table("igm_cheatedit_tbl", "igm_ce", CHEATEDIT_LABELS)

    out_path.write_text("\n".join(out) + "\n")
    print(f"generated {out_path}: HELP {len(HELP_LABELS)} + KEYS {len(KEYS_LABELS)} "
          f"+ STATES {len(STATES_LABELS)} "
          f"+ SAVES {len(SAVES_LABELS)} + TAB {len(TAB_LABELS)} + MANUAL {len(MANUAL_LABELS)} "
          f"+ CHEATS {len(CHEATS_LABELS)} + TRAINER {len(TRAINER_LABELS)} "
          f"lines x {nlang} langs ({', '.join(lang_order)})")


if __name__ == "__main__":
    main()
