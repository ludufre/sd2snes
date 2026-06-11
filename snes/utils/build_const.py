#!/usr/bin/env python3
"""Generate a trilingual const_lang.a65 from the English base snes/const.a65.

The menu renders strings by label. To support a runtime language switch in a
single binary, every *localized* label (one that appears in lang_ptbr / lang_es)
is expanded into:

    <label>_en   .byt <english>, 0
    <label>_ptbr .byt <portuguese>, 0
    <label>_es   .byt <spanish>, 0
    <label>:                              ; 3-entry dispatch table (the public label)
      .word !<label>_en   : .byt ^<label>_en
      .word !<label>_ptbr : .byt ^<label>_ptbr
      .word !<label>_es   : .byt ^<label>_es

All dispatch tables are emitted contiguously between the exported labels
`strtab_lo` and `strtab_hi`. The menu's `resolve_str` recognises a dispatch
table purely by address range: a pointer inside [strtab_lo, strtab_hi) is
indexed by the active language (cur_lang * 3); any other pointer (a plain,
language-neutral string such as "50Hz") passes through unchanged. This keeps
menudata.a65 and every existing `!label`/`^label` reference untouched.

Non-localized lines (data tables, window geometry, neutral strings) are copied
verbatim and keep their original positions.

Usage:
    build_const.py <const.a65> <lang_ptbr.py> <lang_es.py> -o <const_lang.a65>
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
}
DECODE = {v: k for k, v in ACCENTS.items()}

# `LABEL  .byt  <args>` (args may contain quoted strings and raw byte values).
LINE_RE = re.compile(r'^(\s*)(\S+)(\s+\.byt\s+)(.*)$')


def encode_string(text):
    """UTF-8 text (with {NNN} raw-byte placeholders) -> `.byt` argument string."""
    pieces, cur, i = [], "", 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            end = text.index("}", i)
            if cur:
                pieces.append(f'"{cur}"'); cur = ""
            pieces.append(text[i + 1:end])
            i = end + 1
            continue
        if ch in ACCENTS:
            if cur:
                pieces.append(f'"{cur}"'); cur = ""
            pieces.append(str(ACCENTS[ch]))
        else:
            cur += ch
        i += 1
    if cur:
        pieces.append(f'"{cur}"')
    pieces.append("0")
    return ", ".join(pieces)


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
    out = ['"""Spanish strings for the sd2snes menu. Translate each value.',
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


def main():
    base = Path(sys.argv[1])

    if "--dump-en" in sys.argv:
        # build_const.py <const.a65> <lang_ref.py> --dump-en <out.py>
        ref = load_dict(sys.argv[2])
        out_py = sys.argv[sys.argv.index("--dump-en") + 1]
        dump_en_scaffold(base, out_py, list(ref.keys()))
        return

    ptbr = load_dict(sys.argv[2])
    # es (2nd language) is OPTIONAL: omit it to build a smaller EN+PT menu. When
    # absent, es={} -> every es.get() is None -> es_used False -> nlang drops to 2
    # and the es column/strings cost nothing (the trilingual table overflowed C0).
    es = load_dict(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].endswith(".py") else {}
    out_path = Path(sys.argv[sys.argv.index("-o") + 1])

    # Item descriptions (mdesc_*) are not rendered by the current menu (no menu
    # entry offset-11 read exists), so we do NOT expand them into trilingual
    # dispatch tables -- they stay as their single English base string. This
    # keeps the trilingual menu inside one 64K ROM bank. (Their pt-BR/es text
    # still lives in lang_*.py; drop this skip if descriptions become rendered.)
    SKIP_PREFIXES = ("mdesc_",)
    localized = {k for k in (set(ptbr) | set(es))
                 if not k.startswith(SKIP_PREFIXES)}
    lines, en_args = parse_base(base)

    out, order = [], []
    for line in lines:
        m = LINE_RE.match(line)
        if m and m.group(2) in localized:
            order.append(m.group(2))            # moved to the localized block below
        else:
            out.append(line)

    # A dict key with no matching label means a label was renamed/removed in
    # const.a65 without updating the dicts -- from that point on the menu would
    # silently ship the new label in English. Fail the build instead.
    missing = sorted(l for l in localized if l not in en_args)
    if missing:
        sys.exit(f"build_const.py: {len(missing)} translated label(s) not found "
                 f"in {base}: {', '.join(missing)}\n"
                 f"(label renamed/removed in const.a65? update lang_ptbr.py / "
                 f"lang_es.py to match, or the translation silently ships as English)")

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
    WIDTH_LIMITS = (("text_no_", 22), ("cheat_tab_head", 48), ("mtext_", 40))
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
    for lang_name, d in (("pt-BR", ptbr), ("es", es)):
        for label in order:
            text = d.get(label)
            if not text:
                continue
            n, lim = encoded_len(text), budget_for(label)
            if n > lim:
                too_wide.append(f"{label} [{lang_name}]: {n} > {lim} bytes: {text!r}")
    if too_wide:
        sys.exit("build_const.py: translation(s) exceed their UI slot:\n  "
                 + "\n  ".join(too_wide))

    # Intern identical strings so a label whose translations coincide (e.g. an
    # untranslated 'es' that falls back to English) stores each unique byte
    # sequence only once. This keeps the trilingual menu inside one 64K bank.
    pool = {}        # `.byt` args -> shared label
    pool_order = []  # preserve emission order

    def intern(args):
        if args not in pool:
            pool[args] = f"strpool_{len(pool)}"
            pool_order.append(args)
        return pool[args]

    tabledefs = []
    plaindefs = []   # labels whose 3 languages coincide -> plain string, no table
    for label in order:
        en = en_args[label]
        pt_text = ptbr.get(label)
        es_text = es.get(label)
        # Language-neutral? Compare via normalized decode so a translation that
        # merely repeats the English (or an untranslated 'es' that falls back to
        # English) collapses to a single plain string with NO 6-byte dispatch
        # table -- resolve_str passes any pointer outside [strtab_lo,strtab_hi)
        # straight through. This keeps the trilingual menu inside one 64K bank.
        en_norm = encode_string(decode_args(en))
        pt_norm = encode_string(pt_text) if pt_text else en_norm
        es_norm = encode_string(es_text) if es_text else en_norm
        if en_norm == pt_norm == es_norm:
            plaindefs.append((label, en))
            continue
        en_lbl = intern(en)
        pt_lbl = intern(encode_string(pt_text) if pt_text else en)
        es_lbl = intern(encode_string(es_text) if es_text else en)
        tabledefs.append((label, en_lbl, pt_lbl, es_lbl))

    if plaindefs:
        out += ["", "; ==== language-neutral labels (same in all 3 langs): no table ===="]
        for label, args in plaindefs:
            out.append(f"{label} .byt {args}")

    out += ["", "; ==== interned language string pool (deduplicated) ===="]
    for args in pool_order:
        out.append(f"{pool[args]} .byt {args}")

    # Number of language COLUMNS actually present. Trailing columns whose every
    # entry just repeats English (e.g. an unfilled lang_es scaffold) are dropped
    # so they cost no table space; resolve_str maps cur_lang >= strtab_nlang back
    # to English (column 0). Order is fixed EN(0), pt-BR(1), es(2): a column is
    # kept only if it (or a later one) carries a real translation.
    def differs(text, lbl):
        return bool(text) and (encode_string(text)
                               != encode_string(decode_args(en_args.get(lbl, ""))))
    es_used = any(differs(es.get(l), l) for l in order)
    pt_used = es_used or any(differs(ptbr.get(l), l) for l in order)
    nlang = 3 if es_used else (2 if pt_used else 1)

    out += ["", f"; ==== dispatch tables: resolve_str range [strtab_lo, strtab_hi) ===="]
    out += [f"; each table = {nlang} x 16-bit address (EN, pt-BR, es)[:{nlang}]; NO bank byte:",
            "; every pooled string lives in the same bank as strtab_lo, so resolve_str",
            "; uses ^strtab_lo as the bank for all of them. cur_lang >= strtab_nlang -> EN."]
    out.append(f"strtab_nlang .byt {nlang}")
    out.append("strtab_lo")
    for label, en_lbl, pt_lbl, es_lbl in tabledefs:
        cols = (en_lbl, pt_lbl, es_lbl)[:nlang]
        # `label .word ...` (no colon) matches the proven `label .byt ...` style.
        out.append(f"{label} " + " : ".join(f".word !{c}" for c in cols))
    out.append("strtab_hi")

    out_path.write_text("\n".join(out) + "\n")
    print(f"generated {out_path}: {len(order)} localized labels, {nlang} language column(s)")


if __name__ == "__main__":
    main()
