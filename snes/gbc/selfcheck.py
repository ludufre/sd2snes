#!/usr/bin/env python3
"""Structural audit of the assembled GBC player (misc/gbc_snes.bin).

Two of the contract's invariants are cheap to state and expensive to notice the
loss of, because breaking either produces a picture that looks *nearly* right:

  sec. 13.4  stores into the $EF write window are 8 bits.  Data and strobe are
             decoded separately, so a 16-bit store also fires the neighbouring
             strobe -- a 16-bit write of the pad low byte would fire COMMIT a
             second time in the same frame, which the bridge is allowed to
             treat as undefined.

  sec. 13.12 the V counter behind every DMA guard is read as NINE bits.  An
             8-bit read folds V=256..261 onto V=0..5, i.e. hands a transfer
             that is about to run into the display a full window's worth of
             capacity, and VRAM writes past the deadline are dropped SILENTLY.

The player is written so both are checkable without a full disassembler: every
$EF store goes through the single site in GbcEfPut, and every V read goes
through GbcVCount.  This script asserts exactly that -- one $EF store site,
preceded by sep #$20, and no read of $213D that is not part of a latched
two-read pair.  It is a HEURISTIC over the byte image, not a proof: it finds
the byte patterns of the instructions it knows and reasons about their
immediate neighbourhood, so a store hidden behind computed addressing or a
V read spelled some other way would pass unnoticed.  What it does catch is the
regression that actually happens -- someone adding a second, convenient
`sta.l $EF0002` next to the code that needs it.

Usage:  selfcheck.py [--ef-base 0xEF0000] [-v] misc/gbc_snes.bin
"""

import argparse
import struct
import sys

SEP20 = b"\xe2\x20"  # sep #$20
REP20 = b"\xc2\x20"  # rep #$20
REP30 = b"\xc2\x30"  # rep #$30
LDA_2137 = b"\xad\x37\x21"  # lda.w $2137  (latch H/V)
LDA_213D = b"\xad\x3d\x21"  # lda.w $213D  (OPVCT, read twice)
LDA_213F = b"\xad\x3f\x21"  # lda.w $213F  (STAT78, resets the toggle)
AND_01 = b"\x29\x01"  # and.b #$01  (bit 8)

# Long-addressed stores.  These are the only opcodes that can name a bank
# directly; with DBR = 0 (which Reset establishes and nothing changes) no
# absolute or absolute-indexed store can reach $EF at all.
LONG_STORES = {0x8F: "sta.l", 0x9F: "sta.l ,x"}
BLOCK_MOVES = {0x54: "mvn", 0x44: "mvp"}


def find_all(hay, needle, start=0):
    out = []
    i = hay.find(needle, start)
    while i >= 0:
        out.append(i)
        i = hay.find(needle, i + 1)
    return out


def check_ef_stores(rom, ef_base, log):
    """One store site into the $EF window, and it is 8 bits wide."""
    problems = []
    sites = []
    ef_bank = (ef_base >> 16) & 0xFF
    # the six strobes at +00..+05, wire $03's C6_CTL/ROW_DONE at +06/+07 and
    # the counter mailbox at +10..+1B (C6MODE is its sixth word, +1A/+1B)
    lo, hi = ef_base & 0xFFFF, (ef_base & 0xFFFF) + 0x20
    for off in range(len(rom) - 3):
        op = rom[off]
        if op not in LONG_STORES or rom[off + 3] != ef_bank:
            continue
        addr = struct.unpack("<H", rom[off + 1:off + 3])[0]
        # sta.l base,x names the window base and indexes into it; sta.l names a
        # byte of the window outright.  Anything else in the same bank (the
        # harness puts its WRAM working set there) is not a window access.
        if op == 0x9F and addr != (ef_base & 0xFFFF):
            continue
        if op == 0x8F and not lo <= addr < hi:
            continue
        sites.append((off, LONG_STORES[op], addr))
    for off in range(len(rom) - 2):
        if rom[off] in BLOCK_MOVES and ef_bank in (rom[off + 1], rom[off + 2]):
            problems.append(
                "0x%04X: %s touches bank $%02X -- block moves are not 8-bit stores"
                % (off, BLOCK_MOVES[rom[off]], ef_bank)
            )

    if not sites:
        problems.append(
            "no store into the $%06X window found at all: either the write "
            "window moved or this is not the player image" % ef_base
        )
    if len(sites) > 1:
        problems.append(
            "%d store sites into the $%06X window; the player funnels every "
            "strobe through GbcEfPut so there must be exactly one (see sec. "
            "13.4 in the header comment of gbc_snes.asm)" % (len(sites), ef_base)
        )

    for off, mnem, addr in sites:
        log("  window store site at 0x%04X: %s $%02X%04X" % (off, mnem, ef_bank, addr))
        # The M flag has to be 1 (8-bit A) here.  Walk backwards over the
        # nearest flag-width instruction: the store is legal only if a sep #$20
        # is closer than any rep that widens A.
        window = rom[max(0, off - 48):off]
        last_sep = window.rfind(SEP20)
        last_rep = max(window.rfind(REP20), window.rfind(REP30))
        if last_sep < 0:
            problems.append(
                "0x%04X: no sep #$20 within 48 bytes ahead of the window store; "
                "A may be 16 bits" % off
            )
        elif last_rep > last_sep:
            problems.append(
                "0x%04X: the nearest width change ahead of the window store is "
                "a rep (A 16-bit), not a sep -- this store is two bytes wide "
                "and fires the neighbouring strobe" % off
            )
    return problems


def check_v_reads(rom, log):
    """Every read of the V counter is a latched, two-read, 9-bit read."""
    problems = []
    reads = find_all(rom, LDA_213D)
    if not reads:
        problems.append(
            "no read of $213D found: the DMA guard has no live V counter behind "
            "it, so every transfer is authorised blind"
        )
    log("  $213D reads: %d" % len(reads))

    used = set()
    pairs = 0
    for i, off in enumerate(reads):
        if off in used:
            continue
        # the second half of the pair must follow within a handful of bytes
        nxt = next((o for o in reads[i + 1:] if o - off <= 12), None)
        if nxt is None:
            problems.append(
                "0x%04X: read of $213D with no second read within 12 bytes -- an "
                "8-bit V read folds V=256..261 onto V=0..5 (sec. 13.12)" % off
            )
            continue
        used.add(off)
        used.add(nxt)
        pairs += 1
        pre = rom[max(0, off - 24):off]
        if pre.find(LDA_2137) < 0:
            problems.append(
                "0x%04X: the $213D pair is not preceded by a read of $2137 "
                "within 24 bytes; without the latch $213D returns a stale "
                "scanline forever" % off
            )
        if pre.find(LDA_213F) < 0:
            problems.append(
                "0x%04X: no read of $213F ahead of the $213D pair; the "
                "low/high toggle is then in an unknown state and the two reads "
                "may come back swapped" % off
            )
        tail = rom[nxt + 3:nxt + 9]
        if tail.find(AND_01) < 0:
            problems.append(
                "0x%04X: the second $213D read is not masked with #$01 -- bits "
                "1..7 of OPVCT's high read are open bus" % nxt
            )
    log("  latched 9-bit V read pairs: %d" % pairs)
    return problems


def check_image(rom, log):
    problems = []
    if len(rom) > 32768:
        problems.append("image is %d bytes; the player must stay in one LoROM bank" % len(rom))
    if len(rom) < 0x8000:
        problems.append("image is %d bytes: too short to carry a header" % len(rom))
        return problems
    hdr = 0x7FC0
    title = rom[hdr:hdr + 21]
    log("  title  %r" % title)
    log("  mapmode $%02X  carttype $%02X  romsize $%02X  ramsize $%02X"
        % (rom[hdr + 0x15], rom[hdr + 0x16], rom[hdr + 0x17], rom[hdr + 0x18]))
    if rom[hdr + 0x15] != 0x20:
        problems.append("map mode is $%02X, not $20 (LoROM): smc_id will not "
                        "detect the player" % rom[hdr + 0x15])
    if rom[hdr + 0x18] != 0x00:
        problems.append("the header declares SaveRAM ($%02X); sgb_update_romprops "
                        "refuses a player image that does" % rom[hdr + 0x18])
    nmi = struct.unpack("<H", rom[0x7FEA:0x7FEC])[0]
    res = struct.unpack("<H", rom[0x7FFC:0x7FFE])[0]
    log("  NMI $%04X  RESET $%04X" % (nmi, res))
    for name, vec in (("NMI", nmi), ("RESET", res)):
        if not 0x8000 <= vec <= 0xFFFF:
            problems.append("%s vector $%04X is outside the ROM half of the bank" % (name, vec))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--ef-base", default="0xEF0000",
                    help="24-bit base of the write window ($EF0000 on the "
                         "device, $7E1000 in the !GBC_HARNESS = 1 build)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    ef_base = int(args.ef_base, 0)
    rom = open(args.rom, "rb").read()

    def log(msg):
        if args.verbose:
            print(msg)

    problems = []
    log("image:")
    problems += check_image(rom, log)
    log("$EF write window (base $%06X):" % ef_base)
    problems += check_ef_stores(rom, ef_base, log)
    log("V counter:")
    problems += check_v_reads(rom, log)

    if problems:
        print("FAIL: %s" % args.rom)
        for p in problems:
            print("  - %s" % p)
        return 1
    print("OK: %s (%d bytes)" % (args.rom, len(rom)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
