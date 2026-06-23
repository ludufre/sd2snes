#!/usr/bin/env python3
"""Deterministic corpus of valid + malformed IPS/BPS patches for the host
harness. Every malformed file models a real way an SD-card patch can be bad
(truncation, absurd sizes, out-of-range references) -- the patcher must fail
CLEAN on all of them: no hang, no write outside the ROM window.
"""
import sys
import zlib
from pathlib import Path


def vli(n):
    """beat/BPS variable-length integer (matches bps_decode_vli)."""
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(x | 0x80)
            return bytes(out)
        out.append(x)
        n -= 1


def svli(n):
    """signed delta: (abs << 1) | negative-bit."""
    return vli((abs(n) << 1) | (1 if n < 0 else 0))


def bps(source_size, target_size, actions, footer=True, src=b"", tgt=b""):
    body = b"BPS1" + vli(source_size) + vli(target_size) + vli(0) + actions
    if footer:
        body += zlib.crc32(src).to_bytes(4, "little")
        body += zlib.crc32(tgt).to_bytes(4, "little")
        body += zlib.crc32(body).to_bytes(4, "little")
    return body


def a_sourceread(ln):        return vli(((ln - 1) << 2) | 0)
def a_targetread(data):      return vli(((len(data) - 1) << 2) | 1) + data
def a_sourcecopy(ln, delta): return vli(((ln - 1) << 2) | 2) + svli(delta)
def a_targetcopy(ln, delta): return vli(((ln - 1) << 2) | 3) + svli(delta)


def main(outdir):
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)

    rom = bytes((i * 7 + 13) & 0xFF for i in range(4096))
    (out / "rom4k.bin").write_bytes(rom)
    # big enough that rom_base + target_size + len(rom) crosses SRAM_SAVE_ADDR
    rom7m = bytes(1024) * (7 * 1024)
    (out / "rom7m.bin").write_bytes(rom7m)

    # -- valid BPS exercising all four actions (incl. the dist==1 RLE path) --
    # target_rel / source_rel are CUMULATIVE across actions (deltas).
    tgt = bytearray()
    acts = bytearray()
    acts += a_sourceread(16);            tgt += rom[0:16]   # out 16
    acts += a_targetread(b"01234567");   tgt += b"01234567" # out 24
    acts += a_sourcecopy(8, 4);          tgt += rom[4:12]   # src_rel 4, out 32
    acts += a_targetcopy(8, 0);          tgt += tgt[0:8]    # tgt_rel 0->8, dist 32
    acts += a_targetcopy(16, 31)                            # tgt_rel 8+31=39 ==
    tgt += bytes([tgt[39]]) * 16                            # out(40)-1 -> RLE fill
    tgt = bytes(tgt)
    (out / "ok-small.bps").write_bytes(
        bps(len(rom), len(tgt), bytes(acts), src=rom, tgt=tgt))
    (out / "ok-small.bps.target").write_bytes(tgt)

    # -- malformed BPS --------------------------------------------------------
    # < 12 bytes: action_end = fsize - 12 underflows; header VLI hits EOF
    (out / "tiny-truncated.bps").write_bytes(b"BPS1\x00")
    # an action VLI starts inside the action region and runs into EOF
    (out / "vli-straddles-eof.bps").write_bytes(
        bps(len(rom), len(rom), b"\x00", footer=False) + b"\x00" * 12)
    # absurd declared target (16 MB) -- must be rejected before any write
    (out / "huge-target.bps").write_bytes(
        bps(len(rom), 0x1000000, a_sourceread(16), src=rom, tgt=b""))
    # target passes the 8 MB cap but rom7m source backup would cross SaveRAM
    (out / "overflow-window.bps").write_bytes(
        bps(len(rom7m), 0x800000, a_sourceread(16), src=rom7m, tgt=b""))
    # writes 32 bytes into a declared 16-byte target
    (out / "target-overrun.bps").write_bytes(
        bps(len(rom), 16, a_targetread(b"X" * 32), src=rom, tgt=b""))
    # SourceCopy reaches past source_size
    (out / "source-oob.bps").write_bytes(
        bps(len(rom), 64, a_sourcecopy(32, len(rom) - 8), src=rom, tgt=b""))

    # -- valid IPS (offsets >= 512 so the copier-header heuristics stay off) --
    recs = bytearray()
    recs += (0x000200).to_bytes(3, "big") + (16).to_bytes(2, "big") + b"ABCDEFGHIJKLMNOP"
    recs += (0x000300).to_bytes(3, "big") + (0).to_bytes(2, "big") \
            + (32).to_bytes(2, "big") + b"\xEE"                      # RLE
    itgt = bytearray(rom)
    itgt[0x200:0x210] = b"ABCDEFGHIJKLMNOP"
    itgt[0x300:0x320] = b"\xEE" * 32
    (out / "ok-small.ips").write_bytes(b"PATCH" + recs + b"EOF")
    (out / "ok-small.ips.target").write_bytes(bytes(itgt))

    # -- malformed IPS --------------------------------------------------------
    # record far beyond the ROM region (and beyond SRAM_SAVE_ADDR)
    (out / "oob-offset.ips").write_bytes(
        b"PATCH" + (0xF00000).to_bytes(3, "big") + (16).to_bytes(2, "big")
        + b"Z" * 16 + b"EOF")
    # truncated at a record boundary: no EOF marker
    (out / "truncated-noeof.ips").write_bytes(b"PATCH" + bytes(recs))

    print(f"corpus written to {out}/")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "corpus")
