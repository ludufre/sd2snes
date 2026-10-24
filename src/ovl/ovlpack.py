#!/usr/bin/env python3
"""ovlpack.py <image.raw> <fw.hdr> <out>: stamp a card overlay's header.

build = the firmware image CRC genhdr wrote at fw.hdr + 12 (what the loader compares
against fwhdr + 12 in flash), size = the image, crc = CRC32 of the image with the crc
field read as 0 (zlib CRC-32, as crc32.c)."""
import struct, sys, zlib

img = bytearray(open(sys.argv[1], 'rb').read())
hdr = open(sys.argv[2], 'rb').read()
if img[0:3] != b'SOV' or not 0x31 <= img[3] <= 0x39:
    sys.exit('ovlpack: no overlay header at the start of %s' % sys.argv[1])
struct.pack_into('<I', img, 4, struct.unpack_from('<I', hdr, 12)[0])
struct.pack_into('<I', img, 8, len(img))
struct.pack_into('<I', img, 12, 0)
struct.pack_into('<I', img, 12, zlib.crc32(bytes(img)) & 0xffffffff)
open(sys.argv[3], 'wb').write(img)
