#!/usr/bin/env python3
"""Per-bank tail check for the menu image.

The sneslink size assert in the Makefile only bounds the WHOLE file, so once
MENU_SIZE grew past one bank it stopped protecting the individual 64 KB pages:
a full $C1 would silently push nothing anywhere -- the link just fails with a
"Page C1 doesn't have ..." that the ERROR grep does catch, but nobody sees how
CLOSE a bank is until it bursts. This prints the free tail of every bank on
every build and fails when one drops below the floor, so the headroom is a
number in the build log instead of archaeology over bin/m3nu.bin.

Tail = trailing run of 0x00/0xFF filler. Bank $C0 is measured up to $FF00 (the
fixed header.ips block owns $C0FF00-$C0FFFF).
"""
import sys
from pathlib import Path

# Per-bank floors. $C0 is structurally tight (its code is a near-jsr web that
# cannot move banks cheaply) and has lived around ~100 bytes for a while -- the
# floor there only catches an actual overflow-in-progress. New tables/strings
# belong in $C1/$C2, which is what the higher floors defend.
FLOORS = {0: 16}
FLOOR_DEFAULT = 256

def tail_free(chunk: bytes) -> int:
    i = len(chunk)
    while i > 0 and chunk[i - 1] in (0x00, 0xFF):
        i -= 1
    return len(chunk) - i

def main() -> int:
    img = Path(sys.argv[1]).read_bytes()
    size = int(sys.argv[2])
    nbanks = size // 0x10000
    bad = []
    for b in range(nbanks):
        chunk = img[b * 0x10000:(b + 1) * 0x10000]
        if b == 0:
            chunk = chunk[:0xFF00]  # header.ips owns $C0FF00+
        free = tail_free(chunk)
        floor = FLOORS.get(b, FLOOR_DEFAULT)
        print(f"  bank $C{b:X}: {free} bytes free")
        if free < floor:
            bad.append((b, free, floor))
    if bad:
        for b, free, floor in bad:
            print(f"*** bank $C{b:X} nearly full ({free} < {floor} bytes free) -- "
                  f"move objects/tables to a roomier bank ($C1/$C2 have the headroom)")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
