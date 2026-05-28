#!/usr/bin/env python3
"""
Binary-patch the MCU firmware (firmware.img / firmware.im3 / firmware.stm)
to replace English user-facing strings with Brazilian Portuguese.

The MCU firmware in this repo is not rebuildable without Xilinx ISE
(needs fpga_mini.bit). We can still patch the prebuilt binaries.

Constraints:
  - Replacement must fit in the original byte slot (including NUL terminator).
  - For sysinfo strings (rendered by the SNES menu font via hiprint), accented
    bytes 0x82..0x99 are valid:
        a 0x82  agrave 0x83  acirc 0x84  atilde 0x85
        e 0x86  ecirc  0x87
        i 0x88
        o 0x89  ocirc  0x8a  otilde 0x8b
        u 0x8c  cedilla 0x8d
        A.. 0x8e-0x91  E 0x92 0x93  I 0x94  O 0x95-0x97  U 0x98  C 0x99
  - For boot screen strings (rendered by the FPGA loader font), use ASCII only:
    that font may not contain the accent slots, so use plain letters.

Length math is on the BYTES of the literal in the binary (each accent = 1 byte).
Format specifiers (%s, %d, %02x, %ld, etc.) must be preserved EXACTLY.
"""

import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
RELEASE = REPO / "release" / "v1.11.2" / "sd2snes"

# Accented bytes
a_a, a_A = b"\x82", b"\x8e"      # á Á
a_g, a_G = b"\x83", b"\x8f"      # à À
c_a, c_A = b"\x84", b"\x90"      # â Â
t_a, t_A = b"\x85", b"\x91"      # ã Ã
a_e, a_E = b"\x86", b"\x92"      # é É
c_e, c_E = b"\x87", b"\x93"      # ê Ê
a_i, a_I = b"\x88", b"\x94"      # í Í
a_o, a_O = b"\x89", b"\x95"      # ó Ó
c_o, c_O = b"\x8a", b"\x96"      # ô Ô
t_o, t_O = b"\x8b", b"\x97"      # õ Õ
a_u, a_U = b"\x8c", b"\x98"      # ú Ú
ced, CED = b"\x8d", b"\x99"      # ç Ç


def L(s):
    """Compute length of a translation including accent bytes."""
    return len(s.encode("latin-1")) if isinstance(s, str) else len(s)


# (english_literal, portuguese_literal)
# Each pair must satisfy len(pt) <= len(en) (we NUL-pad the trailing bytes).
TRANSLATIONS = [
    # ------ Boot screen (ASCII only — boot font may lack accents) ------
    (b"No SD Card found!",            b"Sem cartao SD!"),
    (b"Please insert SD Card and",    b"Insira o cartao SD e"),
    (b"make sure it is seated",       b"verifique se esta bem"),
    (b"properly.",                    b"encaixado"),
    (b"SD Card inserted!",            b"SD inserido!"),
    (b"Could not load menu ROM!",     b"Falha ao carregar menu!"),
    (b"Error: %s",                    b"Erro: %s"),
    (b"Check that your card is wor-", b"Verifique se cartao esta"),
    (b"king, formatted correctly",    b"formatado corretamente"),
    (b"(MBR+FAT32), and that the",    b"(MBR+FAT32) e que o"),
    (b"exists.",                      b"existe."),
    (b"Loading ...",                  b"Carregando."),
    (b"Loading SGB",                  b"Carreg. SGB"),

    # ------ Sysinfo (rendered by SNES menu font — accents OK) ------
    (b"    Firmware version: %s",
     b"    Vers" + t_a + b"o firmware: %s"),

    (b"    *** SD Card removed/USB busy ***    ",
     b"    *** SD removido/USB ocupado ***    "),

    (b'SD Maker/OEM:    0x%02x, "%c%c"',
     b'Fabricante SD:   0x%02x, "%c%c"'),

    (b'SD Product Name: "%c%c%c%c%c", Rev. %d.%d',
     b'Nome do produto: "%c%c%c%c%c", Rev. %d.%d'),

    (b"SD Serial No.:   %02x%02x%02x%02x, Mfd. %d/%02d",
     b"Serial SD:       %02x%02x%02x%02x, Fab. %d/%02d"),

    (b"SD acc. time: %ld.%03ld / %ld.%03ld ms avg/max",
     b"Acesso SD:    %ld.%03ld / %ld.%03ld ms med/max"),

    (b"Calculating disk space\x7f\x80                ",
     b"Calculando espa" + ced + b"o em disco\x7f\x80            "),

    (b"SD acc. time: measuring\x7f\x80  ",
     b"Acesso SD:    medindo\x7f\x80    "),

    (b"Card usage: %ldMB / %ldMB",
     b"Uso do SD:  %ldMB / %ldMB"),

    (b"CIC state: %s",
     b"Modo CIC: %s"),

    (b"SNES master clock: measuring\x7f\x80",
     b"Clock SNES:           medindo"),

    (b"SNES master clock: %ldHz    ",
     b"Clock SNES:         %ldHz   "),

    # SGB BIOS state words (used inside sysinfo line)
    (b"missing",  b"ausente"),
    (b"mismatch", b"errado  "[:8]),
    (b"checking", b"lendo   "[:8]),

    # CIC state names (used by "CIC state: %s")
    (b"Original or no CIC",          b"Original ou sem"),
    (b"Original CIC (failed)",       b"CIC original (falhou)"),
    (b"SuperCIC enhanced",           b"SuperCIC ampliado"),
    (b"SuperCIC detected, not used", b"SuperCIC detect. n/usado"[:27]),

    # FRESULT friendly names (used by error reports, e.g. boot "Error: %s")
    (b"No error",                  b"Sem erro"),
    (b"Card I/O error",            b"Erro de I/O"),
    (b"Internal FS driver error",  b"Erro interno FS"),
    (b"Drive not ready",           b"Drive nao pronto"[:15]),
    (b"File not found",            b"Arq. nao achado"[:14]),
    (b"Directory not found",       b"Pasta nao achada"[:19]),
    (b"Invalid path name",         b"Caminho invalido"[:17]),
    (b"Access denied",             b"Acesso negado"),
    (b"Access denied (exists)",    b"Negado (ja existe)"[:22]),
    (b"Invalid file object",       b"Arq. invalido"[:19]),
    (b"Write protected",           b"Protegido"[:15]),
    (b"Invalid drive specified",   b"Drive invalido"[:23]),
    (b"No work area",              b"Sem mem. trab."[:12]),
    (b"Not a valid file system",   b"FS invalido"[:23]),
    (b"mkfs() aborted",            b"mkfs() abortou"),
    (b"Drive access timeout",      b"Timeout no SD"[:20]),
    (b"Shared access locked",      b"Acesso bloqueado"[:20]),
    (b"Not enough memory",         b"Sem memoria"[:17]),
    (b"Too many open files",       b"Muitos arq. abertos"),
    (b"Invalid parameter",         b"Param. invalido"[:17]),
]


HEADER_SIZE = 256  # CONFIG_FW_HEADERSIZE in src/config-mk*; same on all variants

# Bootloader (src/bootldr/iap.c:133) skips flashing when the version field in
# the file equals the version already flashed. Derive version from the payload
# CRC so any translation change produces a fresh version and forces a re-flash.
VERSION_PREFIX = b"ptbr-"


def fix_fw_header(data):
    """Recompute CRC32 + bump version so bootloader accepts the patched fw."""
    magic, _ver, size, _crc, _crcc = struct.unpack("<IIIII", data[:20])
    payload = bytes(data[HEADER_SIZE:HEADER_SIZE + size])
    new_crc = zlib.crc32(payload) & 0xFFFFFFFF
    new_crcc = new_crc ^ 0xFFFFFFFF
    new_version = zlib.crc32(VERSION_PREFIX + struct.pack("<I", new_crc)) & 0xFFFFFFFF
    data[4:8] = struct.pack("<I", new_version)
    data[12:16] = struct.pack("<I", new_crc)
    data[16:20] = struct.pack("<I", new_crcc)
    return new_version, new_crc


def patch_file(path):
    data = bytearray(path.read_bytes())
    hits = []
    misses = []
    for en, pt in TRANSLATIONS:
        if len(pt) > len(en):
            print(f"SKIP (too long): {en!r} -> {pt!r}  ({len(pt)} > {len(en)})")
            misses.append(en)
            continue
        # Match either as a NUL-terminated C string or just the literal.
        target = en + b"\x00"
        replacement = pt + b"\x00" * (len(en) - len(pt) + 1)
        idx = data.find(target)
        if idx < 0:
            misses.append(en)
            continue
        # Replace all occurrences.
        while idx >= 0:
            data[idx:idx + len(target)] = replacement
            hits.append((idx, en, pt))
            idx = data.find(target, idx + len(replacement))
    if hits:
        new_ver, new_crc = fix_fw_header(data)
        print(f"  version={new_ver:08x}  crc={new_crc:08x}")
    path.write_bytes(bytes(data))
    return hits, misses


def main():
    targets = ["firmware.img", "firmware.im3", "firmware.stm"]
    any_failed = False
    for name in targets:
        p = RELEASE / name
        if not p.exists():
            print(f"SKIP missing: {p}")
            continue
        hits, misses = patch_file(p)
        print(f"{name}: {len(hits)} strings patched, {len(misses)} not found.")
        for en in misses[:8]:
            print(f"  miss: {en[:40]!r}")
        if not hits:
            any_failed = True
    return 1 if any_failed else 0


if __name__ == "__main__":
    sys.exit(main())
