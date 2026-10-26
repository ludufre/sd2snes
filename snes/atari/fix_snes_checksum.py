#!/usr/bin/env python3
"""Fill the SNES header checksum of a 32 KB LoROM image (in place)."""
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
assert len(d) == 0x8000, "expected 32768 bytes, got %d" % len(d)
d[0x7FDC:0x7FE0] = b"\xff\xff\x00\x00"
s = sum(d) & 0xFFFF
d[0x7FDC:0x7FDE] = (s ^ 0xFFFF).to_bytes(2, "little")
d[0x7FDE:0x7FE0] = s.to_bytes(2, "little")
open(p, "wb").write(d)
print("checksum %04X, complement %04X" % (s, s ^ 0xFFFF))
