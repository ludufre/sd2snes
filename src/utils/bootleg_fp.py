#!/usr/bin/env python3
"""Print bootleg.c table rows (with the 64 KB fingerprint filled in) for ROM files,
and optionally repair "doubled" overdumps.

usage: bootleg_fp.py [--fix OUTDIR] rom1.sfc [rom2.smc ...]

Strips a 512-byte copier header, computes the CRC32 of the whole image and of its
first 64 KB, and prints a ready-to-paste row for every file whose CRC is one of the
known protected bootlegs.

Some circulating dumps (e.g. Picachu, Pokemon Stadium, Tekken 2) are 8 MB files:
the 2 MB LoROM image with every 32 KB bank stored twice (4 MB), mirrored to 8 MB.
The firmware only recognises the clean image, so such files are reported and, with
--fix OUTDIR, written out as the clean image (same file name).
"""
import os, sys, zlib

KNOWN = {
    0x014F0FCF: ("BOOTLEG_PORT6   ", "A Bug's Life"),
    0x752A25D3: ("BOOTLEG_BITSWAP ", "Aladdin 2000"),
    0x52B0D84B: ("BOOTLEG_PORT6   ", "Bananas de Pijamas"),
    0x4F660972: ("BOOTLEG_BITSWAP ", "Digimon Adventure"),
    0xA7813943: ("BOOTLEG_BITSWAP ", "King of Fighters 2000"),
    0x892C6765: ("BOOTLEG_BITSWAP ", "Pocket Monster (Picachu)"),
    0x7C0B798D: ("BOOTLEG_BITSWAP ", "Pokemon Gold Silver"),
    0xF863C642: ("BOOTLEG_BITSWAP ", "Pokemon Stadium"),
    0x5E4ADA04: ("BOOTLEG_BITSWAP ", "Soul Edge Vs Samurai"),
    0xDAD59B9F: ("BOOTLEG_ALU     ", "Street Fighter EX Plus Alpha"),
    0x40242231: ("BOOTLEG_BITSWAP ", "X-Men vs. Street Fighter"),
    0xBAD1D9B8: ("BOOTLEG_BITSWAP ", "Squirrel"),
    0xCDB590E4: ("BOOTLEG_BITSWAP40", "Marvel Super Heroes vs Street Fighter"),
    0x6C303FC9: ("BOOTLEG_KOF98   ", "King of Fighters '98"),
    0xC97D1D7B: ("BOOTLEG_CONSTANT", "Soul Blade"),
    0x45874D3D: ("BOOTLEG_CONSTANT", "Hercules"),
    0x5BBA4EB3: ("BOOTLEG_CONSTANT", "Dragon Ball Z - Final Bout"),
    0xDD7AFCB9: ("BOOTLEG_CONSTANT", "Dragon Ball Z - Final Bout (sound restored)"),
    0x51EEB811: ("BOOTLEG_CONSTANT", "Dragon Ball Z - Final Bout (old partial repair)"),
    0x066687CA: ("BOOTLEG_ALU     ", "Tekken 2"),
}

def crc(b): return zlib.crc32(b) & 0xffffffff

def undouble(data):
    """Return the clean image if data is a known image with doubled 32 KB banks
    (optionally mirrored to a larger power of two), else None."""
    for size in (0x100000, 0x200000, 0x300000, 0x400000):
        span = size * 2
        if len(data) < span or len(data) % span:
            continue
        if any(data[i:i + span] != data[:span] for i in range(0, len(data), span)):
            continue
        banks = [data[i:i + 0x8000] for i in range(0, span, 0x8000)]
        if all(banks[i] == banks[i + 1] for i in range(0, len(banks), 2)):
            clean = b"".join(banks[0::2])
            if crc(clean) in KNOWN:
                return clean
    return None

args = sys.argv[1:]
outdir = None
if args[:1] == ["--fix"]:
    outdir, args = args[1], args[2:]
    os.makedirs(outdir, exist_ok=True)

for path in args:
    data = open(path, "rb").read()
    if len(data) % 1024 == 512:
        data = data[512:]
    fixed = False
    if crc(data) not in KNOWN:
        clean = undouble(data)
        if clean is not None:
            print("# %s: doubled-bank overdump (%d KB) -> %d KB clean image%s" % (
                os.path.basename(path), len(data) // 1024, len(clean) // 1024,
                "" if outdir else " (use --fix OUTDIR to write it)"), file=sys.stderr)
            data, fixed = clean, True
    c = crc(data)
    if c in KNOWN:
        var, name = KNOWN[c]
        print("  { 0x%08X, 0x%08X, 0x%06X, %s },  /* %s */" % (c, crc(data[:0x10000]), len(data), var, name))
        if fixed and outdir:
            open(os.path.join(outdir, os.path.basename(path)), "wb").write(data)
    else:
        print("# %s: crc32=%08X size=%06X -- not a known protected bootleg" % (path, c, len(data)), file=sys.stderr)
