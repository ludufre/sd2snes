#!/usr/bin/env python3
# pack_igmenu.py -- slice igmenu.bin (bank $C2) out of the sneslink -fsmc image and
# patch the crc16(body). Usage: pack_igmenu.py <raw.smc> <igmenu.bin>
#
# sneslink emits a HiROM smc (bank $C0 = file offset 0). The igmenu object is org'd
# `.link page $c2` so its bytes carry the "IGMN" header at address $C20000. We locate
# that magic (= the bank $C2 base) and slice from there so the shipped bin's offset 0
# maps to PSRAM $C20000 when the MCU loads it. The crc16 covers the body from offset 8
# (jml + code), matching src/igmenu.c (crc16_update, init 0xFFFF, xorout 0xFFFF).
import sys


def crc16_update(crc, b):
    # reflected 0xA001 step -- identical to src/crc16.c crc16_update()
    crc ^= b
    for _ in range(8):
        crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def main():
    raw = open(sys.argv[1], "rb").read()
    i = raw.find(b"IGMN")
    if i < 0:
        sys.exit("pack_igmenu: IGMN magic not found in " + sys.argv[1])
    data = bytearray(raw[i:])
    # strip trailing linker padding ($00/$FF) so the shipped bin + crc are compact and
    # deterministic. The strip is blind: it cannot tell padding from a real trailing $00 (it ATE
    # ig_thm_math_def's HDMA-table terminator once), so igmenu.a65 emits ig_end_marker (.byt $5a,
    # neither $00 nor $FF) as its LAST object -- the strip stops there. Lockstep with that label.
    end = len(data)
    while end > 12 and data[end - 1] in (0x00, 0xFF):
        end -= 1
    data = data[:end]
    if len(data) < 12:
        sys.exit("pack_igmenu: body too small (%d)" % len(data))

    # BASE/ENTRY guard: $C20008 must be `jmp igmenu_main` ($4C, 16-bit operand) and
    # igmenu_main must begin with `sep #$20` ($E2 $20). We interpret the jmp's 16-bit
    # target as a FILE offset (valid only if the object is based at $C20000) and confirm
    # it lands on `sep #$20`. This fails loudly if the linker did NOT base the $C2 object
    # at $C20000 (then every absolute reference in the bin would be off by the base and it
    # would crash the SNES in-game).
    if data[8] != 0x4C:
        sys.exit("pack_igmenu: no jmp ($4C) at offset 8 (got %02x)" % data[8])
    tgt = data[9] | (data[10] << 8)
    if tgt + 2 > len(data) or data[tgt] != 0xE2 or data[tgt + 1] != 0x20:
        sys.exit("pack_igmenu: entry/base mismatch -- jmp target (file off 0x%04x) is not "
                 "igmenu_main's `sep #$20`; the $C2 object is not based at $C20000." % tgt)

    crc = 0xFFFF
    for b in data[8:]:
        crc = crc16_update(crc, b)
    crc ^= 0xFFFF
    data[6] = crc & 0xFF
    data[7] = (crc >> 8) & 0xFF
    open(sys.argv[2], "wb").write(data)
    print("igmenu.bin: %d bytes, crc16=%04x" % (len(data), crc))


if __name__ == "__main__":
    main()
