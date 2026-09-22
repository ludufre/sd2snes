#!/usr/bin/env python3
"""Restore the missing sound banks of the Dragon Ball Z - Final Bout (DVS) dump.

usage: repair_dbz_sound.py DBZ_DUMP SF2_DUMP OUT

DBZ_DUMP : Dragon Ball Z - Final Bout (pirate), 2 MB (CRC 5BBA4EB3), or the 4 MB
           doubled-bank overdump of it (undoubled automatically).
SF2_DUMP : Street Fighter II (Japan), 2 MB (CRC 5556C5C9).

What happened: DVS copied Street Fighter II's ROM banks $0A-$0F (sound driver,
instruments, songs, voice samples) verbatim into DBZ's banks $05-$0A and rebased the
upload table at $05:8000 by -5 banks.  The known DBZ dump lost banks $07-$0A
(ROM $038000-$057FFF); they are SF II's $060000-$07FFFF.

Checks before anything is written:
  * both input CRCs
  * DBZ's intact copy ($0280E4-$037FFF) equals SF II at +$28000 byte for byte
  * DBZ's table equals SF II's with every bank byte -5
  * every one of the 76 entries is a valid upload chain in SF II ending exactly on
    the boundary DBZ's own table gives
  * the copy ends at the bank boundary (DBZ bank $0B is not SF II data)
Output CRC32: DD7AFCB9 (the firmware table knows it).
"""
import sys, zlib

B = 0x8000
OFF = 0x28000                       # DBZ ROM offset + OFF = SF II ROM offset
HOLE = (0x38000, 0x58000)

def crc(b): return zlib.crc32(b) & 0xffffffff

def load_dbz(path):
    d = open(path, 'rb').read()
    if len(d) % 1024 == 512: d = d[512:]
    if len(d) == 0x400000:
        banks = [d[i:i + B] for i in range(0, len(d), B)]
        if all(banks[i] == banks[i + 1] for i in range(0, len(banks), 2)):
            d = b''.join(banks[0::2])
    if crc(d) != 0x5BBA4EB3:
        sys.exit('DBZ dump: CRC %08X, want 5BBA4EB3 (the protected dump)' % crc(d))
    return bytearray(d)

def rom(b, a): return ((b & 0x7f) << 15) | (a & 0x7fff)

def chain_end(buf, o):
    for _ in range(16):
        ln = buf[o] | buf[o + 1] << 8
        if ln == 0: return o + 4
        o += 4 + ln
    return None

def main(dbz_path, sf2_path, out_path):
    d = load_dbz(dbz_path)
    s = open(sf2_path, 'rb').read()
    if len(s) % 1024 == 512: s = s[512:]
    if crc(s) != 0x5556C5C9:
        sys.exit('SF II dump: CRC %08X, want 5556C5C9 (Street Fighter II (Japan))' % crc(s))

    t = d[0x28000:0x280e4]; st = s[0x28000 + OFF:0x280e4 + OFF]
    ent = [rom(t[3*i+2], t[3*i] | t[3*i+1] << 8) if any(t[3*i:3*i+3]) else None for i in range(76)]
    assert d[0x280e4:HOLE[0]] == s[0x280e4 + OFF:HOLE[0] + OFF], 'intact sound data differs from SF II'
    assert all(t[3*i:3*i+2] == st[3*i:3*i+2] and (st[3*i+2] - t[3*i+2]) == 5
               for i in range(76) if any(t[3*i:3*i+3])), 'table is not SF II\'s rebased by 5 banks'
    assert not any(d[HOLE[0]:HOLE[1]]), 'hole is not blank - not the expected dump?'
    for i in range(76):
        if ent[i] is None: continue
        e = chain_end(s, ent[i] + OFF)
        nxt = ent[i + 1] if i < 75 else None
        assert e is not None and (nxt is None or (nxt + OFF) - e in (0, 1)), 'entry #%d' % i
    same_after = sum(1 for i in range(HOLE[1], HOLE[1] + B) if d[i] == s[i + OFF])
    assert same_after < B // 16, 'copy does not end at the bank boundary?'

    d[HOLE[0]:HOLE[1]] = s[HOLE[0] + OFF:HOLE[1] + OFF]
    open(out_path, 'wb').write(d)
    print('restored ROM $%06X-$%06X from SF II $%06X-$%06X (all 76 sound entries verified)'
          % (HOLE[0], HOLE[1] - 1, HOLE[0] + OFF, HOLE[1] + OFF - 1))
    print('wrote %s  CRC32 %08X  64 KB fp %08X' % (out_path, crc(d), crc(bytes(d[:0x10000]))))

if __name__ == '__main__':
    if len(sys.argv) != 4: sys.exit(__doc__)
    main(*sys.argv[1:])
